#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void commands(void){
 static long offset=0;static Uint32 checked=0;
 if(SDL_GetTicks()-checked<10)return;checked=SDL_GetTicks();
 const char *p=getenv("DINGUS_COMMANDS");if(!p)return;
 FILE *f=fopen(p,"r");if(!f)return;fseek(f,offset,SEEK_SET);
 char line[256],type[32],name[64],state[32];
 while(fgets(line,sizeof(line),f)){
  SDL_Event e={0};int x=0,y=0;
  if(!strncmp(line,"quit",4)){e.type=SDL_QUIT;}
  else if(sscanf(line,"disconnect %d",&x)==1){e.type=SDL_CONTROLLERDEVICEREMOVED;e.cdevice.which=x;}
  else if(!strncmp(line,"focus lost",10)){e.type=SDL_WINDOWEVENT;e.window.event=SDL_WINDOWEVENT_FOCUS_LOST;}
  else if(sscanf(line,"mouse %d %d",&x,&y)==2){e.type=SDL_MOUSEMOTION;e.motion.xrel=x;e.motion.yrel=y;}
  else if(sscanf(line,"%31s %63s %31s",type,name,state)==3){
   int down=!strcmp(state,"down");
   if(!strcmp(type,"button")){e.type=down?SDL_MOUSEBUTTONDOWN:SDL_MOUSEBUTTONUP;unsigned raw=0;e.button.button=sscanf(name,"%u",&raw)==1?(Uint8)raw:!strcmp(name,"left")?SDL_BUTTON_LEFT:SDL_BUTTON_RIGHT;e.button.state=down?SDL_PRESSED:SDL_RELEASED;}
   else if(!strcmp(type,"key")){e.type=down?SDL_KEYDOWN:SDL_KEYUP;e.key.keysym.sym=(!strcmp(name,"command")?SDLK_LGUI:SDL_GetKeyFromName(name));e.key.keysym.scancode=SDL_GetScancodeFromKey(e.key.keysym.sym);e.key.state=down?SDL_PRESSED:SDL_RELEASED;}
   else if(!strcmp(type,"pad")){e.type=down?SDL_CONTROLLERBUTTONDOWN:SDL_CONTROLLERBUTTONUP;e.cbutton.button=SDL_GameControllerGetButtonFromString(name);e.cbutton.state=down?SDL_PRESSED:SDL_RELEASED;int id=0;if(sscanf(line,"%*s %*s %*s %d",&id)==1)e.cbutton.which=id;}
  }
  if(e.type){SDL_PushEvent(&e);fprintf(stderr,"TEST_INPUT wall_ms=%u %s",SDL_GetTicks(),line);}
 }
 offset=ftell(f);fclose(f);
}

void SDL_RenderPresent(SDL_Renderer *r) {
 static void (*real_present)(SDL_Renderer*)=NULL;
 static Uint32 last=0;
 if (!real_present) real_present=dlsym(RTLD_NEXT,"SDL_RenderPresent");
 commands();
 const char *path=getenv("DINGUS_CAPTURE");
 if(path && SDL_GetTicks()-last>=1000){
  int w=0,h=0;SDL_GetRendererOutputSize(r,&w,&h);
  SDL_Surface *s=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_ARGB8888);
  if(s){if(!SDL_RenderReadPixels(r,NULL,s->format->format,s->pixels,s->pitch)){char name[1024];snprintf(name,sizeof(name),"%s-%06u.bmp",path,SDL_GetTicks()/1000);if((SDL_GetTicks()/1000)%5==0)SDL_SaveBMP(s,name);snprintf(name,sizeof(name),"%s-latest.bmp",path);SDL_SaveBMP(s,name);}SDL_FreeSurface(s);}
  last=SDL_GetTicks();fprintf(stderr,"DINGUS_CAPTURE wall_ms=%u\n",last);fflush(stderr);
 }
 real_present(r);
}
