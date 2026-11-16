// Project: DingusPPC-Pippin
// File: cpu/ppc/fastmem-offset.inc
// Purpose: Verify-mode tests for register-offset fastmem loads and stores.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
//   2026-11-16 - Fastmem self-tests are skipped on hosts without 4 KB pages (e.g. a Pi 5 16 KB kernel).
// Full history: PIPPIN.md and Git history.
// Include in sheep_jit_bridge.cpp after hardware_fastmem_selftest(), before
// sheep_jit_enabled(). Call fastmem_offset_selftest() during verify setup.
// Inherited hardware_fastmem_selftest() already covers the traced payloads.
static void fastmem_offset_selftest(){
 if(!pippin_fastmem_host_supported())return;
 if(!pippin_fastmem_enabled())throw std::runtime_error("Register-offset fixture requires hardware fastmem");
 const auto saved_layout=native_layout;
 const auto saved_cpu=ppc_state;
 const auto saved_flags=exec_flags,saved_next=ppc_next_instruction_address;
 const bool saved_bail=memory_bail;
 auto *const saved_written=written;
 const auto saved_written_size=written_size,saved_written_old=written_old,saved_written_new=written_new;

 // Close the normal-mode coverage gap with the existing full hardware matrix:
 // all 14 D/indexed forms, unaligned/page-end/high-VA cases, read/write aliases,
 // device/permission faults, prefix materialization and mapping transitions.
 native_layout.trace_host=native_layout.trace_old=native_layout.trace_size=0;
 try{hardware_fastmem_selftest();}
 catch(...){native_layout=saved_layout;throw;}
 native_layout=saved_layout;

 auto *read=pippin_fastmem_alloc(4096),*write=pippin_fastmem_alloc(4096);
 if(!read||!write)throw std::runtime_error("Register-offset fixture allocation failed");
 std::array<uint8_t,4096> before_read{},before_write{},native_read{},native_write{};
 constexpr uint32_t root=0x10000000;
 static const unsigned primaries[]={36,38,44};
 static const unsigned store_xo[]={151,215,407};
 unsigned cases=0;
 auto check=[&](bool okay,const char *message,unsigned primary,unsigned indexed,unsigned traced,unsigned mode,unsigned offset,unsigned update){
  if(okay)return;
  fprintf(stderr,"FASTMEM OFFSET primary=%u indexed=%u traced=%u mode=%u offset=%u update=%u case=%u\n",
   primary,indexed,traced,mode,offset,update,cases);
  throw std::runtime_error(message);
 };
 for(unsigned form=0;form<3;form++)for(unsigned indexed=0;indexed<2;indexed++)
 for(unsigned traced=0;traced<2;traced++)for(unsigned mode=0;mode<3;mode++)
 for(unsigned edge=0;edge<2;edge++)for(unsigned update=0;update<2;update++){
  const unsigned primary=primaries[form]+update;
  const unsigned size=primaries[form]==36?4:primaries[form]==38?1:2;
  const unsigned offset=edge?4096-size:1;
  const uint32_t va=mode==2?0xfffff000u:mode==1?0x68066000u:0x1234000u;
  const uint32_t ea=va+offset;
  const unsigned source=update?7:8; // Updating stores must consume old RA.
  for(unsigned i=0;i<4096;i++){
   read[i]=uint8_t(i*37+form*17+mode*29);
   write[i]=uint8_t(i*13+form*23+mode*41+7);
  }
  std::copy_n(read,4096,before_read.begin());std::copy_n(write,4096,before_write.begin());
  sheep_mmu_test_fast_page(read,write,va,0x61,false,mode);
  ppc_state=saved_cpu;ppc_state.pc=root;
  ppc_state.gpr[3]=0xfffffffdu;ppc_state.gpr[4]=0x13572468u;
  ppc_state.gpr[6]=0x120;ppc_state.gpr[7]=ea-(indexed?0x120u:7u);
  ppc_state.gpr[8]=0x89abcdefu^(mode*0x1020301u);
  ppc_state.cr=0x872513abu;ppc_state.spr[SPR::XER]=0xa0000000u;
  const auto initial=ppc_state;
  const uint32_t memory=indexed?
   ((31u<<26)|(source<<21)|(7u<<16)|(6u<<11)|((store_xo[form]+update*32u)<<1)):
   ((primary<<26)|(source<<21)|(7u<<16)|7u);
  const uint32_t ops[]={0x38630001u,memory,0x38630002u,0x38840005u};
  uint8_t *trace_host=nullptr;
  uint32_t trace_old=0xc5a33aa5u,trace_size=0xa5a55a5au;
  auto layout=native_layout;
  layout.trace_host=traced?reinterpret_cast<uintptr_t>(&trace_host):0;
  layout.trace_old=traced?reinterpret_cast<uintptr_t>(&trace_old):0;
  layout.trace_size=traced?reinterpret_cast<uintptr_t>(&trace_size):0;
  auto fn=nw_jit_compile_mapped(ops,4,root,&layout);
  check(fn&&nw_jit_last_fastmem_ops()==1,"Register-offset fixture did not compile a hardware payload",
   primary,indexed,traced,mode,offset,update);
  auto result=initial;memory_bail=false;written=nullptr;
  const auto faults=pippin_fastmem_fault_count();
  const unsigned retired=checked(fn,reinterpret_cast<nw_jit_cpu*>(&result));
  check(retired==4&&!memory_bail&&pippin_fastmem_fault_count()==faults,
   "Register-offset valid mapping unexpectedly faulted or lost its prefix",primary,indexed,traced,mode,offset,update);
  check(written==nullptr,"Register-offset fixture touched the global journal instead of its layout journal",
   primary,indexed,traced,mode,offset,update);
  if(traced){
   // Odd halfwords use the canonical software payload and its host pointer.
   // Both routes must still journal the exact old value and stored bytes.
   auto *expected_host=(size==2&&(ea&1))?write+offset:
    reinterpret_cast<uint8_t*>(pippin_fastmem_write+uintptr_t(ea));
   check(trace_host==expected_host&&trace_size==size&&trace_old==read_be(before_write.data()+offset,size),
    "Register-offset journal lost the write-alias address, old value or size",primary,indexed,traced,mode,offset,update);
   const uint32_t mask=size==4?0xffffffffu:size==2?0xffffu:0xffu;
   check(read_be(trace_host,size)==(initial.gpr[source]&mask),
    "Register-offset journal address does not observe the actual stored bytes",primary,indexed,traced,mode,offset,update);
  }else{
   check(trace_host==nullptr&&trace_old==0xc5a33aa5u&&trace_size==0xa5a55a5au,
    "Normal register-offset payload unexpectedly wrote journal metadata",primary,indexed,traced,mode,offset,update);
  }
  std::copy_n(read,4096,native_read.begin());std::copy_n(write,4096,native_write.begin());
  std::copy(before_read.begin(),before_read.end(),read);std::copy(before_write.begin(),before_write.end(),write);
  ppc_state=initial;exec_flags=0;ppc_next_instruction_address=saved_next;
  for(uint32_t op:ops){
   ppc_main_opcode(ppc_opcode_grabber,op);
   check(!exec_flags,"Register-offset canonical replay unexpectedly changed flags",primary,indexed,traced,mode,offset,update);
   ppc_state.pc+=4;
  }
  check(!std::memcmp(&result,&ppc_state,sizeof(SetPRS))&&
   std::equal(native_read.begin(),native_read.end(),read)&&std::equal(native_write.begin(),native_write.end(),write),
   "Register-offset full CPU/read-write RAM oracle differs",primary,indexed,traced,mode,offset,update);
  sheep_mmu_test_fast_page(nullptr,nullptr,0,0,false,0);
  cases++;
 }
 if(cases!=144)throw std::runtime_error("Register-offset focused coverage count differs");
 reset_native_and_rom();
 if(!pippin_fastmem_release(read)||!pippin_fastmem_release(write))throw std::runtime_error("Register-offset fixture release failed");
 native_layout=saved_layout;ppc_state=saved_cpu;exec_flags=saved_flags;ppc_next_instruction_address=saved_next;
 memory_bail=saved_bail;written=saved_written;written_size=saved_written_size;written_old=saved_written_old;written_new=saved_written_new;
 fprintf(stderr,"FASTMEM OFFSET SELFTEST normal inherited hardware matrix plus %u direct-hit/update/journal CPU/RAM/ABI cases passed\n",cases);
}
