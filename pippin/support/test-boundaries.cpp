// Project: DingusPPC-Pippin
// File: pippin/support/test-boundaries.cpp
// Purpose: Unit test for audio buffer boundaries.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
#include <devices/sound/guestpcm.h>
#include <devices/sound/audiobudget.h>
#include <core/applejack_button_map.h>
#include <cassert>
#include <iostream>
#include <vector>
int main(){
 unsigned cases=0;
 const int buttons[13]={14,15,9,8,0,1,2,17,16,10,13,11,12};
 const int sdl[13]={SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_Y,SDL_CONTROLLER_BUTTON_BACK,SDL_CONTROLLER_BUTTON_GUIDE,SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN,SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
 for(int b=0;b<256;b++){
  int want=-1;for(int j=0;j<13;j++)if(b==sdl[j])want=buttons[j];
  uint8_t got=222;bool ok=map_applejack_button(b,got);
  assert(ok==(want!=-1));assert(got==(want<0?222:want));cases++;
 }
 for(int count: {1,2,3,17,100})for(double step:{.025,.4,1.})for(double initial:{0.,.25,.9}){
  std::deque<int16_t> samples;for(int i=0;i<count;i++){samples.push_back(1234);samples.push_back(-2345);}
  double phase=initial;int16_t out[128];uint64_t consumed=0;
  for(int j=0;j<200 && !samples.empty();j++){
   auto used=consume_guest_pcm(samples,phase,step,out,64,true);consumed+=used.source;
   for(int i=0;i<64;i++)assert((out[2*i]==1234 && out[2*i+1]==-2345)||(out[2*i]==0&&out[2*i+1]==0));
  }
  assert(samples.empty() && consumed==unsigned(count) && phase==0);cases++;
 }
 {std::deque<int16_t> q={12,-12};double phase=0;int16_t out[8];auto u=consume_guest_pcm(q,phase,1,out,4);assert(q.size()==2 && u.source==0 && u.silence==4);cases++;}
 assert(!guest_pcm_ready(false,2,22050,true));assert(guest_pcm_ready(false,2,22050,false));assert(!guest_pcm_ready(false,0,22050,false));cases+=3;
 for(uint32_t rate:{11025u,22050u,44100u}){
  GuestAudioBudget b;uint64_t ns=0,frames=0;
  for(unsigned i=0;i<10000;i++){uint64_t dt=(i*7919ull)%20000000+1;ns+=dt;frames+=b.frames(rate,dt);assert(frames==rate*ns/1000000000ull);cases++;}
 }
 std::cout<<"PASS "<<cases<<" button, terminal-PCM, active-stream and irregular-DMA-budget checks\n";
}
