// Project: DingusPPC-Pippin
// File: cpu/ppc/halfword-guard-tests.inc
// Purpose: Verify-mode tests for halfword loads and stores near guard pages.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
//   2026-11-16 - Fastmem self-tests are skipped on hosts without 4 KB pages (e.g. a Pi 5 16 KB kernel).
// Full history: PIPPIN.md and Git history.
// Actual aliases and unchanged canonical PPC interpreter. No widened access.
static void sheep_halfword_guard_selftest(){
#if defined(__aarch64__)
 if(!pippin_fastmem_host_supported())return;
 SetPRS saved;std::memcpy(&saved,&ppc_state,sizeof(saved));
 const auto flags0=exec_flags,nia0=ppc_next_instruction_address;const auto clock0=g_icycles;
 const bool bail0=memory_bail,timer0=exec_timer,power0=power_on;
 auto *const written0=written;const auto size0=written_size,old0=written_old,new0=written_new;
 std::array<uint8_t,8224> resumed_read{},resumed_write{},expected_write{};
 bool fast_bound=false,pair_bound=false;uint8_t *read=nullptr,*write=nullptr;unsigned passed=0,forms=0,hot=0,metadata=0,readonly=0,chains=0;
 auto need=[&](bool good,const char *why){if(!good){fprintf(stderr,"HALFWORD GUARD failure case=%u %s\n",passed+1,why);throw std::runtime_error("Q185 halfword guard fixture failed");}};
 auto unbind=[&](){if(fast_bound){sheep_mmu_test_fast_page(nullptr,nullptr,0,0,false,0);fast_bound=false;}if(pair_bound){sheep_mmu_test_data_pair(nullptr,nullptr,0,0,0,false);pair_bound=false;}};
 auto cleanup=[&](){unbind();reset_native_and_rom();bool released=true;if(read){released=pippin_fastmem_release(read)&&released;read=nullptr;}if(write){released=pippin_fastmem_release(write)&&released;write=nullptr;}std::memcpy(&ppc_state,&saved,sizeof(saved));exec_flags=flags0;ppc_next_instruction_address=nia0;g_icycles=clock0;exec_timer=timer0;power_on=power0;memory_bail=bail0;written=written0;written_size=size0;written_old=old0;written_new=new0;if(!released)throw std::runtime_error("Q185 fixture release failed");};
 try{
  need(pippin_fastmem_enabled(),"real hardware aliases enabled");read=pippin_fastmem_alloc(4096);write=pippin_fastmem_alloc(4096);need(read&&write,"owned read/write allocations");
  auto layout=native_layout;layout.leaf=1;layout.trace_host=layout.trace_old=layout.trace_size=0;
  need(layout.fast_read_slot&&layout.fast_write_slot&&layout.dtlb_slot,"fast path and original software layout");
  auto run=[&](const uint32_t *ops,unsigned n,SetPRS initial,uint32_t va,unsigned mode,uint16_t permissions,unsigned wanted,unsigned faults,unsigned memories,bool resume){
   unbind();reset_native_and_rom();
   for(unsigned i=0;i<4096;i++){read[i]=uint8_t(i*37u+(i>>8)*13u+passed*11u);write[i]=uint8_t(i*19u+0x65u);}
   read[0]=0x7f;read[1]=0xff;read[2]=0x80;read[3]=0;read[4093]=0x7f;read[4094]=0x80;read[4095]=1;
   std::array<uint8_t,4096> old_read{},old_write{},native_read{},native_write{};std::copy_n(read,4096,old_read.begin());std::copy_n(write,4096,old_write.begin());
   sheep_mmu_test_fast_page(read,write,va,permissions,false,mode);fast_bound=true;
   auto fn=nw_jit_compile_mapped(ops,int(n),initial.pc,&layout);need(fn&&nw_jit_last_fastmem_ops()==memories,"real mapped compile/per-access slow metadata");
   // Decode only the straight-line success body, never pools/cold tails.
   const auto *start=reinterpret_cast<const uint32_t*>(fn),*end=start+2048,*ret=start;unsigned tests=0;
   for(;ret<end&&*ret!=0xd65f03c0u;++ret){}need(ret<end,"bounded success epilogue");
   for(auto *at=start;at<ret;++at)if((*at&0xfff8001fu)==0x37000008u){
    ++tests;int32_t d=int32_t((*at>>5)&0x3fffu);if(d&0x2000)d-=0x4000;
    const uintptr_t target=reinterpret_cast<uintptr_t>(at)+uintptr_t(int64_t(d)*4);
    need(target>reinterpret_cast<uintptr_t>(ret)&&target<reinterpret_cast<uintptr_t>(end),"signed14-bit TBNZ W8 bit0 targets appended original slow path");
   }
   need(tests==memories,"one actual odd-address guard per halfword access");
   SetPRS actual;std::memcpy(&actual,&initial,sizeof(actual));exec_flags=0;ppc_next_instruction_address=0x31415920u;g_icycles=123;exec_timer=false;power_on=true;memory_bail=false;written=nullptr;
   const auto before=pippin_fastmem_fault_count();const unsigned count=checked(fn,reinterpret_cast<nw_jit_cpu*>(&actual));const bool bailed=memory_bail;
   need(count==wanted&&bailed==(wanted<n)&&pippin_fastmem_fault_count()==before+faults,"exact retirement/bail/hardware-fault count");
   need(exec_flags==0&&ppc_next_instruction_address==0x31415920u&&g_icycles==123&&!exec_timer&&power_on,"native leaf clock/control/NIA untouched");
   std::copy_n(read,4096,native_read.begin());std::copy_n(write,4096,native_write.begin());std::copy(old_read.begin(),old_read.end(),read);std::copy(old_write.begin(),old_write.end(),write);
   std::memcpy(&ppc_state,&initial,sizeof(initial));exec_flags=0;ppc_next_instruction_address=0x31415920u;
   for(unsigned i=0;i<wanted;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);need(exec_flags==0,"canonical prefix nonfaulting");ppc_state.pc+=4;}
   need(!std::memcmp(&actual,&ppc_state,sizeof(actual))&&std::equal(native_read.begin(),native_read.end(),read)&&std::equal(native_write.begin(),native_write.end(),write),"canonical fullCPU/read-writeRAM/precise prefix");
   if(resume){
    // Repair this private mapping and replay real remaining scalar opcodes.
    // MMIO is tested as refusal, not a fabricated device side effect.
    unbind();
    std::copy(old_read.begin(),old_read.end(),resumed_read.begin());std::copy(native_write.begin(),native_write.end(),resumed_write.begin());
    for(unsigned i=4096;i<resumed_read.size();i++){resumed_read[i]=uint8_t(i*23u+7u);resumed_write[i]=uint8_t(i*31u+5u);}expected_write=resumed_write;
    sheep_mmu_test_data_pair(resumed_read.data(),resumed_write.data(),va,0x61,0x61,true);pair_bound=true;
    std::memcpy(&ppc_state,&actual,sizeof(actual));exec_flags=0;
    for(unsigned i=wanted;i<n;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);need(exec_flags==0,"canonical repaired continuation plain");ppc_state.pc+=4;}
    SetPRS resumed;std::memcpy(&resumed,&ppc_state,sizeof(resumed));const auto resumed_bytes=resumed_write;resumed_write=expected_write;std::memcpy(&ppc_state,&initial,sizeof(initial));exec_flags=0;
    for(unsigned i=0;i<n;i++){ppc_main_opcode(ppc_opcode_grabber,ops[i]);need(exec_flags==0,"canonical complete reference plain");ppc_state.pc+=4;}
    need(!std::memcmp(&resumed,&ppc_state,sizeof(resumed))&&resumed_bytes==resumed_write,"canonical resume final GPR/LR/RA/RAM");
   }
   unbind();++passed;
  };
  auto input=[&](uint32_t pc){SetPRS c;std::memcpy(&c,&saved,sizeof(c));c.pc=pc;for(unsigned i=0;i<32;i++)c.gpr[i]=0xa5310000u+i*0x1020304u;c.cr=0x59271385u;c.spr[SPR::XER]=0xe0000042u;c.spr[SPR::LR]=0x76543213u;c.spr[SPR::CTR]=0xfedcba98u;return c;};
  const unsigned primaries[]={40,41,42,43,44,45},xos[]={279,311,343,375,407,439},offsets[]={0,2,4094,1,4093,4095},prefixes[]={0,2};
  for(unsigned indexed=0;indexed<2;indexed++)for(unsigned family=0;family<6;family++)for(unsigned offset:offsets){
   auto c=input(0x10000040u);const uint32_t va=0x1234000u;c.gpr[3]=17;c.gpr[7]=va+offset-(indexed?0x120u:0u);c.gpr[6]=0x120;c.gpr[8]=0x89abcdefu;
   const uint32_t mem=indexed?(31u<<26)|(8u<<21)|(7u<<16)|(6u<<11)|(xos[family]<<1):(primaries[family]<<26)|(8u<<21)|(7u<<16);
   const uint32_t ops[]={0x38630001u,mem,0x38630002u,0x38840005u};run(ops,4,c,va,0,0x61,offset==4095?1:4,0,1,false);++forms;
  }
  for(unsigned mode=0;mode<3;mode++)for(unsigned prefix:prefixes)for(unsigned outcome=0;outcome<4;outcome++){
   const uint32_t va=mode==2?0u:mode==1?0x68066000u:0x1234000u;const unsigned offset=outcome==1?1:outcome==3?4094:mode==2?0:2;
   auto c=input(mode==2?0xffffffe4u:0x10000040u);c.gpr[24]=va+offset-2;c.gpr[29]=0x51a02b85u;
   uint32_t ops[7];for(unsigned i=0;i<prefix;i++)ops[i]=0x38630001u;ops[prefix]=0xaf780002u;ops[prefix+1]=0x537d1b78u;ops[prefix+2]=0x7fa803a6u;ops[prefix+3]=0xaf780002u;ops[prefix+4]=0x38630001u;
   const unsigned n=prefix+5,wanted=outcome==2?prefix:outcome==3?prefix+3:n;run(ops,n,c,va,mode,outcome==2?0x62:0x61,wanted,outcome>=2?1:0,2,outcome>=2);++hot;
  }
  for(unsigned which=0;which<2;which++)for(unsigned indexed=0;indexed<2;indexed++)for(unsigned family=0;family<6;family++){
   auto c=input(0x10000040u);const uint32_t va=0x1234000u;c.gpr[7]=va+1-(indexed?0x120u:0u);c.gpr[6]=0x120;c.gpr[8]=0x8123fedcu;
   const uint32_t mem=indexed?(31u<<26)|(8u<<21)|(7u<<16)|(6u<<11)|(xos[family]<<1):(primaries[family]<<26)|(8u<<21)|(7u<<16);const uint32_t ops[]={0x38630001u,mem,0x38630002u,0x38840005u};
   run(ops,4,c,va,0,which?0x41:0x62,which&&family<4?4:1,0,1,false);if(which)++readonly;else++metadata;
  }
  for(unsigned mode=0;mode<3;mode++)for(unsigned store=0;store<2;store++)for(unsigned cross=0;cross<2;cross++){
   const uint32_t va=mode==2?0u:0x1234000u;auto c=input(0x10000040u);c.gpr[24]=va+(cross?4092:0);c.gpr[27]=0x8123fedcu;c.gpr[26]=0x76548001u;
   const unsigned first=store?45:43,second=store?44:42,delta=cross?3:1;
   const uint32_t ops[]={(first<<26)|(27u<<21)|(24u<<16)|1u,(second<<26)|(26u<<21)|(24u<<16)|delta,0x38630001u,0x38840005u};
   run(ops,4,c,va,mode,0x61,cross?1:4,cross?1:0,2,cross);++chains;
  }
  need(forms==72&&hot==24&&metadata==12&&readonly==12&&chains==12&&passed==132,"fixed focused census");
 }catch(...){cleanup();throw;}
 cleanup();fprintf(stderr,"HALFWORD GUARD SELFTEST %u aligned/odd/page/MMIO/permission/LR/prefix/resume/alias/ABI cases passed\n",passed);
#else
 throw std::runtime_error("Q185 halfword guard fixture requires AArch64");
#endif
}
