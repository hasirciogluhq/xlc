#!/bin/sh
# Native arm64 backend tests (macOS on Apple silicon only): the encoder words against the system
# assembler, then the instruction selector on hand-built HIR modules linked with `cc` and run.
# Needs `xl` on PATH. Usage: test/native/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
[ "$(uname -s)-$(uname -m)" = Darwin-arm64 ] || { echo "skip: needs macOS arm64"; exit 0; }

clang -c -target arm64-apple-macos "$here/encode.s" -o "$work/encode.o"
otool -tj "$work/encode.o" | tail -n +3 | awk '{for(i=2;i<=NF;i++) print $i}' >"$work/ref.txt"
xl run "$here/encode.xl" >"$work/mine.txt"
diff "$work/ref.txt" "$work/mine.txt" && echo "ok   [native/encode]"

if [ -f "$here/isel.xl" ]; then
  xl run "$here/isel.xl" -- "$work"
  for o in "$work"/*.o; do
    [ -e "$o" ] || continue
    case $o in */encode.o) continue ;; esac
    cc "$o" -o "${o%.o}"
    set +e; "${o%.o}"; got=$?; set -e
    [ "$got" = 0 ] && echo "ok   [native/$(basename "${o%.o}")]" || { echo "FAIL [native/$(basename "${o%.o}")] exit $got"; exit 1; }
  done
fi
