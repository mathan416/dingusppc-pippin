// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
// Returns the number of instructions consumed; zero falls back to the interpreter.
bool sheep_jit_enabled();
extern bool sheep_rom_only;
extern uintptr_t sheep_rom_host_begin,sheep_rom_host_end;
inline bool sheep_jit_code_page_enabled(const uint8_t *code,uint32_t){
 uintptr_t address=reinterpret_cast<uintptr_t>(code);
 return !sheep_rom_only||(address>=sheep_rom_host_begin&&address<sheep_rom_host_end);
}
extern unsigned char sheep_jit_reject_filter[4194304];
inline unsigned sheep_jit_filter_slot_rom(const uint8_t* p){return (uintptr_t(p)>>2)&1048575;}
inline unsigned sheep_jit_filter_slot_general(const uint8_t* p,uint32_t pc){return ((uintptr_t(p)>>2)^(pc>>14))&4194303;}
inline unsigned sheep_jit_filter_slot(const uint8_t* p,uint32_t pc){return sheep_rom_only?sheep_jit_filter_slot_rom(p):sheep_jit_filter_slot_general(p,pc);}
extern uint64_t sheep_rom_dispatch_words[1048576];
extern bool sheep_rom_decode_ready;
extern bool sheep_rom_direct_enabled;
extern uintptr_t sheep_rom_direct_base;
void sheep_rom_direct_restore(unsigned slot);
void sheep_rom_direct_reset();

inline unsigned sheep_rom_reject_state(unsigned slot){return sheep_rom_dispatch_words[slot]&3;}
inline void sheep_rom_reject_state_set(unsigned slot,unsigned state){
 if((sheep_rom_dispatch_words[slot]&3)==3)sheep_rom_direct_restore(slot);
 auto &word=sheep_rom_dispatch_words[slot];word=(word&~uint64_t(3))|state;
}
inline void sheep_reject_set(const uint8_t *code,uint32_t pc,uint8_t value){
 unsigned slot=sheep_jit_filter_slot(code,pc);sheep_jit_reject_filter[slot]=value;
 if(sheep_rom_only)sheep_rom_reject_state_set(slot,value==255?1:value?2:0);
}
[[gnu::always_inline]] inline bool sheep_rom_reject_retry(unsigned slot){
 auto &counter=sheep_jit_reject_filter[slot];
 if(counter){
  if(counter!=255 && !--counter)sheep_rom_reject_state_set(slot,0);
  return true;
 }
 sheep_rom_reject_state_set(slot,0);return false;
}
inline bool sheep_rom_dispatch_blocked_word(unsigned slot,uint64_t word){
 unsigned state=word&3;
 if(state<2) [[likely]] return state!=0;
 return sheep_rom_reject_retry(slot);
}
inline bool sheep_rom_dispatch_blocked(unsigned slot){return sheep_rom_dispatch_blocked_word(slot,sheep_rom_dispatch_words[slot]);}


inline unsigned sheep_jit_memory_primary(uint32_t op){unsigned p=op>>26;if(p>=32&&p<=45)return p;if(p==31)switch((op>>1)&1023){case 23:return 32;case 55:return 33;case 87:return 34;case 119:return 35;case 151:return 36;case 183:return 37;case 215:return 38;case 247:return 39;case 279:return 40;case 311:return 41;case 343:return 42;case 375:return 43;case 407:return 44;case 439:return 45;}return 0;}
inline bool sheep_jit_maybe(uint32_t op){
 unsigned p=op>>26,x=(op>>1)&1023;
 if((p==10||p==11) && ((op>>21)&3))return false;
 if(p==31){
  if((x==0||x==32) && ((op&1)||((op>>21)&3)))return false;
  if(x==19 && (op & 0x001ff801))return false; // mfcr reserved fields/Rc
  if(x==144 && (op & 0x00100801))return false; // mtcrf reserved fields/Rc
  if((x==26||x==104) && (op & 0x0000f800))return false; // cntlzw RB is reserved
 }
 if(p==31 && (x==339||x==467)){
  unsigned spr=((op>>16)&31)|((op>>6)&0x3e0);
  return !(op&1)&&(spr==1||spr==8||spr==9||spr==26||spr==27); // SRR0/1 additionally require the native supervisor guard.
 }
 if(unsigned memory=sheep_jit_memory_primary(op)){
  if(p==31&&(op&1))return false;
  p=memory;
  bool store=(p==36||p==37||p==38||p==39||p==44||p==45);
  unsigned ra=(op>>16)&31,rd=(op>>21)&31;
  return !(p&1)||(ra && (store||ra!=rd));
 }
 if(p==18 && !(op&2))return true; // Relative b/bl; AA requires separate qualification.
 if(p==16)return true; // Full native CR/CTR/LK/AA branches, qualified by the oracle.
 if(p==19 && !(op&1))switch(x){case 33:case 129:case 193:case 225:case 257:case 289:case 417:case 449:return true;}
 if(p==19 && (x==16||x==528)){
  unsigned bo=(op>>21)&31;
  if(bo==20)return true;
  // Conditional LK=1 has an unqualified upstream not-taken LR side effect.
  if(!(op&1)&&((bo&~1u)==4||(bo&~1u)==12))return true;
 }
 if(p==7||p==8||p==10||p==11||p==12||p==13||p==14||p==15||p==20||p==21||p==23||(p>=24&&p<=29))return true;
 if(p==31)switch(x){case 0:case 32:case 19:case 144:case 26:case 24:case 536:case 922:case 954:case 28:case 60:case 124:case 284:case 316:case 412:case 444:case 476:case 266:case 40:case 235:case 11:case 10:case 138:case 8:case 136:case 522:case 520:case 104:return true;}
 return false;
}
constexpr unsigned sheep_native_max_ops=32;
// decode_bound=0 preserves the legacy execution-budget-limited planner.
// A nonzero bound is accepted only for a freshly qualified immutable ROM span;
// it never permits native execution/publication beyond budget.
unsigned sheep_jit_try(const uint8_t* instructions,unsigned budget,unsigned decode_bound=0);

#include "nw_jit.h"
#include "ppcemu.h"
#include "ppcmmu.h"
extern void ppc_illegalop(uint32_t);
extern PPCOpcode sheep_rom_disabled_fpu_handler;
inline PPCOpcode sheep_rom_cached_handler(uint64_t word,bool disabled){
 if((word&0x80000000u)&&disabled)return sheep_rom_disabled_fpu_handler;
 int64_t words=int32_t(uint32_t(word)<<1)>>3;
 return reinterpret_cast<PPCOpcode>(reinterpret_cast<uintptr_t>(&ppc_illegalop)+words*4);
}

struct SheepHotSlot {
 const uint8_t *host=nullptr;
 uint32_t pc=0,n=0;
 nw_jit_fn fn=nullptr;
 uint64_t epoch=0;
};
extern SheepHotSlot sheep_hot_slots[65536];
extern uint64_t sheep_hot_epoch,sheep_hot_blocks,sheep_hot_instructions;
extern bool sheep_hot_enabled,sheep_jit_memory_bail,sheep_hot_used;
// A short taken branch is a successful translation. Back off only a failed
// guard/zero prefix, preserving the precise original instruction fallback.
inline bool sheep_native_requires_backoff(unsigned consumed){
 return !consumed||(consumed<4&&sheep_jit_memory_bail);
}

bool sheep_rom_direct_publish(const uint8_t*,uint32_t,unsigned,nw_jit_fn);
inline bool sheep_jit_rom_direct(unsigned slot,uint64_t word,uint64_t budget,unsigned &consumed){
 if(!sheep_rom_direct_enabled||(word&3)!=3||uint32_t(word>>32)!=ppc_state.pc)return false;
 unsigned n=((uint32_t(word)>>2)&31)+1;
 if(n>budget)return false;
 auto fn=reinterpret_cast<nw_jit_mapped_fn>(sheep_rom_direct_base+(uintptr_t(uint32_t(word)>>7)<<2));
 uint32_t initial=ppc_state.pc;sheep_jit_memory_bail=false;
 consumed=fn(reinterpret_cast<nw_jit_cpu*>(&ppc_state));
 if(consumed>n)__builtin_trap();
 if(sheep_native_requires_backoff(consumed)){sheep_rom_reject_state_set(slot,2);sheep_jit_reject_filter[slot]=32;}
 uint32_t target=ppc_state.pc;
 ppc_state.pc=consumed?initial+(consumed-1)*4:initial;
 if(consumed){
  if(target!=uint32_t(initial+consumed*4)||(target&~4095u)!=(initial&~4095u)){ppc_next_instruction_address=target;exec_flags=EXEF_BRANCH;}
  sheep_hot_used=true;
 }
 return true;
}

inline unsigned sheep_hot_index(const uint8_t *code,uint32_t){uintptr_t key=uintptr_t(code)>>2;return (key^(key>>16))&65535;}
// Slots are published only for mapped read-only ROM. RAM always retains source
// byte validation in the slow dispatcher. Epochs invalidate recycled native code.
template<bool account_pc=false>
inline bool sheep_jit_hot(const uint8_t *code,uint64_t budget,unsigned &consumed){
 if(!sheep_hot_enabled)return false;
 const auto &slot=sheep_hot_slots[sheep_hot_index(code,ppc_state.pc)];
 if(slot.host!=code||slot.pc!=ppc_state.pc||slot.epoch!=sheep_hot_epoch||slot.n>budget||!slot.fn)return false;
 uint32_t initial=ppc_state.pc;sheep_jit_memory_bail=false;
 consumed=reinterpret_cast<nw_jit_mapped_fn>(slot.fn)(reinterpret_cast<nw_jit_cpu*>(&ppc_state));
 if(consumed>slot.n)__builtin_trap();
 if(sheep_native_requires_backoff(consumed))sheep_reject_set(code,initial,32);
 uint32_t target=ppc_state.pc;
 // The main loop needs the last completed source PC before timer processing.
 // Publish it once here; ordinary callers retain the original initial-PC API.
 ppc_state.pc=account_pc&&consumed?initial+(consumed-1)*4:initial;


 if(consumed){if(target!=uint32_t(initial+consumed*4)||(target&~4095u)!=(initial&~4095u)){ppc_next_instruction_address=target;exec_flags=EXEF_BRANCH;}sheep_hot_used=true;}
 // Real guard backoff restores the opcode word. Any successful prefix,
 // including a short taken branch, may retain or republish its direct entry.
 if(!sheep_native_requires_backoff(consumed)&&sheep_rom_direct_enabled&&sheep_rom_only){
  auto word=sheep_rom_dispatch_words[sheep_jit_filter_slot_rom(code)];
  if((word&3)!=3||uint32_t(word>>32)!=initial)
   sheep_rom_direct_publish(code,initial,slot.n,slot.fn);
 }

 return true;
}

// Match the canonical primary-ITLB hit, reading the current table and mask on
// each mapping lookup. memcpy avoids aliasing the MMU's private TLBEntry type.
struct TLBEntry;
extern TLBEntry *pCurITLB1;
extern uint32_t tlb_size_mask;
extern const bool sheep_itlb_primary_enabled;
inline uint8_t *sheep_itlb_primary_hit(uint32_t va,uint32_t *physical=nullptr){
 if(!sheep_itlb_primary_enabled)return nullptr;
 const auto *entry=reinterpret_cast<const uint8_t*>(pCurITLB1)+size_t((va>>12)&tlb_size_mask)*32;
 uint32_t tag;std::memcpy(&tag,entry,4);
 if(tag!=(va&0xfffff000u))return nullptr;
 int64_t offset;std::memcpy(&offset,entry+8,8);
 if(physical){uint32_t page;std::memcpy(&page,entry+24,4);*physical=page|(va&4095);}
 return reinterpret_cast<uint8_t*>(offset+va);
}
inline uint8_t *sheep_translate_imem(uint32_t va,uint32_t *physical=nullptr){
 if(auto *host=sheep_itlb_primary_hit(va,physical))return host;
 return mmu_translate_imem(va,physical);
}
