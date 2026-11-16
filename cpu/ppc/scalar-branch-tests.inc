// SPDX-License-Identifier: GPL-3.0-or-later
// Exact scalar B/BC transitions versus the frozen original dispatcher and
// independent canonical per-instruction replay. No native-admission changes.
#if defined(__aarch64__)
struct ScalarBranchFrame {
 uint8_t *ram;TLBEntry *new_table;uint32_t new_mask,new_cr;unsigned action,calls;
 struct Seen {uint32_t pc,lr,next,cr;uint64_t clock;} seen[2]{};
};
static ScalarBranchFrame &scalar_branch_frame(){return *reinterpret_cast<ScalarBranchFrame*>(uintptr_t(ppc_state.gpr[30])|(uintptr_t(ppc_state.gpr[31])<<32));}
[[gnu::noinline]] static void scalar_branch_callback(uint32_t op){
 auto &f=scalar_branch_frame();auto &v=f.seen[std::min(f.calls,1u)];v.pc=ppc_state.pc;v.lr=ppc_state.spr[SPR::LR];v.next=ppc_next_instruction_address;v.cr=ppc_state.cr;v.clock=g_icycles;++f.calls;
 ppc_state.gpr[24]+=0x10203u+f.calls;ppc_state.gpr[29]^=0x9e3779b9u+f.calls;f.ram[f.calls*13u]^=uint8_t(op^f.calls);
 if(f.action&&f.calls==1){pCurITLB1=f.new_table;tlb_size_mask=f.new_mask;ppc_state.cr=f.new_cr;}else{ppc_state.cr^=0x51a03c85u;exec_timer=true;}
 generic_continuation_clobber();
}
#endif
int sheep_scalar_branch_selftest(){
#if defined(__aarch64__) && !defined(LOG_INSTRUCTIONS) && !defined(CPU_PROFILING)
 SetPRS saved;std::memcpy(&saved,&ppc_state,sizeof(saved));const auto flags0=exec_flags,next0=ppc_next_instruction_address;const auto clock0=g_icycles;
 const bool timer0=exec_timer,power0=power_on,bail0=sheep_jit_memory_bail,only0=sheep_rom_only,direct0=sheep_rom_direct_enabled;
 const bool old601=is_601,oldinclude=include_601;auto *const decoder0=ppc_opcode_grabber;
 auto *const i1=pCurITLB1,*const i2=pCurITLB2,*const d1=pCurDTLB1,*const d2=pCurDTLB2;const auto im0=CurITLBMode,dm0=CurDTLBMode;const auto mask0=tlb_size_mask;
 const auto begin0=sheep_rom_host_begin,end0=sheep_rom_host_end,base0=sheep_rom_direct_base;
 const auto reason0=power_off_reason;const bool irq0=int_pin,dec0=dec_exception_pending,thrm0=thrm_exception_pending;
 auto callbacks0=std::move(gCtxSyncCallbacks);gCtxSyncCallbacks.clear();
 alignas(4096) std::array<uint8_t,16384> code{};std::array<uint8_t,8192> ram{};
 alignas(32) std::array<uint8_t,4096*32> table_a{},table_b{};
 std::array<uint64_t,4096> old_words{},words{};std::array<uint8_t,4096> old_filter{},filter{},callback{};
 ScalarBranchFrame frame{ram.data(),reinterpret_cast<TLBEntry*>(table_b.data()),0,0,0,0,{}};
 unsigned passed=0,same_page=0,polls=0,cross=0,sequential=0,live=0,unaligned=0;
 for(unsigned i=0;i<4096;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+4*i);old_words[i]=sheep_rom_dispatch_words[slot];old_filter[i]=sheep_jit_reject_filter[slot];}
 auto need=[&](bool okay,const char *why){if(!okay){fprintf(stderr,"SCALAR BRANCH failure case=%u %s\n",passed+1,why);throw std::runtime_error("Q201 scalar branch fixture failed");}};
 auto refresh=[](){nw_jit_layout temporary{};sheep_mmu_jit_layout(&temporary);};
 auto cleanup=[&](){
  sheep_rom_direct_reset();nw_jit_reset();for(unsigned i=0;i<4096;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+4*i);sheep_rom_dispatch_words[slot]=old_words[i];sheep_jit_reject_filter[slot]=old_filter[i];}
  std::memcpy(&ppc_state,&saved,sizeof(saved));pCurITLB1=i1;pCurITLB2=i2;pCurDTLB1=d1;pCurDTLB2=d2;CurITLBMode=im0;CurDTLBMode=dm0;tlb_size_mask=mask0;is_601=old601;include_601=oldinclude;ppc_opcode_grabber=decoder0;refresh();
  exec_flags=flags0;ppc_next_instruction_address=next0;g_icycles=clock0;exec_timer=timer0;power_on=power0;sheep_jit_memory_bail=bail0;sheep_rom_only=only0;sheep_rom_direct_enabled=direct0;sheep_rom_host_begin=begin0;sheep_rom_host_end=end0;sheep_rom_direct_base=base0;
  power_off_reason=reason0;int_pin=irq0;dec_exception_pending=dec0;thrm_exception_pending=thrm0;gCtxSyncCallbacks=std::move(callbacks0);
 };
 try{
  sheep_rom_direct_reset();nw_jit_reset();sheep_rom_only=sheep_rom_direct_enabled=true;sheep_rom_host_begin=reinterpret_cast<uintptr_t>(code.data());sheep_rom_host_end=sheep_rom_host_begin+12288;
  const int64_t delta=int64_t(reinterpret_cast<uintptr_t>(&scalar_branch_callback))-int64_t(reinterpret_cast<uintptr_t>(&ppc_illegalop));need(!(delta&3)&&delta>=-(int64_t(1)<<30)&&delta<(int64_t(1)<<30),"private callback packed range");
  auto blank=[](auto &table){table.fill(0);for(unsigned i=0;i<4096;i++){const uint32_t tag=0xffffffffu;std::memcpy(table.data()+i*32,&tag,4);}};
  auto map=[](auto &table,uint32_t pc,uint8_t *host,uint16_t flags,uint32_t mask){auto *entry=table.data()+size_t((pc>>12)&mask)*32;const uint32_t tag=pc&~4095u;const int64_t off=int64_t(reinterpret_cast<uintptr_t>(host))-int64_t(pc);std::memcpy(entry,&tag,4);std::memcpy(entry+4,&flags,2);std::memcpy(entry+8,&off,8);std::memcpy(entry+16,&off,8);};
  auto put=[&](unsigned off,uint32_t op,bool cb=false){need(off+4<=code.size(),"owned source extent");for(unsigned b=0;b<4;b++)code[off+b]=uint8_t(op>>(24-8*b));const unsigned index=off/4;uint32_t packed=uint32_t(delta)&0x7ffffffcu;if(!cb)need(sheep_encode_rom_handler(op,packed),"canonical scalar handler");words[index]=(uint64_t(op)<<32)|packed|1u;callback[index]=cb;};
  auto branch=[](unsigned family,uint32_t source,uint32_t target,unsigned bo=4,unsigned bi=8,unsigned flags=0){return family<2?(0x48000000u|((target-(flags&2?0u:source))&0x03fffffcu)|family|flags):((16u<<26)|(bo<<21)|(bi<<16)|((target-(flags&2?0u:source))&0xfffcu)|flags);};
  auto trial=[&](unsigned group,unsigned family,unsigned a,unsigned b,unsigned variant){
   code.fill(0);callback.fill(0);filter.fill(255);blank(table_a);blank(table_b);for(unsigned i=0;i<4096;i++)put(i*4,0x60000000u);for(unsigned i=0;i<ram.size();i++)ram[i]=uint8_t(i*37u+11u);
   const uint32_t page=a?0xfffff000u:0x10000000u;uint32_t root=page+128,target=root+64;unsigned start=128,target_off=192,bo=4,bi=8,op_flags=0,expected_retired=2,expected_pending=1,allow_ram=1,source_ram=0;bool taken=true,timer=false,power=true;uint32_t entry_flags=0,live_mask=4095;uint64_t deadline=4096;
   frame.action=0;frame.calls=0;frame.new_mask=0;frame.new_cr=0;
   if(group==0){
    const int displacements[]={4,64,-64,0};target=root+uint32_t(displacements[variant]);target_off=unsigned(int(start)+displacements[variant]);
    if(family==2){bo=b;bi=(b&1)?31:8;taken=a!=0;}
    if(family==3){family=b;op_flags=2;target=64;target_off=64;}
    if(family==4){family=2;bo=b;op_flags=variant;taken=variant==2;target=op_flags&2?64:root+64;target_off=op_flags&2?64:192;}
    if(taken&&target==root){expected_retired=128;}
    else put(taken?target_off:start+4,0x60000000u,true);
   }else if(group==1){
    source_ram=b;start=b?12288+128:128;root=page+128;target=root;target_off=start;
    if(variant==0)deadline=123;else if(variant==1){deadline=124;expected_retired=2;}else if(variant==2)timer=true;else if(variant==3)power=false;else if(variant==5)entry_flags=EXEF_BRANCH|EXEF_FETCH;
    expected_retired=variant==4?128:variant==1?2:variant==5?0:1;expected_pending=variant==5?0:1;
   }else if(group==2||group==3){
    start=4092;root=page+4092;const uint32_t next_page=page+4096u;const unsigned bias=group==3?0:64;target=next_page+bias;target_off=4096+bias;
    if(variant==1){target_off=8192+bias;live_mask=5;}
    if(variant>=2&&variant<=4){target_off=12288+bias+(variant==4);allow_ram=variant!=3;}
    if(variant==7){target_off=4096+bias;live_mask=0;}
    if(variant==2){put(target_off,0x38c60002u,true);deadline=124;}
    else if(variant<2||variant==7)put(target_off,0x60000000u,true);
    else expected_retired=1;
    if(group==3){ // Four classes: ROM, RAM, missing tag, non-memory.
     target_off=variant==1?12288:4096;if(variant==1){put(target_off,0x38c60002u,true);deadline=124;}else if(!variant)put(target_off,0x60000000u,true);else expected_retired=1;
    }
   }else if(group==4){
    start=4092-4;root=page+start;target=page+4096u+64;target_off=8192+64;frame.action=1;frame.new_mask=b?5:0;live_mask=4095;
    put(start,0x60000000u,true);put(4096+64,0x38c60001u);put(4096+68,0x60000000u,true);put(target_off,0x38c60002u);put(target_off+4,0x60000000u,true);
    map(table_b,target,code.data()+target_off,1,frame.new_mask);expected_retired=4;
   }else{
    start=12288+130;root=a?0xfffffffeu:0x10000082u;source_ram=1;target=root+(b?uint32_t(-64):4u);target_off=unsigned(int(start)+(b?-64:4));timer=variant!=0;deadline=124;
    put(target_off,0x38c60002u);expected_retired=timer?1:((target^root)&~4095u)?1:2;
   }
   // A live-callback case begins with the callback and branches at source+4.
   const unsigned branch_off=group==4?start+4:start;const uint32_t branch_pc=group==4?root+4:root;const uint32_t op=branch(family,branch_pc,target,bo,bi,op_flags);put(branch_off,op);
   if(group==0&&!taken)put(start+4,0x60000000u,true);
   map(table_a,page,code.data(),1,live_mask);
   if(group==0&&op_flags&2)map(table_a,target,code.data()+target_off,1,live_mask);
   if(group==2||group==3){
    uint16_t attr=1;if(group==2&&variant==6)attr=0;if(group==3&&variant==3)attr=0;
    map(table_a,target,code.data()+target_off,attr,live_mask);
    if((group==2&&variant==5)||(group==3&&variant==2)){auto *at=table_a.data()+size_t((target>>12)&live_mask)*32;const uint32_t bad=(target&~4095u)^4096u;std::memcpy(at,&bad,4);}
   }
   if(group==4)map(table_a,target,code.data()+4096+64,1,live_mask);
   if(group==5&&((target^root)&~4095u))map(table_a,target,code.data()+target_off,1,live_mask);
   SetPRS initial;std::memcpy(&initial,&saved,sizeof(initial));initial.pc=root;initial.spr[SPR::PVR]=PPC_VER::MPC603;initial.msr=MSR::IR;for(unsigned r=0;r<32;r++)initial.gpr[r]=0xa5310000u^(r*0x123457u+passed*0x10203u);initial.gpr[6]=19;initial.gpr[24]=0x831579bdu;initial.gpr[29]=0x91234567u;initial.cr=0x51a03c85u;initial.spr[SPR::XER]=0xe0000042u;initial.spr[SPR::LR]=0x76543213u;initial.spr[SPR::CTR]=7;
   if(family==2){const uint32_t mask=uint32_t(1)<<(31-bi);const bool truth=bool(bo&8)==taken;initial.cr=(initial.cr&~mask)|(truth?mask:0);}
   frame.new_cr=initial.cr;const uintptr_t fp=reinterpret_cast<uintptr_t>(&frame);initial.gpr[30]=uint32_t(fp);initial.gpr[31]=uint32_t(fp>>32);
   SheepLoopContext context{};context.cpu=reinterpret_cast<uintptr_t>(&ppc_state);context.words=reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words);context.counters=reinterpret_cast<uintptr_t>(sheep_jit_reject_filter);context.native_base=reinterpret_cast<uintptr_t>(nw_jit_native_base());context.illegal=reinterpret_cast<uintptr_t>(&ppc_illegalop);context.fpu_disabled=reinterpret_cast<uintptr_t>(sheep_rom_disabled_fpu_handler);context.timer=reinterpret_cast<uintptr_t>(&exec_timer);context.clock=reinterpret_cast<uintptr_t>(&g_icycles);context.flags=reinterpret_cast<uintptr_t>(&exec_flags);context.next=reinterpret_cast<uintptr_t>(&ppc_next_instruction_address);context.max_cycles=deadline;context.host=code.data()+start;context.page_start=root&~4095u;context.page_end=context.page_start+4095u;context.pc_offset=offsetof(SetPRS,pc);context.cr_offset=offsetof(SetPRS,cr);context.lr_offset=offsetof(SetPRS,spr)+SPR::LR*4;context.disabled=1;context.reject=reinterpret_cast<uintptr_t>(&generic_continuation_reject);context.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);context.power=reinterpret_cast<uintptr_t>(&power_on);context.itlb=reinterpret_cast<uintptr_t>(&pCurITLB1);context.mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);context.rom_begin=sheep_rom_host_begin;context.rom_end=sheep_rom_host_end;context.opcodes=reinterpret_cast<uintptr_t>(OpcodeGrabberNoFPU);context.source_ram=source_ram;context.allow_ram=allow_ram;
   const auto before_code=code;const auto before_ram=ram;const auto before_a=table_a,before_b=table_b;
   auto reset=[&](){std::memcpy(&ppc_state,&initial,sizeof(initial));code=before_code;ram=before_ram;table_a=before_a;table_b=before_b;pCurITLB1=reinterpret_cast<TLBEntry*>(table_a.data());pCurITLB2=i2;pCurDTLB1=d1;pCurDTLB2=d2;CurITLBMode=1;CurDTLBMode=0;tlb_size_mask=live_mask;ppc_opcode_grabber=OpcodeGrabberNoFPU;is_601=include_601=false;exec_flags=entry_flags;ppc_next_instruction_address=0x31415920u;g_icycles=123;exec_timer=timer;power_on=power;sheep_jit_memory_bail=true;int_pin=dec_exception_pending=thrm_exception_pending=false;gCtxSyncCallbacks.clear();frame.calls=0;std::memset(frame.seen,0,sizeof(frame.seen));for(unsigned i=0;i<4096;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+i*4);sheep_rom_dispatch_words[slot]=words[i];sheep_jit_reject_filter[slot]=filter[i];}};
   struct Result {SetPRS cpu;SheepLoopContext ctx;std::array<uint8_t,8192> ram;ScalarBranchFrame::Seen seen[2];uint64_t abi,clock;uint32_t flags,next,mask,im,dm;unsigned calls;bool timer,power,bail,metadata;PPCOpcode *decoder;TLBEntry *table;};
   auto run=[&](unsigned(*fn)(SheepLoopContext*)){reset();Result r{};std::memcpy(&r.ctx,&context,sizeof(context));r.abi=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(fn),reinterpret_cast<nw_jit_cpu*>(&r.ctx));std::memcpy(&r.cpu,&ppc_state,sizeof(r.cpu));r.ram=ram;std::memcpy(r.seen,frame.seen,sizeof(r.seen));r.clock=g_icycles;r.flags=exec_flags;r.next=ppc_next_instruction_address;r.mask=tlb_size_mask;r.im=CurITLBMode;r.dm=CurDTLBMode;r.calls=frame.calls;r.timer=exec_timer;r.power=power_on;r.bail=sheep_jit_memory_bail;r.decoder=ppc_opcode_grabber;r.table=pCurITLB1;r.metadata=code==before_code&&table_a==before_a&&table_b==before_b;for(unsigned i=0;i<4096;i++){const auto slot=sheep_jit_filter_slot_rom(code.data()+i*4);r.metadata=r.metadata&&sheep_rom_dispatch_words[slot]==words[i]&&sheep_jit_reject_filter[slot]==filter[i];}return r;};
   const auto old=run(&sheep_generic_continuation_reference_a64),actual=run(&sheep_cpu_loop_a64);
   need(old.abi==actual.abi&&!(actual.abi>>32)&&!std::memcmp(&old.cpu,&actual.cpu,sizeof(SetPRS))&&!std::memcmp(&old.ctx,&actual.ctx,sizeof(SheepLoopContext))&&old.ram==actual.ram&&!std::memcmp(old.seen,actual.seen,sizeof(old.seen))&&old.clock==actual.clock&&old.flags==actual.flags&&old.next==actual.next&&old.mask==actual.mask&&old.im==actual.im&&old.dm==actual.dm&&old.calls==actual.calls&&old.timer==actual.timer&&old.power==actual.power&&old.bail==actual.bail&&old.decoder==actual.decoder&&old.table==actual.table&&old.metadata&&actual.metadata,"frozen288/new direct scalar exact fullCPU/context/RAM/flags/NIA/clock/ABI/callback/MMU");
   need(actual.ctx.retired==expected_retired&&uint32_t(actual.abi)==expected_pending&&!actual.ctx.native_used,"independent scalar count/cap/pending/entryflags contract");
   if(group==4)need(actual.cpu.gpr[6]==21&&actual.calls==2,"live callback selects new target mapping and current CR");
   if((group==2&&variant==2)||(group==3&&variant==1)||(group==5&&expected_retired==2))need(actual.cpu.gpr[6]==21,"actual RAM target opcode ignores packed callback");
   // Independent canonical replay. Keep the original source-page affine host
   // mapping until a page transition, then consult the live table/mask only.
   reset();uint32_t source_pc=root;uint8_t *host=code.data()+start;bool source_is_ram=source_ram;
   for(unsigned step=0;step<expected_retired;step++){
    const unsigned offset=unsigned(host-code.data());need(offset+4<=code.size(),"canonical source inside owned payload");const uint32_t word=uint32_t(host[0])<<24|uint32_t(host[1])<<16|uint32_t(host[2])<<8|host[3];ppc_state.pc=source_pc;
    if(!source_is_ram&&callback[offset/4])scalar_branch_callback(word);else ppc_main_opcode(ppc_opcode_grabber,word);
    if(step+1==expected_retired&&expected_pending)break;
    const uint32_t next=exec_flags==EXEF_BRANCH?ppc_next_instruction_address:source_pc+4u;need(!exec_flags||exec_flags==EXEF_BRANCH,"canonical nonfinal flags");
    if(((next^source_pc)&~4095u)==0)host+=int32_t(next-source_pc);else{auto *entry=reinterpret_cast<uint8_t*>(pCurITLB1)+size_t((next>>12)&tlb_size_mask)*32;uint32_t tag;uint16_t attr;int64_t off;std::memcpy(&tag,entry,4);std::memcpy(&attr,entry+4,2);std::memcpy(&off,entry+8,8);need(tag==(next&~4095u)&&(attr&1),"canonical mapped nonfinal target");host=reinterpret_cast<uint8_t*>(uintptr_t(off)+next);source_is_ram=uintptr_t(host)<sheep_rom_host_begin||uintptr_t(host)>=sheep_rom_host_end;need(!source_is_ram||allow_ram,"canonical permitted RAM target");}
    exec_flags=0;source_pc=next;ppc_state.pc=next;++g_icycles;
   }
   need(!std::memcmp(&ppc_state,&old.cpu,sizeof(SetPRS))&&ram==old.ram&&g_icycles==old.clock&&exec_flags==old.flags&&ppc_next_instruction_address==old.next&&exec_timer==old.timer&&power_on==old.power&&frame.calls==old.calls&&!std::memcmp(frame.seen,old.seen,sizeof(frame.seen)),"independent canonical opcode/currentPC/LR/NIA/clock/callback/RAM oracle");
   ++passed;
  };
  for(unsigned family=0;family<2;family++)for(unsigned d=0;d<4;d++){trial(0,family,0,0,d);++same_page;}
  for(unsigned bo:{4u,5u,12u,13u})for(unsigned taken=0;taken<2;taken++)for(unsigned d=0;d<4;d++){trial(0,2,taken,bo,d);++same_page;}
  for(unsigned link=0;link<2;link++){trial(0,3,0,link,0);++same_page;}
  for(unsigned bo:{4u,12u})for(unsigned flags=1;flags<4;flags++){trial(0,4,0,bo,flags);++same_page;}
  for(unsigned family=0;family<3;family++)for(unsigned source=0;source<2;source++)for(unsigned event=0;event<6;event++){trial(1,family,0,source,event);++polls;}
  for(unsigned family=0;family<3;family++)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned mapping=0;mapping<8;mapping++){trial(2,family,wrap,0,mapping);++cross;}
  for(unsigned family=0;family<3;family++)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned mapping=0;mapping<4;mapping++){trial(3,family,wrap,0,mapping);++sequential;}
  for(unsigned family=0;family<3;family++)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned mask=0;mask<2;mask++){trial(4,family,wrap,mask,0);++live;}
  for(unsigned family=0;family<2;family++)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned direction=0;direction<2;direction++)for(unsigned timer=0;timer<2;timer++){trial(5,family,wrap,direction,timer);++unaligned;}
  need(same_page==48&&polls==36&&cross==48&&sequential==24&&live==12&&unaligned==16&&passed==184,"fixed focused scalar branch census");
 }catch(...){cleanup();throw;}
 cleanup();fprintf(stderr,"SCALAR BRANCH SELFTEST %u samepage/polls/cap/liveITLB/ROM-RAM/wrap/unaligned/canonical/fullCPU/ABI cases passed\n",passed);return 1;
#else
 return 0;
#endif
}
