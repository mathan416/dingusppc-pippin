// Project: DingusPPC-Pippin
// File: cpu/ppc/generic-continuation-tests.inc
// Purpose: Verify-mode tests for generic JIT continuations.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// Q193 kind0 differential against exact verified Q185/Q175 generic288.
// Actual1/2/3/4 returns use planned4; actual32 uses planned32. No new admission,
// wrapper prefix, resident-state ABI, type3 or deferred CPU state is introduced.
#if defined(__aarch64__)
extern "C" unsigned sheep_generic_continuation_reference_a64(SheepLoopContext*);
asm(R"(
.text
.p2align 4
.global sheep_generic_continuation_reference_a64
.type sheep_generic_continuation_reference_a64,%function
sheep_generic_continuation_reference_a64:
.cfi_startproc
 stp x19,x20,[sp,#-96]!
.cfi_def_cfa_offset 96
.cfi_offset 19,-96
.cfi_offset 20,-88
 stp x21,x22,[sp,#16]
.cfi_offset 21,-80
.cfi_offset 22,-72
 stp x23,x24,[sp,#32]
.cfi_offset 23,-64
.cfi_offset 24,-56
 stp x25,x26,[sp,#48]
.cfi_offset 25,-48
.cfi_offset 26,-40
 stp x27,x28,[sp,#64]
.cfi_offset 27,-32
.cfi_offset 28,-24
 stp x29,x30,[sp,#80]
.cfi_offset 29,-16
.cfi_offset 30,-8
 mov x28,x0
 ldr x19,[x28]
 ldr x20,[x28,#88]
 ldr w9,[x28,#192]
 orr x20,x20,x9
 ldr x21,[x28,#8]
 ldr x23,[x28,#80]
 ldr x29,[x28,#56]
 ldr x22,[x29]
 ldr w24,[x19,#256]
 mov w27,#0
 // Only CPU callbacks can change instruction flags. Start the leaf fast loop
 // with no pending action; the normal C++ path handles any pre-existing flag.
 .word 0xb84003a9 | (((exec_flags-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lgeneric_continuation_reference_cold
.Lgeneric_continuation_reference_fetch:
 tbnz x20,#0,.Lgeneric_continuation_reference_fetch_ram
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 // Decode only the descriptor tag bits; preserve opcode, metadata and index.
 tbz w25,#1,.Lgeneric_continuation_reference_low_tag
 tbnz w25,#0,.Lgeneric_continuation_reference_native
 // Match the canonical retry counter, including its final blocked iteration.
 ldr x11,[x28,#16]
 ldrb w9,[x11,x10]
 cbz w9,.Lgeneric_continuation_reference_retry_zero
 cmp w9,#255
 b.eq .Lgeneric_continuation_reference_scalar
 sub w9,w9,#1
 strb w9,[x11,x10]
 cbnz w9,.Lgeneric_continuation_reference_scalar
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lgeneric_continuation_reference_scalar
.Lgeneric_continuation_reference_fetch_ram:
 // Writable code is fetched anew for every instruction. No native RAM cache.
 ldur w0,[x20,#-1]
 rev w0,w0
 lsl x25,x0,#32
 b .Lgeneric_continuation_reference_scalar
.Lgeneric_continuation_reference_retry_zero:
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lgeneric_continuation_reference_cold
.Lgeneric_continuation_reference_native:
 lsr x9,x25,#32
 cmp w9,w24
 b.ne .Lgeneric_continuation_reference_cold
 ubfx w9,w25,#2,#5
 add w9,w9,#1
 sub x10,x23,x22
 cmp x9,x10
 b.hi .Lgeneric_continuation_reference_cold
 lsr w9,w25,#7
 ldr x10,[x28,#24]
 add x9,x10,w9,uxtw #2
 ldr x10,[x28,#128]
 strb wzr,[x10]
 mov w10,#1
 str w10,[x28,#148]
 mov x0,x19
 blr x9
 mov w26,w0
 ubfx w9,w25,#2,#5
 add w9,w9,#1
 cmp w26,w9
 b.hi .Lgeneric_continuation_reference_trap
 ldr w25,[x19,#256]
 cmp w26,#4
 mov w14,#1024
 b.hs .Lgeneric_continuation_reference_native_fast
 // Successful short branches need no rejection or interpreter round-trip.
 // Zero/failed-guard prefixes retain their original slow-path backoff.
 cbz w26,.Lgeneric_continuation_reference_native_short_guard
 ldr x9,[x28,#128]
 ldrb w9,[x9]
 mov w14,#128
 cbz w9,.Lgeneric_continuation_reference_native_fast
.Lgeneric_continuation_reference_native_short_guard:
 mov x0,x20
 mov w1,w24
 ldr x9,[x28,#120]
 blr x9
 cbnz w26,.Lgeneric_continuation_reference_native_prefix
 str w24,[x19,#256]
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 b .Lgeneric_continuation_reference_scalar
.Lgeneric_continuation_reference_native_fast:
 // All indices address the private, allocated decoder table, even when a
 // guest branch will later miss instruction translation. Prefetch has no
 // architectural side effects; MMU guards still control the actual target.
 sub w9,w25,w24
 add x9,x20,w9,sxtw
 ubfx x9,x9,#2,#20
 prfm pldl1keep,[x21,x9,lsl #3]
 // Qualified native blocks contain no calls and cannot change exec_flags.
 // Keep their same-page target local; synthesize the original last-PC/branch
 // post-step state only when handing control back to the C++ event loop.
 sub w9,w26,#1
 add x20,x20,w9,uxtw #2
 add w24,w24,w9,lsl #2
 add x22,x22,x9
 // W24 is the last executed source PC in this mapped page. No context
 // reload is needed to compare target and source page identities.
.global sheep_generic_continuation_reference_guard_native
.hidden sheep_generic_continuation_reference_guard_native
sheep_generic_continuation_reference_guard_native:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lgeneric_continuation_reference_native_fast_old
 // Preserve the canonical last branch target even when consuming the branch
 // locally. It remains observable at later event and exception boundaries.
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lgeneric_continuation_reference_native_fast_sequential
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
.Lgeneric_continuation_reference_native_fast_sequential:
 add w27,w27,w26
 str x22,[x29]
 cmp x22,x23
 b.hs .Lgeneric_continuation_reference_native_fast_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lgeneric_continuation_reference_native_fast_pending
 cmp w27,w14
 b.hs .Lgeneric_continuation_reference_native_fast_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lgeneric_continuation_reference_native_fast_pending
 // No device/event polling is removed: deadlines, external timer and power
 // are checked after every bounded block, as in the original path.
 sub w9,w25,w24
 sxtw x9,w9
 add x20,x20,x9
 mov w24,w25
 add x22,x22,#1
 str x22,[x29]
 // The native block already stored this exact target into CPU.pc.
 b .Lgeneric_continuation_reference_fetch
.Lgeneric_continuation_reference_native_fast_old:
 str w24,[x19,#256]
 b .Lgeneric_continuation_reference_native_prefix_done
.Lgeneric_continuation_reference_native_fast_pending:
 str w24,[x19,#256]
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lgeneric_continuation_reference_pending
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lgeneric_continuation_reference_pending
.Lgeneric_continuation_reference_native_prefix:
 sub w9,w26,#1
 add x20,x20,w9,uxtw #2
 add w24,w24,w9,lsl #2
 add x22,x22,x9
 str w24,[x19,#256]
.Lgeneric_continuation_reference_native_prefix_done:
 add w9,w24,#4
 cmp w25,w9
 b.ne .Lgeneric_continuation_reference_native_branch
 // Target is already proven to be source+4. Only target bits2..11==0 cross a
 // page, including the 32-bit address wrap from fffffffc to zero.
.global sheep_generic_continuation_reference_guard_next
.hidden sheep_generic_continuation_reference_guard_next
sheep_generic_continuation_reference_guard_next:
 tst w25,#0xffc
 b.ne .Lgeneric_continuation_reference_after
.Lgeneric_continuation_reference_native_branch:
 // All indices address the private, allocated decoder table, even when a
 // guest branch will later miss instruction translation. Prefetch has no
 // architectural side effects; MMU guards still control the actual target.
 sub w9,w25,w24
 add x9,x20,w9,sxtw
 ubfx x9,x9,#2,#20
 prfm pldl1keep,[x21,x9,lsl #3]
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lgeneric_continuation_reference_after
.Lgeneric_continuation_reference_low_tag:
 // Scalar tag1 falls through; native/retry routes keep their old branches.
 tbz w25,#0,.Lgeneric_continuation_reference_cold
.Lgeneric_continuation_reference_scalar:
 mov w26,#1
 lsr x0,x25,#32
 lsr w9,w0,#26
 cmp w9,#18
 b.eq .Lgeneric_continuation_reference_b
 cmp w9,#16
 b.eq .Lgeneric_continuation_reference_bc
.Lgeneric_continuation_reference_handler:
 tbnz x20,#0,.Lgeneric_continuation_reference_ram_handler
 // The packed descriptor stores a signed handler offset and FPU flag.
 ldr w9,[x28,#116]
 cbz w9,.Lgeneric_continuation_reference_normal_handler
 tbz w25,#31,.Lgeneric_continuation_reference_normal_handler
 ldr x9,[x28,#40]
 b .Lgeneric_continuation_reference_call_handler
.Lgeneric_continuation_reference_normal_handler:
 lsl w9,w25,#1
 asr w9,w9,#3
 ldr x10,[x28,#32]
 add x9,x10,w9,sxtw #2
 b .Lgeneric_continuation_reference_call_handler
.Lgeneric_continuation_reference_ram_handler:
 // Exactly the canonical ppc_main_opcode index and selected FPU table.
 lsr w9,w0,#15
 and w9,w9,#0x1f800
 and w10,w0,#0x7ff
 orr w9,w9,w10
 ldr x10,[x28,#184]
 ldr x9,[x10,w9,uxtw #3]
 b .Lgeneric_continuation_reference_call_handler
.Lgeneric_continuation_reference_call_handler:
 blr x9
 ldr x22,[x29]
 b .Lgeneric_continuation_reference_after
.Lgeneric_continuation_reference_b:
 tbnz w0,#1,.Lgeneric_continuation_reference_handler
 tbz w0,#0,.Lgeneric_continuation_reference_b_target
 add w9,w24,#4
 ldr w10,[x28,#112]
 str w9,[x19,x10]
.Lgeneric_continuation_reference_b_target:
 sbfx w9,w0,#0,#26
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lgeneric_continuation_reference_native_branch
.Lgeneric_continuation_reference_bc:
 tst w0,#3
 b.ne .Lgeneric_continuation_reference_handler
 ubfx w9,w0,#21,#5
 and w10,w9,#30
 cmp w10,#4
 b.eq .Lgeneric_continuation_reference_bc_cr
 cmp w10,#12
 b.ne .Lgeneric_continuation_reference_handler
.Lgeneric_continuation_reference_bc_cr:
 ldr w10,[x28,#108]
 ldr w10,[x19,x10]
 ubfx w11,w0,#16,#5
 mov w12,#31
 sub w11,w12,w11
 lsr w10,w10,w11
 and w10,w10,#1
 ubfx w9,w9,#3,#1
 cmp w9,w10
 b.ne .Lgeneric_continuation_reference_after
 sxth w9,w0
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lgeneric_continuation_reference_native_branch
.Lgeneric_continuation_reference_after:
 // Keep the final pre-increment value for ppc_exec's existing event check.
 str x22,[x29]
 add w27,w27,w26
 cmp x22,x23
 b.hs .Lgeneric_continuation_reference_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lgeneric_continuation_reference_pending
 cmp w27,#128
 b.hs .Lgeneric_continuation_reference_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lgeneric_continuation_reference_pending
 .word 0xb84003a9 | (((exec_flags-g_icycles)&0x1ff)<<12)
 cbz w9,.Lgeneric_continuation_reference_advance
 cmp w9,#1
 b.ne .Lgeneric_continuation_reference_pending
 ldr x10,[x28,#64]
 .word 0xb84003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 // Scalar callbacks cannot change the loop's mapped source W24. Keep
 // arbitrary targets subject to the exact existing primary-ITLB mapper.
.global sheep_generic_continuation_reference_guard_branch
.hidden sheep_generic_continuation_reference_guard_branch
sheep_generic_continuation_reference_guard_branch:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lgeneric_continuation_reference_map_target
 // Only a plain same-page branch can be consumed inside the fast loop.
 sub w9,w25,w24
 sxtw x9,w9
 add x20,x20,x9
 mov w24,w25
 str wzr,[x10]
 b .Lgeneric_continuation_reference_continue
.Lgeneric_continuation_reference_advance:
.global sheep_generic_continuation_reference_guard_advance
.hidden sheep_generic_continuation_reference_guard_advance
sheep_generic_continuation_reference_guard_advance:
 add w10,w24,#4
 // Source+4 leaves its page iff target bits2..11 are zero, even unaligned.
 tst w10,#0xffc
 b.ne .Lgeneric_continuation_reference_advance_same
 mov w25,w10
 ldr x10,[x28,#64]
 b .Lgeneric_continuation_reference_map_target
.Lgeneric_continuation_reference_advance_same:
 add w24,w24,#4
 add x20,x20,#4
.Lgeneric_continuation_reference_continue:
 add x22,x22,#1
 str x22,[x29]
 str w24,[x19,#256]
 b .Lgeneric_continuation_reference_fetch
.Lgeneric_continuation_reference_map_target:
 // A primary ITLB hit already carries the canonical translated host offset.
 // Never walk/fill an MMU entry speculatively. Non-memory targets return to C++.
 // The private context always binds these canonical global variables.
 // Read their current values on every map, including after MTMSR callbacks.
 adrp x9,pCurITLB1
 ldr x9,[x9,:lo12:pCurITLB1]
 adrp x11,tlb_size_mask
 ldr w11,[x11,:lo12:tlb_size_mask]
 lsr w12,w25,#12
 and w11,w11,w12
 add x9,x9,x11,lsl #5
 // Read only canonical entry metadata early, never guest payload.
 ldp x11,x9,[x9]
 and w12,w25,#0xfffff000
 cmp w11,w12
 b.ne .Lgeneric_continuation_reference_pending
 tbz x11,#32,.Lgeneric_continuation_reference_pending
 add x9,x9,w25,uxtw
 ldr x11,[x28,#168]
 cmp x9,x11
 b.lo .Lgeneric_continuation_reference_target_ram
 ldr x11,[x28,#176]
 cmp x9,x11
 b.hs .Lgeneric_continuation_reference_target_ram
 mov x20,x9
 str wzr,[x28,#192]
 b .Lgeneric_continuation_reference_mapped
.Lgeneric_continuation_reference_target_ram:
 ldr w11,[x28,#196]
 cbz w11,.Lgeneric_continuation_reference_pending
 tst x9,#3
 b.ne .Lgeneric_continuation_reference_pending
 orr x20,x9,#1
 mov w11,#1
 str w11,[x28,#192]
.Lgeneric_continuation_reference_mapped:
 mov w24,w25
 str w12,[x28,#96]
 add w11,w12,#4095
 str w11,[x28,#100]
 str wzr,[x10]
 b .Lgeneric_continuation_reference_continue
.Lgeneric_continuation_reference_pending:
 mov w0,#1
 b .Lgeneric_continuation_reference_return
.Lgeneric_continuation_reference_cold:
 mov w0,#0
.Lgeneric_continuation_reference_return:
 str w27,[x28,#144]
 and x9,x20,#0xfffffffffffffffe
 str x9,[x28,#88]
 ldp x19,x20,[sp]
.cfi_restore 19
.cfi_restore 20
 ldp x21,x22,[sp,#16]
.cfi_restore 21
.cfi_restore 22
 ldp x23,x24,[sp,#32]
.cfi_restore 23
.cfi_restore 24
 ldp x25,x26,[sp,#48]
.cfi_restore 25
.cfi_restore 26
 ldp x27,x28,[sp,#64]
.cfi_restore 27
.cfi_restore 28
 ldp x29,x30,[sp,#80]
.cfi_restore 29
.cfi_restore 30
 add sp,sp,#96
.cfi_def_cfa_offset 0
 ret
.Lgeneric_continuation_reference_trap:
 brk #0
.cfi_endproc
.size sheep_generic_continuation_reference_a64,.-sheep_generic_continuation_reference_a64
)");
[[gnu::noinline]] static void generic_continuation_clobber(){
 // X18 is the platform register on some hosts. All volatile registers used
 // by either loop, including descriptor/target X7 and NZCV, are overwritten.
 asm volatile(
  "mov x0,#0x170\nmov x1,#0x171\nmov x2,#0x172\nmov x3,#0x173\n"
  "mov x4,#0x174\nmov x5,#0x175\nmov x6,#0x176\nmov x7,#0x177\n"
  "mov x8,#0x178\nmov x9,#0x179\nmov x10,#0x17a\nmov x11,#0x17b\n"
  "mov x12,#0x17c\nmov x13,#0x17d\nmov x14,#0x17e\nmov x15,#0x17f\n"
  "mov x16,#0x180\nmov x17,#0x181\ncmp x7,x8\n"
  :::"x0","x1","x2","x3","x4","x5","x6","x7","x8","x9",
     "x10","x11","x12","x13","x14","x15","x16","x17","cc");
}
struct GenericContinuationFrame {
 uint8_t *ram;TLBEntry *next_table;uint32_t next_mask,target;unsigned action,calls,rejects;uint32_t observed[2][3]{};
};
static GenericContinuationFrame &generic_continuation_frame(){
 return *reinterpret_cast<GenericContinuationFrame*>(uintptr_t(ppc_state.gpr[30])|(uintptr_t(ppc_state.gpr[31])<<32));
}
[[gnu::noinline]] static void generic_continuation_callback(uint32_t op){
 auto &f=generic_continuation_frame();const unsigned i=std::min(f.calls,1u);++f.calls;
 f.observed[i][0]=ppc_state.gpr[24];f.observed[i][1]=ppc_state.gpr[29];f.observed[i][2]=ppc_state.cr;
 ppc_state.gpr[24]+=0x10203u+f.calls;ppc_state.gpr[29]^=0x9e3779b9u+f.calls;ppc_state.cr^=0x51a03c85u;
 f.ram[f.calls*13u]^=uint8_t(op^f.calls);
 if(f.action>=6&&f.calls==1){pCurITLB1=f.next_table;tlb_size_mask=f.next_mask;}
 else if(f.action==1)power_on=false;
 else if(f.action==2){exec_flags=EXEF_EXCEPTION|EXEF_BRANCH;ppc_next_instruction_address=f.target;}
 else exec_timer=true;
 generic_continuation_clobber();
}
[[gnu::noinline]] static void generic_continuation_reject(const uint8_t *code,uint32_t pc){
 ++generic_continuation_frame().rejects;loop_fixture_reject(code,pc);generic_continuation_clobber();
}
#endif
int sheep_generic_continuation_selftest(){
#if defined(__aarch64__) && !defined(LOG_INSTRUCTIONS) && !defined(CPU_PROFILING)
 SetPRS saved;std::memcpy(&saved,&ppc_state,sizeof(saved));const auto flags=exec_flags,next=ppc_next_instruction_address;const auto clock=g_icycles;
 const bool timer0=exec_timer,power0=power_on,bail0=sheep_jit_memory_bail,only=sheep_rom_only,direct=sheep_rom_direct_enabled;
 const bool old601=is_601,oldinclude=include_601;auto *const grabber=ppc_opcode_grabber;
 auto *const i1=pCurITLB1,*const i2=pCurITLB2,*const d1=pCurDTLB1,*const d2=pCurDTLB2;const auto im=CurITLBMode,dm=CurDTLBMode;const auto mask=tlb_size_mask;
 const auto begin=sheep_rom_host_begin,end=sheep_rom_host_end,base=sheep_rom_direct_base;
 const auto reason0=power_off_reason;const bool int0=int_pin,dec0=dec_exception_pending,thrm0=thrm_exception_pending;
 auto sync0=std::move(gCtxSyncCallbacks);gCtxSyncCallbacks.clear();
 alignas(4096) std::array<uint8_t,16384> code{};alignas(4096) std::array<uint8_t,8192> ram{};
 alignas(32) std::array<uint8_t,4096*32> table_a{},table_b{},dtlb{};
 std::array<uint64_t,4096> original_words{},words{};std::array<uint8_t,4096> original_counters{},counters{},callbacks{};
 GenericContinuationFrame frame{ram.data(),nullptr,0,0,0,0,0};
 for(unsigned i=0;i<4096;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+4*i);original_words[i]=sheep_rom_dispatch_words[slot];original_counters[i]=sheep_jit_reject_filter[slot];}
 auto refresh=[](){nw_jit_layout temporary{};sheep_mmu_jit_layout(&temporary);};
 auto cleanup=[&](){
  sheep_rom_direct_reset();nw_jit_reset();for(unsigned i=0;i<4096;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+4*i);sheep_rom_dispatch_words[slot]=original_words[i];sheep_jit_reject_filter[slot]=original_counters[i];}
  std::memcpy(&ppc_state,&saved,sizeof(saved));pCurITLB1=i1;pCurITLB2=i2;pCurDTLB1=d1;pCurDTLB2=d2;CurITLBMode=im;CurDTLBMode=dm;tlb_size_mask=mask;is_601=old601;include_601=oldinclude;ppc_opcode_grabber=grabber;refresh();
  exec_flags=flags;ppc_next_instruction_address=next;g_icycles=clock;exec_timer=timer0;power_on=power0;sheep_jit_memory_bail=bail0;
  sheep_rom_only=only;sheep_rom_direct_enabled=direct;sheep_rom_host_begin=begin;sheep_rom_host_end=end;sheep_rom_direct_base=base;
  power_off_reason=reason0;int_pin=int0;dec_exception_pending=dec0;thrm_exception_pending=thrm0;gCtxSyncCallbacks=std::move(sync0);
 };
 unsigned passed=0,mappers=0,events=0,caps=0,observers=0;bool okay=false;
 auto require=[&](bool good,const char *why){if(!good){fprintf(stderr,"GENERIC CONTINUATION failure case=%u %s\n",passed+1,why);throw std::runtime_error("Q193 native continuation fixture failed");}};
 try{
  sheep_rom_direct_reset();nw_jit_reset();sheep_rom_only=sheep_rom_direct_enabled=true;
  sheep_rom_host_begin=reinterpret_cast<uintptr_t>(code.data());sheep_rom_host_end=sheep_rom_host_begin+12288;
  const int64_t callback_delta=int64_t(reinterpret_cast<uintptr_t>(&generic_continuation_callback))-int64_t(reinterpret_cast<uintptr_t>(&ppc_illegalop));
  require(!(callback_delta&3)&&callback_delta>=-(int64_t(1)<<30)&&callback_delta<(int64_t(1)<<30),"private callback packed range");
  constexpr uint32_t data_va=0x28000000u,nia0=0x31415920u;constexpr uint64_t clock0=123;
  nw_jit_layout layout{};layout.gpr=offsetof(SetPRS,gpr);layout.cr=offsetof(SetPRS,cr);layout.xer=offsetof(SetPRS,spr)+SPR::XER*4;layout.lr=offsetof(SetPRS,spr)+SPR::LR*4;layout.ctr=offsetof(SetPRS,spr)+SPR::CTR*4;layout.pc=offsetof(SetPRS,pc);layout.msr=offsetof(SetPRS,msr);layout.memory=ram.data();layout.leaf=1;layout.dtlb_slot=reinterpret_cast<uintptr_t>(&pCurDTLB1);layout.tlb_mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);layout.tlb_index_bits=12;layout.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);
  auto trial=[&](unsigned group,unsigned count_kind,unsigned wrap,unsigned variant){
   std::memcpy(&ppc_state,&saved,sizeof(saved));ppc_state.spr[SPR::PVR]=PPC_VER::MPC603;ppc_state.msr=MSR::IR;is_601=include_601=false;ppc_opcode_grabber=OpcodeGrabberNoFPU;
   pCurITLB1=reinterpret_cast<TLBEntry*>(table_a.data());pCurITLB2=i2;pCurDTLB1=reinterpret_cast<TLBEntry*>(dtlb.data());pCurDTLB2=d2;CurITLBMode=1;CurDTLBMode=0;tlb_size_mask=4095;
   const uint32_t page=wrap?0xfffff000u:0x10000000u;const unsigned actuals[]={1,2,3,4,32};const unsigned actual=actuals[count_kind];const unsigned planned=actual==32?32:4;
   auto blank=[](auto &table){table.fill(0);for(unsigned i=0;i<4096;i++){const uint32_t bad=0xffffffffu;std::memcpy(table.data()+i*32,&bad,4);}};
   blank(table_a);blank(table_b);blank(dtlb);
   auto map=[](auto &table,uint32_t guest,uint8_t *host,uint16_t attr,uint32_t index_mask){auto *entry=table.data()+size_t((guest>>12)&index_mask)*32;const uint32_t tag=guest&~4095u;const int64_t off=int64_t(reinterpret_cast<uintptr_t>(host))-int64_t(guest);std::memcpy(entry,&tag,4);std::memcpy(entry+4,&attr,2);std::memcpy(entry+8,&off,8);std::memcpy(entry+16,&off,8);};
   for(unsigned i=0;i<ram.size();i++)ram[i]=uint8_t(i*37u+passed*11u);map(dtlb,data_va,ram.data(),0x61,4095);
   code.fill(0);callbacks.fill(0);counters.fill(255);
   auto put=[&](unsigned off,uint32_t op,bool callback=false){require(off+4<=code.size()&&!(off&3),"source word extent");for(unsigned j=0;j<4;j++)code[off+j]=uint8_t(op>>(24-8*j));uint32_t desc=uint32_t(callback_delta)&0x7ffffffcu;if(!callback)require(sheep_encode_rom_handler(op,desc),"canonical scalar descriptor");words[off/4]=(uint64_t(op)<<32)|desc|1u;callbacks[off/4]=callback;};
   for(unsigned i=0;i<4096;i++)put(i*4,0x60000000u);
   auto addi=[](unsigned r,unsigned value){return (14u<<26)|(r<<21)|(r<<16)|(value&65535u);};
   auto branch=[&](unsigned off,uint32_t pc,uint32_t target){put(off,0x48000000u|((target-pc)&0x03fffffcu));};
   auto publish=[&](unsigned off,unsigned n,uint32_t pc){uint32_t ops[32];require(n&&n<=32&&(pc&4095u)+n*4<=4096,"native source page bounds");for(unsigned i=0;i<n;i++)ops[i]=uint32_t(code[off+4*i])<<24|uint32_t(code[off+4*i+1])<<16|uint32_t(code[off+4*i+2])<<8|code[off+4*i+3];auto fn=nw_jit_compile_mapped(ops,n,pc,&layout);const uintptr_t base=reinterpret_cast<uintptr_t>(nw_jit_native_base()),address=reinterpret_cast<uintptr_t>(fn);require(fn&&address>=base&&!((address-base)&3)&&address-base<(uintptr_t(1)<<27),"actual kind0 native body and code arena");words[off/4]=(uint64_t(pc)<<32)|(uint64_t((uint32_t((address-base)>>2)<<5)|(n-1))<<2)|3u;counters[off/4]=0;};
   auto body=[&](unsigned off,unsigned n,unsigned returned,uint32_t pc,uint32_t target){for(unsigned i=0;i<n;i++)put(off+4*i,i%3==2?0x2f981234u:addi(i%3?29:24,i+1));branch(off+4*(returned-1),pc+4*(returned-1),target);publish(off,n,pc);};
   unsigned start=4096-planned*4,target_off=4096,target_bias=0,allow_ram=1;uint32_t target_page=page+4096u,live_mask=4095;
   bool timer=false,power=true;uint32_t entry_flags=0;uint64_t deadline=clock0+planned;unsigned expected_retired=actual,expected_pending=0;bool expect_native=true;
   frame.action=0;frame.target=page+0x1000u;frame.next_table=reinterpret_cast<TLBEntry*>(table_b.data());frame.next_mask=0;
   if(group==0){
    if(variant==1){target_off=8192+64;target_bias=64;live_mask=5;}
    if(variant>=2&&variant<=4){target_off=12288+64+(variant==4);target_bias=64;allow_ram=variant!=3;}
    if(variant==7){target_off=4096+64;target_bias=64;live_mask=0;}
    if(variant==2){put(target_off,0x7c600124u,true);expected_retired=actual+1;expected_pending=1;} // RAM must ignore packed callback and execute real MTMSR.
    else if(variant<2||variant==7)words[target_off/4]&=~3ull;
    else expected_pending=1;
   }else if(group==1){
    words[target_off/4]&=~3ull;
    if(variant==0){deadline=clock0+planned-1;expected_retired=0;expect_native=false;}
    if(variant==1){timer=true;deadline=clock0+planned+1;expected_pending=1;}
    if(variant==2){power=false;deadline=clock0+planned+1;expected_pending=1;}
    if(variant==3)deadline=clock0+planned+1;
    if(variant==4){entry_flags=EXEF_BRANCH|EXEF_FETCH;expected_retired=0;expect_native=false;}
    if(variant==5){expected_retired=0;expect_native=false;}
   }else if(group==2){
    const unsigned prefix=variant==0?actual-1:variant==1?0:1;
    const unsigned native_start=start;start-=prefix*4;
    for(unsigned i=0;i<prefix;i++)put(start+4*i,addi(11,1));
    body(native_start,planned,actual,page+native_start,target_page+64);body(4096+64,planned,actual,target_page+64,page+native_start);
    target_off=4096+64;target_bias=64;deadline=variant==0?clock0+128+(planned-actual):clock0+4096;
    expected_retired=variant==0?127:variant==1?128:129;expected_pending=variant!=0;
   }else{
    target_off=4096+64;target_bias=64;deadline=clock0+4096;frame.action=variant;
    if(variant>=3&&variant<=5)put(target_off,0x7c600124u);
    else put(target_off,0x60000000u,true);
    expected_retired=actual+1;expected_pending=1;
    if(variant>=6){
     frame.next_mask=variant==6?0:5;frame.target=page+8192u+64;
     const uint32_t third=page+8192u+64;body(target_off+4,planned,actual,target_page+68,third);put(8192+64,0x60000000u,true);
     map(table_b,third,code.data()+8192+64,1,frame.next_mask);
     expected_retired=actual+1+actual+1;
    }
   }
   if(group!=2)body(start,planned,actual,page+start,target_page+target_bias);
   if(group==1&&variant==5)words[start/4]+=uint64_t(4)<<32;
   map(table_a,page,code.data(),1,live_mask);
   map(table_a,target_page+target_bias,code.data()+target_off,group==0&&variant==6?0:1,live_mask);
   if(group==0&&variant==5){auto *at=table_a.data()+size_t((target_page>>12)&live_mask)*32;const uint32_t bad=target_page^4096u;std::memcpy(at,&bad,4);}
   const uint32_t root=page+start;
   SetPRS initial;std::memcpy(&initial,&ppc_state,sizeof(initial));initial.pc=root;
   for(unsigned i=0;i<32;i++)initial.gpr[i]=0xa5310000u^(passed*0x10203u+i*0x123457u);
   initial.gpr[3]=0;initial.gpr[5]=data_va+64;initial.gpr[24]=0x831579bdu;initial.gpr[29]=0x91234567u;initial.cr=0xa65a9187u;
   initial.spr[SPR::XER]=0xe0001234u;initial.spr[SPR::LR]=page+512;initial.spr[SPR::CTR]=7;
   if(group==3&&variant==4)initial.gpr[3]=MSR::IR|MSR::PR;
   if(group==3&&variant==5)initial.gpr[3]=MSR::IR|MSR::FP;
   const uintptr_t fp=reinterpret_cast<uintptr_t>(&frame);initial.gpr[30]=uint32_t(fp);initial.gpr[31]=uint32_t(fp>>32);
   SheepLoopContext ctx{};ctx.cpu=reinterpret_cast<uintptr_t>(&ppc_state);ctx.words=reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words);ctx.counters=reinterpret_cast<uintptr_t>(sheep_jit_reject_filter);ctx.native_base=reinterpret_cast<uintptr_t>(nw_jit_native_base());ctx.illegal=reinterpret_cast<uintptr_t>(&ppc_illegalop);ctx.fpu_disabled=reinterpret_cast<uintptr_t>(sheep_rom_disabled_fpu_handler);ctx.timer=reinterpret_cast<uintptr_t>(&exec_timer);ctx.clock=reinterpret_cast<uintptr_t>(&g_icycles);ctx.flags=reinterpret_cast<uintptr_t>(&exec_flags);ctx.next=reinterpret_cast<uintptr_t>(&ppc_next_instruction_address);ctx.max_cycles=deadline;ctx.host=code.data()+start;ctx.page_start=page;ctx.page_end=page+4095u;ctx.pc_offset=offsetof(SetPRS,pc);ctx.cr_offset=offsetof(SetPRS,cr);ctx.lr_offset=offsetof(SetPRS,spr)+SPR::LR*4;ctx.disabled=1;ctx.reject=reinterpret_cast<uintptr_t>(&generic_continuation_reject);ctx.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);ctx.power=reinterpret_cast<uintptr_t>(&power_on);ctx.itlb=reinterpret_cast<uintptr_t>(&pCurITLB1);ctx.mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);ctx.rom_begin=sheep_rom_host_begin;ctx.rom_end=sheep_rom_host_end;ctx.opcodes=reinterpret_cast<uintptr_t>(OpcodeGrabberNoFPU);ctx.allow_ram=allow_ram;
   const auto before_code=code;const auto before_ram=ram;const auto before_a=table_a,before_b=table_b,before_d=dtlb;
   auto reset=[&](){std::memcpy(&ppc_state,&initial,sizeof(initial));code=before_code;ram=before_ram;table_a=before_a;table_b=before_b;dtlb=before_d;
    pCurITLB1=reinterpret_cast<TLBEntry*>(table_a.data());pCurITLB2=i2;pCurDTLB1=reinterpret_cast<TLBEntry*>(dtlb.data());pCurDTLB2=d2;CurITLBMode=1;CurDTLBMode=0;tlb_size_mask=live_mask;ppc_opcode_grabber=OpcodeGrabberNoFPU;is_601=include_601=false;
    gCtxSyncCallbacks.clear();int_pin=dec_exception_pending=thrm_exception_pending=false;power_off_reason=reason0;exec_flags=entry_flags;ppc_next_instruction_address=nia0;g_icycles=clock0;exec_timer=timer;power_on=power;sheep_jit_memory_bail=false;frame.calls=frame.rejects=0;std::memset(frame.observed,0,sizeof(frame.observed));
    for(unsigned i=0;i<4096;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+4*i);sheep_rom_dispatch_words[slot]=words[i];sheep_jit_reject_filter[slot]=counters[i];}
   };
   struct Result {SetPRS cpu;SheepLoopContext ctx;std::array<uint8_t,8192> ram;std::array<uint64_t,4096> words;std::array<uint8_t,4096> counters;uint64_t result,clock;uint32_t flags,next;unsigned calls,rejects;uint32_t observed[2][3];bool timer,power,bail,metadata;TLBEntry *i1,*i2,*d1,*d2;uint32_t mask,im,dm;PPCOpcode *decoder;};
   auto run=[&](unsigned(*fn)(SheepLoopContext*)){reset();Result r{};std::memcpy(&r.ctx,&ctx,sizeof(ctx));r.result=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(fn),reinterpret_cast<nw_jit_cpu*>(&r.ctx));std::memcpy(&r.cpu,&ppc_state,sizeof(r.cpu));r.ram=ram;r.clock=g_icycles;r.flags=exec_flags;r.next=ppc_next_instruction_address;r.timer=exec_timer;r.power=power_on;r.bail=sheep_jit_memory_bail;r.calls=frame.calls;r.rejects=frame.rejects;std::memcpy(r.observed,frame.observed,sizeof(r.observed));r.i1=pCurITLB1;r.i2=pCurITLB2;r.d1=pCurDTLB1;r.d2=pCurDTLB2;r.im=CurITLBMode;r.dm=CurDTLBMode;r.mask=tlb_size_mask;r.decoder=ppc_opcode_grabber;r.metadata=code==before_code&&table_a==before_a&&table_b==before_b&&dtlb==before_d;for(unsigned i=0;i<4096;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+4*i);r.words[i]=sheep_rom_dispatch_words[slot];r.counters[i]=sheep_jit_reject_filter[slot];}return r;};
   const auto reference=run(&sheep_generic_continuation_reference_a64),actual_result=run(&sheep_cpu_loop_a64);
   auto equal=[](const Result &a,const Result &b){return a.result==b.result&&!(a.result>>32)&&!std::memcmp(&a.cpu,&b.cpu,sizeof(a.cpu))&&!std::memcmp(&a.ctx,&b.ctx,sizeof(a.ctx))&&a.ram==b.ram&&a.words==b.words&&a.counters==b.counters&&a.clock==b.clock&&a.flags==b.flags&&a.next==b.next&&a.timer==b.timer&&a.power==b.power&&a.bail==b.bail&&a.calls==b.calls&&a.rejects==b.rejects&&!std::memcmp(a.observed,b.observed,sizeof(a.observed))&&a.i1==b.i1&&a.i2==b.i2&&a.d1==b.d1&&a.d2==b.d2&&a.im==b.im&&a.dm==b.dm&&a.mask==b.mask&&a.decoder==b.decoder&&a.metadata&&b.metadata;};
   require(equal(actual_result,reference),"frozen288/new333 kind0 exact CPU/context/RAM/rawflags/NIA/clock/MMU/decoder/ABI");
   require(reference.ctx.retired==expected_retired&&uint32_t(reference.result)==expected_pending&&bool(reference.ctx.native_used)==expect_native&&!reference.bail&&!reference.rejects,"independent actual count/pending/admission/cap contract");
   // Replay the independently derived retirement count through canonical
   // handlers. Preserve the already-known source affine offset within its page;
   // only a real page transition consults the then-live primary table/mask.
   reset();uint32_t source_pc=root;uint8_t *source_host=code.data()+start;bool replay_ok=true;
   for(unsigned i=0;i<expected_retired;i++){
    source_pc=ppc_state.pc;
    const uintptr_t host=reinterpret_cast<uintptr_t>(source_host),low=reinterpret_cast<uintptr_t>(code.data());
    if(host<low||host+4>low+code.size()){replay_ok=false;break;}
    const unsigned off=unsigned(host-low);const uint32_t op=uint32_t(source_host[0])<<24|uint32_t(source_host[1])<<16|uint32_t(source_host[2])<<8|source_host[3];
    const bool rom=host>=sheep_rom_host_begin&&host<sheep_rom_host_end;
    if(rom&&callbacks[off/4])generic_continuation_callback(op);else ppc_main_opcode(ppc_opcode_grabber,op);
    if(i+1==expected_retired&&expected_pending)break;
    const uint32_t target=exec_flags&EXEF_BRANCH?ppc_next_instruction_address:uint32_t(source_pc+4);
    ppc_state.pc=target;exec_flags=0;++g_icycles;
    if((source_pc&~4095u)==(target&~4095u))source_host+=int32_t(target-source_pc);
    else{
     const auto *entry=reinterpret_cast<const uint8_t*>(pCurITLB1)+size_t((target>>12)&tlb_size_mask)*32;uint32_t tag;uint16_t attrs;int64_t host_off;
     std::memcpy(&tag,entry,4);std::memcpy(&attrs,entry+4,2);std::memcpy(&host_off,entry+8,8);
     if(tag!=(target&~4095u)||!(attrs&1)){replay_ok=false;break;}
     source_host=reinterpret_cast<uint8_t*>(host_off+target);
    }
    if(i+1==expected_retired)source_pc=target;
   }
   const auto &a=actual_result;
   require(replay_ok&&!std::memcmp(&a.cpu,&ppc_state,sizeof(ppc_state))&&a.ram==ram&&a.clock==g_icycles&&a.flags==exec_flags&&a.next==ppc_next_instruction_address&&a.timer==exec_timer&&a.power==power_on&&a.calls==frame.calls&&!std::memcmp(a.observed,frame.observed,sizeof(a.observed))&&a.ctx.host==source_host&&a.ctx.page_start==(source_pc&~4095u)&&a.ctx.page_end==uint32_t((source_pc&~4095u)+4095u),"independent canonical CPU/RAM/clock/rawflags/NIA/current-value callbacks/source replay");
   if(group==0&&variant==2)require(a.ctx.source_ram==1&&!a.calls&&(a.flags&EXEF_FETCH),"aligned enabled RAM executes real MTMSR, ignoring packed ROM callback");
   if(group==3&&variant>=6)require(a.calls==2&&a.i1==reinterpret_cast<TLBEntry*>(table_b.data())&&a.mask==frame.next_mask,"post-callback fresh primary and mask used by next native cross-page edge");
   if(group==3&&variant>=3&&variant<=5)require(a.flags&(variant==5?EXEF_OPC_DECODER:EXEF_FETCH),"actual IR/PR/FP handler preserves pending fetch/decoder barrier");
   ++passed;if(group==0)++mappers;else if(group==1)++events;else if(group==2)++caps;else ++observers;
  };
  for(unsigned kind=0;kind<5;kind++)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned variant=0;variant<8;variant++)trial(0,kind,wrap,variant);
  for(unsigned kind=0;kind<5;kind++)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned variant=0;variant<6;variant++)trial(1,kind,wrap,variant);
  const unsigned cap_kinds[]={1,3,4};for(unsigned kind:cap_kinds)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned variant=0;variant<3;variant++)trial(2,kind,wrap,variant);
  const unsigned observer_kinds[]={1,3};for(unsigned kind:observer_kinds)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned variant=0;variant<8;variant++)trial(3,kind,wrap,variant);
  require(passed==190&&mappers==80&&events==60&&caps==18&&observers==32,"fixed mapper/event/cap/observer census");okay=true;
 }catch(const std::exception &e){fprintf(stderr,"GENERIC CONTINUATION exception: %s\n",e.what());}catch(...){fprintf(stderr,"GENERIC CONTINUATION exception: unknown\n");}
 cleanup();if(okay)fprintf(stderr,"GENERIC CONTINUATION SELFTEST %u mapper=%u events_budget=%u cap=%u observers=%u frozen288/new333/kind0/rawCPU/context/RAM/NIA/flags/clock/ABI/canonical cases passed\n",passed,mappers,events,caps,observers);return okay?1:0;
#else
 return 0;
#endif
}
