// Project: DingusPPC-Pippin
// File: cpu/ppc/sheep_jit_bridge.cpp
// Purpose: Bridge between the SheepShaver-derived ARM64 JIT and the DingusPPC CPU, MMU and devices.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
//   2026-11-16 - Fastmem self-tests are skipped on hosts without 4 KB pages (e.g. a Pi 5 16 KB kernel).
//   2026-11-16 - Verify mode runs the authentic-speed CD model self-test.
// Full history: PIPPIN.md and Git history.
// Experimental Pippin backend: SheepShaver ARM64 emitter + DingusPPC devices.
// Translates qualified arithmetic, branch and guarded RAM operations.
// Device I/O, MMU misses, privileged operations, interrupts and timers retain
// Dingus semantics; normal JIT execution operates directly on its CPU state.
#include "ppcemu.h"
#include "ppcmmu.h"
#include "sheep_jit_bridge.h"
#include "nw_jit.h"
#include "pippin_fastmem.h"
#include "pippin_authentic.h"
#include <array>
#include <cstddef>
#include <core/memaccess.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
SheepHotSlot sheep_hot_slots[65536];
uint64_t sheep_hot_epoch=0,sheep_hot_blocks=0,sheep_hot_instructions=0;
bool sheep_hot_enabled=false,sheep_jit_memory_bail=false;
extern bool sheep_mmu_code_is_rom(const uint8_t*,uint32_t);
extern int nw_jit_island_pack_selftest();
extern int sheep_live_itlb_selftest();
extern int sheep_rom_window_loop_selftest();
extern int sheep_fetch_barrier_selftest();
extern int nw_jit_raw_literals_selftest();
extern "C" uint64_t sheep_jit_abi_check(nw_jit_fn,nw_jit_cpu*);
static unsigned checked(nw_jit_fn fn,nw_jit_cpu *cpu){auto result=sheep_jit_abi_check(fn,cpu);if(result>>32){fprintf(stderr,"ARM64 ABI preservation mask=%llx\n",(unsigned long long)(result>>32));throw std::runtime_error("ARM64 JIT ABI violation");}return uint32_t(result);}
struct Entry {const uint8_t* host=nullptr;uint32_t pc=0;unsigned n=0;unsigned warmup=0;bool negative=false;bool rom=false;unsigned memory_count=0;nw_jit_fn fn=nullptr;std::array<uint8_t,sheep_native_max_ops*4> bytes{};};
static std::array<Entry,65536> cache;
unsigned char sheep_jit_reject_filter[4194304]{};
alignas(64) uint64_t sheep_rom_dispatch_words[1048576]{};
bool sheep_rom_decode_ready=false;
bool sheep_rom_direct_enabled=false;
uintptr_t sheep_rom_direct_base=0;
extern bool sheep_encode_rom_handler(uint32_t,uint32_t&);
static bool rom_direct_any=false;
void sheep_rom_direct_restore(unsigned slot){
 auto &word=sheep_rom_dispatch_words[slot];if((word&3)!=3)return;
 uintptr_t offset=((slot-sheep_jit_filter_slot_rom(reinterpret_cast<const uint8_t*>(sheep_rom_host_begin)))&1048575u)*4u;
 if(offset+4>sheep_rom_host_end-sheep_rom_host_begin)__builtin_trap();
 const auto *p=reinterpret_cast<const uint8_t*>(sheep_rom_host_begin+offset);
 uint32_t op=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
 uint32_t description;if(!sheep_encode_rom_handler(op,description))__builtin_trap();
 word=(uint64_t(op)<<32)|description;
}
void sheep_rom_direct_reset(){
 if(rom_direct_any)for(unsigned slot=0;slot<1048576;slot++)if((sheep_rom_dispatch_words[slot]&3)==3)sheep_rom_direct_restore(slot);
 rom_direct_any=false;sheep_rom_direct_base=0;
}
static void reset_native_and_rom(){sheep_rom_direct_reset();nw_jit_reset();}
bool sheep_rom_direct_publish(const uint8_t *code,uint32_t pc,unsigned n,nw_jit_fn fn){
 uintptr_t host=reinterpret_cast<uintptr_t>(code),address=reinterpret_cast<uintptr_t>(fn),base=reinterpret_cast<uintptr_t>(nw_jit_native_base());
 if(!sheep_rom_direct_enabled||!sheep_rom_decode_ready||host<sheep_rom_host_begin||host>=sheep_rom_host_end||((host|pc)&3)||!n||n>32||host+uintptr_t(n)*4>sheep_rom_host_end||!base||address<base||address-base>=(uintptr_t(1)<<27)||((address-base)&3))return false;
 if(sheep_rom_direct_base!=base){sheep_rom_direct_reset();sheep_rom_direct_base=base;}
 unsigned slot=sheep_jit_filter_slot_rom(code);
 uint32_t packed=uint32_t((address-base)>>2)<<5|(n-1);
 sheep_rom_dispatch_words[slot]=(uint64_t(pc)<<32)|(packed<<2)|3u;
 rom_direct_any=true;return true;
}

PPCOpcode sheep_rom_disabled_fpu_handler=nullptr;

static SetPRS shadow{};
static uint64_t blocks,insns,verified,mismatches,compiles,negative_hits,rom_compiles,ram_compiles,warmups;
static int mode=-1;
bool sheep_rom_only=false;
uintptr_t sheep_rom_host_begin=0,sheep_rom_host_end=0;
extern void sheep_mmu_rom_bounds(uintptr_t&,uintptr_t&);
bool sheep_hot_used=false;
static uint64_t generation;
// Startup fixtures temporarily replace real ROM bytes. Revoke every compiler
// and hot reference before restoring those bytes; normal execution never calls.
void sheep_rom_window_test_reset_native_refs(){
 sheep_rom_direct_reset();
 for(auto &entry:cache)entry=Entry{};
 for(auto &slot:sheep_hot_slots)slot=SheepHotSlot{};
 nw_jit_reset();generation=sheep_hot_epoch=nw_jit_wrap_count();
}
static uint64_t memory_hits=0,memory_misses=0;
extern uint64_t sheep_steady_spans,sheep_steady_instructions;
static void stats(){fprintf(stderr,"SHEEP-PIPPIN JIT mode=%d blocks=%llu instructions=%llu verified=%llu mismatches=%llu compiles=%llu guest_ns=%llu negative_hits=%llu memory_hits=%llu memory_misses=%llu fast_blocks=%llu fast_instructions=%llu fastpath_used=%u rom_compiles=%llu ram_compiles=%llu warmups=%llu counter_scope=%s steady_spans=%llu steady_insns=%llu\n",mode,(unsigned long long)(blocks+sheep_hot_blocks),(unsigned long long)(insns+sheep_hot_instructions),(unsigned long long)verified,(unsigned long long)mismatches,(unsigned long long)compiles,(unsigned long long)get_virt_time_ns(),(unsigned long long)negative_hits,(unsigned long long)memory_hits,(unsigned long long)memory_misses,(unsigned long long)sheep_hot_blocks,(unsigned long long)sheep_hot_instructions,sheep_hot_used,(unsigned long long)rom_compiles,(unsigned long long)ram_compiles,(unsigned long long)warmups,mode==1?"verified":"normal-counters-disabled",(unsigned long long)sheep_steady_spans,(unsigned long long)sheep_steady_instructions);}
extern uint8_t *sheep_mmu_fastmem(uint32_t va,unsigned size,bool write);
static bool &memory_bail=sheep_jit_memory_bail;
static uint8_t *written=nullptr;
static unsigned written_size=0;
static uint32_t written_old=0,written_new=0;

static uint32_t read_be(const uint8_t *p,unsigned n){uint32_t v=0;for(unsigned i=0;i<n;i++)v=(v<<8)|p[i];return v;}
static void write_be(uint8_t *p,unsigned n,uint32_t v){for(unsigned i=0;i<n;i++)p[n-1-i]=uint8_t(v>>(i*8));}
static int mapped_memory(SetPRS *cpu,uint32_t op){
 unsigned prim=op>>26,rd=(op>>21)&31,ra=(op>>16)&31;
 bool store=(prim==36||prim==37||prim==38||prim==39||prim==44||prim==45);
 unsigned size=(prim>=40)?2:(prim==34||prim==35||prim==38||prim==39)?1:4;
 uint32_t ea=(ra?cpu->gpr[ra]:0)+int16_t(op);
 auto *host=sheep_mmu_fastmem(ea,size,store);
 if(!host){memory_bail=true;memory_misses++;return 0;}
 if(store){
  if(mode==1){written=host;written_size=size;written_old=read_be(host,size);}
  write_be(host,size,cpu->gpr[rd]);
  if(mode==1)written_new=read_be(host,size);
 }else{
  uint32_t value=read_be(host,size);
  cpu->gpr[rd]=(prim==42||prim==43)?uint32_t(int32_t(int16_t(value))):value;
 }
 if(prim&1)cpu->gpr[ra]=ea;
 memory_hits++;return 1;
}
static bool eligible(uint32_t op){unsigned p=op>>26;return sheep_jit_maybe(op)&&(sheep_jit_memory_primary(op)||nw_jit_op_supported(op));}
static nw_jit_layout native_layout = {
 offsetof(SetPRS,gpr), offsetof(SetPRS,cr), offsetof(SetPRS,spr)+SPR::XER*4,
 offsetof(SetPRS,spr)+SPR::LR*4, offsetof(SetPRS,spr)+SPR::CTR*4, offsetof(SetPRS,pc), reinterpret_cast<void*>(mapped_memory)
};
extern void sheep_mmu_jit_layout(nw_jit_layout*);
extern void sheep_mmu_test_page(uint8_t*,uint32_t,uint16_t,bool);
static void expanded_integer_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags,next=ppc_next_instruction_address;const bool bail=memory_bail;
 auto layout=native_layout;layout.leaf=1;layout.fast_read_slot=layout.fast_write_slot=0;layout.trace_host=layout.trace_old=layout.trace_size=0;
 unsigned cases=0;
 auto trial=[&](uint32_t op,SetPRS input){
  uint32_t ops[]={0x38630001,0x68840001,op,0x60000000,0x60000000,0x60000000,0x60000000,0x60000000};
  if(!eligible(op))throw std::runtime_error("Expanded integer fixture not qualified");
  auto fn=nw_jit_compile_mapped(ops,8,input.pc,&layout);if(!fn)throw std::runtime_error("Expanded integer fixture compile failed");
  SetPRS actual=input;memory_bail=false;unsigned retired=checked(fn,reinterpret_cast<nw_jit_cpu*>(&actual));bool bailed=memory_bail;
  ppc_state=input;exec_flags=0;ppc_next_instruction_address=next;unsigned expected=0;
  for(;expected<8;){ppc_main_opcode(ppc_opcode_grabber,ops[expected]);expected++;if(exec_flags&~EXEF_BRANCH)throw std::runtime_error("Expanded integer reference trapped");
   if(exec_flags&EXEF_BRANCH){ppc_state.pc=ppc_next_instruction_address;exec_flags=0;break;}ppc_state.pc+=4;}
  if(bailed||retired!=expected||std::memcmp(&actual,&ppc_state,sizeof(SetPRS))){
   fprintf(stderr,"Expanded integer mismatch op=%08x case=%u count=%u/%u pc=%08x/%08x CR=%08x/%08x XER=%08x/%08x LR=%08x/%08x CTR=%08x/%08x\n",op,cases,retired,expected,actual.pc,ppc_state.pc,actual.cr,ppc_state.cr,actual.spr[SPR::XER],ppc_state.spr[SPR::XER],actual.spr[SPR::LR],ppc_state.spr[SPR::LR],actual.spr[SPR::CTR],ppc_state.spr[SPR::CTR]);
   throw std::runtime_error("Expanded integer full-state oracle mismatch");
  }cases++;
 };
 auto input=[&](uint32_t seed){SetPRS cpu=saved;cpu.pc=0x10000000;for(unsigned r=0;r<32;r++)cpu.gpr[r]=seed^(r*0x1020301u);cpu.cr=seed^0x872513abu;cpu.spr[SPR::XER]=seed;return cpu;};
 for(unsigned xo:{33u,129u,193u,225u,257u,289u,417u,449u})for(unsigned d=0;d<32;d++)for(unsigned a=0;a<32;a++)for(unsigned alias=0;alias<2;alias++)for(unsigned truth=0;truth<4;truth++){
  unsigned rb=alias?a:(a+13)%32;auto cpu=input(truth*0x3157a029u);
  cpu.cr=(cpu.cr&~(0x80000000u>>a))|((truth&1)?0x80000000u>>a:0);
  cpu.cr=(cpu.cr&~(0x80000000u>>rb))|((truth&2)?0x80000000u>>rb:0);
  trial((19u<<26)|(d<<21)|(a<<16)|(rb<<11)|(xo<<1),cpu);
 }
 for(unsigned xo:{339u,467u})for(unsigned reg=0;reg<32;reg++)for(unsigned high=0;high<16;high++)for(uint32_t low:{0u,127u,0xff7fu}){
  auto cpu=input((high<<28)|low);cpu.gpr[reg]=((high<<28)|low)^0x15550155u;
  trial((31u<<26)|(reg<<21)|(1u<<16)|(xo<<1),cpu);
 }
 for(unsigned d=0;d<32;d++)for(unsigned a=0;a<32;a++)for(int immediate:{0,1,-1,-32768,32767})for(uint32_t seed:{0u,1u,0xffffffffu,0x80000000u}){
  auto cpu=input(seed);cpu.gpr[a]=seed;trial((8u<<26)|(d<<21)|(a<<16)|uint16_t(immediate),cpu);
 }
 for(unsigned bo=0;bo<32;bo++)for(unsigned bi=0;bi<32;bi++)for(unsigned modifier=0;modifier<4;modifier++)for(unsigned ctr=0;ctr<3;ctr++)for(unsigned cr=0;cr<2;cr++)for(int offset:{0,4,-4,-32768}){
  auto cpu=input((bi&1)?0x81510325u:0x41398512u);cpu.pc=(bi&1)?0xfffffff0u:0x10000000u;
  cpu.spr[SPR::CTR]=ctr;cpu.cr=cr?(0x80000000u>>bi):0;cpu.spr[SPR::LR]=0x13572468;
  trial((16u<<26)|(bo<<21)|(bi<<16)|(uint16_t(offset)&0xfffc)|modifier,cpu);
 }
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;memory_bail=bail;reset_native_and_rom();
 fprintf(stderr,"EXPANDED INTEGER SELFTEST %u full CPU/CR/XER/carry/GPR-alias/CTR/LR/LK/AA/branch/wrap cases passed\n",cases);
}
static void privileged_srr_selftest(){
 const auto saved=ppc_state;const auto saved_flags=exec_flags,saved_nia=ppc_next_instruction_address;const bool saved_bail=memory_bail;
 auto layout=native_layout;layout.leaf=1;layout.fast_read_slot=layout.fast_write_slot=0;layout.trace_host=layout.trace_old=layout.trace_size=0;
 alignas(4096) std::array<uint8_t,4096> ram{},initial{},native_ram{};
 unsigned cases=0;
 auto trial=[&](const uint32_t *ops,unsigned n,const SetPRS &input,unsigned count,bool memory){
  auto fn=nw_jit_compile_mapped(ops,n,input.pc,&layout);if(!fn||nw_jit_last_privilege_guards()!=1)throw std::runtime_error("SRR guard compile/hoisting fixture failed");
  SetPRS actual=input;initial=ram;memory_bail=false;
  unsigned retired=checked(fn,reinterpret_cast<nw_jit_cpu*>(&actual));native_ram=ram;const bool bailed=memory_bail;
  ppc_state=input;ram=initial;exec_flags=0;ppc_next_instruction_address=saved_nia;
  for(unsigned i=0;i<count;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);if(exec_flags)throw std::runtime_error("SRR supervisor fixture unexpectedly trapped");ppc_state.pc+=4;}
  if(retired!=count||bailed!=(count<n)||std::memcmp(&actual,&ppc_state,sizeof(SetPRS))||(memory&&native_ram!=ram)){
   fprintf(stderr,"SRR fixture mismatch case=%u retired=%u/%u PR=%u bail=%u pc=%08x/%08x SRR0=%08x/%08x SRR1=%08x/%08x\n",cases,retired,count,!!(input.msr&MSR::PR),bailed,actual.pc,ppc_state.pc,actual.spr[SPR::SRR0],ppc_state.spr[SPR::SRR0],actual.spr[SPR::SRR1],ppc_state.spr[SPR::SRR1]);
   throw std::runtime_error("SRR full CPU/RAM/privilege-prefix fixture mismatch");
  }
  cases++;
 };
 const uint32_t seeds[]={0,0xffffffffu,0x80000001u,0xabcdef03u};
 auto input=[&](unsigned pr,uint32_t seed){SetPRS cpu=saved;cpu.pc=0x10000000;cpu.msr=(cpu.msr&~MSR::PR)|(pr?MSR::PR:0);for(unsigned i=0;i<32;i++)cpu.gpr[i]=seed^(i*0x1020301u);cpu.spr[SPR::SRR0]=seed^0xdeadbeefu;cpu.spr[SPR::SRR1]=~seed;return cpu;};
 auto opcode=[](unsigned spr,unsigned xo,unsigned reg){return (31u<<26)|(reg<<21)|((spr&31)<<16)|((spr&0x3e0)<<6)|(xo<<1);};
 for(unsigned spr:{26u,27u})for(unsigned xo:{339u,467u})for(unsigned reg=0;reg<32;reg++)for(unsigned pr=0;pr<2;pr++)for(unsigned prefix:{0u,1u,3u})for(uint32_t seed:seeds){
  uint32_t ops[8];for(unsigned i=0;i<8;i++)ops[i]=i&1?0x68840001u:0x38630001u;ops[prefix]=opcode(spr,xo,reg);
  trial(ops,8,input(pr,seed),pr?prefix:8,false);
 }
 for(unsigned pr=0;pr<2;pr++)for(uint32_t seed:seeds)for(unsigned reg=0;reg<16;reg++){
  uint32_t ops[]={opcode(26,467,reg),opcode(27,467,31-reg),opcode(26,339,31-reg),opcode(27,339,reg),0x60000000u,0x60000000u,0x60000000u,0x60000000u};
  trial(ops,8,input(pr,seed),pr?0:8,false);
 }
 sheep_mmu_test_page(ram.data(),0x28000000,0x61,false);
 for(unsigned pr=0;pr<2;pr++)for(uint32_t seed:seeds)for(unsigned xo:{339u,467u})for(unsigned spr:{26u,27u}){
  ram.fill(uint8_t(seed));auto cpu=input(pr,seed);cpu.gpr[6]=0x28000000;
  uint32_t ops[]={0x38630001,0x90660000,opcode(spr,xo,3),0x68840001,0x60000000,0x60000000,0x60000000,0x60000000};
  trial(ops,8,cpu,pr?2:8,true);
 }
 sheep_mmu_test_page(nullptr,0,0,false);ppc_state=saved;exec_flags=saved_flags;ppc_next_instruction_address=saved_nia;memory_bail=saved_bail;reset_native_and_rom();
 fprintf(stderr,"PRIVILEGED SRR SELFTEST %u full CPU/RAM/read/write/alias/guard-hoisting/precise-prefix cases passed\n",cases);
}
static void memory_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 std::array<uint8_t,4096> ram{},original{},native{};
 unsigned passed=0;
 for(unsigned indexed=0;indexed<2;indexed++)for(unsigned primary=32;primary<=45;primary++)for(unsigned variant=0;variant<7;variant++){
  for(unsigned i=0;i<ram.size();i++)ram[i]=uint8_t(i*37+11);
  original=ram;
  uint32_t ea=0x1234010;if(variant==5)ea++;if(variant==6)ea=0x1234fff;
  unsigned size=primary>=40?2:(primary==34||primary==35||primary==38||primary==39)?1:4;
  bool store=primary==36||primary==37||primary==38||primary==39||primary==44||primary==45;
  uint16_t page_flags=1|32|64;if(variant==2)page_flags=2|32|64;if(variant==3)page_flags=1|64;if(variant==4)page_flags=1|32;
  bool access=variant!=1&&variant!=2&&(!store||(variant!=3&&variant!=4))&&((ea&4095)+size<=4096);
  sheep_mmu_test_page(ram.data(),0x1234000,page_flags,variant==1);
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[6]=indexed?0x124:0;ppc_state.gpr[7]=ea-4-ppc_state.gpr[6];ppc_state.gpr[8]=0x89abcdef;
  static const unsigned indexed_xo[]={23,55,87,119,151,183,215,247,279,311,343,375,407,439};
  uint32_t memory_op=indexed?((31u<<26)|(8<<21)|(7<<16)|(6<<11)|(indexed_xo[primary-32]<<1)):((primary<<26)|(8<<21)|(7<<16));
  uint32_t ops[]={0x38e70004,0x69290001,0x396b0003,memory_op};
  auto fn=nw_jit_compile_mapped(ops,4,ppc_state.pc,&native_layout);
  if(!fn)throw std::runtime_error("Memory selftest compilation failed");
  SetPRS result=ppc_state;memory_bail=false;written=nullptr;unsigned actual=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));native=ram;ram=original;
  unsigned count=access?4:3;
  // Exercise the exact inline normal-mode cache path against the same oracle.
  SetPRS initial=ppc_state;auto *key=reinterpret_cast<const uint8_t*>(ops);
  auto &slot=sheep_hot_slots[sheep_hot_index(key,ppc_state.pc)];slot=SheepHotSlot{key,ppc_state.pc,4,fn,sheep_hot_epoch};
  const bool enabled=sheep_hot_enabled;sheep_hot_enabled=true;unsigned hot_count=777;
  if(sheep_jit_hot(key,3,hot_count)||hot_count!=777)throw std::runtime_error("ROM fast cache ignored instruction budget");
  slot.epoch++;if(sheep_jit_hot(key,4,hot_count))throw std::runtime_error("ROM fast cache accepted stale epoch");slot.epoch--;
  if(sheep_jit_hot(key+4,4,hot_count))throw std::runtime_error("ROM fast cache accepted wrong code pointer");
  ppc_state.pc+=4;if(sheep_jit_hot(key,4,hot_count))throw std::runtime_error("ROM fast cache accepted wrong PC");ppc_state.pc-=4;
  bool matched=sheep_jit_hot(key,4,hot_count);SetPRS hot_result=ppc_state;if(hot_count)hot_result.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:initial.pc+hot_count*4;
  if(!matched||hot_count!=count||std::memcmp(hot_result.gpr,result.gpr,sizeof(result.gpr))||hot_result.pc!=result.pc||ram!=native)throw std::runtime_error("ROM fast cache CPU/RAM result mismatch");
  ppc_state=initial;ram=original;exec_flags=0;unsigned accounted_count=777;
  if(!sheep_jit_hot<true>(key,4,accounted_count)||accounted_count!=count||
     ppc_state.pc!=uint32_t(initial.pc+(count?count-1:0)*4)||
     std::memcmp(ppc_state.gpr,result.gpr,sizeof(result.gpr))||ram!=native)
   throw std::runtime_error("Accounted ROM fast cache CPU/RAM/source-PC mismatch");
  sheep_hot_enabled=enabled;slot=SheepHotSlot{};ppc_state=initial;ram=original;
  exec_flags=0;
  for(unsigned i=0;i<count;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);ppc_state.pc+=4;}
  if(actual!=count||memory_bail==access||std::memcmp(result.gpr,ppc_state.gpr,sizeof(result.gpr))||result.pc!=ppc_state.pc||native!=ram){
   fprintf(stderr,"NATIVE RAM SELFTEST FAIL primary=%u variant=%u bail=%u expected=%u pc=%08x/%08x\n",primary,variant,memory_bail,!access,result.pc,ppc_state.pc);
   throw std::runtime_error("Native RAM guard/oracle mismatch");
  }
  passed++;
 }
 sheep_mmu_test_page(nullptr,0,0,false);ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;memory_bail=false;written=nullptr;reset_native_and_rom();
 sheep_hot_blocks=sheep_hot_instructions=0;
 fprintf(stderr,"ACCOUNTED DISPATCH SELFTEST 196 precise source-PC and RAM cases passed\n");
 fprintf(stderr,"NATIVE RAM SELFTEST %u load/store/guard cases passed\nROM FAST-CACHE SELFTEST %u cache/budget/guard cases passed\n",passed,passed);
}

static void accounted_boundary_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 unsigned passed=0;uint32_t ops[16];for(auto &op:ops)op=0x38630001;
 const bool enabled=sheep_hot_enabled;sheep_hot_enabled=true;
 for(uint32_t pc:{0u,0xffc0u,0xffffffc0u,0xfffffff0u})for(unsigned n=1;n<=16;n++){
  if((pc&4095)+n*4>4096)continue;
  ppc_state=saved;ppc_state.pc=pc;ppc_state.gpr[3]=0xfffffffeu;exec_flags=0;
  auto fn=nw_jit_compile_mapped(ops,n,pc,&native_layout);if(!fn)throw std::runtime_error("Boundary block compilation failed");
  SetPRS initial=ppc_state,result=initial;if(checked(fn,reinterpret_cast<nw_jit_cpu*>(&result))!=n)throw std::runtime_error("Boundary native count mismatch");
  auto *key=reinterpret_cast<const uint8_t*>(ops);auto &slot=sheep_hot_slots[sheep_hot_index(key,pc)];slot=SheepHotSlot{key,pc,n,fn,sheep_hot_epoch};
  unsigned count=777;
  if(sheep_jit_hot<true>(key,n-1,count)||count!=777||ppc_state.pc!=pc)throw std::runtime_error("Accounted boundary ignored timer budget");
  if(!sheep_jit_hot<true>(key,n,count)||count!=n||ppc_state.pc!=uint32_t(pc+(n-1)*4))throw std::runtime_error("Accounted boundary source PC differs");
  uint32_t actual=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:ppc_state.pc+4;
  if(actual!=result.pc||std::memcmp(ppc_state.gpr,result.gpr,sizeof(result.gpr)))throw std::runtime_error("Accounted boundary final CPU differs");
  slot=SheepHotSlot{};passed++;
 }
 sheep_hot_enabled=enabled;ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;reset_native_and_rom();
 fprintf(stderr,"ACCOUNTED BOUNDARY SELFTEST %u page/wrap/count/budget cases passed\n",passed);
}

static void long_block_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 unsigned passed=0;std::array<uint8_t,4096> ram{},original,native;uint32_t ops[32];
 for(unsigned n=17;n<=32;n++)for(unsigned variant=0;variant<6;variant++){
  for(unsigned i=0;i<4096;i++)ram[i]=uint8_t(i*37+11);original=ram;
  sheep_mmu_test_page(ram.data(),0x1234000,0x61,variant==4);
  ppc_state=saved;ppc_state.pc=variant==5?0xffffff80u:0x10000000u;ppc_state.gpr[7]=0x1234010;ppc_state.gpr[8]=0x89abcdef;ppc_state.cr=0;
  for(unsigned i=0;i<n;i++)ops[i]=i&1?0x38840002u:0x38630001u;
  unsigned count=n;
  if(variant==1)ops[n-1]=0x91070000u;
  if(variant==2)ops[n-1]=0x81070000u;
  if(variant==3){unsigned at=n-3;ops[at]=(16u<<26)|(4u<<21)|(8u<<16)|0x40u;count=at+1;}
  if(variant==4){ops[n-2]=0x81070000u;count=n-2;}
  auto fn=nw_jit_compile_mapped(ops,n,ppc_state.pc,&native_layout);if(!fn)throw std::runtime_error("Long mapped block compilation failed");
  SetPRS initial=ppc_state,result=initial;exec_flags=0;memory_bail=false;written=nullptr;
  unsigned actual=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));native=ram;ram=original;
  auto *key=reinterpret_cast<const uint8_t*>(ops);auto &slot=sheep_hot_slots[sheep_hot_index(key,initial.pc)];slot=SheepHotSlot{key,initial.pc,n,fn,sheep_hot_epoch};bool enabled=sheep_hot_enabled;sheep_hot_enabled=true;unsigned hot_count=777;
  if(sheep_jit_hot<true>(key,n-1,hot_count)||hot_count!=777||ppc_state.pc!=initial.pc)throw std::runtime_error("Long block ignored timer budget");
  if(!sheep_jit_hot<true>(key,n,hot_count)||hot_count!=count||ppc_state.pc!=initial.pc+(count-1)*4||ram!=native||std::memcmp(ppc_state.gpr,result.gpr,sizeof(result.gpr)))throw std::runtime_error("Long hot block source-PC/CPU/RAM differs");
  auto hot_target=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:ppc_state.pc+4;if(hot_target!=result.pc)throw std::runtime_error("Long hot block branch target differs");
  sheep_hot_enabled=enabled;slot=SheepHotSlot{};ppc_state=initial;ram=original;exec_flags=0;
  for(unsigned i=0;i<count;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);ppc_state.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:ppc_state.pc+4;exec_flags=0;}
  if(actual!=count||result.pc!=ppc_state.pc||result.cr!=ppc_state.cr||std::memcmp(result.gpr,ppc_state.gpr,sizeof(result.gpr))||result.spr[SPR::XER]!=ppc_state.spr[SPR::XER]||ram!=native)throw std::runtime_error("Long block interpreter oracle mismatch");
  passed++;
 }
 sheep_mmu_test_page(nullptr,0,0,false);ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;memory_bail=false;written=nullptr;reset_native_and_rom();
 fprintf(stderr,"LONG BLOCK SELFTEST %u length/CPU/RAM/guard/branch/page/budget cases passed\n",passed);
}

static void superblock_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 std::array<uint8_t,4096> ram{},original{},native{};unsigned passed=0;
 for(unsigned kind=0;kind<3;kind++)for(unsigned bit=0;bit<2;bit++)for(unsigned inside=0;inside<2;inside++){
  for(unsigned i=0;i<ram.size();i++)ram[i]=uint8_t(i*29+7);original=ram;
  sheep_mmu_test_page(ram.data(),0x1234000,1|32|64,false);
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[7]=0x123400c;ppc_state.gpr[8]=0x89abcdef;
  ppc_state.cr=(ppc_state.cr&~(1u<<23))|(bit<<23);uint32_t target=inside?0x10000018:0x10000100;
  ppc_state.spr[SPR::LR]=target;ppc_state.spr[SPR::CTR]=target;
  uint32_t conditional=kind==0?((19u<<26)|(5<<21)|(8<<16)|(16<<1)):kind==1?((16u<<26)|(4<<21)|(8<<16)|((target-0x10000010)&0xfffc)):((19u<<26)|(12<<21)|(8<<16)|(528<<1));
  uint32_t ops[]={0x38e70004,0x91070000,0x81270000,0x39290001,conditional,0xa9470000,0x48000200};
  auto fn=nw_jit_compile_mapped(ops,7,ppc_state.pc,&native_layout);if(!fn)throw std::runtime_error("Superblock compile failed");
  SetPRS initial=ppc_state,result=ppc_state;memory_bail=false;written=nullptr;
  unsigned actual=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));native=ram;ram=original;
  auto *key=reinterpret_cast<const uint8_t*>(ops);auto &slot=sheep_hot_slots[sheep_hot_index(key,initial.pc)];slot=SheepHotSlot{key,initial.pc,7,fn,sheep_hot_epoch};bool enabled=sheep_hot_enabled;sheep_hot_enabled=true;exec_flags=0;unsigned count=0;
  if(!sheep_jit_hot(key,7,count)||count!=actual)throw std::runtime_error("Fast superblock count mismatch");SetPRS hot=ppc_state;hot.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:initial.pc+count*4;
  if(std::memcmp(hot.gpr,result.gpr,sizeof(hot.gpr))||hot.pc!=result.pc||ram!=native)throw std::runtime_error("Fast superblock result mismatch");
  ppc_state=initial;ram=original;exec_flags=0;count=0;
  if(!sheep_jit_hot<true>(key,7,count)||count!=actual||ppc_state.pc!=initial.pc+(count-1)*4)
   throw std::runtime_error("Accounted superblock source-PC/count mismatch");
  hot=ppc_state;hot.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:hot.pc+4;
  if(std::memcmp(hot.gpr,result.gpr,sizeof(hot.gpr))||hot.pc!=result.pc||ram!=native)
   throw std::runtime_error("Accounted superblock branch/RAM mismatch");
  sheep_hot_enabled=enabled;slot=SheepHotSlot{};ppc_state=initial;ram=original;exec_flags=0;unsigned oracle=0;
  while(oracle<7){unsigned index=(ppc_state.pc-initial.pc)/4;if(index>=7)throw std::runtime_error("Oracle escaped before branch");ppc_main_opcode(ppc_opcode_grabber,ops[index]);oracle++;bool branch=exec_flags&EXEF_BRANCH;ppc_state.pc=branch?ppc_next_instruction_address:ppc_state.pc+4;exec_flags=0;if(branch)break;}
  if(actual!=oracle||std::memcmp(result.gpr,ppc_state.gpr,sizeof(result.gpr))||result.cr!=ppc_state.cr||result.pc!=ppc_state.pc||native!=ram)throw std::runtime_error("Superblock interpreter/memory/count mismatch");passed++;
 }
 sheep_mmu_test_page(nullptr,0,0,false);ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;memory_bail=false;written=nullptr;sheep_hot_blocks=sheep_hot_instructions=0;reset_native_and_rom();
 fprintf(stderr,"SUPERBLOCK SELFTEST %u early-branch/store/load/count cases passed\n",passed);
}

static void direct_state_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 uint32_t rng=0x50315050;auto random=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};
 unsigned passed=0;
 for(unsigned iteration=0;iteration<4096;iteration++){
  ppc_state=saved;for(auto &r:ppc_state.gpr)r=random();
  ppc_state.cr=random();ppc_state.spr[SPR::XER]=random()&0xe000007f;
  ppc_state.spr[SPR::LR]=random()&~3u;ppc_state.spr[SPR::CTR]=random()&~3u;ppc_state.pc=0x10000000;
  unsigned rd=random()&31,ra=random()&31,rb=random()&31;
  static const unsigned xo[]={0,32,19,144,26,24,536,922,954,28,60,124,284,316,412,444,476,266,40,235,11,10,138,8,136,522,520,104};
  static const unsigned prim[]={7,10,11,12,13,14,15,20,21,23,24,25,26,27,28,29};
  uint32_t op;
  if(iteration%4==0){op=(31u<<26)|(rd<<21)|(ra<<16)|(rb<<11)|(xo[random()%(sizeof(xo)/sizeof(*xo))]<<1)|(random()&1);}
  else if(iteration%4==1){op=(prim[random()%(sizeof(prim)/sizeof(*prim))]<<26)|(rd<<21)|(ra<<16)|(random()&65535);}
  else if(iteration%4==2){op=(16u<<26)|((random()&1?12u:4u)<<21)|(ra<<16)|((random()&0x7fffu)&~3u);}
  else {op=(18u<<26)|(random()&0x03fffffdu);}
  if(iteration%8==7){unsigned spr=random()&1?8:9;op=(31u<<26)|(rd<<21)|(spr<<16)|((random()&1?339u:467u)<<1);}
  if(iteration%16==3){unsigned bo=(random()&1?12u:4u)|(random()&1);op=(19u<<26)|(bo<<21)|(ra<<16)|((random()&1?16u:528u)<<1);}
  unsigned primary=op>>26,extended=(op>>1)&1023;
  if(primary==10||primary==11)op &= ~(3u<<21);
  if(primary==31){
   if(extended==0||extended==32)op &= ~((3u<<21)|1u);
   if(extended==19)op &= ~0x001ff801u;
   if(extended==144)op &= ~0x00100801u;
   if(extended==26||extended==104)op &= ~0x0000f800u;
  }
  if(!eligible(op))continue;
  uint32_t ops[]={0x38630001,iteration&1?0x38630002u:0x38840002u,iteration&1?0x38630003u:0x38a50003u,op};
  auto fn=nw_jit_compile_mapped(ops,4,ppc_state.pc,&native_layout);if(!fn)continue;
  SetPRS initial=ppc_state,result=ppc_state;if(checked(fn,reinterpret_cast<nw_jit_cpu*>(&result))!=4)throw std::runtime_error("Mapped CPU block count mismatch");
  auto *key=reinterpret_cast<const uint8_t*>(ops);auto &slot=sheep_hot_slots[sheep_hot_index(key,initial.pc)];slot=SheepHotSlot{key,initial.pc,4,fn,sheep_hot_epoch};
  bool enabled=sheep_hot_enabled;sheep_hot_enabled=true;exec_flags=0;unsigned consumed=0;
  if(!sheep_jit_hot(key,4,consumed)||consumed!=4)throw std::runtime_error("Fast CPU cache missed oracle block");
  SetPRS hot_result=ppc_state;hot_result.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:initial.pc+consumed*4;
  if(std::memcmp(hot_result.gpr,result.gpr,sizeof(result.gpr))||hot_result.cr!=result.cr||hot_result.pc!=result.pc||hot_result.spr[SPR::XER]!=result.spr[SPR::XER]||hot_result.spr[SPR::LR]!=result.spr[SPR::LR]||hot_result.spr[SPR::CTR]!=result.spr[SPR::CTR])throw std::runtime_error("Fast CPU cache control-flow/oracle mismatch");
  ppc_state=initial;exec_flags=0;consumed=0;
  if(!sheep_jit_hot<true>(key,4,consumed)||consumed!=4||ppc_state.pc!=initial.pc+12)
   throw std::runtime_error("Accounted CPU cache source-PC/count mismatch");
  hot_result=ppc_state;hot_result.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:hot_result.pc+4;
  if(std::memcmp(hot_result.gpr,result.gpr,sizeof(result.gpr))||hot_result.cr!=result.cr||hot_result.pc!=result.pc||hot_result.spr[SPR::XER]!=result.spr[SPR::XER]||hot_result.spr[SPR::LR]!=result.spr[SPR::LR]||hot_result.spr[SPR::CTR]!=result.spr[SPR::CTR])
   throw std::runtime_error("Accounted CPU cache control-flow/oracle mismatch");
  sheep_hot_enabled=enabled;slot=SheepHotSlot{};ppc_state=initial;
  exec_flags=0;
  for(auto instruction:ops){ppc_main_opcode(ppc_opcode_grabber,instruction);ppc_state.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:ppc_state.pc+4;}
  if(std::memcmp(result.gpr,ppc_state.gpr,sizeof(result.gpr))||result.cr!=ppc_state.cr||result.pc!=ppc_state.pc||result.spr[SPR::XER]!=ppc_state.spr[SPR::XER]||result.spr[SPR::LR]!=ppc_state.spr[SPR::LR]||result.spr[SPR::CTR]!=ppc_state.spr[SPR::CTR]){
   fprintf(stderr,"DIRECT-STATE SELFTEST FAIL op=%08x iteration=%u next=%08x/%08x\n",op,iteration,result.pc,ppc_state.pc);
   throw std::runtime_error("Direct-state JIT selftest failed");
  }
  passed++;
 }
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;reset_native_and_rom();
 fprintf(stderr,"DIRECT-STATE SELFTEST %u randomized blocks passed\n",passed);
}

extern void sheep_mmu_test_data_pair(uint8_t*,uint8_t*,uint32_t,uint16_t,uint16_t,bool);
static void memory_cache_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 std::array<uint8_t,8192> read{},write{},original_read{},original_write{},native_read{},native_write{};unsigned passed=0;
 for(unsigned primary=32;primary<=45;primary++)for(unsigned variant=0;variant<8;variant++){
  for(unsigned i=0;i<read.size();i++){read[i]=uint8_t(i*37+11);write[i]=uint8_t(i*17+5);}original_read=read;original_write=write;
  uint16_t first_flags=1|32|64;if(variant==1)first_flags=1|32;if(variant==2)first_flags=1|64;if(variant==6)first_flags=2;
  bool cross=variant==3||variant==4;uint32_t ea=0x1234000+(cross?4094:16);if(variant==5)ea++;
  bool store=primary==36||primary==37||primary==38||primary==39||primary==44||primary==45;
  unsigned size=primary>=40?2:(primary==34||primary==35||primary==38||primary==39)?1:4;
  unsigned count=4;if(variant==6)count=0;
  else if(variant==4||(store&&(variant==1||variant==2)))count=2;
  uint8_t *writes=variant==7?write.data():read.data();
  sheep_mmu_test_data_pair(read.data(),writes,0x1234000,first_flags,1|32|64,variant!=4);
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[7]=ea-2;ppc_state.gpr[10]=0x89abcdef;exec_flags=0;
  uint32_t ops[]={(43u<<26)|(8<<21)|(7<<16)|2u,0x69290001u,(primary<<26)|(10<<21)|(7<<16)|8u,0x38630001u};
  auto fn=nw_jit_compile_mapped(ops,4,ppc_state.pc,&native_layout);if(!fn||(!nw_jit_last_fastmem_ops()&&nw_jit_last_ram_cache_probes()!=1))throw std::runtime_error("RAM page cache was not emitted");
  SetPRS initial=ppc_state,result=ppc_state;memory_bail=false;written=nullptr;
  unsigned actual=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));native_read=read;native_write=write;read=original_read;write=original_write;
  auto *key=reinterpret_cast<const uint8_t*>(ops);auto &hot=sheep_hot_slots[sheep_hot_index(key,initial.pc)];
  hot=SheepHotSlot{key,initial.pc,4,fn,sheep_hot_epoch};bool enabled=sheep_hot_enabled;sheep_hot_enabled=true;
  ppc_state=initial;exec_flags=0;unsigned consumed=777;
  if(!sheep_jit_hot<true>(key,4,consumed)||consumed!=count||
     ppc_state.pc!=initial.pc+(count?count-1:0)*4||
     std::memcmp(ppc_state.gpr,result.gpr,sizeof(result.gpr))||read!=native_read||write!=native_write)
   throw std::runtime_error("Accounted page-cache zero/partial/full CPU/RAM mismatch");
  hot=SheepHotSlot{};sheep_hot_enabled=enabled;read=original_read;write=original_write;
  ppc_state=initial;exec_flags=0;
  for(unsigned i=0;i<count;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);ppc_state.pc+=4;}
  if(actual!=count||result.pc!=ppc_state.pc||std::memcmp(result.gpr,ppc_state.gpr,sizeof(result.gpr))||result.cr!=ppc_state.cr||result.spr[SPR::XER]!=ppc_state.spr[SPR::XER]||result.spr[SPR::LR]!=ppc_state.spr[SPR::LR]||result.spr[SPR::CTR]!=ppc_state.spr[SPR::CTR]||read!=native_read||write!=native_write){
   fprintf(stderr,"RAM cache primary=%u variant=%u count=%u expected=%u pc=%08x/%08x\n",primary,variant,actual,count,result.pc,ppc_state.pc);throw std::runtime_error("RAM page cache interpreter mismatch");
  }
  passed++;
 }
 sheep_mmu_test_data_pair(nullptr,nullptr,0,0,0,false);ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;memory_bail=false;written=nullptr;reset_native_and_rom();
 fprintf(stderr,"RAM PAGE-CACHE SELFTEST %u same/cross-page/permission/offset/partial cases passed\n",passed);
}

static void memory_ea_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 std::array<uint8_t,4096> ram{},original{},native{};unsigned passed=0;const int displacements[]={-32768,-4096,-4095,-1,0,1,2,4095,4096,32767};
 for(unsigned primary=32;primary<=45;primary++)for(int displacement:displacements)for(unsigned zero_base=0;zero_base<2;zero_base++){
  if(zero_base&&(primary&1))continue;
  for(unsigned i=0;i<ram.size();i++)ram[i]=uint8_t(i*37+11);original=ram;
  uint32_t ea=zero_base?uint32_t(displacement):0x1234040;unsigned ra=zero_base?0:7;
  sheep_mmu_test_page(ram.data(),ea&0xfffff000u,1|32|64,false);
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[7]=ea-uint32_t(displacement);ppc_state.gpr[8]=0x89abcdef;exec_flags=0;
  unsigned size=primary>=40?2:(primary==34||primary==35||primary==38||primary==39)?1:4;
  unsigned count=((ea&4095)+size>4096)?3:4;
  uint32_t ops[]={0x38630001,0x69290001,0x396b0001,(primary<<26)|(8<<21)|(ra<<16)|uint16_t(displacement)};
  auto fn=nw_jit_compile_mapped(ops,4,ppc_state.pc,&native_layout);if(!fn)throw std::runtime_error("RAM EA/literal compile failed");
  SetPRS initial=ppc_state,result=ppc_state;memory_bail=false;written=nullptr;
  unsigned actual=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));native=ram;ram=original;ppc_state=initial;exec_flags=0;
  for(unsigned i=0;i<count;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);ppc_state.pc+=4;}
  if(actual!=count||result.pc!=ppc_state.pc||std::memcmp(result.gpr,ppc_state.gpr,sizeof(result.gpr))||result.cr!=ppc_state.cr||result.spr[SPR::XER]!=ppc_state.spr[SPR::XER]||ram!=native){fprintf(stderr,"RAM EA primary=%u disp=%d zero=%u count=%u/%u\n",primary,displacement,zero_base,actual,count);throw std::runtime_error("RAM EA/literal interpreter mismatch");}
  passed++;
 }
 sheep_mmu_test_page(nullptr,0,0,false);ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;memory_bail=false;written=nullptr;reset_native_and_rom();
 fprintf(stderr,"RAM EA SELFTEST %u signed/zero-base/wrap/literal cases passed\n",passed);
}

static void scalar_lowering_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;uint32_t rng=0x30c0ffee;
 auto random=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};unsigned adds=0,inserts=0,branches=0;
 auto trial=[&](const uint32_t *ops,unsigned n){
  ppc_state=saved;for(auto &r:ppc_state.gpr)r=random();ppc_state.cr=random();ppc_state.spr[SPR::XER]=random()&0xe000007f;ppc_state.pc=0x10000000;exec_flags=0;
  SetPRS initial=ppc_state,result=ppc_state;auto fn=nw_jit_compile_mapped(ops,n,ppc_state.pc,&native_layout);
  if(!fn||checked(fn,reinterpret_cast<nw_jit_cpu*>(&result))!=n)throw std::runtime_error("Scalar lowering compile/count failed");
  ppc_state=initial;exec_flags=0;
  for(unsigned i=0;i<n;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);ppc_state.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:ppc_state.pc+4;exec_flags=0;}
  if(std::memcmp(result.gpr,ppc_state.gpr,sizeof(result.gpr))||result.pc!=ppc_state.pc||result.cr!=ppc_state.cr||result.spr[SPR::XER]!=ppc_state.spr[SPR::XER]||result.spr[SPR::LR]!=ppc_state.spr[SPR::LR]||result.spr[SPR::CTR]!=ppc_state.spr[SPR::CTR]){fprintf(stderr,"Lowering mismatch opcode=%08x n=%u\n",ops[n-1],n);throw std::runtime_error("Scalar lowering interpreter mismatch");}
 };
 const int immediates[]={-32768,-4096,-4095,-256,-255,-1,0,1,255,256,4095,4096,32767};
 for(unsigned primary=14;primary<=15;primary++)for(unsigned zero=0;zero<2;zero++)for(unsigned alias=0;alias<2;alias++)for(int immediate:immediates)for(unsigned repeat=0;repeat<4;repeat++){
  uint32_t op=(primary<<26)|((alias?7u:8u)<<21)|((zero?0u:7u)<<16)|uint16_t(immediate);trial(&op,1);adds++;
 }
 for(unsigned low=0;low<32;low++)for(unsigned width=1;width<=32-low;width++)for(unsigned alias=0;alias<2;alias++)for(unsigned record=0;record<2;record++){
  unsigned mb=32-low-width,me=31-low;uint32_t op=(20u<<26)|((alias?7u:8u)<<21)|(7u<<16)|(low<<11)|(mb<<6)|(me<<1)|record;trial(&op,1);inserts++;
 }
 for(unsigned n=1;n<=16;n++)for(unsigned pattern=0;pattern<3;pattern++){
  uint32_t ops[16];for(unsigned i=0;i<n;i++)ops[i]=pattern==0||i%2==1?0x48000004u:(pattern==1?0x38630001u:0x38840002u);trial(ops,n);branches++;
 }
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;reset_native_and_rom();
 fprintf(stderr,"IMMEDIATE LOWERING SELFTEST %u signed/shift/zero/alias cases passed\nBFI LOWERING SELFTEST %u mask/alias/record cases passed\nFALLTHROUGH BRANCH SELFTEST %u count/control-flow cases passed\n",adds,inserts,branches);
}

static void bit_branch_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;unsigned passed=0;
 for(unsigned type=0;type<3;type++)for(unsigned bit=0;bit<32;bit++)for(unsigned bo:{4u,5u,12u,13u,20u})for(unsigned value=0;value<2;value++)for(unsigned alignment=0;alignment<4;alignment++)for(unsigned link=0;link<2;link++){
  if((type==0&&bo==20)||(link&&(type==0||bo!=20)))continue;
  ppc_state=saved;ppc_state.pc=0x68001000;ppc_state.cr=value?(1u<<(31-bit)):~(1u<<(31-bit));ppc_state.spr[SPR::LR]=0x68002344|alignment;ppc_state.spr[SPR::CTR]=0x68003458|alignment;exec_flags=0;
  uint32_t op=type==0?((16u<<26)|(bo<<21)|(bit<<16)|0x20u):((19u<<26)|(bo<<21)|(bit<<16)|((type==1?16u:528u)<<1)|link);
  SetPRS initial=ppc_state,result=ppc_state;auto fn=nw_jit_compile_mapped(&op,1,ppc_state.pc,&native_layout);
  if(!fn||checked(fn,reinterpret_cast<nw_jit_cpu*>(&result))!=1)throw std::runtime_error("Bit-branch compile/count failed");
  ppc_state=initial;ppc_main_opcode(ppc_opcode_grabber,op);ppc_state.pc=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:ppc_state.pc+4;
  if(result.pc!=ppc_state.pc||result.cr!=ppc_state.cr||result.spr[SPR::LR]!=ppc_state.spr[SPR::LR]||result.spr[SPR::CTR]!=ppc_state.spr[SPR::CTR]||std::memcmp(result.gpr,ppc_state.gpr,sizeof(result.gpr)))throw std::runtime_error("Bit-branch interpreter mismatch");passed++;
 }
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;reset_native_and_rom();fprintf(stderr,"BIT BRANCH SELFTEST %u condition/bit/target cases passed\n",passed);
}

extern void sheep_compact_dispatch_selftest();
static void rom_direct_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 const bool enabled=sheep_rom_direct_enabled,hot=sheep_hot_enabled;
 unsigned passed=0;sheep_rom_direct_enabled=true;sheep_hot_enabled=true;
 std::array<uint8_t,4096> ram{},original{},native{};
 for(unsigned n=1;n<=32;n++)for(unsigned variant=0;variant<5;variant++)for(unsigned seed=0;seed<4;seed++){
  sheep_rom_direct_reset();
  for(unsigned i=0;i<4096;i++)ram[i]=uint8_t(i*13+seed);original=ram;
  sheep_mmu_test_page(ram.data(),0x1234000,0x61,variant==3);
  uint32_t ops[32];for(unsigned i=0;i<n;i++)ops[i]=i&1?0x38840002u:0x38630001u;
  unsigned wanted=n;
  if(variant==1&&n>1){unsigned at=n/2;ops[at]=(16u<<26)|(4u<<21)|(8u<<16)|0x40u;wanted=at+1;}
  if(variant==2||variant==3){ops[n-1]=variant==2?0x91070000u:0x81070000u;if(variant==3)wanted=n-1;}
  if(variant==4)ops[n-1]=0x48000040u;
  ppc_state=saved;ppc_state.pc=seed==3?0xffffff80u:0x10000000u+seed*4096;ppc_state.cr=0;ppc_state.gpr[7]=0x1234010;ppc_state.gpr[8]=0x89abcdef;exec_flags=0;
  SetPRS initial=ppc_state,result=initial;memory_bail=false;written=nullptr;
  auto fn=nw_jit_compile_mapped(ops,n,initial.pc,&native_layout);if(!fn)throw std::runtime_error("ROM direct fixture compile failed");
  unsigned actual=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));native=ram;ram=original;
  if(actual!=wanted)throw std::runtime_error("ROM direct fixture baseline count failed");
  auto *code=reinterpret_cast<const uint8_t*>(sheep_rom_host_begin+((n*1024+variant*16+seed)*4));unsigned slot=sheep_jit_filter_slot_rom(code);
  uint64_t original_word=sheep_rom_dispatch_words[slot];
  if(sheep_rom_direct_publish(code,initial.pc,0,fn)||sheep_rom_direct_publish(code,initial.pc,33,fn)||sheep_rom_direct_publish(code,initial.pc,n,reinterpret_cast<nw_jit_fn>(reinterpret_cast<uintptr_t>(nw_jit_native_base())-4)))throw std::runtime_error("ROM direct accepted invalid descriptor");
  if(!sheep_rom_direct_publish(code,initial.pc,n,fn))throw std::runtime_error("ROM direct descriptor publish failed");
  uint64_t word=sheep_rom_dispatch_words[slot];unsigned count=777;
  if(sheep_jit_rom_direct(slot,word,n-1,count)||count!=777||ram!=original||std::memcmp(&ppc_state,&initial,sizeof(SetPRS)))throw std::runtime_error("ROM direct budget guard failed");
  ppc_state.pc+=4;if(sheep_jit_rom_direct(slot,word,n,count)||count!=777)throw std::runtime_error("ROM direct alias-PC guard failed");ppc_state=initial;
  if(!sheep_jit_rom_direct(slot,word,n,count)||count!=wanted||ram!=native||std::memcmp(ppc_state.gpr,result.gpr,sizeof(result.gpr))||ppc_state.pc!=(wanted?initial.pc+(wanted-1)*4:initial.pc))throw std::runtime_error("ROM direct native count/CPU/RAM/source-PC mismatch");
  uint32_t target=(exec_flags&EXEF_BRANCH)?ppc_next_instruction_address:ppc_state.pc+4;
  if(wanted&&target!=result.pc)throw std::runtime_error("ROM direct native target mismatch");
  sheep_rom_direct_restore(slot);if((sheep_rom_dispatch_words[slot]&~3ull)!=(original_word&~3ull))throw std::runtime_error("ROM direct opcode restore mismatch");
  if(!sheep_rom_direct_publish(code,initial.pc,n,fn))throw std::runtime_error("ROM direct republish failed");
  sheep_rom_reject_state_set(slot,2);if((sheep_rom_dispatch_words[slot]&~3ull)!=(original_word&~3ull))throw std::runtime_error("ROM direct rejection lost original opcode");
  sheep_rom_direct_publish(code,initial.pc,n,fn);sheep_rom_direct_reset();if(sheep_rom_dispatch_words[slot]!=(original_word&~3ull)||sheep_rom_direct_base)throw std::runtime_error("ROM direct generation reset mismatch");
  passed++;
 }
 sheep_rom_direct_reset();reset_native_and_rom();sheep_mmu_test_page(nullptr,0,0,false);ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;written=nullptr;memory_bail=false;sheep_rom_direct_enabled=enabled;sheep_hot_enabled=hot;sheep_hot_used=false;
 fprintf(stderr,"ROM DIRECT SELFTEST %u native/ABI/budget/alias/guard/branch/restore/reset cases passed\n",passed);
}

// A direct entry removed by short-prefix backoff must be recreated when the
// still-valid hot entry subsequently completes a useful prefix. Otherwise every
// reuse is forced out of the steady loop through the original C++ dispatcher.
static void hot_republish_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags,next=ppc_next_instruction_address;
 const bool enabled=sheep_rom_direct_enabled,hot_enabled=sheep_hot_enabled,rom_only=sheep_rom_only;
 sheep_rom_direct_enabled=sheep_hot_enabled=sheep_rom_only=true;unsigned passed=0;
 for(unsigned n:{4u,8u,16u,32u})for(unsigned seed=0;seed<8;seed++)for(unsigned variant=0;variant<5;variant++){
  sheep_rom_direct_reset();uint32_t ops[32];for(unsigned i=0;i<n;i++)ops[i]=i&1?0x38840002u:0x38630001u;
  if(variant==1)ops[0]=0x41820010; // early BEQ: first attempt retires only one
  ppc_state=saved;ppc_state.pc=0x10000000+seed*4096;ppc_state.gpr[3]=seed;ppc_state.gpr[4]=seed+9;ppc_state.cr=variant==1?0x20000000u:0;exec_flags=0;
  auto initial=ppc_state;auto fn=nw_jit_compile_mapped(ops,n,initial.pc,&native_layout);if(!fn)throw std::runtime_error("Republish fixture compile failed");
  auto *code=reinterpret_cast<const uint8_t*>(sheep_rom_host_begin+(n*512+seed*32+variant)*4);auto cell=sheep_jit_filter_slot_rom(code),index=sheep_hot_index(code,initial.pc);
  const auto word=sheep_rom_dispatch_words[cell];const auto counter=sheep_jit_reject_filter[cell];const auto old=sheep_hot_slots[index];
  sheep_hot_slots[index]={code,initial.pc,n,fn,sheep_hot_epoch};
  if(variant==2)sheep_rom_direct_enabled=false;
  if(variant==3)sheep_hot_slots[index].epoch++;
  if(variant==4){if(!sheep_rom_direct_publish(code,initial.pc+4,n,fn))throw std::runtime_error("Republish alias fixture publish failed");}
  unsigned count=777;auto result=initial;unsigned expected=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));ppc_state=initial;exec_flags=0;
  bool handled=sheep_jit_hot<true>(code,n,count);
  if(variant==3){
   if(handled||count!=777||std::memcmp(&ppc_state,&initial,sizeof(initial))||sheep_rom_dispatch_words[cell]!=word)throw std::runtime_error("Republish bypassed hot epoch guard");
  }else{
   if(!handled||count!=expected||ppc_state.pc!=initial.pc+(expected-1)*4)throw std::runtime_error("Republish changed retired/source accounting");
   auto target=exec_flags&EXEF_BRANCH?ppc_next_instruction_address:ppc_state.pc+4;auto actual=ppc_state;actual.pc=target;
   if(std::memcmp(&actual,&result,sizeof(actual)))throw std::runtime_error("Republish changed CPU or branch target");
   if(variant==1){
    if((sheep_rom_dispatch_words[cell]&3)!=3||sheep_jit_reject_filter[cell]!=counter)throw std::runtime_error("Successful short branch lost native entry or changed backoff");
    ppc_state=initial;ppc_state.cr=0;initial=ppc_state;result=initial;expected=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));ppc_state=initial;exec_flags=0;
    if(!sheep_jit_hot<true>(code,n,count)||count!=n)throw std::runtime_error("Republish retry hot block did not execute");
   }
   if(variant==2){if((sheep_rom_dispatch_words[cell]&3)!=0)throw std::runtime_error("Republish ignored disabled direct dispatcher");}
   else{
    uint64_t direct=sheep_rom_dispatch_words[cell];
    if((direct&3)!=3||uint32_t(direct>>32)!=initial.pc)throw std::runtime_error("Compiled hot block failed to republish direct entry");
    ppc_state=initial;exec_flags=0;unsigned again=777;
    if(!sheep_jit_rom_direct(cell,direct,n,again)||again!=expected||ppc_state.pc!=initial.pc+(expected-1)*4)throw std::runtime_error("Republished native entry differs from direct execution");
   }
  }
  sheep_rom_direct_enabled=true;sheep_rom_direct_restore(cell);sheep_rom_dispatch_words[cell]=word;sheep_jit_reject_filter[cell]=counter;sheep_hot_slots[index]=old;passed++;
 }
 sheep_rom_direct_reset();ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;sheep_rom_direct_enabled=enabled;sheep_hot_enabled=hot_enabled;sheep_rom_only=rom_only;
 fprintf(stderr,"HOT REPUBLISH SELFTEST %u CPU/count/alias/epoch/backoff/disable/reuse cases passed\n",passed);
}

// Success-result forwarding must not lose the state preceding a later fault.
static void cold_exit_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags,next=ppc_next_instruction_address;
 std::array<uint8_t,4096> ram{},original{},native_ram{};unsigned passed=0;
 constexpr uint32_t va=0x1234000;
 for(unsigned variant=0;variant<8;variant++)for(unsigned seed=0;seed<32;seed++){
  for(unsigned i=0;i<4096;i++)ram[i]=uint8_t(i*13+seed*7);original=ram;
  uint16_t permissions=variant==3?0x41:variant==4?0x60:0x61;
  sheep_mmu_test_page(ram.data(),va,permissions,false);
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[3]=seed;ppc_state.gpr[4]=seed+9;ppc_state.gpr[5]=0x12345678;ppc_state.gpr[6]=va;ppc_state.gpr[7]=va;
  if(variant==1)ppc_state.gpr[7]=0x76543000; // later tag miss must expose prior ALU results
  if(variant==2)ppc_state.gpr[7]=va+1;
  if(variant==5)ppc_state.gpr[6]=va+1;
  if(variant==6)ppc_state.gpr[7]=va+4095;
  uint32_t ops[]={0x80660000,0x38630005,0x54641838,0x80a70000,0x7ca51a14,0x90a60008,0x38a50001,0x48000040};
  if(variant==7){ops[0]=0xa8660000;ops[3]=0x88a70004;}
  const auto initial=ppc_state;auto result=initial;memory_bail=false;written=nullptr;
  auto fn=nw_jit_compile_mapped(ops,8,initial.pc,&native_layout);if(!fn)throw std::runtime_error("Cold-exit fixture compile failed");
  unsigned n=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));if(n>8)throw std::runtime_error("Cold-exit retired count invalid");
  native_ram=ram;ram=original;ppc_state=initial;exec_flags=0;
  for(unsigned i=0;i<n;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);ppc_state.pc=exec_flags&EXEF_BRANCH?ppc_next_instruction_address:ppc_state.pc+4;exec_flags=0;}
  if(std::memcmp(&result,&ppc_state,sizeof(result))||ram!=native_ram){fprintf(stderr,"COLD EXIT variant=%u seed=%u n=%u pc=%08x/%08x\n",variant,seed,n,result.pc,ppc_state.pc);throw std::runtime_error("Cold-exit forwarding/fault-prefix CPU or RAM mismatch");}
  if((variant==1||variant==6)&&n!=3)throw std::runtime_error("Cold-exit later fault prefix differs");
  if(variant==4&&n!=0)throw std::runtime_error("Cold-exit first fault prefix differs");
  if((variant==2||variant==5)&&n!=8)throw std::runtime_error("Cold-exit legal unaligned access did not complete");
  if(variant==3&&n!=5)throw std::runtime_error("Cold-exit store permission prefix differs");
  sheep_mmu_test_page(nullptr,0,0,false);passed++;
 }
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;written=nullptr;memory_bail=false;
 fprintf(stderr,"COLD EXIT SELFTEST %u load-ALU/store/late-fault/permission/alignment/ABI/count cases passed\n",passed);
}

extern void sheep_mmu_itlb_selftest();

extern bool sheep_encode_rom_handler(uint32_t,uint32_t&);
extern bool sheep_rom_handler_matches(uint32_t,uint64_t);
static void reset_rom_dispatch_status(){sheep_rom_direct_reset();for(auto &word:sheep_rom_dispatch_words)word&=~uint64_t(3);}
// Legal PPC603 unaligned scalar RAM operations must match canonical execution.
// Crossing-page, device, tag and first-write cases must still return the prefix.
static void unaligned_ram_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags,next=ppc_next_instruction_address;
 std::array<uint8_t,4096> ram{},original{},native{};unsigned passed=0;
 static const unsigned xo[]={23,55,87,119,151,183,215,247,279,311,343,375,407,439};
 for(unsigned indexed=0;indexed<2;indexed++)for(unsigned primary=32;primary<=45;primary++)
 for(unsigned offset:{0u,1u,2u,3u,4u,5u,7u,8u,15u,16u,31u,63u,127u,255u,1023u,2047u,4088u,4092u,4093u,4094u,4095u})
 for(unsigned variant=0;variant<5;variant++){
  for(unsigned i=0;i<4096;i++)ram[i]=uint8_t(i*37+primary*11+offset);original=ram;
  const unsigned size=primary>=40?2:(primary==34||primary==35||primary==38||primary==39)?1:4;
  const bool store=primary==36||primary==37||primary==38||primary==39||primary==44||primary==45;
  const uint32_t va=(offset&1)?0xfffff000u:0x1234000u,ea=va+offset;
  const uint16_t permissions=variant==1?0x62:variant==2?0x41:variant==3?0x21:0x61;
  const bool access=offset+size<=4096&&variant!=1&&variant!=4&&(!store||(variant!=2&&variant!=3));
  sheep_mmu_test_page(ram.data(),va,permissions,variant==4);
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[7]=ea-(indexed?0x120:0);ppc_state.gpr[6]=0x120;ppc_state.gpr[8]=0x89abcdef;
  const uint32_t memory=indexed?((31u<<26)|(8<<21)|(7<<16)|(6<<11)|(xo[primary-32]<<1)):((primary<<26)|(8<<21)|(7<<16));
  uint32_t ops[]={0x38630001,memory,0x69290001,0x38840002};
  const auto initial=ppc_state;auto result=initial;memory_bail=false;written=nullptr;
  auto fn=nw_jit_compile_mapped(ops,4,initial.pc,&native_layout);if(!fn)throw std::runtime_error("Unaligned RAM compile failed");
  const unsigned n=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));const bool bail=memory_bail;
  native=ram;ram=original;ppc_state=initial;exec_flags=0;
  const unsigned expected=access?4:1;
  for(unsigned i=0;i<expected;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);ppc_state.pc+=4;}
  if(n!=expected||bail==access||std::memcmp(&result,&ppc_state,sizeof(result))||ram!=native){
   fprintf(stderr,"UNALIGNED RAM primary=%u indexed=%u offset=%u variant=%u count=%u/%u bail=%u\n",primary,indexed,offset,variant,n,expected,bail);
   throw std::runtime_error("Unaligned RAM CPU/memory/count/guard/ABI mismatch");
  }
  sheep_mmu_test_page(nullptr,0,0,false);passed++;
 }
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;written=nullptr;memory_bail=false;
 reset_native_and_rom();fprintf(stderr,"UNALIGNED RAM SELFTEST %u load/store/indexed/update/sign/page/wrap/permissions/ABI cases passed\n",passed);
}

// Bad payload addresses make any access before the metadata guards fail loudly.
// Reading the mapped TLB structure itself is safe, including on every miss.
static void metadata_guard_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags,next=ppc_next_instruction_address;unsigned passed=0;
 const uint32_t va=0x1234000;static const unsigned xo[]={23,55,87,119,151,183,215,247,279,311,343,375,407,439};
 for(unsigned indexed=0;indexed<2;indexed++)for(unsigned pair=0;pair<2;pair++)for(unsigned primary=32;primary<=45;primary++)
 for(unsigned variant=0;variant<4;variant++)for(unsigned seed=0;seed<8;seed++){
  const bool store=primary==36||primary==37||primary==38||primary==39||primary==44||primary==45;
  if(!store&&variant>=2)continue;
  const uint16_t permissions=variant==1?0x62:variant==2?0x41:variant==3?0x21:0x61;
  sheep_mmu_test_page(reinterpret_cast<uint8_t*>(uintptr_t(0x10)),va,permissions,variant==0);
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[3]=seed;ppc_state.gpr[7]=va-(indexed?0x120:0);ppc_state.gpr[6]=0x120;ppc_state.gpr[8]=0x89abcdef;
  const uint32_t memory=indexed?((31u<<26)|(8<<21)|(7<<16)|(6<<11)|(xo[primary-32]<<1)):((primary<<26)|(8<<21)|(7<<16));
  uint32_t ops[]={0x38630001,memory,pair?0x81270000u:0x69290001u,0x38840002};
  const auto initial=ppc_state;auto result=initial,expected=initial;expected.gpr[3]++;expected.pc+=4;memory_bail=false;written=nullptr;
  auto fn=nw_jit_compile_mapped(ops,4,initial.pc,&native_layout);if(!fn)throw std::runtime_error("Metadata guard compile failed");
  const unsigned n=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));
  if(n!=1||!memory_bail||written||std::memcmp(&result,&expected,sizeof(result)))throw std::runtime_error("Metadata guard touched invalid payload or lost precise prefix state");
  sheep_mmu_test_page(nullptr,0,0,false);passed++;
 }
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;written=nullptr;memory_bail=false;reset_native_and_rom();
 fprintf(stderr,"METADATA GUARD SELFTEST %u invalid-payload/tag/device/permission/PTE/paired/ABI cases passed\n",passed);
}

static void prepare_rom_dispatch(){
 sheep_rom_decode_ready=false;
 uintptr_t begin=sheep_rom_host_begin,end=sheep_rom_host_end;
 if(!sheep_itlb_primary_enabled||end<=begin||end-begin>4194304||((begin|end)&3))return;
 for(uintptr_t address=begin;address<end;address+=4){
  auto *code=reinterpret_cast<const uint8_t*>(address);
  uint32_t opcode=read_be(code,4),description;
  if(!sheep_encode_rom_handler(opcode,description))return;
  sheep_rom_dispatch_words[sheep_jit_filter_slot_rom(code)]=(uint64_t(opcode)<<32)|description;
 }
 sheep_rom_decode_ready=true;
}
extern void sheep_mmu_test_fast_page(uint8_t*,uint8_t*,uint32_t,uint16_t,bool,unsigned);
extern void sheep_mmu_test_fast_transition(unsigned);
static void hardware_fastmem_selftest(){
 if(!pippin_authentic_selftest())throw std::runtime_error("Authentic-speed CD model selftest failed");
 if(!pippin_fastmem_host_supported()){fprintf(stderr,"PIPPIN FASTMEM selftests skipped (host page size is not 4 KB)\n");return;}
 if(!pippin_fastmem_enabled())throw std::runtime_error("Requested hardware fastmem is unavailable");
 const auto saved=ppc_state;const auto flags=exec_flags,next=ppc_next_instruction_address;
 auto *read=pippin_fastmem_alloc(4096),*write=pippin_fastmem_alloc(4096);if(!read||!write)throw std::runtime_error("Fastmem shared fixture allocation failed");
 std::array<uint8_t,4096> original_read{},original_write{},native_read{},native_write{};unsigned passed=0,hits=0,misses=0;
 static const unsigned xo[]={23,55,87,119,151,183,215,247,279,311,343,375,407,439};
 for(unsigned indexed=0;indexed<2;indexed++)for(unsigned primary=32;primary<=45;primary++)
 for(unsigned offset:{0u,1u,2u,3u,7u,4092u,4093u,4094u,4095u})for(unsigned variant=0;variant<5;variant++){
  const bool store=primary==36||primary==37||primary==38||primary==39||primary==44||primary==45;
  const unsigned size=primary>=40?2:(primary==34||primary==35||primary==38||primary==39)?1:4;
  uint32_t memory=indexed?((31u<<26)|(8u<<21)|(7u<<16)|(6u<<11)|(xo[primary-32]<<1)):((primary<<26)|(8u<<21)|(7u<<16));
  // Rewriting R3 after the memory op exposes incorrectly elided prefix stores
  // if a hardware fault is omitted from the CPU-forwarding barriers.
  uint32_t ops[]={0x38630001,memory,0x38630002,0x38840005};
  auto fn=nw_jit_compile_mapped(ops,4,0x10000000,&native_layout);if(!fn||!nw_jit_last_fastmem_ops())throw std::runtime_error("Fastmem fixture did not use hardware path");
  for(unsigned mode=0;mode<3;mode++){
   for(unsigned i=0;i<4096;i++){read[i]=uint8_t(i*37+mode*53+primary);write[i]=uint8_t(i*13+mode*29+7);}std::copy_n(read,4096,original_read.begin());std::copy_n(write,4096,original_write.begin());
   const uint32_t va=mode==2?0xfffff000u:mode==1?0x68066000u:0x1234000u;
   const uint16_t permission=variant==1?0x41:variant==2?0x21:variant==3?0x62:0x61;
   sheep_mmu_test_fast_page(read,write,va,permission,variant==4,mode);
   ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[3]=mode+17;ppc_state.gpr[7]=va+offset-(indexed?0x120:0);ppc_state.gpr[6]=0x120;ppc_state.gpr[8]=0x89abcdef^mode;
   const auto initial=ppc_state;auto result=initial;memory_bail=false;written=nullptr;auto before=pippin_fastmem_fault_count();
   const unsigned n=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));bool bail=memory_bail;
   bool access=offset+size<=4096&&variant!=3&&variant!=4&&(!store||(variant!=1&&variant!=2));
   // Odd halfwords now reach the same software checks without an alias fault.
   // Retirement, permissions, CPU state and guest bytes keep the old oracle.
   unsigned wanted=access?4:1;bool should_fault=!access&&offset+size<=4096&&!(size==2&&(offset&1));
   if(n!=wanted||bail==access||pippin_fastmem_fault_count()!=before+unsigned(should_fault))throw std::runtime_error("Fastmem hardware-hit/fault/count/permission mismatch");
   std::copy_n(read,4096,native_read.begin());std::copy_n(write,4096,native_write.begin());std::copy(original_read.begin(),original_read.end(),read);std::copy(original_write.begin(),original_write.end(),write);
   ppc_state=initial;exec_flags=0;for(unsigned i=0;i<wanted;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);ppc_state.pc+=4;}
   if(std::memcmp(&result,&ppc_state,sizeof(result))||!std::equal(native_read.begin(),native_read.end(),read)||!std::equal(native_write.begin(),native_write.end(),write))throw std::runtime_error("Fastmem full CPU/read-write alias/prefix replay mismatch");
   sheep_mmu_test_fast_page(nullptr,nullptr,0,0,false,0);passed++;hits+=access;misses+=should_fault;
  }
 }
 // Exercise real promotion and tracked-invalidation hooks, not just the mapper.
 for(unsigned mode=0;mode<3;mode++)for(unsigned transition=0;transition<4;transition++){
  const uint32_t va=mode==2?0xfffff000u:0x1234000u;
  sheep_mmu_test_fast_page(read,write,va,0x61,false,mode);
  uint32_t ops[]={0x38630001,(32u<<26)|(8u<<21)|(7u<<16),0x38630002,0x38840005};
  auto fn=nw_jit_compile_mapped(ops,4,0x10000000,&native_layout);if(!fn||!nw_jit_last_fastmem_ops())throw std::runtime_error("Transition fastmem fixture compilation failed");
  if(transition>=2)ops[1]=(36u<<26)|(8u<<21)|(7u<<16);
  if(transition>=2){fn=nw_jit_compile_mapped(ops,4,0x10000000,&native_layout);if(!fn)throw std::runtime_error("Write transition compilation failed");}
  if(transition==3)sheep_mmu_test_fast_transition(2);
  sheep_mmu_test_fast_transition(transition);
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[3]=17;ppc_state.gpr[7]=va;ppc_state.gpr[8]=0x12345678;auto initial=ppc_state,result=initial;
  memory_bail=false;written=nullptr;auto before=pippin_fastmem_fault_count();unsigned wanted=transition==3?4:1;
  unsigned n=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));
  if(n!=wanted||pippin_fastmem_fault_count()!=before+unsigned(wanted==1)||result.gpr[3]!=initial.gpr[3]+(wanted==4?3:1))throw std::runtime_error("Fastmem eviction/invalidation/permission update retained stale alias or lost prefix");
  sheep_mmu_test_fast_page(nullptr,nullptr,0,0,false,0);passed++;
 }
 // A secondary-owned mapping must survive primary eviction, then disappear
 // precisely when its last cached owner is replaced or context-invalidated.
 for(unsigned mode=0;mode<3;mode++)for(unsigned transition:{5u,6u}){
  const uint32_t va=mode==2?0xfffff000u:0x1234000u;sheep_mmu_test_fast_page(read,write,va,0x61,false,mode);
  sheep_mmu_test_fast_transition(4);sheep_mmu_test_fast_transition(0);
  uint32_t ops[]={0x38630001,(32u<<26)|(8u<<21)|(7u<<16),0x38630002,0x38840005};
  auto fn=nw_jit_compile_mapped(ops,4,0x10000000,&native_layout);if(!fn||!nw_jit_last_fastmem_ops())throw std::runtime_error("Secondary retention compilation failed");
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[3]=17;ppc_state.gpr[7]=va;auto initial=ppc_state,result=initial,expected=initial;
  expected.gpr[3]+=3;expected.gpr[4]+=5;expected.gpr[8]=read_be(read,4);expected.pc+=16;memory_bail=false;written=nullptr;auto before=pippin_fastmem_fault_count();
  unsigned n=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));if(n!=4||memory_bail||pippin_fastmem_fault_count()!=before||std::memcmp(&result,&expected,sizeof(result)))throw std::runtime_error("Primary eviction revoked a valid secondary mapping");
  sheep_mmu_test_fast_transition(transition);result=initial;expected=initial;expected.gpr[3]++;expected.pc+=4;memory_bail=false;written=nullptr;before=pippin_fastmem_fault_count();
  n=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));if(n!=1||!memory_bail||pippin_fastmem_fault_count()!=before+1||std::memcmp(&result,&expected,sizeof(result)))throw std::runtime_error("Last secondary owner retained stale mapping");
  sheep_mmu_test_fast_page(nullptr,nullptr,0,0,false,0);passed++;
 }
 written=nullptr;memory_bail=false;ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;reset_native_and_rom();
 if(!pippin_fastmem_release(read)||!pippin_fastmem_release(write))throw std::runtime_error("Fastmem fixture release failed");
 fprintf(stderr,"HARDWARE FASTMEM SELFTEST %u full CPU/RAM/alias/mode/wrap/unaligned/fault/materialization/ABI cases passed; %u successful accesses, %u guarded faults\n",passed,hits,misses);
}

static void guard_forwarding_selftest(){
 if(!pippin_fastmem_host_supported())return;
 const auto saved=ppc_state;const auto flags=exec_flags,next=ppc_next_instruction_address;
 auto *read=pippin_fastmem_alloc(4096),*write=pippin_fastmem_alloc(4096);if(!read||!write)throw std::runtime_error("Guard forwarding allocation failed");
 unsigned passed=0;
 for(unsigned seed=0;seed<32;seed++)for(unsigned mode=0;mode<3;mode++)for(unsigned fail=0;fail<3;fail++){
  const uint32_t va=mode==2?0xfffff000u:0x1234000u;
  for(unsigned i=0;i<4096;i++)read[i]=uint8_t(i*17+seed);std::memset(write,0x65,4096);
  sheep_mmu_test_fast_page(read,write,va,0x61,fail==1,mode);
  uint32_t ops[]={0x38630001,0x38840002,(32u<<26)|(8u<<21)|(7u<<16),0x38630003,0x38840004,0x38a30005,0x38c40006,0x38e70007};
  auto fn=nw_jit_compile_mapped(ops,8,0x10000000,&native_layout);if(!fn)throw std::runtime_error("Guard forwarding compilation failed");
  ppc_state=saved;ppc_state.pc=0x10000000;ppc_state.gpr[3]=seed;ppc_state.gpr[4]=seed*13;ppc_state.gpr[7]=va+(fail==2?4095:seed%4);
  const auto initial=ppc_state;auto result=initial;memory_bail=false;written=nullptr;unsigned count=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));
  const unsigned want=fail?2:8;if(count!=want||memory_bail!=bool(fail))throw std::runtime_error("Guard forwarding prefix count differs");
  exec_flags=0;for(unsigned i=0;i<want;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);ppc_state.pc+=4;}
  if(std::memcmp(&result,&ppc_state,sizeof(result)))throw std::runtime_error("Guard forwarding CPU differs at fallthrough/fault boundary");
  sheep_mmu_test_fast_page(nullptr,nullptr,0,0,false,0);passed++;
 }
 if(!pippin_fastmem_release(read)||!pippin_fastmem_release(write))throw std::runtime_error("Guard forwarding release failed");
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;written=nullptr;memory_bail=false;reset_native_and_rom();
 fprintf(stderr,"GUARD FORWARDING SELFTEST %u full CPU/read-cache/fallthrough/precise-prefix cases passed\n",passed);
}

static void fused_rom_selftest(){
 if(!sheep_rom_decode_ready)throw std::runtime_error("Fused ROM decoder unavailable");
 unsigned checked=0;
 for(uintptr_t address=sheep_rom_host_begin;address<sheep_rom_host_end;address+=4){
  auto *code=reinterpret_cast<const uint8_t*>(address);uint64_t word=sheep_rom_dispatch_words[sheep_jit_filter_slot_rom(code)];
  if(uint32_t(word>>32)!=read_be(code,4)||(word&3))throw std::runtime_error("Fused ROM opcode/state differs from immutable source");
  if(!sheep_rom_handler_matches(read_be(code,4),word))throw std::runtime_error("Cached handler differs from canonical FPU/disabled tables");
  checked++;
 }
 fprintf(stderr,"FUSED ROM SELFTEST %u decoded words preserved; %u handler identities passed\n",checked,checked*2);
}

static void packed_reject_selftest(){
 const bool saved=sheep_rom_only;unsigned passed=0;
 auto key=[](unsigned slot){return reinterpret_cast<const uint8_t*>(uintptr_t(slot)*4);};
 for(unsigned index=0;index<128;index++)for(unsigned value=0;value<256;value++){
  unsigned slot=(index*8191)&1048575,left=(slot-1)&1048575,right=(slot+1)&1048575;
  sheep_rom_only=true;sheep_reject_set(key(left),0,17);sheep_reject_set(key(right),0,255);sheep_reject_set(key(slot),0,value);
  uint64_t opcode=sheep_rom_dispatch_words[slot]&~uint64_t(3),left_opcode=sheep_rom_dispatch_words[left]&~uint64_t(3),right_opcode=sheep_rom_dispatch_words[right]&~uint64_t(3);
  unsigned reference=value,steps=value==255?4:value+2;
  for(unsigned tick=0;tick<steps;tick++){
   bool blocked=reference!=0;if(reference&&reference!=255)--reference;
   if(sheep_rom_dispatch_blocked(slot)!=blocked||sheep_jit_reject_filter[slot]!=reference)
    throw std::runtime_error("Packed ROM rejection differs from byte-counter semantics");
   if((sheep_rom_dispatch_words[slot]&~uint64_t(3))!=opcode||(sheep_rom_dispatch_words[left]&~uint64_t(3))!=left_opcode||(sheep_rom_dispatch_words[right]&~uint64_t(3))!=right_opcode)throw std::runtime_error("Status update changed cached opcode");
   unsigned expected=reference==255?1:reference?2:0;
   if(sheep_rom_reject_state(slot)!=expected||sheep_rom_reject_state(left)!=2||sheep_rom_reject_state(right)!=1||sheep_jit_reject_filter[left]!=17||sheep_jit_reject_filter[right]!=255)
    throw std::runtime_error("Packed rejection state/neighbor corruption");
  }
  sheep_rom_only=false;unsigned general=sheep_jit_filter_slot_general(key(slot),0x68000000);unsigned cell=general&1048575;
  uint64_t untouched=sheep_rom_dispatch_words[cell];sheep_reject_set(key(slot),0x68000000,value);
  if(sheep_jit_reject_filter[general]!=value||sheep_rom_dispatch_words[cell]!=untouched)
   throw std::runtime_error("General rejection profile touched ROM packed state");
  passed++;
 }
 sheep_rom_only=saved;std::memset(sheep_jit_reject_filter,0,sizeof(sheep_jit_reject_filter));reset_rom_dispatch_status();
 fprintf(stderr,"FUSED REJECT SELFTEST %u counter/state/neighbor/profile cases passed\n",passed);
}

static void rom_reject_selftest(){
 bool saved=sheep_rom_only;std::array<uint32_t,1024> code{};unsigned passed=0;
 for(unsigned i=0;i<code.size();i++){
  auto *p=reinterpret_cast<const uint8_t*>(&code[i]);sheep_rom_only=true;
  unsigned slot=sheep_jit_filter_slot_rom(p);
  if(slot>=1048576||sheep_jit_filter_slot(p,0x68000000+i*4)!=slot||sheep_jit_filter_slot(p,0xffc00000+i*4)!=slot)throw std::runtime_error("ROM reject alias/index mismatch");
  passed++;sheep_rom_only=false;
  if(sheep_jit_filter_slot(p,0x68000000+i*4)!=sheep_jit_filter_slot_general(p,0x68000000+i*4))throw std::runtime_error("Writable-code reject profile mismatch");passed++;
 }
 sheep_rom_only=saved;fprintf(stderr,"ROM REJECT SELFTEST %u index/alias/profile cases passed\n",passed);
}

static void rom_page_selftest(){
 const bool saved=sheep_rom_only;const uintptr_t begin=sheep_rom_host_begin,end=sheep_rom_host_end;
 uint8_t fixture[16];sheep_rom_host_begin=reinterpret_cast<uintptr_t>(fixture);sheep_rom_host_end=sheep_rom_host_begin+sizeof(fixture);sheep_rom_only=true;
 if(!sheep_jit_code_page_enabled(fixture,0)||!sheep_jit_code_page_enabled(fixture+15,0)||sheep_jit_code_page_enabled(fixture+16,0)||sheep_jit_code_page_enabled(reinterpret_cast<const uint8_t*>(sheep_rom_host_begin-1),0))throw std::runtime_error("ROM host-range bounds failed");
 sheep_rom_only=false;if(!sheep_jit_code_page_enabled(nullptr,0))throw std::runtime_error("Unrestricted native page rejected");
 sheep_rom_only=saved;sheep_rom_host_begin=begin;sheep_rom_host_end=end;
 fprintf(stderr,"ROM PAGE SELFTEST base/end/outside/unrestricted cases passed\n");
}

static void warmup_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 for(auto&e:cache)e=Entry{};
 std::array<uint8_t,16> code;for(unsigned i=0;i<4;i++){code[i*4]=0x38;code[i*4+1]=0x21;code[i*4+2]=0;code[i*4+3]=1;}
 ppc_state.pc=0x10000000;ppc_state.gpr[1]=10;exec_flags=0;
 for(unsigned i=0;i<4;i++)if(sheep_jit_try(code.data(),4)||ppc_state.gpr[1]!=10||ppc_state.pc!=0x10000000)throw std::runtime_error("Cold trace executed before warm-up");
 if(sheep_jit_try(code.data(),4)!=4||ppc_state.gpr[1]!=14)throw std::runtime_error("Warm trace did not compile/execute correctly");
 code[15]=2;ppc_state.gpr[1]=10;
 if(sheep_jit_try(code.data(),4)||ppc_state.gpr[1]!=10)throw std::runtime_error("Changed trace reused stale warm/native code");
 if(sheep_jit_try(code.data(),3)||ppc_state.gpr[1]!=10)throw std::runtime_error("Cold trace ignored timer budget");
 for(auto&e:cache)e=Entry{};reset_native_and_rom();ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;
 fprintf(stderr,"COLD-TRACE SELFTEST warm-up/source-change/budget cases passed\n");
}

extern void sheep_cpu_loop_selftest();
extern void sheep_cpu_loop_cross_selftest();
extern void sheep_cpu_loop_ram_selftest();
extern void sheep_branch_dispatch_selftest();
extern void sheep_mmu_split_read_selftest();
// SPDX-License-Identifier: GPL-3.0-or-later
// Include after the other bridge selftests, before sheep_jit_enabled().
// Planned traces stay >=4 instructions: only their *retired* prefix is short.
static void short_success_policy_selftest(){
 const auto saved=ppc_state,saved_shadow=shadow;
 const auto saved_flags=exec_flags,saved_next=ppc_next_instruction_address;
 const bool saved_direct=sheep_rom_direct_enabled,saved_hot=sheep_hot_enabled;
 const bool saved_only=sheep_rom_only,saved_bail=memory_bail,saved_used=sheep_hot_used;
 const int saved_mode=mode;
 const auto saved_hits=memory_hits,saved_misses=memory_misses;
 auto *const saved_written=written;
 const auto saved_written_size=written_size,saved_written_old=written_old,saved_written_new=written_new;
 if(!sheep_rom_decode_ready||sheep_rom_host_end-sheep_rom_host_begin<128)
  throw std::runtime_error("Short-success fixture requires prepared immutable ROM");
 sheep_rom_direct_reset();
 const auto *code=reinterpret_cast<const uint8_t*>(sheep_rom_host_begin+64);
 const unsigned cell=sheep_jit_filter_slot_rom(code),hot_index=sheep_hot_index(code,0);
 const auto saved_word=sheep_rom_dispatch_words[cell];
 const auto saved_counter=sheep_jit_reject_filter[cell];
 const auto saved_slot=sheep_hot_slots[hot_index];
 // Use the real guarded RAM path, with external hardware fastmem and tracing
 // disabled, so both the native and interpreter oracles use the fixture page.
 auto layout=native_layout;layout.leaf=1;
 layout.fast_read_slot=layout.fast_write_slot=0;
 layout.trace_host=layout.trace_old=layout.trace_size=0;
 constexpr uint32_t va=0x1234000,sentinel=0x31415920;
 alignas(4096) std::array<uint8_t,4096> ram{},original{},native_ram{};
 std::array<uint8_t,sheep_native_max_ops*4> source{};
 unsigned fixtures=0,api_runs=0,backoff_ticks=0;
 sheep_rom_direct_enabled=sheep_hot_enabled=sheep_rom_only=true;
 mode=2; // Exercise the production slow API too, without recursive verify init.

 auto input=[&](unsigned seed){
  SetPRS cpu=saved;
  cpu.pc=seed==0?0x10000000u:seed==1?0x10000ff8u:0xfffffff8u;
  cpu.spr[SPR::PVR]=PPC_VER::MPC603;
  cpu.msr&=~MSR::PR;
  for(unsigned r=0;r<32;r++)cpu.gpr[r]=(0x913579bdu+seed*0x17231u)^(r*0x1020301u);
  cpu.gpr[3]=seed==1?0xffffffffu:seed;
  cpu.gpr[6]=va;cpu.gpr[7]=va;cpu.gpr[8]=0x89abcdefu^seed;
  cpu.cr=0x20000000u;cpu.spr[SPR::XER]=0xa0000000u;
  cpu.spr[SPR::LR]=(cpu.pc+0x181u)|3u;
  cpu.spr[SPR::CTR]=(cpu.pc+0x2a1u)|3u;
  return cpu;
 };
 auto spr_op=[](unsigned spr,unsigned xo,unsigned reg){
  return (31u<<26)|(reg<<21)|((spr&31)<<16)|((spr&0x3e0)<<6)|(xo<<1);
 };
 auto check=[&](bool okay,const char *message,unsigned api,unsigned round,unsigned count){
  if(okay)return;
  fprintf(stderr,"SHORT SUCCESS fixture=%u api=%u round=%u count=%u pc=%08x flags=%x bail=%u state=%u reject=%u\n",
   fixtures,api,round,count,ppc_state.pc,exec_flags,memory_bail,
   sheep_rom_reject_state(cell),sheep_jit_reject_filter[cell]);
  throw std::runtime_error(message);
 };

 auto trial=[&](const uint32_t *ops,unsigned planned,const SetPRS &initial,unsigned wanted,bool fault){
  sheep_rom_direct_reset();
  sheep_rom_dispatch_words[cell]=saved_word&~uint64_t(3);
  sheep_jit_reject_filter[cell]=0;
  for(unsigned i=0;i<ram.size();i++)ram[i]=uint8_t(i*13+fixtures*7);
  original=ram;
  for(unsigned i=0;i<planned;i++)write_be(source.data()+i*4,4,ops[i]);
  auto fn=nw_jit_compile_mapped(ops,planned,initial.pc,&layout);
  check(fn!=nullptr,"Short-success fixture compilation failed",99,0,0);
  auto result=initial;memory_bail=false;written=nullptr;
  unsigned raw=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));
  const bool bailed=memory_bail;native_ram=ram;
  check(raw==wanted&&bailed==fault,"Short-success raw count/bail differs",99,0,raw);

  // Replay exactly the retired prefix. The faulting memory/privileged opcode
  // belongs to the interpreter's next turn and must have no effects here.
  ppc_state=initial;ram=original;exec_flags=0;ppc_next_instruction_address=sentinel;
  for(unsigned i=0;i<wanted;i++){
   ppc_main_opcode(ppc_opcode_grabber,ops[i]);
   check(!(exec_flags&~EXEF_BRANCH),"Short-success reference prefix trapped",99,0,i);
   const bool branch=exec_flags&EXEF_BRANCH;
   ppc_state.pc=branch?ppc_next_instruction_address:ppc_state.pc+4;
   exec_flags=0;
   check(!branch||i+1==wanted,"Short-success reference passed a taken branch",99,0,i);
  }
  check(!std::memcmp(&ppc_state,&result,sizeof(SetPRS))&&ram==native_ram,
   "Short-success full CPU/RAM/interpreter oracle differs",99,0,raw);

  // 0=ordinary hot API, 1=last-source-PC hot API, 2=direct descriptor API,
  // 3=production slow API with a seeded, byte-validated compiled cache hit.
  // The latter uses actual matching synthetic source bytes outside ROM; it
  // tests the post-native rejection policy without changing trace admission.
  for(unsigned api=0;api<4;api++){
   sheep_rom_direct_reset();
   sheep_rom_dispatch_words[cell]=saved_word&~uint64_t(3);
   sheep_jit_reject_filter[cell]=0;
   sheep_rom_only=api!=3;
   sheep_hot_slots[hot_index]={code,initial.pc,planned,fn,sheep_hot_epoch};
   const unsigned cache_index=((uintptr_t(source.data())>>2)^((uint64_t(initial.pc)*0x9e3779b1u)>>16))&(cache.size()-1);
   const auto old_entry=cache[cache_index];
   const unsigned general=sheep_jit_filter_slot_general(source.data(),initial.pc);
   const auto old_general=sheep_jit_reject_filter[general];
   if(api==3){
    auto &entry=cache[cache_index];entry=Entry{};
    entry.host=source.data();entry.pc=initial.pc;entry.n=planned;entry.warmup=4;entry.fn=fn;
    std::memcpy(entry.bytes.data(),source.data(),planned*4);
    sheep_jit_reject_filter[general]=0;
   }
   if(api==2)check(sheep_rom_direct_publish(code,initial.pc,planned,fn),
    "Short-success direct publish failed",api,0,0);
   if(api==1)check(sheep_rom_direct_publish(code,initial.pc+4,planned,fn),
    "Short-success alias descriptor fixture failed",api,0,0);

   auto invoke=[&](unsigned budget,unsigned &count){
    if(api==0)return sheep_jit_hot<false>(code,budget,count);
    if(api==1)return sheep_jit_hot<true>(code,budget,count);
    if(api==2)return sheep_jit_rom_direct(cell,sheep_rom_dispatch_words[cell],budget,count);
    unsigned consumed=sheep_jit_try(source.data(),budget);
    if(consumed)count=consumed;
    return consumed!=0;
   };
   // A 1--3 instruction success still requires the entire >=4-op *planned*
   // budget. Do not admit it just because this input would exit early.
   ppc_state=initial;ram=original;exec_flags=0;ppc_next_instruction_address=sentinel;
   memory_bail=true;sheep_hot_used=false;unsigned count=777;
   const auto before_word=sheep_rom_dispatch_words[cell];
   check(!invoke(planned-1,count)&&count==777&&memory_bail&&!sheep_hot_used&&
    !std::memcmp(&ppc_state,&initial,sizeof(SetPRS))&&ram==original&&
    !exec_flags&&ppc_next_instruction_address==sentinel&&sheep_rom_dispatch_words[cell]==before_word,
    "Short-success execution bypassed planned-budget guard",api,0,count);

   for(unsigned round=0;round<3;round++){
    // First execute a missing/aliased entry, then retain/reuse it. Finally
    // force descriptor removal and require the same successful short hot
    // slot to republish it immediately, without waiting for a long prefix.
    if(round==2&&api<2&&!fault)sheep_rom_direct_restore(cell);
    if(api==2&&(sheep_rom_dispatch_words[cell]&3)!=3)
     check(sheep_rom_direct_publish(code,initial.pc,planned,fn),
      "Short-success direct retry publish failed",api,round,0);
    ppc_state=initial;ram=original;exec_flags=0;ppc_next_instruction_address=sentinel;
    memory_bail=true;written=nullptr;sheep_hot_used=false;count=777;
    const bool handled=invoke(planned,count);
    // The slow API returns 0 for an executed zero-length fault. Hot/direct
    // APIs report a handled descriptor and explicitly return consumed=0.
    if(api==3&&!wanted)count=0;
    check(handled==(api!=3||wanted!=0)&&count==wanted&&memory_bail==fault,
     "Short-success API count/handled/bail differs",api,round,count);
    auto actual=ppc_state;
    const uint32_t source_pc=(api==1||api==2)&&wanted?initial.pc+(wanted-1)*4:initial.pc;
    check(actual.pc==source_pc,"Short-success API last-source-PC accounting differs",api,round,count);
    actual.pc=result.pc;
    check(!std::memcmp(&actual,&result,sizeof(SetPRS))&&ram==native_ram,
     "Short-success API full CPU/RAM prefix differs",api,round,count);
    const bool branch=wanted&&(result.pc!=uint32_t(initial.pc+wanted*4)||
     (result.pc&~4095u)!=(initial.pc&~4095u));
    check(exec_flags==(branch?EXEF_BRANCH:0)&&ppc_next_instruction_address==(branch?result.pc:sentinel),
     "Short-success API branch/next-PC/page/wrap boundary differs",api,round,count);
    if(api<3)check(sheep_hot_used==(wanted!=0),"Short-success hot-use accounting differs",api,round,count);

    const bool reject=!wanted||(wanted<4&&fault);
    const unsigned filter=api==3?general:cell;
    check(sheep_jit_reject_filter[filter]==(reject?32:0),
     "Short-success API rejection policy differs",api,round,count);
    if(api<3){
     if(reject){
      check(sheep_rom_dispatch_words[cell]==((saved_word&~uint64_t(3))|2),
       "Short-success guard backoff failed to restore original opcode",api,round,count);
      for(unsigned tick=0;tick<32;tick++){
       check(sheep_rom_reject_retry(cell)&&sheep_jit_reject_filter[cell]==31-tick&&
        sheep_rom_dispatch_words[cell]==((saved_word&~uint64_t(3))|(tick==31?0:2)),
        "Short-success guard backoff did not last exactly 32 retries",api,round,count);
       backoff_ticks++;
      }
      check(!sheep_rom_reject_retry(cell),"Short-success guard backoff did not expire",api,round,count);
     }else{
      const auto word=sheep_rom_dispatch_words[cell];
      check((word&3)==3&&uint32_t(word>>32)==initial.pc&&(((uint32_t(word)>>2)&31)+1)==planned,
       "Short-success did not retain/republish planned direct descriptor",api,round,count);
      const auto address=sheep_rom_direct_base+(uintptr_t(uint32_t(word)>>7)<<2);
      check(address==reinterpret_cast<uintptr_t>(fn),"Short-success republished wrong native function",api,round,count);
     }
    }else if(reject){
     // The general filter has no packed ROM retry helper. Clear it between
     // forced cache hits, after verifying each independent fault resets 32.
     sheep_jit_reject_filter[general]=0;
    }
    api_runs++;
   }
   if(api==3){cache[cache_index]=old_entry;sheep_jit_reject_filter[general]=old_general;}
  }
  sheep_rom_only=true;
  sheep_mmu_test_page(nullptr,0,0,false);
  fixtures++;
 };

 // Each taken condition retires exactly 1, 2 or 3; its untaken control must
 // complete the original planned trace. Pre-branch stores and post-branch
 // poison stores verify both committed RAM and absence of speculative effects.
 for(unsigned planned:{4u,8u,32u})for(unsigned seed=0;seed<3;seed++)
 for(unsigned wanted=1;wanted<=3;wanted++)for(unsigned kind=0;kind<7;kind++)for(unsigned taken=0;taken<2;taken++){
  uint32_t ops[32];for(unsigned i=0;i<planned;i++)ops[i]=0x38840007u;
  auto cpu=input(seed);cpu.cr=taken?0x20000000u:0;
  if(wanted>=2)ops[0]=0x91060000u; // stw r8,0(r6)
  if(wanted>=3)ops[1]=0x30630001u; // addic r3,r3,1: forwarded GPR/XER before exit
  uint32_t branch=(16u<<26)|(12u<<21)|(2u<<16)|0x40u;
  if(kind==1)branch=(branch&~0xffffu)|0xffc1u; // negative displacement + LK
  if(kind==2){branch=(16u<<26)|(16u<<21)|0x41u;cpu.spr[SPR::CTR]=taken?2:1;}
  if(kind==3)branch=(branch&~0xffffu)|0x102u; // absolute target, other page
  if(kind==4)branch=(branch&~0xffffu)|4u; // taken branch to sequential target
  if(kind==5)branch=(19u<<26)|(12u<<21)|(2u<<16)|(16u<<1); // conditional bclr
  if(kind==6)branch=(19u<<26)|(12u<<21)|(2u<<16)|(528u<<1); // conditional bcctr
  ops[wanted-1]=branch;
  ops[wanted]=0x90860004u; // poison store is reached only by the untaken control
  sheep_mmu_test_page(ram.data(),va,0x61,false);
  trial(ops,planned,cpu,taken?wanted:planned,false);
 }

 // Actual native tag, read-permission, store-permission and supervisor guards
 // must continue to back off for all zero/one/two/three-retirement prefixes.
 // A four-retirement fault is the threshold control and retains old behavior.
 for(unsigned planned:{4u,8u,32u})for(unsigned seed=0;seed<3;seed++)
 for(unsigned prefix=0;prefix<=4;prefix++)for(unsigned kind=0;kind<5;kind++){
  if(prefix>=planned)continue;
  uint32_t ops[32];for(unsigned i=0;i<planned;i++)ops[i]=0x38840007u;
  auto cpu=input(seed);
  if(prefix>=1)ops[0]=0x30630001u;
  if(prefix>=2)ops[1]=0x91060000u; // a prior successful store survives the later fault
  if(prefix>=3)ops[2]=spr_op(SPR::LR,467,3);
  uint16_t permissions=0x61;
  if(kind==0){ops[prefix]=0x81070000u;cpu.gpr[7]=0x76543000u;}
  if(kind==1){ops[prefix]=0x81070000u;permissions=0x60;}
  if(kind==2){ops[prefix]=0x91070000u;permissions=0x41;}
  if(kind>=3){ops[prefix]=spr_op(kind==3?26:27,kind==3?339:467,8);cpu.msr|=MSR::PR;}
  // Permission failures must not make the *earlier* store fail first. The
  // read-denied prefix stays scalar; a store-denied page still permits loads.
  if(kind==1&&prefix>=2)ops[1]=0x68840001u;
  if(kind==2&&prefix>=2)ops[1]=0x80860000u;
  sheep_mmu_test_page(ram.data(),va,permissions,false);
  trial(ops,planned,cpu,prefix,true);
 }

 sheep_rom_only=true;sheep_rom_direct_reset();
 sheep_rom_dispatch_words[cell]=saved_word;sheep_jit_reject_filter[cell]=saved_counter;
 sheep_hot_slots[hot_index]=saved_slot;
 // Like the other emitter selftests, discard test native code and cache hits.
 for(auto &entry:cache)entry=Entry{};
 reset_native_and_rom();sheep_mmu_test_page(nullptr,0,0,false);
 ppc_state=saved;shadow=saved_shadow;exec_flags=saved_flags;ppc_next_instruction_address=saved_next;
 sheep_rom_direct_enabled=saved_direct;sheep_hot_enabled=saved_hot;sheep_rom_only=saved_only;
 memory_bail=saved_bail;sheep_hot_used=saved_used;mode=saved_mode;
 memory_hits=saved_hits;memory_misses=saved_misses;
 written=saved_written;written_size=saved_written_size;written_old=saved_written_old;written_new=saved_written_new;
 fprintf(stderr,"SHORT SUCCESS POLICY SELFTEST %u interpreter-oracle fixtures, %u hot/direct/slow API repeats, %u exact-backoff ticks passed\n",
  fixtures,api_runs,backoff_ticks);
}

#include "fastmem-offset.inc"
extern int nw_jit_shared_alias_literals_selftest();
extern uint64_t g_icycles;
extern volatile bool exec_timer;
#include "halfword-guard-tests.inc"

extern int sheep_page_guards_selftest();
#include "decode-bound.inc"
extern void sheep_rom_plan_bound_selftest();
extern int sheep_tag_dispatch_selftest();
extern int sheep_generic_continuation_selftest();
extern int sheep_scalar_branch_selftest();
bool sheep_jit_enabled(){
 if(mode<0){const char*m=std::getenv("SHEEP_PIPPIN_JIT");mode=m&&!std::strcmp(m,"on")?2:m&&!std::strcmp(m,"verify")?1:0;std::atexit(stats);if(mode){sheep_mmu_jit_layout(&native_layout);native_layout.msr=offsetof(SetPRS,msr);native_layout.srr0=offsetof(SetPRS,spr)+SPR::SRR0*4;native_layout.srr1=offsetof(SetPRS,spr)+SPR::SRR1*4;native_layout.privilege_mask=MSR::PR;native_layout.leaf=native_layout.dtlb_slot!=0;native_layout.memory_bail=reinterpret_cast<uintptr_t>(&memory_bail);if(mode==1){native_layout.trace_host=reinterpret_cast<uintptr_t>(&written);native_layout.trace_old=reinterpret_cast<uintptr_t>(&written_old);native_layout.trace_size=reinterpret_cast<uintptr_t>(&written_size);}reset_native_and_rom();sheep_mmu_rom_bounds(sheep_rom_host_begin,sheep_rom_host_end);prepare_rom_dispatch();if(mode==1){reset_native_and_rom();if(!sheep_rom_window_selftest())throw std::runtime_error("Private ROM window MMU selftest failed");reset_native_and_rom();if(!sheep_rom_window_loop_selftest())throw std::runtime_error("Private ROM window loop selftest failed");reset_native_and_rom();if(!sheep_scalar_branch_selftest())throw std::runtime_error("Private scalar branch selftest failed");reset_native_and_rom();sheep_halfword_guard_selftest();reset_native_and_rom();if(!sheep_generic_continuation_selftest())throw std::runtime_error("Private generic continuation selftest failed");reset_native_and_rom();const bool fetch_ok=sheep_fetch_barrier_selftest()!=0;reset_native_and_rom();const bool raw_ok=nw_jit_raw_literals_selftest()!=0;reset_native_and_rom();fprintf(stderr,"CPU CORRECTNESS SELFTEST SUMMARY fetch=%u raw_literals=%u\n",unsigned(fetch_ok),unsigned(raw_ok));if(!fetch_ok||!raw_ok)throw std::runtime_error("Private MPC603 fetch/raw-literal selftests failed");if(!sheep_live_itlb_selftest())throw std::runtime_error("Private live ITLB selftest failed");reset_native_and_rom();sheep_rom_plan_bound_selftest();reset_native_and_rom();if(!sheep_decode_bound_selftest())throw std::runtime_error("Private ROM decode-bound selftest failed");reset_native_and_rom();if(!sheep_tag_dispatch_selftest())throw std::runtime_error("Private tag dispatch selftest failed");reset_native_and_rom();if(!sheep_page_guards_selftest())throw std::runtime_error("Private v144 selftest failed");reset_native_and_rom();if(!nw_jit_shared_alias_literals_selftest())throw std::runtime_error("Shared alias literal selftest failed");reset_native_and_rom();expanded_integer_selftest();privileged_srr_selftest();direct_state_selftest();accounted_boundary_selftest();long_block_selftest();memory_selftest();superblock_selftest();warmup_selftest();rom_page_selftest();memory_cache_selftest();memory_ea_selftest();scalar_lowering_selftest();bit_branch_selftest();sheep_compact_dispatch_selftest();sheep_branch_dispatch_selftest();sheep_mmu_itlb_selftest();sheep_mmu_split_read_selftest();packed_reject_selftest();rom_reject_selftest();rom_direct_selftest();hot_republish_selftest();cold_exit_selftest();short_success_policy_selftest();unaligned_ram_selftest();metadata_guard_selftest();std::memset(sheep_jit_reject_filter,0,sizeof(sheep_jit_reject_filter));reset_rom_dispatch_status();fused_rom_selftest();sheep_cpu_loop_selftest();sheep_cpu_loop_cross_selftest();sheep_cpu_loop_ram_selftest();hardware_fastmem_selftest();fastmem_offset_selftest();guard_forwarding_selftest();reset_native_and_rom();if(!nw_jit_island_pack_selftest())throw std::runtime_error("Private island pack selftest failed");reset_native_and_rom();sheep_hot_used=false;}const char *window_setting=std::getenv("SHEEP_PIPPIN_ROM_WINDOW");const bool window_requested=window_setting&&!std::strcmp(window_setting,"on");sheep_mmu_rom_window_enable(window_requested);fprintf(stderr,"PIPPIN-ROM-WINDOW-POLICY requested=%u after_startup_tests=1\n",unsigned(window_requested));fprintf(stderr,"SHEEP-PIPPIN isolated ARM64 JIT enabled (%s); hardware/MMU fallback preserved\n",mode==1?"verify":"on");const char *only=std::getenv("SHEEP_PIPPIN_ROM_ONLY");sheep_rom_only=only&&!std::strcmp(only,"1");fprintf(stderr,"Native source scope=%s\n",sheep_rom_only?"immutable ROM":"ROM and RAM");const char *fast=std::getenv("SHEEP_PIPPIN_FAST_CACHE");sheep_hot_enabled=mode==2&&ppc_state.spr[SPR::PVR]==PPC_VER::MPC603&&(!fast||std::strcmp(fast,"off"));fprintf(stderr,"ROM fast dispatcher=%s\n",sheep_hot_enabled?"on":"off");const char *direct=std::getenv("SHEEP_PIPPIN_ROM_DIRECT");sheep_rom_direct_enabled=sheep_hot_enabled&&sheep_rom_only&&(!direct||std::strcmp(direct,"off"));}}
 return mode>0;
}
static void reject_static_trace(const uint8_t *code,uint32_t pc){
 // ROM byte sequences cannot acquire new translatable instructions. Writable
 // code must remain retryable so a guest store can replace an unsupported op.
 sheep_reject_set(code,pc,sheep_mmu_code_is_rom(code,pc)?255:254);
}
unsigned sheep_jit_try(const uint8_t* code,unsigned budget,unsigned decode_bound){
 if(!sheep_jit_enabled())return 0;
 if(!mode||budget<4||ppc_state.spr[SPR::PVR]!=PPC_VER::MPC603)return 0;
 if(sheep_rom_only&&!sheep_mmu_code_is_rom(code,ppc_state.pc)){reject_static_trace(code,ppc_state.pc);return 0;}
 // A real ROM caller may prove a readable decode span independently of the
 // current execution deadline. Default/RAM callers keep the original policy.
 unsigned scan_limit=std::min(budget,sheep_native_max_ops);
 bool bounded_rom=false;
 if(decode_bound&&sheep_mmu_code_is_rom(code,ppc_state.pc)){
  const uintptr_t host=reinterpret_cast<uintptr_t>(code);
  if((host|ppc_state.pc)&3u)return 0;
  if(host<sheep_rom_host_begin||host>=sheep_rom_host_end)return 0;
  const uintptr_t words=(sheep_rom_host_end-host)/4;
  scan_limit=unsigned(std::min<uintptr_t>(std::min(decode_bound,sheep_native_max_ops),
              std::min<uintptr_t>((4096u-(ppc_state.pc&4095u))/4,words)));
  if(scan_limit<4)return 0; // No complete minimum plan; never copy a sentinel past this bound.
  bounded_rom=true;
 }
 unsigned slot=((uintptr_t(code)>>2)^((uint64_t(ppc_state.pc)*0x9e3779b1u)>>16))&(cache.size()-1);auto*entry=&cache[slot];
 nw_jit_fn fn=nullptr;unsigned n=0;std::array<uint32_t,sheep_native_max_ops> ops; // Populated only on compile or verification; cache hits need no scratch writes.
 if(entry->host==code&&entry->pc==ppc_state.pc&&(!bounded_rom||entry->n<=scan_limit)){
  // A stable full warm record may mature on a shorter execution budget.
  // A compiled entry still cannot execute beyond that budget.
  if(entry->n>budget&&(!bounded_rom||entry->fn))return 0;
  if(!std::memcmp(entry->bytes.data(),code,entry->n*4)){if(entry->negative){negative_hits++;reject_static_trace(code,ppc_state.pc);return 0;}if(!entry->fn&&entry->warmup<4){entry->warmup++;sheep_reject_set(code,ppc_state.pc,15);return 0;}fn=entry->fn;n=entry->n;}
 }
 if(!fn){
  n=0; // Warm records have no decoded-op scratch array; rebuild it from byte zero.
  const bool immutable=sheep_mmu_code_is_rom(code,ppc_state.pc);bool stored=false;
  for(;n<scan_limit;n++){
   const uint8_t*p=code+n*4;uint32_t op=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
   if(!eligible(op))break;
   unsigned memory=sheep_jit_memory_primary(op),primary=op>>26,bo=(op>>21)&31,xo=(op>>1)&1023;
   bool store=memory==36||memory==37||memory==38||memory==39||memory==44||memory==45;
   if(store&&stored)break; // Single trial write keeps the verifier transactional.
   ops[n]=op;
   bool conditional=primary==16||(primary==19&&(xo==16||xo==528)&&bo!=20);
   unsigned spr=((op>>16)&31)|((op>>6)&0x3e0);bool srr_transfer=primary==31&&(xo==339||xo==467)&&(spr==26||spr==27);
   if((nw_jit_op_ends_block(op)&&!conditional&&!srr_transfer&&op!=0x48000004u)||(store&&!immutable)){n++;break;}
   stored|=store;
  }
  if(n<4){*entry=Entry{};entry->host=code;entry->pc=ppc_state.pc;entry->n=std::min(n+1,scan_limit);entry->negative=true;std::memcpy(entry->bytes.data(),code,entry->n*4);reject_static_trace(code,ppc_state.pc);return 0;}
  // Avoid translating cold traces. Warm records still validate the complete
  // source span on retry; the interpreter runs every skipped guest instruction.
  if(entry->host!=code||entry->pc!=ppc_state.pc||entry->n!=n||entry->warmup<4||entry->negative||std::memcmp(entry->bytes.data(),code,n*4)){
   *entry=Entry{};entry->host=code;entry->pc=ppc_state.pc;entry->n=n;entry->warmup=1;std::memcpy(entry->bytes.data(),code,n*4);warmups++;sheep_reject_set(code,ppc_state.pc,15);return 0;
  }
  fn=nw_jit_compile_mapped(ops.data(),n,ppc_state.pc,&native_layout);
  // Preserve the prior bounded translation if a larger emission does not fit.
  if(!fn&&n>16){n=16;fn=nw_jit_compile_mapped(ops.data(),n,ppc_state.pc,&native_layout);}
  auto wrapped=nw_jit_wrap_count();if(wrapped!=generation){sheep_rom_direct_reset();for(auto&e:cache)e=Entry{};generation=wrapped;sheep_hot_epoch=wrapped;}entry=&cache[slot];if(!fn){*entry=Entry{};entry->host=code;entry->pc=ppc_state.pc;entry->n=n;entry->negative=true;std::memcpy(entry->bytes.data(),code,n*4);reject_static_trace(code,ppc_state.pc);return 0;}compiles++;auto g=nw_jit_wrap_count();if(g!=generation){sheep_rom_direct_reset();for(auto&e:cache)e=Entry{};generation=g;sheep_hot_epoch=g;}entry=&cache[slot];*entry=Entry{};entry->host=code;entry->pc=ppc_state.pc;entry->n=n;entry->fn=fn;entry->rom=sheep_mmu_code_is_rom(code,ppc_state.pc);if(entry->rom)rom_compiles++;else ram_compiles++;for(unsigned j=0;j<n;j++){entry->memory_count+=sheep_jit_memory_primary(ops[j])!=0;}std::memcpy(entry->bytes.data(),code,n*4);
 }
 // Compilation/cache population may use the complete proven ROM span,
 // but neither native publication nor execution may exceed this deadline.
 if(n>budget)return 0;
 sheep_reject_set(code,ppc_state.pc,0);
 if(mode==2&&entry->rom){auto &hot=sheep_hot_slots[sheep_hot_index(code,ppc_state.pc)];hot=SheepHotSlot{code,ppc_state.pc,n,fn,sheep_hot_epoch};sheep_rom_direct_publish(code,ppc_state.pc,n,fn);}
 if(mode==1)for(unsigned i=0;i<n;i++){const uint8_t*p=code+i*4;ops[i]=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}

 unsigned planned=n;
 memory_bail=false;written=nullptr;
 if(mode==1){
  // Qualified translations access only these fields. Keep the full SetPRS
  // layout for emitted offsets, but copy no FPRs, segment registers or 1024-SPR bank.
  std::memcpy(shadow.gpr,ppc_state.gpr,sizeof(shadow.gpr));
  shadow.pc=ppc_state.pc;shadow.cr=ppc_state.cr;shadow.msr=ppc_state.msr;
  shadow.spr[SPR::XER]=ppc_state.spr[SPR::XER];
  shadow.spr[SPR::LR]=ppc_state.spr[SPR::LR];shadow.spr[SPR::CTR]=ppc_state.spr[SPR::CTR];
  shadow.spr[SPR::SRR0]=ppc_state.spr[SPR::SRR0];shadow.spr[SPR::SRR1]=ppc_state.spr[SPR::SRR1];
  unsigned actual=reinterpret_cast<nw_jit_mapped_fn>(fn)(reinterpret_cast<nw_jit_cpu*>(&shadow));
  if(actual>n)throw std::runtime_error("Mapped block count exceeds plan");n=actual;
  uint32_t pc=ppc_state.pc;
  if(written){written_new=read_be(written,written_size);}
  if(written)write_be(written,written_size,written_old);
  for(unsigned i=0;i<n;i++){
   ppc_main_opcode(ppc_opcode_grabber,ops[i]);
   if(exec_flags & EXEF_BRANCH)ppc_state.pc=ppc_next_instruction_address;
   else ppc_state.pc+=4;
   exec_flags=0; // A folded branch-to-next may precede another trial opcode.
  }
  if(std::memcmp(shadow.gpr,ppc_state.gpr,sizeof(shadow.gpr))||shadow.cr!=ppc_state.cr||shadow.spr[SPR::XER]!=ppc_state.spr[SPR::XER]||shadow.spr[SPR::LR]!=ppc_state.spr[SPR::LR]||shadow.spr[SPR::CTR]!=ppc_state.spr[SPR::CTR]||shadow.spr[SPR::SRR0]!=ppc_state.spr[SPR::SRR0]||shadow.spr[SPR::SRR1]!=ppc_state.spr[SPR::SRR1]||shadow.pc!=ppc_state.pc||(written&&read_be(written,written_size)!=written_new)){
   mismatches++;stats();for(unsigned j=0;j<n;j++)fprintf(stderr," op[%u]=%08x\n",j,ops[j]);fprintf(stderr,"JIT mismatch PC=%08x n=%u next=%08x/%08x CR=%08x/%08x XER=%08x/%08x\n",pc,n,shadow.pc,ppc_state.pc,shadow.cr,ppc_state.cr,shadow.spr[SPR::XER],ppc_state.spr[SPR::XER]);throw std::runtime_error("Pippin JIT/interpreter mismatch");
  }
  ppc_state.pc=pc;verified+=n;
  exec_flags=0;
 }else{
  uint32_t pc=ppc_state.pc;
  unsigned actual=reinterpret_cast<nw_jit_mapped_fn>(fn)(reinterpret_cast<nw_jit_cpu*>(&ppc_state));
  if(actual>n)throw std::runtime_error("Mapped block count exceeds plan");n=actual;
  shadow.pc=ppc_state.pc;
  ppc_state.pc=pc;
 }
 if(sheep_native_requires_backoff(n))sheep_reject_set(code,ppc_state.pc,32);
 if(!n)return 0;
 // Common loop advances instruction accounting, then applies this precise target.
 if(mode==1&&native_layout.dtlb_slot){
  if(n==planned)memory_hits+=entry->memory_count;
  else{memory_misses++;for(unsigned j=0;j<n;j++){uint32_t op=uint32_t(code[j*4])<<24|uint32_t(code[j*4+1])<<16|uint32_t(code[j*4+2])<<8|code[j*4+3];memory_hits+=sheep_jit_memory_primary(op)!=0;}}
 }
 if(shadow.pc!=uint32_t(ppc_state.pc+n*4)||(shadow.pc&~4095u)!=(ppc_state.pc&~4095u)){ppc_next_instruction_address=shadow.pc;exec_flags|=EXEF_BRANCH;}
 if(mode==1){blocks++;insns+=n;}return n;
}
