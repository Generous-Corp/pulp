// test_audio_probe.cpp — RT-probe acceptance.
//
// GATE 2: an allocation-counting test (RtAllocationProbe) asserting ZERO
// allocations across N analyze_output() calls after prepare(). Plus correctness
// for peak/RMS/clip/NaN/silence counting, snapshot sequence monotonicity +
// stale/drop detection, TripleBuffer publish/read coherence, the capture
// SpscRingIndex's drop accounting, AudioStats device-counter mirroring, and the
// complaint-oriented boundary report.

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/audio_boundary_report.hpp>
#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/audio_probe_json.hpp>
#include <pulp/audio/audio_probe_snapshot.hpp>
#include <pulp/audio/audio_scope.hpp>
#include <pulp/audio/audio_scope_json.hpp>
#include <pulp/audio/audio_stats.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <choc/text/choc_JSON.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace pulp::audio;

namespace {

// A fixed-capacity stereo block backed by stable storage. Channel pointers are
// allocated ONCE so building the view never allocates on the audio path.
struct StereoBlock {
    std::vector<float> left;
    std::vector<float> right;
    std::vector<const float*> ptrs;

    explicit StereoBlock(std::size_t frames)
        : left(frames, 0.0f), right(frames, 0.0f) {
        ptrs = {left.data(), right.data()};
    }

    BufferView<const float> view() const {
        return BufferView<const float>(ptrs.data(), 2, left.size());
    }

    void fill(float l, float r) {
        std::fill(left.begin(), left.end(), l);
        std::fill(right.begin(), right.end(), r);
    }
};

struct MonoView {
    std::vector<float> samples;
    std::vector<const float*> ptrs;

    explicit MonoView(std::vector<float> in)
        : samples(std::move(in)) {
        ptrs = {samples.data()};
    }

    BufferView<const float> view() const {
        return BufferView<const float>(ptrs.data(), 1, samples.size());
    }
};

struct MultiView {
    std::vector<std::vector<float>> channels;
    std::vector<const float*> ptrs;

    explicit MultiView(std::vector<std::vector<float>> in)
        : channels(std::move(in)) {
        for (const auto& channel : channels)
            ptrs.push_back(channel.data());
    }

    BufferView<const float> view() const {
        return BufferView<const float>(ptrs.data(), ptrs.size(),
                                       channels.empty() ? 0 : channels.front().size());
    }
};

std::vector<float> sine_wave(double hz, double sample_rate, int samples) {
    std::vector<float> out(static_cast<std::size_t>(samples), 0.0f);
    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i < samples; ++i) {
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(std::sin(2.0 * kPi * hz
                                        * static_cast<double>(i) / sample_rate));
    }
    return out;
}

}  // namespace

TEST_CASE("AudioProbe::analyze_output does not allocate after prepare",
          "[rt-safety][audio-probe][issue-290]") {
    AudioProbe probe;
    probe.prepare(/*max_channels=*/2, /*max_frames=*/256, /*sample_rate=*/48000.0);

    constexpr std::size_t kFrames = 256;
    StereoBlock block(kFrames);
    // A non-trivial signal so every counting branch runs: clipping, a NaN, and
    // ordinary samples — exercising the widest RT path inside the no-alloc scope.
    block.fill(0.5f, -0.5f);
    block.left[10] = 2.0f;   // clip
    block.right[20] = std::numeric_limits<float>::quiet_NaN();  // nan

    const auto v = block.view();

    std::size_t allocation_count = 0;
    std::size_t allocated_bytes = 0;
    {
        pulp::test::RtAllocationProbe alloc_probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (int i = 0; i < 1000; ++i) {
            probe.analyze_output(v);
        }
        allocation_count = alloc_probe.allocation_count();
        allocated_bytes = alloc_probe.allocated_bytes();
    }

    REQUIRE(allocation_count == 0);
    REQUIRE(allocated_bytes == 0);
}

TEST_CASE("AudioProbe::analyze_output with capture ring does not allocate",
          "[rt-safety][audio-probe][issue-290]") {
    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = 1024;
    probe.prepare(2, 256, 48000.0, AudioProbeStage::kStandaloneOutputBoundary, cap);
    REQUIRE(probe.capture_enabled());

    StereoBlock block(256);
    block.fill(0.25f, 0.25f);
    const auto v = block.view();

    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe alloc_probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (int i = 0; i < 500; ++i) probe.analyze_output(v);
        allocation_count = alloc_probe.allocation_count();
    }
    REQUIRE(allocation_count == 0);
}

TEST_CASE("AudioProbe counts peak, RMS, clip, NaN, silence correctly",
          "[audio-probe]") {
    AudioProbe probe;
    probe.prepare(2, 64, 48000.0);

    SECTION("known DC level → exact peak and RMS") {
        StereoBlock block(64);
        block.fill(0.5f, -0.25f);
        probe.analyze_output(block.view());
        const auto snap = probe.latest();
        REQUIRE(snap.channel_count == 2);
        REQUIRE(snap.block_size == 64);
        REQUIRE_THAT(snap.peak[0], WithinAbs(0.5f, 1e-6f));
        REQUIRE_THAT(snap.peak[1], WithinAbs(0.25f, 1e-6f));
        REQUIRE_THAT(snap.rms[0], WithinAbs(0.5f, 1e-5f));
        REQUIRE_THAT(snap.rms[1], WithinAbs(0.25f, 1e-5f));
        REQUIRE_THAT(snap.peak_max, WithinAbs(0.5f, 1e-6f));
        REQUIRE(snap.clip_count == 0);
        REQUIRE(snap.nan_inf_count == 0);
        REQUIRE(snap.silence_run_blocks == 0);  // 0.5 > silence threshold
    }

    SECTION("clipping is counted per-sample, not per-block") {
        StereoBlock block(64);
        block.fill(0.1f, 0.1f);
        block.left[0] = 1.5f;   // clip
        block.left[1] = -2.0f;  // clip
        probe.analyze_output(block.view());
        const auto snap = probe.latest();
        REQUIRE(snap.clip_count == 2);
    }

    SECTION("NaN and Inf are counted and excluded from peak/RMS") {
        StereoBlock block(64);
        block.fill(0.0f, 0.0f);
        block.left[0] = std::numeric_limits<float>::quiet_NaN();
        block.left[1] = std::numeric_limits<float>::infinity();
        probe.analyze_output(block.view());
        const auto snap = probe.latest();
        REQUIRE(snap.nan_inf_count == 2);
        REQUIRE_THAT(snap.peak[0], WithinAbs(0.0f, 1e-6f));  // non-finite skipped
    }

    SECTION("silence run accumulates then resets on signal") {
        StereoBlock quiet(64);
        quiet.fill(0.0f, 0.0f);
        probe.analyze_output(quiet.view());
        probe.analyze_output(quiet.view());
        probe.analyze_output(quiet.view());
        REQUIRE(probe.latest().silence_run_blocks == 3);

        StereoBlock loud(64);
        loud.fill(0.3f, 0.3f);
        probe.analyze_output(loud.view());
        REQUIRE(probe.latest().silence_run_blocks == 0);
    }
}

TEST_CASE("AudioProbe snapshot sequence number is monotonic and gaps detectable",
          "[audio-probe]") {
    AudioProbe probe;
    probe.prepare(1, 32, 44100.0);

    StereoBlock block(32);
    block.fill(0.2f, 0.2f);

    // prepare() publishes sequence 0; the first analyze_output() makes it 1.
    REQUIRE(probe.latest().sequence_number == 0);

    std::uint64_t last = 0;
    for (int i = 0; i < 10; ++i) {
        probe.analyze_output(block.view());
        const auto snap = probe.latest();
        REQUIRE(snap.sequence_number > last);  // strictly monotonic
        last = snap.sequence_number;
    }

    // TripleBuffer is latest-wins: producing several blocks between reads
    // advances the sequence by more than one — a reader sees the gap.
    const std::uint64_t before = last;
    probe.analyze_output(block.view());
    probe.analyze_output(block.view());
    probe.analyze_output(block.view());
    const auto after = probe.latest();
    REQUIRE(after.sequence_number == before + 3);  // gap of 3 is visible
}

TEST_CASE("AudioProbe TripleBuffer publish/read coherence",
          "[audio-probe]") {
    AudioProbe probe;
    probe.prepare(2, 16, 48000.0);

    StereoBlock a(16);
    a.fill(0.1f, 0.1f);
    StereoBlock b(16);
    b.fill(0.8f, 0.8f);

    probe.analyze_output(a.view());
    const auto snap_a = probe.latest();
    REQUIRE_THAT(snap_a.peak_max, WithinAbs(0.1f, 1e-6f));

    probe.analyze_output(b.view());
    const auto snap_b = probe.latest();
    REQUIRE_THAT(snap_b.peak_max, WithinAbs(0.8f, 1e-6f));

    // Re-reading without a new write returns the same coherent value.
    const auto snap_b2 = probe.latest();
    REQUIRE(snap_b2.sequence_number == snap_b.sequence_number);
    REQUIRE_THAT(snap_b2.peak_max, WithinAbs(0.8f, 1e-6f));
}

TEST_CASE("AudioProbe capture ring drains and accounts for drops",
          "[audio-probe]") {
    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = 100;  // small ring to force overflow
    probe.prepare(1, 64, 48000.0, AudioProbeStage::kStandaloneOutputBoundary, cap);

    StereoBlock block(64);
    block.fill(0.5f, 0.5f);

    // First block (64 frames) fits in the 100-frame ring.
    probe.analyze_output(block.view());
    REQUIRE(probe.latest().dropped_capture_frames == 0);

    // Second block: only 36 of 64 fit before the ring is full → 28 dropped,
    // counted (never silently lost).
    probe.analyze_output(block.view());
    REQUIRE(probe.latest().dropped_capture_frames == 28);

    // Consumer drains everything available.
    std::vector<float> out(200, -1.0f);
    const int read = probe.read_capture(out.data(), static_cast<int>(out.size()));
    REQUIRE(read == 100);  // ring held its full capacity
    for (int i = 0; i < read; ++i)
        REQUIRE_THAT(out[static_cast<std::size_t>(i)], WithinAbs(0.5f, 1e-6f));

    // After draining, new writes fit again.
    probe.analyze_output(block.view());
    REQUIRE(probe.latest().dropped_capture_frames == 28);  // unchanged: this block fit
}

TEST_CASE("AudioProbe multichannel capture preserves per-channel samples",
          "[audio-probe][audio-scope]") {
    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = 8;
    probe.prepare(2, 8, 48000.0, AudioProbeStage::kStandaloneOutputBoundary, cap);

    StereoBlock block(8);
    for (std::size_t i = 0; i < block.left.size(); ++i) {
        block.left[i] = static_cast<float>(i);
        block.right[i] = 100.0f + static_cast<float>(i);
    }
    probe.analyze_output(block.view());

    Buffer<float> captured(2, 8);
    const int frames = probe.read_capture(captured.view(), 8);
    REQUIRE(frames == 8);
    for (int i = 0; i < frames; ++i) {
        REQUIRE_THAT(captured.channel(0)[static_cast<std::size_t>(i)],
                     WithinAbs(static_cast<float>(i), 1e-6f));
        REQUIRE_THAT(captured.channel(1)[static_cast<std::size_t>(i)],
                     WithinAbs(100.0f + static_cast<float>(i), 1e-6f));
    }

    // The legacy channel-0 reader still drains the same FIFO and reports the
    // first channel only.
    probe.analyze_output(block.view());
    std::vector<float> ch0(8, -1.0f);
    const int legacy_frames = probe.read_capture(ch0.data(), 8);
    REQUIRE(legacy_frames == 8);
    for (int i = 0; i < legacy_frames; ++i)
        REQUIRE_THAT(ch0[static_cast<std::size_t>(i)],
                     WithinAbs(static_cast<float>(i), 1e-6f));
}

TEST_CASE("AudioStats mirrors device counters without shadowing",
          "[audio-probe][audio-stats]") {
    // The probe owns signal-content counters and NEVER touches device_xruns /
    // cpu_overloads — those are mirrors the host fills from the device owner.
    AudioProbe probe;
    probe.prepare(2, 64, 48000.0);

    StereoBlock block(64);
    block.fill(0.1f, 0.1f);
    block.left[0] = 2.0f;  // clip
    block.left[1] = std::numeric_limits<float>::quiet_NaN();
    probe.analyze_output(block.view());

    AudioStats s = probe.stats();
    REQUIRE(s.callbacks == 1);
    REQUIRE(s.nan_blocks >= 1);
    REQUIRE(s.clipped_blocks >= 1);
    // Probe leaves device mirrors at zero — the host populates them.
    REQUIRE(s.device_xruns == 0);
    REQUIRE(s.cpu_overloads == 0);

    // The host mirrors device-owned counters in afterward; the probe call
    // above did not invent them.
    s.device_xruns = 7;      // e.g. AudioDeviceManager::xrun_count()
    s.cpu_overloads = 3;     // e.g. CoreAudio overload listener
    REQUIRE(s.device_xruns == 7);
    REQUIRE(s.cpu_overloads == 3);
    // Re-querying the probe still reports zero — it does not shadow the host's
    // mirror values.
    REQUIRE(probe.stats().device_xruns == 0);
}

TEST_CASE("AudioStats clipped_blocks / nan_blocks are per-BLOCK, not per-sample",
          "[audio-probe][audio-stats]") {
    // The bug this guards: clipped_blocks/nan_blocks were populated with the
    // per-SAMPLE clip_count / nan_inf_count. A single block with many clipped
    // (or many NaN) samples must count as exactly ONE block.
    AudioProbe probe;
    probe.prepare(2, 64, 48000.0);

    // Block 1: 10 clipped samples + 5 NaN samples — but it is ONE block.
    StereoBlock b1(64);
    b1.fill(0.1f, 0.1f);
    for (int i = 0; i < 10; ++i) b1.left[i] = 2.0f;                       // clips
    for (int i = 10; i < 15; ++i)
        b1.right[i] = std::numeric_limits<float>::quiet_NaN();            // NaN
    probe.analyze_output(b1.view());

    AudioStats s1 = probe.stats();
    REQUIRE(s1.clipped_blocks == 1);  // ONE block, not 10
    REQUIRE(s1.nan_blocks == 1);      // ONE block, not 5
    // The per-sample totals on the snapshot remain the raw sample counts.
    REQUIRE(probe.latest().clip_count == 10);
    REQUIRE(probe.latest().nan_inf_count == 5);

    // Block 2: clean → block tallies unchanged. Block 3: clipped → +1 block.
    StereoBlock clean(64); clean.fill(0.1f, 0.1f);
    probe.analyze_output(clean.view());
    StereoBlock b3(64); b3.fill(0.1f, 0.1f); b3.left[0] = 2.0f;
    probe.analyze_output(b3.view());

    AudioStats s3 = probe.stats();
    REQUIRE(s3.clipped_blocks == 2);  // blocks 1 and 3
    REQUIRE(s3.nan_blocks == 1);      // only block 1
    REQUIRE(s3.callbacks == 3);
}

TEST_CASE("AudioProbe reset clears cumulative counters",
          "[audio-probe]") {
    AudioProbe probe;
    probe.prepare(1, 32, 48000.0);
    StereoBlock block(32);
    block.fill(2.0f, 2.0f);  // clips every sample
    block.left[0] = std::numeric_limits<float>::quiet_NaN();
    probe.analyze_output(block.view());
    REQUIRE(probe.latest().clip_count > 0);
    REQUIRE(probe.latest().nan_inf_count > 0);
    REQUIRE(probe.latest().clipped_blocks > 0);
    REQUIRE(probe.latest().nan_blocks > 0);

    probe.reset();
    auto snap = probe.latest();
    REQUIRE(snap.clip_count == 0);
    REQUIRE(snap.nan_inf_count == 0);
    REQUIRE(snap.clipped_blocks == 0);
    REQUIRE(snap.nan_blocks == 0);
    REQUIRE(snap.callbacks == 0);
    REQUIRE(probe.stats().clipped_blocks == 0);
    REQUIRE(probe.stats().nan_blocks == 0);

    probe.analyze_output(StereoBlock(32).view());  // silence
    snap = probe.latest();
    REQUIRE(snap.clip_count == 0);
    REQUIRE(snap.nan_inf_count == 0);
    REQUIRE(snap.clipped_blocks == 0);
    REQUIRE(snap.nan_blocks == 0);
    REQUIRE(snap.callbacks == 1);

    probe.analyze_output(block.view());
    REQUIRE(probe.latest().clipped_blocks > 0);
    REQUIRE(probe.latest().nan_blocks > 0);

    probe.prepare(1, 32, 48000.0);
    probe.analyze_output(StereoBlock(32).view());  // silence after re-prepare
    snap = probe.latest();
    REQUIRE(snap.clip_count == 0);
    REQUIRE(snap.nan_inf_count == 0);
    REQUIRE(snap.clipped_blocks == 0);
    REQUIRE(snap.nan_blocks == 0);
    REQUIRE(snap.callbacks == 1);
}

TEST_CASE("AudioProbeSnapshot is a trivially-copyable shared schema",
          "[audio-probe]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<AudioProbeSnapshot>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<AudioStats>);
}

// ── Complaint-oriented boundary report ─────────────────────────────────────

namespace {

AudioProbeSnapshot make_snap(AudioProbeStage stage, float peak_max,
                             std::uint64_t silence_run) {
    AudioProbeSnapshot s{};
    s.stage_id = stage;
    s.peak_max = peak_max;
    s.silence_run_blocks = silence_run;
    s.sequence_number = 1;
    return s;
}

}  // namespace

TEST_CASE("Boundary report distinguishes processor / boundary / device faults",
          "[audio-probe][boundary-report]") {
    SECTION("processor silent → processor diagnosis") {
        BoundaryReportInputs in;
        in.processor_output =
            make_snap(AudioProbeStage::kProcessorOutput, 0.0f, 40);
        in.standalone_boundary =
            make_snap(AudioProbeStage::kStandaloneOutputBoundary, 0.0f, 40);
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kProcessorSilent);
        REQUIRE(report.text.find("processor rendered silence") != std::string::npos);
    }

    SECTION("processor signal but boundary silent → boundary diagnosis") {
        BoundaryReportInputs in;
        in.processor_output =
            make_snap(AudioProbeStage::kProcessorOutput, 0.3f, 0);
        in.standalone_boundary =
            make_snap(AudioProbeStage::kStandaloneOutputBoundary, 0.0f, 38);
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kStandaloneBoundarySilent);
        REQUIRE(report.text.find("output buffer not copied") != std::string::npos);
    }

    SECTION("signal present but device xruns → device diagnosis") {
        BoundaryReportInputs in;
        in.processor_output =
            make_snap(AudioProbeStage::kProcessorOutput, 0.3f, 0);
        in.standalone_boundary =
            make_snap(AudioProbeStage::kStandaloneOutputBoundary, 0.3f, 0);
        DeviceStatsView dev;
        dev.callback_running = true;
        dev.sample_rate = 48000.0;
        dev.buffer_size = 128;
        dev.xruns = 12;
        in.device = dev;
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kDeviceProblem);
        REQUIRE(report.text.find("12 xruns") != std::string::npos);
    }

    SECTION("everything healthy → signal present") {
        BoundaryReportInputs in;
        in.processor_output =
            make_snap(AudioProbeStage::kProcessorOutput, 0.3f, 0);
        in.standalone_boundary =
            make_snap(AudioProbeStage::kStandaloneOutputBoundary, 0.3f, 0);
        DeviceStatsView dev;
        dev.callback_running = true;
        dev.sample_rate = 48000.0;
        dev.buffer_size = 128;
        dev.xruns = 0;
        in.device = dev;
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kSignalPresent);
    }

    SECTION("missing stage reported as no-probe, not faked silence") {
        BoundaryReportInputs in;  // nothing populated
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kNoProbeData);
        REQUIRE(report.text.find("Processor output: no probe") != std::string::npos);
        REQUIRE(report.text.find("Standalone boundary: no probe") != std::string::npos);
    }
}

// ── Live Audio Inspector programmatic readout: snapshot → JSON ──────────────
//
// `pulp run --audio-probe-json PATH` writes the live probe's latest snapshot
// (plus the AudioStats subset) as a flat JSON object. The standalone host owns
// the one-shot lifecycle; the snapshot→JSON mapping is factored into the pure
// audio_probe_snapshot_to_json() helper so it is testable without a device.
TEST_CASE("audio_probe_snapshot_to_json carries the documented fields and dBFS",
          "[audio][probe][json][audio-inspector]") {
    AudioProbeSnapshot snap;
    snap.stage_id = AudioProbeStage::kStandaloneOutputBoundary;
    snap.sample_rate = 48000.0;
    snap.block_size = 256;
    snap.channel_count = 2;
    snap.sequence_number = 7;
    snap.peak_max = 0.5f;   // → -6.0206 dBFS
    snap.rms_max = 0.25f;   // → -12.0412 dBFS
    snap.clip_count = 3;
    snap.nan_inf_count = 1;
    snap.clipped_blocks = 2;
    snap.nan_blocks = 1;
    snap.silence_run_blocks = 4;
    snap.callbacks = 99;

    AudioStats stats;
    stats.underruns = 5;
    stats.device_xruns = 6;
    stats.cpu_overloads = 2;

    const auto json = audio_probe_snapshot_to_json(snap, stats);
    const auto v = choc::json::parse(json);

    // choc serializes a whole-valued double (48000.0) as `48000` and parses
    // it back as an int, so use the coercing get<T>() accessors rather than
    // the strict getFloat64()/getInt64().
    REQUIRE(v["stage"].getString() == "standalone_output_boundary");
    REQUIRE(v["sample_rate"].get<double>() == 48000.0);
    REQUIRE(v["block_size"].get<std::int64_t>() == 256);
    REQUIRE(v["channel_count"].get<std::int64_t>() == 2);
    REQUIRE(v["sequence_number"].get<std::int64_t>() == 7);
    REQUIRE(v["peak_max"].get<double>() == Catch::Approx(0.5));
    REQUIRE(v["rms_max"].get<double>() == Catch::Approx(0.25));
    REQUIRE(v["peak_dbfs"].get<double>() == Catch::Approx(-6.0206).margin(1e-3));
    REQUIRE(v["rms_dbfs"].get<double>() == Catch::Approx(-12.0412).margin(1e-3));
    REQUIRE(v["clip_count"].get<std::int64_t>() == 3);
    REQUIRE(v["nan_inf_count"].get<std::int64_t>() == 1);
    REQUIRE(v["clipped_blocks"].get<std::int64_t>() == 2);
    REQUIRE(v["nan_blocks"].get<std::int64_t>() == 1);
    REQUIRE(v["silence_run_blocks"].get<std::int64_t>() == 4);
    REQUIRE(v["callbacks"].get<std::int64_t>() == 99);
    REQUIRE(v["underruns"].get<std::int64_t>() == 5);
    REQUIRE(v["device_xruns"].get<std::int64_t>() == 6);
    REQUIRE(v["cpu_overloads"].get<std::int64_t>() == 2);
}

TEST_CASE("audio_probe_stage_name covers every public stage",
          "[audio][probe][json][audio-inspector]") {
    REQUIRE(audio_probe_stage_name(AudioProbeStage::kProcessorOutput)
            == "processor_output");
    REQUIRE(audio_probe_stage_name(AudioProbeStage::kStandaloneOutputBoundary)
            == "standalone_output_boundary");
    REQUIRE(audio_probe_stage_name(AudioProbeStage::kMeterBridge)
            == "meter_bridge");
    REQUIRE(audio_probe_stage_name(AudioProbeStage::kDeviceCallback)
            == "device_callback");
    REQUIRE(audio_probe_stage_name(AudioProbeStage::kGraphNode)
            == "graph_node");
    REQUIRE(audio_probe_stage_name(AudioProbeStage::kUnknown) == "unknown");
}

TEST_CASE("audio_probe_snapshot_to_json reports silence as null dBFS",
          "[audio][probe][json][audio-inspector]") {
    AudioProbeSnapshot snap;  // peak_max / rms_max default to 0 (true silence)
    AudioStats stats;
    const auto v = choc::json::parse(audio_probe_snapshot_to_json(snap, stats));
    // JSON has no infinity literal, so 0-linear maps to null, letting a reader
    // tell true silence from a finite low level.
    REQUIRE(v["peak_dbfs"].isVoid());
    REQUIRE(v["rms_dbfs"].isVoid());
    REQUIRE(v["stage"].getString() == "unknown");
}

TEST_CASE("audio_probe_snapshot_to_json compact output is a frozen schema golden",
          "[audio][probe][json][audio-inspector][audio-scope]") {
    AudioProbeSnapshot snap;
    snap.stage_id = AudioProbeStage::kStandaloneOutputBoundary;
    snap.sample_rate = 48000.0;
    snap.block_size = 256;
    snap.channel_count = 2;
    snap.sequence_number = 42;
    snap.callbacks = 3;

    AudioStats stats;
    const auto json = audio_probe_snapshot_to_json(snap, stats, false);
    REQUIRE(json ==
            R"({"stage": "standalone_output_boundary", "sample_rate": 48000, "block_size": 256, "channel_count": 2, "sequence_number": 42, "peak_max": 0.0, "rms_max": 0.0, "peak_dbfs": null, "rms_dbfs": null, "clip_count": 0, "nan_inf_count": 0, "clipped_blocks": 0, "nan_blocks": 0, "silence_run_blocks": 0, "callbacks": 3, "underruns": 0, "device_xruns": 0, "cpu_overloads": 0})");
}

TEST_CASE("AudioScope acquisition uses raw tail and rising-zero windows",
          "[audio][scope]") {
    MonoView raw({-1.0f, -0.5f, 0.25f, 0.5f, -0.25f, 0.75f, 1.0f});
    AudioProbeSnapshot snap;
    snap.sample_rate = 48000.0;
    snap.sequence_number = 9;

    AudioScopeAcquisitionConfig cfg;
    cfg.window_samples = 3;
    cfg.trigger_mode = AudioScopeTriggerMode::kNone;
    auto acq = acquire_audio_scope_window(raw.view(), cfg, &snap);
    REQUIRE(acq.ok);
    REQUIRE(acq.window_start == 4);
    REQUIRE(acq.samples == std::vector<float>({-0.25f, 0.75f, 1.0f}));
    REQUIRE(acq.source_sequence_number == 9);

    cfg.trigger_mode = AudioScopeTriggerMode::kRisingZero;
    acq = acquire_audio_scope_window(raw.view(), cfg, &snap);
    REQUIRE(acq.ok);
    REQUIRE(acq.trigger_found);
    REQUIRE(acq.trigger_sample == 2);
    REQUIRE(acq.window_start == 2);
    REQUIRE(acq.samples == std::vector<float>({0.25f, 0.5f, -0.25f}));

    MonoView exact_window({-0.25f, 0.25f, 0.5f});
    cfg.window_samples = 3;
    acq = acquire_audio_scope_window(exact_window.view(), cfg, &snap);
    REQUIRE(acq.ok);
    REQUIRE(acq.trigger_found);
    REQUIRE(acq.trigger_sample == 1);
    REQUIRE(acq.window_start == 0);
    REQUIRE(acq.samples == std::vector<float>({-0.25f, 0.25f, 0.5f}));
}

TEST_CASE("AudioScope acquisition reports edge cases honestly",
          "[audio][scope]") {
    AudioScopeAcquisitionConfig cfg;
    cfg.window_samples = 0;
    MonoView source({0.0f, 1.0f});
    auto acq = acquire_audio_scope_window(source.view(), cfg);
    REQUIRE_FALSE(acq.ok);
    REQUIRE(acq.warnings == std::vector<std::string>{"window_samples_must_be_positive"});

    cfg.window_samples = 4;
    MonoView empty({});
    acq = acquire_audio_scope_window(empty.view(), cfg);
    REQUIRE_FALSE(acq.ok);
    REQUIRE(acq.warnings == std::vector<std::string>{"empty_source"});

    MonoView short_source({0.25f, 0.5f});
    acq = acquire_audio_scope_window(short_source.view(), cfg);
    REQUIRE(acq.ok);
    REQUIRE(acq.window_samples == 2);
    REQUIRE(acq.warnings == std::vector<std::string>{"window_truncated_to_source",
                                                     "trigger_not_found"});

    MultiView multi({{1.0f, 2.0f, 3.0f}, {-1.0f, -2.0f, -3.0f}});
    cfg.window_samples = 2;
    cfg.selected_channel = 99;
    cfg.trigger_mode = AudioScopeTriggerMode::kNone;
    acq = acquire_audio_scope_window(multi.view(), cfg);
    REQUIRE(acq.ok);
    REQUIRE(acq.selected_channel == 0);
    REQUIRE(acq.source_channel_count == 2);
    REQUIRE(acq.samples == std::vector<float>({2.0f, 3.0f}));
    REQUIRE(acq.warnings == std::vector<std::string>{"selected_channel_out_of_range"});

    MonoView no_crossing({0.1f, 0.2f, 0.3f, 0.4f});
    cfg.selected_channel = 0;
    cfg.trigger_mode = AudioScopeTriggerMode::kRisingZero;
    acq = acquire_audio_scope_window(no_crossing.view(), cfg);
    REQUIRE(acq.ok);
    REQUIRE_FALSE(acq.trigger_found);
    REQUIRE(acq.window_start == 2);
    REQUIRE(acq.samples == std::vector<float>({0.3f, 0.4f}));
    REQUIRE(acq.warnings == std::vector<std::string>{"trigger_not_found"});
}

TEST_CASE("AudioScope measurements cover silence DC periodic and nonfinite cases",
          "[audio][scope]") {
    AudioProbeSnapshot snap;
    snap.sample_rate = 48000.0;

    SECTION("silence has scalar measurements but no frequency") {
        MonoView silent({0.0f, 0.0f, 0.0f, 0.0f});
        AudioScopeAcquisitionConfig cfg;
        cfg.window_samples = 4;
        cfg.trigger_mode = AudioScopeTriggerMode::kNone;
        const auto acq = acquire_audio_scope_window(silent.view(), cfg, &snap);
        const auto m = measure_audio_scope_window(acq);
        REQUIRE(m.peak_to_peak_available);
        REQUIRE(m.peak_to_peak == Catch::Approx(0.0));
        REQUIRE(m.rms_available);
        REQUIRE(m.rms == Catch::Approx(0.0));
        REQUIRE(m.dc_offset_available);
        REQUIRE(m.dc_offset == Catch::Approx(0.0));
        REQUIRE_FALSE(m.crest_factor_available);
        REQUIRE_FALSE(m.frequency_available);
        REQUIRE(m.warnings == std::vector<std::string>{"frequency_unavailable_silence"});
    }

    SECTION("pure DC reports offset and unavailable frequency") {
        MonoView dc({0.5f, 0.5f, 0.5f, 0.5f});
        AudioScopeAcquisitionConfig cfg;
        cfg.window_samples = 4;
        cfg.trigger_mode = AudioScopeTriggerMode::kNone;
        const auto acq = acquire_audio_scope_window(dc.view(), cfg, &snap);
        const auto m = measure_audio_scope_window(acq);
        REQUIRE(m.dc_offset == Catch::Approx(0.5));
        REQUIRE(m.rms == Catch::Approx(0.5));
        REQUIRE_FALSE(m.frequency_available);
    }

    SECTION("440 Hz sine estimates frequency conservatively") {
        MonoView sine(sine_wave(440.0, 48000.0, 4096));
        AudioScopeAcquisitionConfig cfg;
        cfg.window_samples = 4096;
        cfg.trigger_mode = AudioScopeTriggerMode::kNone;
        const auto acq = acquire_audio_scope_window(sine.view(), cfg, &snap);
        const auto m = measure_audio_scope_window(acq);
        REQUIRE(m.frequency_available);
        REQUIRE(m.frequency_hz == Catch::Approx(440.0).margin(6.0));
        REQUIRE(m.period_samples == Catch::Approx(48000.0 / 440.0).margin(1.5));
    }

    SECTION("clipped square exposes peak-to-peak and crest factor") {
        MonoView square({-1.0f, -1.0f, 1.0f, 1.0f,
                         -1.0f, -1.0f, 1.0f, 1.0f,
                         -1.0f, -1.0f, 1.0f, 1.0f});
        AudioScopeAcquisitionConfig cfg;
        cfg.window_samples = 12;
        cfg.trigger_mode = AudioScopeTriggerMode::kNone;
        const auto acq = acquire_audio_scope_window(square.view(), cfg, &snap);
        const auto m = measure_audio_scope_window(acq);
        REQUIRE(m.peak_to_peak == Catch::Approx(2.0));
        REQUIRE(m.rms == Catch::Approx(1.0));
        REQUIRE(m.crest_factor_available);
        REQUIRE(m.crest_factor == Catch::Approx(1.0));
    }

    SECTION("nonfinite samples are ignored") {
        MonoView dirty({-1.0f, std::numeric_limits<float>::quiet_NaN(),
                        1.0f, std::numeric_limits<float>::infinity()});
        AudioScopeAcquisitionConfig cfg;
        cfg.window_samples = 4;
        cfg.trigger_mode = AudioScopeTriggerMode::kNone;
        const auto acq = acquire_audio_scope_window(dirty.view(), cfg, &snap);
        const auto m = measure_audio_scope_window(acq);
        REQUIRE(m.peak_to_peak == Catch::Approx(2.0));
        REQUIRE(m.warnings.size() >= 1);
        REQUIRE(m.warnings.front() == "nonfinite_samples_ignored:2");
    }

    SECTION("nonperiodic crossings do not guess frequency") {
        MonoView nonperiodic({-1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
                              1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
                              1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                              -1.0f, 1.0f});
        AudioScopeAcquisitionConfig cfg;
        cfg.window_samples = 17;
        cfg.trigger_mode = AudioScopeTriggerMode::kNone;
        const auto acq = acquire_audio_scope_window(nonperiodic.view(), cfg, &snap);
        const auto m = measure_audio_scope_window(acq);
        REQUIRE_FALSE(m.frequency_available);
        REQUIRE(m.warnings.back() == "frequency_unavailable_nonperiodic");
    }
}

TEST_CASE("AudioScope JSON v1 exposes schema and null unavailable measurements",
          "[audio][scope][json]") {
    AudioScopeResult result;
    result.stage = AudioProbeStage::kStandaloneOutputBoundary;
    result.trigger_mode = AudioScopeTriggerMode::kRisingZero;
    result.acquisition.ok = true;
    result.acquisition.sample_rate = 48000.0;
    result.acquisition.source_channel_count = 1;
    result.acquisition.selected_channel = 0;
    result.acquisition.source_frames = 4;
    result.acquisition.window_start = 0;
    result.acquisition.window_samples = 4;
    result.acquisition.trigger_found = false;
    result.acquisition.source_sequence_number = 42;
    result.acquisition.warnings.push_back("trigger_not_found");
    result.measurements.peak_to_peak_available = true;
    result.measurements.peak_to_peak = 0.0;
    result.measurements.rms_available = true;
    result.measurements.rms = 0.0;
    result.measurements.dc_offset_available = true;
    result.measurements.dc_offset = 0.0;
    result.measurements.warnings.push_back("frequency_unavailable_silence");

    const auto json = audio_scope_result_to_json(result);
    const auto v = choc::json::parse(json);
    REQUIRE(v["schema"].getString() == std::string(kAudioScopeJsonSchema));
    REQUIRE(v["version"].get<std::int64_t>() == kAudioScopeJsonVersion);
    REQUIRE(v["source"]["kind"].getString() == "live_probe");
    REQUIRE(v["source"]["stage"].getString() == "standalone_output_boundary");
    REQUIRE(v["source"]["selected_channel"].get<std::int64_t>() == 0);
    REQUIRE(v["acquisition"]["trigger_mode"].getString() == "rising_zero");
    REQUIRE_FALSE(v["acquisition"]["trigger_found"].get<bool>());
    REQUIRE(v["measurements"]["peak_to_peak"].get<double>() == Catch::Approx(0.0));
    REQUIRE(v["measurements"]["frequency_hz"].isVoid());
    REQUIRE(json.find("\"trigger_not_found\"") != std::string::npos);
    REQUIRE(json.find("\"frequency_unavailable_silence\"") != std::string::npos);
}

TEST_CASE("AudioScope trigger mode parser covers CLI spellings",
          "[audio][scope]") {
    AudioScopeTriggerMode mode = AudioScopeTriggerMode::kRisingZero;
    REQUIRE(parse_audio_scope_trigger_mode("none", mode));
    REQUIRE(mode == AudioScopeTriggerMode::kNone);
    REQUIRE(parse_audio_scope_trigger_mode("rising-zero", mode));
    REQUIRE(mode == AudioScopeTriggerMode::kRisingZero);
    REQUIRE(parse_audio_scope_trigger_mode("rising_zero", mode));
    REQUIRE(mode == AudioScopeTriggerMode::kRisingZero);
    REQUIRE_FALSE(parse_audio_scope_trigger_mode("spectral", mode));
}
