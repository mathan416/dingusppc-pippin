#include <SDL2/SDL.h>
#include <assert.h>
#include <string.h>
static void pad(int id,int b){SDL_Event e={0};e.type=SDL_CONTROLLERBUTTONDOWN;e.cbutton.which=id;e.cbutton.button=b;SDL_PushEvent(&e);}
int main(int argc,char**argv){assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)==0);SDL_Event e;SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);
 if(argc>1&&!strcmp(argv[1],"f4")){memset(&e,0,sizeof(e));e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_F4;SDL_PushEvent(&e);assert(SDL_PollEvent(&e)&&e.type==SDL_QUIT);}
 else {pad(41,SDL_CONTROLLER_BUTTON_BACK);pad(42,SDL_CONTROLLER_BUTTON_START);while(SDL_PollEvent(&e))assert(e.type!=SDL_QUIT);SDL_Delay(1100);while(SDL_PollEvent(&e))assert(e.type!=SDL_QUIT);pad(41,SDL_CONTROLLER_BUTTON_START);while(SDL_PollEvent(&e))assert(e.type!=SDL_QUIT);SDL_Delay(1100);assert(SDL_PollEvent(&e)&&e.type==SDL_QUIT);}
 SDL_Quit();return 0;}
