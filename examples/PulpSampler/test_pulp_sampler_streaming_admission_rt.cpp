#include "test_pulp_sampler_support.hpp"

TEST_CASE("PulpSampler reports deterministic streamed starvation", "[sampler][stream]") {
    TempSamplerWav wav("starvation", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);

    const auto preload = fixture.proc->stream_stats().preload_frames;
    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    const auto blocks = static_cast<std::uint32_t>(preload / 512) + 3;
    for (std::uint32_t index = 1; index < blocks; ++index) {
        block.run(*fixture.proc);
    }

    REQUIRE(fixture.proc->stream_stats().starved_output_frames > 0);
    REQUIRE(fixture.proc->stream_stats().service_starvation_events > 0);
    REQUIRE(fixture.proc->stream_stats().decode_failure_events == 0);
    REQUIRE(fixture.proc->stream_stats().invalid_preload_contract_events == 0);
    REQUIRE(fixture.proc->stream_stats().normal_end_of_source_events == 0);
    REQUIRE_THAT(block.left.back(), WithinAbs(0.0f, 0.0f));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
}

TEST_CASE("PulpSampler distinguishes decode failure from service starvation",
          "[sampler][stream][diagnostics]") {
    TempSamplerWav wav("decode_failure_diagnostics", 100000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    PulpSamplerTestAccess::fail_next_stream_decode(*fixture.proc);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();

    REQUIRE(wait_for_condition([&] {
        block.run(*fixture.proc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return fixture.proc->stream_stats().decode_failure_events != 0;
    }));
    const auto diagnostics = fixture.proc->stream_stats();
    REQUIRE(diagnostics.decode_failure_events == 1);
    REQUIRE(diagnostics.invalid_preload_contract_events == 0);
    REQUIRE(diagnostics.stale_generation_events == 0);
    REQUIRE(diagnostics.normal_end_of_source_events == 0);
    REQUIRE(diagnostics.invalid_render_contract_events == 0);

    // Decode failures are worker-side root causes. If dispatch is then held
    // until the voice reaches the failed/unavailable region, the resulting
    // output starvation is a distinct symptom and both counters must remain.
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);
    REQUIRE(wait_for_condition([&] {
        block.run(*fixture.proc);
        return fixture.proc->stream_stats().service_starvation_events != 0;
    }));
    const auto root_and_symptom = fixture.proc->stream_stats();
    REQUIRE(root_and_symptom.decode_failure_events == 1);
    REQUIRE(root_and_symptom.service_starvation_events > 0);
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);

    fixture.proc->release();
    REQUIRE(fixture.proc->stream_stats().decode_failure_events == 1);
}

TEST_CASE("PulpSampler reports an invalid streamed preload contract distinctly",
          "[sampler][stream][diagnostics]") {
    TempSamplerWav wav("invalid_contract_diagnostics", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    REQUIRE(PulpSamplerTestAccess::invalidate_active_stream_preload_contract(
        *fixture.proc));
    block.run(*fixture.proc);

    const auto diagnostics = fixture.proc->stream_stats();
    REQUIRE(diagnostics.invalid_preload_contract_events == 1);
    REQUIRE(diagnostics.service_starvation_events == 0);
    REQUIRE(diagnostics.decode_failure_events == 0);
    REQUIRE(diagnostics.stale_generation_events == 0);
    REQUIRE(diagnostics.normal_end_of_source_events == 0);
    REQUIRE(diagnostics.invalid_render_contract_events == 0);
}

TEST_CASE("PulpSampler reports normal streamed end of source distinctly",
          "[sampler][stream][diagnostics]") {
    TempSamplerWav wav("normal_eos_diagnostics", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerPitch, 24.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    for (std::uint32_t callback = 0;
         callback < 100 &&
         fixture.proc->stream_stats().normal_end_of_source_events == 0;
         ++callback) {
        block.run(*fixture.proc);
        block.midi_in.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto diagnostics = fixture.proc->stream_stats();
    REQUIRE(diagnostics.normal_end_of_source_events == 1);
    REQUIRE(diagnostics.service_starvation_events == 0);
    REQUIRE(diagnostics.decode_failure_events == 0);
    REQUIRE(diagnostics.invalid_preload_contract_events == 0);
    REQUIRE(diagnostics.stale_generation_events == 0);
    REQUIRE(diagnostics.invalid_render_contract_events == 0);
}

TEST_CASE("PulpSampler keeps staggered streamed voices supplied", "[sampler][stream][polyphony]") {
    TempSamplerWav wav("polyphony", 200000, 0.25f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block;
    for (int voice = 0; voice < PulpSamplerProcessor::kMaxVoices; ++voice) {
        block.midi_in.clear();
        block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
        block.run(*fixture.proc);
        block.midi_in.clear();
        for (int spacing = 1; spacing < 16; ++spacing) {
            block.run(*fixture.proc);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (int tail = 0; tail < 20; ++tail) {
        block.run(*fixture.proc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(fixture.proc->stream_stats().pages_published > 0);
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    REQUIRE_THAT(block.left.front(), WithinAbs(2.0f, 1e-5));
}

TEST_CASE("PulpSampler rejects streamed note admission above serialized source throughput",
          "[sampler][stream][contract][admission]") {
    TempSamplerWav wav("aggregate_rate_admission", 500000, 0.25f);
    SamplerFixture fixture(64);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    const auto source = PulpSamplerTestAccess::published_stream_source(*fixture.proc);
    const auto page_frames =
        PulpSamplerTestAccess::published_stream_page_frames(*fixture.proc);

    SamplerProcessBlock block(64);
    for (int voice = 0; voice < PulpSamplerProcessor::kMaxVoices; ++voice) {
        block.midi_in.add(midi::MidiEvent::note_on(0, 84, 100));
        block.run(*fixture.proc);
        block.midi_in.clear();
    }

    const auto active = PulpSamplerTestAccess::active_streamed_voices_for_source(
        *fixture.proc, source);
    const auto diagnostics = fixture.proc->stream_stats();
    REQUIRE(active > 0);
    REQUIRE(active < PulpSamplerProcessor::kMaxVoices);
    REQUIRE(active * 4.0 * 44100.0 <= page_frames / 0.005);
    REQUIRE((active + 1) * 4.0 * 44100.0 > page_frames / 0.005);
    const auto notes_before_rejected_steal =
        PulpSamplerTestAccess::active_streamed_notes(*fixture.proc);
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 100));
    block.run(*fixture.proc);
    block.midi_in.clear();
    REQUIRE(PulpSamplerTestAccess::active_streamed_notes(*fixture.proc) ==
            notes_before_rejected_steal);
    REQUIRE(fixture.proc->stream_stats().aggregate_rate_admission_rejections ==
            diagnostics.aggregate_rate_admission_rejections + 1);
    REQUIRE(diagnostics.aggregate_rate_admission_rejections > 0);
    REQUIRE(diagnostics.aggregate_rate_automation_rejections == 0);
    REQUIRE(diagnostics.service_starvation_events == 0);
}

TEST_CASE("PulpSampler sheds streamed voices when pitch automation exceeds source throughput",
          "[sampler][stream][contract][automation]") {
    TempSamplerWav wav("aggregate_rate_automation", 500000, 0.25f);
    SamplerFixture fixture(64);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    const auto source = PulpSamplerTestAccess::published_stream_source(*fixture.proc);
    const auto page_frames =
        PulpSamplerTestAccess::published_stream_page_frames(*fixture.proc);

    SamplerProcessBlock block(64);
    for (int voice = 0; voice < PulpSamplerProcessor::kMaxVoices; ++voice) {
        block.midi_in.add(midi::MidiEvent::note_on(0, 60, 100));
        block.run(*fixture.proc);
        block.midi_in.clear();
    }
    REQUIRE(PulpSamplerTestAccess::active_streamed_voices_for_source(
                *fixture.proc, source) == PulpSamplerProcessor::kMaxVoices);

    fixture.store.set_value(kSamplerPitch, 24.0f);
    block.run(*fixture.proc);
    const auto active = PulpSamplerTestAccess::active_streamed_voices_for_source(
        *fixture.proc, source);
    const auto diagnostics = fixture.proc->stream_stats();
    REQUIRE(active > 0);
    REQUIRE(active < PulpSamplerProcessor::kMaxVoices);
    REQUIRE(active * 4.0 * 44100.0 <= page_frames / 0.005);
    REQUIRE(diagnostics.aggregate_rate_admission_rejections == 0);
    REQUIRE(diagnostics.aggregate_rate_automation_rejections > 0);
    REQUIRE(diagnostics.service_starvation_events == 0);
}

TEST_CASE("PulpSampler click-gates an automated source-throughput rejection",
          "[sampler][stream][contract][automation][click]") {
    TempSamplerWav wav("aggregate_rate_automation_fade", 500000, 0.5f);
    SamplerFixture fixture(64);
    fixture.store.set_value(kSamplerGain, 0.0f);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock attack(64);
    attack.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    attack.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::force_active_stream_rate_capacity(
        *fixture.proc, 1.0));
    fixture.store.set_value(kSamplerPitch, 12.0f);

    SamplerProcessBlock fade(64);
    fade.run(*fixture.proc);
    REQUIRE(std::abs(fade.left.front()) > 0.1f);
    REQUIRE_THAT(fade.left.back(), WithinAbs(0.0f, 1.0e-6f));
    REQUIRE(fixture.proc->stream_stats().aggregate_rate_automation_rejections == 1);
    const auto source = PulpSamplerTestAccess::published_stream_source(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_voices_for_source(
                *fixture.proc, source) == 0);
    SamplerProcessBlock tail(64);
    tail.run(*fixture.proc);
    REQUIRE_THAT(tail.left.front(), WithinAbs(0.0f, 1.0e-6f));
    REQUIRE(fixture.proc->stream_stats().aggregate_rate_automation_rejections == 1);

    // A click is an impulsive adjacent-sample discontinuity. Include both block
    // seams in the measured capture so an endpoint-only check cannot miss one.
    std::vector<float> rejection_capture;
    rejection_capture.reserve(1 + fade.left.size() + 1);
    rejection_capture.push_back(attack.left.back());
    rejection_capture.insert(rejection_capture.end(),
                             fade.left.begin(),
                             fade.left.end());
    rejection_capture.push_back(tail.left.front());
    const auto maximum_adjacent_step = [](const auto& samples) {
        float maximum = 0.0f;
        for (std::size_t frame = 1; frame < samples.size(); ++frame)
            maximum = std::max(maximum,
                               std::abs(samples[frame] - samples[frame - 1]));
        return maximum;
    };
    constexpr auto kMaximumClickSafeStep = 0.025f;
    const std::array known_bad_hard_cut{0.5f, 0.0f, 0.0f};
    REQUIRE(maximum_adjacent_step(known_bad_hard_cut) > kMaximumClickSafeStep);
    REQUIRE(maximum_adjacent_step(rejection_capture) < kMaximumClickSafeStep);
}

TEST_CASE("PulpSampler counts fading voices during mid-block note admission",
          "[sampler][stream][contract][automation][admission]") {
    TempSamplerWav wav("aggregate_rate_mid_fade_admission", 500000, 0.5f);
    SamplerFixture fixture(64);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    const auto source = PulpSamplerTestAccess::published_stream_source(*fixture.proc);

    SamplerProcessBlock attack(64);
    attack.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    attack.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::force_active_stream_rate_capacity(
        *fixture.proc, 100000.0));
    fixture.store.set_value(kSamplerPitch, 12.0f);

    SamplerProcessBlock automated(64);
    auto later_low_note = midi::MidiEvent::note_on(0, 48, 127);
    later_low_note.sample_offset = 32;
    automated.midi_in.add(later_low_note);
    automated.run(*fixture.proc);

    const auto diagnostics = fixture.proc->stream_stats();
    REQUIRE(diagnostics.aggregate_rate_automation_rejections == 1);
    REQUIRE(diagnostics.aggregate_rate_admission_rejections == 1);
    REQUIRE(PulpSamplerTestAccess::active_streamed_voices_for_source(
                *fixture.proc, source) == 0);
}

TEST_CASE("PulpSampler in-contract stream torture survives bounded eviction and churn",
          "[sampler][stream][polyphony][torture]") {
    TempSamplerWav short_loop("torture_short_loop", 32000, 0.2f);
    TempSamplerWav long_source("torture_long", 500000, 0.25f);
    SamplerFixture fixture(64);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);

    SamplerProcessBlock block(64);
    REQUIRE(fixture.proc->load_sample_file(short_loop.path));
    fixture.store.set_value(kSamplerLoop, 1.0f);
    fixture.store.set_value(kSamplerReverse, 0.0f);
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 100));
    block.run(*fixture.proc);
    block.midi_in.clear();
    block.midi_in.add(midi::MidiEvent::note_on(0, 67, 100));
    block.run(*fixture.proc);
    block.midi_in.clear();
    const auto short_source =
        PulpSamplerTestAccess::published_stream_source(*fixture.proc);
    const auto short_page_frames =
        PulpSamplerTestAccess::published_stream_page_frames(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_voices_for_source(
                *fixture.proc, short_source) == 2);
    REQUIRE(fixture.proc->load_sample_file(long_source.path));
    const auto long_source_token =
        PulpSamplerTestAccess::published_stream_source(*fixture.proc);
    const auto long_page_frames =
        PulpSamplerTestAccess::published_stream_page_frames(*fixture.proc);
    REQUIRE((long_source_token.source_id != short_source.source_id ||
             long_source_token.source_generation != short_source.source_generation));
    constexpr auto kCertifiedDecoderLatencySeconds = 0.005;
    constexpr std::array notes{48, 60, 72, 84};
    constexpr std::array churn_notes{36, 43};
    constexpr std::uint32_t kTortureCallbacks = 1800;
    constexpr std::uint32_t kChurnIntervalCallbacks = 300;
    constexpr std::uint32_t kLastChurnCallback = 1500;
    const auto independent_note_ratio = [](int note) {
        return std::exp2((note - 60) / 12.0);
    };
    double long_source_maximum_aggregate_ratio = 0.0;
    for (std::size_t voice = 1; voice < notes.size(); ++voice)
        long_source_maximum_aggregate_ratio +=
            independent_note_ratio(notes[voice]);
    for (std::uint32_t callback = kChurnIntervalCallbacks;
         callback <= kLastChurnCallback;
         callback += kChurnIntervalCallbacks) {
        const auto steal = callback / kChurnIntervalCallbacks;
        // Conservative independent upper bound: count every new long-source
        // note without assuming which prior voice the production allocator
        // steals.
        long_source_maximum_aggregate_ratio +=
            independent_note_ratio(churn_notes[steal % churn_notes.size()]);
    }
    const auto short_source_aggregate_ratio =
        1.0 + std::exp2(7.0 / 12.0);
    REQUIRE(short_source_aggregate_ratio * 44100.0 <=
            short_page_frames / kCertifiedDecoderLatencySeconds);
    REQUIRE(long_source_maximum_aggregate_ratio * 44100.0 <=
            long_page_frames / kCertifiedDecoderLatencySeconds);

    constexpr int kConcurrentTortureVoices = 4;
    for (int voice = 0; voice < kConcurrentTortureVoices - 1; ++voice) {
        fixture.store.set_value(kSamplerLoop, voice % 3 == 0 ? 0.0f : 1.0f);
        fixture.store.set_value(kSamplerReverse, voice % 2 == 0 ? 0.0f : 1.0f);
        block.midi_in.add(midi::MidiEvent::note_on(
            0, notes[static_cast<std::size_t>(voice + 1) % notes.size()], 100));
        block.run(*fixture.proc);
        block.midi_in.clear();
    }

    using TortureClock = std::chrono::steady_clock;
    constexpr auto kCallbackFrames = 64;
    constexpr auto kHostSampleRate = 44100;
    constexpr auto kMaximumSharedRunnerOverrun = std::chrono::seconds(2);
    const auto pressure_baseline = fixture.proc->stream_stats();
    const auto torture_started = TortureClock::now();
    bool observed_both_sources_under_eviction = false;
    std::uint32_t first_starved_callback = kTortureCallbacks;
    for (std::uint32_t callback = 0; callback < kTortureCallbacks; ++callback) {
        if (callback != 0 && callback <= kLastChurnCallback &&
            callback % kChurnIntervalCallbacks == 0) {
            // Force deterministic steals while alternating forward one-shots
            // and crossfade loops at mixed note-derived ratios.
            const auto steal = callback / kChurnIntervalCallbacks;
            fixture.store.set_value(kSamplerLoop, steal % 2 == 0 ? 0.0f : 1.0f);
            // Reverse refill recovery is gated separately above. Keep churn
            // steals forward so unpredictable note-on timing does not redefine
            // an intentional held attack as an in-contract output underrun.
            fixture.store.set_value(kSamplerReverse, 0.0f);
            block.midi_in.add(midi::MidiEvent::note_on(
                0, churn_notes[steal % churn_notes.size()], 110));
        }
        block.run(*fixture.proc);
        block.midi_in.clear();
        if (first_starved_callback == kTortureCallbacks &&
            fixture.proc->stream_stats().starved_output_frames != 0)
            first_starved_callback = callback;
        const auto live = fixture.proc->stream_stats();
        observed_both_sources_under_eviction |=
            live.cache_pages_retired > pressure_baseline.cache_pages_retired &&
            live.cache_pages_reused > pressure_baseline.cache_pages_reused &&
            PulpSamplerTestAccess::active_streamed_voices_for_source(
                *fixture.proc, short_source) > 0 &&
            PulpSamplerTestAccess::active_streamed_voices_for_source(
                *fixture.proc, long_source_token) > 0;

        // Drive the owner/service/decode threads at the declared host cadence:
        // callback N may use wall time only until its 64/44.1 kHz deadline.
        const auto deadline = torture_started +
                              std::chrono::duration_cast<TortureClock::duration>(
                                  std::chrono::duration<double>(
                                      (callback + 1) * kCallbackFrames /
                                      static_cast<double>(kHostSampleRate)));
        std::this_thread::sleep_until(deadline);
    }

    const auto torture_elapsed = TortureClock::now() - torture_started;
    const auto rendered_duration = std::chrono::duration_cast<TortureClock::duration>(
        std::chrono::duration<double>(
            kTortureCallbacks * kCallbackFrames /
            static_cast<double>(kHostSampleRate)));
    const auto diagnostics = fixture.proc->stream_stats();
    CAPTURE(first_starved_callback, diagnostics.pages_published,
            diagnostics.cache_pages_retired, diagnostics.cache_pages_reused,
            diagnostics.decode_source_outstanding_high_water,
            diagnostics.decode_completed_frames,
            diagnostics.same_source_reader_concurrency_high_water,
            diagnostics.cache_async_reservations_high_water,
            diagnostics.active_reservations_high_water,
            diagnostics.aggregate_rate_admission_rejections,
            diagnostics.aggregate_rate_automation_rejections,
            diagnostics.decode_scratch_bytes,
            diagnostics.current_total_memory_bytes,
            diagnostics.total_memory_capacity_bytes,
            std::chrono::duration_cast<std::chrono::microseconds>(
                torture_elapsed - rendered_duration)
                .count());
    REQUIRE(diagnostics.pages_published - pressure_baseline.pages_published > 128);
    REQUIRE(diagnostics.cache_pages_retired > pressure_baseline.cache_pages_retired);
    REQUIRE(diagnostics.cache_pages_reused > pressure_baseline.cache_pages_reused);
    REQUIRE(diagnostics.decode_source_outstanding_high_water > 1);
    REQUIRE(diagnostics.decode_completed_frames >
            pressure_baseline.decode_completed_frames);
    REQUIRE(diagnostics.same_source_reader_concurrency_high_water == 1);
    REQUIRE(diagnostics.cache_async_reservations_high_water > 1);
    REQUIRE(diagnostics.active_reservations_high_water > 1);
    REQUIRE(diagnostics.aggregate_rate_admission_rejections == 0);
    REQUIRE(diagnostics.aggregate_rate_automation_rejections == 0);
    REQUIRE(diagnostics.decode_scratch_bytes ==
            2ULL * 8ULL * 2ULL * long_page_frames * sizeof(float));
    REQUIRE(diagnostics.total_memory_capacity_bytes ==
            diagnostics.memory.capacity_bytes + diagnostics.decode_scratch_bytes);
    REQUIRE(diagnostics.current_total_memory_bytes ==
            diagnostics.memory.current_total_bytes + diagnostics.decode_scratch_bytes);
    REQUIRE(diagnostics.peak_total_memory_bytes ==
            diagnostics.memory.peak_total_bytes + diagnostics.decode_scratch_bytes);
    REQUIRE(observed_both_sources_under_eviction);
    // Retain a gross realtime acceptance contract without grading shared-runner
    // scheduling at the former aggregate +10 ms precision. Two seconds of
    // aggregate tolerance absorbs ordinary CI descheduling while still failing
    // a major regression that cannot keep this 2.6-second render near cadence.
    REQUIRE(torture_elapsed <=
            rendered_duration + kMaximumSharedRunnerOverrun);
    REQUIRE(diagnostics.starved_output_frames == 0);
    REQUIRE(diagnostics.service_starvation_events == 0);
    REQUIRE(diagnostics.decode_failure_events == 0);
    REQUIRE(diagnostics.invalid_preload_contract_events == 0);
    REQUIRE(diagnostics.stale_generation_events == 0);
    REQUIRE(diagnostics.invalid_render_contract_events == 0);
    REQUIRE(diagnostics.current_total_memory_bytes <=
            diagnostics.total_memory_capacity_bytes);
    REQUIRE(diagnostics.peak_total_memory_bytes <=
            diagnostics.total_memory_capacity_bytes);
}

TEST_CASE("PulpSampler retains a replaced stream until its voice acknowledges",
          "[sampler][stream]") {
    TempSamplerWav first("replacement_a", 24000, 0.75f);
    TempSamplerWav second("replacement_b", 24000, 0.25f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);
    REQUIRE(fixture.proc->load_sample_file(first.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    REQUIRE(fixture.proc->load_sample_file(second.path));
    REQUIRE(fixture.proc->stream_stats().active_sources == 2);
    REQUIRE(fixture.proc->stream_stats().sources_retired == 0);

    block.midi_in.clear();
    block.midi_in.add(midi::MidiEvent::note_off(0, 60));
    block.run(*fixture.proc);
    REQUIRE(wait_for_condition([&] {
        return fixture.proc->stream_stats().active_sources == 1 &&
               fixture.proc->stream_stats().sources_retired == 1;
    }));
}

TEST_CASE("PulpSampler retires replaced streamed mip bundles as one generation",
          "[sampler][mip][stream][integration][lifetime]") {
    TempSamplerWav first("mip_replacement_a", 24000, 0.75f, 44100);
    TempSamplerMipSidecar first_sidecar(first);
    TempSamplerWav second("mip_replacement_b", 24000, 0.5f, 44100);
    TempSamplerMipSidecar second_sidecar(second);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(first.path));
    const auto one_bundle_memory = fixture.proc->stream_stats().memory;
    REQUIRE(one_bundle_memory.current_preload_bytes > 0);
    REQUIRE(one_bundle_memory.current_page_bytes > 0);
    REQUIRE(one_bundle_memory.current_total_bytes ==
            one_bundle_memory.current_preload_bytes +
                one_bundle_memory.current_page_bytes);
    REQUIRE(one_bundle_memory.current_total_bytes <= one_bundle_memory.capacity_bytes);
    const std::array first_tokens{
        PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 0).source,
        PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 1).source,
        PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 2).source,
    };

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 1);
    REQUIRE(fixture.proc->load_sample_file(second.path));
    REQUIRE(fixture.proc->stream_stats().active_sources == 2);
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 6);
    const auto two_bundle_memory = fixture.proc->stream_stats().memory;
    REQUIRE(two_bundle_memory.current_preload_bytes ==
            2 * one_bundle_memory.current_preload_bytes);
    REQUIRE(two_bundle_memory.current_page_bytes ==
            2 * one_bundle_memory.current_page_bytes);
    REQUIRE(two_bundle_memory.current_total_bytes ==
            two_bundle_memory.current_preload_bytes +
                two_bundle_memory.current_page_bytes);
    REQUIRE(two_bundle_memory.current_total_bytes <= two_bundle_memory.capacity_bytes);
    REQUIRE(two_bundle_memory.peak_total_bytes <= two_bundle_memory.capacity_bytes);
    for (const auto token : first_tokens) {
        REQUIRE(PulpSamplerTestAccess::service_contains_source(*fixture.proc, token));
    }

    block.midi_in.clear();
    block.midi_in.add(midi::MidiEvent::note_off(0, 72));
    block.run(*fixture.proc);
    REQUIRE(wait_for_condition([&] {
        return fixture.proc->stream_stats().active_sources == 1 &&
               fixture.proc->stream_stats().sources_retired == 1 &&
               PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 3;
    }));
    for (const auto token : first_tokens) {
        REQUIRE_FALSE(PulpSamplerTestAccess::service_contains_source(*fixture.proc, token));
    }
    const auto retired_memory = fixture.proc->stream_stats().memory;
    REQUIRE(retired_memory.current_preload_bytes ==
            one_bundle_memory.current_preload_bytes);
    REQUIRE(retired_memory.current_page_bytes == one_bundle_memory.current_page_bytes);
    REQUIRE(retired_memory.current_total_bytes == one_bundle_memory.current_total_bytes);
}

TEST_CASE("PulpSampler rejects a third streamed mip bundle until a slot retires",
          "[sampler][mip][stream][integration][capacity]") {
    TempSamplerWav first("mip_capacity_a", 24000, 0.75f, 44100);
    TempSamplerMipSidecar first_sidecar(first);
    TempSamplerWav second("mip_capacity_b", 24000, 0.5f, 44100);
    TempSamplerMipSidecar second_sidecar(second);
    TempSamplerWav third("mip_capacity_c", 24000, 0.25f, 44100);
    TempSamplerMipSidecar third_sidecar(third);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(first.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(fixture.proc->load_sample_file(second.path));
    const auto second_generation =
        PulpSamplerTestAccess::published_selection_generation(*fixture.proc);
    const auto stage_attempts = PulpSamplerTestAccess::file_stage_attempts(*fixture.proc);
    REQUIRE_FALSE(fixture.proc->load_sample_file(third.path));
    REQUIRE(PulpSamplerTestAccess::file_stage_attempts(*fixture.proc) == stage_attempts);
    REQUIRE(PulpSamplerTestAccess::published_selection_generation(*fixture.proc) ==
            second_generation);
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 6);

    block.midi_in.clear();
    block.midi_in.add(midi::MidiEvent::note_off(0, 72));
    block.run(*fixture.proc);
    REQUIRE(wait_for_condition([&] { return fixture.proc->stream_stats().active_sources == 1; }));
    REQUIRE(fixture.proc->load_sample_file(third.path));
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 6);
}

TEST_CASE("PulpSampler streamed process stays allocation-free for 10000 callbacks",
          "[sampler][stream][rt]") {
    TempSamplerWav wav("rt", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block(64);
    const auto note_on = midi::MidiEvent::note_on(0, 60, 127);
    block.midi_in.add(note_on);
    block.run(*fixture.proc);
    block.midi_in.clear();

    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback) {
        block.midi_in.clear();
        if ((callback % 512) == 0)
            block.midi_in.add(note_on);
        block.run(*fixture.proc);
    }
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("PulpSampler streamed mip process stays allocation-free for 10000 callbacks",
          "[sampler][mip][stream][integration][rt]") {
    TempSamplerWav wav("mip_rt", 1400000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(wav);
    SamplerFixture fixture(64);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block(64);
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 1);

    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback) {
        block.run(*fixture.proc);
    }
    REQUIRE(probe.allocation_count() == 0);
}
