#ifndef SHELL_PARSER_H
#define SHELL_PARSER_H

#include "shell/ast.h"
#include "shell/lexer.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShellPendingHereDocArray ShellPendingHereDocArray;

typedef struct {
    ShellLexer lexer;
    ShellToken current;
    ShellToken previous;
    bool had_error;
    bool panic_mode;
    unsigned int next_rule_mask;
    ShellPendingHereDocArray *pending_here_docs;
    unsigned int structural_closer_mask;
    /* The parse stopped because the source ended inside a construct that more
     * text could still finish, rather than because the text was wrong. */
    bool incomplete;
    /* Suppress diagnostics: set for a completeness probe, which parses text the
     * user may well be in the middle of typing. */
    bool quiet;
} ShellParser;

ShellProgram *shellParseString(const char *source, ShellParser *parser);
void shellParserFree(ShellParser *parser);

/* True when `source` stops in the middle of something more input could finish:
 * an open if/for/while/until/case/brace group or subshell, an unterminated
 * quote or expansion, a here-document with no terminator, or a trailing line
 * continuation.  Interactive callers use this to keep reading with the
 * secondary prompt instead of reporting a syntax error.  Nothing is printed. */
bool shellSourceIsIncomplete(const char *source);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_PARSER_H */
