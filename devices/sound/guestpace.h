// GPL-3.0-or-later. Estimate throughput from duration sums, not a biased mean
// of instantaneous ratios when rendering makes host frame times uneven.
#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>
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
  // Keep the existing 250 ms half-life, scaled to actual elapsed host time.
  double alpha=-std::expm1(-0.69314718055994530942*double(wall)/250000000.0);
  rate+=alpha*(observed-rate);guest=wall=0;return true;
 }
};
