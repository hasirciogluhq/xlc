#!/bin/sh
# Installer: turns on the versioned git hooks of this repository.
#
#   scripts/install-hooks.sh
#
# Points core.hooksPath at .githooks, so the hooks are the ones in the tree and a change to them
# is reviewed like any other change. The previous value, if any, is printed, so it can be put
# back. Nothing outside this repository is touched.
#
# Undo: git config --unset core.hooksPath

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

if [ ! -d .githooks ]; then
    echo "install-hooks: .githooks is missing" >&2
    exit 1
fi

chmod +x .githooks/* scripts/tags.sh

previous=$(git config --get core.hooksPath || true)
git config core.hooksPath .githooks

echo "install-hooks: core.hooksPath = .githooks"
if [ -n "$previous" ]; then
    echo "install-hooks: it was '$previous'; restore it with: git config core.hooksPath '$previous'"
fi
for hook in .githooks/*; do
    echo "install-hooks: active $(basename "$hook")"
done
