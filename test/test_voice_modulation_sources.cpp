#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/voice_modulation_sources.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <cmath>
#include <limits>

using Catch::Matchers::WithinAbs;
using pulp::audio::AhdsrEnvelopeConfig;
using pulp::audio::VoiceEnvelopeSourceSpec;
using pulp::audio::VoiceLfoPhasePolicy;
using pulp::audio::VoiceLfoSourceSpec;
using pulp::audio::VoiceModulationBuffer;
using pulp::audio::VoiceModulationBufferConfig;
using pulp::audio::VoiceModulationRate;
using pulp::audio::VoiceModulationSources;
using pulp::audio::VoiceModulationSourcesConfig;
using pulp::audio::VoiceModulationStatus;
using pulp::audio::VoiceModulationTarget;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586477;

double wrap01(double x) {
    return x - std::floor(x);
}

VoiceModulationSourcesConfig sine_config(double rate_hz,
                                         float depth,
                                         VoiceLfoPhasePolicy policy,
                                         bool unison_spread) {
    VoiceModulationSourcesConfig config;
    config.sample_rate = kSampleRate;
    config.max_frames = 64;
    config.lfo.enabled = true;
    config.lfo.wave = pulp::signal::Lfo::Wave::sine;
    config.lfo.phase_policy = policy;
    config.lfo.rate_hz = rate_hz;
    config.lfo.depth = depth;
    config.lfo.unison_phase_spread = unison_spread;
    config.lfo.seed = 7;
    return config;
}

template <std::size_t MaximumVoices>
std::vector<float> render_audio_rate_lfo(VoiceModulationSources<MaximumVoices>& sources,
                                         std::size_t voice,
                                         std::uint32_t frames) {
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = frames}));
    REQUIRE(buffer.begin_block(frames).ok);
    REQUIRE(sources.append_lfo_lane(voice, buffer, VoiceModulationTarget::Aux0,
                                    VoiceModulationRate::AudioRate, frames)
                .ok);
    const auto* lane = buffer.block().find(VoiceModulationTarget::Aux0);
    REQUIRE(lane != nullptr);
    REQUIRE(lane->frame_count == frames);
    return {lane->values, lane->values + frames};
}

}  // namespace

TEST_CASE("Per-voice LFO lanes match the analytic sine phase law",
          "[audio][voice-mod-sources]") {
    VoiceModulationSources<4> sources;
    REQUIRE(sources.prepare(sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false)));
    sources.note_on(0);

    const auto values = render_audio_rate_lfo(sources, 0, 32);
    for (std::uint32_t frame = 0; frame < 32; ++frame) {
        const double phase = wrap01(static_cast<double>(frame) * 100.0 / kSampleRate);
        const double expected = 0.5 * std::sin(kTwoPi * phase);
        REQUIRE_THAT(static_cast<double>(values[frame]), WithinAbs(expected, 2.0e-6));
    }
}

TEST_CASE("Unison phase spread offsets voices evenly around the cycle",
          "[audio][voice-mod-sources]") {
    VoiceModulationSources<4> sources;
    REQUIRE(sources.prepare(sine_config(100.0, 1.0f, VoiceLfoPhasePolicy::Retrigger, true)));
    for (std::size_t voice = 0; voice < 4; ++voice)
        sources.note_on(voice);

    for (std::size_t voice = 0; voice < 4; ++voice) {
        const auto values = render_audio_rate_lfo(sources, voice, 1);
        const double phase = wrap01(static_cast<double>(voice) / 4.0);
        REQUIRE_THAT(static_cast<double>(values[0]),
                     WithinAbs(std::sin(kTwoPi * phase), 2.0e-6));
    }
}

TEST_CASE("Retriggered LFO restarts the phase sequence on every note-on",
          "[audio][voice-mod-sources]") {
    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(sine_config(500.0, 1.0f, VoiceLfoPhasePolicy::Retrigger, false)));

    sources.note_on(0);
    const auto first = render_audio_rate_lfo(sources, 0, 16);
    sources.note_on(0);
    const auto second = render_audio_rate_lfo(sources, 0, 16);
    REQUIRE(first == second);
}

TEST_CASE("Free-running LFO ignores note-on entirely", "[audio][voice-mod-sources]") {
    VoiceModulationSources<2> interrupted;
    REQUIRE(
        interrupted.prepare(sine_config(500.0, 1.0f, VoiceLfoPhasePolicy::FreeRunning, false)));
    VoiceModulationSources<2> untouched;
    REQUIRE(
        untouched.prepare(sine_config(500.0, 1.0f, VoiceLfoPhasePolicy::FreeRunning, false)));

    const auto first = render_audio_rate_lfo(interrupted, 0, 8);
    interrupted.note_on(0);
    const auto second = render_audio_rate_lfo(interrupted, 0, 8);
    const auto control_first = render_audio_rate_lfo(untouched, 0, 8);
    const auto control_second = render_audio_rate_lfo(untouched, 0, 8);
    REQUIRE(first == control_first);
    REQUIRE(second == control_second);
}

TEST_CASE("Reset restores source state and out-of-range voice events are no-ops",
          "[audio][voice-mod-sources]") {
    auto config = sine_config(500.0, 1.0f, VoiceLfoPhasePolicy::Retrigger, false);
    config.envelope.enabled = true;
    config.envelope.envelope.attack_seconds = 0.001;
    config.envelope.envelope.release_seconds = 0.002;

    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(config));
    sources.note_on(0);
    const auto initial = render_audio_rate_lfo(sources, 0, 16);
    static_cast<void>(render_audio_rate_lfo(sources, 0, 8));

    sources.reset();
    sources.note_on(0);
    REQUIRE(render_audio_rate_lfo(sources, 0, 16) == initial);

    VoiceModulationSources<2> control;
    REQUIRE(control.prepare(config));
    control.note_on(0);
    sources.reset();
    sources.note_on(0);
    sources.note_on(2);
    sources.note_off(2);
    REQUIRE(render_audio_rate_lfo(sources, 0, 16) ==
            render_audio_rate_lfo(control, 0, 16));
}

TEST_CASE("Zero depth leaves the lane exactly constant at zero", "[audio][voice-mod-sources]") {
    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(sine_config(500.0, 0.0f, VoiceLfoPhasePolicy::Retrigger, false)));
    sources.note_on(0);

    const auto values = render_audio_rate_lfo(sources, 0, 32);
    for (const float value : values)
        REQUIRE(value == 0.0f);
}

TEST_CASE("Per-voice seeds make random voices distinct but reproducible",
          "[audio][voice-mod-sources]") {
    auto random_config = [] {
        VoiceModulationSourcesConfig config;
        config.sample_rate = kSampleRate;
        config.max_frames = 64;
        config.lfo.enabled = true;
        config.lfo.wave = pulp::signal::Lfo::Wave::sh_random;
        config.lfo.phase_policy = VoiceLfoPhasePolicy::Retrigger;
        config.lfo.rate_hz = 50.0;
        config.lfo.depth = 1.0f;
        config.lfo.seed = 11;
        return config;
    };

    VoiceModulationSources<2> a;
    REQUIRE(a.prepare(random_config()));
    VoiceModulationSources<2> b;
    REQUIRE(b.prepare(random_config()));
    a.note_on(0);
    a.note_on(1);
    b.note_on(0);
    b.note_on(1);

    const auto a0 = render_audio_rate_lfo(a, 0, 32);
    const auto a1 = render_audio_rate_lfo(a, 1, 32);
    const auto b0 = render_audio_rate_lfo(b, 0, 32);
    REQUIRE(a0 == b0);
    REQUIRE(a0 != a1);
}

TEST_CASE("Envelope lanes rise through attack and fall after note-off",
          "[audio][voice-mod-sources]") {
    VoiceModulationSourcesConfig config;
    config.sample_rate = kSampleRate;
    config.max_frames = 64;
    config.envelope.enabled = true;
    config.envelope.envelope.attack_seconds = 64.0 / kSampleRate;
    config.envelope.envelope.hold_seconds = 0.0;
    config.envelope.envelope.decay_seconds = 0.0;
    config.envelope.envelope.sustain_level = 1.0;
    config.envelope.envelope.release_seconds = 64.0 / kSampleRate;

    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(config));
    sources.note_on(0);

    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = 64}));
    REQUIRE(buffer.begin_block(64).ok);
    REQUIRE(sources
                .append_envelope_lane(0, buffer, VoiceModulationTarget::Gain,
                                      VoiceModulationRate::AudioRate, 64)
                .ok);
    const auto* attack_lane = buffer.block().find(VoiceModulationTarget::Gain);
    REQUIRE(attack_lane != nullptr);
    float previous = -1.0f;
    for (std::uint32_t frame = 0; frame < 64; ++frame) {
        const float value = attack_lane->values[frame];
        REQUIRE(value >= previous);
        previous = value;
    }
    REQUIRE_THAT(static_cast<double>(attack_lane->values[63]), WithinAbs(1.0, 1.0e-4));

    sources.note_off(0);
    VoiceModulationBuffer release_buffer;
    REQUIRE(release_buffer.prepare({.max_lanes = 2, .max_frames = 128}));
    REQUIRE(release_buffer.begin_block(128).ok);
    REQUIRE(sources
                .append_envelope_lane(0, release_buffer, VoiceModulationTarget::Gain,
                                      VoiceModulationRate::AudioRate, 128)
                .ok);
    const auto* release_lane = release_buffer.block().find(VoiceModulationTarget::Gain);
    REQUIRE(release_lane != nullptr);
    REQUIRE(release_lane->values[0] > release_lane->values[127]);
    REQUIRE(release_lane->values[127] <= 1.0e-4f);
}

TEST_CASE("Audio-rate lanes are invariant under block partition", "[audio][voice-mod-sources]") {
    auto config = sine_config(300.0, 0.7f, VoiceLfoPhasePolicy::Retrigger, false);
    config.envelope.enabled = true;
    config.envelope.envelope.attack_seconds = 40.0 / kSampleRate;
    config.envelope.envelope.decay_seconds = 0.02;
    config.envelope.envelope.sustain_level = 0.6;
    config.envelope.envelope.release_seconds = 0.02;
    config.max_frames = 64;

    VoiceModulationSources<2> whole;
    REQUIRE(whole.prepare(config));
    VoiceModulationSources<2> split;
    REQUIRE(split.prepare(config));
    whole.note_on(0);
    split.note_on(0);

    std::array<float, 64> whole_lfo{};
    std::array<float, 64> whole_env{};
    {
        VoiceModulationBuffer buffer;
        REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = 64}));
        REQUIRE(whole.write_voice(0, buffer, VoiceModulationTarget::Aux0,
                                  VoiceModulationTarget::Gain, VoiceModulationRate::AudioRate, 64)
                    .ok);
        const auto* lfo_lane = buffer.block().find(VoiceModulationTarget::Aux0);
        const auto* env_lane = buffer.block().find(VoiceModulationTarget::Gain);
        std::copy(lfo_lane->values, lfo_lane->values + 64, whole_lfo.begin());
        std::copy(env_lane->values, env_lane->values + 64, whole_env.begin());
    }

    std::array<float, 64> split_lfo{};
    std::array<float, 64> split_env{};
    for (std::uint32_t block = 0; block < 2; ++block) {
        VoiceModulationBuffer buffer;
        REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = 32}));
        REQUIRE(split
                    .write_voice(0, buffer, VoiceModulationTarget::Aux0,
                                 VoiceModulationTarget::Gain, VoiceModulationRate::AudioRate, 32)
                    .ok);
        const auto* lfo_lane = buffer.block().find(VoiceModulationTarget::Aux0);
        const auto* env_lane = buffer.block().find(VoiceModulationTarget::Gain);
        std::copy(lfo_lane->values, lfo_lane->values + 32,
                  split_lfo.begin() + static_cast<std::ptrdiff_t>(block * 32));
        std::copy(env_lane->values, env_lane->values + 32,
                  split_env.begin() + static_cast<std::ptrdiff_t>(block * 32));
    }

    REQUIRE(whole_lfo == split_lfo);
    REQUIRE(whole_env == split_env);
}

TEST_CASE("write_voice publishes one lane per enabled source", "[audio][voice-mod-sources]") {
    auto config = sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false);
    config.envelope.enabled = true;
    config.envelope.envelope.attack_seconds = 0.0;
    config.envelope.envelope.release_seconds = 0.01;

    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(config));
    sources.note_on(1);

    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = 16}));
    REQUIRE(sources
                .write_voice(1, buffer, VoiceModulationTarget::Aux1, VoiceModulationTarget::Gain,
                             VoiceModulationRate::Constant, 16)
                .ok);
    const auto& block = buffer.block();
    REQUIRE(block.frame_count == 16);
    const auto* lfo_lane = block.find(VoiceModulationTarget::Aux1);
    const auto* env_lane = block.find(VoiceModulationTarget::Gain);
    REQUIRE(lfo_lane != nullptr);
    REQUIRE(env_lane != nullptr);
    REQUIRE(lfo_lane->rate == VoiceModulationRate::Constant);
    REQUIRE(env_lane->rate == VoiceModulationRate::Constant);
    // Zero-attack envelope is at full level immediately.
    REQUIRE_THAT(static_cast<double>(env_lane->constant_value), WithinAbs(1.0, 1.0e-6));
}

TEST_CASE("Failure contract refuses invalid calls without touching state",
          "[audio][voice-mod-sources]") {
    VoiceModulationSources<2> unprepared;
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 1, .max_frames = 16}));

    SECTION("unprepared bank") {
        auto result = unprepared.append_lfo_lane(0, buffer, VoiceModulationTarget::Aux0,
                                                 VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::NotPrepared);
        REQUIRE_FALSE(result.ok);

        result = unprepared.append_envelope_lane(0, buffer, VoiceModulationTarget::Gain,
                                                 VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::NotPrepared);
        REQUIRE_FALSE(result.ok);
    }

    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false)));

    SECTION("voice out of range") {
        auto result = sources.append_lfo_lane(2, buffer, VoiceModulationTarget::Aux0,
                                              VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::InvalidTarget);

        result = sources.append_envelope_lane(2, buffer, VoiceModulationTarget::Gain,
                                              VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::InvalidTarget);
    }

    SECTION("unprepared buffer") {
        VoiceModulationBuffer unprepared_buffer;
        auto result = sources.append_lfo_lane(0, unprepared_buffer, VoiceModulationTarget::Aux0,
                                              VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::NotPrepared);

        result = sources.append_envelope_lane(0, unprepared_buffer,
                                              VoiceModulationTarget::Gain,
                                              VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::NotPrepared);
    }

    SECTION("zero frame count") {
        auto result = sources.append_lfo_lane(0, buffer, VoiceModulationTarget::Aux0,
                                              VoiceModulationRate::Constant, 0);
        REQUIRE(result.status == VoiceModulationStatus::InvalidFrameCount);

        result = sources.append_envelope_lane(0, buffer, VoiceModulationTarget::Gain,
                                              VoiceModulationRate::Constant, 0);
        REQUIRE(result.status == VoiceModulationStatus::InvalidFrameCount);
    }

    SECTION("frame count above buffer capacity") {
        auto result = sources.append_lfo_lane(0, buffer, VoiceModulationTarget::Aux0,
                                              VoiceModulationRate::Constant, 17);
        REQUIRE(result.status == VoiceModulationStatus::InvalidFrameCount);

        result = sources.append_envelope_lane(0, buffer, VoiceModulationTarget::Gain,
                                              VoiceModulationRate::Constant, 17);
        REQUIRE(result.status == VoiceModulationStatus::InvalidFrameCount);
    }

    SECTION("disabled LFO source") {
        VoiceModulationSourcesConfig envelope_only;
        envelope_only.sample_rate = kSampleRate;
        envelope_only.max_frames = 16;
        envelope_only.envelope.enabled = true;
        VoiceModulationSources<2> envelope_sources;
        REQUIRE(envelope_sources.prepare(envelope_only));
        auto result = envelope_sources.append_lfo_lane(
            0, buffer, VoiceModulationTarget::Aux0, VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::InvalidTarget);
    }

    SECTION("disabled envelope source") {
        auto result = sources.append_envelope_lane(0, buffer, VoiceModulationTarget::Gain,
                                                   VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::InvalidTarget);
    }

    SECTION("audio-rate reservations preserve an existing target lane") {
        auto both = sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false);
        both.envelope.enabled = true;
        VoiceModulationSources<2> full;
        REQUIRE(full.prepare(both));

        VoiceModulationBuffer lfo_collision;
        REQUIRE(lfo_collision.prepare({.max_lanes = 2, .max_frames = 16}));
        REQUIRE(lfo_collision.begin_block(8).ok);
        REQUIRE(lfo_collision.add_constant(VoiceModulationTarget::Aux0, 0.25f).ok);
        auto result = full.append_lfo_lane(0, lfo_collision, VoiceModulationTarget::Aux0,
                                           VoiceModulationRate::AudioRate, 8);
        REQUIRE(result.status == VoiceModulationStatus::DuplicateTarget);
        const auto* lfo_lane = lfo_collision.block().find(VoiceModulationTarget::Aux0);
        REQUIRE(lfo_lane != nullptr);
        REQUIRE(lfo_lane->rate == VoiceModulationRate::Constant);
        REQUIRE(lfo_lane->constant_value == 0.25f);

        VoiceModulationBuffer envelope_collision;
        REQUIRE(envelope_collision.prepare({.max_lanes = 2, .max_frames = 16}));
        REQUIRE(envelope_collision.begin_block(8).ok);
        REQUIRE(envelope_collision.add_constant(VoiceModulationTarget::Gain, 0.75f).ok);
        result = full.append_envelope_lane(0, envelope_collision, VoiceModulationTarget::Gain,
                                           VoiceModulationRate::AudioRate, 8);
        REQUIRE(result.status == VoiceModulationStatus::DuplicateTarget);
        const auto* envelope_lane =
            envelope_collision.block().find(VoiceModulationTarget::Gain);
        REQUIRE(envelope_lane != nullptr);
        REQUIRE(envelope_lane->rate == VoiceModulationRate::Constant);
        REQUIRE(envelope_lane->constant_value == 0.75f);
    }

    SECTION("write_voice with a disabled source") {
        auto result = sources.write_voice(0, buffer, VoiceModulationTarget::Aux0,
                                          VoiceModulationTarget::Gain,
                                          VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::InvalidTarget);
    }

    SECTION("write_voice with one lane of capacity") {
        auto both = sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false);
        both.envelope.enabled = true;
        both.envelope.envelope.attack_seconds = 0.001;
        VoiceModulationSources<2> full;
        REQUIRE(full.prepare(both));
        auto result = full.write_voice(0, buffer, VoiceModulationTarget::Aux0,
                                       VoiceModulationTarget::Gain, VoiceModulationRate::Constant,
                                       8);
        REQUIRE(result.status == VoiceModulationStatus::LaneOverflow);
    }

    SECTION("write_voice with duplicate targets") {
        VoiceModulationBuffer wide;
        REQUIRE(wide.prepare({.max_lanes = 2, .max_frames = 16}));
        auto both = sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false);
        both.envelope.enabled = true;
        both.envelope.envelope.attack_seconds = 0.001;
        VoiceModulationSources<2> full;
        REQUIRE(full.prepare(both));
        auto result = full.write_voice(0, wide, VoiceModulationTarget::Aux0,
                                       VoiceModulationTarget::Aux0, VoiceModulationRate::Constant,
                                       8);
        REQUIRE(result.status == VoiceModulationStatus::DuplicateTarget);
    }

    SECTION("write_voice with a voice out of range") {
        auto result = sources.write_voice(2, buffer, VoiceModulationTarget::Aux0,
                                          VoiceModulationTarget::Gain,
                                          VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::InvalidTarget);
    }

    SECTION("write_voice with zero frame count") {
        auto result = sources.write_voice(0, buffer, VoiceModulationTarget::Aux0,
                                          VoiceModulationTarget::Gain,
                                          VoiceModulationRate::Constant, 0);
        REQUIRE(result.status == VoiceModulationStatus::InvalidFrameCount);
    }

    SECTION("write_voice with frame count above buffer capacity") {
        auto result = sources.write_voice(0, buffer, VoiceModulationTarget::Aux0,
                                          VoiceModulationTarget::Gain,
                                          VoiceModulationRate::Constant, 17);
        REQUIRE(result.status == VoiceModulationStatus::InvalidFrameCount);
    }

    SECTION("write_voice with an unprepared bank") {
        auto result = unprepared.write_voice(0, buffer, VoiceModulationTarget::Aux0,
                                             VoiceModulationTarget::Gain,
                                             VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::NotPrepared);
    }

    SECTION("write_voice with an unprepared buffer") {
        VoiceModulationBuffer unprepared_buffer;
        auto result = sources.write_voice(0, unprepared_buffer, VoiceModulationTarget::Aux0,
                                          VoiceModulationTarget::Gain,
                                          VoiceModulationRate::Constant, 8);
        REQUIRE(result.status == VoiceModulationStatus::NotPrepared);
    }
}

TEST_CASE("Invalid configs leave the bank unprepared", "[audio][voice-mod-sources]") {
    VoiceModulationSources<2> sources;

    auto bad_rate = sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false);
    bad_rate.sample_rate = 0.0;
    REQUIRE_FALSE(sources.prepare(bad_rate));

    auto bad_frames = sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false);
    bad_frames.max_frames = 0;
    REQUIRE_FALSE(sources.prepare(bad_frames));

    auto bad_depth = sine_config(100.0, std::numeric_limits<float>::quiet_NaN(),
                                 VoiceLfoPhasePolicy::Retrigger, false);
    REQUIRE_FALSE(sources.prepare(bad_depth));

    auto bad_lfo_rate = sine_config(-5.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false);
    REQUIRE_FALSE(sources.prepare(bad_lfo_rate));

    auto bad_envelope = sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::Retrigger, false);
    bad_envelope.envelope.enabled = true;
    bad_envelope.envelope.envelope.attack_seconds = -1.0;
    REQUIRE_FALSE(sources.prepare(bad_envelope));

    REQUIRE_FALSE(sources.prepared());
}

TEST_CASE("Live retune leaves LFO phase running where prepare would rewind it",
          "[audio][voice-mod-sources]") {
    const auto config = sine_config(120.0, 1.0f, VoiceLfoPhasePolicy::FreeRunning, false);

    // Reference: one uninterrupted 32-frame render.
    VoiceModulationSources<2> reference;
    REQUIRE(reference.prepare(config));
    const auto whole = render_audio_rate_lfo(reference, 0, 32);

    // Live path: retune to the SAME rate midway. A setter that does not touch
    // phase must leave the second half bit-identical to the reference tail.
    VoiceModulationSources<2> live;
    REQUIRE(live.prepare(config));
    const auto live_head = render_audio_rate_lfo(live, 0, 16);
    REQUIRE(live.set_lfo_rate_hz(config.lfo.rate_hz));
    const auto live_tail = render_audio_rate_lfo(live, 0, 16);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK_THAT(live_head[i], WithinAbs(whole[i], 1.0e-6));
        CHECK_THAT(live_tail[i], WithinAbs(whole[16 + i], 1.0e-6));
    }

    // Negative control: the configure-time path at the same point DOES rewind.
    // Without this the test above could pass on a bank that never advances.
    VoiceModulationSources<2> reconfigured;
    REQUIRE(reconfigured.prepare(config));
    (void)render_audio_rate_lfo(reconfigured, 0, 16);
    REQUIRE(reconfigured.prepare(config));
    const auto rewound = render_audio_rate_lfo(reconfigured, 0, 16);
    for (std::size_t i = 0; i < 16; ++i)
        CHECK_THAT(rewound[i], WithinAbs(whole[i], 1.0e-6));
    // ...and that rewound tail is genuinely different from the running one,
    // so the two paths cannot be confused for one another.
    bool differs = false;
    for (std::size_t i = 0; i < 16 && !differs; ++i)
        differs = std::fabs(rewound[i] - live_tail[i]) > 1.0e-3;
    CHECK(differs);
}

TEST_CASE("Live retune refuses a non-positive rate and leaves the lane alone",
          "[audio][voice-mod-sources]") {
    const auto config = sine_config(120.0, 0.5f, VoiceLfoPhasePolicy::FreeRunning, false);
    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(config));

    const double kept_rate = sources.config().lfo.rate_hz;

    // The shipped LFO clamps a non-positive rate up into its own legal range
    // rather than refusing it, so a setter that took one would leave config()
    // reporting a rate prepare() rejects.
    CHECK_FALSE(sources.set_lfo_rate_hz(0.0));
    CHECK_FALSE(sources.set_lfo_rate_hz(-10.0));
    CHECK(sources.config().lfo.rate_hz == kept_rate);

    // A refused rate leaves the rendered lane untouched.
    VoiceModulationSources<2> untouched;
    REQUIRE(untouched.prepare(config));
    const auto expected = render_audio_rate_lfo(untouched, 0, 16);
    const auto actual = render_audio_rate_lfo(sources, 0, 16);
    REQUIRE(expected.size() == 16);
    for (std::size_t i = 0; i < expected.size(); ++i)
        CHECK_THAT(actual[i], WithinAbs(expected[i], 1.0e-6));

    // ...and the config it still reports stays one prepare() accepts, which is
    // the invariant the refusal exists to hold.
    VoiceModulationSources<2> round_trip;
    CHECK(round_trip.prepare(sources.config()));
}

TEST_CASE("Realtime paths allocate nothing after prepare", "[audio][voice-mod-sources]") {
    auto config = sine_config(200.0, 0.8f, VoiceLfoPhasePolicy::Retrigger, true);
    config.envelope.enabled = true;
    config.envelope.envelope.attack_seconds = 0.002;
    config.envelope.envelope.release_seconds = 0.005;

    VoiceModulationSources<8> sources;
    REQUIRE(sources.prepare(config));

    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = 64}));

    pulp::test::RtAllocationProbe probe;
    sources.reset();
    sources.note_on(3);
    REQUIRE(sources
                .write_voice(3, buffer, VoiceModulationTarget::Aux0, VoiceModulationTarget::Gain,
                             VoiceModulationRate::AudioRate, 64)
                .ok);
    REQUIRE(sources
                .write_voice(3, buffer, VoiceModulationTarget::Aux0, VoiceModulationTarget::Gain,
                             VoiceModulationRate::Constant, 32)
                .ok);
    REQUIRE(sources.set_lfo_rate_hz(311.0));
    REQUIRE(sources.set_lfo_depth(-0.4f));
    sources.set_lfo_wave(pulp::signal::Lfo::Wave::sh_random);
    sources.set_lfo_wave(pulp::signal::Lfo::Wave::triangle);
    sources.note_off(3);
    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("Rate changes retune every voice without disturbing phase",
          "[audio][voice-mod-sources]") {
    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(sine_config(100.0, 1.0f, VoiceLfoPhasePolicy::FreeRunning, false)));

    const auto before = render_audio_rate_lfo(sources, 0, 32);
    REQUIRE(before.size() == 32);

    REQUIRE(sources.set_lfo_rate_hz(400.0));
    REQUIRE_THAT(sources.config().lfo.rate_hz, WithinAbs(400.0, 0.0));

    // The sweep continues from the phase the first block left behind rather
    // than restarting, which is the whole reason this is not `prepare()`.
    const double carried = wrap01(32.0 * 100.0 / kSampleRate);
    const auto after = render_audio_rate_lfo(sources, 0, 16);
    for (std::uint32_t frame = 0; frame < 16; ++frame) {
        const double phase = wrap01(carried + static_cast<double>(frame) * 400.0 / kSampleRate);
        REQUIRE_THAT(static_cast<double>(after[frame]),
                     WithinAbs(std::sin(kTwoPi * phase), 2.0e-6));
    }
}

TEST_CASE("Negative depth inverts the wave exactly", "[audio][voice-mod-sources]") {
    auto config = sine_config(250.0, 1.0f, VoiceLfoPhasePolicy::FreeRunning, false);
    config.lfo.wave = pulp::signal::Lfo::Wave::triangle;

    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(config));
    const auto positive = render_audio_rate_lfo(sources, 0, 48);

    sources.reset();
    REQUIRE(sources.set_lfo_depth(-1.0f));
    const auto inverted = render_audio_rate_lfo(sources, 0, 48);

    REQUIRE(inverted.size() == positive.size());
    bool saw_nonzero = false;
    for (std::size_t frame = 0; frame < positive.size(); ++frame) {
        REQUIRE(inverted[frame] == -positive[frame]);
        saw_nonzero = saw_nonzero || positive[frame] != 0.0f;
    }
    // Without this the identity above would hold trivially on a silent lane.
    REQUIRE(saw_nonzero);
}

TEST_CASE("Wave changes reshape every voice without disturbing phase",
          "[audio][voice-mod-sources]") {
    VoiceModulationSources<2> sources;
    REQUIRE(sources.prepare(sine_config(100.0, 1.0f, VoiceLfoPhasePolicy::FreeRunning, false)));

    const auto sine = render_audio_rate_lfo(sources, 0, 32);
    REQUIRE(sine.size() == 32);

    sources.set_lfo_wave(pulp::signal::Lfo::Wave::triangle);
    REQUIRE(sources.config().lfo.wave == pulp::signal::Lfo::Wave::triangle);

    // The triangle picks up at the phase the sine left behind. A reshape that
    // restarted phase would be a discontinuity on a slow LFO, which is exactly
    // what re-preparing the bank would do.
    const double carried = wrap01(32.0 * 100.0 / kSampleRate);
    const auto triangle = render_audio_rate_lfo(sources, 0, 16);
    for (std::uint32_t frame = 0; frame < 16; ++frame) {
        const double phase = wrap01(carried + static_cast<double>(frame) * 100.0 / kSampleRate);
        const double expected = phase < 0.5 ? -1.0 + 4.0 * phase : 3.0 - 4.0 * phase;
        REQUIRE_THAT(static_cast<double>(triangle[frame]), WithinAbs(expected, 2.0e-6));
    }
    // The two shapes have to actually differ, or the comparison above would
    // pass against an unchanged sine.
    REQUIRE(triangle[0] != sine[0]);
}

TEST_CASE("Live setters refuse unprepared banks and non-finite values",
          "[audio][voice-mod-sources]") {
    VoiceModulationSources<2> sources;
    REQUIRE_FALSE(sources.set_lfo_rate_hz(120.0));
    REQUIRE_FALSE(sources.set_lfo_depth(0.25f));
    sources.set_lfo_wave(pulp::signal::Lfo::Wave::square);
    REQUIRE_FALSE(sources.prepared());
    REQUIRE_THAT(sources.config().lfo.rate_hz, WithinAbs(5.0, 0.0));
    REQUIRE(sources.config().lfo.wave == pulp::signal::Lfo::Wave::sine);

    REQUIRE(sources.prepare(sine_config(100.0, 0.5f, VoiceLfoPhasePolicy::FreeRunning, false)));
    REQUIRE_FALSE(sources.set_lfo_rate_hz(std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(sources.set_lfo_depth(std::numeric_limits<float>::infinity()));
    REQUIRE_THAT(sources.config().lfo.rate_hz, WithinAbs(100.0, 0.0));
    REQUIRE_THAT(static_cast<double>(sources.config().lfo.depth), WithinAbs(0.5, 0.0));
}
