/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-26 The DingusPPC Development Team
          (See CREDITS.MD for more details)

(You may also contact divingkxt or powermax2286 on Discord)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * DingusPPC-Pippin modifications
 * Copyright (c) 2026 Iain Bennett
 * Licensed under the same terms as above.
 * Change log:
 *   2026-11-15 - Q219 baseline (Q48 to Q219, before version control): split MMU fast paths, fastmem publication, ROM window.
 *   2026-11-15 - Q227: precise tlbie (congruence class) and ROM window kept across same-mode MMU changes.
 * Full history: PIPPIN.md and Git history.
 */

/** @file PowerPC Memory Management Unit emulation. */

#include <core/memaccess.h>
#include <devices/memctrl/memctrlbase.h>
#include <devices/memctrl/aspen.h>
#include <devices/common/mmiodevice.h>
#include "ppcemu.h"
#include "pippin_fastmem.h"
#include "ppcmmu.h"

#include <array>
#include <cinttypes>
#include <loguru.hpp>
#include <stdexcept>
#include <vector>
#include <typeinfo>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

//#define MMU_PROFILING // uncomment this to enable MMU profiling
//#define TLB_PROFILING // uncomment this to enable SoftTLB profiling

/* pointer to exception handler to be called when a MMU exception is occurred. */
void (*mmu_exception_handler)(Except_Type exception_type, uint32_t srr1_bits);

/* pointers to BAT update functions. */
std::function<void(uint32_t bat_reg)> ibat_update;
std::function<void(uint32_t bat_reg)> dbat_update;

/** PowerPC-style MMU BAT arrays (NULL initialization isn't prescribed). */
PPC_BAT_entry ibat_array[4] = {{0}};
PPC_BAT_entry dbat_array[4] = {{0}};

static SheepROMWindow sheep_rom_window{};
static bool sheep_rom_window_enabled = false;
static MemCtrlBase* sheep_rom_window_owner = nullptr;
static_assert(offsetof(SheepROMWindow, guest_start) == 0 &&
              offsetof(SheepROMWindow, valid_start_count) == 4 &&
              offsetof(SheepROMWindow, host_offset) == 8 &&
              offsetof(SheepROMWindow, cpu_base) == 16, "ROM window offsets");

const SheepROMWindow* sheep_mmu_rom_window_get() noexcept { return &sheep_rom_window; }
// Q227 churn counters (CPU worker only), reported at exit.
static uint64_t q227_window_invalidations=0,q227_mode_calls=0,q227_mode_itlb_changes=0,
    q227_tlbie_calls=0,q227_tlbie_invalidated=0,q227_tlbie_scanned=0,q227_pat_ctx_changes=0,q227_full_flushes=0;
static bool q227_env_on(const char *name){const char *s=std::getenv(name);return !s||std::strcmp(s,"off");}
static const bool q227_precise_tlbie=q227_env_on("SHEEP_PIPPIN_PRECISE_TLBIE");
static const bool q227_mode_window=q227_env_on("SHEEP_PIPPIN_MODE_WINDOW");
static struct Q227Report{~Q227Report(){fprintf(stderr,"PIPPIN-MMU-CHURN precise_tlbie=%u mode_window=%u window_invalidations=%llu mode_calls=%llu itlb_mode_changes=%llu tlbie=%llu tlbie_invalidated=%llu tlbie_scanned=%llu full_pat_flushes=%llu pat_ctx_changes=%llu\n",
    unsigned(q227_precise_tlbie),unsigned(q227_mode_window),(unsigned long long)q227_window_invalidations,(unsigned long long)q227_mode_calls,(unsigned long long)q227_mode_itlb_changes,
    (unsigned long long)q227_tlbie_calls,(unsigned long long)q227_tlbie_invalidated,(unsigned long long)q227_tlbie_scanned,(unsigned long long)q227_full_flushes,(unsigned long long)q227_pat_ctx_changes);}} q227_report;
void sheep_mmu_rom_window_invalidate() noexcept {
    // CPU worker owns both writers and consumers. No cross-thread snapshot.
    q227_window_invalidations++;
    sheep_rom_window.valid_start_count = 0;
}
static void sheep_rom_map_changed(MemCtrlBase* owner, bool destroying) noexcept {
    if (owner != sheep_rom_window_owner) return;
    sheep_mmu_rom_window_invalidate();
    if (destroying) sheep_rom_window_owner = nullptr;
}
static bool sheep_rom_bind_owner() noexcept {
    if (!mem_ctrl_instance || typeid(*mem_ctrl_instance) != typeid(AspenCtrl))
        return false; // other controllers have direct descriptor writers
    if (sheep_rom_window_owner == mem_ctrl_instance) return true;
    if (sheep_rom_window_owner)
        sheep_rom_window_owner->unbind_instruction_map_observer(&sheep_rom_map_changed);
    sheep_rom_window_owner = nullptr;
    if (!mem_ctrl_instance->bind_instruction_map_observer(&sheep_rom_map_changed)) return false;
    sheep_rom_window_owner = mem_ctrl_instance;
    return true;
}
void sheep_mmu_rom_window_enable(bool enabled) noexcept {
    sheep_mmu_rom_window_invalidate();
    sheep_rom_window_enabled = enabled;
    if (!enabled && sheep_rom_window_owner) {
        sheep_rom_window_owner->unbind_instruction_map_observer(&sheep_rom_map_changed);
        sheep_rom_window_owner = nullptr;
    }
}


#ifdef MMU_PROFILING

/* global variables for lightweight MMU profiling */
uint64_t    dmem_reads_total   = 0; // counts reads from data memory
uint64_t    iomem_reads_total  = 0; // counts I/O memory reads
uint64_t    dmem_writes_total  = 0; // counts writes to data memory
uint64_t    iomem_writes_total = 0; // counts I/O memory writes
uint64_t    exec_reads_total   = 0; // counts reads from executable memory
uint64_t    bat_transl_total   = 0; // counts BAT translations
uint64_t    ptab_transl_total  = 0; // counts page table translations
uint64_t    pat_flushes_total  = 0; // counts full page-translation TLB flushes
uint64_t    unaligned_reads    = 0; // counts unaligned reads
uint64_t    unaligned_writes   = 0; // counts unaligned writes
uint64_t    unaligned_crossp_r = 0; // counts unaligned crosspage reads
uint64_t    unaligned_crossp_w = 0; // counts unaligned crosspage writes

#endif // MMU_PROFILING

#ifdef TLB_PROFILING

/* global variables for lightweight SoftTLB profiling */
uint64_t    num_primary_itlb_hits   = 0; // number of hits in the primary ITLB
uint64_t    num_secondary_itlb_hits = 0; // number of hits in the secondary ITLB
uint64_t    num_itlb_refills        = 0; // number of ITLB refills
uint64_t    num_primary_dtlb_hits   = 0; // number of hits in the primary DTLB
uint64_t    num_secondary_dtlb_hits = 0; // number of hits in the secondary DTLB
uint64_t    num_dtlb_refills        = 0; // number of DTLB refills
uint64_t    num_entry_replacements  = 0; // number of entry replacements

#endif // TLB_PROFILING

/** remember recently used physical memory regions for quicker translation. */
AddressMapEntry last_ptab_area;

/** Dummy pages for catching writes to physical read-only pages */
static std::array<uint64_t, 8192 / sizeof(uint64_t)> dummy_page;

#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE || SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
template <class T>
static inline uint32_t mem_munge_constant() {
    switch (sizeof(T)) {
    case 1: return 7;
    case 2: return 6;
    case 4: return 4;
    case 8: return 0;
    }
}

template <class T>
static uint32_t mem_munge_address(uint32_t guest_va) {
    if (sizeof(T) == sizeof(uint64_t)) {
        // 64 bit access. do nothing, handle later.
        return guest_va;
    }

    uint32_t align_offset = guest_va & (sizeof(T) - 1);
    if (align_offset == 0)
        return guest_va ^ mem_munge_constant<T>();

    // align the address
    guest_va &= ~(sizeof(T) - 1);
    // munge it
    guest_va ^= mem_munge_constant<T>();
    // and subtract the offset
    return guest_va - align_offset;
}
#endif

/** 601-style block address translation. */
static BATResult mpc601_block_address_translation(uint32_t la)
{
    uint32_t pa;    // translated physical address
    uint8_t  prot;  // protection bits for the translated address
    unsigned key;

    bool bat_hit    = false;
    unsigned msr_pr = !!(ppc_state.msr & MSR::PR);

    // I/O controller interface takes precedence over BAT in 601
    // Report BAT miss if T bit is set in the corresponding SR
    if (ppc_state.sr[(la >> 28) & 0x0F] & 0x80000000) {
        return BATResult{false, 0, 0};
    }

    for (int bat_index = 0; bat_index < 4; bat_index++) {
        PPC_BAT_entry* bat_entry = &ibat_array[bat_index];

        if (bat_entry->valid && ((la & bat_entry->hi_mask) == bat_entry->bepi)) {
            bat_hit = true;

            key = (((bat_entry->access & 1) & msr_pr) |
                  (((bat_entry->access >> 1) & 1) & (msr_pr ^ 1)));

            // remapping BAT access from 601-style to PowerPC-style
            static uint8_t access_conv[8] = {2, 2, 2, 1, 0, 1, 2, 1};

            prot = access_conv[(key << 2) | bat_entry->prot];

#ifdef MMU_PROFILING
            bat_transl_total++;
#endif

            // logical to physical translation
            pa = bat_entry->phys_hi | (la & ~bat_entry->hi_mask);
            return BATResult{bat_hit, prot, pa};
        }
    }

    return BATResult{bat_hit, 0, 0};
}

/** PowerPC-style block address translation. */
template <const BATType type>
static BATResult ppc_block_address_translation(uint32_t la)
{
    uint32_t pa = 0;    // translated physical address
    uint8_t  prot = 0;  // protection bits for the translated address
    PPC_BAT_entry *bat_array;

    bool bat_hit    = false;
    unsigned msr_pr = (ppc_state.msr & MSR::PR) != 0;

    bat_array = (type == BATType::IBAT) ? ibat_array : dbat_array;

    // Format: %XY
    // X - supervisor access bit, Y - problem/user access bit
    // Those bits are mutually exclusive
    unsigned access_bits = ((!msr_pr) << 1) | msr_pr;

    for (int bat_index = 0; bat_index < 4; bat_index++) {
        PPC_BAT_entry* bat_entry = &bat_array[bat_index];

        if ((bat_entry->access & access_bits) != 0 && ((la & bat_entry->hi_mask) == bat_entry->bepi)) {
            bat_hit = true;

#ifdef MMU_PROFILING
            bat_transl_total++;
#endif
            // logical to physical translation
            pa = bat_entry->phys_hi | (la & ~bat_entry->hi_mask);
            prot = bat_entry->prot;
            break;
        }
    }

    return BATResult{bat_hit, prot, pa};
}

static inline uint8_t* calc_pteg_addr(uint32_t hash)
{
    uint32_t sdr1_val, pteg_addr;

    sdr1_val = ppc_state.spr[SPR::SDR1];

    pteg_addr = (sdr1_val & 0xFFFF0000) |
        (((sdr1_val & 0x1FF) << 16) & ((hash & 0x7FC00) << 6)) |
        ((hash & 0x3FF) << 6);
#if 0 && SUPPORTS_PPC_LITTLE_ENDIAN_MODE
    if (ppc_state.is_LE)
        pteg_addr ^= mem_munge_constant<uint32_t>();
#endif

    if (pteg_addr >= last_ptab_area.start && pteg_addr <= last_ptab_area.end) {
        return last_ptab_area.mem_ptr + (pteg_addr - last_ptab_area.start);
    } else {
        AddressMapEntry* entry = mem_ctrl_instance->find_range(pteg_addr);
        if (entry && entry->type & (RT_ROM | RT_RAM)) {
            last_ptab_area.start   = entry->start;
            last_ptab_area.end     = entry->end;
            last_ptab_area.mem_ptr = entry->mem_ptr;
            return last_ptab_area.mem_ptr + (pteg_addr - last_ptab_area.start);
        } else {
            ABORT_F("SOS: no page table region was found at %08X!\n", pteg_addr);
        }
    }
}

static bool search_pteg(uint8_t* pteg_addr, uint8_t** ret_pte_addr, uint32_t vsid,
                        uint16_t page_index, uint8_t pteg_num)
{
    /* construct PTE matching word */
    uint32_t pte_check = 0x80000000 | (vsid << 7) | (pteg_num << 6) | (page_index >> 10);
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    bool swap = mem_ctrl_instance->needs_swap_endian(false);
#endif

#ifdef MMU_INTEGRITY_CHECKS
    /* PTEG integrity check that ensures that all matching PTEs have
     identical RPN, WIMG and PP bits (PPC PEM 32-bit 7.6.2, rule 5). */
    uint32_t pte_word2_check;
    bool match_found = false;

    for (int i = 0; i < 8; i++, pteg_addr += 8) {
        if (pte_check == READ_DWORD_BE_A(pteg_addr)) {
            if (match_found) {
                if ((READ_DWORD_BE_A(pteg_addr) & 0xFFFFF07B) != pte_word2_check) {
                    ABORT_F("Multiple PTEs with different RPN/WIMG/PP found!\n");
                }
            } else {
                /* isolate RPN, WIMG and PP fields */
                pte_word2_check = READ_DWORD_BE_A(pteg_addr) & 0xFFFFF07B;
                *ret_pte_addr   = pteg_addr;
            }
        }
    }
#else
    for (int i = 0; i < 8; i++, pteg_addr += 8) {
        if (pte_check == (
            #if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                swap ? (READ_DWORD_LE_A(pteg_addr)) :
            #endif
            (READ_DWORD_BE_A(pteg_addr))
        )) {
            *ret_pte_addr = pteg_addr;
            return true;
        }
    }
#endif

    return false;
}

static PATResult page_address_translation(uint32_t la, bool is_instr_fetch,
                                          unsigned msr_pr, int is_write)
{
    uint32_t sr_val, page_index, pteg_hash1, vsid, pte_word2;
    unsigned key, pp;
    uint8_t* pte_addr;

    sr_val = ppc_state.sr[(la >> 28) & 0x0F];
    if (sr_val & 0x80000000) {
        // check for 601-specific memory-forced I/O segments
        if (((sr_val >> 20) & 0x1FF) == 0x7F) {
            return PATResult{
                (la & 0x0FFFFFFF) | (sr_val << 28),
                0, // prot = read/write
                1  // no C bit updates
            };
        } else {
            ABORT_F("Direct-store segments not supported, LA=0x%X\n", la);
        }
    }

    /* instruction fetch from a no-execute segment will cause ISI exception */
    if ((sr_val & 0x10000000) && is_instr_fetch) {
        mmu_exception_handler(Except_Type::EXC_ISI, 0x10000000);
    }

    page_index = (la >> 12) & 0xFFFF;
    pteg_hash1 = (sr_val & 0x7FFFF) ^ page_index;
    vsid       = sr_val & 0x0FFFFFF;

    if (!search_pteg(calc_pteg_addr(pteg_hash1), &pte_addr, vsid, page_index, 0)) {
        if (!search_pteg(calc_pteg_addr(~pteg_hash1), &pte_addr, vsid, page_index, 1)) {
            if (is_instr_fetch) {
                mmu_exception_handler(Except_Type::EXC_ISI, 0x40000000);
            } else {
                ppc_state.spr[SPR::DSISR] = 0x40000000 | (is_write << 25);
                ppc_state.spr[SPR::DAR]   = la;
                mmu_exception_handler(Except_Type::EXC_DSI, 0);
            }
        }
    }

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    uintptr_t pte_addr2S = (uintptr_t)pte_addr;
    #if 0 && SUPPORTS_PPC_LITTLE_ENDIAN_MODE
    if (ppc_state.is_LE) {
        pte_addr2S ^= mem_munge_constant<uint32_t>();
        pte_addr2S += 4;
        pte_addr2S ^= mem_munge_constant<uint32_t>();
    } else
    #endif
    {
        pte_addr2S += 4;
    }

    uint8_t* pte_addr2 = (uint8_t*)pte_addr2S;
    bool swap = mem_ctrl_instance->needs_swap_endian(false);
    pte_word2 = swap ? (READ_DWORD_LE_A(pte_addr2)) : (READ_DWORD_BE_A(pte_addr2));
#else
    pte_word2 = READ_DWORD_BE_A(pte_addr + 4);
#endif

    key = (((sr_val >> 29) & 1) & msr_pr) | (((sr_val >> 30) & 1) & (msr_pr ^ 1));

    /* check page access */
    pp = pte_word2 & 3;

    // the following scenarios cause DSI/ISI exception:
    // any access with key = 1 and PP = %00
    // write access with key = 1 and PP = %01
    // write access with PP = %11
    if ((key && (!pp || (pp == 1 && is_write))) || (pp == 3 && is_write)) {
        if (is_instr_fetch) {
            mmu_exception_handler(Except_Type::EXC_ISI, 0x08000000);
        } else {
            ppc_state.spr[SPR::DSISR] = 0x08000000 | (is_write << 25);
            ppc_state.spr[SPR::DAR]   = la;
            mmu_exception_handler(Except_Type::EXC_DSI, 0);
        }
    }

    /* update R and C bits */
    /* For simplicity, R is set on each access, C is set only for writes */
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    if (swap) {
        pte_addr2[1] |= 0x01;
        if (is_write) {
            pte_addr2[0] |= 0x80;
        }
    } else {
        pte_addr2[2] |= 0x01;
        if (is_write) {
            pte_addr2[3] |= 0x80;
        }
    }
#else
    pte_addr[6] |= 0x01;
    if (is_write)
        pte_addr[7] |= 0x80;
#endif

    /* return physical address, access protection and C status */
    return PATResult{
        ((pte_word2 & 0xFFFFF000) | (la & 0x00000FFF)),
        static_cast<uint8_t>((key << 2) | pp),
        static_cast<uint8_t>(pte_word2 & 0x80)
    };
}

MapDmaResult mmu_map_dma_mem(uint32_t addr, uint32_t size,
    bool allow_mmio, bool allow_partial, bool is_dbg)
{
    MMIODevice      *devobj  = nullptr;
    uint8_t         *host_va = nullptr;
    uint32_t        dev_base = 0;
    bool            is_writable;
    AddressMapEntry *cur_dma_rgn;
    AddressMapEntry *next_dma_rgn;

    cur_dma_rgn = mem_ctrl_instance->find_range(addr);
    if (!cur_dma_rgn) {
        if (is_dbg) goto fail;
        ABORT_F("SOS: DMA access to unmapped physical memory 0x%08X..0x%08X!",
            addr, addr + size - 1
        );
    }

    if (addr + size - 1 > cur_dma_rgn->end) {
        if (!is_dbg) {
            if (cur_dma_rgn->type & (RT_ROM | RT_RAM))
                LOG_F(WARNING, "this region: 0x%08X..0x%08X (host: 0x%08llX..0x%08llX)",
                    cur_dma_rgn->start, cur_dma_rgn->end,
                    uint64_t(cur_dma_rgn->mem_ptr),
                    uint64_t(cur_dma_rgn->mem_ptr + cur_dma_rgn->end - cur_dma_rgn->start)
                );
            else
                LOG_F(ERROR, "this region: 0x%08X..0x%08X",
                    cur_dma_rgn->start, cur_dma_rgn->end
                );
        }
        next_dma_rgn = mem_ctrl_instance->find_range(cur_dma_rgn->end + 1);
        if (!is_dbg && next_dma_rgn) {
            if (next_dma_rgn->type & (RT_ROM | RT_RAM))
                LOG_F(WARNING, "next region: 0x%08X..0x%08X (host: 0x%08llX..0x%08llX)",
                    next_dma_rgn->start, next_dma_rgn->end,
                    uint64_t(next_dma_rgn->mem_ptr),
                    uint64_t(next_dma_rgn->mem_ptr + next_dma_rgn->end - next_dma_rgn->start)
                );
            else
                LOG_F(ERROR, "next region: 0x%08X..0x%08X",
                    next_dma_rgn->start, next_dma_rgn->end
                );
        }
        if (next_dma_rgn &&
            (cur_dma_rgn->type & (RT_ROM | RT_RAM)) &&
            ((cur_dma_rgn->type & (RT_ROM | RT_RAM)) == (next_dma_rgn->type & (RT_ROM | RT_RAM))) &&
            (next_dma_rgn->mem_ptr == cur_dma_rgn->mem_ptr + cur_dma_rgn->end - cur_dma_rgn->start + 1) &&
            (addr + size - 1 <= next_dma_rgn->end)
        ) {
            if (!is_dbg)
                LOG_F(INFO, "DMA to physical memory 0x%08X..0x%08X is OK!"
                    " The regions are the same type and adjacent in host and guest spaces.",
                    addr, addr + size - 1
                );
        } else {
            if (allow_partial) {
                if (!is_dbg)
                    LOG_F(INFO, "DMA access to physical memory 0x%08X..0x%08X spans regions!",
                        addr, addr + size - 1
                    );
                size = cur_dma_rgn->end - addr + 1;
            } else {
                if (is_dbg) goto fail;
                ABORT_F("DMA access to physical memory 0x%08X..0x%08X spans regions!",
                    addr, addr + size - 1
                );
            }
        }
    }

    if ((cur_dma_rgn->type & RT_MMIO) && !allow_mmio) {
        if (is_dbg) goto fail;
        ABORT_F("SOS: DMA access to a MMIO region 0x%08X..0x%08X (%s) for physical memory 0x%08X..0x%08X is not allowed.",
            cur_dma_rgn->start, cur_dma_rgn->end, cur_dma_rgn->devobj->get_name().c_str(), addr, addr + size - 1
        );
    }

    if (cur_dma_rgn->type & (RT_ROM | RT_RAM)) {
        host_va  = cur_dma_rgn->mem_ptr + (addr - cur_dma_rgn->start);
        is_writable = cur_dma_rgn->type & RT_RAM;
    } else { // RT_MMIO
        devobj = cur_dma_rgn->devobj;
        dev_base = cur_dma_rgn->start;
        is_writable = true; // all MMIO devices must provide a write method
    }

    return MapDmaResult{
        .type = cur_dma_rgn->type,
        .size = size,
        .is_writable = is_writable,
        .host_va = host_va,
        .dev_obj = devobj,
        .dev_base = dev_base,
    };
fail:
    return MapDmaResult{
        .type = RT_NONE
    };
}

constexpr uint32_t TLB_SIZE        = 4096;
constexpr uint32_t TLB2_WAYS       = 4;
constexpr uint32_t TLB_INVALID_TAG = 0xFFFFFFFF;

enum TLBFlags : uint16_t {
    PAGE_MEM      = 1 << 0, // memory page backed by host memory
    PAGE_IO       = 1 << 1, // memory mapped I/O page
    PAGE_NOPHYS   = 1 << 2, // no physical storage for this page (unmapped)
    TLBE_FROM_BAT = 1 << 3, // TLB entry has been translated with BAT
    TLBE_FROM_PAT = 1 << 4, // TLB entry has been translated with PAT
    PAGE_WRITABLE = 1 << 5, // page is writable
    PTE_SET_C     = 1 << 6, // tells if C bit of the PTE needs to be updated
    TLBE_CTX_TRACKED = 1 << 7, // entry pointer is in gTrackedIEntries or gTrackedDEntries
};

constexpr uint16_t TLBE_FROM_TRANSLATION =
    TLBFlags::TLBE_FROM_BAT | TLBFlags::TLBE_FROM_PAT;

typedef struct TLBEntry {
    uint32_t    tag;
    uint16_t    flags;
    uint16_t    lru_bits;
    union {
        struct { // for memory pages
            int64_t host_va_offs_r;
            int64_t host_va_offs_w;
        };
        struct { // for MMIO pages
            AddressMapEntry*    rgn_desc;
            int64_t             dev_base_va;
        };
    };
    uint32_t phys_tag;
    uint32_t reserved;
} TLBEntry;
static void sheep_fast_invalidate(TLBEntry*);
static void sheep_fast_publish(TLBEntry*);
static void sheep_fast_select();
static void sheep_fast_repair(unsigned,uint32_t);

// Track slots populated by either page or block address translation. Context
// changes can then invalidate only populated slots without adding a generation
// check to every TLB lookup.
static std::vector<TLBEntry *> gTrackedIEntries;
static std::vector<TLBEntry *> gTrackedDEntries;

// TLBE_FROM_* sources awaiting invalidation at the next context sync.
static uint16_t gPendingIInvalidationSources = 0;
static uint16_t gPendingDInvalidationSources = 0;

template <const TLBType tlb_type>
static void track_translated_entry(TLBEntry *tlb_entry)
{
    if (!(tlb_entry->flags & TLBE_FROM_TRANSLATION) ||
        (tlb_entry->flags & TLBFlags::TLBE_CTX_TRACKED))
        return;

    tlb_entry->flags |= TLBFlags::TLBE_CTX_TRACKED;
    if (tlb_type == TLBType::ITLB)
        gTrackedIEntries.push_back(tlb_entry);
    else
        gTrackedDEntries.push_back(tlb_entry);
}

static void tlb_invalidate_tracked_entries(std::vector<TLBEntry *> &tracked_entries,
                                           uint16_t sources)
{
    size_t retained_count = 0;
    for (TLBEntry *tlb_entry : tracked_entries) {
        uint16_t source = tlb_entry->flags & TLBE_FROM_TRANSLATION;
        if (source & sources) {
            sheep_fast_invalidate(tlb_entry);
            tlb_entry->tag = TLB_INVALID_TAG;
            tlb_entry->flags &= ~TLBFlags::TLBE_CTX_TRACKED;
        } else if (source) {
            tracked_entries[retained_count++] = tlb_entry;
        } else {
            tlb_entry->flags &= ~TLBFlags::TLBE_CTX_TRACKED;
        }
    }
    tracked_entries.resize(retained_count);
}

template <const TLBType tlb_type>
static void tlb_invalidate_tracked_entries()
{
    uint16_t *pending_sources;
    std::vector<TLBEntry *> *tracked_entries;

    if (tlb_type == TLBType::ITLB) {
        pending_sources = &gPendingIInvalidationSources;
        tracked_entries = &gTrackedIEntries;
    } else {
        pending_sources = &gPendingDInvalidationSources;
        tracked_entries = &gTrackedDEntries;
    }

    if (!*pending_sources)
        return;

    tlb_invalidate_tracked_entries(*tracked_entries, *pending_sources);
    *pending_sources = 0;
}

static void schedule_tlb_invalidation(TLBType tlb_type, uint16_t sources_to_invalidate)
{
    sheep_mmu_rom_window_invalidate();
    uint16_t *pending_sources;

    if (tlb_type == TLBType::ITLB)
        pending_sources = &gPendingIInvalidationSources;
    else
        pending_sources = &gPendingDInvalidationSources;

    if (!*pending_sources) {
        if (tlb_type == TLBType::ITLB)
            add_ctx_sync_action(&tlb_invalidate_tracked_entries<TLBType::ITLB>);
        else
            add_ctx_sync_action(&tlb_invalidate_tracked_entries<TLBType::DTLB>);
    }
    *pending_sources |= sources_to_invalidate;
}

template <const TLBType tlb_type>
static void promote_tlb_entry(TLBEntry *tlb1_entry, const TLBEntry *tlb2_entry)
{
    if constexpr(tlb_type==TLBType::DTLB)sheep_fast_invalidate(tlb1_entry);
    uint16_t tracked = tlb1_entry->flags & TLBFlags::TLBE_CTX_TRACKED;
    *tlb1_entry = *tlb2_entry;
    tlb1_entry->flags &= ~TLBFlags::TLBE_CTX_TRACKED;
    tlb1_entry->flags |= tracked;
    track_translated_entry<tlb_type>(tlb1_entry);
    if constexpr(tlb_type==TLBType::DTLB)sheep_fast_publish(tlb1_entry);
}

// primary ITLB for all MMU modes
static std::array<TLBEntry, TLB_SIZE> itlb1_mode1;
static std::array<TLBEntry, TLB_SIZE> itlb1_mode2;
static std::array<TLBEntry, TLB_SIZE> itlb1_mode3;

// secondary ITLB for all MMU modes
static std::array<TLBEntry, TLB_SIZE*TLB2_WAYS> itlb2_mode1;
static std::array<TLBEntry, TLB_SIZE*TLB2_WAYS> itlb2_mode2;
static std::array<TLBEntry, TLB_SIZE*TLB2_WAYS> itlb2_mode3;

// primary DTLB for all MMU modes
static std::array<TLBEntry, TLB_SIZE> dtlb1_mode1;
static std::array<TLBEntry, TLB_SIZE> dtlb1_mode2;
static std::array<TLBEntry, TLB_SIZE> dtlb1_mode3;

// secondary DTLB for all MMU modes
static std::array<TLBEntry, TLB_SIZE*TLB2_WAYS> dtlb2_mode1;
static std::array<TLBEntry, TLB_SIZE*TLB2_WAYS> dtlb2_mode2;
static std::array<TLBEntry, TLB_SIZE*TLB2_WAYS> dtlb2_mode3;

TLBEntry *pCurITLB1; // current primary ITLB
TLBEntry *pCurITLB2; // current secondary ITLB
TLBEntry *pCurDTLB1; // current primary DTLB
TLBEntry *pCurDTLB2; // current secondary DTLB

extern uint32_t tlb_size_mask;
static TLBEntry *sheep_fast_owners[]={dtlb1_mode1.data(),dtlb1_mode2.data(),dtlb1_mode3.data()};
static TLBEntry *sheep_fast_secondary[]={dtlb2_mode1.data(),dtlb2_mode2.data(),dtlb2_mode3.data()};
static int sheep_fast_owner(const TLBEntry *entry){
 uintptr_t address=reinterpret_cast<uintptr_t>(entry);
 for(unsigned mode=0;mode<3;mode++)for(unsigned level=0;level<2;level++){
  uintptr_t begin=reinterpret_cast<uintptr_t>(level?sheep_fast_secondary[mode]:sheep_fast_owners[mode]);size_t bytes=sizeof(TLBEntry)*TLB_SIZE*(level?TLB2_WAYS:1);
  if(address>=begin&&address-begin<bytes&&(address-begin)%sizeof(TLBEntry)==0)return int(mode);
 }return -1;
}
static void sheep_fast_refresh(unsigned mode,uint32_t va,const TLBEntry *ignore){
 if(mode>=3||(va&4095))return;
 unsigned index=(va>>12)&(TLB_SIZE-1);const TLBEntry *chosen=nullptr;
 auto *primary=&sheep_fast_owners[mode][index];
 if(primary!=ignore&&primary->tag==va)chosen=primary;
 else for(unsigned way=0;way<TLB2_WAYS;way++){auto *entry=&sheep_fast_secondary[mode][index*TLB2_WAYS+way];if(entry!=ignore&&entry->tag==va){chosen=entry;break;}}
 if(chosen)pippin_fastmem_publish(mode,index,va,chosen->flags,uintptr_t(chosen->host_va_offs_r)+va,uintptr_t(chosen->host_va_offs_w)+va);
 else pippin_fastmem_revoke(mode,va);
}
static void sheep_fast_repair(unsigned mode,uint32_t va){sheep_fast_refresh(mode,va,nullptr);}
static void sheep_fast_invalidate(TLBEntry *entry){
 if(!pippin_fastmem_enabled())return;int mode=sheep_fast_owner(entry);if(mode<0)return;
 sheep_fast_refresh(unsigned(mode),entry->tag,entry);
}
static void sheep_fast_publish(TLBEntry *entry){
 if(!pippin_fastmem_enabled())return;int mode=sheep_fast_owner(entry);if(mode<0)return;
 sheep_fast_refresh(unsigned(mode),entry->tag,nullptr);
}
static void sheep_fast_select(){
 unsigned mode=3;if(tlb_size_mask==TLB_SIZE-1)for(unsigned i=0;i<3;i++)if(pCurDTLB1==sheep_fast_owners[i]){mode=i;break;}
 pippin_fastmem_select(mode);
}

uint32_t tlb_size_mask = TLB_SIZE - 1;

// fake TLB entry for handling of unmapped memory accesses
uint64_t    UnmappedVal = -1ULL;
TLBEntry    UnmappedMem = {TLB_INVALID_TAG, TLBFlags::PAGE_NOPHYS, 0, {{0}}};

uint8_t     CurITLBMode = {0xFF}; // current ITLB mode
uint8_t     CurDTLBMode = {0xFF}; // current DTLB mode

void mmu_change_mode()
{
    q227_mode_calls++;
    // Q227: the ROM window is derived from the current ITLB tables only; keep it
    // when an exception/rfi/mtmsr leaves the instruction translation mode unchanged.
    if (!q227_mode_window) sheep_mmu_rom_window_invalidate();
    uint8_t mmu_mode;

    // switch ITLB tables first
    mmu_mode = ((!!(ppc_state.msr & MSR::IR)) << 1) | !!(ppc_state.msr & MSR::PR);

    if (CurITLBMode != mmu_mode) {
        q227_mode_itlb_changes++;
        if (q227_mode_window) sheep_mmu_rom_window_invalidate();
        switch (mmu_mode) {
            case 1: // user mode can't disable translations
                mmu_mode = 0;
            case 0: // real address mode
                pCurITLB1 = &itlb1_mode1[0];
                pCurITLB2 = &itlb2_mode1[0];
                break;
            case 2: // supervisor mode with instruction translation enabled
                pCurITLB1 = &itlb1_mode2[0];
                pCurITLB2 = &itlb2_mode2[0];
                break;
            case 3: // user mode with instruction translation enabled
                pCurITLB1 = &itlb1_mode3[0];
                pCurITLB2 = &itlb2_mode3[0];
                break;
        }
        CurITLBMode = mmu_mode;
    }

    // then switch DTLB tables
    mmu_mode = ((!!(ppc_state.msr & MSR::DR)) << 1) | !!(ppc_state.msr & MSR::PR);

    if (CurDTLBMode != mmu_mode) {
        switch (mmu_mode) {
            case 1: // user mode can't disable translations
                mmu_mode = 0;
            case 0: // real address mode
                pCurDTLB1 = &dtlb1_mode1[0];
                pCurDTLB2 = &dtlb2_mode1[0];
                break;
            case 2: // supervisor mode with data translation enabled
                pCurDTLB1 = &dtlb1_mode2[0];
                pCurDTLB2 = &dtlb2_mode2[0];
                break;
            case 3: // user mode with data translation enabled
                pCurDTLB1 = &dtlb1_mode3[0];
                pCurDTLB2 = &dtlb2_mode3[0];
                break;
        }
        CurDTLBMode = mmu_mode;
    }
    sheep_fast_select();
}

template <uint32_t way>
static inline void tlb2_touch_way(TLBEntry *tlb_entry)
{
    static_assert(way < TLB2_WAYS);

    // Each pair is one branch of the four-way tree-based LRU. Mark the
    // selected way as most recently used, its sibling as the next replacement
    // candidate, and the other pair as less recently used.
    if constexpr (way < 2) {
        tlb_entry[0].lru_bits  = way == 0 ? 0x3 : 0x2;
        tlb_entry[1].lru_bits  = way == 1 ? 0x3 : 0x2;
        tlb_entry[2].lru_bits &= 0x1;
        tlb_entry[3].lru_bits &= 0x1;
    } else {
        tlb_entry[0].lru_bits &= 0x1;
        tlb_entry[1].lru_bits &= 0x1;
        tlb_entry[2].lru_bits  = way == 2 ? 0x3 : 0x2;
        tlb_entry[3].lru_bits  = way == 3 ? 0x3 : 0x2;
    }
}

template <const TLBType tlb_type>
static TLBEntry* tlb2_target_entry(uint32_t gp_va)
{
    TLBEntry *tlb_entry;

    if (tlb_type == TLBType::ITLB) {
        tlb_entry = &pCurITLB2[((gp_va >> PPC_PAGE_SIZE_BITS) & tlb_size_mask) * TLB2_WAYS];
    } else {
        tlb_entry = &pCurDTLB2[((gp_va >> PPC_PAGE_SIZE_BITS) & tlb_size_mask) * TLB2_WAYS];
    }

    // select the target from invalid blocks first
    if (tlb_entry[0].tag == TLB_INVALID_TAG) {
        tlb2_touch_way<0>(tlb_entry);
        return tlb_entry;
    } else if (tlb_entry[1].tag == TLB_INVALID_TAG) {
        tlb2_touch_way<1>(tlb_entry);
        return &tlb_entry[1];
    } else if (tlb_entry[2].tag == TLB_INVALID_TAG) {
        tlb2_touch_way<2>(tlb_entry);
        return &tlb_entry[2];
    } else if (tlb_entry[3].tag == TLB_INVALID_TAG) {
        tlb2_touch_way<3>(tlb_entry);
        return &tlb_entry[3];
    } else { // no free entries, replace an existing one according with the hLRU policy
#ifdef TLB_PROFILING
        num_entry_replacements++;
#endif
        if (tlb_entry[0].lru_bits == 0) {
            tlb2_touch_way<0>(tlb_entry);
            return tlb_entry;
        } else if (tlb_entry[1].lru_bits == 0) {
            tlb2_touch_way<1>(tlb_entry);
            return &tlb_entry[1];
        } else if (tlb_entry[2].lru_bits == 0) {
            tlb2_touch_way<2>(tlb_entry);
            return &tlb_entry[2];
        } else {
            tlb2_touch_way<3>(tlb_entry);
            return &tlb_entry[3];
        }
    }
}

static TLBEntry* itlb2_refill(uint32_t guest_va)
{
    BATResult bat_res;
    uint32_t phys_addr;
    TLBEntry *tlb_entry;
    uint16_t flags = 0;

    /* instruction address translation if enabled */
    if (ppc_state.msr & MSR::IR) {
        // attempt block address translation first
        if (is_601) {
            bat_res = mpc601_block_address_translation(guest_va);
        } else {
            bat_res = ppc_block_address_translation<BATType::IBAT>(guest_va);
        }
        if (bat_res.hit) {
            // check block protection
            // only PP = 0 (no access) causes ISI exception
            if (!bat_res.prot) {
                mmu_exception_handler(Except_Type::EXC_ISI, 0x08000000);
            }
            phys_addr = bat_res.phys;
            flags |= TLBFlags::TLBE_FROM_BAT; // tell the world we come from
        } else {
            // page address translation
            PATResult pat_res = page_address_translation(guest_va, true, !!(ppc_state.msr & MSR::PR), 0);
            phys_addr = pat_res.phys;
            flags = TLBFlags::TLBE_FROM_PAT; // tell the world we come from
        }
    } else { // instruction translation disabled
        phys_addr = guest_va;
    }

    // look up host virtual address
    AddressMapEntry* rgn_desc = mem_ctrl_instance->find_range(phys_addr);
    if (rgn_desc) {
        if (rgn_desc->type & RT_MMIO) {
            ABORT_F("Instruction fetch from MMIO region at 0x%08X!\n", phys_addr);
        }
        // refill the secondary TLB
        const uint32_t tag = guest_va & ~0xFFFUL;
        tlb_entry = tlb2_target_entry<TLBType::ITLB>(tag);
        tlb_entry->tag = tag;
        tlb_entry->flags = flags | TLBFlags::PAGE_MEM |
            (tlb_entry->flags & TLBFlags::TLBE_CTX_TRACKED);
        tlb_entry->host_va_offs_r = (int64_t)rgn_desc->mem_ptr - guest_va +
                                    (phys_addr - rgn_desc->start);
        tlb_entry->phys_tag = phys_addr & ~0xFFFUL;
        track_translated_entry<TLBType::ITLB>(tlb_entry);
    } else {
        ABORT_F("Instruction fetch from unmapped memory at 0x%08X!\n", phys_addr);
    }

    return tlb_entry;
}

static TLBEntry* dtlb2_refill(uint32_t guest_va, int is_write, bool is_dbg = false)
{
    BATResult bat_res;
    uint32_t phys_addr;
    uint16_t flags = 0;
    TLBEntry *tlb_entry;

    const uint32_t tag = guest_va & ~0xFFFUL;

    /* data address translation if enabled */
    if (ppc_state.msr & MSR::DR) {
        // attempt block address translation first
        if (is_601) {
            bat_res = mpc601_block_address_translation(guest_va);
        } else {
            bat_res = ppc_block_address_translation<BATType::DBAT>(guest_va);
        }
        if (bat_res.hit) {
            // check block protection
            if (!bat_res.prot || ((bat_res.prot & 1) && is_write)) {
                if (!is_dbg)
                    LOG_F(9, "BAT DSI exception in TLB2 refill!");
                if (!is_dbg)
                    LOG_F(9, "Attempt to write to read-only region, LA=0x%08X, PC=0x%08X!", guest_va, ppc_state.pc);
                ppc_state.spr[SPR::DSISR] = 0x08000000 | (is_write << 25);
                ppc_state.spr[SPR::DAR]   = guest_va;
                mmu_exception_handler(Except_Type::EXC_DSI, 0);
            }
            phys_addr = bat_res.phys;
            flags = TLBFlags::PTE_SET_C; // prevent PTE.C updates for BAT
            flags |= TLBFlags::TLBE_FROM_BAT; // tell the world we come from
            if (bat_res.prot == 2) {
                flags |= TLBFlags::PAGE_WRITABLE;
            }
        } else {
            // page address translation
            PATResult pat_res = page_address_translation(guest_va, false, !!(ppc_state.msr & MSR::PR), is_write);
            phys_addr = pat_res.phys;
            flags = TLBFlags::TLBE_FROM_PAT; // tell the world we come from
            if (pat_res.prot <= 2 || pat_res.prot == 6) {
                flags |= TLBFlags::PAGE_WRITABLE;
            }
            if (is_write || pat_res.pte_c_status) {
                // C-bit of the PTE is already set so the TLB logic
                // doesn't need to update it anymore
                flags |= TLBFlags::PTE_SET_C;
            }
        }
    } else { // data translation disabled
        phys_addr = guest_va;
        flags = TLBFlags::PTE_SET_C; // no PTE.C updates in real addressing mode
        flags |= TLBFlags::PAGE_WRITABLE; // assume physical pages are writable
    }

    // look up host virtual address
    AddressMapEntry* rgn_desc = mem_ctrl_instance->find_range(phys_addr);
    if (rgn_desc) {
        // refill the secondary TLB
        tlb_entry = tlb2_target_entry<TLBType::DTLB>(tag);
        sheep_fast_invalidate(tlb_entry);
        tlb_entry->tag = tag;
        if (rgn_desc->type & RT_MMIO) { // MMIO region
            tlb_entry->flags = flags | TLBFlags::PAGE_IO |
                (tlb_entry->flags & TLBFlags::TLBE_CTX_TRACKED);
            tlb_entry->rgn_desc = rgn_desc;
            tlb_entry->dev_base_va = guest_va - (phys_addr - rgn_desc->start);
        } else { // memory region backed by host memory
            tlb_entry->flags = flags | TLBFlags::PAGE_MEM |
                (tlb_entry->flags & TLBFlags::TLBE_CTX_TRACKED);
            tlb_entry->host_va_offs_r = (int64_t)rgn_desc->mem_ptr - guest_va +
                                        (phys_addr - rgn_desc->start);
            if (rgn_desc->type & RT_ROM) {
                // redirect writes to the dummy page for ROM regions
                tlb_entry->host_va_offs_w = (int64_t)&dummy_page - tag;
            } else {
                tlb_entry->host_va_offs_w = tlb_entry->host_va_offs_r;
            }
        }
        tlb_entry->phys_tag = phys_addr & ~0xFFFUL;
        track_translated_entry<TLBType::DTLB>(tlb_entry);
        sheep_fast_publish(tlb_entry);
        return tlb_entry;
    } else {
        if (!is_dbg) {
        static uint32_t last_phys_addr = -1;
        static uint32_t first_phys_addr = -1;
        if (phys_addr != last_phys_addr + 4) {
            if (last_phys_addr != -1 && last_phys_addr != first_phys_addr) {
                LOG_F(WARNING, "                                                         ... phys_addr=0x%08X", last_phys_addr);
            }
            first_phys_addr = phys_addr;
            LOG_F(WARNING, "Access to unmapped physical memory, phys_addr=0x%08X", first_phys_addr);
        }
        last_phys_addr = phys_addr;
        }
        return &UnmappedMem;
    }
}

template <const TLBType tlb_type>
static inline TLBEntry* lookup_secondary_tlb(uint32_t guest_va, uint32_t tag) {
    TLBEntry *tlb_entry;

    if (tlb_type == TLBType::ITLB) {
        tlb_entry = &pCurITLB2[((guest_va >> PPC_PAGE_SIZE_BITS) & tlb_size_mask) * TLB2_WAYS];
    } else {
        tlb_entry = &pCurDTLB2[((guest_va >> PPC_PAGE_SIZE_BITS) & tlb_size_mask) * TLB2_WAYS];
    }

    if (tlb_entry[0].tag == tag) {
        tlb2_touch_way<0>(tlb_entry);
    } else if (tlb_entry[1].tag == tag) {
        tlb2_touch_way<1>(tlb_entry);
        tlb_entry = &tlb_entry[1];
    } else if (tlb_entry[2].tag == tag) {
        tlb2_touch_way<2>(tlb_entry);
        tlb_entry = &tlb_entry[2];
    } else if (tlb_entry[3].tag == tag) {
        tlb2_touch_way<3>(tlb_entry);
        tlb_entry = &tlb_entry[3];
    } else {
        return nullptr;
    }
    return tlb_entry;
}

struct TLBLookupResult {
    TLBEntry *primary_entry;
    TLBEntry *matched_entry; // nullptr if both TLBs missed
    bool primary_hit;
};

template <const TLBType tlb_type>
static inline TLBLookupResult lookup_tlb(uint32_t guest_va, uint32_t tag)
{
    TLBEntry *primary_entry;
    if constexpr (tlb_type == TLBType::ITLB) {
        primary_entry = &pCurITLB1[(guest_va >> PPC_PAGE_SIZE_BITS) & tlb_size_mask];
    } else {
        primary_entry = &pCurDTLB1[(guest_va >> PPC_PAGE_SIZE_BITS) & tlb_size_mask];
    }

    if (primary_entry->tag == tag) {
        return TLBLookupResult{primary_entry, primary_entry, true};
    }
    return TLBLookupResult{
        primary_entry,
        lookup_secondary_tlb<tlb_type>(guest_va, tag),
        false
    };
}

// Returns true when the PTE.C bit was updated. Callers writing through a
// primary entry use this to mirror the update into the secondary TLB.
static inline bool prepare_dtlb_write(TLBEntry *tlb_entry, uint32_t guest_va)
{
    if (!(tlb_entry->flags & TLBFlags::PAGE_WRITABLE)) {
        ppc_state.spr[SPR::DSISR] = 0x08000000 | (1 << 25);
        ppc_state.spr[SPR::DAR]   = guest_va;
        mmu_exception_handler(Except_Type::EXC_DSI, 0);
    }

    if (tlb_entry->flags & TLBFlags::PTE_SET_C) {
        return false;
    }

    // Perform full page address translation to update the PTE.C bit.
    page_address_translation(guest_va, false, !!(ppc_state.msr & MSR::PR), true);
    tlb_entry->flags |= TLBFlags::PTE_SET_C;
    sheep_fast_publish(tlb_entry);
    return true;
}

void mmu_dcbz(uint32_t opcode, uint32_t guest_va)
{
    const uint32_t tag = guest_va & ~0xFFFUL;
    TLBEntry *tlb_entry = lookup_tlb<TLBType::DTLB>(guest_va, tag).matched_entry;
    if (tlb_entry == nullptr) {
        // perform full address translation and refill the secondary TLB
        tlb_entry = dtlb2_refill(guest_va, 1);
    }

    // Check if this was in a MMIO region, in which case we avoid doing the
    // write, so that this intermediate state is not visible to the device
    // (usually a dcbz is done to prepare a cache line as a write buffer, and
    // the real values are written later).
    if (tlb_entry->flags & TLBFlags::PAGE_IO) {
        // Still do the same checks that mmu_write_vmem would have done to
        // disallow writes to read-only regions.
        prepare_dtlb_write(tlb_entry, guest_va);
        return;
    }

    // the following is not especially efficient but necessary
    // to make BlockZero under Mac OS 8.x and later to work
    mmu_write_vmem<uint64_t>(opcode, guest_va +  0, 0);
    mmu_write_vmem<uint64_t>(opcode, guest_va +  8, 0);
    mmu_write_vmem<uint64_t>(opcode, guest_va + 16, 0);
    mmu_write_vmem<uint64_t>(opcode, guest_va + 24, 0);
}

uint8_t *mmu_translate_imem(uint32_t vaddr, uint32_t *paddr)
{
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
    if (ppc_state.is_LE) {
        vaddr ^= mem_munge_constant<uint32_t>();
    }
#endif

    TLBEntry *tlb1_entry, *tlb2_entry;
    uint8_t *host_va;

#ifdef MMU_PROFILING
    exec_reads_total++;
#endif

    const uint32_t tag = vaddr & ~0xFFFUL;

    TLBLookupResult tlb_lookup = lookup_tlb<TLBType::ITLB>(vaddr, tag);
    tlb1_entry = tlb_lookup.primary_entry;
    if (tlb_lookup.primary_hit) { // primary ITLB hit -> fast path
#ifdef TLB_PROFILING
        num_primary_itlb_hits++;
#endif
        host_va = (uint8_t *)(tlb1_entry->host_va_offs_r + vaddr);
    } else {
        tlb2_entry = tlb_lookup.matched_entry;
        if (tlb2_entry == nullptr) {
#ifdef TLB_PROFILING
            num_itlb_refills++;
#endif
            // secondary ITLB miss ->
            // perform full address translation and refill the secondary ITLB
            tlb2_entry = itlb2_refill(vaddr);
        }
#ifdef TLB_PROFILING
        else {
            num_secondary_itlb_hits++;
        }
#endif
        // refill the primary ITLB
        promote_tlb_entry<TLBType::ITLB>(tlb1_entry, tlb2_entry);
        host_va = (uint8_t *)(tlb1_entry->host_va_offs_r + vaddr);
    }

    if (paddr)
        *paddr = tlb1_entry->phys_tag | (vaddr & 0xFFFUL);

    return host_va;
}

// Q227: tlbie invalidates PAT entries of the EA's congruence class (EA bits 15-19,
// as on the MPC603) in every segment: a superset of the hardware's invalidation.
static void tlb_invalidate_tracked_pat_class(std::vector<TLBEntry *> &tracked_entries,
                                             uint32_t page_class)
{
    size_t retained_count = 0;
    q227_tlbie_scanned += tracked_entries.size();
    for (TLBEntry *tlb_entry : tracked_entries) {
        uint16_t source = tlb_entry->flags & TLBE_FROM_TRANSLATION;
        if ((source & TLBE_FROM_PAT) && ((tlb_entry->tag >> 12) & 0x1F) == page_class) {
            sheep_fast_invalidate(tlb_entry);
            tlb_entry->tag = TLB_INVALID_TAG;
            tlb_entry->flags &= ~TLBFlags::TLBE_CTX_TRACKED;
            q227_tlbie_invalidated++;
        } else if (source) {
            tracked_entries[retained_count++] = tlb_entry;
        } else {
            tlb_entry->flags &= ~TLBFlags::TLBE_CTX_TRACKED;
        }
    }
    tracked_entries.resize(retained_count);
}

void tlb_flush_pat_ea(uint32_t ea)
{
    q227_tlbie_calls++;
    if (!q227_precise_tlbie) { tlb_flush_all_pat(); return; }
    sheep_mmu_rom_window_invalidate();
    const uint32_t page_class = (ea >> 12) & 0x1F;
    tlb_invalidate_tracked_pat_class(gTrackedIEntries, page_class);
    tlb_invalidate_tracked_pat_class(gTrackedDEntries, page_class);
}

void tlb_flush_all_pat()
{
    q227_full_flushes++;
    sheep_mmu_rom_window_invalidate();
#ifdef MMU_PROFILING
    pat_flushes_total++;
#endif

    tlb_invalidate_tracked_entries(gTrackedIEntries, TLBE_FROM_PAT);
    tlb_invalidate_tracked_entries(gTrackedDEntries, TLBE_FROM_PAT);
}

static void mpc601_bat_update(uint32_t bat_reg)
{
    sheep_mmu_rom_window_invalidate();
    PPC_BAT_entry *ibat_entry, *dbat_entry;
    uint32_t bsm, hi_mask;
    int upper_reg_num;

    upper_reg_num = bat_reg & 0xFFFFFFFE;

    ibat_entry = &ibat_array[(bat_reg - 528) >> 1];
    dbat_entry = &dbat_array[(bat_reg - 528) >> 1];

    if (ppc_state.spr[bat_reg | 1] & 0x40) {
        bsm     = ppc_state.spr[upper_reg_num + 1] & 0x3F;
        hi_mask = ~((bsm << 17) | 0x1FFFF);

        ibat_entry->valid   = true;
        ibat_entry->access  = (ppc_state.spr[upper_reg_num] >> 2) & 3;
        ibat_entry->prot    = ppc_state.spr[upper_reg_num] & 3;
        ibat_entry->hi_mask = hi_mask;
        ibat_entry->phys_hi = ppc_state.spr[upper_reg_num + 1] & hi_mask;
        ibat_entry->bepi    = ppc_state.spr[upper_reg_num] & hi_mask;

        // copy IBAT entry to DBAT entry
        *dbat_entry = *ibat_entry;
    } else {
        // disable the corresponding BAT pairs
        ibat_entry->valid = false;
        dbat_entry->valid = false;
    }

    // MPC601 has unified BATs, so they affect both translation contexts.
    schedule_tlb_invalidation(TLBType::ITLB, TLBE_FROM_TRANSLATION);
    schedule_tlb_invalidation(TLBType::DTLB, TLBE_FROM_TRANSLATION);
}

static void mpc601_dbat_update(uint32_t /*bat_reg*/)
{
    // ppc_illegalop(opcode);
}

static void ppc_ibat_update(uint32_t bat_reg)
{
    sheep_mmu_rom_window_invalidate();
    int upper_reg_num;
    uint32_t bl, hi_mask;
    PPC_BAT_entry* bat_entry;

    upper_reg_num = bat_reg & 0xFFFFFFFE;

    bat_entry = &ibat_array[(bat_reg - 528) >> 1];
    bl        = (ppc_state.spr[upper_reg_num] >> 2) & 0x7FF;
    hi_mask   = ~((bl << 17) | 0x1FFFF);

    bat_entry->access  = ppc_state.spr[upper_reg_num] & 3;
    bat_entry->prot    = ppc_state.spr[upper_reg_num + 1] & 3;
    bat_entry->hi_mask = hi_mask;
    bat_entry->phys_hi = ppc_state.spr[upper_reg_num + 1] & hi_mask;
    bat_entry->bepi    = ppc_state.spr[upper_reg_num] & hi_mask;

    // A new IBAT can shadow an existing page translation.
    schedule_tlb_invalidation(TLBType::ITLB, TLBE_FROM_TRANSLATION);
}

static void ppc_dbat_update(uint32_t bat_reg)
{
    int upper_reg_num;
    uint32_t bl, hi_mask;
    PPC_BAT_entry* bat_entry;

    upper_reg_num = bat_reg & 0xFFFFFFFE;

    bat_entry = &dbat_array[(bat_reg - 536) >> 1];
    bl        = (ppc_state.spr[upper_reg_num] >> 2) & 0x7FF;
    hi_mask   = ~((bl << 17) | 0x1FFFF);

    bat_entry->access  = ppc_state.spr[upper_reg_num] & 3;
    bat_entry->prot    = ppc_state.spr[upper_reg_num + 1] & 3;
    bat_entry->hi_mask = hi_mask;
    bat_entry->phys_hi = ppc_state.spr[upper_reg_num + 1] & hi_mask;
    bat_entry->bepi    = ppc_state.spr[upper_reg_num] & hi_mask;

    // A new DBAT can shadow an existing page translation.
    schedule_tlb_invalidation(TLBType::DTLB, TLBE_FROM_TRANSLATION);
}

void mmu_pat_ctx_changed()
{
    q227_pat_ctx_changes++;
    // Page address translation context changed so invalidate all PAT entries
    // from both ITLB and DTLB.
    schedule_tlb_invalidation(TLBType::ITLB, TLBFlags::TLBE_FROM_PAT);
    schedule_tlb_invalidation(TLBType::DTLB, TLBFlags::TLBE_FROM_PAT);
}

#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
    #if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
        #define ARGS_SWAP_MUNGED , bool needs_swap, bool munged
        #define SWAP_MUNGED , needs_swap, munged
    #else
        #define ARGS_SWAP_MUNGED , bool munged
        #define SWAP_MUNGED , munged
    #endif
#else
    #if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
        #define ARGS_SWAP_MUNGED , bool needs_swap, bool munged
        #define SWAP_MUNGED , needs_swap, munged
    #else
        #define ARGS_SWAP_MUNGED
        #define SWAP_MUNGED
    #endif
#endif

// Forward declarations.
template <class T>
static T read_unaligned(uint32_t opcode, uint32_t guest_va, uint8_t *host_va ARGS_SWAP_MUNGED);
template <class T>
static void write_unaligned(uint32_t opcode, uint32_t guest_va, uint8_t *host_va, T value ARGS_SWAP_MUNGED);

template <class T>
__attribute__((noinline)) static T mmu_read_vmem_slow(uint32_t opcode, uint32_t guest_va)
{
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE || SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    bool munged = false;
#endif
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
    if (ppc_state.is_LE) {
        guest_va = mem_munge_address<T>(guest_va);
        munged = true;
    }
#endif

    TLBEntry *tlb1_entry, *tlb2_entry;
    uint8_t *host_va;

    const uint32_t tag = guest_va & ~0xFFFUL;

    // look up guest virtual address in the primary and secondary TLBs
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    bool needs_swap = false;
#endif
    TLBLookupResult tlb_lookup = lookup_tlb<TLBType::DTLB>(guest_va, tag);
    tlb1_entry = tlb_lookup.primary_entry;
    if (tlb_lookup.primary_hit) { // primary TLB hit -> fast path
#ifdef TLB_PROFILING
        num_primary_dtlb_hits++;
#endif

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
        needs_swap = mem_ctrl_instance->needs_swap_endian(false);
        if (needs_swap) {
            guest_va = mem_munge_address<T>(guest_va);
            munged ^= 1;
        }
#endif

        host_va = (uint8_t *)(tlb1_entry->host_va_offs_r + guest_va);
    } else {
        tlb2_entry = tlb_lookup.matched_entry;
        if (tlb2_entry == nullptr) {
#ifdef TLB_PROFILING
            num_dtlb_refills++;
#endif
            // secondary TLB miss ->
            // perform full address translation and refill the secondary TLB
            tlb2_entry = dtlb2_refill(guest_va, 0);
            if (tlb2_entry->flags & PAGE_NOPHYS) {
                return (T)UnmappedVal;
            }
        }
#ifdef TLB_PROFILING
        else {
            num_secondary_dtlb_hits++;
        }
#endif

        if (tlb2_entry->flags & TLBFlags::PAGE_MEM) { // is it a real memory region?
            // refill the primary TLB
            promote_tlb_entry<TLBType::DTLB>(tlb1_entry, tlb2_entry);

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
            needs_swap = mem_ctrl_instance->needs_swap_endian(false);
            if (needs_swap) {
                guest_va = mem_munge_address<T>(guest_va);
                munged ^= 1;
            }
#endif

            host_va = (uint8_t *)(tlb1_entry->host_va_offs_r + guest_va);
        } else { // otherwise, it's an access to a memory-mapped device
#ifdef MMU_PROFILING
            iomem_reads_total++;
#endif

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
            needs_swap = mem_ctrl_instance->needs_swap_endian(tlb2_entry->rgn_desc);
            if (needs_swap) {
                guest_va = mem_munge_address<T>(guest_va);
                munged ^= 1;
            }
#endif

            if (sizeof(T) == 8) {
                if (guest_va & 3)
                    ppc_alignment_exception(opcode, guest_va);

                uint32_t valueLow = tlb2_entry->rgn_desc->devobj->read(
                    tlb2_entry->rgn_desc->start,
                    static_cast<uint32_t>(guest_va - tlb2_entry->dev_base_va),
                    4);

                uint32_t valueHigh = tlb2_entry->rgn_desc->devobj->read(
                    tlb2_entry->rgn_desc->start,
                    static_cast<uint32_t>(guest_va + 4 - tlb2_entry->dev_base_va),
                    4);

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                if (needs_swap) {
                    uint32_t temp = valueHigh;
                    valueHigh = BYTESWAP_32(valueLow);
                    valueLow  = BYTESWAP_32(temp);
                }
#endif

                return (
                    ((T)(valueLow) << 32) | valueHigh
                );
            }
            else {
                T value = (
                    tlb2_entry->rgn_desc->devobj->read(tlb2_entry->rgn_desc->start,
                        static_cast<uint32_t>(guest_va - tlb2_entry->dev_base_va),
                        sizeof(T))
                );

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                if (needs_swap && sizeof(T) > 1) {
                    value = BYTESWAP_SIZED(value, sizeof(T));
                }
#endif

                return value;
            }
        }
    }

#ifdef MMU_PROFILING
    dmem_reads_total++;
#endif

    // handle unaligned memory accesses
    if (sizeof(T) > 1 && (guest_va & (sizeof(T) - 1))) {
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE || SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
        if (munged)
            guest_va = mem_munge_address<T>(guest_va);
#endif
        return read_unaligned<T>(opcode, guest_va, host_va SWAP_MUNGED);
    }

    // handle aligned memory accesses
    switch(sizeof(T)) {
        case 1:
            return *host_va;
        case 2:
            return
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                needs_swap ? (READ_WORD_LE_A(host_va)) :
#endif
                (READ_WORD_BE_A(host_va));
        case 4:
            return
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                needs_swap ? (READ_DWORD_LE_A(host_va)) :
#endif
                (READ_DWORD_BE_A(host_va));
        case 8:
            return
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                needs_swap ? (READ_QWORD_LE_A(host_va)) :
#endif
                (READ_QWORD_BE_A(host_va));
    }
}

// A leaf primary hit before the original out-of-line MMU implementation.
static inline uint8_t *sheep_primary_read_host(uint32_t va,unsigned size){
#if defined(__aarch64__) && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__ && !SUPPORTS_PPC_LITTLE_ENDIAN_MODE && !SUPPORTS_MEMORY_CTRL_ENDIAN_MODE && !defined(MMU_PROFILING) && !defined(TLB_PROFILING) && !defined(LOG_INSTRUCTIONS)
 if(va&(size-1))return nullptr;
 auto *entry=&pCurDTLB1[(va>>PPC_PAGE_SIZE_BITS)&tlb_size_mask];
 if(entry->tag!=(va&PPC_PAGE_MASK)||!(entry->flags&TLBFlags::PAGE_MEM))return nullptr;
 return reinterpret_cast<uint8_t*>(entry->host_va_offs_r+va);
#else
 return nullptr;
#endif
}
template<class T> inline T mmu_read_vmem(uint32_t opcode,uint32_t va){
 if(auto *host=sheep_primary_read_host(va,sizeof(T))){
  if constexpr(sizeof(T)==1)return *host;
  if constexpr(sizeof(T)==2)return READ_WORD_BE_A(host);
  if constexpr(sizeof(T)==4)return READ_DWORD_BE_A(host);
  if constexpr(sizeof(T)==8)return READ_QWORD_BE_A(host);
 }
 return mmu_read_vmem_slow<T>(opcode,va);
}

// explicitely instantiate all required mmu_read_vmem variants
template uint8_t  mmu_read_vmem<uint8_t>(uint32_t opcode, uint32_t guest_va);
template uint16_t mmu_read_vmem<uint16_t>(uint32_t opcode, uint32_t guest_va);
template uint32_t mmu_read_vmem<uint32_t>(uint32_t opcode, uint32_t guest_va);
template uint64_t mmu_read_vmem<uint64_t>(uint32_t opcode, uint32_t guest_va);

template <class T>
inline void mmu_write_vmem(uint32_t opcode, uint32_t guest_va, T value)
{
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE || SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    bool munged = false;
#endif
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
    if (ppc_state.is_LE) {
        guest_va = mem_munge_address<T>(guest_va);
        munged = true;
    }
#endif

    TLBEntry *tlb1_entry, *tlb2_entry;
    uint8_t *host_va;

    const uint32_t tag = guest_va & ~0xFFFUL;

    // look up guest virtual address in the primary and secondary TLBs
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    bool needs_swap = false;
#endif
    TLBLookupResult tlb_lookup = lookup_tlb<TLBType::DTLB>(guest_va, tag);
    tlb1_entry = tlb_lookup.primary_entry;
    if (tlb_lookup.primary_hit) { // primary TLB hit -> fast path
#ifdef TLB_PROFILING
        num_primary_dtlb_hits++;
#endif
        if (prepare_dtlb_write(tlb1_entry, guest_va)) {
            // don't forget to update the secondary TLB as well
            tlb2_entry = lookup_secondary_tlb<TLBType::DTLB>(guest_va, tag);
            if (tlb2_entry != nullptr) {
                tlb2_entry->flags |= TLBFlags::PTE_SET_C;
            }
        }
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
        needs_swap = mem_ctrl_instance->needs_swap_endian(false);
        if (needs_swap) {
            guest_va = mem_munge_address<T>(guest_va);
            munged ^= 1;
        }
#endif
        host_va = (uint8_t *)(tlb1_entry->host_va_offs_w + guest_va);
    } else {
        tlb2_entry = tlb_lookup.matched_entry;
        if (tlb2_entry == nullptr) {
#ifdef TLB_PROFILING
            num_dtlb_refills++;
#endif
            // secondary TLB miss ->
            // perform full address translation and refill the secondary TLB
            tlb2_entry = dtlb2_refill(guest_va, 1);
            if (tlb2_entry->flags & PAGE_NOPHYS) {
                return;
            }
        }
#ifdef TLB_PROFILING
        else {
            num_secondary_dtlb_hits++;
        }
#endif
        prepare_dtlb_write(tlb2_entry, guest_va);

        if (tlb2_entry->flags & TLBFlags::PAGE_MEM) { // is it a real memory region?
            // refill the primary TLB
            promote_tlb_entry<TLBType::DTLB>(tlb1_entry, tlb2_entry);

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
            needs_swap = mem_ctrl_instance->needs_swap_endian(false);
            if (needs_swap) {
                guest_va = mem_munge_address<T>(guest_va);
                munged ^= 1;
            }
#endif

            host_va = (uint8_t *)(tlb1_entry->host_va_offs_w + guest_va);
        } else { // otherwise, it's an access to a memory-mapped device
#ifdef MMU_PROFILING
            iomem_writes_total++;
#endif

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
            needs_swap = mem_ctrl_instance->needs_swap_endian(tlb2_entry->rgn_desc);
            if (needs_swap) {
                guest_va = mem_munge_address<T>(guest_va);
                munged ^= 1;
            }
#endif

            if (sizeof(T) == 8) {
                if (guest_va & 3)
                    ppc_alignment_exception(opcode, guest_va);

                uint32_t valueLow, valueHigh;
                valueLow = value >> 32;
                valueHigh = (uint32_t)value;
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                if (needs_swap) {
                    valueHigh = BYTESWAP_32(valueLow);
                    valueLow  = (uint32_t)value;
                    valueLow  = BYTESWAP_32(valueLow);
                }
#endif

                tlb2_entry->rgn_desc->devobj->write(tlb2_entry->rgn_desc->start,
                    static_cast<uint32_t>(guest_va - tlb2_entry->dev_base_va),
                    valueLow, 4);
                tlb2_entry->rgn_desc->devobj->write(tlb2_entry->rgn_desc->start,
                    static_cast<uint32_t>(guest_va + 4 - tlb2_entry->dev_base_va),
                    valueHigh, 4);
            } else {

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                if (needs_swap && sizeof(T) > 1) {
                    value = BYTESWAP_SIZED(value, sizeof(T));
                }
#endif

                tlb2_entry->rgn_desc->devobj->write(tlb2_entry->rgn_desc->start,
                    static_cast<uint32_t>(guest_va - tlb2_entry->dev_base_va),
                    value, sizeof(T));
            }
            return;
        }
    }

#ifdef MMU_PROFILING
    dmem_writes_total++;
#endif

#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    // swap now if needed
    if (needs_swap && sizeof(T) > 1) {
        value = BYTESWAP_SIZED(value, sizeof(T));
    }
#endif

    // handle unaligned memory accesses
    if (sizeof(T) > 1 && (guest_va & (sizeof(T) - 1))) {
        // unmunge the guest_va if it was munged
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE || SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
        if (munged)
            guest_va = mem_munge_address<T>(guest_va);
#endif
        write_unaligned<T>(opcode, guest_va, host_va, value SWAP_MUNGED);
        return;
    }

    // handle aligned memory accesses
    switch(sizeof(T)) {
        case 1:
            *host_va = value;
            break;
        case 2:
            WRITE_WORD_BE_A(host_va, value);
            break;
        case 4:
            WRITE_DWORD_BE_A(host_va, value);
            break;
        case 8:
            WRITE_QWORD_BE_A(host_va, value);
            break;
    }
}

// explicitely instantiate all required mmu_write_vmem variants
template void mmu_write_vmem<uint8_t> (uint32_t opcode, uint32_t guest_va, uint8_t value);
template void mmu_write_vmem<uint16_t>(uint32_t opcode, uint32_t guest_va, uint16_t value);
template void mmu_write_vmem<uint32_t>(uint32_t opcode, uint32_t guest_va, uint32_t value);
template void mmu_write_vmem<uint64_t>(uint32_t opcode, uint32_t guest_va, uint64_t value);

template <class T>
static T read_unaligned(uint32_t opcode, uint32_t guest_va, uint8_t *host_va ARGS_SWAP_MUNGED)
{
    if ((sizeof(T) == 8) && (guest_va & 3)) {
#ifndef PPC_TESTS
        ppc_alignment_exception(opcode, guest_va);
#endif
    }

    T result = 0;

    // is it a misaligned cross-page read?
    if ((sizeof(T) > 1) && ((guest_va & 0xFFF) + sizeof(T)) > 0x1000) {
#ifdef MMU_PROFILING
        unaligned_crossp_r++;
#endif
        // Break such a memory access into multiple, bytewise accesses.
        // Because such accesses suffer a performance penalty, they will be
        // presumably very rare so don't waste time optimizing the code below.
        for (int i = 0; i < sizeof(T); guest_va++, i++) {
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
            if (needs_swap) {
                T value = mmu_read_vmem<uint8_t>(opcode, guest_va);
                value <<= (i * 8);
                result |= value;
            } else
#endif
            {
                result = (result << 8) | mmu_read_vmem<uint8_t>(opcode, guest_va);
            }
        }
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE || SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    } else if (sizeof(T) == sizeof(uint64_t) && munged) {
        // Munged host address for an unaligned 64-bit read.
        // Check for cross-page read, to read the upper 32 bits correctly.
        if (((guest_va & 0xFFF) + 12) > 0x1000) {
            // Add the pre-munged address, as munging is a no-op for uint64_t, but not for uint32_t.
            result = mmu_read_vmem<uint32_t>(opcode, guest_va + mem_munge_address<uint32_t>(8));
        } else {
            result =
                #if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                    needs_swap ? (READ_DWORD_LE_U(host_va + 8)) :
                #endif
                (READ_DWORD_BE_U(host_va + 8));
        }
        result <<= 32;
        result |=
            #if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                needs_swap ? (READ_DWORD_LE_U(host_va)) :
            #endif
            (READ_DWORD_BE_U(host_va));
#endif
    } else {
#ifdef MMU_PROFILING
        unaligned_reads++;
#endif
        switch(sizeof(T)) {
            case 1:
                return *host_va;
            case 2:
                return
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                    needs_swap ? (READ_WORD_LE_U(host_va)) :
#endif
                    (READ_WORD_BE_U(host_va));
            case 4:
                return
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                    needs_swap ? (READ_DWORD_LE_U(host_va)) :
#endif
                    (READ_DWORD_BE_U(host_va));
            case 8:
                return
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
                    needs_swap ? (READ_QWORD_LE_U(host_va)) :
#endif
                    (READ_QWORD_BE_U(host_va));
        }
    }
    return result;
}

// explicitely instantiate all required read_unaligned variants
template uint16_t read_unaligned<uint16_t>(uint32_t opcode, uint32_t guest_va, uint8_t* host_va ARGS_SWAP_MUNGED);
template uint32_t read_unaligned<uint32_t>(uint32_t opcode, uint32_t guest_va, uint8_t* host_va ARGS_SWAP_MUNGED);
template uint64_t read_unaligned<uint64_t>(uint32_t opcode, uint32_t guest_va, uint8_t* host_va ARGS_SWAP_MUNGED);

template <class T>
static void write_unaligned(uint32_t opcode, uint32_t guest_va, uint8_t *host_va, T value ARGS_SWAP_MUNGED)
{
    if ((sizeof(T) == 8) && (guest_va & 3)) {
#ifndef PPC_TESTS
        ppc_alignment_exception(opcode, guest_va);
#endif
    }

    // is it a misaligned cross-page write?
    if ((sizeof(T) > 1) && ((guest_va & 0xFFF) + sizeof(T)) > 0x1000) {
#ifdef MMU_PROFILING
        unaligned_crossp_w++;
#endif
        // Break such a memory access into multiple, bytewise accesses.
        // Because such accesses suffer a performance penalty, they will be
        // presumably very rare so don't waste time optimizing the code below.

        uint32_t shift = (sizeof(T) - 1) * 8;

        for (int i = 0; i < sizeof(T); shift -= 8, guest_va++, i++) {
            mmu_write_vmem<uint8_t>(opcode, guest_va, (value >> shift) & 0xFF);
        }
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE || SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
    } else if (sizeof(T) == sizeof(uint64_t) && munged) {
        // Munged host address for an unaligned 64-bit write.
        // Check for cross-page write, to write the upper 32 bits correctly.
        uint32_t value32 = (value >> 32);
        if (((guest_va & 0xFFF) + 12) > 0x1000) {
    #if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
            // The value is endianness swapped already if needs_swap is true, so swap it back in that case.
            if (needs_swap) {
                value32 = (uint32_t)value;
                value32 = BYTESWAP_32(value32);
            }
    #endif
            // Add the pre-munged address, as munging is a no-op for uint64_t, but not for uint32_t.
            mmu_write_vmem<uint32_t>(opcode, guest_va + mem_munge_address<uint32_t>(8), value32);
        } else {
            // Not cross-page, so just write via host address.
            WRITE_DWORD_BE_U(host_va + 8, value32);
        }
        // Write the lower 32 bits.
    #if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
        if (needs_swap) {
            value32 = (value >> 32);
        } else
    #endif
        {
            value32 = (uint32_t)value;
        }
        WRITE_DWORD_BE_U(host_va, value32);
#endif
    } else {
#ifdef MMU_PROFILING
        unaligned_writes++;
#endif
        switch(sizeof(T)) {
            case 1:
                *host_va = value;
                break;
            case 2:
                WRITE_WORD_BE_U(host_va, value);
                break;
            case 4:
                WRITE_DWORD_BE_U(host_va, value);
                break;
            case 8:
                WRITE_QWORD_BE_U(host_va, value);
                break;
        }
    }
}

// explicitely instantiate all required write_unaligned variants
template void write_unaligned<uint16_t>(uint32_t opcode, uint32_t guest_va, uint8_t *host_va, uint16_t value ARGS_SWAP_MUNGED);
template void write_unaligned<uint32_t>(uint32_t opcode, uint32_t guest_va, uint8_t *host_va, uint32_t value ARGS_SWAP_MUNGED);
template void write_unaligned<uint64_t>(uint32_t opcode, uint32_t guest_va, uint8_t *host_va, uint64_t value ARGS_SWAP_MUNGED);


/* MMU profiling. */
#ifdef MMU_PROFILING

#include "utils/profiler.h"
#include <memory>

class MMUProfile : public BaseProfile {
public:
    MMUProfile() : BaseProfile("PPC_MMU") {}

    void populate_variables(std::vector<ProfileVar>& vars) {
        vars.clear();

        vars.push_back({.name = "Data Memory Reads Total",
                        .format = ProfileVarFmt::DEC,
                        .value = dmem_reads_total});

        vars.push_back({.name = "I/O Memory Reads Total",
                        .format = ProfileVarFmt::DEC,
                        .value = iomem_reads_total});

        vars.push_back({.name = "Data Memory Writes Total",
                        .format = ProfileVarFmt::DEC,
                        .value = dmem_writes_total});

        vars.push_back({.name = "I/O Memory Writes Total",
                        .format = ProfileVarFmt::DEC,
                        .value = iomem_writes_total});

        vars.push_back({.name = "Reads from Executable Memory",
                        .format = ProfileVarFmt::DEC,
                        .value = exec_reads_total});

        vars.push_back({.name = "BAT Translations Total",
                        .format = ProfileVarFmt::DEC,
                        .value = bat_transl_total});

        vars.push_back({.name = "Page Table Translations Total",
                        .format = ProfileVarFmt::DEC,
                        .value = ptab_transl_total});

        vars.push_back({.name = "Full PAT TLB Flushes Total",
                        .format = ProfileVarFmt::DEC,
                        .value = pat_flushes_total});

        vars.push_back({.name = "Unaligned Reads Total",
                        .format = ProfileVarFmt::DEC,
                        .value = unaligned_reads});

        vars.push_back({.name = "Unaligned Writes Total",
                        .format = ProfileVarFmt::DEC,
                        .value = unaligned_writes});

        vars.push_back({.name = "Unaligned Crosspage Reads Total",
                        .format = ProfileVarFmt::DEC,
                        .value = unaligned_crossp_r});

        vars.push_back({.name = "Unaligned Crosspage Writes Total",
                        .format = ProfileVarFmt::DEC,
                        .value = unaligned_crossp_w});
    }

    void reset() {
        dmem_reads_total   = 0;
        iomem_reads_total  = 0;
        dmem_writes_total  = 0;
        iomem_writes_total = 0;
        exec_reads_total   = 0;
        bat_transl_total   = 0;
        ptab_transl_total  = 0;
        pat_flushes_total  = 0;
        unaligned_reads    = 0;
        unaligned_writes   = 0;
        unaligned_crossp_r = 0;
        unaligned_crossp_w = 0;
    }
};
#endif

/* SoftTLB profiling. */
#ifdef TLB_PROFILING

#include "utils/profiler.h"
#include <memory>

class TLBProfile : public BaseProfile {
public:
    TLBProfile() : BaseProfile("PPC:MMU:TLB") {}

    void populate_variables(std::vector<ProfileVar>& vars) {
        vars.clear();

        vars.push_back({.name = "Number of hits in the primary ITLB",
            .format = ProfileVarFmt::DEC,
            .value = num_primary_itlb_hits});

        vars.push_back({.name = "Number of hits in the secondary ITLB",
            .format = ProfileVarFmt::DEC,
            .value = num_secondary_itlb_hits});

        vars.push_back({.name = "Number of ITLB refills",
            .format = ProfileVarFmt::DEC,
            .value = num_itlb_refills});

        vars.push_back({.name = "Number of hits in the primary DTLB",
            .format = ProfileVarFmt::DEC,
            .value = num_primary_dtlb_hits});

        vars.push_back({.name = "Number of hits in the secondary DTLB",
            .format = ProfileVarFmt::DEC,
            .value = num_secondary_dtlb_hits});

        vars.push_back({.name = "Number of DTLB refills",
            .format = ProfileVarFmt::DEC,
            .value = num_dtlb_refills});

        vars.push_back({.name = "Number of replaced TLB entries",
            .format = ProfileVarFmt::DEC,
            .value = num_entry_replacements});
    }

    void reset() {
        num_primary_dtlb_hits   = 0;
        num_secondary_dtlb_hits = 0;
        num_dtlb_refills        = 0;
        num_entry_replacements = 0;
    }
};
#endif

uint64_t mem_read_dbg(uint32_t virt_addr, uint32_t size) {
    uint32_t save_dsisr, save_dar;
    uint64_t ret_val;

    /* save MMU-related CPU state */
    save_dsisr            = ppc_state.spr[SPR::DSISR];
    save_dar              = ppc_state.spr[SPR::DAR];
    mmu_exception_handler = dbg_exception_handler;

    try {
        switch (size) {
        case 1:
            ret_val = mmu_read_vmem<uint8_t>(NO_OPCODE, virt_addr);
            break;
        case 2:
            ret_val = mmu_read_vmem<uint16_t>(NO_OPCODE, virt_addr);
            break;
        case 4:
            ret_val = mmu_read_vmem<uint32_t>(NO_OPCODE, virt_addr);
            break;
        case 8:
            ret_val = mmu_read_vmem<uint64_t>(NO_OPCODE, virt_addr);
            break;
        default:
            ret_val = mmu_read_vmem<uint8_t>(NO_OPCODE, virt_addr);
        }
    }
    catch (std::invalid_argument& exc) {
        /* restore MMU-related CPU state */
        mmu_exception_handler     = ppc_exception_handler;
        ppc_state.spr[SPR::DSISR] = save_dsisr;
        ppc_state.spr[SPR::DAR]   = save_dar;

        /* rethrow MMU exception */
        throw exc;
    }
    catch (...) {
        /* restore MMU-related CPU state */
        mmu_exception_handler     = ppc_exception_handler;
        ppc_state.spr[SPR::DSISR] = save_dsisr;
        ppc_state.spr[SPR::DAR]   = save_dar;

        /* rethrow MMU exception */
        throw(false);
    }

    /* restore MMU-related CPU state */
    mmu_exception_handler     = ppc_exception_handler;
    ppc_state.spr[SPR::DSISR] = save_dsisr;
    ppc_state.spr[SPR::DAR]   = save_dar;

    return ret_val;
}

void mem_write_dbg(uint32_t virt_addr, uint64_t value, int size) {
    uint32_t save_dsisr, save_dar;
    uint64_t ret_val;

    // save MMU-related CPU state
    save_dsisr            = ppc_state.spr[SPR::DSISR];
    save_dar              = ppc_state.spr[SPR::DAR];
    mmu_exception_handler = dbg_exception_handler;

    try {
        switch (size) {
        case 1:
            mmu_write_vmem<uint8_t>(NO_OPCODE, virt_addr, value);
            break;
        case 2:
            mmu_write_vmem<uint16_t>(NO_OPCODE, virt_addr, value);
            break;
        case 4:
            mmu_write_vmem<uint32_t>(NO_OPCODE, virt_addr, uint32_t(value));
            break;
        case 8:
            mmu_write_vmem<uint64_t>(NO_OPCODE, virt_addr, value);
            break;
        default:
            mmu_write_vmem<uint8_t>(NO_OPCODE, virt_addr, value);
        }
    } catch (std::invalid_argument& exc) {
        // restore MMU-related CPU state
        mmu_exception_handler     = ppc_exception_handler;
        ppc_state.spr[SPR::DSISR] = save_dsisr;
        ppc_state.spr[SPR::DAR]   = save_dar;

        // rethrow MMU exception
        throw exc;
    }

    // restore MMU-related CPU state
    mmu_exception_handler     = ppc_exception_handler;
    ppc_state.spr[SPR::DSISR] = save_dsisr;
    ppc_state.spr[SPR::DAR]   = save_dar;
}

bool mmu_translate_dbg(uint32_t guest_va, uint32_t &guest_pa) {
    uint32_t save_dsisr, save_dar;
    bool is_mapped;

    /* save MMU-related CPU state */
    save_dsisr            = ppc_state.spr[SPR::DSISR];
    save_dar              = ppc_state.spr[SPR::DAR];
    mmu_exception_handler = dbg_exception_handler;

    try {
        TLBEntry *tlb1_entry, *tlb2_entry;

        const uint32_t tag = guest_va & ~0xFFFUL;

        TLBLookupResult tlb_lookup = lookup_tlb<TLBType::DTLB>(guest_va, tag);
        tlb1_entry = tlb_lookup.primary_entry;

        do {
            if (!tlb_lookup.primary_hit) {
                tlb2_entry = tlb_lookup.matched_entry;
                if (tlb2_entry == nullptr) {
                    // secondary TLB miss ->
                    // perform full address translation and refill the secondary TLB
                    tlb2_entry = dtlb2_refill(guest_va, 0, true);
                    if (tlb2_entry->flags & PAGE_NOPHYS) {
                        is_mapped = false;
                        break;
                    }
                }

                if (tlb2_entry->flags & TLBFlags::PAGE_MEM) { // is it a real memory region?
                    // refill the primary TLB
                    promote_tlb_entry<TLBType::DTLB>(tlb1_entry, tlb2_entry);
                }
                else {
                    tlb1_entry = tlb2_entry;
                }
            }
            guest_pa = tlb1_entry->phys_tag | (guest_va & 0xFFFUL);
            is_mapped = true;
        } while (0);
    }
    catch (std::invalid_argument& exc) {
        LOG_F(WARNING, "Unmapped address 0x%08X", guest_va);
        is_mapped = false;
    }
    catch (...) {
        LOG_F(WARNING, "Unmapped address 0x%08X", guest_va);
        is_mapped = false;
    }

    /* restore MMU-related CPU state */
    mmu_exception_handler     = ppc_exception_handler;
    ppc_state.spr[SPR::DSISR] = save_dsisr;
    ppc_state.spr[SPR::DAR]   = save_dar;

    return is_mapped;
}

template <std::size_t N>
static void invalidate_tlb_entries(std::array<TLBEntry, N> &tlb) {
    for (auto &tlb_el : tlb) {
        sheep_fast_invalidate(&tlb_el);
        tlb_el.tag = TLB_INVALID_TAG;
        tlb_el.flags = 0;
        tlb_el.lru_bits = 0;
        tlb_el.host_va_offs_r = 0;
        tlb_el.host_va_offs_w = 0;
        tlb_el.phys_tag = 0;
        tlb_el.reserved = 0;
    }
}

void ppc_mmu_init()
{
    sheep_mmu_rom_window_enable(false);
    gPendingIInvalidationSources = 0;
    gPendingDInvalidationSources = 0;
    gTrackedIEntries.clear();
    gTrackedDEntries.clear();

    last_ptab_area  = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0, nullptr, nullptr};

    mmu_exception_handler = ppc_exception_handler;

    if (is_601) {
        // use 601-style unified BATs
        ibat_update = &mpc601_bat_update;
        dbat_update = &mpc601_dbat_update;
    } else {
        // use PPC-style BATs
        ibat_update = &ppc_ibat_update;
        dbat_update = &ppc_dbat_update;
    }

    int reg;
    for (reg = 528; reg <= 535; reg++)
        ibat_update(reg);
    for (reg = 536; reg <= 543; reg++)
        dbat_update(reg);

    // invalidate all IDTLB entries
    invalidate_tlb_entries(itlb1_mode1);
    invalidate_tlb_entries(itlb1_mode2);
    invalidate_tlb_entries(itlb1_mode3);
    invalidate_tlb_entries(itlb2_mode1);
    invalidate_tlb_entries(itlb2_mode2);
    invalidate_tlb_entries(itlb2_mode3);
    // invalidate all DTLB entries
    invalidate_tlb_entries(dtlb1_mode1);
    invalidate_tlb_entries(dtlb1_mode2);
    invalidate_tlb_entries(dtlb1_mode3);
    invalidate_tlb_entries(dtlb2_mode1);
    invalidate_tlb_entries(dtlb2_mode2);
    invalidate_tlb_entries(dtlb2_mode3);

    mmu_change_mode();

#ifdef MMU_PROFILING
    gProfilerObj->register_profile("PPC:MMU",
        std::unique_ptr<BaseProfile>(new MMUProfile()));
#endif

#ifdef TLB_PROFILING
    gProfilerObj->register_profile("PPC:MMU:TLB",
    std::unique_ptr<BaseProfile>(new TLBProfile()));
#endif
}

// Experimental JIT: return only an existing primary RAM mapping. No refill,
// exception, MMIO access, permission change or PTE dirty-bit side effect here.
uint8_t *sheep_mmu_fastmem(uint32_t va,unsigned size,bool write)
{
 if((size!=1&&size!=2&&size!=4)||((va&4095)+size>4096))return nullptr;
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
 if(ppc_state.is_LE)return nullptr;
#endif
#if SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
 if(mem_ctrl_instance->needs_swap_endian(false))return nullptr;
#endif
 auto *entry=&pCurDTLB1[(va>>PPC_PAGE_SIZE_BITS)&tlb_size_mask];
 if(entry->tag!=(va&PPC_PAGE_MASK)||!(entry->flags & TLBFlags::PAGE_MEM))return nullptr;
 if(write && ((entry->flags & (TLBFlags::PAGE_WRITABLE|TLBFlags::PTE_SET_C)) != (TLBFlags::PAGE_WRITABLE|TLBFlags::PTE_SET_C)))return nullptr;
 return reinterpret_cast<uint8_t*>((write?entry->host_va_offs_w:entry->host_va_offs_r)+va);
}

#include "nw_jit.h"
#include <cstddef>
void sheep_mmu_jit_layout(nw_jit_layout *layout)
{
#if !SUPPORTS_PPC_LITTLE_ENDIAN_MODE && !SUPPORTS_MEMORY_CTRL_ENDIAN_MODE
 static_assert(sizeof(TLBEntry)==32 && alignof(TLBEntry)>=8 && offsetof(TLBEntry,tag)==0 && offsetof(TLBEntry,flags)==4 && offsetof(TLBEntry,lru_bits)==6);
 static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__, "Fused TLB word requires little-endian host");
 static_assert(offsetof(TLBEntry,host_va_offs_r)==8 && offsetof(TLBEntry,host_va_offs_w)==16);
 static_assert(TLBFlags::PAGE_MEM==1 && TLBFlags::PAGE_WRITABLE==32 && TLBFlags::PTE_SET_C==64);
 layout->dtlb_slot=reinterpret_cast<uintptr_t>(&pCurDTLB1);
 layout->tlb_mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);
 pippin_fastmem_mmu(reinterpret_cast<uintptr_t>(&pCurDTLB2),reinterpret_cast<uintptr_t>(&tlb_size_mask));
 pippin_fastmem_refresher(sheep_fast_repair);
 sheep_fast_select();
 if(pippin_fastmem_enabled()){layout->fast_read_slot=reinterpret_cast<uintptr_t>(&pippin_fastmem_read);layout->fast_write_slot=reinterpret_cast<uintptr_t>(&pippin_fastmem_write);}
 static_assert(TLB_SIZE==4096);
 layout->tlb_index_bits=tlb_size_mask==TLB_SIZE-1?12:0;
#endif
}

// Private native-JIT oracle fixture. Used before guest execution only; the
// existing current DTLB pointer/mask are restored before returning to the ROM.
void sheep_mmu_test_page(uint8_t *ram,uint32_t va,uint16_t flags,bool miss)
{
 static std::array<TLBEntry,TLB_SIZE> fixture;
 static TLBEntry *saved=nullptr;static uint32_t saved_mask;
 if(!ram){pCurDTLB1=saved;tlb_size_mask=saved_mask;saved=nullptr;sheep_fast_select();return;}
 if(!saved){saved=pCurDTLB1;saved_mask=tlb_size_mask;}
 for(auto &entry:fixture)entry.tag=0xffffffff;
 auto &entry=fixture[(va>>12)&(TLB_SIZE-1)];
 entry.tag=miss?(va^0x1000):va;entry.flags=flags|0xa500;entry.lru_bits=0xa55a;
 entry.host_va_offs_r=entry.host_va_offs_w=reinterpret_cast<intptr_t>(ram)-va;
 pCurDTLB1=fixture.data();tlb_size_mask=TLB_SIZE-1;sheep_fast_select();
}

bool sheep_mmu_code_is_rom(const uint8_t *code,uint32_t pc)
{
 auto *entry=&pCurITLB1[(pc>>12)&tlb_size_mask];
 if(entry->tag!=(pc&PPC_PAGE_MASK)||!(entry->flags&TLBFlags::PAGE_MEM)||reinterpret_cast<const uint8_t*>(entry->host_va_offs_r+pc)!=code)return false;
 auto *region=mem_ctrl_instance->find_range(entry->phys_tag);
 return region&&(region->type&RT_ROM)&&!(region->type&RT_RAM);
}

// Pippin's fixed ROM allocation also backs its instruction aliases. This
// classification only gates native lookup; publishing a translation still
// validates its current ITLB mapping and RT_ROM attributes independently.
void sheep_mmu_rom_bounds(uintptr_t &begin,uintptr_t &end)
{
 begin=end=0;
 auto *region=mem_ctrl_instance->find_rom_region();
 if(region&&region->type==RT_ROM&&region->mem_ptr){
  begin=reinterpret_cast<uintptr_t>(region->mem_ptr);
  end=begin+(uint64_t(region->end)-region->start+1);
 }
}

// Q203: called only on a settled canonical fetch, when the live token is invalid.
extern std::vector<std::function<void()>> gCtxSyncCallbacks;
bool sheep_mmu_rom_window_cold_refresh(uint32_t pc, const uint8_t *current_host) noexcept
{
    if (!sheep_rom_window_enabled) return false;
    if (sheep_rom_window.valid_start_count) {
        const uint32_t delta = pc - sheep_rom_window.guest_start;
        if (sheep_rom_window.cpu_base == uintptr_t(&ppc_state) &&
            !(pc & 3u) && delta < sheep_rom_window.valid_start_count &&
            uintptr_t(pc) + sheep_rom_window.host_offset == uintptr_t(current_host))
            return true;
        sheep_mmu_rom_window_invalidate();
    }
    const uint32_t msr = ppc_state.msr;
    if (is_601 || include_601 || ppc_state.spr[SPR::PVR] != PPC_VER::MPC603 ||
        !(msr & MSR::IR) || (msr & MSR::LE) || exec_flags ||
        gPendingIInvalidationSources || !gCtxSyncCallbacks.empty() ||
        !mem_ctrl_instance || !current_host || !sheep_rom_bind_owner() ||
        ((uintptr_t(current_host) | uintptr_t(pc)) & 3u) ||
        tlb_size_mask != TLB_SIZE - 1)
        return false;
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
    if (ppc_state.is_LE) return false;
#endif
    const bool user = (msr & MSR::PR) != 0;
    const unsigned mode = 2u | unsigned(user);
    if (CurITLBMode != mode ||
        pCurITLB1 != (user ? itlb1_mode3.data() : itlb1_mode2.data()) ||
        pCurITLB2 != (user ? itlb2_mode3.data() : itlb2_mode2.data()))
        return false;

    struct Range { uint64_t begin, size, physical; bool active; } ranges[4];
    const unsigned access = user ? 1u : 2u;
    int selected = -1;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t upper = ppc_state.spr[528 + 2 * i];
        const uint32_t lower = ppc_state.spr[529 + 2 * i];
        const uint32_t bl = (upper >> 2) & 0x7ffu;
        if ((bl & (bl + 1u)) || (upper & 0x0001e000u) || (lower & 0x0001ff84u))
            return false; // contiguous legal BL and no reserved raw BAT bits
        const uint32_t mask = ~((bl << 17) | 0x1ffffu);
        const uint32_t logical = upper & mask, physical = lower & mask;
        const auto &derived = ibat_array[i]; // valid is meaningful only for601
        if (derived.access != (upper & 3u) || derived.prot != (lower & 3u) ||
            derived.hi_mask != mask || derived.phys_hi != physical ||
            derived.bepi != logical ||
            (upper & 0xfffe0000u) != logical ||
            (lower & 0xfffe0000u) != physical)
            return false;
        ranges[i] = {logical, (uint64_t(bl) + 1u) << 17, physical,
                     (upper & access) != 0};
        if (ranges[i].active && (pc & mask) == logical) {
            if (selected >= 0 || !(lower & 3u) || (lower & 8u)) return false;
            selected = int(i);
        }
    }
    if (selected < 0) return false;
    const auto &chosen = ranges[selected];
    const uint64_t logical_end = chosen.begin + chosen.size;
    const uint64_t physical_end = chosen.physical + chosen.size;
    if (logical_end > (uint64_t(1) << 32) || physical_end > (uint64_t(1) << 32))
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        if (int(i) != selected && ranges[i].active &&
            ranges[i].begin < logical_end &&
            chosen.begin < ranges[i].begin + ranges[i].size)
            return false; // refuse all overlap, including higher-priority denied BATs
    }
    const uint64_t physical_pc = chosen.physical + (uint64_t(pc) - chosen.begin);
    const auto &entry = pCurITLB1[(pc >> PPC_PAGE_SIZE_BITS) & tlb_size_mask];
    const uint16_t required = TLBFlags::PAGE_MEM | TLBFlags::TLBE_FROM_BAT;
    if (entry.tag != (pc & PPC_PAGE_MASK) || (entry.flags & required) != required ||
        (entry.flags & TLBFlags::PAGE_IO) ||
        (entry.flags & TLBE_FROM_TRANSLATION) != TLBFlags::TLBE_FROM_BAT ||
        uint64_t(entry.phys_tag) + (pc & (PPC_PAGE_SIZE - 1u)) != physical_pc ||
        uintptr_t(entry.host_va_offs_r) + uintptr_t(pc) != uintptr_t(current_host))
        return false;
    auto *rom = mem_ctrl_instance->find_range(uint32_t(physical_pc));
    if (!rom || rom->type != RT_ROM || !rom->mem_ptr ||
        mem_ctrl_instance->find_rom_region() != rom)
        return false;
    const uint64_t rom_end = uint64_t(rom->end) + 1u;
    const uint64_t first = chosen.physical > rom->start ? chosen.physical : rom->start;
    const uint64_t last = physical_end < rom_end ? physical_end : rom_end;
    if (last <= first || physical_pc < first || physical_pc >= last ||
        ((first | last) & (PPC_PAGE_SIZE - 1u)))
        return false;
    const uint64_t size = last - first;
    if (size > UINT32_MAX ||
        !mem_ctrl_instance->instruction_rom_span(rom, uint32_t(first), size))
        return false; // entire physical interval must have this unique ROM descriptor
    const uintptr_t host = uintptr_t(rom->mem_ptr);
    const uint64_t allocation = rom_end - uint64_t(rom->start);
    if (allocation > uint64_t(UINTPTR_MAX - host)) return false;
    const uintptr_t mapped = host + uintptr_t(first - uint64_t(rom->start));
    const uint64_t begin = chosen.begin + first - chosen.physical;
    if (mapped + uintptr_t(physical_pc - first) != uintptr_t(current_host))
        return false;
    if (size < 4 || begin > UINT32_MAX || begin + size > (uint64_t(1) << 32))
        return false;
    sheep_rom_window.guest_start = uint32_t(begin);
    sheep_rom_window.host_offset = mapped - uintptr_t(begin);
    sheep_rom_window.cpu_base = uintptr_t(&ppc_state);
    sheep_rom_window.valid_start_count = uint32_t(size - 3u); // publish validity last
    return true;
}

void sheep_mmu_test_data_pair(uint8_t *read,uint8_t *write,uint32_t page,uint16_t first_flags,uint16_t second_flags,bool second_valid)
{
 static std::array<TLBEntry,TLB_SIZE> fixture;static TLBEntry *saved=nullptr;static uint32_t mask;
 if(!read){pCurDTLB1=saved;tlb_size_mask=mask;saved=nullptr;sheep_fast_select();return;}
 if(!saved){saved=pCurDTLB1;mask=tlb_size_mask;}
 for(auto &entry:fixture)entry.tag=0xffffffff;
 for(unsigned i=0;i<2;i++){
  auto &entry=fixture[((page>>12)+i)&(TLB_SIZE-1)];entry.tag=i&&!second_valid?0xffffffff:page+i*4096;
  entry.flags=(i?second_flags:first_flags)|0x5a00;entry.lru_bits=0x5aa5;entry.host_va_offs_r=reinterpret_cast<intptr_t>(read)-page+i*16;entry.host_va_offs_w=reinterpret_cast<intptr_t>(write)-page+i*16;
 }
 pCurDTLB1=fixture.data();tlb_size_mask=TLB_SIZE-1;sheep_fast_select();
}


#include "sheep_jit_bridge.h"
#if defined(__aarch64__) && !SUPPORTS_PPC_LITTLE_ENDIAN_MODE && !SUPPORTS_MEMORY_CTRL_ENDIAN_MODE && !defined(MMU_PROFILING) && !defined(TLB_PROFILING) && !defined(LOG_INSTRUCTIONS)
static_assert(sizeof(TLBEntry)==32 && offsetof(TLBEntry,tag)==0 && offsetof(TLBEntry,host_va_offs_r)==8 && offsetof(TLBEntry,phys_tag)==24);
const bool sheep_itlb_primary_enabled=true;
#else
const bool sheep_itlb_primary_enabled=false;
#endif
void sheep_mmu_itlb_selftest()
{
#if !SUPPORTS_PPC_LITTLE_ENDIAN_MODE && !SUPPORTS_MEMORY_CTRL_ENDIAN_MODE && !defined(MMU_PROFILING) && !defined(TLB_PROFILING) && !defined(LOG_INSTRUCTIONS)
 static_assert(sizeof(TLBEntry)==32 && offsetof(TLBEntry,tag)==0 && offsetof(TLBEntry,host_va_offs_r)==8 && offsetof(TLBEntry,phys_tag)==24);
 struct Restore { TLBEntry *table=pCurITLB1;uint32_t mask=tlb_size_mask;~Restore(){pCurITLB1=table;tlb_size_mask=mask;} } restore;
 static std::array<TLBEntry,TLB_SIZE> tables[2];
 std::array<uint8_t,4096> ram[2];unsigned passed=0;
 for(unsigned which=0;which<2;which++)for(uint32_t mask:{3u,15u,4095u})
 for(uint32_t page:{0u,0x1000u,0x68066000u,0xffc00000u,0xfffff000u})
 for(unsigned offset:{0u,1u,2u,4u,4092u,4095u})for(unsigned flags:{0u,1u,2u,128u}){
  pCurITLB1=tables[which].data();tlb_size_mask=mask;
  auto &entry=pCurITLB1[(page>>12)&mask];entry.tag=page;entry.flags=flags;
  entry.host_va_offs_r=reinterpret_cast<intptr_t>(ram[which].data())-int64_t(page);
  entry.phys_tag=0x12345000u+which*0x10000u;
  uint32_t va=page+offset,actual=0xdeadbeef,canonical=0xcafebabe;
  auto *fast=sheep_itlb_primary_hit(va,&actual);auto *original=mmu_translate_imem(va,&canonical);
  if(fast!=original||fast!=ram[which].data()+offset||actual!=canonical||sheep_translate_imem(va)!=original)
   throw std::runtime_error("Primary ITLB hit differs from canonical translation");
  passed++;
  entry.tag^=4096;actual=0xdeadbeef;
  if(sheep_itlb_primary_hit(va,&actual)!=nullptr||actual!=0xdeadbeef)
   throw std::runtime_error("Primary ITLB miss changed translation output");
  passed++;
 }
 fprintf(stderr,"ITLB PRIMARY SELFTEST %u table/mask/tag/physical/offset cases passed\n",passed);
#endif
}

void sheep_mmu_split_read_selftest(){
#if defined(__aarch64__) && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__ && !SUPPORTS_PPC_LITTLE_ENDIAN_MODE && !SUPPORTS_MEMORY_CTRL_ENDIAN_MODE && !defined(MMU_PROFILING) && !defined(TLB_PROFILING) && !defined(LOG_INSTRUCTIONS)
 struct Restore{TLBEntry *table=pCurDTLB1;uint32_t mask=tlb_size_mask;~Restore(){pCurDTLB1=table;tlb_size_mask=mask;sheep_fast_select();}}restore;
 static std::array<TLBEntry,TLB_SIZE> tables[2];alignas(16) std::array<uint8_t,4096> ram[2];unsigned passed=0;
 for(unsigned which=0;which<2;which++)for(unsigned i=0;i<4096;i++)ram[which][i]=uint8_t(i*37+which*13+11);
 for(unsigned which=0;which<2;which++)for(uint32_t mask:{3u,15u,4095u})for(uint32_t page:{0u,0x1000u,0x68066000u,0xffc00000u,0xfffff000u})
 for(unsigned offset:{0u,1u,2u,4u,8u,4088u,4092u,4095u})for(unsigned size:{1u,2u,4u,8u})for(unsigned flags:{0u,1u,0x61u,0xa561u}){
  pCurDTLB1=tables[which].data();tlb_size_mask=mask;sheep_fast_select();auto &entry=pCurDTLB1[(page>>12)&mask];entry.tag=page;entry.flags=flags;entry.lru_bits=0xa55a;entry.host_va_offs_r=reinterpret_cast<intptr_t>(ram[which].data())-int64_t(page);
  uint32_t va=page+offset;auto *host=sheep_primary_read_host(va,size);bool aligned=!(va&(size-1)),valid=aligned&&(flags&1);
  if(bool(host)!=valid||(host&&host!=ram[which].data()+offset))throw std::runtime_error("Split MMU primary hit differs");
  if(aligned){bool same=false;switch(size){
   case 1:same=mmu_read_vmem<uint8_t>(0,va)==mmu_read_vmem_slow<uint8_t>(0,va);break;
   case 2:same=mmu_read_vmem<uint16_t>(0,va)==mmu_read_vmem_slow<uint16_t>(0,va);break;
   case 4:same=mmu_read_vmem<uint32_t>(0,va)==mmu_read_vmem_slow<uint32_t>(0,va);break;
   case 8:same=mmu_read_vmem<uint64_t>(0,va)==mmu_read_vmem_slow<uint64_t>(0,va);break;
  }if(!same)throw std::runtime_error("Split MMU read value differs from canonical slow body");}
  passed++;entry.tag^=4096;if(sheep_primary_read_host(va,size))throw std::runtime_error("Split MMU accepted primary tag miss");passed++;
 }
 fprintf(stderr,"SPLIT MMU SELFTEST %u mapping/flags/size/alignment/value cases passed\n",passed);
#endif
}

// Private fixtures bind a real primary slot, including its fastmem coherence
// callbacks. The original slot and active table are restored before boot.
static TLBEntry *fast_fixture_slot=nullptr,*fast_fixture_table=nullptr;
static std::vector<std::pair<TLBEntry*,TLBEntry>> fast_fixture_secondary_saved;
static TLBEntry fast_fixture_saved{};static uint32_t fast_fixture_mask=0;
void sheep_mmu_test_fast_page(uint8_t *read,uint8_t *write,uint32_t va,uint16_t flags,bool miss,unsigned mode){
 if(!read){
  if(!fast_fixture_slot)throw std::runtime_error("Fastmem fixture restore without binding");
  sheep_fast_invalidate(fast_fixture_slot);*fast_fixture_slot=fast_fixture_saved;sheep_fast_publish(fast_fixture_slot);
  for(auto &item:fast_fixture_secondary_saved){sheep_fast_invalidate(item.first);*item.first=item.second;sheep_fast_publish(item.first);}fast_fixture_secondary_saved.clear();
  pCurDTLB1=fast_fixture_table;tlb_size_mask=fast_fixture_mask;fast_fixture_slot=nullptr;fast_fixture_table=nullptr;sheep_fast_select();return;
 }
 if(fast_fixture_slot||mode>=3||(va&4095))throw std::runtime_error("Invalid fastmem fixture binding");
 fast_fixture_table=pCurDTLB1;fast_fixture_mask=tlb_size_mask;pCurDTLB1=sheep_fast_owners[mode];tlb_size_mask=TLB_SIZE-1;
 fast_fixture_slot=&pCurDTLB1[(va>>12)&(TLB_SIZE-1)];fast_fixture_saved=*fast_fixture_slot;sheep_fast_invalidate(fast_fixture_slot);
 *fast_fixture_slot=TLBEntry{};fast_fixture_slot->tag=miss?(va^4096):va;fast_fixture_slot->flags=flags;
 fast_fixture_slot->host_va_offs_r=reinterpret_cast<intptr_t>(read)-va;fast_fixture_slot->host_va_offs_w=reinterpret_cast<intptr_t>(write)-va;
 sheep_fast_publish(fast_fixture_slot);sheep_fast_select();
}
void sheep_mmu_test_fast_transition(unsigned operation){
 if(!fast_fixture_slot)throw std::runtime_error("Fastmem transition without binding");
 if(operation==0){
  auto fresh=*fast_fixture_slot;uintptr_t read=uintptr_t(fresh.host_va_offs_r)+fresh.tag,write=uintptr_t(fresh.host_va_offs_w)+fresh.tag;
  fresh.tag+=TLB_SIZE*4096u;fresh.host_va_offs_r=read-fresh.tag;fresh.host_va_offs_w=write-fresh.tag;
  promote_tlb_entry<TLBType::DTLB>(fast_fixture_slot,&fresh);
 }else if(operation==1){
  fast_fixture_slot->flags|=TLBFlags::TLBE_FROM_PAT;
  std::vector<TLBEntry*> entries{fast_fixture_slot};tlb_invalidate_tracked_entries(entries,TLBFlags::TLBE_FROM_PAT);
 }else if(operation>=4){
  unsigned mode=unsigned(sheep_fast_owner(fast_fixture_slot));uint32_t old_va=fast_fixture_saved.tag;
  if(operation==4){
   old_va=fast_fixture_slot->tag;unsigned index=(old_va>>12)&(TLB_SIZE-1);
   for(unsigned way=0;way<TLB2_WAYS;way++){auto *entry=&sheep_fast_secondary[mode][index*TLB2_WAYS+way];fast_fixture_secondary_saved.emplace_back(entry,*entry);sheep_fast_invalidate(entry);entry->tag=TLB_INVALID_TAG;entry->flags=0;}
   auto *entry=fast_fixture_secondary_saved[0].first;*entry=*fast_fixture_slot;sheep_fast_publish(entry);
  }else{
   if(fast_fixture_secondary_saved.empty())throw std::runtime_error("Secondary fixture not seeded");auto *entry=fast_fixture_secondary_saved[0].first;
   if(operation==5){auto fresh=*entry;uintptr_t r=uintptr_t(fresh.host_va_offs_r)+fresh.tag,w=uintptr_t(fresh.host_va_offs_w)+fresh.tag;sheep_fast_invalidate(entry);fresh.tag+=2*TLB_SIZE*4096u;fresh.host_va_offs_r=r-fresh.tag;fresh.host_va_offs_w=w-fresh.tag;*entry=fresh;sheep_fast_publish(entry);}
   else{entry->flags|=TLBFlags::TLBE_FROM_PAT;std::vector<TLBEntry*> entries{entry};tlb_invalidate_tracked_entries(entries,TLBFlags::TLBE_FROM_PAT);}
  }
 }else{
  fast_fixture_slot->flags=operation==2?0x41:0x61;sheep_fast_publish(fast_fixture_slot);
 }
}

#include "rom-window-tests.inc"
