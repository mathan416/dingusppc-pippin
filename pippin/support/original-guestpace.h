// GPL-3.0-or-later. Estimate throughput from duration sums, not a biased mean
// of instantaneous ratios when rendering makes host frame times uneven.
#pragma once
#include <cstdint>
#include <algorithm>
struct GuestPace {
 uint64_t guest=0,wall=0;
 bool primed=false;
 double rate=1;
 bool update(uint64_t guest_ns,uint64_t wall_ns){
  guest+=guest_ns;wall+=wall_ns;
  if(!wall)return false;
  if(!primed && wall>=20000000){rate=std::clamp(double(guest)/wall,.025,1.0);primed=true;}
  if(wall<250000000)return false;
  double observed=std::clamp(double(guest)/wall,.025,1.0);
  rate=.5*rate+.5*observed;guest=wall=0;return true;
 }
};
