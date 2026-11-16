// Project: DingusPPC-Pippin
// File: cpu/ppc/rom-window-tests.inc
// Purpose: Verify-mode tests for the ROM window and MMU mode changes.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
//   2026-11-15 - Q227: precise tlbie (congruence class) and ROM window kept across same-mode MMU changes.
// Full history: PIPPIN.md and Git history.
// Q203 startup-only canonical MMU custody. No TimerManager or device I/O.
// All instruction tables are copied because the loop oracle may touch any page
// of this real ROM BAT. Original queue/tracked vectors are moved out, not run.
#include <memory>
#include <cstring>

struct SheepROMWindowFixtureState {
    SetPRS cpu;
    AddressMapEntry* rom = mem_ctrl_instance->find_rom_region();
    AddressMapEntry rom_entry = *rom;
    PPCOpcode* decoder = ppc_opcode_grabber;
    PPC_BAT_entry ibats[4], dbats[4];
    decltype(itlb1_mode1) i1[3];
    decltype(itlb2_mode1) i2[3];
    TLBEntry *ip1 = pCurITLB1, *ip2 = pCurITLB2;
    TLBEntry *dp1 = pCurDTLB1, *dp2 = pCurDTLB2;
    uint8_t im = CurITLBMode, dm = CurDTLBMode;
    uint32_t mask = tlb_size_mask;
    unsigned flags = exec_flags;
    uint32_t nia = ppc_next_instruction_address;
    bool old601 = is_601, extras = include_601;
    uint16_t pi = gPendingIInvalidationSources, pd = gPendingDInvalidationSources;
    std::vector<TLBEntry*> tracked_i;
    std::vector<TLBEntry*> tracked_d;
    std::vector<std::function<void()>> queue;
    SheepROMWindow token = sheep_rom_window;
    bool enabled = sheep_rom_window_enabled;
    MemCtrlBase *owner = sheep_rom_window_owner, *controller = mem_ctrl_instance;
    decltype(ibat_update) old_ibat = ibat_update, old_dbat = dbat_update;
    SheepROMWindowFixtureState() {
        std::memcpy(&cpu, &ppc_state, sizeof(cpu));
        std::memcpy(ibats, ibat_array, sizeof(ibats));
        std::memcpy(dbats, dbat_array, sizeof(dbats));
        i1[0]=itlb1_mode1; i1[1]=itlb1_mode2; i1[2]=itlb1_mode3;
        i2[0]=itlb2_mode1; i2[1]=itlb2_mode2; i2[2]=itlb2_mode3;
        tracked_i=std::move(gTrackedIEntries); tracked_d=std::move(gTrackedDEntries);
        queue=std::move(gCtxSyncCallbacks);
        sheep_mmu_rom_window_enable(false);
        gCtxSyncCallbacks.clear(); gTrackedIEntries.clear(); gTrackedDEntries.clear();
        gPendingIInvalidationSources = gPendingDInvalidationSources = 0;
    }
    ~SheepROMWindowFixtureState() {
        sheep_mmu_rom_window_enable(false);
        mem_ctrl_instance = controller;
        *rom = rom_entry;
        ppc_opcode_grabber = decoder;
        std::memcpy(&ppc_state, &cpu, sizeof(cpu));
        std::memcpy(ibat_array, ibats, sizeof(ibats));
        std::memcpy(dbat_array, dbats, sizeof(dbats));
        itlb1_mode1=i1[0]; itlb1_mode2=i1[1]; itlb1_mode3=i1[2];
        itlb2_mode1=i2[0]; itlb2_mode2=i2[1]; itlb2_mode3=i2[2];
        pCurITLB1=ip1; pCurITLB2=ip2; pCurDTLB1=dp1; pCurDTLB2=dp2;
        CurITLBMode=im; CurDTLBMode=dm; tlb_size_mask=mask;
        is_601=old601; include_601=extras; exec_flags=flags;
        ppc_next_instruction_address=nia;
        ibat_update=std::move(old_ibat); dbat_update=std::move(old_dbat);
        gTrackedIEntries=std::move(tracked_i); gTrackedDEntries=std::move(tracked_d);
        gPendingIInvalidationSources=pi; gPendingDInvalidationSources=pd;
        gCtxSyncCallbacks=std::move(queue);
        sheep_fast_select();
        sheep_rom_window_enabled=enabled;
        sheep_rom_window_owner=nullptr;
        if (owner && owner->bind_instruction_map_observer(&sheep_rom_map_changed)) {
            sheep_rom_window_owner=owner;
            sheep_rom_window=token;
        } else sheep_rom_window.valid_start_count=0;
    }
};
static std::unique_ptr<SheepROMWindowFixtureState> sheep_window_fixture;
static uint32_t sheep_window_fixture_base = 0;
static const uint8_t* sheep_window_fixture_host = nullptr;

const uint8_t* sheep_mmu_rom_window_test_begin(uint32_t guest_start) {
    if (sheep_window_fixture || !mem_ctrl_instance ||
        typeid(*mem_ctrl_instance) != typeid(AspenCtrl) || (guest_start & 0x1ffffu))
        throw std::runtime_error("ROM window fixture requires canonical Aspen and aligned BAT");
    auto* rom = mem_ctrl_instance->find_rom_region();
    if (!rom || rom->type != RT_ROM || !rom->mem_ptr || (rom->start & 0x1ffffu) ||
        uint64_t(rom->end)+1-rom->start < 0x20000u)
        throw std::runtime_error("ROM window fixture requires real 128KiB ROM span");
    sheep_window_fixture.reset(new SheepROMWindowFixtureState);
    try {
        is_601=include_601=false; ppc_state.spr[SPR::PVR]=PPC_VER::MPC603;
        ppc_state.msr=MSR::IR; exec_flags=0; tlb_size_mask=TLB_SIZE-1;
        ibat_update=&ppc_ibat_update; dbat_update=&ppc_dbat_update;
        for (unsigned i=0;i<8;i++) ppc_state.spr[528+i]=0;
        ppc_state.spr[528]=guest_start|3u;
        ppc_state.spr[529]=rom->start|3u;
        for (unsigned i=0;i<4;i++) ppc_ibat_update(528+2*i);
        do_ctx_sync();
        invalidate_tlb_entries(itlb1_mode1); invalidate_tlb_entries(itlb1_mode2);
        invalidate_tlb_entries(itlb1_mode3); invalidate_tlb_entries(itlb2_mode1);
        invalidate_tlb_entries(itlb2_mode2); invalidate_tlb_entries(itlb2_mode3);
        gTrackedIEntries.clear();
        mmu_change_mode();
        sheep_window_fixture_base=guest_start;
        sheep_window_fixture_host=mmu_translate_imem(guest_start);
        sheep_mmu_rom_window_enable(true);
        if (!sheep_mmu_rom_window_cold_refresh(guest_start,sheep_window_fixture_host))
            throw std::runtime_error("ROM window fixture canonical token refused");
        return sheep_window_fixture_host;
    } catch (...) { sheep_window_fixture.reset(); throw; }
}
void sheep_mmu_rom_window_test_end() noexcept {
    sheep_window_fixture.reset();
    sheep_window_fixture_host=nullptr;
}

int sheep_rom_window_selftest() {
    unsigned passed=0;
    auto require=[&](bool ok,const char* what) {
        if(!ok) throw std::runtime_error(std::string("ROM WINDOW failure case=")+std::to_string(passed+1)+" "+what);
        ++passed;
    };
    try {
        for (uint32_t base : {0x10000000u,0xfffe0000u}) {
            const auto* host=sheep_mmu_rom_window_test_begin(base);
            const auto* token=sheep_mmu_rom_window_get();
            require(token==&sheep_rom_window && token->guest_start==base &&
                    token->valid_start_count==0x20000u-3 && token->cpu_base==uintptr_t(&ppc_state),
                    "canonical actual-ROM token ABI");
            for(uint32_t delta : {0u,4u,4092u,4096u,0x1fffcu})
                require(uint32_t(base+delta-token->guest_start)<token->valid_start_count &&
                        uintptr_t(base+delta)+token->host_offset==uintptr_t(host+delta), "four-byte range and affine host");
            require(uint32_t(base+0x20000u-token->guest_start)>=token->valid_start_count &&
                    uint32_t(base-4-token->guest_start)>=token->valid_start_count, "both outside bounds and wrap");
            sheep_mmu_rom_window_test_end();
        }
        const uint32_t base=0x10000000u;
        const auto* host=sheep_mmu_rom_window_test_begin(base);
        auto rearm=[&]() {
            sheep_mmu_rom_window_invalidate(); exec_flags=0;
            if(!sheep_mmu_rom_window_cold_refresh(base,host)) throw std::runtime_error("ROM window rearm failed");
        };
        auto refused=[&](const char* why) {
            sheep_mmu_rom_window_invalidate();
            require(!sheep_mmu_rom_window_cold_refresh(base,host)&&!sheep_rom_window.valid_start_count,why);
        };
        ppc_state.spr[SPR::PVR]^=1; refused("foreign model"); ppc_state.spr[SPR::PVR]^=1;
        is_601=true; refused("601 model"); is_601=false;
        include_601=true; refused("601 extras"); include_601=false;
        ppc_state.msr&=~MSR::IR; refused("IR off"); ppc_state.msr|=MSR::IR;
        ppc_state.msr|=MSR::LE; refused("little endian"); ppc_state.msr&=~MSR::LE;
        ppc_state.msr|=MSR::PR; refused("mode disagrees with raw PR"); ppc_state.msr&=~MSR::PR;
        ppc_state.msr|=MSR::PR; mmu_change_mode();
        auto* user_host=mmu_translate_imem(base);
        require(user_host==host&&sheep_mmu_rom_window_cold_refresh(base,user_host),"canonical user access accepted");
        ppc_state.msr&=~MSR::PR; mmu_change_mode(); mmu_translate_imem(base);
        exec_flags=EXEF_FETCH; refused("pending fetch"); exec_flags=0;
        gPendingIInvalidationSources=TLBE_FROM_BAT; refused("pending I invalidation"); gPendingIInvalidationSources=0;
        gCtxSyncCallbacks.push_back([]{}); refused("pending context callback"); gCtxSyncCallbacks.clear();
        tlb_size_mask=15; refused("noncanonical cache geometry"); tlb_size_mask=TLB_SIZE-1;
        auto* canonical=pCurITLB1; pCurITLB1=itlb1_mode3.data(); refused("foreign primary"); pCurITLB1=canonical;
        auto* secondary=pCurITLB2; pCurITLB2=itlb2_mode3.data(); refused("foreign secondary"); pCurITLB2=secondary;
        require(!sheep_mmu_rom_window_cold_refresh(base+1,host+1),"unaligned source");
        require(!sheep_mmu_rom_window_cold_refresh(base,host+4),"current host mismatch");
        for(unsigned bat=0;bat<4;bat++) {
            auto old=ibat_array[bat];
            ibat_array[bat].access^=1; refused("derived access mismatch"); ibat_array[bat]=old;
            ibat_array[bat].prot^=1; refused("derived protection mismatch"); ibat_array[bat]=old;
            ibat_array[bat].phys_hi^=0x20000; refused("derived physical mismatch"); ibat_array[bat]=old;
            ibat_array[bat].hi_mask^=0x20000; refused("derived mask mismatch"); ibat_array[bat]=old;
            ibat_array[bat].bepi^=0x20000; refused("derived logical mismatch"); ibat_array[bat]=old;
        }
        auto raw=ppc_state.spr[528];
        ppc_state.spr[528]|=2u<<2; refused("noncontiguous BL"); ppc_state.spr[528]=raw;
        ppc_state.spr[528]|=0x2000; refused("reserved BATU bits"); ppc_state.spr[528]=raw;
        auto lower=ppc_state.spr[529];
        ppc_state.spr[529]=lower&~3u; ibat_array[0].prot=0;
        refused("selected BAT protection denied"); ppc_state.spr[529]=lower; ibat_array[0].prot=lower&3u;
        ppc_state.spr[529]=lower|8u; refused("guarded instruction BAT refused"); ppc_state.spr[529]=lower;
        ppc_state.spr[529]|=4; refused("reserved BATL bits"); ppc_state.spr[529]=lower;
        ppc_state.spr[530]=raw; ppc_state.spr[531]=lower&~3u;
        ppc_ibat_update(530); do_ctx_sync(); mmu_translate_imem(base);
        refused("overlapping active denied BAT");
        ppc_state.spr[530]=ppc_state.spr[531]=0; ppc_ibat_update(530); do_ctx_sync();
        mmu_translate_imem(base); rearm();
        auto& entry=pCurITLB1[(base>>12)&tlb_size_mask]; auto saved=entry;
        entry.tag^=4096; refused("source primary tag"); entry=saved;
        entry.flags&=~TLBFlags::TLBE_FROM_BAT; refused("source primary provenance"); entry=saved;
        entry.flags|=TLBFlags::PAGE_IO; refused("source primary IO refused"); entry=saved;
        entry.phys_tag^=4096; refused("source physical mismatch"); entry=saved;
        auto* rom=mem_ctrl_instance->find_rom_region(); auto type=rom->type;
        rom->type=RT_RAM; refused("actual region RAM"); rom->type=type;
        rom->type=RT_MMIO; refused("actual region MMIO"); rom->type=type;
        auto* memory=rom->mem_ptr; rom->mem_ptr=nullptr; refused("null actual backing"); rom->mem_ptr=memory;
        rearm();
        ppc_ibat_update(528); require(!sheep_rom_window.valid_start_count,"real IBAT writer revokes immediately");
        do_ctx_sync(); mmu_translate_imem(base); rearm();
        add_ctx_sync_action([]{}); require(!sheep_rom_window.valid_start_count,"queue enqueue revokes"); do_ctx_sync(); rearm();
        // Q227: a same-mode selector call keeps the window (ITLB tables unchanged); a real
        // instruction-translation mode change still revokes it.
        mmu_change_mode(); require(q227_mode_window ? sheep_rom_window.valid_start_count!=0 : !sheep_rom_window.valid_start_count,"mode selector same-mode policy"); rearm();
        { const auto saved_msr=ppc_state.msr; ppc_state.msr^=MSR::IR; mmu_change_mode();
          require(!sheep_rom_window.valid_start_count,"mode selector revokes on ITLB mode change");
          ppc_state.msr=saved_msr; mmu_change_mode(); rearm(); }
        tlb_flush_all_pat(); require(!sheep_rom_window.valid_start_count,"TLBIA path revokes"); rearm();
        mmu_pat_ctx_changed(); require(!sheep_rom_window.valid_start_count,"PAT schedule revokes"); do_ctx_sync(); rearm();
        set_reg("SPR528",raw^0x20000u);
        require(!sheep_rom_window.valid_start_count&&!sheep_mmu_rom_window_cold_refresh(base,host),"numeric debugger write revokes and raw mismatch refuses");
        set_reg("SPR528",raw); rearm();
        set_reg("IBAT0U",raw^0x20000u);
        require(!sheep_rom_window.valid_start_count&&!sheep_mmu_rom_window_cold_refresh(base,host),"named debugger write revokes and raw mismatch refuses");
        set_reg("IBAT0U",raw); rearm();
        ppc_msr_did_change(ppc_state.msr,ppc_state.msr^MSR::IR,false);
        require(!sheep_rom_window.valid_start_count,"actual MSR change revokes"); ppc_state.msr^=MSR::IR; rearm();
        alignas(8) uint8_t backing[4096]={};
        constexpr uint32_t temporary=0xe0000000u;
        if(!mem_ctrl_instance->is_range_free(temporary,4096)) throw std::runtime_error("ROM window fixture private region occupied");
        struct RegionGuard {
            MemCtrlBase* controller; AddressMapEntry* entry=nullptr;
            ~RegionGuard(){if(entry)delete controller->remove_region(entry);}
        } region_guard{mem_ctrl_instance};
        auto* added=mem_ctrl_instance->add_ram_region(temporary,4096,backing);
        region_guard.entry=added;
        require(added&&!sheep_rom_window.valid_start_count,"map insertion revokes"); rearm();
        auto* removed=mem_ctrl_instance->remove_region(added);
        if(removed==added) region_guard.entry=nullptr;
        require(removed==added&&!sheep_rom_window.valid_start_count,"map removal revokes"); delete removed; rearm();
        auto byte=rom->mem_ptr[0]; mem_ctrl_instance->set_data(rom->start,&byte,1);
        require(!sheep_rom_window.valid_start_count,"backing write revokes"); rearm();
        sheep_mmu_rom_window_enable(false);
        require(!sheep_rom_window.valid_start_count&&!sheep_mmu_rom_window_cold_refresh(base,host),"default-off and disable revoke");
        sheep_mmu_rom_window_test_end();
        if(passed!=75) throw std::runtime_error("ROM window fixture census changed"); // Q227: +1 real-mode-change case
        fprintf(stderr,"ROM WINDOW SELFTEST %u canonical BAT/range/provenance/writer cases passed\n",passed);
        return 1;
    } catch(const std::exception& e) {
        fprintf(stderr,"%s\n",e.what()); sheep_mmu_rom_window_test_end(); return 0;
    }
}
