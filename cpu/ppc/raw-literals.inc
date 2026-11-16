// Project: DingusPPC-Pippin
// File: cpu/ppc/raw-literals.inc
// Purpose: Raw literal pools for JIT code.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// Included after nw_jit.cpp's backend #endif. Same fixture in fixed and
// regression-only builds. Root bridge resets ROM descriptors before/after it.
#if defined(__aarch64__)
extern "C" uint64_t sheep_jit_abi_check(nw_jit_fn,nw_jit_cpu*);
static bool island_pack_raw_profile_selftest(bool packed){
 unsigned checks=0,packed_checks=0;
 auto structure=[&](bool okay,const char*why){++packed_checks;if(!okay)fprintf(stderr,"ISLAND PACK RAW failure check=%u: %s\n",packed_checks,why);return okay;};
 auto require=[&](bool okay,const char*why){++checks;if(!okay)fprintf(stderr,"RAW LITERAL failure check=%u %s\n",checks,why);return okay;};
 const auto *previous_layout=g_emit_layout;
 nw_jit_reset();
 const bool okay=[&](){
  if(!require(code_ready()!=0,"native arena"))return false;
  // These values are deliberately not dereferenced. They appear only in the
  // unexecuted failed-privilege tail. The successful real mapped leaf and a
  // separate tiny native reader exercise the actual compiled pool as data.
  const uint64_t values[]={0x01234567aa1303e0ull,0xaa1303e076543210ull,
   0xaa1303e0aa1303e0ull,1,UINT64_MAX,0x0123456789abcdefull,
   0xd65f03c0d503201full,0xfedcba9876543210ull};
  const unsigned lengths[]={4,5};
  for(unsigned n:lengths)for(uint64_t value:values){
   uint32_t ops[5];ops[0]=nw_ppc_mfspr(4,26);for(unsigned i=1;i<n;i++)ops[i]=nw_ppc_addi(3,3,1);
   nw_jit_layout layout={};layout.gpr=offsetof(nw_jit_cpu,gpr);layout.cr=offsetof(nw_jit_cpu,cr);
   layout.xer=offsetof(nw_jit_cpu,xer);layout.lr=offsetof(nw_jit_cpu,lr);layout.ctr=offsetof(nw_jit_cpu,ctr);layout.pc=offsetof(nw_jit_cpu,pc);
   layout.leaf=1;layout.msr=offsetof(nw_jit_cpu,msr);layout.srr0=offsetof(nw_jit_cpu,dec);layout.srr1=offsetof(nw_jit_cpu,fault_ea);
   layout.privilege_mask=1u<<14;layout.memory_bail=uintptr_t(value);
   const uint32_t pc=n==4?0x10000040u:0xfffffff0u;
   auto fn=nw_jit_compile_mapped(ops,int(n),pc,&layout);
   if(!require(fn!=nullptr,"actual mapped compile with literal-backed cold guard"))return false;
   const auto extent=island_pack_last_extent();
   const auto *start=reinterpret_cast<const uint32_t*>(fn);
   const auto *end=reinterpret_cast<const uint32_t*>(g_code+g_code_used);
   bool exact=false;uintptr_t literal=0;unsigned references=0;
   if(!packed){
   if(start<end&&size_t(end-start)<=NW_JIT_MAX_EMIT/4){
    for(auto *at=start;at<end;at++)if((*at&0xff00001fu)==0x58000009u){
     ++references;uint32_t imm=(*at>>5)&0x7ffffu;int32_t distance=(imm&0x40000u)?int32_t(imm)-0x80000:int32_t(imm);
     const uintptr_t target=reinterpret_cast<uintptr_t>(at)+uintptr_t(int64_t(distance)*4);
     if(!(target&7)&&target>=reinterpret_cast<uintptr_t>(start)&&target+8<=reinterpret_cast<uintptr_t>(end)){
      uint64_t actual;memcpy(&actual,reinterpret_cast<const void*>(target),8);
      exact=actual==value;literal=target;
     }
    }
   }
   }else{
    const uintptr_t arena=reinterpret_cast<uintptr_t>(g_code),hot=reinterpret_cast<uintptr_t>(extent.hot_begin),cold=reinterpret_cast<uintptr_t>(extent.cold_begin),finish=reinterpret_cast<uintptr_t>(extent.cold_end);
    if(!structure(extent.committed&&extent.packed&&extent.hot_begin==start&&extent.hot_begin<=extent.hot_code_end&&extent.hot_code_end<=extent.hot_end&&extent.cold_begin<=extent.cold_code_begin&&extent.cold_code_begin<=extent.cold_code_end&&extent.cold_code_end<=extent.pool_begin&&extent.pool_begin<=extent.cold_end&&extent.cold_padding<=4&&extent.cold_code_begin==extent.cold_begin+extent.cold_padding/4&&!(reinterpret_cast<uintptr_t>(extent.pool_begin)&7)&&extent.actual_bytes==size_t(extent.hot_end-extent.hot_begin+extent.cold_end-extent.cold_begin)*4&&extent.actual_bytes<=NW_JIT_MAX_EMIT&&hot>=arena+extent.island*NW_JIT_ISLAND_SIZE+NW_JIT_ISLAND_HEADER&&hot<arena+extent.island*NW_JIT_ISLAND_SIZE+NW_JIT_ISLAND_SIZE/2&&cold>=arena+extent.island*NW_JIT_ISLAND_SIZE+NW_JIT_ISLAND_SIZE/2&&finish<=arena+(extent.island+1)*NW_JIT_ISLAND_SIZE&&extent.bank==extent.island/(NW_JIT_BANK_SIZE/NW_JIT_ISLAND_SIZE),"exact two owned extents and bounded sum, never highwater gap"))return false;
    auto scan=[&](const uint32_t*begin,const uint32_t*finish_code){for(auto*at=begin;at<finish_code;at++)if((*at&0xff00001fu)==0x58000009u){
     ++references;uint32_t imm=(*at>>5)&0x7ffffu;int32_t distance=(imm&0x40000u)?int32_t(imm)-0x80000:int32_t(imm);const uintptr_t target=reinterpret_cast<uintptr_t>(at)+uintptr_t(int64_t(distance)*4);
     if(!(target&7)&&target>=reinterpret_cast<uintptr_t>(extent.pool_begin)&&target+8<=reinterpret_cast<uintptr_t>(extent.cold_end)){uint64_t actual;memcpy(&actual,reinterpret_cast<const void*>(target),8);exact=actual==value;literal=target;}
    }};scan(extent.hot_begin,extent.hot_end);scan(extent.cold_code_begin,extent.cold_code_end);
   }
   if(!require(references==1&&exact,"raw literal full64bits and aligned in-range LDR target"))return false;
   nw_jit_cpu cpu;memset(&cpu,0,sizeof(cpu));for(unsigned i=0;i<32;i++)cpu.gpr[i]=0x12345678u+i*0x1020304u;
   cpu.gpr[3]=0xfffffffeu;cpu.pc=pc;cpu.dec=0x76543210u;cpu.fault_ea=0xfedcba98u;cpu.cr=0xa5a55a5au;cpu.xer=0xe0000042u;cpu.lr=0x31415927u;cpu.ctr=0xabcdef01u;
   nw_jit_cpu expected;memcpy(&expected,&cpu,sizeof(cpu));expected.gpr[4]=cpu.dec;expected.gpr[3]+=n-1;expected.pc=pc+n*4;
   const uint64_t result=sheep_jit_abi_check(fn,&cpu);
   if(!require(result==n&&!memcmp(&cpu,&expected,sizeof(cpu)),"successful native leaf fullCPU/count/ABI; cold pointer untouched"))return false;
   bool emitted=true;uint32_t*probe=nullptr;
   if(!packed){
   const size_t offset=code_reservation_start(g_code_used);
   if(offset+16>NW_JIT_CODE_SIZE)return require(false,"native reader arena capacity");
   uint32_t word=0;
   probe=reinterpret_cast<uint32_t*>(g_code+offset);
   emitted=emitted&&alias_literal_instruction(reinterpret_cast<uintptr_t>(probe),literal,word);
   struct emit e={};e.p=probe;e.end=probe+4;e.leaf=true;
#ifdef __APPLE__
   pthread_jit_write_protect_np(0);
#endif
   emitted=emitted&&emit_w(&e,word)&&emit_w(&e,0xf9000009u)&&emit_w(&e,a64_movz(0,1,0))&&emit_w(&e,0xd65f03c0u);
#ifdef __APPLE__
   pthread_jit_write_protect_np(1);
#endif
   __builtin___clear_cache(reinterpret_cast<char*>(probe),reinterpret_cast<char*>(e.p));
   g_code_used=offset+size_t(e.p-probe)*4;
   }else{
    const auto before_reader=island_pack_cursor_state();auto reader=island_pack_test_literal_reader(literal);probe=reinterpret_cast<uint32_t*>(reader);const auto after_reader=island_pack_cursor_state();const auto saved=island_pack_last_extent();emitted=reader!=nullptr;
    uint32_t instruction=0;const bool encoded=emitted&&alias_literal_instruction(reinterpret_cast<uintptr_t>(probe),literal,instruction);
    if(!structure(encoded&&probe[0]==instruction&&probe[1]==0xf9000009u&&probe[2]==a64_movz(0,1,0)&&probe[3]==0xd65f03c0u&&after_reader.island==before_reader.island&&after_reader.hot==before_reader.hot+16&&after_reader.cold==before_reader.cold&&after_reader.highwater>=before_reader.highwater&&saved.hot_begin==extent.hot_begin&&saved.cold_end==extent.cold_end&&saved.actual_bytes==extent.actual_bytes,"atomic same-island reader exact4words, cursor advance, saved producer extent"))return false;
   }
   uint64_t output[3]={0xfeedfacecafebeefull,0,0xfeedfacecafebeefull};
   uint64_t raw_result=0;if(emitted)raw_result=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(probe),reinterpret_cast<nw_jit_cpu*>(output+1));
   if(!require(emitted&&raw_result==1&&output[1]==value&&output[0]==0xfeedfacecafebeefull&&output[2]==0xfeedfacecafebeefull,"native reader returns actual pool bits with output canaries/ABI"))return false;
  }
  return true;
 }();
 g_emit_layout=previous_layout;nw_jit_reset();
 if(okay&&checks==65){
  if(packed)fprintf(stderr,"RAW LITERAL SELFTEST 65 actual-pool/full64/native-reader/fullCPU/ABI cases passed layout=packed\n");
  else fprintf(stderr,"RAW LITERAL SELFTEST 65 actual-pool/full64/native-reader/fullCPU/ABI cases passed\n");
 }
 if(packed&&okay&&packed_checks==32)fprintf(stderr,"ISLAND PACK RAW SELFTEST 32 owned-extents/atomic-reader checks passed\n");
 return okay&&checks==65&&(!packed||packed_checks==32);
}
int nw_jit_raw_literals_selftest(){
 const bool previous=island_pack_test_mode(false);const bool legacy=island_pack_raw_profile_selftest(false);island_pack_test_mode(true);const bool packed=legacy&&island_pack_raw_profile_selftest(true);island_pack_test_mode(previous);return legacy&&packed;
}
#else
int nw_jit_raw_literals_selftest(){return 0;}
#endif
