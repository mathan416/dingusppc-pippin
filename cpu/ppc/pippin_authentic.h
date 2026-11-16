// Project: DingusPPC-Pippin
// File: cpu/ppc/pippin_authentic.h
// Purpose: Interface for the optional authentic-speed mode.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-16 - Q234: optional authentic-speed mode (CPU governor, CD-ROM timing model).
//   2026-11-16 - Review fixes: CD model calls carry the SCSI target; self-test entry point.
// Full history: PIPPIN.md and Git history.
// Q234: optional authentic-speed mode (CPU governor + CD-ROM timing model).
// Everything here is off by default; when off, callers pay one predictable branch.
#ifndef PIPPIN_AUTHENTIC_H
#define PIPPIN_AUTHENTIC_H

#include <cstdint>

// Parse SHEEP_PIPPIN_AUTHENTIC / SHEEP_PIPPIN_MAX_MIPS / SHEEP_PIPPIN_CD_* once and log
// the effective settings. Safe to call more than once (machine restart); logs once.
void pippin_authentic_init();

// ---- CPU governor (caps guest instruction retirement; guest timers stay realtime) ----
extern bool pippin_gov_enabled;
// Called at the top of process_events() when pippin_gov_enabled && g_realtime.
void pippin_gov_tick(uint64_t icycles, bool sleeping);
// Instructions the governed CPU retires in slice_ns of guest (= wall) time.
uint64_t pippin_gov_insns_for_ns(uint64_t slice_ns);

// ---- CD-ROM timing model (guest-time delays via TimerManager; no host sleeps) ----
extern bool pippin_cd_model_on;
// Target side: every CDB the CD-ROM receives (READ_6/10/12 start a timed stream).
void pippin_cd_on_command(int target, const uint8_t* cdb, uint64_t now_ns);
// Initiator side: MESH DataIn of 'bytes'; returns guest ns to wait before data arrives.
// Only DataIn from the target that issued the timed READ is delayed.
uint64_t pippin_cd_datain_wait_ns(int target, int bytes, uint64_t now_ns);

// Verify-mode self-test of the CD model's scoping and restart handling.
bool pippin_authentic_selftest();

#endif
