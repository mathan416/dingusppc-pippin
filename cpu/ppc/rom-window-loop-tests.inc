// SPDX-License-Identifier: GPL-3.0-or-later
// Frozen Q193333 reference: symbol/local-label renames only; context tail ignored.
#if defined(__aarch64__)
extern "C" unsigned sheep_rom_window_reference_a64(SheepLoopContext*);
asm(R"(
.text
.p2align 4
.global sheep_rom_window_reference_a64
.type sheep_rom_window_reference_a64,%function
sheep_rom_window_reference_a64:
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
 cbnz w9,.Lrw_ref_cold
.Lrw_ref_fetch:
 tbnz x20,#0,.Lrw_ref_fetch_ram
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 // Decode only the descriptor tag bits; preserve opcode, metadata and index.
 tbz w25,#1,.Lrw_ref_low_tag
 tbnz w25,#0,.Lrw_ref_native
 // Match the canonical retry counter, including its final blocked iteration.
 ldr x11,[x28,#16]
 ldrb w9,[x11,x10]
 cbz w9,.Lrw_ref_retry_zero
 cmp w9,#255
 b.eq .Lrw_ref_scalar
 sub w9,w9,#1
 strb w9,[x11,x10]
 cbnz w9,.Lrw_ref_scalar
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lrw_ref_scalar
.Lrw_ref_fetch_ram:
 // Writable code is fetched anew for every instruction. No native RAM cache.
 ldur w0,[x20,#-1]
 rev w0,w0
 lsl x25,x0,#32
 b .Lrw_ref_scalar
.Lrw_ref_retry_zero:
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lrw_ref_cold
.Lrw_ref_native:
 lsr x9,x25,#32
 cmp w9,w24
 b.ne .Lrw_ref_cold
 ubfx w26,w25,#2,#5
 add w26,w26,#1
 sub x10,x23,x22
 cmp x26,x10
 b.hi .Lrw_ref_cold
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
 b.hi .Lrw_ref_trap
 mov w26,w0
 ldr w25,[x19,#256]
 cmp w26,#4
 mov w14,#1024
 b.hs .Lrw_ref_native_fast
 // Successful short branches need no rejection or interpreter round-trip.
 // Zero/failed-guard prefixes retain their original slow-path backoff.
 cbz w26,.Lrw_ref_native_short_guard
 ldr x9,[x28,#128]
 ldrb w9,[x9]
 mov w14,#128
 cbz w9,.Lrw_ref_native_fast
.Lrw_ref_native_short_guard:
 mov x0,x20
 mov w1,w24
 ldr x9,[x28,#120]
 blr x9
 cbnz w26,.Lrw_ref_native_prefix
 str w24,[x19,#256]
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 b .Lrw_ref_scalar
.Lrw_ref_native_fast:
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
.global sheep_rom_window_reference_guard_native
.hidden sheep_rom_window_reference_guard_native
sheep_rom_window_reference_guard_native:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lrw_ref_native_fast_old
 // Preserve the canonical last branch target even when consuming the branch
 // locally. It remains observable at later event and exception boundaries.
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lrw_ref_native_fast_sequential
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
.Lrw_ref_native_fast_sequential:
 add w27,w27,w26
 str x22,[x29]
 cmp x22,x23
 b.hs .Lrw_ref_native_fast_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lrw_ref_native_fast_pending
 cmp w27,w14
 b.hs .Lrw_ref_native_fast_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lrw_ref_native_fast_pending
 // No device/event polling is removed: deadlines, external timer and power
 // are checked after every bounded block, as in the original path.
 mov x20,x12
 mov w24,w25
 add x22,x22,#1
 str x22,[x29]
 // The native block already stored this exact target into CPU.pc.
 b .Lrw_ref_fetch
.Lrw_ref_native_fast_old:
 // Cross-page native exits retain the exact128-instruction span cap. The
 // helper-free leaf cannot change exec_flags; avoid publishing a synthetic
 // BRANCH only to reload/clear it after the same polls and live mapper.
 // NIA remains observable even when the translated target is consumed here.
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 str x22,[x29]
 add w27,w27,w26
 cmp x22,x23
 b.hs .Lrw_ref_native_map_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lrw_ref_native_map_pending
 cmp w27,#128
 b.hs .Lrw_ref_native_map_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lrw_ref_native_map_pending
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
 b.ne .Lrw_ref_native_map_pending
 tbz x11,#32,.Lrw_ref_native_map_pending
 add x9,x9,w25,uxtw
 ldr x11,[x28,#168]
 cmp x9,x11
 b.lo .Lrw_ref_native_map_ram
 ldr x11,[x28,#176]
 cmp x9,x11
 b.hs .Lrw_ref_native_map_ram
 mov x20,x9
 str wzr,[x28,#192]
.Lrw_ref_native_mapped:
 mov w24,w25
 str w12,[x28,#96]
 add w11,w12,#4095
 str w11,[x28,#100]
 // CPU.pc already contains this target. Preserve the old post-step clock
 // and mapped-source update without rolling CPU.pc back and forward.
 add x22,x22,#1
 str x22,[x29]
 b .Lrw_ref_fetch
.Lrw_ref_native_map_ram:
 ldr w11,[x28,#196]
 cbz w11,.Lrw_ref_native_map_pending
 tst x9,#3
 b.ne .Lrw_ref_native_map_pending
 orr x20,x9,#1
 mov w11,#1
 str w11,[x28,#192]
 b .Lrw_ref_native_mapped
.Lrw_ref_native_map_pending:
 // Every unsuccessful handoff exposes the old last-source/BRANCH state,
 // including a sequential cross-page target or32-bit guest-PC wrap.
 str w24,[x19,#256]
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lrw_ref_pending
.Lrw_ref_native_fast_pending:
 str w24,[x19,#256]
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lrw_ref_pending
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lrw_ref_pending
.Lrw_ref_native_prefix:
 sub w9,w26,#1
 add x20,x20,w9,uxtw #2
 add w24,w24,w9,lsl #2
 add x22,x22,x9
 str w24,[x19,#256]
.Lrw_ref_native_prefix_done:
 add w9,w24,#4
 cmp w25,w9
 b.ne .Lrw_ref_native_branch
 // Target is already proven to be source+4. Only target bits2..11==0 cross a
 // page, including the 32-bit address wrap from fffffffc to zero.
.global sheep_rom_window_reference_guard_next
.hidden sheep_rom_window_reference_guard_next
sheep_rom_window_reference_guard_next:
 tst w25,#0xffc
 b.ne .Lrw_ref_after
.Lrw_ref_native_branch:
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
 b .Lrw_ref_after
.Lrw_ref_low_tag:
 // Scalar tag1 falls through; native/retry routes keep their old branches.
 tbz w25,#0,.Lrw_ref_cold
.Lrw_ref_scalar:
 mov w26,#1
 lsr x0,x25,#32
 lsr w9,w0,#26
 cmp w9,#18
 b.eq .Lrw_ref_b
 cmp w9,#16
 b.eq .Lrw_ref_bc
.Lrw_ref_handler:
 tbnz x20,#0,.Lrw_ref_ram_handler
 // The packed descriptor stores a signed handler offset and FPU flag.
 ldr w9,[x28,#116]
 cbz w9,.Lrw_ref_normal_handler
 tbz w25,#31,.Lrw_ref_normal_handler
 ldr x9,[x28,#40]
 b .Lrw_ref_call_handler
.Lrw_ref_normal_handler:
 lsl w9,w25,#1
 asr w9,w9,#3
 ldr x10,[x28,#32]
 add x9,x10,w9,sxtw #2
 b .Lrw_ref_call_handler
.Lrw_ref_ram_handler:
 // Exactly the canonical ppc_main_opcode index and selected FPU table.
 lsr w9,w0,#15
 and w9,w9,#0x1f800
 and w10,w0,#0x7ff
 orr w9,w9,w10
 ldr x10,[x28,#184]
 ldr x9,[x10,w9,uxtw #3]
 b .Lrw_ref_call_handler
.Lrw_ref_call_handler:
 blr x9
 ldr x22,[x29]
 b .Lrw_ref_after
.Lrw_ref_b:
 tbnz w0,#1,.Lrw_ref_handler
 tbz w0,#0,.Lrw_ref_b_target
 add w9,w24,#4
 ldr w10,[x28,#112]
 str w9,[x19,x10]
.Lrw_ref_b_target:
 sbfx w9,w0,#0,#26
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lrw_ref_native_branch
.Lrw_ref_bc:
 tst w0,#3
 b.ne .Lrw_ref_handler
 ubfx w9,w0,#21,#5
 and w10,w9,#30
 cmp w10,#4
 b.eq .Lrw_ref_bc_cr
 cmp w10,#12
 b.ne .Lrw_ref_handler
.Lrw_ref_bc_cr:
 ldr w10,[x28,#108]
 ldr w10,[x19,x10]
 ubfx w11,w0,#16,#5
 mov w12,#31
 sub w11,w12,w11
 lsr w10,w10,w11
 and w10,w10,#1
 ubfx w9,w9,#3,#1
 cmp w9,w10
 b.ne .Lrw_ref_after
 sxth w9,w0
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lrw_ref_native_branch
.Lrw_ref_after:
 // Keep the final pre-increment value for ppc_exec's existing event check.
 str x22,[x29]
 add w27,w27,w26
 cmp x22,x23
 b.hs .Lrw_ref_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lrw_ref_pending
 cmp w27,#128
 b.hs .Lrw_ref_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lrw_ref_pending
 .word 0xb84003a9 | (((exec_flags-g_icycles)&0x1ff)<<12)
 cbz w9,.Lrw_ref_advance
 cmp w9,#1
 b.ne .Lrw_ref_pending
 ldr x10,[x28,#64]
 .word 0xb84003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 // Scalar callbacks cannot change the loop's mapped source W24. Keep
 // arbitrary targets subject to the exact existing primary-ITLB mapper.
.global sheep_rom_window_reference_guard_branch
.hidden sheep_rom_window_reference_guard_branch
sheep_rom_window_reference_guard_branch:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lrw_ref_map_target
 // Only a plain same-page branch can be consumed inside the fast loop.
 sub w9,w25,w24
 sxtw x9,w9
 add x20,x20,x9
 mov w24,w25
 str wzr,[x10]
 b .Lrw_ref_continue
.Lrw_ref_advance:
.global sheep_rom_window_reference_guard_advance
.hidden sheep_rom_window_reference_guard_advance
sheep_rom_window_reference_guard_advance:
 add w10,w24,#4
 // Source+4 leaves its page iff target bits2..11 are zero, even unaligned.
 tst w10,#0xffc
 b.ne .Lrw_ref_advance_same
 mov w25,w10
 ldr x10,[x28,#64]
 b .Lrw_ref_map_target
.Lrw_ref_advance_same:
 add w24,w24,#4
 add x20,x20,#4
.Lrw_ref_continue:
 add x22,x22,#1
 str x22,[x29]
 str w24,[x19,#256]
 b .Lrw_ref_fetch
.Lrw_ref_map_target:
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
 b.ne .Lrw_ref_pending
 tbz x11,#32,.Lrw_ref_pending
 add x9,x9,w25,uxtw
 ldr x11,[x28,#168]
 cmp x9,x11
 b.lo .Lrw_ref_target_ram
 ldr x11,[x28,#176]
 cmp x9,x11
 b.hs .Lrw_ref_target_ram
 mov x20,x9
 str wzr,[x28,#192]
 b .Lrw_ref_mapped
.Lrw_ref_target_ram:
 ldr w11,[x28,#196]
 cbz w11,.Lrw_ref_pending
 tst x9,#3
 b.ne .Lrw_ref_pending
 orr x20,x9,#1
 mov w11,#1
 str w11,[x28,#192]
.Lrw_ref_mapped:
 mov w24,w25
 str w12,[x28,#96]
 add w11,w12,#4095
 str w11,[x28,#100]
 str wzr,[x10]
 b .Lrw_ref_continue
.Lrw_ref_pending:
 mov w0,#1
 b .Lrw_ref_return
.Lrw_ref_cold:
 mov w0,#0
.Lrw_ref_return:
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
.Lrw_ref_trap:
 brk #0
.cfi_endproc
.size sheep_rom_window_reference_a64,.-sheep_rom_window_reference_a64
)");
#endif

#include <vector>
extern const uint8_t* sheep_mmu_rom_window_test_begin(uint32_t);
extern void sheep_mmu_rom_window_test_end() noexcept;
extern void sheep_rom_window_test_reset_native_refs();
#if defined(__aarch64__)
struct ROMWindowLoopFrame {
 uint8_t *ram;uint32_t target;unsigned action,calls;
 struct Seen {uint32_t pc,lr,next,cr;uint64_t clock;} seen[2]{};
};
static ROMWindowLoopFrame &rom_window_loop_frame(){return *reinterpret_cast<ROMWindowLoopFrame*>(uintptr_t(ppc_state.gpr[30])|(uintptr_t(ppc_state.gpr[31])<<32));}
[[gnu::noinline]] static void rom_window_loop_observer(uint32_t op){
 auto &f=rom_window_loop_frame();auto &s=f.seen[std::min(f.calls,1u)];s.pc=ppc_state.pc;s.lr=ppc_state.spr[SPR::LR];s.next=ppc_next_instruction_address;s.cr=ppc_state.cr;s.clock=g_icycles;++f.calls;
 ppc_state.gpr[24]+=0x10203u+f.calls;ppc_state.gpr[29]^=0x9e3779b9u+f.calls;ppc_state.cr^=0x51a03c85u;f.ram[(13*f.calls)%128]^=uint8_t(op^f.calls);
 if(f.calls==1&&f.action==2){ppc_state.spr[529]+=0x20000u;ibat_update(529);do_ctx_sync();mmu_translate_imem(f.target);ppc_next_instruction_address=f.target;exec_flags=EXEF_BRANCH;}
 else if(f.calls==1&&f.action==3){ppc_next_instruction_address=f.target;exec_flags=EXEF_BRANCH;}
 else{if(f.action==1)sheep_mmu_rom_window_invalidate();power_on=false;}
 generic_continuation_clobber();
}
#endif
int sheep_rom_window_loop_selftest(){
#if defined(__aarch64__) && !defined(LOG_INSTRUCTIONS) && !defined(CPU_PROFILING)
 static_assert(sizeof(SheepLoopContext)==208&&offsetof(SheepLoopContext,window)==200,"window context ABI");
 SetPRS saved;std::memcpy(&saved,&ppc_state,sizeof(saved));const auto flags0=exec_flags,next0=ppc_next_instruction_address;const auto clock0=g_icycles;
 const bool timer0=exec_timer,power0=power_on,bail0=sheep_jit_memory_bail,only0=sheep_rom_only,direct0=sheep_rom_direct_enabled,hot0=sheep_hot_enabled,used0=sheep_hot_used,decode0=sheep_rom_decode_ready;
 const auto begin0=sheep_rom_host_begin,end0=sheep_rom_host_end,base0=sheep_rom_direct_base;const auto spans0=sheep_steady_spans,insns0=sheep_steady_instructions;
 const auto reason0=power_off_reason;const bool irq0=int_pin,dec0=dec_exception_pending,thrm0=thrm_exception_pending;auto *const decoder0=ppc_opcode_grabber;
 constexpr uint32_t observer_op=0x4c00002au,nia0=0x31415920u;constexpr unsigned bytes=0x40000,words_count=bytes/4;
 const unsigned oi=(observer_op>>15&0x1f800)|(observer_op&0x7ff);const auto oe=OpcodeGrabber[oi],od=OpcodeGrabberNoFPU[oi];
 std::vector<uint8_t> original_rom(bytes),original_counters(words_count);std::vector<uint64_t> original_words(words_count);
 std::array<uint8_t,128> ram{};ROMWindowLoopFrame frame{ram.data(),0,0,0,{}};SetPRS foreign;
 uint8_t *host=nullptr;bool bound=false,journaled=false;unsigned passed=0,basic=0,polls=0,caps=0,bounds=0,revocation=0,outer=0;
 unsigned dg=0,df=0,dv=0;uint32_t db=0;
 auto need=[&](bool good,const char *why){if(!good){fprintf(stderr,"ROM WINDOW LOOP failure case=%u group=%u family=%u variant=%u base=%08x %s\n",passed+1,dg,df,dv,db,why);throw std::runtime_error("Q203 ROM window loop failed");}};
 auto arm_cleanup=[&](){
  if(journaled){sheep_mmu_rom_window_invalidate();sheep_rom_window_test_reset_native_refs();std::copy(original_rom.begin(),original_rom.end(),host);for(unsigned i=0;i<words_count;i++){const auto slot=sheep_jit_filter_slot_rom(host+4*i);sheep_rom_dispatch_words[slot]=original_words[i];sheep_jit_reject_filter[slot]=original_counters[i];}journaled=false;}
  if(bound){sheep_mmu_rom_window_test_end();bound=false;}nw_jit_reset();
 };
 auto cleanup=[&](){arm_cleanup();OpcodeGrabber[oi]=oe;OpcodeGrabberNoFPU[oi]=od;std::memcpy(&ppc_state,&saved,sizeof(saved));exec_flags=flags0;ppc_next_instruction_address=next0;g_icycles=clock0;exec_timer=timer0;power_on=power0;sheep_jit_memory_bail=bail0;power_off_reason=reason0;int_pin=irq0;dec_exception_pending=dec0;thrm_exception_pending=thrm0;ppc_opcode_grabber=decoder0;sheep_rom_only=only0;sheep_rom_direct_enabled=direct0;sheep_hot_enabled=hot0;sheep_hot_used=used0;sheep_rom_decode_ready=decode0;sheep_rom_host_begin=begin0;sheep_rom_host_end=end0;sheep_rom_direct_base=base0;sheep_steady_spans=spans0;sheep_steady_instructions=insns0;};
 try{
  sheep_rom_direct_reset();nw_jit_reset();need(sheep_itlb_primary_enabled&&sheep_jit_enabled(),"initialized private JIT/primary path");
  OpcodeGrabber[oi]=OpcodeGrabberNoFPU[oi]=rom_window_loop_observer;
  const int64_t delta=int64_t(reinterpret_cast<uintptr_t>(&rom_window_loop_observer))-int64_t(reinterpret_cast<uintptr_t>(&ppc_illegalop));need(!(delta&3)&&delta>=-(int64_t(1)<<30)&&delta<(int64_t(1)<<30),"private observer packed range");
  struct Result {SetPRS cpu,global;SheepLoopContext ctx;std::array<uint8_t,128> ram;ROMWindowLoopFrame::Seen seen[2];std::array<uint8_t,4096*32> primary;SheepROMWindow window;uint64_t abi,clock,spans,insns;uint32_t flags,next,mask,im,dm,target_tag;uintptr_t i1,i2;unsigned calls;bool timer,power,bail,metadata,primary_target;};
  auto trial=[&](unsigned group,unsigned family,uint32_t base,unsigned variant){
   dg=group;df=family;dv=variant;db=base;
   const unsigned counts[]={1,1,2,4,4,32};const unsigned n=counts[family],planned=family>=2?(family==5?32:4):1;
   unsigned start=4096-planned*4,target_off=(family==1||family==4)?4096:4160;
   uint32_t root=base+start,target=base+target_off;unsigned mode=group==0?variant:group==1?variant&1u:group==2?variant:0,event=group==1?variant>>1:4;
   const int offsets[]={-4,0x1fffc,0x1fffd,0x1fffe,0x1ffff,0x20000};if(group==3)target=base+uint32_t(offsets[variant]);
   const bool foreign_cpu=mode==4;const bool success=group==2||group==4||group==5||(!(group==1)&&!foreign_cpu&&(group!=3||variant==1));
   const unsigned expected_retired=group==2?128:group==1&&event==3?0:group==1&&event==0&&family>=2?0:success?n+(group==4?3:1):n;
   const unsigned expected_pending=expected_retired?1:0;
   auto arm=[&](unsigned which,bool force_legacy=false){
    try{
     host=const_cast<uint8_t*>(sheep_mmu_rom_window_test_begin(base));bound=true;auto *rom=mem_ctrl_instance->find_rom_region();need(rom&&uint64_t(rom->end)+1-rom->start>=bytes,"actual ROM owned journal span");
     std::copy_n(host,bytes,original_rom.begin());for(unsigned i=0;i<words_count;i++){const auto slot=sheep_jit_filter_slot_rom(host+4*i);original_words[i]=sheep_rom_dispatch_words[slot];original_counters[i]=sheep_jit_reject_filter[slot];}journaled=true;
     auto put=[&](unsigned off,uint32_t op){need(off+4<=bytes&&!(off&3),"owned ROM word");for(unsigned b=0;b<4;b++)host[off+b]=uint8_t(op>>(24-8*b));uint32_t packed;need(sheep_encode_rom_handler(op,packed),"canonical integer/observer descriptor");const auto slot=sheep_jit_filter_slot_rom(host+off);sheep_rom_dispatch_words[slot]=(uint64_t(op)<<32)|packed|1u;sheep_jit_reject_filter[slot]=255;};
     for(unsigned off=0;off<bytes;off+=4)put(off,0x60000000u);
     auto branch=[&](unsigned off,uint32_t pc,uint32_t to){put(off,0x48000000u|((to-pc)&0x03fffffcu));};
     if(family==1)put(start,0x60000000u);else if(family<2)branch(start,root,target);
     else{for(unsigned i=0;i<planned;i++)put(start+4*i,0x3b180001u);branch(start+4*(n-1),root+4*(n-1),target);}
     if(group==2){branch(target_off,target,root);}
     else if(group==3){put(start,observer_op);if(variant==1)put(0x1fffc,observer_op);}
     else if(group==4){put(target_off,observer_op);put(8192+64,0x38c60001u);put(8192+68,observer_op);put(0x20000+8192+64,0x38c60002u);put(0x20000+8192+68,observer_op);}
     else put(target_off,observer_op);
     if(group==5){if(variant==0){put(target_off,0x38c60002u);put(target_off+4,observer_op);const auto slot=sheep_jit_filter_slot_rom(host+target_off);sheep_rom_dispatch_words[slot]&=~3ull;}else put(start,observer_op);}
     nw_jit_layout layout{};layout.gpr=offsetof(SetPRS,gpr);layout.cr=offsetof(SetPRS,cr);layout.xer=offsetof(SetPRS,spr)+SPR::XER*4;layout.lr=offsetof(SetPRS,spr)+SPR::LR*4;layout.ctr=offsetof(SetPRS,spr)+SPR::CTR*4;layout.pc=offsetof(SetPRS,pc);layout.leaf=1;layout.memory=ram.data();layout.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);
     auto publish=[&](unsigned off,uint32_t pc,unsigned count){uint32_t ops[32];for(unsigned i=0;i<count;i++)ops[i]=uint32_t(host[off+4*i])<<24|uint32_t(host[off+4*i+1])<<16|uint32_t(host[off+4*i+2])<<8|host[off+4*i+3];auto fn=nw_jit_compile_mapped(ops,int(count),pc,&layout);const auto nb=reinterpret_cast<uintptr_t>(nw_jit_native_base()),address=reinterpret_cast<uintptr_t>(fn);need(fn&&address>=nb&&!((address-nb)&3)&&address-nb<(uintptr_t(1)<<27),"real ordinary native producer");const auto slot=sheep_jit_filter_slot_rom(host+off);sheep_rom_dispatch_words[slot]=(uint64_t(pc)<<32)|(uint64_t((uint32_t((address-nb)>>2)<<5)|(count-1))<<2)|3u;sheep_jit_reject_filter[slot]=0;};
     if(family>=2)publish(start,root,planned);
     if(group==2&&family>=2){for(unsigned i=0;i<planned;i++)put(target_off+4*i,0x3b180001u);branch(target_off+4*(n-1),target+4*(n-1),root);publish(target_off,target,planned);}
     auto *source=mmu_translate_imem(root);if(group!=3||variant==1)mmu_translate_imem(target);
     if(group==4)mmu_translate_imem(base+8192+64);
     SetPRS input;std::memcpy(&input,&ppc_state,sizeof(input));input.pc=root;for(unsigned r=0;r<32;r++)input.gpr[r]=0xa5310000u^(r*0x123457u);input.gpr[6]=19;input.cr=0x51a03c85u;input.spr[SPR::XER]=0xe0000042u;input.spr[SPR::LR]=0x76543213u;input.spr[SPR::CTR]=7;const auto fp=reinterpret_cast<uintptr_t>(&frame);input.gpr[30]=uint32_t(fp);input.gpr[31]=uint32_t(fp>>32);std::memcpy(&ppc_state,&input,sizeof(input));std::memcpy(&foreign,&input,sizeof(input));
     exec_flags=group==1&&event==3?EXEF_BRANCH|EXEF_FETCH:0;ppc_next_instruction_address=nia0;g_icycles=123;exec_timer=group==1&&event==1;power_on=!(group==1&&event==2);sheep_jit_memory_bail=false;int_pin=dec_exception_pending=thrm_exception_pending=false;ppc_opcode_grabber=OpcodeGrabberNoFPU;sheep_rom_only=sheep_rom_direct_enabled=sheep_rom_decode_ready=true;sheep_hot_enabled=false;sheep_hot_used=false;sheep_rom_host_begin=uintptr_t(host);sheep_rom_host_end=uintptr_t(host+bytes);sheep_rom_direct_base=uintptr_t(nw_jit_native_base());sheep_steady_spans=sheep_steady_instructions=0;
     for(unsigned i=0;i<ram.size();i++)ram[i]=uint8_t(i*37u+11u);frame.calls=0;std::memset(frame.seen,0,sizeof(frame.seen));frame.action=group==3?3:group==4?2:group==5&&variant==1?1:0;frame.target=group==4?base+8192+64:target;
     auto *const target_slot=reinterpret_cast<uint8_t*>(pCurITLB1)+size_t((target>>12)&tlb_size_mask)*32;const uint32_t bad=(target&~4095u)^4096u;
     const bool poison=(mode>=3||group==3||group==5)&&!force_legacy;
     if(poison)std::memcpy(target_slot,&bad,4);
     if(mode==1)sheep_mmu_rom_window_invalidate();
     const auto *window=mode==2?nullptr:sheep_mmu_rom_window_get();
     if(window&&window->valid_start_count)need(window->cpu_base==uintptr_t(&ppc_state)&&uintptr_t(window->guest_start)+window->host_offset>=sheep_rom_host_begin&&uintptr_t(window->guest_start)+window->host_offset+uint64_t(window->valid_start_count)+3<=sheep_rom_host_end,"real token lies inside registered decoder ROM");
     SheepLoopContext ctx{};ctx.cpu=uintptr_t(foreign_cpu?&foreign:&ppc_state);ctx.words=uintptr_t(sheep_rom_dispatch_words);ctx.counters=uintptr_t(sheep_jit_reject_filter);ctx.native_base=uintptr_t(nw_jit_native_base());ctx.illegal=uintptr_t(&ppc_illegalop);ctx.fpu_disabled=uintptr_t(sheep_rom_disabled_fpu_handler);ctx.timer=uintptr_t(&exec_timer);ctx.clock=uintptr_t(&g_icycles);ctx.flags=uintptr_t(&exec_flags);ctx.next=uintptr_t(&ppc_next_instruction_address);ctx.max_cycles=group==1&&event==0?123+planned-1:4096;ctx.host=source;ctx.page_start=root&~4095u;ctx.page_end=ctx.page_start+4095u;ctx.pc_offset=offsetof(SetPRS,pc);ctx.cr_offset=offsetof(SetPRS,cr);ctx.lr_offset=offsetof(SetPRS,spr)+SPR::LR*4;ctx.disabled=1;ctx.reject=uintptr_t(&sheep_cpu_loop_reject);ctx.memory_bail=uintptr_t(&sheep_jit_memory_bail);ctx.power=uintptr_t(&power_on);ctx.itlb=uintptr_t(&pCurITLB1);ctx.mask=uintptr_t(&tlb_size_mask);ctx.rom_begin=uintptr_t(host);ctx.rom_end=uintptr_t(host+bytes);ctx.opcodes=uintptr_t(OpcodeGrabberNoFPU);ctx.allow_ram=1;ctx.window=window;
     const auto payload=std::vector<uint8_t>(host,host+bytes);std::vector<uint64_t> words(words_count);std::vector<uint8_t> counters(words_count);for(unsigned i=0;i<words_count;i++){auto slot=sheep_jit_filter_slot_rom(host+4*i);words[i]=sheep_rom_dispatch_words[slot];counters[i]=sheep_jit_reject_filter[slot];}
     Result out{};out.ctx=ctx;
     if(which<3){auto fn=which==0?&sheep_rom_window_reference_a64:which==1?&sheep_generic_continuation_reference_a64:&sheep_cpu_loop_a64;out.abi=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(fn),reinterpret_cast<nw_jit_cpu*>(&out.ctx));}
     else if(which<6){
      if(which==4)ppc_exec_inner<main,big_end,true,2>(0,0);else if(which==5)ppc_exec_inner<main,big_end,true,3>(0,0);
      else{unsigned steps=0;while(power_on&&steps<128){const auto *code=mmu_translate_imem(ppc_state.pc);ppc_main_opcode(ppc_opcode_grabber,ppc_read_instruction(code));++g_icycles;if(exec_flags){ppc_state.pc=ppc_next_instruction_address;exec_flags=0;}else ppc_state.pc+=4;++steps;}need(!power_on,"bounded independent outer canonical stop");}
     }else{
      if(foreign_cpu)std::memcpy(&ppc_state,&input,sizeof(input));
      for(unsigned step=0;step<expected_retired;step++){const auto *code=mmu_translate_imem(ppc_state.pc);ppc_main_opcode(ppc_opcode_grabber,ppc_read_instruction(code));if(step+1==expected_retired)break;ppc_state.pc=exec_flags==EXEF_BRANCH?ppc_next_instruction_address:ppc_state.pc+4u;exec_flags=0;++g_icycles;}
     }
     std::memcpy(&out.cpu,foreign_cpu&&which<3?&foreign:&ppc_state,sizeof(out.cpu));std::memcpy(&out.global,&ppc_state,sizeof(out.global));out.ram=ram;std::memcpy(out.seen,frame.seen,sizeof(out.seen));std::memcpy(out.primary.data(),pCurITLB1,out.primary.size());out.window=*sheep_mmu_rom_window_get();out.clock=g_icycles;out.flags=exec_flags;out.next=ppc_next_instruction_address;out.timer=exec_timer;out.power=power_on;out.bail=sheep_jit_memory_bail;out.calls=frame.calls;out.mask=tlb_size_mask;out.im=CurITLBMode;out.dm=CurDTLBMode;out.i1=uintptr_t(pCurITLB1);out.i2=uintptr_t(pCurITLB2);out.spans=sheep_steady_spans;out.insns=sheep_steady_instructions;std::memcpy(&out.target_tag,reinterpret_cast<uint8_t*>(pCurITLB1)+size_t((target>>12)&tlb_size_mask)*32,4);out.primary_target=uintptr_t(sheep_itlb_primary_hit(target))==uintptr_t(host)+uint32_t(target-base);out.metadata=std::equal(payload.begin(),payload.end(),host);for(unsigned i=0;i<words_count;i++){auto slot=sheep_jit_filter_slot_rom(host+4*i);out.metadata=out.metadata&&words[i]==sheep_rom_dispatch_words[slot]&&counters[i]==sheep_jit_reject_filter[slot];}
     arm_cleanup();return out;
    }catch(...){arm_cleanup();throw;}
   };
   auto architecture=[&](const Result&a,const Result&b){return !std::memcmp(&a.cpu,&b.cpu,sizeof(SetPRS))&&a.ram==b.ram&&!std::memcmp(a.seen,b.seen,sizeof(a.seen))&&a.clock==b.clock&&a.flags==b.flags&&a.next==b.next&&a.calls==b.calls&&a.timer==b.timer&&a.power==b.power&&a.bail==b.bail&&a.mask==b.mask&&a.im==b.im&&a.dm==b.dm&&a.i1==b.i1&&a.i2==b.i2;};
   if(group==5){const auto reference=arm(3),old=arm(4),actual=arm(5);need(architecture(old,actual)&&architecture(reference,actual),"actual outer fresh-canonical versus legacy/window C++ fullCPU/clock/observer");need(actual.spans>0&&actual.insns>0,"actual outer window ASM selected");if(variant==0)need(actual.cpu.gpr[6]==21&&actual.calls==1&&actual.primary_target,"cold0 target canonical refill before classification");else need(actual.calls==1&&!actual.window.valid_start_count&&!actual.primary_target&&actual.target_tag==((target&~4095u)^4096u),"pending1 invalidator/power observed before any refresh or target fetch");}
   else{
    const auto old=arm(0),frozen=arm(1),actual=arm(2);const bool distinct=group==0&&mode==3||group==3&&variant==1;
    if(distinct){need(old.ctx.retired==n&&frozen.ctx.retired==n&&uint32_t(old.abi)==1&&uint32_t(frozen.abi)==1&&old.calls==(group==3?1:0),"poisoned primary really refuses both frozen loops");const auto legacy=arm(0,true);need(architecture(legacy,actual)&&!std::memcmp(&legacy.ctx,&actual.ctx,sizeof(SheepLoopContext))&&legacy.abi==actual.abi,"real window bypass equals valid legacy mapping fullCPU/context/ABI");need(!actual.primary_target&&actual.target_tag==((target&~4095u)^4096u)&&actual.calls==(group==3?2:1),"window executes target observer without filling poisoned primary");}
    else need(architecture(old,actual)&&architecture(frozen,actual)&&!std::memcmp(&old.ctx,&actual.ctx,sizeof(SheepLoopContext))&&!std::memcmp(&frozen.ctx,&actual.ctx,sizeof(SheepLoopContext))&&old.abi==actual.abi&&frozen.abi==actual.abi&&old.primary==actual.primary&&frozen.primary==actual.primary,"exact frozen333/288/new fullCPU/context/MMU/RAM/NIA/clock/ABI");
    need(!(actual.abi>>32)&&uint32_t(actual.abi)==expected_pending&&actual.ctx.retired==expected_retired&&old.metadata&&frozen.metadata&&actual.metadata,"independent retirement/return/source-descriptor integrity");if(foreign_cpu)need(!std::memcmp(&old.global,&actual.global,sizeof(SetPRS))&&!std::memcmp(&frozen.global,&actual.global,sizeof(SetPRS)),"foreign context never writes global CPU");const auto canonical=arm(6,true);need(architecture(canonical,actual),"independent per-op fresh-translation fullCPU/clock/flags/callback oracle");if(group==4)need(actual.cpu.gpr[6]==21&&actual.calls==2&&!actual.window.valid_start_count,"actual IBAT revocation chooses new physical ROM+ADDI2");
   }
   ++passed;
  };
  for(unsigned f=0;f<6;f++)for(uint32_t base:{0x10000000u,0xfffe0000u})for(unsigned v=0;v<5;v++){trial(0,f,base,v);++basic;}
  for(unsigned f:{0u,2u,3u,5u})for(uint32_t base:{0x10000000u,0xfffe0000u})for(unsigned v=0;v<8;v++){trial(1,f,base,v);++polls;}
  for(unsigned f:{0u,2u,3u})for(uint32_t base:{0x10000000u,0xfffe0000u})for(unsigned v=0;v<2;v++){trial(2,f,base,v);++caps;}
  for(uint32_t base:{0x10000000u,0xfffe0000u})for(unsigned v=0;v<6;v++){trial(3,0,base,v);++bounds;}
  for(unsigned f=0;f<6;f++)for(uint32_t base:{0x10000000u,0xfffe0000u}){trial(4,f,base,0);++revocation;}
  for(uint32_t base:{0x10000000u,0xfffe0000u})for(unsigned v=0;v<2;v++){trial(5,0,base,v);++outer;}
  need(basic==60&&polls==64&&caps==12&&bounds==12&&revocation==12&&outer==4&&passed==164,"bounded window loop integration census");
 }catch(...){cleanup();throw;}
 cleanup();fprintf(stderr,"ROM WINDOW LOOP SELFTEST %u raw/frozen333-288/canonical/BAT/foreign/bounds/revocation/cold0/pending1/ABI cases passed\n",passed);return 1;
#else
 return 0;
#endif
}
