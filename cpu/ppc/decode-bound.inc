// Project: DingusPPC-Pippin
// File: cpu/ppc/decode-bound.inc
// Purpose: Verify-mode tests for JIT decode bounds at the end of ROM and guarded pages.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
//   2026-11-16 - Guarded-page test uses the host page size (works on 16 KB-page kernels).
// Full history: PIPPIN.md and Git history.
// Private Q161 bridge fixture: actual planner/emitter + canonical interpreter.
#include <devices/memctrl/memctrlbase.h>
#include <sys/mman.h>
#if defined(__aarch64__)
extern uint64_t g_icycles;
extern volatile bool exec_timer;
extern bool power_on;
int sheep_decode_bound_selftest(){
 const auto saved=ppc_state,saved_shadow=shadow;const auto saved_flags=exec_flags,saved_next=ppc_next_instruction_address;
 const auto saved_clock=g_icycles;const bool saved_timer=exec_timer,saved_power=power_on;
 const bool saved_only=sheep_rom_only,saved_direct=sheep_rom_direct_enabled,saved_hot=sheep_hot_enabled,saved_bail=memory_bail,saved_used=sheep_hot_used;
 const auto saved_begin=sheep_rom_host_begin,saved_end=sheep_rom_host_end;const int saved_mode=mode;const auto saved_layout=native_layout;
 auto *const saved_itlb=pCurITLB1;const auto saved_mask=tlb_size_mask;
 auto *const saved_written=written;const auto saved_size=written_size,saved_old=written_old,saved_new=written_new;
 const uint64_t statistics[]={blocks,insns,verified,mismatches,compiles,negative_hits,rom_compiles,ram_compiles,warmups,memory_hits,memory_misses};
 alignas(4096) std::array<uint8_t,8192> code{};alignas(32) std::array<uint8_t,4096*32> itlb{};
 std::array<uint8_t,4096> ram{},original_ram{},expected_ram{};
 std::array<uint64_t,2048> old_words{};std::array<uint8_t,2048> old_counters{};std::array<SheepHotSlot,2048> old_hot{};
 unsigned passed=0,positive=0,legacy=0,bounds=0,protected_cases=0;bool okay=false;
 unsigned general_index=0;uint8_t general_old=0;bool general_touched=false,data_mapped=false;
 reset_native_and_rom();
 for(unsigned i=0;i<2048;i++){const auto*p=code.data()+i*4;const auto s=sheep_jit_filter_slot_rom(p);old_words[i]=sheep_rom_dispatch_words[s];old_counters[i]=sheep_jit_reject_filter[s];old_hot[i]=sheep_hot_slots[sheep_hot_index(p,0)];}
 auto require=[&](bool good,const char*why){if(!good){fprintf(stderr,"DECODE BOUND failure case=%u %s\n",passed+1,why);throw std::runtime_error("Q161 decode-bound fixture failed");}};
 auto cache_slot=[](const uint8_t*p,uint32_t pc){return ((uintptr_t(p)>>2)^((uint64_t(pc)*0x9e3779b1u)>>16))&(cache.size()-1);};
 auto restore_general=[&](){if(general_touched){sheep_jit_reject_filter[general_index]=general_old;general_touched=false;}};
 auto cleanup=[&](){
  restore_general();sheep_rom_only=true;sheep_rom_host_begin=reinterpret_cast<uintptr_t>(code.data());sheep_rom_host_end=sheep_rom_host_begin+code.size();reset_native_and_rom();
  for(unsigned i=0;i<2048;i++){const auto*p=code.data()+i*4;auto s=sheep_jit_filter_slot_rom(p);sheep_rom_dispatch_words[s]=old_words[i];sheep_jit_reject_filter[s]=old_counters[i];sheep_hot_slots[sheep_hot_index(p,0)]=old_hot[i];}
  for(auto&e:cache)e=Entry{};if(data_mapped)sheep_mmu_test_page(nullptr,0,0,false);pCurITLB1=saved_itlb;tlb_size_mask=saved_mask;
  native_layout=saved_layout;mode=saved_mode;sheep_rom_only=saved_only;sheep_rom_host_begin=saved_begin;sheep_rom_host_end=saved_end;
  sheep_rom_direct_enabled=saved_direct;sheep_hot_enabled=saved_hot;memory_bail=saved_bail;sheep_hot_used=saved_used;
  ppc_state=saved;shadow=saved_shadow;exec_flags=saved_flags;ppc_next_instruction_address=saved_next;g_icycles=saved_clock;exec_timer=saved_timer;power_on=saved_power;
  written=saved_written;written_size=saved_size;written_old=saved_old;written_new=saved_new;
  blocks=statistics[0];insns=statistics[1];verified=statistics[2];mismatches=statistics[3];compiles=statistics[4];negative_hits=statistics[5];rom_compiles=statistics[6];ram_compiles=statistics[7];warmups=statistics[8];memory_hits=statistics[9];memory_misses=statistics[10];
 };
 try{
  auto*rom=mem_ctrl_instance?mem_ctrl_instance->find_rom_region():nullptr;require(rom&&rom->type==RT_ROM&&rom->mem_ptr,"actual Pippin ROM region");
  constexpr uint32_t sentinel=0x31415920u;constexpr uint64_t clock=123;constexpr uint32_t data_va=0x1234000u;
  auto cpu_seed=[&](uint32_t pc,unsigned bit){SetPRS cpu;std::memcpy(&cpu,&saved,sizeof(cpu));cpu.pc=pc;cpu.spr[SPR::PVR]=PPC_VER::MPC603;cpu.msr&=~MSR::PR;for(unsigned r=0;r<32;r++)cpu.gpr[r]=0x913579bdu^(r*0x1020301u);cpu.gpr[7]=data_va+16;cpu.cr=(0xa65a9187u&~(1u<<23))|(bit<<23);cpu.spr[SPR::XER]=0xe1234567u;cpu.spr[SPR::CTR]=0xffffffffu;cpu.spr[SPR::LR]=(pc+256)|3u;return cpu;};
  auto reset=[&](const SetPRS&cpu){ppc_state=cpu;exec_flags=0;ppc_next_instruction_address=sentinel;g_icycles=clock;exec_timer=true;power_on=true;memory_bail=false;written=nullptr;ram=original_ram;};
  auto untouched=[&](const SetPRS&cpu){return !std::memcmp(&ppc_state,&cpu,sizeof(cpu))&&ram==original_ram&&exec_flags==0&&ppc_next_instruction_address==sentinel&&g_icycles==clock&&exec_timer&&power_on&&!memory_bail;};
  // I/D primary tables share one global index mask. Keep both at4095 so
  // canonical memory replay and the exported12-bit native DTLB agree.
  auto map_code=[&](const uint8_t*source,uint32_t pc,bool immutable){itlb.fill(0);for(unsigned i=0;i<4096;i++){const uint32_t bad=0xffffffffu;std::memcpy(itlb.data()+i*32,&bad,4);}auto*e=itlb.data()+((pc>>12)&4095)*32;const uint32_t page=pc&~4095u,phys=immutable?rom->start:0;const uint16_t flags=1;const int64_t offset=int64_t(reinterpret_cast<uintptr_t>(source))-int64_t(pc);std::memcpy(e,&page,4);std::memcpy(e+4,&flags,2);std::memcpy(e+8,&offset,8);std::memcpy(e+16,&offset,8);std::memcpy(e+24,&phys,4);pCurITLB1=reinterpret_cast<TLBEntry*>(itlb.data());tlb_size_mask=4095;};
  auto prepare=[&](const uint32_t*ops,unsigned n,uint32_t pc,bool immutable,int wanted_mode,bool miss=false){
   restore_general();sheep_rom_only=true;sheep_rom_host_begin=reinterpret_cast<uintptr_t>(code.data());sheep_rom_host_end=sheep_rom_host_begin+code.size();sheep_rom_direct_reset();
   code.fill(0);for(unsigned i=0;i<n;i++)write_be(code.data()+64+i*4,4,ops[i]);
   for(unsigned i=0;i<2048;i++){auto*p=code.data()+i*4;const auto slot=sheep_jit_filter_slot_rom(p);uint32_t d;require(sheep_encode_rom_handler(read_be(p,4),d),"descriptor encoding");sheep_rom_dispatch_words[slot]=(uint64_t(read_be(p,4))<<32)|d;sheep_jit_reject_filter[slot]=0;sheep_hot_slots[sheep_hot_index(p,pc)]=SheepHotSlot{};}
   sheep_mmu_test_page(ram.data(),data_va,0x61,miss);data_mapped=true;map_code(code.data()+64,pc,immutable);
   native_layout=saved_layout;native_layout.leaf=1;native_layout.tlb_index_bits=12;native_layout.fast_read_slot=native_layout.fast_write_slot=0;
   mode=wanted_mode;if(mode==2)native_layout.trace_host=native_layout.trace_old=native_layout.trace_size=0;
   sheep_rom_only=immutable;sheep_rom_direct_enabled=sheep_hot_enabled=true;
   cache[cache_slot(code.data()+64,pc)]=Entry{};
   if(!immutable){general_index=sheep_jit_filter_slot_general(code.data()+64,pc);general_old=sheep_jit_reject_filter[general_index];general_touched=true;sheep_jit_reject_filter[general_index]=0;}
  };
  for(unsigned i=0;i<original_ram.size();i++)original_ram[i]=uint8_t(i*37u+19u);
  // Full plans mature at budget4, including compile-without-execution. Both
  // real bridge modes, full canonical CPU/RAM, branches, one-store and guard.
  for(unsigned wanted_mode=1;wanted_mode<=2;wanted_mode++)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned bit=0;bit<2;bit++)for(unsigned shape=0;shape<5;shape++){
   uint32_t ops[32];for(auto&o:ops)o=0x38630001u;unsigned n=shape==0?5:shape==2?6:shape==4?32:8,planned=shape==2?4:n;
   if(shape==0)ops[4]=0x4bffff80u;
   if(shape==1){ops[1]=0x41880040u;ops[7]=0x4bffff80u;}
   if(shape==2){ops[2]=0x91070000u;ops[4]=0x91270004u;}
   if(shape==3)ops[2]=0x81070000u;
   const uint32_t pc=wrap?0xffffff80u:0x10000040u;prepare(ops,n,pc,true,wanted_mode,shape==3);auto initial=cpu_seed(pc,bit);const auto*source=code.data()+64;const auto original_code=code;
   require(sheep_mmu_code_is_rom(source,pc),"fake primary ROM qualification");
   for(unsigned budget=0;budget<4;budget++){reset(initial);require(!sheep_jit_try(source,budget,32)&&untouched(initial)&&!cache[cache_slot(source,pc)].host,"cold floor4 independent of decode bound");}
   for(unsigned warm=0;warm<4;warm++){reset(initial);require(!sheep_jit_try(source,4,32)&&untouched(initial),"warm observation retired guest state");const auto&e=cache[cache_slot(source,pc)];require(e.n==planned&&!e.fn&&!e.negative&&e.warmup==warm+1,"full stable plan/warm count at short budget");}
   // Compute the independent canonical successful prefix before any raw call.
   reset(initial);unsigned expected_count=0;const unsigned limit=shape==3?2:planned;
   for(;expected_count<limit;){ppc_main_opcode(ppc_opcode_grabber,ops[expected_count]);expected_count++;const bool branch=exec_flags&EXEF_BRANCH;ppc_state.pc=branch?ppc_next_instruction_address:uint32_t(ppc_state.pc+4);exec_flags=0;if(branch)break;}
   const SetPRS canonical=ppc_state;expected_ram=ram;const uint32_t target=canonical.pc;
   auto check_bridge=[&](unsigned count,bool account_source){SetPRS complete=ppc_state;complete.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:uint32_t(account_source?ppc_state.pc+4:pc+count*4);const bool branch=target!=uint32_t(pc+count*4)||(target&~4095u)!=(pc&~4095u);const uint32_t raw_pc=account_source&&count?uint32_t(pc+(count-1)*4):pc;require(count==expected_count&&!std::memcmp(&complete,&canonical,sizeof(complete))&&ram==expected_ram&&ppc_state.pc==raw_pc&&exec_flags==(branch?uint32_t(EXEF_BRANCH):0u)&&ppc_next_instruction_address==(branch?target:sentinel)&&g_icycles==clock&&exec_timer&&power_on&&memory_bail==(shape==3),"bridge full CPU/RAM/raw flags/NIA/source-PC/effective-PC/count/controls oracle");};
   reset(initial);const unsigned first=sheep_jit_try(source,4,32);auto&e=cache[cache_slot(source,pc)];require(e.n==planned&&e.fn&&e.rom&&!e.negative,"real full plan compiled");const auto fn=e.fn;
   const auto slot=sheep_jit_filter_slot_rom(source);
   if(planned>4){require(!first&&untouched(initial)&&(sheep_rom_dispatch_words[slot]&3)!=3&&!sheep_hot_slots[sheep_hot_index(source,pc)].fn,"overbudget compile must neither retire nor publish");}else check_bridge(first,false);
   for(unsigned budget=0;budget<planned;budget++){reset(initial);require(!sheep_jit_try(source,budget,32)&&untouched(initial)&&cache[cache_slot(source,pc)].fn==fn,"compiled full entry ignored execution budget");}
   reset(initial);require(sheep_jit_try(source,planned,32)==expected_count,"full-budget bridge call count");check_bridge(expected_count,false);
   reset(initial);SetPRS raw=initial;const unsigned raw_count=checked(fn,reinterpret_cast<nw_jit_cpu*>(&raw));require(raw_count==expected_count&&!std::memcmp(&raw,&canonical,sizeof(raw))&&ram==expected_ram&&memory_bail==(shape==3),"actual mapped native raw ABI/fullstate oracle");
   if(wanted_mode==2&&shape!=3){
    reset(initial);unsigned count=777;require(!sheep_jit_hot<true>(source,planned-1,count)&&count==777&&untouched(initial),"hot entry ignored full planned budget");
    require(!sheep_jit_rom_direct(slot,sheep_rom_dispatch_words[slot],planned-1,count)&&count==777&&untouched(initial),"direct entry ignored full planned budget");
    reset(initial);require(sheep_jit_hot<true>(source,planned,count),"real hot use of full plan");check_bridge(count,true);
    reset(initial);require(sheep_jit_rom_direct(slot,sheep_rom_dispatch_words[slot],planned,count),"real direct use of full plan");check_bridge(count,true);
   }
   require(code==original_code,"source ROM fixture modified");positive++;passed++;
  }
  // Default API retains sticky budget4 plans; RAM ignores a nonzero32 bound.
  for(unsigned wanted_mode=1;wanted_mode<=2;wanted_mode++)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned immutable=0;immutable<2;immutable++){
   const uint32_t ops[]={0x38630001u,0x38630001u,0x38630001u,0x38630001u,0x4bffff80u};const uint32_t pc=wrap?0xffffff80u:0x10000040u;prepare(ops,5,pc,immutable,wanted_mode);auto initial=cpu_seed(pc,0);const auto*source=code.data()+64;const unsigned bound=immutable?0:32;
   for(unsigned warm=0;warm<4;warm++){reset(initial);require(!sheep_jit_try(source,4,bound)&&untouched(initial),"legacy/RAM warm behavior");}
   reset(initial);require(sheep_jit_try(source,4,bound)==4,"legacy/RAM first compile length");const auto fn=cache[cache_slot(source,pc)].fn;require(fn&&cache[cache_slot(source,pc)].n==4,"legacy/RAM scan follows execution budget");
   reset(initial);require(sheep_jit_try(source,32,bound)==4&&cache[cache_slot(source,pc)].fn==fn&&cache[cache_slot(source,pc)].n==4&&ppc_state.gpr[3]==initial.gpr[3]+4,"legacy/RAM positive entry remains reused");legacy++;passed++;
  }
  restore_general();
  struct BoundCase{unsigned host_words,request,expected;uint32_t pc;unsigned host_offset,begin_shift;};
  const BoundCase cases[]={
   {3,32,0,0x10000040u,64,0},{4,32,4,0x10000040u,64,0},{5,32,5,0x10000040u,64,0},{8,5,5,0x10000040u,64,0},
   {32,0xffffffffu,32,0x10000040u,64,0},{32,32,0,0x10000ff4u,64,0},{32,32,4,0x10000ff0u,64,0},{32,32,5,0x10000fecu,64,0},
   {32,32,5,0xffffffecu,64,0},{32,1,0,0x10000040u,64,0},{32,3,0,0x10000040u,64,0},{32,4,4,0x10000040u,64,0},
   {32,32,0,0x10000040u,64,65},{0,32,0,0x10000040u,64,0},{32,32,0,0x10000040u,65,0},{32,32,0,0x10000041u,64,0}};
  for(const auto&c:cases){uint32_t ops[32];for(auto&o:ops)o=0x38630001u;prepare(ops,32,c.pc,true,2);const auto*source=code.data()+c.host_offset;map_code(source,c.pc,true);sheep_rom_host_begin=reinterpret_cast<uintptr_t>(code.data())+c.begin_shift;sheep_rom_host_end=reinterpret_cast<uintptr_t>(source)+c.host_words*4;cache[cache_slot(source,c.pc)]=Entry{};const auto initial=cpu_seed(c.pc,0);reset(initial);require(!sheep_jit_try(source,4,c.request)&&untouched(initial),"bounded warm/decline touched CPU");const auto&e=cache[cache_slot(source,c.pc)];require(c.expected?(e.n==c.expected&&e.warmup==1&&!e.fn):!e.host,"explicit decode bound clamp/root qualification");bounds++;passed++;}
  // Real PROT_NONE after the final source page proves no fifth-word read or
  // negative-cache sentinel copy beyond ROMremaining. RAM uses execution4.
  for(unsigned kind=0;kind<3;kind++){
   const size_t host_page=size_t(sysconf(_SC_PAGESIZE));
   auto*pages=static_cast<uint8_t*>(mmap(nullptr,2*host_page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));require(pages!=MAP_FAILED,"guarded source mmap");
   const unsigned words=kind==0?3:4;auto*source=pages+host_page-words*4;for(unsigned i=0;i<words;i++)write_be(source+i*4,4,0x38630001u);
   require(!mprotect(pages+host_page,host_page,PROT_NONE),"protected next source page");
   const uint32_t pc=0x10000040u;const auto slot=sheep_jit_filter_slot_rom(source),hot=sheep_hot_index(source,pc);const auto old_word=sheep_rom_dispatch_words[slot];const auto old_counter=sheep_jit_reject_filter[slot];const auto old_slot=sheep_hot_slots[hot];
   bool mapped_cleanup=false;
   try{
    sheep_rom_only=true;sheep_rom_host_begin=reinterpret_cast<uintptr_t>(code.data());sheep_rom_host_end=sheep_rom_host_begin+code.size();sheep_rom_direct_reset();
    sheep_rom_host_begin=reinterpret_cast<uintptr_t>(pages);sheep_rom_host_end=sheep_rom_host_begin+host_page;map_code(source,pc,kind!=2);sheep_rom_only=kind!=2;mode=2;native_layout.trace_host=native_layout.trace_old=native_layout.trace_size=0;
    uint32_t description;require(sheep_encode_rom_handler(0x38630001u,description),"guarded source description");sheep_rom_dispatch_words[slot]=(uint64_t(0x38630001u)<<32)|description;sheep_jit_reject_filter[slot]=0;sheep_hot_slots[hot]=SheepHotSlot{};cache[cache_slot(source,pc)]=Entry{};
    if(kind==2){general_index=sheep_jit_filter_slot_general(source,pc);general_old=sheep_jit_reject_filter[general_index];general_touched=true;sheep_jit_reject_filter[general_index]=0;}
    const auto initial=cpu_seed(pc,0);
    for(unsigned warm=0;warm<4;warm++){reset(initial);require(!sheep_jit_try(source,4,32)&&untouched(initial),"guarded source warm bound");}
    reset(initial);const unsigned actual=sheep_jit_try(source,4,32);require(kind==0?(!actual&&untouched(initial)&&!cache[cache_slot(source,pc)].host):(actual==4&&ppc_state.gpr[3]==initial.gpr[3]+4&&cache[cache_slot(source,pc)].n==4),"protected source decode/read/execute bounds");
    sheep_rom_only=true;sheep_rom_direct_reset();restore_general();sheep_rom_dispatch_words[slot]=old_word;sheep_jit_reject_filter[slot]=old_counter;sheep_hot_slots[hot]=old_slot;cache[cache_slot(source,pc)]=Entry{};mapped_cleanup=true;
   }catch(...){if(!mapped_cleanup){sheep_rom_only=true;sheep_rom_direct_reset();restore_general();sheep_rom_dispatch_words[slot]=old_word;sheep_jit_reject_filter[slot]=old_counter;sheep_hot_slots[hot]=old_slot;cache[cache_slot(source,pc)]=Entry{};}munmap(pages,2*host_page);throw;}
   require(!munmap(pages,2*host_page),"guarded source munmap");protected_cases++;passed++;
  }
  require(passed==67&&positive==40&&legacy==8&&bounds==16&&protected_cases==3,"fixed case census");okay=true;
 }catch(const std::exception&e){fprintf(stderr,"DECODE BOUND fixture exception: %s\n",e.what());}
 cleanup();if(okay)fprintf(stderr,"DECODE BOUND SELFTEST %u stable-ROM-plan/warm/CPU/RAM/rawABI/hot/direct/guard/budget/bounds cases passed\n",passed);return okay?1:0;
}
#else
int sheep_decode_bound_selftest(){return 0;}
#endif
