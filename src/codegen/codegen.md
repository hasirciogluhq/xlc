# Codegen notes

The memory file of the native code generator: the layout of src/codegen, the target facts it
relies on, each with its source, and what is done and what is open. Keep it current with every
codegen change.

## Layout

- **target/** The target description. Parses a triple into arch, vendor, os, env, the object
  format, the pointer width and the endianness; `hostTarget()`. session/config.xl and cli.xl
  read it. Knows nothing about code generation.
- **obj/** The format-neutral object: sections by kind (SEC_TEXT, SEC_CSTRING, SEC_CONST,
  SEC_DATA), symbols spelled as in the source with a binding (global, local, weak), and
  relocations by meaning (R_ABS64, R_BRANCH26, R_PAGE21, R_PAGEOFF12, R_GOT_PAGE21,
  R_GOT_PAGEOFF12). Selectors fill it, writers serialize it.
- **macho/** The Mach-O writer (macOS). Owns the `_` symbol prefix, section and segment names,
  ARM64_RELOC_* numbers and the nlist order.
- **elf/** The ELF writer, `writeElf(o, machine)`: ELF64 with RELA for EM_AARCH64, ELF32 with
  REL for EM_ARM (addends stay in the section bytes, `$a`/`$d` mapping symbols per section).
  Owns the R_AARCH64_* and R_ARM_* numbers and the locals-first symbol order.
- **coff/** The COFF writer, `writeCoff(o)`: IMAGE_FILE_MACHINE_ARM64 objects for Windows. Owns
  the IMAGE_REL_ARM64_* numbers, section symbols, weak externals and the `__imp_` rewrite of
  GOT-style accesses (see "Windows arm64").
- **common/** What every selector shares: layout.xl (`Layouts(m, ptrBytes)`: sizes, alignments,
  field offsets at the target's pointer width), types.xl (`scalarOfW(m, ty, ptrBits)`),
  syms.xl (the `Syms` table and `flushSyms` into the object).
- **arm64/encode/** The A64 instruction encoders (index.xl integer and memory, float.xl, atomic.xl).
- **arm64/isel/** The A64 instruction selector over the flat HIR (frames, expressions,
  statements, calls, conventions, data, boot). The OS shows only in `syscall` (call.xl) and the
  boot code (data.xl), both switched on `g.target.os`.
- **arm32/encode/** The A32 (ARM state) encoders: index.xl integer, memory, branches, ldrex/strex;
  vfp.xl the VFPv3 arithmetic, compare, conversions and core<->VFP moves.
- **arm32/isel/** The A32 selector (armv7-a, VFPv3-D16, AAPCS32 hard-float, Linux only): gen.xl
  state and value model, conv.xl the convention, expr.xl, call.xl, stmt.xl, func.xl, data.xl
  (data, boot, runtime routines), index.xl `select`.
- **native/** The driver: target -> selector -> writer -> file. Rejects targets without a backend
  with E0714.
- **header.xl** The C header writer (`--emit=header`).
- Slots for later: **x86_64/** (encode, isel).
- The link step is src/driver/link.xl (per OS, see "Linking").

## Targets

- Triple form `arch-vendor-os[-env]`; the vendor may be left out (`aarch64-linux-gnu`), so the
  OS is found by name. https://clang.llvm.org/docs/CrossCompilation.html#target-triple,
  https://llvm.org/doxygen/classllvm_1_1Triple.html
- Arm triples in use:
  - macOS: `arm64-apple-macosx<ver>` (clang's spelling), `aarch64-apple-darwin` (Rust and GNU
    spelling). arm64 only; macOS never had a 32-bit Arm user space.
  - iOS: `arm64-apple-ios`. Not supported (no runtime, code signing required).
  - Linux 64-bit: `aarch64-unknown-linux-gnu`, `aarch64-unknown-linux-musl`, `aarch64-linux-gnu`.
  - Linux 32-bit: `armv7-unknown-linux-gnueabihf`, `arm-linux-gnueabihf` (hard float, VFP),
    `armv7-unknown-linux-musleabihf`, `arm-unknown-linux-gnueabi` (soft float).
  - Android: `aarch64-linux-android`, `armv7-linux-androideabi`. Not supported.
  - Windows on Arm: `aarch64-pc-windows-msvc`, short `aarch64-windows` (the OS is found by
    name). COFF, see "Windows arm64".
  - FreeBSD: `aarch64-unknown-freebsd`. Not supported.
  - Bare metal: `aarch64-unknown-none`, `armv7a-none-eabi`. Rejected (no OS, no entry contract).
  - Rust target list for reference: https://doc.rust-lang.org/nightly/rustc/platform-support.html
- **Supported now:** aarch64 on macos (Mach-O), linux (ELF) and windows (COFF), arm (32-bit) on linux (ELF32),
  and the generic OS for both (`aarch64-unknown`, `armv7-unknown`).
- **The generic OS rule.** A triple with no known OS word (`aarch64`, `aarch64-unknown`,
  `aarch64-unknown-unknown`) is the generic OS and behaves exactly as Linux: ELF, Linux syscalls,
  own `_start`, static link without a C library. The sources see `os == "linux"`;
  `Target.generic` records that it was not named. Rationale: the only OS xlc can target without a
  C library and with a stable syscall ABI is Linux, so "unknown" picks the one that works.
- `arm` (armv7*, thumbv7*, armhf) parses, pointer width 32, linux only. Code is always A32
  (ARM state) for armv7-a with VFPv3-D16 and the hard-float ABI, whatever the triple's
  sub-architecture word says.

## arm64 calling convention and registers

- AAPCS64: x0-x7 arguments and results, x8 indirect result, x9-x15 temporaries, x16/x17 IP0/IP1
  (linker veneers may clobber them), x18 platform register, x19-x28 callee-saved, x29 frame
  pointer, x30 link register, sp 16-byte aligned at every public interface; d8-d15 low 64 bits
  callee-saved. https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst
- Apple: x18 reserved, never touched; frame pointer always kept with a frame record; stack
  arguments packed at natural size for the C convention; red zone 128 bytes; `char` signed;
  variadic arguments always on the stack.
  https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms
- Linux: x18 is a temporary; no red zone. The selector never uses x18 and always keeps x29, so
  one register plan serves both OSes.
- The xlang convention (internal) is the same on every arm64 OS: ints/pointers in x0-x7, floats
  in d0-d7, aggregates by the address of a caller copy, aggregate result through x8, stack
  arguments one 8-byte slot each.
- **Gap:** the C convention (conv.xl, `export(c)`, `declare`) implements the Apple variant
  (packed stack arguments). Linux AAPCS64 gives every stack argument an 8-byte slot. Out of
  scope for now; a C call with more than 8 integer arguments of under 8 bytes on Linux is wrong.

## System calls

- Linux arm64: number in x8 (w8), arguments x0-x5, `svc #0`, result in x0, `-errno` on failure
  (-4095..-1). Numbers from the generic table include/uapi/asm-generic/unistd.h (write 64,
  exit 93, exit_group 94). https://man7.org/linux/man-pages/man2/syscall.2.html,
  https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h
- Linux arm32 EABI: number in r7, arguments r0-r6, `swi 0x0` (`svc #0`), result r0, `-errno`.
  Numbers from arch/arm/tools/syscall.tbl (write 4, exit_group 248). Same man page.
- macOS arm64: number in x16 (class 2 Unix calls as `0x2000000 | n`, e.g. write 0x2000004),
  arguments x0.., `svc #0x80`; carry set means error with a positive errno in x0; the selector
  negates it so `syscall` returns `-errno` on every OS. Apple does not promise this interface
  (libSystem is the stable ABI). https://github.com/apple-oss-distributions/xnu
  (bsd/kern/syscalls.master, osfmk/arm64/...)

## Objects

- Mach-O: header `MH_MAGIC_64`, `CPU_TYPE_ARM64`, `MH_OBJECT`; one unnamed LC_SEGMENT_64 with
  every section, LC_BUILD_VERSION (macOS 11.0), LC_SYMTAB. Symbols carry one `_` prefix.
  ARM64_RELOC_UNSIGNED 0, BRANCH26 2, PAGE21 3, PAGEOFF12 4, GOT_LOAD_PAGE21 5,
  GOT_LOAD_PAGEOFF12 6; PAGEOFF12 takes the scale from the instruction; addends are in place.
  https://github.com/apple-oss-distributions/cctools/blob/main/include/mach-o/arm64/reloc.h
- ELF64 (aarch64): ELFCLASS64, ELFDATA2LSB, ET_REL, EM_AARCH64 183, RELA only. No symbol
  prefix. Local symbols first, `.symtab` sh_info is the first global. Relocations:
  R_AARCH64_ABS64 257, ADR_PREL_PG_HI21 275, ADD_ABS_LO12_NC 277, LDST8/16/32/64/128_ABS_LO12_NC
  278/284/285/286/299, JUMP26 282 (`b`), CALL26 283 (`bl`), ADR_GOT_PAGE 311, LD64_GOT_LO12_NC
  312. https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst,
  https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html
- Sections in ELF: `.text` (AX), `.rodata.str1.1` (AMS, entsize 1), `.data.rel.ro` (WA, the
  vtables and tables with pointers), `.data` (WA).
- ELF32 (arm): ELFCLASS32, EM_ARM 40, REL (RELA is also allowed; REL keeps the addend in the
  instruction or data word), e_flags 0x05000000 (EABI v5) | 0x400 (EF_ARM_ABI_FLOAT_HARD).
  R_ARM_ABS32 2, CALL 28 (`bl`, encoded 0xebfffffe so the REL addend is -8), JUMP24 29,
  MOVW_ABS_NC 43, MOVT_ABS 44. Mapping symbols `$a` (code) and `$d` (data) at offset 0 of each
  section. https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst
- AAPCS32 (arm): r0-r3 arguments and result (r0:r1 for 64-bit), r4-r11 callee-saved, r12
  IP, r13 sp, r14 lr, r15 pc; sp 8-byte aligned at public interfaces; a 64-bit core argument
  takes an even register pair (C.3) or an 8-aligned stack slot and is never split; hard float
  passes f32 in s0-s15 and f64 in d0-d7 with back-filling until a VFP argument reaches the stack;
  a memory result goes through an address in r0 and the arguments start at r1; s16-s31 are
  callee-saved. https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst

## arm32 selector

- Value model (simple, correct, not fast): every local in an fp-relative frame slot; an
  expression leaves its value in r0 or r0:r1 (floats as bits), a memory value as its address in
  r0; a binary operation keeps the left value with `push {r0, r1}`. r2, r3, r12, lr scratch;
  r4-r10 unused except around `svc` (r4-r7 saved there) and in `main`/udivmod64 (saved).
- Frame: `push {r11, lr}; mov r11, sp; movw/movt ip, #frame; sub sp, sp, ip`, epilog
  `mov sp, r11; pop {r11, pc}`. Stack parameters at [fp + 8].
- No integer divide instruction is assumed (`sdiv`/`udiv` are optional in armv7-a): division,
  64-bit shifts and `str ==` call file-local routines emitted on use (xl$udivmod32,
  xl$udivmod64, xl$lsl64, xl$lsr64, xl$asr64, xl$streq).
- Every address is `movw`/`movt` with MOVW_ABS_NC/MOVT_ABS: static, non-PIC programs only.
- **Gaps (E0714 or wrong):** T32 (Thumb-2) encoding: only A32 is emitted; `try` and landing pads
  (the unwind table lists every function with no pad); `__builtin_asm`; 64-bit atomics
  (ldrexd/strexd) and atomics on sub-word types other than load/store; float to 64-bit integer
  conversion; aggregates by value in the C convention (`export(c)`/`declare` composites);
  position-independent code for `-shared`; a register allocator.

## Entry and boot

- Every whole-program object defines `_xl_initAll`, `_xl_finiAll` and the C `main(argc, argv)`
  stub that runs init, the `@entry` function (symbol `xl$entry`), fini, and returns the result.
- macOS: `ld` (through `cc`) adds crt and libSystem; dyld calls `main` through LC_MAIN. The
  kernel refuses static arm64 executables, so there is no `_start` on macOS.
- Linux and the generic OS: the object also defines a **weak** `_start`: the kernel enters with
  sp 16-aligned at argc, then argv, NULL, envp, NULL, auxv
  (https://github.com/torvalds/linux/blob/master/fs/binfmt_elf.c create_elf_tables). `_start`
  clears x29/x30, calls `main(argc, argv)`, then `exit_group(result)`. With `--crt` the C
  library's crt1 `_start` is strong and wins.

## Linking

- macOS: `cc -arch arm64 -o out objs -lLIB` (shared: `-dynamiclib`), static library with
  `libtool -static`.
- Linux and the generic OS: `ld.lld -static -o out objs` (or GNU `ld` on a Linux host), shared
  with `-shared`, static library with `llvm-ar rcs` or `ar rcs`. Works from any host: no C
  library, no sysroot. `--crt` uses the host `cc` and needs a Linux host.
- arm32: the same with `-m armelf_linux_eabi` (GNU `ld` defaults to its host emulation).
- Docker runs the results here: `docker run --rm --platform linux/arm64 -v DIR:/w busybox /w/prog`
  (`linux/arm/v7` for arm32).
  OrbStack quirk: the first exec of a bind-mounted file inside `sh -c` may say "not found"; run
  the binary as the container command.

## Windows arm64

Sources: PE Format, https://learn.microsoft.com/en-us/windows/win32/debug/pe-format; Overview of
ARM64 ABI conventions, https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions;
ARM64 exception handling, https://learn.microsoft.com/en-us/cpp/build/arm64-exception-handling.

- COFF object: 20-byte file header (Machine 0xAA64, NumberOfSections, TimeDateStamp 0,
  PointerToSymbolTable, NumberOfSymbols counting aux records, SizeOfOptionalHeader 0,
  Characteristics 0), 40-byte section headers, raw data, 10-byte relocations (VirtualAddress,
  SymbolTableIndex, Type u16), 18-byte symbol records, then the string table (u32 total size
  including itself, offsets count those 4 bytes). A name over 8 bytes is 4 zero bytes plus its
  string table offset.
- Sections: `.text` CNT_CODE|MEM_EXECUTE|MEM_READ (0x60000020), `.rdata` (literals and SEC_CONST)
  CNT_INITIALIZED_DATA|MEM_READ (0x40000040), `.data` adds MEM_WRITE (0xC0000040); alignment
  IMAGE_SCN_ALIGN_* = (log2 + 1) << 20 (objects only, at most 8192). No `.bss` kind is produced.
- Symbols: storage class EXTERNAL 2, STATIC 3 (locals and section symbols), WEAK_EXTERNAL 105;
  Type 0x20 for functions. Each section has a STATIC section symbol with a format-5 aux record
  (Length, NumberOfRelocations, NumberOfLinenumbers, CheckSum, Number, Selection). A weak
  definition `x` is written as the defined EXTERNAL `.weak.x.default` plus the WEAK_EXTERNAL `x`
  with a format-3 aux record (TagIndex, Characteristics IMAGE_WEAK_EXTERN_SEARCH_ALIAS = 3 from
  winnt.h), the LLVM form; checked with lld-link against a strong definition.
- Relocations: ADDR64 0x0E (8 data bytes, addend in place), BRANCH26 0x03 (`b` and `bl`),
  PAGEBASE_REL21 0x04 (`adrp`), PAGEOFFSET_12A 0x06 (`add`), PAGEOFFSET_12L 0x07 (`ldr`/`str`,
  scale from the instruction). Others in the spec: ADDR32 0x01, ADDR32NB 0x02, REL21 0x05,
  SECREL 0x08, SECREL_LOW12A/HIGH12A/LOW12L 0x09-0x0B, TOKEN 0x0C, SECTION 0x0D, BRANCH19 0x0F,
  BRANCH14 0x10, REL32 0x11. Addends are carried in the patched instruction or data, as Mach-O.
- No GOT: an R_GOT_* access of `x` (a declared global or the address of a declared function) is
  written against `__imp_x`, the import address slot; `adrp` + `ldr` of it loads the address.
- ABI vs AAPCS64: x18 is the TEB pointer in user mode and must never be written (the selector
  never uses x18 on any OS); x29 must point at the {x29, x30} record (always kept here); the 16
  bytes below sp are reserved, no red zone; sp 16-aligned with alignment faults enabled; non-variadic
  parameter passing is AAPCS64; variadic calls use no FP/SIMD registers, all arguments go as if on
  a stack whose first 64 bytes are x0-x7 (not implemented: no variadic C calls are emitted).
- Stack probes: a frame of 4 KiB or more must touch each page in order (`__chkstk`, x15 = size /
  16). The selector emits none; instead the link commits the whole stack
  (`/stack:8388608,8388608`), so no guard page can be skipped.
- System calls: Windows has no stable system call numbers; `syscall(...)` on windows is E0714
  with a hint to use an `@library("kernel32") declare fn` import.
- Entry: `mainCRTStartup` (data.xl `emitWinStart`): argc/argv from the `register(args)`
  function when present, else 0/NULL; calls `main` (init, entry, fini) and passes the result to
  kernel32 `ExitProcess`, called through the import thunk.
- Link: `lld-link /machine:arm64 /nodefaultlib /stack:8M,8M /subsystem:console
  /entry:mainCRTStartup objs <lib>.lib...`; one import library per DLL (kernel32 always, plus
  every `@library`) generated from a `.def` file by `llvm-dlltool -m arm64`, so no Windows SDK is
  needed and any host links. `-shared`: `/dll /noentry`. Static: `llvm-lib /out:`.
- **Gaps:** no `.pdata`/`.xdata` unwind data (the image loads and runs; SEH, debuggers and ETW
  cannot unwind through xlang frames; `try` uses the stdlib's own pad table, not SEH); `--crt` is
  rejected; a DLL exports nothing (no `/export:` or `.edata` for `export(c)` yet) and runs no
  DllMain; imported data needs `__imp_` slots from the import library (functions only are
  verified); the C convention's packed stack arguments are the Apple variant (same gap as Linux).
- Verified here (macOS host): objects read back with llvm-readobj; exit.xl, packs.xl and a
  kernel32 WriteFile program link with lld-link into ARM64 PE images with the right imports.
  Not run: no Windows on Arm host. test/e2e/run.sh 3c runs them when it is on Windows arm64.

## Language facts met while writing codegen

- A line may not start with a binary operator (`||`, `+`): a newline ends the expression unless
  a bracket is open. Put the operator at the end of the line.
- `type` and `str` are keywords and cannot name fields or variables.
- `String` has no indexed store; build bytes in a `Vec<u8>` or in order.

## Fixes over the first version

- The selector no longer knows Mach-O: section kinds and neutral relocation kinds replace
  `__text`/`ARM64_RELOC_*`; a stray literal `3` for PAGE21 in expr.xl is now `R_PAGE21`.
- Mach-O symbols are written locals, defined externals, undefined, whatever order the selector
  used.
- `legalize` (emit.xl) no longer divides by a zero access size for an untyped HIR node.

## Status

- Done: macOS arm64 as before; Linux arm64 and the generic OS (ELF writer, Linux syscalls,
  `_start`, static ld.lld link), verified in Docker by test/e2e/run.sh section 3b; Linux arm32
  and the generic OS (A32 selector, ELF32, `_start` with exit_group 248), verified the same way
  (exit, write, packs; lazy_methods and impl_calls also pass by hand). layout_queries.xl assumes
  64-bit pointers and is not run for arm32.
- Windows arm64 (COFF, lld-link): objects and linked images verified statically, not run (see
  "Windows arm64"). Mach-O fix: the segment file image now mirrors its memory image, so
  filesize == vmsize (nm rejected objects where the file padding differed); run.sh section 3
  checks every object with llvm-readobj and the Mach-O one with nm.
- Open: the arm32 gaps above; the Windows gaps above; the Linux variant of the arm64 C convention; x86-64; `@packed`/`@align`/`@layout(shuffle)` are laid out as plain v1.
