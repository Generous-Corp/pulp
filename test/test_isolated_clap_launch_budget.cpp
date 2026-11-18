// SPDX-License-Identifier: MIT
// Launch budgets of the isolated CLAP worker, measured against a real worker
// process. The test worker build holds its hello reply for
// PULP_G9_HELLO_DELAY_MS and the fixture plugin holds its load for
// PULP_G9_LOAD_DELAY_MS, so each launch exchange can be made slow on its own.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <libproc.h>
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/runtime/crypto.hpp>
#include <string>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <vector>

using namespace pulp;
using namespace host;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
IsolatedClapOptions delayed_options() {
    return {{PULP_ISOLATED_CLAP_FIXTURE, {"clap", "dev.pulp.g9.lifecycle.healthy"}},
            PULP_DELAYED_CLAP_WORKER_PATH,
            *runtime::sha256_file_hex(PULP_ISOLATED_CLAP_FIXTURE, std::uint64_t{1} << 32),
            *runtime::sha256_file_hex(PULP_DELAYED_CLAP_WORKER_PATH, std::uint64_t{1} << 32)};
}

// Sets the delays and a witness file for the workers this scope launches.
struct Delays {
    std::filesystem::path witness = std::filesystem::temp_directory_path() /
                                    ("pulp-g9-launch-witness-" + std::to_string(::getpid()));
    Delays(int hello_ms, int load_ms) {
        std::filesystem::remove(witness);
        ::setenv("PULP_G9_WITNESS", witness.c_str(), 1);
        ::setenv("PULP_G9_HELLO_DELAY_MS", std::to_string(hello_ms).c_str(), 1);
        ::setenv("PULP_G9_LOAD_DELAY_MS", std::to_string(load_ms).c_str(), 1);
    }
    ~Delays() {
        ::unsetenv("PULP_G9_WITNESS");
        ::unsetenv("PULP_G9_HELLO_DELAY_MS");
        ::unsetenv("PULP_G9_LOAD_DELAY_MS");
        std::filesystem::remove(witness);
    }
    // Workers that reached the vendor load.
    std::vector<int> loaded() const {
        std::ifstream input(witness);
        std::string stage;
        int pid, ppid, frames;
        std::vector<int> result;
        while (input >> stage >> pid >> ppid >> frames)
            if (stage == "load")
                result.push_back(pid);
        return result;
    }
};

bool live_process(int pid) {
    int query[]{CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    if (::sysctl(query, 4, &info, &size, nullptr, 0) != 0)
        return ::kill(pid, 0) == 0;
    return size != 0 && info.kp_proc.p_stat != SZOMB;
}

// Live (non-zombie) direct children of this process.
std::vector<int> live_children() {
    std::vector<pid_t> pids(4096);
    const int bytes = ::proc_listpids(PROC_PPID_ONLY, static_cast<std::uint32_t>(::getpid()),
                                      pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    REQUIRE(bytes >= 0);
    std::vector<int> result;
    for (std::size_t i = 0; i < static_cast<std::size_t>(bytes) / sizeof(pid_t); ++i)
        if (pids[i] > 0 && live_process(pids[i]))
            result.push_back(pids[i]);
    return result;
}

std::vector<int> new_live_children(const std::vector<int>& before) {
    std::vector<int> result;
    for (int pid : live_children())
        if (std::find(before.begin(), before.end(), pid) == before.end())
            result.push_back(pid);
    return result;
}
} // namespace

TEST_CASE("Isolated CLAP worker with a slow hello and a slow load still launches",
          "[host][isolation]") {
    // About 3 s for hello and 3 s for the load report: more than one exchange
    // budget in total, inside each exchange's own budget.
    const auto before = live_children();
    Delays delays(3000, 3000);
    const auto started = Clock::now();
    auto result = load_isolated_clap(delayed_options());
    const auto elapsed = Clock::now() - started;
    INFO("elapsed ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                       << " error=" << static_cast<int>(result.error));
    REQUIRE(result);
    REQUIRE(elapsed > 5s);
    REQUIRE(elapsed < 10s);
    const int worker = result.control->status().worker_pid;
    REQUIRE(delays.loaded() == std::vector<int>{worker});
    // Control for the leak checks below: the child census sees a live worker.
    const auto running = new_live_children(before);
    REQUIRE(std::find(running.begin(), running.end(), worker) != running.end());
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
    REQUIRE(new_live_children(before).empty());
}

TEST_CASE("Isolated CLAP hello past its budget fails launch and leaves no worker",
          "[host][isolation]") {
    const auto before = live_children();
    Delays delays(6000, 0);
    const auto started = Clock::now();
    auto result = load_isolated_clap(delayed_options());
    const auto elapsed = Clock::now() - started;
    INFO("elapsed ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    REQUIRE_FALSE(result);
    REQUIRE(result.error == IsolationReason::launch_failed);
    // The hello budget, not the worker's own 6 s delay, ended the launch, and
    // the worker never reached the vendor load.
    REQUIRE(elapsed >= 5s);
    REQUIRE(elapsed < 5900ms);
    REQUIRE(delays.loaded().empty());
    REQUIRE(new_live_children(before).empty());
}

TEST_CASE("Isolated CLAP load report past its own budget fails launch and leaves no worker",
          "[host][isolation]") {
    // A 3 s hello and then a 6 s load: the load ends at 9 s, inside the 10 s
    // session, so only the load's own budget, counted from hello, refuses it.
    const auto before = live_children();
    Delays delays(3000, 6000);
    const auto started = Clock::now();
    auto result = load_isolated_clap(delayed_options());
    const auto elapsed = Clock::now() - started;
    INFO("elapsed ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    REQUIRE_FALSE(result);
    REQUIRE(result.error == IsolationReason::launch_failed);
    REQUIRE(elapsed >= 8s);
    REQUIRE(elapsed < 8900ms);
    REQUIRE(delays.loaded().size() == 1);
    REQUIRE(new_live_children(before).empty());
}
