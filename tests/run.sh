#!/usr/bin/env bash
set -euo pipefail

# Thin wrapper that runs the manifest-driven exsh regression harness against the
# standalone build. EXSH_BIN defaults to ./build/exsh (the standalone CMake
# output); callers and CTest override it, e.g. the umbrella points it at
# build/bin/exsh over this same corpus. Extra args are forwarded to the harness.
#
# A whole run also covers how the line editor wraps, which only shows on a terminal.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export EXSH_BIN="${EXSH_BIN:-$SCRIPT_DIR/../build/exsh}"
status=0
python3 "$SCRIPT_DIR/exsh_test_harness.py" "$@" || status=$?
if [ "$#" -eq 0 ]; then
    python3 "$SCRIPT_DIR/exsh_interactive_test_harness.py" --executable "$EXSH_BIN" --only line_wrap || status=$?
fi
exit "$status"
