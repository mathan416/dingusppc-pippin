// Project: DingusPPC-Pippin
// File: pippin/support/controller/test-pointer.c
// Purpose: Unit test for pointer input.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdio.h>
static SDL_Event next_event(Uint32 type){SDL_Event e;Uint32 until=SDL_GetTicks()+500;do{while(SDL_PollEvent(&e)){assert(e.type!=SDL_QUIT);if(e.type==type)return e;}SDL_Delay(1);}while(SDL_GetTicks()<until);assert(!"Expected pointer event missing");return e;}
int main(void){assert(!SDL_Init(SDL_INIT_VIDEO));SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);
 int deltas[][2]={{1,0},{-1,0},{0,1},{0,-1},{127,-65},{-128,127},{0,0}};
 for(unsigned i=0;i<sizeof(deltas)/sizeof(*deltas);i++){SDL_Event e={0};e.type=SDL_MOUSEMOTION;e.motion.which=123;e.motion.xrel=deltas[i][0];e.motion.yrel=deltas[i][1];assert(SDL_PushEvent(&e)==1);e=next_event(SDL_MOUSEMOTION);assert(e.motion.xrel==deltas[i][0]&&e.motion.yrel==deltas[i][1]);}
 for(int b=SDL_BUTTON_LEFT;b<=SDL_BUTTON_RIGHT;b++)for(int down=0;down<2;down++){SDL_Event e={0};e.type=down?SDL_MOUSEBUTTONDOWN:SDL_MOUSEBUTTONUP;e.button.which=123;e.button.button=b;e.button.state=down?SDL_PRESSED:SDL_RELEASED;assert(SDL_PushEvent(&e)==1);SDL_Event got=next_event(e.type);assert(got.button.button==b&&got.button.state==e.button.state);}
 puts("PASS: 7 relative pointer deltas and 6 mouse button transitions preserved by cabinet adapter (virtual events only)");SDL_Quit();}
