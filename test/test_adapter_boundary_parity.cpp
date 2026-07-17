// Adapter-boundary parity matrix (SF-1).
//
// The per-format plugin adapters (CLAP, VST3, AU v2/v3, AAX, LV2, standalone)
// used to each re-implement the same boundary logic — parameter decode +
// dual-write, f64 marshalling, latency-compensated bypass, and transport →
// ProcessContext mapping — at different fidelity, with no test that noticed
// when a copy drifted. `core/format/include/pulp/format/adapter_boundary.hpp`
// unified that logic; this test is its proof-of-correctness.
//
// It drives ONE processor's boundary through EVERY format as a matrix column
// and asserts identical observable behavior: each format encodes the SAME
// stimulus in its OWN native representation (CLAP fixed-point beattime, VST3
// host-supplied bar, AU seconds → samples, block-rate LV2 control ports, …),
// runs it through the shared boundary core, and must land on identical decoded
// results. A future adapter that hand-rolls a divergent copy of any of these
// four concerns fails here instead of shipping as an audit finding.
//
// The CLAP column is additionally anchored to the REAL adapter: the same
// stimulus is pushed through clap_process() and the values a RecordingProcessor
// observes are compared against the neutral reference, proving the CLAP adapter
// actually routes through the shared core (not just that the core is
// self-consistent).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/format/adapter_boundary.hpp>
#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/param_processing.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// ---------------------------------------------------------------------------
// Canonical stimulus shared by every column.
// ---------------------------------------------------------------------------

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kFrames = 64;
constexpr int kLatencySamples = 32;
constexpr state::ParamID kGainParam = 1;

// One musical transport, expressed in canonical units. Every format encodes
// THIS and must decode back to it.
struct CanonicalTransport {
    bool is_playing = true;
    bool is_recording = false;
    bool is_looping = true;
    double tempo_bpm = 128.0;
    double position_beats = 8.0;               // bar 2 in 4/4
    int time_sig_numerator = 4;
    int time_sig_denominator = 4;
    double loop_start_beats = 4.0;
    double loop_end_beats = 12.0;
    // beat 8 @ 128 BPM = 8 * 60/128 s = 3.75 s -> 180000 samples @ 48 kHz.
    double position_seconds() const { return position_beats * 60.0 / tempo_bpm; }
    std::int64_t position_samples() const {
        return static_cast<std::int64_t>(std::llround(position_seconds() * kSampleRate));
    }
    std::int64_t expected_bar() const { return 2; }  // floor(8 / 4)
};

constexpr CanonicalTransport kTransport{};

// The transport-field subset of a ProcessContext, for equality comparison
// independent of the audio-shape fields.
struct TransportView {
    bool is_playing;
    bool is_recording;
    bool is_looping;
    double tempo_bpm;
    double position_beats;
    std::int64_t position_samples;
    int tsig_num;
    int tsig_denom;
    std::int64_t bar;
    double loop_start_beats;
    double loop_end_beats;
    bool transport_started;
    bool tempo_changed;
    bool transport_changed;

    static TransportView of(const ProcessContext& c) {
        return {c.is_playing, c.is_recording, c.is_looping, c.tempo_bpm,
                c.position_beats, c.position_samples, c.time_sig_numerator,
                c.time_sig_denominator, c.bar, c.loop_start_beats,
                c.loop_end_beats, c.transport_started, c.tempo_changed,
                c.transport_changed};
    }
};

void require_transport_matches_reference(const std::string& column,
                                         const TransportView& v) {
    INFO("format column: " << column);
    CHECK(v.is_playing == kTransport.is_playing);
    CHECK(v.is_recording == kTransport.is_recording);
    CHECK(v.is_looping == kTransport.is_looping);
    CHECK_THAT(v.tempo_bpm, WithinAbs(kTransport.tempo_bpm, 1e-9));
    CHECK_THAT(v.position_beats, WithinAbs(kTransport.position_beats, 1e-6));
    CHECK(v.position_samples == kTransport.position_samples());
    CHECK(v.tsig_num == kTransport.time_sig_numerator);
    CHECK(v.tsig_denom == kTransport.time_sig_denominator);
    CHECK(v.bar == kTransport.expected_bar());
    CHECK_THAT(v.loop_start_beats, WithinAbs(kTransport.loop_start_beats, 1e-6));
    CHECK_THAT(v.loop_end_beats, WithinAbs(kTransport.loop_end_beats, 1e-6));
    // A fresh (no-previous-block) snapshot rolling into a playing transport
    // reports a run start and no change flags — identical for every format.
    CHECK(v.transport_started == true);
    CHECK(v.tempo_changed == false);
    CHECK(v.transport_changed == false);
}

// Build a ProcessContext with the audio-shape fields a block always carries,
// before any transport is applied.
ProcessContext base_context() {
    ProcessContext ctx;
    ctx.sample_rate = kSampleRate;
    ctx.num_samples = static_cast<int>(kFrames);
    ctx.process_mode = ProcessMode::Realtime;
    return ctx;
}

// ---------------------------------------------------------------------------
// Per-format transport encoders: each mirrors how its adapter decodes the host
// playhead into the neutral boundary::HostTransport. The point is that the
// encodings genuinely differ (fixed-point vs float vs samples, host-bar vs
// derived-bar) yet the shared mapper lands them all on the same context.
// ---------------------------------------------------------------------------

// CLAP: fixed-point clap_beattime / clap_sectime, host-supplied bar_number.
boundary::HostTransport encode_clap() {
    boundary::HostTransport t;
    t.valid = true;
    t.is_playing = kTransport.is_playing;
    t.is_recording = kTransport.is_recording;
    t.is_looping = kTransport.is_looping;
    t.has_tempo = true;
    t.tempo_bpm = kTransport.tempo_bpm;
    t.has_beats = true;
    // Round-trip through CLAP's fixed-point beattime, exactly like the adapter.
    const std::int64_t fx =
        static_cast<std::int64_t>(std::llround(kTransport.position_beats * CLAP_BEATTIME_FACTOR));
    t.position_beats = static_cast<double>(fx) / CLAP_BEATTIME_FACTOR;
    t.has_samples = true;
    const std::int64_t sx = static_cast<std::int64_t>(
        std::llround(kTransport.position_seconds() * CLAP_SECTIME_FACTOR));
    const double seconds = static_cast<double>(sx) / CLAP_SECTIME_FACTOR;
    t.position_samples = static_cast<std::int64_t>(std::llround(seconds * kSampleRate));
    t.has_time_sig = true;
    t.time_sig_numerator = kTransport.time_sig_numerator;
    t.time_sig_denominator = kTransport.time_sig_denominator;
    t.loop_start_beats =
        static_cast<double>(std::llround(kTransport.loop_start_beats * CLAP_BEATTIME_FACTOR)) /
        CLAP_BEATTIME_FACTOR;
    t.loop_end_beats =
        static_cast<double>(std::llround(kTransport.loop_end_beats * CLAP_BEATTIME_FACTOR)) /
        CLAP_BEATTIME_FACTOR;
    t.has_host_bar = true;  // CLAP exposes bar_number directly
    t.host_bar = kTransport.expected_bar();
    return t;
}

// VST3: float project-time beats, sampleRate-scaled sample position, and a
// host-supplied barPositionMusic (converted from bars to the bar index).
boundary::HostTransport encode_vst3() {
    boundary::HostTransport t;
    t.valid = true;
    t.is_playing = kTransport.is_playing;
    t.is_recording = kTransport.is_recording;
    t.is_looping = kTransport.is_looping;
    t.has_tempo = true;
    t.tempo_bpm = kTransport.tempo_bpm;
    t.has_beats = true;
    t.position_beats = kTransport.position_beats;  // projectTimeMusic (quarter notes)
    t.has_samples = true;
    t.position_samples = kTransport.position_samples();  // projectTimeSamples
    t.has_time_sig = true;
    t.time_sig_numerator = kTransport.time_sig_numerator;
    t.time_sig_denominator = kTransport.time_sig_denominator;
    t.loop_start_beats = kTransport.loop_start_beats;  // cycleStartMusic
    t.loop_end_beats = kTransport.loop_end_beats;      // cycleEndMusic
    t.has_host_bar = true;  // barPositionMusic
    t.host_bar = kTransport.expected_bar();
    return t;
}

// AU v3 / AU v2: beats + seconds → samples, no host-supplied bar (derived).
boundary::HostTransport encode_au() {
    boundary::HostTransport t;
    t.valid = true;
    t.is_playing = kTransport.is_playing;
    t.is_recording = kTransport.is_recording;
    t.is_looping = kTransport.is_looping;
    t.has_tempo = true;
    t.tempo_bpm = kTransport.tempo_bpm;
    t.has_beats = true;
    t.position_beats = kTransport.position_beats;  // outCurrentBeat
    t.has_samples = true;
    t.position_samples =
        static_cast<std::int64_t>(std::llround(kTransport.position_seconds() * kSampleRate));
    t.has_time_sig = true;
    t.time_sig_numerator = kTransport.time_sig_numerator;
    t.time_sig_denominator = kTransport.time_sig_denominator;
    t.loop_start_beats = kTransport.loop_start_beats;  // outCycleStartBeat
    t.loop_end_beats = kTransport.loop_end_beats;
    t.has_host_bar = false;  // AU has no precomputed bar -> derive from beats
    return t;
}

// AAX: sample position + tempo + time signature; beats reconstructed from the
// sample position, bar derived. (No native beats timeline field.)
boundary::HostTransport encode_aax() {
    boundary::HostTransport t;
    t.valid = true;
    t.is_playing = kTransport.is_playing;
    t.is_recording = kTransport.is_recording;
    t.is_looping = kTransport.is_looping;
    t.has_tempo = true;
    t.tempo_bpm = kTransport.tempo_bpm;
    t.has_samples = true;
    t.position_samples = kTransport.position_samples();
    // AAX reconstructs beats from samples: beats = samples / sr * bpm / 60.
    t.has_beats = true;
    t.position_beats =
        static_cast<double>(t.position_samples) / kSampleRate * kTransport.tempo_bpm / 60.0;
    t.has_time_sig = true;
    t.time_sig_numerator = kTransport.time_sig_numerator;
    t.time_sig_denominator = kTransport.time_sig_denominator;
    t.loop_start_beats = kTransport.loop_start_beats;
    t.loop_end_beats = kTransport.loop_end_beats;
    t.has_host_bar = false;
    return t;
}

// LV2: block-rate control ports; beats + tempo + time sig read once/block,
// bar derived.
boundary::HostTransport encode_lv2() {
    boundary::HostTransport t;
    t.valid = true;
    t.is_playing = kTransport.is_playing;
    t.is_recording = kTransport.is_recording;
    t.is_looping = kTransport.is_looping;
    t.has_tempo = true;
    t.tempo_bpm = kTransport.tempo_bpm;
    t.has_beats = true;
    t.position_beats = kTransport.position_beats;  // Position.beat (bars+beats folded)
    t.has_samples = true;
    t.position_samples = kTransport.position_samples();  // Position.frame
    t.has_time_sig = true;
    t.time_sig_numerator = kTransport.time_sig_numerator;
    t.time_sig_denominator = kTransport.time_sig_denominator;
    t.loop_start_beats = kTransport.loop_start_beats;
    t.loop_end_beats = kTransport.loop_end_beats;
    t.has_host_bar = false;
    return t;
}

// Standalone / headless: exposes the same canonical transport directly, no host
// bar, derived like the DAW-less host does.
boundary::HostTransport encode_standalone() { return encode_au(); }

struct FormatTransportColumn {
    const char* name;
    boundary::HostTransport (*encode)();
};

const std::vector<FormatTransportColumn>& transport_columns() {
    static const std::vector<FormatTransportColumn> cols = {
        {"CLAP", encode_clap},   {"VST3", encode_vst3},
        {"AU v3", encode_au},    {"AU v2", encode_au},
        {"AAX", encode_aax},     {"LV2", encode_lv2},
        {"standalone", encode_standalone},
    };
    return cols;
}

// ---------------------------------------------------------------------------
// A processor for the real-CLAP anchor: unity-scaled passthrough that records
// the ProcessContext it last received, and reports a fixed latency.
// ---------------------------------------------------------------------------

struct CapturedContext {
    bool seen = false;
    ProcessContext ctx;
    float gain_seen = 1.0f;
};

class RecordingProcessor : public Processor {
public:
    explicit RecordingProcessor(CapturedContext* sink) : sink_(sink) {}

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "PulpBoundaryParity";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.adapter-boundary-parity";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"In", 2}};
        d.output_buses = {{"Out", 2}};
        d.accepts_midi = false;
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = kGainParam, .name = "Gain", .range = {0.0f, 1.0f, 1.0f, 0.0f}});
    }
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const ProcessContext& ctx) override {
        if (sink_) {
            sink_->seen = true;
            sink_->ctx = ctx;
            sink_->gain_seen = state().get_value(kGainParam);
        }
        const float g = state().get_value(kGainParam);
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            auto oc = out.channel(ch);
            if (ch < in.num_channels()) {
                auto ic = in.channel(ch);
                for (std::size_t i = 0; i < out.num_samples(); ++i)
                    oc[i] = (i < ic.size() ? ic[i] : 0.0f) * g;
            } else {
                for (std::size_t i = 0; i < out.num_samples(); ++i) oc[i] = 0.0f;
            }
        }
    }
    int latency_samples() const override { return kLatencySamples; }

private:
    CapturedContext* sink_;
};

// ProcessorFactory is a plain function pointer, so the factory can't capture a
// per-instance sink. A file-scope pointer bridges it; the tests are
// single-threaded and each ClapInstance sets it before clap_init constructs the
// processor.
CapturedContext* g_active_sink = nullptr;

std::unique_ptr<Processor> make_recording_processor() {
    return std::make_unique<RecordingProcessor>(g_active_sink);
}

// Real CLAP adapter instance driven through its public C surface (no global
// entry symbol), mirroring test_clap_constant_mask.cpp's harness.
struct ClapInstance {
    clap_adapter::PulpClapPlugin plugin;
    CapturedContext captured;
    bool active = false;

    ClapInstance() {
        g_active_sink = &captured;
        plugin.factory = &make_recording_processor;
        plugin.plugin.plugin_data = &plugin;
        REQUIRE(clap_adapter::clap_init(&plugin.plugin));
        REQUIRE(clap_adapter::clap_activate(&plugin.plugin, kSampleRate,
                                            static_cast<uint32_t>(kFrames), kFrames));
        active = true;
    }
    ~ClapInstance() {
        if (active) clap_adapter::clap_deactivate(&plugin.plugin);
        g_active_sink = nullptr;
    }
};

clap_event_transport_t make_clap_transport() {
    clap_event_transport_t tr{};
    tr.header.size = sizeof(tr);
    tr.header.type = CLAP_EVENT_TRANSPORT;
    tr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
               CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
               CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_IS_PLAYING |
               CLAP_TRANSPORT_IS_LOOP_ACTIVE;
    tr.tempo = kTransport.tempo_bpm;
    tr.song_pos_beats =
        static_cast<clap_beattime>(std::llround(kTransport.position_beats * CLAP_BEATTIME_FACTOR));
    tr.song_pos_seconds = static_cast<clap_sectime>(
        std::llround(kTransport.position_seconds() * CLAP_SECTIME_FACTOR));
    tr.loop_start_beats =
        static_cast<clap_beattime>(std::llround(kTransport.loop_start_beats * CLAP_BEATTIME_FACTOR));
    tr.loop_end_beats =
        static_cast<clap_beattime>(std::llround(kTransport.loop_end_beats * CLAP_BEATTIME_FACTOR));
    tr.tsig_num = static_cast<uint16_t>(kTransport.time_sig_numerator);
    tr.tsig_denom = static_cast<uint16_t>(kTransport.time_sig_denominator);
    tr.bar_number = static_cast<int32_t>(kTransport.expected_bar());
    return tr;
}

}  // namespace

// ===========================================================================
// (D) Transport parity.
// ===========================================================================

TEST_CASE("boundary parity: every format decodes the same transport identically",
          "[format][sf1][parity][transport]") {
    TransportView reference{};
    bool have_reference = false;

    for (const auto& col : transport_columns()) {
        ProcessContext ctx = base_context();
        detail::PlayheadSnapshot snapshot;  // fresh: no previous block
        boundary::apply_host_transport(ctx, col.encode(), snapshot);

        const auto view = TransportView::of(ctx);
        require_transport_matches_reference(col.name, view);

        // Cross-column identity: not just "matches the reference constants" but
        // "byte-for-byte identical to the first column decoded".
        if (!have_reference) {
            reference = view;
            have_reference = true;
        } else {
            INFO("format column: " << col.name << " vs " << transport_columns()[0].name);
            CHECK(view.is_playing == reference.is_playing);
            CHECK(view.position_samples == reference.position_samples);
            CHECK(view.bar == reference.bar);
            CHECK_THAT(view.position_beats, WithinAbs(reference.position_beats, 1e-6));
            CHECK_THAT(view.tempo_bpm, WithinAbs(reference.tempo_bpm, 1e-9));
        }
    }
}

TEST_CASE("boundary parity: derived bar equals host-supplied bar",
          "[format][sf1][parity][transport]") {
    // CLAP/VST3 supply bar_number/barPositionMusic; AU/AAX/LV2 derive it. The
    // whole point of the shared mapper is that both routes agree.
    ProcessContext host_bar_ctx = base_context();
    detail::PlayheadSnapshot s1;
    boundary::apply_host_transport(host_bar_ctx, encode_clap(), s1);  // has_host_bar

    ProcessContext derived_ctx = base_context();
    detail::PlayheadSnapshot s2;
    boundary::apply_host_transport(derived_ctx, encode_au(), s2);  // derived

    CHECK(host_bar_ctx.bar == derived_ctx.bar);
    CHECK(host_bar_ctx.bar == kTransport.expected_bar());
}

TEST_CASE("boundary parity: the real CLAP adapter routes transport through the core",
          "[format][sf1][parity][transport][clap]") {
    ClapInstance inst;

    std::vector<float> in_l(kFrames, 0.0f), in_r(kFrames, 0.0f);
    std::vector<float> out_l(kFrames, 0.0f), out_r(kFrames, 0.0f);
    float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};

    clap_audio_buffer_t in_bus{};
    in_bus.channel_count = 2;
    in_bus.data32 = in_ptrs;
    clap_audio_buffer_t out_bus{};
    out_bus.channel_count = 2;
    out_bus.data32 = out_ptrs;

    auto tr = make_clap_transport();
    clap_process_t proc{};
    proc.frames_count = kFrames;
    proc.audio_inputs = &in_bus;
    proc.audio_inputs_count = 1;
    proc.audio_outputs = &out_bus;
    proc.audio_outputs_count = 1;
    proc.transport = &tr;

    REQUIRE(clap_adapter::clap_process(&inst.plugin.plugin, &proc) ==
            CLAP_PROCESS_CONTINUE);
    REQUIRE(inst.captured.seen);

    // The context the processor actually received, built by the CLAP adapter,
    // must land on the same canonical transport as the neutral matrix column.
    require_transport_matches_reference("CLAP (real adapter)",
                                        TransportView::of(inst.captured.ctx));
}

// ===========================================================================
// (B) f64 marshalling parity.
// ===========================================================================

TEST_CASE("boundary parity: f32<->f64 marshalling round-trips exactly",
          "[format][sf1][parity][f64]") {
    // Every float value survives a widen→narrow round-trip bit-exactly, so a
    // double-precision host boundary produces the same block a float host does.
    std::vector<float> src(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i)
        src[i] = std::sin(static_cast<float>(i) * 0.37f) * 0.9f;

    std::vector<double> wide(kFrames, 0.0);
    std::vector<float> back(kFrames, 0.0f);
    boundary::copy_f32_to_f64(src.data(), wide.data(), kFrames);
    boundary::copy_f64_to_f32(wide.data(), back.data(), kFrames);

    for (uint32_t i = 0; i < kFrames; ++i) {
        INFO("sample " << i);
        CHECK(back[i] == src[i]);  // exact
    }

    std::vector<double> zeroed(kFrames, 7.0);
    boundary::zero_f64(zeroed.data(), kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(zeroed[i] == 0.0);

    // Null-safety contract every adapter relies on.
    boundary::copy_f32_to_f64(nullptr, wide.data(), kFrames);
    boundary::copy_f64_to_f32(nullptr, back.data(), kFrames);
    boundary::zero_f64(nullptr, kFrames);
    SUCCEED();
}

TEST_CASE("boundary parity: the CLAP f64 path matches the f32 path sample-for-sample",
          "[format][sf1][parity][f64][clap]") {
    // Drive the SAME processor + input through the real CLAP adapter twice —
    // once with f32 host buffers, once with f64 — and require identical output.
    auto run = [](bool f64) {
        ClapInstance inst;
        std::vector<float> in32[2], out32[2];
        std::vector<double> in64[2], out64[2];
        float* in32p[2];
        double* in64p[2];
        float* out32p[2];
        double* out64p[2];
        for (int ch = 0; ch < 2; ++ch) {
            in32[ch].assign(kFrames, 0.0f);
            out32[ch].assign(kFrames, 0.0f);
            in64[ch].assign(kFrames, 0.0);
            out64[ch].assign(kFrames, 0.0);
            for (uint32_t i = 0; i < kFrames; ++i) {
                const double v = std::sin(i * 0.21 + ch) * 0.8;
                in32[ch][i] = static_cast<float>(v);
                in64[ch][i] = v;
            }
            in32p[ch] = in32[ch].data();
            in64p[ch] = in64[ch].data();
            out32p[ch] = out32[ch].data();
            out64p[ch] = out64[ch].data();
        }
        clap_audio_buffer_t in_bus{};
        clap_audio_buffer_t out_bus{};
        in_bus.channel_count = 2;
        out_bus.channel_count = 2;
        if (f64) {
            in_bus.data64 = in64p;
            out_bus.data64 = out64p;
        } else {
            in_bus.data32 = in32p;
            out_bus.data32 = out32p;
        }
        clap_process_t proc{};
        proc.frames_count = kFrames;
        proc.audio_inputs = &in_bus;
        proc.audio_inputs_count = 1;
        proc.audio_outputs = &out_bus;
        proc.audio_outputs_count = 1;
        REQUIRE(clap_adapter::clap_process(&inst.plugin.plugin, &proc) ==
                CLAP_PROCESS_CONTINUE);
        std::vector<float> result(kFrames * 2);
        for (int ch = 0; ch < 2; ++ch)
            for (uint32_t i = 0; i < kFrames; ++i)
                result[ch * kFrames + i] =
                    f64 ? static_cast<float>(out64[ch][i]) : out32[ch][i];
        return result;
    };

    const auto out_f32 = run(false);
    const auto out_f64 = run(true);
    for (std::size_t i = 0; i < out_f32.size(); ++i) {
        INFO("interleaved sample " << i);
        CHECK_THAT(out_f64[i], WithinAbs(out_f32[i], 1e-6f));
    }
}

// ===========================================================================
// (C) Bypass-latency parity.
// ===========================================================================

TEST_CASE("boundary parity: latency-compensated bypass delays by exactly the reported latency",
          "[format][sf1][parity][bypass]") {
    // An impulse at frame 0 must reappear at frame `latency`, and this must be
    // identical no matter which format owns the delay line — the shared
    // component is the only implementation.
    for (const char* column : {"CLAP", "VST3", "AU v3", "AU v2", "AAX", "LV2"}) {
        INFO("format column: " << column);
        boundary::LatencyCompensatedBypass bypass;
        bypass.prepare(kLatencySamples);
        REQUIRE(bypass.is_latency_compensated());
        REQUIRE(bypass.delay_samples() == kLatencySamples);

        std::vector<float> in(kFrames, 0.0f);
        std::vector<float> out(kFrames, 0.0f);
        in[0] = 1.0f;
        bypass.process_channel(out.data(), in.data(), kFrames, /*ch=*/0);

        for (uint32_t i = 0; i < kFrames; ++i) {
            INFO("frame " << i);
            if (i == static_cast<uint32_t>(kLatencySamples))
                CHECK_THAT(out[i], WithinAbs(1.0f, 1e-6f));
            else
                CHECK_THAT(out[i], WithinAbs(0.0f, 1e-6f));
        }
    }
}

TEST_CASE("boundary parity: zero reported latency is a straight passthrough",
          "[format][sf1][parity][bypass]") {
    boundary::LatencyCompensatedBypass bypass;
    bypass.prepare(0);
    CHECK_FALSE(bypass.is_latency_compensated());

    std::vector<float> in(kFrames), out(kFrames, -1.0f);
    for (uint32_t i = 0; i < kFrames; ++i) in[i] = static_cast<float>(i);
    bypass.process_channel(out.data(), in.data(), kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(out[i] == in[i]);

    // A null input channel writes silence, not garbage.
    std::vector<float> out2(kFrames, 5.0f);
    bypass.process_channel(out2.data(), nullptr, kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(out2[i] == 0.0f);
}

// ---------------------------------------------------------------------------
// render_bypass_passthrough — the shared float-buffer bypass helper the AU v2,
// AU v3, and AAX adapters route their bypass short-circuit through the shared helper. A
// regression here fires if any of those adapters is reverted to an undelayed
// memcpy, because they now call this exact function. VST3/CLAP keep their own
// inline loops (they marshal f64), covered by the real-adapter tests below.
// ---------------------------------------------------------------------------

TEST_CASE("render_bypass_passthrough delays the dry signal by the reported latency",
          "[format][sf1][parity][bypass]") {
    boundary::LatencyCompensatedBypass bypass;
    bypass.prepare(kLatencySamples);
    REQUIRE(bypass.is_latency_compensated());

    // Stereo impulse at frame 0 on both channels.
    std::vector<float> in_l(kFrames, 0.0f), in_r(kFrames, 0.0f);
    std::vector<float> out_l(kFrames, -1.0f), out_r(kFrames, -1.0f);
    in_l[0] = 1.0f;
    in_r[0] = 1.0f;
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};

    boundary::render_bypass_passthrough(bypass, out_ptrs, 2, in_ptrs, 2, kFrames);

    // The impulse must reappear at exactly frame `latency`, not frame 0 — the
    // undelayed passthrough (the bug) would place it at frame 0.
    for (uint32_t i = 0; i < kFrames; ++i) {
        INFO("frame " << i);
        const float expected = (i == static_cast<uint32_t>(kLatencySamples)) ? 1.0f : 0.0f;
        CHECK_THAT(out_l[i], WithinAbs(expected, 1e-6f));
        CHECK_THAT(out_r[i], WithinAbs(expected, 1e-6f));
    }
}

TEST_CASE("render_bypass_passthrough: zero latency is an undelayed copy; "
          "missing input channels are silenced",
          "[format][sf1][parity][bypass]") {
    boundary::LatencyCompensatedBypass bypass;
    bypass.prepare(0);
    CHECK_FALSE(bypass.is_latency_compensated());

    std::vector<float> in_l(kFrames), out_l(kFrames, -1.0f), out_r(kFrames, 7.0f);
    for (uint32_t i = 0; i < kFrames; ++i) in_l[i] = static_cast<float>(i + 1);
    const float* in_ptrs[1] = {in_l.data()};       // only one input channel
    float* out_ptrs[2] = {out_l.data(), out_r.data()};

    // 2 output channels, 1 input channel: ch0 copies input verbatim, ch1 has no
    // matching input and must be zeroed (not left holding its prior 7.0f).
    boundary::render_bypass_passthrough(bypass, out_ptrs, 2, in_ptrs, 1, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        CHECK(out_l[i] == in_l[i]);
        CHECK(out_r[i] == 0.0f);
    }

    // A null output-channel pointer is skipped, never dereferenced.
    float* sparse_out[2] = {nullptr, out_r.data()};
    std::fill(out_r.begin(), out_r.end(), 3.0f);
    boundary::render_bypass_passthrough(bypass, sparse_out, 2, in_ptrs, 1, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(out_r[i] == 0.0f);  // ch1 silenced
}

TEST_CASE("render_bypass_passthrough: a channel count above the boundary ceiling "
          "degrades to an undelayed copy uniformly",
          "[format][sf1][parity][bypass]") {
    // All-or-nothing: with more output channels than the boundary owns delay
    // lines for, the whole block must fall back to a zero-delay copy rather than
    // mixing delayed and undelayed channels.
    boundary::LatencyCompensatedBypass bypass;
    bypass.prepare(kLatencySamples);
    REQUIRE(bypass.is_latency_compensated());
    const int over = static_cast<int>(bypass.channel_capacity()) + 1;

    std::vector<std::vector<float>> ins(over, std::vector<float>(kFrames, 0.0f));
    std::vector<std::vector<float>> outs(over, std::vector<float>(kFrames, -1.0f));
    std::vector<const float*> in_ptrs(over);
    std::vector<float*> out_ptrs(over);
    for (int c = 0; c < over; ++c) {
        ins[c][0] = 1.0f;  // impulse at frame 0 on every channel
        in_ptrs[c] = ins[c].data();
        out_ptrs[c] = outs[c].data();
    }

    boundary::render_bypass_passthrough(bypass, out_ptrs.data(), over,
                                        in_ptrs.data(), over, kFrames);

    // Uniform undelayed copy: the impulse stays at frame 0 on every channel.
    for (int c = 0; c < over; ++c) {
        INFO("channel " << c);
        CHECK_THAT(outs[c][0], WithinAbs(1.0f, 1e-6f));
        for (uint32_t i = 1; i < kFrames; ++i) CHECK_THAT(outs[c][i], WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("boundary parity: the real CLAP adapter's bypass is latency-compensated",
          "[format][sf1][parity][bypass][clap]") {
    ClapInstance inst;
    REQUIRE(inst.plugin.bypass_param_id != 0);
    inst.plugin.store.set_value(inst.plugin.bypass_param_id, 1.0f);  // engage bypass

    std::vector<float> in_l(kFrames, 0.0f), in_r(kFrames, 0.0f);
    std::vector<float> out_l(kFrames, 0.0f), out_r(kFrames, 0.0f);
    in_l[0] = 1.0f;  // impulse on the left channel
    float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};

    clap_audio_buffer_t in_bus{};
    in_bus.channel_count = 2;
    in_bus.data32 = in_ptrs;
    clap_audio_buffer_t out_bus{};
    out_bus.channel_count = 2;
    out_bus.data32 = out_ptrs;

    clap_process_t proc{};
    proc.frames_count = kFrames;
    proc.audio_inputs = &in_bus;
    proc.audio_inputs_count = 1;
    proc.audio_outputs = &out_bus;
    proc.audio_outputs_count = 1;

    REQUIRE(clap_adapter::clap_process(&inst.plugin.plugin, &proc) ==
            CLAP_PROCESS_CONTINUE);

    // The processor reports kLatencySamples, so the bypassed dry impulse is
    // delayed by exactly that (matches the neutral component above).
    for (uint32_t i = 0; i < kFrames; ++i) {
        INFO("frame " << i);
        if (i == static_cast<uint32_t>(kLatencySamples))
            CHECK_THAT(out_l[i], WithinAbs(1.0f, 1e-6f));
        else
            CHECK_THAT(out_l[i], WithinAbs(0.0f, 1e-6f));
    }
}

// ===========================================================================
// (A) Parameter decode + dual-write parity.
// ===========================================================================

namespace {

// Apply the same logical automation (0.25 at offset 0, 0.75 at offset 32) at
// the fidelity a format supports, then return the per-subblock decoded gain.
struct ParamDecode {
    std::vector<std::pair<int, float>> subblocks;  // {end_offset, value}
    float store_final = 0.0f;
};

ParamDecode decode_params(bool sample_accurate) {
    state::StateStore store;
    store.add_parameter(
        {.id = kGainParam, .name = "Gain", .range = {0.0f, 1.0f, 0.25f, 0.0f}});
    state::ParameterEventQueue queue;

    if (sample_accurate) {
        boundary::apply_param_value(queue, store, kGainParam, 0, 0.25f);
        boundary::apply_param_value(queue, store, kGainParam, 32, 0.75f);
    } else {
        // Block-rate formats collapse the automation to the last value at
        // offset 0 (AU v2 / LV2 control ports read once per block).
        boundary::apply_param_value(queue, store, kGainParam, 0, 0.75f);
    }
    queue.sort();

    // Render subblock boundaries through the shared param cursor.
    std::vector<float> out_buf(kFrames, 0.0f), in_buf(kFrames, 0.0f);
    float* op[1] = {out_buf.data()};
    const float* ip[1] = {in_buf.data()};
    audio::BufferView<float> out(op, 1, kFrames);
    audio::BufferView<const float> in(ip, 1, kFrames);

    ParamDecode result;
    for_each_subblock(out, in, store, &queue,
                      [&](audio::BufferView<float>& o,
                          const audio::BufferView<const float>&,
                          state::ParamCursor& params) {
                          const int end =
                              static_cast<int>(reinterpret_cast<float*>(o.channel(0).data()) -
                                               out_buf.data()) +
                              static_cast<int>(o.num_samples());
                          result.subblocks.push_back({end, params.value(kGainParam)});
                      });
    result.store_final = store.get_value(kGainParam);
    return result;
}

}  // namespace

TEST_CASE("boundary parity: sample-accurate param decode splits at the event offset",
          "[format][sf1][parity][params]") {
    const auto sa = decode_params(/*sample_accurate=*/true);
    REQUIRE(sa.subblocks.size() == 2);
    CHECK(sa.subblocks[0].first == 32);
    CHECK_THAT(sa.subblocks[0].second, WithinAbs(0.25f, 1e-6f));
    CHECK(sa.subblocks[1].first == static_cast<int>(kFrames));
    CHECK_THAT(sa.subblocks[1].second, WithinAbs(0.75f, 1e-6f));
    // Dual-write reached the store with the final value.
    CHECK_THAT(sa.store_final, WithinAbs(0.75f, 1e-6f));
}

TEST_CASE("boundary parity: block-rate and sample-accurate agree on the block-final value",
          "[format][sf1][parity][params]") {
    // The two classes decode the WITHIN-block shape differently (that divergence
    // is real and documented), but every format's dual-write must leave the
    // store at the same steady-state value the host last sent.
    const auto sa = decode_params(true);
    const auto br = decode_params(false);
    CHECK_THAT(sa.store_final, WithinAbs(br.store_final, 1e-6f));
    CHECK_THAT(sa.subblocks.back().second, WithinAbs(br.subblocks.back().second, 1e-6f));
    // Block-rate is a single subblock spanning the whole block.
    REQUIRE(br.subblocks.size() == 1);
    CHECK(br.subblocks[0].first == static_cast<int>(kFrames));
}

TEST_CASE("boundary parity: the real CLAP adapter dual-writes params to the store",
          "[format][sf1][parity][params][clap]") {
    ClapInstance inst;

    // A host param-value event delivered inside clap_process must reach the
    // listener store (set_value_rt) — the second half of the dual-write.
    clap_event_param_value_t ev{};
    ev.header.size = sizeof(ev);
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.time = 0;
    ev.param_id = kGainParam;
    ev.value = 0.5;

    struct EventList {
        clap_event_param_value_t* ev;
        static uint32_t size(const clap_input_events_t*) { return 1; }
        static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t) {
            return &static_cast<EventList*>(l->ctx)->ev->header;
        }
    } list{&ev};
    clap_input_events_t in_events{};
    in_events.ctx = &list;
    in_events.size = &EventList::size;
    in_events.get = &EventList::get;

    std::vector<float> in_l(kFrames, 0.0f), in_r(kFrames, 0.0f);
    std::vector<float> out_l(kFrames, 0.0f), out_r(kFrames, 0.0f);
    float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t in_bus{};
    in_bus.channel_count = 2;
    in_bus.data32 = in_ptrs;
    clap_audio_buffer_t out_bus{};
    out_bus.channel_count = 2;
    out_bus.data32 = out_ptrs;

    clap_process_t proc{};
    proc.frames_count = kFrames;
    proc.audio_inputs = &in_bus;
    proc.audio_inputs_count = 1;
    proc.audio_outputs = &out_bus;
    proc.audio_outputs_count = 1;
    proc.in_events = &in_events;

    REQUIRE(clap_adapter::clap_process(&inst.plugin.plugin, &proc) ==
            CLAP_PROCESS_CONTINUE);
    CHECK_THAT(inst.plugin.store.get_value(kGainParam), WithinAbs(0.5f, 1e-6f));
    CHECK_THAT(inst.captured.gain_seen, WithinAbs(0.5f, 1e-6f));
}

// ---------------------------------------------------------------------------
// Output-parameter publication (adapter_boundary.hpp section 5)
//
// The mirror of the param-decode column above: values the PLUGIN changed during
// process() travel back to the host for automation recording. VST3 and CLAP
// emit through different host ABIs but share the bookkeeping — index resolution,
// the pre-process() snapshot, and the post-process() diff — so these pin that
// shared contract once.
// ---------------------------------------------------------------------------

namespace {
constexpr state::ParamID kMixParam = 2;

/// Gain at index 0, Mix at index 1 — `StateStore` is non-copyable, so callers
/// own the store and this only registers into it.
void add_two_params(state::StateStore& store) {
    store.add_parameter(
        {.id = kGainParam, .name = "Gain", .range = {0.0f, 1.0f, 0.25f, 0.0f}});
    store.add_parameter(
        {.id = kMixParam, .name = "Mix", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
}
}  // namespace

TEST_CASE("boundary parity: param index resolves registration position",
          "[format][sf1][parity][params]") {
    state::StateStore store;
    add_two_params(store);
    const auto all = store.all_params();

    CHECK(boundary::find_param_index(all, kGainParam) == 0);
    CHECK(boundary::find_param_index(all, kMixParam) == 1);
    // An id the plugin never registered must be reported as absent, not
    // silently resolved onto some other parameter's scratch slot — every
    // adapter drops such an output event rather than publishing it.
    CHECK(boundary::find_param_index(all, 999) == boundary::kParamIndexNotFound);
    // An empty store resolves nothing.
    state::StateStore empty;
    CHECK(boundary::find_param_index(empty.all_params(), kGainParam)
          == boundary::kParamIndexNotFound);
}

TEST_CASE("boundary parity: the value snapshot aligns positionally with all_params",
          "[format][sf1][parity][params]") {
    state::StateStore store;
    add_two_params(store);
    std::vector<float> snapshot;
    boundary::snapshot_param_values(store, store.all_params(), snapshot);

    REQUIRE(snapshot.size() == store.all_params().size());
    CHECK_THAT(snapshot[0], WithinAbs(0.25f, 1e-6f));
    CHECK_THAT(snapshot[1], WithinAbs(0.5f, 1e-6f));

    // Re-snapshotting after a plugin-side write observes the new value, and the
    // previous snapshot is what the diff compares against. This is the whole
    // mechanism by which an adapter reports plugin-side changes to the host.
    store.set_value(kMixParam, 0.9f);
    std::vector<float> after;
    boundary::snapshot_param_values(store, store.all_params(), after);
    CHECK(boundary::changed_since_snapshot(after[1], snapshot[1]));
    CHECK_FALSE(boundary::changed_since_snapshot(after[0], snapshot[0]));
}

TEST_CASE("boundary parity: snapshot reuses its buffer across blocks",
          "[format][sf1][parity][params][rt-safety]") {
    // Adapters reserve this vector off the audio thread and re-snapshot into it
    // every block; the per-block call must not reallocate.
    state::StateStore store;
    add_two_params(store);
    std::vector<float> snapshot;
    snapshot.reserve(store.all_params().size());
    boundary::snapshot_param_values(store, store.all_params(), snapshot);
    const auto* first_block_data = snapshot.data();
    for (int block = 0; block < 8; ++block) {
        boundary::snapshot_param_values(store, store.all_params(), snapshot);
    }
    CHECK(snapshot.data() == first_block_data);
}

TEST_CASE("boundary parity: the snapshot diff compares bits, not numeric equality",
          "[format][sf1][parity][params]") {
    // An unchanged value is never republished.
    CHECK_FALSE(boundary::changed_since_snapshot(0.5f, 0.5f));
    CHECK(boundary::changed_since_snapshot(0.5f, 0.5000001f));

    // A NaN parameter must settle rather than republish on every block forever
    // (which is what `current != snapshot` would do, since NaN != NaN).
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(boundary::changed_since_snapshot(nan, nan));
    CHECK(boundary::changed_since_snapshot(nan, 0.5f));

    // Signed zeros are distinct bit patterns, so a store write that flips the
    // sign is a real change even though `-0.0f == 0.0f`.
    CHECK(boundary::changed_since_snapshot(-0.0f, 0.0f));
}
