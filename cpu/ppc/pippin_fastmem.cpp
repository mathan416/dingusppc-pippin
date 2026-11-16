// SPDX-License-Identifier: GPL-3.0-or-later
// Process-private ARM64 fastmem. Missing/protected pages resume the unchanged
// software guards; only registered JIT payload PCs can use fault recovery.
#include "pippin_fastmem.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <map>
#include <unordered_map>
#if defined(__aarch64__) && defined(__linux__)
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
namespace {
constexpr size_t page=4096,space=size_t(1)<<32,modes=4,slots=4096,fault_capacity=131072;
struct Physical {uint8_t *base;size_t size;int fd;};
struct Mapping {uint32_t va=0xffffffffu;bool read=false,write=false;int read_fd=-1,write_fd=-1;uintptr_t read_host=0,write_host=0;uint64_t age=0;};
using Fault=PippinFastmemFault;
static_assert(std::atomic<bool>::is_always_lock_free,"Signal flags must be lock-free");
static_assert(std::atomic<size_t>::is_always_lock_free,"Signal fault table must be lock-free");
struct FaultEntry{Fault record{};volatile sig_atomic_t patch=0;};
struct Bank {std::array<FaultEntry,fault_capacity> entries{};std::atomic<size_t> size{0};};
struct Manager {
 uint8_t *arena=nullptr;bool usable=false;unsigned selected=3;
 std::vector<std::unique_ptr<Physical>> physical;
 std::array<std::unordered_map<uint32_t,Mapping>,modes> maps{};
 std::map<uint64_t,std::pair<unsigned,uint32_t>> age;uint64_t serial=0;
 struct Repair{unsigned mode;uint32_t va;};std::array<Repair,64> repairs{};volatile sig_atomic_t repair_count=0;PippinFastmemRefresh refresh=nullptr;
 std::array<Bank,2> banks{};uintptr_t code=0;size_t code_size=0;
 struct sigaction previous{};bool installed=false;volatile sig_atomic_t faults=0;
 uint64_t published=0,invalidated=0,patched=0;uintptr_t dtlb2_slot=0,mask_slot=0;std::atomic<bool> pending_patch{false};
 uintptr_t base(unsigned mode,bool write)const{return uintptr_t(arena)+(mode*2+unsigned(write))*space;}
 void none(uintptr_t address){
  void *result=mmap(reinterpret_cast<void*>(address),page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_NORESERVE,-1,0);
  if(result!=reinterpret_cast<void*>(address))throw std::runtime_error("Fastmem alias invalidation failed");
 }
 void revoke(unsigned mode,uint32_t va){
  if(mode>=modes)return;auto found=maps[mode].find(va);if(found==maps[mode].end())return;auto &r=found->second;
  if(r.read)none(base(mode,false)+r.va);if(r.write)none(base(mode,true)+r.va);
  if(r.read||r.write)invalidated++;age.erase(r.age);maps[mode].erase(found);
 }
 int alias(uintptr_t target,uintptr_t host,bool write){
  for(auto &p:physical)if(p){
   uintptr_t first=uintptr_t(p->base);
   if(host<first||host-first>p->size||p->size-(host-first)<page||((host-first)&(page-1)))continue;
   void *result=mmap(reinterpret_cast<void*>(target),page,write?(PROT_READ|PROT_WRITE):PROT_READ,MAP_SHARED|MAP_FIXED,p->fd,off_t(host-first));
   if(result!=reinterpret_cast<void*>(target))throw std::runtime_error("Fastmem physical alias mapping failed");return p->fd;
  }
  return -1;
 }
 Manager(){
  const char *setting=std::getenv("SHEEP_PIPPIN_FASTMEM");if(!setting||std::strcmp(setting,"on")||sysconf(_SC_PAGESIZE)!=long(page))return;
  void *result=mmap(nullptr,space*modes*2,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
  if(result==MAP_FAILED){fprintf(stderr,"PIPPIN FASTMEM unavailable: arena errno=%d\n",errno);return;}
  arena=static_cast<uint8_t*>(result);usable=true;
  pippin_fastmem_read=base(3,false);pippin_fastmem_write=base(3,true);
  fprintf(stderr,"PIPPIN FASTMEM private arenas ready (host page=4096)\n");
 }
};
static Manager *instance=nullptr;
static Manager &manager(){if(!instance)instance=new Manager;return *instance;}
static void statistics(){if(instance)fprintf(stderr,"PIPPIN FASTMEM usable=%u physical=%zu published=%llu invalidated=%llu guarded_faults=%d patched=%llu\n",instance->usable,instance->physical.size(),(unsigned long long)instance->published,(unsigned long long)instance->invalidated,int(instance->faults),(unsigned long long)instance->patched);}
static void fault_handler(int signal,siginfo_t *info,void *context){
 auto *m=instance;auto *cpu=static_cast<ucontext_t*>(context);uintptr_t pc=cpu->uc_mcontext.pc,address=uintptr_t(info->si_addr);
 if(m&&m->installed&&pc>=m->code&&pc-m->code<m->code_size){
  unsigned bank=unsigned((pc-m->code)/(m->code_size/2));auto &b=m->banks[bank];size_t count=b.size.load(std::memory_order_acquire),lo=0,hi=count;
  while(lo<hi){size_t mid=lo+(hi-lo)/2;if(b.entries[mid].record.pc<pc)lo=mid+1;else hi=mid;}
  if(lo<count&&b.entries[lo].record.pc==pc){auto &entry=b.entries[lo];const auto &r=entry.record;uintptr_t start=r.write?pippin_fastmem_write:pippin_fastmem_read;
   if(start&&address>=start&&address-start<space){if(m->faults<INT_MAX)m->faults=m->faults+1;
    uint32_t page_va=uint32_t(address-start)&~4095u;unsigned repair_count=unsigned(m->repair_count);
    if(repair_count<m->repairs.size()&&(!repair_count||m->repairs[repair_count-1].mode!=m->selected||m->repairs[repair_count-1].va!=page_va)){m->repairs[repair_count]={m->selected,page_va};m->repair_count=sig_atomic_t(repair_count+1);}

    // A confirmed device/unmapped secondary entry can use the software path
    // directly on later calls. A mere primary miss never disables fast RAM.
    if(m->dtlb2_slot&&m->mask_slot){uintptr_t table=*reinterpret_cast<const uintptr_t*>(m->dtlb2_slot);uint32_t mask=*reinterpret_cast<const uint32_t*>(m->mask_slot);uint32_t va=uint32_t(address-start),tag=va&~4095u;
     if(table&&mask<slots)for(unsigned way=0;way<4;way++){auto *slot=reinterpret_cast<const uint8_t*>(table+(((va>>12)&mask)*4+way)*32);uint32_t found=*reinterpret_cast<const uint32_t*>(slot);uint16_t flags=*reinterpret_cast<const uint16_t*>(slot+4);
      if(found==tag&&(flags&6)){entry.patch=1;m->pending_patch.store(true,std::memory_order_release);break;}
     }
    }
    cpu->uc_mcontext.pc=r.slow;return;}
  }
 }
 // Never absorb a fault from unrelated code or an unregistered payload PC.
 if(m&&(m->previous.sa_flags&SA_SIGINFO)&&m->previous.sa_sigaction){m->previous.sa_sigaction(signal,info,context);return;}
 if(m&&m->previous.sa_handler!=SIG_DFL&&m->previous.sa_handler!=SIG_IGN&&m->previous.sa_handler){m->previous.sa_handler(signal);return;}
 struct sigaction action{};action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);sigaction(signal,&action,nullptr);raise(signal);
}
}
uintptr_t pippin_fastmem_read=0,pippin_fastmem_write=0;
bool pippin_fastmem_enabled(){return manager().usable;}
uint8_t *pippin_fastmem_alloc(size_t size){
 auto &m=manager();if(!m.usable||!size||(size&(page-1)))return nullptr;
 int fd=int(syscall(SYS_memfd_create,"pippin-private-memory",MFD_CLOEXEC));if(fd<0)return nullptr;
 if(ftruncate(fd,off_t(size))){close(fd);return nullptr;}
 void *result=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(result==MAP_FAILED){close(fd);return nullptr;}
 m.physical.push_back(std::make_unique<Physical>(Physical{static_cast<uint8_t*>(result),size,fd}));return static_cast<uint8_t*>(result);
}
bool pippin_fastmem_release(uint8_t *pointer){
 auto &m=manager();for(auto &p:m.physical)if(p&&p->base==pointer){
  // Guest execution has stopped before memory-controller destruction.
  for(unsigned mode=0;mode<modes;mode++){
   for(auto it=m.maps[mode].begin();it!=m.maps[mode].end();){auto &r=it->second;
    if(r.read_fd==p->fd){m.none(m.base(mode,false)+r.va);r.read=false;r.read_fd=-1;r.read_host=0;}
    if(r.write_fd==p->fd){m.none(m.base(mode,true)+r.va);r.write=false;r.write_fd=-1;r.write_host=0;}
    if(!r.read&&!r.write){m.age.erase(r.age);it=m.maps[mode].erase(it);}else ++it;
   }
  }
  munmap(p->base,p->size);close(p->fd);p.reset();return true;
 }return false;
}
void pippin_fastmem_select(unsigned mode){
 auto &m=manager();if(!m.usable)return;if(mode>=modes)mode=3;
 // Unknown fixture contexts never inherit translations from a previous table.
 if(mode==3)while(!m.maps[3].empty())m.revoke(3,m.maps[3].begin()->first);
 m.selected=mode;pippin_fastmem_read=m.base(mode,false);pippin_fastmem_write=m.base(mode,true);
}
void pippin_fastmem_revoke(unsigned mode,uint32_t va){auto&m=manager();if(m.usable)m.revoke(mode,va);}
void pippin_fastmem_publish(unsigned mode,unsigned slot,uint32_t va,uint16_t flags,uintptr_t read,uintptr_t write){
 auto&m=manager();if(!m.usable||mode>=modes||slot>=slots||(va&(page-1)))return;
 uintptr_t want_read=(flags&1)?read:0,want_write=(flags&0x61)==0x61?write:0;
 auto found=m.maps[mode].find(va);
 if(found!=m.maps[mode].end()&&found->second.read_host==want_read&&found->second.write_host==want_write)return;
 m.revoke(mode,va);if(!want_read&&!want_write)return;
 // Bound resident aliases even when a guest touches a large sparse address space.
 while(m.age.size()>=8192){auto victim=m.age.begin()->second;m.revoke(victim.first,victim.second);}
 Mapping r;r.va=va;
 if(want_read){r.read_fd=m.alias(m.base(mode,false)+va,read,false);r.read=r.read_fd>=0;if(r.read)r.read_host=read;}
 if(want_write){r.write_fd=m.alias(m.base(mode,true)+va,write,true);r.write=r.write_fd>=0;if(r.write)r.write_host=write;}
 if(r.read||r.write){r.age=++m.serial;m.maps[mode].emplace(va,r);m.age.emplace(r.age,std::make_pair(mode,va));m.published++;}

}
void pippin_fastmem_code(void *code,size_t size){
 auto &m=manager();if(!m.usable)return;m.code=uintptr_t(code);m.code_size=size;
 if(!m.installed){struct sigaction action{};sigemptyset(&action.sa_mask);action.sa_sigaction=fault_handler;action.sa_flags=SA_SIGINFO;
  if(sigaction(SIGSEGV,&action,&m.previous))throw std::runtime_error("Fastmem signal handler installation failed");m.installed=true;std::atexit(statistics);
 }
}
void pippin_fastmem_clear(unsigned bank){auto&m=manager();if(bank<2)m.banks[bank].size.store(0,std::memory_order_release);}
bool pippin_fastmem_faults(const PippinFastmemFault *records,size_t added){
 auto&m=manager();if(!added)return true;if(!m.usable||!m.installed)return false;
 auto pc=records[0].pc;if(pc<m.code||pc-m.code>=m.code_size)return false;
 unsigned bank=unsigned((pc-m.code)/(m.code_size/2));auto &b=m.banks[bank];size_t count=b.size.load(std::memory_order_relaxed);
 if(added>fault_capacity-count||(count&&pc<=b.entries[count-1].record.pc))return false;
 for(size_t i=0;i<added;i++){
  const auto &r=records[i];if(r.pc<m.code||r.pc-m.code>=m.code_size||unsigned((r.pc-m.code)/(m.code_size/2))!=bank||(i&&r.pc<=records[i-1].pc)||r.slow<m.code||r.slow-m.code>=m.code_size)return false;
 }
 for(size_t i=0;i<added;i++){b.entries[count+i].record=records[i];b.entries[count+i].patch=0;}b.size.store(count+added,std::memory_order_release);return true;
}
void pippin_fastmem_mmu(uintptr_t secondary,uintptr_t mask){auto &m=manager();m.dtlb2_slot=secondary;m.mask_slot=mask;}
void pippin_fastmem_refresher(PippinFastmemRefresh refresh){manager().refresh=refresh;}
void pippin_fastmem_poll(){
 auto &m=manager();if(!m.usable)return;
 unsigned count=unsigned(m.repair_count);m.repair_count=0;
 if(m.refresh)for(unsigned i=0;i<count;i++)if(m.repairs[i].mode<3)m.refresh(m.repairs[i].mode,m.repairs[i].va);
 if(!m.pending_patch.exchange(false,std::memory_order_acquire))return;
 for(auto &bank:m.banks){size_t count=bank.size.load(std::memory_order_acquire);for(size_t i=0;i<count;i++){auto &entry=bank.entries[i];if(entry.patch!=1)continue;
  int64_t delta=(int64_t(entry.record.slow)-int64_t(entry.record.pc))/4;
  if(delta<-(int64_t(1)<<25)||delta>=(int64_t(1)<<25))throw std::runtime_error("Fastmem software branch range");
  auto *where=reinterpret_cast<uint32_t*>(entry.record.pc);*where=0x14000000u|(uint32_t(delta)&0x03ffffffu);
  __builtin___clear_cache(reinterpret_cast<char*>(where),reinterpret_cast<char*>(where+1));entry.patch=2;m.patched++;
 }}
}
unsigned pippin_fastmem_fault_count(){auto&m=manager();return unsigned(m.faults);}
#else
uintptr_t pippin_fastmem_read=0,pippin_fastmem_write=0;
bool pippin_fastmem_enabled(){return false;}uint8_t*pippin_fastmem_alloc(size_t){return nullptr;}bool pippin_fastmem_release(uint8_t*){return false;}
void pippin_fastmem_select(unsigned){}void pippin_fastmem_revoke(unsigned,uint32_t){}void pippin_fastmem_publish(unsigned,unsigned,uint32_t,uint16_t,uintptr_t,uintptr_t){}
void pippin_fastmem_refresher(PippinFastmemRefresh){}void pippin_fastmem_mmu(uintptr_t,uintptr_t){}void pippin_fastmem_poll(){}void pippin_fastmem_code(void*,size_t){}void pippin_fastmem_clear(unsigned){}bool pippin_fastmem_faults(const PippinFastmemFault*,size_t n){return n==0;}unsigned pippin_fastmem_fault_count(){return 0;}
#endif
