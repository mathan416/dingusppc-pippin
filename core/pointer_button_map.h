// Project: DingusPPC-Pippin
// File: core/pointer_button_map.h
// Purpose: Map SDL mouse buttons to the ADB mouse button mask.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// EventManager stores an eight-bit ADB mouse mask.
#pragma once
#include <cstdint>
inline bool pippin_pointer_button(uint8_t button,uint8_t& mask){
 if(button<1 || button>8)return false;
 unsigned bit=button==2?2:button==3?1:button-1;
 mask=uint8_t(1u<<bit);return true;
}
