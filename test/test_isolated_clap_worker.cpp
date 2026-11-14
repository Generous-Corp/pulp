// SPDX-License-Identifier: MIT
#include "harness/scoped_rt_process_probe.hpp"
#include "isolated_clap_endpoint.hpp"
#include "isolated_recovery_test_hook.hpp"
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <limits>
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/runtime/crypto.hpp>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <thread>
#include <unistd.h>

using namespace pulp;
using namespace host;
using namespace std::chrono_literals;
namespace {
IsolatedClapOptions options(std::string mode = "healthy") {
    return {{PULP_ISOLATED_CLAP_FIXTURE, {"clap", "dev.pulp.g9.lifecycle." + mode}},
            PULP_CLAP_WORKER_PATH,
            *runtime::sha256_file_hex(PULP_ISOLATED_CLAP_FIXTURE, std::uint64_t{1} << 32),
            *runtime::sha256_file_hex(PULP_CLAP_WORKER_PATH, std::uint64_t{1} << 32)};
}
struct Buffers {
    audio::Buffer<float> input{2, 64}, output{2, 64};
    midi::MidiBuffer midi_in, midi_out;
    ParameterEventQueue params;
    void fill(float scale = 1) {
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n)
                input.channel(c)[n] = scale * static_cast<float>(1 + n + 64 * c) / 256;
    }
    void process(PluginSlot& slot, int frames = 64) {
        auto in = static_cast<const audio::Buffer<float>&>(input).view();
        auto out = output.view();
        test::ScopedRtProcessProbe probe;
        slot.process(out, in, midi_in, midi_out, params, frames);
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
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("pulp-g9-witness-" + std::to_string(::getpid()));
    Witness() {
        std::filesystem::remove(path);
        ::setenv("PULP_G9_WITNESS", path.c_str(), 1);
    }
    ~Witness() {
        ::unsetenv("PULP_G9_WITNESS");
        std::filesystem::remove(path);
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
    int query[]{CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    if (::sysctl(query, 4, &info, &size, nullptr, 0) != 0)
        return ::kill(pid, 0) == 0;
    return size != 0 && info.kp_proc.p_stat != SZOMB;
}
void healthy_render(IsolatedClapResult& result, float gain, int count = 8) {
    REQUIRE(result);
    REQUIRE(result.control->status().worker_pid != ::getpid());
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
}
} // namespace

TEST_CASE("Isolated CLAP real healthy worker adds one block", "[host][isolation]") {
    Witness witness;
    auto result = load_isolated_clap(options());
    healthy_render(result, 0.5f, 32);
    const auto processes = witness.pids("process");
    REQUIRE_FALSE(processes.empty());
    for (int pid : processes) {
        REQUIRE(pid == result.control->status().worker_pid);
        REQUIRE(pid != ::getpid());
    }
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP retained group cleans nested descendants after leader exit",
          "[host][isolation]") {
    for (const auto* mode : {"descendant", "exited-leader"}) {
        INFO(mode);
        Witness witness;
        auto result = load_isolated_clap(options(mode));
        REQUIRE(result);
        REQUIRE(result.slot->prepare(48000, 64));
        Buffers buffers;
        buffers.fill();
        buffers.process(*result.slot);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (witness.pids("grandchild").empty() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        const auto children = witness.pids("descendant");
        const auto grandchildren = witness.pids("grandchild");
        REQUIRE(children.size() == 1);
        REQUIRE(grandchildren.size() == 1);
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        const auto cleanup = result.control->stop();
        const bool survived = live_process(children[0]) || live_process(grandchildren[0]);
        // Mutation controls still finish cleanup through the real retained API
        // before asserting, so a broken supervisor cannot leak hostile children.
        if (cleanup == IsolationCleanup::incomplete)
            result.control->cleanup(std::chrono::steady_clock::now() + 2s);
        INFO("live descendant survived disabled cleanup=" << survived);
        REQUIRE(cleanup == IsolationCleanup::complete);
        REQUIRE_FALSE(survived);
    }
}

TEST_CASE("Isolated CLAP stopped state recovers into a new generation", "[host][isolation]") {
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
    REQUIRE(recovered.control->status().generation != generation);
    healthy_render(recovered, 0.75f);
    Buffers stale;
    stale.fill();
    stale.process(*first.slot);
    REQUIRE(stale.silent());
    REQUIRE(first.control->status().reason != IsolationReason::none);
    REQUIRE(recovered.control->stop() == IsolationCleanup::complete);
    config.plugin_sha256[0] = config.plugin_sha256[0] == 'a' ? 'b' : 'a';
    REQUIRE(recover_isolated_clap(config, *first.control).error ==
            IsolationReason::checkpoint_invalid);
}

TEST_CASE("Isolated CLAP rejects partial errors and nonfinite output", "[host][isolation]") {
    for (const auto* mode : {"partial-error", "nan", "process-abort", "never-return"}) {
        INFO(mode);
        auto result = load_isolated_clap(options(mode));
        REQUIRE(result);
        REQUIRE(result.slot->prepare(48000, 64));
        Buffers buffers;
        buffers.fill();
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        std::this_thread::sleep_for(20ms);
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        REQUIRE(result.slot->process_status() == PluginSlot::ProcessStatus::ok);
        REQUIRE(result.control->status().reason != IsolationReason::none);
        std::this_thread::sleep_for(40ms);
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
}

TEST_CASE("Isolated CLAP deadline fault uses paced callback entry", "[host][isolation]") {
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
    std::this_thread::sleep_for(40ms);
    buffers.process(*result.slot);
    REQUIRE(buffers.silent());
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP graph fault preserves hosted and built-in siblings with PDC",
          "[host][isolation]") {
    auto faulty = load_isolated_clap(options("partial-error"));
    auto healthy = load_isolated_clap(options("slow"));
    REQUIRE(faulty);
    REQUIRE(healthy);
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "input");
    const auto broken = graph.add_plugin_node(std::move(faulty.slot), 2, 2, "isolated fault");
    const auto good = graph.add_plugin_node(std::move(healthy.slot), 2, 2, "isolated healthy");
    const auto gain = graph.add_gain_node("built-in gain");
    REQUIRE(graph.set_node_gain(gain, 0.25f));
    const auto output = graph.add_output_node(6, "separate sibling outputs");
    for (PortIndex channel = 0; channel < 2; ++channel) {
        REQUIRE(graph.connect(input, channel, broken, channel));
        REQUIRE(graph.connect(input, channel, good, channel));
        REQUIRE(graph.connect(input, channel, gain, channel));
        REQUIRE(graph.connect(broken, channel, output, channel));
        REQUIRE(graph.connect(good, channel, output, channel + 2));
        REQUIRE(graph.connect(gain, channel, output, channel + 4));
    }
    REQUIRE(graph.prepare(48000, 64));
    REQUIRE(graph.latency_samples() == 64);
    audio::Buffer<float> source(2, 64), rendered(6, 64);
    std::array<float, 128> previous{};
    for (int block = 0; block < 12; ++block) {
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n)
                source.channel(c)[n] = static_cast<float>(block + 1 + n + 64 * c) / 256;
        if (block)
            std::this_thread::sleep_for(10ms);
        auto in = static_cast<const audio::Buffer<float>&>(source).view();
        auto out = rendered.view();
        {
            test::ScopedRtProcessProbe probe;
            graph.process(out, in, 64);
        }
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n) {
                const auto prior = previous[c * 64 + n];
                REQUIRE(rendered.channel(c)[n] == 0);
                REQUIRE(rendered.channel(c + 2)[n] == prior * 0.5f);
                REQUIRE(rendered.channel(c + 4)[n] == prior * 0.25f);
                previous[c * 64 + n] = source.channel(c)[n];
            }
    }
    REQUIRE(faulty.control->status().reason != IsolationReason::none);
    REQUIRE(healthy.control->status().reason == IsolationReason::none);
    graph.release();
    REQUIRE(faulty.control->stop() == IsolationCleanup::complete);
    REQUIRE(healthy.control->stop() == IsolationCleanup::complete);
}

namespace {
// One parallel pair: an isolated worker on outputs 0-1 and an in-process
// built-in gain sibling on outputs 2-3, both fed by the same graph input.
struct SiblingRender {
    std::vector<float> isolated, sibling;
    std::chrono::nanoseconds worst_post_kill_callback{0};
    IsolationReason observed_reason = IsolationReason::none;
    IsolationStatus before_release;
};
constexpr int kSiblingBlocks = 16;
constexpr int kKillAfterBlock = 6;
// The callback period of MacClapStereo64V1 (64 frames at 48 kHz). The guide
// promises a callback without waits, so a fault must never stretch a block.
constexpr auto kBlockPeriod = std::chrono::microseconds(64'000'000 / 48'000);

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
            REQUIRE(pid != ::getpid());
            REQUIRE(::kill(pid, SIGKILL) == 0);
            // Wait for the supervisor rather than racing the callback, so the
            // first terminal reason is the observed worker exit.
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (isolated.control->status().reason == IsolationReason::none &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            result.observed_reason = isolated.control->status().reason;
        }
        auto in = static_cast<const audio::Buffer<float>&>(source).view();
        auto out = rendered.view();
        std::chrono::nanoseconds elapsed{};
        {
            // The trap build aborts on callback allocation, locks, poll,
            // send/recv or kill, so the probe covers the post-fault blocks.
            test::ScopedRtProcessProbe probe;
            const auto begin = std::chrono::steady_clock::now();
            graph.process(out, in, 64);
            elapsed = std::chrono::steady_clock::now() - begin;
        }
        if (kill_worker && block >= kKillAfterBlock)
            result.worst_post_kill_callback = std::max(result.worst_post_kill_callback, elapsed);
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n) {
                result.isolated.push_back(rendered.channel(c)[n]);
                result.sibling.push_back(rendered.channel(c + 2)[n]);
            }
    }
    // Releasing the graph stops a healthy worker, so read the render outcome first.
    result.before_release = isolated.control->status();
    graph.release();
    return result;
}
} // namespace

TEST_CASE("Isolated CLAP killed worker keeps in-process sibling bit-exact", "[host][isolation]") {
    auto healthy = load_isolated_clap(options());
    const auto reference = render_with_sibling(healthy, false);
    REQUIRE(reference.before_release.reason == IsolationReason::none);
    REQUIRE(healthy.control->stop() == IsolationCleanup::complete);

    auto faulted = load_isolated_clap(options());
    const auto killed = render_with_sibling(faulted, true);
    const auto status = killed.before_release;
    INFO("reason=" << static_cast<int>(status.reason));
    REQUIRE(killed.observed_reason == IsolationReason::worker_exited);
    REQUIRE(status.reason == IsolationReason::worker_exited);
    REQUIRE(status.lifecycle == IsolationLifecycle::terminal);
    // The first terminal reason never clears, including after graph release.
    REQUIRE(faulted.control->status().reason == IsolationReason::worker_exited);

    // The sibling path never observes the fault: every sample of every block,
    // before and after the kill, matches the fault-free reference exactly.
    REQUIRE(killed.sibling.size() == reference.sibling.size());
    for (std::size_t i = 0; i < reference.sibling.size(); ++i) {
        INFO("sample=" << i);
        REQUIRE(killed.sibling[i] == reference.sibling[i]);
    }
    // The isolated path matches the reference until the kill and then latches
    // silence instead of holding or repeating its last completion.
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

    INFO("worst post-kill callback ns=" << killed.worst_post_kill_callback.count());
    REQUIRE(killed.worst_post_kill_callback < kBlockPeriod);
    REQUIRE(faulted.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP finite registry reserves before spawn", "[host][isolation]") {
    auto first = load_isolated_clap(options());
    auto second = load_isolated_clap(options());
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(load_isolated_clap(options()).error == IsolationReason::registry_full);
    REQUIRE(first.control->stop() == IsolationCleanup::complete);
    REQUIRE(second.control->stop() == IsolationCleanup::complete);
    auto next = load_isolated_clap(options());
    REQUIRE(next);
    REQUIRE(next.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP two incomplete cleanups retain finite reservations", "[host][isolation]") {
    auto first = load_isolated_clap(options());
    auto second = load_isolated_clap(options());
    REQUIRE(first);
    REQUIRE(second);
    first.control->cleanup(std::chrono::steady_clock::now() - 1ms);
    second.control->cleanup(std::chrono::steady_clock::now() - 1ms);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while ((first.control->status().cleanup == IsolationCleanup::pending ||
            second.control->status().cleanup == IsolationCleanup::pending) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE(first.control->status().cleanup == IsolationCleanup::incomplete);
    REQUIRE(second.control->status().cleanup == IsolationCleanup::incomplete);
    REQUIRE(load_isolated_clap(options()).error == IsolationReason::quarantine_active);
    REQUIRE(first.control->cleanup(std::chrono::steady_clock::now() + 2s) ==
            IsolationCleanup::complete);
    REQUIRE(second.control->cleanup(std::chrono::steady_clock::now() + 2s) ==
            IsolationCleanup::complete);
    auto next = load_isolated_clap(options());
    REQUIRE(next);
    REQUIRE(next.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP raw lengths use explicit padding and drain", "[host][isolation]") {
    for (int length : {1, 63, 64, 65, 128}) {
        INFO(length);
        auto result = load_isolated_clap(options());
        REQUIRE(result);
        REQUIRE(result.slot->prepare(48000, 64));
        Buffers buffers;
        const int blocks = (length + 63) / 64;
        std::vector<float> collected;
        for (int block = 0; block <= blocks; ++block) {
            for (std::size_t c = 0; c < 2; ++c)
                for (int n = 0; n < 64; ++n) {
                    const int index = block * 64 + n;
                    buffers.input.channel(c)[n] =
                        index < length ? static_cast<float>(index + 1) / 256 : 0;
                }
            if (block)
                std::this_thread::sleep_for(10ms);
            buffers.process(*result.slot);
            REQUIRE(result.control->status().reason == IsolationReason::none);
            if (block == 0)
                REQUIRE(buffers.silent());
            else
                for (float sample : buffers.output.channel(0))
                    collected.push_back(sample);
        }
        for (int n = 0; n < blocks * 64; ++n)
            REQUIRE(collected[static_cast<std::size_t>(n)] ==
                    (n < length ? static_cast<float>(n + 1) / 512 : 0));
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
}

TEST_CASE("Isolated CLAP identity and unsupported shapes refuse", "[host][isolation]") {
    auto config = options();
    config.plugin_sha256.assign(64, '0');
    REQUIRE(load_isolated_clap(config).error == IsolationReason::identity_mismatch);
    config = options();
    config.worker_path = "/nonexistent/g9-worker";
    REQUIRE(load_isolated_clap(config).error == IsolationReason::worker_missing);
    config = options();
    config.worker_sha256.assign(64, '0');
    REQUIRE(load_isolated_clap(config).error == IsolationReason::identity_mismatch);
    {
        Witness witness;
        config = options();
        config.worker_path = PULP_MISMATCHED_CLAP_WORKER_PATH;
        config.worker_sha256 =
            *runtime::sha256_file_hex(config.worker_path, std::uint64_t{1} << 32);
        REQUIRE(load_isolated_clap(config).error == IsolationReason::launch_failed);
        REQUIRE(witness.pids("load").empty());
    }
    for (const auto* mode : {"latency", "tail", "mono", "load-abort"}) {
        INFO(mode);
        REQUIRE_FALSE(load_isolated_clap(options(mode)));
    }
    for (int frames : {1, 63, 65, 128}) {
        auto result = load_isolated_clap(options());
        REQUIRE(result);
        REQUIRE_FALSE(result.slot->prepare(48000, frames));
        REQUIRE(result.control->status().reason == IsolationReason::unsupported);
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
    auto result = load_isolated_clap(options());
    REQUIRE(result);
    auto settings = result.slot->host_settings();
    settings.deliver_transport = true;
    REQUIRE_FALSE(result.slot->set_host_settings(settings));
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP capped checkpoint rejects ignored errors", "[host][isolation]") {
    for (const auto* mode :
         {"save-oversize", "save-ignore-error", "state-save-abort", "save-hang"}) {
        INFO(mode);
        auto result = load_isolated_clap(options(mode));
        REQUIRE(result);
        REQUIRE_FALSE(result.control->checkpoint());
        REQUIRE(result.control->accepted_checkpoint().empty());
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
}

TEST_CASE("Isolated CLAP checkpoint rejection preserves accepted state", "[host][isolation]") {
    auto config = options("save-once");
    auto result = load_isolated_clap(config);
    REQUIRE(result);
    REQUIRE(result.control->checkpoint());
    const auto accepted = result.control->accepted_checkpoint();
    REQUIRE_FALSE(result.control->checkpoint());
    REQUIRE(result.control->accepted_checkpoint() == accepted);
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
    auto recovered = recover_isolated_clap(config, *result.control);
    healthy_render(recovered, 0.5f);
    REQUIRE(recovered.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP corrupt and faulting restore cannot recover", "[host][isolation]") {
    for (const auto* mode : {"corrupt-state", "state-load-abort", "restore-hang"}) {
        INFO(mode);
        auto config = options(mode);
        auto first = load_isolated_clap(config);
        REQUIRE(first);
        REQUIRE(first.control->checkpoint());
        REQUIRE(first.control->stop() == IsolationCleanup::complete);
        const auto started = std::chrono::steady_clock::now();
        REQUIRE_FALSE(recover_isolated_clap(config, *first.control));
        REQUIRE(std::chrono::steady_clock::now() - started < 8s);
    }
}

TEST_CASE("Isolated CLAP lifecycle load prepare and destroy are bounded", "[host][isolation]") {
    auto begin = std::chrono::steady_clock::now();
    REQUIRE_FALSE(load_isolated_clap(options("load-hang")));
    REQUIRE(std::chrono::steady_clock::now() - begin < 8s);
    for (const auto* mode : {"activate-abort", "prepare-hang"}) {
        INFO(mode);
        auto result = load_isolated_clap(options(mode));
        REQUIRE(result);
        begin = std::chrono::steady_clock::now();
        REQUIRE_FALSE(result.slot->prepare(48000, 64));
        REQUIRE(std::chrono::steady_clock::now() - begin < 8s);
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
    for (const auto* mode : {"destroy-abort", "destroy-hang"}) {
        INFO(mode);
        auto result = load_isolated_clap(options(mode));
        REQUIRE(result);
        begin = std::chrono::steady_clock::now();
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
        REQUIRE(std::chrono::steady_clock::now() - begin < 8s);
    }
}

TEST_CASE("Isolated CLAP graph policy refuses before and after prepare", "[host][isolation]") {
    for (int mode = 0; mode < 3; ++mode) {
        auto result = load_isolated_clap(options());
        REQUIRE(result);
        SignalGraph graph;
        // Legacy void setters before insertion still express an unsupported request.
        if (mode == 0)
            graph.set_canonical_executor_routing_enabled(false);
        if (mode == 1)
            graph.set_parallel_routing_enabled(true);
        if (mode == 2)
            graph.set_anticipation_enabled(true);
        const auto input = graph.add_input_node(2, "input");
        const auto effect = graph.add_plugin_node(std::move(result.slot), 2, 2, "isolated");
        const auto output = graph.add_output_node(2, "output");
        for (PortIndex c = 0; c < 2; ++c) {
            REQUIRE(graph.connect(input, c, effect, c));
            REQUIRE(graph.connect(effect, c, output, c));
        }
        REQUIRE_FALSE(graph.prepare(48000, 64));
        REQUIRE(result.control->status().reason == IsolationReason::none);
        graph.set_canonical_executor_routing_enabled(true);
        graph.set_parallel_routing_enabled(false);
        graph.set_anticipation_enabled(false);
        REQUIRE_FALSE(graph.prepare(44100, 64));
        REQUIRE_FALSE(graph.prepare(48000, 128));
        REQUIRE(graph.prepare(48000, 64));
        graph.set_canonical_executor_routing_enabled(false);
        graph.set_parallel_routing_enabled(true);
        graph.set_anticipation_enabled(true);
        REQUIRE(graph.canonical_executor_routing_enabled());
        REQUIRE_FALSE(graph.parallel_routing_enabled());
        REQUIRE_FALSE(graph.anticipation_enabled());
        REQUIRE_FALSE(graph.prepare(48000, 64));
        graph.begin_swap_edit();
        REQUIRE(graph.prepare_swap(48000, 64) == SignalGraph::SwapResult::NeedsEagerPrepare);
        REQUIRE(graph.stage_plugin_replacement(effect, {}) ==
                SignalGraph::SwapResult::NeedsEagerPrepare);
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit);
        REQUIRE(edit->prepare(48000, 64) != SignalGraph::PreparedTopologyEdit::Result::Prepared);
        Buffers buffers;
        buffers.fill();
        auto in = static_cast<const audio::Buffer<float>&>(buffers.input).view();
        auto out = buffers.output.view();
        graph.process(out, in, 64);
        REQUIRE(buffers.silent());
        std::this_thread::sleep_for(10ms);
        graph.process(out, in, 64);
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n)
                REQUIRE(buffers.output.channel(c)[n] == buffers.input.channel(c)[n] * 0.5f);
        REQUIRE(result.control->status().reason == IsolationReason::none);
        graph.release();
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
}

TEST_CASE("Isolated CLAP cross-process publication wraps slots with changing audio",
          "[host][isolation]") {
    auto result = load_isolated_clap(options());
    REQUIRE(result);
    REQUIRE(result.slot->prepare(48000, 64));
    Buffers buffers;
    std::array<float, 128> previous{};
    // 96 publications wrap the kSlots=3 ring 32 times and cover every one of
    // the 31 audio scales three times. Each publication is one more chance for
    // a shared CI host to stall the polling worker past the slack below, which
    // ends the stream as deadline_missed (1024 x 20ms failed at 565 and 796).
    for (int block = 0; block < 96; ++block) {
        buffers.fill(static_cast<float>(1 + block % 31) / 32);
        // This checks ownership and sample identity, not a scheduling budget.
        // Keep explicit functional slack on shared CI hosts; paced RT admission
        // requires a separate quiet-host qualification lane.
        if (block)
            std::this_thread::sleep_for(100ms);
        buffers.process(*result.slot);
        INFO("publication=" << block);
        REQUIRE(result.control->status().reason == IsolationReason::none);
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n) {
                REQUIRE(buffers.output.channel(c)[n] == previous[c * 64 + n] * 0.5f);
                previous[c * 64 + n] = buffers.input.channel(c)[n];
            }
    }
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP unsupported proxy operations diagnose terminal silence",
          "[host][isolation]") {
    for (int operation = 0; operation < 7; ++operation) {
        INFO("operation=" << operation);
        auto result = load_isolated_clap(options());
        REQUIRE(result);
        REQUIRE(result.slot->prepare(48000, 64));
        Buffers buffers;
        buffers.fill();
        switch (operation) {
        case 0:
            REQUIRE(result.slot->serialize_dsp_state().empty());
            break;
        case 1:
            REQUIRE_FALSE(result.slot->restore_dsp_state({}));
            break;
        case 2:
            REQUIRE(result.slot->create_editor_view() == nullptr);
            break;
        case 3:
            result.slot->set_parameter(0, 0.5f);
            break;
        case 4:
            result.slot->set_preferred_channel_layout(1, 2);
            break;
        case 5: {
            auto settings = result.slot->host_settings();
            settings.process_mode = format::ProcessMode::Offline;
            REQUIRE_FALSE(result.slot->set_host_settings(settings));
            break;
        }
        case 6: {
            auto settings = result.slot->host_settings();
            settings.prefer_double_precision = true;
            REQUIRE_FALSE(result.slot->set_host_settings(settings));
            break;
        }
        }
        REQUIRE(result.control->status().reason == IsolationReason::unsupported);
        result.slot->begin_process();
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        REQUIRE(result.slot->process_status() == PluginSlot::ProcessStatus::ok);
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
}

TEST_CASE("Isolated CLAP bundle init receives resource root", "[host][isolation]") {
    Witness witness;
    const auto bundle = std::filesystem::path(witness.path.string() + ".clap");
    std::filesystem::create_directories(bundle / "Contents" / "MacOS");
    std::filesystem::create_directories(bundle / "Contents" / "Resources");
    const auto binary = bundle / "Contents" / "MacOS" / bundle.stem();
    std::filesystem::copy_file(PULP_ISOLATED_CLAP_FIXTURE, binary,
                               std::filesystem::copy_options::overwrite_existing);
    std::ofstream(bundle / "Contents" / "Resources" / "required.txt") << "G9 resource\n";
    ::setenv("PULP_G9_REQUIRE_BUNDLE", "1", 1);
    auto config = options();
    config.selection.path = bundle.string();
    auto result = load_isolated_clap(config);
    ::unsetenv("PULP_G9_REQUIRE_BUNDLE");
    // Always clean the test-owned bundle, including a failed admission.
    if (result) {
        REQUIRE(result.slot->info().path == std::filesystem::canonical(bundle).string());
        healthy_render(result, 0.5f);
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
    std::filesystem::remove_all(bundle);
    REQUIRE(result);
}

TEST_CASE("Isolated CLAP graph rejects mono before worker preparation", "[host][isolation]") {
    Witness witness;
    auto result = load_isolated_clap(options());
    REQUIRE(result);
    SignalGraph graph;
    graph.add_plugin_node(std::move(result.slot), 1, 1, "unsupported mono");
    REQUIRE_FALSE(graph.prepare(48000, 64));
    REQUIRE(witness.pids("prepare").empty());
    REQUIRE(result.control->status().reason == IsolationReason::none);
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP refuses SysEx and UMP sidecars", "[host][isolation]") {
    for (int sidecar = 0; sidecar < 4; ++sidecar) {
        auto result = load_isolated_clap(options());
        REQUIRE(result);
        REQUIRE(result.slot->prepare(48000, 64));
        Buffers buffers;
        buffers.fill();
        midi::UmpBuffer ump;
        auto& events = sidecar < 2 ? buffers.midi_in : buffers.midi_out;
        if (sidecar % 2) {
            ump.add(midi::UmpPacket::note_on_2(0, 0, 60, 0x8000), 0);
            events.attach_ump(&ump);
        } else {
            const std::uint8_t bytes[]{0xf0, 0x7d, 0x01, 0xf7};
            REQUIRE(events.add_sysex_copy(bytes, sizeof(bytes), 0));
        }
        REQUIRE(buffers.midi_in.empty());
        buffers.process(*result.slot);
        REQUIRE(buffers.silent());
        REQUIRE(result.control->status().reason == IsolationReason::unsupported);
        REQUIRE(result.control->stop() == IsolationCleanup::complete);
    }
    auto result = load_isolated_clap(options("sysex-output"));
    REQUIRE(result);
    REQUIRE(result.slot->prepare(48000, 64));
    Buffers buffers;
    buffers.fill();
    buffers.process(*result.slot);
    std::this_thread::sleep_for(10ms);
    buffers.process(*result.slot);
    REQUIRE(buffers.silent());
    REQUIRE(result.control->status().reason == IsolationReason::plugin_failed);
    REQUIRE(result.control->stop() == IsolationCleanup::complete);
}

namespace {
// A prepared 48k/64 graph with one isolated node under test (outputs 0-1), an
// in-process gain sibling (outputs 2-3) and, optionally, a healthy isolated
// sibling (outputs 4-5). Every path carries the isolated node's 64-sample PDC,
// so each output is a fixed gain applied to the previous block's input.
struct RecoveryRig {
    SignalGraph graph;
    NodeId node = 0, gain = 0;
    bool hosted_sibling = false;
    audio::Buffer<float> source{2, 64}, rendered{6, 64};
    std::array<float, 128> previous{};
    int block = 0;

    RecoveryRig(std::unique_ptr<PluginSlot> isolated, std::unique_ptr<PluginSlot> sibling = {}) {
        hosted_sibling = sibling != nullptr;
        const auto input = graph.add_input_node(2, "input");
        node = graph.add_plugin_node(std::move(isolated), 2, 2, "recoverable");
        gain = graph.add_gain_node("built-in sibling");
        REQUIRE(graph.set_node_gain(gain, 0.25f));
        const auto other =
            hosted_sibling ? graph.add_plugin_node(std::move(sibling), 2, 2, "hosted sibling") : 0;
        const auto output = graph.add_output_node(6, "outputs");
        for (PortIndex c = 0; c < 2; ++c) {
            REQUIRE(graph.connect(input, c, node, c));
            REQUIRE(graph.connect(input, c, gain, c));
            REQUIRE(graph.connect(node, c, output, c));
            REQUIRE(graph.connect(gain, c, output, c + 2));
            if (hosted_sibling) {
                REQUIRE(graph.connect(input, c, other, c));
                REQUIRE(graph.connect(other, c, output, c + 4));
            }
        }
        REQUIRE(graph.prepare(48000, 64));
        REQUIRE(graph.latency_samples() == 64);
    }
    // Renders one paced block and returns the node's gain: 0 for silence, the
    // exact multiplier otherwise, or -1 when the node output is neither.
    // Siblings are checked bit-exactly on every block.
    float step() {
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n)
                source.channel(c)[n] = static_cast<float>(block + 1 + n + 64 * c) / 256;
        if (block++)
            std::this_thread::sleep_for(10ms);
        auto in = static_cast<const audio::Buffer<float>&>(source).view();
        auto out = rendered.view();
        {
            test::ScopedRtProcessProbe probe;
            graph.process(out, in, 64);
        }
        bool silent = true;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n) {
                const auto prior = previous[c * 64 + n];
                INFO("block=" << block << " channel=" << c << " sample=" << n);
                REQUIRE(rendered.channel(c + 2)[n] == prior * 0.25f);
                if (hosted_sibling)
                    REQUIRE(rendered.channel(c + 4)[n] == prior * 0.5f);
                silent = silent && rendered.channel(c)[n] == 0.0f;
            }
        float observed = silent ? 0.0f : -1.0f;
        for (float candidate : {0.5f, 0.75f}) {
            bool match = !silent;
            for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t n = 0; n < 64; ++n)
                    match = match && rendered.channel(c)[n] == previous[c * 64 + n] * candidate;
            if (match)
                observed = candidate;
        }
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n)
                previous[c * 64 + n] = source.channel(c)[n];
        return observed;
    }
    const PluginSlot* published() const {
        return graph.live_plugin_slot(node);
    }
};

IsolationReason kill_worker(IsolatedClapControl& control) {
    const int pid = control.status().worker_pid;
    REQUIRE(pid > 0);
    REQUIRE(pid != ::getpid());
    REQUIRE(::kill(pid, SIGKILL) == 0);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (control.status().reason == IsolationReason::none &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return control.status().reason;
}
} // namespace

TEST_CASE("Isolated CLAP terminal graph node recovers from its checkpoint", "[host][isolation]") {
    // The state fixture saves gain 0.75 while rendering at 0.5, so recovered
    // output proves the checkpoint (not a fresh default instance) was restored.
    auto state = load_isolated_clap(options("state"));
    auto healthy = load_isolated_clap(options());
    REQUIRE(state);
    REQUIRE(healthy);
    REQUIRE(state.control->checkpoint());
    const auto checkpoint = state.control->accepted_checkpoint();
    REQUIRE(checkpoint.size() == sizeof(float));
    const auto first_generation = state.control->status().generation;
    const auto first_pid = state.control->status().worker_pid;
    RecoveryRig rig(std::move(state.slot), std::move(healthy.slot));
    REQUIRE(rig.step() == 0.0f);
    for (int block = 0; block < 5; ++block)
        REQUIRE(rig.step() == 0.5f);

    REQUIRE(kill_worker(*state.control) == IsolationReason::worker_exited);
    const auto* terminal = rig.published();
    for (int block = 0; block < 4; ++block)
        REQUIRE(rig.step() == 0.0f);

    auto recovered =
        recover_isolated_graph_node(rig.graph, rig.node, options("state"), *state.control);
    INFO("outcome=" << static_cast<int>(recovered.outcome)
                    << " worker_error=" << static_cast<int>(recovered.worker_error));
    REQUIRE(recovered);
    REQUIRE(recovered.control->status().reason == IsolationReason::none);
    REQUIRE(recovered.control->status().generation != first_generation);
    REQUIRE(recovered.control->status().worker_pid != first_pid);
    REQUIRE(recovered.control->accepted_checkpoint() == checkpoint);
    REQUIRE(rig.published() != terminal);
    REQUIRE(rig.graph.last_swap_diagnostics().reason == LiveSwapFallbackReason::None);
    // The old generation stays terminal; the new one renders one block of
    // latency silence, then the checkpointed gain.
    REQUIRE(state.control->status().reason == IsolationReason::worker_exited);
    REQUIRE(rig.step() == 0.0f);
    for (int block = 0; block < 6; ++block)
        REQUIRE(rig.step() == 0.75f);
    REQUIRE(recovered.control->status().reason == IsolationReason::none);
    REQUIRE(healthy.control->status().reason == IsolationReason::none);
    rig.graph.release();
    REQUIRE(recovered.control->stop() == IsolationCleanup::complete);
    REQUIRE(healthy.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP graph recovery refusals keep the silent node published",
          "[host][isolation]") {
    auto state = load_isolated_clap(options("state"));
    REQUIRE(state);
    REQUIRE(state.control->checkpoint());
    RecoveryRig rig(std::move(state.slot));
    REQUIRE(rig.step() == 0.0f);
    REQUIRE(rig.step() == 0.5f);
    const auto* original = rig.published();

    REQUIRE(recover_isolated_graph_node(rig.graph, rig.gain, options("state"), *state.control)
                .outcome == IsolatedNodeRecovery::not_isolated_node);
    {
        auto other = load_isolated_clap(options());
        REQUIRE(other);
        REQUIRE(
            recover_isolated_graph_node(rig.graph, rig.node, options(), *other.control).outcome ==
            IsolatedNodeRecovery::control_mismatch);
        REQUIRE(other.control->stop() == IsolationCleanup::complete);
    }
    // A healthy node is never preempted, and it keeps rendering afterwards.
    REQUIRE(recover_isolated_graph_node(rig.graph, rig.node, options("state"), *state.control)
                .outcome == IsolatedNodeRecovery::not_terminal);
    REQUIRE(state.control->status().reason == IsolationReason::none);
    {
        // The graph-level guard refuses a same-identity proxy over a healthy node.
        auto spare = load_isolated_clap(options("state"));
        REQUIRE(spare);
        REQUIRE(rig.graph.publish_isolated_recovery(rig.node, *original, std::move(spare.slot)) ==
                SignalGraph::SwapResult::NeedsEagerPrepare);
        REQUIRE(rig.graph.last_swap_diagnostics().message == "isolated node is not terminal");
        REQUIRE(spare.control->stop() == IsolationCleanup::complete);
    }
    REQUIRE(rig.step() == 0.5f);
    REQUIRE(rig.published() == original);

    REQUIRE(kill_worker(*state.control) == IsolationReason::worker_exited);
    REQUIRE(rig.step() == 0.0f);
    REQUIRE(recover_isolated_graph_node(rig.graph, rig.node, options(), *state.control).outcome ==
            IsolatedNodeRecovery::identity_mismatch);
    auto foreign = options("state");
    foreign.plugin_sha256[0] = foreign.plugin_sha256[0] == 'a' ? 'b' : 'a';
    REQUIRE(recover_isolated_graph_node(rig.graph, rig.node, foreign, *state.control).outcome ==
            IsolatedNodeRecovery::identity_mismatch);
    auto broken_worker = options("state");
    broken_worker.worker_path = PULP_MISMATCHED_CLAP_WORKER_PATH;
    broken_worker.worker_sha256 =
        *runtime::sha256_file_hex(broken_worker.worker_path, std::uint64_t{1} << 32);
    const auto failed =
        recover_isolated_graph_node(rig.graph, rig.node, broken_worker, *state.control);
    REQUIRE(failed.outcome == IsolatedNodeRecovery::worker_failed);
    REQUIRE(failed.worker_error == IsolationReason::launch_failed);
    REQUIRE(failed.control == nullptr);
    // Graph-level guard: a different-identity proxy is refused before prepare,
    // and the refusal does not invalidate the live snapshot.
    {
        auto other = load_isolated_clap(options());
        REQUIRE(other);
        REQUIRE(rig.graph.publish_isolated_recovery(rig.node, *original, std::move(other.slot)) ==
                SignalGraph::SwapResult::NeedsEagerPrepare);
        REQUIRE(rig.graph.last_swap_diagnostics().reason ==
                LiveSwapFallbackReason::UntrustedIdentity);
        REQUIRE(other.control->stop() == IsolationCleanup::complete);
    }
    for (int block = 0; block < 3; ++block)
        REQUIRE(rig.step() == 0.0f);
    REQUIRE(rig.published() == original);
    REQUIRE(state.control->status().reason == IsolationReason::worker_exited);

    // Refusals left the node recoverable with the correct identity.
    auto recovered =
        recover_isolated_graph_node(rig.graph, rig.node, options("state"), *state.control);
    REQUIRE(recovered);
    REQUIRE(rig.step() == 0.0f);
    REQUIRE(rig.step() == 0.75f);
    rig.graph.release();
    REQUIRE(recovered.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP graph recovery without a checkpoint refuses", "[host][isolation]") {
    auto plain = load_isolated_clap(options());
    REQUIRE(plain);
    RecoveryRig rig(std::move(plain.slot));
    REQUIRE(rig.step() == 0.0f);
    REQUIRE(rig.step() == 0.5f);
    const auto* original = rig.published();
    REQUIRE(kill_worker(*plain.control) == IsolationReason::worker_exited);
    REQUIRE(recover_isolated_graph_node(rig.graph, rig.node, options(), *plain.control).outcome ==
            IsolatedNodeRecovery::no_checkpoint);
    REQUIRE(rig.step() == 0.0f);
    REQUIRE(rig.published() == original);
    rig.graph.release();
    REQUIRE(plain.control->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP graph recovery races a running callback", "[host][isolation]") {
    auto state = load_isolated_clap(options("state"));
    REQUIRE(state);
    REQUIRE(state.control->checkpoint());
    RecoveryRig rig(std::move(state.slot));
    REQUIRE(rig.step() == 0.0f);
    REQUIRE(rig.step() == 0.5f);

    // Catch2 assertions are not thread-safe, so the callback thread only
    // records outcomes; RecoveryRig::step() is not used off the test thread.
    std::atomic<bool> done{false};
    std::atomic<int> sibling_mismatches{0}, invalid_node_blocks{0}, blocks{0};
    std::atomic<int> recovered_blocks{0};
    std::thread callback([&] {
        audio::Buffer<float> source(2, 64), rendered(6, 64);
        std::array<float, 128> previous{};
        for (std::size_t i = 0; i < previous.size(); ++i)
            previous[i] = rig.previous[i];
        // Bounded: about 8 s of paced 64-frame callbacks at most.
        for (int block = 0; block < 800 && !done.load(std::memory_order_acquire); ++block) {
            for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t n = 0; n < 64; ++n)
                    source.channel(c)[n] = static_cast<float>(block + 7 + n + 64 * c) / 256;
            std::this_thread::sleep_for(10ms);
            auto in = static_cast<const audio::Buffer<float>&>(source).view();
            auto out = rendered.view();
            {
                test::ScopedRtProcessProbe probe;
                rig.graph.process(out, in, 64);
            }
            bool silent = true, half = true, recovered = true;
            for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t n = 0; n < 64; ++n) {
                    const auto prior = previous[c * 64 + n];
                    if (rendered.channel(c + 2)[n] != prior * 0.25f)
                        sibling_mismatches.fetch_add(1, std::memory_order_relaxed);
                    const auto value = rendered.channel(c)[n];
                    silent = silent && value == 0.0f;
                    half = half && value == prior * 0.5f;
                    recovered = recovered && value == prior * 0.75f;
                    previous[c * 64 + n] = source.channel(c)[n];
                }
            if (!silent && !half && !recovered)
                invalid_node_blocks.fetch_add(1, std::memory_order_relaxed);
            if (recovered && !silent)
                recovered_blocks.fetch_add(1, std::memory_order_relaxed);
            blocks.fetch_add(1, std::memory_order_release);
        }
    });

    // Repeated kill and recovery while the callback keeps running. Each
    // recovered generation inherits the checkpoint it was restored from.
    std::shared_ptr<IsolatedClapControl> current = state.control;
    std::vector<std::shared_ptr<IsolatedClapControl>> retired;
    bool all_recovered = true;
    for (int cycle = 0; cycle < 3 && all_recovered; ++cycle) {
        const int before = recovered_blocks.load(std::memory_order_acquire);
        const auto wait_until = std::chrono::steady_clock::now() + 3s;
        while (cycle && recovered_blocks.load(std::memory_order_acquire) < before + 3 &&
               std::chrono::steady_clock::now() < wait_until)
            std::this_thread::sleep_for(1ms);
        const int pid = current->status().worker_pid;
        all_recovered = pid > 0 && ::kill(pid, SIGKILL) == 0;
        const auto fault_deadline = std::chrono::steady_clock::now() + 2s;
        while (current->status().reason == IsolationReason::none &&
               std::chrono::steady_clock::now() < fault_deadline)
            std::this_thread::sleep_for(1ms);
        auto recovered =
            recover_isolated_graph_node(rig.graph, rig.node, options("state"), *current);
        all_recovered = all_recovered && recovered;
        retired.push_back(current);
        if (recovered)
            current = recovered.control;
    }
    const int settled = recovered_blocks.load(std::memory_order_acquire);
    const auto wait_until = std::chrono::steady_clock::now() + 3s;
    while (recovered_blocks.load(std::memory_order_acquire) < settled + 3 &&
           std::chrono::steady_clock::now() < wait_until)
        std::this_thread::sleep_for(1ms);
    done.store(true, std::memory_order_release);
    callback.join();

    INFO("blocks=" << blocks.load() << " recovered_blocks=" << recovered_blocks.load());
    REQUIRE(all_recovered);
    REQUIRE(sibling_mismatches.load() == 0);
    REQUIRE(invalid_node_blocks.load() == 0);
    REQUIRE(recovered_blocks.load() >= 9);
    REQUIRE(current->status().reason == IsolationReason::none);
    for (const auto& control : retired)
        REQUIRE(control->status().lifecycle == IsolationLifecycle::terminal);
    rig.graph.release();
    REQUIRE(current->stop() == IsolationCleanup::complete);
}

TEST_CASE("Isolated CLAP concurrent recoveries of one node publish exactly once",
          "[host][isolation]") {
    // Two control threads race to recover the same terminal node. Both usually
    // pass the checks and spawn a worker before either publishes, so the loser
    // is refused at publication and must stop its own worker; a leaked
    // reservation would leave the next load refused.
    bool saw_publication_refused = false;
    for (int round = 0; round < 6 && !saw_publication_refused; ++round) {
        INFO("round=" << round);
        auto state = load_isolated_clap(options("state"));
        REQUIRE(state);
        REQUIRE(state.control->checkpoint());
        RecoveryRig rig(std::move(state.slot));
        REQUIRE(rig.step() == 0.0f);
        REQUIRE(rig.step() == 0.5f);
        REQUIRE(kill_worker(*state.control) == IsolationReason::worker_exited);

        std::atomic<int> ready{0};
        std::array<IsolatedNodeRecoveryResult, 2> results;
        auto recover = [&](std::size_t index) {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < 2)
                std::this_thread::yield();
            results[index] =
                recover_isolated_graph_node(rig.graph, rig.node, options("state"), *state.control);
        };
        std::thread first(recover, 0), second(recover, 1);
        first.join();
        second.join();

        const int winners = (results[0] ? 1 : 0) + (results[1] ? 1 : 0);
        REQUIRE(winners == 1);
        const auto& winner = results[0] ? results[0] : results[1];
        const auto& loser = results[0] ? results[1] : results[0];
        INFO("loser outcome=" << static_cast<int>(loser.outcome));
        REQUIRE((loser.outcome == IsolatedNodeRecovery::control_mismatch ||
                 loser.outcome == IsolatedNodeRecovery::publication_refused));
        // A loser that stopped its worker cleanly hands back no control.
        REQUIRE(loser.control == nullptr);
        saw_publication_refused |= loser.outcome == IsolatedNodeRecovery::publication_refused;

        // Immediately after the race: the winner holds one reservation, and a
        // loser without a control must already have released its own
        // synchronously rather than leaving it to its supervisor.
        auto next = load_isolated_clap(options());
        INFO("next load error=" << static_cast<int>(next.error));
        REQUIRE(next);
        REQUIRE(next.control->stop() == IsolationCleanup::complete);
        REQUIRE(rig.step() == 0.0f);
        REQUIRE(rig.step() == 0.75f);
        rig.graph.release();
        REQUIRE(winner.control->stop() == IsolationCleanup::complete);
    }
    REQUIRE(saw_publication_refused);
}

TEST_CASE("Isolated CLAP graph recovery races release on another control thread",
          "[host][isolation]") {
    // Recovery reads the published proxy under the graph lock and shares its
    // ownership, so a concurrent release() can only turn recovery into a typed
    // refusal, never a dangling or torn snapshot read.
    for (int round = 0; round < 6; ++round) {
        INFO("round=" << round);
        auto state = load_isolated_clap(options("state"));
        REQUIRE(state);
        REQUIRE(state.control->checkpoint());
        RecoveryRig rig(std::move(state.slot));
        REQUIRE(rig.step() == 0.0f);
        REQUIRE(kill_worker(*state.control) == IsolationReason::worker_exited);

        std::atomic<bool> released{false};
        std::thread releaser([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(round * 15));
            rig.graph.release();
            released.store(true, std::memory_order_release);
        });
        IsolatedNodeRecoveryResult result;
        // Bounded: recover until it succeeds or the graph no longer publishes.
        for (int attempt = 0; attempt < 50; ++attempt) {
            result =
                recover_isolated_graph_node(rig.graph, rig.node, options("state"), *state.control);
            if (result || result.outcome != IsolatedNodeRecovery::publication_refused)
                break;
        }
        releaser.join();
        INFO("outcome=" << static_cast<int>(result.outcome));
        REQUIRE((result.outcome == IsolatedNodeRecovery::recovered ||
                 result.outcome == IsolatedNodeRecovery::not_isolated_node ||
                 result.outcome == IsolatedNodeRecovery::publication_refused));
        REQUIRE((result.outcome == IsolatedNodeRecovery::recovered) == (result.control != nullptr));
        REQUIRE(released.load(std::memory_order_acquire));
        // Released graphs refuse a second isolated prepare instead of reviving.
        REQUIRE_FALSE(rig.graph.prepare(48000, 64));
        if (result)
            REQUIRE(result.control->stop() == IsolationCleanup::complete);
        REQUIRE(state.control->stop() == IsolationCleanup::complete);
        // No reservation leaked from any interleaving.
        auto next = load_isolated_clap(options());
        REQUIRE(next);
        REQUIRE(next.control->stop() == IsolationCleanup::complete);
    }
}

TEST_CASE("Isolated CLAP graph recovery lookup races release without a torn read",
          "[host][isolation]") {
    // A tight loop of cheap refusals (the control does not own the node) keeps
    // the published-proxy lookup running while another control thread
    // releases the graph. The lookup must take the graph lock; an unlocked
    // read races release() and is reported by ThreadSanitizer.
    auto state = load_isolated_clap(options("state"));
    auto other = load_isolated_clap(options());
    REQUIRE(state);
    REQUIRE(other);
    RecoveryRig rig(std::move(state.slot));
    REQUIRE(rig.step() == 0.0f);
    std::atomic<bool> released{false};
    std::atomic<int> calls{0}, unexpected{0};
    const auto foreign_options = options();
    std::thread spinner([&] {
        // Bounded: stops a little after release, or after 200000 calls.
        int after_release = 0;
        for (int i = 0; i < 200000 && after_release < 1000; ++i) {
            const auto outcome =
                recover_isolated_graph_node(rig.graph, rig.node, foreign_options, *other.control)
                    .outcome;
            if (outcome != IsolatedNodeRecovery::control_mismatch &&
                outcome != IsolatedNodeRecovery::not_isolated_node)
                unexpected.fetch_add(1, std::memory_order_relaxed);
            calls.fetch_add(1, std::memory_order_relaxed);
            if (released.load(std::memory_order_acquire))
                ++after_release;
        }
    });
    const auto started = std::chrono::steady_clock::now() + 2s;
    while (calls.load(std::memory_order_relaxed) < 100 &&
           std::chrono::steady_clock::now() < started)
        std::this_thread::yield();
    rig.graph.release();
    released.store(true, std::memory_order_release);
    spinner.join();
    INFO("calls=" << calls.load());
    REQUIRE(unexpected.load() == 0);
    REQUIRE(rig.published() == nullptr);
    REQUIRE(state.control->stop() == IsolationCleanup::complete);
    REQUIRE(other.control->stop() == IsolationCleanup::complete);
}

namespace {
// Installs the post-prepare recovery hook for one scope and always clears it.
struct PreparedHook {
    PreparedHook(detail::IsolatedRecoveryPreparedHook hook, void* context) {
        detail::set_isolated_recovery_prepared_hook_for_test(hook, context);
    }
    ~PreparedHook() {
        detail::set_isolated_recovery_prepared_hook_for_test(nullptr, nullptr);
    }
};
} // namespace

TEST_CASE("Isolated CLAP recovery refuses a worker that exits after prepare", "[host][isolation]") {
    auto state = load_isolated_clap(options("state"));
    REQUIRE(state);
    REQUIRE(state.control->checkpoint());
    RecoveryRig rig(std::move(state.slot));
    REQUIRE(rig.step() == 0.0f);
    REQUIRE(rig.step() == 0.5f);
    REQUIRE(kill_worker(*state.control) == IsolationReason::worker_exited);
    const auto terminal = rig.graph.published_plugin_slot(rig.node);
    REQUIRE(terminal);
    REQUIRE(state.control->cleanup(std::chrono::steady_clock::now() + 2s) ==
            IsolationCleanup::complete);

    // The fresh worker prepares, then exits inside the window between the
    // unlocked prepare and the locked revalidation.
    auto fresh = recover_isolated_clap(options("state"), *state.control);
    REQUIRE(fresh);
    struct Context {
        IsolatedClapControl* control;
        IsolationReason observed = IsolationReason::none;
    } context{fresh.control.get()};
    {
        PreparedHook hook(
            [](PluginSlot& replacement, void* raw) {
                auto& ctx = *static_cast<Context*>(raw);
                ctx.observed = kill_worker(*ctx.control);
                (void)replacement;
            },
            &context);
        REQUIRE(rig.graph.publish_isolated_recovery(rig.node, *terminal, std::move(fresh.slot)) ==
                SignalGraph::SwapResult::NeedsEagerPrepare);
    }
    REQUIRE(context.observed == IsolationReason::worker_exited);
    REQUIRE(rig.graph.last_swap_diagnostics().reason == LiveSwapFallbackReason::PrepareFailed);
    REQUIRE(rig.graph.published_plugin_slot(rig.node) == terminal);
    REQUIRE(rig.step() == 0.0f);
    REQUIRE(fresh.control->stop() == IsolationCleanup::complete);

    // Through the orchestrator the same window reports publication_refused,
    // stops the fresh worker, and leaves the node recoverable.
    {
        PreparedHook hook([](PluginSlot& replacement, void*) { replacement.release(); }, nullptr);
        const auto refused =
            recover_isolated_graph_node(rig.graph, rig.node, options("state"), *state.control);
        REQUIRE(refused.outcome == IsolatedNodeRecovery::publication_refused);
        REQUIRE(refused.control == nullptr);
    }
    REQUIRE(rig.graph.last_swap_diagnostics().reason == LiveSwapFallbackReason::PrepareFailed);
    REQUIRE(rig.graph.published_plugin_slot(rig.node) == terminal);
    auto next = load_isolated_clap(options());
    REQUIRE(next);
    REQUIRE(next.control->stop() == IsolationCleanup::complete);

    auto recovered =
        recover_isolated_graph_node(rig.graph, rig.node, options("state"), *state.control);
    REQUIRE(recovered);
    REQUIRE(rig.step() == 0.0f);
    REQUIRE(rig.step() == 0.75f);
    rig.graph.release();
    REQUIRE(recovered.control->stop() == IsolationCleanup::complete);
}
