// Project: DingusPPC-Pippin
// File: devices/sound/guestpcm_assembler.h
// Purpose: Reassemble stereo PCM frames that span DMA buffers.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// Stereo big-endian PCM frames may span DMA buffers.
#pragma once
#include <cstdint>
#include <cstddef>
class GuestPcmAssembler {
 uint8_t tail[4]{};unsigned pending=0;
public:
 void reset(){pending=0;}
 unsigned buffered_bytes()const{return pending;}
 template<class Emit> void append(const uint8_t* bytes,size_t count,Emit emit){
  while(count){
   while(pending<4 && count){tail[pending++]=*bytes++;--count;}
   if(pending==4){
    int16_t left=static_cast<int16_t>((uint16_t(tail[0])<<8)|tail[1]);
    int16_t right=static_cast<int16_t>((uint16_t(tail[2])<<8)|tail[3]);
    emit(left,right);pending=0;
   }
  }
 }
};
