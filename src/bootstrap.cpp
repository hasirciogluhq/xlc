// The bootstrap shim: the released `xlc` that builds this repository links the bootstrap binary
// as a plain C++ executable, so nothing runs the xlang boot functions. This constructor does:
// it runs every `@init` function of the stdlib and the program before `main` (bootstrap.xl)
// starts. The shipped binary needs none of this: its `--crt` link registers the boot hooks as
// image constructors.
extern "C" void _xl_initAll();

__attribute__((constructor)) static void xlcBootstrapInit() {
    _xl_initAll();
}
