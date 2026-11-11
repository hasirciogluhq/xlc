#!/usr/bin/env bash
# Packages a built xlc into a release archive in ./dist.
#
# Usage: scripts/package.sh <version> <os> <arch> <binary>
#   version  release version without the leading "v" (1.0.0 or 1.0.0-rc.1)
#   os       darwin | linux | windows
#   arch     amd64 | arm64
#   binary   path to the built compiler (xlc, or xlc.exe on windows)
#
# Output:
#   dist/xlc_<version>_<os>_<arch>.tar.gz   (zip on windows)
#   dist/xlc_<version>_<os>_<arch>.<ext>.sha256
set -euo pipefail

if [ "$#" -ne 4 ]; then
  echo "usage: $0 <version> <os> <arch> <binary>" >&2
  exit 2
fi
version="$1" os="$2" arch="$3" binary="$4"

case "$os" in darwin | linux | windows) ;; *) echo "error: unsupported os '$os'" >&2; exit 2 ;; esac
case "$arch" in amd64 | arm64) ;; *) echo "error: unsupported arch '$arch'" >&2; exit 2 ;; esac
[ -f "$binary" ] || { echo "error: compiler binary not found: $binary" >&2; exit 1; }

# Fail early if the binary cannot even start, and if it reports another version than the tag.
reported="$("$binary" --version)"
if [ "$reported" != "$version" ]; then
  echo "error: binary reports '$reported' but the release is '$version'" >&2
  exit 1
fi

name="xlc_${version}_${os}_${arch}"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p dist

exe="xlc"
[ "$os" = "windows" ] && exe="xlc.exe"
cp "$binary" "$stage/$exe"
chmod 0755 "$stage/$exe"
# Smaller archive; not fatal when strip is missing or refuses the file.
case "$os" in
  darwin) strip -x "$stage/$exe" || true ;;
  *) strip --strip-unneeded "$stage/$exe" || true ;;
esac

if [ "$os" = "windows" ]; then
  archive="dist/$name.zip"
  (cd "$stage" && 7z a -tzip "$OLDPWD/$archive" "$exe" >/dev/null 2>&1 || zip -q "$OLDPWD/$archive" "$exe")
else
  archive="dist/$name.tar.gz"
  tar -C "$stage" -czf "$archive" "$exe"
fi

if command -v sha256sum >/dev/null 2>&1; then
  hash="$(sha256sum "$archive" | cut -d' ' -f1)"
else
  hash="$(shasum -a 256 "$archive" | cut -d' ' -f1)"
fi
printf '%s  %s\n' "$hash" "$(basename "$archive")" >"$archive.sha256"
echo "packaged $archive ($hash)"
