// Q234: optional authentic-speed mode for the Pippin hybrid (off by default).
//
// CPU governor: coarse token bucket checked once per process_events() batch. Guest time stays
// the host clock (--realtime); only instruction retirement is capped, so timers, VBL and audio
// keep their real rates. The governor sleeps (absolute CLOCK_MONOTONIC deadline) when the
// emulated CPU is ahead of max_mips, and process_events() sizes its realtime batches at the
// governed rate so timer deadlines are honoured. Instruction-count jumps of the MSR[POW]
// sleep loop are not charged.
//
// CD model: single-segment drive buffer. After a seek the head streams from the target block at
// the transfer rate and keeps reading ahead until the buffer (readahead KiB) holds that much
// beyond the last block the host asked for. A READ whose first block lies in the buffered window
// (the last <buffer> blocks behind the head, or ahead of it within the read-ahead span) needs no
// seek: its data is available as soon as the head has passed it. Any other READ pays the access
// latency and restarts the segment. Access time grows with seek distance (sled travel ~ sqrt of
// distance): access = min + (avg - min) * sqrt(distance / (disc/3)), where avg is the drive's
// rated average access time (1/3-stroke convention) and min covers settle + rotational latency.
// The MESH holds its DataIn sequencer step (TimerManager one-shot, guest time) until the bytes
// the step asks for would have arrived. No host sleeps; guest timers keep running.
#include "pippin_authentic.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern bool g_realtime;

bool pippin_gov_enabled = false;
bool pippin_cd_model_on = false;

// Calibrated preset for SHEEP_PIPPIN_AUTHENTIC=on (see performance-v234-authentic-speed/review):
// 26 MIPS fits the CPU-bound L-Zone fly-in on hardware; 4x CD (Apple CR-504-L) with 180/50 ms access.
static constexpr double   PRESET_MAX_MIPS      = 26.0;
static constexpr unsigned PRESET_CD_KIBPS      = 600;   // 4x (1x = 75 sectors/s x 2048 B = 150 KiB/s)
static constexpr double   PRESET_CD_SEEK_MS    = 180.0; // rated average access (1/3 stroke)
static constexpr double   PRESET_CD_SEEK_MIN_MS = 50.0; // short seek: settle + rotational latency
static constexpr double   CD_DISC_BLOCKS = 333000.0;    // 74-minute disc, 2048-byte blocks
static constexpr unsigned PRESET_CD_READAHEAD  = 256;   // KiB drive buffer

namespace {
struct Gov {
    double   max_mips = 0, ns_per_insn = 0, insn_per_ns = 0;
    bool     started = false, was_sleeping = false;
    uint64_t last_icycles = 0, due_ns = 0, first_ns = 0, charged = 0;
    uint64_t sleeps = 0, slept_ns = 0, ticks = 0, idle_ticks = 0, clamps = 0;
} gov;

struct Cd {
    double   bytes_per_ns = 0, seek_ns = 0, seek_min_ns = 0, readahead_bytes = 0, blk_ns = 0, buf_blocks = 0;
    bool     trace = false, active = false, have_prev = false;
    double   seg_lba = 0, seg_t0 = 0, last_req_end = 0, cap_lba = 0;   // segment state (blocks)
    double   cmd_start_ns = 0, prev_end_ns = 0;
    uint64_t issued = 0;
    uint64_t reads = 0, seeks = 0, datain = 0, bytes = 0, held = 0;
    double   wait_ns = 0;
} cd;

bool logged = false;

constexpr uint64_t GOV_SLACK_NS = 150'000;     // sleep only when >= 0.15 ms ahead
constexpr uint64_t GOV_BURST_NS = 4'000'000;   // at most 4 ms of lost time may be caught up
constexpr uint64_t GOV_MAX_AHEAD_NS = 50'000'000;

uint64_t mono_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// Strict decimal: digits{1,int_digits}[.digits{1,frac_digits}]
bool parse_decimal(const char* s, int int_digits, int frac_digits, double& out) {
    if (!s || !*s) return false;
    int i = 0, n = 0; double v = 0;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; n++; }
    if (n < 1 || n > int_digits) return false;
    if (s[i] == '.') {
        i++; int f = 0; double scale = 0.1;
        while (s[i] >= '0' && s[i] <= '9') { v += (s[i] - '0') * scale; scale /= 10; i++; f++; }
        if (f < 1 || f > frac_digits) return false;
    }
    if (s[i]) return false;
    out = v; return true;
}

void stats_at_exit() {
    if (pippin_gov_enabled) {
        const uint64_t span = gov.started ? mono_ns() - gov.first_ns : 0;
        fprintf(stderr, "PIPPIN-GOVERNOR stats max_mips=%.3f ticks=%llu idle_ticks=%llu sleeps=%llu slept_ms=%.1f "
                "clamps=%llu charged_insns=%llu span_ms=%.1f\n", gov.max_mips,
                (unsigned long long)gov.ticks, (unsigned long long)gov.idle_ticks, (unsigned long long)gov.sleeps,
                gov.slept_ns / 1e6, (unsigned long long)gov.clamps, (unsigned long long)gov.charged, span / 1e6);
    }
    if (pippin_cd_model_on) {
        fprintf(stderr, "PIPPIN-CD-MODEL stats reads=%llu seeks=%llu datain=%llu held=%llu bytes=%llu wait_ms=%.1f\n",
                (unsigned long long)cd.reads, (unsigned long long)cd.seeks, (unsigned long long)cd.datain,
                (unsigned long long)cd.held, (unsigned long long)cd.bytes, cd.wait_ns / 1e6);
    }
}
} // namespace

void pippin_authentic_init() {
    if (logged) return;
    logged = true;
    bool ok = true;
    const char* a = std::getenv("SHEEP_PIPPIN_AUTHENTIC");
    bool authentic = false;
    if (a) {
        if (!std::strcmp(a, "on")) authentic = true;
        else if (std::strcmp(a, "off")) { ok = false; fprintf(stderr, "PIPPIN-AUTHENTIC invalid SHEEP_PIPPIN_AUTHENTIC='%s' (on|off); ignored\n", a); }
    }
    double mips = authentic ? PRESET_MAX_MIPS : 0;
    double kibps = authentic ? PRESET_CD_KIBPS : 0;
    double seek_ms = PRESET_CD_SEEK_MS;
    double ra_kib = PRESET_CD_READAHEAD;
    double seek_min_ms = PRESET_CD_SEEK_MIN_MS;

    if (const char* s = std::getenv("SHEEP_PIPPIN_MAX_MIPS")) {
        double v;
        if (!std::strcmp(s, "off")) mips = 0;
        else if (parse_decimal(s, 4, 3, v) && (v == 0 || (v >= 1 && v <= 2000))) mips = v;
        else { ok = false; fprintf(stderr, "PIPPIN-AUTHENTIC invalid SHEEP_PIPPIN_MAX_MIPS='%s' (off|0|1..2000); ignored\n", s); }
    }
    if (const char* s = std::getenv("SHEEP_PIPPIN_CD_SPEED")) {
        double v; const size_t n = std::strlen(s);
        if (!std::strcmp(s, "off")) kibps = 0;
        else if (n >= 2 && n <= 3 && s[n - 1] == 'x') {
            char buf[4] = {}; std::memcpy(buf, s, n - 1);
            if (parse_decimal(buf, 2, 0, v) && v >= 1 && v <= 64) kibps = v * 150;
            else { ok = false; fprintf(stderr, "PIPPIN-AUTHENTIC invalid SHEEP_PIPPIN_CD_SPEED='%s'; ignored\n", s); }
        } else if (parse_decimal(s, 5, 0, v) && (v == 0 || (v >= 16 && v <= 99999))) kibps = v;
        else { ok = false; fprintf(stderr, "PIPPIN-AUTHENTIC invalid SHEEP_PIPPIN_CD_SPEED='%s' (off|0|Nx|KiB/s); ignored\n", s); }
    }
    if (const char* s = std::getenv("SHEEP_PIPPIN_CD_SEEK_MS")) {
        double v;
        if (parse_decimal(s, 4, 2, v) && v <= 2000) seek_ms = v;
        else { ok = false; fprintf(stderr, "PIPPIN-AUTHENTIC invalid SHEEP_PIPPIN_CD_SEEK_MS='%s' (0..2000); ignored\n", s); }
    }
    if (const char* s = std::getenv("SHEEP_PIPPIN_CD_SEEK_MIN_MS")) {
        double v;
        if (parse_decimal(s, 4, 2, v) && v <= 2000) seek_min_ms = v;
        else { ok = false; fprintf(stderr, "PIPPIN-AUTHENTIC invalid SHEEP_PIPPIN_CD_SEEK_MIN_MS='%s' (0..2000); ignored\n", s); }
    }
    if (seek_min_ms > seek_ms) seek_min_ms = seek_ms;
    if (const char* s = std::getenv("SHEEP_PIPPIN_CD_READAHEAD_KB")) {
        double v;
        if (parse_decimal(s, 4, 0, v) && v <= 4096) ra_kib = v;
        else { ok = false; fprintf(stderr, "PIPPIN-AUTHENTIC invalid SHEEP_PIPPIN_CD_READAHEAD_KB='%s' (0..4096); ignored\n", s); }
    }
    if (const char* s = std::getenv("SHEEP_PIPPIN_CD_TRACE")) cd.trace = !std::strcmp(s, "1");

    pippin_gov_enabled = mips > 0;
    gov = Gov{};
    if (pippin_gov_enabled) {
        gov.max_mips = mips;
        gov.ns_per_insn = 1000.0 / mips;
        gov.insn_per_ns = mips / 1000.0;
    }
    pippin_cd_model_on = kibps > 0;
    const bool trace = cd.trace;
    cd = Cd{};
    cd.trace = trace;
    if (pippin_cd_model_on) {
        cd.bytes_per_ns = kibps * 1024.0 / 1e9;
        cd.seek_ns = seek_ms * 1e6;
        cd.seek_min_ns = seek_min_ms * 1e6;
        cd.readahead_bytes = ra_kib * 1024.0;
        cd.blk_ns = 2048.0 / cd.bytes_per_ns;            // CD-ROM mode 1 user block
        cd.buf_blocks = cd.readahead_bytes / 2048.0;
    }
    fprintf(stderr, "PIPPIN-AUTHENTIC mode=%s governor=%s max_mips=%.3f cd_model=%s cd_speed_kibps=%.0f cd_speed_x=%.2f "
            "cd_seek_ms=%.2f cd_seek_min_ms=%.2f cd_readahead_kib=%.0f realtime=%u parse_ok=%u\n",
            authentic ? "on" : "off", pippin_gov_enabled ? (g_realtime ? "on" : "inactive-needs-realtime") : "off", mips,
            pippin_cd_model_on ? "on" : "off", kibps, kibps / 150.0, pippin_cd_model_on ? seek_ms : 0.0,
            pippin_cd_model_on ? seek_min_ms : 0.0, pippin_cd_model_on ? ra_kib : 0.0, unsigned(g_realtime), unsigned(ok));
    if (pippin_gov_enabled || pippin_cd_model_on) std::atexit(stats_at_exit);
}

uint64_t pippin_gov_insns_for_ns(uint64_t slice_ns) {
    return uint64_t(double(slice_ns) * gov.insn_per_ns);
}

void pippin_gov_tick(uint64_t icycles, bool sleeping) {
    const uint64_t now = mono_ns();
    gov.ticks++;
    if (sleeping || gov.was_sleeping || !gov.started || icycles < gov.last_icycles) {
        // MSR[POW] sleep loop advances g_icycles to the next deadline without executing:
        // never charge it. Resynchronise the bucket instead.
        if (!gov.started) gov.first_ns = now;
        gov.started = true;
        gov.was_sleeping = sleeping;
        gov.last_icycles = icycles;
        gov.due_ns = now;
        gov.idle_ticks += sleeping;
        return;
    }
    const uint64_t delta = icycles - gov.last_icycles;
    gov.last_icycles = icycles;
    gov.charged += delta;
    gov.due_ns += uint64_t(double(delta) * gov.ns_per_insn);
    if (gov.due_ns + GOV_BURST_NS < now) gov.due_ns = now - GOV_BURST_NS;   // emulator fell behind
    if (gov.due_ns > now + GOV_MAX_AHEAD_NS) { gov.due_ns = now + GOV_MAX_AHEAD_NS; gov.clamps++; }
    if (gov.due_ns > now + GOV_SLACK_NS) {
#ifdef TIMER_ABSTIME
        timespec ts;
        ts.tv_sec = time_t(gov.due_ns / 1000000000ull);
        ts.tv_nsec = long(gov.due_ns % 1000000000ull);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
#else
        const uint64_t rel = gov.due_ns - now;
        timespec ts{time_t(rel / 1000000000ull), long(rel % 1000000000ull)};
        nanosleep(&ts, nullptr);
#endif
        gov.sleeps++;
        gov.slept_ns += gov.due_ns - now;
    }
}

static double cd_access_ns(double dist_blocks) {
    double frac = std::sqrt(dist_blocks / (CD_DISC_BLOCKS / 3.0));
    if (frac > 1.733) frac = 1.733;   // full stroke
    return cd.seek_min_ns + (cd.seek_ns - cd.seek_min_ns) * frac;
}

void pippin_cd_on_command(const uint8_t* cdb, uint64_t now_ns) {
    cd.active = false;
    uint64_t lba, nblocks;
    switch (cdb[0]) {
    case 0x08: // READ(6)
        lba = (uint64_t(cdb[1] & 0x1F) << 16) | (uint64_t(cdb[2]) << 8) | cdb[3];
        nblocks = cdb[4] ? cdb[4] : 256;
        break;
    case 0x28: // READ(10)
        lba = (uint64_t(cdb[2]) << 24) | (uint64_t(cdb[3]) << 16) | (uint64_t(cdb[4]) << 8) | cdb[5];
        nblocks = (uint64_t(cdb[7]) << 8) | cdb[8];
        break;
    case 0xA8: // READ(12)
        lba = (uint64_t(cdb[2]) << 24) | (uint64_t(cdb[3]) << 16) | (uint64_t(cdb[4]) << 8) | cdb[5];
        nblocks = (uint64_t(cdb[6]) << 24) | (uint64_t(cdb[7]) << 16) | (uint64_t(cdb[8]) << 8) | cdb[9];
        break;
    default:
        return;
    }
    if (!nblocks) return;
    const double now = double(now_ns);
    const double L = double(lba);
    bool hit = false;
    double from = 0;   // head position (blocks) for a seek
    if (cd.have_prev) {
        const double head = cd.seg_lba + (now - cd.seg_t0) / cd.blk_ns;   // unclamped head position
        const double frontier = head < cd.cap_lba ? head : cd.cap_lba;
        const double low = frontier - cd.buf_blocks > cd.seg_lba ? frontier - cd.buf_blocks : cd.seg_lba;
        from = head < cd.seg_lba ? cd.seg_lba : frontier;
        hit = L >= low && L <= cd.cap_lba;
        // A forward request beyond the head: the firmware seeks if that beats reading up to it.
        if (hit && L > frontier && (L - frontier) * cd.blk_ns > cd_access_ns(L - frontier))
            hit = false;
        if (hit && head > cd.cap_lba)   // head had stopped with a full buffer; it resumes now
            cd.seg_t0 = now - (cd.cap_lba - cd.seg_lba) * cd.blk_ns;
    }
    if (hit) {
        cd.cmd_start_ns = cd.seg_t0 + (L - cd.seg_lba) * cd.blk_ns;
        const double end = L + double(nblocks);
        if (end > cd.last_req_end) cd.last_req_end = end;
    } else {
        const double access = cd_access_ns(L > from ? L - from : from - L);
        cd.seg_lba = L;
        cd.seg_t0 = now + access;
        cd.cmd_start_ns = cd.seg_t0;
        cd.last_req_end = L + double(nblocks);
        cd.seeks++;
    }
    cd.cap_lba = cd.last_req_end + cd.buf_blocks;
    const bool sequential = hit;
    cd.reads++;
    cd.issued = 0;
    cd.active = true;
    cd.have_prev = true;
    cd.prev_end_ns = cd.cmd_start_ns;
    if (cd.trace)
        fprintf(stderr, "PIPPIN-CD read t_ms=%.3f lba=%llu blocks=%llu hit=%u start_in_ms=%.3f\n", now / 1e6,
                (unsigned long long)lba, (unsigned long long)nblocks, unsigned(sequential), (cd.cmd_start_ns - now) / 1e6);
}

uint64_t pippin_cd_datain_wait_ns(int bytes, uint64_t now_ns) {
    if (!cd.active || bytes <= 0) return 0;
    cd.datain++;
    cd.issued += uint64_t(bytes);
    cd.bytes += uint64_t(bytes);
    cd.prev_end_ns = cd.cmd_start_ns + double(cd.issued) / cd.bytes_per_ns;
    const double now = double(now_ns);
    if (cd.prev_end_ns <= now) return 0;
    const double wait = cd.prev_end_ns - now;
    cd.held++;
    cd.wait_ns += wait;
    return uint64_t(wait);
}
