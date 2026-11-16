// SPDX-License-Identifier: GPL-3.0-or-later
// Private actual-ASM tag routing oracle. Include after loop_fixture_event().
// Export/call int sheep_tag_dispatch_selftest() from the private bridge verify
// setup, then reset native/ROM dispatch before other fixtures or gameplay.
static unsigned tag_dispatch_callback_calls,tag_dispatch_callback_mode;
static void tag_dispatch_callback(uint32_t opcode){
 tag_dispatch_callback_calls++;
 ppc_state.gpr[9]=opcode^0x5a96c3e1u;
 if(tag_dispatch_callback_mode==1)ppc_state.pc+=0x2000u;
 if(tag_dispatch_callback_mode==2){
  ppc_state.pc^=0x40000000u;
  exec_flags=EXEF_EXCEPTION|EXEF_BRANCH;
  ppc_next_instruction_address=ppc_state.pc+0x1040u;
 }
}

int sheep_tag_dispatch_selftest(){
 if(!sheep_direct_poll_layout())throw std::runtime_error("Tag-dispatch direct polling layout invalid");
 const auto saved=ppc_state;const auto saved_flags=exec_flags,saved_next=ppc_next_instruction_address;
 const auto saved_clock=g_icycles;const bool saved_timer=exec_timer,saved_power=power_on,saved_bail=sheep_jit_memory_bail;
 const unsigned saved_calls=tag_dispatch_callback_calls,saved_mode=tag_dispatch_callback_mode;
 alignas(16) std::array<uint8_t,16> code{};
 std::array<uint64_t,4> original_words{};std::array<uint8_t,4> original_counters{};
 sheep_rom_direct_reset();nw_jit_reset();
 for(unsigned i=0;i<4;i++){
  const unsigned slot=sheep_jit_filter_slot_rom(code.data()+i*4);
  original_words[i]=sheep_rom_dispatch_words[slot];original_counters[i]=sheep_jit_reject_filter[slot];
 }
 auto cleanup=[&](){
  for(unsigned i=0;i<4;i++){
   const unsigned slot=sheep_jit_filter_slot_rom(code.data()+i*4);
   sheep_rom_dispatch_words[slot]=original_words[i];sheep_jit_reject_filter[slot]=original_counters[i];
  }
  sheep_rom_direct_reset();nw_jit_reset();
  ppc_state=saved;exec_flags=saved_flags;ppc_next_instruction_address=saved_next;g_icycles=saved_clock;
  exec_timer=saved_timer;power_on=saved_power;sheep_jit_memory_bail=saved_bail;
  tag_dispatch_callback_calls=saved_calls;tag_dispatch_callback_mode=saved_mode;
 };
 unsigned passed=0;
 try{
  nw_jit_layout layout={offsetof(SetPRS,gpr),offsetof(SetPRS,cr),offsetof(SetPRS,spr)+SPR::XER*4,
   offsetof(SetPRS,spr)+SPR::LR*4,offsetof(SetPRS,spr)+SPR::CTR*4,offsetof(SetPRS,pc),reinterpret_cast<void*>(1)};
  layout.leaf=1;layout.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);
  const int64_t callback_delta=int64_t(reinterpret_cast<uintptr_t>(&tag_dispatch_callback))-int64_t(reinterpret_cast<uintptr_t>(&ppc_illegalop));
  if((callback_delta&3)||callback_delta<-(int64_t(1)<<30)||callback_delta>=(int64_t(1)<<30))
   throw std::runtime_error("Tag-dispatch private callback cannot be encoded");
  const uint32_t callback_description=uint32_t(callback_delta)&0x7ffffffcu;
  // native_kind: 0..3 => actual prefix1..4 in a planned4 block; 4=>alias
  // refusal; 5=>budget3 refusal. -1 uses the ordinary scalar descriptor.
  auto trial=[&](uint32_t root,unsigned operand,unsigned tag,unsigned retry,bool disabled,bool fpu,
                 int native_kind,uint32_t starting_flags,unsigned callback_mode){
   uint32_t ops[]={0x38630000u|(operand?0xffffu:1u),0x38840003u,0x38a5fffeu,0x38c60007u};
   const unsigned expected_native=native_kind>=0&&native_kind<4?unsigned(native_kind)+1:0;
   if(expected_native&&expected_native<4){
    const unsigned index=expected_native-1;
    ops[index]=0x48000000u|((0x40u-index*4)&0x03fffffcu); // non-next terminal B
   }
   std::array<uint64_t,4> words{};std::array<uint8_t,4> counters{};
   for(unsigned i=0;i<4;i++){
    for(unsigned j=0;j<4;j++)code[i*4+j]=uint8_t(ops[i]>>(24-j*8));
    uint32_t description;
    if(!sheep_encode_rom_handler(ops[i],description))throw std::runtime_error("Tag-dispatch fixture decode failed");
    words[i]=(uint64_t(ops[i])<<32)|(description&0x7ffffffcu)|1u;counters[i]=uint8_t(0x40+i);
   }
   words[0]=(words[0]&~0x80000003ull)|(fpu?0x80000000ull:0)|tag;counters[0]=uint8_t(retry);
   if(callback_mode)words[0]=(uint64_t(ops[0])<<32)|callback_description|1u;
   if(tag==3){
    auto fn=nw_jit_compile_mapped(ops,4,root,&layout);
    if(!fn)throw std::runtime_error("Tag-dispatch mapped native fixture compilation failed");
    const uintptr_t displacement=reinterpret_cast<uintptr_t>(fn)-reinterpret_cast<uintptr_t>(nw_jit_native_base());
    if((displacement&3)||displacement>=(uintptr_t(1)<<27))throw std::runtime_error("Tag-dispatch native fixture descriptor out of range");
    const uint32_t packed=(uint32_t(displacement>>2)<<5)|3u;
    words[0]=(uint64_t(root+(native_kind==4?4u:0u))<<32)|(uint64_t(packed)<<2)|3u;
   }
   for(unsigned i=0;i<4;i++){
    const unsigned slot=sheep_jit_filter_slot_rom(code.data()+i*4);
    sheep_rom_dispatch_words[slot]=words[i];sheep_jit_reject_filter[slot]=counters[i];
   }
   ppc_state=saved;ppc_state.pc=root;ppc_state.gpr[3]=operand?0x80000000u:0x7fffffffu;
   ppc_state.gpr[4]=0x12345678u;ppc_state.gpr[5]=0x89abcdefu;ppc_state.gpr[6]=0x10203040u;
   ppc_state.gpr[9]=0x24681357u;ppc_state.cr=0x59a63c87u;ppc_state.spr[SPR::XER]=0xe0001234u;
   ppc_state.spr[SPR::LR]=0x13572468u;ppc_state.spr[SPR::CTR]=0x98765432u;
   exec_flags=starting_flags;ppc_next_instruction_address=0x31415920u;exec_timer=true;power_on=true;g_icycles=123;
   sheep_jit_memory_bail=true;tag_dispatch_callback_calls=0;tag_dispatch_callback_mode=callback_mode;
   const auto initial=ppc_state;
   const unsigned budget=native_kind==5?3u:64u;
   SheepLoopContext ctx{};
   ctx.cpu=reinterpret_cast<uintptr_t>(&ppc_state);ctx.words=reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words);
   ctx.counters=reinterpret_cast<uintptr_t>(sheep_jit_reject_filter);ctx.native_base=reinterpret_cast<uintptr_t>(nw_jit_native_base());
   ctx.illegal=reinterpret_cast<uintptr_t>(&ppc_illegalop);ctx.fpu_disabled=reinterpret_cast<uintptr_t>(&tag_dispatch_callback);
   ctx.timer=reinterpret_cast<uintptr_t>(&exec_timer);ctx.clock=reinterpret_cast<uintptr_t>(&g_icycles);
   ctx.flags=reinterpret_cast<uintptr_t>(&exec_flags);ctx.next=reinterpret_cast<uintptr_t>(&ppc_next_instruction_address);
   ctx.max_cycles=123+budget;ctx.host=code.data();ctx.page_start=root&~4095u;ctx.page_end=ctx.page_start+4095u;
   ctx.pc_offset=offsetof(SetPRS,pc);ctx.cr_offset=offsetof(SetPRS,cr);ctx.lr_offset=offsetof(SetPRS,spr)+SPR::LR*4;
   ctx.disabled=disabled;ctx.reject=reinterpret_cast<uintptr_t>(&loop_fixture_reject);
   ctx.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);ctx.power=reinterpret_cast<uintptr_t>(&power_on);
   ctx.itlb=reinterpret_cast<uintptr_t>(&pCurITLB1);ctx.mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);
   ctx.rom_begin=reinterpret_cast<uintptr_t>(code.data());ctx.rom_end=reinterpret_cast<uintptr_t>(code.data()+code.size());
   const uint64_t result=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(&sheep_cpu_loop_a64),reinterpret_cast<nw_jit_cpu*>(&ctx));
   const auto actual=ppc_state;const auto actual_flags=exec_flags,actual_next=ppc_next_instruction_address;
   const auto actual_clock=g_icycles;const bool actual_timer=exec_timer,actual_power=power_on,actual_bail=sheep_jit_memory_bail;
   const unsigned actual_calls=tag_dispatch_callback_calls;
   unsigned expected=0;bool native=false;
   if(!starting_flags){
    if(tag==1||(tag==2&&retry))expected=1;
    if(tag==3&&native_kind>=0&&native_kind<4){expected=expected_native;native=true;}
   }
   const bool pending=expected!=0;
   if((result>>32)||bool(uint32_t(result))!=pending||ctx.retired!=expected||bool(ctx.native_used)!=native||actual_bail==native)
    throw std::runtime_error("Tag-dispatch route/count/native/bail/ABI contract differs");
   for(unsigned i=0;i<4;i++){
    uint64_t expected_word=words[i];uint8_t expected_counter=counters[i];
    if(!i&&!starting_flags&&tag==2&&retry!=255){
     expected_counter=retry?uint8_t(retry-1):0;
     if(!expected_counter)expected_word&=~3ull;
    }
    const unsigned slot=sheep_jit_filter_slot_rom(code.data()+i*4);
    if(sheep_rom_dispatch_words[slot]!=expected_word||sheep_jit_reject_filter[slot]!=expected_counter)
     throw std::runtime_error("Tag-dispatch changed wrong index, retry count or descriptor metadata");
   }
   ppc_state=initial;exec_flags=starting_flags;ppc_next_instruction_address=0x31415920u;
   g_icycles=123;exec_timer=true;power_on=true;tag_dispatch_callback_calls=0;
   for(unsigned i=0;i<expected;i++){
    const unsigned index=uint32_t(ppc_state.pc-root)/4;
    if(index>=4)throw std::runtime_error("Tag-dispatch interpreter replay left private source");
    if(callback_mode||(!native&&disabled&&fpu))tag_dispatch_callback(ops[index]);
    else ppc_main_opcode(ppc_opcode_grabber,ops[index]);
    if(i+1==expected&&pending)break;
    if(exec_flags&EXEF_BRANCH){ppc_state.pc=ppc_next_instruction_address;exec_flags=0;}
    else ppc_state.pc+=4;
    g_icycles++;
   }
   const auto *last_host=code.data()+(expected?(expected-1)*4:0);
   if(std::memcmp(&actual,&ppc_state,sizeof(actual))||actual_flags!=exec_flags||actual_next!=ppc_next_instruction_address||
      actual_clock!=g_icycles||actual_timer!=exec_timer||actual_power!=power_on||actual_calls!=tag_dispatch_callback_calls||
      ctx.host!=last_host||ctx.page_start!=(root&~4095u)||ctx.page_end!=uint32_t((root&~4095u)+4095u)||ctx.source_ram)
    throw std::runtime_error("Tag-dispatch full CPU/operand/flags/NIA/clock/source/callback oracle differs");
   passed++;
  };
  const uint32_t roots[]={0x10000000u,0x7ffff000u,0x80000000u,0xfffff000u};
  for(uint32_t root:roots)for(unsigned operand=0;operand<2;operand++)for(unsigned mode=0;mode<4;mode++){
   const bool disabled=mode&1,fpu=mode&2;
   trial(root,operand,0,71,disabled,fpu,-1,0,0);
   trial(root,operand,1,71,disabled,fpu,-1,0,0);
   trial(root,operand,2,0,disabled,fpu,-1,0,0);
   trial(root,operand,2,1,disabled,fpu,-1,0,0);
   trial(root,operand,2,255,disabled,fpu,-1,0,0);
  }
  for(uint32_t root:roots)for(unsigned operand=0;operand<2;operand++)for(int kind=0;kind<6;kind++)
   trial(root,operand,3,83,false,false,kind,0,0);
  for(uint32_t flags:{uint32_t(EXEF_BRANCH),uint32_t(EXEF_RFI),uint32_t(EXEF_EXCEPTION),
      uint32_t(EXEF_EXCEPTION|EXEF_BRANCH),16u,32u})for(unsigned tag=0;tag<4;tag++)
   trial(0x10000000u,0,tag,1,false,false,tag==3?3:-1,flags,0);
  for(uint32_t root:roots)for(unsigned mode=1;mode<=2;mode++)
   trial(root,mode&1,1,79,false,false,-1,0,mode);
  if(passed!=240)throw std::runtime_error("Tag-dispatch fixture case count differs from 240");
 }catch(...){cleanup();throw;}
 cleanup();
 fprintf(stderr,"TAG DISPATCH SELFTEST %u actual-ASM tag/retry/native/alias/mode/operand/flags/ABI cases passed\n",passed);
 return 1;
}
