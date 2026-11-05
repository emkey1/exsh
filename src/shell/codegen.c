#include "shell/codegen.h"
#include "shell/builtins.h"
#include "shell/word_encoding.h"
#include "shell/quote_markers.h"
#include "shell/function.h"
#include "vm/string_sentinels.h"
#include "core/utils.h"
#include "vm/vm.h"
#include "core/globals.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void encodeHexDigits(size_t value, size_t width, char *out) {
    static const char digits[] = "0123456789ABCDEF";
    if (!out) {
        return;
    }
    for (size_t i = 0; i < width; ++i) {
        size_t shift = (width - 1 - i) * 4;
        size_t nibble;
        if (shift >= sizeof(size_t) * 8) {
            nibble = 0;
        } else {
            nibble = (value >> shift) & 0xF;
        }
        out[i] = digits[nibble & 0xF];
    }
}

static size_t buildCommandSubstitutionMetadata(const ShellWord *word, char **out_meta) {
    if (out_meta) {
        *out_meta = NULL;
    }
    if (!word) {
        return 0;
    }
    size_t count = word->command_substitutions.count;
    size_t meta_len = 4; // always store count
    for (size_t i = 0; i < count; ++i) {
        const ShellCommandSubstitution *sub = &word->command_substitutions.items[i];
        size_t cmd_len = sub->command ? strlen(sub->command) : 0;
        meta_len += 1 + 6 + 6 + cmd_len;
    }
    if (meta_len == 0) {
        return 0;
    }
    char *meta = (char *)malloc(meta_len);
    if (!meta) {
        return 0;
    }
    size_t offset = 0;
    encodeHexDigits(count, 4, meta + offset);
    offset += 4;
    for (size_t i = 0; i < count; ++i) {
        const ShellCommandSubstitution *sub = &word->command_substitutions.items[i];
        meta[offset++] = (sub->style == SHELL_COMMAND_SUBSTITUTION_BACKTICK) ? 'B' : 'D';
        encodeHexDigits(sub->span_length, 6, meta + offset);
        offset += 6;
        size_t cmd_len = sub->command ? strlen(sub->command) : 0;
        encodeHexDigits(cmd_len, 6, meta + offset);
        offset += 6;
        if (cmd_len > 0 && sub->command) {
            memcpy(meta + offset, sub->command, cmd_len);
        }
        offset += cmd_len;
    }
    if (out_meta) {
        *out_meta = meta;
    } else {
        free(meta);
    }
    return meta_len;
}

static int addStringConstant(BytecodeChunk *chunk, const char *str) {
    Value val = makeString(str ? str : "");
    int index = addConstantToChunk(chunk, &val);
    freeValue(&val);
    return index;
}

static int addBuiltinNameConstant(BytecodeChunk *chunk,
                                  const char *encoded_name,
                                  const char *canonical_hint) {
    if (!chunk) {
        return -1;
    }
    int name_index = addStringConstant(chunk, encoded_name);
    if (name_index < 0) {
        return name_index;
    }

    if (getBuiltinLowercaseIndex(chunk, name_index) >= 0) {
        return name_index;
    }

    const char *lower_source = (canonical_hint && *canonical_hint) ? canonical_hint : encoded_name;
    char lowered[MAX_SYMBOL_LENGTH];
    strncpy(lowered, lower_source ? lower_source : "", sizeof(lowered) - 1);
    lowered[sizeof(lowered) - 1] = '\0';
    toLowerString(lowered);

    Value lower_val = makeString(lowered);
    int lower_index = addConstantToChunk(chunk, &lower_val);
    freeValue(&lower_val);
    setBuiltinLowercaseIndex(chunk, name_index, lower_index);
    return name_index;
}

static void emitConstantOperand(BytecodeChunk *chunk, int constant_index, int line) {
    if (constant_index < 0) {
        fprintf(stderr, "shell codegen error: negative constant index.\n");
        return;
    }
    if (constant_index <= 0xFF) {
        writeBytecodeChunk(chunk, CONSTANT, line);
        writeBytecodeChunk(chunk, (uint8_t)constant_index, line);
        return;
    }
    if (constant_index <= 0xFFFF) {
        writeBytecodeChunk(chunk, CONSTANT16, line);
        emitShort(chunk, (uint16_t)constant_index, line);
        return;
    }
    fprintf(stderr, "shell codegen error: constant table overflow (%d).\n", constant_index);
}

static void emitPushString(BytecodeChunk *chunk, const char *value, int line) {
    int idx = addStringConstant(chunk, value);
    emitConstantOperand(chunk, idx, line);
}

static char *encodeWord(const ShellWord *word) {
    if (!word) {
        return strdup("");
    }
    uint8_t flags = 0;
    if (word->single_quoted) {
        flags |= SHELL_WORD_FLAG_SINGLE_QUOTED;
    }
    if (word->double_quoted) {
        flags |= SHELL_WORD_FLAG_DOUBLE_QUOTED;
    }
    if (word->has_parameter_expansion) {
        flags |= SHELL_WORD_FLAG_HAS_PARAM;
    }
    if (word->has_command_substitution || word->command_substitutions.count > 0) {
        flags |= SHELL_WORD_FLAG_HAS_COMMAND;
    }
    if (word->has_arithmetic_expansion) {
        flags |= SHELL_WORD_FLAG_HAS_ARITHMETIC;
    }
    if (word->is_assignment) {
        flags |= SHELL_WORD_FLAG_ASSIGNMENT;
    }
    const char *text = word->text ? word->text : "";
    size_t len = strlen(text);
    char *meta = NULL;
    size_t meta_len = buildCommandSubstitutionMetadata(word, &meta);
    char len_hex[6];
    encodeHexDigits(meta_len, 6, len_hex);
    size_t total_len = 2 + 6 + meta_len + len + 1;
    char *encoded = (char *)malloc(total_len);
    if (!encoded) {
        free(meta);
        return NULL;
    }
    encoded[0] = SHELL_WORD_ENCODE_PREFIX;
    encoded[1] = (char)(flags + 1);
    memcpy(encoded + 2, len_hex, 6);
    if (meta_len > 0 && meta) {
        memcpy(encoded + 8, meta, meta_len);
    }
    memcpy(encoded + 8 + meta_len, text, len + 1);
    free(meta);
    return encoded;
}

static char *encodeHexString(const char *input) {
    if (!input) {
        return strdup("");
    }
    size_t len = strlen(input);
    char *encoded = (char *)malloc(len * 2 + 1);
    if (!encoded) {
        return NULL;
    }
    static const char hex_digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; ++i) {
        unsigned char byte = (unsigned char)input[i];
        encoded[2 * i] = hex_digits[(byte >> 4) & 0xF];
        encoded[2 * i + 1] = hex_digits[byte & 0xF];
    }
    encoded[len * 2] = '\0';
    return encoded;
}

static void emitPushWord(BytecodeChunk *chunk, const ShellWord *word, int line) {
    char *encoded = encodeWord(word);
    if (!encoded) {
        emitPushString(chunk, "", line);
        return;
    }
    emitPushString(chunk, encoded, line);
    free(encoded);
}

#define LOOP_COND_KIND_NONE   0
#define LOOP_COND_KIND_TEST   1
#define LOOP_COND_KIND_BRACKET 2
#define LOOP_COND_KIND_COLON  3
#define LOOP_COND_KIND_TRUE   4
#define LOOP_COND_KIND_FALSE  5
#define LOOP_COND_KIND_ARITH  6

typedef struct {
    int kind;
    char **encoded_words;
    size_t word_count;
    char *arith_expression;
} LoopConditionSpec;

static void initLoopConditionSpec(LoopConditionSpec *spec) {
    if (!spec) {
        return;
    }
    spec->kind = LOOP_COND_KIND_NONE;
    spec->encoded_words = NULL;
    spec->word_count = 0;
    spec->arith_expression = NULL;
}

static void freeLoopConditionSpec(LoopConditionSpec *spec) {
    if (!spec) {
        return;
    }
    if (spec->encoded_words) {
        for (size_t i = 0; i < spec->word_count; ++i) {
            free(spec->encoded_words[i]);
        }
        free(spec->encoded_words);
    }
    spec->encoded_words = NULL;
    spec->word_count = 0;
    if (spec->arith_expression) {
        free(spec->arith_expression);
        spec->arith_expression = NULL;
    }
    spec->kind = LOOP_COND_KIND_NONE;
}

static bool wordIsLiteralCommand(const ShellWord *word) {
    if (!word || !word->text) {
        return false;
    }
    if (word->single_quoted || word->double_quoted) {
        return false;
    }
    if (word->has_parameter_expansion || word->has_command_substitution || word->has_arithmetic_expansion) {
        return false;
    }
    return true;
}

static bool gatherLoopConditionSpec(const ShellLoop *loop, LoopConditionSpec *out_spec) {
    if (!loop || !out_spec) {
        return false;
    }
    initLoopConditionSpec(out_spec);
    if (loop->is_for || loop->is_cstyle_for) {
        return false;
    }
    const ShellCommand *cond = loop->condition;
    if (!cond) {
        return false;
    }
    if (cond->type == SHELL_COMMAND_PIPELINE && cond->data.pipeline && cond->data.pipeline->command_count == 1) {
        const ShellPipeline *pipeline = cond->data.pipeline;
        if (pipeline && pipeline->commands && pipeline->commands[0]) {
            cond = pipeline->commands[0];
        }
    }
    if (cond->exec.runs_in_background) {
        return false;
    }
    if (cond->type == SHELL_COMMAND_SIMPLE) {
        const ShellWordArray *words = &cond->data.simple.words;
        if (cond->redirections.count > 0 || words->count == 0) {
            return false;
        }
        const ShellWord *first = words->items[0];
        if (!wordIsLiteralCommand(first)) {
            return false;
        }
        const char *cmd = first->text ? first->text : "";
        int kind = LOOP_COND_KIND_NONE;
        if (strcmp(cmd, "test") == 0) {
            if (words->count > 4) {
                return false;
            }
            kind = LOOP_COND_KIND_TEST;
        } else if (strcmp(cmd, "[") == 0) {
            if (words->count < 2 || words->count > 5) {
                return false;
            }
            const ShellWord *last = words->items[words->count - 1];
            if (!last || !last->text || strcmp(last->text, "]") != 0) {
                return false;
            }
            if (!wordIsLiteralCommand(last)) {
                return false;
            }
            kind = LOOP_COND_KIND_BRACKET;
        } else if (strcmp(cmd, ":") == 0) {
            if (words->count != 1) {
                return false;
            }
            kind = LOOP_COND_KIND_COLON;
        } else if (strcmp(cmd, "true") == 0) {
            if (words->count != 1) {
                return false;
            }
            kind = LOOP_COND_KIND_TRUE;
        } else if (strcmp(cmd, "false") == 0) {
            if (words->count != 1) {
                return false;
            }
            kind = LOOP_COND_KIND_FALSE;
        } else {
            return false;
        }
        if (kind == LOOP_COND_KIND_TEST || kind == LOOP_COND_KIND_BRACKET) {
            for (size_t i = 1; i < words->count; ++i) {
                const ShellWord *word = words->items[i];
                if (!word || !word->text) {
                    continue;
                }
                if (strcmp(word->text, "-a") == 0 || strcmp(word->text, "-o") == 0) {
                    return false;
                }
            }
            char **encoded = (char **)calloc(words->count, sizeof(char *));
            if (!encoded) {
                return false;
            }
            for (size_t i = 0; i < words->count; ++i) {
                encoded[i] = encodeWord(words->items[i]);
                if (!encoded[i]) {
                    for (size_t j = 0; j < i; ++j) {
                        free(encoded[j]);
                    }
                    free(encoded);
                    return false;
                }
            }
            out_spec->encoded_words = encoded;
            out_spec->word_count = words->count;
        }
        out_spec->kind = kind;
        return true;
    } else if (cond->type == SHELL_COMMAND_ARITHMETIC) {
        const char *expr = cond->data.arithmetic.expression ? cond->data.arithmetic.expression : "";
        if (cond->redirections.count > 0) {
            return false;
        }
        out_spec->arith_expression = strdup(expr);
        if (!out_spec->arith_expression) {
            return false;
        }
        out_spec->kind = LOOP_COND_KIND_ARITH;
        return true;
    }
    return false;
}

typedef enum {
    LOOP_BODY_KIND_NONE = 0,
    LOOP_BODY_KIND_COLON,
    LOOP_BODY_KIND_TRUE,
    LOOP_BODY_KIND_FALSE,
    LOOP_BODY_KIND_TEST,
    LOOP_BODY_KIND_BRACKET,
    LOOP_BODY_KIND_ARITH,
    LOOP_BODY_KIND_TEST_ARITH,
    LOOP_BODY_KIND_BRACKET_ARITH
} LoopBodyKind;

typedef struct {
    LoopBodyKind kind;
    char **test_words;
    size_t test_word_count;
    char *arith_expression;
} LoopBodySpec;

static void initLoopBodySpec(LoopBodySpec *spec) {
    if (!spec) {
        return;
    }
    spec->kind = LOOP_BODY_KIND_NONE;
    spec->test_words = NULL;
    spec->test_word_count = 0;
    spec->arith_expression = NULL;
}

static void freeLoopBodySpec(LoopBodySpec *spec) {
    if (!spec) {
        return;
    }
    if (spec->test_words) {
        for (size_t i = 0; i < spec->test_word_count; ++i) {
            free(spec->test_words[i]);
        }
        free(spec->test_words);
        spec->test_words = NULL;
    }
    spec->test_word_count = 0;
    if (spec->arith_expression) {
        free(spec->arith_expression);
        spec->arith_expression = NULL;
    }
    spec->kind = LOOP_BODY_KIND_NONE;
}

typedef enum {
    BODY_CMD_NONE = 0,
    BODY_CMD_COLON,
    BODY_CMD_TRUE,
    BODY_CMD_FALSE,
    BODY_CMD_TEST,
    BODY_CMD_BRACKET,
    BODY_CMD_ARITH,
    BODY_CMD_OTHER
} BodyCommandType;

static const ShellCommand *unwrapPipelineCommand(const ShellCommand *cmd) {
    if (!cmd || cmd->type != SHELL_COMMAND_PIPELINE || !cmd->data.pipeline) {
        return cmd;
    }
    const ShellPipeline *pipeline = cmd->data.pipeline;
    if (!pipeline || pipeline->command_count != 1 || !pipeline->commands || !pipeline->commands[0]) {
        return cmd;
    }
    return pipeline->commands[0];
}

static bool captureArithmeticExpression(const ShellCommand *cmd, LoopBodySpec *spec) {
    if (!cmd || cmd->type != SHELL_COMMAND_ARITHMETIC || !spec) {
        return false;
    }
    const char *expr = cmd->data.arithmetic.expression ? cmd->data.arithmetic.expression : "";
    spec->arith_expression = strdup(expr);
    if (!spec->arith_expression) {
        return false;
    }
    return true;
}

static bool captureTestWords(const ShellCommand *cmd, LoopBodySpec *spec) {
    if (!cmd || cmd->type != SHELL_COMMAND_SIMPLE || !spec) {
        return false;
    }
    const ShellWordArray *words = &cmd->data.simple.words;
    size_t count = words->count;
    if (count == 0) {
        return false;
    }
    char **encoded = (char **)calloc(count, sizeof(char *));
    if (!encoded) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        encoded[i] = encodeWord(words->items[i]);
        if (!encoded[i]) {
            for (size_t j = 0; j < i; ++j) {
                free(encoded[j]);
            }
            free(encoded);
            return false;
        }
    }
    spec->test_words = encoded;
    spec->test_word_count = count;
    return true;
}

static BodyCommandType classifyBodyCommand(const ShellCommand *cmd) {
    if (!cmd) {
        return BODY_CMD_NONE;
    }
    if (cmd->exec.runs_in_background || cmd->exec.is_async_parent) {
        return BODY_CMD_OTHER;
    }
    switch (cmd->type) {
        case SHELL_COMMAND_SIMPLE: {
            const ShellWordArray *words = &cmd->data.simple.words;
            const ShellRedirectionArray *redirs = &cmd->redirections;
            if (redirs->count > 0) {
                return BODY_CMD_OTHER;
            }
            if (words->count == 0) {
                return BODY_CMD_NONE;
            }
            const ShellWord *first = words->items[0];
            if (!wordIsLiteralCommand(first) || !first->text) {
                return BODY_CMD_OTHER;
            }
            const char *text = first->text;
            if (strcmp(text, ":") == 0) {
                return BODY_CMD_COLON;
            }
            if (strcmp(text, "true") == 0) {
                return BODY_CMD_TRUE;
            }
            if (strcmp(text, "false") == 0) {
                return BODY_CMD_FALSE;
            }
            if (strcmp(text, "test") == 0) {
                return BODY_CMD_TEST;
            }
            if (strcmp(text, "[") == 0) {
                if (words->count < 2) {
                    return BODY_CMD_OTHER;
                }
                const ShellWord *last = words->items[words->count - 1];
                if (!last || !last->text || strcmp(last->text, "]") != 0 || !wordIsLiteralCommand(last)) {
                    return BODY_CMD_OTHER;
                }
                return BODY_CMD_BRACKET;
            }
            return BODY_CMD_OTHER;
        }
        case SHELL_COMMAND_ARITHMETIC: {
            const ShellRedirectionArray *redirs = &cmd->redirections;
            if (redirs->count > 0) {
                return BODY_CMD_OTHER;
            }
            return BODY_CMD_ARITH;
        }
        default:
            return BODY_CMD_OTHER;
    }
}

static bool populateSingleBodySpec(BodyCommandType type, const ShellCommand *cmd, LoopBodySpec *spec) {
    switch (type) {
        case BODY_CMD_COLON:
            spec->kind = LOOP_BODY_KIND_COLON;
            return true;
        case BODY_CMD_TRUE:
            spec->kind = LOOP_BODY_KIND_TRUE;
            return true;
        case BODY_CMD_FALSE:
            spec->kind = LOOP_BODY_KIND_FALSE;
            return true;
        case BODY_CMD_TEST:
            if (!captureTestWords(cmd, spec)) {
                return false;
            }
            spec->kind = LOOP_BODY_KIND_TEST;
            return true;
        case BODY_CMD_BRACKET:
            if (!captureTestWords(cmd, spec)) {
                return false;
            }
            spec->kind = LOOP_BODY_KIND_BRACKET;
            return true;
        case BODY_CMD_ARITH:
            if (!captureArithmeticExpression(cmd, spec)) {
                return false;
            }
            spec->kind = LOOP_BODY_KIND_ARITH;
            return true;
        case BODY_CMD_NONE:
            spec->kind = LOOP_BODY_KIND_COLON;
            return true;
        default:
            return false;
    }
}

static bool gatherLoopBodySpec(const ShellLoop *loop, LoopBodySpec *out_spec) {
    if (!loop || !out_spec) {
        return false;
    }
    initLoopBodySpec(out_spec);

    const ShellProgram *body = loop->body;
    if (!body) {
        out_spec->kind = LOOP_BODY_KIND_COLON;
        return true;
    }

    typedef struct {
        BodyCommandType type;
        const ShellCommand *cmd;
    } BodyComponent;

    BodyComponent components[3];
    size_t component_count = 0;

    for (size_t i = 0; i < body->commands.count; ++i) {
        const ShellCommand *candidate = body->commands.items[i];
        if (!candidate) {
            continue;
        }
        if (candidate->exec.runs_in_background || candidate->exec.is_async_parent) {
            return false;
        }
        const ShellCommand *unwrapped = unwrapPipelineCommand(candidate);
        BodyCommandType type = classifyBodyCommand(unwrapped);
        if (type == BODY_CMD_NONE) {
            continue;
        }
        if (type == BODY_CMD_OTHER) {
            return false;
        }
        components[component_count].type = type;
        components[component_count].cmd = unwrapped;
        component_count++;
        if (component_count > 2) {
            return false;
        }
    }

    if (component_count == 0) {
        out_spec->kind = LOOP_BODY_KIND_COLON;
        return true;
    }

    if (component_count == 1) {
        if (!populateSingleBodySpec(components[0].type, components[0].cmd, out_spec)) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
            return false;
        }
        return true;
    }

    BodyCommandType firstType = components[0].type;
    BodyCommandType secondType = components[1].type;

    if (firstType == BODY_CMD_COLON || firstType == BODY_CMD_TRUE) {
        bool ok = populateSingleBodySpec(secondType, components[1].cmd, out_spec);
        if (!ok) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
        }
        return ok;
    }

    if (firstType == BODY_CMD_ARITH && (secondType == BODY_CMD_COLON || secondType == BODY_CMD_TRUE)) {
        bool ok = populateSingleBodySpec(firstType, components[0].cmd, out_spec);
        if (!ok) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
        }
        return ok;
    }

    if ((firstType == BODY_CMD_TEST || firstType == BODY_CMD_BRACKET) && secondType == BODY_CMD_ARITH) {
        if (!captureTestWords(components[0].cmd, out_spec)) {
            return false;
        }
        if (!captureArithmeticExpression(components[1].cmd, out_spec)) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
            return false;
        }
        out_spec->kind = (firstType == BODY_CMD_TEST) ? LOOP_BODY_KIND_TEST_ARITH : LOOP_BODY_KIND_BRACKET_ARITH;
        return true;
    }

    if (firstType == BODY_CMD_ARITH && (secondType == BODY_CMD_COLON || secondType == BODY_CMD_TRUE)) {
        bool ok = populateSingleBodySpec(firstType, components[0].cmd, out_spec);
        if (!ok) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
        }
        return ok;
    }

    if ((firstType == BODY_CMD_COLON || firstType == BODY_CMD_TRUE) && (secondType == BODY_CMD_COLON || secondType == BODY_CMD_TRUE)) {
        out_spec->kind = LOOP_BODY_KIND_COLON;
        return true;
    }

    freeLoopBodySpec(out_spec);
    initLoopBodySpec(out_spec);
    return false;
}

static void emitPushInt(BytecodeChunk *chunk, int value, int line) {
    Value constant = makeInt(value);
    int index = addConstantToChunk(chunk, &constant);
    freeValue(&constant);
    emitConstantOperand(chunk, index, line);
}

static void emitCallHost(BytecodeChunk *chunk, HostFunctionID id, int line) {
    writeBytecodeChunk(chunk, CALL_HOST, line);
    writeBytecodeChunk(chunk, (uint8_t)id, line);
}

static void emitBuiltinProc(BytecodeChunk *chunk, const char *name, uint8_t arg_count, int line) {
    const char *canonical = shellBuiltinCanonicalName(name);
    int name_index = addBuiltinNameConstant(chunk, canonical, canonical);
    int builtin_id = shellGetBuiltinId(name);
    if (builtin_id < 0) {
        fprintf(stderr, "shell codegen warning: unknown builtin '%s'\n", name);
        writeBytecodeChunk(chunk, CALL_BUILTIN, line);
        emitShort(chunk, (uint16_t)name_index, line);
        writeBytecodeChunk(chunk, arg_count, line);
        return;
    }
    writeBytecodeChunk(chunk, CALL_BUILTIN_PROC, line);
    emitShort(chunk, (uint16_t)builtin_id, line);
    emitShort(chunk, (uint16_t)name_index, line);
    writeBytecodeChunk(chunk, arg_count, line);
}

static const char *redirTypeName(ShellRedirectionType type) {
    switch (type) {
        case SHELL_REDIRECT_INPUT: return "<";
        case SHELL_REDIRECT_READ_WRITE: return "<>";
        case SHELL_REDIRECT_OUTPUT: return ">";
        case SHELL_REDIRECT_APPEND: return ">>";
        case SHELL_REDIRECT_HEREDOC: return "<<";
        case SHELL_REDIRECT_HERE_STRING: return "<<<";
        case SHELL_REDIRECT_DUP_INPUT: return "<&";
        case SHELL_REDIRECT_DUP_OUTPUT: return ">&";
        case SHELL_REDIRECT_CLOBBER: return ">|";
        case SHELL_REDIRECT_OUTPUT_AND_ERROR: return "&>";
        case SHELL_REDIRECT_APPEND_OUTPUT_AND_ERROR: return "&>>";
    }
    return "";
}

static char *buildPipelineMetadata(const ShellPipeline *pipeline) {
    size_t stage_count = pipeline ? pipeline->command_count : 0;
    bool negated = pipeline ? shellPipelineHasExplicitNegation(pipeline) : false;
    size_t merge_len = stage_count;

    char *merge = (char *)malloc(merge_len + 1);
    if (!merge) {
        return NULL;
    }
    for (size_t i = 0; i < merge_len; ++i) {
        merge[i] = shellPipelineGetMergeStderr(pipeline, i) ? '1' : '0';
    }
    merge[merge_len] = '\0';

    size_t meta_len = (size_t)snprintf(NULL, 0, "stages=%zu;negated=%d;merge=%s",
                                      stage_count,
                                      negated ? 1 : 0,
                                      merge);
    char *meta = (char *)malloc(meta_len + 1);
    if (!meta) {
        free(merge);
        return NULL;
    }
    snprintf(meta, meta_len + 1, "stages=%zu;negated=%d;merge=%s",
             stage_count,
             negated ? 1 : 0,
             merge);
    free(merge);
    return meta;
}

static char *buildRedirectionMetadata(const ShellRedirection *redir) {
    if (!redir) {
        return NULL;
    }
    const char *fd_text = (redir->io_number && *redir->io_number) ? redir->io_number : "";
    const char *type_name = redirTypeName(redir->type);

    char *encoded_word = NULL;
    {
        ShellWord *target = shellRedirectionGetWordTarget(redir);
        if (target) {
            char *encoded = encodeWord(target);
            if (encoded) {
                encoded_word = encodeHexString(encoded);
            }
            free(encoded);
        }
    }
    if (!encoded_word) {
        encoded_word = strdup("");
        if (!encoded_word) {
            return NULL;
        }
    }

    const char *dup_target = shellRedirectionGetDupTarget(redir);
    char *dup_hex = encodeHexString(dup_target ? dup_target : "");
    if (!dup_hex) {
        free(encoded_word);
        return NULL;
    }

    const char *here_body = shellRedirectionGetHereDocument(redir);
    bool here_quoted = shellRedirectionHereDocumentIsQuoted(redir);
    char *here_hex = encodeHexString(here_body ? here_body : "");
    if (!here_hex) {
        free(encoded_word);
        free(dup_hex);
        return NULL;
    }

    const char *here_string_literal = shellRedirectionGetHereStringLiteral(redir);
    char *here_string_hex = encodeHexString(here_string_literal ? here_string_literal : "");
    if (!here_string_hex) {
        free(encoded_word);
        free(dup_hex);
        free(here_hex);
        return NULL;
    }

    size_t meta_len = (size_t)snprintf(NULL, 0,
                                       "redir:fd=%s;type=%s;word=%s;dup=%s;here=%s;hereq=%d;hstr=%s",
                                       fd_text,
                                       type_name ? type_name : "",
                                       encoded_word,
                                       dup_hex,
                                       here_hex,
                                       here_quoted ? 1 : 0,
                                       here_string_hex);
    char *meta = (char *)malloc(meta_len + 1);
    if (!meta) {
        free(encoded_word);
        free(dup_hex);
        free(here_hex);
        free(here_string_hex);
        return NULL;
    }
    snprintf(meta, meta_len + 1,
             "redir:fd=%s;type=%s;word=%s;dup=%s;here=%s;hereq=%d;hstr=%s",
             fd_text,
             type_name ? type_name : "",
             encoded_word,
             dup_hex,
             here_hex,
             here_quoted ? 1 : 0,
             here_string_hex);

    free(encoded_word);
    free(dup_hex);
    free(here_hex);
    free(here_string_hex);
    return meta;
}

static void compileCommand(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background);
static void compileCommandFinal(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background, bool final);
static void compileBody(BytecodeChunk *chunk, const ShellCommand *command, bool background);
static bool commandIsCompound(const ShellCommand *command);
static void compileProgram(BytecodeChunk *chunk, const ShellProgram *program);
static void compilePipeline(BytecodeChunk *chunk, const ShellPipeline *pipeline, bool runs_in_background);
static void compileCase(BytecodeChunk *chunk, const ShellCase *case_stmt, int line);
static void compileFunction(BytecodeChunk *chunk, const ShellFunction *function, int line);
static void compileArithmetic(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background);

/* Parameters whose answer depends on the process asking, or that change when read. */
static bool parameterIsPerProcess(const char *name, size_t len) {
    static const char *const kNames[] = {"BASHPID", "RANDOM"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        if (strlen(kNames[i]) == len && strncmp(kNames[i], name, len) == 0) {
            return true;
        }
    }
    return false;
}

static size_t parameterNameLength(const char *text) {
    size_t len = 0;
    if (isalpha((unsigned char)text[0]) || text[0] == '_') {
        while (isalnum((unsigned char)text[len]) || text[len] == '_') {
            len++;
        }
    } else if (isdigit((unsigned char)text[0])) {
        while (isdigit((unsigned char)text[len])) {
            len++;
        }
    } else if (text[0] && strchr("@*#?-$!", text[0])) {
        len = 1;
    }
    return len;
}

/* Whether `${` at text only reads a variable: a name, then nothing more than an operator that
 * cannot assign, fail or evaluate. What such an operator's own word holds comes next in the scan. */
static bool braceExpansionOnlyReads(const char *text) {
    const char *p = text;
    if (p[0] == '#' && p[1] != '}') {
        p++;
    }
    size_t len = parameterNameLength(p);
    if (len == 0 || parameterIsPerProcess(p, len)) {
        return false;
    }
    p += len;
    if (p[0] == '[') {
        /* A subscript is arithmetic, which can assign, unless it is one of these. */
        p++;
        if ((p[0] == '@' || p[0] == '*') && p[1] == ']') {
            p += 2;
        } else {
            while (isdigit((unsigned char)*p)) {
                p++;
            }
            if (*p != ']' || p[-1] == '[') {
                return false;
            }
            p++;
        }
    }
    if (p[0] == ':') {
        return p[1] == '-' || p[1] == '+';
    }
    return p[0] && strchr("}-+#%/^,@", p[0]) != NULL;
}

/* Whether expanding this text can do more than read variables. */
static bool textExpansionActs(const char *text) {
    for (const char *p = text; p && *p; ++p) {
        if (*p == '`' || ((*p == '<' || *p == '>') && p[1] == '(')) {
            return true;
        }
        if (*p != '$') {
            continue;
        }
        if (p[1] == '(' || p[1] == '[') {
            return true;
        }
        if (p[1] == '{') {
            if (!braceExpansionOnlyReads(p + 2)) {
                return true;
            }
        } else if (parameterIsPerProcess(p + 1, parameterNameLength(p + 1))) {
            return true;
        }
    }
    return false;
}

static bool wordExpansionActs(const ShellWord *word) {
    if (!word) {
        return false;
    }
    if (word->has_command_substitution || word->command_substitutions.count > 0 || word->has_arithmetic_expansion) {
        return true;
    }
    return word->has_parameter_expansion && textExpansionActs(word->text);
}

/* Under bash a job or a stage forks first and expands its words there, so what they do stays in
 * it and the shell does not wait for them. Here a simple command expands its words on the shell
 * before it starts, which is cheaper and is all that plain reads need. One whose words can do more
 * starts as a body, as a compound command does. */
static bool commandExpansionsAct(const ShellCommand *command) {
    if (!command || command->type != SHELL_COMMAND_SIMPLE) {
        return false;
    }
    const ShellWordArray *words = &command->data.simple.words;
    bool saw_name = false;
    for (size_t i = 0; i < words->count; ++i) {
        const ShellWord *word = words->items[i];
        if (wordExpansionActs(word)) {
            return true;
        }
        /* exec replaces the job's process, and only a body has one to replace. */
        if (!saw_name && word && !word->is_assignment) {
            saw_name = true;
            if (word->text && strcmp(word->text, "exec") == 0) {
                return true;
            }
        }
    }
    const ShellRedirectionArray *redirs = &command->redirections;
    for (size_t i = 0; i < redirs->count; ++i) {
        const ShellRedirection *redir = redirs->items[i];
        const char *here_body = shellRedirectionGetHereDocument(redir);
        if (wordExpansionActs(shellRedirectionGetWordTarget(redir)) ||
            textExpansionActs(shellRedirectionGetDupTarget(redir)) ||
            textExpansionActs(shellRedirectionGetHereStringLiteral(redir)) ||
            (here_body && !shellRedirectionHereDocumentIsQuoted(redir) && textExpansionActs(here_body))) {
            return true;
        }
    }
    return false;
}

/* What `jobs` and `lps` call a job started as a body: its words as written, since the shell
 * does not know what they expand to. */
static char *buildJobLabel(const ShellCommand *command) {
    enum { LABEL_MAX = 256 };
    char *label = (char *)malloc(LABEL_MAX);
    if (!label) {
        return NULL;
    }
    size_t length = 0;
    const ShellWordArray *words = &command->data.simple.words;
    for (size_t i = 0; i < words->count; ++i) {
        const char *text = words->items[i] && words->items[i]->text ? words->items[i]->text : "";
        if (i > 0 && length + 1 < LABEL_MAX) {
            label[length++] = ' ';
        }
        for (; *text && length + 1 < LABEL_MAX; ++text) {
            if (*text != SHELL_QUOTE_MARK_SINGLE && *text != SHELL_QUOTE_MARK_DOUBLE && *text != SHELL_QUOTE_MARK_BYTE) {
                label[length++] = *text;
            }
        }
    }
    label[length] = '\0';
    return label;
}

static bool commandIsAloneInPipeline(const ShellCommand *command) {
    return command->exec.is_pipeline_head && command->exec.is_pipeline_tail;
}

static void compileSimpleWith(BytecodeChunk *chunk, const ShellCommand *command, const char *meta);

static void compileSimple(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background) {
    char meta[128];
    snprintf(meta, sizeof(meta), "bg=%d;pipe=%d;head=%d;tail=%d;line=%d;col=%d",
             (command && command->exec.runs_in_background) || runs_in_background ? 1 : 0,
             command ? command->exec.pipeline_index : -1,
             command && command->exec.is_pipeline_head ? 1 : 0,
             command && command->exec.is_pipeline_tail ? 1 : 0,
             command ? command->line : 0,
             command ? command->column : 0);
    compileSimpleWith(chunk, command, meta);
}

/* The only command of a body: a pipeline of one, whose `job` asks that it become the job rather
 * than run under it, as a forked child execs. Only a job on its own loses stdin: a stage has its
 * pipe, or the shell's. */
static void compileJobCommand(BytecodeChunk *chunk, const ShellCommand *command) {
    char meta[128];
    snprintf(meta, sizeof(meta), "bg=0;pipe=0;head=1;tail=1;line=%d;col=%d;job=1%s", command->line, command->column,
             commandIsAloneInPipeline(command) ? ";jobnull=1" : "");
    emitPushString(chunk, "stages=1;negated=0;merge=0", command->line);
    emitBuiltinProc(chunk, "__shell_pipeline", 1, command->line);
    compileSimpleWith(chunk, command, meta);
}

static void compileSimpleWith(BytecodeChunk *chunk, const ShellCommand *command, const char *meta) {
    int line = command ? command->line : 0;
    emitPushString(chunk, meta, line);

    size_t arg_count = 1; // metadata entry
    if (command) {
        const ShellWordArray *words = &command->data.simple.words;
        for (size_t i = 0; i < words->count; ++i) {
            const ShellWord *word = words->items[i];
            emitPushWord(chunk, word, line);
            arg_count++;
        }
        const ShellRedirectionArray *redirs = &command->redirections;
        for (size_t i = 0; i < redirs->count; ++i) {
            const ShellRedirection *redir = redirs->items[i];
            char *serialized = buildRedirectionMetadata(redir);
            if (!serialized) {
                emitPushString(chunk, "redir:fd=;type=;word=;dup=;here=", line);
            } else {
                emitPushString(chunk, serialized, line);
                free(serialized);
            }
            arg_count++;
        }
    }

    if (arg_count > 255) {
        fprintf(stderr, "shell codegen warning: argument vector truncated (%zu).\n", arg_count);
        arg_count = 255;
    }
    emitBuiltinProc(chunk, "__shell_exec", (uint8_t)arg_count, line);
}

static void compileArithmetic(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background) {
    int line = command ? command->line : 0;
    char meta[128];
    snprintf(meta, sizeof(meta), "bg=%d;pipe=%d;head=%d;tail=%d;line=%d;col=%d",
             (command && command->exec.runs_in_background) || runs_in_background ? 1 : 0,
             command ? command->exec.pipeline_index : -1,
             command && command->exec.is_pipeline_head ? 1 : 0,
             command && command->exec.is_pipeline_tail ? 1 : 0,
             command ? command->line : 0,
             command ? command->column : 0);
    emitPushString(chunk, meta, line);

    size_t arg_count = 1;
    const char *expr = (command && command->data.arithmetic.expression)
                           ? command->data.arithmetic.expression
                           : "";
    emitPushString(chunk, expr, line);
    arg_count++;

    const ShellRedirectionArray *redirs = command ? shellCommandGetRedirections(command) : NULL;
    size_t redir_count = redirs ? redirs->count : 0;
    for (size_t i = 0; i < redir_count; ++i) {
        const ShellRedirection *redir = redirs->items[i];
        char *serialized = buildRedirectionMetadata(redir);
        if (!serialized) {
            emitPushString(chunk, "redir:fd=;type=;word=;dup=;here=", line);
        } else {
            emitPushString(chunk, serialized, line);
            free(serialized);
        }
        arg_count++;
    }

    if (arg_count > 255) {
        fprintf(stderr, "shell codegen warning: arithmetic command args truncated (%zu).\n", arg_count);
        arg_count = 255;
    }
    emitBuiltinProc(chunk, "__shell_arithmetic", (uint8_t)arg_count, line);
}

static void compilePipeline(BytecodeChunk *chunk, const ShellPipeline *pipeline, bool runs_in_background) {
    if (!pipeline) {
        return;
    }
    int line = (pipeline->command_count > 0 && pipeline->commands[0]) ? pipeline->commands[0]->line : 0;
    if (pipeline->timed) {
        emitBuiltinProc(chunk, "__shell_time_begin", 0, line);
    }
    if (pipeline->timed && pipeline->command_count == 0) {
        emitPushString(chunk, pipeline->time_posix ? "pe" : "e", line);
        emitBuiltinProc(chunk, "__shell_time_end", 1, line);
        return;
    }
    char *meta = buildPipelineMetadata(pipeline);
    if (meta && runs_in_background && pipeline->command_count > 1) {
        size_t meta_len = strlen(meta) + sizeof(";async=1");
        char *grown = (char *)realloc(meta, meta_len);
        if (grown) {
            meta = grown;
            strcat(meta, ";async=1");
        }
    }
    /* A simple command's status is inverted when its pipeline finishes. The commands
     * inside a compound one are pipelines of their own, so it is inverted here, after
     * them. A subshell is started as one command, so it counts as simple. */
    ShellCommand *only_stage = pipeline->command_count == 1 ? pipeline->commands[0] : NULL;
    bool negate_compound = pipeline->negated && commandIsCompound(only_stage) &&
                           only_stage->type != SHELL_COMMAND_SUBSHELL;
    /* errexit ignores a pipeline whose status is inverted, and what runs inside it. */
    if (pipeline->negated) {
        emitBuiltinProc(chunk, "__shell_enter_condition", 0, line);
    }
    if (negate_compound) {
        emitPushString(chunk, "stages=1;negated=0;merge=0", line);
        free(meta);
    } else if (!meta) {
        emitPushString(chunk, "stages=0;negated=0;merge=", line);
    } else {
        emitPushString(chunk, meta, line);
        free(meta);
    }
    emitBuiltinProc(chunk, "__shell_pipeline", 1, line);
    for (size_t i = 0; i < pipeline->command_count; ++i) {
        bool stage_background = runs_in_background && (i + 1 == pipeline->command_count);
        ShellCommand *stage_command = pipeline->commands[i];
        if (stage_command) {
            shellCommandPropagatePipelineMetadata(stage_command,
                                                  (int)i,
                                                  i == 0,
                                                  (i + 1) == pipeline->command_count);
        }
        if ((commandIsCompound(stage_command) && (pipeline->command_count > 1 || stage_background)) ||
            ((pipeline->command_count > 1 || stage_background) && commandExpansionsAct(stage_command))) {
            compileBody(chunk, stage_command, stage_background);
        } else {
            compileCommand(chunk, stage_command, stage_background);
        }
    }
    if (negate_compound) {
        emitPushString(chunk, "negate", line);
        emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 1, line);
    } else if (pipeline->negated) {
        emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
    }
    if (pipeline->timed) {
        emitPushString(chunk, pipeline->time_posix ? "p" : "", line);
        emitBuiltinProc(chunk, "__shell_time_end", 1, line);
    }
}

static void compileLogical(BytecodeChunk *chunk, const ShellLogicalList *logical, int line) {
    if (!logical || logical->count == 0) {
        return;
    }

    size_t pipeline_count = logical->count;
    size_t connector_count = (pipeline_count > 0) ? (pipeline_count - 1) : 0;

    bool guard_condition = connector_count > 0;
    if (guard_condition) {
        emitBuiltinProc(chunk, "__shell_enter_condition", 0, line);
    }

    if (connector_count == 0) {
        compilePipeline(chunk, logical->pipelines[0], false);
        if (guard_condition) {
            emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
        }
        return;
    }

    int *patch_sites = (int *)malloc(connector_count * sizeof(int));
    int *pipeline_starts = (int *)malloc(pipeline_count * sizeof(int));
    if (!patch_sites || !pipeline_starts) {
        free(patch_sites);
        free(pipeline_starts);
        for (size_t i = 0; i < pipeline_count; ++i) {
            compilePipeline(chunk, logical->pipelines[i], false);
            if (i + 1 < pipeline_count) {
                ShellLogicalConnector connector = logical->connectors[i + 1];
                const char *name = connector == SHELL_LOGICAL_AND ? "__shell_and" : "__shell_or";
                char meta[32];
                snprintf(meta, sizeof(meta), "connector=%s", connector == SHELL_LOGICAL_AND ? "&&" : "||");
                emitPushString(chunk, meta, line);
                emitBuiltinProc(chunk, name, 1, line);
            }
        }
        if (guard_condition) {
            emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
        }
        return;
    }

    size_t patch_count = 0;
    for (size_t i = 0; i < pipeline_count - 1; ++i) {
        pipeline_starts[i] = chunk->count;
        compilePipeline(chunk, logical->pipelines[i], false);
        emitCallHost(chunk, HOST_FN_SHELL_LAST_STATUS, line);
        emitPushInt(chunk, 0, line);
        writeBytecodeChunk(chunk, EQUAL, line);
        if (logical->connectors[i + 1] == SHELL_LOGICAL_OR) {
            writeBytecodeChunk(chunk, NOT, line);
        }
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
        patch_sites[patch_count++] = chunk->count;
        emitInt32(chunk, 0xFFFFFFFF, line);
    }

    /* Only the pipeline after the last connector can trip errexit, so it runs outside
     * the guard. */
    pipeline_starts[pipeline_count - 1] = chunk->count;
    emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
    compilePipeline(chunk, logical->pipelines[pipeline_count - 1], false);
    writeBytecodeChunk(chunk, JUMP, line);
    int done_jump = chunk->count;
    emitInt32(chunk, 0xFFFFFFFF, line);

    int leave_label = chunk->count;
    emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
    patchInt32(chunk, done_jump, (uint32_t)(chunk->count - (done_jump + 4)));

    /* The list groups from the left: when a fails in `a && b || c`, b is skipped and c
     * still runs. A short circuit lands on the pipeline after the next connector of the
     * other kind, or past the list if there is none. */
    for (size_t i = 0; i < patch_count; ++i) {
        int index = patch_sites[i];
        int32_t target = leave_label;
        for (size_t next = i + 1; next + 1 < pipeline_count; ++next) {
            if (logical->connectors[next + 1] != logical->connectors[i + 1]) {
                target = pipeline_starts[next + 1];
                break;
            }
        }
        uint32_t offset = (uint32_t)(target - (index + 4));
        patchInt32(chunk, index, offset);
    }

    free(patch_sites);
    free(pipeline_starts);
}

/* Opens a compound command's redirections. Returns the jump that skips its body
 * when one of them fails, for compileScopeLeave to patch; -1 when there are none. */
static int compileScopeEnter(BytecodeChunk *chunk, const ShellCommand *command, bool final) {
    int line = command->line;
    int index = command->exec.pipeline_index;
    char meta[128];
    snprintf(meta, sizeof(meta), "mode=enter;subshell=%d%s", index, final ? ";final=1" : "");
    emitPushString(chunk, meta, line);
    size_t arg_count = 1;
    /* Serialize any attached redirections for the enter phase. */
    const ShellRedirectionArray *enter_redirs = shellCommandGetRedirections(command);
    if (enter_redirs && enter_redirs->count > 0) {
        for (size_t i = 0; i < enter_redirs->count && arg_count < 255; ++i) {
            const ShellRedirection *redir = enter_redirs->items[i];
            char *serialized = buildRedirectionMetadata(redir);
            if (serialized) {
                emitPushString(chunk, serialized, line);
                free(serialized);
            } else {
                emitPushString(chunk, "redir:fd=;type=;word=;dup=;here=", line);
            }
            arg_count++;
        }
    }
    emitBuiltinProc(chunk, "__shell_subshell", (uint8_t)arg_count, line);
    if (arg_count == 1) {
        return -1;
    }
    emitCallHost(chunk, HOST_FN_SHELL_SCOPE_IS_READY, line);
    writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
    int skip_jump = chunk->count;
    emitInt32(chunk, 0xFFFFFFFF, line);
    return skip_jump;
}

static void compileScopeLeave(BytecodeChunk *chunk, int skip_jump, int line) {
    if (skip_jump >= 0) {
        patchInt32(chunk, skip_jump, (uint32_t)(chunk->count - (skip_jump + 4)));
    }
    emitPushString(chunk, "mode=leave", line);
    emitBuiltinProc(chunk, "__shell_subshell", 1, line);
}

static bool commandHasRedirections(const ShellCommand *command) {
    const ShellRedirectionArray *redirs = shellCommandGetRedirections(command);
    return redirs && redirs->count > 0;
}

static bool commandIsCompound(const ShellCommand *command) {
    if (!command) {
        return false;
    }
    switch (command->type) {
        case SHELL_COMMAND_SUBSHELL:
        case SHELL_COMMAND_BRACE_GROUP:
        case SHELL_COMMAND_LOOP:
        case SHELL_COMMAND_CONDITIONAL:
        case SHELL_COMMAND_CASE:
            return true;
        default:
            return false;
    }
}

/* A subshell, and any compound command that is a pipeline stage or in the background,
 * runs apart from the shell. Its body becomes a chunk of its own and __shell_exec_body
 * starts it as one command, so it takes the path of any other stage. The redirections
 * stay in the body, where they apply after the stage's pipe, as POSIX orders them. They
 * are final: nothing in the body runs after them, so they are not undone. */
static void compileBody(BytecodeChunk *chunk, const ShellCommand *command, bool background) {
    if (!command) {
        return;
    }
    int line = command->line;
    ShellCompiledFunction *compiled = (ShellCompiledFunction *)calloc(1, sizeof(ShellCompiledFunction));
    if (!compiled) {
        fprintf(stderr, "shell codegen warning: unable to allocate a compound command's chunk.\n");
        return;
    }
    compiled->magic = SHELL_COMPILED_FUNCTION_MAGIC;
    initBytecodeChunk(&compiled->chunk);
    if (command->type == SHELL_COMMAND_LOGICAL) {
        compileLogical(&compiled->chunk, command->data.logical, line);
    } else if (command->type == SHELL_COMMAND_PIPELINE) {
        compilePipeline(&compiled->chunk, command->data.pipeline, false);
    } else if (command->type == SHELL_COMMAND_SUBSHELL) {
        bool scoped = commandHasRedirections(command);
        int skip_jump = scoped ? compileScopeEnter(&compiled->chunk, command, true) : -1;
        compileProgram(&compiled->chunk, command->data.subshell.body);
        if (scoped) {
            compileScopeLeave(&compiled->chunk, skip_jump, line);
        }
    } else if (command->type == SHELL_COMMAND_SIMPLE) {
        compileJobCommand(&compiled->chunk, command);
    } else {
        compileCommandFinal(&compiled->chunk, command, false, true);
    }
    writeBytecodeChunk(&compiled->chunk, RETURN, 0);

    Value ptr = makePointer(compiled, SHELL_FUNCTION_PTR_SENTINEL);
    int ptr_index = addConstantToChunk(chunk, &ptr);
    char meta[128];
    snprintf(meta, sizeof(meta), "bg=%d;pipe=%d;head=%d;tail=%d;line=%d;col=%d%s",
             background ? 1 : 0,
             command->exec.pipeline_index,
             command->exec.is_pipeline_head ? 1 : 0,
             command->exec.is_pipeline_tail ? 1 : 0,
             line,
             command->column,
             command->type == SHELL_COMMAND_SIMPLE && commandIsAloneInPipeline(command) ? ";simple=1" : "");
    emitConstantOperand(chunk, ptr_index, line);
    emitPushString(chunk, meta, line);
    char *label = command->type == SHELL_COMMAND_SIMPLE ? buildJobLabel(command) : NULL;
    if (label && *label) {
        emitPushString(chunk, label, line);
        emitBuiltinProc(chunk, "__shell_exec_body", 3, line);
    } else {
        emitBuiltinProc(chunk, "__shell_exec_body", 2, line);
    }
    free(label);
}

static void compileLoop(BytecodeChunk *chunk, const ShellCommand *command, bool final) {
    if (!command) {
        return;
    }
    const ShellLoop *loop = command->data.loop;
    if (!loop) {
        return;
    }

    int line = command->line;

    bool isFor = loop->is_for;
    bool isCStyle = loop->is_cstyle_for;
    const ShellRedirectionArray *redirs = loop ? &loop->redirections : NULL;
    size_t redir_count = redirs ? redirs->count : 0;

    LoopConditionSpec cond_spec;
    initLoopConditionSpec(&cond_spec);
    bool cond_fast = false;
    size_t cond_payload_count = 0;
    if (!isFor && !isCStyle && gatherLoopConditionSpec(loop, &cond_spec)) {
        if (cond_spec.kind != LOOP_COND_KIND_NONE) {
            cond_fast = true;
            if (cond_spec.kind == LOOP_COND_KIND_ARITH) {
                cond_payload_count = 1;
            } else if (cond_spec.kind == LOOP_COND_KIND_TEST || cond_spec.kind == LOOP_COND_KIND_BRACKET) {
                cond_payload_count = cond_spec.word_count;
            } else {
                cond_payload_count = 0;
            }
        }
    }

    LoopBodySpec body_spec;
    initLoopBodySpec(&body_spec);
    bool body_fast = false;
    size_t body_payload_count = 0;
    size_t body_expression_count = 0;
    if (!isFor && !isCStyle && gatherLoopBodySpec(loop, &body_spec)) {
        if (body_spec.kind != LOOP_BODY_KIND_NONE) {
            body_fast = true;
            switch (body_spec.kind) {
                case LOOP_BODY_KIND_ARITH:
                    body_expression_count = 1;
                    break;
                case LOOP_BODY_KIND_TEST:
                case LOOP_BODY_KIND_BRACKET:
                    body_payload_count = body_spec.test_word_count;
                    break;
                case LOOP_BODY_KIND_TEST_ARITH:
                case LOOP_BODY_KIND_BRACKET_ARITH:
                    body_payload_count = body_spec.test_word_count;
                    body_expression_count = 1;
                    break;
                default:
                    break;
            }
        }
    }

    if (!isFor && !isCStyle) {
        size_t projected = 1 + cond_payload_count + body_payload_count + body_expression_count + redir_count;
        if (projected > 255) {
            if (body_fast && (body_payload_count + body_expression_count) > 0) {
                body_fast = false;
                body_payload_count = 0;
                body_expression_count = 0;
            }
            projected = 1 + cond_payload_count + body_payload_count + body_expression_count + redir_count;
            if (projected > 255 && cond_fast && cond_payload_count > 0) {
                cond_fast = false;
                cond_payload_count = 0;
                freeLoopConditionSpec(&cond_spec);
                initLoopConditionSpec(&cond_spec);
            }
        }
    }

    int cond_kind_meta = cond_fast ? cond_spec.kind : LOOP_COND_KIND_NONE;
    size_t cond_words_meta = cond_fast ? cond_payload_count : 0;
    int body_kind_meta = body_fast ? body_spec.kind : LOOP_BODY_KIND_NONE;
    size_t body_words_meta = body_fast ? body_payload_count : 0;

    if (!body_fast) {
        body_payload_count = 0;
        body_expression_count = 0;
    }

    char meta[192];
    if (isFor) {
        snprintf(meta,
                 sizeof(meta),
                 "mode=for;redirs=%zu;condkind=0;condwords=0;bodykind=0;bodywords=0%s",
                 redir_count,
                 final ? ";final=1" : "");
    } else if (isCStyle) {
        snprintf(meta,
                 sizeof(meta),
                 "mode=cfor;redirs=%zu;condkind=0;condwords=0;bodykind=0;bodywords=0%s",
                 redir_count,
                 final ? ";final=1" : "");
    } else {
        snprintf(meta,
                 sizeof(meta),
                 "mode=%s;redirs=%zu;condkind=%d;condwords=%zu;bodykind=%d;bodywords=%zu%s",
                 loop->is_until ? "until" : "while",
                 redir_count,
                 cond_kind_meta,
                 cond_words_meta,
                 body_kind_meta,
                 body_words_meta,
                 final ? ";final=1" : "");
    }
    emitPushString(chunk, meta, line);

    uint8_t arg_count = 1;
    if (cond_fast) {
        if (cond_spec.kind == LOOP_COND_KIND_ARITH) {
            const char *expr = cond_spec.arith_expression ? cond_spec.arith_expression : "";
            emitPushString(chunk, expr, line);
            if (arg_count < 255) {
                arg_count++;
            } else {
                cond_fast = false;
            }
        } else if (cond_spec.kind == LOOP_COND_KIND_TEST || cond_spec.kind == LOOP_COND_KIND_BRACKET) {
            for (size_t i = 0; i < cond_spec.word_count; ++i) {
                if (arg_count >= 255) {
                    cond_fast = false;
                    break;
                }
                const char *encoded = cond_spec.encoded_words[i] ? cond_spec.encoded_words[i] : "";
                emitPushString(chunk, encoded, line);
                arg_count++;
            }
            if (!cond_fast) {
                freeLoopConditionSpec(&cond_spec);
                initLoopConditionSpec(&cond_spec);
                cond_payload_count = 0;
                cond_kind_meta = LOOP_COND_KIND_NONE;
            }
        }
    }
    if (body_fast) {
        if (body_words_meta > 0) {
            for (size_t i = 0; i < body_spec.test_word_count; ++i) {
                if (arg_count >= 255) {
                    body_fast = false;
                    break;
                }
                const char *encoded = body_spec.test_words[i] ? body_spec.test_words[i] : "";
                emitPushString(chunk, encoded, line);
                arg_count++;
            }
        }
        if (body_fast && body_expression_count > 0) {
            const char *expr = body_spec.arith_expression ? body_spec.arith_expression : "";
            emitPushString(chunk, expr, line);
            if (arg_count < 255) {
                arg_count++;
            } else {
                body_fast = false;
            }
        }
        if (!body_fast) {
            body_kind_meta = LOOP_BODY_KIND_NONE;
            body_words_meta = 0;
            body_payload_count = 0;
            body_expression_count = 0;
        }
    }
    if (isFor) {
        if (loop->for_variable) {
            emitPushWord(chunk, loop->for_variable, line);
            arg_count++;
        } else {
            emitPushString(chunk, "", line);
            arg_count++;
        }
        size_t list_count = loop->for_values.count;
        if (list_count + 2 > 255) {
            fprintf(stderr, "shell codegen warning: for loop list truncated (%zu).\n", list_count);
            list_count = 253;
        }
        for (size_t i = 0; i < list_count; ++i) {
            emitPushWord(chunk, loop->for_values.items[i], line);
            if (arg_count < 255) {
                arg_count++;
            }
        }
    } else if (isCStyle) {
        const char *init = loop->cstyle_init ? loop->cstyle_init : "";
        const char *cond = loop->cstyle_condition ? loop->cstyle_condition : "";
        const char *update = loop->cstyle_update ? loop->cstyle_update : "";
        emitPushString(chunk, init, line);
        if (arg_count < 255) arg_count++;
        emitPushString(chunk, cond, line);
        if (arg_count < 255) arg_count++;
        emitPushString(chunk, update, line);
        if (arg_count < 255) arg_count++;
    }

    if (redirs && redir_count > 0) {
        size_t emit_count = redir_count;
        if ((size_t)arg_count + emit_count > 255) {
            if (arg_count < 255) {
                emit_count = 255 - arg_count;
            } else {
                emit_count = 0;
            }
            fprintf(stderr, "shell codegen warning: loop redirections truncated (%zu).\n", redir_count);
        }
        for (size_t i = 0; i < emit_count; ++i) {
            const ShellRedirection *redir = redirs->items[i];
            char *serialized = buildRedirectionMetadata(redir);
            if (!serialized) {
                emitPushString(chunk, "redir:fd=;type=;word=;dup=;here=", line);
            } else {
                emitPushString(chunk, serialized, line);
                free(serialized);
            }
            if (arg_count < 255) {
                arg_count++;
            }
        }
    }

    emitBuiltinProc(chunk, "__shell_loop", arg_count, line);

    bool usesLoopReady = isFor || isCStyle;
    /* A while or until whose redirection failed must not evaluate its condition:
     * `read` would take the shell's own stdin. A for loop checks this each pass. */
    int redirFailedJump = -1;
    if (!usesLoopReady && redirs && redirs->count > 0) {
        emitCallHost(chunk, HOST_FN_SHELL_LOOP_IS_READY, line);
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
        redirFailedJump = chunk->count;
        emitInt32(chunk, 0xFFFFFFFF, line);
    }

    int conditionStart = chunk->count;
    int exitJump = -1;
    bool fusedLoop = false;

    if (usesLoopReady) {
        emitCallHost(chunk, HOST_FN_SHELL_LOOP_IS_READY, line);
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
        exitJump = chunk->count;
        emitInt32(chunk, 0xFFFFFFFF, line);
    } else if (cond_fast && body_fast && cond_kind_meta != LOOP_COND_KIND_NONE) {
        fusedLoop = true;
        emitCallHost(chunk, HOST_FN_SHELL_LOOP_CHECK_BODY, line);
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
        exitJump = chunk->count;
        emitInt32(chunk, 0xFFFFFFFF, line);
    } else {
        if (cond_fast && cond_kind_meta != LOOP_COND_KIND_NONE) {
            emitCallHost(chunk, HOST_FN_SHELL_LOOP_CHECK_CONDITION, line);
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            exitJump = chunk->count;
            emitInt32(chunk, 0xFFFFFFFF, line);
        } else {
            emitBuiltinProc(chunk, "__shell_enter_condition", 0, line);
            compileCommand(chunk, loop->condition, false);
            emitCallHost(chunk, HOST_FN_SHELL_LAST_STATUS, line);
            emitPushInt(chunk, 0, line);
            writeBytecodeChunk(chunk, EQUAL, line);
            if (loop->is_until) {
                writeBytecodeChunk(chunk, NOT, line);
            }
            emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            exitJump = chunk->count;
            emitInt32(chunk, 0xFFFFFFFF, line);
        }
    }

    int exitJump2 = -1;
    if (!fusedLoop) {
        if (body_fast) {
            emitCallHost(chunk, HOST_FN_SHELL_LOOP_EXEC_BODY, line);
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            exitJump2 = chunk->count;
            emitInt32(chunk, 0xFFFFFFFF, line);
        } else {
            compileProgram(chunk, loop->body);
            emitCallHost(chunk, HOST_FN_SHELL_LOOP_ADVANCE, line);
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            exitJump2 = chunk->count;
            emitInt32(chunk, 0xFFFFFFFF, line);
        }
    }

    writeBytecodeChunk(chunk, JUMP, line);
    int loopJump = chunk->count;
    emitInt32(chunk, 0xFFFFFFFF, line);

    int exitLabel = chunk->count;
    emitBuiltinProc(chunk, "__shell_loop_end", 0, line);

    uint32_t loopOffset = (uint32_t)(conditionStart - (loopJump + 4));
    patchInt32(chunk, loopJump, loopOffset);

    if (redirFailedJump >= 0) {
        patchInt32(chunk, redirFailedJump, (uint32_t)(exitLabel - (redirFailedJump + 4)));
    }

    if (exitJump >= 0) {
        uint32_t exitOffset = (uint32_t)(exitLabel - (exitJump + 4));
        patchInt32(chunk, exitJump, exitOffset);
    }

    if (exitJump2 >= 0) {
        uint32_t exitOffset2 = (uint32_t)(exitLabel - (exitJump2 + 4));
        patchInt32(chunk, exitJump2, exitOffset2);
    }

    freeLoopConditionSpec(&cond_spec);
    freeLoopBodySpec(&body_spec);
}

static void compileConditional(BytecodeChunk *chunk, const ShellConditional *conditional, int line) {
    if (!conditional) {
        return;
    }
    emitPushString(chunk, "branch=if", line);
    emitBuiltinProc(chunk, "__shell_if", 1, line);
    emitBuiltinProc(chunk, "__shell_enter_condition", 0, line);
    compileCommand(chunk, conditional->condition, false);
    emitCallHost(chunk, HOST_FN_SHELL_LAST_STATUS, line);
    emitPushInt(chunk, 0, line);
    writeBytecodeChunk(chunk, EQUAL, line);
    /* `$?` in an else or elif branch is the condition's status. */
    emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
    writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
    int elseJump = chunk->count;
    emitInt32(chunk, 0xFFFFFFFF, line);
    compileProgram(chunk, conditional->then_branch);
    writeBytecodeChunk(chunk, JUMP, line);
    int endJump = chunk->count;
    emitInt32(chunk, 0xFFFFFFFF, line);
    uint32_t elseOffset = (uint32_t)(chunk->count - (elseJump + 4));
    patchInt32(chunk, elseJump, elseOffset);
    if (conditional->else_branch) {
        compileProgram(chunk, conditional->else_branch);
    } else {
        /* No branch ran: the if command's status is 0, which leaving a condition sets. */
        emitBuiltinProc(chunk, "__shell_enter_condition", 0, line);
        emitBuiltinProc(chunk, "__shell_leave_condition", 0, line);
    }
    uint32_t endOffset = (uint32_t)(chunk->count - (endJump + 4));
    patchInt32(chunk, endJump, endOffset);
}

static void compileCase(BytecodeChunk *chunk, const ShellCase *case_stmt, int line) {
    if (!case_stmt) {
        return;
    }
    char meta[64];
    snprintf(meta, sizeof(meta), "clauses=%zu", case_stmt->clauses.count);
    emitPushString(chunk, meta, line);
    emitPushWord(chunk, case_stmt->subject, line);
    emitBuiltinProc(chunk, "__shell_case", 2, line);

    size_t clause_count = case_stmt->clauses.count;
    int *end_jumps = NULL;
    size_t end_jump_count = 0;
    if (clause_count > 0) {
        end_jumps = (int *)calloc(clause_count, sizeof(int));
        if (!end_jumps) {
            fprintf(stderr, "shell codegen warning: unable to allocate case jump table.\n");
        }
    }

    int fall_through_jump = -1;
    for (size_t i = 0; i < clause_count; ++i) {
        ShellCaseClause *clause = case_stmt->clauses.items[i];
        size_t pattern_count = clause ? clause->patterns.count : 0;
        char clause_meta[64];
        snprintf(clause_meta, sizeof(clause_meta), "index=%zu;patterns=%zu", i, pattern_count);
        int clause_line = clause ? clause->line : line;
        emitPushString(chunk, clause_meta, clause_line);
        size_t max_patterns = pattern_count;
        if (max_patterns > 254) {
            fprintf(stderr, "shell codegen warning: case clause %zu patterns truncated.\n", i);
            max_patterns = 254;
        }
        for (size_t j = 0; j < max_patterns; ++j) {
            const ShellWord *pattern = clause->patterns.items[j];
            emitPushWord(chunk, pattern, clause_line);
        }
        size_t arg_count = max_patterns + 1;
        emitBuiltinProc(chunk, "__shell_case_clause", (uint8_t)arg_count, clause_line);

        emitCallHost(chunk, HOST_FN_SHELL_LAST_STATUS, clause_line);
        emitPushInt(chunk, 0, clause_line);
        writeBytecodeChunk(chunk, EQUAL, clause_line);
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, clause_line);
        int skip_body_jump = chunk->count;
        emitInt32(chunk, 0xFFFFFFFF, clause_line);

        /* The match was reported through the status. The body sees the one from before. */
        if (clause && clause->body && clause->body->commands.count > 0) {
            emitPushString(chunk, "restore", clause_line);
            emitBuiltinProc(chunk, "__shell_case_clause", 1, clause_line);
        }
        /* ;& lands here: this body runs with no test and the status left by the last one. */
        if (fall_through_jump >= 0) {
            patchInt32(chunk, fall_through_jump, (uint32_t)(chunk->count - (fall_through_jump + 4)));
            fall_through_jump = -1;
        }
        compileProgram(chunk, clause ? clause->body : NULL);

        ShellCaseTerminator terminator = clause ? clause->terminator : SHELL_CASE_END;
        if (terminator == SHELL_CASE_TEST_NEXT && i + 1 < clause_count) {
            /* ;;& goes on to test the next clause as if nothing had matched. */
            emitPushString(chunk, "rematch", clause_line);
            emitBuiltinProc(chunk, "__shell_case_clause", 1, clause_line);
        } else if (terminator == SHELL_CASE_FALL_THROUGH && i + 1 < clause_count) {
            writeBytecodeChunk(chunk, JUMP, clause_line);
            fall_through_jump = chunk->count;
            emitInt32(chunk, 0xFFFFFFFF, clause_line);
        } else {
            writeBytecodeChunk(chunk, JUMP, clause_line);
            int end_jump_pos = chunk->count;
            emitInt32(chunk, 0xFFFFFFFF, clause_line);
            if (end_jumps && end_jump_count < clause_count) {
                end_jumps[end_jump_count++] = end_jump_pos;
            }
        }
        uint32_t skip_offset = (uint32_t)(chunk->count - (skip_body_jump + 4));
        patchInt32(chunk, skip_body_jump, skip_offset);
    }

    if (end_jumps) {
        for (size_t i = 0; i < end_jump_count; ++i) {
            int pos = end_jumps[i];
            if (pos >= 0) {
                uint32_t offset = (uint32_t)(chunk->count - (pos + 4));
                patchInt32(chunk, pos, offset);
            }
        }
        free(end_jumps);
    }

    emitBuiltinProc(chunk, "__shell_case_end", 0, line);
}

static void compileFunction(BytecodeChunk *chunk, const ShellFunction *function, int line) {
    if (!function) {
        return;
    }
    ShellCompiledFunction *compiled = (ShellCompiledFunction *)calloc(1, sizeof(ShellCompiledFunction));
    if (!compiled) {
        fprintf(stderr, "shell codegen warning: unable to allocate function chunk for '%s'.\n",
                function->name ? function->name : "<anonymous>");
        return;
    }
    compiled->magic = SHELL_COMPILED_FUNCTION_MAGIC;
    shellCompile(function->body, &compiled->chunk);
    Value ptr = makePointer(compiled, SHELL_FUNCTION_PTR_SENTINEL);
    int ptr_index = addConstantToChunk(chunk, &ptr);
    emitPushString(chunk, function->name ? function->name : "", line);
    emitPushString(chunk, function->parameter_metadata ? function->parameter_metadata : "", line);
    emitConstantOperand(chunk, ptr_index, line);
    emitBuiltinProc(chunk, "__shell_define_function", 3, line);
}

static void compileCommand(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background) {
    compileCommandFinal(chunk, command, runs_in_background, false);
}

static void compileCommandFinal(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background, bool final) {
    if (!command) {
        return;
    }
    /* A loop applies its own redirections, and a simple command takes them with
     * its words. */
    int scope_skip_jump = -1;
    bool scoped = (command->type == SHELL_COMMAND_BRACE_GROUP ||
                   command->type == SHELL_COMMAND_CONDITIONAL ||
                   command->type == SHELL_COMMAND_CASE) &&
                  commandHasRedirections(command);
    if (scoped) {
        scope_skip_jump = compileScopeEnter(chunk, command, final);
    }
    switch (command->type) {
        case SHELL_COMMAND_SIMPLE:
            compileSimple(chunk, command, runs_in_background);
            break;
        case SHELL_COMMAND_ARITHMETIC:
            compileArithmetic(chunk, command, runs_in_background);
            break;
        case SHELL_COMMAND_PIPELINE:
#if defined(PSCAL_TARGET_IOS)
            /* A stage can run apart from the shell only as a thread the shell joins. */
            if ((command->exec.runs_in_background || runs_in_background) &&
                command->data.pipeline && command->data.pipeline->command_count > 1) {
                /* The body's copy of a DEBUG trap is not the shell's, which runs for each simple
                 * command here. */
                size_t debug_traps = 0;
                for (size_t i = 0; i < command->data.pipeline->command_count; ++i) {
                    debug_traps += commandIsCompound(command->data.pipeline->commands[i]) ? 0 : 1;
                }
                char wrapper_meta[64];
                snprintf(wrapper_meta, sizeof(wrapper_meta), "stages=1;negated=0;merge=0;dbg=%zu", debug_traps);
                emitPushString(chunk, wrapper_meta, command->line);
                emitBuiltinProc(chunk, "__shell_pipeline", 1, command->line);
                compileBody(chunk, command, true);
                break;
            }
#endif
            compilePipeline(chunk, command->data.pipeline, command->exec.runs_in_background || runs_in_background);
            break;
        case SHELL_COMMAND_LOGICAL:
            if (command->exec.runs_in_background || runs_in_background) {
                emitPushString(chunk, "stages=1;negated=0;merge=0", command->line);
                emitBuiltinProc(chunk, "__shell_pipeline", 1, command->line);
                compileBody(chunk, command, true);
            } else {
                compileLogical(chunk, command->data.logical, command->line);
            }
            break;
        case SHELL_COMMAND_SUBSHELL:
            compileBody(chunk, command, command->exec.runs_in_background || runs_in_background);
            break;
        case SHELL_COMMAND_BRACE_GROUP:
            compileProgram(chunk, command->data.brace_group.body);
            break;
        case SHELL_COMMAND_LOOP:
            compileLoop(chunk, command, final);
            break;
        case SHELL_COMMAND_CONDITIONAL:
            compileConditional(chunk, command->data.conditional, command->line);
            break;
        case SHELL_COMMAND_CASE:
            compileCase(chunk, command->data.case_stmt, command->line);
            break;
        case SHELL_COMMAND_FUNCTION:
            compileFunction(chunk, command->data.function, command->line);
            break;
    }
    if (scoped) {
        compileScopeLeave(chunk, scope_skip_jump, command->line);
    }
}

static void compileProgram(BytecodeChunk *chunk, const ShellProgram *program) {
    if (!program) {
        return;
    }
    for (size_t i = 0; i < program->commands.count; ++i) {
        const ShellCommand *command = program->commands.items[i];
        if (command && command->exec.job_text) {
            emitPushString(chunk, command->exec.job_text, command->line);
            emitBuiltinProc(chunk, "__shell_job_text", 1, command->line);
        }
        compileCommand(chunk, command, false);
    }
}

void shellCompile(const ShellProgram *program, BytecodeChunk *chunk) {
    if (!chunk) {
        return;
    }
    initBytecodeChunk(chunk);
    compileProgram(chunk, program);
    writeBytecodeChunk(chunk, RETURN, 0);
}
