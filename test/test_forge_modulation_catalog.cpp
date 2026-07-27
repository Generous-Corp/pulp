// Forge modulation catalog — bake-layer param-injection tests.
//
// The companion to test_forge_lofi_catalog.cpp, over the nodes in
// forge_modulation_catalog.hpp. Same bar and same fixture shape: for each node,
// a CONTROL-THREAD injection of its macro knob changes the BAKED node's output
// over the real production path (bake() -> BakedGraphProcessor ->
// claim_param_injection -> ParamInjector -> routed executor -> ParamCursor), with
// no re-bake.
//
//   * mod_lfo   — rate injection changes the control signal's cycle count; the
//                 delay lifecycle holds the output at the unipolar neutral 0.5
//                 for exactly the delay; the random waveforms are deterministic
//                 across two identical bakes.
//   * mod_lpg   — a struck cell decays; a re-strike mid-decay lands louder than
//                 a cold one; colour selects between VCA-lean and filter-lean;
//                 and the node never boosts.
//   * mod_slew  — a step at the input takes the injected rise time to arrive.
//   * mod_transient — the same gesture 24 dB apart produces the same CV, which
//                 an env_follower cannot do.
//   * mod_env   — a rising edge on the control input fires a shape whose length
//                 is the injected attack + hold + decay.
//
// Plus an RT-allocation probe over every node's process path.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_modulation_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp::host;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace mod_cat = pulp::host::forge_modulation;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;

// Bake `in(input_channels) -> custom -> out(out_channels)`, wiring one input
// port per custom input and one output port per custom output, then prepare the
// baked processor. Same shape as the lo-fi catalog fixture.
struct BakedFixture {
    SignalGraph g;
    LowerResult result;
    NodeId custom_node = 0;

    explicit BakedFixture(const CustomNodeType& type, int input_channels = 1,
                          int out_channels = 1) {
        REQUIRE(g.register_custom_node_type(type));
        const auto in = g.add_input_node(std::max(1, input_channels), "In");
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
        pc.input_channels = std::max(1, input_channels);
        pc.output_channels = out_channels;
        result.processor->prepare(pc);
    }

    BakedGraphProcessor& baked() {
        return *static_cast<BakedGraphProcessor*>(result.processor.get());
    }
};

std::vector<float> run_block(pulp::format::Processor& proc,
                             const std::vector<std::vector<float>>& in_channels) {
    const auto num_ch = static_cast<std::uint32_t>(in_channels.size());
    std::vector<const float*> in_ptrs(in_channels.size());
    for (std::size_t c = 0; c < in_channels.size(); ++c)
        in_ptrs[c] = in_channels[c].data();

    std::vector<float> output(static_cast<std::size_t>(kFrames), 0.0f);
    float* out_ptr = output.data();

    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), num_ch,
                                                 static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(&out_ptr, 1u, static_cast<std::uint32_t>(kFrames));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;
    proc.process(out_view, in_view, midi_in, midi_out, ctx);
    return output;
}

std::vector<float> run_block(pulp::format::Processor& proc, const std::vector<float>& mono) {
    return run_block(proc, std::vector<std::vector<float>>{mono});
}

/// Render `blocks` blocks of the same input and concatenate the output.
std::vector<float> render(pulp::format::Processor& proc,
                          const std::vector<std::vector<float>>& in_channels, int blocks) {
    std::vector<float> all;
    all.reserve(static_cast<std::size_t>(blocks * kFrames));
    for (int b = 0; b < blocks; ++b) {
        const auto out = run_block(proc, in_channels);
        all.insert(all.end(), out.begin(), out.end());
    }
    return all;
}

std::vector<float> constant(float value) {
    return std::vector<float>(static_cast<std::size_t>(kFrames), value);
}

std::vector<float> silence() {
    return constant(0.0f);
}

float rms(const std::vector<float>& b) {
    if (b.empty())
        return 0.0f;
    double sum = 0.0;
    for (float v : b)
        sum += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(sum / static_cast<double>(b.size())));
}

float peak(const std::vector<float>& b) {
    float m = 0.0f;
    for (float v : b)
        m = std::max(m, std::fabs(v));
    return m;
}

/// Crude spectral-centroid proxy: the RMS of the first difference, normalized by
/// the RMS of the signal. Enough to say "this is brighter than that".
float brightness(const std::vector<float>& b) {
    if (b.size() < 2)
        return 0.0f;
    std::vector<float> difference;
    difference.reserve(b.size() - 1);
    for (std::size_t i = 1; i < b.size(); ++i)
        difference.push_back(b[i] - b[i - 1]);
    return rms(difference) / std::max(rms(b), 1.0e-9f);
}

int upward_crossings(const std::vector<float>& b, float level) {
    int count = 0;
    for (std::size_t i = 1; i < b.size(); ++i)
        if (b[i - 1] <= level && b[i] > level)
            ++count;
    return count;
}

pulp::state::ParameterEvent immediate(pulp::state::ParamID id, float value,
                                      std::int32_t offset = 0) {
    return {id, offset, value, /*ramp_duration_sample_frames=*/0};
}

/// Inject one macro value onto the fixture's custom node.
void inject(BakedFixture& fixture, pulp::state::ParamID id, float value) {
    auto injector = fixture.baked().claim_param_injection(fixture.custom_node);
    REQUIRE(injector.valid());
    injector.inject(immediate(id, value));
}

/// Noise excitation with a fixed seed, so a render is reproducible.
std::vector<float> seeded_noise(std::uint32_t& state) {
    std::vector<float> v(static_cast<std::size_t>(kFrames), 0.0f);
    for (auto& sample : v) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        sample = static_cast<float>(state >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
    return v;
}

} // namespace

// ── mod_lfo ──────────────────────────────────────────────────────────────────

TEST_CASE("mod_lfo rate injection changes the control signal's cycle count",
          "[forge][catalog][mod]") {
    // 3000 blocks is 8 s, long enough that the partial cycle at the end is a
    // rounding detail rather than the measurement.
    constexpr int kBlocks = 3000;
    constexpr double kSeconds = kBlocks * kFrames / kSr;

    auto cycles_at = [](float rate_hz) {
        BakedFixture fixture(mod_cat::make_mod_lfo_node(), /*input_channels=*/1);
        inject(fixture, mod_cat::kModLfoRateHz, rate_hz);
        inject(fixture, mod_cat::kModLfoDepth, 1.0f);
        const auto cv = render(*fixture.result.processor, {silence()}, kBlocks);
        return upward_crossings(cv, 0.5f);
    };

    for (float rate : {2.0f, 8.0f}) {
        const int measured = cycles_at(rate);
        const int expected = static_cast<int>(static_cast<double>(rate) * kSeconds);
        REQUIRE(std::abs(measured - expected) <= 1);
    }
}

TEST_CASE("mod_lfo output is a unipolar control signal", "[forge][catalog][mod]") {
    BakedFixture fixture(mod_cat::make_mod_lfo_node());
    inject(fixture, mod_cat::kModLfoRateHz, 5.0f);
    inject(fixture, mod_cat::kModLfoDepth, 1.0f);
    const auto cv = render(*fixture.result.processor, {silence()}, 200);
    for (float x : cv) {
        REQUIRE(x >= 0.0f);
        REQUIRE(x <= 1.0f);
    }
    REQUIRE(*std::max_element(cv.begin(), cv.end()) > 0.95f);
    REQUIRE(*std::min_element(cv.begin(), cv.end()) < 0.05f);

    // Depth 0 parks it at the neutral midpoint rather than at zero, which is
    // what "no modulation" means for a unipolar destination.
    BakedFixture flat(mod_cat::make_mod_lfo_node());
    inject(flat, mod_cat::kModLfoDepth, 0.0f);
    for (float x : render(*flat.result.processor, {silence()}, 20))
        REQUIRE_THAT(x, WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("mod_lfo delay holds the neutral output for exactly the delay", "[forge][catalog][mod]") {
    BakedFixture fixture(mod_cat::make_mod_lfo_node());
    inject(fixture, mod_cat::kModLfoRateHz, 10.0f);
    inject(fixture, mod_cat::kModLfoDepth, 1.0f);
    inject(fixture, mod_cat::kModLfoDelayMs, 100.0f); // 4800 samples

    const auto cv = render(*fixture.result.processor, {silence()}, 200);
    // Everything inside the delay is the unipolar neutral value.
    for (int i = 0; i < 4800; ++i)
        REQUIRE_THAT(cv[static_cast<std::size_t>(i)], WithinAbs(0.5f, 1e-6f));
    // And it moves shortly after, once the sine leaves its own zero crossing.
    const std::vector<float> after(cv.begin() + 4800, cv.begin() + 6000);
    REQUIRE(*std::max_element(after.begin(), after.end()) > 0.9f);
}

TEST_CASE("mod_lfo lifecycle controls apply at their exact injected sample offset",
          "[forge][catalog][mod]") {
    BakedFixture fixture(mod_cat::make_mod_lfo_node());
    auto injector =
        fixture.baked().claim_param_injection(fixture.custom_node);
    REQUIRE(injector.valid());

    pulp::state::ParameterEventQueue events;
    REQUIRE(events.push(immediate(mod_cat::kModLfoRateHz, 2000.0f, 0)));
    REQUIRE(events.push(immediate(mod_cat::kModLfoDepth, 1.0f, 0)));
    REQUIRE(events.push(immediate(mod_cat::kModLfoDelayMs, 100.0f, 0)));
    REQUIRE(events.push(immediate(mod_cat::kModLfoDelayMs, 0.0f, 64)));
    REQUIRE(injector.inject(events) == InjectStatus::Ok);

    const auto cv = run_block(*fixture.result.processor, silence());
    for (int i = 0; i <= 64; ++i)
        REQUIRE_THAT(cv[static_cast<std::size_t>(i)],
                     WithinAbs(0.5f, 1.0e-6f));
    // The first active sample evaluates the sine at its zero crossing; the
    // following sample proves that phase advancement resumed in this block.
    REQUIRE_THAT(cv[65], WithinAbs(0.5f, 1.0e-6f));
    REQUIRE(cv[66] > 0.6f);
}

TEST_CASE("mod_lfo quadratic fade is distinct from linear fade",
          "[forge][catalog][mod]") {
    auto faded = [](bool quadratic) {
        BakedFixture fixture(mod_cat::make_mod_lfo_node());
        inject(fixture, mod_cat::kModLfoRateHz, 10.0f);
        inject(fixture, mod_cat::kModLfoDepth, 1.0f);
        inject(fixture, mod_cat::kModLfoWave, 4.0f); // positive square
        inject(fixture, mod_cat::kModLfoFadeInMs, 100.0f);
        inject(fixture, mod_cat::kModLfoFadeQuadratic,
               quadratic ? 1.0f : 0.0f);
        return render(*fixture.result.processor, {silence()}, 10);
    };

    const auto linear = faded(false);
    const auto quadratic = faded(true);
    REQUIRE(linear[1200] > quadratic[1200] + 0.05f);
    REQUIRE_THAT(linear[1200], WithinAbs(0.625f, 0.002f));
    REQUIRE_THAT(quadratic[1200], WithinAbs(0.53125f, 0.002f));
}

TEST_CASE("mod_lfo random waveforms bake deterministically", "[forge][catalog][mod]") {
    auto render_random = [](float wave_id) {
        BakedFixture fixture(mod_cat::make_mod_lfo_node());
        inject(fixture, mod_cat::kModLfoRateHz, 12.0f);
        inject(fixture, mod_cat::kModLfoDepth, 1.0f);
        inject(fixture, mod_cat::kModLfoWave, wave_id);
        return render(*fixture.result.processor, {silence()}, 100);
    };

    // 5 = sample-and-hold random, 6 = smooth random.
    for (float wave : {5.0f, 6.0f}) {
        const auto a = render_random(wave);
        const auto b = render_random(wave);
        REQUIRE(a == b);
        // A random waveform that never moves is not one.
        REQUIRE(*std::max_element(a.begin(), a.end()) - *std::min_element(a.begin(), a.end()) >
                0.3f);
    }

    // The two random waves are genuinely different shapes: sample-and-hold
    // steps, smooth random glides.
    const auto stepped = render_random(5.0f);
    const auto glided = render_random(6.0f);
    REQUIRE(brightness(stepped) > brightness(glided));
}

TEST_CASE("mod_lfo injects morph, triangle bias, and phase", "[forge][catalog][mod]") {
    auto lfo_render = [](float wave, float morph, bool morph_enabled, float triangle_bias,
                         float phase_degrees) {
        BakedFixture fixture(mod_cat::make_mod_lfo_node());
        inject(fixture, mod_cat::kModLfoRateHz, 10.0f);
        inject(fixture, mod_cat::kModLfoDepth, 1.0f);
        inject(fixture, mod_cat::kModLfoWave, wave);
        inject(fixture, mod_cat::kModLfoShapeMorph, morph);
        inject(fixture, mod_cat::kModLfoMorphEnabled, morph_enabled ? 1.0f : 0.0f);
        inject(fixture, mod_cat::kModLfoTriangleBias, triangle_bias);
        inject(fixture, mod_cat::kModLfoPhaseDegrees, phase_degrees);
        return render(*fixture.result.processor, {silence()}, 40);
    };

    const auto pure_triangle = lfo_render(1.0f, 0.0f, false, 0.0f, 0.0f);
    const auto morphed_triangle = lfo_render(0.0f, 1.0f, true, 0.0f, 0.0f);
    REQUIRE(pure_triangle == morphed_triangle);

    const auto biased_triangle = lfo_render(1.0f, 0.0f, false, 0.8f, 0.0f);
    const auto pure_peak =
        std::distance(pure_triangle.begin(),
                      std::max_element(pure_triangle.begin(), pure_triangle.begin() + 4800));
    const auto biased_peak =
        std::distance(biased_triangle.begin(),
                      std::max_element(biased_triangle.begin(), biased_triangle.begin() + 4800));
    REQUIRE(biased_peak > pure_peak);

    const auto unshifted_sine = lfo_render(0.0f, 0.0f, false, 0.0f, 0.0f);
    const auto quarter_phase = lfo_render(0.0f, 0.0f, false, 0.0f, 90.0f);
    REQUIRE_THAT(unshifted_sine.front(), WithinAbs(0.5f, 1.0e-6f));
    REQUIRE_THAT(quarter_phase.front(), WithinAbs(1.0f, 1.0e-6f));
}

TEST_CASE("mod_lfo random segments and finite fade-out change its lifecycle",
          "[forge][catalog][mod]") {
    auto smooth_random = [](float segments) {
        BakedFixture fixture(mod_cat::make_mod_lfo_node());
        inject(fixture, mod_cat::kModLfoRateHz, 8.0f);
        inject(fixture, mod_cat::kModLfoDepth, 1.0f);
        inject(fixture, mod_cat::kModLfoWave, 6.0f);
        inject(fixture, mod_cat::kModLfoRandomSegments, segments);
        return render(*fixture.result.processor, {silence()}, 100);
    };

    const auto one_segment = smooth_random(1.0f);
    const auto eight_segments = smooth_random(8.0f);
    std::vector<float> difference(one_segment.size());
    for (std::size_t i = 0; i < difference.size(); ++i)
        difference[i] = one_segment[i] - eight_segments[i];
    REQUIRE(rms(difference) > 0.05f);

    BakedFixture finite(mod_cat::make_mod_lfo_node());
    inject(finite, mod_cat::kModLfoRateHz, 20.0f);
    inject(finite, mod_cat::kModLfoDepth, 1.0f);
    inject(finite, mod_cat::kModLfoRepeatCount, 1.0f);
    inject(finite, mod_cat::kModLfoFadeOutMs, 25.0f);
    const auto one_shot = render(*finite.result.processor, {silence()}, 40);

    REQUIRE(peak(one_shot) > 0.9f);
    for (auto it = one_shot.end() - kFrames; it != one_shot.end(); ++it)
        REQUIRE_THAT(*it, WithinAbs(0.5f, 1.0e-5f));
}

// ── mod_lpg ──────────────────────────────────────────────────────────────────

TEST_CASE("mod_lpg gated mode opens and closes with its control", "[forge][catalog][mod]") {
    BakedFixture fixture(mod_cat::make_mod_lpg_node(), /*input_channels=*/2);
    inject(fixture, mod_cat::kModLpgColour, 0.0f); // pure VCA, filter wide open

    std::uint32_t seed = 12345u;
    const auto excitation = seeded_noise(seed);

    // Closed.
    const auto closed = render(*fixture.result.processor, {excitation, constant(0.0f)}, 20);
    // Open.
    const auto open = render(*fixture.result.processor, {excitation, constant(1.0f)}, 40);
    const std::vector<float> settled(open.end() - 1280, open.end());

    REQUIRE(rms(closed) < 0.01f * rms(settled));
    REQUIRE(rms(settled) > 0.1f);
}

TEST_CASE("mod_lpg struck mode pings, decays, and accumulates on a roll", "[forge][catalog][mod]") {
    BakedFixture fixture(mod_cat::make_mod_lpg_node(), /*input_channels=*/2);
    inject(fixture, mod_cat::kModLpgStruck, 1.0f);
    inject(fixture, mod_cat::kModLpgDecayMs, 150.0f);
    inject(fixture, mod_cat::kModLpgColour, 0.5f);

    std::uint32_t seed = 777u;
    const auto excitation = seeded_noise(seed);
    const auto low = constant(0.0f);
    const auto high = constant(1.0f);

    // One strike is a block of high CV followed by silence on the control port.
    auto strike_and_measure = [&](int quiet_blocks) {
        (void)run_block(*fixture.result.processor, {excitation, high});
        std::vector<float> tail;
        for (int b = 0; b < quiet_blocks; ++b) {
            const auto out = run_block(*fixture.result.processor, {excitation, low});
            tail.insert(tail.end(), out.begin(), out.end());
        }
        return tail;
    };

    // A cold strike, then two more 30 ms apart (about 11 blocks of 128).
    const auto cold = strike_and_measure(11);
    const auto second = strike_and_measure(11);
    const auto third = strike_and_measure(11);

    // The ping decays within its own tail...
    const std::vector<float> head(cold.begin(), cold.begin() + 256);
    const std::vector<float> foot(cold.end() - 256, cold.end());
    REQUIRE(rms(foot) < rms(head));

    // ...and a re-strike into a still-conducting cell lands louder than a cold
    // one, which is the roll behaviour the vactrol model exists for.
    REQUIRE(peak(second) > peak(cold));
    REQUIRE(peak(third) > peak(second));
}

TEST_CASE("mod_lpg struck velocity follows the control's peak, not its threshold crossing",
          "[forge][catalog][mod]") {
    // Sampling the control once at the instant it crosses the strike threshold
    // reads roughly the threshold for ANY source with a finite rise, so a
    // slewed or transient-derived control would lose its velocity entirely —
    // which is exactly the `slew -> lpg` and `transient -> lpg` wiring this node
    // is for. A hit that ramps to full must land close to one that steps there.
    auto struck_peak = [](int rise_blocks) {
        BakedFixture fixture(mod_cat::make_mod_lpg_node(), /*input_channels=*/2);
        inject(fixture, mod_cat::kModLpgStruck, 1.0f);
        inject(fixture, mod_cat::kModLpgColour, 0.0f);
        inject(fixture, mod_cat::kModLpgDecayMs, 400.0f);

        std::uint32_t seed = 606u;
        const auto excitation = seeded_noise(seed);
        float highest = 0.0f;
        for (int b = 0; b < 30; ++b) {
            float level = 1.0f;
            if (rise_blocks > 0 && b < rise_blocks)
                level = static_cast<float>(b + 1) / static_cast<float>(rise_blocks);
            const auto out = run_block(*fixture.result.processor, {excitation, constant(level)});
            highest = std::max(highest, peak(out));
        }
        return highest;
    };

    const float stepped = struck_peak(0); // instant control
    const float ramped = struck_peak(4);  // ~10 ms rise, as a slew would give

    REQUIRE(stepped > 0.1f);
    // Before the peak-tracking fix the ramped hit came back at roughly a third
    // of the stepped one, because it struck at the threshold and coasted.
    REQUIRE(ramped > 0.8f * stepped);
}

TEST_CASE("mod_lpg colour trades amplitude for brightness", "[forge][catalog][mod]") {
    auto render_at_colour = [](float colour, float control) {
        BakedFixture fixture(mod_cat::make_mod_lpg_node(), /*input_channels=*/2);
        inject(fixture, mod_cat::kModLpgColour, colour);
        std::uint32_t seed = 4242u;
        const auto excitation = seeded_noise(seed);
        return render(*fixture.result.processor, {excitation, constant(control)}, 60);
    };

    // Half open. Colour 0 attenuates without touching the spectrum; colour 1
    // leaves the gain stage alone and takes the top off instead. Both lose
    // level — a lowpass on broadband material always does — but only one of them
    // changes the timbre, which is the whole point of the control.
    const auto vca_lean = render_at_colour(0.0f, 0.5f);
    const auto filter_lean = render_at_colour(1.0f, 0.5f);
    REQUIRE(brightness(filter_lean) < 0.7f * brightness(vca_lean));

    // And colour 0 is exactly the amplitude law, with the filter sitting at its
    // 12 kHz ceiling: a 1 kHz tone comes back at control^1.5 and nothing else.
    // (It is not transparent to *white noise* — a 12 kHz ceiling always costs a
    // broadband source its top octave — which is why this measures a tone.)
    BakedFixture tone_fixture(mod_cat::make_mod_lpg_node(), /*input_channels=*/2);
    inject(tone_fixture, mod_cat::kModLpgColour, 0.0f);
    std::vector<float> tone(static_cast<std::size_t>(kFrames), 0.0f);
    double phase = 0.0;
    const double step = 2.0 * 3.14159265358979323846 * 1000.0 / kSr;
    std::vector<float> measured;
    for (int b = 0; b < 60; ++b) {
        for (auto& sample : tone) {
            sample = static_cast<float>(std::sin(phase));
            phase += step;
        }
        const auto block = run_block(*tone_fixture.result.processor, {tone, constant(0.5f)});
        if (b >= 40)
            measured.insert(measured.end(), block.begin(), block.end());
    }
    const float expected_gain = std::pow(0.5f, 1.5f);
    REQUIRE_THAT(rms(measured) * std::sqrt(2.0f), WithinRel(expected_gain, 0.01f));
}

TEST_CASE("mod_lpg never boosts", "[forge][catalog][mod]") {
    for (float colour : {0.0f, 0.5f, 1.0f}) {
        BakedFixture fixture(mod_cat::make_mod_lpg_node(), /*input_channels=*/2);
        inject(fixture, mod_cat::kModLpgColour, colour);
        std::uint32_t seed = 99u;
        const auto excitation = seeded_noise(seed);
        const auto out = render(*fixture.result.processor, {excitation, constant(1.0f)}, 60);
        REQUIRE(peak(out) <= peak(excitation) + 1e-4f);
    }
}

TEST_CASE("mod_lpg never boosts at full brightness near Nyquist", "[forge][catalog][mod]") {
    // Adversarial version of the noise probe above: an open cell at the
    // brightness ceiling, full-scale Nyquist-rate alternation to pump the
    // filter's integrator state, then a DC step to read that state out. The
    // cell caps its commanded cutoff at sample_rate / 4, which is what keeps
    // the declared unity worst-case gain true.
    BakedFixture fixture(mod_cat::make_mod_lpg_node(), /*input_channels=*/2);
    inject(fixture, mod_cat::kModLpgColour, 1.0f);
    inject(fixture, mod_cat::kModLpgBrightnessHz, 18000.0f);

    std::vector<float> alternating(static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i)
        alternating[static_cast<std::size_t>(i)] = (i % 2 == 0) ? 1.0f : -1.0f;

    (void)render(*fixture.result.processor, {silence(), constant(1.0f)}, 40); // open the cell
    const auto pumped = render(*fixture.result.processor, {alternating, constant(1.0f)}, 8);
    const auto exposed = render(*fixture.result.processor, {constant(1.0f), constant(1.0f)}, 2);
    REQUIRE(peak(pumped) <= 1.0f + 1e-4f);
    REQUIRE(peak(exposed) <= 1.0f + 1e-4f);
}

TEST_CASE("mod_lpg rise and darkness injections change the gated response",
          "[forge][catalog][mod]") {
    auto gated = [](float rise_ms, float darkness_hz, float control,
                    const std::vector<float>& excitation) {
        BakedFixture fixture(mod_cat::make_mod_lpg_node(), /*input_channels=*/2);
        inject(fixture, mod_cat::kModLpgColour, control > 0.0f ? 0.0f : 1.0f);
        inject(fixture, mod_cat::kModLpgRiseMs, rise_ms);
        inject(fixture, mod_cat::kModLpgDarknessHz, darkness_hz);
        return render(*fixture.result.processor, {excitation, constant(control)}, 20);
    };

    const auto fast = gated(0.05f, 40.0f, 1.0f, constant(1.0f));
    const auto slow = gated(100.0f, 40.0f, 1.0f, constant(1.0f));
    REQUIRE(rms(std::vector<float>(fast.begin(), fast.begin() + kFrames)) >
            4.0f * rms(std::vector<float>(slow.begin(), slow.begin() + kFrames)));

    // 375 Hz is exactly one cycle per block at this fixture's 48 kHz / 128
    // geometry. It is high enough to distinguish a 40 Hz floor from a 500 Hz
    // floor, but unlike Nyquist alternation it exercises the filter's stable
    // in-band response rather than its guarded edge.
    std::vector<float> tone(static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i)
        tone[static_cast<std::size_t>(i)] =
            std::sin(6.2831853071795864769f * static_cast<float>(i) / static_cast<float>(kFrames));
    const auto dark = gated(2.0f, 40.0f, 0.0f, tone);
    const auto open = gated(2.0f, 500.0f, 0.0f, tone);
    const std::vector<float> dark_settled(dark.end() - 4 * kFrames, dark.end());
    const std::vector<float> open_settled(open.end() - 4 * kFrames, open.end());
    REQUIRE(rms(open_settled) > 4.0f * rms(dark_settled));
}

TEST_CASE("mod_lpg strike threshold and refractory injections gate repeated hits",
          "[forge][catalog][mod]") {
    auto struck = [](float threshold, float refractory_ms, float pulse_level, int blocks) {
        BakedFixture fixture(mod_cat::make_mod_lpg_node(), /*input_channels=*/2);
        inject(fixture, mod_cat::kModLpgStruck, 1.0f);
        inject(fixture, mod_cat::kModLpgColour, 0.0f);
        inject(fixture, mod_cat::kModLpgRiseMs, 0.05f);
        inject(fixture, mod_cat::kModLpgDecayMs, 400.0f);
        inject(fixture, mod_cat::kModLpgStrikeThreshold, threshold);
        inject(fixture, mod_cat::kModLpgRefractoryMs, refractory_ms);
        auto pulse = constant(0.0f);
        pulse.front() = pulse_level;
        return render(*fixture.result.processor, {constant(1.0f), pulse}, blocks);
    };

    REQUIRE(peak(struck(0.3f, 0.0f, 0.5f, 1)) > 0.01f);
    REQUIRE(peak(struck(0.7f, 0.0f, 0.5f, 1)) < 1.0e-6f);

    const auto repeated = struck(0.3f, 0.0f, 1.0f, 10);
    const auto suppressed = struck(0.3f, 100.0f, 1.0f, 10);
    REQUIRE(peak(std::vector<float>(repeated.end() - kFrames, repeated.end())) >
            peak(std::vector<float>(suppressed.end() - kFrames, suppressed.end())));
}

// ── mod_slew ─────────────────────────────────────────────────────────────────

TEST_CASE("mod_slew linear rise takes exactly the injected time", "[forge][catalog][mod]") {
    BakedFixture fixture(mod_cat::make_mod_slew_node());
    inject(fixture, mod_cat::kModSlewRiseMs, 10.0f); // 480 samples
    inject(fixture, mod_cat::kModSlewFallMs, 10.0f);
    inject(fixture, mod_cat::kModSlewCurved, 0.0f);

    // Establish a resting value first. The node adopts its first control sample
    // after a reset rather than ramping to it from zero, so the ramp under test
    // has to be a step the limiter actually sees as a change — measuring from
    // render start would measure the adopt, not the slew.
    (void)render(*fixture.result.processor, {constant(0.0f)}, 1);

    const auto ramp = render(*fixture.result.processor, {constant(1.0f)}, 20);
    // The step arrives 480 samples after the input does, within a sample.
    int arrived = -1;
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        if (ramp[i] >= 0.999f) {
            arrived = static_cast<int>(i);
            break;
        }
    }
    REQUIRE(arrived >= 478);
    REQUIRE(arrived <= 482);
    // And it is a ramp, not a step: the midpoint is halfway.
    REQUIRE_THAT(ramp[240], WithinAbs(0.5f, 0.01f));
}

TEST_CASE("mod_slew starts at its control, not at zero", "[forge][catalog][mod]") {
    // A control signal's resting value is 0.5, not 0. Resetting the limiter to
    // zero made every render open with a rise-time ramp from silence up to
    // whatever the control already was — a start-of-playback modulation
    // transient in any `... -> slew -> CV port` chain, and a long one at a long
    // rise time.
    BakedFixture fixture(mod_cat::make_mod_slew_node());
    inject(fixture, mod_cat::kModSlewRiseMs, 1000.0f); // a long ramp makes it obvious
    inject(fixture, mod_cat::kModSlewFallMs, 1000.0f);

    // A control already sitting at 0.8 when the render begins.
    const auto out = render(*fixture.result.processor, {constant(0.8f)}, 8);
    for (float x : out)
        REQUIRE_THAT(x, WithinAbs(0.8f, 1e-6f));

    // The limiter still limits once it is running: a step away from the adopted
    // value is slewed, not followed.
    const auto stepped = render(*fixture.result.processor, {constant(0.0f)}, 1);
    REQUIRE(stepped.front() < 0.8f);
    REQUIRE(stepped.back() > 0.7f); // 128 samples of a 1 s ramp is a small move
}

TEST_CASE("mod_slew fall time is independent of rise time", "[forge][catalog][mod]") {
    BakedFixture fixture(mod_cat::make_mod_slew_node());
    inject(fixture, mod_cat::kModSlewRiseMs, 1.0f);   // fast up
    inject(fixture, mod_cat::kModSlewFallMs, 100.0f); // slow down

    (void)render(*fixture.result.processor, {constant(1.0f)}, 10);
    const auto falling = render(*fixture.result.processor, {constant(0.0f)}, 10);
    // 100 ms is 4800 samples; 1280 samples in it is only a quarter of the way.
    REQUIRE(falling.back() > 0.6f);
}

// ── mod_transient ────────────────────────────────────────────────────────────

TEST_CASE("mod_transient output does not depend on input level", "[forge][catalog][mod]") {
    auto detect = [](float scale) {
        BakedFixture fixture(mod_cat::make_mod_transient_node());
        std::uint32_t seed = 31337u;
        std::vector<float> out;
        for (int b = 0; b < 60; ++b) {
            auto excitation = seeded_noise(seed);
            // A burst every 16 blocks.
            const float envelope = (b % 16 < 2) ? 1.0f : 0.05f;
            for (auto& sample : excitation)
                sample *= scale * envelope;
            const auto block = run_block(*fixture.result.processor, {excitation});
            out.insert(out.end(), block.begin(), block.end());
        }
        return out;
    };

    const auto loud = detect(0.5f);
    const auto quiet = detect(0.0316f); // 24 dB down
    REQUIRE(peak(loud) > 0.2f);
    for (std::size_t i = 0; i < loud.size(); ++i)
        REQUIRE_THAT(quiet[i], WithinAbs(loud[i], 1e-3f));
}

TEST_CASE("mod_transient invert produces a ducking control", "[forge][catalog][mod]") {
    auto detect = [](float invert) {
        BakedFixture fixture(mod_cat::make_mod_transient_node());
        inject(fixture, mod_cat::kModTransientInvert, invert);
        std::uint32_t seed = 5150u;
        std::vector<float> out;
        for (int b = 0; b < 40; ++b) {
            auto excitation = seeded_noise(seed);
            const float envelope = (b % 16 < 2) ? 1.0f : 0.05f;
            for (auto& sample : excitation)
                sample *= 0.5f * envelope;
            const auto block = run_block(*fixture.result.processor, {excitation});
            out.insert(out.end(), block.begin(), block.end());
        }
        return out;
    };

    const auto normal = detect(0.0f);
    const auto ducking = detect(1.0f);
    for (std::size_t i = 0; i < normal.size(); ++i)
        REQUIRE_THAT(ducking[i], WithinAbs(1.0f - normal[i], 1e-5f));
}

// ── mod_env ──────────────────────────────────────────────────────────────────

TEST_CASE("mod_env fires on a rising control edge and runs its full shape",
          "[forge][catalog][mod]") {
    BakedFixture fixture(mod_cat::make_mod_env_node());
    inject(fixture, mod_cat::kModEnvAttackMs, 10.0f); // 480
    inject(fixture, mod_cat::kModEnvHoldMs, 10.0f);   // 480
    inject(fixture, mod_cat::kModEnvDecayMs, 10.0f);  // 480 -> 1440 total
    inject(fixture, mod_cat::kModEnvThreshold, 0.3f);

    // One block of a high control, then quiet.
    const auto fired = run_block(*fixture.result.processor, {constant(1.0f)});
    const auto tail = render(*fixture.result.processor, {constant(0.0f)}, 20);

    std::vector<float> all(fired);
    all.insert(all.end(), tail.begin(), tail.end());

    REQUIRE(*std::max_element(all.begin(), all.end()) > 0.99f);
    // The shape is over 1440 samples after the edge, and stays over.
    for (std::size_t i = 1500; i < all.size(); ++i)
        REQUIRE_THAT(all[i], WithinAbs(0.0f, 1e-5f));
    REQUIRE(all[1400] > 0.0f);
}

TEST_CASE("mod_env ignores a control that never crosses the threshold", "[forge][catalog][mod]") {
    BakedFixture fixture(mod_cat::make_mod_env_node());
    inject(fixture, mod_cat::kModEnvThreshold, 0.8f);
    const auto out = render(*fixture.result.processor, {constant(0.5f)}, 20);
    for (float x : out)
        REQUIRE_THAT(x, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("mod_env delay, depth, and velocity are independently injectable",
          "[forge][catalog][mod]") {
    auto shaped = [](float delay_ms, float depth, bool velocity_sensitive, float trigger_level) {
        BakedFixture fixture(mod_cat::make_mod_env_node());
        inject(fixture, mod_cat::kModEnvDelayMs, delay_ms);
        inject(fixture, mod_cat::kModEnvAttackMs, 1.0f);
        inject(fixture, mod_cat::kModEnvHoldMs, 5.0f);
        inject(fixture, mod_cat::kModEnvDecayMs, 5.0f);
        inject(fixture, mod_cat::kModEnvThreshold, 0.3f);
        inject(fixture, mod_cat::kModEnvDepth, depth);
        inject(fixture, mod_cat::kModEnvVelocitySensitive, velocity_sensitive ? 1.0f : 0.0f);
        return render(*fixture.result.processor, {constant(trigger_level)}, 12);
    };

    const auto delayed = shaped(10.0f, 1.0f, false, 0.5f);
    for (int i = 0; i < 480; ++i)
        REQUIRE_THAT(delayed[static_cast<std::size_t>(i)], WithinAbs(0.0f, 1.0e-7f));
    REQUIRE(peak(std::vector<float>(delayed.begin() + 480, delayed.end())) > 0.9f);

    const auto half_depth = shaped(0.0f, 0.5f, false, 0.5f);
    const auto velocity = shaped(0.0f, 1.0f, true, 0.5f);
    REQUIRE_THAT(peak(half_depth), WithinAbs(0.5f, 1.0e-4f));
    REQUIRE_THAT(peak(velocity), WithinAbs(0.5f, 1.0e-4f));
}

TEST_CASE("mod_env can unlink its attack and decay curves",
          "[forge][catalog][mod]") {
    auto shaped = [](bool independent, float attack_curve, float decay_curve) {
        BakedFixture fixture(mod_cat::make_mod_env_node());
        inject(fixture, mod_cat::kModEnvAttackMs, 10.0f);
        inject(fixture, mod_cat::kModEnvHoldMs, 0.0f);
        inject(fixture, mod_cat::kModEnvDecayMs, 10.0f);
        inject(fixture, mod_cat::kModEnvCurve, 0.0f);
        inject(fixture, mod_cat::kModEnvIndependentCurves,
               independent ? 1.0f : 0.0f);
        inject(fixture, mod_cat::kModEnvAttackCurve, attack_curve);
        inject(fixture, mod_cat::kModEnvDecayCurve, decay_curve);
        const auto fired =
            run_block(*fixture.result.processor, {constant(1.0f)});
        auto tail = render(*fixture.result.processor, {constant(0.0f)}, 8);
        tail.insert(tail.begin(), fired.begin(), fired.end());
        return tail;
    };

    const auto linked = shaped(false, 1.0f, -1.0f);
    const auto attack_shaped = shaped(true, 1.0f, 0.0f);
    const auto decay_shaped = shaped(true, 0.0f, -1.0f);

    REQUIRE(rms(std::vector<float>(linked.begin(), linked.begin() + 480))
            != rms(std::vector<float>(attack_shaped.begin(),
                                      attack_shaped.begin() + 480)));
    REQUIRE(rms(std::vector<float>(linked.begin() + 480,
                                   linked.begin() + 960))
            != rms(std::vector<float>(decay_shaped.begin() + 480,
                                      decay_shaped.begin() + 960)));
}

TEST_CASE("mod_env loop and refractory injections control repeated shapes",
          "[forge][catalog][mod]") {
    auto repeated = [](bool loop, int loop_count, float refractory_ms, int blocks) {
        BakedFixture fixture(mod_cat::make_mod_env_node());
        inject(fixture, mod_cat::kModEnvAttackMs, 0.1f);
        inject(fixture, mod_cat::kModEnvHoldMs, 0.0f);
        inject(fixture, mod_cat::kModEnvDecayMs, 1.0f);
        inject(fixture, mod_cat::kModEnvThreshold, 0.3f);
        inject(fixture, mod_cat::kModEnvLoop, loop ? 1.0f : 0.0f);
        inject(fixture, mod_cat::kModEnvLoopCount, static_cast<float>(loop_count));
        inject(fixture, mod_cat::kModEnvRefractoryMs, refractory_ms);
        auto pulse = constant(0.0f);
        pulse.front() = 1.0f;
        return render(*fixture.result.processor, {pulse}, blocks);
    };

    const auto one_shot = repeated(false, 0, 100.0f, 2);
    const auto three_loops = repeated(true, 3, 100.0f, 2);
    REQUIRE(peak(std::vector<float>(one_shot.begin() + kFrames, one_shot.end())) < 1.0e-6f);
    REQUIRE(peak(std::vector<float>(three_loops.begin() + kFrames, three_loops.end())) > 0.1f);

    const auto retriggered = repeated(false, 0, 0.0f, 8);
    const auto refractory = repeated(false, 0, 100.0f, 8);
    REQUIRE(peak(std::vector<float>(retriggered.end() - kFrames, retriggered.end())) > 0.1f);
    REQUIRE(peak(std::vector<float>(refractory.end() - kFrames, refractory.end())) < 1.0e-6f);
}

// ── RT safety ────────────────────────────────────────────────────────────────

TEST_CASE("Modulation catalog nodes are allocation-free in process",
          "[forge][catalog][mod][rt-safety]") {
    struct Case {
        CustomNodeType type;
        int input_channels;
    };
    std::vector<Case> cases;
    cases.push_back({mod_cat::make_mod_lfo_node(), 1});
    cases.push_back({mod_cat::make_mod_lpg_node(), 2});
    cases.push_back({mod_cat::make_mod_slew_node(), 1});
    cases.push_back({mod_cat::make_mod_transient_node(), 1});
    cases.push_back({mod_cat::make_mod_env_node(), 1});

    for (auto& c : cases) {
        BakedFixture fixture(c.type, c.input_channels);
        std::uint32_t seed = 8u;
        std::vector<std::vector<float>> in;
        for (int ch = 0; ch < std::max(1, c.type.num_input_ports); ++ch)
            in.push_back(seeded_noise(seed));

        // Every buffer and pointer array the render needs is allocated BEFORE
        // the probe arms. The convenience `run_block()` builds two vectors per
        // call, which would be counted against the node under test.
        std::vector<const float*> in_ptrs(in.size());
        for (std::size_t ch = 0; ch < in.size(); ++ch)
            in_ptrs[ch] = in[ch].data();
        std::vector<float> output(static_cast<std::size_t>(kFrames), 0.0f);
        float* out_ptr = output.data();
        pulp::audio::BufferView<const float> in_view(in_ptrs.data(),
                                                     static_cast<std::uint32_t>(in.size()),
                                                     static_cast<std::uint32_t>(kFrames));
        pulp::audio::BufferView<float> out_view(&out_ptr, 1u, static_cast<std::uint32_t>(kFrames));
        pulp::midi::MidiBuffer midi_in, midi_out;
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = kSr;
        ctx.num_samples = kFrames;

        auto& proc = *fixture.result.processor;
        auto render_one = [&] { proc.process(out_view, in_view, midi_in, midi_out, ctx); };

        // Settle first so the probe sees the steady state, not first-touch.
        for (int b = 0; b < 4; ++b)
            render_one();

        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 8; ++b)
            render_one();
        REQUIRE(probe.allocation_count() == 0);
    }
}
