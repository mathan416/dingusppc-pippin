// SPDX-License-Identifier: GPL-3.0-or-later
// Q219 startup-only packed-product oracle. No global CPU/MMU or live device I/O.
#include <array>
#include <memory>
#include <cstring>
#include <stdexcept>
#if defined(__aarch64__)
extern "C" uint64_t sheep_jit_abi_check(nw_jit_fn,nw_jit_cpu*);
int nw_jit_island_pack_selftest(){
 const bool previous=island_pack_test_mode(true);const auto*layout0=g_emit_layout;
 unsigned passed=0,resumes=0,boundaries=0,lifetime=0,budget=0;
 auto need=[&](bool okay,const char*why){if(!okay){fprintf(stderr,"ISLAND PACK failure case=%u %s\n",passed+1,why);throw std::runtime_error("Q219 island packing oracle failed");}};
 struct Slot{uint32_t tag;uint16_t flags,lru;int64_t read,write;uint32_t physical,reserved;};static_assert(sizeof(Slot)==32,"Existing mapped DTLB32 contract");
 auto table=std::make_unique<std::array<Slot,4096>>();alignas(4096)std::array<uint8_t,8192>read{},write{},original_read{},original_write{};
 constexpr uint32_t data_page=0x12340000u,root=0x68014000u;uintptr_t read_slot=0,write_slot=0,dtlb=reinterpret_cast<uintptr_t>(table->data());uint32_t mask=4095;uint8_t bail=0;
 nw_jit_layout layout={};layout.gpr=offsetof(nw_jit_cpu,gpr);layout.cr=offsetof(nw_jit_cpu,cr);layout.xer=offsetof(nw_jit_cpu,xer);layout.lr=offsetof(nw_jit_cpu,lr);layout.ctr=offsetof(nw_jit_cpu,ctr);layout.pc=offsetof(nw_jit_cpu,pc);layout.memory=read.data();layout.leaf=1;layout.tlb_index_bits=12;layout.dtlb_slot=uintptr_t(&dtlb);layout.tlb_mask=uintptr_t(&mask);layout.memory_bail=uintptr_t(&bail);layout.fast_read_slot=uintptr_t(&read_slot);layout.fast_write_slot=uintptr_t(&write_slot);
 auto seed=[&](unsigned serial){for(unsigned i=0;i<read.size();i++){read[i]=uint8_t(i*37u+serial*13u+0x81u);write[i]=uint8_t(i*19u+serial*7u+0x65u);}original_read=read;original_write=write;for(auto&e:*table){e={};e.tag=0xffffffffu;}for(unsigned page=0;page<2;page++){auto&e=(*table)[((data_page>>12)+page)&mask];e.tag=data_page+page*4096;e.flags=0x61;e.read=int64_t(uintptr_t(read.data()+page*4096))-e.tag;e.write=int64_t(uintptr_t(write.data()+page*4096))-e.tag;}read_slot=uintptr_t(read.data())-data_page;write_slot=uintptr_t(write.data())-data_page;bail=0;};
 auto input=[&](unsigned serial){nw_jit_cpu cpu={};for(unsigned i=0;i<32;i++)cpu.gpr[i]=0x913579bdu^(i*0x1020301u+serial*37u);cpu.gpr[6]=data_page+128;cpu.gpr[9]=~uint32_t(uintptr_t(read.data()+129));cpu.cr=0xa65a9187u;cpu.xer=0xe0000042u;cpu.lr=0x76543213u;cpu.ctr=0x31415920u;cpu.pc=root;cpu.dec=0xa5a55a5au;cpu.mem=read.data();cpu.mem_base=data_page;cpu.mem_size=uint32_t(read.size());return cpu;};
 auto delta=[](uint32_t word,unsigned shift,unsigned bits){const uint32_t raw=(word>>shift)&((uint32_t(1)<<bits)-1);return(raw&(uint32_t(1)<<(bits-1)))?int64_t(raw)-(int64_t(1)<<bits):int64_t(raw);};
 auto inspect=[&](const IslandPackExtent&e,nw_jit_fn fn){
  const uintptr_t arena=uintptr_t(g_code),base=arena+size_t(e.island)*NW_JIT_ISLAND_SIZE;need(e.committed&&e.packed&&e.hot_begin==reinterpret_cast<uint32_t*>(fn)&&e.generation==nw_jit_wrap_count()&&e.bank==e.island/(NW_JIT_BANK_SIZE/NW_JIT_ISLAND_SIZE),"actual producer identity/island/bank/current generation");
  need(e.hot_begin<=e.hot_code_end&&e.hot_code_end<=e.hot_end&&e.cold_begin<=e.cold_code_begin&&e.cold_code_begin<=e.cold_code_end&&e.cold_code_end<=e.pool_begin&&e.pool_begin<=e.cold_end&&e.cold_padding<=4&&e.cold_code_begin==e.cold_begin+e.cold_padding/4&&e.actual_bytes==size_t(e.hot_end-e.hot_begin+e.cold_end-e.cold_begin)*4&&e.actual_bytes<=NW_JIT_MAX_EMIT&&uintptr_t(e.hot_begin)>=base+NW_JIT_ISLAND_HEADER&&uintptr_t(e.hot_end)<=base+ISLAND_PACK_HALF&&uintptr_t(e.cold_begin)>=base+ISLAND_PACK_HALF&&uintptr_t(e.cold_end)<=base+NW_JIT_ISLAND_SIZE&&!(uintptr_t(e.pool_begin)&7),"exact two ranges, padding charged once, alignedpool and8192sum");
  need(e.hot_code_end>e.hot_begin&&e.hot_code_end[-1]==0xd65f03c0u&&size_t(e.hot_end-e.hot_code_end)==e.veneer_count,"veneers follow LAST success RET and neverfallthrough");
  if(e.cold_padding)need(e.cold_begin[0]==0xd503201fu&&!island_pack_contains_code(e,uintptr_t(e.cold_begin)),"leading cold NOPpadding is not instruction range");
  auto scan=[&](const uint32_t*begin,const uint32_t*end){for(auto*at=begin;at<end;at++){const uint32_t w=*at;unsigned shift=0,bits=0;bool literal=false;
   need((w&0xfc000000u)!=0x94000000u&&(w&0xfffffc1fu)!=0xd63f0000u,"leaf ranges contain no BL/BLR");
   if((w&0xfc000000u)==0x14000000u)bits=26;else if((w&0xff000010u)==0x54000000u||(w&0x7e000000u)==0x34000000u){shift=5;bits=19;}else if((w&0x7e000000u)==0x36000000u){shift=5;bits=14;}else if((w&0xff000000u)==0x58000000u){shift=5;bits=19;literal=true;}
   if(!bits)continue;const uintptr_t target=uintptr_t(int64_t(uintptr_t(at))+4*delta(w,shift,bits));
   if(literal)need(!(target&7)&&((target>=uintptr_t(e.pool_begin)&&target+8<=uintptr_t(e.cold_end))||target==base||target==base+8),"actual LDR19 exactownedrawpool or immutableheader target");
   else need(island_pack_contains_code(e,target),"all B26/Bcond19/CB19/TB14 targets actualcode, neverpool/gap");
   if(at<e.hot_code_end&&(w&0xfff8001fu)==0x37000008u){uintptr_t far=target;
    if(target>=uintptr_t(e.hot_code_end)&&target<uintptr_t(e.hot_end)){const uint32_t jump=*reinterpret_cast<const uint32_t*>(target);far=uintptr_t(int64_t(target)+4*delta(jump,0,26));need((jump&0xfc000000u)==0x14000000u,"nearveneer is B26");}
    else need(target>=uintptr_t(e.cold_code_begin)&&target<uintptr_t(e.cold_code_end)&&int64_t(target)-int64_t(uintptr_t(at))>=-32768&&int64_t(target)-int64_t(uintptr_t(at))<32768,"nearby actualcold entry may be reached directly by TB14");
    unsigned payload=0;while(payload<e.fault_count&&e.faults[payload].pc<=uintptr_t(at))++payload;need(payload<e.fault_count&&far==e.faults[payload].slow,"TB14 direct ornearveneer reaches thisaccess registeredsoftware entry");}
  }};scan(e.hot_begin,e.hot_end);scan(e.cold_code_begin,e.cold_code_end);
  for(unsigned i=0;i<e.fault_count;i++){const auto&f=e.faults[i];need(f.pc>=uintptr_t(e.hot_begin)&&f.pc<uintptr_t(e.hot_code_end)&&f.slow>=uintptr_t(e.cold_code_begin)&&f.slow<uintptr_t(e.cold_code_end)&&(!i||f.pc>e.faults[i-1].pc)&&(f.pc-arena)/NW_JIT_BANK_SIZE==(f.slow-arena)/NW_JIT_BANK_SIZE,"registeredfault PC/slow pair samebank/samefunction/sortedhot");}
  for(auto*at=e.hot_code_end;at<e.hot_end;at++){need((*at&0xfc000000u)==0x14000000u,"veneer is one B26");const auto target=uintptr_t(int64_t(uintptr_t(at))+4*delta(*at,0,26));need(target>=uintptr_t(e.cold_code_begin)&&target<uintptr_t(e.cold_code_end),"everyveneer exactcoldcode target");}
  need(!island_pack_contains_code(e,base+ISLAND_PACK_HALF/2)&&!island_pack_contains_code(e,uintptr_t(e.pool_begin)),"gap/pool excluded bycodeextent contract");
 };
 auto compile=[&](const uint32_t*ops,unsigned n,uint32_t pc){auto fn=nw_jit_compile_mapped(ops,int(n),pc,&layout);need(fn!=nullptr,"actual supportedmapped product compile");return fn;};
 auto run=[&](nw_jit_fn fn,const nw_jit_cpu&initial,const nw_jit_cpu&expected,const std::array<uint8_t,8192>&expected_write,unsigned n){auto cpu=initial;bail=0;const auto before=pippin_fastmem_fault_count();const auto result=sheep_jit_abi_check(fn,&cpu);need(result==n&&!bail&&!std::memcmp(&cpu,&expected,sizeof(cpu))&&read==original_read&&write==expected_write&&pippin_fastmem_fault_count()==before,"fullCPU/count/ABI/RAM/canaries with no pretendedsignal");};
 auto patch=[&](uintptr_t pc,uint32_t word){
#ifdef __APPLE__
  pthread_jit_write_protect_np(0);
#endif
  *reinterpret_cast<uint32_t*>(pc)=word;
#ifdef __APPLE__
  pthread_jit_write_protect_np(1);sys_icache_invalidate(reinterpret_cast<void*>(pc),4);
#endif
  __builtin___clear_cache(reinterpret_cast<char*>(pc),reinterpret_cast<char*>(pc+4));
 };
 const uint32_t simple[]={uint32_t(40u<<26|4u<<21|6u<<16),nw_ppc_addi(12,4,5),nw_ppc_addi(13,12,-7),0x48000004u};
 auto simple_run=[&](nw_jit_fn fn,unsigned serial){seed(serial);auto a=input(serial),z=a;z.gpr[4]=uint32_t(read[128])<<8|read[129];z.gpr[12]=z.gpr[4]+5;z.gpr[13]=z.gpr[12]-7;z.pc+=16;run(fn,a,z,original_write,4);};
 auto same_cursor=[&](const IslandPackCursorState&a,const IslandPackCursorState&b){return a.island==b.island&&a.hot==b.hot&&a.cold==b.cold&&a.highwater==b.highwater&&a.wraps==b.wraps&&a.emitted==b.emitted&&a.packed_compiles==b.packed_compiles&&a.hot_bytes==b.hot_bytes&&a.cold_bytes==b.cold_bytes&&a.veneers==b.veneers&&a.live==b.live;};
 try{
  // The ordinary hot path may keep W9=GPR9. A cold software SUCCESS clobbers
  // X9 then rejoins before ADDI's read; the incoming resume must kill thatfact.
  for(unsigned store=0;store<2;store++)for(unsigned forced_patch=0;forced_patch<2;forced_patch++)for(unsigned serial=0;serial<6;serial++){
   const unsigned displacement=forced_patch?2:1;const uint32_t primary=store?44:40;const uint32_t ops[]={primary<<26|4u<<21|6u<<16,nw_ppc_rlwimi(8,9,8,0,23),primary<<26|10u<<21|6u<<16|displacement,nw_ppc_addi(12,9,5),nw_ppc_addi(13,8,-7),0x48000004u};
   for(unsigned packed=0;packed<2;packed++){island_pack_test_mode(packed!=0);seed(serial+store*13);auto a=input(serial),z=a;auto expected_write=original_write;const uint32_t rotated=(a.gpr[9]<<8)|(a.gpr[9]>>24);z.gpr[8]=(a.gpr[8]&0xffu)|(rotated&0xffffff00u);z.gpr[12]=a.gpr[9]+5;z.gpr[13]=z.gpr[8]-7;z.pc+=24;
    if(store){expected_write[128]=uint8_t(a.gpr[4]>>8);expected_write[129]=uint8_t(a.gpr[4]);expected_write[128+displacement]=uint8_t(a.gpr[10]>>8);expected_write[129+displacement]=uint8_t(a.gpr[10]);}else{z.gpr[4]=uint32_t(read[128])<<8|read[129];z.gpr[10]=uint32_t(read[128+displacement])<<8|read[129+displacement];}
    auto fn=compile(ops,6,a.pc);const auto e=island_pack_last_extent();if(packed)inspect(e,fn);need(e.fault_count==2&&e.faults[1].write==bool(store),"two actualfaultrecords sourcewritekind");
    uint32_t saved=0;if(forced_patch){const auto&f=e.faults[1];saved=*reinterpret_cast<uint32_t*>(f.pc);const int64_t d=(int64_t(f.slow)-int64_t(f.pc))/4;need(d>=-(int64_t(1)<<25)&&d<(int64_t(1)<<25),"simulatedregisteredpayload branch range");patch(f.pc,a64_b(int(d)));}
    try{run(fn,a,z,expected_write,6);}catch(...){if(forced_patch)patch(e.faults[1].pc,saved);throw;}if(forced_patch)patch(e.faults[1].pc,saved);
   }++passed;++resumes;
  }
  // Actual source/emission reaches both ends of each half. Exhaustion of
  // either reservation advances the complete paired island, never justonehalf.
  for(unsigned kind=0;kind<8;kind++){island_pack_test_mode(true);seed(kind);const size_t last_hot=ISLAND_PACK_HALF-NW_JIT_MAX_EMIT,last_cold=NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT-4;const size_t hot=kind&1?last_hot:NW_JIT_ISLAND_HEADER,cold=kind&2?last_cold:ISLAND_PACK_HALF;size_t island=kind==7?NW_JIT_ISLANDS-1:0;
   const size_t chosen_hot=kind==4||kind==6||kind==7?last_hot+4:hot,chosen_cold=kind==5||kind==6||kind==7?last_cold+4:cold;need(island_pack_test_seek(island,chosen_hot,chosen_cold),"atomic matched two-cursor boundary seek");auto fn=compile(simple,4,root);const auto e=island_pack_last_extent();inspect(e,fn);const bool advance=kind>=4;need(e.island==(advance?(island+1)%NW_JIT_ISLANDS:island),"production eitherhalf fullreserve/lastfit/advance/wrap");simple_run(fn,kind);++passed;++boundaries;
  }
  island_pack_test_mode(true);seed(0);auto first=compile(simple,4,root);const auto first_extent=island_pack_last_extent();inspect(first_extent,first);nw_jit_cache_put(0x1000,root,0,0,first,4);
  // Stable earlier cold data cannot be overwritten when the next main packs.
  seed(1);auto second=compile(simple,4,root);inspect(island_pack_last_extent(),second);simple_run(first,1);need(nw_jit_cache_get(0x1000,root,0,0,nullptr)==first,"earlier function/cache/cold after sameisland compile");++passed;++lifetime;
  const uintptr_t poison=uintptr_t(g_code)+ISLAND_PACK_HALF/2;uint32_t saved_gap[4];memcpy(saved_gap,reinterpret_cast<void*>(poison),sizeof(saved_gap));const uint32_t forbidden[]={0x94000000u,0xd63f0120u,0xaa1303e0u,0xd65f03c0u};
  for(unsigned i=0;i<4;i++)patch(poison+4*i,forbidden[i]);try{seed(2);auto fn=compile(simple,4,root);const auto e=island_pack_last_extent();inspect(e,fn);simple_run(fn,2);need(!memcmp(forbidden,reinterpret_cast<void*>(poison),sizeof(forbidden)),"unallocatedgap call-looking poison neitherwritten norleafscanned");}catch(...){for(unsigned i=0;i<4;i++)patch(poison+4*i,saved_gap[i]);throw;}for(unsigned i=0;i<4;i++)patch(poison+4*i,saved_gap[i]);++passed;++lifetime;
  {const auto state=island_pack_cursor_state();need(island_pack_test_seek(state.island,ISLAND_PACK_HALF-NW_JIT_MAX_EMIT+4,state.cold),"hot exhaustion forwardseek");seed(3);auto fn=compile(simple,4,root);inspect(island_pack_last_extent(),fn);simple_run(first,3);need(nw_jit_cache_get(0x1000,root,0,0,nullptr)==first,"hot advance preserves earlier island cold/cache");++passed;++lifetime;}
  {const auto state=island_pack_cursor_state();need(island_pack_test_seek(state.island,state.hot,NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT),"cold exhaustion forwardseek");seed(4);auto fn=compile(simple,4,root);inspect(island_pack_last_extent(),fn);simple_run(first,4);need(nw_jit_cache_get(0x1000,root,0,0,nullptr)==first,"cold advance preserves earlier island cold/cache");++passed;++lifetime;}
  const size_t bank_island=NW_JIT_BANK_SIZE/NW_JIT_ISLAND_SIZE;need(island_pack_test_seek(bank_island,NW_JIT_ISLAND_HEADER,ISLAND_PACK_HALF),"enter bankone atomically");seed(5);auto bank_one=compile(simple,4,root);const auto bank_extent=island_pack_last_extent();inspect(bank_extent,bank_one);nw_jit_cache_put(0x2000,root+0x1000,0,0,bank_one,4);simple_run(first,5);simple_run(bank_one,5);need(nw_jit_cache_get(0x1000,root,0,0,nullptr)==first,"only destination bank invalidated");++passed;++lifetime;
  need(island_pack_test_seek(NW_JIT_ISLANDS-1,ISLAND_PACK_HALF-NW_JIT_MAX_EMIT+4,NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT),"fullwrap matchedcursors");seed(6);auto wrapped=compile(simple,4,root);const auto wrapped_extent=island_pack_last_extent();inspect(wrapped_extent,wrapped);need(wrapped_extent.bank==0&&wrapped_extent.generation>first_extent.generation&&!nw_jit_cache_get(0x1000,root,0,0,nullptr)&&nw_jit_cache_get(0x2000,root+0x1000,0,0,nullptr)==bank_one,"rewrap cannot restore stale bankzero capability");simple_run(bank_one,6);simple_run(wrapped,6);++passed;++lifetime;
  nw_jit_cache_put(0x3000,root+0x2000,0,0,wrapped,4);need(island_pack_test_seek(bank_island,NW_JIT_ISLAND_HEADER,ISLAND_PACK_HALF),"recycle bankone sameprivatecontrol");seed(7);auto recycled=compile(simple,4,root);inspect(island_pack_last_extent(),recycled);need(!nw_jit_cache_get(0x2000,root+0x1000,0,0,nullptr)&&nw_jit_cache_get(0x3000,root+0x2000,0,0,nullptr)==wrapped,"bankone reuse drops only its retired functions/cold");simple_run(wrapped,7);simple_run(recycled,7);++passed;++lifetime;
  nw_jit_reset();need(!island_pack_cursor_state().live&&!island_pack_cursor_state().highwater&&!island_pack_last_extent().committed,"reset clears pairedallocator/producer/capturedhighwater");for(const auto&pool:g_alias_literals)need(!pool.ready,"reset all islandheaders");seed(8);auto reset=compile(simple,4,root);inspect(island_pack_last_extent(),reset);simple_run(reset,8);++passed;++lifetime;
  for(unsigned kind=0;kind<3;kind++){island_pack_test_mode(true);seed(kind+31);uint32_t ops[32];for(unsigned i=0;i<32;i++)ops[i]=uint32_t((kind==0?40u:kind==1?44u:36u)<<26|4u<<21|6u<<16|(kind==2?4*i:2*i));auto a=input(kind),z=a;auto expected_write=original_write;z.pc+=128;
   if(!kind){z.gpr[4]=uint32_t(read[190])<<8|read[191];}else for(unsigned i=0;i<32;i++){const unsigned at=128+(kind==2?4*i:2*i),size=kind==2?4:2;for(unsigned b=0;b<size;b++)expected_write[at+b]=uint8_t(a.gpr[4]>>((size-1-b)*8));}
   auto fn=compile(ops,32,a.pc);const auto e=island_pack_last_extent();inspect(e,fn);need(e.fault_count==32&&e.actual_bytes<=8192&&(kind==2||e.veneer_count==32),"32memory product sum/count andallhalfword nearveneers");run(fn,a,z,expected_write,32);++passed;++budget;
  }
  {island_pack_test_mode(true);seed(99);auto live=compile(simple,4,root);const auto extent=island_pack_last_extent();need(island_pack_test_seek(1,NW_JIT_ISLAND_HEADER,ISLAND_PACK_HALF)&&!g_alias_literals[1].ready,"failedproduct uses fresh samebank header, preserving priorlive product");const auto before=island_pack_cursor_state();const auto aliases=g_alias_literals[1];uint32_t oversized[64];for(auto&op:oversized)op=uint32_t(40u<<26|4u<<21|6u<<16);uint64_t header[8];memcpy(header,g_code+NW_JIT_ISLAND_SIZE,sizeof(header));need(!nw_jit_compile_mapped(oversized,64,root,&layout),"validmapped firstop writesaliasheader before oversizedslowcountdecline");const auto after=island_pack_cursor_state();need(same_cursor(before,after)&&!memcmp(header,g_code+NW_JIT_ISLAND_SIZE,sizeof(header))&&g_alias_literals[1].ready==aliases.ready&&g_alias_literals[1].read_slot==aliases.read_slot&&g_alias_literals[1].write_slot==aliases.write_slot,"failedcompile rolls back initializedheader/ready/cursors/statbytes withoutpublication");simple_run(live,99);need(island_pack_last_extent().hot_begin==extent.hot_begin&&island_pack_last_extent().committed,"failedcompile leaves previous successfulextent physical lifetime");++passed;++budget;}
  need(passed==44&&resumes==24&&boundaries==8&&lifetime==8&&budget==4,"fixed meaningful packing census");
 }catch(...){g_emit_layout=layout0;island_pack_test_mode(previous);throw;}
 g_emit_layout=layout0;island_pack_test_mode(previous);fprintf(stderr,"ISLAND PACK SELFTEST %u resumes=%u boundaries=%u lifetime=%u budget=%u fullCPU/RAM/ABI/ranges/veneers/faultpairs passed\n",passed,resumes,boundaries,lifetime,budget);return 1;
}
#else
int nw_jit_island_pack_selftest(){return 0;}
#endif
