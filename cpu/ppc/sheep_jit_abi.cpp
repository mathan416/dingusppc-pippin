// SPDX-License-Identifier: GPL-3.0-or-later
#include "nw_jit.h"
#if defined(__aarch64__)
asm(R"(
.text
.align 2
.global sheep_jit_abi_check
.type sheep_jit_abi_check, %function
sheep_jit_abi_check:
sub sp,sp,#160
stp x19,x20,[sp,#0]
stp x21,x22,[sp,#16]
stp x23,x24,[sp,#32]
stp x25,x26,[sp,#48]
stp x27,x28,[sp,#64]
stp x29,x30,[sp,#80]
stp d8,d9,[sp,#96]
stp d10,d11,[sp,#112]
stp d12,d13,[sp,#128]
stp d14,d15,[sp,#144]
mov x16,x0
mov x0,x1
mov x19,#19
mov x20,#20
mov x21,#21
mov x22,#22
mov x23,#23
mov x24,#24
mov x25,#25
mov x26,#26
mov x27,#27
mov x28,#28
mov x29,#29
fmov d8,x19
fmov d9,x20
fmov d10,x21
fmov d11,x22
fmov d12,x23
fmov d13,x24
fmov d14,x25
fmov d15,x26
blr x16
mov w17,w0
mov x0,#0
cmp x19,#19
cset x9,ne
orr x0,x0,x9,lsl #0
cmp x20,#20
cset x9,ne
orr x0,x0,x9,lsl #1
cmp x21,#21
cset x9,ne
orr x0,x0,x9,lsl #2
cmp x22,#22
cset x9,ne
orr x0,x0,x9,lsl #3
cmp x23,#23
cset x9,ne
orr x0,x0,x9,lsl #4
cmp x24,#24
cset x9,ne
orr x0,x0,x9,lsl #5
cmp x25,#25
cset x9,ne
orr x0,x0,x9,lsl #6
cmp x26,#26
cset x9,ne
orr x0,x0,x9,lsl #7
cmp x27,#27
cset x9,ne
orr x0,x0,x9,lsl #8
cmp x28,#28
cset x9,ne
orr x0,x0,x9,lsl #9
cmp x29,#29
cset x9,ne
orr x0,x0,x9,lsl #10
fmov x10,d8
cmp x10,#19
cset x9,ne
orr x0,x0,x9,lsl #11
fmov x10,d9
cmp x10,#20
cset x9,ne
orr x0,x0,x9,lsl #12
fmov x10,d10
cmp x10,#21
cset x9,ne
orr x0,x0,x9,lsl #13
fmov x10,d11
cmp x10,#22
cset x9,ne
orr x0,x0,x9,lsl #14
fmov x10,d12
cmp x10,#23
cset x9,ne
orr x0,x0,x9,lsl #15
fmov x10,d13
cmp x10,#24
cset x9,ne
orr x0,x0,x9,lsl #16
fmov x10,d14
cmp x10,#25
cset x9,ne
orr x0,x0,x9,lsl #17
fmov x10,d15
cmp x10,#26
cset x9,ne
orr x0,x0,x9,lsl #18
orr x0,x17,x0,lsl #32
ldp d8,d9,[sp,#96]
ldp d10,d11,[sp,#112]
ldp d12,d13,[sp,#128]
ldp d14,d15,[sp,#144]
ldp x19,x20,[sp,#0]
ldp x21,x22,[sp,#16]
ldp x23,x24,[sp,#32]
ldp x25,x26,[sp,#48]
ldp x27,x28,[sp,#64]
ldp x29,x30,[sp,#80]
add sp,sp,#160
ret
.size sheep_jit_abi_check, .-sheep_jit_abi_check
)");
#else
extern "C" uint64_t sheep_jit_abi_check(nw_jit_fn fn,nw_jit_cpu *cpu){return reinterpret_cast<nw_jit_mapped_fn>(fn)(cpu);}
#endif
