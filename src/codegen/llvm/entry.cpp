#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

using gen_detail::definedHere;
using gen_detail::hasTerminator;
using gen_detail::isUnknownType;

// Program entry and hooks: `_xl_initAll`/`_xl_finiAll`, the shared-library load hooks, the
// `_start`/`mainCRTStartup` stubs and the driver of the whole pass (`Gen::run`).

// ---------------------------------------------------------------------------------------------
// Entry point and driver

template <std::ranges::input_range R> llvm::Function* Gen::emitHookAll(std::string name, R&& fns) {
    // The hook functions are named without `$` so that the raw AT&T start stubs can call them: a
    // `$` starting an operand is an immediate there.
    llvm::Function* all =
        llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), false),
                               llvm::GlobalValue::ExternalLinkage,
                               name,
                               mod_);
    all->addFnAttr(llvm::Attribute::NoUnwind);
    B_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", all));
    for (const std::string& fn : fns) {
        if (llvm::Function* f = functionNamed(fn)) {
            B_.CreateCall(f);
        }
    }
    B_.CreateRetVoid();
    return all;
}

/// @abi the entry symbol is `_start` on ELF and `mainCRTStartup` on PE; a user symbol with either
/// @abi name is E0726, so this lookup is the only way `@entry` becomes the program entry.
const hir::Function* Gen::entryDecl() const {
    const auto it = std::ranges::find_if(hm_.functions, &hir::Function::is_entry);
    return it == hm_.functions.end() ? nullptr : &*it;
}

void Gen::emitSharedHooks(llvm::Function* init_all, llvm::Function* fini_all) {
    // Hidden, so no other image's hook can interpose these. The loader runs the load hook when the
    // library is mapped (`@init`, then the `@entry` if the library has one) and the unload hook
    // when it is torn down (`@fini`). Windows DLLs have no hook yet: see xlang-docs/known-limitations.md
    // (DllMain).
    init_all->setVisibility(llvm::GlobalValue::HiddenVisibility);
    fini_all->setVisibility(llvm::GlobalValue::HiddenVisibility);
    if (in_.config.target.os == "windows") {
        return;
    }
    llvm::Function* load =
        llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), false),
                               llvm::GlobalValue::InternalLinkage,
                               "_xl_load",
                               mod_);
    B_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", load));
    B_.CreateCall(init_all);
    if (const hir::Function* entry = entryDecl()) {
        if (llvm::Function* f = functionNamed(entry->name)) {
            B_.CreateCall(f);
        }
    }
    B_.CreateRetVoid();
    llvm::appendToGlobalCtors(mod_, load, 65535, nullptr);
    if (in_.config.target.os == "macos") {
        // LLVM lowers `llvm.global_dtors` on Mach-O through `__cxa_atexit`, which the dylib link
        // does not provide. The pointer goes into `__mod_term_func` instead: dyld calls it when the
        // image is torn down. It is kept alive as compiler-used, so dead stripping cannot drop it.
        auto* term = new llvm::GlobalVariable(mod_,
                                              fini_all->getType(),
                                              true,
                                              llvm::GlobalValue::InternalLinkage,
                                              fini_all,
                                              "_xl_finiTerm");
        term->setSection("__DATA,__mod_term_func");
        llvm::appendToCompilerUsed(mod_, {term});
    } else {
        llvm::appendToGlobalDtors(mod_, fini_all, 65535, nullptr);
    }
}

void Gen::emitEntry() {
    const TargetInfo& t = in_.config.target;
    const hir::Function* entry = entryDecl();
    if (entry == nullptr) {
        return; // the driver reports an executable without `@entry` (E0739) before code generation
    }
    const bool mach_o = t.os == "macos";
    // macOS prefixes every C-visible name with `_`.
    const auto cName = [mach_o](const std::string& name) { return (mach_o ? "_" : "") + name; };
    const std::string sym = cName(entry->symbol.empty() ? entry->name : entry->symbol);
    const std::string init_sym = cName("_xl_initAll");
    const std::string fini_sym = cName("_xl_finiAll");
    const bool returns_int = entry->ret.kind == TypeKind::Int;
    // The stub runs `_xl_initAll`, then the entry, then `_xl_finiAll`, and exits with the entry's
    // result (0 for a void entry). The result waits in a callee-saved register (x86: r12d, aarch64:
    // w19) across the teardown call. Every call keeps the stack 16-byte aligned at the call site.
    const std::string x86_result = returns_int ? "\tmovl %eax, %r12d\n" : "\txorl %r12d, %r12d\n";
    const std::string arm_result = returns_int ? "\tmov w19, w0\n" : "\tmov w19, #0\n";
    // `@entry fn main(argc: i32, argv: **u8)`: the stub keeps argc and argv in callee-saved
    // registers across `_xl_initAll`. Linux puts them on the initial stack (argc at `(%rsp)`, argv
    // at `8(%rsp)`,
    // `[sp]` and `[sp, #8]` on aarch64). macOS dyld calls the entry like C `main`: the arguments
    // arrive in the C argument registers. An entry without parameters skips all of this.
    const bool takes_args = entry->param_count > 0;
    const std::string x86_save_linux =
        takes_args ? "\tmovl (%rsp), %r12d\n\tleaq 8(%rsp), %r13\n" : "";
    const std::string x86_save_macos = takes_args ? "\tmovl %edi, %r12d\n\tmovq %rsi, %r13\n" : "";
    const std::string x86_pass = takes_args ? "\tmovl %r12d, %edi\n\tmovq %r13, %rsi\n" : "";
    const std::string arm_save_linux = takes_args ? "\tldr x19, [sp]\n\tadd x20, sp, #8\n" : "";
    const std::string arm_save_macos = takes_args ? "\tmov x19, x0\n\tmov x20, x1\n" : "";
    const std::string arm_pass = takes_args ? "\tmov w0, w19\n\tmov x1, x20\n" : "";
    std::string text;
    if (t.os == "windows") {
        // The process ends through ntdll's `NtTerminateProcess` (the linker driver always imports
        // it for Windows executables).
        // An entry with `argc`/`argv` gets them from the `register(args)` function (the OS passes
        // only a command-line string): it runs after the boot functions, stores the count in the
        // spare slot above the shadow space and returns the vector. The driver rejects an entry
        // with parameters and no such function (E0755). rsi/rdi survive the calls (Win64).
        std::string win_args;
        if (takes_args) {
            const llvm::Function* args_fn = functionNamed(hm_.args_function);
            if (args_fn == nullptr) {
                return;
            }
            win_args = "\tleaq 32(%rsp), %rcx\n\tcall " + args_fn->getName().str() +
                       "\n\tmovq %rax, %rsi\n\tmovl 32(%rsp), %edi\n\tmovl %edi, %ecx\n"
                       "\tmovq %rsi, %rdx\n";
        }
        text = ".text\n.globl mainCRTStartup\nmainCRTStartup:\n\tsubq $40, %rsp\n\tcall " +
               init_sym + "\n" + win_args + "\tcall " + sym + "\n";
        text += x86_result + "\tcall " + fini_sym + "\n\tmovl %r12d, %edx\n";
        text += "\tmovq $-1, %rcx\n\tcall NtTerminateProcess\n\tint3\n";
        // Without a C runtime nothing else defines the stack probe LLVM calls for frames over 4 KiB
        // (it touches every page so the guard page extends the stack) nor the marker MSVC-style
        // object code references when floating point appears.
        text += ".globl __chkstk\n__chkstk:\n\tpushq %rcx\n\tpushq %rax\n\tcmpq $0x1000, %rax\n"
                "\tleaq 24(%rsp), %rcx\n\tjb 2f\n1:\n\tsubq $0x1000, %rcx\n\ttestq %rcx, (%rcx)\n"
                "\tsubq $0x1000, %rax\n\tcmpq $0x1000, %rax\n\tja 1b\n2:\n\tsubq %rax, %rcx\n"
                "\ttestq %rcx, (%rcx)\n\tpopq %rax\n\tpopq %rcx\n\tret\n";
        text += ".data\n.globl _fltused\n_fltused:\n\t.long 0\n";
    } else if (t.os != "linux" && t.os != "macos") {
        return;
    } else if (t.arch == "x86_64") {
        const bool linux_os = t.os == "linux";
        text = ".text\n.globl _start\n_start:\n\txorl %ebp, %ebp\n" +
               (linux_os ? x86_save_linux : x86_save_macos) + "\tandq $-16, %rsp\n\tcall " +
               init_sym + "\n" + x86_pass + "\tcall " + sym + "\n" + x86_result + "\tcall " +
               fini_sym + "\n\tmovl %r12d, %edi\n";
        // Linux: exit_group ends the worker threads too.
        text +=
            linux_os ? "\tmovl $231, %eax\n\tsyscall\n" : "\tmovl $0x2000001, %eax\n\tsyscall\n";
    } else {
        text = ".text\n.globl _start\n" + std::string(mach_o ? ".p2align 2\n" : "") + "_start:\n" +
               (mach_o ? arm_save_macos : arm_save_linux) + "\tbl " + init_sym + "\n" + arm_pass +
               "\tbl " + sym + "\n" + arm_result + "\tbl " + fini_sym + "\n\tmov w0, w19\n";
        text += mach_o ? "\tmov x16, #1\n\tsvc #0x80\n"
                       : "\tmov x8, #94\n\tsvc #0\n"; // exit_group on Linux
    }
    mod_.appendModuleInlineAsm(text);
    out_.has_entry = true;
}

bool Gen::run() {
    for (const hir::Global& g : hm_.globals) {
        declareGlobal(g);
    }
    for (const hir::Function& f : hm_.functions) {
        declareFunction(f);
    }
    buildTables();
    if (!hm_.unwind_function.empty()) {
        llvm::PointerType* ptr = llvm::PointerType::getUnqual(ctx_);
        pads_ptr_ = new llvm::GlobalVariable(mod_,
                                             ptr,
                                             true,
                                             llvm::GlobalValue::InternalLinkage,
                                             llvm::ConstantPointerNull::get(ptr),
                                             "_xl_unwindPads");
        never_ = new llvm::GlobalVariable(mod_,
                                          B_.getInt8Ty(),
                                          false,
                                          llvm::GlobalValue::InternalLinkage,
                                          B_.getInt8(0),
                                          "_xl_unwindNever");
        llvm::IntegerType* word = DL_.getIntPtrType(ctx_);
        pads_count_ = new llvm::GlobalVariable(mod_,
                                               word,
                                               true,
                                               llvm::GlobalValue::InternalLinkage,
                                               llvm::ConstantInt::get(word, 0),
                                               "_xl_unwindPadCount");
    }

    if (in_.config.debug) {
        di_ = std::make_unique<llvm::DIBuilder>(mod_);
        llvm::DIFile* file = di_->createFile(in_.module_name, ".");
        cu_ = di_->createCompileUnit(
            llvm::dwarf::DW_LANG_C, file, "xlang", in_.config.opt_level > 0, "", 0);
        mod_.addModuleFlag(
            llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        mod_.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
    }
    for (const hir::Global& g : hm_.globals) {
        if (definedHere(in_, g.module)) {
            defineGlobal(g);
        }
    }
    for (const hir::Function& f : hm_.functions) {
        if (definedHere(in_, f.module)) {
            defineFunction(f);
        }
    }
    // Whole-program outputs (the root module, or the only object of a plain build) own the boot
    // code.
    if (!in_.only_module || in_.only_module->empty()) {
        llvm::Function* init_all = emitHookAll("_xl_initAll", hm_.init_functions);
        // Teardown is the mirror of startup: the `@fini` functions run in reverse declaration
        // order.
        llvm::Function* fini_all =
            emitHookAll("_xl_finiAll", hm_.fini_functions | std::views::reverse);
        if (in_.config.crt_link && in_.output == Output::Executable) {
            // `--crt`: the platform CRT calls `main` and runs the static initializers, so the
            // boot hooks register as image constructors/destructors and no freestanding entry
            // stub is emitted. On Mach-O `llvm.global_dtors` lowers through `__cxa_atexit`,
            // which the C++ runtime link provides.
            llvm::appendToGlobalCtors(mod_, init_all, 65535, nullptr);
            llvm::appendToGlobalDtors(mod_, fini_all, 65535, nullptr);
        } else {
            switch (in_.output) {
            case Output::Executable:
                emitEntry();
                break;
            case Output::Shared:
                emitSharedHooks(init_all, fini_all);
                break;
            case Output::Static:
            case Output::Object:
                // Weak, so an executable that links the archive keeps its own strong hooks without a
                // duplicate-symbol error, and a host without them gets these definitions.
                init_all->setLinkage(llvm::GlobalValue::WeakAnyLinkage);
                fini_all->setLinkage(llvm::GlobalValue::WeakAnyLinkage);
                break;
            }
        }
    }
    if (pads_ptr_ != nullptr) {
        finishPadTable();
    }
    if (di_) {
        di_->finalize();
    }
    if (failed_) {
        return false;
    }
    std::string verify_error;
    llvm::raw_string_ostream os(verify_error);
    if (llvm::verifyModule(mod_, &os)) {
        os.flush();
        if (std::getenv("XLANG_DUMP_IR") != nullptr) {
            mod_.print(llvm::errs(), nullptr);
        }
        in_.diagnostics.error("E0714",
                              Span{},
                              "internal error: generated module failed verification: " +
                                  verify_error.substr(0, 600),
                              "code generation");
        return false;
    }
    return true;
}

} // namespace xlang::codegen
