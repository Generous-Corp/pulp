// Headless tests for the GPU fault-continuity test plug-in: the callback path
// the REAPER gpu-fault smoke relies on (staging, PDC alignment, capture,
// fault arming), driven directly with varying callback sizes.

#include "gpu_fault_continuity.hpp"
#include "gpu_fault_stimulus.hpp"

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/buffer.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using pulp::examples::gpu_fault::GpuFaultContinuityProcessor;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kFrames = 9600;
constexpr int kMaxBlock = 512;

struct RunDir {
    fs::path path;
    RunDir() {
        path = fs::temp_directory_path() /
               ("pulp-gpu-fault-test-" + std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
        ::setenv("PULP_GPU_FAULT_RUN_DIR", path.c_str(), 1);
        ::setenv("PULP_GPU_FAULT_IR", (path / "ir.f32").c_str(), 1);
        ::setenv("PULP_GPU_FAULT_REQUEST", (path / "fault-request.json").c_str(), 1);
    }
    ~RunDir() {
        ::unsetenv("PULP_GPU_FAULT_RUN_DIR");
        ::unsetenv("PULP_GPU_FAULT_IR");
        ::unsetenv("PULP_GPU_FAULT_REQUEST");
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<float> make_ir() {
    std::vector<float> ir(64);
    for (std::size_t k = 0; k < ir.size(); ++k)
        ir[k] = static_cast<float>(std::exp(-0.08 * static_cast<double>(k)) *
                                   (k % 3 == 0 ? 1.0 : -0.5));
    return ir;
}

std::vector<float> make_input() {
    std::vector<float> x(kFrames);
    std::uint32_t state = 0x5EED;
    for (auto& v : x) {
        state = 1664525u * state + 1013904223u;
        v = 0.4f * (static_cast<float>(state) / 4294967296.0f * 2.0f - 1.0f);
    }
    return x;
}

void write_ir(const fs::path& dir, const std::vector<float>& ir) {
    std::ofstream out(dir / "ir.f32", std::ios::binary);
    out.write(reinterpret_cast<const char*>(ir.data()),
              static_cast<std::streamsize>(ir.size() * sizeof(float)));
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool wait_for(const fs::path& events, const std::string& needle) {
    for (int i = 0; i < 500; ++i) {
        if (read_text(events).find(needle) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    return false;
}

struct Run {
    std::vector<float> output; // channel 0
    std::size_t allocations = 0;
};

// Drive the processor through `x` with a rotating set of callback sizes, as a
// host would, playing from project sample 0.
Run drive(GpuFaultContinuityProcessor& p, const std::vector<float>& x, bool paced = false) {
    static constexpr int kSizes[] = {37, 128, 256, 511, 64, 1};
    Run run;
    run.output.reserve(x.size());
    std::vector<float> in_l(kMaxBlock), in_r(kMaxBlock), out_l(kMaxBlock), out_r(kMaxBlock);
    pulp::midi::MidiBuffer midi_in, midi_out;
    std::size_t pos = 0;
    for (int call = 0; pos < x.size(); ++call) {
        const auto n = std::min<std::size_t>(kSizes[call % 6], x.size() - pos);
        std::copy_n(x.data() + pos, n, in_l.data());
        std::copy_n(x.data() + pos, n, in_r.data());
        const float* ins[] = {in_l.data(), in_r.data()};
        float* outs[] = {out_l.data(), out_r.data()};
        const pulp::audio::BufferView<const float> input(ins, 2, n);
        pulp::audio::BufferView<float> output(outs, 2, n);
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = kRate;
        ctx.num_samples = static_cast<int>(n);
        ctx.is_playing = true;
        ctx.position_samples = static_cast<std::int64_t>(pos);
        {
            pulp::test::RtAllocationProbe probe;
            p.process(output, input, midi_in, midi_out, ctx);
            run.allocations += probe.allocation_count();
        }
        run.output.insert(run.output.end(), out_l.begin(), out_l.begin() + n);
        pos += n;
        if (paced) // the callback's own period, as a realtime host would call it
            std::this_thread::sleep_for(std::chrono::microseconds(n * 1'000'000 / kRate));
    }
    return run;
}

std::vector<double> reference(const std::vector<float>& x, const std::vector<float>& ir,
                              std::size_t delay) {
    std::vector<double> y(x.size(), 0.0);
    for (std::size_t n = delay; n < x.size(); ++n)
        for (std::size_t k = 0; k < ir.size() && k <= n - delay; ++k)
            y[n] += static_cast<double>(ir[k]) * x[n - delay - k];
    return y;
}

double max_error(const std::vector<float>& got, const std::vector<double>& want) {
    double worst = 0.0;
    for (std::size_t i = 0; i < got.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(got[i]) - want[i]));
    return worst;
}

// gpu_blocks of the delivery counters in the health event tagged `phase`.
long long gpu_blocks_in(const std::string& text, const std::string& phase) {
    const auto at = text.find(phase);
    if (at == std::string::npos)
        return -1;
    const auto key = text.find("\"gpu_blocks\":", at);
    return key == std::string::npos ? -1 : std::atoll(text.c_str() + key + 13);
}

pulp::format::PrepareContext prepare_context() {
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = kRate;
    ctx.max_buffer_size = kMaxBlock;
    return ctx;
}

bool on_shared_path(const fs::path& events) {
    return read_text(events).find("\"path\":\"shared_memory\"") != std::string::npos;
}

} // namespace

TEST_CASE("gpu fault plug-in output is conv(input) delayed by its reported PDC",
          "[gpu-fault-continuity]") {
    RunDir dir;
    const auto ir = make_ir();
    write_ir(dir.path, ir);
    const auto x = make_input();
    Run run;
    int pdc = 0;
    std::uint32_t quantum = 0;
    {
        GpuFaultContinuityProcessor p;
        p.prepare(prepare_context());
        REQUIRE(wait_for(dir.path / "events.jsonl", "\"event\":\"prepared\""));
        if (!on_shared_path(dir.path / "events.jsonl"))
            SKIP("no shared-I/O GPU provider on this host");
        const auto prepared = read_text(dir.path / "events.jsonl");
        CHECK(prepared.find("\"node\":\"convolver\"") != std::string::npos);
        CHECK(prepared.find("\"storage\":\"shared\"") != std::string::npos);
        CHECK(prepared.find("\"timestamps\":false") != std::string::npos);
        pdc = p.latency_samples();
        quantum = p.quantum();
        // Paced like a realtime host, so the GPU has time to deliver within
        // its lead and the null below covers GPU-delivered blocks, not only
        // the CPU fallback.
        run = drive(p, x, /*paced=*/true);
        const auto d = p.delivery();
        INFO("gpu_blocks=" << d.gpu_blocks << " cpu_fallback_blocks=" << d.cpu_fallback_blocks
                           << " priming_blocks=" << d.priming_blocks);
        CHECK(d.gpu_blocks + d.cpu_fallback_blocks + d.priming_blocks == x.size() / quantum);
        WARN("delivery: gpu_blocks=" << d.gpu_blocks
                                     << " cpu_fallback_blocks=" << d.cpu_fallback_blocks);
    }
    // The callback copies into a ring and the GPU transport; it never allocates.
    CHECK(run.allocations == 0);
    // Positive control: the reported PDC aligns the output with the reference.
    CHECK(max_error(run.output, reference(x, ir, static_cast<std::size_t>(pdc))) < 1e-4);
    // Negative control: one quantum off is far outside tolerance, so the check
    // above can tell an aligned stream from a misaligned one.
    CHECK(max_error(run.output, reference(x, ir, static_cast<std::size_t>(pdc) + quantum)) > 1e-2);

    // The capture holds every callback sample, in order, input and output.
    const auto blob = read_text(dir.path / "capture.bin");
    REQUIRE(blob.size() > 20);
    CHECK(blob.compare(0, 4, "PGFC") == 0);
    std::uint32_t version = 0;
    std::memcpy(&version, blob.data() + 4, 4);
    CHECK(version == 2);
    // v2 record header: position, frames, flags, host time, then cumulative
    // gpu / fallback / silence / priming delivery counters.
    constexpr std::size_t kHeader = 56;
    std::size_t at = 20, frames = 0, records = 0;
    std::uint64_t last_gpu = 0, last_time = 0;
    bool outputs_match = true, counters_monotonic = true;
    while (at + kHeader <= blob.size()) {
        std::uint64_t position = 0, host_time = 0, gpu = 0;
        std::uint32_t n = 0, flags = 0;
        std::memcpy(&position, blob.data() + at, 8);
        std::memcpy(&n, blob.data() + at + 8, 4);
        std::memcpy(&flags, blob.data() + at + 12, 4);
        std::memcpy(&host_time, blob.data() + at + 16, 8);
        std::memcpy(&gpu, blob.data() + at + 24, 8);
        counters_monotonic = counters_monotonic && gpu >= last_gpu && host_time >= last_time;
        last_gpu = gpu;
        last_time = host_time;
        at += kHeader;
        CHECK(position == frames);
        CHECK((flags & 1u) == 1u);
        CHECK(((flags & 8u) != 0) == (records == 0));
        const char* out_l = blob.data() + at + 2 * n * sizeof(float);
        for (std::uint32_t i = 0; i < n && frames + i < run.output.size(); ++i) {
            float v = 0.0f;
            std::memcpy(&v, out_l + i * sizeof(float), sizeof(float));
            outputs_match = outputs_match && v == run.output[frames + i];
        }
        at += 4u * n * sizeof(float);
        frames += n;
        ++records;
    }
    CHECK(at == blob.size());
    CHECK(frames == x.size());
    CHECK(outputs_match);
    CHECK(counters_monotonic);
    CHECK(last_gpu > 0); // paced, so the GPU delivered and the record says so
}

TEST_CASE("gpu fault plug-in arms a fault only when it can inject it", "[gpu-fault-continuity]") {
    RunDir dir;
    write_ir(dir.path, make_ir());
    const auto events = dir.path / "events.jsonl";

    SECTION("render_force_loss fires at the requested project sample through the editor hook") {
        std::ofstream(dir.path / "fault-request.json")
            << R"({"fault": "render_force_loss", "at_project_sample": 4000,)"
               R"( "after_project_sample": 9000})";
        GpuFaultContinuityProcessor p;
        std::atomic<int> hook_calls{0};
        p.set_render_loss_hook([&] { ++hook_calls; });
        p.prepare(prepare_context());
        REQUIRE(wait_for(events, "\"event\":\"fault_armed\""));
        drive(p, make_input());
        REQUIRE(wait_for(events, "\"event\":\"fault_fired\""));
        const auto text = read_text(events);
        CHECK(text.find("\"at_project_sample\":4000") != std::string::npos);
        CHECK(text.find("\"phase\":\"before\"") != std::string::npos);
        CHECK(hook_calls.load() == 1);
        CHECK(p.render_loss_requested());
        p.set_render_loss_hook(nullptr);
    }

    SECTION("render_force_loss with no editor open is refused, not reported as fired") {
        std::ofstream(dir.path / "fault-request.json")
            << R"({"fault": "render_force_loss", "at_project_sample": 4000})";
        GpuFaultContinuityProcessor p;
        p.prepare(prepare_context());
        REQUIRE(wait_for(events, "\"event\":\"fault_armed\""));
        drive(p, make_input());
        REQUIRE(wait_for(events, "no editor is open"));
        CHECK(read_text(events).find("\"event\":\"fault_fired\"") == std::string::npos);
        CHECK_FALSE(p.render_loss_requested());
    }

    SECTION("an unknown fault is refused, never faked") {
        std::ofstream(dir.path / "fault-request.json")
            << R"({"fault": "melt_the_gpu", "at_project_sample": 4000})";
        GpuFaultContinuityProcessor p;
        p.prepare(prepare_context());
        REQUIRE(wait_for(events, "\"event\":\"fault_refused\""));
        drive(p, make_input());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto text = read_text(events);
        CHECK(text.find("\"event\":\"fault_armed\"") == std::string::npos);
        CHECK(text.find("\"event\":\"fault_fired\"") == std::string::npos);
    }
}

TEST_CASE("provider faults fire in the running GPU path and audio stays continuous",
          "[gpu-fault-continuity]") {
    struct Case {
        const char* fault;
        const char* point;
        bool loss;
        bool reprepare = false;
    };
    const Case cases[] = {
        {"force_loss_before", "before_submit", true},
        {"force_loss_between", "between_submit_and_completion_registration", true},
        {"force_loss_after", "after_completion_registration", true},
        {"delay_completion", "process_events_stall", false},
        {"worker_stall", "process_events_stall", false},
        {"missing_completion", "completion_callback_swallowed", false},
        // A host that prepares again (the VST3 adapter does, repeatedly) gets
        // a new device; the fault must hit that one, never the freed one.
        {"force_loss_after", "after_completion_registration", true, true},
    };
    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.fault << (c.reprepare ? " after a re-prepare" : "")) {
            RunDir dir;
            const auto ir = make_ir();
            write_ir(dir.path, ir);
            const auto events = dir.path / "events.jsonl";
            std::ofstream(dir.path / "fault-request.json")
                << "{\"fault\": \"" << c.fault
                << "\", \"at_project_sample\": 4000, \"after_project_sample\": 9000,"
                   " \"delay_ms\": 300}";
            const auto x = make_input();
            Run run;
            int pdc = 0;
            {
                GpuFaultContinuityProcessor p;
                p.prepare(prepare_context());
                REQUIRE(wait_for(events, "\"event\":\"prepared\""));
                if (!on_shared_path(events))
                    SKIP("no shared-I/O GPU provider on this host");
                if (c.reprepare) {
                    p.release();
                    p.prepare(prepare_context());
                    REQUIRE(wait_for(events, "\"prepare_index\":2"));
                }
                REQUIRE(wait_for(events, "\"event\":\"fault_armed\""));
                pdc = p.latency_samples();
                run = drive(p, x, /*paced=*/true);
                REQUIRE(wait_for(events, "\"event\":\"provider_fault_applied\""));
                REQUIRE(wait_for(events, "\"phase\":\"after\""));
            }
            const auto text = read_text(events);
            INFO(text);
            CHECK(text.find(std::string("\"point\":\"") + c.point + "\"") != std::string::npos);
            if (c.loss) {
                // A lost device delivers nothing more: the GPU count is frozen
                // between the "at" and "after" snapshots. (The transport does
                // not yet name the loss in recovery_reason for this node;
                // WP-A makes that truthful, so it is reported, not required.)
                const auto at = gpu_blocks_in(text, "\"phase\":\"at\"");
                const auto after = gpu_blocks_in(text, "\"phase\":\"after\"");
                CHECK(at >= 0);
                CHECK(after == at);
                WARN(c.fault << " recovery_reason after: "
                             << (text.find("\"recovery_reason\":\"provider-lost\"") !=
                                         std::string::npos
                                     ? "provider-lost"
                                     : "not reported"));
            }
            CHECK(run.allocations == 0);
            // The fault happened inside the provider; the output did not notice.
            CHECK(max_error(run.output, reference(x, ir, static_cast<std::size_t>(pdc))) < 1e-4);
        }
    }
}

TEST_CASE("only the first instance to prepare owns the run directory", "[gpu-fault-continuity]") {
    RunDir dir;
    write_ir(dir.path, make_ir());
    GpuFaultContinuityProcessor first;
    first.prepare(prepare_context());
    REQUIRE(wait_for(dir.path / "events.jsonl", "\"event\":\"prepared\""));
    const auto before = read_text(dir.path / "events.jsonl");
    GpuFaultContinuityProcessor second;
    second.prepare(prepare_context());
    drive(second, make_input());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // The second instance wrote nothing: one prepared event, and the capture
    // is still the bare header because only `second` processed audio.
    const auto after = read_text(dir.path / "events.jsonl");
    CHECK(after == before);
    CHECK(fs::file_size(dir.path / "capture.bin") == 20);
}

namespace {

struct RequestRun {
    std::string events;
    std::string capture;
    Run run;
    int pdc = 0;
    bool shared = false;
};

RequestRun run_request(const std::string& request) {
    RunDir dir;
    write_ir(dir.path, make_ir());
    const auto events = dir.path / "events.jsonl";
    std::ofstream(dir.path / "fault-request.json") << request;
    RequestRun out;
    {
        GpuFaultContinuityProcessor p;
        p.prepare(prepare_context());
        REQUIRE(wait_for(events, "\"event\":\"prepared\""));
        out.shared = on_shared_path(events);
        if (!out.shared)
            return out;
        wait_for(events, "\"event\":\"fault_armed\"");
        out.pdc = p.latency_samples();
        out.run = drive(p, make_input(), /*paced=*/true);
        wait_for(events, "\"phase\":\"after\"");
    }
    out.events = read_text(events);
    out.capture = read_text(dir.path / "capture.bin");
    return out;
}

} // namespace

TEST_CASE("planted negatives keep audio flowing but break the null", "[gpu-fault-continuity]") {
    const auto x = make_input();
    const auto ir = make_ir();
    for (const char* planted : {"no_fallback", "fallback_misaligned", "fallback_stale"}) {
        DYNAMIC_SECTION(planted) {
            const auto r =
                run_request(std::string(R"({"fault": "force_loss_after",)") +
                            R"( "at_project_sample": 4000, "after_project_sample": 9000,)" +
                            R"( "planted_negative": ")" + planted + R"("})");
            if (!r.shared)
                SKIP("no shared-I/O GPU provider on this host");
            INFO(r.events);
            CHECK(r.events.find(std::string("\"planted_negative\":\"") + planted + "\"") !=
                  std::string::npos);
            CHECK(r.run.allocations == 0);
            // The planted shape must be caught: the output no longer nulls...
            const auto want = reference(x, ir, static_cast<std::size_t>(r.pdc));
            CHECK(max_error(r.run.output, want) > 1e-2);
            // ...and never before the fault sample (4000): a planted deviation
            // that started early would not prove the null caught the fault.
            std::size_t first = r.run.output.size();
            for (std::size_t i = 0; i < r.run.output.size(); ++i) {
                if (std::abs(static_cast<double>(r.run.output[i]) - want[i]) > 1e-4) {
                    first = i;
                    break;
                }
            }
            CHECK(first >= 4000u);
        }
    }
}

TEST_CASE("the positive control fires and faults nothing", "[gpu-fault-continuity]") {
    const auto r = run_request(R"({"fault": "positive_control", "at_project_sample": 4000,)"
                               R"( "after_project_sample": 9000})");
    if (!r.shared)
        SKIP("no shared-I/O GPU provider on this host");
    INFO(r.events);
    CHECK(r.events.find("\"event\":\"fault_fired\",\"fault\":\"positive_control\"") !=
          std::string::npos);
    CHECK(r.events.find("\"event\":\"provider_fault_applied\"") == std::string::npos);
    CHECK(max_error(r.run.output,
                    reference(make_input(), make_ir(), static_cast<std::size_t>(r.pdc))) < 1e-4);
}

TEST_CASE("an internal stimulus never reads or stores host input", "[gpu-fault-continuity]") {
    ::setenv("PULP_GPU_FAULT_INPUT", "internal", 1);
    const auto r = run_request(R"({"fault": "positive_control", "at_project_sample": 4000,)"
                               R"( "after_project_sample": 9000})");
    ::unsetenv("PULP_GPU_FAULT_INPUT");
    if (!r.shared)
        SKIP("no shared-I/O GPU provider on this host");
    CHECK(r.events.find("\"input\":\"internal\"") != std::string::npos);
    // Every captured input sample, on every channel, is the internal stimulus
    // at its project position (or silence while stopped), never the host's.
    constexpr std::size_t kHeader = 56;
    std::size_t at = 20, checked = 0, mismatched = 0;
    while (at + kHeader <= r.capture.size()) {
        std::uint64_t position = 0;
        std::uint32_t n = 0, flags = 0;
        std::memcpy(&position, r.capture.data() + at, 8);
        std::memcpy(&n, r.capture.data() + at + 8, 4);
        std::memcpy(&flags, r.capture.data() + at + 12, 4);
        at += kHeader;
        const bool playing = (flags & 1u) != 0;
        for (std::uint32_t ch = 0; ch < 2; ++ch) {
            for (std::uint32_t i = 0; i < n; ++i) {
                float v = 0.0f;
                std::memcpy(&v, r.capture.data() + at + (ch * n + i) * sizeof(float),
                            sizeof(float));
                const float want =
                    playing ? pulp::examples::gpu_fault::internal_stimulus(position + i, kRate)
                            : 0.0f;
                mismatched += v != want;
                ++checked;
            }
        }
        at += 4u * n * sizeof(float);
    }
    CHECK(checked > 0);
    CHECK(mismatched == 0);
    // And the output is the convolution of that stimulus, not of the host input.
    CHECK(max_error(r.run.output,
                    reference(make_input(), make_ir(), static_cast<std::size_t>(r.pdc))) > 1e-2);
}

TEST_CASE("gpu_off puts the plug-in on the staged path", "[gpu-fault-continuity]") {
    ::setenv("PULP_GPU_FAULT_PROVIDER", "staged", 1);
    RunDir dir;
    write_ir(dir.path, make_ir());
    {
        GpuFaultContinuityProcessor p;
        p.prepare(prepare_context());
        REQUIRE(wait_for(dir.path / "events.jsonl", "\"event\":\"prepared\""));
    }
    ::unsetenv("PULP_GPU_FAULT_PROVIDER");
    CHECK_FALSE(on_shared_path(dir.path / "events.jsonl"));
    const auto prepared = read_text(dir.path / "events.jsonl");
    CHECK(prepared.find("\"storage\":\"shared\"") == std::string::npos);
    CHECK(prepared.find("\"node\":\"convolver\"") != std::string::npos);
}

TEST_CASE("run-directory files are not followed through a symlink", "[gpu-fault-continuity]") {
    RunDir dir;
    write_ir(dir.path, make_ir());
    const auto events = dir.path / "events.jsonl";
    // The request the harness would write, but reached through a planted link.
    const auto real = dir.path / "elsewhere.json";
    std::ofstream(real) << R"({"fault": "positive_control", "at_project_sample": 4000})";
    fs::create_symlink(real, dir.path / "fault-request.json");
    GpuFaultContinuityProcessor p;
    p.prepare(prepare_context());
    REQUIRE(wait_for(events, "\"event\":\"prepared\""));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(read_text(events).find("\"event\":\"fault_armed\"") == std::string::npos);
    // Positive control: the same request as a regular file arms.
    fs::remove(dir.path / "fault-request.json");
    fs::copy_file(real, dir.path / "fault-request.json");
    CHECK(wait_for(events, "\"event\":\"fault_armed\""));
}
