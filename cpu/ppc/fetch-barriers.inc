// SPDX-License-Identifier: GPL-3.0-or-later
// Included after ppc_exec_inner. The exact shared C++ dispatcher is exercised
// with a compile-only deadline/path selector; production instantiations use0.
// No live TimerManager queue is inspected, drained, rescheduled or replaced.
#include <vector>
extern std::vector<std::function<void()>> gCtxSyncCallbacks;
static void fetch_barrier_stop(uint32_t) { power_on=false; }
static void fetch_barrier_interrupt(uint32_t) { ppc_assert_int(); }
static void fetch_barrier_disabled_fpu(uint32_t) { ppc_state.gpr[7]=2; }
int sheep_fetch_barrier_selftest(){
#if defined(__aarch64__) && !defined(LOG_INSTRUCTIONS) && !defined(CPU_PROFILING)
 SetPRS saved_cpu;std::memcpy(&saved_cpu,&ppc_state,sizeof(saved_cpu));const auto saved_flags=exec_flags,saved_next=ppc_next_instruction_address;
 const auto saved_clock=g_icycles;const auto saved_reason=power_off_reason;
 const bool saved_timer=exec_timer,saved_power=power_on,saved_int=int_pin,saved_dec=dec_exception_pending,saved_thrm=thrm_exception_pending;
 auto*const saved_grabber=ppc_opcode_grabber;
 const auto saved_fpu_disabled=sheep_rom_disabled_fpu_handler;
 auto*const saved_i1=pCurITLB1;auto*const saved_i2=pCurITLB2;auto*const saved_d1=pCurDTLB1;auto*const saved_d2=pCurDTLB2;
 const auto saved_imode=CurITLBMode,saved_dmode=CurDTLBMode;const auto saved_mask=tlb_size_mask;
 const auto saved_begin=sheep_rom_host_begin,saved_end=sheep_rom_host_end,saved_base=sheep_rom_direct_base;
 const bool saved_only=sheep_rom_only,saved_direct=sheep_rom_direct_enabled,saved_hot=sheep_hot_enabled,saved_used=sheep_hot_used;
 const auto saved_spans=sheep_steady_spans,saved_insns=sheep_steady_instructions;
 auto saved_callbacks=std::move(gCtxSyncCallbacks);gCtxSyncCallbacks.clear();
 constexpr uint32_t stop_op=0x4c00002au,interrupt_op=0x4c00006au,fmr_op=0xfc201090u;
 const unsigned stop_index=(stop_op>>15&0x1f800)|(stop_op&0x7ff),interrupt_index=(interrupt_op>>15&0x1f800)|(interrupt_op&0x7ff);
 const PPCOpcode stop_enabled=OpcodeGrabber[stop_index],stop_disabled=OpcodeGrabberNoFPU[stop_index];
 const PPCOpcode int_enabled=OpcodeGrabber[interrupt_index],int_disabled=OpcodeGrabberNoFPU[interrupt_index];
 const unsigned fmr_index=(fmr_op>>15&0x1f800)|(fmr_op&0x7ff);
 const PPCOpcode fmr_enabled=OpcodeGrabber[fmr_index],fmr_disabled=OpcodeGrabberNoFPU[fmr_index];
 alignas(4096) std::array<uint8_t,8192> code{};std::array<uint64_t,2048> saved_words{};std::array<uint8_t,2048> saved_counters{};
 for(unsigned i=0;i<2048;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+4*i);saved_words[i]=sheep_rom_dispatch_words[slot];saved_counters[i]=sheep_jit_reject_filter[slot];}
 struct Patch{uint8_t*slot=nullptr;std::array<uint8_t,32> value{};};std::array<Patch,2> patches{};
 unsigned passed=0,failed=0;bool okay=false;
 auto require=[&](bool value,const char*why){if(!value){fprintf(stderr,"FETCH BARRIER failure case=%u %s\n",passed+1,why);throw std::runtime_error("Q174 instruction fetch regression failed");}};
 auto restore_slots=[&](){for(auto&p:patches)if(p.slot){std::memcpy(p.slot,p.value.data(),32);p.slot=nullptr;}};
 auto restore=[&](){restore_slots();for(unsigned i=0;i<2048;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+4*i);sheep_rom_dispatch_words[slot]=saved_words[i];sheep_jit_reject_filter[slot]=saved_counters[i];}OpcodeGrabber[stop_index]=stop_enabled;OpcodeGrabberNoFPU[stop_index]=stop_disabled;OpcodeGrabber[interrupt_index]=int_enabled;OpcodeGrabberNoFPU[interrupt_index]=int_disabled;OpcodeGrabberNoFPU[fmr_index]=fmr_disabled;gCtxSyncCallbacks=std::move(saved_callbacks);std::memcpy(&ppc_state,&saved_cpu,sizeof(ppc_state));ppc_opcode_grabber=saved_grabber;sheep_rom_disabled_fpu_handler=saved_fpu_disabled;pCurITLB1=saved_i1;pCurITLB2=saved_i2;pCurDTLB1=saved_d1;pCurDTLB2=saved_d2;CurITLBMode=saved_imode;CurDTLBMode=saved_dmode;tlb_size_mask=saved_mask;nw_jit_layout layout{};sheep_mmu_jit_layout(&layout);exec_flags=saved_flags;ppc_next_instruction_address=saved_next;g_icycles=saved_clock;exec_timer=saved_timer;power_on=saved_power;power_off_reason=saved_reason;int_pin=saved_int;dec_exception_pending=saved_dec;thrm_exception_pending=saved_thrm;sheep_rom_host_begin=saved_begin;sheep_rom_host_end=saved_end;sheep_rom_direct_base=saved_base;sheep_rom_only=saved_only;sheep_rom_direct_enabled=saved_direct;sheep_hot_enabled=saved_hot;sheep_hot_used=saved_used;sheep_steady_spans=saved_spans;sheep_steady_instructions=saved_insns;};
 auto put=[&](unsigned at,uint32_t op){for(unsigned j=0;j<4;j++)code[at+j]=uint8_t(op>>(24-8*j));uint32_t desc;require(sheep_encode_rom_handler(op,desc),"fixture scalar handler encoding");const auto slot=sheep_jit_filter_slot_rom(code.data()+at);sheep_rom_dispatch_words[slot]=(uint64_t(op)<<32)|desc|1;sheep_jit_reject_filter[slot]=255;};
 auto slot_for=[&](TLBEntry*table,uint32_t page){return reinterpret_cast<uint8_t*>(table)+size_t((page>>12)&4095)*32;};
 auto map=[&](uint8_t*slot,uint32_t page,uint8_t*host){const uint16_t flags=1;const int64_t offset=int64_t(reinterpret_cast<uintptr_t>(host))-int64_t(page);std::memset(slot,0,32);std::memcpy(slot,&page,4);std::memcpy(slot+4,&flags,2);std::memcpy(slot+8,&offset,8);};
 struct Result{SetPRS cpu;uint64_t clock,spans,insns;uint32_t flags,next;bool timer,power,intpin;uintptr_t i1;};
 try{
  require(sheep_rom_decode_ready&&sheep_itlb_primary_enabled,"initialized immutable decoder and primary mapper");
  require(!(sheep_compact_primary_mask&(uint64_t(1)<<19)),"extended primary is not compacted");
  OpcodeGrabber[stop_index]=OpcodeGrabberNoFPU[stop_index]=fetch_barrier_stop;
  OpcodeGrabber[interrupt_index]=OpcodeGrabberNoFPU[interrupt_index]=fetch_barrier_interrupt;
  require(fmr_enabled!=fmr_disabled&&fmr_disabled==ppc_fpu_off,"real FMR has canonical FPU-disabled selector");
  OpcodeGrabberNoFPU[fmr_index]=fetch_barrier_disabled_fpu;
  sheep_rom_only=sheep_rom_direct_enabled=true;sheep_hot_enabled=false;
  sheep_rom_host_begin=reinterpret_cast<uintptr_t>(code.data());sheep_rom_host_end=sheep_rom_host_begin+code.size();sheep_rom_direct_base=reinterpret_cast<uintptr_t>(nw_jit_native_base());
  // kind0 IR disable;1 PR supervisor->problem;2 real ISYNC executes one
  // deferred mapping action;3 real external exception;4 DR-only negative;
  // 5 real interrupt FP1->FP0 with safe disabled-selector observer (not a
  // claim of NO_FPU delivery, and never any exc_env replacement/longjmp).
  // ISYNC uses the real context callback queue with a deterministic synthetic
  // mapping writer. It does not claim to exercise a hashed PTE/BAT refill.
  for(unsigned path=1;path<=2;path++)for(unsigned prefix=0;prefix<2;prefix++)for(unsigned kind=0;kind<6;kind++){
   restore_slots();gCtxSyncCallbacks.clear();
   const uint32_t page=prefix?0xfff00000u:0u,root=page+0x400u;
   const bool interrupt=kind==3||kind==5;
   const uint32_t old_msr=MSR::IR|(prefix?MSR::IP:0)|(interrupt?MSR::EE:0)|(kind==4?MSR::DR:0)|(kind==5?MSR::FP:0);
   const uint32_t new_msr=kind==0?old_msr&~MSR::IR:kind==1?old_msr|MSR::PR:interrupt?old_msr&~(MSR::IR|MSR::EE|MSR::FP):kind==4?old_msr&~MSR::DR:old_msr;
   ppc_state=saved_cpu;ppc_state.msr=old_msr;tlb_size_mask=4095;CurITLBMode=CurDTLBMode=0xff;mmu_change_mode();auto*const old_table=pCurITLB1;
   ppc_state.msr=new_msr;mmu_change_mode();auto*const new_table=pCurITLB1;
   patches[0].slot=slot_for(old_table,page);std::memcpy(patches[0].value.data(),patches[0].slot,32);
   if(new_table!=old_table){patches[1].slot=slot_for(new_table,page);std::memcpy(patches[1].value.data(),patches[1].slot,32);}
   for(unsigned i=0;i<2048;i++)put(i*4,0x60000000u);
   const uint32_t first=kind==2?0x4c00012cu:interrupt?interrupt_op:(31u<<26)|(5u<<21)|(146u<<1);
   const unsigned marker=interrupt?0x500:kind==2?0x404:0x408,stop=marker+(kind==5?8:4);
   for(unsigned bank=0;bank<2;bank++){put(bank*4096+0x400,first);if(kind!=2&&!interrupt)put(bank*4096+0x404,0x4c00012cu);put(bank*4096+marker,0x38c60000u+((bank||kind==5)?2u:1u));if(kind==5){const unsigned at=bank*4096+marker+4;for(unsigned j=0;j<4;j++)code[at+j]=uint8_t(fmr_op>>(24-8*j));const intptr_t delta=intptr_t(reinterpret_cast<uintptr_t>(fmr_enabled))-intptr_t(reinterpret_cast<uintptr_t>(&ppc_illegalop));require(!(delta&3)&&delta>=-(int64_t(1)<<30)&&delta<(int64_t(1)<<30),"real FMR packed descriptor range");const auto slot=sheep_jit_filter_slot_rom(code.data()+at);sheep_rom_dispatch_words[slot]=(uint64_t(fmr_op)<<32)|(uint32_t(delta)&0x7ffffffcu)|0x80000001u;sheep_jit_reject_filter[slot]=255;}put(bank*4096+stop,stop_op);}
   SetPRS initial;std::memcpy(&initial,&saved_cpu,sizeof(initial));initial.pc=root;initial.msr=old_msr;initial.gpr[5]=new_msr;initial.gpr[6]=17;initial.gpr[7]=0;initial.cr=0x59a63c87u;initial.spr[SPR::XER]=0xe0001234u;initial.spr[SPR::LR]=0x13572468u;initial.spr[SPR::CTR]=0x98765432u;initial.fpr[1].int64_r=0x3ff0000000000000ull;initial.fpr[2].int64_r=0x4000000000000000ull;
   auto prepare=[&](){gCtxSyncCallbacks.clear();map(patches[0].slot,page,code.data());if(new_table!=old_table)map(patches[1].slot,page,code.data()+4096);std::memcpy(&ppc_state,&initial,sizeof(ppc_state));ppc_opcode_grabber=kind==5?OpcodeGrabber:OpcodeGrabberNoFPU;sheep_rom_disabled_fpu_handler=kind==5?fetch_barrier_disabled_fpu:ppc_fpu_off;tlb_size_mask=4095;CurITLBMode=CurDTLBMode=0xff;mmu_change_mode();exec_flags=0;ppc_next_instruction_address=0x31415920;g_icycles=123;exec_timer=false;power_on=true;int_pin=dec_exception_pending=thrm_exception_pending=false;sheep_steady_spans=sheep_steady_instructions=0;if(kind==2){auto*const entry=patches[0].slot;gCtxSyncCallbacks.push_back([&,entry](){map(entry,page,code.data()+4096);});}};
   auto result=[&](){Result value{};std::memcpy(&value.cpu,&ppc_state,sizeof(value.cpu));value.clock=g_icycles;value.spans=sheep_steady_spans;value.insns=sheep_steady_instructions;value.flags=exec_flags;value.next=ppc_next_instruction_address;value.timer=exec_timer;value.power=power_on;value.intpin=int_pin;value.i1=reinterpret_cast<uintptr_t>(pCurITLB1);return value;};
   // Independent dispatcher oracle: canonical translation before EVERY fetch.
   // This is the same fixture in regression-only and fixed builds; there is
   // no simulated old handler or conditional expected-old opcode path.
   prepare();unsigned steps=0;
   while(power_on&&steps<12){const auto*host=mmu_translate_imem(ppc_state.pc);ppc_main_opcode(ppc_opcode_grabber,ppc_read_instruction(host));g_icycles++;if(exec_flags){ppc_state.pc=ppc_next_instruction_address;exec_flags=0;}else ppc_state.pc+=4;steps++;}
   require(!power_on&&steps<12,"fresh-mapping oracle reaches owned stop callback");const auto expected=result();
   const unsigned delta=kind==4?1:2;require(expected.cpu.gpr[6]==17+delta,"fresh canonical translation selects expected new opcode");
   prepare();if(path==1)ppc_exec_inner<main,big_end,true,1>(0,0);else ppc_exec_inner<main,big_end,true,2>(0,0);const auto actual=result();
   const bool selected=actual.cpu.gpr[6]==17+delta;
   const bool disabled=kind!=5||(actual.cpu.gpr[7]==2&&!(actual.cpu.msr&MSR::FP)&&actual.cpu.fpr[1].int64_r==initial.fpr[1].int64_r);
   const bool full=!std::memcmp(&actual.cpu,&expected.cpu,sizeof(SetPRS))&&actual.clock==expected.clock&&actual.flags==expected.flags&&actual.next==expected.next&&actual.timer==expected.timer&&actual.power==expected.power&&actual.intpin==expected.intpin&&actual.i1==expected.i1;
   const bool route=(path==1?!actual.spans:actual.spans>0)&&(path!=2||(actual.insns>0&&actual.insns<=steps));
   // Retain every old-build counterexample, then return failure after cleanup.
   // A failed semantic case is never converted to a success or discarded.
   if(!selected||!disabled||!full||!route||!gCtxSyncCallbacks.empty()){
    failed++;fprintf(stderr,"FETCH BARRIER REGRESSION case=%u path=%u prefix=%u kind=%u selected=%u disabled=%u full=%u route=%u r6=%u/%u r7=%u/%u pc=%08x/%08x clock=%llu/%llu\n",passed+1,path,prefix,kind,unsigned(selected),unsigned(disabled),unsigned(full),unsigned(route),actual.cpu.gpr[6],expected.cpu.gpr[6],actual.cpu.gpr[7],expected.cpu.gpr[7],actual.cpu.pc,expected.cpu.pc,(unsigned long long)actual.clock,(unsigned long long)expected.clock);
   }
   passed++;
  }
  require(passed==24,"fixed regression census");okay=failed==0;
 }catch(const std::exception&e){fprintf(stderr,"FETCH BARRIER exception: %s\n",e.what());}
 restore();fprintf(stderr,"FETCH BARRIER SUMMARY cases=%u failed=%u\n",passed,failed);if(okay)fprintf(stderr,"FETCH BARRIER SELFTEST %u actual-C++/ASM/MTMSR/ISYNC/exception/fresh-map/fullCPU cases passed\n",passed);return okay?1:0;
#else
 return 0;
#endif
}
