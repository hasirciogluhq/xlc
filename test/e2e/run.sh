#!/bin/bash
# End-to-end tests of the compiler binary itself, on the host it runs on: everything that depends on the
# operating system or the CPU. (The C++ suite in test/unit holds the platform-independent checks.)
#   test/e2e/run.sh [path/to/xlc]        (default: $XLC, else `xlc` on PATH)
#
# 1. The binary starts and reports the version in xmake.lua, and links only against the OS itself: a
#    release is copied to machines that have none of the build machine's packages (a Homebrew `libz3.dylib`
#    that is missing there makes `xlc --version` abort with a dyld error).
# 2. It builds, links and runs native programs: each *.xl here is compiled with no stdlib and started.
# 3. It writes an object file for every target it supports, whatever the host is.
set -u
here=$(cd "$(dirname "$0")" && pwd)
xlc=${1:-${XLC:-xlc}}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pass=0; fail=0

case "$(uname -s)" in
  Darwin) os=macos ;;
  Linux) os=linux ;;
  MINGW* | MSYS* | CYGWIN*) os=windows ;;
  *) echo "FAIL [e2e] unsupported OS $(uname -s)"; exit 1 ;;
esac
exe=; [ "$os" = windows ] && exe=.exe

ok()  { echo "ok   [e2e/$1]"; pass=$((pass + 1)); }
bad() { echo "FAIL [e2e/$1] $2"; fail=$((fail + 1)); }

# --- 1. the binary --------------------------------------------------------------------------------------
declared=$(sed -n 's/^local xlc_version = "\(.*\)"$/\1/p' "$here/../../xmake.lua")
reported=$("$xlc" --version 2>&1)
if [ "$reported" = "$declared" ]; then ok "version"; else bad "version" "reports '$reported', xmake.lua declares '$declared'"; fi

# Dynamic libraries the binary needs, one per line; empty when the platform tool is missing.
neededLibraries() {
  local bin
  bin=$(command -v "$xlc" || echo "$xlc")
  case "$os" in
    macos) otool -L "$bin" | tail -n +2 | awk '{ print $1 }' ;;
    linux) readelf -d "$bin" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' ;;
    windows) command -v dumpbin >/dev/null && dumpbin //dependents "$bin" | awk '/\.dll$/ { print $1 }' ;;
  esac
}
# What the OS itself provides. Anything else is a package of the build machine.
systemLibrary() {
  case "$os:$1" in
    macos:/usr/lib/* | macos:/System/*) return 0 ;;
    linux:libc.so.* | linux:libm.so.* | linux:libdl.so.* | linux:libpthread.so.* | linux:librt.so.* | linux:libutil.so.*) return 0 ;;
    linux:ld-linux-* | linux:libz.so.*) return 0 ;;   # zlib is part of every distribution's base system
    windows:[Kk][Ee][Rr][Nn][Ee][Ll]32.[Dd][Ll][Ll] | windows:[Aa][Dd][Vv][Aa][Pp][Ii]32.[Dd][Ll][Ll]) return 0 ;;
    windows:[Ss][Hh][Ee][Ll][Ll]32.[Dd][Ll][Ll] | windows:[Oo][Ll][Ee]32.[Dd][Ll][Ll] | windows:[Ww][Ss]2_32.[Dd][Ll][Ll]) return 0 ;;
    windows:[Vv][Ee][Rr][Ss][Ii][Oo][Nn].[Dd][Ll][Ll] | windows:[Nn][Tt][Dd][Ll][Ll].[Dd][Ll][Ll] | windows:[Uu][Cc][Rr][Tt][Bb][Aa][Ss][Ee].[Dd][Ll][Ll]) return 0 ;;
    windows:[Aa][Pp][Ii]-[Mm][Ss]-[Ww][Ii][Nn]-*) return 0 ;;
    windows:[Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee]140*.[Dd][Ll][Ll] | windows:[Mm][Ss][Vv][Cc][Pp]140*.[Dd][Ll][Ll]) return 0 ;;
  esac
  return 1
}
foreign=
while read -r lib; do
  [ -n "$lib" ] && ! systemLibrary "$lib" && foreign="$foreign $lib"
done < <(neededLibraries)
if [ -z "$foreign" ]; then ok "links-only-system-libraries"; else bad "links-only-system-libraries" "needs$foreign"; fi

# --- 2. native programs ---------------------------------------------------------------------------------
# name | expected exit code | expected stdout (a `grep -F` substring) | runs on Windows | compiler flags
programs=(
  "exit|42||1|"
  "write|0|xlc e2e write|0|"
  "syscall_registers|0|xlc e2e registers|0|-O2"
)
for row in "${programs[@]}"; do
  IFS='|' read -r name code want on_windows flags <<<"$row"
  [ "$os" = windows ] && [ "$on_windows" = 0 ] && { echo "skip [e2e/$name] not on Windows"; continue; }
  # shellcheck disable=SC2086 # the flags column is a list of words
  if ! "$xlc" "$here/$name.xl" $flags -o "$work/$name$exe" 2>"$work/$name.err"; then
    bad "$name" "build"; cat "$work/$name.err"; continue
  fi
  out=$("$work/$name$exe" 2>&1); got=$?
  if [ "$got" != "$code" ]; then bad "$name" "exit $got, want $code"; echo "$out"
  elif [ -n "$want" ] && ! grep -qF "$want" <<<"$out"; then bad "$name" "stdout lacks '$want'"; echo "$out"
  else ok "$name"; fi
done

# --- 2b. the C convention -------------------------------------------------------------------------------
# c_abi.xl and its C peer c_abi.c pass structs by value both ways; the peer is built by the host C
# compiler (clang-cl on Windows, `$CC` or `cc` elsewhere) without libc and linked into the program.
if [ "$os" = windows ]; then cc_cmd=clang-cl; else cc_cmd=${CC:-cc}; fi
if ! command -v "$cc_cmd" >/dev/null 2>&1; then
  echo "skip [e2e/c_abi] no C compiler ($cc_cmd)"
else
  if [ "$os" = windows ]; then
    peer=c_abi_peer.obj
    (cd "$work" && clang-cl -nologo -O2 -GS- -Zl -c -Fo"$peer" -- "$here/c_abi.c") >"$work/c_abi.err" 2>&1
  else
    peer=c_abi_peer.o
    "$cc_cmd" -O2 -fPIC -fno-stack-protector -c "$here/c_abi.c" -o "$work/$peer" >"$work/c_abi.err" 2>&1
  fi
  if [ $? != 0 ]; then bad "c_abi" "C peer"; cat "$work/c_abi.err"
  elif ! "$xlc" "$here/c_abi.xl" "$work/$peer" --allow W0301 -o "$work/c_abi$exe" 2>"$work/c_abi.err"; then
    bad "c_abi" "build"; cat "$work/c_abi.err"
  else
    "$work/c_abi$exe"; got=$?
    if [ "$got" = 0 ]; then ok "c_abi"; else bad "c_abi" "call $got came back wrong (see c_abi.xl, c_abi.c)"; fi
  fi
fi

# --- 3. every target ------------------------------------------------------------------------------------
for target in x86_64-linux-gnu aarch64-linux-gnu x86_64-apple-darwin aarch64-apple-darwin x86_64-pc-windows-msvc; do
  if "$xlc" "$here/object.xl" --emit=obj --target "$target" -o "$work/t-$target.o" 2>"$work/t.err" && [ -s "$work/t-$target.o" ]; then
    ok "object-$target"
  else
    bad "object-$target" "no object file"; cat "$work/t.err"
  fi
done

echo "e2e: $pass passed, $fail failed"
[ "$fail" = 0 ]
