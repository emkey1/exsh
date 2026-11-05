#include "shell/runner.h"

#include "compiler/bytecode.h"
#include "core/cache.h"
#include "shell/builtins.h"
#include "shell/codegen.h"
#include "shell/opt.h"
#include "shell/parser.h"
#include "shell/semantics.h"
#include "symbol/symbol.h"
#include "backend_ast/builtin.h"
#include "vm/vm.h"
#include "core/globals.h"
#include "common/path_virtualization.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#if defined(PSCAL_TARGET_IOS)
#include <spawn.h>
#include <sys/wait.h>
#include "runtime/vproc/vproc.h"
extern char **environ;
#endif

#if defined(PSCAL_TARGET_IOS)
static void shellRunnerFreeParamArray(char **values, int count) {
    if (!values) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(values[i]);
    }
    free(values);
}
#endif

#if defined(PSCAL_TARGET_IOS)
/* Codegen differs here, so an iOS-flavoured host build must not load the plain one's chunks. */
static const char *const kShellCompilerId = "shell-ios";
#else
static const char *const kShellCompilerId = "shell";
#endif

#if defined(PSCAL_TARGET_IOS)
extern void pscalRuntimeDebugLog(const char *message);

/* The Swift-side logger allocates, which can trip guard allocators if some
 * other component has previously scribbled on the heap. To keep the shell
 * usable even when optional diagnostics are enabled (MallocScribble, guard
 * malloc, etc.), we make runtime debug logging opt‑in on iOS.
 *
 * Enable by setting PSCALI_RUNTIME_DEBUG=1 in the environment. */
static bool runtimeDebugLogEnabled(void) {
    static bool initialized = false;
    static bool enabled = false;
    if (!initialized) {
        const char *env = getenv("PSCALI_RUNTIME_DEBUG");
        enabled = (env && *env && strcmp(env, "0") != 0);
        initialized = true;
    }
    return enabled;
}

static void runtimeDebugLog(const char *message) {
    if (!message) {
        return;
    }
    if (!runtimeDebugLogEnabled()) {
        return;
    }
    if (&pscalRuntimeDebugLog != NULL) {
        pscalRuntimeDebugLog(message);
    }
}
#else
static bool g_shell_debug_enabled = false;
static bool g_shell_debug_inited = false;

static void runtimeDebugLog(const char *message) {
    if (!g_shell_debug_inited) {
        const char *env = getenv("PSCAL_SHELL_DEBUG");
        g_shell_debug_enabled = (env && *env && strcmp(env, "0") != 0);
        g_shell_debug_inited = true;
    }
    if (message && g_shell_debug_enabled) {
        fprintf(stderr, "%s\n", message);
    }
}
#endif

static PSCAL_THREAD_LOCAL int gShellSymbolTableDepth = 0;
static PSCAL_THREAD_LOCAL VM *gShellThreadOwnerVm = NULL;

#if defined(PSCAL_TARGET_IOS)
static bool shellReadShebangLine(const char *path, char **out_line) {
    if (out_line) {
        *out_line = NULL;
    }
    if (!path || !out_line) {
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char buffer[512];
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buffer[n] = '\0';
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t offset = 0;
    if (n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        offset = 3;
    }
    if (buffer[offset] != '#' || buffer[offset + 1] != '!') {
        return false;
    }
    const char *line_start = buffer + offset + 2;
    while (*line_start == ' ' || *line_start == '\t') {
        line_start++;
    }
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n' && *line_end != '\r') {
        line_end++;
    }
    size_t line_len = (size_t)(line_end - line_start);
    char *line = (char *)malloc(line_len + 1);
    if (!line) {
        return false;
    }
    memcpy(line, line_start, line_len);
    line[line_len] = '\0';
    *out_line = line;
    return true;
}

static int shellRunExshShebang(const char *path, char *const *argv) {
    if (!path || !*path) {
        return -1;
    }
    char *source = shellLoadFile(path);
    if (!source) {
        return -1;
    }

    char **saved_params = gParamValues;
    int saved_count = gParamCount;
    bool saved_owned = gParamValuesOwned;

    char **new_params = NULL;
    int new_params_count = 0;
    int new_count = 0;
    if (argv) {
        while (argv[1 + new_count]) {
            new_count++;
        }
    }
    bool replaced_params = false;
    if (new_count > 0) {
        new_params = (char **)calloc((size_t)new_count, sizeof(char *));
        if (!new_params) {
            free(source);
            return -1;
        }
        new_params_count = new_count;
        bool ok = true;
        for (int i = 0; i < new_count; ++i) {
            new_params[i] = strdup(argv[i + 1] ? argv[i + 1] : "");
            if (!new_params[i]) {
                shellRunnerFreeParamArray(new_params, i);
                free(source);
                ok = false;
                break;
            }
        }
        if (!ok) {
            return -1;
        }
        gParamValues = new_params;
        gParamCount = new_count;
        gParamValuesOwned = true;
        replaced_params = true;
    }

    const char *previous_arg0 = shellRuntimeGetArg0();
    char *saved_arg0 = previous_arg0 ? strdup(previous_arg0) : NULL;
    shellRuntimeSetArg0(path);

    ShellRunOptions opts = {0};
    opts.no_cache = 1;
    opts.quiet = true;
    opts.exit_on_signal = shellRuntimeExitOnSignal();
    opts.suppress_warnings = true;
    opts.frontend_path = previous_arg0 ? previous_arg0 : "exsh";

    bool exit_requested = false;
    ShellCwdScope cwd_scope;
    shellCwdScopeEnter(&cwd_scope);
    int status = shellRunSource(source, path, &opts, &exit_requested);
    shellCwdScopeLeave(&cwd_scope);

    shellRuntimeSetArg0(saved_arg0);
    free(saved_arg0);

    /* gParamValues is one global for every thread. A script in a pipeline runs
     * beside the shell, which may have swapped in an array that is not ours, and
     * `set --` frees ours and makes its own. Free only what is still ours: a
     * leaked array is small, an invalid free takes down the whole app on iOS. */
    if (replaced_params && gParamValues == new_params) {
        shellRunnerFreeParamArray(new_params, new_params_count);
    }
    gParamValues = saved_params;
    gParamCount = saved_count;
    gParamValuesOwned = saved_owned;

    free(source);

    /* Shebang scripts run as command bodies, not as sourced shell state.
     * Keep an internal `exit` scoped to the script invocation. */
    (void)exit_requested;
    return status;
}

extern int pascal_main(int argc, char **argv);
extern int clike_main(int argc, char **argv);
extern int rea_main(int argc, char **argv);
extern int aether_main(int argc, char **argv);
extern int pscalvm_main(int argc, char **argv);
extern int pscaljson2bc_main(int argc, char **argv);
#ifdef BUILD_DASCAL
extern int dascal_main(int argc, char **argv);
#endif
#ifdef BUILD_PSCALD
extern int pscald_main(int argc, char **argv);
extern int pscalasm_main(int argc, char **argv);
#endif

#if defined(PSCAL_TARGET_IOS)
static bool shellRunnerIsStopStatus(int status) {
    if (status < 128 || status >= 128 + NSIG) {
        return false;
    }
    int sig = status - 128;
    return sig == SIGTSTP || sig == SIGSTOP || sig == SIGTTIN || sig == SIGTTOU;
}
#endif

static int shellSpawnToolRunner(const char *tool_name, int argc, char **argv) {
    /* On iOS we cannot reliably spawn; dispatch tool entrypoints directly. */
    struct {
        const char *name;
        int (*entry)(int, char **);
    } table[] = {
        {"pascal", pascal_main},
        {"clike", clike_main},
        {"rea", rea_main},
        {"aether", aether_main},
        {"pscalvm", pscalvm_main},
        {"pscaljson2bc", pscaljson2bc_main},
#ifdef BUILD_DASCAL
        {"dascal", dascal_main},
#endif
#ifdef BUILD_PSCALD
        {"pscald", pscald_main},
        {"pscalasm", pscalasm_main},
#endif
    };
    const char *name = tool_name && *tool_name ? tool_name : (argc > 0 && argv && argv[0] ? argv[0] : NULL);
    if (!name) {
        return 127;
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcasecmp(name, table[i].name) == 0) {
#if defined(PSCAL_TARGET_IOS)
            VProc *active_vp = vprocCurrent();
            int stage_pid_for_stop = active_vp ? vprocPid(active_vp) : -1;
            int shell_pid_for_stop = vprocGetShellSelfPid();
            bool cooperative_stop_scope = false;
            if (stage_pid_for_stop > 0 &&
                stage_pid_for_stop != shell_pid_for_stop &&
                vprocIsShellSelfThread()) {
                /* Shebang frontends executing inline on the shell thread must
                 * keep SIGTSTP cooperative so Ctrl-Z unwinds to the prompt. */
                vprocSetStopUnsupported(stage_pid_for_stop, true);
                cooperative_stop_scope = true;
                /*
                 * Frontend VMs report cooperative Ctrl-Z through shell runtime
                 * status. Reset the marker so stale values never leak into a
                 * fresh invocation.
                 */
                shellRuntimeSetLastStatus(0);
            }
#endif
            int status = table[i].entry(argc, argv);
            /* No process exit follows to flush the frontend's buffered output. It
             * would land after this command's redirection is gone, or never. */
            fflush(NULL);
#if defined(PSCAL_TARGET_IOS)
            if (cooperative_stop_scope && status == EXIT_SUCCESS) {
                int runtime_status = shellRuntimeLastStatus();
                if (shellRunnerIsStopStatus(runtime_status)) {
                    status = runtime_status;
                }
            }
            if (cooperative_stop_scope && stage_pid_for_stop > 0) {
                vprocSetStopUnsupported(stage_pid_for_stop, false);
            }
#endif
            return status;
        }
    }
    fprintf(stderr, "%s: tool runner unavailable for '%s'\n",
            name, name);
    return 127;
}

static const char *shellResolveToolName(const char *interpreter) {
    if (!interpreter || !*interpreter) {
        return NULL;
    }
    const char *base = strrchr(interpreter, '/');
    base = base ? base + 1 : interpreter;
    if (strcasecmp(base, "pascal") == 0) return "pascal";
    if (strcasecmp(base, "clike") == 0) return "clike";
    if (strcasecmp(base, "rea") == 0) return "rea";
    if (strcasecmp(base, "pscalvm") == 0) return "pscalvm";
    if (strcasecmp(base, "pscaljson2bc") == 0) return "pscaljson2bc";
    if (strcasecmp(base, "dascal") == 0) return "dascal";
    if (strcasecmp(base, "pscald") == 0) return "pscald";
    if (strcasecmp(base, "pscalasm") == 0) return "pscalasm";
    if (strcasecmp(base, "sh") == 0) return "exsh";
    if (strcasecmp(base, "exsh") == 0) return "exsh";
    return NULL;
}

int shellMaybeExecShebangTool(const char *path, char *const *argv) {
    if (!path || !*path) {
        return -1;
    }
    char *line = NULL;
    if (!shellReadShebangLine(path, &line)) {
        return -1;
    }

    char *tokens[8];
    size_t token_count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(line, " \t", &saveptr);
    while (token && token_count < 8) {
        tokens[token_count++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }
    if (token_count == 0) {
        free(line);
        return -1;
    }

    size_t interpreter_index = 0;
    const char *interpreter = tokens[interpreter_index];
    const char *base = strrchr(interpreter, '/');
    base = base ? base + 1 : interpreter;
    if (strcmp(base, "env") == 0 && token_count >= 2) {
        interpreter_index = 1;
        interpreter = tokens[interpreter_index];
    }
    const char *tool_name = shellResolveToolName(interpreter);
    if (!tool_name) {
        free(line);
        return -1;
    }
    if (strcasecmp(tool_name, "exsh") == 0) {
        int status = shellRunExshShebang(path, argv);
        free(line);
        return status;
    }

    size_t shebang_argc = 0;
    if (token_count > interpreter_index + 1) {
        shebang_argc = token_count - (interpreter_index + 1);
    }
    size_t script_argc = 0;
    if (argv) {
        while (argv[1 + script_argc]) {
            script_argc++;
        }
    }

    size_t total_args = 1 + shebang_argc + 1 + script_argc;
    char **tool_argv = (char **)calloc(total_args, sizeof(char *));
    if (!tool_argv) {
        free(line);
        return EXIT_FAILURE;
    }

    bool ok = true;
    size_t idx = 0;
    tool_argv[idx++] = strdup(tool_name);
    for (size_t i = 0; ok && i < shebang_argc; ++i) {
        tool_argv[idx++] = strdup(tokens[interpreter_index + 1 + i]);
        if (!tool_argv[idx - 1]) {
            ok = false;
        }
    }
    tool_argv[idx++] = strdup(path);
    if (!tool_argv[idx - 1]) {
        ok = false;
    }
    for (size_t i = 0; ok && i < script_argc; ++i) {
        const char *arg = argv[1 + i];
        tool_argv[idx++] = strdup(arg ? arg : "");
        if (!tool_argv[idx - 1]) {
            ok = false;
        }
    }

    int status = EXIT_FAILURE;
    if (ok) {
        status = shellSpawnToolRunner(tool_name, (int)total_args, tool_argv);
    } else {
        fprintf(stderr, "%s: out of memory launching tool runner\n", tool_name);
    }

    for (size_t i = 0; i < total_args; ++i) {
        free(tool_argv[i]);
    }
    free(tool_argv);
    free(line);
    return status;
}
#endif
void shellSymbolTableScopeInit(ShellSymbolTableScope *scope) {
    if (!scope) {
        return;
    }
    memset(scope, 0, sizeof(*scope));
}

bool shellSymbolTableScopePush(ShellSymbolTableScope *scope) {
    if (!scope) {
        return false;
    }

    HashTable *new_global = createHashTable();
    HashTable *new_const = createHashTable();
    HashTable *new_procedure = createHashTable();
    if (!new_global || !new_const || !new_procedure) {
        if (new_global) {
            freeHashTable(new_global);
        }
        if (new_const) {
            freeHashTable(new_const);
        }
        if (new_procedure) {
            freeHashTable(new_procedure);
        }
        return false;
    }

    scope->saved_global = globalSymbols;
    scope->saved_const_global = constGlobalSymbols;
    scope->saved_procedure_table = procedure_table;
    scope->saved_current_procedure_table = current_procedure_table;

    scope->new_global = new_global;
    scope->new_const_global = new_const;
    scope->new_procedure_table = new_procedure;
    scope->active = true;

    globalSymbols = new_global;
    constGlobalSymbols = new_const;
    procedure_table = new_procedure;
    current_procedure_table = procedure_table;
    gShellSymbolTableDepth++;
    return true;
}

void shellSymbolTableScopePop(ShellSymbolTableScope *scope) {
    if (!scope || !scope->active) {
        return;
    }

    if (gShellSymbolTableDepth > 0) {
        gShellSymbolTableDepth--;
    }

    if (globalSymbols == scope->new_global) {
        freeHashTable(globalSymbols);
    }
    if (constGlobalSymbols == scope->new_const_global) {
        freeHashTable(constGlobalSymbols);
    }
    if (procedure_table == scope->new_procedure_table) {
        freeHashTable(procedure_table);
    }

    globalSymbols = scope->saved_global;
    constGlobalSymbols = scope->saved_const_global;
    procedure_table = scope->saved_procedure_table;
    current_procedure_table = scope->saved_current_procedure_table;

    scope->new_global = NULL;
    scope->new_const_global = NULL;
    scope->new_procedure_table = NULL;
    scope->active = false;
}

bool shellSymbolTableScopeIsActive(void) {
    return gShellSymbolTableDepth > 0;
}

char *shellLoadFile(const char *path) {
    if (!path) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Unable to open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    /* To the end and not by its size, since a script can come from a pipe. */
    size_t capacity = 4096;
    size_t len = 0;
    char *buffer = (char *)malloc(capacity);
    while (buffer) {
        size_t got = fread(buffer + len, 1, capacity - len - 1, f);
        len += got;
        if (got == 0) {
            break;
        }
        if (len + 1 == capacity) {
            capacity *= 2;
            char *grown = (char *)realloc(buffer, capacity);
            if (!grown) {
                free(buffer);
            }
            buffer = grown;
        }
    }
    if (!buffer || ferror(f)) {
        int err = buffer ? errno : ENOMEM;
        fclose(f);
        free(buffer);
        errno = err;
        return NULL;
    }
    fclose(f);
    buffer[len] = '\0';
    return buffer;
}

/* `return 1 2` and the like drop what the shell last read, the rest of its line with it, and
 * it reads on from there. The whole text was compiled at once here, so what is left of it is
 * compiled again. A substitution is one command, and ends. */
static InterpretResult shellRunAfterDiscard(VM *vm, const ShellProgram *program, const ShellRunOptions *options,
                                            size_t redirect_scope_depth, const ShellDiscardBase *base,
                                            InterpretResult result) {
    BytecodeChunk rest_chunk;
    bool rest_chunk_live = false;
    while (shellRuntimeConsumeDiscard()) {
        shellRuntimeAbandonCommand(base);
        shellRuntimeUnwindRedirectScopes(redirect_scope_depth);
        if (options->inherit_locals) {
            shellRuntimeSetLastStatus(1);
            break;
        }
        int line = 0;
        if (vm->chunk && vm->chunk->lines && vm->lastInstruction) {
            line = vm->chunk->lines[vm->lastInstruction - vm->chunk->code];
        }
        size_t next = 0;
        while (next < program->commands.count && program->commands.items[next] &&
               program->commands.items[next]->line <= line) {
            next++;
        }
        if (rest_chunk_live) {
            vm->chunk = NULL;
            freeBytecodeChunk(&rest_chunk);
            rest_chunk_live = false;
        }
        if (next >= program->commands.count) {
            break;
        }
        ShellProgram rest = *program;
        rest.commands.items += next;
        rest.commands.count -= next;
        shellCompile(&rest, &rest_chunk);
        rest_chunk_live = true;
        vmResetExecutionState(vm);
        result = interpretBytecode(vm, &rest_chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
        if (!options->keep_line_state) {
            shellRuntimeFinishChunk(vm);
        }
        shellRuntimeUnwindRedirectScopes(redirect_scope_depth);
    }
    if (rest_chunk_live) {
        vm->chunk = NULL;
        freeBytecodeChunk(&rest_chunk);
    }
    return result;
}

/* Bash's name for what it was reading: a file by its name, anything else as $0 and the kind
 * of text. At a prompt it is only the shell's own name, with no line number and no line. */
static void shellReportSyntaxError(const ShellParser *parser, const char *path) {
    static const struct {
        const char *path;
        const char *kind;
    } kinds[] = {
        {"<command>", "-c: "},
        {"<eval>", "eval: "},
        {"<command-substitution>", "command substitution: "},
        {"<process-substitution>", "process substitution: "},
        {"<exit-trap>", "exit trap: "},
        {"<err-trap>", "error trap: "},
        {"<debug-trap>", "debug trap: "},
        {"<return-trap>", "return trap: "},
        {"<trap", "trap: "}, /* And the rest of the label is the signal's number. */
    };
    const char *name = shellRuntimeGetArg0();
    if (!name || !*name) {
        name = "exsh";
    }
    const char *kind = "";

    bool from_prompt = path && (strcmp(path, "<stdin>") == 0 || strcmp(path, "<eval>") == 0);
    if (from_prompt && shellRuntimeIsInteractive()) {
        const char *slash = strrchr(name, '/');
        const char *shell = slash ? slash + 1 : name;
        for (size_t i = 0; i < parser->warning_count; ++i) {
            fprintf(stderr, "%s: %s\n", shell, parser->warnings[i].message);
        }
        if (parser->syntax_error) {
            fprintf(stderr, "%s: %s\n", shell, parser->syntax_error_message ? parser->syntax_error_message : "");
        }
        return;
    }
    if (path && path[0] == '<') {
        /* <stdin> has no kind: a script read that way goes by $0 alone. */
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
            if (strncmp(path, kinds[i].path, strlen(kinds[i].path)) == 0) {
                kind = kinds[i].kind;
                break;
            }
        }
    } else if (path && *path) {
        name = path;
    }
    /* A warning comes while the text runs, so it has no kind. */
    for (size_t i = 0; i < parser->warning_count; ++i) {
        fprintf(stderr, "%s: line %d: %s\n", name, parser->warnings[i].line, parser->warnings[i].message);
    }
    if (parser->syntax_error) {
        int line = parser->syntax_error_line;
        fprintf(stderr, "%s: %sline %d: %s\n", name, kind, line,
                parser->syntax_error_message ? parser->syntax_error_message : "");
        if (parser->syntax_error_text) {
            fprintf(stderr, "%s: %sline %d: `%s\'\n", name, kind, line, parser->syntax_error_text);
        }
    }
}

int shellRunSource(const char *source,
                   const char *path,
                   const ShellRunOptions *options,
                   bool *out_exit_requested) {
    if (out_exit_requested) {
        *out_exit_requested = false;
    }
    if (!source || !options) {
        return EXIT_FAILURE;
    }
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] enter path=%s source='%s'\n",
                path ? path : "(null)",
                source ? source : "(null)");
    }

#if defined(SIGPIPE)
    static bool sigpipe_ignored = false;
    if (!sigpipe_ignored) {
        signal(SIGPIPE, SIG_IGN);
        sigpipe_ignored = true;
    }
#endif

    bool previous_suppress = shellSemanticsWarningsSuppressed();
    shellSemanticsSetWarningSuppressed(options->suppress_warnings);

    bool previous_exit_on_signal = shellRuntimeExitOnSignal();

    shellRuntimePushScript();

    bool source_pushed = false;
    if (!shellRuntimeTrackSourcePush(path ? path : "")) {
        shellRuntimePopScript();
        shellRuntimeSetExitOnSignal(previous_exit_on_signal);
        shellSemanticsSetWarningSuppressed(previous_suppress);
        return EXIT_FAILURE;
    }
    source_pushed = true;

    vmSetVerboseErrors(options->verbose_errors);

    if (options->exit_on_signal) {
        shellRuntimeSetExitOnSignal(true);
    }

    ShellSymbolTableScope table_scope;
    shellSymbolTableScopeInit(&table_scope);
    bool table_scope_owned = false;
    if (!shellSymbolTableScopeIsActive()) {
        if (!shellSymbolTableScopePush(&table_scope)) {
            fprintf(stderr, "shell: failed to allocate symbol tables.\n");
            if (source_pushed) {
                shellRuntimeTrackSourcePop();
                source_pushed = false;
            }
            shellRuntimePopScript();
            shellRuntimeSetExitOnSignal(previous_exit_on_signal);
            shellSemanticsSetWarningSuppressed(previous_suppress);
            return EXIT_FAILURE;
        }
        table_scope_owned = true;
    } else {
        current_procedure_table = procedure_table;
    }
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] symbols-ready\n");
    }
    registerAllBuiltins();
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] builtins-registered\n");
    }

    int exit_code = EXIT_FAILURE;
    ShellProgram *program = NULL;
    ShellSemanticContext sem_ctx;
    bool sem_ctx_initialized = false;
    BytecodeChunk chunk;
    bool chunk_initialized = false;
    /* eval, source and a trap work on the shell's own state and a script may call them in a loop,
     * where building and freeing a VM was 40% of an eval. They keep one, as functions do, and
     * one that runs inside another has to make its own. */
    static VM shared_vm;
    static bool shared_vm_ready = false;
    static bool shared_vm_in_use = false;
    bool using_shared_vm = false;
    VM *vm = NULL;
    bool vm_initialized = false;
    bool left_stages = false;
    ShellRuntimeState *vm_shell_ctx = NULL;
    VM *previous_vm_for_context = NULL;
    bool vm_context_swapped = false;
    bool vm_stack_dumped = false;
    VM *previous_thread_owner = NULL;
    bool assigned_thread_owner = false;
    bool exit_flag = false;
    bool should_run_exit_trap = false;
    bool trap_exit_requested = false;
    bool syntax_error_later = false;

    /* `#ifdef` is a comment in a shell script, and in a string or here document it is text. */
    const char *parse_src = source;

    ShellParser parser;
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] parsing\n");
    }
    program = shellParseStringAtLine(parse_src, &parser, options->first_line);
    syntax_error_later = parser.syntax_error && program;
    if (!syntax_error_later && (parser.syntax_error || parser.warning_count > 0)) {
        shellReportSyntaxError(&parser, path);
    }
    if (!syntax_error_later) {
        shellParserFree(&parser);
    }
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] parsed had_error=%d program=%p\n",
                (int)parser.had_error,
                (void *)program);
    }
    if ((parser.had_error && !syntax_error_later) || !program) {
        if (!parser.syntax_error) {
            fprintf(stderr, "Parsing failed.\n");
        }
        exit_code = parser.syntax_error ? parser.syntax_error_status : 2;
        goto cleanup;
    }

    /* eval '' and an empty sourced file succeed, whatever came before. */
    if (options->share_runtime_context && program->commands.count == 0) {
        shellRuntimeSetLastStatus(0);
    }

    if (options->dump_ast_json) {
        shellDumpAstJson(stdout, program);
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    shellInitSemanticContext(&sem_ctx);
    sem_ctx_initialized = true;
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] semantic-analyze\n");
    }
    ShellSemanticResult sem_result = shellAnalyzeProgram(&sem_ctx, program);
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] semantic-done err=%d warn=%d\n",
                sem_result.error_count,
                sem_result.warning_count);
    }
    if (sem_result.warning_count > 0 && !options->suppress_warnings) {
        fprintf(stderr, "Semantic analysis produced %d warning(s).\n", sem_result.warning_count);
    }
    if (sem_result.error_count > 0) {
        fprintf(stderr, "Semantic analysis failed with %d error(s).\n", sem_result.error_count);
        goto cleanup;
    }

    initBytecodeChunk(&chunk);
    chunk_initialized = true;
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] chunk-init\n");
    }
    bool used_cache = false;
    if (!options->no_cache && path && path[0]) {
        used_cache = loadBytecodeFromCache(path, kShellCompilerId, options->frontend_path, NULL, 0, &chunk);
    }

    if (!used_cache) {
        ShellOptConfig opt_config = { false };
        shellRunOptimizations(program, &opt_config);
        shellCompile(program, &chunk);
        if (getenv("PSCALI_PROMPT_DEBUG")) {
            fprintf(stderr, "[shell-run] compile-done\n");
        }
        if (!options->no_cache && path && path[0]) {
            saveBytecodeToCache(path, kShellCompilerId, &chunk);
        }
        if (!options->quiet) {
            fprintf(stderr, "Compilation successful. Bytecode size: %d bytes, Constants: %d\n",
                    chunk.count, chunk.constants_count);
        }
        if (options->dump_bytecode) {
            fprintf(stderr, "--- Compiling Shell Script to Bytecode ---\n");
            const char* disasm_name = path ? bytecodeDisplayNameForPath(path) : "script";
            disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
            if (!options->dump_bytecode_only) {
                fprintf(stderr, "\n--- executing Script with VM ---\n");
            }
        }
    } else {
        if (!options->quiet) {
            fprintf(stderr, "Loaded cached bytecode. Bytecode size: %d bytes, Constants: %d\n",
                    chunk.count, chunk.constants_count);
        }
        if (options->dump_bytecode) {
            const char* disasm_name = path ? bytecodeDisplayNameForPath(path) : "script";
            disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
            if (!options->dump_bytecode_only) {
                fprintf(stderr, "\n--- executing Script with VM (cached) ---\n");
            }
        }
    }

    if (options->dump_bytecode_only) {
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    if (options->share_runtime_context && !__atomic_test_and_set(&shared_vm_in_use, __ATOMIC_ACQUIRE)) {
        if (!shared_vm_ready) {
            initVM(&shared_vm);
            shared_vm_ready = true;
        } else {
            vmResetExecutionState(&shared_vm);
        }
        vm = &shared_vm;
        using_shared_vm = true;
    } else {
        vm = (VM *)calloc(1, sizeof(VM));
        if (!vm) {
            fprintf(stderr, "shell: failed to allocate VM instance.\n");
            goto cleanup;
        }
        initVM(vm);
    }
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] vm-init\n");
    }
    vm_initialized = true;
    if (options->share_runtime_context) {
        vm->frontendContext = shellRuntimeCurrentContext();
    } else {
        vm_shell_ctx = shellRuntimeCreateContext();
        if (!vm_shell_ctx) {
            fprintf(stderr, "shell: failed to allocate shell runtime context.\n");
            goto cleanup;
        }
        if (options->inherit_locals) {
            /* The top frame is the substitution's own marker. */
            shellRuntimeContextInheritLocals(vm_shell_ctx, 1);
        } else {
            shellRuntimeContextResetReturnScope(vm_shell_ctx);
        }
        shellRuntimeContextSetTextDepth(vm_shell_ctx, shellRuntimeIsOutermostScript() ? 0 : 1);
        vm->frontendContext = vm_shell_ctx;
    }
    previous_thread_owner = gShellThreadOwnerVm;
    if (!gShellThreadOwnerVm) {
        gShellThreadOwnerVm = vm;
        assigned_thread_owner = true;
    }
    vm->threadOwner = gShellThreadOwnerVm ? gShellThreadOwnerVm : vm;
    vm->trace_head_instructions = options->vm_trace_head > 0 ? options->vm_trace_head : 0;
    previous_vm_for_context = shellSwapCurrentVm(vm);
    vm_context_swapped = true;

    size_t redirect_scope_depth = shellRuntimeRedirectScopeDepth();
    ShellDiscardBase discard_base;
    shellRuntimeDiscardBase(&discard_base);
    void *substitution_traps = options->inherit_locals ? shellRuntimeSubstitutionTrapsPutAside() : NULL;
    InterpretResult result = interpretBytecode(vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
    left_stages = !options->share_runtime_context && shellRuntimeChunkLeftStages();
    /* At a prompt that waits for the line after: a known difference, listed in the release notes. */
    if (!options->keep_line_state) {
        shellRuntimeFinishChunk(vm);
    }
    shellRuntimeUnwindRedirectScopes(redirect_scope_depth);
    if (!options->share_runtime_context) {
        result = shellRunAfterDiscard(vm, program, options, redirect_scope_depth, NULL, result);
    } else if (shellRuntimeDiscardStaysInText()) {
        result = shellRunAfterDiscard(vm, program, options, redirect_scope_depth, &discard_base, result);
    }
    shellRuntimeSubstitutionTrapsBack(substitution_traps);
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] interpret-done result=%d\n", (int)result);
    }
    if (result == INTERPRET_RUNTIME_ERROR) {
        runtimeDebugLog("[shell] interpretBytecode -> runtime error; dumping VM stack");
        vmDumpStackInfoDetailed(vm, "shell runtime error");
        vm_stack_dumped = true;
    }
    int last_status = shellRuntimeLastStatus();
    exit_flag = shellRuntimeConsumeExitRequested();
    if (result == INTERPRET_RUNTIME_ERROR && exit_flag) {
        result = INTERPRET_OK;
    }
    /* A line at the prompt is not the end of a script: the prompt runs it when it ends. */
    should_run_exit_trap = !options->keep_line_state && shellRuntimeIsOutermostScript() &&
                           (!shellRuntimeIsInteractive() || exit_flag);
    exit_code = (result == INTERPRET_OK) ? last_status : EXIT_FAILURE;
    if (syntax_error_later) {
        syntax_error_later = false;
        /* What ended the run before bash would read that far means nothing reports it. */
        const volatile bool *errexit_pending = shellRuntimePendingExitFlag();
        if (!exit_flag && !shellRuntimeReturnPending() && !(errexit_pending && *errexit_pending)) {
            shellReportSyntaxError(&parser, path);
            exit_code = parser.syntax_error_status;
            shellRuntimeSetLastStatus(exit_code);
        }
        shellParserFree(&parser);
    }
    {
        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf),
                 "[shell] interpret result=%d last_status=%d exit_flag=%s exit_code=%d",
                 (int)result,
                 last_status,
                 exit_flag ? "true" : "false",
                 exit_code);
        runtimeDebugLog(log_buf);
    }
    if (exit_code != EXIT_SUCCESS && vm && !vm_stack_dumped) {
        char dump_label[64];
        snprintf(dump_label, sizeof(dump_label), "shell exit code %d", exit_code);
        vmDumpStackInfoDetailed(vm, dump_label);
        vm_stack_dumped = true;
    }
cleanup:
    if (syntax_error_later) {
        shellReportSyntaxError(&parser, path);
        exit_code = parser.syntax_error_status;
        shellParserFree(&parser);
    }
    if (should_run_exit_trap) {
        /* What the script's last command was sent. */
        shellRuntimeProcessPendingSignals();
        if (shellRuntimeConsumeExitRequested()) {
            exit_flag = true;
            exit_code = shellRuntimeLastStatus();
        }
        shellRuntimeRunExitTrap();
        trap_exit_requested = shellRuntimeConsumeExitRequested();
        exit_flag = exit_flag || trap_exit_requested;
        if (trap_exit_requested) {
            exit_code = shellRuntimeLastStatus();
        }
    }
    if ((exit_code != EXIT_SUCCESS || exit_flag) ) {
        char final_log[256];
        snprintf(final_log, sizeof(final_log),
                 "[shell] final exit_code=%d exit_flag=%s trap_exit=%s",
                 exit_code,
                 exit_flag ? "true" : "false",
                 trap_exit_requested ? "true" : "false");
        runtimeDebugLog(final_log);
    }
    if (exit_code != EXIT_SUCCESS && vm && !vm_stack_dumped) {
        char dump_label[64];
        snprintf(dump_label, sizeof(dump_label), "shell final exit %d", exit_code);
        vmDumpStackInfoDetailed(vm, dump_label);
        vm_stack_dumped = true;
    }
    if (source_pushed) {
        shellRuntimeTrackSourcePop();
        source_pushed = false;
    }
    shellSemanticsSetWarningSuppressed(previous_suppress);
    shellRuntimeSetExitOnSignal(previous_exit_on_signal);
    if (out_exit_requested) {
        *out_exit_requested = exit_flag;
    } else {
        (void)exit_flag;
    }
    if (vm_context_swapped) {
        shellRestoreCurrentVm(previous_vm_for_context);
        vm_context_swapped = false;
    }
    /* From the context that pushed it. Popped from this run's copy, the caller stays a level
     * too deep and no longer counts as the outermost script: no EXIT trap. */
    shellRuntimePopScript();
    if (vm_shell_ctx && options->keep_line_state) {
        shellRuntimeKeepLineState(vm_shell_ctx);
    }
    if (assigned_thread_owner) {
        gShellThreadOwnerVm = previous_thread_owner;
    }
    if (using_shared_vm) {
        pscalRuntimeVmFlushOutput();
        vmResetExecutionState(vm);
        vm->frontendContext = NULL;
        vm = NULL;
        __atomic_clear(&shared_vm_in_use, __ATOMIC_RELEASE);
    } else if (vm_initialized && vm && !left_stages) {
        freeVM(vm);
    }
    if (vm_shell_ctx && !left_stages) {
        shellRuntimeDestroyContext(vm_shell_ctx);
    }
    vm_shell_ctx = NULL;
    if (vm && !left_stages) {
        free(vm);
    }
    if (chunk_initialized) {
        freeBytecodeChunk(&chunk);
    }
    if (sem_ctx_initialized) {
        shellFreeSemanticContext(&sem_ctx);
    }
    if (program) {
        shellFreeProgram(program);
    }
    if (table_scope_owned) {
        shellSymbolTableScopePop(&table_scope);
    }
    vmOpcodeProfileDump();
    return exit_code;
}
