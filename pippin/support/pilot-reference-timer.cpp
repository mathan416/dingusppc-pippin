// Project: DingusPPC-Pippin
// File: pippin/support/pilot-reference-timer.cpp
// Purpose: Reference timer used by benchmarks.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// Private benchmark instrumentation for a SHA-256-pinned, unchanged reference.
#include <link.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <ctime>
static uintptr_t timer_address;
static uint64_t last_present_guest_ns;
static int locate(dl_phdr_info* info,size_t,void*) {
 if(info->dlpi_name && *info->dlpi_name)return 0;
 const char *configured=std::getenv("PIPPIN_TIMER_OFFSET");
 const uintptr_t offset=configured?std::strtoull(configured,nullptr,16):0; // atomic owner-published guest clock in private pilot
 for(unsigned i=0;i<info->dlpi_phnum;i++){
  const auto& p=info->dlpi_phdr[i];
  if(p.p_type==PT_LOAD&&(p.p_flags&PF_X)&&offset>=p.p_vaddr&&offset+4<=p.p_vaddr+p.p_memsz){timer_address=info->dlpi_addr+offset;break;}
 }
 return 1;
}
static void report(){std::fprintf(stderr,"PIPPIN-TIMER guest_ns=%llu snapshot=last_present\n",(unsigned long long)last_present_guest_ns);}

__attribute__((constructor)) static void initialize(){dl_iterate_phdr(locate,nullptr);if(!timer_address)std::abort();std::atexit(report);}

extern "C" void SDL_RenderPresent(void *renderer){
 using Present=void(*)(void*);static auto next=reinterpret_cast<Present>(dlsym(RTLD_NEXT,"SDL_RenderPresent"));
 if(!next)std::abort();next(renderer);
 timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);uint64_t now=uint64_t(ts.tv_sec)*1000+ts.tv_nsec/1000000;
 static uint64_t start=now,last=0,presents=0; ++presents;
 if(timer_address)last_present_guest_ns=reinterpret_cast<uint64_t(*)()>(timer_address)();
 if(timer_address&&now-start>=last+5000){last=now-start;std::fprintf(stderr,"PIPPIN-TIMER wall_ms=%llu guest_ns=%llu presents=%llu\n",(unsigned long long)last,(unsigned long long)last_present_guest_ns,(unsigned long long)presents);}
}
