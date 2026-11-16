// SPDX-License-Identifier: GPL-3.0-or-later
// Private ARM64 differential oracle: exact Q136 steady-loop assembly, with
// only symbol names changed. Its old context-page guards stay independent of
// the candidate; no function-size, instruction-offset or tag-route assumption.
#if defined(__aarch64__)
extern "C" unsigned sheep_page_guard_reference_a64(SheepLoopContext*);
extern "C" const uint32_t sheep_page_guard_native[],sheep_page_guard_next[],sheep_page_guard_branch[],sheep_page_guard_advance[];
asm(R"(
.text
.p2align 4
.global sheep_page_guard_reference_a64
.type sheep_page_guard_reference_a64,%function
sheep_page_guard_reference_a64:
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
 cbnz w9,.Lpg_ref_cold
.Lpg_ref_fetch:
 tbnz x20,#0,.Lpg_ref_fetch_ram
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 and w9,w25,#3
 cbz w9,.Lpg_ref_cold
 cmp w9,#3
 b.eq .Lpg_ref_native
 cmp w9,#2
 b.ne .Lpg_ref_scalar
 // Match the canonical retry counter, including its final blocked iteration.
 ldr x11,[x28,#16]
 ldrb w9,[x11,x10]
 cbz w9,.Lpg_ref_retry_zero
 cmp w9,#255
 b.eq .Lpg_ref_scalar
 sub w9,w9,#1
 strb w9,[x11,x10]
 cbnz w9,.Lpg_ref_scalar
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lpg_ref_scalar
.Lpg_ref_fetch_ram:
 // Writable code is fetched anew for every instruction. No native RAM cache.
 ldur w0,[x20,#-1]
 rev w0,w0
 lsl x25,x0,#32
 b .Lpg_ref_scalar
.Lpg_ref_retry_zero:
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lpg_ref_cold
.Lpg_ref_native:
 lsr x9,x25,#32
 cmp w9,w24
 b.ne .Lpg_ref_cold
 ubfx w9,w25,#2,#5
 add w9,w9,#1
 sub x10,x23,x22
 cmp x9,x10
 b.hi .Lpg_ref_cold
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
 b.hi .Lpg_ref_trap
 ldr w25,[x19,#256]
 cmp w26,#4
 mov w14,#1024
 b.hs .Lpg_ref_native_fast
 // Successful short branches need no rejection or interpreter round-trip.
 // Zero/failed-guard prefixes retain their original slow-path backoff.
 cbz w26,.Lpg_ref_native_short_guard
 ldr x9,[x28,#128]
 ldrb w9,[x9]
 mov w14,#128
 cbz w9,.Lpg_ref_native_fast
.Lpg_ref_native_short_guard:
 mov x0,x20
 mov w1,w24
 ldr x9,[x28,#120]
 blr x9
 cbnz w26,.Lpg_ref_native_prefix
 str w24,[x19,#256]
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 b .Lpg_ref_scalar
.Lpg_ref_native_fast:
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
 and w9,w25,#0xfffff000
 ldr w10,[x28,#96]
 cmp w9,w10
 b.ne .Lpg_ref_native_fast_old
 // Preserve the canonical last branch target even when consuming the branch
 // locally. It remains observable at later event and exception boundaries.
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lpg_ref_native_fast_sequential
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
.Lpg_ref_native_fast_sequential:
 add w27,w27,w26
 str x22,[x29]
 cmp x22,x23
 b.hs .Lpg_ref_native_fast_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lpg_ref_native_fast_pending
 cmp w27,w14
 b.hs .Lpg_ref_native_fast_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lpg_ref_native_fast_pending
 // No device/event polling is removed: deadlines, external timer and power
 // are checked after every bounded block, as in the original path.
 sub w9,w25,w24
 sxtw x9,w9
 add x20,x20,x9
 mov w24,w25
 add x22,x22,#1
 str x22,[x29]
 // The native block already stored this exact target into CPU.pc.
 b .Lpg_ref_fetch
.Lpg_ref_native_fast_old:
 str w24,[x19,#256]
 b .Lpg_ref_native_prefix_done
.Lpg_ref_native_fast_pending:
 str w24,[x19,#256]
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lpg_ref_pending
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lpg_ref_pending
.Lpg_ref_native_prefix:
 sub w9,w26,#1
 add x20,x20,w9,uxtw #2
 add w24,w24,w9,lsl #2
 add x22,x22,x9
 str w24,[x19,#256]
.Lpg_ref_native_prefix_done:
 add w9,w24,#4
 cmp w25,w9
 b.ne .Lpg_ref_native_branch
 and w9,w25,#0xfffff000
 ldr w10,[x28,#96]
 cmp w9,w10
 b.eq .Lpg_ref_after
.Lpg_ref_native_branch:
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
 b .Lpg_ref_after
.Lpg_ref_scalar:
 mov w26,#1
 lsr x0,x25,#32
 lsr w9,w0,#26
 cmp w9,#18
 b.eq .Lpg_ref_b
 cmp w9,#16
 b.eq .Lpg_ref_bc
.Lpg_ref_handler:
 tbnz x20,#0,.Lpg_ref_ram_handler
 // The packed descriptor stores a signed handler offset and FPU flag.
 ldr w9,[x28,#116]
 cbz w9,.Lpg_ref_normal_handler
 tbz w25,#31,.Lpg_ref_normal_handler
 ldr x9,[x28,#40]
 b .Lpg_ref_call_handler
.Lpg_ref_normal_handler:
 lsl w9,w25,#1
 asr w9,w9,#3
 ldr x10,[x28,#32]
 add x9,x10,w9,sxtw #2
 b .Lpg_ref_call_handler
.Lpg_ref_ram_handler:
 // Exactly the canonical ppc_main_opcode index and selected FPU table.
 lsr w9,w0,#15
 and w9,w9,#0x1f800
 and w10,w0,#0x7ff
 orr w9,w9,w10
 ldr x10,[x28,#184]
 ldr x9,[x10,w9,uxtw #3]
 b .Lpg_ref_call_handler
.Lpg_ref_call_handler:
 blr x9
 ldr x22,[x29]
 b .Lpg_ref_after
.Lpg_ref_b:
 tbnz w0,#1,.Lpg_ref_handler
 tbz w0,#0,.Lpg_ref_b_target
 add w9,w24,#4
 ldr w10,[x28,#112]
 str w9,[x19,x10]
.Lpg_ref_b_target:
 sbfx w9,w0,#0,#26
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lpg_ref_native_branch
.Lpg_ref_bc:
 tst w0,#3
 b.ne .Lpg_ref_handler
 ubfx w9,w0,#21,#5
 and w10,w9,#30
 cmp w10,#4
 b.eq .Lpg_ref_bc_cr
 cmp w10,#12
 b.ne .Lpg_ref_handler
.Lpg_ref_bc_cr:
 ldr w10,[x28,#108]
 ldr w10,[x19,x10]
 ubfx w11,w0,#16,#5
 mov w12,#31
 sub w11,w12,w11
 lsr w10,w10,w11
 and w10,w10,#1
 ubfx w9,w9,#3,#1
 cmp w9,w10
 b.ne .Lpg_ref_after
 sxth w9,w0
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lpg_ref_native_branch
.Lpg_ref_after:
 // Keep the final pre-increment value for ppc_exec's existing event check.
 str x22,[x29]
 add w27,w27,w26
 cmp x22,x23
 b.hs .Lpg_ref_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lpg_ref_pending
 cmp w27,#128
 b.hs .Lpg_ref_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lpg_ref_pending
 .word 0xb84003a9 | (((exec_flags-g_icycles)&0x1ff)<<12)
 cbz w9,.Lpg_ref_advance
 cmp w9,#1
 b.ne .Lpg_ref_pending
 ldr x10,[x28,#64]
 .word 0xb84003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 and w9,w25,#0xfffff000
 ldr w11,[x28,#96]
 cmp w9,w11
 b.ne .Lpg_ref_map_target
 // Only a plain same-page branch can be consumed inside the fast loop.
 sub w9,w25,w24
 sxtw x9,w9
 add x20,x20,x9
 mov w24,w25
 str wzr,[x10]
 b .Lpg_ref_continue
.Lpg_ref_advance:
 add w10,w24,#4
 and w9,w10,#0xfffff000
 ldr w11,[x28,#96]
 cmp w9,w11
 b.eq .Lpg_ref_advance_same
 mov w25,w10
 ldr x10,[x28,#64]
 b .Lpg_ref_map_target
.Lpg_ref_advance_same:
 add w24,w24,#4
 add x20,x20,#4
.Lpg_ref_continue:
 add x22,x22,#1
 str x22,[x29]
 str w24,[x19,#256]
 b .Lpg_ref_fetch
.Lpg_ref_map_target:
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
 b.ne .Lpg_ref_pending
 tbz x11,#32,.Lpg_ref_pending
 add x9,x9,w25,uxtw
 ldr x11,[x28,#168]
 cmp x9,x11
 b.lo .Lpg_ref_target_ram
 ldr x11,[x28,#176]
 cmp x9,x11
 b.hs .Lpg_ref_target_ram
 mov x20,x9
 str wzr,[x28,#192]
 b .Lpg_ref_mapped
.Lpg_ref_target_ram:
 ldr w11,[x28,#196]
 cbz w11,.Lpg_ref_pending
 tst x9,#3
 b.ne .Lpg_ref_pending
 orr x20,x9,#1
 mov w11,#1
 str w11,[x28,#192]
.Lpg_ref_mapped:
 mov w24,w25
 str w12,[x28,#96]
 add w11,w12,#4095
 str w11,[x28,#100]
 str wzr,[x10]
 b .Lpg_ref_continue
.Lpg_ref_pending:
 mov w0,#1
 b .Lpg_ref_return
.Lpg_ref_cold:
 mov w0,#0
.Lpg_ref_return:
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
.Lpg_ref_trap:
 brk #0
.cfi_endproc
.size sheep_page_guard_reference_a64,.-sheep_page_guard_reference_a64
)");

static uint32_t page_guard_callback_target;
static unsigned page_guard_callback_kind;
static uint8_t *page_guard_callback_ram;
static void page_guard_fixture_callback(uint32_t){
 ppc_state.gpr[11]^=0x5a39c187u;
 page_guard_callback_ram[3]^=0x5au;
 ppc_next_instruction_address=page_guard_callback_target;
 exec_flags=page_guard_callback_kind>=4?(EXEF_EXCEPTION|EXEF_BRANCH):EXEF_BRANCH;
 if(page_guard_callback_kind==5)ppc_state.pc=page_guard_callback_target^0x12345000u;
}

int sheep_page_guards_selftest(){
 const auto saved=ppc_state;const auto saved_flags=exec_flags,saved_next=ppc_next_instruction_address;
 const auto saved_clock=g_icycles;const bool saved_timer=exec_timer,saved_power=power_on;
 const auto saved_table=pCurITLB1;const auto saved_mask=tlb_size_mask;
 const bool saved_bail=sheep_jit_memory_bail,saved_used=sheep_hot_used;
 alignas(16) std::array<uint8_t,8192> code{};
 std::array<uint8_t,4096> ram{},initial_ram{};
 alignas(32) std::array<uint8_t,256> itlb{};
 std::array<uint64_t,2048> saved_words{};std::array<uint8_t,2048> saved_counters{};
 for(unsigned i=0;i<2048;i++){
  const auto slot=sheep_jit_filter_slot_rom(code.data()+i*4);
  saved_words[i]=sheep_rom_dispatch_words[slot];saved_counters[i]=sheep_jit_reject_filter[slot];
 }
 sheep_rom_direct_reset();nw_jit_reset();
 unsigned checks=0,current_kind=0,current_page=0,current_event=0,current_deadline=0;
 auto require=[&](bool okay,const char *what){
  ++checks;if(!okay)fprintf(stderr,"PAGE GUARDS failure check=%u kind=%u page=%08x event=%u deadline=%u: %s\n",
   checks,current_kind,current_page,current_event,current_deadline,what);return okay;
 };
 const bool okay=[&](){
  // Check lowered instructions at named sites, independent of surrounding code
  // offsets or another experiment's tag layout. B.NE imm19 may auto-relocate.
  auto bne=[](uint32_t op){return (op&0xff00001fu)==0x54000001u;};
  if(!require(sheep_page_guard_native[0]==0x4a180329u&&sheep_page_guard_native[1]==0x72144d3fu&&bne(sheep_page_guard_native[2])&&
    sheep_page_guard_next[0]==0x721e273fu&&bne(sheep_page_guard_next[1])&&
    sheep_page_guard_branch[0]==0x4a180329u&&sheep_page_guard_branch[1]==0x72144d3fu&&bne(sheep_page_guard_branch[2])&&
    sheep_page_guard_advance[0]==0x1100130au&&sheep_page_guard_advance[1]==0x721e255fu&&bne(sheep_page_guard_advance[2]),
    "actual lowered guards have 3/2/3/3 words, removing four page-context loads and six instructions"))return false;
  if(!sheep_direct_poll_layout())return false;
  nw_jit_layout layout={offsetof(SetPRS,gpr),offsetof(SetPRS,cr),offsetof(SetPRS,spr)+SPR::XER*4,
   offsetof(SetPRS,spr)+SPR::LR*4,offsetof(SetPRS,spr)+SPR::CTR*4,offsetof(SetPRS,pc),reinterpret_cast<void*>(1)};
  layout.leaf=1;layout.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);
  auto put=[&](unsigned offset,uint32_t op,uintptr_t callback=0){
   for(unsigned byte=0;byte<4;byte++)code[offset+byte]=uint8_t(op>>(24-byte*8));
   uint32_t description;
   if(callback){
    const uintptr_t delta=callback-reinterpret_cast<uintptr_t>(&ppc_illegalop);
    if(delta&3)return false;description=uint32_t(delta)&0x7ffffffcu;
   }else if(!sheep_encode_rom_handler(op,description))return false;
   const auto slot=sheep_jit_filter_slot_rom(code.data()+offset);
   sheep_rom_dispatch_words[slot]=(uint64_t(op)<<32)|description|1;sheep_jit_reject_filter[slot]=255;return true;
  };
  auto map=[&](uint32_t page,uint8_t *host){
   auto *entry=itlb.data()+((page>>12)&7)*32;
   const int64_t offset=int64_t(reinterpret_cast<uintptr_t>(host))-page;const uint16_t memory=1;
   std::memcpy(entry,&page,4);std::memcpy(entry+4,&memory,2);std::memcpy(entry+8,&offset,8);
  };
  const uint32_t pages[]={0x10000000u,0x7ffff000u,0x80000000u,0xfffff000u};
  const unsigned deadlines[]={0,4,127,1025};
  // All low12 source positions, including unaligned PCs and 32-bit wrap.
  // Production execution below uses aligned instruction addresses.
  bool arithmetic=true;
  for(uint32_t page:pages)for(unsigned offset=0;offset<4096;offset++){
   const uint32_t source=page+offset,target=source+4;
   arithmetic=arithmetic&&((((source^target)&0xfffff000u)==0)==((target&0xffcu)!=0));
   for(uint32_t other:pages){
    const uint32_t arbitrary=other+((offset*17u)&4095);
    arithmetic=arithmetic&&((((source^arbitrary)&0xfffff000u)==0)==((source&0xfffff000u)==(arbitrary&0xfffff000u)));
   }
  }
  if(!require(arithmetic,"all4096 offsets exact +4/XOR page equivalence, unaligned/sign/wrap"))return false;
  for(uint32_t page:pages)for(unsigned kind=0;kind<12;kind++)for(unsigned event=0;event<3;event++)for(unsigned deadline:deadlines){
   current_kind=kind;current_page=page;current_event=event;current_deadline=deadline;
   for(unsigned i=0;i<2048;i++)if(!put(i*4,0x60000000u))return false;
   for(unsigned i=0;i<8;i++){const uint32_t invalid=0xffffffffu;std::memcpy(itlb.data()+i*32,&invalid,4);}
   const bool native=kind>=6;
   const unsigned start=native?4080:kind==0?4092:256;
   const uint32_t root=page+start,other=(kind==3||kind==4||kind==5||kind==11)?(page^0x80000000u)+4096:page+4096;
   const uint32_t target=kind==1?page+64:kind==6||kind==10?root:kind==8?page+4092:
    kind==0||kind==9?other:other+64;
   map(page,code.data());map(other,code.data()+4096);
   pCurITLB1=reinterpret_cast<TLBEntry*>(itlb.data());tlb_size_mask=7;
   const unsigned target_offset=(target&0xfffff000u)==page?target-page:4096+(target-other);
   if(!put(target_offset,0x48000000u))return false; // scalar branch-to-self
   uint32_t ops[4]={0x38630001u,0x38840002u,0x38a50003u,0x60000000u};
   if(kind==0){if(!put(start,ops[0]))return false;}
   else if(kind==1||kind==2){if(!put(start,0x48000000u|((target-root)&0x03fffffcu)))return false;}
   else if(kind>=3&&kind<=5){if(!put(start,0x60000000u,reinterpret_cast<uintptr_t>(&page_guard_fixture_callback)))return false;}
   else{
    const unsigned actual=kind==6?1:kind==7?2:kind==8?3:4;
    if(kind==11)ops[3]=0x4e800020u; // BLR to an arbitrary signed/wrapped target
    else ops[actual-1]=0x41820000u|((target-(root+(actual-1)*4))&0xfffcu); // CR0 EQ taken
    if(kind==8)ops[3]=0x48000000u; // distinct scalar followup at branch-to-next
    for(unsigned i=0;i<4;i++)if(!put(start+i*4,ops[i]))return false;
    auto fn=nw_jit_compile_mapped(ops,4,root,&layout);
    const uintptr_t base=reinterpret_cast<uintptr_t>(nw_jit_native_base()),address=reinterpret_cast<uintptr_t>(fn);
    if(!fn||address<base||((address-base)&3)||address-base>=(uintptr_t(1)<<27))return false;
    const auto slot=sheep_jit_filter_slot_rom(code.data()+start);
    sheep_rom_dispatch_words[slot]=(uint64_t(root)<<32)|(uint32_t(((address-base)>>2)<<5|3)<<2)|3;
    sheep_jit_reject_filter[slot]=0;
   }
   for(unsigned i=0;i<ram.size();i++)ram[i]=uint8_t(i*17+kind*13+event);initial_ram=ram;
   ppc_state=saved;ppc_state.pc=root;ppc_state.cr=0x20000000u;ppc_state.gpr[3]=17;ppc_state.gpr[4]=23;
   ppc_state.gpr[5]=31;ppc_state.spr[SPR::LR]=target;
   const auto initial=ppc_state;
   constexpr uint64_t clock_start=123;constexpr uint32_t next_start=0x31415920u;
   page_guard_callback_target=target;page_guard_callback_kind=kind;page_guard_callback_ram=ram.data();
   SheepLoopContext initial_ctx{};
   initial_ctx.cpu=reinterpret_cast<uintptr_t>(&ppc_state);initial_ctx.words=reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words);
   initial_ctx.counters=reinterpret_cast<uintptr_t>(sheep_jit_reject_filter);initial_ctx.native_base=reinterpret_cast<uintptr_t>(nw_jit_native_base());
   initial_ctx.illegal=reinterpret_cast<uintptr_t>(&ppc_illegalop);initial_ctx.fpu_disabled=reinterpret_cast<uintptr_t>(sheep_rom_disabled_fpu_handler);
   initial_ctx.timer=reinterpret_cast<uintptr_t>(&exec_timer);initial_ctx.clock=reinterpret_cast<uintptr_t>(&g_icycles);
   initial_ctx.flags=reinterpret_cast<uintptr_t>(&exec_flags);initial_ctx.next=reinterpret_cast<uintptr_t>(&ppc_next_instruction_address);
   initial_ctx.max_cycles=clock_start+deadline;initial_ctx.host=code.data()+start;initial_ctx.page_start=page;initial_ctx.page_end=page+4095;
   initial_ctx.pc_offset=offsetof(SetPRS,pc);initial_ctx.cr_offset=offsetof(SetPRS,cr);initial_ctx.lr_offset=offsetof(SetPRS,spr)+SPR::LR*4;
   initial_ctx.disabled=ppc_opcode_grabber==OpcodeGrabberNoFPU;initial_ctx.reject=reinterpret_cast<uintptr_t>(&loop_fixture_reject);
   initial_ctx.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);initial_ctx.power=reinterpret_cast<uintptr_t>(&power_on);
   initial_ctx.itlb=reinterpret_cast<uintptr_t>(&pCurITLB1);initial_ctx.mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);
   initial_ctx.rom_begin=reinterpret_cast<uintptr_t>(code.data());initial_ctx.rom_end=reinterpret_cast<uintptr_t>(code.data()+code.size());
   initial_ctx.opcodes=reinterpret_cast<uintptr_t>(ppc_opcode_grabber);
   struct Result {SetPRS cpu;SheepLoopContext context;std::array<uint8_t,4096> ram;uint64_t result,clock;uint32_t flags,next;bool timer,power,bail;};
   auto run=[&](unsigned (*loop)(SheepLoopContext*)){Result out{};
    ppc_state=initial;ram=initial_ram;exec_flags=0;ppc_next_instruction_address=next_start;
    exec_timer=event==1;power_on=event!=2;g_icycles=clock_start;sheep_jit_memory_bail=false;
    out.context=initial_ctx;out.result=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(loop),reinterpret_cast<nw_jit_cpu*>(&out.context));
    std::memcpy(&out.cpu,&ppc_state,sizeof(out.cpu));out.ram=ram;out.clock=g_icycles;
    out.flags=exec_flags;out.next=ppc_next_instruction_address;out.timer=exec_timer;out.power=power_on;out.bail=sheep_jit_memory_bail;return out;
   };
   const auto reference=run(&sheep_page_guard_reference_a64),candidate=run(&sheep_cpu_loop_a64);
   const bool raw_matches=!(reference.result>>32)&&candidate.result==reference.result&&
    !std::memcmp(&candidate.cpu,&reference.cpu,sizeof(candidate.cpu))&&candidate.ram==reference.ram&&
    candidate.clock==reference.clock&&candidate.flags==reference.flags&&candidate.next==reference.next&&
    candidate.timer==reference.timer&&candidate.power==reference.power&&candidate.bail==reference.bail&&
    candidate.context.retired==reference.context.retired&&candidate.context.native_used==reference.context.native_used&&
    candidate.context.host==reference.context.host&&candidate.context.page_start==reference.context.page_start&&
    candidate.context.page_end==reference.context.page_end&&candidate.context.source_ram==reference.context.source_ram;
   // Independent guest replay also checks instruction effects/accounting. The
   // exact Q136 differential above checks raw native branch-to-next NIA/flags;
   // their inherited normalization below is compared by effective next PC.
   ppc_state=initial;ram=initial_ram;exec_flags=0;ppc_next_instruction_address=next_start;
   exec_timer=event==1;power_on=event!=2;g_icycles=clock_start;
   uint8_t *source_host=code.data()+start;uint32_t source_pc=root;
   bool replay_valid=true;const bool pending=uint32_t(candidate.result)!=0;
   for(unsigned retired=0;retired<candidate.context.retired;retired++){
    source_pc=ppc_state.pc;source_host=sheep_itlb_primary_hit(source_pc);
    if(!source_host||source_host<code.data()||source_host+4>code.data()+code.size()){replay_valid=false;break;}
    const uint32_t op=uint32_t(source_host[0])<<24|uint32_t(source_host[1])<<16|uint32_t(source_host[2])<<8|source_host[3];
    if(kind>=3&&kind<=5&&source_pc==root)page_guard_fixture_callback(op);else ppc_main_opcode(ppc_opcode_grabber,op);
    // Pending leaves one final post-step to ppc_exec. Cold has already
    // consumed that step and points at the next unexecuted fetch, even when
    // that fetch declines native admission because its budget is now short.
    if(retired+1==candidate.context.retired&&pending)break;
    ppc_state.pc=exec_flags&EXEF_BRANCH?ppc_next_instruction_address:ppc_state.pc+4;exec_flags=0;++g_icycles;
    if(retired+1==candidate.context.retired){source_pc=ppc_state.pc;source_host=sheep_itlb_primary_hit(source_pc);}
   }
   bool action=candidate.flags==exec_flags&&candidate.next==ppc_next_instruction_address;
   if(kind==8&&!candidate.flags&&exec_flags==EXEF_BRANCH&&ppc_next_instruction_address==uint32_t(ppc_state.pc+4))action=true;
   const bool canonical_matches=replay_valid&&!std::memcmp(&candidate.cpu,&ppc_state,sizeof(ppc_state))&&candidate.ram==ram&&
    candidate.clock==g_icycles&&candidate.timer==exec_timer&&candidate.power==power_on&&action&&
    candidate.context.host==source_host&&candidate.context.page_start==(source_pc&0xfffff000u)&&
    candidate.context.page_end==uint32_t((source_pc&0xfffff000u)+4095)&&!candidate.bail;
   const bool cap_matches=!(native&&deadline>=4&&event==0&&(kind==6||kind==10)&&deadline==1025)||
    candidate.context.retired==(kind==6?128u:1024u);
   if(!raw_matches||!canonical_matches||!cap_matches){
    auto effective=[](bool pending,uint32_t pc,uint32_t flags,uint32_t next){return !pending?pc:flags&EXEF_BRANCH?next:uint32_t(pc+4);};
    auto dump=[&](const char *name,const Result &out){
     fprintf(stderr,"PAGE GUARDS %s result=%016llx pc=%08x gpr3=%08x gpr4=%08x gpr5=%08x gpr11=%08x cr=%08x xer=%08x lr=%08x ctr=%08x clock=%llu flags=%08x nia=%08x effective=%08x retired=%u host=%p pages=%08x..%08x native=%u ram=%u timer=%u power=%u bail=%u\n",
      name,(unsigned long long)out.result,out.cpu.pc,out.cpu.gpr[3],out.cpu.gpr[4],out.cpu.gpr[5],out.cpu.gpr[11],out.cpu.cr,
      out.cpu.spr[SPR::XER],out.cpu.spr[SPR::LR],out.cpu.spr[SPR::CTR],(unsigned long long)out.clock,out.flags,out.next,
      effective(uint32_t(out.result)!=0,out.cpu.pc,out.flags,out.next),out.context.retired,(void*)out.context.host,out.context.page_start,out.context.page_end,
      out.context.native_used,out.context.source_ram,unsigned(out.timer),unsigned(out.power),unsigned(out.bail));
    };
    fprintf(stderr,"PAGE GUARDS components raw=%u canonical=%u cap=%u replay=%u action=%u pending=%u\n",
     unsigned(raw_matches),unsigned(canonical_matches),unsigned(cap_matches),unsigned(replay_valid),unsigned(action),unsigned(pending));
    dump("candidate",candidate);dump("Q136-reference",reference);
    fprintf(stderr,"PAGE GUARDS canonical pc=%08x gpr3=%08x gpr4=%08x gpr5=%08x gpr11=%08x cr=%08x xer=%08x lr=%08x ctr=%08x clock=%llu flags=%08x nia=%08x effective=%08x source=%08x host=%p pages=%08x..%08x timer=%u power=%u\n",
     ppc_state.pc,ppc_state.gpr[3],ppc_state.gpr[4],ppc_state.gpr[5],ppc_state.gpr[11],ppc_state.cr,
     ppc_state.spr[SPR::XER],ppc_state.spr[SPR::LR],ppc_state.spr[SPR::CTR],(unsigned long long)g_icycles,exec_flags,ppc_next_instruction_address,
     effective(pending,ppc_state.pc,exec_flags,ppc_next_instruction_address),source_pc,(void*)source_host,source_pc&0xfffff000u,
     uint32_t((source_pc&0xfffff000u)+4095),unsigned(exec_timer),unsigned(power_on));
    for(unsigned reg=0;reg<32;reg++)if(candidate.cpu.gpr[reg]!=reference.cpu.gpr[reg]||candidate.cpu.gpr[reg]!=ppc_state.gpr[reg])
     fprintf(stderr,"PAGE GUARDS GPR%u candidate=%08x reference=%08x canonical=%08x\n",reg,candidate.cpu.gpr[reg],reference.cpu.gpr[reg],ppc_state.gpr[reg]);
    const auto *actual_bytes=reinterpret_cast<const uint8_t*>(&candidate.cpu),*old_bytes=reinterpret_cast<const uint8_t*>(&reference.cpu),*guest_bytes=reinterpret_cast<const uint8_t*>(&ppc_state);
    for(unsigned byte=0;byte<sizeof(ppc_state);byte++)if(actual_bytes[byte]!=old_bytes[byte]||actual_bytes[byte]!=guest_bytes[byte]){
     fprintf(stderr,"PAGE GUARDS first CPU byte offset=%u candidate=%02x reference=%02x canonical=%02x\n",byte,unsigned(actual_bytes[byte]),unsigned(old_bytes[byte]),unsigned(guest_bytes[byte]));break;
    }
    for(unsigned byte=0;byte<ram.size();byte++)if(candidate.ram[byte]!=reference.ram[byte]||candidate.ram[byte]!=ram[byte]){
     fprintf(stderr,"PAGE GUARDS first RAM byte offset=%u candidate=%02x reference=%02x canonical=%02x\n",byte,unsigned(candidate.ram[byte]),unsigned(reference.ram[byte]),unsigned(ram[byte]));break;
    }
   }
   if(!require(raw_matches&&canonical_matches&&cap_matches,"candidate/Q136 exact raw state, canonical full CPU/RAM/source/count/events/ABI"))return false;
  }
  return true;
 }();
 for(unsigned i=0;i<2048;i++){
  const auto slot=sheep_jit_filter_slot_rom(code.data()+i*4);
  sheep_rom_dispatch_words[slot]=saved_words[i];sheep_jit_reject_filter[slot]=saved_counters[i];
 }
 sheep_rom_direct_reset();nw_jit_reset();
 ppc_state=saved;exec_flags=saved_flags;ppc_next_instruction_address=saved_next;g_icycles=saved_clock;
 exec_timer=saved_timer;power_on=saved_power;pCurITLB1=saved_table;tlb_size_mask=saved_mask;
 sheep_jit_memory_bail=saved_bail;sheep_hot_used=saved_used;page_guard_callback_ram=nullptr;
 constexpr unsigned expected=578;
 if(okay&&checks!=expected){fprintf(stderr,"PAGE GUARDS count %u expected %u\n",checks,expected);return 0;}
 if(okay)fprintf(stderr,"PAGE GUARDS SELFTEST %u lowered-site/old-ASM/full-CPU/RAM/ABI/wrap/branch/prefix/cap/event cases passed\n",checks);
 return okay?1:0;
}
#endif
