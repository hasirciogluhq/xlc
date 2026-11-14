#!/bin/sh
# Generator: collects the compiler comment tags into one view, and checks that every tag is
# well-formed. This is the tool behind COMPILER-TAGS.md; it needs nothing but grep and sort.
#
#   scripts/tags.sh list [tag]    print every tag line, grouped by tag then by path and line
#   scripts/tags.sh check         validate the tags and exit non-zero when one is malformed
#   scripts/tags.sh sync          rewrite the collected view inside COMPILER-TAGS.md, when it differs
#
# `sync` owns exactly one block of COMPILER-TAGS.md, between the BEGIN and END marker lines.
# Everything outside the markers is written by hand and is never touched. The block is always
# derived from the source, so it is reproduced byte for byte on a clean tree.
#
# `check` enforces the rules COMPILER-TAGS.md states:
#   - a tag starts the comment and is lower case ASCII, so `grep -rn "@tag" src/` finds all of it,
#   - a line that continues a tag repeats the tag instead of continuing with a bare `///`,
#   - only known tags are used,
#   - a tag is not left without a following declaration.
#
# Exit codes: 0 clean, 1 rule violation, 2 bad usage.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# The tag vocabulary, exactly the set COMPILER-TAGS.md defines.
known="grammar comment invariant ownership errno layout abi spec todo cost"

usage() {
    echo "usage: scripts/tags.sh list [tag] | check | sync" >&2
    exit 2
}

# Every occurrence of one tag, as `path:line:text`.
#
# The order is forced here, not left to grep: `grep -r` walks directories in readdir order, which
# is the filesystem's business (inode order on Linux, hash order on APFS) and differs between the
# developer's machine and CI. The collected view is a committed file, so it must be derived from
# the source alone. LC_ALL=C keeps the collation the same on every host.
# -t: -k1,1 -k2,2n sorts by path, then by line number numerically (99 before 100).
hits() {
    (cd "$root" && grep -rn "@$1 " --include='*.cpp' --include='*.h' --include='*.hpp' \
        src include 2>/dev/null || true) | LC_ALL=C sort -t: -k1,1 -k2,2n
}

# Every scanned source: the compiler itself. Docs and tests never carry tags.
sources() {
    cd "$root" && grep -rl --include='*.cpp' --include='*.h' --include='*.hpp' '@' src \
        include 2>/dev/null || true
}

cmd_list() {
    want=${1:-}
    for tag in $known; do
        if [ -n "$want" ] && [ "$want" != "$tag" ]; then
            continue
        fi
        found=$(hits "$tag")
        if [ -n "$found" ]; then
            echo "== @$tag"
            # printf, never echo: dash's echo interprets backslash escapes, so a tag text that
            # contains a literal `\n\t\r` (the lexer escape list) would print real control
            # bytes and git would see the generated file as binary.
            printf '%s\n' "$found"
            echo
        fi
    done
}

cmd_check() {
    status=0

    # 1. An unknown tag: a comment that starts with `@` and a word that is not in the vocabulary.
    unknown=$(cd "$root" && grep -rnE '///[[:space:]]+@[a-z]+' --include='*.cpp' --include='*.h' \
        --include='*.hpp' src include 2>/dev/null |
        sed -E 's/.*\/\/\/[[:space:]]+@([a-z]+).*/\1/' | sort -u)
    for word in $unknown; do
        case " $known " in
        *" $word "*) ;;
        *)
            echo "COMPILER-TAGS.md: unknown tag @$word"
            (cd "$root" && grep -rn "/// *@$word" --include='*.cpp' --include='*.h' \
                --include='*.hpp' src include 2>/dev/null | sed 's/^/  /') || true
            status=1
            ;;
        esac
    done

    # 2. A continuation line: a `///` doc comment directly after a tag line that repeats no tag.
    #    That is the line the grep would lose.
    orphans=$(cd "$root" && awk '
        FNR == 1 { prev_tag = 0 }
        {
            is_doc = ($0 ~ /^[[:space:]]*\/\/\//)
            has_tag = ($0 ~ /^[[:space:]]*\/\/\/[[:space:]]+@[a-z]+/)
            if (is_doc && prev_tag && !has_tag) {
                print FILENAME ":" FNR ":" $0
            }
            prev_tag = has_tag ? 1 : 0
            if (!is_doc) prev_tag = 0
        }
    ' $(sources))
    if [ -n "$orphans" ]; then
        echo "COMPILER-TAGS.md: a tag continuation line must repeat its tag:"
        printf '%s\n' "$orphans" | sed 's/^/  /'
        status=1
    fi

    # 3. A tag with nothing under it: the next line must not be blank or another non-doc line.
    dangling=$(cd "$root" && awk '
        FNR == 1 { pending = 0 }
        {
            if (pending) {
                is_doc = ($0 ~ /^[[:space:]]*\/\/\//)
                if ($0 ~ /^[[:space:]]*$/) {
                    print FILENAME ":" FNR - 1 ": tag is followed by a blank line"
                    pending = 0
                } else if (!is_doc) {
                    pending = 0
                }
            }
            if ($0 ~ /^[[:space:]]*\/\/\/[[:space:]]+@[a-z]+/) pending = 1
        }
    ' $(sources))
    if [ -n "$dangling" ]; then
        echo "COMPILER-TAGS.md: a tag must sit right above its declaration:"
        printf '%s\n' "$dangling" | sed 's/^/  /'
        status=1
    fi

    if [ "$status" -eq 0 ]; then
        grammar=$(cd "$root" && grep -rc "@grammar" --include='*.cpp' src 2>/dev/null |
            awk -F: '{s += $2} END {print s + 0}')
        echo "tags ok: $(echo $known | wc -w | tr -d ' ') tags, $grammar @grammar lines, grep finds all of them"
    fi
    return "$status"
}

# The collected view is rewritten into the marked block of COMPILER-TAGS.md. It is built from the
# source alone and carries no timestamp, so a clean tree reproduces it byte for byte.
cmd_sync() {
    doc=$root/COMPILER-TAGS.md
    if [ ! -f "$doc" ]; then
        echo "COMPILER-TAGS.md: $doc is missing" >&2
        return 1
    fi
    begin='<!-- BEGIN COLLECTED TAGS -->'
    end='<!-- END COLLECTED TAGS -->'
    if ! grep -qF "$begin" "$doc" || ! grep -qF "$end" "$doc"; then
        echo "COMPILER-TAGS.md: the '$begin' / '$end' markers are missing" >&2
        return 1
    fi

    tmp=$(mktemp)
    block=$(mktemp)
    trap 'rm -f "$tmp" "$block"' EXIT

    {
        echo "$begin"
        echo "<!-- written by scripts/tags.sh sync; every line below is a tag found in src/ and"
        echo "     include/, grouped by tag and then by file. Do not edit inside the markers. -->"
        echo
        total=0
        for tag in $known; do
            found=$(hits "$tag")
            [ -n "$found" ] || continue
            count=$(printf '%s\n' "$found" | wc -l | tr -d ' ')
            total=$((total + count))
            echo "### @$tag ($count)"
            echo
            echo '```'
            # printf, never echo: see the note in cmd_list.
            printf '%s\n' "$found"
            echo '```'
            echo
        done
        echo "Total: $total tag lines."
        echo "$end"
    } >"$block"

    # Replace only what sits between the markers; the rest of the document is hand written.
    awk -v begin="$begin" -v end="$end" -v block="$block" '
        $0 == begin { while ((getline line < block) > 0) print line; skip = 1; next }
        $0 == end   { skip = 0; next }
        !skip       { print }
    ' "$doc" >"$tmp"

    if cmp -s "$tmp" "$doc"; then
        echo "tags sync: COMPILER-TAGS.md is up to date"
        return 0
    fi
    cat "$tmp" >"$doc"
    echo "tags sync: COMPILER-TAGS.md updated"
    return 0
}

case "${1:-}" in
list) shift; cmd_list "${1:-}" ;;
check) cmd_check ;;
sync) cmd_sync ;;
*) usage ;;
esac
