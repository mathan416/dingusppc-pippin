// GPL-3.0-or-later. Stereo big-endian PCM frames may span DMA buffers.
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
