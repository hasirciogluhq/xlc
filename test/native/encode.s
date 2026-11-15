and x0, x1, #0xff
and x0, x1, #0xff00ff00ff00ff00
orr x0, x1, #0x8000000000000000
eor x0, x1, #0xf0f0f0f0f0f0f0f0
and x0, x1, #0xf00000000000000f
and x0, x1, #0x5555555555555555
and x0, x1, #0x7fffffff
and x0, x1, #0xfffffffffffffffe
lsl x0, x1, #5
lsr x0, x1, #7
asr x0, x1, #63
madd x0, x1, x2, x3
add w0, w1, w2
sub w0, w1, w2
mul w0, w1, w2
ldrh w0, [x1, #6]
strh w0, [x1, #6]
ldrsb x0, [x1, #3]
ldrsh x0, [x1, #6]
fadd d0, d1, d2
fsub s0, s1, s2
fmul d0, d1, d2
fdiv s0, s1, s2
fneg d0, d1
fmov d0, d1
fcmp d1, d2
fmov d0, x1
fmov s0, w1
fmov x0, d1
fmov w0, s1
scvtf d0, x1
scvtf s0, x1
ucvtf d0, x1
ucvtf s0, x1
fcvtzs x0, d1
fcvtzs x0, s1
fcvtzu x0, d1
fcvtzu x0, s1
fcvt d0, s1
fcvt s0, d1
ldr d0, [x1, #16]
ldr s0, [x1, #8]
str d0, [x1, #16]
str s0, [x1, #8]
ldar x0, [x1]
ldar w0, [x1]
ldarb w0, [x1]
stlr x0, [x1]
stlrh w0, [x1]
swpal x2, x3, [x1]
ldaddal w2, w3, [x1]
casal x2, x3, [x1]
dmb ish
