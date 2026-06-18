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
