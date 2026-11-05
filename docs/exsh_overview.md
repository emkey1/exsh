# exsh Front End

The exsh front end compiles POSIX-style shell scripts to PSCAL bytecode and
executes them through the shared virtual machine. It focuses on orchestrating
external processes, pipelines, and the existing PSCAL builtin catalog instead of
implementing a full interactive shell.

## Command line usage

Build the project with CMake and invoke the new `exsh` executable:

```sh
cmake -S . -B build
cmake --build build
build/bin/exsh script.exsh [arguments]
```

`exsh` accepts the same tracing and bytecode inspection flags as the other front
ends:

- `-v` prints the generated version string and latest git tag.
- `-c <command> [name] [args...]` executes an inline command string. When
  additional arguments follow the string, the first argument becomes `$0` for
  diagnostics and the remainder populate the positional parameters so inline
  runs match Bash semantics.
- `--dump-ast-json` writes the parsed program to stdout in JSON form.
- `--dump-bytecode` disassembles the compiled bytecode before execution.
- `--dump-bytecode-only` disassembles and exits without running the VM.
- `--dump-ext-builtins` lists available shell builtins.
- `--no-cache` compiles fresh; it neither reads nor writes the bytecode cache.
- `--prune-cache` removes cache entries whose source file no longer exists, then exits.
- `--semantic-warnings` enables semantic analysis diagnostics prior to execution.
- `--vm-trace-head=<N>` traces the first `N` VM instructions (the same toggle as
  the Pascal and CLike front ends).

Example scripts live under `Examples/exsh/` and cover pipelines, conditionals,
and environment-aware builtins.

Omitting the command string after `-c` emits the diagnostic `exsh: -c: option
requires an argument` and exits with status `2`, mirroring Bash's behaviour.

## Interactive mode

When `exsh` starts with a controlling TTY it first looks for `~/.exshrc`. If the
file exists it is executed with caching disabled; requesting `exit` inside the
file terminates the shell before the interactive loop begins.【F:src/shell/main.c†L178-L241】
Job control is initialised after the RC hook runs. The runtime only enables job
control when `exsh` owns the terminal; otherwise foreground/background helpers
degrade gracefully.【F:src/shell/main.c†L2413-L2420】【F:src/backend_ast/shell.c†L404-L485】【F:src/backend_ast/shell.c†L3729-L3731】

### Prompt customisation

`exsh` honours the `PS1` environment variable for its prompt. Bash-style escape
sequences are expanded, including time specifiers (`\t`, `\T`, `\@`, `\A`,
`\d`, `\D`), working-directory markers (`\w`, `\W`), user and host tokens
(`\u`, `\h`, `\H`), alert/escape characters, and octal/hex escapes. If `PS1`
is unset, the shell defaults to `exsh$ `.【F:src/shell/main.c†L354-L557】

### Line editing, search, and completion

Interactive sessions provide an Emacs-style editing experience. Common control
keys jump to the start/end of the line (`Ctrl-A`, `Ctrl-E`), move the cursor
(`Ctrl-B`, `Ctrl-F`), and navigate history (`Ctrl-P`, `Ctrl-N`). Kill and yank
commands (`Ctrl-U`, `Ctrl-K`, `Ctrl-W`, `Ctrl-Y`) manipulate the kill buffer,
while `Ctrl-L` repaints the screen. Incremental reverse search (`Ctrl-R`) and
`Alt-.` style last-argument insertion reuse the recorded history list. Tab
completion expands the current word via pathname globbing and prints ambiguous
matches in the familiar column layout.【F:src/shell/main.c†L1562-L1760】【F:src/shell/main.c†L2089-L2165】【F:src/shell/main.c†L1120-L1284】

### History handling

The interactive loop performs history expansion before executing a command and
echoes the expanded text when appropriate. All non-empty lines are recorded so
subsequent sessions can navigate, search, and reinsert previous commands. Tilde
expansion runs after history resolution so `!$` and friends can still be joined
with home-directory shortcuts.【F:src/shell/main.c†L2295-L2335】【F:src/backend_ast/shell.c†L3733-L3800】【F:src/backend_ast/shell.c†L3865-L3923】

History persistence is opt-in. Set `HISTFILE` (for example to
`$HOME/.exsh_history`) to load existing entries at startup and append each new
interactive line. `HISTSIZE` limits the in-memory list, and `HISTFILESIZE`
prunes the on-disk history to the last N entries (both default to 1000/2000
when unset). Writes use append mode to avoid clobbering other tabs; trimming
only rewrites the tail of the file when limits are exceeded.【F:src/shell/main.c†L3636-L3640】【F:src/backend_ast/shell/shell_word_expansion.inc†L196-L289】

## Bytecode caching

Compiled scripts are cached in `~/.pscal/bc_cache`. Cache entries are now keyed
by both the source path and the compiler identifier reported by exsh, so the front
end can coexist with Pascal, CLike and Rea bytecode without collisions. Use
`--no-cache` to force a recompile for the current script, and `--prune-cache`
to remove the entries (of every front end) whose source file no longer exists.
It judges nothing else, and keeps any entry it cannot check; `--verbose` lists
each entry it removes or keeps.

## Environment variables

The exsh front end honours the standard process environment:

- What a program sees is what bash would pass it: variables that came in from the
  environment or were exported, plus a `NAME=value` prefix on the command, which also
  reaches what a function, `eval`, `.` or `command` starts. Other variables, arrays and
  locals that are not exported stay in the shell, whatever their size. A local exports on
  its own and takes on the export of the name it hides. On iOS, where nothing execs, each
  program gets a private copy of that environment instead: applets, frontends, a nested
  `sh` and a script run as a command all read that, what they set stays there, and a
  thread they start reads it too. Code that reads the process environment directly (Go
  tools, `VPROC_SHIM_DISABLED` sources) still sees every variable of the session's shell.
- `EXSH_LAST_STATUS` mirrors the most recent exit status observed by the
  runtime and is updated after every builtin or pipeline execution.
- Caching relies on `$HOME` to locate the cache directory and `$PATH` to resolve
  executables when necessary.

Direct parameter interpolation now behaves like a POSIX shell: `$NAME`,
`${NAME}`, `$?`, `$#`, `$@`, `$*`, and positional parameters expand to the
current environment and argument vector. `setenv` accepts `NAME [VALUE]` to set
variables (printing the environment when invoked without arguments), and the
new `unsetenv` builtin mirrors `unset` for scripts that prefer csh-style
naming. The VM argument vector (`$0`, `$1` …) maps directly to the parameters
passed after the script path (`gParamValues` inside the VM), so invoking
`build/bin/exsh script.exsh 1 2 3` exposes `1`, `2`, and `3` to the program.

Interactive sessions also support history expansion beyond `!!`. Numeric
designators like `!-2` and `!42`, prefix/substring searches (`!foo`, `!?bar?`),
and word designators (`!$`, `!*`, `!^`, `!:2`) work against the recorded
history before execution, providing the usual convenience shorthands. Regular
expression lookups (`!?/expr/?`) and substitution designators (`!:s/old/new/` or
`!:gs/old/new/`) are honoured as well, with replacement strings supporting tab
escapes via `\t`.

### Interactive startup configuration

When `exsh` starts in interactive mode it looks for `~/.exshrc` and sources the
file before presenting the prompt. The repository includes a ready-to-use
template under `etc/skel/.exshrc` that configures a colourful prompt, quality of
life aliases, PSCAL builtin helpers, and hooks for per-machine overrides. Copy
the file to your home directory (or use it as a reference when building your
own) to get a sensible default environment immediately.
## Builtins and interaction with the VM

exsh reuses the shared builtin registry defined in `backend_ast/builtin.c` and
extends it with orchestration helpers implemented in
`backend_ast/shell.c`. The following categories are available out of the box:

- What a script without PSCAL extensions gets is meant to be what bash does, down
  to the status and the words a command sees: quoting, all of `${...}`, arithmetic,
  `[[ ]]` with `=~`, brace and tilde expansion, arrays, process substitution and
  `set -aCefuvx`. Aliases expand at a prompt, and in a script only after
  `shopt -s expand_aliases`. The `bash_parity_*` tests compare the two directly. Still
  different: `select`, `compgen`/`complete`/`compopt`, `suspend`, `declare -n`,
  `printf '%(fmt)T'` and `exit abc`. A job's text in `jobs` is as it was
  written, where bash lays a compound command out again, and `jobs -l` gives a pipeline one
  line. A hard limit set in `$(...)` on a host sets only the soft one, as the substitution
  is this process.
  `set` and `declare` list variables
  but not functions, since a function keeps no source to print. A failed
  assignment with no command, such as `readonly r=1; r=2`, ends the script where bash
  goes on with the next line. Failed arithmetic in a word drops the rest of its line and
  the script reads on, as under bash, but after a compound command over several lines what
  follows it on its last line still runs. Arithmetic syntax errors say `syntax error`, as
  bash does up to 5.2, where 5.3 says `arithmetic syntax error`. Unquoted parentheses among
  a command's words are plain words,
  so `find . ( -name a )` runs where bash reports a syntax error. `set -o posix` does not make a
  failing special builtin end the script, `set -v` is accepted but prints nothing,
  `set -x` does not show compound commands, a `DEBUG` trap runs only for top-level
  commands, and `[[ ]]` expands all of its operands first and runs one bash can't parse, such
  as `[[ a == ]]`. On iOS, where nothing
  execs, `./f` runs without the execute bit and `cd` resolves links at once; `/bin/x`
  means `x`, so `/bin/cd` moves the shell, and `which` names builtins and applets, since
  they have no file; `exec` and `env` run a program as a call, and `exec -a` and `-l`
  do not change its name. `>(cmd)` is a file that the command reads when the command
  naming it is done, so under `exec 3> >(cmd)` or `exec > >(cmd)` it runs at once, finds
  the file empty, and what the script writes afterwards is lost.
  A signal that would end the shell there takes effect at its next command, after any
  foreground command or `wait`, and its caller sees status 128 + the signal rather than a death by
  signal; and a foreground applet is not a child, so `trap ... CHLD` runs only for
  subshells and jobs.
- A command substitution runs in the shell's own process, so that
  `tid=$(builtin ThreadSpawnBuiltin ...)` leaves the thread where the script can wait for
  it. Otherwise it behaves as a subshell: `exec cmd` in one runs the command and ends only
  the substitution, `exec 3> f` in one lasts until it ends, and its background jobs are not
  the shell's afterwards. The shell's jobs are no children of its own: it can list them,
  `wait` does not wait for them, and `wait -n` gives 0 at once for each, then 127, as
  bash does. It reads to end of file, so it waits for a job that still holds
  its output, as bash does, and an interrupt ends that wait. Still different: an `EXIT`
  trap set in a substitution does not run, and a trap it sets takes that signal even when
  it was sent to the shell. On iOS `x=$(a | b > f &)` also waits for the job, whose last
  stage keeps a copy of the pipe.
- `{name}> f`, with any redirection operator, lets the shell pick a descriptor from 10 up
  and puts its number in `name`, which may be an array element. It stays open after the
  command until the script closes it with `{name}>&-`. That goes for `exec`, a builtin, a
  function and a compound command. On a program, a stage or a job it ends with them and
  the shell never hears of the number, as under bash, where each of those is a process;
  with no command at all it only makes the file. In a substitution it ends with the
  substitution. `command prog` counts as the program, and `shopt -s varredir_close` closes
  the descriptor when the command ends, all but `exec`'s, and leaves the name set. A
  word that a `{name}` before it can change, or that holds `$(...)`, expands when its
  redirection's turn comes, so `exec {a}> f {b}>&$a` works. Other words still expand
  before any redirection acts, and that only shows through `${x:=...}`, whose assignment
  stays in the shell on a program, where under bash the process has it.
- Shell control builtins (`cd`, `pwd`, `exit`, `alias`, `export`, `setenv`,
  `unset`, `unsetenv`) and an interactive `help` builtin for listing and
  describing available commands. The directory-stack helpers (`dirs`, `pushd`,
  `popd`) mirror Bash: they maintain `$PWD`, print the stack after each
  mutation, and reject unsupported flag combinations so parity tests stay
  precise.【F:src/backend_ast/shell/shell_builtins.inc†L2495-L2627】【F:Tests/exsh/tests/bash_parity_directory_stack.exsh†L1-L32】
- Pipeline helpers (`__shell_exec`, `__shell_exec_body`, `__shell_pipeline`,
  `__shell_and`, `__shell_or`, `__shell_subshell`, `__shell_loop`, `__shell_if`).
  A subshell, and any compound command that is a pipeline stage or in the
  background, compiles to a chunk of its own that `__shell_exec_body` starts as one
  command. It runs apart from the shell: a forked child on a host, a thread and
  vproc with a private environment and definitions on iOS. A builtin or function in
  the background or as a stage runs the same way there, so what it changes stays
  there, its file mask with the rest. A simple command in the background or as a
  stage expands its words on the shell before it starts when all they do is read
  variables, and an expansion that fails there (`set -u`) is that command's failure,
  not the shell's. When its words can do more (`$(...)`, `$((...))`, `${v:=x}`,
  `${v:?x}`, a subscript, `$RANDOM`, `$BASHPID`) or the command is `exec`, it starts
  as a body too, so they expand in it. The command then becomes the job or stage: an
  exec with no fork on a host, the job's own thread and vproc on iOS. So `$!`, `wait`
  and `kill` mean the command, as they do for a job whose words are plain. `jobs`
  names such a job by its words as written. `__shell_subshell` only
  opens and closes the redirections of a compound command. When that command is all
  a body runs, nothing runs after its redirections, so no copy of what they replaced
  is kept: a background job that sends its output elsewhere does not hold the caller's
  stdout open.
- The complete PSCAL builtin catalog (HTTP, sockets, JSON, extended math/string
  groups, optional SQLite/SDL bindings) via `registerAllBuiltins()`.
- Command-dispatch diagnostics such as `hash` and `enable` let
  scripts inspect the cached PATH table or enumerate disabled builtins without
  leaving the shell.【F:src/backend_ast/shell/shell_builtins.inc†L7243-L7310】

The `builtin` shell command bridges these catalogs into scripts. Invoke it with
the VM builtin name followed by any arguments. Values are treated as strings by
default; prefix a token with `int:`, `float:`/`double:`/`real:`, `bool:` or
`str:` to coerce the argument to the corresponding VM type. Supplying `nil` (or
`nil:`) passes the VM's `nil` value directly. When the target builtin returns a
value, `builtin` prints it to `stdout`; procedures that return `void` simply set
`EXSH_LAST_STATUS` to `0` on success.

High-level control-flow syntax (`if`, `while`/`until`, and `for`) lowers to the
loop helpers above, which cooperate with real VM jump opcodes. The runtime
evaluates conditions using shell truthiness rules and only executes the branch
or loop body whose guard succeeds.

Job control follows Bash's conventions. `jobs` collates the active table,
`disown` removes entries without killing them, `fg`/`bg` resume stopped
pipelines, and `wait`/`kill` honour `%`-style specifiers while normalising
signal names and integers. All helpers consult the shared job ledger so output
stays stable when pipelines fork or launch nested processes.【F:src/backend_ast/shell/shell_builtins.inc†L7949-L8119】
One difference: every child gets a process group of its own, with job control off too,
where bash leaves a script's children in the shell's group. So `kill 0` in a script
reaches only the shell, and `kill -- -$!` finds the job where bash says no such process.

## Accessing core and extended builtins

`shellRunSource` registers the full PSCAL builtin catalog before compiling a
script, so every shell run has access to the same VM services as the Pascal and
C-like front ends.【F:src/shell/runner.c†L53-L105】 Shell-specific helpers are
compiled into the runtime and exposed through builtin handlers such as `cd`,
`jobs`, `fg`, and `bg` for job control, along with `builtin` for invoking VM
routines directly.【F:src/backend_ast/shell.c†L5985-L6767】

The `builtin` command accepts the VM builtin name followed by arguments. Prefix
tokens with `int:`, `float:`/`double:`/`real:`, `bool:`, `str:`/`raw:`, or
`nil`/`nil:` to coerce shell words into the expected VM type; otherwise values
are passed as strings. Return values are printed automatically when the
underlying builtin reports success.【F:src/backend_ast/shell.c†L3462-L3561】【F:src/backend_ast/shell.c†L6767-L6805】

Extended builtin groups are linked at build time. You can inspect the compiled
inventory by running `exsh --dump-ext-builtins`, which produces the same
category/group/function listing used by other front ends.【F:src/shell/main.c†L2351-L2387】
Available categories include math, system, SQLite, graphics, yyjson, and other
optional modules controlled by the `-DENABLE_EXT_BUILTIN_*` CMake flags.【F:Docs/extended_builtins.md†L15-L44】

EXIT traps now run with a clean execution context. The runtime snapshots exit
and loop flags before invoking the trap body, merges any new requests after it
finishes, and only escalates deferred `set -e` failures once conditional guards
have had a chance to handle them. This keeps `&&`, `||`, and `if` short-circuits
behaving like Bash while preserving pending exits when the trap requests an
override.【F:src/backend_ast/shell/shell_runtime_state.inc†L1667-L1779】
An EXIT trap sees the redirections around the `exit` that ended the script, as
under Bash, except those opened inside the text of an `eval`, a sourced file or
a trap, which are undone first. A `return` in a trap's text ends the function or
sourced file the trap broke into. A function starts without its caller's `ERR`
and `RETURN` traps unless `set -E` and `set -T` hand them on, or `declare -ft`
for that one function.

A trap's signal is noted when it arrives and its command runs at the next command
boundary, after the status of the command that was running is set. Only `wait`
stops early for it, with status 128 + the signal, and the job stays waited for.
On iOS a signal waits on the task table: each builtin, `eval` and sourced file runs
under a vproc that ends with the command, so a trap is set on the task that is the
shell (`shellRuntimeOwnPid`), not on the current vproc, and the shell's thread
collects what waits there at each boundary (`vprocDeliverTaskSignals`). A new stage
or job takes ignored signals from the shell and none of its traps
(`vprocResetCaughtSignals`, as exec would). The shell's thread is never cancelled,
even by way of a stage running on it: what would end the shell waits for it to unwind
(`vprocSetUnwindsItself`).

A substitution runs in the shell's process, where under bash it is a process of its
own that a signal sent to the shell does not reach. So what is sent waits for it, and
the shell takes it when the command the substitution was part of is done: `x=$(cmd)`
assigns, then the trap runs, and its output and its `exit` are the shell's
(`gShellSubstitutionSignals`). A trap the substitution sets itself is its own, and it
takes that signal whichever pid it was sent to, since `$$` and `$BASHPID` are one
process here. What it does to a trap ends with it. Ctrl-C reaches a foreground's whole
group and under bash a substitution's process dies of it, so `SIGINT` and `SIGQUIT`
end the body and the shell's trap runs afterwards. `kill -INT $$` from inside does the
same, where bash's substitution would go on.

## Worker threads and builtin orchestration

`ThreadSpawnBuiltin` lets shell scripts queue allow-listed VM builtins—such as
`dnslookup`, the asynchronous HTTP helpers, and `delay`—onto the shared worker
pool. Pass either the builtin name or numeric id followed by any arguments; the
call returns a thread handle. Pair it with `WaitForThread` to block until the
worker finishes and surface its status through `EXSH_LAST_STATUS`, or call
`ThreadGetResult(handle, true)` to read and immediately clear any cached
payload/status pair so the slot can be reused.【F:Examples/exsh/threading_demo†L1-L30】

`ThreadPoolSubmit` exposes the same interface but marks the call as
submit-only so interactive scripts can continue processing foreground commands.
Use `ThreadSetName` to attach descriptive labels to workers and
`ThreadLookup("label")` to map those names back to handles from other parts of
the script. `ThreadPause`, `ThreadResume`, and `ThreadCancel` expose cooperative
control over long-running assignments, while `ThreadStats` produces a JSON-ready
snapshot of pool usage for dashboards and logs.【F:src/backend_ast/builtin.c†L4930-L5794】

The bundled examples demonstrate these helpers in practice:

- `Examples/exsh/threading_demo` runs a DNS lookup alongside a timer and prints
  the stored result.
- `Examples/exsh/parallel-check` scales the pattern across an arbitrary host
  list, tagging each slot via `ThreadSetName` before reporting success and
  failure counts.

## Grammar coverage

The parser now accepts the broader POSIX grammar expected by automation-heavy
scripts. Here documents honour quoting rules (`<<word` expands variables while
`<<'word'` keeps literal text), brace groups run in the current shell context
and can appear inside pipelines, and leading `!` prefixes invert pipeline exit
statuses. `time` is a keyword in front of a pipeline, with `-p` and `TIMEFORMAT`;
quoted (`\time`) it is the command or applet. Multi-line `for` loops with backslash continuations and complex
`case` clauses split across lines (including pattern alternation with `|`) are
all recognised. Targeted regression scripts under `Tests/exsh/tests/`
exercise these constructs so future grammar work can expand safely.

Because `exsh` feeds every script through the same VM, shell programs can invoke
VM builtins directly—for example calling `HttpRequest` from a pipeline stage or
mixing shell conditionals with VM networking. Extended builtin groups that are
enabled at configure time are linked into the exsh binary automatically and
exposed through the same dispatch table as other front ends.

## Working with examples and tests

Sample scripts demonstrating pipelines, conditionals, and environment-focused
builtins live in `Examples/exsh/`. The regression suite under `Tests/exsh/`
runs these scripts through `build/bin/exsh`, checks stdout/stderr, and verifies
exit codes. The CI workflow executes the new suite alongside the Pascal, CLike,
and Rea test runs.
