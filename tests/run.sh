#!/usr/bin/env bash
set -euo pipefail

# Thin wrapper that runs the manifest-driven exsh regression harness against the
# standalone build. EXSH_BIN defaults to ./build/exsh (the standalone CMake
# output); callers and CTest override it, e.g. the umbrella points it at
# build/bin/exsh over this same corpus. Extra args are forwarded to the harness.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXSH_BIN="${EXSH_BIN:-$SCRIPT_DIR/../build/exsh}" \
    exec python3 "$SCRIPT_DIR/exsh_test_harness.py" "$@"
