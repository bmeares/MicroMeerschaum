#!/usr/bin/env bash
# Run against the Unix-port interpreter containing the native _mm module.
set -euo pipefail
mp=$(realpath "${1:?usage: run.sh /path/to/micropython [--sqlite]}")
tests=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$tests/../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
export MICROPYPATH="$repo/ports/micropython:$tests"
"$mp" "$tests/pipe.py" "$work"
if [[ "${2:-}" == --sqlite ]]; then
    "$mp" "$tests/sqlite_pipe.py" "$work"
fi

cmake -S "$repo/core" -B "$work/build" -DMM_SANITIZE=OFF > /dev/null
cmake --build "$work/build" --target micromeerschaum_posix -j 4 > /dev/null
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$repo/core/include" "$tests/handoff.c" \
    "$work/build/libmicromeerschaum_posix.a" "$work/build/libmicromeerschaum.a" -o "$work/native"
mkdir "$work/python-first" "$work/native-first"
(cd "$work/python-first" && "$mp" "$tests/handoff.py" start && "$work/native" finish)
(cd "$work/native-first" && "$work/native" start && "$mp" "$tests/handoff.py" finish)
echo "C and MicroPython recovered each other's original records and delivery cursor."
