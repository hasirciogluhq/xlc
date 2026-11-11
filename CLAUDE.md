# xlc — notes for Claude

## Language of work
- Repo content is English (code, comments, docs, commits, diagnostics). User chat is Turkish.

## Purpose
- `xlc` is the xlang compiler. It only translates what it is given and returns the result: it writes a file or streams to stdout, binary output included. It does no packaging, no running, no stdlib or package management.
- This repository is mounted as the `xlc/` submodule of https://github.com/hasirciogluhq/xlang (language, stdlib, tests, CLI). Language rules, stdlib and tests live there; docs live in https://github.com/hasirciogluhq/xlang-docs.

## Git
- Never add `Co-Authored-By` trailers or any "Generated with Claude Code" line to commits or PR descriptions.
- Never force-push or rewrite published history without an explicit request.

## Editing
- File edits use the built-in Read/Edit/Write tools. Bash is for build, run, inspect, and moving or deleting whole files.
- Compiler code is C++23 minimum (C++26 where supported), modern library idioms, RAII, no raw owning pointers, no macros where `constexpr`/templates work.
