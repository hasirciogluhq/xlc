# Contributing to xlc

`xlc` is licensed under the [Apache License 2.0](LICENSE). Unless you state otherwise, any contribution you submit is licensed under the same terms (Apache-2.0 §5).

## Scope

`xlc` only translates source into an object, library, executable, IR or C header. Running programs, package management and the standard library belong to other repositories (see the [xlang](https://github.com/hasirciogluhq/xlang) overview).

## Workflow

1. Open an issue first for language or CLI changes.
2. Fork, branch from `main`, keep the change focused.
3. Build and test:
   ```
   xmake f -P . -m release && xmake -P .
   xmake build -P . xlc-tests && xmake run -P . xlc-tests
   test/e2e/run.sh build/<platform>/<arch>/release/xlc
   ```
   Unit tests (`test/unit`) are platform independent: parser, sema, layout, HIR, diagnostics. E2E tests (`test/e2e`) are platform related: the binary starts and links only the OS, programs are built, linked and run, one object per target. A compiler bug gets a unit test that fails without the fix; a bug that shows on one OS only also gets an e2e entry.
4. Language behavior is exercised by the end-to-end tests in [xl-std](https://github.com/hasirciogluhq/xl-std) (`test/`); run them against your build for any semantic change.
5. Update the documentation in [xlang-docs](https://github.com/hasirciogluhq/xlang-docs) for user-visible changes.
6. Open a pull request against `main` and fill in the template.

## Code

C++23 minimum, C++26 where the compiler supports it; modern library idioms, RAII, no raw owning pointers, no macros where `constexpr` or templates work. Diagnostics carry a code and say how to fix the problem.

## Security and conduct

Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md). Participation is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).
