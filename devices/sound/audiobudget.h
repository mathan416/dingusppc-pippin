// Project: DingusPPC-Pippin
// File: devices/sound/audiobudget.h
// Purpose: Audio frame budget across irregular guest DMA periods.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// Keep the fractional frame across irregular guest timer ticks.
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
