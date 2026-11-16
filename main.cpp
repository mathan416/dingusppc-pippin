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
 *   2026-11-15 - Q219 baseline (Q48 to Q219, before version control): CPU-owner thread, realtime clock, Pippin cabinet options.
 *   2026-11-16 - --jit-selftest option for CI (JIT self-tests without running guest code).
 * Full history: PIPPIN.md and Git history.
 */

// The main runfile - main.cpp
// This is where the magic begins

#include <core/hostevents.h>
#include <core/timermanager.h>
#include <cpu/ppc/ppcdisasm.h>
#include <cpu/ppc/ppcemu.h>
#include <cpu/ppc/ppcmmu.h>
#include <debugger/debugger.h>
#include <devices/common/ofnvram.h>
#include <devices/common/viacuda.h>
#include <machines/machinebase.h>
#include <machines/machinefactory.h>
#include <utils/profiler.h>
#include <main.h>
#include <main_cpu_owner.h>
#include <cpu/ppc/sheep_jit_bridge.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <pthread.h>
#include <sys/syscall.h>
#endif

#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iostream>
#include <optional>
#include <CLI11.hpp>
#include <loguru.hpp>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
using namespace std;

namespace pippin_cpu_owner {
namespace {
static_assert(std::atomic<bool>::is_always_lock_free,
              "CPU-owner signal requests require lock-free atomic bool");
struct MainJob {
    std::function<void()> fn;
    bool synchronous = false;
    bool finished = false;
    std::exception_ptr error;
    std::mutex mutex;
    std::condition_variable changed;
};
struct OwnerState {
    bool enable = false;
    std::thread::id main_thread;
    std::atomic<bool> running{false}, stop{false}, done{false};
    std::atomic<uint64_t> guest_ns{0};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<MainJob>> main_jobs;
    std::deque<std::function<void()>> guest_jobs;
    std::exception_ptr error;
    uint64_t guest_posted = 0, guest_run = 0, main_run = 0;
    size_t guest_peak = 0, main_peak = 0;
} owner;
thread_local bool worker_thread = false;
constexpr size_t main_capacity = 64, guest_capacity = 4096;
uint64_t current_tid() {
#ifdef __linux__
    return uint64_t(syscall(SYS_gettid));
#else
    return uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
void name_current_thread(const char *name) {
#ifdef __linux__
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}
void remember_error(std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (!owner.error) owner.error = error;
    owner.stop.store(true, std::memory_order_relaxed);
}
void enqueue_main(const std::shared_ptr<MainJob>& job) {
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        if (owner.main_jobs.size() >= main_capacity) {
            owner.stop.store(true, std::memory_order_relaxed);
            throw std::runtime_error("CPU-owner main job queue overflow");
        }
        owner.main_jobs.push_back(job);
        owner.main_peak = std::max(owner.main_peak, owner.main_jobs.size());
    }
    owner.changed.notify_one();
}
void service_main_jobs() {
    if (!is_main_thread()) throw std::runtime_error("CPU-owner main job on wrong thread");
    // Work posted during presentation belongs to the next pump iteration, so
    // continuous frame production cannot starve SDL input/quit capture.
    std::deque<std::shared_ptr<MainJob>> batch;
    { std::lock_guard<std::mutex> lock(owner.mutex); batch.swap(owner.main_jobs); }
    for (auto& job : batch) {
        std::exception_ptr error;
        try { job->fn(); } catch (...) { error = std::current_exception(); }
        ++owner.main_run;
        if (job->synchronous) {
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->error = error;
                job->finished = true;
            }
            job->changed.notify_one();
        } else if (error) {
            remember_error(error);
        }
    }
}
void term_request(int) {
    // No locks, SDL, guest state, or condition-variable operation in a handler.
    owner.stop.store(true, std::memory_order_relaxed);
}
}
void initialize(bool enable) {
    if (active()) throw std::runtime_error("CPU-owner initialized while active");
    owner.main_thread = std::this_thread::get_id();
    owner.enable = enable;
}
bool enabled() noexcept { return owner.enable; }
bool active() noexcept { return owner.running.load(std::memory_order_acquire); }
bool is_main_thread() noexcept { return std::this_thread::get_id() == owner.main_thread; }
bool is_worker_thread() noexcept { return worker_thread; }
void request_stop() noexcept { owner.stop.store(true, std::memory_order_relaxed); }
bool request_stop_if_active() noexcept {
    if (!owner.running.load(std::memory_order_relaxed)) return false;
    owner.stop.store(true, std::memory_order_relaxed);
    return true;
}
void call_main(std::function<void()> fn) {
    if (!active() || is_main_thread()) { fn(); return; }
    if (!is_worker_thread()) throw std::runtime_error("CPU-owner unknown main-job caller");
    auto job = std::make_shared<MainJob>();
    job->fn = std::move(fn); job->synchronous = true;
    enqueue_main(job);
    std::unique_lock<std::mutex> lock(job->mutex);
    job->changed.wait(lock, [&] { return job->finished; });
    if (job->error) std::rethrow_exception(job->error);
}
void post_main(std::function<void()> fn) {
    if (!active() || is_main_thread()) { fn(); return; }
    if (!is_worker_thread()) throw std::runtime_error("CPU-owner unknown main-post caller");
    auto job = std::make_shared<MainJob>(); job->fn = std::move(fn); enqueue_main(job);
}
void post_guest(std::function<void()> fn) {
    if (!active() || is_worker_thread()) { fn(); return; }
    if (!is_main_thread()) throw std::runtime_error("CPU-owner unknown guest-post caller");
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (owner.guest_jobs.size() >= guest_capacity) {
        owner.stop.store(true, std::memory_order_relaxed);
        throw std::runtime_error("CPU-owner input queue overflow; stopping without dropping a successful input");
    }
    owner.guest_jobs.push_back(std::move(fn)); ++owner.guest_posted;
    owner.guest_peak = std::max(owner.guest_peak, owner.guest_jobs.size());
}
void publish_guest_time() {
    if (active() && !is_worker_thread())
        throw std::runtime_error("CPU-owner guest time published by non-owner");
    owner.guest_ns.store(get_virt_time_ns(), std::memory_order_release);
}
void drain_guest() {
    if (!active() || !is_worker_thread())
        throw std::runtime_error("CPU-owner guest drain on wrong thread");
    // One bounded FIFO batch per original guest polling callback. Jobs may
    // synchronously request main work, but never hold this mutex while doing so.
    std::deque<std::function<void()>> batch;
    { std::lock_guard<std::mutex> lock(owner.mutex); batch.swap(owner.guest_jobs); }
    for (auto& job : batch) { job(); ++owner.guest_run; }
    if (owner.stop.load(std::memory_order_relaxed)) power_off(po_quit);
    publish_guest_time();
}
void run(std::function<void()> guest_code) {
    if (!enabled() || !is_main_thread() || active())
        throw std::runtime_error("CPU-owner invalid launch state");
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        if (!owner.main_jobs.empty() || !owner.guest_jobs.empty())
            throw std::runtime_error("CPU-owner stale work before launch");
        owner.error = nullptr; owner.guest_posted = owner.guest_run = owner.main_run = 0;
        owner.guest_peak = owner.main_peak = 0;
    }
    owner.stop.store(false, std::memory_order_relaxed);
    owner.done.store(false, std::memory_order_relaxed);
    publish_guest_time();
    const uint64_t main_tid = current_tid();
    name_current_thread("pippin-sdl");
    auto old_term = std::signal(SIGTERM, term_request);
    if (old_term == SIG_ERR) throw std::runtime_error("CPU-owner SIGTERM installation failed");
    owner.running.store(true, std::memory_order_release);
    std::thread worker;
    try {
        worker = std::thread([main_tid, guest_code = std::move(guest_code)] {
            worker_thread = true; name_current_thread("pippin-cpu");
            fprintf(stderr, "PIPPIN-CPU-OWNER start schema=1 main_tid=%llu worker_tid=%llu\n",
                    (unsigned long long)main_tid, (unsigned long long)current_tid());
            try { guest_code(); publish_guest_time(); }
            catch (...) { remember_error(std::current_exception()); }
            owner.done.store(true, std::memory_order_release);
            owner.changed.notify_one(); worker_thread = false;
        });
    } catch (...) {
        owner.running.store(false, std::memory_order_release);
        std::signal(SIGTERM, old_term); throw;
    }
    // Keep servicing synchronous UI requests even after a stop/error request.
    // Joining early would deadlock a worker waiting for call_main completion.
    while (!owner.done.load(std::memory_order_acquire)) {
        if (!owner.stop.load(std::memory_order_relaxed)) {
            try { EventManager::get_instance()->capture_host_events(); }
            catch (...) { remember_error(std::current_exception()); }
        }
        service_main_jobs();
        std::unique_lock<std::mutex> lock(owner.mutex);
        owner.changed.wait_for(lock, std::chrono::milliseconds(1), [] {
            return owner.done.load(std::memory_order_acquire) || !owner.main_jobs.empty();
        });
    }
    service_main_jobs();
    worker.join();
    // The worker cannot publish more callbacks after join. Complete all prior
    // async frame jobs before the caller can destroy a Display or its device.
    service_main_jobs();
    std::exception_ptr error; size_t abandoned;
    {
        std::lock_guard<std::mutex> lock(owner.mutex);
        error = owner.error; abandoned = owner.guest_jobs.size(); owner.guest_jobs.clear();
    }
    owner.running.store(false, std::memory_order_release);
    std::signal(SIGTERM, old_term);
    fprintf(stderr, "PIPPIN-CPU-OWNER stop schema=1 guest_posted=%llu guest_run=%llu main_run=%llu guest_peak=%zu main_peak=%zu shutdown_pending=%zu failure=%u\n",
            (unsigned long long)owner.guest_posted, (unsigned long long)owner.guest_run,
            (unsigned long long)owner.main_run, owner.guest_peak, owner.main_peak, abandoned, unsigned(bool(error)));
    if (error) std::rethrow_exception(error);
}
}
extern "C" uint64_t pippin_cpu_owner_guest_time_ns() noexcept {
    return pippin_cpu_owner::owner.guest_ns.load(std::memory_order_acquire);
}


extern bool g_auto_grab_mouse;
extern bool g_swap_command_option;

static void sigint_handler(int signum) {
    if (pippin_cpu_owner::request_stop_if_active()) return;
    power_off(po_signal_interrupt);
}

static void sigabrt_handler(int signum) {
    // A fatal worker signal must never destroy SDL/device objects concurrently.
    if (pippin_cpu_owner::active()) std::_Exit(128 + signum);
    LOG_F(INFO, "Shutting down...");

    delete gMachineObj.release();
    cleanup();
}

static string appDescription = string(
    "\nDingusPPC - Alpha 1.04c (10/4/2026)          "
    "\nWritten by divingkatae, maximumspatium,      "
    "\njoevt, mihaip, kkaisershot, et. al.          "
    "\n(c) 2018-2026 The DingusPPC Dev Team.        "
    "\nThis is a build intended for testing.        "
    "\nUse at your own discretion.                  "
    "\n"
);

static uint32_t keyboard_id = 0;

static std::optional<std::chrono::year_month_day> parse_calendar_date(const std::string& value)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        return std::nullopt;
    for (size_t i = 0; i < value.size(); i++) {
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9'))
            return std::nullopt;
    }

    std::chrono::year_month_day date{
        std::chrono::year{std::stoi(value.substr(0, 4))},
        std::chrono::month{unsigned(std::stoi(value.substr(5, 2)))},
        std::chrono::day{unsigned(std::stoi(value.substr(8, 2)))}
    };
    if (!date.ok() || int(date.year()) == 0)
        return std::nullopt;
    return date;
}

static std::optional<std::chrono::local_seconds> parse_start_date(std::string value, bool is_deterministic)
{
    if (value.empty()) {
        if (!is_deterministic)
            return std::nullopt;
        // March 24, 2001 was the public release date of Mac OS X.
        value = "2001-03-24";
    }

    auto date = *parse_calendar_date(value);
    int seconds_of_day = 12 * 60 * 60; // Noon
    if (!is_deterministic) {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        auto local_time = *std::localtime(&now);
        seconds_of_day = local_time.tm_hour * 60 * 60 + local_time.tm_min * 60 + local_time.tm_sec;
    }
    return std::chrono::local_days{date} + std::chrono::seconds{seconds_of_day};
}

/// Check for an existing directory (returns error message if check fails)
class WorkingDirectoryValidator : public CLI::detail::ExistingDirectoryValidator {
public:
    WorkingDirectoryValidator() {
        func_ = [](std::string& filename) {
            std::string result = CLI::ExistingDirectory.operator()(filename);
            if (result.empty()) {
                #ifdef _WIN32
                _chdir(filename.c_str());
                #else
                chdir(filename.c_str());
                #endif
            }
            return result;
        };
    }
};

const WorkingDirectoryValidator WorkingDirectory;

// --jit-selftest: run the JIT verify-mode self-tests after machine setup, then exit.
static bool g_jit_selftest = false;

void run_machine(
    std::string machine_str, char *rom_data, size_t rom_size, uint32_t execution_mode
    ,const std::vector<std::string> &env_vars
    ,bool deterministic_interactive
    ,uint32_t profiling_interval_ms
);

int main(int argc, char** argv) {

    uint32_t execution_mode = interpreter;

    CLI::App app(appDescription);
    app.allow_extras();

    app.set_help_all_flag("--help-all", "Print this help message, help for subcommands, and exit");

    bool debugger_skip = true;
    bool debugger_enter = false;
    bool deterministic_interactive = false;
    string deterministic_mode = "strict";
    string start_date;
    string keyboard_string = "Eng_USA";

    const std::map<std::string, int> kbd_map{
        {"Eng_USA", 0}, {"Eng_GBR", 1}, {"Fra_FRA", 10}, {"Deu_DEU", 20},
        {"Ita_ITA", 30},{"Spa_ESP", 40}, {"Jpn_JPN", 80},
    };

    string bootrom_path("bootrom.bin");
    string working_directory_path(".");

    auto emu = app.add_subcommand("", "Emulation");
    auto execution_mode_group = emu->add_option_group("execution mode")
        ->require_option(-1);
    execution_mode_group->add_flag("-r,--run", debugger_skip,
        "Run the emulator immediately (skip enterring the built-in debugger)");
    execution_mode_group->add_flag("-d,--debugger", debugger_enter,
        "Enter the built-in debugger");
    emu->add_option("-k,--keyboard", keyboard_string, "Specify keyboard ID");
    emu->add_option("-w,--workingdir", working_directory_path, "Specifies working directory")
        ->check(WorkingDirectory)->capture_default_str();
    auto bootrom_opt = emu->add_option("-b,--bootrom", bootrom_path, "Specifies BootROM path")
        ->check(CLI::ExistingFile)->capture_default_str();
    emu->add_flag("--jit-selftest", g_jit_selftest,
        "Run the ARM64 JIT verify-mode self-tests after machine setup and exit (0 = pass); no guest code runs");
    emu->add_flag("--realtime", g_realtime,
        "Advance guest timers using the host clock");
    auto deterministic_opt = emu->add_flag("--deterministic", is_deterministic,
        "Use deterministic execution");
    emu->add_option("--start-date", start_date,
        "Set the guest RTC start date (YYYY-MM-DD), keeping the current local time; "
        "deterministic mode defaults to 2001-03-24 at noon")
        ->check(CLI::Validator([](std::string& value) {
            return parse_calendar_date(value) ? std::string() :
                std::string("Expected a valid date in YYYY-MM-DD format");
        }, "YYYY-MM-DD"));
    emu->add_option("--deterministic-mode", deterministic_mode,
        "Select deterministic features (strict or interactive)")
        ->needs(deterministic_opt)
        ->check(CLI::IsMember({"strict", "interactive"}));

    bool              log_to_stderr = false;
    loguru::Verbosity log_verbosity = loguru::Verbosity_INFO;
    bool              log_no_uptime = false;
    emu->add_flag("--log-to-stderr", log_to_stderr,
        "Send internal logging to stderr (instead of dingusppc.log)");
    emu->add_option("--log-verbosity", log_verbosity,
        "Adjust logging verbosity (default is 0 a.k.a. INFO)")
        ->check(CLI::Number);
    emu->add_flag("--log-no-uptime", log_no_uptime,
        "Disable the uptime preamble of logged messages");

    std::vector<std::string> env_vars;
    emu->add_option("--setenv", env_vars, "Set Open Firmware variables at startup")
        ->take_all();

    uint32_t profiling_interval_ms = 0;
#ifdef CPU_PROFILING
    emu->add_option("--profiling-interval-ms", profiling_interval_ms,
        "Specifies periodic interval (in ms) at which to output CPU profiling information");
#endif

    string       machine_str;
    CLI::Option* machine_opt = emu->add_option("-m,--machine",
        machine_str, "Specify machine ID");

    emu->add_flag("--auto-grab-mouse", g_auto_grab_mouse,
        "The guest cursor causes mouse to be grabbed");
    emu->add_flag("--swap-command-option", g_swap_command_option,
        "Swap the Command and Option keys (physical Alt/AltGr becomes Command)");

    auto list_cmd = app.add_subcommand("list",
        "Display available machine configurations and exit");

    string sub_arg;

    list_cmd->add_option("machines", sub_arg, "List supported machines");
    list_cmd->add_option("properties", sub_arg, "List available properties");

    CLI11_PARSE(app, argc, argv);

    deterministic_interactive = deterministic_mode == "interactive";
    if (g_realtime && is_deterministic && !deterministic_interactive) {
        return app.exit(CLI::ValidationError("--realtime",
            "Use --deterministic-mode=interactive with realtime timing"));
    }

    if (*list_cmd) {
        if (sub_arg == "machines") {
            MachineFactory::list_machines();
        } else if (sub_arg == "properties") {
            MachineFactory::list_properties();
        } else {
            cout << "Unknown list subcommand " << sub_arg << endl;
        }
        return 0;
    }

    if (bootrom_opt->count() == 0) {
        // it was not specified on the command line, so validate the default file name.
        std::string msg = bootrom_opt->get_validator()->operator()(bootrom_path);
        if (!msg.empty()) {
            CLI::ParseError e(msg, CLI::ExitCodes::ValidationError);
            return app.exit(e);
        }
    }

    if (debugger_enter || !debugger_skip) {
        execution_mode = debugger;
    }

    const char* owner_option = std::getenv("SHEEP_CPU_OWNER");
    const bool owner_on = owner_option && std::strcmp(owner_option, "on") == 0;
    if (owner_option && !owner_on && std::strcmp(owner_option, "off") != 0) {
        std::cerr << "SHEEP_CPU_OWNER must be on or off\n";
        return 1;
    }
    if (owner_on && execution_mode != interpreter) {
        std::cerr << "SHEEP_CPU_OWNER=on requires run mode; interactive debugger stays on the serial path\n";
        return 1;
    }
    pippin_cpu_owner::initialize(owner_on);


    if (auto start_time = parse_start_date(start_date, is_deterministic))
        ViaCuda::set_start_time(*start_time);

    /* initialize logging */
    loguru::g_preamble_date    = false;
    loguru::g_preamble_time    = false;
    loguru::g_preamble_thread  = false;
    loguru::g_preamble_uptime  = !log_no_uptime;

    if (execution_mode == interpreter && !log_to_stderr) {
        loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
        loguru::init(argc, argv);
        loguru::add_file("dingusppc.log", loguru::Append, log_verbosity);
    } else {
        loguru::g_stderr_verbosity = log_verbosity;
        loguru::init(argc, argv);
    }

    auto rom_data = std::unique_ptr<char[]>(new char[4 * 1024 * 1024]);
    memset(&rom_data[0], 0, static_cast<size_t>(4 * 1024 * 1024));
    size_t rom_size = MachineFactory::read_boot_rom(bootrom_path, &rom_data[0]);
    if (!rom_size) {
        return 1;
    }

    string machine_str_from_rom = MachineFactory::machine_name_from_rom(&rom_data[0], rom_size);
    if (machine_str_from_rom.empty()) {
        LOG_F(ERROR, "Could not autodetect machine from ROM.");
    } else {
        LOG_F(INFO, "Machine detected from ROM as: %s", machine_str_from_rom.c_str());
    }
    if (*machine_opt) {
        LOG_F(INFO, "Machine option was passed in: %s", machine_str.c_str());
    } else {
        machine_str = machine_str_from_rom;
    }
        if (machine_str.empty()) {
        LOG_F(ERROR, "Must specify a machine or provide a supported ROM.");
            return 1;
    }

    // Hook to allow properties to be read from the command-line, regardless
    // of when they are registered.
    MachineFactory::get_setting_value = [&](const std::string& name) -> std::optional<std::string> {
        CLI::App sa;
        sa.allow_extras();

        std::string value;
        sa.add_option("--" + name, value)->expected(0,1);
        try {
            sa.parse(app.remaining_for_passthrough());
        } catch (const CLI::Error& e) {
            ABORT_F("Cannot parse CLI: %s", e.get_name().c_str());
        }

        if (sa.count("--" + name) > 0) {
            return value;
        } else {
            return std::nullopt;
        }
    };

    if (MachineFactory::register_machine_settings(machine_str) < 0) {
        return 1;
    }

    cout << "BootROM path: " << bootrom_path << endl;
    cout << "Execution mode: " << execution_mode << endl;
    if (g_realtime)
        cout << "Guest timers use the host clock." << endl;
    if (is_deterministic) {
        cout << "Using deterministic execution mode; disk, NVRAM, and PRAM changes will not be saved." << endl;
        if (deterministic_interactive) {
            cout << "Mouse and keyboard input enabled; execution will not be fully deterministic." << endl;
        } else {
            cout << "Mouse and keyboard input will be ignored." << endl;
        }
    }

    if (!init()) {
        LOG_F(ERROR, "Cannot initialize");
        return 1;
    }

    // initialize global profiler object
    gProfilerObj.reset(new Profiler());

    // graceful handling of fatal errors
    loguru::set_fatal_handler([](const loguru::Message& message) {
        // Make sure the reason for the failure is visible (it may have been
        // sent to the logfile only).
        cerr << message.preamble << message.indentation << message.prefix << message.message << endl;
        // In the pilot a fatal error may originate on the worker. Fail the
        // private process instead of entering stdin or deleting live SDL/device
        // state on that thread. Ordinary shutdown still joins and persists.
        if (pippin_cpu_owner::active()) std::_Exit(1);
        set_power_off_reason(po_enter_debugger);
        DppcDebugger::get_instance()->enter_debugger();

        // Ensure that NVRAM and other state is persisted before we terminate.
        delete gMachineObj.release();
    });

    // redirect SIGINT to our own handler
    signal(SIGINT, sigint_handler);

    // redirect SIGABRT to our own handler
    signal(SIGABRT, sigabrt_handler);

    keyboard_id = kbd_map.at(keyboard_string);

    while (true) {
        try {
            run_machine(
                machine_str,
                &rom_data[0],
                rom_size,
                execution_mode,
                env_vars,
                deterministic_interactive,
                profiling_interval_ms);
        } catch (const std::exception& error) {
            std::cerr << "CPU-owner failure: " << error.what() << std::endl;
            TimerManager::get_instance()->cancel_all_timers();
            EventManager::get_instance()->disconnect_handlers();
            delete gMachineObj.release();
            cleanup();
            return 1;
        } catch (...) {
            std::cerr << "CPU-owner failure: unknown exception" << std::endl;
            TimerManager::get_instance()->cancel_all_timers();
            EventManager::get_instance()->disconnect_handlers();
            delete gMachineObj.release();
            cleanup();
            return 1;
        }
        if (power_off_reason == po_restarting) {
            LOG_F(INFO, "Restarting...");
            power_on = true;
            continue;
        }
        if (power_off_reason == po_shutting_down) {
            if (execution_mode != debugger) {
                LOG_F(INFO, "Shutdown.");
                break;
            }
            LOG_F(INFO, "Shutdown...");
            power_on = true;
            continue;
        }
        break;
    }

    // if we didn't delete this then delete it now
    delete gMachineObj.release();

    cleanup();

    return 0;
}

void run_machine(std::string machine_str, char* rom_data,
    size_t rom_size,
    uint32_t execution_mode,
    const std::vector<std::string> &env_vars,
    bool deterministic_interactive,
    uint32_t
#ifdef CPU_PROFILING
     profiling_interval_ms
#endif
) {
    if (g_jit_selftest) {
        // Self-tests use their own code fixtures; the boot ROM only has to let the machine be built.
        setenv("SHEEP_PIPPIN_JIT", "verify", 1);
        setenv("SHEEP_PIPPIN_FASTMEM", "on", 0);
    }

    if (MachineFactory::create_machine_for_id(machine_str, rom_data, rom_size) < 0) {
        return;
    }

    if (g_jit_selftest) {
        bool ok = false;
        try {
            ok = sheep_jit_enabled();
        } catch (const std::exception& e) {
            fprintf(stderr, "PIPPIN JIT SELFTEST error: %s\n", e.what());
        }
        fprintf(stderr, "PIPPIN JIT SELFTEST %s\n", ok ? "PASS" : "FAIL");
        fflush(stderr);
        std::_Exit(ok ? 0 : 1);
    }

    if (!env_vars.empty()) {
        OfConfigUtils ofnvram;
        if (!ofnvram.init()) {
            for (const auto &env_var : env_vars) {
                auto pos = env_var.find('=');
                if (pos != std::string::npos) {
                    std::string name = env_var.substr(0, pos);
                    std::string value = env_var.substr(pos + 1);
                    if (ofnvram.setenv(name, value)) {
                        LOG_F(INFO, "Set Open Firmware variable %s to %s", name.c_str(), value.c_str());
                    } else {
                        LOG_F(WARNING, "Cannot set Open Firmware variable %s to %s", name.c_str(), value.c_str());
                    }
                } else {
                    LOG_F(WARNING, "Invalid format for --setenv: %s", env_var.c_str());
                }
            }
        } else {
            LOG_F(WARNING, "Cannot initialize NVRAM wrapper, will not set Open Firmware variables");
        }
    }

    static TimerInfo deterministic_timer;
    if (is_deterministic && !deterministic_interactive) {
        EventManager::get_instance()->disable_input_handlers();
        // Log the PC and instruction every second to make it easier to validate
        // that execution is the same every time.
        TimerManager::get_instance()->add_cyclic_timer(deterministic_timer, MSECS_TO_NSECS(1000), [](uint64_t, uint64_t) {
            // We may be running after an exceptions or RFI changed the address
            // translation context. In those cases the PC address cannot be
            // translated (it would either be stale or invalid).
            if (exec_flags & (EXEF_EXCEPTION | EXEF_RFI)) {
                LOG_F(INFO, "TS=%016llu PC=0x%08x transition pending to PC=0x%08x",
                      get_virt_time_ns(), ppc_state.pc, ppc_next_instruction_address);
                return;
            }

            PPCDisasmContext ctx;
            ctx.instr_code = ppc_read_instruction(mmu_translate_imem(ppc_state.pc));
            ctx.instr_addr = ppc_state.pc;
            ctx.simplified = false;
            auto op_name = disassemble_single(&ctx);
            LOG_F(INFO, "TS=%016llu PC=0x%08x executing %s", get_virt_time_ns(), ppc_state.pc, op_name.c_str());
        });
    }

    EventManager::get_instance()->set_keyboard_locale(keyboard_id);

    // set up system wide event polling using
    // default Macintosh polling rate of 11 ms
    static TimerInfo event_timer;
    TimerManager::get_instance()->add_cyclic_timer(event_timer, MSECS_TO_NSECS(11), [](uint64_t, uint64_t) {
        EventManager::get_instance()->poll_events();
    });

#ifdef CPU_PROFILING
    uint32_t profiling_timer;
    if (profiling_interval_ms > 0) {
        profiling_timer = TimerManager::get_instance()->add_cyclic_timer(MSECS_TO_NSECS(profiling_interval_ms),
            [](uint64_t, uint64_t)
        {
            gProfilerObj->print_profile("PPC_CPU");
        });
    }
#endif

    if (pippin_cpu_owner::enabled()) {
        // Match the debugger's automatic "go" path without allowing a worker
        // to enter blocking stdin. All guest state below is worker-owned.
        pippin_cpu_owner::run([] {
            set_power_off_reason(po_none);
            power_on = true;
            ppc_exec();
            if (power_off_reason == po_shut_down) set_power_off_reason(po_shutting_down);
            else if (power_off_reason == po_restart) set_power_off_reason(po_restarting);
            else if (power_off_reason == po_quit) set_power_off_reason(po_quitting);
        });
        // A debugger/disassembly request is an explicit ownership transfer:
        // after join, return to the unchanged main-thread serial debugger.
        if (power_off_reason != po_shutting_down && power_off_reason != po_restarting &&
            power_off_reason != po_quitting) {
            LOG_F(INFO, "CPU-owner returning to serial debugger");
            pippin_cpu_owner::initialize(false);
            DppcDebugger::get_instance()->enter_debugger();
        }
    } else switch (execution_mode) {
    case interpreter:
        set_power_off_reason(po_starting_up);
        DppcDebugger::get_instance()->enter_debugger();
        break;
    case threaded_int:
        set_power_off_reason(po_starting_up);
        DppcDebugger::get_instance()->enter_debugger();
        break;
    case debugger:
        set_power_off_reason(po_enter_debugger);
        DppcDebugger::get_instance()->enter_debugger();
        break;
    default:
        LOG_F(ERROR, "Invalid EXECUTION MODE");
        return;
    }

    LOG_F(INFO, "Cleaning up...");
    TimerManager::get_instance()->cancel_timer(event_timer);
#ifdef CPU_PROFILING
    if (profiling_interval_ms > 0) {
        TimerManager::get_instance()->cancel_timer(profiling_timer);
    }
#endif
    if (is_deterministic && !deterministic_interactive) {
        TimerManager::get_instance()->cancel_timer(deterministic_timer);
    }
    TimerManager::get_instance()->cancel_all_timers();
    EventManager::get_instance()->disconnect_handlers();
    delete gMachineObj.release();
}
