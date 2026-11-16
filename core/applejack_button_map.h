// GPL-3.0-or-later. SDL buttons without AppleJack equivalents are ignored.
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
