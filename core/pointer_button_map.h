// GPL-3.0-or-later. EventManager stores an eight-bit ADB mouse mask.
#pragma once
#include <cstdint>
inline bool pippin_pointer_button(uint8_t button,uint8_t& mask){
 if(button<1 || button>8)return false;
 unsigned bit=button==2?2:button==3?1:button-1;
 mask=uint8_t(1u<<bit);return true;
}
