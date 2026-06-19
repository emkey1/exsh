# exsh — the PSCAL shell front end (an interactive / scripting POSIX-ish shell).
#
# exsh parses shell source into the shared PSCAL AST, which the shared bytecode
# compiler lowers and the PSCAL VM runs. A few shared shell pieces (the backend
# shell runtime and the shared AST/builtin helpers) live in pscal-core; exsh
# compiles them from there. Pulls pscal-core in via FetchContent, so building
# this repo on its own fetches and builds the shared core automatically.
#
# Note: exsh's cross-front-end integration (driving Pascal/CLike/Rea in-process)
# and smallclue/SDL bits are mobile/iOS-gated and are not part of this standalone
# host build.
cmake_minimum_required(VERSION 3.18)
project(exsh C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall)
if(UNIX AND NOT APPLE)
    add_compile_definitions(_GNU_SOURCE _POSIX_C_SOURCE=200809L)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    add_compile_options(-Wno-nullability-completeness)
endif()

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
    pscal_core
    GIT_REPOSITORY https://github.com/emkey1/pscal-core.git
    GIT_TAG main
)
FetchContent_MakeAvailable(pscal_core)

include_directories(
    ${pscal_core_SOURCE_DIR}/src
    ${pscal_core_SOURCE_DIR}/src/third_party/yyjson
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_compile_definitions(
    PROGRAM_VERSION="0.0.0"
    PSCAL_GIT_TAG="untagged"
    ENABLE_EXT_BUILTIN_MATH ENABLE_EXT_BUILTIN_STRINGS ENABLE_EXT_BUILTIN_SYSTEM
    ENABLE_EXT_BUILTIN_USER ENABLE_EXT_BUILTIN_YYJSON ENABLE_EXT_BUILTIN_3D
    ENABLE_EXT_BUILTIN_GRAPHICS
)

set(EXSH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/lexer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/ast.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/semantics.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/codegen.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/builtins.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/runner.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/opt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/stubs.c
    # Shared shell pieces that live in pscal-core:
    ${pscal_core_SOURCE_DIR}/src/backend_ast/shell.c
    ${pscal_core_SOURCE_DIR}/src/common/ast_shared.c
    ${pscal_core_SOURCE_DIR}/src/common/builtin_shared.c
    ${pscal_core_SOURCE_DIR}/src/ext_builtins/shell_frontend.c
)

add_executable(exsh ${EXSH_SOURCES})
target_compile_definitions(exsh PRIVATE FRONTEND_SHELL)
target_link_libraries(exsh PRIVATE pscal_core_static Threads::Threads m)

# ---- Install ----------------------------------------------------------------
# `cmake --install build` puts `exsh` on PATH and ships the examples and the
# shell docs beside it. The rules below install only exsh's own artifacts; the
# fetched pscal-core also contributes its own install rules (static lib,
# headers, docs), so a full install additionally lays those down.
include(GNUInstallDirs)
install(TARGETS exsh RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/examples/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/exsh/examples)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs/
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---- Test -------------------------------------------------------------------
# `ctest` runs the manifest-driven regression corpus in tests/ against the
# binary just built. The harness treats exit code 77 as a skip, so cases that
# need a sibling component or a capability a standalone clone does not ship
# self-skip and a minimal build stays green.
enable_testing()
add_test(NAME exsh_tests COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh)
set_tests_properties(exsh_tests PROPERTIES
    ENVIRONMENT "EXSH_BIN=$<TARGET_FILE:exsh>")
