#ifndef SHELL_WORD_SCAN_H
#define SHELL_WORD_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Where an expansion ends in raw shell text. Quotes and nested expansions hide
 * closers, so `$(echo ")")`, `${x:-"}"}` and `${x:-${y}}` all need more than a
 * depth count. The lexer, the parser and the word expander must agree on this,
 * or a recorded substitution stops lining up with its word.
 *
 * Each takes the offset just past the opener and returns the offset just past
 * the closer, or SHELL_SCAN_UNTERMINATED. */
#define SHELL_SCAN_UNTERMINATED ((size_t)-1)

#if defined(__GNUC__) || defined(__clang__)
#define SHELL_SCAN_FN static __attribute__((unused))
#else
#define SHELL_SCAN_FN static
#endif

SHELL_SCAN_FN size_t shellScanDollarParen(const char *s, size_t len, size_t i);
SHELL_SCAN_FN size_t shellScanBraceParam(const char *s, size_t len, size_t i, bool in_double);
SHELL_SCAN_FN size_t shellScanArithmetic(const char *s, size_t len, size_t i);

SHELL_SCAN_FN size_t shellScanSingleQuoted(const char *s, size_t len, size_t i) {
    while (i < len && s[i] != '\'') {
        i++;
    }
    return i < len ? i + 1 : SHELL_SCAN_UNTERMINATED;
}

SHELL_SCAN_FN size_t shellScanBacktick(const char *s, size_t len, size_t i) {
    while (i < len && s[i] != '`') {
        i += s[i] == '\\' ? 2 : 1;
    }
    return i < len ? i + 1 : SHELL_SCAN_UNTERMINATED;
}

/* The body of $'...', from just past the opening quote. Writes the decoded bytes to out
 * when given (it needs len - i bytes) and returns the offset past the closing quote. A
 * NUL ends the string; the rest of the quotes is still read. */
SHELL_SCAN_FN size_t shellScanAnsiC(const char *s, size_t len, size_t i, char *out, size_t *out_len) {
    size_t n = 0;
    bool stopped = false;
    while (i < len && s[i] != '\'') {
        unsigned char c = (unsigned char)s[i++];
        int value = c;
        bool keep_backslash = false;
        if (c == '\\' && i < len) {
            unsigned char esc = (unsigned char)s[i++];
            switch (esc) {
                case 'a': value = '\a'; break;
                case 'b': value = '\b'; break;
                case 'e':
                case 'E': value = 27; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case 'v': value = '\v'; break;
                case '\\':
                case '\'':
                case '"':
                case '?': value = esc; break;
                case 'x': {
                    int digits = 0;
                    value = 0;
                    while (digits < 2 && i < len && strchr("0123456789abcdefABCDEF", s[i]) && s[i]) {
                        char d = s[i++];
                        value = value * 16 + (d <= '9' ? d - '0' : ((d | 0x20) - 'a' + 10));
                        digits++;
                    }
                    if (digits == 0) {
                        keep_backslash = true;
                        value = 'x';
                    }
                    break;
                }
                case 'c':
                    if (i < len) {
                        char ctl = s[i++];
                        value = ctl == '?' ? 127 : (ctl & 0x1f);
                    } else {
                        value = 'c';
                    }
                    break;
                default:
                    if (esc >= '0' && esc <= '7') {
                        int digits = 1;
                        value = esc - '0';
                        while (digits < 3 && i < len && s[i] >= '0' && s[i] <= '7') {
                            value = value * 8 + (s[i++] - '0');
                            digits++;
                        }
                        value &= 0xff;
                    } else {
                        keep_backslash = true;
                        value = esc;
                    }
                    break;
            }
        }
        if (value == 0) {
            stopped = true;
        }
        if (out && !stopped) {
            if (keep_backslash) {
                out[n++] = '\\';
            }
            out[n++] = (char)value;
        }
    }
    if (out_len) {
        *out_len = n;
    }
    return i < len ? i + 1 : SHELL_SCAN_UNTERMINATED;
}

/* s[i] is `$`. Returns i + 1 when no bracketed expansion starts there. */
SHELL_SCAN_FN size_t shellScanDollar(const char *s, size_t len, size_t i, bool in_double) {
    if (i + 1 >= len) {
        return i + 1;
    }
    if (s[i + 1] == '\'' && !in_double) {
        return shellScanAnsiC(s, len, i + 2, NULL, NULL);
    }
    if (s[i + 1] == '{') {
        return shellScanBraceParam(s, len, i + 2, in_double);
    }
    if (s[i + 1] != '(') {
        return i + 1;
    }
    if (i + 2 < len && s[i + 2] == '(') {
        size_t end = shellScanArithmetic(s, len, i + 3);
        if (end != SHELL_SCAN_UNTERMINATED) {
            return end;
        }
    }
    return shellScanDollarParen(s, len, i + 2);
}

SHELL_SCAN_FN size_t shellScanDoubleQuoted(const char *s, size_t len, size_t i) {
    while (i < len) {
        char c = s[i];
        if (c == '"') {
            return i + 1;
        }
        if (c == '\\') {
            i += 2;
        } else if (c == '`') {
            i = shellScanBacktick(s, len, i + 1);
        } else if (c == '$') {
            i = shellScanDollar(s, len, i, true);
        } else {
            i++;
        }
        if (i == SHELL_SCAN_UNTERMINATED) {
            return i;
        }
    }
    return SHELL_SCAN_UNTERMINATED;
}

/* Only `${` nests inside a parameter expansion: a plain `{` does not. */
SHELL_SCAN_FN size_t shellScanBraceParam(const char *s, size_t len, size_t i, bool in_double) {
    while (i < len) {
        char c = s[i];
        if (c == '}') {
            return i + 1;
        }
        if (c == '\\') {
            i += 2;
        } else if (c == '\'' && !in_double) {
            i = shellScanSingleQuoted(s, len, i + 1);
        } else if (c == '"') {
            i = shellScanDoubleQuoted(s, len, i + 1);
        } else if (c == '`') {
            i = shellScanBacktick(s, len, i + 1);
        } else if (c == '$') {
            i = shellScanDollar(s, len, i, in_double);
        } else {
            i++;
        }
        if (i == SHELL_SCAN_UNTERMINATED) {
            return i;
        }
    }
    return SHELL_SCAN_UNTERMINATED;
}

/* After `$((` or `((`: two openers are pending. Refuses text whose last two
 * closers are not adjacent, which makes it `$( (cmd); cmd )` or a nested subshell. */
SHELL_SCAN_FN size_t shellScanArithmetic(const char *s, size_t len, size_t i) {
    int depth = 2;
    while (i < len) {
        char c = s[i];
        if (c == '(') {
            depth++;
            i++;
        } else if (c == ')') {
            depth--;
            if (depth == 1) {
                return (i + 1 < len && s[i + 1] == ')') ? i + 2 : SHELL_SCAN_UNTERMINATED;
            }
            i++;
        } else if (c == '\\') {
            i += 2;
        } else if (c == '\'') {
            i = shellScanSingleQuoted(s, len, i + 1);
        } else if (c == '"') {
            i = shellScanDoubleQuoted(s, len, i + 1);
        } else if (c == '`') {
            i = shellScanBacktick(s, len, i + 1);
        } else if (c == '$') {
            i = shellScanDollar(s, len, i, false);
        } else {
            i++;
        }
        if (i == SHELL_SCAN_UNTERMINATED) {
            return i;
        }
    }
    return SHELL_SCAN_UNTERMINATED;
}

SHELL_SCAN_FN bool shellScanIsWordEnd(const char *s, size_t len, size_t i) {
    return i >= len || strchr(" \t\r\n;&|()<>", s[i]) != NULL;
}

SHELL_SCAN_FN bool shellScanWordIs(const char *s, size_t len, size_t i, const char *word) {
    size_t n = strlen(word);
    return i + n <= len && memcmp(s + i, word, n) == 0 && shellScanIsWordEnd(s, len, i + n);
}

/* A command inside `$(...)` may hold a case statement, whose patterns close a
 * parenthesis they never opened. `)` at a case's own depth is one of those. */
SHELL_SCAN_FN size_t shellScanDollarParen(const char *s, size_t len, size_t i) {
    enum { MAX_CASES = 16 };
    int case_depths[MAX_CASES];
    int case_count = 0;
    int depth = 1;
    bool word_start = true;
    bool command_pos = true;
    while (i < len) {
        char c = s[i];
        switch (c) {
            case '(':
                depth++;
                i++;
                word_start = command_pos = true;
                continue;
            case ')':
                i++;
                word_start = command_pos = true;
                if (case_count > 0 && depth == case_depths[case_count - 1]) {
                    continue;
                }
                if (--depth == 0) {
                    return i;
                }
                continue;
            case ' ':
            case '\t':
            case '\r':
                i++;
                word_start = true;
                continue;
            case '\n':
            case ';':
            case '&':
            case '|':
                i++;
                word_start = command_pos = true;
                continue;
            case '#':
                if (word_start) {
                    while (i < len && s[i] != '\n') {
                        i++;
                    }
                    continue;
                }
                break;
            default:
                break;
        }
        if (word_start) {
            if (case_count > 0 && shellScanWordIs(s, len, i, "esac")) {
                case_count--;
                command_pos = false;
                word_start = false;
                i += 4;
                continue;
            }
            if (command_pos) {
                if (shellScanWordIs(s, len, i, "case")) {
                    if (case_count < MAX_CASES) {
                        case_depths[case_count++] = depth;
                    }
                    command_pos = false;
                    word_start = false;
                    i += 4;
                    continue;
                }
                command_pos = shellScanWordIs(s, len, i, "then") || shellScanWordIs(s, len, i, "do") ||
                              shellScanWordIs(s, len, i, "else") || shellScanWordIs(s, len, i, "elif") ||
                              shellScanWordIs(s, len, i, "if") || shellScanWordIs(s, len, i, "while") ||
                              shellScanWordIs(s, len, i, "until") || shellScanWordIs(s, len, i, "!") ||
                              shellScanWordIs(s, len, i, "{");
            }
        }
        word_start = false;
        if (c == '\\') {
            i += 2;
        } else if (c == '\'') {
            i = shellScanSingleQuoted(s, len, i + 1);
        } else if (c == '"') {
            i = shellScanDoubleQuoted(s, len, i + 1);
        } else if (c == '`') {
            i = shellScanBacktick(s, len, i + 1);
        } else if (c == '$') {
            i = shellScanDollar(s, len, i, false);
        } else {
            i++;
        }
        if (i == SHELL_SCAN_UNTERMINATED) {
            return i;
        }
    }
    return SHELL_SCAN_UNTERMINATED;
}

#endif /* SHELL_WORD_SCAN_H */
