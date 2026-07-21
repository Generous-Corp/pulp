// Forge lo-fi DSP catalog — bake-layer param-injection tests.
//
// For each catalog node (forge_lofi_catalog.hpp) this proves that a CONTROL-
// THREAD injection of its macro knob changes the BAKED node's audio output,
// without re-baking, over the real production path (bake() → BakedGraphProcessor
// → claim_param_injection → ParamInjector → routed executor → ParamCursor). It is
// the F2-b sibling of test_baked_graph_param_injection.cpp (F2-a), reusing the
// same fixture shape:
//
//   * Filter  — a low cutoff attenuates a high-frequency tone (spectral change).
//   * Waveshaper — higher drive saturates a sine (RMS/peak grow: more THD).
//   * Dry/Wet — mix crossfades a dry vs a wet constant (dry/wet balance).
//   * Noise   — level raises the noise floor of a silent input.
//   * Bitcrush — bit_depth raises quantization error; sample_rate_reduction
//                introduces sample-and-hold plateaus.
//
// Plus one RT-allocation probe over every node's process path.

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

// Bake a graph `in(input_channels) → custom → out(1)`, wiring one input-node
// port to each of the custom node's input ports, and prepare the baked
// processor. Mirrors the F2-a BakedFixture but supports a multi-port node.
struct BakedFixture {
    SignalGraph g;
    LowerResult result;
    NodeId custom_node = 0;

    explicit BakedFixture(const CustomNodeType& type, int input_channels = 1) {
        REQUIRE(g.register_custom_node_type(type));
        const auto in = g.add_input_node(input_channels, "In");
        custom_node = g.add_custom_node(type.type_id, 1, "Node");
        const auto out = g.add_output_node(1, "Out");
        for (int port = 0; port < type.num_input_ports; ++port) {
            REQUIRE(g.connect(in, static_cast<PortIndex>(port), custom_node,
                              static_cast<PortIndex>(port)));
        }
        REQUIRE(g.connect(custom_node, 0, out, 0));
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
        pc.output_channels = 1;
        result.processor->prepare(pc);
    }

    BakedGraphProcessor& baked() {
        return *static_cast<BakedGraphProcessor*>(result.processor.get());
    }
};

// Render one block of a multi-channel input through `proc`; returns mono output.
std::vector<float> run_block(pulp::format::Processor& proc,
                             const std::vector<std::vector<float>>& in_channels) {
    const auto num_ch = static_cast<std::uint32_t>(in_channels.size());
    std::vector<const float*> in_ptrs(in_channels.size());
    for (std::size_t c = 0; c < in_channels.size(); ++c) in_ptrs[c] = in_channels[c].data();

    std::vector<float> output(static_cast<std::size_t>(kFrames), 0.0f);
    float* out_ptr = output.data();

    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), num_ch,
                                                 static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(&out_ptr, 1, static_cast<std::uint32_t>(kFrames));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;
    proc.process(out_view, in_view, midi_in, midi_out, ctx);
    return output;
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

// ── RT safety: every node's inject + process path is allocation-free ─────
TEST_CASE("Forge lo-fi: inject + process is allocation-free for every catalog node",
          "[host][baked][param-injection][forge][forge-lofi][rt]") {
    struct Case {
        CustomNodeType type;
        int input_channels;
        pulp::state::ParamID knob;
        float value;
    };
    std::vector<Case> cases;
    cases.push_back({lofi::make_delay_node(), 1, lofi::kDelayTimeMs, 120.0f});
    cases.push_back({lofi::make_filter_node(), 1, lofi::kFilterCutoffHz, 1200.0f});
    cases.push_back({lofi::make_waveshaper_node(), 1, lofi::kWaveshaperDrive, 8.0f});
    cases.push_back({lofi::make_drywet_node(), 2, lofi::kDryWetMix, 0.5f});
    cases.push_back({lofi::make_noise_node(), 1, lofi::kNoiseLevel, 0.5f});
    cases.push_back({lofi::make_bitcrush_node(), 1, lofi::kBitcrushBitDepth, 6.0f});

    for (auto& c : cases) {
        BakedFixture fx(c.type, c.input_channels);
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        REQUIRE(inj.valid());

        // Pre-allocate all render buffers/views OUTSIDE the probe.
        std::vector<std::vector<float>> in_channels(
            static_cast<std::size_t>(c.input_channels),
            std::vector<float>(kFrames, 0.1f));
        std::vector<const float*> in_ptrs(in_channels.size());
        for (std::size_t ch = 0; ch < in_channels.size(); ++ch)
            in_ptrs[ch] = in_channels[ch].data();
        std::vector<float> output(kFrames, 0.0f);
        float* out_ptr = output.data();
        pulp::audio::BufferView<const float> in_view(
            in_ptrs.data(), static_cast<std::uint32_t>(c.input_channels),
            static_cast<std::uint32_t>(kFrames));
        pulp::audio::BufferView<float> out_view(&out_ptr, 1,
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
