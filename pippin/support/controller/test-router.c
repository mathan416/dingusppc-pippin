// Project: DingusPPC-Pippin
// File: pippin/support/controller/test-router.c
// Purpose: Unit test for Controller Router input.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int attach(const char *name,int product){SDL_VirtualJoystickDesc d={0};d.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;d.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;d.naxes=4;d.nbuttons=15;d.nhats=1;d.vendor_id=0x1209;d.product_id=product;d.name=name;return SDL_JoystickAttachVirtualEx(&d);}
static void drain(void){SDL_Delay(15);SDL_Event e;while(SDL_PollEvent(&e))assert(e.type!=SDL_QUIT);}
static void button(SDL_Joystick*j,int raw,int expected){SDL_Event e;int down=0,up=0;SDL_JoystickSetVirtualButton(j,raw,1);SDL_Delay(15);SDL_PumpEvents();while(SDL_PollEvent(&e)){assert(e.type!=SDL_QUIT);if(e.type==SDL_CONTROLLERBUTTONDOWN){assert(e.cbutton.button==expected);down++;}}
 SDL_JoystickSetVirtualButton(j,raw,0);SDL_Delay(raw==12?300:15);SDL_PumpEvents();while(SDL_PollEvent(&e)){assert(e.type!=SDL_QUIT);if(e.type==SDL_CONTROLLERBUTTONUP){assert(e.cbutton.button==expected);up++;}}fprintf(stderr,"raw=%d expected=%d down=%d up=%d\n",raw,expected,down,up);assert(down==1&&up==1);}
int main(void){SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)==0);
 int other=attach("VirtualGlove Merged Player 2",0x5652),p=attach("VirtualGlove Merged Player 1",0x5651);assert(other>=0&&p>=0);assert(!SDL_IsGameController(other));assert(!SDL_GameControllerOpen(other));assert(SDL_IsGameController(p));
 SDL_GameController*c=SDL_GameControllerOpen(p);assert(c);SDL_Joystick*j=SDL_GameControllerGetJoystick(c);drain();
 int raw[]={0,1,3,4,6,7,10,11,12};int want[]={SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_Y,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_BACK,SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_GUIDE};
 for(unsigned i=0;i<sizeof(raw)/sizeof(*raw);i++)button(j,raw[i],want[i]);
 for(int h=0;h<4;h++){int masks[]={1,2,4,8},buttons[]={SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_RIGHT,SDL_CONTROLLER_BUTTON_DPAD_DOWN,SDL_CONTROLLER_BUTTON_DPAD_LEFT};SDL_JoystickSetVirtualHat(j,0,masks[h]);SDL_Delay(15);SDL_Event e;int n=0;while(SDL_PollEvent(&e))if(e.type==SDL_CONTROLLERBUTTONDOWN){assert(e.cbutton.button==buttons[h]);n++;}assert(n==1);SDL_JoystickSetVirtualHat(j,0,0);drain();}
 SDL_Event e={0};e.type=SDL_CONTROLLERBUTTONDOWN;e.cbutton.which=9999;e.cbutton.button=SDL_CONTROLLER_BUTTON_A;SDL_PushEvent(&e);while(SDL_PollEvent(&e))assert(e.type!=SDL_CONTROLLERBUTTONDOWN);
 SDL_JoystickSetVirtualButton(j,0,1);SDL_JoystickSetVirtualButton(j,10,1);drain();assert(SDL_JoystickDetachVirtual(p)==0);SDL_Delay(20);int released=0;while(SDL_PollEvent(&e)){assert(e.type!=SDL_QUIT);if(e.type==SDL_CONTROLLERBUTTONUP)released++;}assert(released>=2);
 p=attach("VirtualGlove Merged Player 1",0x5651);drain();assert(SDL_IsGameController(p));c=SDL_GameControllerOpen(p);assert(c);j=SDL_GameControllerGetJoystick(c);
 SDL_JoystickSetVirtualButton(j,11,1);drain();SDL_Delay(1100);drain();SDL_JoystickSetVirtualButton(j,10,1);drain();SDL_Delay(1100);assert(SDL_PollEvent(&e)&&e.type==SDL_QUIT);
 puts("PASS: Player 1 selection, all mapped buttons/directions, rejection of Player 2, disconnect release, reconnect, fresh exit combo");SDL_Quit();return 0;}
