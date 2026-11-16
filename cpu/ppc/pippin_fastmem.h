// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
// All mappings belong to this emulator process. Physical buffers remain the
// canonical memory-controller storage; aliases never copy guest payloads.
bool pippin_fastmem_enabled();
uint8_t *pippin_fastmem_alloc(size_t);
bool pippin_fastmem_release(uint8_t*);
void pippin_fastmem_select(unsigned);
void pippin_fastmem_revoke(unsigned,uint32_t);
void pippin_fastmem_publish(unsigned,unsigned,uint32_t,uint16_t,uintptr_t,uintptr_t);
extern uintptr_t pippin_fastmem_read,pippin_fastmem_write;
void pippin_fastmem_code(void*,size_t);
void pippin_fastmem_clear(unsigned);
struct PippinFastmemFault{uintptr_t pc,slow;bool write;};
bool pippin_fastmem_faults(const PippinFastmemFault*,size_t);
unsigned pippin_fastmem_fault_count();

void pippin_fastmem_mmu(uintptr_t,uintptr_t);
void pippin_fastmem_poll();

using PippinFastmemRefresh=void(*)(unsigned,uint32_t);
void pippin_fastmem_refresher(PippinFastmemRefresh);
