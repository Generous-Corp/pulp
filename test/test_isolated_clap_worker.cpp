// SPDX-License-Identifier: MIT
#include "harness/scoped_rt_process_probe.hpp"
#include "isolated_clap_endpoint.hpp"
#include <array>
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
    for (int block = 0; block < 1024; ++block) {
        buffers.fill(static_cast<float>(1 + block % 31) / 32);
        // This checks ownership and sample identity, not a scheduling budget.
        // Keep explicit functional slack on shared CI hosts; paced RT admission
        // requires a separate quiet-host qualification lane.
        if (block)
            std::this_thread::sleep_for(20ms);
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
