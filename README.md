# exsh

**exsh** is the **PSCAL** shell front end: an interactive and scripting
POSIX-ish shell that parses shell source into the shared PSCAL AST, which the
shared bytecode compiler lowers and the PSCAL VM runs.

exsh carries no VM or code generator of its own — it builds against
[`pscal-core`](https://github.com/emkey1/pscal-core), pulled in automatically via
CMake `FetchContent`. A few shared shell pieces (the backend shell runtime and
the shared AST/builtin helpers) live in pscal-core and are compiled from there.

exsh's in-process cross-front-end integration (driving Pascal / CLike / Rea) and
its smallclue/SDL features are mobile/iOS-gated and are not part of this
standalone host build.

## Build

```sh
cmake -S . -B build      # fetches and builds pscal-core, then exsh
cmake --build build -j
./build/exsh script.sh
```

The binary lands at `build/exsh`. Run it interactively with no arguments, pass
a script path to run it, or use `-c 'command'` to run an inline command.

A plain configure gives `RelWithDebInfo` (`-O2 -g`). Pass
`-DCMAKE_BUILD_TYPE=Debug` for a debugger or a sanitizer, or `Release` for `-O3`
with no debug info. A build directory keeps the type it was first configured
with.

## Versions

exsh's version is `MAJOR.MINOR.PATCH`, kept in [`VERSION`](VERSION), with what each release
brings in [`CHANGELOG.md`](CHANGELOG.md). It is bumped once per release that changes exsh, not
per commit:

- **MAJOR** for a change that breaks a script that worked before,
- **MINOR** for new features or changed behaviour,
- **PATCH** for fixes only.

`exsh --version` (or `-v`) prints it as `exsh 2.0.0`, followed by the build's own details. A
script reads it from `$EXSH_VERSION`, or as parts from `EXSH_VERSINFO`, laid out like
`BASH_VERSINFO`: major, minor, patch, build, release status, machine. `BASH_VERSION` and
`BASH_VERSINFO` stay at the bash release exsh matches (`5.2.0(1)-exsh`), since scripts
branch on them.

## Install

```sh
cmake --install build --prefix /usr/local
```

This puts the `exsh` binary in `<prefix>/bin`, the example scripts in
`<prefix>/share/exsh/examples`, and the shell docs (plus this README) in
`<prefix>/share/doc/exsh`. The fetched pscal-core dependency contributes its own
install rules as well, so a full install also lays down its static library,
headers, and docs.

## Test

The manifest-driven regression corpus lives in [`tests/`](tests/) and runs under
CTest against the binary you just built:

```sh
ctest --test-dir build --output-on-failure
```

You can also run the harness directly. It picks up the binary from `EXSH_BIN`
(falling back to `build/exsh`), which is how the umbrella build exercises the
same corpus:

```sh
EXSH_BIN=./build/exsh tests/run.sh            # all tests
EXSH_BIN=./build/exsh tests/run.sh --only printf   # a subset by id substring
tests/run.sh --list                            # list available cases
```

The harness treats exit code 77 as a skip. A few cases drive a sibling
component's example programs (CLike `hello` / `simple_web_server`) that a
standalone exsh clone does not ship; those self-skip so a minimal build stays
green, while a full checkout that has them runs them for real.

## Examples

Runnable scripts live in [`examples/exsh/`](examples/exsh/), covering builtins,
conditionals, pipelines, redirection, functions, and threading demos:

```sh
./build/exsh examples/exsh/pipeline
./build/exsh examples/exsh/functions
```

## Docs

Shell documentation is in [`docs/`](docs/):

- [`exsh_overview.md`](docs/exsh_overview.md): the shell overview and feature guide
- [`exsh_bash_builtin_coverage.txt`](docs/exsh_bash_builtin_coverage.txt): bash builtin coverage matrix
