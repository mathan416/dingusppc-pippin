// Project: DingusPPC-Pippin
// File: cpu/ppc/pippin_fastmem.h
// Purpose: Fastmem interface.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
//   2026-11-16 - Hosts without 4 KB pages fall back to the software path, with a log line; pippin_fastmem_host_supported().
// Full history: PIPPIN.md and Git history.
#pragma once
#include <cstddef>
#include <cstdint>
// All mappings belong to this emulator process. Physical buffers remain the
// canonical memory-controller storage; aliases never copy guest payloads.
// True when the host page size is 4 KB, which fastmem requires.
bool pippin_fastmem_host_supported();
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
