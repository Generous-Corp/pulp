// Forge lo-fi DSP catalog — bake-layer param-injection tests.
//
// For each catalog node (forge_lofi_catalog.hpp) this proves that a CONTROL-
// THREAD injection of its macro knob changes the BAKED node's audio output,
// without re-baking, over the real production path (bake() → BakedGraphProcessor
// → claim_param_injection → ParamInjector → routed executor → ParamCursor). It is
// the F2-b sibling of test_baked_graph_param_injection.cpp (F2-a), reusing the
// same fixture shape:
//
//   * Filter  — a low cutoff attenuates a high-frequency tone (spectral change);
//               resonance is a MEASURED peak at cutoff, and each registered mode
//               is located on a swept magnitude response, not assumed.
//   * Waveshaper — higher drive saturates a sine (RMS/peak grow: more THD).
//   * Dry/Wet — mix crossfades a dry vs a wet constant (dry/wet balance).
//   * Noise   — level raises the noise floor of a silent input.
//   * Bitcrush — bit_depth raises quantization error; sample_rate_reduction
//                introduces sample-and-hold plateaus.
//   * Trim    — a dB setting attenuates/boosts by exactly that many dB.
//   * Ping-pong — an impulse in one channel produces echoes that alternate
//                 sides, asserted as a per-echo L:R energy ratio.
//   * Reverb — mix injection exposes a wet tail, decay changes its duration,
//                and the normalized maximum-decay path respects its gain bound.
//   * Compressor — a loud sine above threshold is measurably reduced, the
//                reduction grows with ratio, and a slow release pumps (gain
//                reduction recovers slowly over time).
//   * Gate — signal below threshold is attenuated to near-silence, above
//                threshold passes, and the hold time keeps it open after the
//                key drops below threshold.
//
// Plus one RT-allocation probe over every node's process path, and a boundedness
// check proving the dynamics nodes never expand (so the Forge output guard stays
// transparent).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_lofi_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp::host;
namespace lofi = pulp::host::forge_lofi;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;

// Bake a graph `in(input_channels) → custom → out(output_channels)`, wiring one
// input-node port to each of the custom node's input ports and each of its
// output ports to the matching output-node port, then prepare the baked
// processor. Mirrors the F2-a BakedFixture but supports a multi-port node.
struct BakedFixture {
    SignalGraph g;
    LowerResult result;
    NodeId custom_node = 0;
    int output_channels = 1;

    explicit BakedFixture(const CustomNodeType& type, int input_channels = 1,
                          int out_channels = 1)
        : output_channels(out_channels) {
        REQUIRE(g.register_custom_node_type(type));
        const auto in = g.add_input_node(input_channels, "In");
        custom_node = g.add_custom_node(type.type_id, 1, "Node");
        const auto out = g.add_output_node(out_channels, "Out");
        for (int port = 0; port < type.num_input_ports; ++port) {
            REQUIRE(g.connect(in, static_cast<PortIndex>(port), custom_node,
                              static_cast<PortIndex>(port)));
        }
        for (int port = 0; port < out_channels; ++port) {
            REQUIRE(g.connect(custom_node, static_cast<PortIndex>(port), out,
                              static_cast<PortIndex>(port)));
        }
        g.set_canonical_executor_routing_enabled(true);
        REQUIRE(g.prepare(kSr, kFrames));

        result = bake(g);
        REQUIRE(result.accepted);
        REQUIRE(result.processor);
        REQUIRE(result.reason == LowerRejectReason::None);

        pulp::format::PrepareContext pc;
        pc.sample_rate = kSr;
        pc.max_buffer_size = kFrames;
        pc.input_channels = input_channels;
        pc.output_channels = out_channels;
        result.processor->prepare(pc);
    }

    BakedGraphProcessor& baked() {
        return *static_cast<BakedGraphProcessor*>(result.processor.get());
    }
};

// Render one block of a multi-channel input through `proc`; returns every
// output channel.
std::vector<std::vector<float>> run_block_multi(
    pulp::format::Processor& proc, const std::vector<std::vector<float>>& in_channels,
    int out_channels) {
    const auto num_ch = static_cast<std::uint32_t>(in_channels.size());
    std::vector<const float*> in_ptrs(in_channels.size());
    for (std::size_t c = 0; c < in_channels.size(); ++c) in_ptrs[c] = in_channels[c].data();

    std::vector<std::vector<float>> output(
        static_cast<std::size_t>(out_channels),
        std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f));
    std::vector<float*> out_ptrs(static_cast<std::size_t>(out_channels));
    for (std::size_t c = 0; c < out_ptrs.size(); ++c) out_ptrs[c] = output[c].data();

    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), num_ch,
                                                 static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(out_ptrs.data(),
                                            static_cast<std::uint32_t>(out_channels),
                                            static_cast<std::uint32_t>(kFrames));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;
    proc.process(out_view, in_view, midi_in, midi_out, ctx);
    return output;
}

// Render one block of a multi-channel input through `proc`; returns mono output.
std::vector<float> run_block(pulp::format::Processor& proc,
                             const std::vector<std::vector<float>>& in_channels) {
    return run_block_multi(proc, in_channels, /*out_channels=*/1).front();
}

// Convenience: mono input.
std::vector<float> run_block(pulp::format::Processor& proc, const std::vector<float>& mono) {
    return run_block(proc, std::vector<std::vector<float>>{mono});
}

std::vector<float> sine(int n, double freq_hz, float amp = 1.0f) {
    std::vector<float> v(static_cast<std::size_t>(n), 0.0f);
    for (int k = 0; k < n; ++k) {
        v[static_cast<std::size_t>(k)] =
            amp * static_cast<float>(std::sin(2.0 * M_PI * freq_hz * k / kSr));
    }
    return v;
}

float rms(const std::vector<float>& b) {
    double sum = 0.0;
    for (float v : b) sum += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(sum / static_cast<double>(b.size())));
}

float peak(const std::vector<float>& b) {
    float m = 0.0f;
    for (float v : b) m = std::max(m, std::fabs(v));
    return m;
}

pulp::state::ParameterEvent immediate(pulp::state::ParamID id, float value,
                                      std::int32_t offset = 0) {
    return {id, offset, value, /*ramp_duration_sample_frames=*/0};
}

// Settle a stateful node at a fixed injected knob value, returning the last
// block's output (steady state). `feed` is repeated each block.
std::vector<float> settle(pulp::format::Processor& proc, const std::vector<float>& feed,
                          int blocks = 8) {
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b) out = run_block(proc, feed);
    return out;
}

// Measured magnitude response, in dB, of a mono node driven by a unit sine at
// `freq_hz`. Two details make this a real measurement rather than an
// approximation:
//
//   * The excitation is PHASE-CONTINUOUS across blocks. Restarting the phase
//     every block would inject a step discontinuity whose broadband splash
//     swamps a filter's stopband.
//   * The magnitude is read out by a single-bin DFT at the probe frequency over
//     a whole number of cycles, not by block RMS. Block RMS silently reports a
//     low-frequency passband as several dB down simply because 128 samples is a
//     fraction of one cycle — and it counts harmonics as signal.
float gain_db_at(pulp::format::Processor& proc, double freq_hz, int settle_blocks = 16,
                 double measure_cycles = 16.0) {
    const double step = 2.0 * M_PI * freq_hz / kSr;
    double phase = 0.0;
    std::vector<float> tone(static_cast<std::size_t>(kFrames), 0.0f);

    auto drive_one_block = [&](std::vector<double>* phases) {
        for (int k = 0; k < kFrames; ++k) {
            if (phases != nullptr) (*phases)[static_cast<std::size_t>(k)] = phase;
            tone[static_cast<std::size_t>(k)] = static_cast<float>(std::sin(phase));
            phase += step;
        }
        return run_block(proc, tone);
    };

    for (int b = 0; b < settle_blocks; ++b) drive_one_block(nullptr);

    const int measure_blocks = std::max(
        4, static_cast<int>(std::ceil(measure_cycles * kSr / freq_hz / kFrames)));
    std::vector<double> phases(static_cast<std::size_t>(kFrames), 0.0);
    double re = 0.0, im = 0.0;
    long n = 0;
    for (int b = 0; b < measure_blocks; ++b) {
        const auto out = drive_one_block(&phases);
        for (int k = 0; k < kFrames; ++k) {
            const double v = out[static_cast<std::size_t>(k)];
            re += v * std::cos(phases[static_cast<std::size_t>(k)]);
            im += v * std::sin(phases[static_cast<std::size_t>(k)]);
            ++n;
        }
    }
    // Correlating a unit sine against itself over n samples gives n/2, so the
    // scaled bin magnitude IS the response magnitude for a unit-amplitude drive.
    const double mag = 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
    return static_cast<float>(20.0 * std::log10(std::max(mag, 1e-12)));
}

}  // namespace

// ── Filter: cutoff changes spectral content ──────────────────────────────
TEST_CASE("Forge lo-fi: injecting filter cutoff changes a baked tone's spectrum",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_filter_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // A high-frequency tone well above a low cutoff but below a high cutoff.
    const auto tone = sine(kFrames, /*freq=*/8000.0, /*amp=*/0.8f);

    // Cutoff wide open (20 kHz) → the 8 kHz tone largely passes.
    REQUIRE(inj.inject(immediate(lofi::kFilterCutoffHz, 20000.0f)) == InjectStatus::Ok);
    const float open_rms = rms(settle(*fx.result.processor, tone));

    // Cutoff low (300 Hz) → the 8 kHz tone is strongly attenuated.
    REQUIRE(inj.inject(immediate(lofi::kFilterCutoffHz, 300.0f)) == InjectStatus::Ok);
    const float closed_rms = rms(settle(*fx.result.processor, tone));

    INFO("open_rms=" << open_rms << " closed_rms=" << closed_rms);
    CHECK(open_rms > 0.3f);            // the tone passes when open
    CHECK(closed_rms < open_rms * 0.25f);  // and is cut when the cutoff drops
}

// ── Waveshaper: drive changes saturation (THD/level) ─────────────────────
TEST_CASE("Forge lo-fi: injecting waveshaper drive saturates a baked sine",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_waveshaper_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    const auto tone = sine(kFrames, /*freq=*/220.0, /*amp=*/0.5f);

    // Unity drive → mild tanh, output tracks the input (peak ~tanh(0.5)).
    REQUIRE(inj.inject(immediate(lofi::kWaveshaperDrive, 1.0f)) == InjectStatus::Ok);
    const auto clean = settle(*fx.result.processor, tone, /*blocks=*/2);

    // High drive → tanh saturates toward a square: RMS and peak both grow.
    REQUIRE(inj.inject(immediate(lofi::kWaveshaperDrive, 32.0f)) == InjectStatus::Ok);
    const auto driven = settle(*fx.result.processor, tone, /*blocks=*/2);

    INFO("clean rms=" << rms(clean) << " peak=" << peak(clean)
                      << "  driven rms=" << rms(driven) << " peak=" << peak(driven));
    CHECK(rms(driven) > rms(clean) * 1.5f);  // more energy from saturation
    CHECK(peak(driven) > 0.95f);             // near full-scale clip
    CHECK(peak(clean) < 0.6f);               // mild before the knob turns
}

// ── Dry/Wet: mix crossfades dry vs wet ───────────────────────────────────
TEST_CASE("Forge lo-fi: injecting dry/wet mix crossfades a baked node",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_drywet_node(), /*input_channels=*/2);
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Dry = constant +0.25 (port 0), wet = constant +0.75 (port 1). The Linear
    // curve gives out = dry·(1-mix) + wet·mix.
    const std::vector<float> dry(kFrames, 0.25f);
    const std::vector<float> wet(kFrames, 0.75f);
    const std::vector<std::vector<float>> stereo{dry, wet};

    REQUIRE(inj.inject(immediate(lofi::kDryWetMix, 0.0f)) == InjectStatus::Ok);
    const auto fully_dry = run_block(*fx.result.processor, stereo);
    for (float v : fully_dry) CHECK(v == Catch::Approx(0.25f).margin(1e-4));

    REQUIRE(inj.inject(immediate(lofi::kDryWetMix, 1.0f)) == InjectStatus::Ok);
    const auto fully_wet = run_block(*fx.result.processor, stereo);
    for (float v : fully_wet) CHECK(v == Catch::Approx(0.75f).margin(1e-4));

    REQUIRE(inj.inject(immediate(lofi::kDryWetMix, 0.5f)) == InjectStatus::Ok);
    const auto half = run_block(*fx.result.processor, stereo);
    for (float v : half) CHECK(v == Catch::Approx(0.5f).margin(1e-4));  // midpoint
}

// ── Noise: level raises the noise floor ──────────────────────────────────
TEST_CASE("Forge lo-fi: injecting noise level raises a baked node's noise floor",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_noise_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    const std::vector<float> silence(kFrames, 0.0f);

    // level 0 → silent input stays silent (no hiss).
    REQUIRE(inj.inject(immediate(lofi::kNoiseLevel, 0.0f)) == InjectStatus::Ok);
    const auto clean = run_block(*fx.result.processor, silence);
    CHECK(rms(clean) == Catch::Approx(0.0f).margin(1e-6));

    // level up → a measurable noise floor appears.
    REQUIRE(inj.inject(immediate(lofi::kNoiseLevel, 0.5f)) == InjectStatus::Ok);
    const auto hiss = run_block(*fx.result.processor, silence);
    INFO("clean rms=" << rms(clean) << " hiss rms=" << rms(hiss));
    CHECK(rms(hiss) > 0.1f);  // ~0.5/sqrt(3) ≈ 0.29 for uniform white

    // Turning it back down restores silence (knob is live, not latched-on).
    REQUIRE(inj.inject(immediate(lofi::kNoiseLevel, 0.0f)) == InjectStatus::Ok);
    const auto quiet_again = run_block(*fx.result.processor, silence);
    CHECK(rms(quiet_again) == Catch::Approx(0.0f).margin(1e-6));
}

// ── Bitcrush: bit_depth changes quantization error ───────────────────────
TEST_CASE("Forge lo-fi: injecting bitcrush bit_depth changes quantization error",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_bitcrush_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // A smooth sine so quantization error is well-defined. No rate reduction so
    // this isolates the bit-depth axis.
    const auto tone = sine(kFrames, /*freq=*/440.0, /*amp=*/0.9f);

    auto quant_error = [&](float bits) {
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kBitcrushBitDepth, bits)));
        REQUIRE(q.push(immediate(lofi::kBitcrushRateDiv, 1.0f)));  // full rate
        REQUIRE(inj.inject(q) == InjectStatus::Ok);
        const auto out = run_block(*fx.result.processor, tone);
        double e = 0.0;
        for (std::size_t k = 0; k < out.size(); ++k) {
            const double d = static_cast<double>(out[k]) - tone[k];
            e += d * d;
        }
        return e;
    };

    const double err_16 = quant_error(16.0f);  // near-lossless
    const double err_3 = quant_error(3.0f);    // 8 levels: coarse
    INFO("err_16=" << err_16 << " err_3=" << err_3);
    CHECK(err_3 > err_16 * 10.0);   // coarse quantization is far noisier
    CHECK(err_16 < 1e-3);           // 16-bit is effectively transparent here
}

// ── Bitcrush: sample_rate_reduction introduces sample-and-hold plateaus ───
TEST_CASE("Forge lo-fi: injecting bitcrush rate reduction decimates a baked tone",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_bitcrush_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // A tone that changes every sample so sample-and-hold plateaus are visible.
    const auto tone = sine(kFrames, /*freq=*/2000.0, /*amp=*/0.9f);

    // Count sample-to-sample changes in the output. Full rate: nearly every
    // sample differs; heavy reduction: only ~n/reduction samples change.
    auto changes = [&](float reduction) {
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kBitcrushBitDepth, 16.0f)));  // isolate rate axis
        REQUIRE(q.push(immediate(lofi::kBitcrushRateDiv, reduction)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);
        const auto out = run_block(*fx.result.processor, tone);
        int c = 0;
        for (std::size_t k = 1; k < out.size(); ++k) {
            if (std::fabs(out[k] - out[k - 1]) > 1e-6f) ++c;
        }
        return c;
    };

    const int full = changes(1.0f);
    const int decimated = changes(8.0f);
    INFO("full-rate changes=" << full << " reduced(8x) changes=" << decimated);
    CHECK(full > kFrames / 2);              // full rate tracks the tone
    CHECK(decimated < full / 3);            // 8x hold flattens most steps
    CHECK(decimated > 0);                   // but still latches periodically
}

// ── Delay: time + feedback are live on the baked feedback echo ───────────
TEST_CASE("Forge lo-fi: injecting delay feedback changes a baked echo's tail",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_delay_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Feed an impulse once, then measure the summed energy of `blocks` silent
    // blocks. Both params are set together via a batch queue (a single-event
    // inject would collapse two same-node params — the mailbox is latest-wins).
    const std::vector<float> impulse = [] {
        std::vector<float> v(kFrames, 0.0f);
        v[0] = 1.0f;
        return v;
    }();
    const std::vector<float> silence(kFrames, 0.0f);

    auto tail_energy = [&](float time_ms, float feedback, int blocks) {
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kDelayTimeMs, time_ms)));
        REQUIRE(q.push(immediate(lofi::kDelayFeedback, feedback)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);
        run_block(*fx.result.processor, impulse);
        double e = 0.0;
        for (int b = 0; b < blocks; ++b) {
            const auto out = run_block(*fx.result.processor, silence);
            for (float v : out) e += static_cast<double>(v) * v;
        }
        return e;
    };

    // A short 2 ms tap: feedback 0 gives a single echo that dies out; feedback
    // 0.9 recirculates into a long decaying comb — much more summed tail energy.
    const double fb_low = tail_energy(2.0f, 0.0f, 24);
    const double fb_high = tail_energy(2.0f, 0.9f, 24);
    INFO("fb_low=" << fb_low << " fb_high=" << fb_high);
    CHECK(fb_high > fb_low * 1.5);   // more feedback => more tail

    // Two clearly different tap times leave clearly different near-field energy
    // over the same window — proof the injected time reaches the baked read tap.
    const double t_a = tail_energy(2.0f, 0.7f, 8);
    const double t_b = tail_energy(40.0f, 0.7f, 8);
    const double hi = std::max(t_a, t_b);
    const double lo = std::min(t_a, t_b);
    INFO("t(2ms)=" << t_a << " t(40ms)=" << t_b);
    CHECK(hi > lo * 1.5);            // the injected time moved the echo
}

// ── Filter: resonance is a real, measured peak at cutoff ─────────────────
TEST_CASE("Forge lo-fi: injecting filter resonance peaks the response at cutoff",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_filter_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    constexpr double kCutoff = 1000.0;
    REQUIRE(inj.inject(immediate(lofi::kFilterCutoffHz,
                                 static_cast<float>(kCutoff))) == InjectStatus::Ok);

    // A TPT state-variable lowpass has |H| = Q at its cutoff, so the gain
    // DIFFERENCE between the maximally flat setting and a resonant one is the
    // whole claim: measure it rather than assert "sounds resonant".
    REQUIRE(inj.inject(immediate(lofi::kFilterResonance, 0.707f)) == InjectStatus::Ok);
    const float flat_db = gain_db_at(*fx.result.processor, kCutoff);

    REQUIRE(inj.inject(immediate(lofi::kFilterResonance, 12.0f)) == InjectStatus::Ok);
    const float resonant_db = gain_db_at(*fx.result.processor, kCutoff);

    const float delta_db = resonant_db - flat_db;
    INFO("gain at cutoff: Q=0.707 -> " << flat_db << " dB, Q=12 -> " << resonant_db
                                       << " dB, delta " << delta_db << " dB");
    CHECK(flat_db == Catch::Approx(-3.0f).margin(1.0f));  // maximally flat: −3 dB
    CHECK(delta_db >= 12.0f);                             // the claim's bar
    CHECK(delta_db == Catch::Approx(24.6f).margin(2.0f)); // 20·log10(12 / 0.707)

    // Resonance must not leak into the passband: well below cutoff both
    // settings pass unity, so a build that binds Resonance stays usable.
    REQUIRE(inj.inject(immediate(lofi::kFilterResonance, 12.0f)) == InjectStatus::Ok);
    CHECK(gain_db_at(*fx.result.processor, kCutoff / 16.0) ==
          Catch::Approx(0.0f).margin(1.0f));
}

// ── Filter: each registered mode really is that mode ─────────────────────
TEST_CASE("Forge lo-fi: filter mode types measure as lowpass/highpass/bandpass/notch",
          "[host][baked][param-injection][forge][forge-lofi]") {
    constexpr double kCutoff = 1000.0;

    // Sweep a 1/24-octave grid two octaves either side of cutoff and take the
    // measured response's extremum — the bandpass peak and the notch null are
    // both located, not assumed.
    auto swept = [](const CustomNodeType& type, float resonance) {
        BakedFixture fx(type);
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        REQUIRE(inj.inject(immediate(lofi::kFilterCutoffHz,
                                     static_cast<float>(kCutoff))) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(lofi::kFilterResonance, resonance)) ==
                InjectStatus::Ok);
        std::vector<std::pair<double, float>> curve;
        for (int step = -48; step <= 48; ++step) {
            const double f = kCutoff * std::pow(2.0, step / 24.0);
            curve.emplace_back(f, gain_db_at(*fx.result.processor, f));
        }
        return curve;
    };

    SECTION("bandpass peaks at cutoff — the 'telephone' band") {
        const auto curve = swept(lofi::make_filter_node(pulp::signal::Svf::Mode::bandpass), 8.0f);
        double peak_hz = 0.0;
        float peak_db = -1e9f;
        for (const auto& [f, db] : curve) {
            if (db > peak_db) { peak_db = db; peak_hz = f; }
        }
        // ±1/4 octave around 1 kHz.
        INFO("bandpass peak at " << peak_hz << " Hz (" << peak_db << " dB)");
        CHECK(peak_hz >= kCutoff * std::pow(2.0, -0.25));
        CHECK(peak_hz <= kCutoff * std::pow(2.0, 0.25));
        // Two octaves out on both sides the band has genuinely rejected.
        CHECK(curve.front().second < peak_db - 20.0f);
        CHECK(curve.back().second < peak_db - 20.0f);
    }

    SECTION("notch nulls at cutoff") {
        const auto curve = swept(lofi::make_filter_node(pulp::signal::Svf::Mode::notch), 8.0f);
        double null_hz = 0.0;
        float null_db = 1e9f;
        for (const auto& [f, db] : curve) {
            if (db < null_db) { null_db = db; null_hz = f; }
        }
        INFO("notch null at " << null_hz << " Hz (" << null_db << " dB)");
        CHECK(null_hz >= kCutoff * std::pow(2.0, -0.25));
        CHECK(null_hz <= kCutoff * std::pow(2.0, 0.25));
        CHECK(null_db < -20.0f);
        // Both skirts return to unity — a notch, not a bandstop shelf.
        CHECK(curve.front().second == Catch::Approx(0.0f).margin(1.0f));
        CHECK(curve.back().second == Catch::Approx(0.0f).margin(1.0f));
    }

    SECTION("lowpass and highpass are mirror images") {
        const auto lp = swept(lofi::make_filter_node(pulp::signal::Svf::Mode::lowpass), 0.707f);
        const auto hp = swept(lofi::make_filter_node(pulp::signal::Svf::Mode::highpass), 0.707f);
        INFO("lowpass  low=" << lp.front().second << " dB high=" << lp.back().second
                             << " dB\nhighpass low=" << hp.front().second
                             << " dB high=" << hp.back().second << " dB");
        CHECK(lp.front().second == Catch::Approx(0.0f).margin(1.0f));  // passes lows
        CHECK(lp.back().second < -20.0f);                              // rejects highs
        CHECK(hp.back().second == Catch::Approx(0.0f).margin(1.0f));   // passes highs
        CHECK(hp.front().second < -20.0f);                             // rejects lows
    }
}

// ── Trim: an honest dB gain stage ────────────────────────────────────────
TEST_CASE("Forge lo-fi: injecting trim gain scales a baked signal by exactly that many dB",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_trim_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    const auto tone = sine(kFrames, /*freq=*/440.0, /*amp=*/1.0f);
    const float in_rms = rms(tone);

    struct Case { float db; };
    for (const float db : {-24.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f}) {
        REQUIRE(inj.inject(immediate(lofi::kTrimGainDb, db)) == InjectStatus::Ok);
        const auto out = settle(*fx.result.processor, tone, /*blocks=*/2);
        const float measured =
            20.0f * std::log10(std::max(rms(out), 1e-12f) / in_rms);
        INFO("trim " << db << " dB -> measured " << measured << " dB");
        CHECK(measured == Catch::Approx(db).margin(0.1f));
    }

    // 0 dB is exactly unity, not merely close: an untouched Level is a no-op.
    REQUIRE(inj.inject(immediate(lofi::kTrimGainDb, 0.0f)) == InjectStatus::Ok);
    const auto unity = settle(*fx.result.processor, tone, /*blocks=*/2);
    for (std::size_t k = 0; k < unity.size(); ++k) CHECK(unity[k] == tone[k]);
}

// ── Ping-pong: echoes measurably alternate channels ──────────────────────
TEST_CASE("Forge lo-fi: ping-pong echoes alternate left and right",
          "[host][baked][param-injection][forge][forge-lofi][stereo]") {
    constexpr float kTimeMs = 10.0f;   // 480 samples at 48 kHz
    constexpr float kFeedback = 0.8f;
    const int delay_samples = static_cast<int>(kTimeMs * 0.001f * kSr);

    // Render an impulse in the LEFT channel only, then silence, and keep the
    // whole stereo tail so each echo can be located by index. A fresh bake per
    // call, because the delay lines carry the previous render's tail.
    auto render_tail = [&](float width) {
        BakedFixture fx(lofi::make_ping_pong_node(), /*input_channels=*/2,
                        /*out_channels=*/2);
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kPingPongTimeMs, kTimeMs)));
        REQUIRE(q.push(immediate(lofi::kPingPongFeedback, kFeedback)));
        REQUIRE(q.push(immediate(lofi::kPingPongWidth, width)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);

        std::vector<float> left, right;
        std::vector<std::vector<float>> in{std::vector<float>(kFrames, 0.0f),
                                           std::vector<float>(kFrames, 0.0f)};
        constexpr int kBlocks = 40;  // 5120 samples: ten 480-sample echoes
        for (int b = 0; b < kBlocks; ++b) {
            in[0].assign(kFrames, 0.0f);
            in[1].assign(kFrames, 0.0f);
            if (b == 0) in[0][0] = 1.0f;  // impulse, LEFT only
            const auto out = run_block_multi(*fx.result.processor, in, 2);
            left.insert(left.end(), out[0].begin(), out[0].end());
            right.insert(right.end(), out[1].begin(), out[1].end());
        }
        return std::pair{left, right};
    };

    // Energy in a small window centred on echo `n`.
    auto echo_energy = [&](const std::vector<float>& ch, int n) {
        const int centre = n * delay_samples;
        double e = 0.0;
        for (int k = centre - 8; k <= centre + 8; ++k) {
            if (k < 0 || k >= static_cast<int>(ch.size())) continue;
            e += static_cast<double>(ch[static_cast<std::size_t>(k)]) *
                 ch[static_cast<std::size_t>(k)];
        }
        return e;
    };

    SECTION("width 1 — the bounce") {
        const auto [left, right] = render_tail(/*width=*/1.0f);
        // Echo n lands on the left for odd n and the right for even n. Assert
        // the ratio, not merely "some energy on both sides": a stereo-but-not-
        // bouncing delay would put every echo on both.
        for (int n = 1; n <= 8; ++n) {
            const double on = echo_energy(n % 2 == 1 ? left : right, n);
            const double off = echo_energy(n % 2 == 1 ? right : left, n);
            const double ratio_db =
                10.0 * std::log10(std::max(on, 1e-30) / std::max(off, 1e-30));
            INFO("echo " << n << " expected on the " << (n % 2 == 1 ? "left" : "right")
                         << ": on=" << on << " off=" << off << " ratio " << ratio_db
                         << " dB");
            CHECK(on > 1e-6);          // the echo is actually there
            CHECK(ratio_db >= 40.0);   // and it is on ONE side
        }
    }

    SECTION("width 0 — the same measurement finds no bounce") {
        // The discriminating control: the identical node and the identical
        // measurement, with the cross-blend collapsed. Every echo now leaves
        // both outputs equally, so the alternation assertion above cannot pass
        // — which is what makes its passing at width 1 mean something.
        const auto [left, right] = render_tail(/*width=*/0.0f);
        for (int n = 1; n <= 8; ++n) {
            const double l = echo_energy(left, n);
            const double r = echo_energy(right, n);
            const double ratio_db =
                10.0 * std::log10(std::max(l, 1e-30) / std::max(r, 1e-30));
            INFO("echo " << n << " l=" << l << " r=" << r << " ratio " << ratio_db
                         << " dB");
            CHECK(l > 1e-6);
            CHECK(ratio_db == Catch::Approx(0.0).margin(0.1));  // dead centre
        }
    }
}

// ── Reverb: mix injection exposes the wet tail ────────────────────────────
TEST_CASE("Forge lo-fi: injecting reverb mix switches from dry impulse to wet tail",
          "[host][baked][param-injection][forge][forge-lofi]") {
    auto render = [](float mix) {
        BakedFixture fx(lofi::make_reverb_node());
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kReverbDecay, 1.0f)));
        REQUIRE(q.push(immediate(lofi::kReverbDamping, 0.2f)));
        REQUIRE(q.push(immediate(lofi::kReverbMix, mix)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);

        std::vector<float> impulse(kFrames, 0.0f);
        impulse[0] = 1.0f;
        std::vector<float> stream = run_block(*fx.result.processor, impulse);
        const std::vector<float> silence(kFrames, 0.0f);
        for (int b = 0; b < 80; ++b) {
            const auto block = run_block(*fx.result.processor, silence);
            stream.insert(stream.end(), block.begin(), block.end());
        }
        return stream;
    };

    const auto dry = render(0.0f);
    const auto wet = render(1.0f);
    CHECK(dry.front() == Catch::Approx(1.0f));
    CHECK(peak(std::vector<float>(dry.begin() + 1, dry.end())) < 1e-7f);
    CHECK(std::fabs(wet.front()) < 1e-7f);  // the first wet tap is delayed
    CHECK(peak(std::vector<float>(wet.begin() + kFrames, wet.end())) > 1e-3f);
}

// ── Reverb: decay injection controls tail duration ───────────────────────
TEST_CASE("Forge lo-fi: injecting reverb decay changes the measured tail energy",
          "[host][baked][param-injection][forge][forge-lofi]") {
    auto late_tail_energy = [](float decay) {
        BakedFixture fx(lofi::make_reverb_node());
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kReverbDecay, decay)));
        REQUIRE(q.push(immediate(lofi::kReverbDamping, 0.0f)));
        REQUIRE(q.push(immediate(lofi::kReverbMix, 1.0f)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);

        std::vector<float> impulse(kFrames, 0.0f);
        impulse[0] = 1.0f;
        run_block(*fx.result.processor, impulse);
        const std::vector<float> silence(kFrames, 0.0f);
        double energy = 0.0;
        // Compare a window around 0.5 s: a 0.1 s RT60 tail is effectively gone,
        // while a 2 s RT60 tail still has plainly measurable energy.
        constexpr int kWindowStartBlock = 180;
        constexpr int kWindowEndBlock = 210;
        for (int b = 1; b <= kWindowEndBlock; ++b) {
            const auto out = run_block(*fx.result.processor, silence);
            if (b >= kWindowStartBlock) {
                for (float v : out) energy += static_cast<double>(v) * v;
            }
        }
        return energy;
    };

    const double short_tail = late_tail_energy(0.1f);
    const double long_tail = late_tail_energy(2.0f);
    INFO("reverb late energy short=" << short_tail << " long=" << long_tail);
    CHECK(long_tail > 1e-8);
    CHECK(long_tail > short_tail * 100.0);
}

// ── Reverb: maximum-decay steady input respects Forge's 2x contract ──────
TEST_CASE("Forge lo-fi: normalized reverb stays finite and within its declared gain",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_reverb_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());
    pulp::state::ParameterEventQueue q;
    REQUIRE(q.push(immediate(lofi::kReverbDecay, 10.0f)));
    REQUIRE(q.push(immediate(lofi::kReverbDamping, 0.0f)));
    REQUIRE(q.push(immediate(lofi::kReverbMix, 1.0f)));
    REQUIRE(inj.inject(q) == InjectStatus::Ok);

    const std::vector<float> unity(kFrames, 1.0f);
    float max_output = 0.0f;
    float final_mean = 0.0f;
    // Eight seconds reaches >99% of the raw FDN's DC steady state at 10 s RT60.
    const int blocks = static_cast<int>(8.0 * kSr / kFrames);
    for (int b = 0; b < blocks; ++b) {
        const auto out = run_block(*fx.result.processor, unity);
        for (float v : out) {
            REQUIRE(std::isfinite(v));
            max_output = std::max(max_output, std::fabs(v));
        }
        if (b == blocks - 1) {
            for (float v : out) final_mean += v / static_cast<float>(kFrames);
        }
    }
    INFO("reverb max=" << max_output << " final mean=" << final_mean);
    CHECK(final_mean > 0.5f);  // exercised the formerly ~35x buildup, not only startup
    CHECK(max_output <= 2.0f);
}

// ── Compressor: level reduction above threshold, growing with ratio ──────
TEST_CASE("Forge lo-fi: compressor reduces a loud sine and reduction grows with ratio",
          "[host][baked][param-injection][forge][forge-lofi]") {
    // Peak output level of a −6 dBFS sine (amp 0.5) after the envelope settles,
    // at threshold −18 dB and a given ratio. The classic transfer function is
    // out_db = threshold + (in_db − threshold)/ratio for in above threshold, so a
    // −6 dBFS peak → −18 + (−6 − −18)/ratio. Measured at ratio 4 that is −15 dBFS.
    auto settled_peak = [&](float ratio) {
        BakedFixture fx(lofi::make_compressor_node());
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kCompThresholdDb, -18.0f)));
        REQUIRE(q.push(immediate(lofi::kCompRatio, ratio)));
        REQUIRE(q.push(immediate(lofi::kCompAttackMs, 5.0f)));
        REQUIRE(q.push(immediate(lofi::kCompReleaseMs, 80.0f)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);
        const auto tone = sine(kFrames, /*freq=*/220.0, /*amp=*/0.5f);
        // Settle well past the 80 ms release so the peak-riding envelope is steady.
        return peak(settle(*fx.result.processor, tone, /*blocks=*/120));
    };

    const float in_peak = 0.5f;  // −6 dBFS
    const float r1 = settled_peak(1.0f);
    const float r2 = settled_peak(2.0f);
    const float r4 = settled_peak(4.0f);
    const float r20 = settled_peak(20.0f);
    auto db = [](float x) { return 20.0f * std::log10(std::max(x, 1e-9f)); };
    INFO("in=" << db(in_peak) << " dBFS  r1=" << db(r1) << " r2=" << db(r2)
               << " r4=" << db(r4) << " r20=" << db(r20) << " dBFS");

    // ratio 1 → bit-transparent (no reduction).
    CHECK(db(r1) == Catch::Approx(db(in_peak)).margin(0.5));
    // ratio 4 → the −15 dBFS claim (±2 dB for the peak-riding envelope).
    CHECK(db(r4) == Catch::Approx(-15.0f).margin(2.0f));
    // Reduction is monotonic in ratio — this ordering is the negative control:
    // if the ratio knob were ignored, all four peaks would be equal and it fails.
    CHECK(r1 > r2);
    CHECK(r2 > r4);
    CHECK(r4 > r20);
    // A loud input above threshold is measurably quieter at a musical ratio.
    CHECK(r4 < in_peak * 0.6f);  // > ~4.4 dB of gain reduction
}

// ── Compressor: a slow release pumps (gain reduction recovers over time) ──
TEST_CASE("Forge lo-fi: compressor slow release pumps on a burst-then-quiet input",
          "[host][baked][param-injection][forge][forge-lofi]") {
    // Drum-loop-shaped input: a loud burst opens deep gain reduction, then a quiet
    // below-threshold probe tone reveals the gain recovering. The number of blocks
    // the probe needs to climb back to half its recovered level measures the
    // release-driven pump: a slow release keeps the signal ducked far longer.
    auto recovery_blocks = [&](float release_ms) {
        BakedFixture fx(lofi::make_compressor_node());
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kCompThresholdDb, -18.0f)));
        REQUIRE(q.push(immediate(lofi::kCompRatio, 8.0f)));
        REQUIRE(q.push(immediate(lofi::kCompAttackMs, 5.0f)));
        REQUIRE(q.push(immediate(lofi::kCompReleaseMs, release_ms)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);

        // Engage: 12 blocks of a loud sine (amp 0.9, ~−0.9 dBFS) settles the
        // envelope deep into gain reduction.
        const auto burst = sine(kFrames, /*freq=*/500.0, /*amp=*/0.9f);
        settle(*fx.result.processor, burst, /*blocks=*/12);

        // Probe: a quiet sine (amp 0.1, −20 dBFS, below threshold → no reduction
        // of its own). Record its per-block RMS as the gain recovers.
        const auto probe = sine(kFrames, /*freq=*/500.0, /*amp=*/0.1f);
        std::vector<float> traj;
        for (int b = 0; b < 90; ++b) traj.push_back(rms(run_block(*fx.result.processor, probe)));
        const float recovered = traj.back();  // fully-recovered probe level
        for (int b = 0; b < static_cast<int>(traj.size()); ++b) {
            if (traj[static_cast<std::size_t>(b)] >= 0.5f * recovered) return b;
        }
        return static_cast<int>(traj.size());
    };

    const int fast = recovery_blocks(10.0f);
    const int slow = recovery_blocks(120.0f);
    INFO("recovery blocks: fast(10ms)=" << fast << " slow(120ms)=" << slow);
    CHECK(fast < 8);          // a fast release recovers almost immediately
    CHECK(slow > fast * 2);   // the slow release pumps — ducked far longer
    CHECK(slow < 90);         // but it does recover (not a stuck gain)
}

// ── Gate: below threshold is silenced, above threshold passes ─────────────
TEST_CASE("Forge lo-fi: gate attenuates below threshold and passes above it",
          "[host][baked][param-injection][forge][forge-lofi]") {
    BakedFixture fx(lofi::make_gate_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    pulp::state::ParameterEventQueue q;
    REQUIRE(q.push(immediate(lofi::kGateThresholdDb, -40.0f)));
    REQUIRE(q.push(immediate(lofi::kGateAttackMs, 1.0f)));
    REQUIRE(q.push(immediate(lofi::kGateHoldMs, 5.0f)));   // short hold: closes on the quiet tone
    REQUIRE(q.push(immediate(lofi::kGateReleaseMs, 40.0f)));
    REQUIRE(inj.inject(q) == InjectStatus::Ok);

    // Loud tone (amp 0.5, −6 dBFS, above −40) passes.
    const auto loud = sine(kFrames, /*freq=*/300.0, /*amp=*/0.5f);
    const float loud_rms = rms(settle(*fx.result.processor, loud, /*blocks=*/8));

    // Quiet tone (amp 0.006, ≈ −44 dBFS, below −40) is gated to near-silence.
    const auto quiet = sine(kFrames, /*freq=*/300.0, /*amp=*/0.006f);
    const float quiet_in = rms(quiet);
    const float quiet_rms = rms(settle(*fx.result.processor, quiet, /*blocks=*/80));

    INFO("loud in=" << rms(loud) << " out=" << loud_rms
                    << "  quiet in=" << quiet_in << " out=" << quiet_rms);
    CHECK(loud_rms > rms(loud) * 0.9f);       // above threshold passes ~unattenuated
    // Below threshold is silenced — this is the negative control: without wired
    // attenuation, out would equal in and this fails.
    CHECK(quiet_rms < quiet_in * 0.01f);      // > 40 dB of attenuation
}

// ── Gate: the hold time keeps the gate open after the key drops ───────────
TEST_CASE("Forge lo-fi: gate hold keeps a baked gate open through a quiet tail",
          "[host][baked][param-injection][forge][forge-lofi]") {
    // A loud burst opens the gate, then a quiet below-threshold tail follows. With
    // a long hold the tail passes for the hold window; with no hold the gate closes
    // immediately. Measure the tail RMS well inside the long-hold window.
    auto tail_after_burst = [&](float hold_ms) {
        BakedFixture fx(lofi::make_gate_node());
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kGateThresholdDb, -40.0f)));
        REQUIRE(q.push(immediate(lofi::kGateAttackMs, 1.0f)));
        REQUIRE(q.push(immediate(lofi::kGateHoldMs, hold_ms)));
        REQUIRE(q.push(immediate(lofi::kGateReleaseMs, 40.0f)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);

        // Open the gate with a loud burst (amp 0.5, above −40).
        const auto burst = sine(kFrames, /*freq=*/300.0, /*amp=*/0.5f);
        settle(*fx.result.processor, burst, /*blocks=*/6);

        // Quiet tail (amp 0.006, below −40). Average its RMS over blocks 5..20
        // (~13–53 ms) — inside a 200 ms hold, past a 40 ms release.
        const auto tail = sine(kFrames, /*freq=*/300.0, /*amp=*/0.006f);
        double acc = 0.0;
        int cnt = 0;
        for (int b = 0; b < 21; ++b) {
            const float r = rms(run_block(*fx.result.processor, tail));
            if (b >= 5) { acc += r; ++cnt; }
        }
        return static_cast<float>(acc / cnt);
    };

    const auto tail = sine(kFrames, /*freq=*/300.0, /*amp=*/0.006f);
    const float tail_in = rms(tail);
    const float open = tail_after_burst(200.0f);  // hold keeps it open
    const float closed = tail_after_burst(0.0f);  // no hold → closes at once
    INFO("tail in=" << tail_in << "  hold=200ms=" << open << "  hold=0=" << closed);
    CHECK(open > tail_in * 0.5f);   // held open — the quiet tail passes
    CHECK(open > closed * 5.0f);    // hold-dependent: the negative control for hold
    CHECK(closed < tail_in * 0.2f); // with no hold the tail is gated
}

// ── Boundedness: the dynamics nodes never expand, at extreme settings ─────
// Both nodes only ever apply a gain ≤ 1 (the compressor has no makeup; the gate
// floors at −80 dB), so a hot input can never diverge — the property that keeps
// the Forge runtime output guard transparent. (The guard itself lives in the
// Forge repo; here we prove the node output it wraps stays finite and bounded.)
TEST_CASE("Forge lo-fi: dynamics nodes keep output finite and bounded at extremes",
          "[host][baked][param-injection][forge][forge-lofi]") {
    const auto hot = sine(kFrames, /*freq=*/1000.0, /*amp=*/0.99f);
    const float hot_peak = peak(hot);

    {
        // Max-ratio compressor, lowest threshold, fastest attack.
        BakedFixture fx(lofi::make_compressor_node());
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kCompThresholdDb, -60.0f)));
        REQUIRE(q.push(immediate(lofi::kCompRatio, 20.0f)));
        REQUIRE(q.push(immediate(lofi::kCompAttackMs, 0.1f)));
        REQUIRE(q.push(immediate(lofi::kCompReleaseMs, 10.0f)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);
        const auto out = settle(*fx.result.processor, hot, /*blocks=*/40);
        for (float v : out) REQUIRE(std::isfinite(v));
        CHECK(peak(out) <= hot_peak + 1e-4f);  // never expands past the input
    }
    {
        // Gate at extremes: highest threshold, fastest attack, no hold.
        BakedFixture fx(lofi::make_gate_node());
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kGateThresholdDb, 0.0f)));
        REQUIRE(q.push(immediate(lofi::kGateAttackMs, 0.1f)));
        REQUIRE(q.push(immediate(lofi::kGateHoldMs, 0.0f)));
        REQUIRE(q.push(immediate(lofi::kGateReleaseMs, 10.0f)));
        REQUIRE(inj.inject(q) == InjectStatus::Ok);
        const auto out = settle(*fx.result.processor, hot, /*blocks=*/40);
        for (float v : out) REQUIRE(std::isfinite(v));
        CHECK(peak(out) <= hot_peak + 1e-4f);  // gain ≤ 1 always
    }
}

// ── RT safety: every node's inject + process path is allocation-free ─────
TEST_CASE("Forge lo-fi: inject + process is allocation-free for every catalog node",
          "[host][baked][param-injection][forge][forge-lofi][rt]") {
    struct Case {
        CustomNodeType type;
        int input_channels;
        pulp::state::ParamID knob;
        float value;
        int output_channels = 1;
    };
    std::vector<Case> cases;
    cases.push_back({lofi::make_delay_node(), 1, lofi::kDelayTimeMs, 120.0f});
    cases.push_back({lofi::make_filter_node(), 1, lofi::kFilterCutoffHz, 1200.0f});
    cases.push_back({lofi::make_filter_node(pulp::signal::Svf::Mode::highpass), 1,
                     lofi::kFilterResonance, 6.0f});
    cases.push_back({lofi::make_filter_node(pulp::signal::Svf::Mode::bandpass), 1,
                     lofi::kFilterResonance, 6.0f});
    cases.push_back({lofi::make_filter_node(pulp::signal::Svf::Mode::notch), 1,
                     lofi::kFilterResonance, 6.0f});
    cases.push_back({lofi::make_waveshaper_node(), 1, lofi::kWaveshaperDrive, 8.0f});
    cases.push_back({lofi::make_drywet_node(), 2, lofi::kDryWetMix, 0.5f});
    cases.push_back({lofi::make_noise_node(), 1, lofi::kNoiseLevel, 0.5f});
    cases.push_back({lofi::make_bitcrush_node(), 1, lofi::kBitcrushBitDepth, 6.0f});
    cases.push_back({lofi::make_trim_node(), 1, lofi::kTrimGainDb, -6.0f});
    cases.push_back({lofi::make_ping_pong_node(), 2, lofi::kPingPongTimeMs, 120.0f,
                     /*output_channels=*/2});
    cases.push_back({lofi::make_reverb_node(), 1, lofi::kReverbDecay, 1.0f});
    cases.push_back({lofi::make_compressor_node(), 1, lofi::kCompThresholdDb, -18.0f});
    cases.push_back({lofi::make_gate_node(), 1, lofi::kGateThresholdDb, -40.0f});
    cases.push_back({lofi::make_lfo_node(), 1, lofi::kLfoRateHz, 2.0f});
    cases.push_back({lofi::make_vca_node(), 2, lofi::kVcaGain, 1.0f});
    cases.push_back({lofi::make_env_follower_node(), 1, lofi::kEnvAttackMs, 10.0f});
    cases.push_back({lofi::make_filter_cv_node(), 2, lofi::kFilterCvBaseHz, 800.0f});
    cases.push_back({lofi::make_delay_cv_node(), 2, lofi::kDelayCvBaseMs, 12.0f});
    cases.push_back({lofi::make_auto_pan_node(), 2, lofi::kAutoPanDepth, 1.0f,
                     /*output_channels=*/2});
    cases.push_back({lofi::make_width_node(), 2, lofi::kWidthAmount, 1.0f,
                     /*output_channels=*/2});
    cases.push_back({lofi::make_phaser_node(), 1, lofi::kPhaserDepth, 0.7f});

    for (auto& c : cases) {
        BakedFixture fx(c.type, c.input_channels, c.output_channels);
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());

        // Pre-allocate all render buffers/views OUTSIDE the probe.
        std::vector<std::vector<float>> in_channels(
            static_cast<std::size_t>(c.input_channels),
            std::vector<float>(kFrames, 0.1f));
        std::vector<const float*> in_ptrs(in_channels.size());
        for (std::size_t ch = 0; ch < in_channels.size(); ++ch)
            in_ptrs[ch] = in_channels[ch].data();
        std::vector<std::vector<float>> out_channels(
            static_cast<std::size_t>(c.output_channels),
            std::vector<float>(kFrames, 0.0f));
        std::vector<float*> out_ptrs(out_channels.size());
        for (std::size_t ch = 0; ch < out_channels.size(); ++ch)
            out_ptrs[ch] = out_channels[ch].data();
        pulp::audio::BufferView<const float> in_view(
            in_ptrs.data(), static_cast<std::uint32_t>(c.input_channels),
            static_cast<std::uint32_t>(kFrames));
        pulp::audio::BufferView<float> out_view(
            out_ptrs.data(), static_cast<std::uint32_t>(c.output_channels),
            static_cast<std::uint32_t>(kFrames));
        pulp::midi::MidiBuffer midi_in, midi_out;
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = kSr;
        ctx.num_samples = kFrames;

        // Warm up so first-call lazy paths are primed.
        REQUIRE(inj.inject(immediate(c.knob, c.value)) == InjectStatus::Ok);
        fx.result.processor->process(out_view, in_view, midi_in, midi_out, ctx);

        // Snapshot the counters the instant the render window ends — BEFORE the
        // INFO below, whose lazy string build would otherwise allocate inside the
        // still-live probe and be misattributed to the node.
        pulp::test::RtAllocationProbe probe;
        REQUIRE(inj.inject(immediate(c.knob, c.value * 0.5f)) == InjectStatus::Ok);
        fx.result.processor->process(out_view, in_view, midi_in, midi_out, ctx);
        fx.result.processor->process(out_view, in_view, midi_in, midi_out, ctx);
        const auto alloc_count = probe.allocation_count();
        const auto alloc_bytes = probe.allocated_bytes();
        INFO("node type: " << c.type.type_id);
        CHECK(alloc_count == 0);
        CHECK(alloc_bytes == 0);
    }
}

// ═════════════════════════════════════════════════════════════════════════
// CV primitive pack — control-signal-as-audio-port composition proofs.
//
// The unlock: an lfo/env_follower emits a UNIPOLAR control signal on its audio
// output; a vca/filter_cv/delay_cv reads that control on a dedicated CV INPUT
// PORT. Modulation is ordinary graph topology, so tremolo/auto-wah/chorus/pump
// are COMPOSITIONS. These tests measure the composed effect numerically and pair
// each with a negative control that swaps the CV source for a DC constant — if
// the CV port were not load-bearing the positive case would look identical.
// ═════════════════════════════════════════════════════════════════════════

namespace {

// Bake + prepare an already-wired graph; returns the LowerResult (owns the
// baked Processor). in_ch/out_ch are the descriptor bus arity.
LowerResult bake_and_prepare(SignalGraph& g, int in_ch, int out_ch) {
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    LowerResult r = bake(g);
    REQUIRE(r.accepted);
    REQUIRE(r.processor);
    REQUIRE(r.reason == LowerRejectReason::None);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr;
    pc.max_buffer_size = kFrames;
    pc.input_channels = in_ch;
    pc.output_channels = out_ch;
    r.processor->prepare(pc);
    return r;
}

BakedGraphProcessor& as_baked(LowerResult& r) {
    return *static_cast<BakedGraphProcessor*>(r.processor.get());
}

// Render `total` samples in kFrames blocks, feeding `feed` (one vector per input
// channel, each >= total), returning the concatenated mono output.
std::vector<float> run_stream(pulp::format::Processor& proc,
                              const std::vector<std::vector<float>>& feed, int total) {
    const int nch = static_cast<int>(feed.size());
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(total));
    std::vector<std::vector<float>> blk(static_cast<std::size_t>(nch),
                                        std::vector<float>(kFrames, 0.0f));
    for (int start = 0; start < total; start += kFrames) {
        for (int c = 0; c < nch; ++c)
            for (int k = 0; k < kFrames; ++k) {
                const int idx = start + k;
                blk[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] =
                    idx < total ? feed[static_cast<std::size_t>(c)][static_cast<std::size_t>(idx)] : 0.0f;
            }
        const auto o = run_block(proc, blk);
        const int n = std::min(kFrames, total - start);
        for (int k = 0; k < n; ++k) out.push_back(o[static_cast<std::size_t>(k)]);
    }
    return out;
}

std::vector<float> dc(int n, float v) { return std::vector<float>(static_cast<std::size_t>(n), v); }

bool inj_ok(ParamInjector& inj, pulp::state::ParameterEventQueue& q) {
    return inj.inject(q) == InjectStatus::Ok;
}

// Settle a stateful node fed a multi-channel block; returns the last block.
std::vector<float> settle_multi(pulp::format::Processor& proc,
                                const std::vector<std::vector<float>>& feed, int blocks = 8) {
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b) out = run_block(proc, feed);
    return out;
}

// Dominant frequency (Hz) in [fmin, fmax]: argmax of a Goertzel magnitude sweep
// over the mean-removed signal. Accurate for a clean periodic control signal.
float dominant_freq(const std::vector<float>& x, double sr, double fmin, double fmax,
                    double step = 0.05) {
    double mean = 0.0;
    for (float v : x) mean += v;
    mean /= static_cast<double>(x.size());
    const int N = static_cast<int>(x.size());
    const double kTwoPi = 6.28318530717958647692;
    double best_f = fmin, best_mag = -1.0;
    for (double f = fmin; f <= fmax; f += step) {
        double re = 0.0, im = 0.0;
        const double w = kTwoPi * f / sr;
        for (int k = 0; k < N; ++k) {
            const double s = x[static_cast<std::size_t>(k)] - mean;
            re += s * std::cos(w * k);
            im -= s * std::sin(w * k);
        }
        const double mag = re * re + im * im;
        if (mag > best_mag) { best_mag = mag; best_f = f; }
    }
    return static_cast<float>(best_f);
}

float vmin(const std::vector<float>& x) { return *std::min_element(x.begin(), x.end()); }
float vmax(const std::vector<float>& x) { return *std::max_element(x.begin(), x.end()); }

// Goertzel magnitude of `x` at frequency `f`.
float goertzel_mag(const std::vector<float>& x, double sr, double f) {
    const double kTwoPi = 6.28318530717958647692;
    const double w = kTwoPi * f / sr;
    double re = 0.0, im = 0.0;
    for (int k = 0; k < static_cast<int>(x.size()); ++k) {
        re += x[static_cast<std::size_t>(k)] * std::cos(w * k);
        im -= x[static_cast<std::size_t>(k)] * std::sin(w * k);
    }
    return static_cast<float>(std::sqrt(re * re + im * im));
}

// Spectral centroid (Hz) over a fixed set of partials, from their Goertzel
// magnitudes — robust and level-independent (unlike a coarse linear-bin sweep,
// which under-weights a sub-bin fundamental).
float partials_centroid(const std::vector<float>& x, double sr,
                        const std::vector<double>& partials) {
    double num = 0.0, den = 0.0;
    for (double f : partials) {
        const double m = goertzel_mag(x, sr, f);
        num += f * m;
        den += m;
    }
    return den > 0.0 ? static_cast<float>(num / den) : 0.0f;
}


}  // namespace

// ── LFO smoke: a 0-input custom source bakes and oscillates ───────────────
// Directly answers the composition-doc's unverified item: executor behavior for
// a 0-input custom node. Bakes `lfo → out`, renders 1 s, asserts a [0,1] control
// signal oscillating at the injected rate.
TEST_CASE("Forge CV: a 0-input LFO source bakes and emits a rate-accurate control signal",
          "[host][baked][param-injection][forge][forge-cv]") {
    SignalGraph g;
    const auto lfo_type = lofi::make_lfo_node();
    REQUIRE(g.register_custom_node_type(lfo_type));
    const auto lfo = g.add_custom_node(lofi::kLfoTypeId, 1, "LFO");
    REQUIRE(lfo != 0);
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(lfo, 0, out, 0));
    LowerResult r = bake_and_prepare(g, /*in_ch=*/1, /*out_ch=*/1);

    ParamInjector inj = as_baked(r).claim_param_injection(lfo);
    REQUIRE(inj.valid());
    pulp::state::ParameterEventQueue q;
    REQUIRE(q.push(immediate(lofi::kLfoRateHz, 5.0f)));
    REQUIRE(q.push(immediate(lofi::kLfoDepth, 1.0f)));
    REQUIRE(q.push(immediate(lofi::kLfoShape, 0.0f)));  // sine
    REQUIRE(inj.inject(q) == InjectStatus::Ok);

    const int total = static_cast<int>(kSr);  // 1 second
    const auto y = run_stream(*r.processor, {dc(total, 0.0f)}, total);
    const float f = dominant_freq(y, kSr, 1.0, 12.0);
    INFO("lfo est freq=" << f << " min=" << vmin(y) << " max=" << vmax(y));
    CHECK(f == Catch::Approx(5.0f).margin(0.25f));  // 5 ±0.25 Hz
    CHECK(vmin(y) >= 0.0f);                          // unipolar
    CHECK(vmax(y) <= 1.0f);
    CHECK(vmax(y) - vmin(y) > 0.9f);                 // full-depth swing
}

// ── Tremolo: lfo → vca produces measurable amplitude modulation ───────────
TEST_CASE("Forge CV: tremolo (lfo → vca) amplitude-modulates at the LFO rate",
          "[host][baked][param-injection][forge][forge-cv]") {
    const auto lfo_type = lofi::make_lfo_node();
    const auto vca_type = lofi::make_vca_node();

    // Populate `g` with the tremolo graph; returns the LFO node id (the CV source).
    auto build = [&](SignalGraph& g, bool cv_wired) {
        REQUIRE(g.register_custom_node_type(lfo_type));
        REQUIRE(g.register_custom_node_type(vca_type));
        const auto in = g.add_input_node(2, "In");   // ch0 = signal, ch1 = DC key
        const auto lfo = g.add_custom_node(lofi::kLfoTypeId, 1, "LFO");
        const auto vca = g.add_custom_node(lofi::kVcaTypeId, 1, "VCA");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, vca, 0));           // signal → VCA.0
        if (cv_wired)
            REQUIRE(g.connect(lfo, 0, vca, 1));      // LFO CV → VCA.1  (tremolo)
        else
            REQUIRE(g.connect(in, 1, vca, 1));       // DC → VCA.1      (control)
        REQUIRE(g.connect(vca, 0, out, 0));
        return lfo;
    };

    const int total = static_cast<int>(kSr);  // 1 s
    // Carrier: DC 1.0 on ch0 so the VCA output IS the gain envelope; ch1 = DC 1.0.
    const std::vector<std::vector<float>> feed{dc(total, 1.0f), dc(total, 1.0f)};

    // Positive: LFO drives the CV port.
    SignalGraph gp;
    const auto lfo_p = build(gp, /*cv_wired=*/true);
    LowerResult rp = bake_and_prepare(gp, 2, 1);
    {
        ParamInjector li = as_baked(rp).claim_param_injection(lfo_p);
        REQUIRE(li.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kLfoRateHz, 5.0f)));
        REQUIRE(q.push(immediate(lofi::kLfoDepth, 1.0f)));
        REQUIRE(inj_ok(li, q));
    }
    const auto yp = run_stream(*rp.processor, feed, total);
    const float fp = dominant_freq(yp, kSr, 1.0, 12.0);
    const float depth_index = (vmax(yp) - vmin(yp)) / (vmax(yp) + vmin(yp) + 1e-9f);
    INFO("tremolo freq=" << fp << " AM index=" << depth_index
                         << " min=" << vmin(yp) << " max=" << vmax(yp));
    CHECK(fp == Catch::Approx(5.0f).margin(0.25f));  // AM at the LFO rate
    CHECK(depth_index > 0.9f);                        // deep tremolo (near 100%)

    // Negative control: same graph, CV port fed a DC constant → no modulation.
    SignalGraph gn;
    build(gn, /*cv_wired=*/false);
    LowerResult rn = bake_and_prepare(gn, 2, 1);
    const auto yn = run_stream(*rn.processor, feed, total);
    INFO("no-CV control: min=" << vmin(yn) << " max=" << vmax(yn));
    CHECK(vmax(yn) - vmin(yn) < 0.01f);  // flat — the CV port is load-bearing
}

// ── Auto-wah: env_follower → filter_cv cutoff tracks input level ──────────
TEST_CASE("Forge CV: auto-wah (env_follower → filter_cv) centroid tracks input level",
          "[host][baked][param-injection][forge][forge-cv]") {
    const auto env_type = lofi::make_env_follower_node();
    const auto filt_type = lofi::make_filter_cv_node();

    // A bright source (broadband-ish: sum of harmonics) so the moving cutoff
    // shifts the spectral centroid measurably.
    auto bright = [&](int n, float amp) {
        std::vector<float> v(static_cast<std::size_t>(n), 0.0f);
        for (int k = 0; k < n; ++k) {
            const double t = static_cast<double>(k) / kSr;
            v[static_cast<std::size_t>(k)] = amp * static_cast<float>(
                (std::sin(2 * M_PI * 200 * t) + std::sin(2 * M_PI * 1200 * t) +
                 std::sin(2 * M_PI * 4000 * t)) / 3.0);
        }
        return v;
    };

    auto centroid_at = [&](float amp, bool env_wired) {
        SignalGraph g;
        REQUIRE(g.register_custom_node_type(env_type));
        REQUIRE(g.register_custom_node_type(filt_type));
        const auto in = g.add_input_node(2, "In");  // ch0 = signal, ch1 = DC-0 key
        const auto env = g.add_custom_node(lofi::kEnvFollowerTypeId, 1, "Env");
        const auto filt = g.add_custom_node(lofi::kFilterCvTypeId, 1, "Filt");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, filt, 0));          // signal → filter.0
        if (env_wired) {
            REQUIRE(g.connect(in, 0, env, 0));       // signal → env
            REQUIRE(g.connect(env, 0, filt, 1));     // env CV → filter cutoff
        } else {
            REQUIRE(g.connect(in, 1, filt, 1));      // DC 0 → cutoff CV (base only)
        }
        REQUIRE(g.connect(filt, 0, out, 0));
        LowerResult r = bake_and_prepare(g, 2, 1);
        {   // A low base with a clean (near-Butterworth) lowpass: quiet sits at
            // ~200 Hz, a loud envelope opens ~4.5 octaves above it.
            ParamInjector fi = as_baked(r).claim_param_injection(filt);
            REQUIRE(fi.valid());
            pulp::state::ParameterEventQueue q;
            REQUIRE(q.push(immediate(lofi::kFilterCvBaseHz, 400.0f)));
            REQUIRE(q.push(immediate(lofi::kFilterCvAmountOct, 4.0f)));
            REQUIRE(q.push(immediate(lofi::kFilterCvResonance, 1.0f)));
            REQUIRE(inj_ok(fi, q));
        }
        if (env_wired) {
            ParamInjector ei = as_baked(r).claim_param_injection(env);
            REQUIRE(ei.valid());
            pulp::state::ParameterEventQueue q;
            REQUIRE(q.push(immediate(lofi::kEnvAttackMs, 5.0f)));
            REQUIRE(q.push(immediate(lofi::kEnvReleaseMs, 60.0f)));
            REQUIRE(q.push(immediate(lofi::kEnvSensitivity, 1.5f)));
            REQUIRE(inj_ok(ei, q));
        }
        const int total = static_cast<int>(kSr / 2);  // 0.5 s, let ballistics settle
        const std::vector<std::vector<float>> feed{bright(total, amp), dc(total, 0.0f)};
        const auto y = run_stream(*r.processor, feed, total);
        // Centroid over the settled tail (skip the attack transient), measured
        // at the source's three partials — a clean, level-independent readout of
        // where the moving cutoff sits.
        const std::vector<float> tail(y.end() - 8192, y.end());
        return partials_centroid(tail, kSr, {200.0, 1200.0, 4000.0});
    };

    const float quiet = centroid_at(0.04f, /*env_wired=*/true);
    const float loud  = centroid_at(0.9f,  /*env_wired=*/true);
    INFO("auto-wah centroid quiet=" << quiet << " loud=" << loud
                                    << " octaves=" << std::log2(loud / quiet));
    CHECK(loud > quiet * 2.0f);   // >= 1 octave brighter when driven loud

    // Negative control: no env edge → cutoff stays at base → centroid ~level-independent.
    const float flat_quiet = centroid_at(0.04f, /*env_wired=*/false);
    const float flat_loud  = centroid_at(0.9f,  /*env_wired=*/false);
    INFO("no-env control quiet=" << flat_quiet << " loud=" << flat_loud);
    CHECK(flat_loud < flat_quiet * 1.4f);  // barely moves without the CV edge
}

// ── Node-level CV port proofs (CV fed from an input channel, deterministic) ──

// VCA: gain CV on port 1 scales the signal on port 0.
TEST_CASE("Forge CV: VCA multiplies its signal by the port-1 gain CV",
          "[host][baked][param-injection][forge][forge-cv]") {
    BakedFixture fx(lofi::make_vca_node(), /*input_channels=*/2);
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());
    REQUIRE(inj.inject(immediate(lofi::kVcaGain, 1.0f)) == InjectStatus::Ok);

    const std::vector<float> sig(kFrames, 0.5f);
    const std::vector<float> cv_zero(kFrames, 0.0f);
    const std::vector<float> cv_one(kFrames, 1.0f);
    const auto off = run_block(*fx.result.processor, {sig, cv_zero});
    const auto on = run_block(*fx.result.processor, {sig, cv_one});
    for (float v : off) CHECK(v == Catch::Approx(0.0f).margin(1e-6));   // CV 0 → silence
    for (float v : on) CHECK(v == Catch::Approx(0.5f).margin(1e-6));    // CV 1 → passthrough
}

// filter_cv: cutoff CV on port 1 opens the lowpass (a high tone passes more).
TEST_CASE("Forge CV: filter_cv cutoff CV on port 1 sweeps the lowpass",
          "[host][baked][param-injection][forge][forge-cv]") {
    BakedFixture fx(lofi::make_filter_cv_node(), /*input_channels=*/2);
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());
    pulp::state::ParameterEventQueue q;
    REQUIRE(q.push(immediate(lofi::kFilterCvBaseHz, 300.0f)));
    REQUIRE(q.push(immediate(lofi::kFilterCvAmountOct, 5.0f)));  // CV 1 → ~9.6 kHz
    REQUIRE(q.push(immediate(lofi::kFilterCvResonance, 1.0f)));
    REQUIRE(inj_ok(inj, q));

    const auto tone = sine(kFrames, 6000.0, 0.8f);  // above the base cutoff
    const std::vector<float> cv_lo(kFrames, 0.0f);  // cutoff = base (300 Hz)
    const std::vector<float> cv_hi(kFrames, 1.0f);  // cutoff opens ~5 oct
    const float closed = rms(settle_multi(*fx.result.processor, {tone, cv_lo}));
    const float open = rms(settle_multi(*fx.result.processor, {tone, cv_hi}));
    INFO("filter_cv closed=" << closed << " open=" << open);
    CHECK(open > closed * 3.0f);   // the CV opens the filter for the 6 kHz tone
}

// delay_cv: time CV on port 1 moves the read tap (echo latency changes).
TEST_CASE("Forge CV: delay_cv time CV on port 1 moves the delay tap",
          "[host][baked][param-injection][forge][forge-cv]") {
    auto latency_for = [&](float cv_val) {
        BakedFixture fx(lofi::make_delay_cv_node(), /*input_channels=*/2);
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());
        pulp::state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(lofi::kDelayCvBaseMs, 5.0f)));
        REQUIRE(q.push(immediate(lofi::kDelayCvDepthMs, 20.0f)));  // CV 1 → +20 ms
        REQUIRE(q.push(immediate(lofi::kDelayCvFeedback, 0.0f)));
        REQUIRE(q.push(immediate(lofi::kDelayCvMix, 1.0f)));       // fully wet
        REQUIRE(inj_ok(inj, q));

        // Impulse on ch0; constant CV on ch1. Find the wet-tap peak sample index.
        std::vector<float> impulse(kFrames, 0.0f);
        impulse[0] = 1.0f;
        const std::vector<float> cv(kFrames, cv_val);
        // Render enough blocks to cover a 25 ms tap (~1200 samples).
        std::vector<float> stream;
        const std::vector<std::vector<float>> first{impulse, cv};
        auto o = run_block(*fx.result.processor, first);
        stream.insert(stream.end(), o.begin(), o.end());
        for (int b = 0; b < 12; ++b) {
            o = run_block(*fx.result.processor, {std::vector<float>(kFrames, 0.0f), cv});
            stream.insert(stream.end(), o.begin(), o.end());
        }
        int peak_idx = 0;
        float peak_v = 0.0f;
        for (int k = 0; k < static_cast<int>(stream.size()); ++k)
            if (std::fabs(stream[static_cast<std::size_t>(k)]) > peak_v) {
                peak_v = std::fabs(stream[static_cast<std::size_t>(k)]);
                peak_idx = k;
            }
        return peak_idx;
    };

    const int lat_lo = latency_for(0.0f);  // ~5 ms  → ~240 samples
    const int lat_hi = latency_for(1.0f);  // ~25 ms → ~1200 samples
    INFO("delay_cv tap lo=" << lat_lo << " hi=" << lat_hi << " samples");
    CHECK(lat_hi > lat_lo + 500);  // the injected time CV moved the tap far later
}

// ── env_follower invert: a loud input DUCKS the CV (pump primitive) ───────
TEST_CASE("Forge CV: env_follower invert emits a ducking CV for pumping",
          "[host][baked][param-injection][forge][forge-cv]") {
    BakedFixture fx(lofi::make_env_follower_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());
    pulp::state::ParameterEventQueue q;
    REQUIRE(q.push(immediate(lofi::kEnvAttackMs, 5.0f)));
    REQUIRE(q.push(immediate(lofi::kEnvReleaseMs, 60.0f)));
    REQUIRE(q.push(immediate(lofi::kEnvSensitivity, 1.0f)));
    REQUIRE(q.push(immediate(lofi::kEnvInvert, 1.0f)));  // duck
    REQUIRE(inj_ok(inj, q));

    // Quiet first from a fresh instance (env = 0 → CV = 1); then a loud input
    // ducks it. 60 silent blocks (160 ms) >> the 60 ms release, so the quiet
    // measurement is fully settled regardless of order.
    const auto quiet = settle(*fx.result.processor, dc(kFrames, 0.0f), 60);
    const auto loud = settle(*fx.result.processor, sine(kFrames, 200.0, 0.9f), 40);
    const float loud_cv = rms(loud);
    const float quiet_cv = rms(quiet);
    INFO("inverted env: loud_cv=" << loud_cv << " quiet_cv=" << quiet_cv);
    CHECK(quiet_cv > 0.9f);          // silence → CV near 1 (full gain)
    CHECK(loud_cv < quiet_cv * 0.5f);  // loud input ducks the CV down
}
