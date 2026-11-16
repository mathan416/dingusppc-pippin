// Project: DingusPPC-Pippin
// File: cpu/ppc/flags-bfi.inc
// Purpose: Condition-flag helpers for JIT code generation.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// Include in ppcexec.cpp after loop_fixture_event(), then call once at the
// beginning of sheep_cpu_loop_selftest(). This is the Q129 mapped-leaf oracle.
static void sheep_flags_bfi_selftest(){
 const auto saved=ppc_state;const auto saved_flags=exec_flags,saved_next=ppc_next_instruction_address;
 const bool saved_bail=sheep_jit_memory_bail;
 nw_jit_layout layout={offsetof(SetPRS,gpr),offsetof(SetPRS,cr),offsetof(SetPRS,spr)+SPR::XER*4,
  offsetof(SetPRS,spr)+SPR::LR*4,offsetof(SetPRS,spr)+SPR::CTR*4,offsetof(SetPRS,pc),reinterpret_cast<void*>(1)};
 sheep_mmu_jit_layout(&layout);layout.leaf=1;layout.fast_read_slot=layout.fast_write_slot=0;
 layout.trace_host=layout.trace_old=layout.trace_size=0;layout.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);
 unsigned cases=0;
 auto input=[&](unsigned old_flags){
  SetPRS cpu=saved;cpu.pc=0x10000000u;
  for(unsigned r=0;r<32;r++)cpu.gpr[r]=0x913579bdu^(r*0x1020301u);
  cpu.spr[SPR::XER]=(old_flags<<29)|0x01234567u;
  cpu.cr=(old_flags&1)?0xffffffffu:0;
  cpu.spr[SPR::LR]=0x1357246bu;cpu.spr[SPR::CTR]=0x24681357u;
  return cpu;
 };
 auto opx=[](unsigned xo,unsigned rd,unsigned ra,unsigned rb,unsigned record){
  return (31u<<26)|(rd<<21)|(ra<<16)|(rb<<11)|(xo<<1)|record;
 };
 auto trial=[&](const char *label,const uint32_t *ops,const SetPRS &initial,unsigned helpers,unsigned cr_fields){
  auto fn=nw_jit_compile_mapped(ops,4,initial.pc,&layout);
  if(!fn)throw std::runtime_error("Flags-BFI mapped-leaf fixture compilation failed");
  const uintptr_t begin=reinterpret_cast<uintptr_t>(fn),base=reinterpret_cast<uintptr_t>(nw_jit_native_base());
  const uintptr_t end=base+nw_jit_code_used();
  if(begin<base||begin>=end||((begin|end)&3))throw std::runtime_error("Flags-BFI native block bounds invalid");
  auto contains=[&](uint32_t wanted){
   for(uintptr_t address=begin;address<end;address+=4){uint32_t word;std::memcpy(&word,reinterpret_cast<const void*>(address),4);if(word==wanted)return true;}
   return false;
  };
  // These exact BFI/ORR words were checked with an independent A64 assembler.
  // Requiring them prevents this fixture silently testing the old backend or
  // a helper fallback instead of the three modified mapped-leaf emitters.
  if(((helpers&1)&&!contains(0x33030149u))||((helpers&2)&&(!contains(0x33020149u)||!contains(0x2a0a7d29u))))
   throw std::runtime_error("Flags-BFI fixture did not emit the new CA/OV/sticky-SO instructions");
  for(unsigned field=0;field<8;field++)if(cr_fields&(1u<<field)){
   const unsigned shift=28-field*4;
   const uint32_t bfi=0x33000d2au|(((32-shift)&31u)<<16);
   if(!contains(bfi))throw std::runtime_error("Flags-BFI fixture did not emit the requested CR-field insertion");
  }
  auto actual=initial;sheep_jit_memory_bail=false;
  const uint64_t checked=sheep_jit_abi_check(fn,reinterpret_cast<nw_jit_cpu*>(&actual));
  const bool bailed=sheep_jit_memory_bail;
  ppc_state=initial;exec_flags=0;ppc_next_instruction_address=saved_next;
  for(unsigned i=0;i<4;i++){
   ppc_main_opcode(ppc_opcode_grabber,ops[i]);
   if(exec_flags)throw std::runtime_error("Flags-BFI arithmetic/compare reference unexpectedly trapped or branched");
   ppc_state.pc+=4;
  }
  if((checked>>32)||uint32_t(checked)!=4||bailed||std::memcmp(&actual,&ppc_state,sizeof(SetPRS))){
   fprintf(stderr,"FLAGS BFI %s case=%u count=%u ABI=%08x bail=%u pc=%08x/%08x CR=%08x/%08x XER=%08x/%08x\n",
    label,cases,uint32_t(checked),uint32_t(checked>>32),bailed,actual.pc,ppc_state.pc,actual.cr,ppc_state.cr,
    actual.spr[SPR::XER],ppc_state.spr[SPR::XER]);
   for(unsigned i=0;i<4;i++)fprintf(stderr," op[%u]=%08x\n",i,ops[i]);
   throw std::runtime_error("Flags-BFI full CPU/interpreter oracle mismatch");
  }
  cases++;
 };
 struct Pair {uint32_t a,b;};
 const Pair arithmetic[]={
  {0,0},{1,2},{0xffffffffu,1},{0x7fffffffu,1},
  {0x80000000u,0x80000000u},{0x80000000u,1},{1,0x80000000u},{0xffffffffu,0xffffffffu}
 };
 // 768 cases. Each arithmetic family reaches all four new (CA,OV) output
 // combinations, under all eight incoming (SO,OV,CA) states. OE+Rc forces
 // the same live ADDS/SUBS NZCV through CA, OV and the CR0/SO update. Aliases
 // also expose any corruption of W8 before the result reaches its destination.
 for(unsigned xo:{522u,520u})for(unsigned record=0;record<2;record++)for(unsigned rd:{5u,3u,4u})
 for(const auto &pair:arithmetic)for(unsigned old_flags=0;old_flags<8;old_flags++){
  auto cpu=input(old_flags);cpu.gpr[3]=pair.a;cpu.gpr[4]=pair.b;
  uint32_t ops[]={opx(xo,rd,3,4,record),0x398c0001u,0x69ad0001u,0x60000000u};
  trial("OE/carry/overflow/record/result-alias",ops,cpu,3,record?1:0);
 }
 const Pair comparisons[]={
  {0,0},{1,0},{0,1},{0x80000000u,0},
  {0x7fffffffu,0xffffffffu},{0xffffffffu,0x80000000u},
  {0x80000000u,0x7fffffffu},{0xffffffffu,0xffffffffu}
 };
 const Pair immediates[]={
  {0,0},{1,0},{0,1},{0x80000000u,0x8000u},
  {0x7fffffffu,0x7fffu},{0xffffffffu,0xffffu},
  {0x0000ffffu,0xffffu},{0xffff8000u,0x8000u}
 };
 // 2048 cases. Signed/unsigned register and immediate comparisons target all
 // eight fields, including CR7's sh=0/BFXIL alias. Every old XER bit state is
 // retained; its SO alone is copied into the selected nibble. The nonselected
 // fields begin alternately all-zero/all-one to catch leakage in either way.
 for(unsigned field=0;field<8;field++)for(unsigned kind=0;kind<4;kind++)
 for(unsigned pair=0;pair<8;pair++)for(unsigned old_flags=0;old_flags<8;old_flags++){
  const auto values=kind<2?comparisons[pair]:immediates[pair];
  auto cpu=input(old_flags);cpu.gpr[3]=values.a;cpu.gpr[4]=values.b;
  const uint32_t compare=kind<2?opx(kind?32:0,field*4,3,4,0):
   ((kind==2?11u:10u)<<26)|(field<<23)|(3u<<16)|uint16_t(values.b);
  uint32_t ops[]={compare,0x398c0001u,0x69ad0001u,0x60000000u};
  trial("all-CR-fields/signed-unsigned/register-immediate/SO",ops,cpu,0,1u<<field);
 }
 // Eight sequences force newly raised overflow followed by nonoverflow: OV
 // must clear, SO must remain set, CA must still change independently, and a
 // subsequent compare must copy sticky SO into a different CR field.
 for(unsigned old_flags=0;old_flags<8;old_flags++){
  auto cpu=input(old_flags);cpu.gpr[3]=0x7fffffffu;cpu.gpr[4]=1;cpu.gpr[10]=cpu.gpr[11]=0;
  uint32_t ops[]={opx(522,5,3,4,1),opx(522,6,10,11,1),opx(520,7,10,11,1),opx(0,7*4,5,6,0)};
  trial("raise-overflow/clear-OV/retain-SO/compare",ops,cpu,3,(1u<<0)|(1u<<7));
 }
 if(cases!=2824)throw std::runtime_error("Flags-BFI deterministic coverage count differs");
 sheep_rom_direct_reset();nw_jit_reset();ppc_state=saved;exec_flags=saved_flags;ppc_next_instruction_address=saved_next;sheep_jit_memory_bail=saved_bail;
 fprintf(stderr,"FLAGS BFI SELFTEST %u mapped-leaf/full-CPU/ABI/CA/OV/sticky-SO/eight-CR-field cases passed\n",cases);
}
