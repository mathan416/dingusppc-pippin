// Project: DingusPPC-Pippin
// File: cpu/ppc/shared-alias-literals.inc
// Purpose: Verify-mode tests for literals shared between memory aliases.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// PRIVATE VERIFY FIXTURE: include in nw_jit.cpp immediately before its final
// #else (the !__aarch64__ stubs). Declare/call the function from the bridge's
// verify setup, then reset_native_and_rom() before other fixtures or gameplay.
// It deliberately recycles code banks and resets the backend; no external
// dispatch/cache pointer may be used across this fixture.
static bool island_pack_alias_profile_selftest(bool packed)
{
 unsigned checks=0,packed_checks=0;
 IslandPackExtent extent{};
 auto structure=[&](bool okay,const char*why){++packed_checks;if(!okay)fprintf(stderr,"ISLAND PACK ALIAS failure check=%u: %s\n",packed_checks,why);return okay;};
 auto require=[&](bool okay,const char *what){
  checks++;
  if(!okay)fprintf(stderr,"SHARED ALIAS LITERALS failure at check %u: %s\n",checks,what);
  return okay;
 };
 nw_jit_reset();
 const bool okay=[&](){
  if(!require(code_ready()!=0,"code arena allocation"))return false;
  uint32_t word=0;
  constexpr uintptr_t anchor=0x2000000;
  auto target=[](uintptr_t instruction,uint32_t op){
   const uint32_t raw=(op>>5)&0x7ffffu;
   const int32_t delta=(raw&0x40000u)?int32_t(raw)-0x80000:int32_t(raw);
   return instruction+uintptr_t(int64_t(delta)*4);
  };
  const int64_t valid_distances[]={-(int64_t(1)<<20),-int64_t(4),int64_t(0),int64_t(4),(int64_t(1)<<20)-4};
  const int64_t invalid_distances[]={-(int64_t(1)<<20)-4,(int64_t(1)<<20),int64_t(2)};
  for(int64_t distance : valid_distances){
   if(!require(alias_literal_instruction(anchor,anchor+uintptr_t(distance),word)&&
      (word&0xff00001fu)==0x58000009u&&target(anchor,word)==anchor+uintptr_t(distance),
      "signed LDR-literal encoding boundary"))return false;
  }
  for(int64_t distance : invalid_distances)
   if(!require(!alias_literal_instruction(anchor,anchor+uintptr_t(distance),word),"out-of-range/unaligned literal rejected"))return false;
  for(size_t island=0;island<NW_JIT_ISLANDS;island++){
   const size_t base=island*NW_JIT_ISLAND_SIZE;
   if(!require(code_reservation_start(base)==base+NW_JIT_ISLAND_HEADER&&
      code_reservation_start(base+NW_JIT_ISLAND_HEADER-4)==base+NW_JIT_ISLAND_HEADER&&
      code_reservation_start(base+NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT)==base+NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT&&
      code_reservation_start(base+NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT+4)==base+NW_JIT_ISLAND_SIZE+NW_JIT_ISLAND_HEADER,
      "all island reservation/header/last-fit boundaries"))return false;
   if(!require(alias_literal_instruction(anchor+NW_JIT_ISLAND_HEADER,anchor,word)&&target(anchor+NW_JIT_ISLAND_HEADER,word)==anchor&&
      alias_literal_instruction(anchor+NW_JIT_ISLAND_HEADER,anchor+8,word)&&target(anchor+NW_JIT_ISLAND_HEADER,word)==anchor+8&&
      alias_literal_instruction(anchor+NW_JIT_ISLAND_SIZE-4,anchor,word)&&target(anchor+NW_JIT_ISLAND_SIZE-4,word)==anchor&&
      alias_literal_instruction(anchor+NW_JIT_ISLAND_SIZE-4,anchor+8,word)&&target(anchor+NW_JIT_ISLAND_SIZE-4,word)==anchor+8,
      "first/last instruction can reach both header literals"))return false;
  }

  alignas(16) uint8_t read_a[32]={},write_a[32]={},read_b[32]={},write_b[32]={};
  read_a[8]=0x12;read_a[9]=0x34;read_a[10]=0x56;read_a[11]=0x78;
  read_b[8]=0x89;read_b[9]=0xab;read_b[10]=0xcd;read_b[11]=0xef;
  uintptr_t read_slot=reinterpret_cast<uintptr_t>(read_a),write_slot=reinterpret_cast<uintptr_t>(write_a);
  uintptr_t read_slot_b=reinterpret_cast<uintptr_t>(read_b),write_slot_b=reinterpret_cast<uintptr_t>(write_b);
  uintptr_t dummy_dtlb=0;uint32_t dummy_mask=1;uint8_t bail=0;
  nw_jit_layout layout={};
  layout.gpr=offsetof(nw_jit_cpu,gpr);layout.cr=offsetof(nw_jit_cpu,cr);layout.xer=offsetof(nw_jit_cpu,xer);
  layout.lr=offsetof(nw_jit_cpu,lr);layout.ctr=offsetof(nw_jit_cpu,ctr);layout.pc=offsetof(nw_jit_cpu,pc);
  layout.memory=read_a;layout.leaf=1;layout.tlb_index_bits=1;
  layout.dtlb_slot=reinterpret_cast<uintptr_t>(&dummy_dtlb);layout.tlb_mask=reinterpret_cast<uintptr_t>(&dummy_mask);
  layout.memory_bail=reinterpret_cast<uintptr_t>(&bail);
  layout.fast_read_slot=reinterpret_cast<uintptr_t>(&read_slot);layout.fast_write_slot=reinterpret_cast<uintptr_t>(&write_slot);
  auto other=layout;other.fast_read_slot=reinterpret_cast<uintptr_t>(&read_slot_b);other.fast_write_slot=reinterpret_cast<uintptr_t>(&write_slot_b);
  constexpr uint32_t root=0x10000000;
  const uint32_t ops[]={0x81070000u,0x91070000u,0x38630001u,0x48000004u}; // LWZ/STW, ADDI, counted B-next.
  auto run=[&](nw_jit_fn fn,uint32_t expected,uint8_t *destination){
   nw_jit_cpu cpu={};cpu.gpr[7]=8;cpu.pc=root;bail=0;
   const uint32_t n=reinterpret_cast<nw_jit_mapped_fn>(fn)(&cpu);
   return n==4&&!bail&&cpu.pc==root+16&&cpu.gpr[3]==1&&cpu.gpr[8]==expected&&
    destination[8]==uint8_t(expected>>24)&&destination[9]==uint8_t(expected>>16)&&
    destination[10]==uint8_t(expected>>8)&&destination[11]==uint8_t(expected);
  };
  auto header_is=[&](size_t island,const nw_jit_layout &expected){
   uint64_t pair[2];memcpy(pair,g_code+island*NW_JIT_ISLAND_SIZE,sizeof(pair));
   const auto &pool=g_alias_literals[island];
   return pool.ready&&pool.read_slot==expected.fast_read_slot&&pool.write_slot==expected.fast_write_slot&&
    pair[0]==expected.fast_read_slot&&pair[1]==expected.fast_write_slot;
  };
  auto compile=[&](const uint32_t*program,int length,uint32_t pc,const nw_jit_layout*shape){
   auto fn=nw_jit_compile_mapped(program,length,pc,shape);
   if(packed&&fn){extent=island_pack_last_extent();const auto arena=reinterpret_cast<uintptr_t>(g_code),hot=reinterpret_cast<uintptr_t>(extent.hot_begin),cold=reinterpret_cast<uintptr_t>(extent.cold_begin),finish=reinterpret_cast<uintptr_t>(extent.cold_end);
    if(!structure(extent.committed&&extent.packed&&extent.hot_begin==reinterpret_cast<uint32_t*>(fn)&&extent.hot_begin<=extent.hot_code_end&&extent.hot_code_end<=extent.hot_end&&extent.cold_begin<=extent.cold_code_begin&&extent.cold_code_begin<=extent.cold_code_end&&extent.cold_code_end<=extent.pool_begin&&extent.pool_begin<=extent.cold_end&&!(reinterpret_cast<uintptr_t>(extent.pool_begin)&7)&&extent.cold_padding<=4&&extent.cold_code_begin==extent.cold_begin+extent.cold_padding/4&&extent.actual_bytes==size_t(extent.hot_end-extent.hot_begin+extent.cold_end-extent.cold_begin)*4&&extent.actual_bytes<=NW_JIT_MAX_EMIT&&hot>=arena+extent.island*NW_JIT_ISLAND_SIZE+NW_JIT_ISLAND_HEADER&&hot<arena+extent.island*NW_JIT_ISLAND_SIZE+NW_JIT_ISLAND_SIZE/2&&cold>=arena+extent.island*NW_JIT_ISLAND_SIZE+NW_JIT_ISLAND_SIZE/2&&finish<=arena+(extent.island+1)*NW_JIT_ISLAND_SIZE,"exact main/cold/pool extents and actualsum"))return nw_jit_fn(nullptr);
   }return fn;
  };
  auto refs=[&](nw_jit_fn fn,size_t end,const nw_jit_layout &expected,bool shared){
   const size_t start=size_t(reinterpret_cast<uint8_t *>(fn)-g_code),island=start/NW_JIT_ISLAND_SIZE;
   if(!packed&&(start%NW_JIT_ISLAND_SIZE<NW_JIT_ISLAND_HEADER||end>start+NW_JIT_MAX_EMIT||
      (end-1)/NW_JIT_ISLAND_SIZE!=island))return false;
   if(packed&&(!extent.committed||extent.hot_begin!=reinterpret_cast<uint32_t*>(fn)||extent.island!=island))return false;
   unsigned found[2]={};
   auto scan=[&](size_t begin,size_t finish){for(size_t off=begin;off+4<=finish;off+=4){
    uint32_t op;memcpy(&op,g_code+off,sizeof(op));
    if((op&0xff00001fu)!=0x58000009u)continue;
    const uintptr_t address=target(reinterpret_cast<uintptr_t>(g_code+off),op);
    const uintptr_t arena=reinterpret_cast<uintptr_t>(g_code);
    if(address<arena||address>arena+NW_JIT_CODE_SIZE-8)continue;
    uint64_t value;memcpy(&value,reinterpret_cast<const void *>(address),sizeof(value));
    for(unsigned i=0;i<2;i++)if(value==(i?expected.fast_write_slot:expected.fast_read_slot)){
     const uintptr_t pool=arena+island*NW_JIT_ISLAND_SIZE+i*8;
     const bool local=packed?(address>=reinterpret_cast<uintptr_t>(extent.pool_begin)&&address+8<=reinterpret_cast<uintptr_t>(extent.cold_end)):(address>=arena+start&&address+8<=arena+end);
     if(shared?address!=pool:!local)return false;
     found[i]++;
    }
   }return true;};
   if(packed){if(!scan(size_t(reinterpret_cast<uint8_t*>(extent.hot_begin)-g_code),size_t(reinterpret_cast<uint8_t*>(extent.hot_end)-g_code))||!scan(size_t(reinterpret_cast<uint8_t*>(extent.cold_code_begin)-g_code),size_t(reinterpret_cast<uint8_t*>(extent.cold_code_end)-g_code)))return false;}
   else if(!scan(start,end))return false;
   return found[0]==1&&found[1]==1;
  };
  auto first=compile(ops,4,root,&layout);
  if(!require(first&&reinterpret_cast<uint8_t *>(first)==g_code+NW_JIT_ISLAND_HEADER&&
     refs(first,g_code_used,layout,true)&&header_is(0,layout)&&run(first,0x12345678u,write_a),"first shared pair/function and current value"))return false;
  read_slot=reinterpret_cast<uintptr_t>(read_b);write_slot=reinterpret_cast<uintptr_t>(write_b);
  if(!require(run(first,0x89abcdefu,write_b)&&header_is(0,layout),"same slot addresses read new base VALUES on the next call"))return false;
  auto same=compile(ops,4,root,&layout);
  if(!require(same&&refs(same,g_code_used,layout,true)&&header_is(0,layout),"second function shares exact immutable pair"))return false;
  auto mismatch=compile(ops,4,root,&other);
  if(!require(mismatch&&refs(mismatch,g_code_used,other,false)&&header_is(0,layout)&&run(mismatch,0x89abcdefu,write_b),"pair mismatch uses local literals without pool mutation"))return false;
  auto half=layout;half.fast_write_slot=other.fast_write_slot;
  auto partial=compile(ops,4,root,&half);
  if(!require(partial&&refs(partial,g_code_used,half,false)&&header_is(0,layout),"one changed address falls back for both aliases"))return false;

  if(packed){if(!structure(island_pack_test_seek(0,NW_JIT_ISLAND_SIZE/2-NW_JIT_MAX_EMIT,NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT-4),"packed last simultaneous full reservations"))return false;}else g_code_used=NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT;
  auto last=compile(ops,4,root,&layout);
  if(!require(last&&reinterpret_cast<uint8_t *>(last)==g_code+(packed?NW_JIT_ISLAND_SIZE/2:NW_JIT_ISLAND_SIZE)-NW_JIT_MAX_EMIT&&
     refs(last,g_code_used,layout,true)&&run(last,0x89abcdefu,write_b),"last full reservation stays in old island with backward literals"))return false;
  if(packed){const auto state=island_pack_cursor_state();if(!structure(island_pack_test_seek(state.island,state.hot+4,state.cold),"packed forward-only overrun seek preserves earlier products"))return false;}else g_code_used=NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT+4;
  auto next=compile(ops,4,root,&other);
  if(!require(next&&reinterpret_cast<uint8_t *>(next)==g_code+NW_JIT_ISLAND_SIZE+NW_JIT_ISLAND_HEADER&&
     refs(next,g_code_used,other,true)&&header_is(0,layout)&&header_is(1,other),"island advance skips header and preserves earlier pair"))return false;

  // Failed oversized nonleaf emission must not reach the following header.
  const size_t next_header=(packed?2:1)*NW_JIT_ISLAND_SIZE;uint64_t saved_next[2];memcpy(saved_next,g_code+next_header,sizeof(saved_next));
  uint32_t oversized[1024];for(auto &op:oversized)op=0x38630001u;
  if(packed){const auto state=island_pack_cursor_state();if(!structure(island_pack_test_seek(state.island,NW_JIT_ISLAND_SIZE/2-NW_JIT_MAX_EMIT,NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT-4),"packed oversize probe is forward-unused in current island"))return false;}else g_code_used=NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT;
  const auto before_failed=island_pack_cursor_state();const size_t old_used=g_code_used;
  auto too_big=nw_jit_compile_uncached(oversized,1024,root);
  if(packed){const auto after=island_pack_cursor_state();if(!structure(after.island==before_failed.island&&after.hot==before_failed.hot&&after.cold==before_failed.cold&&after.highwater==before_failed.highwater&&after.wraps==before_failed.wraps&&after.emitted==before_failed.emitted,"failed emission commits no fragment/cursor/highwater/epoch"))return false;}
  if(!require(!too_big&&g_code_used==(packed?old_used:NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT)&&
     !memcmp(saved_next,g_code+next_header,sizeof(saved_next))&&header_is(1,other),"nonleaf oversize fallback leaves following header intact"))return false;

  // Cache entries in the unaffected bank and its immutable pool must survive.
  nw_jit_cache_put(0x1000,root,0,0,first,4);
  if(packed){if(!structure(island_pack_test_seek(NW_JIT_BANK_SIZE/NW_JIT_ISLAND_SIZE,NW_JIT_ISLAND_HEADER,NW_JIT_ISLAND_SIZE/2),"packed exact destination bank entry"))return false;}else g_code_used=NW_JIT_BANK_SIZE; // Exact-bank-boundary case, not just near-boundary.
  auto bank_one=compile(ops,4,root,&other);
  const size_t bank_island=NW_JIT_BANK_SIZE/NW_JIT_ISLAND_SIZE;
  if(!require(bank_one&&reinterpret_cast<uint8_t *>(bank_one)==g_code+NW_JIT_BANK_SIZE+NW_JIT_ISLAND_HEADER&&
     header_is(bank_island,other)&&header_is(0,layout)&&nw_jit_cache_get(0x1000,root,0,0,nullptr)==first,
     "bank-one entry invalidates only destination bank"))return false;
  if(packed&&!structure(run(first,0x89abcdefu,write_b),"unrelated bank original code AND cold data remain executable"))return false;
  nw_jit_cache_put(0x2000,root+0x1000,0,0,bank_one,4);
  if(packed){if(!structure(island_pack_test_seek(NW_JIT_ISLANDS-1,NW_JIT_ISLAND_SIZE/2-NW_JIT_MAX_EMIT+4,NW_JIT_ISLAND_SIZE-NW_JIT_MAX_EMIT),"packed final-island exhaustion wraps exact destination bank"))return false;}else g_code_used=NW_JIT_CODE_SIZE-NW_JIT_MAX_EMIT+4;
  auto wrapped=compile(ops,4,root,&other);
  if(!require(wrapped&&reinterpret_cast<uint8_t *>(wrapped)==g_code+NW_JIT_ISLAND_HEADER&&header_is(0,other)&&
     !g_alias_literals[1].ready&&header_is(bank_island,other)&&
     nw_jit_cache_get(0x1000,root,0,0,nullptr)==nullptr&&
     nw_jit_cache_get(0x2000,root+0x1000,0,0,nullptr)==bank_one&&run(bank_one,0x89abcdefu,write_b),
     "full wrap drops recycled bank pools/cache and preserves live other-bank code"))return false;
  if(packed){if(!structure(island_pack_test_seek(NW_JIT_BANK_SIZE/NW_JIT_ISLAND_SIZE,NW_JIT_ISLAND_HEADER,NW_JIT_ISLAND_SIZE/2),"packed bank-one recycle invalidates its pair and products"))return false;}else g_code_used=NW_JIT_BANK_SIZE;
  auto reclaimed=compile(ops,4,root,&layout);
  if(!require(reclaimed&&header_is(bank_island,layout)&&header_is(0,other)&&
     !nw_jit_cache_get(0x2000,root+0x1000,0,0,nullptr),"bank-one recycle permits new pair and drops its old cache"))return false;
  nw_jit_reset();
  if(packed){const auto state=island_pack_cursor_state();if(!structure(!state.highwater&&!state.emitted&&!island_pack_last_extent().committed,"packed reset clears full allocator and last producer extent"))return false;}
  for(const auto &pool:g_alias_literals)if(!require(!pool.ready,"reset clears every island pair"))return false;
  if(!require(!g_code_used,"reset clears allocator"))return false;
  auto reset=compile(ops,4,root,&layout);
  return require(reset&&reinterpret_cast<uint8_t *>(reset)==g_code+NW_JIT_ISLAND_HEADER&&
   refs(reset,g_code_used,layout,true)&&header_is(0,layout)&&run(reset,0x89abcdefu,write_b),"reset reinitializes first island from new slot addresses");
 }();
 nw_jit_reset();
 const unsigned expected_checks=22+3*NW_JIT_ISLANDS;
 if(okay&&checks!=expected_checks){
  fprintf(stderr,"SHARED ALIAS LITERALS check count %u differs from expected %u\n",checks,expected_checks);
  return 0;
 }
 if(okay){
  if(packed)fprintf(stderr,"SHARED ALIAS LITERALS SELFTEST %u allocator/range/pair/current-value/bank/reset checks passed layout=packed\n",checks);
  else fprintf(stderr,"SHARED ALIAS LITERALS SELFTEST %u allocator/range/pair/current-value/bank/reset checks passed\n",checks);
 }
 if(packed&&okay&&packed_checks==19)fprintf(stderr,"ISLAND PACK ALIAS SELFTEST 19 extent/cursor/failed-commit/cold-lifetime checks passed\n");
 return okay&&(!packed||packed_checks==19);
}
int nw_jit_shared_alias_literals_selftest(){
 const bool previous=island_pack_test_mode(false);const bool legacy=island_pack_alias_profile_selftest(false);island_pack_test_mode(true);const bool packed=legacy&&island_pack_alias_profile_selftest(true);island_pack_test_mode(previous);return legacy&&packed;
}
