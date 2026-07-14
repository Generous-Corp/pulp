#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/polyphase_block_oversampler.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ────────────────────────────────────────────────────────────────────────
// PolyphaseBlockOversamplerT — block-oriented, multi-channel oversampling
// that exposes the intermediate oversampled buffer between the up and
// down passes.
//
// The test that matters most here is "joint-channel work between up and
// down is possible" (below): it does something OversamplerT's fused
// per-sample callback API genuinely cannot express, because two
// independent OversamplerT instances each finish their whole N-sample loop
// before returning, so channel R's oversampled sample at index i doesn't
// exist while channel L's callback for index i is running.
// ────────────────────────────────────────────────────────────────────────

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

float trailing_rms(const std::vector<float>& samples, std::size_t skip) {
    if (samples.size() <= skip)
        return 0.0f;
    double acc = 0.0;
    const std::size_t n = samples.size() - skip;
    for (std::size_t i = skip; i < samples.size(); ++i)
        acc += static_cast<double>(samples[i]) * samples[i];
    return static_cast<float>(std::sqrt(acc / static_cast<double>(n)));
}

float to_db(float lin) {
    return 20.0f * std::log10(std::max(lin, 1e-30f));
}

std::vector<float> sine(float cycles_per_sample, std::size_t n, float phase = 0.0f) {
    std::vector<float> out(n);
    const float w = 2.0f * kPi * cycles_per_sample;
    for (std::size_t i = 0; i < n; ++i)
        out[i] = std::sin(w * static_cast<float>(i) + phase);
    return out;
}

// Deterministic pseudo-random sequence in [-1, 1), fixed seed -- same
// generator halfband_iir's own white-noise tests use.
std::vector<float> pseudo_noise(std::size_t n, uint32_t seed) {
    std::vector<float> out(n);
    uint32_t state = seed;
    for (std::size_t i = 0; i < n; ++i) {
        state = state * 1103515245u + 12345u;
        out[i] = static_cast<int32_t>(state) / 2147483648.0f;
    }
    return out;
}

} // namespace

TEST_CASE("PolyphaseBlockOversampler up/down round-trip recovers a passband signal",
          "[signal][oversampling][block]") {
    constexpr std::size_t kN = 2048;
    constexpr std::size_t kChannels = 2;
    PolyphaseBlockOversampler oversampler;
    oversampler.prepare(kChannels, /*num_stages=*/2, kN); // x4
    REQUIRE(oversampler.oversampling_factor() == 4);

    const auto left = sine(0.05f, kN);
    const auto right = sine(0.07f, kN);
    const float* inputs[kChannels] = {left.data(), right.data()};

    auto view = oversampler.process_up(inputs, kChannels, kN);
    REQUIRE(view.get_num_samples() == kN * 4);

    std::vector<float> out_left(kN, 0.0f), out_right(kN, 0.0f);
    float* outputs[kChannels] = {out_left.data(), out_right.data()};
    oversampler.process_down(outputs, kChannels, kN);

    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        const auto& in = ch == 0 ? left : right;
        const auto& out = ch == 0 ? out_left : out_right;
        const float in_rms = trailing_rms(in, kN / 4);
        const float out_rms = trailing_rms(out, kN / 4);
        const float gain_db = to_db(out_rms / in_rms);
        INFO("channel=" << ch << " round-trip gain_db=" << gain_db);
        REQUIRE(gain_db < 0.5f);
        REQUIRE(gain_db > -0.5f);
    }
}

TEST_CASE("PolyphaseBlockOversampler supports every stage count", "[signal][oversampling][block]") {
    for (int num_stages : {1, 2, 3, 4}) {
        PolyphaseBlockOversampler oversampler;
        constexpr std::size_t kN = 256;
        oversampler.prepare(1, num_stages, kN);
        REQUIRE(oversampler.oversampling_factor() == (1 << num_stages));

        std::vector<float> input(kN, 0.0f);
        input[0] = 1.0f;
        const float* inputs[1] = {input.data()};
        auto view = oversampler.process_up(inputs, 1, kN);
        REQUIRE(view.get_num_samples() == kN * static_cast<std::size_t>(1 << num_stages));

        std::vector<float> output(kN, 0.0f);
        float* outputs[1] = {output.data()};
        oversampler.process_down(outputs, 1, kN);
        for (float sample : output)
            REQUIRE(std::isfinite(sample));
    }
}

TEST_CASE("PolyphaseBlockOversampler prepare() clamps a negative num_stages instead of "
          "converting it to a huge size_t",
          "[signal][oversampling][block][bounds]") {
    // A negative num_stages used to become static_cast<std::size_t>(-1) --
    // effectively SIZE_MAX -- which stages_.assign() would try to satisfy
    // and throw length_error / bad_alloc on. Clamped to 0 stages, prepare()
    // and a 0-stage round trip (see the zero-stage test below) both succeed.
    constexpr std::size_t kN = 64;
    PolyphaseBlockOversampler oversampler;
    oversampler.prepare(1, -3, kN);
    REQUIRE(oversampler.num_stages() == 0);
    REQUIRE(oversampler.oversampling_factor() == 1);
}

TEST_CASE("PolyphaseBlockOversampler prepare() clamps an absurd num_stages instead of "
          "letting oversampling_factor()'s int shift hit UB",
          "[signal][oversampling][block][bounds]") {
    // stages_.size() feeding `1 << stages_.size()` as a plain `int` is UB at
    // or past the width of int (>= 31 for a typical 32-bit int). Clamped to
    // kMaxStages, oversampling_factor() stays a well-defined (if enormous)
    // positive int no matter how large a caller's num_stages argument is.
    constexpr std::size_t kN = 4;
    PolyphaseBlockOversampler oversampler;
    oversampler.prepare(1, 1'000'000, kN);
    REQUIRE(oversampler.num_stages() <= 24);
    REQUIRE(oversampler.oversampling_factor() > 0);
}

TEST_CASE("PolyphaseBlockOversampler with zero stages gives a writable, round-tripping "
          "x1 view even when input and output are different buffers",
          "[signal][oversampling][block][bounds]") {
    // A zero-stage config previously returned a view that aliased `input`
    // directly (const_cast-ing it), and process_down() was a bare no-op --
    // so an out-of-place caller (output != input) never saw the joint edit
    // made through the view, and an in-place caller was writing through a
    // pointer this class never actually owned. The owned passthrough
    // scratch fixes both: the view is genuinely writable, and
    // process_down() copies it into whatever `output` buffer the caller
    // passed, aliased or not.
    constexpr std::size_t kN = 32;
    PolyphaseBlockOversampler oversampler;
    oversampler.prepare(1, 0, kN);
    REQUIRE(oversampler.num_stages() == 0);
    REQUIRE(oversampler.oversampling_factor() == 1);

    std::vector<float> input(kN, 0.0f);
    for (std::size_t i = 0; i < kN; ++i)
        input[i] = static_cast<float>(i) * 0.1f;
    const float* inputs[1] = {input.data()};

    auto view = oversampler.process_up(inputs, 1, kN);
    REQUIRE(view.get_num_samples() == kN);
    float* block = view.channel_pointer(0);
    for (std::size_t i = 0; i < kN; ++i)
        block[i] *= 2.0f;

    std::vector<float> output(kN, -1.0f); // deliberately a separate buffer from input
    float* outputs[1] = {output.data()};
    oversampler.process_down(outputs, 1, kN);

    for (std::size_t i = 0; i < kN; ++i) {
        INFO("i=" << i);
        REQUIRE_THAT(output[i], WithinAbs(input[i] * 2.0f, 1e-6f));
    }
    // The caller's original input buffer must be untouched -- the view
    // wrote into owned scratch, not through a const_cast of `input`.
    for (std::size_t i = 0; i < kN; ++i)
        REQUIRE_THAT(input[i], WithinAbs(static_cast<float>(i) * 0.1f, 1e-6f));
}

TEST_CASE("PolyphaseBlockOversampler clamps num_channels/num_samples that exceed what "
          "prepare() configured instead of indexing past prepared buffers",
          "[signal][oversampling][block][bounds]") {
    // Every per-channel scratch buffer is sized from prepare()'s
    // num_channels / max_block_size. A caller passing a larger count to
    // process_up()/process_down() (e.g. a stale block-size assumption after
    // a host resize) used to index and write straight past those buffers.
    constexpr std::size_t kPreparedN = 64;
    PolyphaseBlockOversampler oversampler;
    oversampler.prepare(1, 2, kPreparedN); // 1 channel, x4, block size 64

    std::vector<float> input(kPreparedN * 4, 0.0f); // caller lies about size
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i) * 0.01f;
    const float* inputs[1] = {input.data()};

    auto view = oversampler.process_up(inputs, 1, kPreparedN * 4);
    // Clamped to the prepared block size, not the inflated caller count.
    REQUIRE(view.get_num_samples() == kPreparedN * 4);

    std::vector<float> output(kPreparedN * 4, 0.0f);
    float* outputs[1] = {output.data()};
    oversampler.process_down(outputs, 1, kPreparedN * 4);
    for (float sample : output)
        REQUIRE(std::isfinite(sample));
}

TEST_CASE("PolyphaseBlockOversampler reset restores a fresh instance exactly",
          "[signal][oversampling][block][reset]") {
    constexpr std::size_t kN = 128;
    PolyphaseBlockOversampler warmed, fresh;
    warmed.prepare(1, 2, kN);
    fresh.prepare(1, 2, kN);

    const auto noise = pseudo_noise(kN, 0xC0FFEEu);
    const float* inputs[1] = {noise.data()};
    std::vector<float> scratch(kN, 0.0f);
    float* outputs[1] = {scratch.data()};

    for (int pass = 0; pass < 5; ++pass) {
        (void)warmed.process_up(inputs, 1, kN);
        warmed.process_down(outputs, 1, kN);
    }
    warmed.reset();

    const auto probe = sine(0.1f, kN);
    const float* probe_inputs[1] = {probe.data()};
    std::vector<float> warmed_out(kN, 0.0f), fresh_out(kN, 0.0f);
    float* warmed_outputs[1] = {warmed_out.data()};
    float* fresh_outputs[1] = {fresh_out.data()};

    (void)warmed.process_up(probe_inputs, 1, kN);
    warmed.process_down(warmed_outputs, 1, kN);
    (void)fresh.process_up(probe_inputs, 1, kN);
    fresh.process_down(fresh_outputs, 1, kN);

    for (std::size_t i = 0; i < kN; ++i) {
        INFO("i=" << i);
        REQUIRE(warmed_out[i] == fresh_out[i]);
    }
}

TEST_CASE("EllipticPolyphaseBlockOversampler round-trips a passband signal",
          "[signal][oversampling][block][elliptic]") {
    constexpr std::size_t kN = 2048;
    EllipticPolyphaseBlockOversampler oversampler;
    oversampler.prepare(1, 3, kN); // x8

    const auto input = sine(0.03f, kN);
    const float* inputs[1] = {input.data()};
    auto view = oversampler.process_up(inputs, 1, kN);
    REQUIRE(view.get_num_samples() == kN * 8);

    std::vector<float> output(kN, 0.0f);
    float* outputs[1] = {output.data()};
    oversampler.process_down(outputs, 1, kN);

    const float in_rms = trailing_rms(input, kN / 4);
    const float out_rms = trailing_rms(output, kN / 4);
    const float gain_db = to_db(out_rms / in_rms);
    INFO("elliptic round-trip gain_db=" << gain_db);
    REQUIRE(gain_db < 0.5f);
    REQUIRE(gain_db > -0.5f);
}

// ────────────────────────────────────────────────────────────────────────
// The argument for this whole file: joint-channel work between up and
// down. This is IMPOSSIBLE with OversamplerT's fused, per-channel callback
// API -- there is no point at which both channels' oversampled sample i
// exist simultaneously across two independent OversamplerT instances.
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("PolyphaseBlockOversampler intermediate buffer supports a stereo-linked "
          "shared envelope impossible with the fused per-channel API",
          "[signal][oversampling][block][joint-channel]") {
    constexpr std::size_t kN = 1024;
    constexpr std::size_t kChannels = 2;
    PolyphaseBlockOversampler oversampler;
    oversampler.prepare(kChannels, 2, kN); // x4

    // Deliberately different content per channel so a per-channel-only
    // process could never produce a shared result.
    const auto left = sine(0.11f, kN);
    const auto right = sine(0.11f, kN, kPi * 0.5f); // 90 degrees out of phase
    const float* inputs[kChannels] = {left.data(), right.data()};

    auto view = oversampler.process_up(inputs, kChannels, kN);
    const std::size_t oversampled_n = view.get_num_samples();

    // Snapshot the pristine oversampled magnitudes before the joint edit
    // mutates them in place, so the "was this actually computed jointly"
    // check below has an independent ground truth to compare against.
    std::vector<float> pristine_left(view.channel_pointer(0), view.channel_pointer(0) + oversampled_n);
    std::vector<float> pristine_right(view.channel_pointer(1),
                                      view.channel_pointer(1) + oversampled_n);

    // Stereo-linked envelope: ONE shared |L|/|R| average computed per
    // oversampled sub-index, then applied identically to both channels --
    // exactly the sidechain-compressor / shared-dither shape GAP 1
    // describes. Requires reading both channels' oversampled sample i
    // *before* writing either -- which is only possible because both
    // channels' oversampled buffers exist simultaneously here.
    float* left_block = view.channel_pointer(0);
    float* right_block = view.channel_pointer(1);
    for (std::size_t i = 0; i < oversampled_n; ++i) {
        const float envelope = 0.5f * (std::fabs(pristine_left[i]) + std::fabs(pristine_right[i]));
        // Apply the same gain reduction to both channels from the same
        // shared value -- a stereo-linked compressor's defining property.
        const float gain = 1.0f - 0.5f * envelope;
        left_block[i] = pristine_left[i] * gain;
        right_block[i] = pristine_right[i] * gain;
    }

    // The shared envelope must actually have been computed jointly: at any
    // sub-index where |L| != |R|, the gain applied is neither "as if only L
    // existed" nor "as if only R existed" -- it sits strictly between the
    // two single-channel gains. If the block API silently processed each
    // channel independently, one of those two equalities would hold.
    bool found_joint_sample = false;
    for (std::size_t i = 0; i < oversampled_n; ++i) {
        const float solo_left_gain = 1.0f - 0.5f * std::fabs(pristine_left[i]);
        const float solo_right_gain = 1.0f - 0.5f * std::fabs(pristine_right[i]);
        if (std::fabs(solo_left_gain - solo_right_gain) <= 1e-3f || pristine_left[i] == 0.0f)
            continue;
        const float applied_left_gain = left_block[i] / pristine_left[i];
        const bool matches_solo_left = std::fabs(applied_left_gain - solo_left_gain) < 1e-4f;
        const bool matches_solo_right = std::fabs(applied_left_gain - solo_right_gain) < 1e-4f;
        found_joint_sample = true;
        REQUIRE_FALSE(matches_solo_left);
        REQUIRE_FALSE(matches_solo_right);
        break;
    }
    REQUIRE(found_joint_sample);

    std::vector<float> out_left(kN, 0.0f), out_right(kN, 0.0f);
    float* outputs[kChannels] = {out_left.data(), out_right.data()};
    oversampler.process_down(outputs, kChannels, kN);

    for (float sample : out_left)
        REQUIRE(std::isfinite(sample));
    for (float sample : out_right)
        REQUIRE(std::isfinite(sample));

    // With the shared gain reduction applied, output RMS must be lower
    // than a plain pass-through round-trip would give -- confirms the
    // joint-channel edit actually reached the decimated output.
    PolyphaseBlockOversampler reference;
    reference.prepare(kChannels, 2, kN);
    auto ref_view = reference.process_up(inputs, kChannels, kN);
    static_cast<void>(ref_view);
    std::vector<float> ref_left(kN, 0.0f), ref_right(kN, 0.0f);
    float* ref_outputs[kChannels] = {ref_left.data(), ref_right.data()};
    reference.process_down(ref_outputs, kChannels, kN);

    const float processed_rms = trailing_rms(out_left, kN / 4) + trailing_rms(out_right, kN / 4);
    const float reference_rms = trailing_rms(ref_left, kN / 4) + trailing_rms(ref_right, kN / 4);
    INFO("processed_rms=" << processed_rms << " reference_rms=" << reference_rms);
    REQUIRE(processed_rms < reference_rms);
}
