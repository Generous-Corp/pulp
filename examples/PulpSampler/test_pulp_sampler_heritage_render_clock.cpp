#include "test_pulp_sampler_heritage_support.hpp"

#include <catch2/catch_approx.hpp>

namespace {

audio::SampleHeritageProfile
live_cyclic_profile(double factor, std::uint16_t divisions = 0,
                    audio::SampleHeritageSeedPolicy seed_policy =
                        audio::SampleHeritageSeedPolicy::RestartFromProfileSeed) {
    return {
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.live-cyclic-v3",
        .host_sample_rate = 48000.0,
        .voice = {{audio::SampleHeritageBlockDomain::Voice, false,
                   audio::SampleHeritageVoiceLiveCyclicStretchBlock{
                       factor, 10.0, 1.0, true, divisions,
                       divisions == 0 && seed_policy ==
                                             audio::SampleHeritageSeedPolicy::RestartFromProfileSeed
                           ? 0u
                           : 0x1234u,
                       seed_policy}}},
    };
}

audio::SampleHeritageProfile variable_pitch_live_cyclic_profile(double factor) {
    return {
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.variable-pitch-live-cyclic-v3",
        .host_sample_rate = 48000.0,
        .voice =
            {
                {audio::SampleHeritageBlockDomain::Voice, false,
                 audio::SampleHeritageVoicePitchBlock{
                     audio::SampleHeritagePitchFamily::VariableClock}},
                {audio::SampleHeritageBlockDomain::Voice, false,
                 audio::SampleHeritageVoiceLiveCyclicStretchBlock{
                     factor, 10.0, 1.0, true, 0, 0,
                     audio::SampleHeritageSeedPolicy::RestartFromProfileSeed}},
            },
    };
}

} // namespace

TEST_CASE("PulpSampler heritage render is bitwise callback-partition invariant",
          "[audio][sampler][heritage][partition]") {
    const auto profile = two_leg_profile();
    auto sample = make_sine(48000);
    HeritageFixture whole(4096, &profile);
    HeritageFixture split(4096, &profile);
    whole.load(sample);
    split.load(sample);
    constexpr std::array one{std::size_t{4096}};
    constexpr std::array many{std::size_t{1},    std::size_t{31},  std::size_t{128},
                              std::size_t{7},    std::size_t{333}, std::size_t{524},
                              std::size_t{1024}, std::size_t{2048}};
    REQUIRE(std::accumulate(many.begin(), many.end(), std::size_t{0}) == 4096);
    const auto contiguous = render(whole, one);
    const auto partitioned = render(split, many);
    const auto mismatch =
        std::mismatch(contiguous.begin(), contiguous.end(), partitioned.begin(), partitioned.end());
    INFO("first mismatch frame: " << std::distance(contiguous.begin(), mismatch.first));
    REQUIRE(mismatch.first == contiguous.end());
}

TEST_CASE("PulpSampler heritage clock ratio preserves sampler pitch",
          "[audio][sampler][heritage][pitch]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(4096, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{4096}};
    const auto output = render(fixture, block);
    const auto measured = std::span<const float>(output).subspan(256);
    REQUIRE(tone_projection(measured, 440.0, 48000.0) > 0.8);
    REQUIRE(tone_projection(measured, 440.0, 48000.0) >
            tone_projection(measured, 880.0, 48000.0) * 8.0);
}

TEST_CASE("PulpSampler factor-one live cyclic stretch is bit transparent",
          "[audio][sampler][heritage][typed][stretch][bypass]") {
    const auto profile = live_cyclic_profile(1.0);
    auto sample = make_sine(8192);
    HeritageFixture clean(1024);
    HeritageFixture stretched(1024, &profile);
    clean.load(sample);
    stretched.load(sample);
    constexpr std::array blocks{std::size_t{17}, std::size_t{63}, std::size_t{128},
                                std::size_t{816}};
    REQUIRE(render(clean, blocks) == render(stretched, blocks));
}

TEST_CASE("PulpSampler live cyclic stretch changes duration without shifting tone",
          "[audio][sampler][heritage][typed][stretch][pitch]") {
    const auto profile = live_cyclic_profile(2.0);
    auto sample = make_sine(8192);
    HeritageFixture fixture(4096, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{4096}};
    const auto output = render(fixture, block);
    const auto measured = std::span<const float>(output).first(480);
    REQUIRE(tone_projection(measured, 440.0, 48000.0) > 0.7);
    REQUIRE(tone_projection(measured, 440.0, 48000.0) >
            tone_projection(measured, 220.0, 48000.0) * 6.0);
}

TEST_CASE("PulpSampler live cyclic stretch is callback-partition invariant",
          "[audio][sampler][heritage][typed][stretch][partition]") {
    const auto profile = live_cyclic_profile(1.6, 4);
    auto sample = make_sine(8192);
    HeritageFixture whole(1024, &profile);
    HeritageFixture split(1024, &profile);
    whole.load(sample);
    split.load(sample);
    constexpr std::array one{std::size_t{1024}};
    constexpr std::array many{std::size_t{17}, std::size_t{63},  std::size_t{128},
                              std::size_t{7},  std::size_t{333}, std::size_t{476}};
    REQUIRE(render(whole, one) == render(split, many));
}

TEST_CASE("PulpSampler live cyclic one-shots end at the stretched lifetime",
          "[audio][sampler][heritage][typed][stretch][eof]") {
    const auto profile = live_cyclic_profile(2.0);
    auto sample = make_sine(1000);
    HeritageFixture fixture(4096, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{4096}};
    const auto output = render(fixture, block);
    REQUIRE(std::any_of(output.begin(), output.begin() + 2000,
                        [](float sample_value) { return std::abs(sample_value) > 0.01f; }));
    REQUIRE(std::all_of(output.begin() + 2000, output.end(),
                        [](float sample_value) { return sample_value == 0.0f; }));
    REQUIRE(PulpSamplerHeritageTestAccess::active_voices(fixture.processor) == 0);
}

TEST_CASE("PulpSampler variable-clock pitch preserves live cyclic one-shot lifetime",
          "[audio][sampler][heritage][typed][pitch][stretch][eof][partition]") {
    const auto profile = variable_pitch_live_cyclic_profile(2.0);
    const auto sample = make_sine(1000);
    HeritageFixture whole(4096, &profile);
    HeritageFixture split(4096, &profile);
    whole.load(sample);
    split.load(sample);
    constexpr std::array one{std::size_t{4096}};
    constexpr std::array many{std::size_t{17},   std::size_t{63},  std::size_t{128},
                              std::size_t{257},  std::size_t{31},  std::size_t{509},
                              std::size_t{1043}, std::size_t{2048}};
    const auto contiguous = render(whole, one, 0, 72);
    REQUIRE(contiguous == render(split, many, 0, 72));
    const auto latency = static_cast<std::size_t>(whole.processor.latency_samples());
    REQUIRE(latency > 0);
    REQUIRE(split.processor.latency_samples() == static_cast<int>(latency));
    const auto first_audible = static_cast<std::size_t>(std::distance(
        contiguous.begin(),
        std::find_if(contiguous.begin(), contiguous.end(),
                     [](float value) { return std::abs(value) > 0.01f; })));
    REQUIRE(first_audible >= latency);
    REQUIRE(first_audible <= latency + 2);
    REQUIRE(std::any_of(contiguous.begin() + latency + 900, contiguous.begin() + latency + 1000,
                        [](float value) { return std::abs(value) > 0.01f; }));
    REQUIRE(std::all_of(contiguous.begin() + latency + 1000, contiguous.end(),
                        [](float value) { return std::abs(value) <= 0.01f; }));
    REQUIRE(PulpSamplerHeritageTestAccess::active_voices(whole.processor) == 0);
    REQUIRE(PulpSamplerHeritageTestAccess::active_voices(split.processor) == 0);
}

TEST_CASE("PulpSampler heritage preserves host-offset MIDI causality",
          "[audio][sampler][heritage][midi]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(256, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{256}};
    const auto output = render(fixture, block, 64);
    REQUIRE(std::all_of(output.begin(), output.begin() + 64,
                        [](float sample_value) { return sample_value == 0.0f; }));
    REQUIRE(std::any_of(output.begin() + 64, output.end(),
                        [](float sample_value) { return std::abs(sample_value) > 0.01f; }));
}

TEST_CASE("PulpSampler heritage reports and renders causal impulse latency",
          "[audio][sampler][heritage][latency]") {
    const auto profile = clock_profile(2.0);
    std::vector<float> impulse(512, 0.0f);
    impulse[0] = 1.0f;
    HeritageFixture fixture(128, &profile);
    fixture.load(impulse);
    constexpr std::array block{std::size_t{128}};
    const auto output = render(fixture, block);
    const auto peak = static_cast<std::size_t>(std::distance(
        output.begin(), std::max_element(output.begin(), output.end(), [](float left, float right) {
            return std::abs(left) < std::abs(right);
        })));
    const auto reported_latency =
        static_cast<std::size_t>(fixture.processor.latency_samples());
    REQUIRE(reported_latency > 0);
    REQUIRE(peak == reported_latency);
    REQUIRE(fixture.processor.descriptor().tail_samples == -1);
}

TEST_CASE("PulpSampler all-bypassed heritage uses the exact legacy render path",
          "[audio][sampler][heritage][bypass]") {
    const auto profile = clock_profile(2.0, true);
    auto sample = make_sine(48000);
    HeritageFixture disabled(512);
    HeritageFixture bypassed(512, &profile);
    disabled.load(sample);
    bypassed.load(sample);
    constexpr std::array blocks{std::size_t{17}, std::size_t{63}, std::size_t{128},
                                std::size_t{304}};
    REQUIRE(render(disabled, blocks) == render(bypassed, blocks));
    REQUIRE(bypassed.processor.latency_samples() == 0);
}

TEST_CASE("PulpSampler typed heritage processes simultaneous voices independently",
          "[audio][sampler][heritage][typed][polyphony]") {
    const auto profile = typed_voice_profile(1.25);
    auto sample = make_sine(8192);
    HeritageFixture fixture(4096, &profile);
    fixture.load(sample);

    std::vector<float> left(4096, 0.0f);
    std::vector<float> right(4096, 0.0f);
    float* output_ptrs[]{left.data(), right.data()};
    const float* input_ptrs[]{nullptr, nullptr};
    audio::BufferView<float> output(output_ptrs, 2, left.size());
    audio::BufferView<const float> input(input_ptrs, 0, left.size());
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    format::ProcessContext context{fixture.sample_rate, static_cast<int>(left.size())};
    fixture.processor.process(output, input, midi_in, midi_out, context);

    const auto latency =
        static_cast<std::size_t>(fixture.processor.latency_samples());
    REQUIRE(latency + 256 < left.size());
    const auto measured = std::span<const float>(left).subspan(latency + 256);
    REQUIRE(tone_projection(measured, 440.0, 48000.0) > 0.25);
    REQUIRE(tone_projection(measured, 880.0, 48000.0) > 0.25);
}

TEST_CASE("PulpSampler typed voice heritage is callback-partition invariant",
          "[audio][sampler][heritage][typed][partition]") {
    const auto profile = typed_voice_profile(1.25);
    auto sample = make_sine(48000);
    HeritageFixture whole(1024, &profile);
    HeritageFixture split(1024, &profile);
    whole.load(sample);
    split.load(sample);
    constexpr std::array one{std::size_t{1024}};
    constexpr std::array many{std::size_t{1}, std::size_t{31},  std::size_t{128},
                              std::size_t{7}, std::size_t{333}, std::size_t{524}};
    REQUIRE(render(whole, one) == render(split, many));
}

TEST_CASE("PulpSampler typed bus runs once after each segment mix",
          "[audio][sampler][heritage][typed][bus]") {
    const auto profile = typed_bus_noise_profile();
    HeritageFixture whole(512, &profile);
    HeritageFixture split(512, &profile);
    constexpr std::array one{std::size_t{512}};
    constexpr std::array many{std::size_t{17}, std::size_t{63}, std::size_t{128}, std::size_t{304}};
    const auto whole_output = render(whole, one);
    REQUIRE(std::any_of(whole_output.begin(), whole_output.end(),
                        [](float sample) { return sample != 0.0f; }));
    REQUIRE(whole_output == render(split, many));
}

TEST_CASE("PulpSampler passes exact voice activity to typed bus gating",
          "[audio][sampler][heritage][typed][bus][gate]") {
    const auto gated_profile =
        typed_bus_profile(0.05f, 0.01f, audio::SampleHeritageNoiseGate::VoiceActive);
    const auto idle_profile =
        typed_bus_profile(0.0f, 0.01f, audio::SampleHeritageNoiseGate::VoiceActive);
    HeritageFixture gated_silent(256, &gated_profile);
    HeritageFixture idle_silent(256, &idle_profile);
    constexpr std::array block{std::size_t{256}};
    REQUIRE(render(gated_silent, block, 0, 60, false) == render(idle_silent, block, 0, 60, false));

    std::vector<float> silent_sample(4096, 0.0f);
    HeritageFixture gated_active(256, &gated_profile);
    HeritageFixture idle_active(256, &idle_profile);
    gated_active.load(silent_sample);
    idle_active.load(silent_sample);
    REQUIRE(render(gated_active, block) != render(idle_active, block));
}

TEST_CASE("PulpSampler closes typed bus gating when a voice ends mid-segment",
          "[audio][sampler][heritage][typed][bus][gate][boundary]") {
    const auto profile =
        typed_bus_profile(0.05f, 0.0f, audio::SampleHeritageNoiseGate::VoiceActive);
    HeritageFixture fixture(64, &profile);
    fixture.load(std::vector<float>(13, 0.0f));
    constexpr std::array block{std::size_t{64}};
    const auto output = render(fixture, block);
    REQUIRE(std::any_of(output.begin(), output.begin() + 13,
                        [](float sample) { return sample != 0.0f; }));
    REQUIRE(std::all_of(output.begin() + 13, output.end(),
                        [](float sample) { return sample == 0.0f; }));
}

TEST_CASE("PulpSampler applies typed output drive once after voice summing",
          "[audio][sampler][heritage][typed][bus][mix]") {
    const auto profile =
        typed_bus_profile(0.0f, 0.0f, audio::SampleHeritageNoiseGate::AlwaysOn, 2.0f, 0.7f);
    std::vector<float> sample(4096, 0.3f);
    HeritageFixture fixture(512, &profile);
    fixture.load(sample);
    fixture.load(sample);

    std::vector<float> left(512, 0.0f);
    std::vector<float> right(512, 0.0f);
    float* output_ptrs[]{left.data(), right.data()};
    const float* input_ptrs[]{nullptr, nullptr};
    audio::BufferView<float> output(output_ptrs, 2, left.size());
    audio::BufferView<const float> input(input_ptrs, 0, left.size());
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    format::ProcessContext context{fixture.sample_rate, static_cast<int>(left.size())};
    fixture.processor.process(output, input, midi_in, midi_out, context);
    constexpr float summed_voices = 0.6f;
    constexpr float drive = 2.0f;
    constexpr float ceiling = 0.7f;
    constexpr float driven_sum = summed_voices * drive;
    const float expected = ceiling * driven_sum /
                           (ceiling + std::abs(driven_sum));
    const auto peak = *std::max_element(left.begin(), left.end());
    REQUIRE(peak == Catch::Approx(expected).margin(1.0e-6f));
    constexpr float driven_voice = 0.3f * drive;
    const float separately_saturated =
        2.0f * ceiling * driven_voice / (ceiling + std::abs(driven_voice));
    REQUIRE(peak < separately_saturated);
    REQUIRE(std::all_of(left.begin(), left.end(),
                        [=](float value) {
                            return value > -ceiling && value < ceiling;
                        }));
}

TEST_CASE("PulpSampler all-bypassed typed heritage is the exact clean path",
          "[audio][sampler][heritage][typed][bypass]") {
    const auto profile = typed_voice_profile(2.0, true);
    auto sample = make_sine(48000);
    HeritageFixture clean(512);
    HeritageFixture bypassed(512, &profile);
    clean.load(sample);
    bypassed.load(sample);
    constexpr std::array blocks{std::size_t{17}, std::size_t{63}, std::size_t{128},
                                std::size_t{304}};
    REQUIRE(render(clean, blocks) == render(bypassed, blocks));
    REQUIRE(bypassed.processor.latency_samples() == 0);
}

TEST_CASE("PulpSampler keeps active record-commit blocks out of playback",
          "[audio][sampler][heritage][typed][record-commit]") {
    auto playback =
        typed_bus_profile(0.0f, 0.0f, audio::SampleHeritageNoiseGate::AlwaysOn, 1.25f, 0.9f);
    playback.profile_id = "neutral.full-playback-control-v3";
    playback.voice = typed_rich_voice_profile().voice;

    auto full = playback;
    full.profile_id = "neutral.full-playback-record-v3";
    full.record_commit = {
        {audio::SampleHeritageBlockDomain::RecordCommit, false,
         audio::SampleHeritageRecordInputDriveClipBlock{1.5f, 0.8f}},
        {audio::SampleHeritageBlockDomain::RecordCommit, false,
         audio::SampleHeritageRecordRateBlock{audio::SampleHeritageRecordFilterFamily::OnePole,
                                              24000.0, audio::SampleHeritageCutoffLaw::FixedHz,
                                              9000.0, 1, 0.0f, 0.0f}},
        {audio::SampleHeritageBlockDomain::RecordCommit, false,
         audio::SampleHeritageRecordConverterBlock{
             audio::SampleHeritageConverterFamily::MuLaw, 8.0f, 0.1f, 0.0f, 0x4321u,
             audio::SampleHeritageSeedPolicy::RestartFromProfileSeed}},
    };

    auto sample = make_sine(4096);
    HeritageFixture control(512, &playback);
    HeritageFixture with_record_commit(512, &full);
    control.load(sample);
    with_record_commit.load(sample);
    constexpr std::array partitions{std::size_t{17}, std::size_t{63}, std::size_t{128},
                                    std::size_t{304}};
    REQUIRE(render(control, partitions) == render(with_record_commit, partitions));
    REQUIRE(with_record_commit.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::Ready);
}

TEST_CASE("PulpSampler treats a record-only profile as the exact clean path",
          "[audio][sampler][heritage][typed][record-commit][bypass]") {
    audio::SampleHeritageProfile profile;
    profile.profile_id = "neutral.record-only-v3";
    profile.host_sample_rate = 48000.0;
    profile.record_commit = {
        {audio::SampleHeritageBlockDomain::RecordCommit, false,
         audio::SampleHeritageRecordInputDriveClipBlock{2.0f, 0.5f}},
    };

    auto sample = make_sine(4096);
    HeritageFixture clean(512);
    HeritageFixture record_only(512, &profile);
    clean.load(sample);
    record_only.load(sample);
    constexpr std::array partitions{std::size_t{17}, std::size_t{63}, std::size_t{128},
                                    std::size_t{304}};
    REQUIRE(render(clean, partitions) == render(record_only, partitions));
    REQUIRE(record_only.processor.latency_samples() == 0);
}

TEST_CASE("PulpSampler resets typed voice state before slot retrigger",
          "[audio][sampler][heritage][typed][reset]") {
    const auto profile = typed_converter_profile();
    auto sample = make_sine(128);
    HeritageFixture fixture(256, &profile);
    HeritageFixture reference(256, &profile);
    fixture.load(sample);
    reference.load(sample);
    constexpr std::array block{std::size_t{256}};
    const auto first = render(fixture, block);
    const auto retriggered = render(fixture, block);
    const auto fresh = render(reference, block);
    REQUIRE(first == fresh);
    REQUIRE(retriggered == fresh);
}

TEST_CASE("PulpSampler routes rich typed voice blocks through native DSP",
          "[audio][sampler][heritage][typed][native]") {
    const auto profile = typed_rich_voice_profile();
    auto sample = make_sine(4096);
    HeritageFixture whole(2048, &profile);
    HeritageFixture split(2048, &profile);
    whole.load(sample);
    split.load(sample);
    constexpr std::array one{std::size_t{2048}};
    constexpr std::array many{std::size_t{17},  std::size_t{63},  std::size_t{128},
                              std::size_t{304}, std::size_t{512}, std::size_t{1024}};
    const auto contiguous = render(whole, one);
    const auto partitioned = render(split, many);
    REQUIRE(contiguous == partitioned);
    REQUIRE(std::all_of(contiguous.begin(), contiguous.end(),
                        [](float value) { return std::isfinite(value); }));
    REQUIRE(contiguous != std::vector<float>(sample.begin(), sample.begin() + contiguous.size()));
}

TEST_CASE("PulpSampler clamps a typed heritage block to the prepared maximum",
          "[audio][sampler][heritage][typed][max-block]") {
    const auto profile = typed_rich_voice_profile();
    auto sample = make_sine(4096);
    constexpr std::size_t prepared_frames = 512;
    HeritageFixture prepared(prepared_frames, &profile);
    HeritageFixture oversized(prepared_frames, &profile);
    prepared.load(sample);
    oversized.load(sample);
    constexpr std::array legal{prepared_frames};
    constexpr std::array oversize{prepared_frames * 4};
    const auto reference = render(prepared, legal);
    const auto clamped = render(oversized, oversize);
    REQUIRE(reference.size() == prepared_frames);
    REQUIRE(clamped.size() == prepared_frames * 4);
    REQUIRE(std::equal(reference.begin(), reference.end(), clamped.begin()));
    REQUIRE(std::all_of(clamped.begin() + prepared_frames, clamped.end(),
                        [](float value) { return value == 0.0f; }));
}

TEST_CASE("PulpSampler drains a finite typed voice tail after source exhaustion",
          "[audio][sampler][heritage][typed][tail]") {
    const auto profile = typed_filter_profile();
    std::vector<float> impulse(128, 0.0f);
    impulse.back() = 1.0f;
    HeritageFixture fixture(128, &profile);
    fixture.load(impulse);
    constexpr std::array block{std::size_t{128}};

    (void)render(fixture, block);
    const auto tail_frames =
        PulpSamplerHeritageTestAccess::heritage_voice_tail_frames(fixture.processor);
    REQUIRE(tail_frames > 0);
    REQUIRE(PulpSamplerHeritageTestAccess::active_voices(fixture.processor) == 1);
    const auto tail = render(fixture, block, 0, 60, false);
    REQUIRE(std::any_of(tail.begin(), tail.end(), [](float sample) { return sample != 0.0f; }));

    const auto remaining_callbacks =
        static_cast<std::size_t>((tail_frames + block[0] - 1) / block[0]) + 1;
    for (std::size_t callback = 0; callback < remaining_callbacks; ++callback)
        (void)render(fixture, block, 0, 60, false);
    REQUIRE(PulpSamplerHeritageTestAccess::active_voices(fixture.processor) == 0);
}

TEST_CASE("PulpSampler credits tail frames consumed in the source-ending block",
          "[audio][sampler][heritage][typed][tail]") {
    const auto profile = typed_filter_profile();
    std::vector<float> impulse(96, 0.0f);
    impulse.back() = 1.0f;
    HeritageFixture fixture(128, &profile);
    fixture.load(impulse);
    constexpr std::array block{std::size_t{128}};
    (void)render(fixture, block);
    const auto total = PulpSamplerHeritageTestAccess::heritage_voice_tail_frames(fixture.processor);
    const auto remaining =
        PulpSamplerHeritageTestAccess::remaining_heritage_tail_frames(fixture.processor);
    REQUIRE(remaining > 0);
    REQUIRE(remaining < total);
}

TEST_CASE("PulpSampler adapts typed streamed stereo to the legacy mono channel",
          "[audio][sampler][heritage][typed][stream][channels]") {
    HeritageTempStereoWav source("typed_mono");
    const auto profile = typed_filter_profile();
    HeritageFixture fixture(256, &profile, 48000.0, {}, 1);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    constexpr std::array block{std::size_t{256}};
    const auto output = render(fixture, block);
    REQUIRE(std::any_of(output.begin(), output.end(), [](float sample) { return sample > 0.01f; }));
    REQUIRE(
        std::none_of(output.begin(), output.end(), [](float sample) { return sample < -0.01f; }));
}

TEST_CASE("PulpSampler prepares typed voice and bus RNG continuation",
          "[audio][sampler][heritage][typed][state]") {
    auto voice = typed_converter_profile();
    auto& converter =
        std::get<audio::SampleHeritageVoiceConverterBlock>(voice.voice.front().parameters);
    converter.seed_policy = audio::SampleHeritageSeedPolicy::ContinueSerializedState;
    SamplerHeritageRuntime voice_runtime;
    REQUIRE(voice_runtime.configure(voice) == PulpSamplerHeritageStatus::PendingPrepare);
    REQUIRE(voice_runtime.prepare(48000.0, 2, 128, 4.0) == PulpSamplerHeritageStatus::Ready);
    const auto voice_state = voice_runtime.capture_runtime_state();
    REQUIRE(voice_state.valid());
    for (std::size_t slot = 0; slot < voice_state.state.voice_states.size(); ++slot) {
        REQUIRE(voice_state.state.voice_states[slot].slot_index == slot);
        REQUIRE(voice_state.state.voice_states[slot].engine.rng_state_count == 1);
    }

    auto bus = typed_bus_noise_profile();
    auto& noise = std::get<audio::SampleHeritageBusNoiseIdleBlock>(bus.bus.front().parameters);
    noise.seed_policy = audio::SampleHeritageSeedPolicy::ContinueSerializedState;
    SamplerHeritageRuntime bus_runtime;
    REQUIRE(bus_runtime.configure(bus) == PulpSamplerHeritageStatus::PendingPrepare);
    REQUIRE(bus_runtime.prepare(48000.0, 2, 128, 4.0) == PulpSamplerHeritageStatus::Ready);
    const auto bus_state = bus_runtime.capture_runtime_state();
    REQUIRE(bus_state.valid());
    REQUIRE(bus_state.state.bus_state.rng_state_count == 1);

    SamplerHeritageRuntime restart_runtime;
    const auto restart = typed_converter_profile();
    REQUIRE(restart_runtime.configure(restart) == PulpSamplerHeritageStatus::PendingPrepare);
    REQUIRE(restart_runtime.prepare(48000.0, 2, 128, 4.0) == PulpSamplerHeritageStatus::Ready);
    REQUIRE_FALSE(restart_runtime.capture_runtime_state().valid());
}

TEST_CASE("PulpSampler prepares typed live stretch with continuable shuffle",
          "[audio][sampler][heritage][typed][stretch]") {
    audio::SampleHeritageProfile live{
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.typed-live-state-v3",
        .host_sample_rate = 48000.0,
        .voice = {{audio::SampleHeritageBlockDomain::Voice, false,
                   audio::SampleHeritageVoiceLiveCyclicStretchBlock{
                       1.0, 10.0, 1.0, true, 0, 0x1234u,
                       audio::SampleHeritageSeedPolicy::ContinueSerializedState}}},
    };
    SamplerHeritageRuntime live_runtime;
    REQUIRE(live_runtime.configure(live) == PulpSamplerHeritageStatus::PendingPrepare);
    REQUIRE(live_runtime.prepare(48000.0, 2, 128, 4.0) == PulpSamplerHeritageStatus::Ready);
    const auto captured = live_runtime.capture_runtime_state();
    REQUIRE(captured.valid());
    REQUIRE(captured.state.voice_states[0].engine.rng_state_count == 1);
    REQUIRE(captured.state.voice_states[0].engine.rng_states[0].stage_type ==
            audio::SampleHeritageRuntimeRngStageType::LiveCyclic);
    SamplerHeritageRuntime restored;
    REQUIRE(restored.configure(live) == PulpSamplerHeritageStatus::PendingPrepare);
    REQUIRE(restored.prepare(48000.0, 2, 128, 4.0, &captured.state) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(
        restored.capture_runtime_state().state.voice_states[0].engine.rng_states[0].random_state ==
        captured.state.voice_states[0].engine.rng_states[0].random_state);
}

TEST_CASE("PulpSampler typed per-voice failure clears the whole segment",
          "[audio][sampler][heritage][typed][failure]") {
    const auto profile = typed_filter_profile();
    auto sample = make_sine(4096);
    HeritageFixture fixture(128, &profile);
    fixture.load(sample);
    PulpSamplerHeritageTestAccess::fail_next_plan(fixture.processor);
    constexpr std::array block{std::size_t{128}};
    const auto output = render(fixture, block);
    REQUIRE(std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0.0f; }));
    REQUIRE(fixture.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::RenderPlanFailed);
}

TEST_CASE("PulpSampler typed pitch artifacts suppress only automatic resident assistance",
          "[audio][sampler][heritage][typed][mip]") {
    const auto profile = typed_pitch_artifact_profile();
    auto sample = make_sine(4096);

    HeritageFixture polynomial(512, &profile);
    polynomial.load(sample);
    polynomial.store.set_value(kSamplerInterpolation, 3.0f);
    constexpr std::array block{std::size_t{512}};
    (void)render(polynomial, block, 0, 72);
    const auto polynomial_diagnostics = polynomial.processor.diagnostics().interpolation;
    REQUIRE(polynomial_diagnostics.resident_mip_suppressions > 0);
    REQUIRE(polynomial_diagnostics.sinc_promotion_suppressions > 0);

    HeritageFixture explicit_sinc(512, &profile);
    explicit_sinc.load(sample);
    explicit_sinc.store.set_value(kSamplerInterpolation, 5.0f);
    (void)render(explicit_sinc, block, 0, 72);
    const auto sinc_diagnostics = explicit_sinc.processor.diagnostics().interpolation;
    REQUIRE(sinc_diagnostics.resident_mip_suppressions == 0);
    REQUIRE(sinc_diagnostics.sinc_promotion_suppressions == 0);
}

TEST_CASE("PulpSampler typed pitch factor one is the exact clean render path",
          "[audio][sampler][heritage][typed][pitch][bypass]") {
    auto sample = make_sine(8192);
    constexpr std::array blocks{std::size_t{17}, std::size_t{63}, std::size_t{128},
                                std::size_t{304}, std::size_t{3584}};
    for (const auto family : {audio::SampleHeritagePitchFamily::VariableClock,
                              audio::SampleHeritagePitchFamily::DropRepeat,
                              audio::SampleHeritagePitchFamily::EarlyLinear}) {
        const auto profile = typed_pitch_artifact_profile(family);
        HeritageFixture clean(4096);
        HeritageFixture pitched(4096, &profile);
        clean.load(sample);
        pitched.load(sample);
        const auto clean_output = render(clean, blocks);
        const auto pitched_output = render(pitched, blocks);
        const auto latency = static_cast<std::size_t>(pitched.processor.latency_samples());
        REQUIRE(latency < pitched_output.size());
        REQUIRE(std::equal(pitched_output.begin() + static_cast<std::ptrdiff_t>(latency),
                           pitched_output.end(), clean_output.begin()));
    }
}

TEST_CASE("PulpSampler typed pitch families keep simultaneous note clocks independent",
          "[audio][sampler][heritage][typed][pitch][polyphony][shipping-gate][g1][image]") {
    auto sample = make_sine(32768);
    for (const auto family : {audio::SampleHeritagePitchFamily::VariableClock,
                              audio::SampleHeritagePitchFamily::DropRepeat,
                              audio::SampleHeritagePitchFamily::EarlyLinear}) {
        auto profile = typed_pitch_artifact_profile(family);
        profile.voice.push_back(
            {audio::SampleHeritageBlockDomain::Voice, false,
             audio::SampleHeritageVoiceHoldDroopBlock{
                 audio::SampleHeritageHoldMode::ZeroOrder, 2, 0.0f}});
        profile.voice.push_back(
            {audio::SampleHeritageBlockDomain::Voice, false,
             audio::SampleHeritageVoiceReconstructionBlock{
                 audio::SampleHeritageReconstructionFamily::OnePole,
                 audio::SampleHeritageCutoffLaw::FixedHz, 20000.0, 1, 0.0f}});
        HeritageFixture fixture(16384, &profile);
        fixture.load(sample);

        std::vector<float> left(16384, 0.0f);
        std::vector<float> right(16384, 0.0f);
        float* output_ptrs[]{left.data(), right.data()};
        const float* input_ptrs[]{nullptr, nullptr};
        audio::BufferView<float> output(output_ptrs, 2, left.size());
        audio::BufferView<const float> input(input_ptrs, 0, left.size());
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;
        midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
        midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
        format::ProcessContext context{fixture.sample_rate, static_cast<int>(left.size())};
        fixture.processor.process(output, input, midi_in, midi_out, context);

        const auto latency = static_cast<std::size_t>(fixture.processor.latency_samples());
        REQUIRE(latency + 2048 < left.size());
        const auto measured = std::span<const float>(left).subspan(latency + 1024);
        REQUIRE(tone_projection(measured, 440.0, 48000.0) > 0.25);
        REQUIRE(tone_projection(measured, 880.0, 48000.0) > 0.25);
        const auto lower_voice_image =
            tone_projection(measured, 24000.0 - 440.0, 48000.0);
        const auto upper_voice_image =
            tone_projection(measured, 24000.0 - 880.0, 48000.0);
        const auto unrelated = tone_projection(measured, 15000.0, 48000.0);
        CAPTURE(static_cast<int>(family), lower_voice_image, upper_voice_image,
                unrelated);
        REQUIRE(lower_voice_image > 0.000001);
        REQUIRE(upper_voice_image > 0.000001);
        REQUIRE(lower_voice_image > unrelated * 2.0);
        REQUIRE(upper_voice_image > unrelated * 2.0);
    }
}

TEST_CASE("PulpSampler fixed-clock pitch families match their sample-index oracles",
          "[audio][sampler][heritage][typed][pitch]") {
    std::vector<float> ramp(256);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<float>(frame) / 256.0f;
    constexpr std::array block{std::size_t{16}};

    const auto drop_profile =
        typed_pitch_artifact_profile(audio::SampleHeritagePitchFamily::DropRepeat);
    HeritageFixture drop(16, &drop_profile);
    drop.load(ramp);
    const auto dropped = render(drop, block, 0, 48);
    for (std::size_t frame = 0; frame < dropped.size(); ++frame)
        REQUIRE(dropped[frame] == ramp[frame / 2]);

    const auto linear_profile =
        typed_pitch_artifact_profile(audio::SampleHeritagePitchFamily::EarlyLinear);
    HeritageFixture linear(16, &linear_profile);
    linear.load(ramp);
    const auto interpolated = render(linear, block, 0, 48);
    for (std::size_t frame = 0; frame < interpolated.size(); ++frame)
        REQUIRE(interpolated[frame] ==
                (ramp[frame / 2] +
                 (frame % 2 == 0 ? 0.0f : (ramp[frame / 2 + 1] - ramp[frame / 2]) * 0.5f)));
}

TEST_CASE("PulpSampler typed pitch families are callback partition invariant",
          "[audio][sampler][heritage][typed][pitch][partition]") {
    auto sample = make_sine(8192);
    constexpr std::array one{std::size_t{1024}};
    constexpr std::array many{std::size_t{1}, std::size_t{31},  std::size_t{128},
                              std::size_t{7}, std::size_t{333}, std::size_t{524}};
    for (const auto family : {audio::SampleHeritagePitchFamily::VariableClock,
                              audio::SampleHeritagePitchFamily::DropRepeat,
                              audio::SampleHeritagePitchFamily::EarlyLinear}) {
        const auto profile = typed_pitch_artifact_profile(family, false, 1.25);
        HeritageFixture whole(4096, &profile);
        HeritageFixture split(4096, &profile);
        whole.load(sample);
        split.load(sample);
        REQUIRE(render(whole, one, 0, 67) == render(split, many, 0, 67));
    }
}

TEST_CASE("PulpSampler fixed-clock pitch artifacts suppress streamed mip assistance",
          "[audio][sampler][heritage][typed][pitch][stream][mip]") {
    HeritageTempWav source("typed_pitch_mip", 2000000);
    audio::SampleMipBuildOptions options;
    options.level_count = 1;
    auto built = audio::build_sample_mip_sidecar(source.path, options);
    INFO(built.error);
    REQUIRE(built.ok);
    auto cleanup = runtime::make_scope_guard([&] {
        std::error_code error;
        std::filesystem::remove(built.manifest_path, error);
        for (const auto& path : built.payload_paths)
            std::filesystem::remove(path, error);
    });

    const auto profile = typed_pitch_artifact_profile(audio::SampleHeritagePitchFamily::DropRepeat);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    constexpr std::array block{std::size_t{64}};
    (void)render(fixture, block, 0, 72);
    const auto interpolation = fixture.processor.diagnostics().interpolation;
    REQUIRE(interpolation.streamed_mip_suppressions > 0);
    REQUIRE(interpolation.sinc_promotion_suppressions > 0);
}
