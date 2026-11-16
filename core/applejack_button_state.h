// Project: DingusPPC-Pippin
// File: core/applejack_button_state.h
// Purpose: Track which session owns each logical AppleJack button.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// Session-local logical AppleJack button ownership.
#pragma once
#include <core/applejack_button_map.h>
#include <map>
class AppleJackButtonState {
 std::map<uint32_t,uint32_t> held;
 bool any(uint8_t button)const{for(const auto& entry:held)if(entry.second&(1u<<button))return true;return false;}
public:
 template<class Emit> void change(uint32_t id,Uint8 sdl_button,bool down,Emit emit){
  uint8_t button=0;if(!map_applejack_button(sdl_button,button))return;
  auto it=held.find(id);uint32_t old=it==held.end()?0:it->second,bit=1u<<button;
  if(bool(old&bit)==down)return;
  bool before=any(button);uint32_t next=down?old|bit:old&~bit;
  if(next)held[id]=next;else if(it!=held.end())held.erase(it);
  bool after=any(button);if(before!=after)emit(id,button,after);
 }
 template<class Emit> void release(uint32_t id,Emit emit){
  auto it=held.find(id);if(it==held.end())return;
  uint32_t bits=it->second;held.erase(it);
  for(uint8_t b=0;b<32;b++)if((bits&(1u<<b))&&!any(b))emit(id,b,false);
 }
 template<class Emit> void release_all(Emit emit){while(!held.empty())release(held.begin()->first,emit);}
};
