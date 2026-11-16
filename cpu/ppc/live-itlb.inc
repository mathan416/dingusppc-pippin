// Project: DingusPPC-Pippin
// File: cpu/ppc/live-itlb.inc
// Purpose: Live instruction TLB used by the JIT dispatcher.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// Q164 private fixture; include after page-guards.inc. Reference instruction
// and directive text is frozen Q161 ASM with symbol/local-label renames only.
#if defined(__aarch64__)
extern "C" unsigned sheep_live_itlb_reference_a64(SheepLoopContext*);
extern TLBEntry *pCurITLB2,*pCurDTLB1,*pCurDTLB2;
extern uint8_t CurITLBMode,CurDTLBMode;
asm(R"(
.text
.p2align 4
.global sheep_live_itlb_reference_a64
.type sheep_live_itlb_reference_a64,%function
sheep_live_itlb_reference_a64:
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
 cbnz w9,.Lli_ref_cold
.Lli_ref_fetch:
 tbnz x20,#0,.Lli_ref_fetch_ram
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 // Decode only the descriptor tag bits; preserve opcode, metadata and index.
 tbz w25,#1,.Lli_ref_low_tag
 tbnz w25,#0,.Lli_ref_native
 // Match the canonical retry counter, including its final blocked iteration.
 ldr x11,[x28,#16]
 ldrb w9,[x11,x10]
 cbz w9,.Lli_ref_retry_zero
 cmp w9,#255
 b.eq .Lli_ref_scalar
 sub w9,w9,#1
 strb w9,[x11,x10]
 cbnz w9,.Lli_ref_scalar
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lli_ref_scalar
.Lli_ref_fetch_ram:
 // Writable code is fetched anew for every instruction. No native RAM cache.
 ldur w0,[x20,#-1]
 rev w0,w0
 lsl x25,x0,#32
 b .Lli_ref_scalar
.Lli_ref_retry_zero:
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lli_ref_cold
.Lli_ref_native:
 lsr x9,x25,#32
 cmp w9,w24
 b.ne .Lli_ref_cold
 ubfx w9,w25,#2,#5
 add w9,w9,#1
 sub x10,x23,x22
 cmp x9,x10
 b.hi .Lli_ref_cold
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
 b.hi .Lli_ref_trap
 ldr w25,[x19,#256]
 cmp w26,#4
 mov w14,#1024
 b.hs .Lli_ref_native_fast
 // Successful short branches need no rejection or interpreter round-trip.
 // Zero/failed-guard prefixes retain their original slow-path backoff.
 cbz w26,.Lli_ref_native_short_guard
 ldr x9,[x28,#128]
 ldrb w9,[x9]
 mov w14,#128
 cbz w9,.Lli_ref_native_fast
.Lli_ref_native_short_guard:
 mov x0,x20
 mov w1,w24
 ldr x9,[x28,#120]
 blr x9
 cbnz w26,.Lli_ref_native_prefix
 str w24,[x19,#256]
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 b .Lli_ref_scalar
.Lli_ref_native_fast:
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
.global sheep_live_itlb_reference_guard_native
.hidden sheep_live_itlb_reference_guard_native
sheep_live_itlb_reference_guard_native:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lli_ref_native_fast_old
 // Preserve the canonical last branch target even when consuming the branch
 // locally. It remains observable at later event and exception boundaries.
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lli_ref_native_fast_sequential
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
.Lli_ref_native_fast_sequential:
 add w27,w27,w26
 str x22,[x29]
 cmp x22,x23
 b.hs .Lli_ref_native_fast_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lli_ref_native_fast_pending
 cmp w27,w14
 b.hs .Lli_ref_native_fast_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lli_ref_native_fast_pending
 // No device/event polling is removed: deadlines, external timer and power
 // are checked after every bounded block, as in the original path.
 sub w9,w25,w24
 sxtw x9,w9
 add x20,x20,x9
 mov w24,w25
 add x22,x22,#1
 str x22,[x29]
 // The native block already stored this exact target into CPU.pc.
 b .Lli_ref_fetch
.Lli_ref_native_fast_old:
 str w24,[x19,#256]
 b .Lli_ref_native_prefix_done
.Lli_ref_native_fast_pending:
 str w24,[x19,#256]
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lli_ref_pending
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lli_ref_pending
.Lli_ref_native_prefix:
 sub w9,w26,#1
 add x20,x20,w9,uxtw #2
 add w24,w24,w9,lsl #2
 add x22,x22,x9
 str w24,[x19,#256]
.Lli_ref_native_prefix_done:
 add w9,w24,#4
 cmp w25,w9
 b.ne .Lli_ref_native_branch
 // Target is already proven to be source+4. Only target bits2..11==0 cross a
 // page, including the 32-bit address wrap from fffffffc to zero.
.global sheep_live_itlb_reference_guard_next
.hidden sheep_live_itlb_reference_guard_next
sheep_live_itlb_reference_guard_next:
 tst w25,#0xffc
 b.ne .Lli_ref_after
.Lli_ref_native_branch:
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
 b .Lli_ref_after
.Lli_ref_low_tag:
 // Scalar tag1 falls through; native/retry routes keep their old branches.
 tbz w25,#0,.Lli_ref_cold
.Lli_ref_scalar:
 mov w26,#1
 lsr x0,x25,#32
 lsr w9,w0,#26
 cmp w9,#18
 b.eq .Lli_ref_b
 cmp w9,#16
 b.eq .Lli_ref_bc
.Lli_ref_handler:
 tbnz x20,#0,.Lli_ref_ram_handler
 // The packed descriptor stores a signed handler offset and FPU flag.
 ldr w9,[x28,#116]
 cbz w9,.Lli_ref_normal_handler
 tbz w25,#31,.Lli_ref_normal_handler
 ldr x9,[x28,#40]
 b .Lli_ref_call_handler
.Lli_ref_normal_handler:
 lsl w9,w25,#1
 asr w9,w9,#3
 ldr x10,[x28,#32]
 add x9,x10,w9,sxtw #2
 b .Lli_ref_call_handler
.Lli_ref_ram_handler:
 // Exactly the canonical ppc_main_opcode index and selected FPU table.
 lsr w9,w0,#15
 and w9,w9,#0x1f800
 and w10,w0,#0x7ff
 orr w9,w9,w10
 ldr x10,[x28,#184]
 ldr x9,[x10,w9,uxtw #3]
 b .Lli_ref_call_handler
.Lli_ref_call_handler:
 blr x9
 ldr x22,[x29]
 b .Lli_ref_after
.Lli_ref_b:
 tbnz w0,#1,.Lli_ref_handler
 tbz w0,#0,.Lli_ref_b_target
 add w9,w24,#4
 ldr w10,[x28,#112]
 str w9,[x19,x10]
.Lli_ref_b_target:
 sbfx w9,w0,#0,#26
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lli_ref_native_branch
.Lli_ref_bc:
 tst w0,#3
 b.ne .Lli_ref_handler
 ubfx w9,w0,#21,#5
 and w10,w9,#30
 cmp w10,#4
 b.eq .Lli_ref_bc_cr
 cmp w10,#12
 b.ne .Lli_ref_handler
.Lli_ref_bc_cr:
 ldr w10,[x28,#108]
 ldr w10,[x19,x10]
 ubfx w11,w0,#16,#5
 mov w12,#31
 sub w11,w12,w11
 lsr w10,w10,w11
 and w10,w10,#1
 ubfx w9,w9,#3,#1
 cmp w9,w10
 b.ne .Lli_ref_after
 sxth w9,w0
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lli_ref_native_branch
.Lli_ref_after:
 // Keep the final pre-increment value for ppc_exec's existing event check.
 str x22,[x29]
 add w27,w27,w26
 cmp x22,x23
 b.hs .Lli_ref_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lli_ref_pending
 cmp w27,#128
 b.hs .Lli_ref_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lli_ref_pending
 .word 0xb84003a9 | (((exec_flags-g_icycles)&0x1ff)<<12)
 cbz w9,.Lli_ref_advance
 cmp w9,#1
 b.ne .Lli_ref_pending
 ldr x10,[x28,#64]
 .word 0xb84003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 // Scalar callbacks cannot change the loop's mapped source W24. Keep
 // arbitrary targets subject to the exact existing primary-ITLB mapper.
.global sheep_live_itlb_reference_guard_branch
.hidden sheep_live_itlb_reference_guard_branch
sheep_live_itlb_reference_guard_branch:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lli_ref_map_target
 // Only a plain same-page branch can be consumed inside the fast loop.
 sub w9,w25,w24
 sxtw x9,w9
 add x20,x20,x9
 mov w24,w25
 str wzr,[x10]
 b .Lli_ref_continue
.Lli_ref_advance:
.global sheep_live_itlb_reference_guard_advance
.hidden sheep_live_itlb_reference_guard_advance
sheep_live_itlb_reference_guard_advance:
 add w10,w24,#4
 // Source+4 leaves its page iff target bits2..11 are zero, even unaligned.
 tst w10,#0xffc
 b.ne .Lli_ref_advance_same
 mov w25,w10
 ldr x10,[x28,#64]
 b .Lli_ref_map_target
.Lli_ref_advance_same:
 add w24,w24,#4
 add x20,x20,#4
.Lli_ref_continue:
 add x22,x22,#1
 str x22,[x29]
 str w24,[x19,#256]
 b .Lli_ref_fetch
.Lli_ref_map_target:
 // A primary ITLB hit already carries the canonical translated host offset.
 // Never walk/fill an MMU entry speculatively. Non-memory targets return to C++.
 ldr x9,[x28,#152]
 ldr x9,[x9]
 ldr x11,[x28,#160]
 ldr w11,[x11]
 lsr w12,w25,#12
 and w11,w11,w12
 add x9,x9,x11,lsl #5
 // Read only canonical entry metadata early, never guest payload.
 ldp x11,x9,[x9]
 and w12,w25,#0xfffff000
 cmp w11,w12
 b.ne .Lli_ref_pending
 tbz x11,#32,.Lli_ref_pending
 add x9,x9,w25,uxtw
 ldr x11,[x28,#168]
 cmp x9,x11
 b.lo .Lli_ref_target_ram
 ldr x11,[x28,#176]
 cmp x9,x11
 b.hs .Lli_ref_target_ram
 mov x20,x9
 str wzr,[x28,#192]
 b .Lli_ref_mapped
.Lli_ref_target_ram:
 ldr w11,[x28,#196]
 cbz w11,.Lli_ref_pending
 tst x9,#3
 b.ne .Lli_ref_pending
 orr x20,x9,#1
 mov w11,#1
 str w11,[x28,#192]
.Lli_ref_mapped:
 mov w24,w25
 str w12,[x28,#96]
 add w11,w12,#4095
 str w11,[x28,#100]
 str wzr,[x10]
 b .Lli_ref_continue
.Lli_ref_pending:
 mov w0,#1
 b .Lli_ref_return
.Lli_ref_cold:
 mov w0,#0
.Lli_ref_return:
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
.Lli_ref_trap:
 brk #0
.cfi_endproc
.size sheep_live_itlb_reference_a64,.-sheep_live_itlb_reference_a64
)");
// Q175 callback contract only: deliberately changes the canonical primary
// pointer/mask without an instruction-context flag. Real MTMSR now correctly
// requests FETCH and is covered independently by FETCH BARRIER SELFTEST24.
static void live_itlb_flag_neutral_rebind(uint32_t){
 const uintptr_t address=uintptr_t(ppc_state.gpr[30])|(uintptr_t(ppc_state.gpr[31])<<32);
 pCurITLB1=reinterpret_cast<TLBEntry*>(address);tlb_size_mask=ppc_state.gpr[29];
}
int sheep_live_itlb_selftest(){
 const auto saved_cpu=ppc_state;const auto saved_flags=exec_flags,saved_next=ppc_next_instruction_address;const auto saved_clock=g_icycles;
 const bool saved_timer=exec_timer,saved_power=power_on,saved_bail=sheep_jit_memory_bail;
 auto*const saved_i1=pCurITLB1;auto*const saved_i2=pCurITLB2;auto*const saved_d1=pCurDTLB1;auto*const saved_d2=pCurDTLB2;
 const auto saved_mask=tlb_size_mask;const auto saved_imode=CurITLBMode,saved_dmode=CurDTLBMode;
 alignas(4096) std::array<uint8_t,12288> code{};alignas(4096) std::array<uint8_t,4096> ram{};alignas(32) std::array<uint8_t,4096*32> table{};
 std::array<uint64_t,3072> words{};std::array<uint8_t,3072> counters{};unsigned passed=0;bool okay=false;
 struct Patch{uint8_t*slot=nullptr;std::array<uint8_t,32> old{};};std::array<Patch,3> patches{};
 for(unsigned i=0;i<3072;i++){unsigned slot=sheep_jit_filter_slot_rom(code.data()+4*i);words[i]=sheep_rom_dispatch_words[slot];counters[i]=sheep_jit_reject_filter[slot];}
 auto require=[&](bool good,const char*why){if(!good){fprintf(stderr,"LIVE ITLB failure case=%u %s\n",passed+1,why);throw std::runtime_error("Q164 live ITLB fixture failed");}};
 auto restore=[&](){for(auto&p:patches)if(p.slot){std::memcpy(p.slot,p.old.data(),32);p.slot=nullptr;}for(unsigned i=0;i<3072;i++){const unsigned slot=sheep_jit_filter_slot_rom(code.data()+4*i);sheep_rom_dispatch_words[slot]=words[i];sheep_jit_reject_filter[slot]=counters[i];}ppc_state=saved_cpu;pCurITLB1=saved_i1;pCurITLB2=saved_i2;pCurDTLB1=saved_d1;pCurDTLB2=saved_d2;tlb_size_mask=saved_mask;CurITLBMode=saved_imode;CurDTLBMode=saved_dmode;nw_jit_layout layout{};sheep_mmu_jit_layout(&layout);exec_flags=saved_flags;ppc_next_instruction_address=saved_next;g_icycles=saved_clock;exec_timer=saved_timer;power_on=saved_power;sheep_jit_memory_bail=saved_bail;};
 auto put=[&](unsigned offset,uint32_t op){for(unsigned j=0;j<4;j++)code[offset+j]=uint8_t(op>>(24-j*8));uint32_t desc;require(sheep_encode_rom_handler(op,desc),"canonical descriptor encoding");auto slot=sheep_jit_filter_slot_rom(code.data()+offset);sheep_rom_dispatch_words[slot]=(uint64_t(op)<<32)|desc|1;sheep_jit_reject_filter[slot]=255;};
 auto map=[&](uint8_t*entries,uint32_t mask,uint32_t pc,uint8_t*host,unsigned flags,bool miss){auto*p=entries+size_t((pc>>12)&mask)*32;const uint32_t page=(pc&~4095u)^(miss?4096u:0u);const uint16_t bits=uint16_t(flags);const int64_t offset=int64_t(reinterpret_cast<uintptr_t>(host))-int64_t(pc&~4095u);std::memcpy(p,&page,4);std::memcpy(p+4,&bits,2);std::memcpy(p+8,&offset,8);};
 auto context=[&](uint8_t*source,uint32_t pc,uint64_t deadline){SheepLoopContext c{};c.cpu=reinterpret_cast<uintptr_t>(&ppc_state);c.words=reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words);c.counters=reinterpret_cast<uintptr_t>(sheep_jit_reject_filter);c.native_base=reinterpret_cast<uintptr_t>(nw_jit_native_base());c.illegal=reinterpret_cast<uintptr_t>(&ppc_illegalop);c.fpu_disabled=reinterpret_cast<uintptr_t>(sheep_rom_disabled_fpu_handler);c.timer=reinterpret_cast<uintptr_t>(&exec_timer);c.clock=reinterpret_cast<uintptr_t>(&g_icycles);c.flags=reinterpret_cast<uintptr_t>(&exec_flags);c.next=reinterpret_cast<uintptr_t>(&ppc_next_instruction_address);c.max_cycles=deadline;c.host=source;c.page_start=pc&~4095u;c.page_end=c.page_start+4095;c.pc_offset=offsetof(SetPRS,pc);c.cr_offset=offsetof(SetPRS,cr);c.lr_offset=offsetof(SetPRS,spr)+SPR::LR*4;c.disabled=ppc_opcode_grabber==OpcodeGrabberNoFPU;c.reject=reinterpret_cast<uintptr_t>(&loop_fixture_reject);c.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);c.power=reinterpret_cast<uintptr_t>(&power_on);c.itlb=reinterpret_cast<uintptr_t>(&pCurITLB1);c.mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);c.rom_begin=reinterpret_cast<uintptr_t>(code.data());c.rom_end=c.rom_begin+code.size();c.opcodes=reinterpret_cast<uintptr_t>(ppc_opcode_grabber);c.allow_ram=1;return c;};
 struct Result{SetPRS cpu;SheepLoopContext context;uint64_t result,clock;uint32_t flags,next;bool timer,power,bail;uintptr_t itlb;uint32_t mask;};
 auto run=[&](unsigned(*loop)(SheepLoopContext*),const SetPRS&initial,SheepLoopContext ctx){require(ctx.itlb==reinterpret_cast<uintptr_t>(&pCurITLB1)&&ctx.mask==reinterpret_cast<uintptr_t>(&tlb_size_mask),"private canonical globals binding");ppc_state=initial;exec_flags=0;ppc_next_instruction_address=0x31415920;g_icycles=123;exec_timer=false;power_on=true;sheep_jit_memory_bail=false;Result out{};out.result=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(loop),reinterpret_cast<nw_jit_cpu*>(&ctx));std::memcpy(&out.cpu,&ppc_state,sizeof(ppc_state));out.context=ctx;out.clock=g_icycles;out.flags=exec_flags;out.next=ppc_next_instruction_address;out.timer=exec_timer;out.power=power_on;out.bail=sheep_jit_memory_bail;out.itlb=reinterpret_cast<uintptr_t>(pCurITLB1);out.mask=tlb_size_mask;return out;};
 auto equal=[&](const Result&a,const Result&b){return a.result==b.result&&!(a.result>>32)&&!std::memcmp(&a.cpu,&b.cpu,sizeof(a.cpu))&&!std::memcmp(&a.context,&b.context,sizeof(a.context))&&a.clock==b.clock&&a.flags==b.flags&&a.next==b.next&&a.timer==b.timer&&a.power==b.power&&a.bail==b.bail&&a.itlb==b.itlb&&a.mask==b.mask;};
 alignas(32) std::array<uint8_t,4096*32> rebound_table{};
 try{
  //96 actual mapper traversals/declines with canonical source/current globals.
  const uint32_t masks[]={0u,5u,4095u};
  for(unsigned wrap=0;wrap<2;wrap++)for(unsigned sequential=0;sequential<2;sequential++)for(unsigned alias=0;alias<2;alias++)for(unsigned kind=0;kind<4;kind++)for(uint32_t mask:masks){
   for(unsigned i=0;i<3072;i++)put(i*4,0x60000000u);table.fill(0);for(unsigned i=0;i<4096;i++){const uint32_t bad=0xffffffffu;std::memcpy(table.data()+i*32,&bad,4);}for(unsigned i=0;i<4096;i+=4){const uint32_t op=0x38630009u;for(unsigned j=0;j<4;j++)ram[i+j]=uint8_t(op>>(24-j*8));}
   const uint32_t page=wrap?0xfffff000u:0x10000000u,pc=page+(sequential?4092u:0u),target=page+4096u;const unsigned target_offset=alias?8192:4096;put(target_offset,0x38630007u);if(!sequential)put(0,0x48001000u);
   uint8_t*host=kind==3?ram.data():code.data()+target_offset;map(table.data(),mask,target,host,kind==2?0:1,kind==1);pCurITLB1=reinterpret_cast<TLBEntry*>(table.data());tlb_size_mask=mask;
   SetPRS initial=saved_cpu;initial.pc=pc;initial.gpr[3]=17;auto ctx=context(code.data()+(sequential?4092:0),pc,124);
   const auto before_code=code;const auto before_ram=ram;const auto before_table=table;auto reference=run(&sheep_live_itlb_reference_a64,initial,ctx);auto candidate=run(&sheep_cpu_loop_a64,initial,ctx);
   require(equal(candidate,reference)&&code==before_code&&ram==before_ram&&table==before_table,"full raw old/new CPU/RAM/context/metadata/ABI");
   const bool hit=kind==0||kind==3;require(uint32_t(candidate.result)==1&&candidate.context.retired==(hit?2u:1u)&&!candidate.context.native_used,"actual mapper retirement/handoff path");
   // Canonical replay retains final pre-step CPU/flags/NIA and preincrement clock.
   ppc_state=initial;exec_flags=0;ppc_next_instruction_address=0x31415920;g_icycles=123;
   ppc_main_opcode(ppc_opcode_grabber,sequential?0x60000000u:0x48001000u);
   if(hit){ppc_state.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:uint32_t(ppc_state.pc+4);exec_flags=0;g_icycles++;ppc_main_opcode(ppc_opcode_grabber,kind==3?0x38630009u:0x38630007u);}
   require(!std::memcmp(&candidate.cpu,&ppc_state,sizeof(ppc_state))&&candidate.flags==exec_flags&&candidate.next==ppc_next_instruction_address&&candidate.clock==g_icycles,"canonical complete CPU/rawflags/NIA/clock oracle");passed++;
  }
  // A synthetic flag-neutral callback changes both live mapper inputs. This
  // retains the dispatcher contract without asserting obsolete MTMSR flags0.
  // The separate frozen24-case real-handler oracle is not changed or relaxed.
  const uint32_t root=0x10000000u,target=root+4096,old_mask=4095,new_mask=0;
  auto*const old_table=reinterpret_cast<TLBEntry*>(table.data());
  auto*const new_table=reinterpret_cast<TLBEntry*>(rebound_table.data());
  table.fill(0);for(unsigned i=0;i<4096;i++){const uint32_t invalid=0xffffffffu;std::memcpy(table.data()+i*32,&invalid,4);std::memcpy(rebound_table.data()+i*32,&invalid,4);}
  for(unsigned i=0;i<3072;i++)put(i*4,0x60000000u);put(4,0x48000ffcu);put(4096,0x38c60002u);put(8192,0x38c60001u);
  const intptr_t callback_delta=intptr_t(reinterpret_cast<uintptr_t>(&live_itlb_flag_neutral_rebind))-intptr_t(reinterpret_cast<uintptr_t>(&ppc_illegalop));
  require(!(callback_delta&3)&&callback_delta>=-(int64_t(1)<<30)&&callback_delta<(int64_t(1)<<30),"flag-neutral handler packed range");
  const auto callback_slot=sheep_jit_filter_slot_rom(code.data());
  sheep_rom_dispatch_words[callback_slot]=(uint64_t(0x60000000u)<<32)|(uint32_t(callback_delta)&0x7ffffffcu)|1u;sheep_jit_reject_filter[callback_slot]=255;
  map(table.data(),old_mask,root,code.data(),1,false);map(table.data(),old_mask,target,code.data()+8192,1,false);map(rebound_table.data(),new_mask,target,code.data()+4096,1,false);
  SetPRS initial=saved_cpu;initial.pc=root;initial.gpr[6]=17;initial.gpr[29]=new_mask;const uintptr_t new_address=reinterpret_cast<uintptr_t>(new_table);initial.gpr[30]=uint32_t(new_address);initial.gpr[31]=uint32_t(new_address>>32);
  const auto before_old=table;const auto before_new=rebound_table;const auto before_code=code;auto ctx=context(code.data(),root,125);
  auto select_old=[&](){pCurITLB1=old_table;tlb_size_mask=old_mask;};
  select_old();auto reference=run(&sheep_live_itlb_reference_a64,initial,ctx);select_old();auto candidate=run(&sheep_cpu_loop_a64,initial,ctx);
  require(equal(candidate,reference)&&candidate.context.retired==3&&uint32_t(candidate.result)==1&&candidate.cpu.gpr[6]==19&&candidate.itlb==new_address&&candidate.mask==new_mask,"flag-neutral callback followed by fresh primary pointer/mask map");
  // Deliberately bind the frozen reference to snapshots instead of globals.
  // This is a negative oracle only, never a supported production context.
  select_old();ppc_state=initial;exec_flags=0;ppc_next_instruction_address=0x31415920;g_icycles=123;exec_timer=false;power_on=true;sheep_jit_memory_bail=false;
  auto stale=ctx;stale.itlb=reinterpret_cast<uintptr_t>(&old_table);stale.mask=reinterpret_cast<uintptr_t>(&old_mask);
  const uint64_t stale_result=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(&sheep_live_itlb_reference_a64),reinterpret_cast<nw_jit_cpu*>(&stale));
  SetPRS stale_expected;std::memcpy(&stale_expected,&candidate.cpu,sizeof(stale_expected));stale_expected.gpr[6]=18;
  auto stale_context_expected=candidate.context;stale_context_expected.itlb=reinterpret_cast<uintptr_t>(&old_table);stale_context_expected.mask=reinterpret_cast<uintptr_t>(&old_mask);stale_context_expected.host=code.data()+8192;
  require(!(stale_result>>32)&&uint32_t(stale_result)==1&&!std::memcmp(&ppc_state,&stale_expected,sizeof(ppc_state))&&!std::memcmp(&stale,&stale_context_expected,sizeof(stale))&&g_icycles==candidate.clock&&exec_flags==candidate.flags&&ppc_next_instruction_address==candidate.next&&exec_timer==candidate.timer&&power_on==candidate.power&&sheep_jit_memory_bail==candidate.bail&&pCurITLB1==new_table&&tlb_size_mask==new_mask,"snapshot negative selects old opcode with otherwise exact CPU/context/clock/ABI");
  // Replay the explicit callback, then canonical B and ADDI, with a live hit.
  select_old();ppc_state=initial;exec_flags=0;ppc_next_instruction_address=0x31415920;g_icycles=123;live_itlb_flag_neutral_rebind(0x60000000u);require(exec_flags==0&&pCurITLB1==new_table&&tlb_size_mask==new_mask,"synthetic callback is actually flag-neutral");ppc_state.pc+=4;g_icycles++;ppc_main_opcode(ppc_opcode_grabber,0x48000ffcu);ppc_state.pc=ppc_next_instruction_address;exec_flags=0;g_icycles++;require(sheep_itlb_primary_hit(ppc_state.pc)==code.data()+4096,"canonical live target host");ppc_main_opcode(ppc_opcode_grabber,0x38c60002u);
  require(!std::memcmp(&candidate.cpu,&ppc_state,sizeof(ppc_state))&&candidate.flags==exec_flags&&candidate.next==ppc_next_instruction_address&&candidate.clock==g_icycles&&code==before_code&&table==before_old&&rebound_table==before_new,"flag-neutral replay/fullCPU/rawflags/NIA/clock/source/table nonmutation");passed++;
  require(passed==97,"fixed mapper/flag-neutral-rebind case census");okay=true;
 }catch(const std::exception&e){fprintf(stderr,"LIVE ITLB exception: %s\n",e.what());}
 restore();if(okay)fprintf(stderr,"LIVE ITLB SELFTEST %u old/new ASM/fullCPU/RAM/mask/page/ABI/flag-neutral-rebind cases passed\n",passed);return okay?1:0;
}
#else
int sheep_live_itlb_selftest(){return 0;}
#endif
