# xlc — notes for Claude

## Language of work
- Repo content is English (code, comments, docs, commits, diagnostics). User chat is Turkish.

## Purpose
- `xlc` is the xlang compiler. It only translates what it is given and returns the result: it writes a file or streams to stdout, binary output included. It does no packaging, no running, no stdlib or package management.
- This repository is mounted as the `xlc/` submodule of https://github.com/hasirciogluhq/xlang (language, stdlib, tests, CLI). Language rules, stdlib and tests live there; docs live in https://github.com/hasirciogluhq/xlang-docs.

## Tests (hard rule)
- **Unit tests are platform independent; e2e tests are platform related.** A unit test checks logic that gives the same result on every OS and CPU (parsing, sema, layout, HIR, diagnostics), runs in milliseconds in one process and needs no linker, no child process and no files outside the test. An e2e test checks what depends on the OS or the CPU or on a real toolchain: the binary starts and links only the OS itself, a program is built, linked and run, a syscall, an object file per target.
- Unit tests are the bulk: `test/unit/` (`xmake run xlc-tests`). The e2e table in `test/e2e/run.sh` stays small and runs on the host in CI on Linux, macOS and Windows. A test that needs the host belongs in e2e, never in unit; a logic check never goes into e2e.
- Every compiler bug gets a unit test that fails without the fix. A bug that only shows on one OS (a missing library, a calling convention, an object format) also gets an e2e entry. Run the tests of what changed while working and each full suite once at the end.
- A release is built only from a tree whose unit and e2e tests pass on the OS being built.

## Git
- Never add `Co-Authored-By` trailers or any "Generated with Claude Code" line to commits or PR descriptions.
- Never force-push or rewrite published history without an explicit request.

## Editing
- File edits use the built-in Read/Edit/Write tools. Bash is for build, run, inspect, and moving or deleting whole files.
- Compiler code is C++23 minimum (C++26 where supported), modern library idioms, RAII, no raw owning pointers, no macros where `constexpr`/templates work.
