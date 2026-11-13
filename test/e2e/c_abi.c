/* e2e peer of c_abi.xl: C functions that take and return structs by value, compiled by the host C
 * compiler, plus `c_abi_run`, which calls the xlang `export(c)` functions the same way. Every struct
 * picks a different rule of the target's C convention (see src/codegen/llvm/c_abi/c_abi.hpp). No
 * libc: the program starts in xlang and links only these functions. */

struct Pair { int a, b; };                  /* 8 bytes of integers */
struct Vec2 { float x, y; };                /* 8 bytes of floats: SSE / HFA / one integer register */
struct Mixed { long long n; double d; };    /* INTEGER + SSE */
struct Rgb { unsigned char r, g, b; };      /* 3 bytes: not a power of two */
struct Tri { float x, y, z; };              /* 12 bytes of floats */
struct Quad { double a, b, c, d; };         /* 32 bytes: an HFA on arm64, memory on x86-64 */
struct Big { long long a, b, c, d; };       /* 32 bytes: memory everywhere */
struct Two64 { long long a, b; };           /* two registers or none */

/* The stdlib provides these to xlang programs (xl-std/mem); this one has no stdlib. Volatile, so the
 * C compiler does not turn the loops back into calls of themselves. */
typedef __SIZE_TYPE__ size_t;
void *memset(void *dest, int c, size_t n) {
    volatile unsigned char *d = dest;
    while (n--) *d++ = (unsigned char)c;
    return dest;
}
void *memcpy(void *dest, const void *src, size_t n) {
    volatile unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

struct Pair cPair(struct Pair p) { struct Pair r = {p.a * 10, p.b * 10}; return r; }
struct Vec2 cVec2(struct Vec2 v) { struct Vec2 r = {v.y, v.x}; return r; }
struct Mixed cMixed(struct Mixed m) { struct Mixed r = {m.n + 1, m.d * 2}; return r; }
struct Rgb cRgb(struct Rgb c) { struct Rgb r = {(unsigned char)(c.b + 10), c.g, (unsigned char)(c.r + 10)}; return r; }
struct Tri cTri(struct Tri t) { struct Tri r = {t.z, t.y, t.x}; return r; }
struct Quad cQuad(struct Quad q) { struct Quad r = {q.d, q.c, q.b, q.a}; return r; }
struct Big cBig(struct Big b) { struct Big r = {b.d, b.c, b.b, b.a + 100}; return r; }

/* Registers run out: the struct goes to the stack whole, and (SysV) a later integer still takes the
 * last free register. Each value is one digit of the result, so any misplaced argument shows. */
long long cSpill5(long long a, long long b, long long c, long long d, long long e, struct Two64 p, long long f) {
    return a * 10000000 + b * 1000000 + c * 100000 + d * 10000 + e * 1000 + p.a * 100 + p.b * 10 + f;
}
long long cSpill7(long long a, long long b, long long c, long long d, long long e, long long g, long long h,
                  struct Two64 p, long long f) {
    return a * 100000000 + b * 10000000 + c * 1000000 + d * 100000 + e * 10000 + g * 1000 + h * 100 + p.a * 10 + p.b + f;
}

/* Narrow integers: the caller widens them (zeroext/signext). */
int cWiden(signed char a, unsigned char b, short c, _Bool d) { return a + b + c + (d ? 1000 : 0); }
signed char cNarrow(void) { return -5; }

/* The xlang side of the same shapes. */
struct Pair xlPair(struct Pair p);
struct Vec2 xlVec2(struct Vec2 v);
struct Mixed xlMixed(struct Mixed m);
struct Rgb xlRgb(struct Rgb c);
struct Tri xlTri(struct Tri t);
struct Quad xlQuad(struct Quad q);
struct Big xlBig(struct Big b);
long long xlSpill5(long long a, long long b, long long c, long long d, long long e, struct Two64 p, long long f);
long long xlSpill7(long long a, long long b, long long c, long long d, long long e, long long g, long long h,
                   struct Two64 p, long long f);
short xlSmall(signed char a, unsigned short b);

/* 0 when every call came back right, else the number of the first wrong one. */
int c_abi_run(void) {
    struct Pair p = xlPair((struct Pair){3, 4});
    if (p.a != 4 || p.b != 3) return 101;
    struct Vec2 v = xlVec2((struct Vec2){1.5f, 2.25f});
    if (v.x != 3.75f || v.y != 0.75f) return 102;
    struct Mixed m = xlMixed((struct Mixed){41, 0.25});
    if (m.n != 42 || m.d != 0.5) return 103;
    struct Rgb c = xlRgb((struct Rgb){1, 2, 3});
    if (c.r != 2 || c.g != 3 || c.b != 4) return 104;
    struct Tri t = xlTri((struct Tri){1.0f, 2.0f, 3.0f});
    if (t.x != 2.0f || t.y != 4.0f || t.z != 6.0f) return 105;
    struct Quad q = xlQuad((struct Quad){1.0, 2.0, 3.0, 4.0});
    if (q.a != 10.0 || q.b != 1.0 || q.c != 2.0 || q.d != 3.0) return 106;
    struct Big b = xlBig((struct Big){1, 2, 3, 4});
    if (b.a != 10 || b.b != 2 || b.c != 3 || b.d != 4) return 107;
    if (xlSpill5(1, 2, 3, 4, 5, (struct Two64){6, 7}, 8) != 12345678) return 108;
    if (xlSpill7(1, 2, 3, 4, 5, 6, 7, (struct Two64){8, 9}, 1) != 123456790) return 109;
    if (xlSmall(-3, 60000) != (short)(60000 - 3)) return 110;
    return 0;
}
