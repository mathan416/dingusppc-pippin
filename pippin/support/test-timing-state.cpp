// Project: DingusPPC-Pippin
// File: pippin/support/test-timing-state.cpp
// Purpose: Unit test for audio pacing and button state.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
#include <devices/sound/guestpace.h>
#include <core/applejack_button_state.h>
#define GuestPace OriginalGuestPace
#include "original-guestpace.h"
#undef GuestPace
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include <tuple>
int main(){
 unsigned cases=0;
 auto run=[](uint64_t chunk,bool old){
  GuestPace p;p.primed=true;p.rate=.2;OriginalGuestPace o;o.primed=true;o.rate=.2;
  for(uint64_t t=0;t<1000000000;t+=chunk){p.update(chunk,chunk);o.update(chunk,chunk);}
  return old?o.rate:p.rate;
 };
 assert(std::abs(run(250000000,true)-run(1000000000,true))>.3);cases++;
 for(uint64_t chunk:{10000000ull,250000000ull,500000000ull,1000000000ull}){assert(std::abs(run(chunk,false)-.95)<1e-12);cases++;}
 for(double initial:{.025,.2,.5,1.})for(double target:{.025,.2,.5,1.}){
  GuestPace a,b;a.primed=b.primed=true;a.rate=b.rate=initial;
  a.update(uint64_t(target*1000000000),1000000000);
  for(int i=0;i<4;i++)b.update(uint64_t(target*250000000),250000000);
  assert(std::isfinite(a.rate)&&a.rate>=.025&&a.rate<=1);assert(std::abs(a.rate-b.rate)<1e-12);cases++;
 }
 {GuestPace p;assert(!p.update(0,0)&&p.rate==1);cases++;}
 AppleJackButtonState state;
 const int sdl[13]={SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_Y,SDL_CONTROLLER_BUTTON_BACK,SDL_CONTROLLER_BUTTON_GUIDE,SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN,SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
 const int guest[13]={14,15,9,8,0,1,2,17,16,10,13,11,12};
 bool reference[8][18]={};std::vector<std::pair<int,bool>> got;
 auto emit=[&](uint32_t id,uint8_t b,bool down){assert(id<8);got.emplace_back(b,down);};
 uint32_t rng=1234567;
 for(unsigned t=0;t<12000;t++){
  rng=rng*1664525+1013904223;unsigned id=(rng>>12)%8,op=(rng>>20)%10,button=(rng>>4)%256;
  bool before[18]={};for(auto& row:reference)for(int b=0;b<18;b++)before[b]=before[b]||row[b];
  got.clear();
  if(op==0){for(auto& row:reference)for(bool& b:row)b=false;state.release_all(emit);}
  else if(op<3){for(bool& b:reference[id])b=false;state.release(id,emit);}
  else{
   int mapped=-1;for(int j=0;j<13;j++)if(button==unsigned(sdl[j]))mapped=guest[j];
   if(mapped>=0)reference[id][mapped]=op%2;
   state.change(id,button,op%2,emit);
  }
  bool after[18]={};for(auto& row:reference)for(int b=0;b<18;b++)after[b]=after[b]||row[b];
  // Bulk releases may emit in per-device order: compare logical transitions.
  for(int b=0;b<18;b++){
   unsigned n=0;for(auto e:got)if(e.first==b){assert(e.second==after[b]);n++;}
   assert(n==unsigned(before[b]!=after[b]));
  }
  cases++;
 }
 // Explicit duplicate, focus-loss, overlapping owners and reconnect sequences.
 AppleJackButtonState s;std::vector<std::pair<int,bool>> events;
 auto collect=[&](uint32_t,uint8_t b,bool d){events.emplace_back(b,d);};
 s.change(1,SDL_CONTROLLER_BUTTON_B,true,collect);s.change(1,SDL_CONTROLLER_BUTTON_B,true,collect);s.change(2,SDL_CONTROLLER_BUTTON_B,true,collect);assert(events.size()==1);
 s.release(1,collect);assert(events.size()==1);s.release(2,collect);assert(events.size()==2&&!events.back().second);
 s.change(3,SDL_CONTROLLER_BUTTON_A,true,collect);s.release_all(collect);assert(events.size()==4&&!events.back().second);
 s.change(3,SDL_CONTROLLER_BUTTON_A,false,collect);assert(events.size()==4);
 s.change(4,SDL_CONTROLLER_BUTTON_A,true,collect);s.release(4,collect);assert(events.size()==6);cases+=6;
 std::cout<<"PASS "<<cases<<" elapsed-time pace and controller ownership/disconnect/focus/reconnect checks\n";
 std::cout<<"Old one-second pace response (250/500/1000 ms chunks): "<<run(250000000,true)<<" "<<run(500000000,true)<<" "<<run(1000000000,true)<<"; candidate "<<run(250000000,false)<<" "<<run(500000000,false)<<" "<<run(1000000000,false)<<"\n";
}
