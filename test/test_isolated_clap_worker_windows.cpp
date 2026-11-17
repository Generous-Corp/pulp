// SPDX-License-Identifier: MIT
// The Windows isolated CLAP worker driven through the public loader: a real
// lifecycle fixture module, the production worker, the retained job-object
// custody and the shared callback endpoint. Owner-run on the Windows VM.
#include "fixtures/isolated_clap/windows_callback_traps.hpp"
#include "harness/scoped_rt_process_probe.hpp"
#include "isolated_clap_protocol.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/runtime/crypto.hpp>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace pulp;
using namespace host;
using namespace std::chrono_literals;
namespace traps = pulp::test::windows_traps;

namespace {
constexpr const char* kDescriptorPrefix = "dev.pulp.g9.lifecycle.";
// The callback period of the profile (64 frames at 48 kHz).
constexpr auto kBlockPeriod = std::chrono::microseconds(64'000'000 / 48'000);

std::string file_hash(const char* path) {
    return *runtime::sha256_file_hex(path, std::uint64_t{1} << 32);
}
IsolatedClapOptions options(const std::string& mode = "healthy") {
    return {{PULP_ISOLATED_CLAP_FIXTURE, {"clap", kDescriptorPrefix + mode}},
            PULP_CLAP_WORKER_PATH,
            file_hash(PULP_ISOLATED_CLAP_FIXTURE),
            file_hash(PULP_CLAP_WORKER_PATH)};
}

// One import per trap kind. Each must be redirected before any callback is
// measured, so a zero count never comes from an uninstalled trap.
constexpr const char* kRequiredImports[] = {"HeapAlloc",           "AcquireSRWLockExclusive",
                                            "WaitForSingleObject", "PeekNamedPipe",
                                            "CloseHandle",         "OpenProcess"};

// Installs the import-table traps in this process and requires each kind's
// import to be redirected. Catch2 cases run as separate processes under
// ctest, so every measuring case installs them itself.
void require_traps() {
    const auto& redirected = traps::install();
    for (const auto* name : kRequiredImports) {
        INFO("import=" << name);
        REQUIRE(std::find(redirected.begin(), redirected.end(), name) != redirected.end());
    }
}

// Every callback below runs inside both probes: operator new counting and the
// import-table traps for heap, lock, wait, pipe I/O, handle and process calls.
// Constructing a probe installs the traps, outside any measured scope.
struct CallbackProbe {
    unsigned allocations = 0;
    traps::Counts calls;
    CallbackProbe() {
        require_traps();
    }
    void add(const test::ScopedRtProcessProbe& probe, const traps::Scope& scope) {
        allocations += static_cast<unsigned>(probe.allocation_count());
        const auto seen = scope.counts();
        for (std::size_t k = 0; k < seen.calls.size(); ++k)
            calls.calls[k] += seen.calls[k];
    }
    bool clean() const {
        return allocations == 0 && calls.total() == 0;
    }
};

// Runs one callback inside both probes and returns its duration.
template <typename Callback>
std::chrono::nanoseconds measure(CallbackProbe& probe, Callback&& callback) {
    traps::Scope scope;
    test::ScopedRtProcessProbe allocations;
    const auto begin = std::chrono::steady_clock::now();
    callback();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    probe.add(allocations, scope);
    return elapsed;
}

struct Buffers {
    audio::Buffer<float> input{2, 64}, output{2, 64};
    midi::MidiBuffer midi_in, midi_out;
    ParameterEventQueue params;
    CallbackProbe probe;
    std::chrono::nanoseconds worst{0};
    void fill(float scale = 1) {
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n)
                input.channel(c)[n] = scale * static_cast<float>(1 + n + 64 * c) / 256;
    }
    void process(PluginSlot& slot, int frames = 64) {
        auto in = static_cast<const audio::Buffer<float>&>(input).view();
        auto out = output.view();
        const auto elapsed =
            measure(probe, [&] { slot.process(out, in, midi_in, midi_out, params, frames); });
        worst = std::max(worst, elapsed);
    }
    bool silent() const {
        for (std::size_t c = 0; c < 2; ++c)
            for (auto sample : output.channel(c))
                if (sample != 0)
                    return false;
        return true;
    }
};

struct Witness {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("pulp-g9-witness-" + std::to_string(GetCurrentProcessId()));
    Witness() {
        std::filesystem::remove(path);
        _putenv_s("PULP_G9_WITNESS", path.string().c_str());
    }
    ~Witness() {
        _putenv_s("PULP_G9_WITNESS", "");
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::vector<int> pids(std::string_view wanted) const {
        std::ifstream input(path);
        std::string stage;
        int pid, ppid, frames;
        std::vector<int> result;
        while (input >> stage >> pid >> ppid >> frames)
            if (stage == wanted)
                result.push_back(pid);
        return result;
    }
};

bool live_process(int pid) {
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

std::filesystem::path process_image(int pid) {
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return {};
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, image.data(), &size) != FALSE;
    CloseHandle(process);
    if (!ok)
        return {};
    image.resize(size);
    std::error_code ec;
    return std::filesystem::canonical(image, ec);
}

bool kill_process(int pid) {
    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    const bool killed = TerminateProcess(process, 9) != FALSE;
    CloseHandle(process);
    return killed;
}

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

void healthy_render(IsolatedClapResult& result, float gain, int count = 8) {
    REQUIRE(result);
    REQUIRE(result.control->status().worker_pid > 0);
    REQUIRE(result.control->status().worker_pid != static_cast<int>(GetCurrentProcessId()));
    REQUIRE(result.slot->latency_samples() == 64);
    REQUIRE(result.slot->prepare(48000, 64));
    Buffers buffers;
    buffers.fill();
    buffers.process(*result.slot);
    REQUIRE(buffers.silent());
    for (int block = 1; block <= count; ++block) {
        // Functional scheduling slack, not a realtime qualification result.
        std::this_thread::sleep_for(10ms);
        buffers.process(*result.slot);
        INFO("block=" << block << " reason=" << static_cast<int>(result.control->status().reason));
        REQUIRE(result.control->status().reason == IsolationReason::none);
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n)
                REQUIRE(buffers.output.channel(c)[n] == gain * buffers.input.channel(c)[n]);
    }
    INFO("allocations=" << buffers.probe.allocations << " trapped=" << buffers.probe.calls.total());
    REQUIRE(buffers.probe.clean());
}
} // namespace

TEST_CASE("Windows callback traps observe each kind they claim to cover",
          "[host][isolation][windows][negative-control]") {
    // Instrument control: every later zero count is meaningful only if the
    // traps see these calls when they are made on purpose.
    require_traps();
    traps::Counts seen;
    unsigned allocations = 0;
    {
        traps::Scope scope;
        test::ScopedRtProcessProbe probe;
        void* volatile raw = std::malloc(64);
        std::free(raw);
        int* volatile boxed = new int(3);
        delete boxed;
        std::mutex mutex;
        mutex.lock();
        mutex.unlock();
        const HANDLE event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        WaitForSingleObject(event, 0);
        DWORD available = 0;
        PeekNamedPipe(event, nullptr, 0, nullptr, &available, nullptr);
        CloseHandle(event);
        const HANDLE self =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
        if (self != nullptr)
            CloseHandle(self);
        seen = scope.counts();
        allocations = static_cast<unsigned>(probe.allocation_count());
    }
    CHECK(seen[traps::Kind::allocation] >= 1);
    CHECK(seen[traps::Kind::lock] >= 1);
    CHECK(seen[traps::Kind::wait] >= 1);
    CHECK(seen[traps::Kind::io] >= 1);
    CHECK(seen[traps::Kind::handle] >= 2);
    CHECK(seen[traps::Kind::process] >= 1);
    CHECK(allocations >= 1);
}

TEST_CASE("Windows callback probes install the traps in a case of their own",
          "[host][isolation][windows][negative-control]") {
    // Run alone, as ctest runs each case, no other case has installed the
    // traps. A probe must still count deliberate calls in its measured scope.
    CallbackProbe probe;
    measure(probe, [] {
        const HANDLE event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        WaitForSingleObject(event, 0);
        CloseHandle(event);
        const HANDLE self =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
        if (self != nullptr)
            CloseHandle(self);
    });
    CHECK(probe.calls[traps::Kind::wait] >= 1);
    CHECK(probe.calls[traps::Kind::handle] >= 2);
    CHECK(probe.calls[traps::Kind::process] >= 1);
    CHECK_FALSE(probe.clean());
}

TEST_CASE("Windows isolated CLAP real worker adds one block with bound identity",
          "[host][isolation][windows]") {
    Witness witness;
    const auto config = options();
    auto result = load_isolated_clap(config);
    REQUIRE(result);
    const int pid = result.control->status().worker_pid;
    std::error_code ec;
    CHECK(std::filesystem::equivalent(process_image(pid), PULP_CLAP_WORKER_PATH, ec));
    CHECK(result.slot->info().unique_id == config.selection.type.descriptor_id);
    CHECK(result.slot->info().format == PluginFormat::CLAP);
    CHECK(result.control->status().generation != 0);
    healthy_render(result, 0.5f, 32);
    const auto processes = witness.pids("process");
    REQUIRE_FALSE(processes.empty());
    for (int seen : processes)
        REQUIRE(seen == pid);
    std::printf("D8_RECEIPT worker_pid=%d worker_sha256=%s plugin_sha256=%s descriptor=%s "
                "generation=%llu processed_blocks=%zu\n",
                pid, config.worker_sha256.c_str(), config.plugin_sha256.c_str(),
                config.selection.type.descriptor_id.c_str(),
                static_cast<unsigned long long>(result.control->status().generation),
                processes.size());
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
    CHECK_FALSE(live_process(pid));
}

TEST_CASE("Windows isolated CLAP measures exactly one block of latency",
          "[host][isolation][windows]") {
    auto result = load_isolated_clap(options());
    REQUIRE(result);
    REQUIRE(result.slot->prepare(48000, 64));
    Buffers buffers;
    std::vector<float> rendered;
    constexpr int kImpulseBlock = 3;
    constexpr int kImpulseFrame = 17;
    for (int block = 0; block < 8; ++block) {
        for (std::size_t c = 0; c < 2; ++c)
            std::fill_n(buffers.input.channel(c).data(), 64, 0.0f);
        if (block == kImpulseBlock) {
            buffers.input.channel(0)[kImpulseFrame] = 1.0f;
            buffers.input.channel(1)[kImpulseFrame] = -1.0f;
        }
        if (block)
            std::this_thread::sleep_for(10ms);
        buffers.process(*result.slot);
        REQUIRE(result.control->status().reason == IsolationReason::none);
        for (float sample : buffers.output.channel(0))
            rendered.push_back(sample);
    }
    std::vector<std::size_t> nonzero;
    for (std::size_t i = 0; i < rendered.size(); ++i)
        if (rendered[i] != 0)
            nonzero.push_back(i);
    REQUIRE(nonzero.size() == 1);
    const auto measured = static_cast<int>(nonzero[0]) - (kImpulseBlock * 64 + kImpulseFrame);
    std::printf("D8_RECEIPT latency_reported=%d latency_measured=%d impulse_gain=%g\n",
                result.slot->latency_samples(), measured, rendered[nonzero[0]]);
    CHECK(measured == result.slot->latency_samples());
    CHECK(measured == 64);
    CHECK(rendered[nonzero[0]] == 0.5f);
    REQUIRE(buffers.probe.clean());
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Windows isolated CLAP refuses identity mismatches before vendor code",
          "[host][isolation][windows]") {
    auto config = options();
    config.plugin_sha256.assign(64, '0');
    CHECK(load_isolated_clap(config).error == IsolationReason::identity_mismatch);
    config = options();
    config.worker_sha256.assign(64, '0');
    CHECK(load_isolated_clap(config).error == IsolationReason::identity_mismatch);
    config = options();
    config.worker_path = "C:/nonexistent/pulp-clap-worker.exe";
    CHECK(load_isolated_clap(config).error == IsolationReason::worker_missing);
    config = options();
    config.selection.type.descriptor_id = "dev.pulp.g9.lifecycle.not-in-factory";
    CHECK(load_isolated_clap(config).error == IsolationReason::launch_failed);
    {
        // A worker from another build refuses hello before loading the module.
        Witness witness;
        config = options();
        config.worker_path = PULP_MISMATCHED_CLAP_WORKER_PATH;
        config.worker_sha256 = file_hash(PULP_MISMATCHED_CLAP_WORKER_PATH);
        CHECK(load_isolated_clap(config).error == IsolationReason::launch_failed);
        CHECK(witness.pids("load").empty());
    }
    {
        // Positive control for the witness above: the production worker loads.
        Witness witness;
        auto loaded = load_isolated_clap(options());
        REQUIRE(loaded);
        CHECK(witness.pids("load").size() == 1);
        REQUIRE(loaded.control->stop() == IsolationCleanup::complete);
    }
    for (const auto* mode : {"latency", "tail", "mono", "load-abort"}) {
        INFO(mode);
        CHECK_FALSE(load_isolated_clap(options(mode)));
    }
}

TEST_CASE("Windows isolated CLAP malformed and partial completions latch silence",
          "[host][isolation][windows]") {
    struct Expectation {
        const char* mode;
        std::vector<IsolationReason> reasons;
    };
    const Expectation cases[] = {
        {"partial-error", {IsolationReason::plugin_failed}},
        {"nan", {IsolationReason::invalid_completion}},
        {"never-return", {IsolationReason::deadline_missed}},
        {"process-abort", {IsolationReason::worker_exited, IsolationReason::deadline_missed}},
    };
    for (const auto& expected : cases) {
        INFO(expected.mode);
        auto result = load_isolated_clap(options(expected.mode));
        REQUIRE(result);
        REQUIRE(result.slot->prepare(48000, 64));
        Buffers buffers;
        buffers.fill();
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        std::this_thread::sleep_for(40ms);
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        REQUIRE(result.slot->process_status() == PluginSlot::ProcessStatus::ok);
        const auto reason = result.control->status().reason;
        INFO("reason=" << static_cast<int>(reason));
        CHECK(std::find(expected.reasons.begin(), expected.reasons.end(), reason) !=
              expected.reasons.end());
        std::this_thread::sleep_for(40ms);
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        CHECK(buffers.probe.clean());
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
}

TEST_CASE("Windows isolated CLAP stalled worker never stretches the callback",
          "[host][isolation][windows]") {
    auto result = load_isolated_clap(options("overrun"));
    REQUIRE(result);
    REQUIRE(result.slot->prepare(48000, 64));
    Buffers buffers;
    buffers.fill();
    buffers.process(*result.slot);
    std::this_thread::sleep_for(1334us);
    buffers.process(*result.slot);
    REQUIRE(buffers.silent());
    REQUIRE(result.control->status().reason == IsolationReason::deadline_missed);
    for (int block = 0; block < 16; ++block)
        buffers.process(*result.slot);
    REQUIRE(buffers.silent());
    std::printf("D8_RECEIPT stalled_worker_worst_callback_ns=%lld block_period_ns=%lld\n",
                static_cast<long long>(buffers.worst.count()),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(kBlockPeriod).count()));
    CHECK(buffers.worst < kBlockPeriod);
    CHECK(buffers.probe.clean());
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Windows isolated CLAP lifecycle hangs are bounded", "[host][isolation][windows]") {
    auto begin = std::chrono::steady_clock::now();
    CHECK_FALSE(load_isolated_clap(options("load-hang")));
    CHECK(std::chrono::steady_clock::now() - begin < 8s);
    for (const auto* mode : {"activate-abort", "prepare-hang"}) {
        INFO(mode);
        auto result = load_isolated_clap(options(mode));
        REQUIRE(result);
        begin = std::chrono::steady_clock::now();
        CHECK_FALSE(result.slot->prepare(48000, 64));
        CHECK(std::chrono::steady_clock::now() - begin < 8s);
        if (std::string_view(mode) == "prepare-hang")
            CHECK(result.control->status().reason == IsolationReason::lifecycle_timeout);
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
    for (const auto* mode : {"destroy-abort", "destroy-hang"}) {
        INFO(mode);
        auto result = load_isolated_clap(options(mode));
        REQUIRE(result);
        begin = std::chrono::steady_clock::now();
        CHECK(result.control->stop() == IsolationCleanup::complete);
        CHECK(std::chrono::steady_clock::now() - begin < 8s);
    }
}

TEST_CASE("Windows isolated CLAP failed state saves keep no checkpoint",
          "[host][isolation][windows]") {
    for (const auto* mode :
         {"save-oversize", "save-ignore-error", "state-save-abort", "save-hang"}) {
        INFO(mode);
        auto result = load_isolated_clap(options(mode));
        REQUIRE(result);
        CHECK_FALSE(result.control->checkpoint());
        CHECK(result.control->accepted_checkpoint().empty());
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
    // Positive control: a saving plugin checkpoints and recovers into a new
    // generation with its restored gain.
    auto config = options("state");
    auto first = load_isolated_clap(config);
    healthy_render(first, 0.5f);
    REQUIRE(first.control->pause());
    REQUIRE(first.control->checkpoint());
    REQUIRE(first.control->accepted_checkpoint().size() == sizeof(float));
    const auto generation = first.control->status().generation;
    REQUIRE(first.control->stop() == IsolationCleanup::complete);
    auto recovered = recover_isolated_clap(config, *first.control);
    REQUIRE(recovered);
    CHECK(recovered.control->status().generation != generation);
    healthy_render(recovered, 0.75f);
    REQUIRE(recovered.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Windows isolated CLAP cleanup reaches descendants after leader exit",
          "[host][isolation][windows]") {
    for (const auto* mode : {"descendant", "exited-leader"}) {
        INFO(mode);
        Witness witness;
        auto result = load_isolated_clap(options(mode));
        REQUIRE(result);
        REQUIRE(result.slot->prepare(48000, 64));
        Buffers buffers;
        buffers.fill();
        buffers.process(*result.slot);
        REQUIRE(eventually([&] { return !witness.pids("grandchild").empty(); }, 5000ms));
        const auto children = witness.pids("descendant");
        const auto grandchildren = witness.pids("grandchild");
        REQUIRE(children.size() == 1);
        REQUIRE(grandchildren.size() == 1);
        // Positive control for the liveness probe below. An exited leader is
        // seen by the supervisor, whose cleanup may already have ended both.
        if (std::string_view(mode) == "descendant")
            CHECK(live_process(grandchildren[0]));
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        const auto cleanup = result.control->stop();
        const bool survived = live_process(children[0]) || live_process(grandchildren[0]);
        if (cleanup == IsolationCleanup::incomplete)
            result.control->cleanup(std::chrono::steady_clock::now() + 2s);
        std::printf("D8_RECEIPT mode=%s descendant=%d grandchild=%d cleanup=%d survived=%d\n", mode,
                    children[0], grandchildren[0], static_cast<int>(cleanup), survived ? 1 : 0);
        REQUIRE(cleanup == IsolationCleanup::complete);
        REQUIRE_FALSE(survived);
        CHECK(buffers.probe.clean());
    }
}

namespace {
struct SiblingRender {
    std::vector<float> isolated, sibling;
    std::chrono::nanoseconds worst_post_kill_callback{0};
    IsolationReason observed_reason = IsolationReason::none;
    IsolationStatus before_release;
    CallbackProbe probe;
};
constexpr int kSiblingBlocks = 16;
constexpr int kKillAfterBlock = 6;

SiblingRender render_with_sibling(IsolatedClapResult& isolated, bool kill_worker) {
    REQUIRE(isolated);
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "input");
    const auto worker = graph.add_plugin_node(std::move(isolated.slot), 2, 2, "isolated");
    const auto gain = graph.add_gain_node("in-process sibling");
    REQUIRE(graph.set_node_gain(gain, 0.25f));
    const auto output = graph.add_output_node(4, "isolated and sibling");
    for (PortIndex channel = 0; channel < 2; ++channel) {
        REQUIRE(graph.connect(input, channel, worker, channel));
        REQUIRE(graph.connect(input, channel, gain, channel));
        REQUIRE(graph.connect(worker, channel, output, channel));
        REQUIRE(graph.connect(gain, channel, output, channel + 2));
    }
    REQUIRE(graph.prepare(48000, 64));
    REQUIRE(graph.latency_samples() == 64);
    SiblingRender result;
    audio::Buffer<float> source(2, 64), rendered(4, 64);
    for (int block = 0; block < kSiblingBlocks; ++block) {
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n)
                source.channel(c)[n] = static_cast<float>(block + 1 + n + 64 * c) / 256;
        if (block)
            std::this_thread::sleep_for(10ms);
        if (kill_worker && block == kKillAfterBlock) {
            const int pid = isolated.control->status().worker_pid;
            REQUIRE(pid > 0);
            REQUIRE(pid != static_cast<int>(GetCurrentProcessId()));
            REQUIRE(kill_process(pid));
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (isolated.control->status().reason == IsolationReason::none &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            result.observed_reason = isolated.control->status().reason;
        }
        auto in = static_cast<const audio::Buffer<float>&>(source).view();
        auto out = rendered.view();
        const auto elapsed = measure(result.probe, [&] { graph.process(out, in, 64); });
        if (kill_worker && block >= kKillAfterBlock)
            result.worst_post_kill_callback = std::max(result.worst_post_kill_callback, elapsed);
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n) {
                result.isolated.push_back(rendered.channel(c)[n]);
                result.sibling.push_back(rendered.channel(c + 2)[n]);
            }
    }
    result.before_release = isolated.control->status();
    graph.release();
    return result;
}
} // namespace

TEST_CASE("Windows isolated CLAP killed worker keeps in-process sibling bit-exact",
          "[host][isolation][windows]") {
    auto healthy = load_isolated_clap(options());
    const auto reference = render_with_sibling(healthy, false);
    REQUIRE(reference.before_release.reason == IsolationReason::none);
    REQUIRE(healthy.control->stop() == IsolationCleanup::complete);

    auto faulted = load_isolated_clap(options());
    const auto killed = render_with_sibling(faulted, true);
    INFO("reason=" << static_cast<int>(killed.before_release.reason));
    REQUIRE(killed.observed_reason == IsolationReason::worker_exited);
    REQUIRE(killed.before_release.reason == IsolationReason::worker_exited);
    REQUIRE(killed.before_release.lifecycle == IsolationLifecycle::terminal);
    REQUIRE(killed.sibling.size() == reference.sibling.size());
    for (std::size_t i = 0; i < reference.sibling.size(); ++i) {
        INFO("sample=" << i);
        REQUIRE(killed.sibling[i] == reference.sibling[i]);
    }
    const std::size_t kill_sample = static_cast<std::size_t>(kKillAfterBlock) * 128;
    bool reference_nonzero_after_kill = false;
    for (std::size_t i = 0; i < reference.isolated.size(); ++i) {
        INFO("sample=" << i);
        if (i < kill_sample) {
            REQUIRE(killed.isolated[i] == reference.isolated[i]);
        } else {
            REQUIRE(killed.isolated[i] == 0.0f);
            reference_nonzero_after_kill |= reference.isolated[i] != 0.0f;
        }
    }
    REQUIRE(reference_nonzero_after_kill);
    std::printf("D8_RECEIPT sibling_samples=%zu post_kill_worst_callback_ns=%lld "
                "graph_allocations=%u graph_trapped_calls=%u\n",
                killed.sibling.size(),
                static_cast<long long>(killed.worst_post_kill_callback.count()),
                killed.probe.allocations + reference.probe.allocations,
                killed.probe.calls.total() + reference.probe.calls.total());
    CHECK(killed.worst_post_kill_callback < kBlockPeriod);
    CHECK(reference.probe.clean());
    CHECK(killed.probe.clean());
    REQUIRE(faulted.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Windows isolated CLAP paced run at the block period",
          "[host][isolation][windows][.paced]") {
    // Hidden: a timing receipt for a quiet host, not a gate. It paces callbacks
    // at the real block period and reports how many blocks completed in time.
    auto result = load_isolated_clap(options());
    REQUIRE(result);
    REQUIRE(result.slot->prepare(48000, 64));
    Buffers buffers;
    buffers.fill();
    int completed = 0;
    auto next = std::chrono::steady_clock::now();
    for (int block = 0; block < 4000; ++block) {
        while (std::chrono::steady_clock::now() < next) {
        }
        next += kBlockPeriod;
        buffers.process(*result.slot);
        if (result.control->status().reason != IsolationReason::none)
            break;
        ++completed;
    }
    std::printf("D8_RECEIPT paced_blocks_completed=%d of 4000 reason=%d worst_callback_ns=%lld\n",
                completed, static_cast<int>(result.control->status().reason),
                static_cast<long long>(buffers.worst.count()));
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
}
