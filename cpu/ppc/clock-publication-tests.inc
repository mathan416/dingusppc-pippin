// SPDX-License-Identifier: GPL-3.0-or-later
// Exact Q201361 reference; only global-clock stores differ in the candidate.
#include <signal.h>
#if defined(__aarch64__)
extern "C" unsigned sheep_clock_publication_reference_a64(SheepLoopContext*);
asm(R"(
.text
.p2align 4
.global sheep_clock_publication_reference_a64
.type sheep_clock_publication_reference_a64,%function
sheep_clock_publication_reference_a64:
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
 cbnz w9,.Lcp_ref_cold
.Lcp_ref_fetch:
 tbnz x20,#0,.Lcp_ref_fetch_ram
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 // Decode only the descriptor tag bits; preserve opcode, metadata and index.
 tbz w25,#1,.Lcp_ref_low_tag
 tbnz w25,#0,.Lcp_ref_native
 // Match the canonical retry counter, including its final blocked iteration.
 ldr x11,[x28,#16]
 ldrb w9,[x11,x10]
 cbz w9,.Lcp_ref_retry_zero
 cmp w9,#255
 b.eq .Lcp_ref_scalar
 sub w9,w9,#1
 strb w9,[x11,x10]
 cbnz w9,.Lcp_ref_scalar
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lcp_ref_scalar
.Lcp_ref_fetch_ram:
 // Writable code is fetched anew for every instruction. No native RAM cache.
 ldur w0,[x20,#-1]
 rev w0,w0
 lsl x25,x0,#32
 b .Lcp_ref_scalar
.Lcp_ref_retry_zero:
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lcp_ref_cold
.Lcp_ref_native:
 lsr x9,x25,#32
 cmp w9,w24
 b.ne .Lcp_ref_cold
 ubfx w26,w25,#2,#5
 add w26,w26,#1
 sub x10,x23,x22
 cmp x26,x10
 b.hi .Lcp_ref_cold
 lsr w9,w25,#7
 ldr x10,[x28,#24]
 add x9,x10,w9,uxtw #2
 ldr x10,[x28,#128]
 strb wzr,[x10]
 mov w10,#1
 str w10,[x28,#148]
 mov x0,x19
 blr x9
 cmp w0,w26
 b.hi .Lcp_ref_trap
 mov w26,w0
 ldr w25,[x19,#256]
 cmp w26,#4
 mov w14,#1024
 b.hs .Lcp_ref_native_fast
 // Successful short branches need no rejection or interpreter round-trip.
 // Zero/failed-guard prefixes retain their original slow-path backoff.
 cbz w26,.Lcp_ref_native_short_guard
 ldr x9,[x28,#128]
 ldrb w9,[x9]
 mov w14,#128
 cbz w9,.Lcp_ref_native_fast
.Lcp_ref_native_short_guard:
 mov x0,x20
 mov w1,w24
 ldr x9,[x28,#120]
 blr x9
 cbnz w26,.Lcp_ref_native_prefix
 str w24,[x19,#256]
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 b .Lcp_ref_scalar
.Lcp_ref_native_fast:
 // All indices address the private, allocated decoder table, even when a
 // guest branch will later miss instruction translation. Prefetch has no
 // architectural side effects; MMU guards still control the actual target.
 sub w9,w25,w24
 add x12,x20,w9,sxtw
 ubfx x9,x12,#2,#20
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
.global sheep_clock_publication_reference_guard_native
.hidden sheep_clock_publication_reference_guard_native
sheep_clock_publication_reference_guard_native:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lcp_ref_native_fast_old
 // Preserve the canonical last branch target even when consuming the branch
 // locally. It remains observable at later event and exception boundaries.
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lcp_ref_native_fast_sequential
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
.Lcp_ref_native_fast_sequential:
 add w27,w27,w26
 str x22,[x29]
 cmp x22,x23
 b.hs .Lcp_ref_native_fast_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lcp_ref_native_fast_pending
 cmp w27,w14
 b.hs .Lcp_ref_native_fast_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lcp_ref_native_fast_pending
 // No device/event polling is removed: deadlines, external timer and power
 // are checked after every bounded block, as in the original path.
 mov x20,x12
 mov w24,w25
 add x22,x22,#1
 str x22,[x29]
 // The native block already stored this exact target into CPU.pc.
 b .Lcp_ref_fetch
.Lcp_ref_native_fast_old:
 // Cross-page native exits retain the exact128-instruction span cap. The
 // helper-free leaf cannot change exec_flags; avoid publishing a synthetic
 // BRANCH only to reload/clear it after the same polls and live mapper.
 // NIA remains observable even when the translated target is consumed here.
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 str x22,[x29]
 add w27,w27,w26
 cmp x22,x23
 b.hs .Lcp_ref_native_map_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lcp_ref_native_map_pending
 cmp w27,#128
 b.hs .Lcp_ref_native_map_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lcp_ref_native_map_pending
 // Read the current primary table and mask on every cross-page edge. Keep
 // the old tag/PAGE_MEM/host-offset/ROM-or-RAM checks, with no mapping cache.
 adrp x9,pCurITLB1
 ldr x9,[x9,:lo12:pCurITLB1]
 adrp x11,tlb_size_mask
 ldr w11,[x11,:lo12:tlb_size_mask]
 lsr w12,w25,#12
 and w11,w11,w12
 add x9,x9,x11,lsl #5
 ldp x11,x9,[x9]
 and w12,w25,#0xfffff000
 cmp w11,w12
 b.ne .Lcp_ref_native_map_pending
 tbz x11,#32,.Lcp_ref_native_map_pending
 add x9,x9,w25,uxtw
 ldr x11,[x28,#168]
 cmp x9,x11
 b.lo .Lcp_ref_native_map_ram
 ldr x11,[x28,#176]
 cmp x9,x11
 b.hs .Lcp_ref_native_map_ram
 mov x20,x9
 str wzr,[x28,#192]
.Lcp_ref_native_mapped:
 mov w24,w25
 str w12,[x28,#96]
 add w11,w12,#4095
 str w11,[x28,#100]
 // CPU.pc already contains this target. Preserve the old post-step clock
 // and mapped-source update without rolling CPU.pc back and forward.
 add x22,x22,#1
 str x22,[x29]
 b .Lcp_ref_fetch
.Lcp_ref_native_map_ram:
 ldr w11,[x28,#196]
 cbz w11,.Lcp_ref_native_map_pending
 tst x9,#3
 b.ne .Lcp_ref_native_map_pending
 orr x20,x9,#1
 mov w11,#1
 str w11,[x28,#192]
 b .Lcp_ref_native_mapped
.Lcp_ref_native_map_pending:
 // Every unsuccessful handoff exposes the old last-source/BRANCH state,
 // including a sequential cross-page target or32-bit guest-PC wrap.
 str w24,[x19,#256]
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lcp_ref_pending
.Lcp_ref_native_fast_pending:
 str w24,[x19,#256]
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lcp_ref_pending
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lcp_ref_pending
.Lcp_ref_native_prefix:
 sub w9,w26,#1
 add x20,x20,w9,uxtw #2
 add w24,w24,w9,lsl #2
 add x22,x22,x9
 str w24,[x19,#256]
.Lcp_ref_native_prefix_done:
 add w9,w24,#4
 cmp w25,w9
 b.ne .Lcp_ref_native_branch
 // Target is already proven to be source+4. Only target bits2..11==0 cross a
 // page, including the 32-bit address wrap from fffffffc to zero.
.global sheep_clock_publication_reference_guard_next
.hidden sheep_clock_publication_reference_guard_next
sheep_clock_publication_reference_guard_next:
 tst w25,#0xffc
 b.ne .Lcp_ref_after
.Lcp_ref_native_branch:
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
 b .Lcp_ref_after
.Lcp_ref_scalar_branch:
 // Relative B/BL and the already-qualified taken BC contain no callbacks.
 // NIA is observable even for an explicit branch to source+4. Keep that
 // unconditional store, but materialize BRANCH only on a pending handoff.
 // Match the leaf native path's target-PC custody: any pending return below
 // restores the last source before C++ performs its post-instruction step.
 str w25,[x19,#256]
 sub w9,w25,w24
 add x12,x20,w9,sxtw
 ubfx x9,x12,#2,#20
 prfm pldl1keep,[x21,x9,lsl #3]
 eor w9,w25,w24
 tst w9,#0xfffff000
 // W26 is exactly1; no native prefix PC/clock adjustment is necessary.
 // The existing live mapper and every failed-cross-page boundary are reused.
 b.ne .Lcp_ref_native_fast_old
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 str x22,[x29]
 add w27,w27,w26
 cmp x22,x23
 b.hs .Lcp_ref_scalar_branch_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lcp_ref_scalar_branch_pending
 cmp w27,#128
 b.hs .Lcp_ref_scalar_branch_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lcp_ref_scalar_branch_pending
 mov x20,x12
 mov w24,w25
 add x22,x22,#1
 str x22,[x29]
 b .Lcp_ref_fetch
.Lcp_ref_scalar_branch_pending:
 str w24,[x19,#256]
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lcp_ref_pending
.Lcp_ref_low_tag:
 // Scalar tag1 falls through; native/retry routes keep their old branches.
 tbz w25,#0,.Lcp_ref_cold
.Lcp_ref_scalar:
 mov w26,#1
 lsr x0,x25,#32
 lsr w9,w0,#26
 cmp w9,#18
 b.eq .Lcp_ref_b
 cmp w9,#16
 b.eq .Lcp_ref_bc
.Lcp_ref_handler:
 tbnz x20,#0,.Lcp_ref_ram_handler
 // The packed descriptor stores a signed handler offset and FPU flag.
 ldr w9,[x28,#116]
 cbz w9,.Lcp_ref_normal_handler
 tbz w25,#31,.Lcp_ref_normal_handler
 ldr x9,[x28,#40]
 b .Lcp_ref_call_handler
.Lcp_ref_normal_handler:
 lsl w9,w25,#1
 asr w9,w9,#3
 ldr x10,[x28,#32]
 add x9,x10,w9,sxtw #2
 b .Lcp_ref_call_handler
.Lcp_ref_ram_handler:
 // Exactly the canonical ppc_main_opcode index and selected FPU table.
 lsr w9,w0,#15
 and w9,w9,#0x1f800
 and w10,w0,#0x7ff
 orr w9,w9,w10
 ldr x10,[x28,#184]
 ldr x9,[x10,w9,uxtw #3]
 b .Lcp_ref_call_handler
.Lcp_ref_call_handler:
 blr x9
 ldr x22,[x29]
 b .Lcp_ref_after
.Lcp_ref_b:
 tbnz w0,#1,.Lcp_ref_handler
 tbz w0,#0,.Lcp_ref_b_target
 add w9,w24,#4
 ldr w10,[x28,#112]
 str w9,[x19,x10]
.Lcp_ref_b_target:
 sbfx w9,w0,#0,#26
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lcp_ref_scalar_branch
.Lcp_ref_bc:
 tst w0,#3
 b.ne .Lcp_ref_handler
 ubfx w9,w0,#21,#5
 and w10,w9,#30
 cmp w10,#4
 b.eq .Lcp_ref_bc_cr
 cmp w10,#12
 b.ne .Lcp_ref_handler
.Lcp_ref_bc_cr:
 ldr w10,[x28,#108]
 ldr w10,[x19,x10]
 ubfx w11,w0,#16,#5
 mov w12,#31
 sub w11,w12,w11
 lsr w10,w10,w11
 and w10,w10,#1
 ubfx w9,w9,#3,#1
 cmp w9,w10
 b.ne .Lcp_ref_after
 sxth w9,w0
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lcp_ref_scalar_branch
.Lcp_ref_after:
 // Keep the final pre-increment value for ppc_exec's existing event check.
 str x22,[x29]
 add w27,w27,w26
 cmp x22,x23
 b.hs .Lcp_ref_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lcp_ref_pending
 cmp w27,#128
 b.hs .Lcp_ref_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lcp_ref_pending
 .word 0xb84003a9 | (((exec_flags-g_icycles)&0x1ff)<<12)
 cbz w9,.Lcp_ref_advance
 cmp w9,#1
 b.ne .Lcp_ref_pending
 ldr x10,[x28,#64]
 .word 0xb84003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 // Scalar callbacks cannot change the loop's mapped source W24. Keep
 // arbitrary targets subject to the exact existing primary-ITLB mapper.
.global sheep_clock_publication_reference_guard_branch
.hidden sheep_clock_publication_reference_guard_branch
sheep_clock_publication_reference_guard_branch:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lcp_ref_map_target
 // Only a plain same-page branch can be consumed inside the fast loop.
 sub w9,w25,w24
 sxtw x9,w9
 add x20,x20,x9
 mov w24,w25
 str wzr,[x10]
 b .Lcp_ref_continue
.Lcp_ref_advance:
.global sheep_clock_publication_reference_guard_advance
.hidden sheep_clock_publication_reference_guard_advance
sheep_clock_publication_reference_guard_advance:
 add w10,w24,#4
 // Source+4 leaves its page iff target bits2..11 are zero, even unaligned.
 tst w10,#0xffc
 b.ne .Lcp_ref_advance_same
 mov w25,w10
 ldr x10,[x28,#64]
 b .Lcp_ref_map_target
.Lcp_ref_advance_same:
 add w24,w24,#4
 add x20,x20,#4
.Lcp_ref_continue:
 add x22,x22,#1
 str x22,[x29]
 str w24,[x19,#256]
 b .Lcp_ref_fetch
.Lcp_ref_map_target:
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
 b.ne .Lcp_ref_pending
 tbz x11,#32,.Lcp_ref_pending
 add x9,x9,w25,uxtw
 ldr x11,[x28,#168]
 cmp x9,x11
 b.lo .Lcp_ref_target_ram
 ldr x11,[x28,#176]
 cmp x9,x11
 b.hs .Lcp_ref_target_ram
 mov x20,x9
 str wzr,[x28,#192]
 b .Lcp_ref_mapped
.Lcp_ref_target_ram:
 ldr w11,[x28,#196]
 cbz w11,.Lcp_ref_pending
 tst x9,#3
 b.ne .Lcp_ref_pending
 orr x20,x9,#1
 mov w11,#1
 str w11,[x28,#192]
.Lcp_ref_mapped:
 mov w24,w25
 str w12,[x28,#96]
 add w11,w12,#4095
 str w11,[x28,#100]
 str wzr,[x10]
 b .Lcp_ref_continue
.Lcp_ref_pending:
 mov w0,#1
 b .Lcp_ref_return
.Lcp_ref_cold:
 mov w0,#0
.Lcp_ref_return:
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
.Lcp_ref_trap:
 brk #0
.cfi_endproc
.size sheep_clock_publication_reference_a64,.-sheep_clock_publication_reference_a64
)");
#endif
#if defined(__aarch64__)
extern void sheep_mmu_test_fast_page(uint8_t*,uint8_t*,uint32_t,uint16_t,bool,unsigned);
struct ClockPublicationFrame {
 SetPRS *cpu; nw_jit_fn body; uint64_t *clock;volatile bool *timer;bool *power;uint32_t *flags,*next;
 uint32_t first_pc,fail_pc;unsigned action,handler_calls,native_calls,reject_calls,seen_count;
 int64_t handler_delta;uint64_t native_abi;
 struct Seen {uint64_t clock;uint32_t kind,pc,next,flags,gpr6,lr,cr,pad;} seen[8]{};
 struct sigaction previous_segv{};
 volatile sig_atomic_t signal_calls=0,signal_clock_low=0,signal_clock_high=0;
};
static ClockPublicationFrame &clock_publication_frame(const SetPRS *cpu){
 return *reinterpret_cast<ClockPublicationFrame*>(uintptr_t(cpu->gpr[30])|(uintptr_t(cpu->gpr[31])<<32));
}
static void clock_publication_seen(ClockPublicationFrame &f,unsigned kind){
 const unsigned i=f.seen_count++;if(i<8)f.seen[i]={*f.clock,kind,f.cpu->pc,*f.next,*f.flags,f.cpu->gpr[6],f.cpu->spr[SPR::LR],f.cpu->cr};
}
[[gnu::noinline]] static unsigned clock_publication_native(nw_jit_cpu *raw){
 auto &f=clock_publication_frame(reinterpret_cast<SetPRS*>(raw));clock_publication_seen(f,1);++f.native_calls;
 if(f.action==4)*f.timer=true;if(f.action==5)*f.power=false;
 const uint64_t result=sheep_jit_abi_check(f.body,raw);f.native_abi|=result>>32;return unsigned(result);
}
[[gnu::noinline]] static void clock_publication_handler(uint32_t op){
 auto &f=clock_publication_frame(&ppc_state);clock_publication_seen(f,2);++f.handler_calls;
 // A failed native load is deliberately handed to a private stopping handler;
 // it is a dispatcher/clock witness, not a substitute architectural DSI oracle.
 if((op>>26)==32){*f.flags=EXEF_EXCEPTION|EXEF_BRANCH;*f.next=0x00000c00u;*f.timer=true;f.cpu->gpr[6]^=0x1234u;}
 else{
  ppc_main_opcode(ppc_opcode_grabber,op);
  if(f.cpu->pc==f.first_pc){if(f.action==1)*f.clock+=7;}
  else *f.timer=true;
 }
 generic_continuation_clobber();
}
[[gnu::noinline]] static void clock_publication_reject(const uint8_t *code,uint32_t){
 auto &f=clock_publication_frame(&ppc_state);clock_publication_seen(f,3);++f.reject_calls;
 const uint32_t op=uint32_t(code[0])<<24|uint32_t(code[1])<<16|uint32_t(code[2])<<8|code[3];
 const auto slot=sheep_jit_filter_slot_rom(code);
 sheep_rom_dispatch_words[slot]=(uint64_t(op)<<32)|(uint32_t(f.handler_delta)&0x7ffffffcu)|2u;sheep_jit_reject_filter[slot]=32;
 generic_continuation_clobber();
}
static void clock_publication_signal(int signal,siginfo_t *info,void *context){
 auto &f=clock_publication_frame(&ppc_state);const uint64_t clock=*f.clock;
 f.signal_clock_low=sig_atomic_t(uint32_t(clock));f.signal_clock_high=sig_atomic_t(uint32_t(clock>>32));++f.signal_calls;
 f.previous_segv.sa_sigaction(signal,info,context);
}
#endif
int sheep_clock_publication_selftest(){
#if defined(__aarch64__) && !defined(LOG_INSTRUCTIONS) && !defined(CPU_PROFILING)
 SetPRS saved;std::memcpy(&saved,&ppc_state,sizeof(saved));
 const auto flags0=exec_flags,next0=ppc_next_instruction_address;const uint64_t clock0=g_icycles;
 const bool timer0=exec_timer,power0=power_on,bail0=sheep_jit_memory_bail,only0=sheep_rom_only,direct0=sheep_rom_direct_enabled;
 const bool model0=is_601,extras0=include_601;auto *const decoder0=ppc_opcode_grabber;
 auto *const i10=pCurITLB1,*const i20=pCurITLB2,*const d10=pCurDTLB1,*const d20=pCurDTLB2;const auto im0=CurITLBMode,dm0=CurDTLBMode;const auto mask0=tlb_size_mask;
 const auto begin0=sheep_rom_host_begin,end0=sheep_rom_host_end,base0=sheep_rom_direct_base;
 auto callbacks0=std::move(gCtxSyncCallbacks);gCtxSyncCallbacks.clear();
 alignas(4096) std::array<uint8_t,8192> code{},ram{};
 alignas(32) std::array<uint8_t,4096*32> itlb{},dtlb{};
 std::array<uint64_t,2048> words{},original_words{};std::array<uint8_t,2048> filters{},original_filters{};
 alignas(8) std::array<uint8_t,1024> private_scheduler{};
 SetPRS foreign{};ClockPublicationFrame frame{};bool fast_bound=false,signal_bound=false;uint8_t *physical_read=nullptr,*physical_write=nullptr;
 unsigned passed=0,observers=0,private_cases=0,fault_cases=0;
 auto need=[&](bool ok,const char *why){if(!ok){fprintf(stderr,"CLOCK PUBLICATION failure case=%u %s\n",passed+1,why);throw std::runtime_error("Q205 clock publication selftest failed");}};
 for(unsigned i=0;i<2048;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+i*4);original_words[i]=sheep_rom_dispatch_words[slot];original_filters[i]=sheep_jit_reject_filter[slot];}
 auto unbind=[&](){if(signal_bound){sigaction(SIGSEGV,&frame.previous_segv,nullptr);signal_bound=false;}if(fast_bound){sheep_mmu_test_fast_page(nullptr,nullptr,0,0,false,0);fast_bound=false;}};
 auto refresh=[](){nw_jit_layout layout{};sheep_mmu_jit_layout(&layout);};
 auto cleanup=[&](){
  unbind();sheep_rom_direct_reset();nw_jit_reset();for(unsigned i=0;i<2048;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+i*4);sheep_rom_dispatch_words[slot]=original_words[i];sheep_jit_reject_filter[slot]=original_filters[i];}
  bool released=true;if(physical_read){released=pippin_fastmem_release(physical_read)&&released;physical_read=nullptr;}if(physical_write){released=pippin_fastmem_release(physical_write)&&released;physical_write=nullptr;}
  std::memcpy(&ppc_state,&saved,sizeof(saved));pCurITLB1=i10;pCurITLB2=i20;pCurDTLB1=d10;pCurDTLB2=d20;CurITLBMode=im0;CurDTLBMode=dm0;tlb_size_mask=mask0;is_601=model0;include_601=extras0;ppc_opcode_grabber=decoder0;refresh();
  exec_flags=flags0;ppc_next_instruction_address=next0;g_icycles=clock0;exec_timer=timer0;power_on=power0;sheep_jit_memory_bail=bail0;sheep_rom_only=only0;sheep_rom_direct_enabled=direct0;sheep_rom_host_begin=begin0;sheep_rom_host_end=end0;sheep_rom_direct_base=base0;gCtxSyncCallbacks=std::move(callbacks0);
  if(!released)throw std::runtime_error("Q205 physical alias release failed");
 };
 try{
  sheep_rom_direct_reset();nw_jit_reset();sheep_rom_only=sheep_rom_direct_enabled=true;sheep_rom_host_begin=reinterpret_cast<uintptr_t>(code.data());sheep_rom_host_end=sheep_rom_host_begin+code.size();
  frame.handler_delta=int64_t(reinterpret_cast<uintptr_t>(&clock_publication_handler))-int64_t(reinterpret_cast<uintptr_t>(&ppc_illegalop));need(!(frame.handler_delta&3)&&frame.handler_delta>=-(int64_t(1)<<30)&&frame.handler_delta<(int64_t(1)<<30),"private observer handler packed range");
  auto blank=[](auto &table){table.fill(0);for(unsigned i=0;i<4096;i++){const uint32_t tag=0xffffffffu;std::memcpy(table.data()+i*32,&tag,4);}};
  auto map=[](auto &table,uint32_t pc,uint8_t *host,uint16_t flags){auto *at=table.data()+size_t((pc>>12)&4095u)*32;const uint32_t tag=pc&~4095u;const int64_t offset=int64_t(reinterpret_cast<uintptr_t>(host))-pc;std::memcpy(at,&tag,4);std::memcpy(at+4,&flags,2);std::memcpy(at+8,&offset,8);std::memcpy(at+16,&offset,8);};
  auto put=[&](unsigned offset,uint32_t op,bool observer=false){need(offset+4<=code.size(),"private source bounds");for(unsigned i=0;i<4;i++)code[offset+i]=uint8_t(op>>(24-i*8));uint32_t packed=uint32_t(frame.handler_delta)&0x7ffffffcu;if(!observer)need(sheep_encode_rom_handler(op,packed),"canonical descriptor");words[offset/4]=(uint64_t(op)<<32)|packed|1u;};
  auto trial=[&](unsigned group,unsigned kind,unsigned variant){
   unbind();nw_jit_reset();code.fill(0);filters.fill(255);blank(itlb);blank(dtlb);for(unsigned i=0;i<2048;i++)put(i*4,0x60000000u);for(unsigned i=0;i<ram.size();i++)ram[i]=uint8_t(i*37+passed*11);
   const unsigned actuals[]={0,1,2,3,4,32};unsigned actual=actuals[kind],planned=actual==32?32:4,prefix=3;
   constexpr uint32_t page=0x10000000u,data_va=0x28000000u;constexpr uint64_t initial_clock=0x10000007bu;
   const unsigned start=128;unsigned first=start+prefix*4,body=first+4,target=4096+64;uint32_t root=page+start;bool foreign_cpu=false,private_clock=false,guarded=actual==0,initial_timer=false,initial_power=true;uint32_t entry_flags=0;uint64_t deadline=initial_clock+4096;
   frame.action=group==0?variant:0;
   if(group==0&&variant==7){prefix=actual==0?128:actual==1?127:actual==2?126:actual==3?125:actual==4?124:96;first=start+prefix*4;body=first+4;}
   if(group==0&&variant==6){guarded=true;if(actual==32)actual=31;else if(actual==4)planned=5;}
   if(group==0&&variant==3)entry_flags=EXEF_BRANCH|EXEF_FETCH;
   if(group==1){foreign_cpu=kind&1;private_clock=kind&2;prefix=3;first=start;body=start+prefix*4;actual=planned=4;guarded=false;frame.action=variant==3?4:0;}
   if(group==2){prefix=variant?31:3;first=start;body=start+prefix*4;planned=4;actual=kind?2:0;guarded=true;target=body+16;frame.action=0;}
   const uint32_t prefix_op=kind%4==0?0x48000004u:kind%4==1?0x48000005u:((16u<<26)|((kind%4==2?12u:4u)<<21)|(8u<<16)|4u);
   for(unsigned i=0;i<prefix;i++)put(start+i*4,prefix_op);
   if(group==0){put(first,0x38c60001u,true);put(target,0x38c60002u,true);}
   else words[target/4]&=~3ull;
   uint32_t ops[32]{};
   for(unsigned i=0;i<planned;i++){ops[i]=0x3b180001u;put(body+i*4,ops[i]);}
   if(guarded){
    ops[actual]=(32u<<26)|(8u<<21)|(7u<<16);put(body+actual*4,ops[actual],true);
   }else{
    ops[actual-1]=0x48000000u|((page+target-(page+body+(actual-1)*4))&0x03fffffcu);put(body+(actual-1)*4,ops[actual-1]);
   }
   if(group==2){ops[0]=(32u<<26)|(8u<<21)|(7u<<16);ops[1]=0x3b180001u;ops[2]=(32u<<26)|(9u<<21)|(7u<<16)|4u;ops[3]=0x48000004u;for(unsigned i=0;i<4;i++)put(body+i*4,ops[i],i==actual);}
   if(group==1){
    if(variant==0)words[start/4]&=~3ull;
    if(variant==1){put(start,0x48000000u);words[body/4]&=~3ull;}
   }
   map(itlb,page,code.data(),1);map(itlb,page+4096,code.data()+4096,1);map(dtlb,data_va,ram.data(),0x61);
   nw_jit_layout layout{};layout.gpr=offsetof(SetPRS,gpr);layout.cr=offsetof(SetPRS,cr);layout.xer=offsetof(SetPRS,spr)+SPR::XER*4;layout.lr=offsetof(SetPRS,spr)+SPR::LR*4;layout.ctr=offsetof(SetPRS,spr)+SPR::CTR*4;layout.pc=offsetof(SetPRS,pc);layout.memory=ram.data();layout.leaf=1;layout.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);layout.dtlb_slot=reinterpret_cast<uintptr_t>(&pCurDTLB1);layout.tlb_mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);layout.tlb_index_bits=12;
   if(group==2){
    need(pippin_fastmem_enabled(),"actual hardware aliases enabled");if(!physical_read)physical_read=pippin_fastmem_alloc(4096);if(!physical_write)physical_write=pippin_fastmem_alloc(4096);need(physical_read&&physical_write,"owned hardware fault allocations");
    sheep_mmu_jit_layout(&layout);layout.leaf=1;layout.trace_host=layout.trace_old=layout.trace_size=0;layout.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);
   }
   pCurDTLB1=reinterpret_cast<TLBEntry*>(dtlb.data());pCurITLB1=reinterpret_cast<TLBEntry*>(itlb.data());tlb_size_mask=4095;
   frame.body=nw_jit_compile_mapped(ops,planned,page+body,&layout);need(frame.body,"actual kind0 mapped body");
   const uintptr_t shim=reinterpret_cast<uintptr_t>(&clock_publication_native);words[body/4]=(uint64_t(page+body)<<32)|(uint64_t(planned-1)<<2)|3u;filters[body/4]=0;
   SetPRS initial;std::memcpy(&initial,&saved,sizeof(initial));initial.pc=root;initial.msr=MSR::IR;initial.spr[SPR::PVR]=PPC_VER::MPC603;for(unsigned i=0;i<32;i++)initial.gpr[i]=0xa5310000u^(i*0x1020304u+passed*37u);initial.gpr[6]=19;initial.gpr[7]=group==2?data_va+(kind?4092:0):data_va+8192;initial.cr=0xa65a9187u|(1u<<23);initial.spr[SPR::XER]=0xe0000042u;
   const uintptr_t fp=reinterpret_cast<uintptr_t>(&frame);initial.gpr[30]=uint32_t(fp);initial.gpr[31]=uint32_t(fp>>32);
   SheepLoopContext ctx{};ctx.cpu=reinterpret_cast<uintptr_t>(foreign_cpu?&foreign:&ppc_state);ctx.words=reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words);ctx.counters=reinterpret_cast<uintptr_t>(sheep_jit_reject_filter);ctx.native_base=shim;ctx.illegal=reinterpret_cast<uintptr_t>(&ppc_illegalop);ctx.fpu_disabled=reinterpret_cast<uintptr_t>(sheep_rom_disabled_fpu_handler);ctx.pc_offset=offsetof(SetPRS,pc);ctx.cr_offset=offsetof(SetPRS,cr);ctx.lr_offset=offsetof(SetPRS,spr)+SPR::LR*4;ctx.disabled=1;ctx.reject=reinterpret_cast<uintptr_t>(&clock_publication_reject);ctx.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);ctx.host=code.data()+start;ctx.page_start=page;ctx.page_end=page+4095;ctx.itlb=reinterpret_cast<uintptr_t>(&pCurITLB1);ctx.mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);ctx.rom_begin=sheep_rom_host_begin;ctx.rom_end=sheep_rom_host_end;ctx.opcodes=reinterpret_cast<uintptr_t>(OpcodeGrabberNoFPU);ctx.allow_ram=1;
   const auto original_code=code,original_ram=ram;const auto original_itlb=itlb,original_dtlb=dtlb;
   auto bank=[&](const void *address){const intptr_t displacement=intptr_t(address)-intptr_t(&g_icycles);need(displacement>=-256&&displacement<=255,"validated direct scheduler displacement");return private_scheduler.data()+512+displacement;};
   auto reset=[&](){
    unbind();std::memcpy(&ppc_state,&initial,sizeof(initial));std::memcpy(&foreign,&initial,sizeof(initial));code=original_code;ram=original_ram;itlb=original_itlb;dtlb=original_dtlb;pCurITLB1=reinterpret_cast<TLBEntry*>(itlb.data());pCurITLB2=i20;pCurDTLB1=reinterpret_cast<TLBEntry*>(dtlb.data());pCurDTLB2=d20;CurITLBMode=1;CurDTLBMode=0;tlb_size_mask=4095;ppc_opcode_grabber=OpcodeGrabberNoFPU;is_601=include_601=false;
    exec_flags=entry_flags;ppc_next_instruction_address=0x31415920u;g_icycles=initial_clock;exec_timer=initial_timer;power_on=initial_power;sheep_jit_memory_bail=false;gCtxSyncCallbacks.clear();private_scheduler.fill(0);
    frame.cpu=foreign_cpu?&foreign:&ppc_state;frame.clock=private_clock?reinterpret_cast<uint64_t*>(bank(&g_icycles)):&g_icycles;frame.timer=private_clock?reinterpret_cast<volatile bool*>(bank(const_cast<const bool*>(&exec_timer))):&exec_timer;frame.power=private_clock?reinterpret_cast<bool*>(bank(&power_on)):&power_on;frame.flags=private_clock?reinterpret_cast<uint32_t*>(bank(&exec_flags)):&exec_flags;frame.next=private_clock?reinterpret_cast<uint32_t*>(bank(&ppc_next_instruction_address)):&ppc_next_instruction_address;
    *frame.clock=initial_clock;*frame.flags=entry_flags;*frame.next=0x31415920u;*frame.timer=initial_timer;*frame.power=initial_power;frame.first_pc=page+first;frame.fail_pc=page+body+actual*4;frame.handler_calls=frame.native_calls=frame.reject_calls=frame.seen_count=0;frame.native_abi=0;frame.signal_calls=frame.signal_clock_low=frame.signal_clock_high=0;std::memset(frame.seen,0,sizeof(frame.seen));
    ctx.clock=reinterpret_cast<uintptr_t>(frame.clock);ctx.timer=reinterpret_cast<uintptr_t>(frame.timer);ctx.flags=reinterpret_cast<uintptr_t>(frame.flags);ctx.next=reinterpret_cast<uintptr_t>(frame.next);ctx.power=reinterpret_cast<uintptr_t>(frame.power);ctx.max_cycles=deadline;
    if(group==0&&variant==2)ctx.max_cycles=initial_clock+prefix+1+planned-1;
    if(group==2){
     for(unsigned i=0;i<4096;i++){physical_read[i]=uint8_t(i*37+11);physical_write[i]=uint8_t(i*19+7);}
     sheep_mmu_test_fast_page(physical_read,physical_write,data_va,kind?0x61:0x62,false,0);fast_bound=true;
     need(sigaction(SIGSEGV,nullptr,&frame.previous_segv)==0&&(frame.previous_segv.sa_flags&SA_SIGINFO)&&frame.previous_segv.sa_sigaction,"existing actual fault handler");
     struct sigaction hook=frame.previous_segv;hook.sa_sigaction=&clock_publication_signal;need(sigaction(SIGSEGV,&hook,nullptr)==0,"bounded forwarding fault observer");signal_bound=true;
    }
    for(unsigned i=0;i<2048;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+i*4);sheep_rom_dispatch_words[slot]=words[i];sheep_jit_reject_filter[slot]=filters[i];}
   };
   struct Result {SetPRS cpu,global_cpu;SheepLoopContext context;std::array<uint8_t,8192> ram;std::array<uint64_t,2048> words;std::array<uint8_t,2048> filters;ClockPublicationFrame::Seen seen[8];uint64_t abi,clock,global_clock,native_abi,signal_clock;uint32_t flags,next;unsigned handlers,natives,rejects,observations,faults,signals;bool timer,power,bail,metadata;TLBEntry *i1,*i2,*d1,*d2;uint32_t im,dm,mask;PPCOpcode *decoder;};
   auto run=[&](unsigned(*fn)(SheepLoopContext*)){reset();Result r{};std::memcpy(&r.context,&ctx,sizeof(ctx));const unsigned faults=pippin_fastmem_fault_count();r.abi=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(fn),reinterpret_cast<nw_jit_cpu*>(&r.context));std::memcpy(&r.cpu,frame.cpu,sizeof(r.cpu));std::memcpy(&r.global_cpu,&ppc_state,sizeof(r.global_cpu));r.ram=ram;std::memcpy(r.seen,frame.seen,sizeof(r.seen));r.clock=*frame.clock;r.global_clock=g_icycles;r.flags=*frame.flags;r.next=*frame.next;r.timer=*frame.timer;r.power=*frame.power;r.bail=sheep_jit_memory_bail;r.handlers=frame.handler_calls;r.natives=frame.native_calls;r.rejects=frame.reject_calls;r.observations=frame.seen_count;r.native_abi=frame.native_abi;r.faults=pippin_fastmem_fault_count()-faults;r.signals=frame.signal_calls;r.signal_clock=(uint64_t(uint32_t(frame.signal_clock_high))<<32)|uint32_t(frame.signal_clock_low);r.i1=pCurITLB1;r.i2=pCurITLB2;r.d1=pCurDTLB1;r.d2=pCurDTLB2;r.im=CurITLBMode;r.dm=CurDTLBMode;r.mask=tlb_size_mask;r.decoder=ppc_opcode_grabber;r.metadata=code==original_code&&itlb==original_itlb;for(unsigned i=0;i<2048;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+4*i);r.words[i]=sheep_rom_dispatch_words[slot];r.filters[i]=sheep_jit_reject_filter[slot];}unbind();return r;};
   const auto old=run(&sheep_clock_publication_reference_a64),now=run(&sheep_cpu_loop_a64);
   need(old.abi==now.abi&&!(now.abi>>32)&&!now.native_abi&&old.native_abi==now.native_abi&&!std::memcmp(&old.cpu,&now.cpu,sizeof(SetPRS))&&!std::memcmp(&old.global_cpu,&now.global_cpu,sizeof(SetPRS))&&!std::memcmp(&old.context,&now.context,sizeof(SheepLoopContext))&&old.ram==now.ram&&old.words==now.words&&old.filters==now.filters&&!std::memcmp(old.seen,now.seen,sizeof(old.seen))&&old.clock==now.clock&&old.global_clock==now.global_clock&&old.flags==now.flags&&old.next==now.next&&old.timer==now.timer&&old.power==now.power&&old.bail==now.bail&&old.handlers==now.handlers&&old.natives==now.natives&&old.rejects==now.rejects&&old.observations==now.observations&&old.faults==now.faults&&old.signals==now.signals&&old.signal_clock==now.signal_clock&&old.i1==now.i1&&old.i2==now.i2&&old.d1==now.d1&&old.d2==now.d2&&old.im==now.im&&old.dm==now.dm&&old.mask==now.mask&&old.decoder==now.decoder&&old.metadata&&now.metadata,"frozen361/new358 exact CPU/context/RAM/clock/callback/fault/MMU/ABI");
   need(now.observations<=8,"bounded callback timeline");
   if(group==0){
    const bool before_native=(variant==2||variant==3||(variant==7&&prefix>=127));need(now.natives==unsigned(!before_native),"actual native activation/refusal boundary");
    if(now.natives){const uint64_t expected=initial_clock+prefix+1+(variant==1?7:0);bool found=false;for(unsigned i=0;i<now.observations;i++)if(now.seen[i].kind==1){need(now.seen[i].clock==expected,"native entry observes independently derived published clock");found=true;}need(found,"native-entry witness present");}
    if(variant==1)need(now.clock>=initial_clock+7,"canonical callback clock write retained after reload");
    if(variant==7)need(now.context.retired==128||now.context.retired==129,"exact original cap/whole native-block overshoot");
    if(variant==3)need(now.context.retired==0&&!now.observations&&!uint32_t(now.abi),"pre-existing flags cold0 unchanged");
   }else if(group==1){
    const unsigned expected=variant==0?0:variant==1?128:variant==2?7:7;need(now.context.retired==expected,"private/foreign cold/cap/native retirement");
    if(variant>=2){need(now.natives==1,"private/foreign actual native entry");bool found=false;for(unsigned i=0;i<now.observations;i++)if(now.seen[i].kind==1){need(now.seen[i].clock==initial_clock+prefix,"private/foreign native pre-call clock");found=true;}need(found,"private/native witness");}
    if(private_clock)need(now.global_clock==initial_clock,"private scheduler does not write real global clock");
   }else{
    need(now.faults==1&&now.signals==1&&now.natives==1&&now.rejects==1&&now.bail,"one genuine first/second hardware fault and unchanged precise bailout");
    need(now.signal_clock==initial_clock+prefix,"actual SIGSEGV entry sees native-entry published clock");
    need(now.context.retired==prefix+actual+1,"fault prefix plus one explicit private stopping scalar handler");
   }
   // A separate canonical opcode replay covers the normal complete chain,
   // foreign/native-to-cold return, and each genuine fault's executed prefix.
   // The stopping load callback remains explicitly synthetic; do not claim a
   // guest DSI vector proof from the host fault forwarding observer.
   if((group==0&&kind>0&&variant<=1)||(group==1&&variant==2)||group==2){
    reset();auto advance=[&](){const uint32_t pc=ppc_state.pc;const uint32_t next=exec_flags&EXEF_BRANCH?ppc_next_instruction_address:pc+4u;need(!exec_flags||exec_flags==EXEF_BRANCH,"canonical plain intermediate flags");ppc_state.pc=next;exec_flags=0;++g_icycles;};
    for(unsigned i=0;i<prefix;i++){ppc_main_opcode(ppc_opcode_grabber,prefix_op);advance();}
    if(group==0){ppc_main_opcode(ppc_opcode_grabber,0x38c60001u);if(variant==1)g_icycles+=7;advance();}
    for(unsigned i=0;i<actual;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);advance();}
    if(group==0){ppc_main_opcode(ppc_opcode_grabber,0x38c60002u);exec_timer=true;}
    if(group==2){clock_publication_handler(ops[actual]);}
    need(!std::memcmp(&ppc_state,&now.cpu,sizeof(SetPRS)),"independent canonical complete CPU/Rc/LR/XER and exact fault prefix");
    if(!private_clock)need(g_icycles==now.clock&&exec_flags==now.flags&&ppc_next_instruction_address==now.next&&exec_timer==now.timer&&power_on==now.power,"independent canonical clock/flags/NIA/event boundary");
    else need(now.clock==initial_clock+prefix+actual&&now.flags==exec_flags&&now.next==ppc_next_instruction_address,"private bank canonical count and final NIA/flags");
    unbind();
   }
   ++passed;if(group==0)++observers;else if(group==1)++private_cases;else ++fault_cases;
  };
  for(unsigned kind=0;kind<6;kind++)for(unsigned variant=0;variant<8;variant++)trial(0,kind,variant);
  for(unsigned identities=0;identities<4;identities++)for(unsigned variant=0;variant<4;variant++)trial(1,identities,variant);
  for(unsigned first_second=0;first_second<2;first_second++)for(unsigned prefix=0;prefix<2;prefix++)trial(2,first_second,prefix);
  need(passed==68&&observers==48&&private_cases==16&&fault_cases==4,"fixed focused clock-publication census");
 }catch(...){cleanup();throw;}
 cleanup();fprintf(stderr,"CLOCK PUBLICATION SELFTEST %u native_observers=%u private_foreign=%u genuine_faults=%u frozen361/fullCPU/context/RAM/flags/NIA/finalclock/ABI cases passed\n",passed,observers,private_cases,fault_cases);return 1;
#else
 return 0;
#endif
}
