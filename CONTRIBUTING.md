# Contributing to xlc

`xlc` is licensed under the [Apache License 2.0](LICENSE). Unless you state otherwise, any
contribution you submit is licensed under the same terms (Apache-2.0 section 5).

## Scope

`xlc` only translates source into an object, library, executable or C header. Running programs,
package management and the standard library belong to other repositories. Where an issue goes
is in [SUPPORT.md](SUPPORT.md); open an issue first for a new target, flag or diagnostic, and in
the [xlang](https://github.com/hasirciogluhq/xlang) repository for a language change.

## Workflow

1. Fork, branch from `main`, keep the change focused.
2. Build and test. The compiler is an `xl` package written in xlang:
   ```
   xl build src/main.xl -o build/xlc       # the compiler
   test/e2e/run.sh build/xlc               # programs built, linked and run on the host
   test/native/run.sh                      # the encoders against the system assembler
   ```
   `test/e2e/run.sh` also cross-builds Linux programs and runs them in Docker when `docker` and
   `ld.lld` are present, and links Windows executables when `lld-link` is present.
3. Every fixed bug gets a test that fails without the fix: a small program in `test/e2e/` with
   a row in `run.sh`, built by the compiler under test and run (its exit code names the failed
   check). `xl test` would use the installed compiler, not yours.
4. Language behavior is also exercised by the tests of
   [xl-std](https://github.com/hasirciogluhq/xl-std); run them against your build for a semantic
   change.
5. Update the documentation for user-visible changes: this repository (`README.md`,
   `src/codegen/codegen.md`) and [xlang-docs](https://github.com/hasirciogluhq/xlang-docs).
6. Install the git hooks once per clone: `scripts/install-hooks.sh`. The pre-commit hook checks
   the compiler comment tags (`COMPILER-TAGS.md`); `scripts/tags.sh check` runs it by hand.
7. Open a pull request against `main` and fill in the template.

## Code

- Compiler code is xlang, in the style of the stdlib: `for x in`, ternary, `switch`, `defer`,
  `String`, `Vec<T>`. Comments are English and explain each non-obvious layout, flag or ABI
  rule, with a link to the specification.
- A diagnostic has a code and says how to fix the problem.
- When a part is ported from the old C++ compiler, its bugs are fixed, not copied; the fix is
  noted in the comment.

## Security and conduct

Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md). Participation is
governed by the [Code of Conduct](CODE_OF_CONDUCT.md).
