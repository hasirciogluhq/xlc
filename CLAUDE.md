# xlc -- notes for Claude

Rule 1. Every document, rule, skill and note must be in RFC style: plain English, ASCII only. The
basic Markdown is allowed: a "-" at the start of a line, and **bold** to underline a point. No
other decorative characters and no cute formatting; if it renders as style rather than meaning, it
is banned.

## Language of work

- Repo content is English: code, comments, docs, commits, diagnostics. User chat is Turkish.

## Purpose

- xlc is the xlang compiler. It only translates what it is given and returns the result: it writes
  a file or streams to stdout, binary output included. It does no packaging, no running, no stdlib
  or package management.
- This repository is mounted as the xlc/ submodule of https://github.com/hasirciogluhq/xlang
  (language, stdlib, tests, CLI). Language rules, stdlib and tests live there. Docs live in
  https://github.com/hasirciogluhq/xlang-docs.

## The compiler does not know the stdlib (hard rule)

- Work in this repository never touches, reads for testing, or depends on the stdlib (xl-std).
  The compiler knows no stdlib type, function or file; a change here is built and verified with
  the tests of this repository only (test/unit, test/e2e), never by compiling the stdlib or a
  program that imports it.
- A compiler change that breaks the stdlib is the stdlib's own work, done in its own repository
  and session, not here.

## Tests (hard rule)

- **Unit tests are platform independent; e2e tests are platform related.** A unit test checks
  logic that gives the same result on every OS and CPU (parsing, sema, layout, HIR, diagnostics),
  runs in milliseconds in one process, and needs no linker, no child process and no files outside
  the test. An e2e test checks what depends on the OS or the CPU or on a real toolchain: the
  binary starts and links only the OS itself, a program is built, linked and run, a syscall, an
  object file per target.
- Unit tests are the bulk, in test/unit (xmake run xlc-tests). The e2e table in test/e2e/run.sh
  stays small and runs on the host in CI on Linux, macOS and Windows. A test that needs the host
  belongs in e2e, never in unit. A logic check never goes into e2e.
- Every compiler bug gets a unit test that fails without the fix. A bug that only shows on one OS
  (a missing library, a calling convention, an object format) also gets an e2e entry. Run the
  tests of what changed while working, and each full suite once at the end.
- A release is built only from a tree whose unit and e2e tests pass on the OS being built.

## Git

- Never add Co-Authored-By trailers or any "Generated with Claude Code" line to commits or pull
  request descriptions.
- Never force-push or rewrite published history without an explicit request.

## Editing

- File edits use the built-in Read, Edit and Write tools. Bash is for build, run, inspect, and
  moving or deleting whole files.
- Compiler code is C++23 minimum, C++26 where supported, with modern library idioms, RAII, no raw
  owning pointers, and no macros where constexpr or templates work.

## Grammar comments (hard rule)

- Every form the parser accepts carries a @grammar comment, so the whole language grammar is found
  with grep -rn "@grammar" src/.
- The full tag vocabulary is defined in COMPILER-TAGS.md at the repository root: @grammar,
  @invariant, @ownership, @errno, @layout, @abi, @spec, @todo, @cost. A new tag is added to that
  file first, with its rule and its example, and then used in the same change.
- A tag goes only where it is true and useful. A tag that is only decoration is an error.
- A @grammar comment goes only where it belongs: on the parser function that accepts the form
  (parseFor, parseClosure and so on, in src/lang/parser/item, src/lang/parser/expr and
  src/lang/parser/stmt). Never on helpers, sema, HIR or codegen code. The parser is the one place
  that decides what is accepted.
- The line under the tag is the form itself, short, in the language's own syntax:

      /// @grammar `for i in lo..hi { body }`, `for x, i in s { body }`, `for init; cond; step { body }`
      Stmt Parser::parseFor() {

- One tag per form. When a form changes, its tag changes in the same edit. A new form without a
  tag is an incomplete change.
