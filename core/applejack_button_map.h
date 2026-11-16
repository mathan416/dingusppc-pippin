// Project: DingusPPC-Pippin
// File: core/applejack_button_map.h
// Purpose: Map SDL game-controller buttons to AppleJack buttons.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// SDL buttons without AppleJack equivalents are ignored.
#pragma once
#include <SDL.h>
#include <core/hostevents.h>
inline bool map_applejack_button(Uint8 button,uint8_t& mapped){
                switch (button) {
                    case SDL_CONTROLLER_BUTTON_BACK:          mapped = GamepadButton::FrontLeft;    return true;
                    case SDL_CONTROLLER_BUTTON_GUIDE:         mapped = GamepadButton::FrontMiddle;  return true;
                    case SDL_CONTROLLER_BUTTON_START:         mapped = GamepadButton::FrontRight;   return true;
                    case SDL_CONTROLLER_BUTTON_Y:             mapped = GamepadButton::Blue;         return true;
                    case SDL_CONTROLLER_BUTTON_X:             mapped = GamepadButton::Yellow;       return true;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:       mapped = GamepadButton::Up;           return true;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     mapped = GamepadButton::Left;         return true;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    mapped = GamepadButton::Right;        return true;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     mapped = GamepadButton::Down;         return true;
                    case SDL_CONTROLLER_BUTTON_A:             mapped = GamepadButton::Red;          return true;
                    case SDL_CONTROLLER_BUTTON_B:             mapped = GamepadButton::Green;        return true;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: mapped = GamepadButton::RightTrigger; return true;
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  mapped = GamepadButton::LeftTrigger;  return true;
                    default: return false;
                }
}
