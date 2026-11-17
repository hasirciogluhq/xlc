# Where to report

xlang is several repositories. The [xlang](https://github.com/hasirciogluhq/xlang) repository
is the general tracker: the language, the `xl` tool, packages, install and every question. This
repository takes only problems that are clearly in the compiler.

| Problem | Repository |
| --- | --- |
| `xlc` crashes, panics or hangs | xlc |
| `xlc` builds a program that does the wrong thing (miscompile) | xlc |
| A wrong, missing or confusing diagnostic (`Exxxx`, `Wxxxx`) | xlc |
| An object file, executable or link step that is wrong for a target | xlc |
| An `xlc` command line flag | xlc |
| A new target, diagnostic or compiler flag | xlc |
| A new language feature or a change to the language | [xlang](https://github.com/hasirciogluhq/xlang) |
| `xl build`, `xl run`, packages, install scripts | [xlang](https://github.com/hasirciogluhq/xlang) |
| A stdlib function or type behaves wrongly | [xl-std](https://github.com/hasirciogluhq/xl-std) |
| The documentation site | [xlang-docs](https://github.com/hasirciogluhq/xlang-docs) |
| A security problem | private report, see [SECURITY.md](SECURITY.md) |

**Not sure?** Open it in [xlang](https://github.com/hasirciogluhq/xlang). It is moved to the
right repository there.

**Compiler or stdlib?** If the program misbehaves only with the stdlib (`--import std`), first
try to reproduce it without the stdlib. If it still fails, it is a compiler bug (xlc); if it
does not, report it to xl-std.

## A good compiler report

- `xlc --version`, the host OS and CPU, and the `--target` if one was given.
- The smallest `.xl` file that shows the problem, without the stdlib when possible.
- The exact command line.
- What happened (the full output, the exit code) and what you expected.
