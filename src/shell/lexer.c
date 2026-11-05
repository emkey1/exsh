#include "shell/lexer.h"
#include "core/utils.h"
#include "shell/quote_markers.h"
#include "shell/word_scan.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *shellCopyRange(const char *src, size_t start, size_t end) {
    if (end <= start) {
        return strdup("");
    }
    size_t len = end - start;
    char *out = (char *)malloc(len + 1);
    if (!out) {
        fprintf(stderr, "shell lexer: allocation failed\n");
        return NULL;
    }
    memcpy(out, src + start, len);
    out[len] = '\0';
    return out;
}

static int peekChar(const ShellLexer *lexer) {
    if (!lexer || lexer->pos >= lexer->length) {
        return EOF;
    }
    return (unsigned char)lexer->src[lexer->pos];
}

static int advanceChar(ShellLexer *lexer) {
    if (!lexer || lexer->pos >= lexer->length) {
        return EOF;
    }
    unsigned char c = lexer->src[lexer->pos++];
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
        lexer->at_line_start = true;
    } else {
        lexer->column++;
        lexer->at_line_start = false;
    }
    return c;
}

/* {name}> f or {name[i]}> f: the word has to touch the operator, and `{fd}<(cmd)` is a word as
 * `a<(cmd)` is. Gives the length of the word, or 0. */
static size_t redirectionVariableLength(const ShellLexer *lexer) {
    const char *text = lexer->src + lexer->pos;
    size_t left = lexer->length - lexer->pos;
    size_t i = 1;
    if (left < 4 || text[0] != '{' || !(isalpha((unsigned char)text[1]) || text[1] == '_')) {
        return 0;
    }
    while (i < left && (isalnum((unsigned char)text[i]) || text[i] == '_')) {
        i++;
    }
    if (i < left && text[i] == '[') {
        size_t close = i + 1;
        /* The runtime gets this text in a list of its own, between `;` and `=`. */
        while (close < left && text[close] != ']' && !strchr(" \t\n;=&|<>(){}\"'`\\", text[close])) {
            close++;
        }
        if (close >= left || text[close] != ']' || close == i + 1) {
            return 0;
        }
        i = close + 1;
    }
    if (i + 1 >= left || text[i] != '}' || (text[i + 1] != '<' && text[i + 1] != '>')) {
        return 0;
    }
    if (i + 2 < left && text[i + 2] == '(') {
        return 0;
    }
    return i + 1;
}

static void skipCommentToNewline(ShellLexer *lexer) {
    if (!lexer) {
        return;
    }
    // The caller peeked '#' but has not consumed it yet.
    advanceChar(lexer); // consume '#'
    while (true) {
        int next = peekChar(lexer);
        if (next == '\n' || next == EOF) {
            break;
        }
        advanceChar(lexer);
    }
}

static void skipInlineWhitespace(ShellLexer *lexer) {
    while (true) {
        int c = peekChar(lexer);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            advanceChar(lexer);
            continue;
        }
        if (c == '#') {
            // Shell comments: skip until newline but preserve the newline itself.
            skipCommentToNewline(lexer);
            continue;
        }
        break;
    }
}

static bool isValidNameLexeme(const char *lexeme, size_t length) {
    if (!lexeme || length == 0) {
        return false;
    }
    size_t prefix_len = 0;
    if (!consumeShellIdentifier(lexeme, length, &prefix_len, false)) {
        return false;
    }
    const unsigned char backslash = 92;
    const unsigned char single_quote = 39;
    const unsigned char double_quote = 34;
    bool in_brackets = false;
    bool in_single = false;
    bool in_double = false;
    for (size_t i = prefix_len; i < length; ++i) {
        unsigned char ch = (unsigned char)lexeme[i];
        if (in_single) {
            if (ch == backslash && i + 1 < length) {
                ++i;
                continue;
            }
            if (ch == single_quote) {
                in_single = false;
            }
            continue;
        }
        if (in_double) {
            if (ch == backslash && i + 1 < length) {
                ++i;
                continue;
            }
            if (ch == double_quote) {
                in_double = false;
            }
            continue;
        }
        if (in_brackets) {
            if (ch == backslash && i + 1 < length) {
                ++i;
                continue;
            }
            if (ch == single_quote) {
                in_single = true;
                continue;
            }
            if (ch == double_quote) {
                in_double = true;
                continue;
            }
            if (ch == 93) { /* ']' */
                in_brackets = false;
                continue;
            }
            if (ch == 91) { /* '[' */
                return false;
            }
            continue;
        }
        if (ch == 91) {
            in_brackets = true;
            continue;
        }
        if (ch != 91) {
            return false;
        }
    }
    return !in_brackets && !in_single && !in_double;
}



static ShellTokenType checkReservedWord(const char *lexeme);

static ShellToken makeSimpleToken(ShellLexer *lexer, ShellTokenType type, const char *lexeme, size_t len) {
    ShellToken tok;
    tok.type = type;
    tok.base_type = type;
    tok.reserved_type = type;
    tok.length = len;
    tok.single_quoted = false;
    tok.double_quoted = false;
    tok.contains_parameter_expansion = false;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = false;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer ? lexer->rule_mask : 0u;
    tok.command_starts = lexer ? (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0 : false;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.lexeme = NULL;
    if (lexeme && len > 0) {
        tok.lexeme = shellCopyRange(lexeme, 0, len);
    } else if (lexeme) {
        tok.lexeme = strdup("");
    }
    return tok;
}

static ShellToken makeTokenFromRange(ShellLexer *lexer, ShellTokenType type, size_t start, size_t end,
                                     bool singleQuoted, bool doubleQuoted, bool hasParam, bool hasArithmetic) {
    ShellToken tok;
    tok.type = type;
    tok.base_type = type;
    tok.reserved_type = type;
    tok.length = (end > start) ? (end - start) : 0;
    tok.single_quoted = singleQuoted;
    tok.double_quoted = doubleQuoted;
    tok.contains_parameter_expansion = hasParam;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = hasArithmetic;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer ? lexer->rule_mask : 0u;
    tok.command_starts = lexer ? (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0 : false;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.lexeme = (lexer && lexer->src) ? shellCopyRange(lexer->src, start, end) : NULL;
    return tok;
}

static ShellToken makeEOFToken(ShellLexer *lexer) {
    ShellToken tok;
    tok.type = SHELL_TOKEN_EOF;
    tok.base_type = SHELL_TOKEN_EOF;
    tok.reserved_type = SHELL_TOKEN_EOF;
    tok.lexeme = strdup("");
    tok.length = 0;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.single_quoted = false;
    tok.double_quoted = false;
    tok.contains_parameter_expansion = false;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = false;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer ? lexer->rule_mask : 0u;
    tok.command_starts = false;
    return tok;
}

static ShellToken makeErrorToken(ShellLexer *lexer, const char *message) {
    ShellToken tok;
    tok.type = SHELL_TOKEN_ERROR;
    tok.base_type = SHELL_TOKEN_ERROR;
    tok.reserved_type = SHELL_TOKEN_ERROR;
    tok.lexeme = message ? strdup(message) : strdup("lexer error");
    tok.length = tok.lexeme ? strlen(tok.lexeme) : 0;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.single_quoted = false;
    tok.double_quoted = false;
    tok.contains_parameter_expansion = false;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = false;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer ? lexer->rule_mask : 0u;
    tok.command_starts = lexer ? (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0 : false;
    return tok;
}

/* The source ended inside something that `closer` would finish, opened on `line`. A caller at
 * a prompt can read another line; anything else reports it. The first one counts. */
static void lexerMarkUnterminated(ShellLexer *lexer, char closer, int line) {
    lexer->unterminated = true;
    if (!lexer->unterminated_closer) {
        lexer->unterminated_closer = closer;
        lexer->unterminated_line = line;
    }
}

/* Bash reports `$(` where the text ended, and everything else where it opened. */
static int lexerLineAtEnd(const ShellLexer *lexer) {
    int line = lexer->line;
    for (size_t i = lexer->pos; i < lexer->length; ++i) {
        if (lexer->src[i] == '\n') {
            line++;
        }
    }
    return (lexer->length > 0 && lexer->src[lexer->length - 1] != '\n') ? line + 1 : line;
}

/* A lexer error that is really "the source stopped here". */
static ShellToken makeUnterminatedToken(ShellLexer *lexer, const char *message, char closer, int line) {
    lexerMarkUnterminated(lexer, closer, line);
    return makeErrorToken(lexer, message);
}

static ShellToken scanParameter(ShellLexer *lexer) {
    size_t start = lexer->pos;
    int first = advanceChar(lexer); // consume '$'
    (void)first;
    int c = peekChar(lexer);
    bool command_sub = false;
    bool arithmetic = false;
    if (c == '{') {
        size_t stop = shellScanBraceParam(lexer->src, lexer->length, lexer->pos + 1, false);
        if (stop == SHELL_SCAN_UNTERMINATED) {
            int line = lexer->line;
            lexer->pos = lexer->length;
            return makeUnterminatedToken(lexer, "Unterminated parameter expansion", '}', line);
        }
        while (lexer->pos < stop) {
            advanceChar(lexer);
        }
    } else if (c == '(') {
        size_t stop = SHELL_SCAN_UNTERMINATED;
        if (lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '(') {
            stop = shellScanArithmetic(lexer->src, lexer->length, lexer->pos + 2);
            arithmetic = stop != SHELL_SCAN_UNTERMINATED;
        }
        if (!arithmetic) {
            stop = shellScanDollarParen(lexer->src, lexer->length, lexer->pos + 1);
            command_sub = true;
        }
        if (stop == SHELL_SCAN_UNTERMINATED) {
            bool arith_open = lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '(';
            int line = arith_open ? lexer->line : lexerLineAtEnd(lexer);
            lexer->pos = lexer->length;
            return makeUnterminatedToken(lexer, "Unterminated command substitution", ')', line);
        }
        while (lexer->pos < stop) {
            advanceChar(lexer);
        }
    } else {
        /* '#' must be listed here: without it the '$' is consumed alone, the
         * word ends, and the trailing '#' is then taken for a comment, which
         * silently swallows the rest of the line (e.g. `if [ $# -eq 0 ]`). */
        if (c == '?' || c == '@' || c == '*' || c == '!' || c == '-' || c == '$' || c == '#') {
            advanceChar(lexer);
        } else if (isdigit(c)) {
            /* A positional parameter. The identifier rule below refuses a leading
             * digit, which left the '$' as a word of its own (`$1` printed `$ 1`). */
            while (isalnum(peekChar(lexer)) || peekChar(lexer) == '_') {
                advanceChar(lexer);
            }
        } else {
            size_t name_bytes = 0;
            const char *name_start = lexer->src + lexer->pos;
            size_t remaining = lexer->length - lexer->pos;
            if (consumeShellIdentifier(name_start, remaining, &name_bytes, true)) {
                for (size_t i = 0; i < name_bytes; ++i) {
                    advanceChar(lexer);
                }
            }
        }
    }
    size_t end = lexer->pos;
    ShellToken tok = makeTokenFromRange(lexer, SHELL_TOKEN_PARAMETER, start, end, false, false, true, arithmetic);
    tok.contains_command_substitution = command_sub;
    return tok;
}

static bool isOperatorDelimiter(int c) {
    switch (c) {
        case '\n':
        case ';':
        case '&':
        case '|':
        case '(': case ')':
        case '{': case '}':
        case '<': case '>':
            return true;
        default:
            return false;
    }
}

static bool isStructuralWordCandidate(int c) {
    switch (c) {
        case '(':
        case ')':
        case '{':
        case '}':
            return true;
        default:
            return false;
    }
}

static bool lexerAllowsStructuralWordLiterals(const ShellLexer *lexer) {
    if (!lexer) {
        return false;
    }

    unsigned int mask = lexer->rule_mask;
    if ((mask & SHELL_LEXER_RULE_4) != 0) {
        // Case patterns rely on ')' remaining a structural token even though
        // they are parsed outside command-start contexts.
        return false;
    }

    return true;
}

static bool appendWordByte(char **buffer, size_t *len, size_t *cap, char byte) {
    if (*len + 1 >= *cap) {
        size_t new_cap = *cap ? *cap * 2 : 32;
        char *tmp = (char *)realloc(*buffer, new_cap);
        if (!tmp) {
            return false;
        }
        *buffer = tmp;
        *cap = new_cap;
    }
    (*buffer)[(*len)++] = byte;
    return true;
}

/* The body of $'...', from just past the opening quote: as literal as single quotes. */
static bool appendAnsiCQuoted(ShellLexer *lexer, char **buffer, size_t *len, size_t *cap) {
    size_t room = lexer->length - lexer->pos + 1;
    char *decoded = (char *)malloc(room);
    if (!decoded) {
        return false;
    }
    size_t decoded_len = 0;
    size_t stop = shellScanAnsiC(lexer->src, lexer->length, lexer->pos, decoded, &decoded_len);
    if (stop == SHELL_SCAN_UNTERMINATED) {
        lexerMarkUnterminated(lexer, '\'', lexer->line);
        stop = lexer->length;
    }
    while (lexer->pos < stop) {
        advanceChar(lexer);
    }
    bool ok = appendWordByte(buffer, len, cap, SHELL_QUOTE_MARK_SINGLE);
    for (size_t i = 0; ok && i < decoded_len; ++i) {
        if (decoded[i] == SHELL_QUOTE_MARK_SINGLE || decoded[i] == SHELL_QUOTE_MARK_DOUBLE ||
            decoded[i] == SHELL_QUOTE_MARK_BYTE) {
            ok = appendWordByte(buffer, len, cap, SHELL_QUOTE_MARK_BYTE);
        }
        ok = ok && appendWordByte(buffer, len, cap, decoded[i]);
    }
    free(decoded);
    return ok && appendWordByte(buffer, len, cap, SHELL_QUOTE_MARK_SINGLE);
}

/* Inside [[ ]] words end on the operators, not on the shell's usual set. After
 * =~ parentheses and bars are regex text, and a space inside the parentheses too. */
static bool doubleBracketWordEnds(const ShellLexer *lexer, int c, int *regexDepth) {
    if (lexer->dbracket_regex) {
        if (c == '(') {
            (*regexDepth)++;
            return false;
        }
        if (c == ')' && *regexDepth > 0) {
            (*regexDepth)--;
            return false;
        }
        if (c == '|' || *regexDepth > 0) {
            return false;
        }
    }
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v' || c == '\n' ||
           c == '(' || c == ')' || c == '&' || c == '|' || c == '<' || c == '>' || c == ';';
}

static ShellToken scanWord(ShellLexer *lexer) {
    bool singleQuoted = false;
    bool doubleQuoted = false;
    bool sawSingleQuotedSegment = false;
    bool sawDoubleQuotedSegment = false;
    bool sawUnquotedSegment = false;
    bool hasParam = false;
    bool hasCommand = false;

    bool inBacktick = false;
    bool hasArithmetic = false;
    int quoteLine = lexer->line;
    int arrayLine = lexer->line;

    bool allowStructuralLiterals = lexerAllowsStructuralWordLiterals(lexer);

    bool inArrayLiteral = false;
    int arrayParenDepth = 0;
    int literalParenDepth = 0;
    int regexDepth = 0;
    int extglobDepth = 0;

    size_t firstUnquotedEq = (size_t)-1;

    char *buffer = NULL;
    size_t bufLen = 0;
    size_t bufCap = 0;

    while (true) {
        int c = peekChar(lexer);
        if (c == EOF) {
            break;
        }
        /* <(cmd) and >(cmd) are words, and the text inside stays raw until they run. */
        if ((c == '<' || c == '>') && !singleQuoted && !doubleQuoted && !inBacktick && !lexer->dbracket &&
            lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '(') {
            size_t stop = shellScanDollarParen(lexer->src, lexer->length, lexer->pos + 2);
            if (stop == SHELL_SCAN_UNTERMINATED) {
                lexerMarkUnterminated(lexer, ')', lexer->line);
                stop = lexer->length;
            }
            while (lexer->pos < stop) {
                if (!appendWordByte(&buffer, &bufLen, &bufCap, (char)advanceChar(lexer))) {
                    free(buffer);
                    return makeErrorToken(lexer, "Out of memory while scanning word");
                }
            }
            hasParam = true;
            sawUnquotedSegment = true;
            continue;
        }
        bool startingArrayLiteral = false;
        /* An extended pattern is part of the word, parentheses, bars, spaces and all.
         * `f+()` is not one: nothing is in the parentheses. */
        bool opensExtglob = extglobDepth == 0 && c == '(' && bufLen > 0 && !singleQuoted && !doubleQuoted &&
                            !inBacktick && strchr("@?*+!", buffer[bufLen - 1]) && lexer->pos + 1 < lexer->length &&
                            lexer->src[lexer->pos + 1] != ')' && !inArrayLiteral;
        if (opensExtglob) {
            extglobDepth = 1;
        } else if (extglobDepth > 0 && !singleQuoted && !doubleQuoted) {
            if (c == '(') {
                extglobDepth++;
            } else if (c == ')') {
                extglobDepth--;
            }
        } else if (!singleQuoted && !doubleQuoted && !inBacktick && lexer->dbracket) {
            if (doubleBracketWordEnds(lexer, c, &regexDepth)) {
                break;
            }
        } else if (!singleQuoted && !doubleQuoted && !inBacktick) {
            if (inArrayLiteral && arrayParenDepth > 0 && c == '#' && lexer->pos > 0 &&
                strchr(" \t\r\n(", lexer->src[lexer->pos - 1])) {
                skipCommentToNewline(lexer);
                continue;
            }
            if (!inArrayLiteral && firstUnquotedEq != (size_t)-1 && c == '(' &&
                bufLen == firstUnquotedEq + 1) {
                startingArrayLiteral = true;
            }

            bool allowArrayWhitespace = inArrayLiteral && arrayParenDepth > 0;
            if (!allowArrayWhitespace) {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                    break;
                }
                if (c == '\n') {
                    break;
                }
            }

            if (!(startingArrayLiteral || (inArrayLiteral && arrayParenDepth > 0 && (c == '(' || c == ')')))) {
                /* `{` and `}` are reserved only as whole words: foo{a,b} and x={a,b}. */
                bool treat_as_operator = isOperatorDelimiter(c) && c != '{' && c != '}';
                if (treat_as_operator && inArrayLiteral && arrayParenDepth > 0 && c == '\n') {
                    treat_as_operator = false;
                }
                if (treat_as_operator && (c == '(' || c == ')')) {
                    if (allowStructuralLiterals && (lexer->rule_mask & SHELL_LEXER_RULE_1) == 0) {
                        /* `(echo s)`: the ')' is not part of the word. One the
                         * word opened itself stays in it, as in `^(ab)c$`. */
                        treat_as_operator = c == ')' && lexer->rparen_closes && literalParenDepth == 0;
                        if (c == '(') {
                            literalParenDepth++;
                        } else if (c == ')' && literalParenDepth > 0) {
                            literalParenDepth--;
                        }
                    }
                }
                if (treat_as_operator) {
                    break;
                }
            }
        }
        advanceChar(lexer);
        bool escapedChar = false;
        if (!singleQuoted && !doubleQuoted && !inBacktick && firstUnquotedEq != (size_t)-1) {
            if (startingArrayLiteral) {
                inArrayLiteral = true;
                arrayParenDepth = 1;
                arrayLine = lexer->line;
            } else if (inArrayLiteral) {
                if (c == '(') {
                    arrayParenDepth++;
                } else if (c == ')') {
                    if (arrayParenDepth > 0) {
                        arrayParenDepth--;
                        if (arrayParenDepth == 0) {
                            inArrayLiteral = false;
                        }
                    }
                }
            }
        }
        if (c == '\\') {
            int next = peekChar(lexer);
            if (singleQuoted || next == EOF) {
                if (next == EOF && !singleQuoted) {
                    /* A backslash as the last byte of the source is a line
                     * continuation whose continuation line never arrived. */
                    lexer->unterminated = true;
                }
                c = '\\';
            } else if (next == '\n') {
                advanceChar(lexer);
                continue;
            } else if (inBacktick) {
                /* normalizeBacktickCommand takes this level off when the command
                 * is built, so it stays as written. Only \" belongs to the double
                 * quotes around the backticks. */
                advanceChar(lexer);
                if (!(doubleQuoted && next == '"') && !appendWordByte(&buffer, &bufLen, &bufCap, '\\')) {
                    free(buffer);
                    return makeErrorToken(lexer, "Out of memory while scanning word");
                }
                c = next;
                escapedChar = true;
            } else if (!doubleQuoted || next == '\\' || next == '"' || next == '$' || next == '`') {
                advanceChar(lexer);
                c = next;
                escapedChar = true;
            } else {
                c = '\\';
            }
        } else if (c == '\'' && !doubleQuoted) {
            bool enteringSingle = !singleQuoted;
            singleQuoted = !singleQuoted;
            quoteLine = enteringSingle ? lexer->line : quoteLine;
            if (bufLen + 1 >= bufCap) {
                bufCap = bufCap ? bufCap * 2 : 32;
                char *tmp = (char *)realloc(buffer, bufCap);
                if (!tmp) {
                    free(buffer);
                    return makeErrorToken(lexer, "Out of memory while scanning word");
                }
                buffer = tmp;
            }
            buffer[bufLen++] = SHELL_QUOTE_MARK_SINGLE;
            if (enteringSingle) {
                sawSingleQuotedSegment = true;
            }
            continue;
        } else if (c == '"' && !singleQuoted) {
            bool enteringDouble = !doubleQuoted;
            doubleQuoted = !doubleQuoted;
            quoteLine = enteringDouble ? lexer->line : quoteLine;
            if (bufLen + 1 >= bufCap) {
                bufCap = bufCap ? bufCap * 2 : 32;
                char *tmp = (char *)realloc(buffer, bufCap);
                if (!tmp) {
                    free(buffer);
                    return makeErrorToken(lexer, "Out of memory while scanning word");
                }
                buffer = tmp;
            }
            buffer[bufLen++] = SHELL_QUOTE_MARK_DOUBLE;
            if (enteringDouble) {
                sawDoubleQuotedSegment = true;
            }
            continue;
        } else if (c == '$' && !singleQuoted) {
            int next = peekChar(lexer);
            if (!doubleQuoted && next == '"') {
                /* $"text" is "text": no locale to translate it for. */
                continue;
            }
            if (!doubleQuoted && next == '\'') {
                advanceChar(lexer);
                if (!appendAnsiCQuoted(lexer, &buffer, &bufLen, &bufCap)) {
                    free(buffer);
                    return makeErrorToken(lexer, "Out of memory while scanning word");
                }
                sawSingleQuotedSegment = true;
                continue;
            }
            hasParam = true;
            if (!appendWordByte(&buffer, &bufLen, &bufCap, (char)c)) {
                free(buffer);
                return makeErrorToken(lexer, "Out of memory while scanning word");
            }
            if (next == '{' || next == '(') {
                size_t stop = SHELL_SCAN_UNTERMINATED;
                if (next == '{') {
                    stop = shellScanBraceParam(lexer->src, lexer->length, lexer->pos + 1, doubleQuoted);
                } else {
                    if (lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '(') {
                        stop = shellScanArithmetic(lexer->src, lexer->length, lexer->pos + 2);
                        hasArithmetic = hasArithmetic || stop != SHELL_SCAN_UNTERMINATED;
                    }
                    if (stop == SHELL_SCAN_UNTERMINATED) {
                        hasCommand = true;
                        stop = shellScanDollarParen(lexer->src, lexer->length, lexer->pos + 1);
                    }
                }
                if (stop == SHELL_SCAN_UNTERMINATED) {
                    bool brace = lexer->pos < lexer->length && lexer->src[lexer->pos] == '{';
                    bool arith_open = lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '(';
                    lexerMarkUnterminated(lexer, brace ? '}' : ')',
                                          (brace || arith_open) ? lexer->line : lexerLineAtEnd(lexer));
                    stop = lexer->length;
                }
                while (lexer->pos < stop) {
                    if (!appendWordByte(&buffer, &bufLen, &bufCap, (char)advanceChar(lexer))) {
                        free(buffer);
                        return makeErrorToken(lexer, "Out of memory while scanning word");
                    }
                }
                continue;
            } else {
                /* See the note in scanParameter: '#' belongs to the special
                 * parameter set, or `$#` degrades into a comment. */
                if (next == '?' || next == '@' || next == '*' || next == '!' || next == '-' || next == '$' || next == '#') {
                    if (!appendWordByte(&buffer, &bufLen, &bufCap, (char)advanceChar(lexer))) {
                        free(buffer);
                        return makeErrorToken(lexer, "Out of memory while scanning word");
                    }
                } else {
                    size_t name_bytes = 0;
                    const char *name_start = lexer->src + lexer->pos;
                    size_t remaining = lexer->length - lexer->pos;
                    if (consumeShellIdentifier(name_start, remaining, &name_bytes, true)) {
                        for (size_t consumed = 0; consumed < name_bytes; ++consumed) {
                            if (!appendWordByte(&buffer, &bufLen, &bufCap, (char)advanceChar(lexer))) {
                                free(buffer);
                                return makeErrorToken(lexer, "Out of memory while scanning word");
                            }
                        }
                    }
                }
                continue;
            }
        }
        if (c == '`' && !singleQuoted && !escapedChar) {
            if (!inBacktick) {
                hasCommand = true;
                inBacktick = true;
                quoteLine = lexer->line;
            } else {
                inBacktick = false;
            }
        }

        /* An escaped character is as literal as a single-quoted one, and expansion,
         * field splitting and globbing need to know: `\$x`, `a\ b`, `\*`. */
        bool literalChar = escapedChar && !inBacktick;
        if ((literalChar && !appendWordByte(&buffer, &bufLen, &bufCap, SHELL_QUOTE_MARK_SINGLE)) ||
            !appendWordByte(&buffer, &bufLen, &bufCap, (char)c) ||
            (literalChar && !appendWordByte(&buffer, &bufLen, &bufCap, SHELL_QUOTE_MARK_SINGLE))) {
            free(buffer);
            return makeErrorToken(lexer, "Out of memory while scanning word");
        }
        if (firstUnquotedEq == (size_t)-1 && c == '=' && !singleQuoted && !doubleQuoted && !inBacktick &&
            !escapedChar) {
            firstUnquotedEq = bufLen - 1;
        }
        if (singleQuoted) {
            sawSingleQuotedSegment = true;
        } else if (doubleQuoted) {
            sawDoubleQuotedSegment = true;
        } else {
            sawUnquotedSegment = true;
        }
    }

    if ((singleQuoted || doubleQuoted || inBacktick) && lexer->pos >= lexer->length) {
        /* The word ran to the end of the source with a quote still open. */
        lexerMarkUnterminated(lexer, singleQuoted ? '\'' : (doubleQuoted ? '"' : '`'), quoteLine);
    } else if (inArrayLiteral && arrayParenDepth > 0 && lexer->pos >= lexer->length) {
        lexerMarkUnterminated(lexer, ')', arrayLine);
        lexer->unterminated_array = true;
    }

    if (buffer && bufLen < bufCap) {
        buffer[bufLen] = '\0';
    } else if (buffer) {
        char *tmp = (char *)realloc(buffer, bufLen + 1);
        if (!tmp) {
            free(buffer);
            return makeErrorToken(lexer, "Out of memory finalizing word");
        }
        buffer = tmp;
        buffer[bufLen] = '\0';
    }

    ShellToken tok;
    tok.type = SHELL_TOKEN_WORD;
    tok.base_type = SHELL_TOKEN_WORD;
    tok.reserved_type = SHELL_TOKEN_WORD;
    tok.length = bufLen;
    tok.lexeme = buffer ? buffer : strdup("");
    tok.line = lexer->line;
    tok.column = lexer->column;
    tok.single_quoted = sawSingleQuotedSegment && !sawDoubleQuotedSegment && !sawUnquotedSegment;
    tok.double_quoted = sawDoubleQuotedSegment && !sawSingleQuotedSegment && !sawUnquotedSegment;
    tok.contains_parameter_expansion = hasParam;
    tok.contains_command_substitution = hasCommand;
    tok.contains_arithmetic_expansion = hasArithmetic;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer->rule_mask;
    tok.command_starts = (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0;

    ShellTokenType reserved = SHELL_TOKEN_WORD;
    if (tok.lexeme) {
        reserved = checkReservedWord(tok.lexeme);
    }
    if (reserved != SHELL_TOKEN_WORD) {
        tok.reserved_candidate = true;
        tok.reserved_type = reserved;
        tok.type = reserved;
    }

    if (firstUnquotedEq != (size_t)-1 && firstUnquotedEq > 0 && tok.lexeme && firstUnquotedEq < tok.length) {
        size_t nameEnd = firstUnquotedEq;
        if (nameEnd > 1 && tok.lexeme[nameEnd - 1] == '+') {
            nameEnd--; /* x+=2 */
        }
        if (isValidNameLexeme(tok.lexeme, nameEnd)) {
            tok.assignment_candidate = true;
            if (!tok.reserved_candidate) {
                tok.type = SHELL_TOKEN_ASSIGNMENT_WORD;
            }
        }
    }

    if (!tok.assignment_candidate && reserved == SHELL_TOKEN_WORD && tok.lexeme && firstUnquotedEq == (size_t)-1) {
        if (isValidNameLexeme(tok.lexeme, tok.length)) {
            tok.name_candidate = true;
        }
    }

    return tok;
}

void shellInitLexer(ShellLexer *lexer, const char *source) {
    if (!lexer) {
        return;
    }
    lexer->src = source ? source : "";
    lexer->length = source ? strlen(source) : 0;
    lexer->pos = 0;
    lexer->line = 1;
    lexer->column = 1;
    lexer->at_line_start = true;
    lexer->rule_mask = SHELL_LEXER_RULE_1;
    lexer->unterminated = false;
    lexer->unterminated_closer = '\0';
    lexer->unterminated_line = 0;
    lexer->unterminated_array = false;
    lexer->token_start = 0;
    lexer->final_newline_given = false;
    lexer->rparen_closes = false;
    lexer->dbracket = false;
    lexer->dbracket_regex = false;
}

void shellFreeToken(ShellToken *token) {
    if (!token) {
        return;
    }
    if (token->lexeme) {
        free(token->lexeme);
        token->lexeme = NULL;
    }
}

static ShellTokenType checkReservedWord(const char *lexeme) {
    if (!lexeme) {
        return SHELL_TOKEN_WORD;
    }
    if (strcmp(lexeme, "function") == 0) return SHELL_TOKEN_FUNCTION;
    if (strcmp(lexeme, "if") == 0) return SHELL_TOKEN_IF;
    if (strcmp(lexeme, "then") == 0) return SHELL_TOKEN_THEN;
    if (strcmp(lexeme, "elif") == 0) return SHELL_TOKEN_ELIF;
    if (strcmp(lexeme, "else") == 0) return SHELL_TOKEN_ELSE;
    if (strcmp(lexeme, "fi") == 0) return SHELL_TOKEN_FI;
    if (strcmp(lexeme, "for") == 0) return SHELL_TOKEN_FOR;
    if (strcmp(lexeme, "while") == 0) return SHELL_TOKEN_WHILE;
    if (strcmp(lexeme, "until") == 0) return SHELL_TOKEN_UNTIL;
    if (strcmp(lexeme, "do") == 0) return SHELL_TOKEN_DO;
    if (strcmp(lexeme, "done") == 0) return SHELL_TOKEN_DONE;
    if (strcmp(lexeme, "in") == 0) return SHELL_TOKEN_IN;
    if (strcmp(lexeme, "case") == 0) return SHELL_TOKEN_CASE;
    if (strcmp(lexeme, "esac") == 0) return SHELL_TOKEN_ESAC;
    return SHELL_TOKEN_WORD;
}

/* Between [[ and ]] newlines separate like spaces and the operators are words the
 * builtin reads, so that && and < stop meaning what they mean outside. */
static ShellToken scanDoubleBracketToken(ShellLexer *lexer) {
    while (true) {
        int c = peekChar(lexer);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v' || c == '\n') {
            advanceChar(lexer);
        } else if (c == '\\' && lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '\n') {
            advanceChar(lexer);
            advanceChar(lexer);
        } else if (c == '#') {
            skipCommentToNewline(lexer);
        } else {
            break;
        }
    }
    int c = peekChar(lexer);
    if (c == EOF) {
        return makeEOFToken(lexer);
    }
    lexer->token_start = lexer->pos;
    bool regex = lexer->dbracket_regex;
    if (!regex || (c != '(' && c != '|')) {
        size_t rest = lexer->length - lexer->pos;
        const char *at = lexer->src + lexer->pos;
        if (rest >= 2 && (memcmp(at, "&&", 2) == 0 || memcmp(at, "||", 2) == 0)) {
            advanceChar(lexer);
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_WORD, at, 2);
        }
        /* Whatever else ends a word has to be consumed too, or this never moves on. */
        if (c == '(' || c == ')' || c == '<' || c == '>' || c == '&' || c == '|' || c == ';') {
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_WORD, at, 1);
        }
    }
    ShellToken word = scanWord(lexer);
    lexer->dbracket_regex = false;
    if (!word.lexeme) {
        return makeErrorToken(lexer, "Failed to allocate word");
    }
    return word;
}

/* `{` and `}` are operators only when they stand alone. */
static bool braceStandsAlone(const ShellLexer *lexer) {
    int after = lexer->pos + 1 < lexer->length ? (unsigned char)lexer->src[lexer->pos + 1] : EOF;
    return after == EOF || isspace(after) || (isOperatorDelimiter(after) && after != '{' && after != '}');
}

ShellToken shellNextToken(ShellLexer *lexer) {
    if (!lexer) {
        return makeEOFToken(NULL);
    }
    if (lexer->dbracket) {
        return scanDoubleBracketToken(lexer);
    }

    while (true) {
        int c = peekChar(lexer);
        if (c == EOF) {
            /* A last line with no newline ends in one all the same, as it does for bash, so
             * `for` at the end of a string says the same as `for` at the end of a line. */
            if (lexer->length > 0 && lexer->src[lexer->length - 1] != '\n' && !lexer->final_newline_given) {
                lexer->final_newline_given = true;
                lexer->token_start = lexer->length;
                lexer->line++;
                lexer->column = 1;
                return makeSimpleToken(lexer, SHELL_TOKEN_NEWLINE, "\n", 1);
            }
            return makeEOFToken(lexer);
        }
        if (c == '\n') {
            lexer->token_start = lexer->pos;
            advanceChar(lexer);
            ShellToken tok = makeSimpleToken(lexer, SHELL_TOKEN_NEWLINE, "\n", 1);
            return tok;
        }
        /* A line continuation BETWEEN tokens is not a token: splice the lines
         * and carry on.  Leaving it to scanWord instead started a word on the
         * backslash, consumed the newline and then stopped on the next line's
         * indent -- producing an EMPTY word.  Harmless nearly everywhere, but
         * a case pattern list took that empty word for a pattern and then
         * reported "Expected ')' after case pattern" on the real one.
         * A backslash at the very end of the source is NOT this: it is an
         * unfinished continuation, and scanWord flags it as such. */
        if (c == '\\' && lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '\n') {
            advanceChar(lexer);
            advanceChar(lexer);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            advanceChar(lexer);
            continue;
        }
        if (c == '#') {
            // Skip comment but leave newline for subsequent handling.
            skipCommentToNewline(lexer);
            continue;
        }
        break;
    }

    skipInlineWhitespace(lexer);

    int c = peekChar(lexer);
    if (c == EOF) {
        return makeEOFToken(lexer);
    }
    lexer->token_start = lexer->pos;

    if (c == '{') {
        size_t variable_length = redirectionVariableLength(lexer);
        if (variable_length > 0) {
            size_t start = lexer->pos;
            for (size_t i = 0; i < variable_length; ++i) {
                advanceChar(lexer);
            }
            return makeTokenFromRange(lexer, SHELL_TOKEN_IO_NUMBER, start, lexer->pos, false, false, false, false);
        }
    }

    bool command_starts = (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0;
    if (!command_starts && lexerAllowsStructuralWordLiterals(lexer) && isStructuralWordCandidate(c)) {
        if (c == ')' && lexer->rparen_closes) {
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_RPAREN, ")", 1);
        }
        ShellToken word = scanWord(lexer);
        if (!word.lexeme) {
            return makeErrorToken(lexer, "Failed to allocate word");
        }
        return word;
    }

    if (isdigit(c)) {
        size_t start = lexer->pos;
        while (isdigit(peekChar(lexer))) {
            advanceChar(lexer);
        }
        int next = peekChar(lexer);
        if (next == '<' || next == '>') {
            size_t end = lexer->pos;
            ShellToken tok = makeTokenFromRange(lexer, SHELL_TOKEN_IO_NUMBER, start, end, false, false, false, false);
            return tok;
        }
        lexer->pos = start;
        lexer->column -= (int)(lexer->pos - start);
    }

    switch (c) {
        case ';': {
            advanceChar(lexer);
            if (peekChar(lexer) == ';') {
                advanceChar(lexer);
                if (peekChar(lexer) == '&') {
                    advanceChar(lexer);
                    return makeSimpleToken(lexer, SHELL_TOKEN_DSEMI_AMP, ";;&", 3);
                }
                return makeSimpleToken(lexer, SHELL_TOKEN_DSEMI, ";;", 2);
            }
            if (peekChar(lexer) == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_SEMI_AMP, ";&", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_SEMICOLON, ";", 1);
        }
        case '&': {
            advanceChar(lexer);
            int next = peekChar(lexer);
            if (next == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_AND_AND, "&&", 2);
            }
            if (next == '>') {
                advanceChar(lexer);
                if (peekChar(lexer) == '>') {
                    advanceChar(lexer);
                    return makeSimpleToken(lexer, SHELL_TOKEN_ANDDGREAT, "&>>", 3);
                }
                return makeSimpleToken(lexer, SHELL_TOKEN_ANDGREAT, "&>", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_AMPERSAND, "&", 1);
        }
        case '!': {
            if ((lexer->rule_mask & SHELL_LEXER_RULE_1) != 0) {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_BANG, "!", 1);
            }
            break;
        }
        case '|': {
            advanceChar(lexer);
            int next = peekChar(lexer);
            if (next == '|') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_OR_OR, "||", 2);
            }
            if (next == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_PIPE_AMP, "|&", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_PIPE, "|", 1);
        }
        case '(': {
            advanceChar(lexer);
            if (peekChar(lexer) == '(') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_DLPAREN, "((", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_LPAREN, "(", 1);
        }
        case ')': {
            /* Always one at a time: `( (true))` closes two subshells, and the
             * parser reads the end of an arithmetic command from the source. */
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_RPAREN, ")", 1);
        }
        case '{': {
            if (!braceStandsAlone(lexer)) {
                break;
            }
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_LBRACE, "{", 1);
        }
        case '}': {
            if (!braceStandsAlone(lexer)) {
                break;
            }
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_RBRACE, "}", 1);
        }
        case '<': {
            if (lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '(') {
                break;
            }
            advanceChar(lexer);
            int next = peekChar(lexer);
            if (next == '<') {
                advanceChar(lexer);
                int third = peekChar(lexer);
                if (third == '<') {
                    advanceChar(lexer);
                    return makeSimpleToken(lexer, SHELL_TOKEN_TLESS, "<<<", 3);
                }
                if (third == '-') {
                    advanceChar(lexer);
                    return makeSimpleToken(lexer, SHELL_TOKEN_DLESSDASH, "<<-", 3);
                }
                return makeSimpleToken(lexer, SHELL_TOKEN_DLESS, "<<", 2);
            }
            if (next == '>') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_LESSGREAT, "<>", 2);
            }
            if (next == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_LESSAND, "<&", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_LT, "<", 1);
        }
        case '>': {
            if (lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '(') {
                break;
            }
            advanceChar(lexer);
            int next = peekChar(lexer);
            if (next == '>') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_DGREAT, ">>", 2);
            }
            if (next == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_GREATAND, ">&", 2);
            }
            if (next == '|') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_CLOBBER, ">|", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_GT, ">", 1);
        }
        case '$': {
            /* A parameter with more of the word after it ($HOME/bin, ${x}y, $a$b)
             * is one word. scanParameter's token ends at the parameter and the
             * rest became the next argument, so scanWord takes the whole word, as
             * it does when the '$' is not first. */
            ShellLexer rewind = *lexer;
            ShellToken param = scanParameter(lexer);
            if (param.type == SHELL_TOKEN_PARAMETER) {
                int after = peekChar(lexer);
                if (after != EOF && !isspace(after) &&
                    (!isOperatorDelimiter(after) || after == '{' || after == '}')) {
                    shellFreeToken(&param);
                    *lexer = rewind;
                    break;
                }
            }
            return param;
        }
        default:
            break;
    }

    ShellToken word = scanWord(lexer);
    if (!word.lexeme) {
        return makeErrorToken(lexer, "Failed to allocate word");
    }
    return word;
}

const char *shellTokenTypeName(ShellTokenType type) {
    switch (type) {
        case SHELL_TOKEN_WORD: return "WORD";
        case SHELL_TOKEN_NAME: return "NAME";
        case SHELL_TOKEN_ASSIGNMENT_WORD: return "ASSIGNMENT_WORD";
        case SHELL_TOKEN_PARAMETER: return "PARAM";
        case SHELL_TOKEN_IO_NUMBER: return "IO_NUMBER";
        case SHELL_TOKEN_NEWLINE: return "NEWLINE";
        case SHELL_TOKEN_SEMICOLON: return "SEMICOLON";
        case SHELL_TOKEN_AMPERSAND: return "AMPERSAND";
        case SHELL_TOKEN_BANG: return "BANG";
        case SHELL_TOKEN_PIPE: return "PIPE";
        case SHELL_TOKEN_PIPE_AMP: return "PIPE_AMP";
        case SHELL_TOKEN_AND_AND: return "AND_AND";
        case SHELL_TOKEN_OR_OR: return "OR_OR";
        case SHELL_TOKEN_LPAREN: return "LPAREN";
        case SHELL_TOKEN_RPAREN: return "RPAREN";
        case SHELL_TOKEN_DLPAREN: return "DLPAREN";
        case SHELL_TOKEN_LBRACE: return "LBRACE";
        case SHELL_TOKEN_RBRACE: return "RBRACE";
        case SHELL_TOKEN_FUNCTION: return "FUNCTION";
        case SHELL_TOKEN_IF: return "IF";
        case SHELL_TOKEN_THEN: return "THEN";
        case SHELL_TOKEN_ELIF: return "ELIF";
        case SHELL_TOKEN_ELSE: return "ELSE";
        case SHELL_TOKEN_FI: return "FI";
        case SHELL_TOKEN_FOR: return "FOR";
        case SHELL_TOKEN_WHILE: return "WHILE";
        case SHELL_TOKEN_UNTIL: return "UNTIL";
        case SHELL_TOKEN_DO: return "DO";
        case SHELL_TOKEN_DONE: return "DONE";
        case SHELL_TOKEN_IN: return "IN";
        case SHELL_TOKEN_CASE: return "CASE";
        case SHELL_TOKEN_ESAC: return "ESAC";
        case SHELL_TOKEN_DSEMI: return "DSEMI";
        case SHELL_TOKEN_SEMI_AMP: return "SEMI_AMP";
        case SHELL_TOKEN_DSEMI_AMP: return "DSEMI_AMP";
        case SHELL_TOKEN_LT: return "LT";
        case SHELL_TOKEN_GT: return "GT";
        case SHELL_TOKEN_DGREAT: return "DGREAT";
        case SHELL_TOKEN_DLESS: return "DLESS";
        case SHELL_TOKEN_DLESSDASH: return "DLESSDASH";
        case SHELL_TOKEN_TLESS: return "TLESS";
        case SHELL_TOKEN_LESSGREAT: return "LESSGREAT";
        case SHELL_TOKEN_GREATAND: return "GREATAND";
        case SHELL_TOKEN_LESSAND: return "LESSAND";
        case SHELL_TOKEN_CLOBBER: return "CLOBBER";
        case SHELL_TOKEN_ANDGREAT: return "ANDGREAT";
        case SHELL_TOKEN_ANDDGREAT: return "ANDDGREAT";
        case SHELL_TOKEN_COMMENT: return "COMMENT";
        case SHELL_TOKEN_EOF: return "EOF";
        case SHELL_TOKEN_ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

/* The token just read is a number or {name} touching `<` or `>`. Inside `[[ ]]` the lexer hands the
 * operator over as a word, and bash's grammar still takes these for redirections, so they are an
 * error there. */
bool shellLexerAtRedirectionWord(const ShellLexer *lexer, const ShellToken *token) {
    if (!lexer || !token || !token->lexeme || token->length == 0 || token->single_quoted || token->double_quoted ||
        lexer->pos < token->length || lexer->pos >= lexer->length) {
        return false;
    }
    char next = lexer->src[lexer->pos];
    if (next != '<' && next != '>') {
        return false;
    }
    if (lexer->pos + 1 < lexer->length && lexer->src[lexer->pos + 1] == '(') {
        return false;
    }
    bool digits = true;
    for (size_t i = 0; i < token->length && digits; ++i) {
        digits = isdigit((unsigned char)token->lexeme[i]) != 0;
    }
    if (digits) {
        return true;
    }
    ShellLexer at_word = *lexer;
    at_word.pos -= token->length;
    return redirectionVariableLength(&at_word) == token->length;
}

void shellLexerSetRuleMask(ShellLexer *lexer, unsigned int mask) {
    if (!lexer) {
        return;
    }
    lexer->rule_mask = mask;
}

unsigned int shellLexerGetRuleMask(const ShellLexer *lexer) {
    return lexer ? lexer->rule_mask : 0u;
}
