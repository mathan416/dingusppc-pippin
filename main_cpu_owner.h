// Private Pippin CPU-owner pilot. SDL belongs to the original main thread.
#ifndef PIPPIN_MAIN_CPU_OWNER_H
#define PIPPIN_MAIN_CPU_OWNER_H

#include <cstdint>
#include <functional>

namespace pippin_cpu_owner {
// Initialization is main-only, before constructing the machine.
void initialize(bool enable);
bool enabled() noexcept;
bool active() noexcept;
bool is_main_thread() noexcept;
bool is_worker_thread() noexcept;

// Main jobs may touch SDL, but must not touch guest/device state. call_main
// propagates the job's exception to its waiting worker. No caller holds an
// input/frame mutex while waiting. post_main callers coalesce frame jobs.
void call_main(std::function<void()> job);
void post_main(std::function<void()> job);

// Owned, ordered input/control jobs execute only at the existing guest event
// timer. Overflow is a recorded fail-stop, never a successful dropped release.
void post_guest(std::function<void()> job);
void drain_guest();
void publish_guest_time();
void request_stop() noexcept;
bool request_stop_if_active() noexcept;

// Main pumps SDL and main jobs while one worker executes guest code. All main
// jobs are drained and the worker is joined before this returns or rethrows.
void run(std::function<void()> guest_code);
}

// The private SDL hook must use this atomic snapshot, never call the guest
// clock getter on the presentation thread. Units are guest nanoseconds.
extern "C" uint64_t pippin_cpu_owner_guest_time_ns() noexcept;
#endif
