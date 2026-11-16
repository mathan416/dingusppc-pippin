// Project: DingusPPC-Pippin
// File: pippin/support/test-fragments-pointer.cpp
// Purpose: Unit test for PCM fragments and pointer buttons.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
#include <devices/sound/guestpcm_assembler.h>
#include <core/pointer_button_map.h>
#include <cassert>
#include <vector>
#include <utility>
#include <iostream>
int main(){
 unsigned cases=0;std::vector<uint8_t> bytes;std::vector<std::pair<int16_t,int16_t>> expected;
 for(int v=-32768;v<=32767;v++){int16_t l=v,r=-1-v;expected.emplace_back(l,r);for(int16_t n:{l,r}){bytes.push_back(uint16_t(n)>>8);bytes.push_back(uint16_t(n)&255);}}
 for(unsigned chunk=1;chunk<=64;chunk++){
  GuestPcmAssembler a;size_t index=0;auto emit=[&](int16_t l,int16_t r){assert(index<expected.size());assert((std::make_pair(l,r)==expected[index++]));};
  for(size_t i=0;i<bytes.size();){size_t n=std::min(size_t(chunk),bytes.size()-i);a.append(bytes.data()+i,n,emit);i+=n;}
  assert(index==expected.size() && a.buffered_bytes()==0);cases++;
 }
 for(unsigned split=0;split<=1024;split++){
  GuestPcmAssembler a;size_t index=0;auto emit=[&](int16_t l,int16_t r){assert((std::make_pair(l,r)==expected[index++]));};
  a.append(bytes.data(),split,emit);a.append(bytes.data()+split,1024-split,emit);assert(index==256 && !a.buffered_bytes());cases++;
 }
 {GuestPcmAssembler a;unsigned n=0;auto emit=[&](int16_t l,int16_t r){assert(l==expected[0].first&&r==expected[0].second);n++;};a.append(bytes.data(),3,emit);assert(n==0&&a.buffered_bytes()==3);a.reset();a.append(bytes.data(),4,emit);assert(n==1&&!a.buffered_bytes());cases++;}
 for(unsigned b=0;b<256;b++){uint8_t m=123;bool ok=pippin_pointer_button(b,m);assert(ok==(b>=1&&b<=8));if(ok){unsigned bit=b==2?2:b==3?1:b-1;assert(m==(1u<<bit));}else assert(m==123);cases++;}
 std::cout<<"PASS "<<cases<<" fragment/reset/pointer cases; all 65536 signed PCM values decoded with 64 chunk sizes\n";
}
