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
 *   2026-11-15 - Q219 baseline (Q48 to Q219, before version control): JIT dispatch, CPU-owner thread, realtime guest clock.
 *   2026-11-15 - Q226: read exec-entry environment settings once.
 *   2026-11-16 - Q234: optional authentic-speed mode (CPU governor, CD-ROM timing model).
 *   2026-11-16 - Review fixes: monotonic guest clock (steady_clock); governor batches never exceed the ungoverned size.
 *   2026-11-16 - F12: endian change invalidates the ROM window only when the mode changes.
 * Full history: PIPPIN.md and Git history.
 */

#include <core/timermanager.h>
#include <loguru.hpp>
#include "ppcemu.h"
#include "ppcmmu.h"
#include "ppcdisasm.h"
#include "sheep_jit_bridge.h"
#include "pippin_fastmem.h"
#include "pippin_authentic.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <setjmp.h>
#include <stdexcept>
#include <stdio.h>
#include <string>

#ifdef __APPLE__
#include <ctime>
#endif

using namespace std;
using namespace dppc_interpreter;

MemCtrlBase* mem_ctrl_instance = 0;

bool is_601 = false;
bool include_601 = false;
PPCPowMode ppc_pow_mode = PPCPowMode::None;
uint32_t ppc_pow_hid0_mask = 0;

// 603/7xx HID0 power-saving mode bits. They are selected in HID0,
// then entered by setting MSR[POW] with mtmsr.
static constexpr uint32_t HID0_DOZE  = 0x00800000;
static constexpr uint32_t HID0_NAP   = 0x00400000;
static constexpr uint32_t HID0_SLEEP = 0x00200000;
static constexpr uint32_t HID0_POWER_SAVE_MASK = HID0_DOZE | HID0_NAP | HID0_SLEEP;

bool is_deterministic = false;

bool power_on = false;
Po_Cause power_off_reason = po_enter_debugger;

const char * get_po_name(Po_Cause reason) {
#define onename(x) case x: return #x;
    switch (reason) {
    onename(po_none)
    onename(po_starting_up)
    onename(po_quit)
    onename(po_quitting)
    onename(po_shut_down)
    onename(po_shutting_down)
    onename(po_restart)
    onename(po_restarting)
    onename(po_disassemble_on)
    onename(po_disassemble_off)
    onename(po_enter_debugger)
    onename(po_entered_debugger)
    onename(po_signal_interrupt)
    onename(po_benchmark_exception)
    onename(po_endian_switch)
    default: return "unknown";
    }
#undef onename
}

void p_set_power_off_reason(const char* file, int line, Po_Cause new_reason) {
    if (new_reason != power_off_reason) {
        power_off_reason = new_reason;
#if 1
        LOG_F(INFO, "power_off_reason changed to %s at \"%s\":%d", get_po_name(new_reason), file, line);
#endif
    }
}

void p_power_off(const char* file, int line, Po_Cause new_reason) {
    power_on = false;
    p_set_power_off_reason(file, line, new_reason);
}

SetPRS ppc_state;
#ifdef LOG_INSTRUCTIONS
uint32_t pcp;
#define ATPCP , &pcp
#define INCPC(amount) do { pc_real += (amount); pcp += (amount); } while(0)
#else
#define ATPCP
#define INCPC(amount) pc_real += (amount)
#endif

uint32_t ppc_next_instruction_address;    // Used for branching, setting up the NIA

unsigned exec_flags; // execution control flags
// FIXME: exec_timer is read by main thread ppc_main_opcode;
// written by audio dbdma DMAChannel::update_irq .. add_immediate_timer
volatile bool exec_timer;
bool int_pin = false; // interrupt request pin state: true - asserted
bool dec_exception_pending = false;
bool thrm_exception_pending = false;

/* variables related to virtual time */
bool g_realtime = false;
uint64_t g_nanoseconds_base;
uint64_t g_icycles;
int      icnt_factor;

/* global variables related to the timebase facility */
uint64_t tbr_wr_timestamp;  // stores vCPU virtual time of the last TBR write
uint64_t rtc_timestamp;     // stores vCPU virtual time of the last RTC write
uint64_t tbr_wr_value;      // last value written to the TBR
uint32_t tbr_freq_ghz;      // TBR/RTC driving frequency in GHz expressed as a
                            // 32 bit fraction less than 1.0 (999.999999 MHz maximum).
uint32_t tbr_freq_shift;    // If 32 bits is not sufficient, then include a shift.
uint64_t tbr_period_ns;     // TBR/RTC period in ns expressed as a 64 bit value
                            // with 32 fractional bits (<1 Hz minimum).
uint64_t dec_wr_timestamp;  // stores vCPU virtual time of the last DEC write
uint32_t dec_wr_value;      // last value written to the DEC register
uint32_t rtc_lo;            // MPC601 RTC lower, counts nanoseconds
uint32_t rtc_hi;            // MPC601 RTC upper, counts seconds

#ifdef CPU_PROFILING

/* global variables for lightweight CPU profiling */
uint64_t num_executed_instrs;
uint64_t num_supervisor_instrs;
uint64_t num_int_loads;
uint64_t num_int_stores;
uint64_t exceptions_processed;
#ifdef CPU_PROFILING_OPS
std::unordered_map<uint32_t, uint64_t> num_opcodes;
#endif

#include "utils/profiler.h"
#include <memory>

class CPUProfile : public BaseProfile {
public:
    CPUProfile() : BaseProfile("PPC_CPU") {}

    void populate_variables(std::vector<ProfileVar>& vars) {
        vars.clear();

        vars.push_back({.name = "Executed Instructions Total",
                        .format = ProfileVarFmt::DEC,
                        .value = num_executed_instrs});

        vars.push_back({.name = "Executed Supervisor Instructions",
                        .format = ProfileVarFmt::DEC,
                        .value = num_supervisor_instrs});

        vars.push_back({.name = "Integer Load Instructions",
                        .format = ProfileVarFmt::DEC,
                        .value = num_int_loads});

        vars.push_back({.name = "Integer Store Instructions",
                        .format = ProfileVarFmt::DEC,
                        .value = num_int_stores});

        vars.push_back({.name = "Exceptions processed",
                        .format = ProfileVarFmt::DEC,
                        .value = exceptions_processed});

        // Generate top N op counts with readable names.
#ifdef CPU_PROFILING_OPS
        PPCDisasmContext ctx;
        ctx.instr_addr = 0;
        ctx.simplified = false;
        std::vector<std::pair<std::string, uint64_t>> op_name_counts;
        for (const auto& pair : num_opcodes) {
            ctx.instr_code = pair.first;
            auto op_name = disassemble_single(&ctx);
            op_name_counts.emplace_back(op_name, pair.second);
        }
        size_t top_ops_size = std::min(op_name_counts.size(), size_t(20));
        std::partial_sort(
            op_name_counts.begin(), op_name_counts.begin() + top_ops_size, op_name_counts.end(),
            [](const auto& a, const auto& b) {
                return b.second < a.second;
            }
        );
        op_name_counts.resize(top_ops_size);
        for (const auto& pair : op_name_counts) {
            vars.push_back({.name = "Instruction " + pair.first,
                            .format = ProfileVarFmt::COUNT,
                            .value = pair.second,
                            .count_total = num_executed_instrs});
        }
#endif
    }

    void reset() {
        num_executed_instrs = 0;
        num_supervisor_instrs = 0;
        num_int_loads = 0;
        num_int_stores = 0;
        exceptions_processed = 0;
#ifdef CPU_PROFILING_OPS
        num_opcodes.clear();
#endif
    }
};

#endif

#ifdef LOG_INSTRUCTIONS
InstructionRec InstructionLog[InstructionLogSize] = {0};
uint64_t InstructionNumber = 0;
#endif

/** Opcode lookup table, indexed by
    primary opcode (bits 0...5) and modifier (bits 21...31). */
static PPCOpcode OpcodeGrabber[64 * 2048];

/** Alternate lookup table when floating point instructions are disabled.
    Floating point instructions are mapped to ppc_fpu_off,
    everything else is the same.*/
static PPCOpcode OpcodeGrabberNoFPU[64 * 2048];

// Private JIT fallback dispatch: operand bits need not index a 1 MiB table.
// Only primary groups proven identical for every discarded bit are compacted.
alignas(64) static PPCOpcode SheepCompactHandlers[2][64 * 4];
static uint64_t sheep_compact_primary_mask = 0;
static PPCOpcode *sheep_compact_decoder(PPCOpcode *decoder) {
    if (decoder == OpcodeGrabber) return SheepCompactHandlers[0];
    if (decoder == OpcodeGrabberNoFPU) return SheepCompactHandlers[1];
    return nullptr;
}
static void sheep_build_compact_decoders() {
    sheep_compact_primary_mask = 0;
    PPCOpcode *tables[] = {OpcodeGrabber, OpcodeGrabberNoFPU};
    for (unsigned primary = 0; primary < 64; ++primary) {
        bool identical = true;
        for (unsigned mode = 0; mode < 2; ++mode) {
            auto *table = tables[mode];
            for (unsigned modifier = 0; modifier < 4; ++modifier)
                SheepCompactHandlers[mode][primary * 4 + modifier] = table[primary * 2048 + modifier];
            for (unsigned modifier = 4; modifier < 2048; ++modifier)
                identical &= table[primary * 2048 + modifier] == table[primary * 2048 + (modifier & 3)];
        }
        if (identical) sheep_compact_primary_mask |= uint64_t(1) << primary;
    }
}
void sheep_compact_dispatch_selftest() {
    PPCOpcode *tables[] = {OpcodeGrabber, OpcodeGrabberNoFPU};
    unsigned checked = 0;
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned primary = 0; primary < 64; ++primary)
            for (unsigned modifier = 0; modifier < 2048; ++modifier) {
                auto actual = (sheep_compact_primary_mask & (uint64_t(1) << primary))
                    ? SheepCompactHandlers[mode][primary * 4 + (modifier & 3)]
                    : tables[mode][primary * 2048 + modifier];
                if (actual != tables[mode][primary * 2048 + modifier])
                    throw std::runtime_error("Compact decoder differs from canonical opcode table");
                ++checked;
            }
    fprintf(stderr, "COMPACT DISPATCH SELFTEST %u FPU/disabled handler identities passed; mask=%016llx\n",
            checked, (unsigned long long)sheep_compact_primary_mask);
}


void ppc_msr_did_change(uint32_t old_msr_val, uint32_t new_msr_val, bool set_next_instruction_address) {
    if ((old_msr_val ^ new_msr_val) & (MSR::IR | MSR::PR | MSR::LE))
        sheep_mmu_rom_window_invalidate();
    ppc_state.msr = new_msr_val;
    if ((old_msr_val ^ new_msr_val) & MSR::FP) {
        bool newFP = (new_msr_val & MSR::FP) != 0;
        ppc_opcode_grabber = newFP ? OpcodeGrabber : OpcodeGrabberNoFPU;
        //LOG_F(INFO, "changed FP to %s", newFP ? "yes" : "no");
#if 1
        exec_flags |= EXEF_OPC_DECODER;
        if (set_next_instruction_address) {
            // Even though we're setting an exception flag, we want normal
            // instruction execution to continue.
            ppc_next_instruction_address = ppc_state.pc + 4;
        }
#else
        power_off(po_endian_switch);
#endif
    }
}

void ppc_change_endian(bool newLE) {
    // F12: exceptions call this with the current mode; only a real change invalidates the ROM window.
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
    if (ppc_state.is_LE != newLE)
        sheep_mmu_rom_window_invalidate();
#else
    if (newLE)   // big-endian only: the mode never changes, and LE is rejected below
        sheep_mmu_rom_window_invalidate();
#endif
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
    if (ppc_state.is_LE != newLE) {
        LOG_F(INFO, "changed endian to %s", newLE ? "LE" : "BE");
        ppc_state.is_LE = newLE;
        power_off(po_endian_switch);
    }
#else
    if (newLE) {
        LOG_F(ERROR, "unsupported endian %s", newLE ? "LE" : "BE");
        power_off(po_enter_debugger);
    }
#endif
}

PPCOpcode* ppc_opcode_grabber = OpcodeGrabberNoFPU;

/** Exception helpers. */

void ppc_illegalop(uint32_t opcode) {
    // Zero values are used as sentinels in the TNT ROM and are thus somewhat
    // common, don't log them.
    // Example: ciexit in Old World Open Firmware uses opcode 0.
    if (opcode) {
        PPCDisasmContext ctx;
        ctx.instr_code = opcode;
        ctx.instr_addr = 0;
        ctx.simplified = false;
        auto op_name = disassemble_single(&ctx);
        LOG_F(WARNING, "Illegal opcode exception raised for 0x%08x (decoded as %s). The PC was 0x%08x.", opcode, op_name.c_str(), ppc_state.pc);
    }

    ppc_exception_handler(Except_Type::EXC_PROGRAM, Exc_Cause::ILLEGAL_OP);
}

void ppc_fpu_off(uint32_t opcode) {
    ppc_exception_handler(Except_Type::EXC_NO_FPU, Exc_Cause::FPU_OFF);
}

void ppc_assert_int() {
    int_pin = true;
    if (ppc_state.msr & MSR::EE) {
        LOG_F(5, "CPU ExtIntHandler called");
        ppc_exception_handler(Except_Type::EXC_EXT_INT, 0);
    } else {
        LOG_F(5, "CPU IRQ ignored!");
    }
}

void ppc_release_int() {
    int_pin = false;
}

/** Opcode decoding functions. */

/* Dispatch using primary and modifier opcode */
void ppc_main_opcode(PPCOpcode *opcodeGrabber, uint32_t opcode)
{
#ifdef CPU_PROFILING
    num_executed_instrs++;
#if defined(CPU_PROFILING_OPS)
    num_opcodes[opcode]++;
#endif
#endif

#ifdef LOG_INSTRUCTIONS
    if (InstructionNumber && !InstructionLog[(InstructionNumber - 1) & (InstructionLogSize - 1)].flags_after) {
        // This happens for all exceptions except EXC_EXT_INT && EXC_DECR
        //LOG_F(ERROR, "previous instruction did not complete");
    }
    InstructionRec * irec = &InstructionLog[InstructionNumber & (InstructionLogSize - 1)];
    irec->cycle = InstructionNumber++;
    irec->addr = ppc_state.pc;
    irec->paddr = pcp;
    irec->ins = opcode;
    irec->msr = ppc_state.msr;
    irec->flags_before = exec_flags | (exec_timer << 7);
    irec->flags_after = 0;
#endif

    opcodeGrabber[(opcode >> 15 & 0x1F800) | (opcode & 0x7FF)](opcode);

#ifdef LOG_INSTRUCTIONS
    irec->flags_after = exec_flags | (exec_timer << 7) | 0x80000000;
    irec->msr_after = ppc_state.msr;
#endif
}

bool sheep_encode_rom_handler(uint32_t opcode,uint32_t &description){
 unsigned index=(opcode>>15&0x1f800)|(opcode&0x7ff);
 auto enabled=OpcodeGrabber[index],disabled=OpcodeGrabberNoFPU[index];
 bool floating=enabled!=disabled;
 if(floating&&disabled!=ppc_fpu_off)return false;
 uintptr_t base=reinterpret_cast<uintptr_t>(&ppc_illegalop),target=reinterpret_cast<uintptr_t>(enabled);
 if((base|target)&3)return false;
 int64_t words=(int64_t(target)-int64_t(base))/4;
 if(words<-(int64_t(1)<<28)||words>=(int64_t(1)<<28))return false;
 description=((uint32_t(words)&0x1fffffffu)<<2)|(floating?0x80000000u:0u);
 sheep_rom_disabled_fpu_handler=ppc_fpu_off;
 return true;
}
bool sheep_rom_handler_matches(uint32_t opcode,uint64_t word){
 unsigned index=(opcode>>15&0x1f800)|(opcode&0x7ff);
 return sheep_rom_cached_handler(word,false)==OpcodeGrabber[index]&&sheep_rom_cached_handler(word,true)==OpcodeGrabberNoFPU[index];
}

static long long cpu_now_ns() {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    // Guest time must be monotonic: libstdc++'s high_resolution_clock is the wall clock,
    // which NTP can step (a Pi has no RTC and steps its clock once the network syncs).
    static_assert(std::chrono::steady_clock::is_steady, "guest clock must be monotonic");
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

uint64_t get_virt_time_ns()
{
    if (g_realtime) {
        return cpu_now_ns() - g_nanoseconds_base;
    } else {
        return g_icycles << icnt_factor;
    }
}

void set_virt_time_ns(uint64_t time_now)
{
    if (g_realtime) {
        g_nanoseconds_base = cpu_now_ns() - time_now - 5000;
    } else {
        g_icycles = time_now >> icnt_factor;
    }
    uint64_t time_new = get_virt_time_ns();
    if (g_realtime && time_new > time_now) {
        g_nanoseconds_base += 2 * (time_new - time_now);
        time_new = get_virt_time_ns();
    }
    LOG_F(INFO, "time before: %lld  after: %lld  change: %lld", time_now, time_new, time_new - time_now);
}

static constexpr uint64_t TIMER_POLL_INTERVAL_NS = 500'000;

static uint64_t process_events()
{
    exec_timer = false;
    // Q234: optional CPU governor (off by default: one predictable branch per batch).
    if (pippin_gov_enabled && g_realtime) [[unlikely]]
        pippin_gov_tick(g_icycles, (exec_flags & EXEF_SLEEP) != 0);
    uint64_t next_ns = TimerManager::get_instance()->process_timers();
    pippin_fastmem_poll();
    if (g_realtime) {
        uint64_t time_now = get_virt_time_ns();
        uint64_t delay_ns;
        if (next_ns) {
            delay_ns = next_ns > time_now ? next_ns - time_now : 0;
        } else {
            delay_ns = TIMER_POLL_INTERVAL_NS;
        }
        // Host time and the instruction counter advance independently. Schedule
        // a relative instruction batch, capped so long deadlines still poll.
        // Comparing an absolute wall-time deadline with g_icycles can otherwise
        // make a fast interpreter process timers after every instruction.
        const uint64_t slice_ns = std::min(delay_ns, TIMER_POLL_INTERVAL_NS);
        // Q234: a governed CPU retires fewer instructions per guest ns; size the batch so the
        // next timer deadline is reached on time instead of 62.5/max_mips times late.
        // A cap above the ungoverned rate must not enlarge the batch: timer callbacks would wait longer.
        if (pippin_gov_enabled) [[unlikely]]
            return g_icycles + std::min<uint64_t>(pippin_gov_insns_for_ns(slice_ns), slice_ns >> icnt_factor) + 1;
        return g_icycles + (slice_ns >> icnt_factor) + 1;
    }
    return (
        (
            next_ns ?
                next_ns
            :
                get_virt_time_ns() + TIMER_POLL_INTERVAL_NS
        ) >> icnt_factor
    ) + 1;
}

static void force_cycle_counter_reload()
{
    // tell the interpreter loop to reload cycle counter
    exec_timer = true;
}

int increment_icnt_factor()
{
    uint64_t time_now = get_virt_time_ns();
    icnt_factor += 1;
    set_virt_time_ns(time_now);
    force_cycle_counter_reload();
    return icnt_factor;
}

int decrement_icnt_factor()
{
    if (icnt_factor > 0) {
        uint64_t time_now = get_virt_time_ns();
        icnt_factor -= 1;
        set_virt_time_ns(time_now);
        force_cycle_counter_reload();
    }
    return icnt_factor;
}

int get_icnt_factor()
{
    return icnt_factor;
}

bool toggle_g_realtime()
{
    uint64_t time_now = get_virt_time_ns();
    g_realtime = !g_realtime;
    set_virt_time_ns(time_now);
    force_cycle_counter_reload();
    return g_realtime;
}

typedef enum {
    main,
    until,
    debug,
} ppc_exec_type_t;

// inner interpreter loop
// Branches that miss the JIT still dominated interpreted CPU samples. Keep
// canonical PowerPC semantics but bypass the table lookup and indirect call.
[[gnu::always_inline]] static inline bool sheep_execute_simple_branch(uint32_t opcode){
 unsigned primary=opcode>>26;
 if(primary==18){
  int32_t offset=int32_t((opcode&~3u)<<6)>>6;
  ppc_next_instruction_address=(opcode&2)?uint32_t(offset):uint32_t(ppc_state.pc+offset);
  if(opcode&1)ppc_state.spr[SPR::LR]=ppc_state.pc+4;
  exec_flags=EXEF_BRANCH;return true;
 }
 if(primary==16){
  unsigned bo=(opcode>>21)&31,bi=(opcode>>16)&31;
  if(!(bo&4))--ppc_state.spr[SPR::CTR];
  bool ctr_ok=(bo&4)||((ppc_state.spr[SPR::CTR]!=0)==!(bo&2));
  bool cnd_ok=(bo&16)||(!(ppc_state.cr&(0x80000000u>>bi))==!(bo&8));
  if(ctr_ok&&cnd_ok){
   int32_t offset=int16_t(opcode&~3u);
   ppc_next_instruction_address=(opcode&2)?uint32_t(offset):uint32_t(ppc_state.pc+offset);
   exec_flags=EXEF_BRANCH;
  }
  if(opcode&1)ppc_state.spr[SPR::LR]=ppc_state.pc+4;
  return true;
 }
 return false;
}

void sheep_branch_dispatch_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags;const auto next=ppc_next_instruction_address;
 unsigned passed=0;
 // Both canonical decoders must agree for every opcode modifier before using
 // the primary-opcode specialization on RAM or decoded immutable ROM.
 PPCOpcode *tables[]={OpcodeGrabber,OpcodeGrabberNoFPU};
 PPCOpcode b[]={ppc_b<LK0,AA0>,ppc_b<LK1,AA0>,ppc_b<LK0,AA1>,ppc_b<LK1,AA1>};
 PPCOpcode bc[]={ppc_bc<LK0,AA0>,ppc_bc<LK1,AA0>,ppc_bc<LK0,AA1>,ppc_bc<LK1,AA1>};
 for(auto table:tables)for(unsigned modifier=0;modifier<2048;modifier++){
  if(table[18*2048+modifier]!=b[modifier&3]||table[16*2048+modifier]!=bc[modifier&3])
   throw std::runtime_error("Simple branch decoder specialization is not canonical");
 }
 auto compare=[&](uint32_t op){
  const auto initial=ppc_state;exec_flags=EXEF_OPC_DECODER;ppc_next_instruction_address=0x31415920;
  if(!sheep_execute_simple_branch(op))throw std::runtime_error("Simple branch rejected fixture");
  const auto result=ppc_state;const auto actual_flags=exec_flags,actual_next=ppc_next_instruction_address;
  ppc_state=initial;exec_flags=EXEF_OPC_DECODER;ppc_next_instruction_address=0x31415920;
  ppc_main_opcode(OpcodeGrabber,op);
  if(std::memcmp(&ppc_state,&result,sizeof(result))||actual_flags!=exec_flags||actual_next!=ppc_next_instruction_address)
   throw std::runtime_error("Simple branch state/target/flags differs from canonical interpreter");
  passed++;
 };
 for(uint32_t pc:{0u,0xfffffffcu})for(unsigned aa_lk=0;aa_lk<4;aa_lk++){
  for(uint32_t offset:{0u,4u,0x01fffffcu,0x02000000u,0x03fffffcu}){
   ppc_state=saved;ppc_state.pc=pc;compare((18u<<26)|offset|aa_lk);
  }
  for(unsigned bo=0;bo<32;bo++)for(unsigned bi=0;bi<32;bi++)for(unsigned bit=0;bit<2;bit++)
   for(uint32_t ctr:{0u,1u,2u,0xffffffffu})for(uint32_t offset:{0u,0x7ffcu,0xfffcu}){
    ppc_state=saved;ppc_state.pc=pc;ppc_state.cr=bit?(0x80000000u>>bi):0;
    ppc_state.spr[SPR::CTR]=ctr;compare((16u<<26)|(bo<<21)|(bi<<16)|offset|aa_lk);
   }
 }
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;
 fprintf(stderr,"SIMPLE BRANCH SELFTEST %u canonical CPU/CR/CTR/LR/AA/LK/wrap/target/flags cases passed; 8192 decoder identities checked\n",passed);
}

#include <cstddef>
// X29 is the actual global instruction-clock address in every private loop
// context. Nearby scheduler fields use assembler-resolved displacements rather
// than loading their pointers from context on every instruction. Keep every
// original volatile timer, power, flags and deadline check in its old place.
// Separate immutable-source planning from the current execution deadline.
// The caller has already translated this source page; never inspect a future
// page or create/fill a mapping while deciding how much ROM may be decoded.
unsigned sheep_rom_plan_bound(const uint8_t *code,uint32_t pc,uint32_t page_end){
 const uintptr_t host=reinterpret_cast<uintptr_t>(code);
 const uintptr_t begin=sheep_rom_host_begin,end=sheep_rom_host_end;
 if(!sheep_rom_only||!begin||end<=begin||host<begin||host>=end||((host|pc)&3))return 0;
 if(page_end<pc||((page_end^pc)&0xfffff000u))return 0;
 const uint64_t page_words=(uint64_t(page_end)-pc+1)/4;
 const uintptr_t rom_words=(end-host)/4;
 return unsigned(std::min<uint64_t>(sheep_native_max_ops,std::min<uint64_t>(page_words,rom_words)));
}
void sheep_rom_plan_bound_selftest(){
 const uintptr_t saved_begin=sheep_rom_host_begin,saved_end=sheep_rom_host_end;const bool saved_only=sheep_rom_only;
 unsigned passed=0;
 // Pure integer-address oracle: no guest or artificial source bytes are read.
 for(unsigned mode=0;mode<4;mode++)for(uintptr_t offset:{uintptr_t(0),uintptr_t(1),uintptr_t(4),uintptr_t(124),uintptr_t(4092),uintptr_t(4096),uintptr_t(8188),uintptr_t(8192)})
 for(uint32_t pc:{0u,1u,4u,0xff0u,0xffcu,0x1000u,0xfffffff0u,0xfffffffcu})for(unsigned tail=0;tail<4;tail++){
  sheep_rom_only=mode!=0;sheep_rom_host_begin=0x10000000u;
  sheep_rom_host_end=mode==1?sheep_rom_host_begin:mode==2?sheep_rom_host_begin+8190:sheep_rom_host_begin+8192;
  const uintptr_t host=sheep_rom_host_begin+offset;
  const uint32_t page_end=tail==0?(pc|4095u):tail==1?pc:tail==2?pc+3u:(pc^4096u)|4095u;
  unsigned expected=0;
  if(sheep_rom_only&&sheep_rom_host_end>sheep_rom_host_begin&&host>=sheep_rom_host_begin&&host<sheep_rom_host_end&&!(host&3)&&!(pc&3)&&page_end>=pc&&(page_end>>12)==(pc>>12)){
   for(unsigned i=0;i<32;i++){
    const uint64_t last_guest=uint64_t(pc)+uint64_t(i)*4+3;
    const uint64_t last_host=uint64_t(host)+uint64_t(i)*4+3;
    if(last_guest>page_end||last_host>=sheep_rom_host_end)break;
    expected++;
   }
  }
  if(sheep_rom_plan_bound(reinterpret_cast<const uint8_t*>(host),pc,page_end)!=expected)throw std::runtime_error("ROM decode bound differs from per-word address oracle");
  passed++;
 }
 sheep_rom_host_begin=saved_begin;sheep_rom_host_end=saved_end;sheep_rom_only=saved_only;
 fprintf(stderr,"ROM PLAN BOUNDS SELFTEST %u mode/page/wrap/host/alignment/partial-word cases passed\n",passed);
}
static bool sheep_direct_poll_layout(){
 const uintptr_t clock=reinterpret_cast<uintptr_t>(&g_icycles);
 for(uintptr_t field:{reinterpret_cast<uintptr_t>(&exec_timer),reinterpret_cast<uintptr_t>(&exec_flags),reinterpret_cast<uintptr_t>(&power_on),reinterpret_cast<uintptr_t>(&ppc_next_instruction_address)}){
  const int64_t offset=int64_t(field)-int64_t(clock);if(offset < -256||offset > 255)return false;
 }
 return true;
}
// Experimental steady-state ROM loop. Every scalar instruction retains the
// canonical handler. Native blocks use the existing bounded, guarded ABI.
// Return 1 leaves exactly one final post-instruction event/accounting step for
// ppc_exec; return 0 leaves an unexecuted cold opcode for the original path.
uint64_t sheep_steady_spans=0,sheep_steady_instructions=0;
// The original fields retain their offsets; a live ROM-window pointer is
// appended to the private ABI. Legacy aggregate fixtures leave it null.
struct SheepLoopContext {
 uintptr_t cpu,words,counters,native_base,illegal,fpu_disabled,timer,clock,flags,next;
 uint64_t max_cycles;uint8_t *host;
 uint32_t page_start,page_end,pc_offset,cr_offset,lr_offset,disabled;
 uintptr_t reject,memory_bail,power;uint32_t retired,native_used;uintptr_t itlb,mask,rom_begin,rom_end,opcodes;uint32_t source_ram,allow_ram;
 const SheepROMWindow *window;
};
static_assert(offsetof(SheepLoopContext,max_cycles)==80&&offsetof(SheepLoopContext,reject)==120&&offsetof(SheepLoopContext,window)==200&&sizeof(SheepLoopContext)==208,"ARM64 CPU-loop ABI");
extern "C" unsigned sheep_cpu_loop_a64(SheepLoopContext*);
extern "C" void sheep_cpu_loop_reject(const uint8_t *code,uint32_t pc){sheep_reject_set(code,pc,32);}
#if defined(__aarch64__)
static_assert(offsetof(SetPRS,pc)==256,"Specialized CPU loop requires the validated SetPRS layout");
asm(R"(
.text
.p2align 4
.global sheep_cpu_loop_a64
.type sheep_cpu_loop_a64,%function
sheep_cpu_loop_a64:
.cfi_startproc
 stp x19,x20,[sp,#-96]!
.cfi_def_cfa_offset 96
.cfi_offset 19,-96
.cfi_offset 20,-88
 stp x21,x22,[sp,#16]
.cfi_offset 21,-80
.cfi_offset 22,-72
 stp x23,x24,[sp,#32]
.cfi_offset 23,-64
.cfi_offset 24,-56
 stp x25,x26,[sp,#48]
.cfi_offset 25,-48
.cfi_offset 26,-40
 stp x27,x28,[sp,#64]
.cfi_offset 27,-32
.cfi_offset 28,-24
 stp x29,x30,[sp,#80]
.cfi_offset 29,-16
.cfi_offset 30,-8
 mov x28,x0
 ldr x19,[x28]
 ldr x20,[x28,#88]
 ldr w9,[x28,#192]
 orr x20,x20,x9
 ldr x21,[x28,#8]
 ldr x23,[x28,#80]
 ldr x29,[x28,#56]
 ldr x22,[x29]
 // X22 is authoritative between observers. Publish before every external call
 // (including native fault entry), and on every return or trap. Polls below
 // consume X22 directly; canonical handlers may change memory, so their reload
 // remains mandatory. The asynchronous owned main thread uses its atomic time
 // snapshot; raw diagnostic clock reads between observers remain non-atomic.
 ldr w24,[x19,#256]
 mov w27,#0
 // Every published window belongs to the same global CPU object. Cache only
 // that immutable identity; start/count/host offset stay live at every probe.
 ldr x9,[x28,#200]
 cbz x9,.Lsl_window_identity_done
 ldr x9,[x9,#16]
 cmp x19,x9
 cset x27,eq
 lsl x27,x27,#32
.Lsl_window_identity_done:
 // Only CPU callbacks can change instruction flags. Start the leaf fast loop
 // with no pending action; the normal C++ path handles any pre-existing flag.
 .word 0xb84003a9 | (((exec_flags-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lsl_cold
.Lsl_fetch:
 tbnz x20,#0,.Lsl_fetch_ram
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 // Decode only the descriptor tag bits; preserve opcode, metadata and index.
 tbz w25,#1,.Lsl_low_tag
 tbnz w25,#0,.Lsl_native
 // Match the canonical retry counter, including its final blocked iteration.
 ldr x11,[x28,#16]
 ldrb w9,[x11,x10]
 cbz w9,.Lsl_retry_zero
 cmp w9,#255
 b.eq .Lsl_scalar
 sub w9,w9,#1
 strb w9,[x11,x10]
 cbnz w9,.Lsl_scalar
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lsl_scalar
.Lsl_fetch_ram:
 // Writable code is fetched anew for every instruction. No native RAM cache.
 ldur w0,[x20,#-1]
 rev w0,w0
 lsl x25,x0,#32
 b .Lsl_scalar
.Lsl_retry_zero:
 and x9,x25,#0xfffffffffffffffc
 str x9,[x21,x10,lsl #3]
 b .Lsl_cold
.Lsl_native:
 lsr x9,x25,#32
 cmp w9,w24
 b.ne .Lsl_cold
 ubfx w26,w25,#2,#5
 add w26,w26,#1
 sub x10,x23,x22
 cmp x26,x10
 b.hi .Lsl_cold
 lsr w9,w25,#7
 ldr x10,[x28,#24]
 add x9,x10,w9,uxtw #2
 ldr x10,[x28,#128]
 strb wzr,[x10]
 mov w10,#1
 str w10,[x28,#148]
 mov x0,x19
 str x22,[x29]
 blr x9
 cmp w0,w26
 b.hi .Lsl_trap
 mov w26,w0
 ldr w25,[x19,#256]
 cmp w26,#4
 mov w14,#1024
 b.hs .Lsl_native_fast
 // Successful short branches need no rejection or interpreter round-trip.
 // Zero/failed-guard prefixes retain their original slow-path backoff.
 cbz w26,.Lsl_native_short_guard
 ldr x9,[x28,#128]
 ldrb w9,[x9]
 mov w14,#128
 cbz w9,.Lsl_native_fast
.Lsl_native_short_guard:
 mov x0,x20
 mov w1,w24
 ldr x9,[x28,#120]
 str x22,[x29]
 blr x9
 cbnz w26,.Lsl_native_prefix
 str w24,[x19,#256]
 ubfx x10,x20,#2,#20
 ldr x25,[x21,x10,lsl #3]
 b .Lsl_scalar
.Lsl_native_fast:
 // All indices address the private, allocated decoder table, even when a
 // guest branch will later miss instruction translation. Prefetch has no
 // architectural side effects; MMU guards still control the actual target.
 sub w9,w25,w24
 add x12,x20,w9,sxtw
 ubfx x9,x12,#2,#20
 prfm pldl1keep,[x21,x9,lsl #3]
 // Qualified native blocks contain no calls and cannot change exec_flags.
 // Keep their same-page target local; synthesize the original last-PC/branch
 // post-step state only when handing control back to the C++ event loop.
 sub w9,w26,#1
 add x20,x20,w9,uxtw #2
 add w24,w24,w9,lsl #2
 add x22,x22,x9
 // W24 is the last executed source PC in this mapped page. No context
 // reload is needed to compare target and source page identities.
.global sheep_page_guard_native
.hidden sheep_page_guard_native
sheep_page_guard_native:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lsl_native_fast_old
 // Preserve the canonical last branch target even when consuming the branch
 // locally. It remains observable at later event and exception boundaries.
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lsl_native_fast_sequential
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
.Lsl_native_fast_sequential:
 add x27,x27,x26
 cmp x22,x23
 b.hs .Lsl_native_fast_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lsl_native_fast_pending
 cmp w27,w14
 b.hs .Lsl_native_fast_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lsl_native_fast_pending
 // No device/event polling is removed: deadlines, external timer and power
 // are checked after every bounded block, as in the original path.
 mov x20,x12
 mov w24,w25
 add x22,x22,#1
 // The native block already stored this exact target into CPU.pc.
 b .Lsl_fetch
.Lsl_native_fast_old:
 // Cross-page native exits retain the exact128-instruction span cap. The
 // helper-free leaf cannot change exec_flags; avoid publishing a synthetic
 // BRANCH only to reload/clear it after the same polls and live mapper.
 // NIA remains observable even when the translated target is consumed here.
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 add x27,x27,x26
 cmp x22,x23
 b.hs .Lsl_native_map_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lsl_native_map_pending
 cmp w27,#128
 b.hs .Lsl_native_map_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lsl_native_map_pending
 // A live worker-owned ROM window proves the complete target word. Reload
 // validity on every edge; all MMU/source writers revoke it before mutation.
 tbz x27,#32,.Lsl_native_window_miss
 ldr x9,[x28,#200]
 ldp w11,w12,[x9]
 sub w11,w25,w11
 cmp w11,w12
 b.hs .Lsl_native_window_miss
 ldr x9,[x9,#8]
 add x20,x9,w25,uxtw
 and w12,w25,#0xfffff000
 str wzr,[x28,#192]
 b .Lsl_native_mapped
.Lsl_native_window_miss:
 // Read the current primary table and mask on every cross-page edge. Keep
 // the old tag/PAGE_MEM/host-offset/ROM-or-RAM checks, with no mapping cache.
 adrp x9,pCurITLB1
 ldr x9,[x9,:lo12:pCurITLB1]
 adrp x11,tlb_size_mask
 ldr w11,[x11,:lo12:tlb_size_mask]
 lsr w12,w25,#12
 and w11,w11,w12
 add x9,x9,x11,lsl #5
 ldp x11,x9,[x9]
 and w12,w25,#0xfffff000
 cmp w11,w12
 b.ne .Lsl_native_map_pending
 tbz x11,#32,.Lsl_native_map_pending
 add x9,x9,w25,uxtw
 ldr x11,[x28,#168]
 cmp x9,x11
 b.lo .Lsl_native_map_ram
 ldr x11,[x28,#176]
 cmp x9,x11
 b.hs .Lsl_native_map_ram
 mov x20,x9
 str wzr,[x28,#192]
.Lsl_native_mapped:
 mov w24,w25
 str w12,[x28,#96]
 add w11,w12,#4095
 str w11,[x28,#100]
 // CPU.pc already contains this target. Preserve the old post-step clock
 // and mapped-source update without rolling CPU.pc back and forward.
 add x22,x22,#1
 b .Lsl_fetch
.Lsl_native_map_ram:
 ldr w11,[x28,#196]
 cbz w11,.Lsl_native_map_pending
 tst x9,#3
 b.ne .Lsl_native_map_pending
 orr x20,x9,#1
 mov w11,#1
 str w11,[x28,#192]
 b .Lsl_native_mapped
.Lsl_native_map_pending:
 // Every unsuccessful handoff exposes the old last-source/BRANCH state,
 // including a sequential cross-page target or32-bit guest-PC wrap.
 str w24,[x19,#256]
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lsl_pending
.Lsl_native_fast_pending:
 str w24,[x19,#256]
 add w9,w24,#4
 cmp w25,w9
 b.eq .Lsl_pending
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lsl_pending
.Lsl_native_prefix:
 sub w9,w26,#1
 add x20,x20,w9,uxtw #2
 add w24,w24,w9,lsl #2
 add x22,x22,x9
 str w24,[x19,#256]
.Lsl_native_prefix_done:
 add w9,w24,#4
 cmp w25,w9
 b.ne .Lsl_native_branch
 // Target is already proven to be source+4. Only target bits2..11==0 cross a
 // page, including the 32-bit address wrap from fffffffc to zero.
.global sheep_page_guard_next
.hidden sheep_page_guard_next
sheep_page_guard_next:
 tst w25,#0xffc
 b.ne .Lsl_after
.Lsl_native_branch:
 // All indices address the private, allocated decoder table, even when a
 // guest branch will later miss instruction translation. Prefetch has no
 // architectural side effects; MMU guards still control the actual target.
 sub w9,w25,w24
 add x9,x20,w9,sxtw
 ubfx x9,x9,#2,#20
 prfm pldl1keep,[x21,x9,lsl #3]
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lsl_after
.Lsl_scalar_branch:
 // Relative B/BL and the already-qualified taken BC contain no callbacks.
 // NIA is observable even for an explicit branch to source+4. Keep that
 // unconditional store, but materialize BRANCH only on a pending handoff.
 // Match the leaf native path's target-PC custody: any pending return below
 // restores the last source before C++ performs its post-instruction step.
 str w25,[x19,#256]
 sub w9,w25,w24
 add x12,x20,w9,sxtw
 ubfx x9,x12,#2,#20
 prfm pldl1keep,[x21,x9,lsl #3]
 eor w9,w25,w24
 tst w9,#0xfffff000
 // W26 is exactly1; no native prefix PC/clock adjustment is necessary.
 // The existing live mapper and every failed-cross-page boundary are reused.
 b.ne .Lsl_native_fast_old
 .word 0xb80003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 add x27,x27,x26
 cmp x22,x23
 b.hs .Lsl_scalar_branch_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lsl_scalar_branch_pending
 cmp w27,#128
 b.hs .Lsl_scalar_branch_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lsl_scalar_branch_pending
 mov x20,x12
 mov w24,w25
 add x22,x22,#1
 b .Lsl_fetch
.Lsl_scalar_branch_pending:
 str w24,[x19,#256]
 mov w10,#1
 .word 0xb80003aa | (((exec_flags-g_icycles)&0x1ff)<<12)
 b .Lsl_pending
.Lsl_low_tag:
 // Scalar tag1 falls through; native/retry routes keep their old branches.
 tbz w25,#0,.Lsl_cold
.Lsl_scalar:
 mov w26,#1
 lsr x0,x25,#32
 lsr w9,w0,#26
 cmp w9,#18
 b.eq .Lsl_b
 cmp w9,#16
 b.eq .Lsl_bc
.Lsl_handler:
 tbnz x20,#0,.Lsl_ram_handler
 // The packed descriptor stores a signed handler offset and FPU flag.
 ldr w9,[x28,#116]
 cbz w9,.Lsl_normal_handler
 tbz w25,#31,.Lsl_normal_handler
 ldr x9,[x28,#40]
 b .Lsl_call_handler
.Lsl_normal_handler:
 lsl w9,w25,#1
 asr w9,w9,#3
 ldr x10,[x28,#32]
 add x9,x10,w9,sxtw #2
 b .Lsl_call_handler
.Lsl_ram_handler:
 // Exactly the canonical ppc_main_opcode index and selected FPU table.
 lsr w9,w0,#15
 and w9,w9,#0x1f800
 and w10,w0,#0x7ff
 orr w9,w9,w10
 ldr x10,[x28,#184]
 ldr x9,[x10,w9,uxtw #3]
 b .Lsl_call_handler
.Lsl_call_handler:
 str x22,[x29]
 blr x9
 ldr x22,[x29]
 b .Lsl_after
.Lsl_b:
 tbnz w0,#1,.Lsl_handler
 tbz w0,#0,.Lsl_b_target
 add w9,w24,#4
 ldr w10,[x28,#112]
 str w9,[x19,x10]
.Lsl_b_target:
 sbfx w9,w0,#0,#26
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lsl_scalar_branch
.Lsl_bc:
 tst w0,#3
 b.ne .Lsl_handler
 ubfx w9,w0,#21,#5
 and w10,w9,#30
 cmp w10,#4
 b.eq .Lsl_bc_cr
 cmp w10,#12
 b.ne .Lsl_handler
.Lsl_bc_cr:
 ldr w10,[x28,#108]
 ldr w10,[x19,x10]
 ubfx w11,w0,#16,#5
 mov w12,#31
 sub w11,w12,w11
 lsr w10,w10,w11
 and w10,w10,#1
 ubfx w9,w9,#3,#1
 cmp w9,w10
 b.ne .Lsl_after
 sxth w9,w0
 and w9,w9,#0xfffffffc
 add w25,w24,w9
 b .Lsl_scalar_branch
.Lsl_after:
 // Keep the final pre-increment value for ppc_exec's existing event check.
 add x27,x27,x26
 cmp x22,x23
 b.hs .Lsl_pending
 .word 0x384003a9 | (((exec_timer-g_icycles)&0x1ff)<<12)
 cbnz w9,.Lsl_pending
 cmp w27,#128
 b.hs .Lsl_pending
 .word 0x384003a9 | (((power_on-g_icycles)&0x1ff)<<12)
 cbz w9,.Lsl_pending
 .word 0xb84003a9 | (((exec_flags-g_icycles)&0x1ff)<<12)
 cbz w9,.Lsl_advance
 cmp w9,#1
 b.ne .Lsl_pending
 ldr x10,[x28,#64]
 .word 0xb84003b9 | (((ppc_next_instruction_address-g_icycles)&0x1ff)<<12)
 // Scalar callbacks cannot change the loop's mapped source W24. Keep
 // arbitrary targets subject to the exact existing primary-ITLB mapper.
.global sheep_page_guard_branch
.hidden sheep_page_guard_branch
sheep_page_guard_branch:
 eor w9,w25,w24
 tst w9,#0xfffff000
 b.ne .Lsl_map_target
 // Only a plain same-page branch can be consumed inside the fast loop.
 sub w9,w25,w24
 sxtw x9,w9
 add x20,x20,x9
 mov w24,w25
 str wzr,[x10]
 b .Lsl_continue
.Lsl_advance:
.global sheep_page_guard_advance
.hidden sheep_page_guard_advance
sheep_page_guard_advance:
 add w10,w24,#4
 // Source+4 leaves its page iff target bits2..11 are zero, even unaligned.
 tst w10,#0xffc
 b.ne .Lsl_advance_same
 mov w25,w10
 ldr x10,[x28,#64]
 b .Lsl_map_target
.Lsl_advance_same:
 add w24,w24,#4
 add x20,x20,#4
.Lsl_continue:
 add x22,x22,#1
 str w24,[x19,#256]
 b .Lsl_fetch
.Lsl_map_target:
 // X10 remains the original flags pointer through this no-call probe.
 tbz x27,#32,.Lsl_window_miss
 ldr x9,[x28,#200]
 ldp w11,w12,[x9]
 sub w11,w25,w11
 cmp w11,w12
 b.hs .Lsl_window_miss
 ldr x9,[x9,#8]
 add x20,x9,w25,uxtw
 and w12,w25,#0xfffff000
 str wzr,[x28,#192]
 b .Lsl_mapped
.Lsl_window_miss:
 // A primary ITLB hit already carries the canonical translated host offset.
 // Never walk/fill an MMU entry speculatively. Non-memory targets return to C++.
 // The private context always binds these canonical global variables.
 // Read their current values on every map, including after MTMSR callbacks.
 adrp x9,pCurITLB1
 ldr x9,[x9,:lo12:pCurITLB1]
 adrp x11,tlb_size_mask
 ldr w11,[x11,:lo12:tlb_size_mask]
 lsr w12,w25,#12
 and w11,w11,w12
 add x9,x9,x11,lsl #5
 // Read only canonical entry metadata early, never guest payload.
 ldp x11,x9,[x9]
 and w12,w25,#0xfffff000
 cmp w11,w12
 b.ne .Lsl_pending
 tbz x11,#32,.Lsl_pending
 add x9,x9,w25,uxtw
 ldr x11,[x28,#168]
 cmp x9,x11
 b.lo .Lsl_target_ram
 ldr x11,[x28,#176]
 cmp x9,x11
 b.hs .Lsl_target_ram
 mov x20,x9
 str wzr,[x28,#192]
 b .Lsl_mapped
.Lsl_target_ram:
 ldr w11,[x28,#196]
 cbz w11,.Lsl_pending
 tst x9,#3
 b.ne .Lsl_pending
 orr x20,x9,#1
 mov w11,#1
 str w11,[x28,#192]
.Lsl_mapped:
 mov w24,w25
 str w12,[x28,#96]
 add w11,w12,#4095
 str w11,[x28,#100]
 str wzr,[x10]
 b .Lsl_continue
.Lsl_pending:
 mov w0,#1
 b .Lsl_return
.Lsl_cold:
 mov w0,#0
.Lsl_return:
 str x22,[x29]
 str w27,[x28,#144]
 and x9,x20,#0xfffffffffffffffe
 str x9,[x28,#88]
 ldp x19,x20,[sp]
.cfi_restore 19
.cfi_restore 20
 ldp x21,x22,[sp,#16]
.cfi_restore 21
.cfi_restore 22
 ldp x23,x24,[sp,#32]
.cfi_restore 23
.cfi_restore 24
 ldp x25,x26,[sp,#48]
.cfi_restore 25
.cfi_restore 26
 ldp x27,x28,[sp,#64]
.cfi_restore 27
.cfi_restore 28
 ldp x29,x30,[sp,#80]
.cfi_restore 29
.cfi_restore 30
 add sp,sp,#96
.cfi_def_cfa_offset 0
 ret
.Lsl_trap:
 str x22,[x29]
 brk #0
.cfi_endproc
.size sheep_cpu_loop_a64,.-sheep_cpu_loop_a64
)");
#else
extern "C" unsigned sheep_cpu_loop_a64(SheepLoopContext*){return 0;}
#endif

extern bool sheep_encode_rom_handler(uint32_t,uint32_t&);
extern void sheep_mmu_jit_layout(nw_jit_layout*);
extern void sheep_mmu_test_page(uint8_t*,uint32_t,uint16_t,bool);
extern "C" uint64_t sheep_jit_abi_check(nw_jit_fn,nw_jit_cpu*);
static void loop_fixture_reject(const uint8_t *code,uint32_t){
 uint32_t op=uint32_t(code[0])<<24|uint32_t(code[1])<<16|uint32_t(code[2])<<8|code[3],desc;
 if(!sheep_encode_rom_handler(op,desc))throw std::runtime_error("Steady fixture restore failed");
 auto slot=sheep_jit_filter_slot_rom(code);sheep_rom_dispatch_words[slot]=(uint64_t(op)<<32)|desc|2;sheep_jit_reject_filter[slot]=32;
}
static void loop_fixture_event(uint32_t){exec_flags=EXEF_EXCEPTION|EXEF_BRANCH;ppc_next_instruction_address=0x10000040;exec_timer=true;}
#include "tag-dispatch.inc"
#include "flags-bfi.inc"
int sheep_clock_publication_selftest();
void sheep_cpu_loop_selftest(){
 if(!sheep_clock_publication_selftest())throw std::runtime_error("Private clock publication selftest failed");
 sheep_flags_bfi_selftest();
 if(!sheep_direct_poll_layout())throw std::runtime_error("Direct CPU polling layout exceeds signed instruction range");
 const auto saved=ppc_state;const auto saved_flags=exec_flags,saved_next=ppc_next_instruction_address;const auto saved_clock=g_icycles;const bool saved_timer=exec_timer,saved_power=power_on;
 std::array<uint8_t,4096> code{},ram{},original{},native_ram{};
 std::array<uint64_t,16> words{};std::array<unsigned char,16> counters{};unsigned passed=0;
 constexpr uint32_t root=0x10000000,va=0x28000000;
 for(unsigned pattern=0;pattern<17;pattern++)for(unsigned seed=0;seed<8;seed++)for(unsigned deadline: {0u,1u,7u,31u,127u,129u,511u,1025u}){
  const bool short_case=pattern==4||pattern==15||pattern==16;
  sheep_mmu_test_page(ram.data(),va,0x61,false);ram.fill(uint8_t(seed*13));original=ram;
  ppc_state=saved;ppc_state.pc=root;ppc_state.gpr[3]=seed;ppc_state.gpr[4]=0;ppc_state.gpr[5]=0;ppc_state.gpr[6]=va;ppc_state.cr=seed&1?0x80000000u:0;exec_flags=0;ppc_next_instruction_address=0x31415920;exec_timer=pattern==6||(short_case&&(seed&2));power_on=pattern!=7&&!(short_case&&(seed&4));g_icycles=123;
  uint32_t ops[]={0x38630001,0x7c642a78,0x2c03000b,0x4180fff4,0x54651838,0x90660000,0x80860000,0x4bffffe4,0x60000000,0x60000000,0x60000000,0x60000000,0x60000000,0x60000000,0x60000000,0x60000000};
  if(pattern==9){for(auto &op:ops)op=0x60000000;ppc_state.pc=root+4080;}
  if(pattern==10){ops[3]=0x4e800020;ppc_state.spr[SPR::LR]=root+16;}
  if(pattern==15)ops[1]=0x4180000c;
  if(pattern==16)ops[2]=0x41800008;
  if(pattern==11)ops[0]=0x48001000;
  if(pattern==12){ops[3]=0x4200fff4;ppc_state.spr[SPR::CTR]=seed+2;}
  if(pattern==13)ops[0]=0x48000011;
  if(pattern==4)ops[0]=0x41800010; // short native conditional prefix
  for(unsigned i=0;i<16;i++){
   for(unsigned j=0;j<4;j++)code[i*4+j]=uint8_t(ops[i]>>(24-j*8));
   unsigned slot=sheep_jit_filter_slot_rom(code.data()+i*4);words[i]=sheep_rom_dispatch_words[slot];counters[i]=sheep_jit_reject_filter[slot];
   uint32_t desc;if(!sheep_encode_rom_handler(ops[i],desc))throw std::runtime_error("Steady fixture decode failed");
   sheep_rom_dispatch_words[slot]=(uint64_t(ops[i])<<32)|desc|1;sheep_jit_reject_filter[slot]=255;
  }
  auto first=sheep_jit_filter_slot_rom(code.data());
  if(pattern==1){sheep_rom_dispatch_words[first]=(sheep_rom_dispatch_words[first]&~3ull)|2;sheep_jit_reject_filter[first]=(seed&4)?0:3;}
  if(pattern==3)sheep_rom_dispatch_words[first]&=~3ull; // unexecuted cold source
  nw_jit_layout layout={offsetof(SetPRS,gpr),offsetof(SetPRS,cr),offsetof(SetPRS,spr)+SPR::XER*4,offsetof(SetPRS,spr)+SPR::LR*4,offsetof(SetPRS,spr)+SPR::CTR*4,offsetof(SetPRS,pc),reinterpret_cast<void*>(1)};
  sheep_mmu_jit_layout(&layout);layout.leaf=1;layout.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);
  if(pattern==2||pattern==4||pattern==8||pattern==15||pattern==16){
   auto fn=nw_jit_compile_mapped(ops,8,root,&layout);if(!fn)throw std::runtime_error("Steady native fixture compile failed");
   auto displacement=reinterpret_cast<uintptr_t>(fn)-reinterpret_cast<uintptr_t>(nw_jit_native_base());
   sheep_rom_dispatch_words[first]=(uint64_t(root+(pattern==8?4:0))<<32)|(uint32_t((displacement>>2)<<5|7)<<2)|3;
  }
  if(pattern==5){
   auto target=reinterpret_cast<uintptr_t>(&loop_fixture_event),base=reinterpret_cast<uintptr_t>(&ppc_illegalop);
   if((target-base)&3)throw std::runtime_error("Steady event fixture handler alignment");
   unsigned slot=sheep_jit_filter_slot_rom(code.data()+16);uint32_t desc=uint32_t(target-base)&0x7ffffffc;
   sheep_rom_dispatch_words[slot]=(uint64_t(ops[4])<<32)|desc|1;
  }
  SheepLoopContext ctx{reinterpret_cast<uintptr_t>(&ppc_state),reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words),reinterpret_cast<uintptr_t>(sheep_jit_reject_filter),reinterpret_cast<uintptr_t>(nw_jit_native_base()),reinterpret_cast<uintptr_t>(&ppc_illegalop),reinterpret_cast<uintptr_t>(sheep_rom_disabled_fpu_handler),reinterpret_cast<uintptr_t>(&exec_timer),reinterpret_cast<uintptr_t>(&g_icycles),reinterpret_cast<uintptr_t>(&exec_flags),reinterpret_cast<uintptr_t>(&ppc_next_instruction_address),123+deadline,code.data(),root,root+4095,offsetof(SetPRS,pc),offsetof(SetPRS,cr),offsetof(SetPRS,spr)+SPR::LR*4,ppc_opcode_grabber==OpcodeGrabberNoFPU,reinterpret_cast<uintptr_t>(&loop_fixture_reject),reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail),reinterpret_cast<uintptr_t>(&power_on),0,0,reinterpret_cast<uintptr_t>(&pCurITLB1),reinterpret_cast<uintptr_t>(&tlb_size_mask),reinterpret_cast<uintptr_t>(code.data()),reinterpret_cast<uintptr_t>(code.data()+code.size())};
  if(pattern==14)exec_flags=1u<<(seed%6);
  const auto initial=ppc_state;auto result=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(&sheep_cpu_loop_a64),reinterpret_cast<nw_jit_cpu*>(&ctx));
  if(result>>32)throw std::runtime_error("Steady CPU-loop ARM64 ABI violation");
  if((pattern==4||pattern==15||pattern==16)&&ctx.native_used&&(sheep_rom_dispatch_words[first]&3)!=3)throw std::runtime_error("ASM successful short branch entered rejection backoff");
  const bool pending=uint32_t(result)!=0;const auto native=ppc_state;const auto native_flags=exec_flags,native_next=ppc_next_instruction_address;const auto native_clock=g_icycles;const auto native_timer=exec_timer;native_ram=ram;
  if(ctx.retired>1056||(!ctx.retired&&pending)||ctx.retired>deadline+1)throw std::runtime_error("Steady loop retired/event budget differs");
  ppc_state=initial;ram=original;exec_flags=pattern==14?(1u<<(seed%6)):0;ppc_next_instruction_address=0x31415920;exec_timer=pattern==6||(short_case&&(seed&2));g_icycles=123;
  uint8_t *host=code.data();
  for(unsigned i=0;i<ctx.retired;i++){
   unsigned index=(ppc_state.pc-initial.pc)/4;if(index>=16)throw std::runtime_error("Steady replay left fixture page");
   if(pattern==5&&index==4)loop_fixture_event(ops[index]);else ppc_main_opcode(ppc_opcode_grabber,ops[index]);
   if(i+1==ctx.retired&&pending)break;
   uint32_t before=ppc_state.pc;
   if(exec_flags&EXEF_BRANCH){ppc_state.pc=ppc_next_instruction_address;exec_flags=0;}else ppc_state.pc+=4;
   host+=int64_t(ppc_state.pc)-before;g_icycles++;
  }
  if(std::memcmp(&native,&ppc_state,sizeof(native))||native_ram!=ram||native_flags!=exec_flags||native_next!=ppc_next_instruction_address||native_clock!=g_icycles||native_timer!=exec_timer||ctx.host!=host){
   fprintf(stderr,"STEADY failure pattern=%u seed=%u deadline=%u retired=%u pending=%u pc=%08x/%08x cycles=%llu/%llu flags=%x/%x host=%td/%td\n",pattern,seed,deadline,ctx.retired,pending,native.pc,ppc_state.pc,(unsigned long long)native_clock,(unsigned long long)g_icycles,native_flags,exec_flags,ctx.host-code.data(),host-code.data());
   throw std::runtime_error("Steady CPU-loop CPU/RAM/clock/flags/source-PC differs");
  }
  for(unsigned i=0;i<16;i++){unsigned slot=sheep_jit_filter_slot_rom(code.data()+i*4);sheep_rom_dispatch_words[slot]=words[i];sheep_jit_reject_filter[slot]=counters[i];}
  sheep_mmu_test_page(nullptr,0,0,false);passed++;
 }
 ppc_state=saved;exec_flags=saved_flags;ppc_next_instruction_address=saved_next;g_icycles=saved_clock;exec_timer=saved_timer;power_on=saved_power;
 fprintf(stderr,"STEADY LOOP SELFTEST %u CPU/RAM/ABI/native/branch/cold/retry/event/clock/source cases passed\n",passed);
}

static void loop_fixture_rfi(uint32_t){exec_flags=EXEF_RFI|EXEF_BRANCH;ppc_next_instruction_address=0x10001000;}
void sheep_cpu_loop_cross_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags,next=ppc_next_instruction_address;const auto clock=g_icycles;const bool timer=exec_timer,power=power_on;const auto table=pCurITLB1;const auto mask=tlb_size_mask;
 std::array<uint8_t,12288> code{};std::array<uint8_t,4096> other_ram{};alignas(32) std::array<uint8_t,256> itlb{};
 std::array<uint64_t,3072> old_words{};std::array<uint8_t,3072> old_counters{};unsigned passed=0;
 for(unsigned i=0;i<3072;i++){auto slot=sheep_jit_filter_slot_rom(code.data()+i*4);old_words[i]=sheep_rom_dispatch_words[slot];old_counters[i]=sheep_jit_reject_filter[slot];}
 auto put=[&](unsigned offset,uint32_t op,uintptr_t handler=0){
  for(unsigned j=0;j<4;j++)code[offset+j]=uint8_t(op>>(24-j*8));uint32_t desc;
  if(handler)desc=uint32_t(handler-reinterpret_cast<uintptr_t>(&ppc_illegalop))&0x7ffffffc;
  else if(!sheep_encode_rom_handler(op,desc))throw std::runtime_error("Cross-page fixture decode failed");
  auto slot=sheep_jit_filter_slot_rom(code.data()+offset);sheep_rom_dispatch_words[slot]=(uint64_t(op)<<32)|desc|1;sheep_jit_reject_filter[slot]=255;
 };
 for(unsigned pattern=0;pattern<9;pattern++)for(unsigned seed=0;seed<4;seed++)for(unsigned deadline:{0u,1u,7u,31u,127u,129u,511u,1025u}){
  for(unsigned i=0;i<3072;i++)put(i*4,0x60000000);
  uint32_t root=pattern==7?0xfffff000u:0x10000000u,second=root+4096;
  for(unsigned i=0;i<8;i++){uint32_t invalid=0xffffffffu;std::memcpy(itlb.data()+i*32,&invalid,4);}
  auto map=[&](uint32_t page,uint8_t *host){auto *entry=itlb.data()+((page>>12)&7)*32;int64_t offset=int64_t(reinterpret_cast<uintptr_t>(host))-page;uint16_t memory=1;std::memcpy(entry,&page,4);std::memcpy(entry+4,&memory,2);std::memcpy(entry+8,&offset,8);};
  auto *target=code.data()+(pattern==1?8192:4096);
  map(root,code.data());map(second,target);
  if(pattern==2||pattern==6){uint32_t invalid=second^4096;std::memcpy(itlb.data()+((second>>12)&7)*32,&invalid,4);}
  if(pattern==3)map(second,reinterpret_cast<uint8_t*>(uintptr_t(0x56780000)));
  if(pattern==8)map(second,other_ram.data());
  pCurITLB1=reinterpret_cast<TLBEntry*>(itlb.data());tlb_size_mask=7;
  ppc_state=saved;ppc_state.pc=root;exec_flags=0;ppc_next_instruction_address=0x31415920;exec_timer=false;power_on=true;g_icycles=123+seed*100;
  if(pattern==5||pattern==6||pattern==7)ppc_state.pc=root+4080;
  else if(pattern==4)put(0,0x60000000,reinterpret_cast<uintptr_t>(&loop_fixture_rfi));
  else{put(0,0x48001000);put(pattern==1?8192:4096,0x4bfff000);}
  auto *initial_host=sheep_itlb_primary_hit(ppc_state.pc);if(!initial_host)throw std::runtime_error("Cross-page fixture root mapping missing");
  SheepLoopContext ctx{reinterpret_cast<uintptr_t>(&ppc_state),reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words),reinterpret_cast<uintptr_t>(sheep_jit_reject_filter),reinterpret_cast<uintptr_t>(nw_jit_native_base()),reinterpret_cast<uintptr_t>(&ppc_illegalop),reinterpret_cast<uintptr_t>(sheep_rom_disabled_fpu_handler),reinterpret_cast<uintptr_t>(&exec_timer),reinterpret_cast<uintptr_t>(&g_icycles),reinterpret_cast<uintptr_t>(&exec_flags),reinterpret_cast<uintptr_t>(&ppc_next_instruction_address),g_icycles+deadline,initial_host,root,root+4095,offsetof(SetPRS,pc),offsetof(SetPRS,cr),offsetof(SetPRS,spr)+SPR::LR*4,ppc_opcode_grabber==OpcodeGrabberNoFPU,reinterpret_cast<uintptr_t>(&loop_fixture_reject),reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail),reinterpret_cast<uintptr_t>(&power_on),0,0,reinterpret_cast<uintptr_t>(&pCurITLB1),reinterpret_cast<uintptr_t>(&tlb_size_mask),reinterpret_cast<uintptr_t>(code.data()),reinterpret_cast<uintptr_t>(code.data()+code.size())};
  const auto initial=ppc_state;const auto initial_clock=g_icycles;
  auto result=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(&sheep_cpu_loop_a64),reinterpret_cast<nw_jit_cpu*>(&ctx));
  if(result>>32||uint32_t(result)!=1||!ctx.retired||ctx.retired>128||ctx.retired>deadline+1)throw std::runtime_error("Cross-page loop ABI/count/deadline differs");
  const auto native=ppc_state;const auto native_flags=exec_flags,native_next=ppc_next_instruction_address;const auto native_clock=g_icycles;
  ppc_state=initial;exec_flags=0;ppc_next_instruction_address=0x31415920;g_icycles=initial_clock;
  for(unsigned i=0;i<ctx.retired;i++){
   auto *host=sheep_itlb_primary_hit(ppc_state.pc);if(!host||host<code.data()||host>=code.data()+code.size())throw std::runtime_error("Cross-page replay left readonly fixture");
   uint32_t op=uint32_t(host[0])<<24|uint32_t(host[1])<<16|uint32_t(host[2])<<8|host[3];
   if(pattern==4&&ppc_state.pc==root)loop_fixture_rfi(op);else ppc_main_opcode(ppc_opcode_grabber,op);
   if(i+1==ctx.retired)break;
   ppc_state.pc=exec_flags&EXEF_BRANCH?ppc_next_instruction_address:ppc_state.pc+4;exec_flags=0;g_icycles++;
  }
  auto *last_host=sheep_itlb_primary_hit(ppc_state.pc);
  if(std::memcmp(&native,&ppc_state,sizeof(native))||native_flags!=exec_flags||native_next!=ppc_next_instruction_address||native_clock!=g_icycles||ctx.host!=last_host||ctx.page_start!=(ppc_state.pc&~4095u)||ctx.page_end!=uint32_t(ctx.page_start+4095)){
   fprintf(stderr,"CROSS LOOP pattern=%u seed=%u deadline=%u retired=%u pc=%08x/%08x cycles=%llu/%llu flags=%x/%x page=%08x host=%td/%td\n",pattern,seed,deadline,ctx.retired,native.pc,ppc_state.pc,(unsigned long long)native_clock,(unsigned long long)g_icycles,native_flags,exec_flags,ctx.page_start,ctx.host-code.data(),last_host-code.data());
   throw std::runtime_error("Cross-page loop CPU/clock/flags/mapping/alias/source differs");
  }
  if((pattern==2||pattern==3||pattern==4||pattern==8)&&ctx.retired!=1)throw std::runtime_error("Cross-page miss/RFI/non-ROM guard consumed extra code");
  if(pattern==6&&ctx.retired!=std::min(4u,deadline+1))throw std::runtime_error("Sequential page miss consumed extra code");
  passed++;
 }
 for(unsigned i=0;i<3072;i++){auto slot=sheep_jit_filter_slot_rom(code.data()+i*4);sheep_rom_dispatch_words[slot]=old_words[i];sheep_jit_reject_filter[slot]=old_counters[i];}
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;g_icycles=clock;exec_timer=timer;power_on=power;pCurITLB1=table;tlb_size_mask=mask;
 fprintf(stderr,"CROSS PAGE LOOP SELFTEST %u branch/sequential/alias/wrap/miss/RFI/non-ROM/ABI/clock cases passed\n",passed);
}

void sheep_cpu_loop_ram_selftest(){
 const auto saved=ppc_state;const auto flags=exec_flags,next=ppc_next_instruction_address;const auto clock=g_icycles;const bool timer=exec_timer,power=power_on;const auto table=pCurITLB1;const auto mask=tlb_size_mask;
 alignas(16) std::array<uint8_t,8192> code{},initial_code{},native_code{};std::array<uint8_t,4096> ram{},initial_ram{},native_ram{};alignas(32) std::array<uint8_t,256> itlb{};
 constexpr uint32_t root=0x10000000,va=0x28000000;
 const auto a=sheep_jit_filter_slot_rom(code.data()),b=sheep_jit_filter_slot_rom(code.data()+4096);const auto aw=sheep_rom_dispatch_words[a],bw=sheep_rom_dispatch_words[b];const auto ac=sheep_jit_reject_filter[a],bc=sheep_jit_reject_filter[b];unsigned passed=0;
 auto put=[&](unsigned offset,uint32_t op){for(unsigned j=0;j<4;j++)code[offset+j]=uint8_t(op>>(24-j*8));};
 for(unsigned pattern=0;pattern<9;pattern++)for(unsigned seed=0;seed<8;seed++)for(unsigned deadline:{0u,1u,7u,31u,127u,129u,511u,1025u}){
  for(unsigned i=0;i<8192;i+=4)put(i,0x60000000);
  for(unsigned i=0;i<4096;i++)ram[i]=uint8_t(i*7+seed*13);
  for(unsigned i=0;i<8;i++){uint32_t invalid=0xffffffffu;std::memcpy(itlb.data()+i*32,&invalid,4);}
  auto map=[&](uint32_t page,uint8_t *host,uint16_t flags=1){auto *entry=itlb.data()+((page>>12)&7)*32;int64_t offset=int64_t(reinterpret_cast<uintptr_t>(host))-page;std::memcpy(entry,&page,4);std::memcpy(entry+4,&flags,2);std::memcpy(entry+8,&offset,8);};
  map(root,code.data());map(root+4096,code.data()+4096);
  bool smc=pattern==1||pattern==6||pattern==8;
  sheep_mmu_test_page(smc?code.data():ram.data(),smc?(pattern==6?root+4096:root):va,0x61,false);
  pCurITLB1=reinterpret_cast<TLBEntry*>(itlb.data());tlb_size_mask=7;
  ppc_state=saved;ppc_state.pc=pattern==3?root+4096:root;ppc_state.gpr[3]=seed;ppc_state.gpr[4]=0;ppc_state.gpr[5]=0;ppc_state.gpr[6]=smc?(pattern==6?root+4096:root):va;ppc_state.gpr[7]=pattern==8?0x48000008u:0x38630009u;ppc_state.cr=0;exec_flags=0;ppc_next_instruction_address=0x31415920;exec_timer=false;power_on=true;g_icycles=123;
  sheep_rom_dispatch_words[a]=aw;sheep_rom_dispatch_words[b]=bw;
  if(pattern==2||pattern==3||pattern==4||pattern==5){
   put(0,0x48001000);put(4096,0x4bfff000);uint32_t desc;if(!sheep_encode_rom_handler(0x4bfff000,desc))throw std::runtime_error("Live RAM fixture decoder unavailable");sheep_rom_dispatch_words[b]=(uint64_t(0x4bfff000)<<32)|desc|1;
  }else if(smc){
   put(0,0x90e60014);put(4,0x38630001); // STW r7,20(r6), then ADDI r3,1
   if(pattern==8){put(24,0x38630007);put(28,0x38630009);} // injected branch skips ADDI #7
  }else{
   uint32_t ops[]={0x38630001,0x7c642a78,0x2c03000b,0x4180fff4,0x54651838,0x90660000,0x80860000,0x4bffffe4};for(unsigned i=0;i<8;i++)put(i*4,ops[i]);
  }
  if(pattern==4)map(root+4096,reinterpret_cast<uint8_t*>(uintptr_t(0x56780000)),2); // I/O, never raw fetched
  if(pattern==5){uint32_t invalid=0xffffffffu;std::memcpy(itlb.data()+32,&invalid,4);}
  if(pattern==7)sheep_rom_dispatch_words[a]=0xffffffff00000003ull; // RAM must ignore ROM/native metadata
  initial_code=code;initial_ram=ram;
  auto *initial_host=sheep_itlb_primary_hit(ppc_state.pc);
  SheepLoopContext ctx{reinterpret_cast<uintptr_t>(&ppc_state),reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words),reinterpret_cast<uintptr_t>(sheep_jit_reject_filter),reinterpret_cast<uintptr_t>(nw_jit_native_base()),reinterpret_cast<uintptr_t>(&ppc_illegalop),reinterpret_cast<uintptr_t>(sheep_rom_disabled_fpu_handler),reinterpret_cast<uintptr_t>(&exec_timer),reinterpret_cast<uintptr_t>(&g_icycles),reinterpret_cast<uintptr_t>(&exec_flags),reinterpret_cast<uintptr_t>(&ppc_next_instruction_address),123+deadline,initial_host,ppc_state.pc&~4095u,uint32_t((ppc_state.pc&~4095u)+4095),offsetof(SetPRS,pc),offsetof(SetPRS,cr),offsetof(SetPRS,spr)+SPR::LR*4,ppc_opcode_grabber==OpcodeGrabberNoFPU,reinterpret_cast<uintptr_t>(&loop_fixture_reject),reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail),reinterpret_cast<uintptr_t>(&power_on),0,0,reinterpret_cast<uintptr_t>(&pCurITLB1),reinterpret_cast<uintptr_t>(&tlb_size_mask),reinterpret_cast<uintptr_t>(code.data()+4096),reinterpret_cast<uintptr_t>(code.data()+8192),reinterpret_cast<uintptr_t>(ppc_opcode_grabber),pattern==3?0u:1u,1};
  const auto initial=ppc_state;auto result=sheep_jit_abi_check(reinterpret_cast<nw_jit_fn>(&sheep_cpu_loop_a64),reinterpret_cast<nw_jit_cpu*>(&ctx));
  if(result>>32||uint32_t(result)!=1||!ctx.retired||ctx.retired>128||ctx.retired>deadline+1)throw std::runtime_error("Live RAM loop ABI/event/count differs");
  const auto native=ppc_state;const auto native_flags=exec_flags,native_next=ppc_next_instruction_address;const auto native_clock=g_icycles;native_code=code;native_ram=ram;
  code=initial_code;ram=initial_ram;ppc_state=initial;exec_flags=0;ppc_next_instruction_address=0x31415920;g_icycles=123;
  for(unsigned i=0;i<ctx.retired;i++){
   auto *host=sheep_itlb_primary_hit(ppc_state.pc);if(!host||host<code.data()||host+4>code.data()+code.size())throw std::runtime_error("Live RAM replay mapping unavailable");
   uint32_t op=uint32_t(host[0])<<24|uint32_t(host[1])<<16|uint32_t(host[2])<<8|host[3];ppc_main_opcode(ppc_opcode_grabber,op);
   if(i+1==ctx.retired)break;
   ppc_state.pc=exec_flags&EXEF_BRANCH?ppc_next_instruction_address:ppc_state.pc+4;exec_flags=0;g_icycles++;
  }
  auto *last_host=sheep_itlb_primary_hit(ppc_state.pc);bool last_ram=last_host<code.data()+4096||last_host>=code.data()+8192;
  if(std::memcmp(&native,&ppc_state,sizeof(native))||code!=native_code||ram!=native_ram||native_flags!=exec_flags||native_next!=ppc_next_instruction_address||native_clock!=g_icycles||ctx.host!=last_host||ctx.page_start!=(ppc_state.pc&~4095u)||bool(ctx.source_ram)!=last_ram){
   fprintf(stderr,"LIVE RAM pattern=%u seed=%u deadline=%u retired=%u pc=%08x/%08x cycles=%llu/%llu flags=%x/%x mode=%u/%u host=%td/%td\n",pattern,seed,deadline,ctx.retired,native.pc,ppc_state.pc,(unsigned long long)native_clock,(unsigned long long)g_icycles,native_flags,exec_flags,ctx.source_ram,last_ram,ctx.host-code.data(),last_host-code.data());
   throw std::runtime_error("Live RAM loop CPU/code/SMC/data/clock/flags/mapping differs");
  }
  if((pattern==4||pattern==5)&&ctx.retired!=1)throw std::runtime_error("Live RAM I/O/miss guard consumed extra code");
  if((pattern==1||pattern==6)&&ctx.retired>=6&&ppc_state.gpr[3]!=seed+10)throw std::runtime_error("Live RAM used stale self-modified arithmetic");
  sheep_mmu_test_page(nullptr,0,0,false);passed++;
 }
 sheep_rom_dispatch_words[a]=aw;sheep_rom_dispatch_words[b]=bw;sheep_jit_reject_filter[a]=ac;sheep_jit_reject_filter[b]=bc;
 ppc_state=saved;exec_flags=flags;ppc_next_instruction_address=next;g_icycles=clock;exec_timer=timer;power_on=power;pCurITLB1=table;tlb_size_mask=mask;
 fprintf(stderr,"LIVE RAM LOOP SELFTEST %u CPU/RAM/SMC/alias/ROM-switch/I-O/miss/ABI/event/clock cases passed\n",passed);
}

#include "page-guards.inc"
#include "live-itlb.inc"

template <ppc_exec_type_t exec_type, endian_switch endian, bool rom_profile = false, unsigned fixture_path = 0>
static void ppc_exec_inner(uint32_t start_addr, uint32_t size)
{
    // Private fixture instantiations avoid touching live TimerManager queues.
    static_assert(fixture_path <= 3, "Invalid private dispatcher fixture path");
    uint64_t max_cycles = fixture_path ? ~uint64_t(0) : 0;
    uint32_t page_start, eb_start, eb_end = 0;
    uint32_t opcode;
    PPCOpcode* opcode_grabber = ppc_opcode_grabber;
    const bool sheep_enabled = sheep_jit_enabled();
    const SheepROMWindow *rom_window = nullptr;
    if constexpr (exec_type == main && endian == big_end && rom_profile && (fixture_path == 0 || fixture_path == 3)) {
        // Q226: the environment is fixed after startup; read it once, not on every exception re-entry.
        static const bool window_env_on = [] { const char *window_setting = std::getenv("SHEEP_PIPPIN_ROM_WINDOW"); return window_setting && !std::strcmp(window_setting,"on"); }();
        if (sheep_enabled && (fixture_path == 3 || window_env_on))
            rom_window = sheep_mmu_rom_window_get();
    }
    PPCOpcode *compact_decoder = sheep_compact_decoder(opcode_grabber);
    uint64_t compact_mask = compact_decoder ? sheep_compact_primary_mask : 0;
    const bool decode_rom = rom_profile && sheep_rom_decode_ready;
    uint8_t* pc_real;
    bool sheep_page_enabled = false,steady_page_enabled=false;
    auto validate_window_registration = [&]() {
        if(!rom_window || !rom_window->valid_start_count)return;
        const uintptr_t first=uintptr_t(rom_window->guest_start)+rom_window->host_offset;
        const uintptr_t bytes=uintptr_t(rom_window->valid_start_count)+3u;
        if(first<sheep_rom_host_begin || first>=sheep_rom_host_end || bytes>sheep_rom_host_end-first)
            sheep_mmu_rom_window_invalidate();
    };
    // The MMU proves the current actual ROM; the dispatcher additionally binds
    // its complete span to the registered decoded-ROM allocation, only cold.
    validate_window_registration();
    auto refresh_sheep_window = [&](uint32_t pc) {
        if(sheep_mmu_rom_window_cold_refresh(pc,pc_real))validate_window_registration();
    };
    auto update_sheep_page = [&](uint32_t pc, bool allow_window_refresh = true) {
        if constexpr (exec_type == main && endian == big_end)
        {
            sheep_page_enabled = sheep_enabled && sheep_jit_code_page_enabled(pc_real, pc);
            steady_page_enabled=sheep_page_enabled;
            if(!steady_page_enabled&&sheep_itlb_primary_enabled&&sheep_itlb_primary_hit(pc)==pc_real){
                const auto *entry=reinterpret_cast<const uint8_t*>(pCurITLB1)+size_t((pc>>12)&tlb_size_mask)*32;
                uint16_t flags;std::memcpy(&flags,entry+4,2);steady_page_enabled=(flags&1)!=0;
            }
            if(allow_window_refresh && rom_window && !rom_window->valid_start_count && sheep_page_enabled && !exec_flags)
                refresh_sheep_window(pc);
        }
    };

    auto translate_instruction = [&](uint32_t pc, uint32_t *physical=nullptr) {
        if constexpr (rom_profile) return sheep_translate_imem(pc, physical);
        return mmu_translate_imem(pc, physical);
    };

    SheepLoopContext steady{};
    steady.cpu=reinterpret_cast<uintptr_t>(&ppc_state);
    steady.words=reinterpret_cast<uintptr_t>(sheep_rom_dispatch_words);
    steady.counters=reinterpret_cast<uintptr_t>(sheep_jit_reject_filter);
    steady.illegal=reinterpret_cast<uintptr_t>(&ppc_illegalop);
    steady.fpu_disabled=reinterpret_cast<uintptr_t>(sheep_rom_disabled_fpu_handler);
    steady.timer=reinterpret_cast<uintptr_t>(&exec_timer);
    steady.clock=reinterpret_cast<uintptr_t>(&g_icycles);
    steady.flags=reinterpret_cast<uintptr_t>(&exec_flags);
    steady.next=reinterpret_cast<uintptr_t>(&ppc_next_instruction_address);
    steady.pc_offset=offsetof(SetPRS,pc);steady.cr_offset=offsetof(SetPRS,cr);
    steady.lr_offset=offsetof(SetPRS,spr)+SPR::LR*4;
    steady.reject=reinterpret_cast<uintptr_t>(&sheep_cpu_loop_reject);
    steady.memory_bail=reinterpret_cast<uintptr_t>(&sheep_jit_memory_bail);
    steady.power=reinterpret_cast<uintptr_t>(&power_on);
    steady.itlb=reinterpret_cast<uintptr_t>(&pCurITLB1);steady.mask=reinterpret_cast<uintptr_t>(&tlb_size_mask);
    steady.rom_begin=sheep_rom_host_begin;steady.rom_end=sheep_rom_host_end;steady.window=rom_window;
    static const bool steady_env_on=[]{const char *steady_setting=std::getenv("SHEEP_PIPPIN_CPU_LOOP");return !steady_setting||std::strcmp(steady_setting,"off");}();
    static const bool direct_poll_ok=sheep_direct_poll_layout();
    const bool steady_enabled=fixture_path ? (fixture_path==2 || fixture_path==3) : (steady_env_on&&direct_poll_ok);
    static const bool ram_env_on=[]{const char *ram_setting=std::getenv("SHEEP_PIPPIN_CPU_RAM");return !ram_setting||std::strcmp(ram_setting,"off");}();
    steady.allow_ram=ram_env_on;
    while (power_on) {
        if (exec_type == debug)
            if (ppc_state.pc >= start_addr && ppc_state.pc < start_addr + size)
                break;

        if (ppc_state.pc >= eb_end) {
            // define boundaries of the next execution block
            // max execution block length = one memory page
            eb_start   = ppc_state.pc;
            page_start = eb_start & PPC_PAGE_MASK;
            eb_end     = page_start + PPC_PAGE_SIZE - 1;
            exec_flags = 0;
            pc_real    = translate_instruction(eb_start ATPCP); // &pcp
            update_sheep_page(eb_start);
        }

        bool steady_step=false;
#if defined(__aarch64__) && !defined(LOG_INSTRUCTIONS) && !defined(CPU_PROFILING)
        if constexpr(exec_type==main && endian==big_end && rom_profile){
            if(steady_enabled && steady_page_enabled && (sheep_page_enabled||steady.allow_ram) && !(reinterpret_cast<uintptr_t>(pc_real)&3) && sheep_rom_decode_ready && sheep_itlb_primary_enabled && sheep_rom_direct_enabled && !exec_timer && g_icycles<max_cycles){
                steady.native_base=sheep_rom_direct_base;steady.max_cycles=max_cycles;
                steady.host=pc_real;steady.page_start=page_start;steady.page_end=eb_end;
                steady.disabled=opcode_grabber==OpcodeGrabberNoFPU;
                steady.opcodes=reinterpret_cast<uintptr_t>(opcode_grabber);
                steady.source_ram=!sheep_jit_code_page_enabled(pc_real,ppc_state.pc);
                steady_step=sheep_cpu_loop_a64(&steady)!=0;
                if(steady.native_used)sheep_hot_used=true;
                sheep_steady_spans++;sheep_steady_instructions+=steady.retired;
                pc_real=steady.host;page_start=steady.page_start;eb_end=steady.page_end;update_sheep_page(ppc_state.pc,!steady_step);
            }
        }
#endif
        if(!steady_step){
        // A window hit may reach cold code without populating its primary ITLB.
        // This is an unexecuted opcode: canonical fetch precedes classification.
        // Pending returns reach the original event/flag handling first instead.
        if(rom_window && !exec_flags && rom_window->valid_start_count &&
           uint32_t(ppc_state.pc-rom_window->guest_start)<rom_window->valid_start_count &&
           uintptr_t(pc_real)==uintptr_t(ppc_state.pc)+rom_window->host_offset &&
           sheep_itlb_primary_hit(ppc_state.pc)!=pc_real){
            pc_real=translate_instruction(ppc_state.pc ATPCP);
            page_start=ppc_state.pc&PPC_PAGE_MASK;eb_end=page_start+PPC_PAGE_SIZE-1;
            update_sheep_page(ppc_state.pc);
        }
        unsigned jit_count = 0;
        bool decoded_instruction = false;
        uint64_t decoded_word = 0;
        if constexpr (exec_type == main && endian == big_end) {
            if (sheep_page_enabled) {
                bool blocked;bool direct_handled=false;
                if constexpr (rom_profile) {
                    unsigned slot = sheep_jit_filter_slot_rom(pc_real);
                    uint64_t word = sheep_rom_dispatch_words[slot];
                    if((word&3)==3&&!exec_timer&&g_icycles<max_cycles)
                        direct_handled=sheep_jit_rom_direct(slot,word,max_cycles-g_icycles,jit_count);
                    blocked=(word&3)==3?false:sheep_rom_dispatch_blocked_word(slot,word);
                    decoded_instruction = decode_rom;
                    decoded_word = word;
                } else {
                    auto &reject = sheep_jit_reject_filter[sheep_jit_filter_slot_general(pc_real, ppc_state.pc)];
                    blocked = reject != 0;
                    if (reject && reject != 255) --reject;
                }
                if (!direct_handled && !blocked && !exec_timer && g_icycles < max_cycles) {
                    if(!sheep_jit_hot<true>(pc_real,max_cycles-g_icycles,jit_count)) {
                        unsigned budget = std::min<uint64_t>(sheep_native_max_ops, max_cycles - g_icycles);
                        budget = std::min(budget, (eb_end - ppc_state.pc + 1) / 4);
                        unsigned decode_bound=0;
                        if constexpr(rom_profile)decode_bound=sheep_rom_plan_bound(pc_real,ppc_state.pc,eb_end);
                        jit_count = sheep_jit_try(pc_real, budget,decode_bound);
                        if (jit_count) ppc_state.pc += (jit_count - 1) * 4;
                    }
                }
            }
        }
        if (jit_count) {
            g_icycles += jit_count - 1;
            INCPC((jit_count - 1) * 4);
        } else {
            opcode = decoded_instruction && (decoded_word&3)!=3 ? uint32_t(decoded_word >> 32) : ppc_read_instruction(pc_real);
#if !defined(CPU_PROFILING) && !defined(LOG_INSTRUCTIONS)
            if constexpr (rom_profile) {
                unsigned primary = opcode >> 26;
                if (sheep_execute_simple_branch(opcode)) {}
                else if (decoded_instruction && (decoded_word&3)!=3 && compact_decoder)
                    sheep_rom_cached_handler(decoded_word, opcode_grabber == OpcodeGrabberNoFPU)(opcode);
                else if (compact_mask & (uint64_t(1) << primary))
                    compact_decoder[primary * 4 + (opcode & 3)](opcode);
                else
                    ppc_main_opcode(opcode_grabber, opcode);
            } else
#endif
                ppc_main_opcode(opcode_grabber, opcode);
        }
        } // Original cold dispatcher or exactly one pending steady-loop step.
        if (g_icycles++ >= max_cycles || exec_timer) [[unlikely]]
            max_cycles = process_events();

        if (exec_flags) {
            if ((exec_flags & EXEF_SLEEP) && !(exec_flags & EXEF_EXCEPTION)) [[unlikely]] {
                while (power_on && (exec_flags & EXEF_SLEEP)) {
                    max_cycles = process_events();
                    if (!(exec_flags & EXEF_SLEEP)) {
                        break;
                    }
                    if (max_cycles > g_icycles) {
                        g_icycles = max_cycles;
                    } else {
                        g_icycles++;
                    }
                }
            }
            if (exec_flags & (EXEF_OPC_DECODER | EXEF_EXCEPTION)) [[unlikely]] {
                opcode_grabber = ppc_opcode_grabber;
                compact_decoder = sheep_compact_decoder(opcode_grabber);
                compact_mask = compact_decoder ? sheep_compact_primary_mask : 0;
            }
            // define next execution block
            eb_start = ppc_next_instruction_address;
            if (!(exec_flags & (EXEF_RFI | EXEF_EXCEPTION | EXEF_FETCH)) && (eb_start & PPC_PAGE_MASK) == page_start) {
                if (endian == big_end)
                    INCPC((int)eb_start - (int)ppc_state.pc);
                else {
                    pc_real = translate_instruction(eb_start ATPCP); // &pcp
                    update_sheep_page(eb_start);
                }
            } else {
                page_start = eb_start & PPC_PAGE_MASK;
                eb_end = page_start + PPC_PAGE_SIZE - 1;
                pc_real = translate_instruction(eb_start ATPCP); // &pcp
                update_sheep_page(eb_start);
            }
            ppc_state.pc = eb_start;
            exec_flags = 0;
        } else [[likely]] {
            ppc_state.pc += 4;
            if (endian == big_end)
                INCPC(4);
            else {
                pc_real = translate_instruction(ppc_state.pc ATPCP); // &pcp
                update_sheep_page(ppc_state.pc);
            }
        }

        // Revoked windows are requalified only after event/flag processing and
        // an already-settled canonical source proof; never fetch a pending target.
        if(rom_window && !rom_window->valid_start_count && power_on && sheep_page_enabled &&
           sheep_itlb_primary_hit(ppc_state.pc)==pc_real)
            refresh_sheep_window(ppc_state.pc);

        if (exec_type == until)
            if (ppc_state.pc == start_addr)
                break;
    }
}

#include "fetch-barriers.inc"

/** Execute PPC code as long as power is on. */

// inner interpreter loop
template void ppc_exec_inner<main, big_end>(uint32_t start_addr, uint32_t size);
template void ppc_exec_inner<main, little_end>(uint32_t start_addr, uint32_t size);

// outer interpreter loop
void ppc_exec()
{
    if (setjmp(exc_env)) {
        // process low-level exceptions
        //LOG_F(9, "PPC-EXEC: low_level exception raised!");
        ppc_state.pc = ppc_next_instruction_address;
    }

    while (power_on) {
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
        if (ppc_state.is_LE)
            ppc_exec_inner<main, little_end>(0, 0);
        else
#endif
        [[likely]] {
            if (sheep_jit_enabled() && sheep_rom_only)
                ppc_exec_inner<main, big_end, true>(0, 0);
            else
                ppc_exec_inner<main, big_end>(0, 0);
        }
        if (!power_on && power_off_reason == po_endian_switch) [[unlikely]] {
            power_on = true;
        }
    }
}

/** Execute one PPC instruction. */
void ppc_exec_single()
{
    if (setjmp(exc_env)) {
        // process low-level exceptions
        //LOG_F(9, "PPC-EXEC: low_level exception raised!");
        ppc_state.pc = ppc_next_instruction_address;
        exec_flags = 0;
        return;
    }

    uint8_t* pc_real = mmu_translate_imem(ppc_state.pc ATPCP); // &pcp
    uint32_t opcode = ppc_read_instruction(pc_real);
    ppc_main_opcode(ppc_opcode_grabber, opcode);
    g_icycles++;
    process_events();

    if (exec_flags) {
        ppc_state.pc = ppc_next_instruction_address;
        exec_flags = 0;
    } else {
        ppc_state.pc += 4;
    }
}

/** Execute PPC code until goal_addr is reached. */

// inner interpreter loop
template void ppc_exec_inner<until, big_end>(uint32_t start_addr, uint32_t size);
template void ppc_exec_inner<until, little_end>(uint32_t start_addr, uint32_t size);

// outer interpreter loop
void ppc_exec_until(volatile uint32_t goal_addr) {
    if (setjmp(exc_env)) {
        // process low-level exceptions
        // LOG_F(9, "PPC-EXEC: low_level exception raised!");
        ppc_state.pc = ppc_next_instruction_address;
    }

    while (power_on) {
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
        if (ppc_state.is_LE)
            ppc_exec_inner<until, little_end>(goal_addr, 0);
        else
#endif
        [[likely]] {
            ppc_exec_inner<until, big_end>(goal_addr, 0);
        }
        if (!power_on && power_off_reason == po_endian_switch) [[unlikely]] {
            power_on = true;
        }
        if (ppc_state.pc == goal_addr)
            break;
    }
}

/** Execute PPC code until control is reached the specified region. */

// inner interpreter loop
template void ppc_exec_inner<debug, big_end>(uint32_t start_addr, uint32_t size);
template void ppc_exec_inner<debug, little_end>(uint32_t start_addr, uint32_t size);

// outer interpreter loop
void ppc_exec_dbg(volatile uint32_t start_addr, volatile uint32_t size)
{
    if (setjmp(exc_env)) {
        // process low-level exceptions
        //LOG_F(9, "PPC-EXEC: low_level exception raised!");
        ppc_state.pc = ppc_next_instruction_address;
    }

    while (power_on && (ppc_state.pc < start_addr || ppc_state.pc >= start_addr + size)) {
#if SUPPORTS_PPC_LITTLE_ENDIAN_MODE
        if (ppc_state.is_LE)
            ppc_exec_inner<debug, little_end>(start_addr, size);
        else
#endif
        [[likely]] {
            ppc_exec_inner<debug, big_end>(start_addr, size);
        }
        if (!power_on && power_off_reason == po_endian_switch) [[unlikely]] {
            power_on = true;
        }
    }
}

/*
Opcode table macros:
- d is for dot (RC).
- o is for overflow (OV).
- c is for carry CARRY0/CARRY1. It also works for other options:
  SHFT0/SHFT1, RIGHT0/LEFT1, uint8_t/uint16_t/uint32_t, and int8_t/int16_t.
- a is for absolute address (AA).
- l is for link register (LK).
- r is for raw (adding custom entries to the table)
 */

#define OPr(opcode, mod, fn) \
do { \
    OpcodeGrabber[((opcode) << 11) | (mod)] = fn; \
} while (0)

#define OPr_fp(opcode, mod, fn) \
do { \
    OPr(opcode, mod, fn); \
    OpcodeGrabberNoFPU[((opcode) << 11) | (mod)] = ppc_fpu_off; \
} while (0)

#define OP(opcode, fn) \
do { \
    for (uint32_t mod = 0; mod < 2048; mod++) { \
        OPr(opcode, mod, fn); \
    } \
} while (0)

#define OP_fp(opcode, fn) \
do { \
    for (uint32_t mod = 0; mod < 2048; mod++) { \
        OPr_fp(opcode, mod, fn); \
    } \
} while (0)

#define OPX(opcode, subopcode, fn) OPr(opcode, (subopcode)<<1, fn)

#define OPX_fp(opcode, subopcode, fn) OPr_fp(opcode, (subopcode)<<1, fn)

#define OPXd(opcode, subopcode, fn) \
do { \
    OPr(opcode, ((subopcode)<<1) | 0, fn<RC0>); \
    OPr(opcode, ((subopcode)<<1) | 1, fn<RC1>); \
} while (0)

#define OPXd_fp(opcode, subopcode, fn) \
do { \
    OPr_fp(opcode, ((subopcode)<<1) | 0, fn<RC0>); \
    OPr_fp(opcode, ((subopcode)<<1) | 1, fn<RC1>); \
} while (0)

#define OPXod(opcode, subopcode, fn) \
do { \
    OPr(opcode, ((subopcode)<<1) | 0x000, (fn<RC0, OV0>)); \
    OPr(opcode, ((subopcode)<<1) | 0x001, (fn<RC1, OV0>)); \
    OPr(opcode, ((subopcode)<<1) | 0x400, (fn<RC0, OV1>)); \
    OPr(opcode, ((subopcode)<<1) | 0x401, (fn<RC1, OV1>)); \
} while (0)

#define OPXdc(opcode, subopcode, fn, carry) \
do { \
    OPr(opcode, ((subopcode)<<1) | 0, (fn<carry, RC0>)); \
    OPr(opcode, ((subopcode)<<1) | 1, (fn<carry, RC1>)); \
} while (0)

#define OPXdc_fp(opcode, subopcode, fn, carry) \
do { \
    OPr_fp(opcode, ((subopcode)<<1) | 0, (fn<carry, RC0>)); \
    OPr_fp(opcode, ((subopcode)<<1) | 1, (fn<carry, RC1>)); \
} while (0)

#define OPXcod(opcode, subopcode, fn, carry) \
do { \
    OPr(opcode, ((subopcode)<<1) | 0x000, (fn<carry, RC0, OV0>)); \
    OPr(opcode, ((subopcode)<<1) | 0x001, (fn<carry, RC1, OV0>)); \
    OPr(opcode, ((subopcode)<<1) | 0x400, (fn<carry, RC0, OV1>)); \
    OPr(opcode, ((subopcode)<<1) | 0x401, (fn<carry, RC1, OV1>)); \
} while (0)

#define OPla(opcode, subopcode, fn) \
do { \
    for (uint32_t mod = 0; mod < 512; mod++) { \
        OPr(opcode, (mod << 2) | (subopcode), fn); \
    } \
} while (0)

#define OP31(subopcode, fn) OPX(31, subopcode, fn)
#define OP31_fp(subopcode, fn) OPX_fp(31, subopcode, fn)
#define OP31d(subopcode, fn) OPXd(31, subopcode, fn)
#define OP31od(subopcode, fn) OPXod(31, subopcode, fn)
#define OP31dc(subopcode, fn, carry) OPXdc(31, subopcode, fn, carry)
#define OP31cod(subopcode, fn, carry) OPXcod(31, subopcode, fn, carry)

#define OP63(subopcode, fn) OPX_fp(63, subopcode, fn)
#define OP63d(subopcode, fn) OPXd_fp(63, subopcode, fn)
#define OP63dc(subopcode, fn, carry) OPXdc_fp(63, subopcode, fn, carry)

#define OP59d(subopcode, fn) OPXd_fp(59, (subopcode), fn)

#define OP59cd(subopcode, fn) \
do { \
    for (uint32_t ccccc = 0; ccccc < 32; ccccc++) { \
        OPXd_fp(59, (ccccc << 5) | (subopcode), fn); \
    } \
} while (0)

void initialize_ppc_opcode_table() {
    auto opcodeGrabberSize = sizeof(OpcodeGrabber) / sizeof(OpcodeGrabber[0]);
    std::fill_n(OpcodeGrabber, opcodeGrabberSize, ppc_illegalop);
    std::fill_n(OpcodeGrabberNoFPU, opcodeGrabberSize, ppc_illegalop);

    OP(3,  ppc_twi);
    //OP(4,  ppc_opcode4); - Altivec instructions not emulated yet. Uncomment once they're implemented.
    OP(7,  ppc_mulli);
    OP(8,  ppc_subfic);
    if (is_601 || include_601) OP(9, power_dozi);
    OP(10, ppc_cmpli);
    OP(11, ppc_cmpi);
    OP(12, ppc_addic<RC0>);
    OP(13, ppc_addic<RC1>);
    OP(14, ppc_addi<SHFT0>);
    OP(15, ppc_addi<SHFT1>);
    OP(17, ppc_sc);
    OP(20, ppc_rlwimi);
    OP(21, ppc_rlwinm);
    if (is_601 || include_601) OP(22, power_rlmi);
    OP(23, ppc_rlwnm);
    OP(24, ppc_ori<SHFT0>);
    OP(25, ppc_ori<SHFT1>);
    OP(26, ppc_xori<SHFT0>);
    OP(27, ppc_xori<SHFT1>);
    OP(28, ppc_andirc<SHFT0>);
    OP(29, ppc_andirc<SHFT1>);
    OP(32, ppc_lz<uint32_t>);
    OP(33, ppc_lzu<uint32_t>);
    OP(34, ppc_lz<uint8_t>);
    OP(35, ppc_lzu<uint8_t>);
    OP(36, ppc_st<uint32_t>);
    OP(37, ppc_stu<uint32_t>);
    OP(38, ppc_st<uint8_t>);
    OP(39, ppc_stu<uint8_t>);
    OP(40, ppc_lz<uint16_t>);
    OP(41, ppc_lzu<uint16_t>);
    OP(42, ppc_lha);
    OP(43, ppc_lhau);
    OP(44, ppc_st<uint16_t>);
    OP(45, ppc_stu<uint16_t>);
    OP(46, ppc_lmw);
    OP(47, ppc_stmw);
    OP_fp(48, ppc_lfs);
    OP_fp(49, ppc_lfsu);
    OP_fp(50, ppc_lfd);
    OP_fp(51, ppc_lfdu);
    OP_fp(52, ppc_stfs);
    OP_fp(53, ppc_stfsu);
    OP_fp(54, ppc_stfd);
    OP_fp(55, ppc_stfdu);

    OPla(16, 0x0, (dppc_interpreter::ppc_bc<LK0, AA0>)); // bc
    OPla(16, 0x1, (dppc_interpreter::ppc_bc<LK1, AA0>)); // bcl
    OPla(16, 0x2, (dppc_interpreter::ppc_bc<LK0, AA1>)); // bca
    OPla(16, 0x3, (dppc_interpreter::ppc_bc<LK1, AA1>)); // bcla

    OPla(18, 0x0, (dppc_interpreter::ppc_b<LK0, AA0>)); // b
    OPla(18, 0x1, (dppc_interpreter::ppc_b<LK1, AA0>)); // bl
    OPla(18, 0x2, (dppc_interpreter::ppc_b<LK0, AA1>)); // ba
    OPla(18, 0x3, (dppc_interpreter::ppc_b<LK1, AA1>)); // bla

    OPr(19, 0, ppc_mcrf);
    OPr(19, 32, ppc_bclr<LK0>);
    OPr(19, 33, ppc_bclr<LK1>);
    OPr(19, 66, ppc_crnor);
    OPr(19, 100, ppc_rfi);
    OPr(19, 258, ppc_crandc);
    OPr(19, 300, ppc_isync);
    OPr(19, 386, ppc_crxor);
    OPr(19, 450, ppc_crnand);
    OPr(19, 514, ppc_crand);
    OPr(19, 578, ppc_creqv);
    OPr(19, 834, ppc_crorc);
    OPr(19, 898, ppc_cror);
    OPr(19, 1056, (is_601 ? ppc_bcctr<LK0, IS601> : ppc_bcctr<LK0, NOT601>));
    OPr(19, 1057, (is_601 ? ppc_bcctr<LK1, IS601> : ppc_bcctr<LK1, NOT601>));

    OP31(0,      ppc_cmp);
    OP31(4,      ppc_tw);
    OP31(32,     ppc_cmpl);

    OP31cod(8,   ppc_subf, CARRY1);
    OP31cod(40,  ppc_subf, CARRY0);
    OP31od(104,  ppc_neg);
    OP31od(136,  ppc_subfe);
    OP31od(200,  ppc_subfze);
    OP31od(232,  ppc_subfme);

    OP31cod(10,  ppc_add, CARRY1);
    OP31od(138,  ppc_adde);
    OP31od(202,  ppc_addze);
    OP31od(234,  ppc_addme);
    OP31cod(266, ppc_add, CARRY0);

    OP31d(11,    ppc_mulhwu);
    OP31d(75,    ppc_mulhw);
    OP31od(235,  ppc_mullw);
    OP31od(459,  ppc_divwu);
    OP31od(491,  ppc_divw);

    OP31(20,     ppc_lwarx);
    OP31(23,     ppc_lzx<uint32_t>);
    OP31(55,     ppc_lzux<uint32_t>);
    OP31(87,     ppc_lzx<uint8_t>);
    OP31(119,    ppc_lzux<uint8_t>);
    OP31(279,    ppc_lzx<uint16_t>);
    OP31(311,    ppc_lzux<uint16_t>);
    OP31(343,    ppc_lhax);
    OP31(375,    ppc_lhaux);
    OP31(533,    ppc_lswx);
    OP31(534,    ppc_lwbrx);
    OP31_fp(535,    ppc_lfsx);
    OP31_fp(567,    ppc_lfsux);
    OP31(597,    ppc_lswi);
    OP31_fp(599,    ppc_lfdx);
    OP31_fp(631,    ppc_lfdux);
    OP31(790,    ppc_lhbrx);

    OPr(31, (150<<1) | 1, ppc_stwcx); // No Rc=0 variant.
    OP31(151,    ppc_stx<uint32_t>);
    OP31(183,    ppc_stux<uint32_t>);
    OP31(215,    ppc_stx<uint8_t>);
    OP31(247,    ppc_stux<uint8_t>);
    OP31(407,    ppc_stx<uint16_t>);
    OP31(439,    ppc_stux<uint16_t>);
    OP31(661,    ppc_stswx);
    OP31(662,    ppc_stwbrx);
    OP31_fp(663,    ppc_stfsx);
    OP31_fp(695,    ppc_stfsux);
    OP31(725,    ppc_stswi);
    OP31_fp(727,    ppc_stfdx);
    OP31_fp(759,    ppc_stfdux);
    OP31(918,    ppc_sthbrx);
    if (!is_601) OP31_fp(983, ppc_stfiwx);

    OP31(310,    ppc_eciwx);
    OP31(438,    ppc_ecowx);

    OP31dc(24,   ppc_shift, LEFT1); // slw
    OP31dc(28,   ppc_logical, ppc_and);
    OP31dc(60,   ppc_logical, ppc_andc);
    OP31dc(124,  ppc_logical, ppc_nor);
    OP31dc(284,  ppc_logical, ppc_eqv);
    OP31dc(316,  ppc_logical, ppc_xor);
    OP31dc(412,  ppc_logical, ppc_orc);
    OP31dc(444,  ppc_logical, ppc_or);
    OP31dc(476,  ppc_logical, ppc_nand);
    OP31dc(536,  ppc_shift, RIGHT0); // srw
    OP31d(792,   ppc_sraw);
    OP31d(824,   ppc_srawi);
    OP31dc(922,  ppc_exts, int16_t);
    OP31dc(954,  ppc_exts, int8_t);

    OP31d(26,    ppc_cntlzw);

    OP31(19,     ppc_mfcr);
    OP31(83,     ppc_mfmsr);
    OP31(144,    ppc_mtcrf);
    OP31(146,    ppc_mtmsr);
    OP31(210,    ppc_mtsr);
    OP31(242,    ppc_mtsrin);
    OP31(339,    ppc_mfspr);
    if (!is_601) OP31(371, ppc_mftb);
    OP31(467,    ppc_mtspr);
    OP31(512,    ppc_mcrxr);
    OP31(595,    ppc_mfsr);
    OP31(659,    ppc_mfsrin);

    OP31(54,     ppc_dcbst);
    OP31(86,     ppc_dcbf);
    OP31(246,    ppc_dcbtst);
    OP31(278,    ppc_dcbt);
    OP31(598,    ppc_sync);
    OP31(470,    ppc_dcbi);
    OP31(1014,   ppc_dcbz);

    if (is_601 || include_601) {
        OP31d(29,   power_maskg);
        OP31od(107, power_mul);
        OP31d(152,  power_slq);
        OP31d(153,  power_sle);
        OP31d(184,  power_sliq);
        OP31d(216,  power_sllq);
        OP31d(217,  power_sleq);
        OP31d(248,  power_slliq);
        OP31od(264, power_doz);
        OP31d(277,  power_lscbx);
        OP31od(331, power_div);
        OP31od(360, power_abs);
        OP31od(363, power_divs);
        OP31od(488, power_nabs);
        OP31(531,   power_clcs);
        OP31d(537,  power_rrib);
        OP31d(541,  power_maskir);
        OP31d(664,  power_srq);
        OP31d(665,  power_sre);
        OP31d(696,  power_sriq);
        OP31d(728,  power_srlq);
        OP31d(729,  power_sreq);
        OP31d(760,  power_srliq);
        OP31d(920,  power_sraq);
        OP31d(921,  power_srea);
        OP31d(952,  power_sraiq);
    }

    OP31(306,    ppc_tlbie);
    if (!is_601) OP31(370, ppc_tlbia);
    if (!is_601) OP31(566, ppc_tlbsync);
    OP31(854,    ppc_eieio);
    OP31(982,    ppc_icbi);
    if (!is_601) OP31(978, ppc_tlbld);
    if (!is_601) OP31(1010, ppc_tlbli);

    OP59d(18,    ppc_fdivs);
    OP59d(20,    ppc_fsubs);
    OP59d(21,    ppc_fadds);
    if (ppc_state.spr[SPR::PVR] == PPC_VER::MPC970MP) OP59d(22, ppc_fsqrts);
    if (!is_601) OP59d(24, ppc_fres);
    OP59cd(25,   ppc_fmuls);
    OP59cd(28,   ppc_fmsubs);
    OP59cd(29,   ppc_fmadds);
    OP59cd(30,   ppc_fnmsubs);
    OP59cd(31,   ppc_fnmadds);

    OP63(0,      ppc_fcmpu);
    OP63d(12,    ppc_frsp);
    OP63d(14,    ppc_fctiw);
    OP63d(15,    ppc_fctiwz);
    OP63d(18,    ppc_fdiv);
    OP63d(20,    ppc_fsub);
    OP63d(21,    ppc_fadd);
    if (ppc_state.spr[SPR::PVR] == PPC_VER::MPC970MP) OP63d(22, ppc_fsqrt);
    if (!is_601) OP63d(26, ppc_frsqrte);
    OP63(32,     ppc_fcmpo);
    OP63d(38,    ppc_mtfsb1);
    OP63d(40,    ppc_fneg);
    OP63(64,     ppc_mcrfs);
    OP63d(70,    ppc_mtfsb0);
    OP63d(72,    ppc_fmr);
    OP63d(134,   ppc_mtfsfi);
    OP63d(136,   ppc_fnabs);
    OP63d(264,   ppc_fabs);
    if (is_601) OP63dc(583, ppc_mffs, IS601); else OP63dc(583, ppc_mffs, NOT601);
    OP63d(711,   ppc_mtfsf);

    for (int i = 0; i < 1024; i += 32) {
        if (!is_601) OP63d(i + 23, ppc_fsel);
        OP63d(i + 25, ppc_fmul);
        OP63d(i + 28, ppc_fmsub);
        OP63d(i + 29, ppc_fmadd);
        OP63d(i + 30, ppc_fnmsub);
        OP63d(i + 31, ppc_fnmadd);
    }

    for (auto i = 0; i < opcodeGrabberSize; i++) {
        if (OpcodeGrabberNoFPU[i] != ppc_fpu_off) {
            OpcodeGrabberNoFPU[i] = OpcodeGrabber[i];
        }
    }
    sheep_build_compact_decoders();
}

void ppc_cpu_init(MemCtrlBase* mem_ctrl, uint32_t cpu_version, bool do_include_601, uint64_t tb_freq)
{
    sheep_mmu_rom_window_enable(false);
    mem_ctrl_instance = mem_ctrl;
    pippin_authentic_init();   // Q234: parse and log authentic-speed knobs once

    int_pin = false;
    std::memset(&ppc_state, 0, sizeof(ppc_state));
    set_host_rounding_mode(0);

    ppc_state.spr[SPR::PVR] = cpu_version;
    is_601 = (cpu_version >> 16) == 1;
    include_601 = !is_601 & do_include_601;
    ppc_pow_mode = PPCPowMode::None;
    ppc_pow_hid0_mask = 0;

    switch (cpu_version) {
    case PPC_VER::MPC603:
    case PPC_VER::MPC603E:
    case PPC_VER::MPC603EV:
    case PPC_VER::MPC750:
        // 603/7xx enter power-saving modes through HID0 doze/nap/sleep
        // bits selected before MSR[POW] is set.
        ppc_pow_mode = PPCPowMode::HID0;
        ppc_pow_hid0_mask = HID0_POWER_SAVE_MASK;
        break;
    case PPC_VER::MPC604:
    case PPC_VER::MPC604E:
        // 604-family processors use MSR[POW] directly.
        ppc_pow_mode = PPCPowMode::Unconditional;
        break;
    default:
        break;
    }

    initialize_ppc_opcode_table();

    // initialize emulator timers
    TimerManager::get_instance()->set_time_now_cb(&get_virt_time_ns);
    TimerManager::get_instance()->set_notify_changes_cb(&force_cycle_counter_reload);

    // initialize time base facility
    g_nanoseconds_base = cpu_now_ns();
    g_icycles = 0;

//                    //                                        // PDM cpu clock calculated at 0x403036CC in r3
//  icnt_factor = 11; // 1 instruction = 2048 ns =    0.488 MHz // 00068034 =     0.426036 MHz = 2347.219 ns // floppy doesn't work
//  icnt_factor = 10; // 1 instruction = 1024 ns =    0.977 MHz // 000D204C =     0.860236 MHz = 1162.471 ns //  [0..10] MHz = invalid clock for PDM gestalt calculation
//  icnt_factor =  9; // 1 instruction =  512 ns =    1.953 MHz // 001A6081 =     1.728641 MHz =  578.489 ns //  [0..10] MHz = invalid clock for PDM gestalt calculation
//  icnt_factor =  8; // 1 instruction =  256 ns =    3.906 MHz // 0034E477 =     3.466359 MHz =  288.487 ns //  [0..10] MHz = invalid clock for PDM gestalt calculation
//  icnt_factor =  7; // 1 instruction =  128 ns =    7.813 MHz // 0069E54C =     6.939980 MHz =  144.092 ns //  [0..10] MHz = invalid clock for PDM gestalt calculation
//  icnt_factor =  6; // 1 instruction =   64 ns =   15.625 MHz // 00D3E6F5 =    13.887221 MHz =   72.008 ns // (10..60] = 50, (60..73] = 66, (73..100] = 80 MHz
//  icnt_factor =  5; // 1 instruction =   32 ns =   31.250 MHz // 01A7B672 =    27.768434 MHz =   36.012 ns //
    icnt_factor =  4; // 1 instruction =   16 ns =   62.500 MHz // 034F0F0F =    55.512847 MHz =   18.013 ns // 6100/60 in Apple System Profiler
//  icnt_factor =  3; // 1 instruction =    8 ns =  125.000 MHz // 069E1E1E =   111.025694 MHz =    9.006 ns // (100...) MHz = invalid clock for PDM gestalt calculation
//  icnt_factor =  2; // 1 instruction =    4 ns =  250.000 MHz // 0D3C3C3C =   222.051388 MHz =    4.503 ns // (100...) MHz = invalid clock for PDM gestalt calculation
//  icnt_factor =  1; // 1 instruction =    2 ns =  500.000 MHz // 1A611A7B =   442.571387 MHz =    2.259 ns // (100...) MHz = invalid clock for PDM gestalt calculation
//  icnt_factor =  0; // 1 instruction =    1 ns = 1500.000 MHz // 3465B2D9 =   879.080153 MHz =    1.137 ns // (100...) MHz = invalid clock for PDM gestalt calculation

    tbr_wr_timestamp = 0;
    rtc_timestamp = 0;
    tbr_wr_value = 0;
    if (is_601)
        tb_freq <<= 7;
    tbr_freq_shift = 0;
    uint64_t x;
    for (x = (tb_freq << 32) / NS_PER_SEC; x >> 32; x >>= 1)
        tbr_freq_shift++;
    tbr_freq_ghz = (uint32_t)x;
    tbr_period_ns = ((uint64_t)NS_PER_SEC << 32) / tb_freq;

    exec_flags = 0;
    exec_timer = false;

    dec_wr_value = 0;

#ifdef LOG_INSTRUCTIONS
    pcp = 0;
#endif

    uint32_t new_msr_val;
    if (is_601) {
        /* MPC601 sets MSR[ME] bit during hard reset / Power-On */
        new_msr_val = (MSR::ME + MSR::IP);
    } else {
        new_msr_val = MSR::IP;
        ppc_state.spr[SPR::DEC_S] = 0xFFFFFFFFUL;
        ppc_change_endian((new_msr_val & MSR::LE) != 0);
    }
    ppc_msr_did_change(new_msr_val, new_msr_val, false);

    ppc_mmu_init();

    /* redirect code execution to reset vector */
    ppc_state.pc = 0xFFF00100;

#ifdef CPU_PROFILING
    gProfilerObj->register_profile("PPC_CPU",
        std::unique_ptr<BaseProfile>(new CPUProfile()));
#endif
}

static map<string, int> SPRName2Num = {
    {"XER",    SPR::XER},       {"LR",     SPR::LR},    {"CTR",    SPR::CTR},
    {"DEC",    SPR::DEC_S},     {"PVR",    SPR::PVR},   {"SPRG0",  SPR::SPRG0},
    {"SPRG1",  SPR::SPRG1},     {"SPRG2",  SPR::SPRG2}, {"SPRG3",  SPR::SPRG3},
    {"SRR0",   SPR::SRR0},      {"SRR1",   SPR::SRR1},  {"IBAT0U", 528},
    {"IBAT0L", 529},            {"IBAT1U", 530},        {"IBAT1L", 531},
    {"IBAT2U", 532},            {"IBAT2L", 533},        {"IBAT3U", 534},
    {"IBAT3L", 535},            {"DBAT0U", 536},        {"DBAT0L", 537},
    {"DBAT1U", 538},            {"DBAT1L", 539},        {"DBAT2U", 540},
    {"DBAT2L", 541},            {"DBAT3U", 542},        {"DBAT3L", 543},
    {"HID0",   SPR::HID0},      {"HID1",   SPR::HID1},  {"IABR",   1010},
    {"DABR",   1013},           {"L2CR",   1017},       {"ICTC",   1019},
    {"THRM1",  SPR::THRM1},     {"THRM2",  SPR::THRM2}, {"THRM3",  SPR::THRM3},
    {"PIR",    1023},           {"TBL",    SPR::TBL_S}, {"TBU",    SPR::TBU_S},
    {"SDR1",   SPR::SDR1},      {"MQ",     SPR::MQ},    {"RTCU",   SPR::RTCU_S},
    {"RTCL",   SPR::RTCL_S},    {"DSISR",  SPR::DSISR}, {"DAR",    SPR::DAR},
    {"MMCR0",  SPR::MMCR0},     {"PMC1",   SPR::PMC1},  {"PMC2",   SPR::PMC2},
    {"SDA",    SPR::SDA},       {"SIA",    SPR::SIA},   {"MMCR1",  SPR::MMCR1},
    {"PMC3",   SPR::PMC3},      {"PMC4",   SPR::PMC4},  {"EAR",    SPR::EAR},

// get value from supervisor index same as we do for SPR268 and SPR269
    {"TBL_U",  SPR::TBL_S},     {"TBU_U",  SPR::TBU_S},
};

static uint64_t reg_op(string& reg_name, uint64_t val, bool is_write) {
    string reg_name_u, reg_num_str;
    unsigned reg_num;
    map<string, int>::iterator spr;

    if (reg_name.length() < 2)
        goto bail_out;

    reg_name_u = reg_name;

    /* convert reg_name string to uppercase */
    std::for_each(reg_name_u.begin(), reg_name_u.end(), [](char& c) {
        c = ::toupper(c);
    });

    try {
        if (reg_name_u == "PC") {
            if (is_write)
                ppc_state.pc = (uint32_t)val;
            return ppc_state.pc;
        }
        if (reg_name_u == "MSR") {
            if (is_write) {
                uint32_t old_msr_val = ppc_state.msr;
                uint32_t new_msr_val = (uint32_t)val;
                if (!is_601) {
                    ppc_change_endian((new_msr_val & MSR::LE) != 0);
                }
                ppc_msr_did_change(old_msr_val, new_msr_val, false);
            }
            return ppc_state.msr;
        }
        if (reg_name_u == "CR") {
            if (is_write)
                ppc_state.cr = (uint32_t)val;
            return ppc_state.cr;
        }
        if (reg_name_u == "FPSCR") {
            if (is_write)
                ppc_state.fpscr = (uint32_t)val;
            return ppc_state.fpscr;
        }
    } catch (...) {
    }

    try {
        if (reg_name_u.substr(0, 1) == "R") {
            reg_num_str = reg_name_u.substr(1);
            reg_num     = (unsigned)stoul(reg_num_str, NULL, 0);
            if (reg_num < 32) {
                if (is_write)
                    ppc_state.gpr[reg_num] = (uint32_t)val;
                return ppc_state.gpr[reg_num];
            }
        }
    } catch (...) {
    }

    try {
        if (reg_name_u.substr(0, 1) == "F") {
            reg_num_str = reg_name_u.substr(1);
            reg_num     = (unsigned)stoul(reg_num_str, NULL, 0);
            if (reg_num < 32) {
                if (is_write)
                    ppc_state.fpr[reg_num].int64_r = val;
                return ppc_state.fpr[reg_num].int64_r;
            }
        }
    } catch (...) {
    }

    try {
        if (reg_name_u.substr(0, 3) == "SPR") {
            reg_num_str = reg_name_u.substr(3);
            reg_num     = (unsigned)stoul(reg_num_str, NULL, 0);
            if (reg_num < 1024) {
                switch (reg_num) {
                case SPR::DEC_U  : reg_num = SPR::DEC_S  ; break;
                case SPR::RTCL_U : reg_num = SPR::RTCL_S ; break;
                case SPR::RTCU_U : reg_num = SPR::RTCU_S ; break;
                case SPR::TBL_U  : reg_num = SPR::TBL_S  ; break;
                case SPR::TBU_U  : reg_num = SPR::TBU_S  ; break;
                }
                if (is_write) {
                    ppc_state.spr[reg_num] = (uint32_t)val;
                    if (reg_num == SPR::HID0)
                        if (is_601) {
                            ppc_change_endian((val & 0x10000000) != 0);
                        }
                }
                return ppc_state.spr[reg_num];
            }
        }
    } catch (...) {
    }

    try {
        if (reg_name_u.substr(0, 2) == "SR") {
            reg_num_str = reg_name_u.substr(2);
            reg_num     = (unsigned)stoul(reg_num_str, NULL, 0);
            if (reg_num < 16) {
                if (is_write)
                    ppc_state.sr[reg_num] = (uint32_t)val;
                return ppc_state.sr[reg_num];
            }
        }
    } catch (...) {
    }

    try {
        spr = SPRName2Num.find(reg_name_u);
        if (spr != SPRName2Num.end()) {
            if (is_write)
                ppc_state.spr[spr->second] = (uint32_t)val;
            return ppc_state.spr[spr->second];
        }
    } catch (...) {
    }

bail_out:
    throw std::invalid_argument(string("Unknown register ") + reg_name);
}

uint64_t get_reg(string reg_name) {
    return reg_op(reg_name, 0, false);
}

void set_reg(string reg_name, uint64_t val) {
    sheep_mmu_rom_window_invalidate();
    reg_op(reg_name, val, true);
}

#include "generic-continuation-tests.inc"

#include "scalar-branch-tests.inc"

#include "clock-publication-tests.inc"

#include "rom-window-loop-tests.inc"
