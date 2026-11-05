#include "shell/parser.h"
#include "core/utils.h"
#include "backend_ast/builtin.h"
#include "shell/quote_markers.h"
#include "shell/word_scan.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RULE_MASK_COMMAND_START (SHELL_LEXER_RULE_1 | SHELL_LEXER_RULE_7)
#define RULE_MASK_COMMAND_CONTINUATION (SHELL_LEXER_RULE_7)
#define RULE_MASK_REDIRECT_TARGET (SHELL_LEXER_RULE_2)
#define RULE_MASK_HEREDOC_DELIMITER (SHELL_LEXER_RULE_3)
#define RULE_MASK_CASE_PATTERN (SHELL_LEXER_RULE_4)
#define RULE_MASK_FOR_NAME (SHELL_LEXER_RULE_5)
#define RULE_MASK_FOR_LIST (SHELL_LEXER_RULE_6 | SHELL_LEXER_RULE_1)
#define RULE_MASK_FUNCTION_NAME (SHELL_LEXER_RULE_8 | SHELL_LEXER_RULE_1)
#define RULE_MASK_FUNCTION_BODY (SHELL_LEXER_RULE_9)

#define STRUCTURAL_CLOSER_RPAREN (1u << 0)
#define STRUCTURAL_CLOSER_RBRACE (1u << 1)

typedef struct {
    ShellRedirection *redir;
    char *delimiter;
    bool strip_tabs;
    bool quoted;
    int line;
} ShellPendingHereDoc;

typedef struct ShellPendingHereDocArray {
    ShellPendingHereDoc *items;
    size_t count;
    size_t capacity;
} ShellPendingHereDocArray;

static void pendingHereDocArrayInit(ShellPendingHereDocArray *array);
static void pendingHereDocArrayFree(ShellPendingHereDocArray *array);
static void pendingHereDocArrayPush(ShellPendingHereDocArray *array, ShellRedirection *redir,
                                    const char *delimiter, bool strip_tabs, bool quoted, int line);

static void parserWordArrayInit(ShellWordArray *array);
static bool parserWordArrayAppend(ShellWordArray *array, ShellWord *word);
static void parserWordArrayFree(ShellWordArray *array);

static void parserScheduleRuleMask(ShellParser *parser, unsigned int mask);
static unsigned int parserStructuralCloserBit(ShellTokenType type);
static void shellParserAdvance(ShellParser *parser);
static void shellParserConsume(ShellParser *parser, ShellTokenType type);
static bool shellParserCheck(const ShellParser *parser, ShellTokenType type);
static bool shellParserMatch(ShellParser *parser, ShellTokenType type);
static void shellParserSynchronize(ShellParser *parser);
static void parserErrorAt(ShellParser *parser, const ShellToken *token, const char *message);
static void parserUnexpectedToken(ShellParser *parser);
static void parserSplitDoubleParenUnlessArithmetic(ShellParser *parser);
static void parserFinishedLine(ShellParser *parser, const ShellProgram *program);
static void parserReclassifyCurrentToken(ShellParser *parser, unsigned int mask);

static bool parserConsumePendingHereDocs(ShellParser *parser);
static char *parserCopyWordWithoutMarkers(const ShellWord *word);
static char *parserCopyTrimmedRange(const char *src, size_t start, size_t end);
static bool parserExtractCStyleForSegments(ShellParser *parser, size_t start_pos, char **init_out, char **cond_out,
                                          char **update_out);
static bool parserExtractArithmeticCommandExpression(ShellParser *parser, size_t start_pos, char **expr_out);

static bool parseCompleteCommands(ShellParser *parser, ShellProgram *program);
static bool parseCompleteCommand(ShellParser *parser, ShellProgram *program);
static bool parseList(ShellParser *parser, ShellProgram *program);
static ShellCommand *parseAndOr(ShellParser *parser);
static ShellPipeline *parsePipeline(ShellParser *parser);
static ShellCommand *parsePipelineCommand(ShellParser *parser);
static ShellCommand *parseCommand(ShellParser *parser);
static ShellCommand *parseSimpleCommand(ShellParser *parser);
static ShellCommand *parseArithmeticCommand(ShellParser *parser);
static ShellCommand *parseCompoundCommand(ShellParser *parser);
static ShellCommand *parseBraceGroup(ShellParser *parser);
static ShellCommand *parseSubshell(ShellParser *parser);
static ShellProgram *parseCompoundListUntil(ShellParser *parser, ShellTokenType terminator1,
                                           ShellTokenType terminator2, ShellTokenType terminator3);
static ShellCommand *parseIfClause(ShellParser *parser);
static ShellCommand *parseWhileClause(ShellParser *parser, bool is_until);
static ShellCommand *parseForClause(ShellParser *parser);
static ShellCommand *parseCStyleForClause(ShellParser *parser, int line, int column);
static ShellCommand *parseCaseClause(ShellParser *parser);
static ShellCommand *parseFunctionDefinition(ShellParser *parser);
static ShellCommand *parseFunctionDefinitionFromName(ShellParser *parser);
static bool parserIsFunctionDefinitionStart(ShellParser *parser);
static void parseLinebreak(ShellParser *parser);
static bool tokenStartsCommand(const ShellToken *token);

static ShellWord *parseWordToken(ShellParser *parser);
static ShellRedirection *parseRedirection(ShellParser *parser, bool *strip_tabs_out);

static void populateWordExpansions(ShellWord *word);
static bool parseDollarCommandSubstitution(const char *text, size_t start, size_t *out_span,
                                           char **out_command);
static bool parseBacktickCommandSubstitution(const char *text, size_t start, size_t *out_span,
                                             char **out_command);
static char *normalizeDollarCommand(const char *command, size_t len);
static char *normalizeBacktickCommand(const char *command, size_t len);

static ShellProgram *shellParseStringInternal(const char *source, ShellParser *parser, bool quiet,
                                              int first_line) {
    if (!parser) {
        return NULL;
    }

    memset(parser, 0, sizeof(*parser));
    parser->quiet = quiet;
    shellInitLexer(&parser->lexer, source);
    if (first_line > 0) {
        parser->lexer.line = first_line;
    }
    parser->had_error = false;
    parser->panic_mode = false;
    parser->next_rule_mask = RULE_MASK_COMMAND_START;
    parser->pending_here_docs = (ShellPendingHereDocArray *)calloc(1, sizeof(ShellPendingHereDocArray));
    if (!parser->pending_here_docs) {
        return NULL;
    }
    pendingHereDocArrayInit(parser->pending_here_docs);

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    ShellProgram *program = shellCreateProgram();
    if (!program) {
        return NULL;
    }
    parser->top_level = program;

    if (parseCompleteCommands(parser, program) && parser->lexer.unterminated_closer) {
        /* A word can run off the end without the grammar minding. */
        parserUnexpectedToken(parser);
    }
    if (parser->had_error) {
        /* Bash has run what came on the lines before a syntax error by the time it reads it.
         * What is left is for the caller to run, and then to report the error. */
        if (parser->syntax_error && parser->complete_commands > 0) {
            for (size_t i = parser->complete_commands; i < program->commands.count; ++i) {
                shellFreeCommand(program->commands.items[i]);
            }
            program->commands.count = parser->complete_commands;
        } else {
            shellFreeProgram(program);
            program = NULL;
        }
    }

    return program;
}

ShellProgram *shellParseString(const char *source, ShellParser *parser) {
    return shellParseStringInternal(source, parser, false, 0);
}

ShellProgram *shellParseStringAtLine(const char *source, ShellParser *parser, int first_line) {
    return shellParseStringInternal(source, parser, false, first_line);
}

bool shellSourceIsIncomplete(const char *source) {
    if (!source || !*source) {
        return false;
    }
    ShellParser parser;
    ShellProgram *program = shellParseStringInternal(source, &parser, true, 0);
    /* A word can run off the end with a quote still open without the parser
     * ever raising an error, so the lexer's own verdict counts too. */
    bool incomplete = parser.incomplete || parser.lexer.unterminated;
    if (program) {
        shellFreeProgram(program);
    }
    shellParserFree(&parser);
    return incomplete;
}

void shellParserFree(ShellParser *parser) {
    if (!parser) {
        return;
    }
    shellFreeToken(&parser->current);
    shellFreeToken(&parser->previous);
    free(parser->syntax_error_message);
    parser->syntax_error_message = NULL;
    free(parser->syntax_error_text);
    parser->syntax_error_text = NULL;
    for (size_t i = 0; i < parser->warning_count; ++i) {
        free(parser->warnings[i].message);
    }
    free(parser->warnings);
    parser->warnings = NULL;
    parser->warning_count = 0;
    if (parser->pending_here_docs) {
        pendingHereDocArrayFree(parser->pending_here_docs);
        free(parser->pending_here_docs);
        parser->pending_here_docs = NULL;
    }
}

static void pendingHereDocArrayInit(ShellPendingHereDocArray *array) {
    if (!array) {
        return;
    }
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void pendingHereDocArrayFree(ShellPendingHereDocArray *array) {
    if (!array) {
        return;
    }
    for (size_t i = 0; i < array->count; ++i) {
        free(array->items[i].delimiter);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void pendingHereDocArrayPush(ShellPendingHereDocArray *array, ShellRedirection *redir,
                                    const char *delimiter, bool strip_tabs, bool quoted, int line) {
    if (!array || !redir || !delimiter) {
        return;
    }
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        ShellPendingHereDoc *new_items =
            (ShellPendingHereDoc *)realloc(array->items, new_capacity * sizeof(ShellPendingHereDoc));
        if (!new_items) {
            return;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    ShellPendingHereDoc *entry = &array->items[array->count++];
    entry->redir = redir;
    entry->delimiter = strdup(delimiter);
    entry->strip_tabs = strip_tabs;
    entry->quoted = quoted;
    entry->line = line;
}

static void parserWordArrayInit(ShellWordArray *array) {
    if (!array) {
        return;
    }
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static bool parserWordArrayAppend(ShellWordArray *array, ShellWord *word) {
    if (!array || !word) {
        return false;
    }
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        ShellWord **new_items = (ShellWord **)realloc(array->items, new_capacity * sizeof(ShellWord *));
        if (!new_items) {
            return false;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = word;
    return true;
}

static void parserWordArrayFree(ShellWordArray *array) {
    if (!array) {
        return;
    }
    for (size_t i = 0; i < array->count; ++i) {
        shellFreeWord(array->items[i]);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void parserScheduleRuleMask(ShellParser *parser, unsigned int mask) {
    if (!parser) {
        return;
    }
    parser->next_rule_mask = mask;
}

static unsigned int parserStructuralCloserBit(ShellTokenType type) {
    switch (type) {
        case SHELL_TOKEN_RPAREN:
            return STRUCTURAL_CLOSER_RPAREN;
        case SHELL_TOKEN_RBRACE:
            return STRUCTURAL_CLOSER_RBRACE;
        default:
            return 0u;
    }
}

static void parserSetStructuralClosers(ShellParser *parser, unsigned int mask) {
    parser->structural_closer_mask = mask;
    parser->lexer.rparen_closes = (mask & STRUCTURAL_CLOSER_RPAREN) != 0;
}

static void applyLexicalRules(ShellToken *token) {
    if (!token) {
        return;
    }
    unsigned int mask = token->rule_mask;
    bool reserved_allowed = (mask & SHELL_LEXER_RULE_1) != 0;
    bool treat_as_assignment = (mask & SHELL_LEXER_RULE_7) != 0;
    bool treat_as_for_name = (mask & SHELL_LEXER_RULE_5) != 0;
    bool treat_as_function_name = (mask & SHELL_LEXER_RULE_8) != 0;
    bool force_word_context = (mask & (SHELL_LEXER_RULE_2 | SHELL_LEXER_RULE_3 | SHELL_LEXER_RULE_4 |
                                      SHELL_LEXER_RULE_9)) != 0;

    if (force_word_context && token->reserved_candidate) {
        token->type = SHELL_TOKEN_WORD;
    } else if (token->reserved_candidate) {
        token->type = reserved_allowed ? token->reserved_type : SHELL_TOKEN_WORD;
    }

    if (!treat_as_assignment && token->type == SHELL_TOKEN_ASSIGNMENT_WORD && !token->assignment_candidate) {
        token->type = SHELL_TOKEN_WORD;
    }
    if (treat_as_assignment && token->assignment_candidate) {
        token->type = SHELL_TOKEN_ASSIGNMENT_WORD;
    }

    if ((treat_as_for_name || treat_as_function_name) && token->name_candidate) {
        token->type = SHELL_TOKEN_NAME;
    }

    if ((mask & SHELL_LEXER_RULE_6) != 0 && token->reserved_candidate) {
        token->type = token->reserved_type;
    }

    if ((mask & SHELL_LEXER_RULE_1) != 0 && token->lexeme && token->length == 1) {
        ShellTokenType structural = SHELL_TOKEN_ERROR;
        switch (token->lexeme[0]) {
            case '(': structural = SHELL_TOKEN_LPAREN; break;
            case ')': structural = SHELL_TOKEN_RPAREN; break;
            case '{': structural = SHELL_TOKEN_LBRACE; break;
            case '}': structural = SHELL_TOKEN_RBRACE; break;
            default: break;
        }
        if (structural != SHELL_TOKEN_ERROR) {
            token->type = structural;
            token->base_type = structural;
            token->reserved_type = structural;
        }
    }
}

static void shellParserAdvance(ShellParser *parser) {
    if (!parser) {
        return;
    }
    shellFreeToken(&parser->previous);
    parser->previous = parser->current;

    if (parser->previous.type == SHELL_TOKEN_NEWLINE) {
        parserConsumePendingHereDocs(parser);
    }

    shellLexerSetRuleMask(&parser->lexer, parser->next_rule_mask);
    parser->current = shellNextToken(&parser->lexer);
    applyLexicalRules(&parser->current);
}

static bool shellParserCheck(const ShellParser *parser, ShellTokenType type) {
    return parser && parser->current.type == type;
}

static bool shellParserMatch(ShellParser *parser, ShellTokenType type) {
    if (!parser || !shellParserCheck(parser, type)) {
        return false;
    }
    shellParserAdvance(parser);
    return true;
}

static void parserErrorAt(ShellParser *parser, const ShellToken *token, const char *message) {
    if (!parser || parser->had_error) {
        return;
    }
    /* An error whose offending token is the end of the source - or that the
     * lexer already flagged - means the text simply stopped early.  Record that
     * so an interactive caller can ask for the rest instead of giving up. */
    if (parser->lexer.unterminated ||
        (token && token->type == SHELL_TOKEN_EOF) ||
        (!token && parser->lexer.pos >= parser->lexer.length)) {
        parser->incomplete = true;
    }
    if (!parser->quiet) {
        int line = token ? token->line : parser->lexer.line;
        int column = token ? token->column : parser->lexer.column;
        fprintf(stderr, "shell parse error at %d:%d: %s\n", line, column,
                message ? message : "error");
    }
    parser->had_error = true;
    parser->panic_mode = true;
}

/* What bash says, kept for the caller. */
static void parserSyntaxError(ShellParser *parser, int line, int status, const char *format, const char *what) {
    size_t size = strlen(format) + strlen(what) + 1;
    char *message = (char *)malloc(size);
    if (message) {
        snprintf(message, size, format, what);
    }
    parser->syntax_error_message = message;
    parser->syntax_error_line = line;
    parser->syntax_error_status = status;
    parser->syntax_error = true;
    parser->had_error = true;
    parser->panic_mode = true;
}

/* The current token can't stand here. A bash reads a line at a time and shows the one it is on;
 * text that ran out is not wrong, and more of it may come at a prompt. */
static void parserUnexpectedToken(ShellParser *parser) {
    if (!parser || parser->had_error) {
        return;
    }
    const ShellLexer *lexer = &parser->lexer;
    const ShellToken *token = &parser->current;
    if (lexer->unterminated_closer) {
        char closer[2] = {lexer->unterminated_closer, '\0'};
        parser->incomplete = true;
        parserSyntaxError(parser, lexer->unterminated_line, lexer->unterminated_array ? 1 : 2,
                          "unexpected EOF while looking for matching `%s'", closer);
        return;
    }
    if (token->type == SHELL_TOKEN_EOF) {
        parser->incomplete = true;
        parserSyntaxError(parser, token->line, 2, "syntax error: unexpected end of file%s", "");
        return;
    }
    if (token->type == SHELL_TOKEN_ERROR) {
        parserErrorAt(parser, token, token->lexeme);
        return;
    }
    const char *src = lexer->src;
    size_t start = lexer->token_start;
    size_t end = start;
    while (start > 0 && src[start - 1] != '\n') {
        start--;
    }
    while (end < lexer->length && src[end] != '\n') {
        end++;
    }
    /* A newline token already counts the line it ended. */
    bool newline = token->type == SHELL_TOKEN_NEWLINE;
    char *text = newline ? strdup("newline") : strndup(src + lexer->token_start, lexer->pos - lexer->token_start);
    parserSyntaxError(parser, newline ? token->line - 1 : token->line, 2, "syntax error near unexpected token `%s'",
                      text ? text : "");
    free(text);
    parser->syntax_error_text = strndup(src + start, end - start);
}

/* A command ends at a separator, or at what closes the list it is in: `( a ) b` has neither. */
static bool parserAfterSeparator(const ShellParser *parser) {
    return parser->previous.type == SHELL_TOKEN_SEMICOLON || parser->previous.type == SHELL_TOKEN_AMPERSAND ||
           parser->previous.type == SHELL_TOKEN_NEWLINE;
}

/* A do, then, else, { or ( needs a command in it. What closes it comes too early. */
static void parserRequireCommand(ShellParser *parser, const ShellProgram *list) {
    if (parser && list && list->commands.count == 0 && parser->current.type != SHELL_TOKEN_EOF) {
        parserUnexpectedToken(parser);
    }
}

static void shellParserConsume(ShellParser *parser, ShellTokenType type) {
    if (!parser) {
        return;
    }
    if (parser->current.type == type) {
        shellParserAdvance(parser);
        return;
    }
    parserUnexpectedToken(parser);
}

static void shellParserSynchronize(ShellParser *parser) {
    if (!parser) {
        return;
    }
    while (parser->current.type != SHELL_TOKEN_EOF) {
        if (parser->previous.type == SHELL_TOKEN_SEMICOLON || parser->previous.type == SHELL_TOKEN_NEWLINE) {
            parser->panic_mode = false;
            return;
        }
        switch (parser->current.type) {
            case SHELL_TOKEN_IF:
            case SHELL_TOKEN_THEN:
            case SHELL_TOKEN_ELIF:
            case SHELL_TOKEN_ELSE:
            case SHELL_TOKEN_FI:
            case SHELL_TOKEN_FOR:
            case SHELL_TOKEN_WHILE:
            case SHELL_TOKEN_UNTIL:
            case SHELL_TOKEN_DO:
            case SHELL_TOKEN_DONE:
            case SHELL_TOKEN_CASE:
            case SHELL_TOKEN_ESAC:
                parser->panic_mode = false;
                return;
            default:
                break;
        }
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
    }
}

static void parserReclassifyCurrentToken(ShellParser *parser, unsigned int mask) {
    if (!parser) {
        return;
    }
    parser->current.rule_mask = mask;
    applyLexicalRules(&parser->current);
}

static void parserWarnHereDocEnd(ShellParser *parser, const ShellPendingHereDoc *pending) {
    const char *format = "warning: here-document at line %d delimited by end-of-file (wanted `%s')";
    size_t size = strlen(format) + 32 + strlen(pending->delimiter);
    char *message = (char *)malloc(size);
    ShellParseWarning *grown =
        (ShellParseWarning *)realloc(parser->warnings, (parser->warning_count + 1) * sizeof(ShellParseWarning));
    if (!message || !grown) {
        free(message);
        parser->warnings = grown ? grown : parser->warnings;
        return;
    }
    snprintf(message, size, format, pending->line, pending->delimiter);
    const ShellLexer *lexer = &parser->lexer;
    bool ended_line = lexer->final_newline_given || (lexer->length > 0 && lexer->src[lexer->length - 1] == '\n');
    parser->warnings = grown;
    parser->warnings[parser->warning_count].line = ended_line ? lexer->line - 1 : lexer->line;
    parser->warnings[parser->warning_count].message = message;
    parser->warning_count++;
}

static bool parserConsumePendingHereDocs(ShellParser *parser) {
    if (!parser || !parser->pending_here_docs || parser->pending_here_docs->count == 0) {
        return true;
    }
    ShellPendingHereDocArray *array = parser->pending_here_docs;
    ShellLexer *lexer = &parser->lexer;
    const char *src = lexer->src;
    size_t length = lexer->length;

    for (size_t idx = 0; idx < array->count; ++idx) {
        ShellPendingHereDoc *pending = &array->items[idx];
        size_t buffer_capacity = 0;
        size_t buffer_length = 0;
        char *buffer = NULL;
        bool done = false;
        while (!done) {
            if (lexer->pos >= length) {
                if (parser->quiet) {
                    /* At a prompt more of it may come. */
                    parser->incomplete = true;
                    parser->had_error = true;
                    free(buffer);
                    return false;
                }
                /* It ends with the text, and bash says so. */
                parserWarnHereDocEnd(parser, pending);
                break;
            }
            size_t line_start = lexer->pos;
            int line_number = lexer->line;
            int column_number = lexer->column;
            while (lexer->pos < length && src[lexer->pos] != '\n') {
                lexer->pos++;
                lexer->column++;
            }
            size_t raw_len = lexer->pos - line_start;
            const char *raw_text = src + line_start;

            char *line = (char *)malloc(raw_len + 1);
            if (!line) {
                free(buffer);
                return false;
            }
            memcpy(line, raw_text, raw_len);
            line[raw_len] = '\0';

            if (lexer->pos < length && src[lexer->pos] == '\n') {
                lexer->pos++;
                lexer->line++;
                lexer->column = 1;
            }

            const char *comparison = line;
            if (pending->strip_tabs) {
                while (*comparison == '\t') {
                    comparison++;
                }
            }

            bool matches = strcmp(comparison, pending->delimiter) == 0;
            if (matches) {
                free(line);
                done = true;
                break;
            }

            const char *body_line = line;
            if (pending->strip_tabs) {
                while (*body_line == '\t') {
                    body_line++;
                }
            }

            size_t body_len = strlen(body_line);
            if (buffer_length + body_len + 1 > buffer_capacity) {
                size_t new_capacity = buffer_capacity ? buffer_capacity * 2 : 64;
                while (new_capacity < buffer_length + body_len + 1) {
                    new_capacity *= 2;
                }
                char *tmp = (char *)realloc(buffer, new_capacity);
                if (!tmp) {
                    free(line);
                    free(buffer);
                    return false;
                }
                buffer = tmp;
                buffer_capacity = new_capacity;
            }
            memcpy(buffer + buffer_length, body_line, body_len);
            buffer_length += body_len;
            buffer[buffer_length++] = '\n';
            free(line);
            (void)line_number;
            (void)column_number;
        }
        if (!pending->redir) {
            free(buffer);
            continue;
        }
        if (!buffer) {
            buffer = strdup("");
        } else {
            if (buffer_length == 0) {
                char *tmp = (char *)realloc(buffer, 1);
                if (tmp) {
                    buffer = tmp;
                }
            } else {
                char *tmp = (char *)realloc(buffer, buffer_length + 1);
                if (tmp) {
                    buffer = tmp;
                }
                buffer[buffer_length] = '\0';
            }
        }
        shellRedirectionSetHereDocument(pending->redir, buffer ? buffer : "", pending->quoted);
        free(buffer);
        free(pending->delimiter);
        pending->delimiter = NULL;
    }

    array->count = 0;
    return true;
}

/* `<<"EOF"`, `<<\\EOF` and `<<E'O'F` all leave a here document unexpanded. */
static bool wordHasQuoting(const ShellWord *word) {
    if (!word) {
        return false;
    }
    return word->single_quoted || word->double_quoted ||
           (word->text && (strchr(word->text, SHELL_QUOTE_MARK_SINGLE) || strchr(word->text, SHELL_QUOTE_MARK_DOUBLE)));
}

static char *parserCopyWordWithoutMarkers(const ShellWord *word) {
    if (!word || !word->text) {
        return NULL;
    }
    size_t len = strlen(word->text);
    char *result = (char *)malloc(len + 1);
    if (!result) {
        return NULL;
    }
    size_t out = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = word->text[i];
        if (c == SHELL_QUOTE_MARK_SINGLE || c == SHELL_QUOTE_MARK_DOUBLE) {
            continue;
        }
        result[out++] = c;
    }
    result[out] = '\0';
    char *shrunk = (char *)realloc(result, out + 1);
    return shrunk ? shrunk : result;
}

static char *parserCopyTrimmedRange(const char *src, size_t start, size_t end) {
    if (!src || end <= start) {
        return strdup("");
    }
    while (start < end && isspace((unsigned char)src[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)src[end - 1])) {
        end--;
    }
    size_t len = end > start ? (end - start) : 0;
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    if (len > 0) {
        memcpy(out, src + start, len);
    }
    out[len] = '\0';
    return out;
}

/* `for ((` with nothing to close it. The lexer has moved past the opening, not to the end. */
static void parserRanOutInParen(ShellParser *parser) {
    parser->lexer.unterminated = true;
    parser->lexer.unterminated_closer = ')';
    parser->lexer.unterminated_line = parser->lexer.line;
    parserUnexpectedToken(parser);
}

static bool parserExtractArithmeticCommandExpression(ShellParser *parser, size_t start_pos, char **expr_out) {
    if (!parser || !parser->lexer.src) {
        return false;
    }
    ShellLexer *lexer = &parser->lexer;
    const char *src = lexer->src;
    size_t length = lexer->length;
    size_t pos = start_pos;
    int depth = 1;
    size_t expr_end = SIZE_MAX;
    int line = lexer->line;
    int column = lexer->column;

    while (pos < length) {
        char ch = src[pos];
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0) {
                expr_end = pos;
                pos++;
                column++;
                break;
            }
        }
        if (ch == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
        pos++;
    }

    if (expr_end == SIZE_MAX) {
        parserRanOutInParen(parser);
        return false;
    }
    if (pos >= length || src[pos] != ')') {
        parserErrorAt(parser, &parser->current, "Expected '))' to close arithmetic command");
        return false;
    }

    column++;
    pos++;

    char *expr = parserCopyTrimmedRange(src, start_pos, expr_end);
    if (!expr) {
        parserErrorAt(parser, &parser->current, "Out of memory parsing arithmetic command");
        return false;
    }

    if (expr_out) {
        *expr_out = expr;
    } else {
        free(expr);
    }

    lexer->pos = pos;
    lexer->line = line;
    lexer->column = column;
    lexer->at_line_start = (column == 1);

    shellLexerSetRuleMask(lexer, parser->next_rule_mask);
    shellFreeToken(&parser->current);
    parser->current = shellNextToken(lexer);
    applyLexicalRules(&parser->current);
    return true;
}

static bool parserExtractCStyleForSegments(ShellParser *parser, size_t start_pos, char **init_out, char **cond_out,
                                          char **update_out) {
    if (!parser || !parser->lexer.src) {
        return false;
    }
    ShellLexer *lexer = &parser->lexer;
    const char *src = lexer->src;
    size_t length = lexer->length;
    size_t pos = start_pos;
    int depth = 1;
    size_t semicolons[2] = {SIZE_MAX, SIZE_MAX};
    size_t semicolon_count = 0;
    size_t expr_end = SIZE_MAX;
    int line = lexer->line;
    int column = lexer->column;

    while (pos < length) {
        char ch = src[pos];
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0) {
                expr_end = pos;
                pos++;
                column++;
                break;
            }
        } else if (ch == ';' && depth == 1 && semicolon_count < 2) {
            semicolons[semicolon_count++] = pos;
        }
        if (ch == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
        pos++;
    }

    if (expr_end == SIZE_MAX) {
        parserRanOutInParen(parser);
        return false;
    }
    if (pos >= length || src[pos] != ')') {
        parserErrorAt(parser, &parser->current, "Expected '))' to close arithmetic for clause");
        return false;
    }

    column++;
    pos++;

    if (semicolon_count < 2 || semicolons[0] == SIZE_MAX || semicolons[1] == SIZE_MAX) {
        parserErrorAt(parser, &parser->current,
                      "Arithmetic for clause requires two ';' separators");
        return false;
    }

    size_t init_start = start_pos;
    size_t init_end = semicolons[0];
    size_t cond_start = semicolons[0] + 1;
    size_t cond_end = semicolons[1];
    size_t update_start = semicolons[1] + 1;
    size_t update_end = expr_end;

    char *init = parserCopyTrimmedRange(src, init_start, init_end);
    char *cond = parserCopyTrimmedRange(src, cond_start, cond_end);
    char *update = parserCopyTrimmedRange(src, update_start, update_end);
    if (!init || !cond || !update) {
        free(init);
        free(cond);
        free(update);
        parserErrorAt(parser, &parser->current, "Out of memory parsing arithmetic for clause");
        return false;
    }

    if (init_out) {
        *init_out = init;
    } else {
        free(init);
    }
    if (cond_out) {
        *cond_out = cond;
    } else {
        free(cond);
    }
    if (update_out) {
        *update_out = update;
    } else {
        free(update);
    }

    lexer->pos = pos;
    lexer->line = line;
    lexer->column = column;
    lexer->at_line_start = (column == 1);

    shellLexerSetRuleMask(lexer, parser->next_rule_mask);
    shellFreeToken(&parser->current);
    parser->current = shellNextToken(lexer);
    applyLexicalRules(&parser->current);
    return true;
}

static void parseLinebreak(ShellParser *parser) {
    if (!parser) {
        return;
    }
    while (parser->current.type == SHELL_TOKEN_NEWLINE) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
    }
}

static bool tokenStartsCommand(const ShellToken *token) {
    if (!token) {
        return false;
    }
    switch (token->type) {
        case SHELL_TOKEN_WORD:
        case SHELL_TOKEN_ASSIGNMENT_WORD:
        case SHELL_TOKEN_NAME:
        case SHELL_TOKEN_PARAMETER:
        case SHELL_TOKEN_IO_NUMBER:
        case SHELL_TOKEN_LPAREN:
        case SHELL_TOKEN_LBRACE:
        case SHELL_TOKEN_BANG:
        case SHELL_TOKEN_FUNCTION:
        case SHELL_TOKEN_IF:
        case SHELL_TOKEN_WHILE:
        case SHELL_TOKEN_UNTIL:
        case SHELL_TOKEN_FOR:
        case SHELL_TOKEN_CASE:
            return true;
        default:
            return false;
    }
}

/* Bash reads a line at a time, so what the top level finished when a line ended has run by the
 * time it reads on. A word still open at the end of the text belongs to the last line. */
static void parserFinishedLine(ShellParser *parser, const ShellProgram *program) {
    if (program == parser->top_level && !parser->had_error && !parser->lexer.unterminated_closer) {
        parser->complete_commands = program->commands.count;
    }
}

static bool parseCompleteCommands(ShellParser *parser, ShellProgram *program) {
    if (!parser || !program) {
        return false;
    }
    parseLinebreak(parser);
    while (!parser->had_error && parser->current.type != SHELL_TOKEN_EOF) {
        if (!parseCompleteCommand(parser, program)) {
            if (parser->panic_mode) {
                shellParserSynchronize(parser);
            } else {
                return false;
            }
        }
        if (parser->current.type == SHELL_TOKEN_NEWLINE || parser->current.type == SHELL_TOKEN_EOF) {
            parserFinishedLine(parser, program);
        }
        parseLinebreak(parser);
    }
    return !parser->had_error;
}

static bool parseCompleteCommand(ShellParser *parser, ShellProgram *program) {
    if (!parser || !program) {
        return false;
    }
    if (!parseList(parser, program)) {
        return false;
    }
    if (!parserAfterSeparator(parser) && parser->current.type != SHELL_TOKEN_NEWLINE &&
        parser->current.type != SHELL_TOKEN_EOF) {
        parserUnexpectedToken(parser);
        return false;
    }
    return true;
}

static bool parseList(ShellParser *parser, ShellProgram *program) {
    if (!parser || !program) {
        return false;
    }
    while (true) {
        ShellCommand *command = parseAndOr(parser);
        if (!command) {
            return false;
        }
        shellProgramAddCommand(program, command);

        if (parser->current.type == SHELL_TOKEN_AMPERSAND || parser->current.type == SHELL_TOKEN_SEMICOLON) {
            if (parser->current.type == SHELL_TOKEN_AMPERSAND) {
                command->exec.runs_in_background = true;
                command->exec.is_async_parent = true;
            }
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
            shellParserAdvance(parser);
            if (parser->current.type == SHELL_TOKEN_NEWLINE || parser->current.type == SHELL_TOKEN_EOF) {
                parserFinishedLine(parser, program);
            }
            parseLinebreak(parser);
            if (!tokenStartsCommand(&parser->current)) {
                break;
            }
            continue;
        }
        break;
    }
    return true;
}

static ShellCommand *parseAndOr(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    ShellPipeline *first = parsePipeline(parser);
    if (!first) {
        return NULL;
    }

    ShellLogicalList *logical = NULL;
    while (parser->current.type == SHELL_TOKEN_AND_AND || parser->current.type == SHELL_TOKEN_OR_OR) {
        ShellLogicalConnector connector =
            (parser->current.type == SHELL_TOKEN_AND_AND) ? SHELL_LOGICAL_AND : SHELL_LOGICAL_OR;
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
        ShellPipeline *next = parsePipeline(parser);
        if (!next) {
            break;
        }
        if (!logical) {
            logical = shellCreateLogicalList();
            shellLogicalListAdd(logical, first, SHELL_LOGICAL_AND);
        }
        shellLogicalListAdd(logical, next, connector);
    }

    if (logical) {
        ShellCommand *cmd = shellCreateLogicalCommand(logical);
        if (cmd) {
            if (first && first->command_count > 0 && first->commands[0]) {
                cmd->line = first->commands[0]->line;
                cmd->column = first->commands[0]->column;
            } else {
                cmd->line = parser->current.line;
                cmd->column = parser->current.column;
            }
        }
        return cmd;
    }

    ShellCommand *cmd = shellCreatePipelineCommand(first);
    if (cmd && first) {
        if (first->command_count > 0 && first->commands[0]) {
            cmd->line = first->commands[0]->line;
            cmd->column = first->commands[0]->column;
        } else {
            cmd->line = parser->current.line;
            cmd->column = parser->current.column;
        }
        for (size_t i = 0; i < first->command_count; ++i) {
            ShellCommand *member = first->commands[i];
            member->exec.pipeline_index = (int)i;
            member->exec.is_pipeline_head = (i == 0);
            member->exec.is_pipeline_tail = (i + 1 == first->command_count);
        }
    }
    return cmd;
}

static bool tokenIsWordText(const ShellToken *token, const char *text) {
    return (token->type == SHELL_TOKEN_WORD || token->type == SHELL_TOKEN_NAME) && token->lexeme &&
           !token->single_quoted && !token->double_quoted && strcmp(token->lexeme, text) == 0;
}

/* `time` opens a pipeline as `!` does, but the lexer has no token for it: the word is a name
 * anywhere else, `time=1` and `for time in` among them. An alias comes first, and `time ()`
 * defines a function. */
static bool parserAtTimeKeyword(ShellParser *parser) {
    return tokenIsWordText(&parser->current, "time") && !parserIsFunctionDefinitionStart(parser) &&
           !shellRuntimeAliasExpands("time");
}

static ShellPipeline *parsePipeline(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    bool negate = false;
    bool timed = false;
    bool time_posix = false;
    while (parser->current.type == SHELL_TOKEN_BANG || parserAtTimeKeyword(parser)) {
        bool is_time = parser->current.type != SHELL_TOKEN_BANG;
        negate = is_time ? negate : !negate;
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        if (!is_time) {
            continue;
        }
        timed = true;
        if (tokenIsWordText(&parser->current, "-p")) {
            time_posix = true;
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
            shellParserAdvance(parser);
        }
        if (tokenIsWordText(&parser->current, "--")) {
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
            shellParserAdvance(parser);
        }
    }

    ShellPipeline *pipeline = shellCreatePipeline();
    if (!pipeline) {
        return NULL;
    }
    shellPipelineSetNegated(pipeline, negate);
    pipeline->timed = timed;
    pipeline->time_posix = time_posix;

    /* `time` alone times nothing, and succeeds. */
    bool alone = parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE ||
                 parser->current.type == SHELL_TOKEN_EOF;
    if (timed && alone) {
        return pipeline;
    }

    /* `!` alone is `! :`, as in bash 5. 3.2 calls it an error. */
    ShellCommand *command = NULL;
    if (negate && alone) {
        command = shellCreateSimpleCommand();
        if (command) {
            shellCommandAddWord(command, shellCreateWord(":", false, false, false, false, parser->previous.line,
                                                         parser->previous.column));
            command->line = parser->previous.line;
            command->column = parser->previous.column;
        }
    } else {
        command = parsePipelineCommand(parser);
    }
    if (!command) {
        return pipeline;
    }
    shellPipelineAddCommand(pipeline, command);

    while (parser->current.type == SHELL_TOKEN_PIPE || parser->current.type == SHELL_TOKEN_PIPE_AMP) {
        ShellTokenType op = parser->current.type;
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
        ShellCommand *next = parsePipelineCommand(parser);
        if (!next) {
            break;
        }
        shellPipelineAddCommand(pipeline, next);
        if (op == SHELL_TOKEN_PIPE_AMP && pipeline->command_count >= 2) {
            shellPipelineSetMergeStderr(pipeline, pipeline->command_count - 2, true);
        }
    }

    for (size_t i = 0; i < pipeline->command_count; ++i) {
        ShellCommand *member = pipeline->commands[i];
        member->exec.pipeline_index = (int)i;
        member->exec.is_pipeline_head = (i == 0);
        member->exec.is_pipeline_tail = (i + 1 == pipeline->command_count);
    }

    return pipeline;
}

static ShellCommand *parsePipelineCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    ShellCommand *command = parseCommand(parser);
    /* The bodies start on the next line. On `cat <<EOF | wc -l` that is still ahead of
     * the lexer, and shellParserAdvance reads them when it gets past the newline. */
    if (command && parser->pending_here_docs && parser->pending_here_docs->count > 0 &&
        parser->current.type == SHELL_TOKEN_NEWLINE) {
        parserConsumePendingHereDocs(parser);
    }
    return command;
}

/* `((` opens arithmetic only if its text ends in `))`. Otherwise it is two subshells:
 * `((echo a); echo b)`. The token is split by handing its second byte back. */
static void parserSplitDoubleParenUnlessArithmetic(ShellParser *parser) {
    if (parser->current.type != SHELL_TOKEN_DLPAREN) {
        return;
    }
    if (shellScanArithmetic(parser->lexer.src, parser->lexer.length, parser->lexer.pos) != SHELL_SCAN_UNTERMINATED) {
        return;
    }
    parser->lexer.pos--;
    parser->lexer.column--;
    shellFreeToken(&parser->current);
    parser->current.type = SHELL_TOKEN_LPAREN;
    parser->current.base_type = SHELL_TOKEN_LPAREN;
    parser->current.reserved_type = SHELL_TOKEN_LPAREN;
    parser->current.lexeme = strdup("(");
    parser->current.length = 1;
}

static ShellCommand *parseCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    parserSplitDoubleParenUnlessArithmetic(parser);
    switch (parser->current.type) {
        case SHELL_TOKEN_FUNCTION:
            return parseFunctionDefinition(parser);
        case SHELL_TOKEN_LBRACE:
        case SHELL_TOKEN_LPAREN:
        case SHELL_TOKEN_IF:
        case SHELL_TOKEN_FOR:
        case SHELL_TOKEN_WHILE:
        case SHELL_TOKEN_UNTIL:
        case SHELL_TOKEN_CASE:
            return parseCompoundCommand(parser);
        default:
            if (parserIsFunctionDefinitionStart(parser)) {
                return parseFunctionDefinitionFromName(parser);
            }
            return parseSimpleCommand(parser);
    }
}

static ShellCommand *parseArithmeticCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }

    int line = parser->current.line;
    int column = parser->current.column;

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    size_t start_pos = parser->lexer.pos;
    if (parser->current.length <= parser->lexer.pos) {
        start_pos = parser->lexer.pos - parser->current.length;
    }

    char *expression = NULL;
    if (!parserExtractArithmeticCommandExpression(parser, start_pos, &expression)) {
        free(expression);
        return NULL;
    }

    ShellCommand *command = shellCreateArithmeticCommand(expression);
    if (!command) {
        return NULL;
    }
    command->line = line;
    command->column = column;

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);

    while (!parser->had_error) {
        bool strip_tabs = false;
        ShellRedirection *redir = parseRedirection(parser, &strip_tabs);
        if (!redir) {
            break;
        }
        shellCommandAddRedirection(command, redir);
        if (redir->type == SHELL_REDIRECT_HEREDOC && parser->pending_here_docs) {
            ShellWord *target = shellRedirectionGetWordTarget(redir);
            char *delimiter = parserCopyWordWithoutMarkers(target);
            if (!delimiter) {
                delimiter = strdup("");
            }
            bool quoted = wordHasQuoting(target);
            pendingHereDocArrayPush(parser->pending_here_docs, redir, delimiter ? delimiter : "",
                                    strip_tabs, quoted, parser->previous.line);
            free(delimiter);
        }
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
    }

    if (parser->had_error) {
        shellFreeCommand(command);
        return NULL;
    }

    return command;
}

static bool tokenIsBareWord(const ShellToken *token, const char *text) {
    return token->lexeme && strcmp(token->lexeme, text) == 0;
}

/* The current token is `[[`. Everything up to the `]]` is one conditional expression:
 * the lexer hands the operators over as words. */
static void parseDoubleBracketWords(ShellParser *parser, ShellCommand *command) {
    parser->lexer.dbracket = true;
    while (!parser->had_error) {
        if (parser->current.type == SHELL_TOKEN_EOF) {
            parserErrorAt(parser, &parser->current, "Expected `]]' to close conditional expression");
            break;
        }
        if (parser->current.type == SHELL_TOKEN_ERROR) {
            parserErrorAt(parser, &parser->current, parser->current.lexeme);
            break;
        }
        bool closing = tokenIsBareWord(&parser->current, "]]");
        if (closing) {
            parser->lexer.dbracket = false;
        } else if (tokenIsBareWord(&parser->current, "=~")) {
            parser->lexer.dbracket_regex = true;
        }
        ShellWord *word = parseWordToken(parser);
        if (word) {
            populateWordExpansions(word);
            shellCommandAddWord(command, word);
        }
        if (closing) {
            break;
        }
    }
    parser->lexer.dbracket = false;
    parser->lexer.dbracket_regex = false;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
}

static ShellCommand *parseSimpleCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    if (parser->current.type == SHELL_TOKEN_DLPAREN) {
        return parseArithmeticCommand(parser);
    }
    ShellCommand *command = shellCreateSimpleCommand();
    if (!command) {
        return NULL;
    }
    command->line = parser->current.line;
    command->column = parser->current.column;

    bool seen_word = false;
    bool conditional = false;
    if ((parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_NAME) &&
        tokenIsBareWord(&parser->current, "[[")) {
        parseDoubleBracketWords(parser, command);
        seen_word = true;
        conditional = true;
    }
    while (!parser->had_error) {
        if (parser->current.type == SHELL_TOKEN_WORD && parser->current.lexeme && parser->current.length == 1) {
            char ch = parser->current.lexeme[0];
            bool treat_as_closer = ch == '}' && (parser->structural_closer_mask & STRUCTURAL_CLOSER_RBRACE);
            if (treat_as_closer && !parser->current.single_quoted && !parser->current.double_quoted) {
                parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
                if (parser->current.type == SHELL_TOKEN_RBRACE) {
                    break;
                }
            }
        }
        if (parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_ASSIGNMENT_WORD ||
            parser->current.type == SHELL_TOKEN_NAME || parser->current.type == SHELL_TOKEN_PARAMETER) {
            if (conditional) {
                break;
            }
            ShellWord *word = parseWordToken(parser);
            if (word) {
                populateWordExpansions(word);
                shellCommandAddWord(command, word);
                seen_word = true;
            }
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
            continue;
        }

        bool strip_tabs = false;
        ShellRedirection *redir = parseRedirection(parser, &strip_tabs);
        if (redir) {
            shellCommandAddRedirection(command, redir);
            if (redir->type == SHELL_REDIRECT_HEREDOC && parser->pending_here_docs) {
                ShellWord *target = shellRedirectionGetWordTarget(redir);
                char *delimiter = parserCopyWordWithoutMarkers(target);
                if (!delimiter) {
                    delimiter = strdup("");
                }
                bool quoted = wordHasQuoting(target);
                pendingHereDocArrayPush(parser->pending_here_docs, redir, delimiter ? delimiter : "",
                                        strip_tabs, quoted, parser->previous.line);
                free(delimiter);
            }
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
            continue;
        }
        break;
    }

    if (!seen_word && command->data.simple.words.count == 0 && command->redirections.count == 0) {
        shellFreeCommand(command);
        parserUnexpectedToken(parser);
        return NULL;
    }
    return command;
}

static ShellCommand *parseCompoundCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    ShellCommand *command = NULL;
    switch (parser->current.type) {
        case SHELL_TOKEN_LBRACE:
            command = parseBraceGroup(parser);
            break;
        case SHELL_TOKEN_LPAREN:
            command = parseSubshell(parser);
            break;
        case SHELL_TOKEN_IF:
            command = parseIfClause(parser);
            break;
        case SHELL_TOKEN_WHILE:
            command = parseWhileClause(parser, false);
            break;
        case SHELL_TOKEN_UNTIL:
            command = parseWhileClause(parser, true);
            break;
        case SHELL_TOKEN_FOR:
            command = parseForClause(parser);
            break;
        case SHELL_TOKEN_CASE:
            command = parseCaseClause(parser);
            break;
        default:
            break;
    }

    if (!command) {
        return NULL;
    }

    while (true) {
        bool strip_tabs = false;
        ShellRedirection *redir = parseRedirection(parser, &strip_tabs);
        if (!redir) {
            break;
        }
        shellCommandAddRedirection(command, redir);
        if (redir->type == SHELL_REDIRECT_HEREDOC && parser->pending_here_docs) {
            ShellWord *target = shellRedirectionGetWordTarget(redir);
            char *delimiter = parserCopyWordWithoutMarkers(target);
            if (!delimiter) {
                delimiter = strdup("");
            }
            bool quoted = wordHasQuoting(target);
            pendingHereDocArrayPush(parser->pending_here_docs, redir, delimiter ? delimiter : "",
                                    strip_tabs, quoted, parser->previous.line);
            free(delimiter);
        }
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
    }

    return command;
}

static ShellCommand *parseBraceGroup(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);
    parseLinebreak(parser);
    ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_RBRACE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserRequireCommand(parser, body);
    shellParserConsume(parser, SHELL_TOKEN_RBRACE);
    ShellCommand *command = shellCreateBraceGroupCommand(body);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseSubshell(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);
    parseLinebreak(parser);
    ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_RPAREN, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserRequireCommand(parser, body);
    shellParserConsume(parser, SHELL_TOKEN_RPAREN);
    ShellCommand *command = shellCreateSubshellCommand(body);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellProgram *parseCompoundListUntil(ShellParser *parser, ShellTokenType terminator1,
                                           ShellTokenType terminator2, ShellTokenType terminator3) {
    if (!parser) {
        return NULL;
    }
    unsigned int saved_closer_mask = parser->structural_closer_mask;
    parserSetStructuralClosers(parser, saved_closer_mask | parserStructuralCloserBit(terminator1) |
                                           parserStructuralCloserBit(terminator2) |
                                           parserStructuralCloserBit(terminator3));

    ShellProgram *program = shellCreateProgram();
    if (!program) {
        parserSetStructuralClosers(parser, saved_closer_mask);
        return NULL;
    }
    parseLinebreak(parser);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    while (!parser->had_error && parser->current.type != terminator1 && parser->current.type != terminator2 &&
           parser->current.type != terminator3 && parser->current.type != SHELL_TOKEN_SEMI_AMP &&
           parser->current.type != SHELL_TOKEN_DSEMI_AMP && parser->current.type != SHELL_TOKEN_EOF) {
        if (!parseList(parser, program)) {
            break;
        }
        parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
        ShellTokenType next = parser->current.type;
        if (!parserAfterSeparator(parser) && next != SHELL_TOKEN_NEWLINE && next != terminator1 &&
            next != terminator2 && next != terminator3 && next != SHELL_TOKEN_SEMI_AMP &&
            next != SHELL_TOKEN_DSEMI_AMP && next != SHELL_TOKEN_EOF) {
            parserUnexpectedToken(parser);
            break;
        }
        parseLinebreak(parser);
        parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    }
    parserSetStructuralClosers(parser, saved_closer_mask);
    return program;
}

/* The condition of an if or while is a whole list, and its last command decides:
 * `while read x; [ -n "$x" ]; do`. One command stays as it is, which is what the
 * loop's fast paths look for. */
static ShellCommand *parseConditionList(ShellParser *parser, ShellTokenType terminator) {
    int line = parser->current.line;
    int column = parser->current.column;
    ShellProgram *list = parseCompoundListUntil(parser, terminator, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    if (!list) {
        return NULL;
    }
    if (list->commands.count == 1) {
        ShellCommand *only = list->commands.items[0];
        list->commands.count = 0;
        shellFreeProgram(list);
        return only;
    }
    if (list->commands.count == 0) {
        shellFreeProgram(list);
        parserUnexpectedToken(parser);
        return NULL;
    }
    ShellCommand *group = shellCreateBraceGroupCommand(list);
    if (group) {
        group->line = line;
        group->column = column;
    }
    return group;
}

static ShellCommand *parseIfClause(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);
    /* The condition is a compound_list, which may open with a linebreak: `if`
     * (or `elif`) alone on its line, condition on the next. */
    parseLinebreak(parser);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    ShellCommand *condition = parseConditionList(parser, SHELL_TOKEN_THEN);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_THEN);
    parseLinebreak(parser);
    ShellProgram *then_block = parseCompoundListUntil(parser, SHELL_TOKEN_ELIF, SHELL_TOKEN_ELSE, SHELL_TOKEN_FI);

    ShellProgram *else_block = NULL;
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserRequireCommand(parser, then_block);
    if (parser->current.type == SHELL_TOKEN_ELIF) {
        ShellCommand *elif_cmd = parseIfClause(parser);
        else_block = shellCreateProgram();
        shellProgramAddCommand(else_block, elif_cmd);
    } else if (parser->current.type == SHELL_TOKEN_ELSE) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
        else_block = parseCompoundListUntil(parser, SHELL_TOKEN_FI, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
        parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
        parserRequireCommand(parser, else_block);
        shellParserConsume(parser, SHELL_TOKEN_FI);
    } else {
        parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
        shellParserConsume(parser, SHELL_TOKEN_FI);
    }

    ShellConditional *conditional = shellCreateConditional(condition, then_block, else_block);
    ShellCommand *command = shellCreateConditionalCommand(conditional);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseWhileClause(ShellParser *parser, bool is_until) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);
    /* Same as `if`: `while`/`until` may sit alone on its line. */
    parseLinebreak(parser);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    ShellCommand *condition = parseConditionList(parser, SHELL_TOKEN_DO);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_DO);
    parseLinebreak(parser);
    ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_DONE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserRequireCommand(parser, body);
    shellParserConsume(parser, SHELL_TOKEN_DONE);
    ShellLoop *loop = shellCreateLoop(is_until, condition, body);
    ShellCommand *command = shellCreateLoopCommand(loop);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseCStyleForClause(ShellParser *parser, int line, int column) {
    if (!parser) {
        return NULL;
    }
    char *init = NULL;
    char *cond = NULL;
    char *update = NULL;

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    size_t start_pos = parser->lexer.pos;
    if (parser->current.length <= parser->lexer.pos) {
        start_pos = parser->lexer.pos - parser->current.length;
    }

    if (!parserExtractCStyleForSegments(parser, start_pos, &init, &cond, &update)) {
        free(init);
        free(cond);
        free(update);
        return NULL;
    }

    parseLinebreak(parser);
    if (parser->current.type == SHELL_TOKEN_SEMICOLON) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
    }
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_DO);
    parseLinebreak(parser);
    ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_DONE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserRequireCommand(parser, body);
    shellParserConsume(parser, SHELL_TOKEN_DONE);

    ShellLoop *loop = shellCreateCStyleForLoop(init, cond, update, body);
    free(init);
    free(cond);
    free(update);
    if (!loop) {
        shellFreeProgram(body);
        return NULL;
    }

    ShellCommand *command = shellCreateLoopCommand(loop);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseForClause(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    if (parser->current.type == SHELL_TOKEN_DLPAREN) {
        return parseCStyleForClause(parser, line, column);
    }

    /* Any word will do here, `for in in in` and all. One that can't be a name is only an error
     * when the loop runs, and that error shows it as written. */
    parserScheduleRuleMask(parser, RULE_MASK_FOR_NAME);
    parserReclassifyCurrentToken(parser, RULE_MASK_FOR_NAME);
    if (parser->current.type != SHELL_TOKEN_NAME && parser->current.type != SHELL_TOKEN_WORD &&
        parser->current.type != SHELL_TOKEN_ASSIGNMENT_WORD && parser->current.type != SHELL_TOKEN_PARAMETER) {
        parserUnexpectedToken(parser);
        return NULL;
    }
    char *name_text = strndup(parser->lexer.src + parser->lexer.token_start,
                              parser->lexer.pos - parser->lexer.token_start);
    ShellWord *name_word = shellCreateWord(name_text ? name_text : "", false, false, false, false,
                                           parser->current.line, parser->current.column);
    free(name_text);
    shellParserAdvance(parser);

    ShellProgram *body = NULL;
    ShellWordArray value_words;
    parserWordArrayInit(&value_words);

    parseLinebreak(parser);
    parserReclassifyCurrentToken(parser, RULE_MASK_FOR_LIST);
    if (parser->current.type == SHELL_TOKEN_IN) {
        /* What follows is words, `do` too, up to a separator. */
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
        shellParserAdvance(parser);
        while (parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_ASSIGNMENT_WORD ||
               parser->current.type == SHELL_TOKEN_NAME || parser->current.type == SHELL_TOKEN_PARAMETER) {
            ShellWord *word = parseWordToken(parser);
            if (word) {
                populateWordExpansions(word);
                if (!parserWordArrayAppend(&value_words, word)) {
                    shellFreeWord(word);
                    parserWordArrayFree(&value_words);
                    shellFreeWord(name_word);
                    return NULL;
                }
            }
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
            if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
                break;
            }
        }
    } else {
        /* With no `in` it is "$@". With an `in` that comes to nothing it is nothing:
         * `for x in $empty` inside a function must not walk the function's arguments. */
        const char all_args[] = {SHELL_QUOTE_MARK_DOUBLE, '$', '@', SHELL_QUOTE_MARK_DOUBLE, '\0'};
        ShellWord *word = shellCreateWord(all_args, false, true, true, false, line, column);
        if (word && !parserWordArrayAppend(&value_words, word)) {
            shellFreeWord(word);
        }
    }

    if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
    }

    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_DO);
    parseLinebreak(parser);
    body = parseCompoundListUntil(parser, SHELL_TOKEN_DONE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    parserRequireCommand(parser, body);
    shellParserConsume(parser, SHELL_TOKEN_DONE);

    ShellLoop *loop = shellCreateForLoop(name_word, &value_words, body);
    if (!loop) {
        parserWordArrayFree(&value_words);
        shellFreeWord(name_word);
        return NULL;
    }
    ShellCommand *command = shellCreateLoopCommand(loop);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseCaseClause(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    /* Any word will do, `case in in` and all. */
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
    shellParserAdvance(parser);
    ShellWord *subject = parseWordToken(parser);
    parseLinebreak(parser);
    parserScheduleRuleMask(parser, RULE_MASK_FOR_LIST);
    parserReclassifyCurrentToken(parser, RULE_MASK_FOR_LIST);
    shellParserConsume(parser, SHELL_TOKEN_IN);
    parseLinebreak(parser);

    ShellCase *case_stmt = shellCreateCase(subject);
    if (!case_stmt) {
        return NULL;
    }

    while (!parser->had_error && parser->current.type != SHELL_TOKEN_ESAC && parser->current.type != SHELL_TOKEN_EOF) {
        if (parser->current.type == SHELL_TOKEN_NEWLINE) {
            parseLinebreak(parser);
            continue;
        }
        ShellCaseClause *clause = shellCreateCaseClause(parser->current.line, parser->current.column);
        if (!clause) {
            shellFreeCase(case_stmt);
            return NULL;
        }
        parserScheduleRuleMask(parser, RULE_MASK_CASE_PATTERN);
        if (parser->current.type == SHELL_TOKEN_LPAREN) {
            shellParserAdvance(parser);
            parserScheduleRuleMask(parser, RULE_MASK_CASE_PATTERN);
        }
        /* Every pattern is a word, `if)` too, and there is one at least. */
        parserReclassifyCurrentToken(parser, RULE_MASK_CASE_PATTERN);
        while (!parser->had_error) {
            ShellWord *pattern = parseWordToken(parser);
            if (pattern) {
                populateWordExpansions(pattern);
                shellCaseClauseAddPattern(clause, pattern);
            }
            if (!shellParserMatch(parser, SHELL_TOKEN_PIPE)) {
                break;
            }
            parserScheduleRuleMask(parser, RULE_MASK_CASE_PATTERN);
        }
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserConsume(parser, SHELL_TOKEN_RPAREN);
        parseLinebreak(parser);
        ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_DSEMI, SHELL_TOKEN_ESAC, SHELL_TOKEN_EOF);
        clause->body = body;
        shellCaseAddClause(case_stmt, clause);
        if (parser->current.type == SHELL_TOKEN_SEMI_AMP) {
            clause->terminator = SHELL_CASE_FALL_THROUGH;
        } else if (parser->current.type == SHELL_TOKEN_DSEMI_AMP) {
            clause->terminator = SHELL_CASE_TEST_NEXT;
        }
        if (parser->current.type == SHELL_TOKEN_DSEMI || parser->current.type == SHELL_TOKEN_SEMI_AMP ||
            parser->current.type == SHELL_TOKEN_DSEMI_AMP) {
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
            shellParserAdvance(parser);
            parseLinebreak(parser);
        } else {
            break;
        }
    }
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_ESAC);

    ShellCommand *command = shellCreateCaseCommand(case_stmt);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

/* Any plain word names a function, as in my-func() and my.func(). */
static bool tokenIsFunctionName(const ShellToken *token) {
    if (token->type == SHELL_TOKEN_NAME) {
        return true;
    }
    if (token->type != SHELL_TOKEN_WORD || token->reserved_candidate || !token->lexeme || !token->lexeme[0] ||
        token->contains_parameter_expansion || token->contains_command_substitution ||
        token->contains_arithmetic_expansion) {
        return false;
    }
    return strchr(token->lexeme, SHELL_QUOTE_MARK_SINGLE) == NULL &&
           strchr(token->lexeme, SHELL_QUOTE_MARK_DOUBLE) == NULL && strchr(token->lexeme, '=') == NULL;
}

static bool parserIsFunctionDefinitionStart(ShellParser *parser) {
    if (!parser) {
        return false;
    }
    const ShellToken *token = &parser->current;
    if (!tokenIsFunctionName(token)) {
        return false;
    }

    ShellLexer lookahead = parser->lexer;
    shellLexerSetRuleMask(&lookahead, RULE_MASK_COMMAND_START);
    ShellToken next = shellNextToken(&lookahead);
    applyLexicalRules(&next);
    bool result = false;
    if (next.type == SHELL_TOKEN_LPAREN) {
        shellFreeToken(&next);
        shellLexerSetRuleMask(&lookahead, RULE_MASK_COMMAND_START);
        ShellToken closing = shellNextToken(&lookahead);
        applyLexicalRules(&closing);
        if (closing.type == SHELL_TOKEN_RPAREN) {
            result = true;
        }
        shellFreeToken(&closing);
    } else {
        shellFreeToken(&next);
    }
    return result;
}

/* A compound command, with nothing else allowed: `f() echo x` has no body. */
static ShellCommand *parseFunctionBody(ShellParser *parser) {
    parseLinebreak(parser);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    if (parser->had_error) {
        return NULL;
    }
    parserSplitDoubleParenUnlessArithmetic(parser);
    if (parser->current.type == SHELL_TOKEN_DLPAREN) {
        return parseArithmeticCommand(parser);
    }
    if ((parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_NAME) &&
        tokenIsBareWord(&parser->current, "[[")) {
        return parseSimpleCommand(parser);
    }
    ShellCommand *body = parseCompoundCommand(parser);
    if (!body) {
        parserUnexpectedToken(parser);
    }
    return body;
}

static ShellCommand *parseFunctionDefinitionFromName(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    int line = parser->current.line;
    int column = parser->current.column;
    const char *name_lexeme = parser->current.lexeme ? parser->current.lexeme : "";
    char *name_copy = strdup(name_lexeme);
    if (!name_copy) {
        parserErrorAt(parser, &parser->current, "Out of memory");
        return NULL;
    }

    parserScheduleRuleMask(parser, RULE_MASK_FUNCTION_NAME);
    parserReclassifyCurrentToken(parser, RULE_MASK_FUNCTION_NAME);
    shellParserAdvance(parser);

    if (parser->current.type == SHELL_TOKEN_LPAREN) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserConsume(parser, SHELL_TOKEN_RPAREN);
    } else {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    }

    ShellCommand *body_command = parseFunctionBody(parser);
    if (!body_command) {
        free(name_copy);
        return NULL;
    }

    ShellProgram *body_program = shellCreateProgram();
    shellProgramAddCommand(body_program, body_command);

    ShellFunction *function = shellCreateFunction(name_copy, "", body_program);
    free(name_copy);
    ShellCommand *command = shellCreateFunctionCommand(function);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseFunctionDefinition(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    parserScheduleRuleMask(parser, RULE_MASK_FUNCTION_NAME);
    parserReclassifyCurrentToken(parser, RULE_MASK_FUNCTION_NAME);
    if (!tokenIsFunctionName(&parser->current)) {
        parserUnexpectedToken(parser);
        return NULL;
    }
    shellParserAdvance(parser);
    const char *name_lexeme = parser->previous.lexeme ? parser->previous.lexeme : "";
    char *name_copy = strdup(name_lexeme);
    if (!name_copy) {
        parserErrorAt(parser, &parser->previous, "Out of memory");
        return NULL;
    }

    if (parser->current.type == SHELL_TOKEN_LPAREN) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserConsume(parser, SHELL_TOKEN_RPAREN);
    } else {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    }

    ShellCommand *body_command = parseFunctionBody(parser);
    if (!body_command) {
        free(name_copy);
        return NULL;
    }

    ShellProgram *body_program = shellCreateProgram();
    shellProgramAddCommand(body_program, body_command);

    ShellFunction *function = shellCreateFunction(name_copy, "", body_program);
    free(name_copy);
    ShellCommand *command = shellCreateFunctionCommand(function);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellWord *parseWordToken(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    if (parser->current.type != SHELL_TOKEN_WORD && parser->current.type != SHELL_TOKEN_ASSIGNMENT_WORD &&
        parser->current.type != SHELL_TOKEN_NAME && parser->current.type != SHELL_TOKEN_PARAMETER) {
        parserUnexpectedToken(parser);
        return NULL;
    }
    ShellToken token = parser->current;
    unsigned int scheduled_mask = parser->next_rule_mask;
    unsigned int continuation_mask = RULE_MASK_COMMAND_CONTINUATION;
    if ((parser->current.rule_mask & SHELL_LEXER_RULE_4) != 0u || (scheduled_mask & SHELL_LEXER_RULE_4) != 0u) {
        continuation_mask = RULE_MASK_CASE_PATTERN;
    }
    parserScheduleRuleMask(parser, continuation_mask);
    shellParserAdvance(parser);
    ShellWord *word = shellCreateWord(token.lexeme ? token.lexeme : "", token.single_quoted, token.double_quoted,
                                      token.contains_parameter_expansion, token.contains_arithmetic_expansion,
                                      token.line, token.column);
    if (word) {
        if (token.type == SHELL_TOKEN_ASSIGNMENT_WORD) {
            word->is_assignment = true;
        }
        if (token.contains_command_substitution) {
            word->has_command_substitution = true;
        }
        if (token.type == SHELL_TOKEN_PARAMETER && token.lexeme && token.lexeme[0] == '$' && token.lexeme[1]) {
            shellWordAddExpansion(word, token.lexeme + 1);
        }
    }
    return word;
}

static ShellRedirection *parseRedirection(ShellParser *parser, bool *strip_tabs_out) {
    if (strip_tabs_out) {
        *strip_tabs_out = false;
    }
    if (!parser) {
        return NULL;
    }
    ShellToken number_token = parser->current;
    char *io_number_copy = NULL;
    if (number_token.type == SHELL_TOKEN_IO_NUMBER && number_token.lexeme) {
        io_number_copy = strdup(number_token.lexeme);
    }
    ShellTokenType redir_type = SHELL_TOKEN_ERROR;
    if (parser->current.type == SHELL_TOKEN_IO_NUMBER) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        redir_type = parser->current.type;
    } else if (parser->current.type == SHELL_TOKEN_LT || parser->current.type == SHELL_TOKEN_GT ||
               parser->current.type == SHELL_TOKEN_DGREAT || parser->current.type == SHELL_TOKEN_DLESS ||
               parser->current.type == SHELL_TOKEN_DLESSDASH || parser->current.type == SHELL_TOKEN_TLESS ||
               parser->current.type == SHELL_TOKEN_GREATAND ||
               parser->current.type == SHELL_TOKEN_LESSAND || parser->current.type == SHELL_TOKEN_LESSGREAT ||
               parser->current.type == SHELL_TOKEN_CLOBBER || parser->current.type == SHELL_TOKEN_ANDGREAT ||
               parser->current.type == SHELL_TOKEN_ANDDGREAT) {
        redir_type = parser->current.type;
    } else {
        return NULL;
    }

    if (number_token.type != SHELL_TOKEN_IO_NUMBER) {
        number_token.lexeme = NULL;
    }

    bool strip_tabs = (redir_type == SHELL_TOKEN_DLESSDASH);
    if (strip_tabs_out) {
        *strip_tabs_out = strip_tabs;
    }

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    ShellRedirectionType type;
    switch (redir_type) {
        case SHELL_TOKEN_LT: type = SHELL_REDIRECT_INPUT; break;
        case SHELL_TOKEN_GT: type = SHELL_REDIRECT_OUTPUT; break;
        case SHELL_TOKEN_DGREAT: type = SHELL_REDIRECT_APPEND; break;
        case SHELL_TOKEN_DLESS:
        case SHELL_TOKEN_DLESSDASH: type = SHELL_REDIRECT_HEREDOC; break;
        case SHELL_TOKEN_TLESS: type = SHELL_REDIRECT_HERE_STRING; break;
        case SHELL_TOKEN_LESSAND: type = SHELL_REDIRECT_DUP_INPUT; break;
        case SHELL_TOKEN_GREATAND: type = SHELL_REDIRECT_DUP_OUTPUT; break;
        case SHELL_TOKEN_LESSGREAT: type = SHELL_REDIRECT_READ_WRITE; break;
        case SHELL_TOKEN_CLOBBER: type = SHELL_REDIRECT_CLOBBER; break;
        case SHELL_TOKEN_ANDGREAT: type = SHELL_REDIRECT_OUTPUT_AND_ERROR; break;
        case SHELL_TOKEN_ANDDGREAT: type = SHELL_REDIRECT_APPEND_OUTPUT_AND_ERROR; break;
        default: return NULL;
    }

    /* A file can be called `in` or `do`: the target was read as if a command could start. */
    unsigned int target_mask = type == SHELL_REDIRECT_HEREDOC ? RULE_MASK_HEREDOC_DELIMITER : RULE_MASK_REDIRECT_TARGET;
    parserScheduleRuleMask(parser, target_mask);
    parserReclassifyCurrentToken(parser, target_mask);
    ShellWord *target = parseWordToken(parser);
    if (!target) {
        return NULL;
    }
    populateWordExpansions(target);

    const char *io_number_text = io_number_copy ? io_number_copy : number_token.lexeme;
    ShellRedirection *redir = shellCreateRedirection(type, io_number_text, target, number_token.line,
                                                    number_token.column);
    free(io_number_copy);
    if (redir && (type == SHELL_REDIRECT_DUP_INPUT || type == SHELL_REDIRECT_DUP_OUTPUT)) {
        char *dup_copy = parserCopyWordWithoutMarkers(target);
        const char *text = dup_copy ? dup_copy : (target && target->text ? target->text : "");
        if (text && text[0] == '&') {
            shellRedirectionSetDupTarget(redir, text + 1);
        } else {
            shellRedirectionSetDupTarget(redir, text);
        }
        free(dup_copy);
    }
    if (redir && type == SHELL_REDIRECT_HERE_STRING) {
        char *literal = parserCopyWordWithoutMarkers(target);
        shellRedirectionSetHereStringLiteral(redir, literal ? literal : "");
        free(literal);
    }
    return redir;
}

static bool parseDollarCommandSubstitution(const char *text, size_t start, size_t *out_span,
                                           char **out_command) {
    if (!text || start >= strlen(text) || text[start] != '$' || text[start + 1] != '(') {
        return false;
    }
    size_t i = shellScanDollarParen(text, strlen(text), start + 2);
    if (i == SHELL_SCAN_UNTERMINATED) {
        return false;
    }
    size_t len = i - start;
    if (out_span) {
        *out_span = len;
    }
    if (out_command) {
        *out_command = normalizeDollarCommand(text + start + 2, len - 3);
    }
    return true;
}

static bool parseBacktickCommandSubstitution(const char *text, size_t start, size_t *out_span,
                                             char **out_command) {
    if (!text || text[start] != '`') {
        return false;
    }
    size_t i = start + 1;
    while (text[i]) {
        if (text[i] == '`') {
            break;
        }
        if (text[i] == '\\' && text[i + 1]) {
            i += 2;
            continue;
        }
        i++;
    }
    if (text[i] != '`') {
        return false;
    }
    if (out_span) {
        *out_span = i - start + 1;
    }
    if (out_command) {
        *out_command = normalizeBacktickCommand(text + start + 1, i - start - 1);
    }
    return true;
}

static char *normalizeDollarCommand(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            out[j++] = '\'';
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            out[j++] = '"';
            continue;
        }
        if (c == '\\' && i + 1 < len && command[i + 1] == '\n') {
            i++;
            continue;
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static char *normalizeBacktickCommand(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            out[j++] = '\'';
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            out[j++] = '"';
            continue;
        }
        if (c == '\\' && i + 1 < len) {
            char next = command[i + 1];
            if (next == '\n') {
                i++;
                continue;
            }
            if (next == '\\' || next == '`' || next == '$') {
                out[j++] = next;
                i++;
                continue;
            }
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static void populateWordExpansions(ShellWord *word) {
    if (!word || !word->text) {
        return;
    }
    const char *text = word->text;
    size_t len = strlen(text);
    size_t i = 0;
    bool in_single = false;
    bool in_double = false;
    while (i < len) {
        char c = text[i];
        /* Expansion skips these, so one recorded here would hold up the real ones after it. */
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            in_single = !in_single;
            i++;
            continue;
        }
        if (in_single) {
            i++;
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            in_double = !in_double;
            i++;
            continue;
        }
        if (!in_double && (c == '<' || c == '>') && i + 1 < len && text[i + 1] == '(') {
            size_t stop = shellScanDollarParen(text, len, i + 2);
            i = stop == SHELL_SCAN_UNTERMINATED ? len : stop;
            continue;
        }
        if (c == '$') {
            size_t span = 0;
            char *command = NULL;
            if (i + 2 < len && text[i + 1] == '(' && text[i + 2] == '(') {
                size_t stop = shellScanArithmetic(text, len, i + 3);
                if (stop != SHELL_SCAN_UNTERMINATED) {
                    /* The expander runs what is in there itself. */
                    i = stop;
                    continue;
                }
            }
            if (i + 1 < len && text[i + 1] == '(') {
                if (parseDollarCommandSubstitution(text, i, &span, &command)) {
                    if (command) {
                        shellWordAddCommandSubstitution(word, SHELL_COMMAND_SUBSTITUTION_DOLLAR, command, span);
                        free(command);
                    }
                    i += span;
                    continue;
                }
            }
            size_t j = i + 1;
            if (j < len && text[j] == '{') {
                j++;
                const char *start = text + j;
                size_t name_len = 0;
                if (j < len && text[j] && text[j] != '}') {
                    consumeShellIdentifier(start, len - j, &name_len, true);
                    j += name_len;
                }
                if (name_len > 0) {
                    char *name = (char *)malloc(name_len + 1);
                    if (name) {
                        memcpy(name, start, name_len);
                        name[name_len] = '\0';
                        shellWordAddExpansion(word, name);
                        free(name);
                    }
                }
                size_t stop = shellScanBraceParam(text, len, i + 2, false);
                i = stop == SHELL_SCAN_UNTERMINATED ? len : stop;
                continue;
            } else {
                const char *start = text + j;
                size_t name_len = 0;
                consumeShellIdentifier(start, len - j, &name_len, true);
                j += name_len;
                if (name_len > 0) {
                    char *name = (char *)malloc(name_len + 1);
                    if (name) {
                        memcpy(name, start, name_len);
                        name[name_len] = '\0';
                        shellWordAddExpansion(word, name);
                        free(name);
                    }
                }
                i = j;
                continue;
            }
        } else if (c == '`') {
            size_t span = 0;
            char *command = NULL;
            if (parseBacktickCommandSubstitution(text, i, &span, &command)) {
                if (command) {
                    shellWordAddCommandSubstitution(word, SHELL_COMMAND_SUBSTITUTION_BACKTICK, command, span);
                    free(command);
                }
                i += span;
                continue;
            }
        }
        i++;
    }
}
