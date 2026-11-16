// GPL-3.0-or-later. Keep the fractional frame across irregular guest timer ticks.
#pragma once
#include <cstdint>
struct GuestAudioBudget {
 uint64_t remainder=0;
 uint32_t frames(uint32_t rate,uint64_t elapsed_ns){
  uint64_t total=remainder+uint64_t(rate)*elapsed_ns;
  remainder=total%1000000000ULL;
  return uint32_t(total/1000000000ULL);
 }
};
