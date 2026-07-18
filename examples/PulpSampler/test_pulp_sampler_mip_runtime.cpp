#include "test_pulp_sampler_support.hpp"

TEST_CASE("Streamed mip sidecar rejects stale and truncated manifests",
          "[sampler][mip][stream][sidecar]") {
    SECTION("stale source digest") {
        TempSamplerWav source("mip_sidecar_stale", 4096, 0.5f, 48000);
        TempSamplerMipSidecar sidecar(source);
        std::fstream manifest(sidecar.manifest_path,
                              std::ios::binary | std::ios::in | std::ios::out);
        manifest.seekp(16);
        const char corrupt = static_cast<char>(0xff);
        REQUIRE(manifest.write(&corrupt, 1));
        manifest.close();
        RetainedSamplerFile opened(source.path);
        const auto& base = opened.reader;
        REQUIRE(load_sampler_stream_mip_sidecar(source.path, base, opened.retained).status ==
                SamplerStreamMipSidecarStatus::Invalid);
    }
    SECTION("truncated record") {
        TempSamplerWav source("mip_sidecar_truncated", 4096, 0.5f, 48000);
        TempSamplerMipSidecar sidecar(source);
        std::error_code error;
        std::filesystem::resize_file(sidecar.manifest_path, 100, error);
        REQUIRE_FALSE(error);
        RetainedSamplerFile opened(source.path);
        const auto& base = opened.reader;
        REQUIRE(load_sampler_stream_mip_sidecar(source.path, base, opened.retained).status ==
                SamplerStreamMipSidecarStatus::Invalid);
    }
}

TEST_CASE("Streamed mip sidecar rejects payload replacement", "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_payload", 4096, 0.5f, 48000);
    TempSamplerMipSidecar sidecar(source);
    std::ofstream payload(sidecar.payload_paths[0], std::ios::binary | std::ios::app);
    const char extra = 0;
    REQUIRE(payload.write(&extra, 1));
    payload.close();
    RetainedSamplerFile opened(source.path);
    const auto& base = opened.reader;
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, base, opened.retained).status ==
            SamplerStreamMipSidecarStatus::Invalid);
}

TEST_CASE("PulpSampler atomically publishes authenticated streamed mip bundles",
          "[sampler][mip][stream][integration]") {
    TempSamplerWav source("mip_bundle_publish", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);
    SamplerFixture fixture;
    const auto resident_generation =
        PulpSamplerTestAccess::published_selection_generation(*fixture.proc);
    PulpSamplerTestAccess::pause_before_bundle_publish(*fixture.proc, true);

    std::atomic<bool> loaded{false};
    std::thread loader([&] {
        loaded.store(fixture.proc->load_sample_file(source.path), std::memory_order_release);
    });
    const bool reached_publish_barrier = wait_for_condition(
        [&] { return PulpSamplerTestAccess::bundle_publish_paused(*fixture.proc); });
    const auto kind_while_paused = PulpSamplerTestAccess::published_source_kind(*fixture.proc);
    const auto generation_while_paused =
        PulpSamplerTestAccess::published_selection_generation(*fixture.proc);
    PulpSamplerTestAccess::pause_before_bundle_publish(*fixture.proc, false);
    loader.join();

    REQUIRE(reached_publish_barrier);
    REQUIRE(kind_while_paused == SamplerPublishedSourceKind::Resident);
    REQUIRE(generation_while_paused == resident_generation);
    REQUIRE(loaded.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::published_stream_mip_count(*fixture.proc) == 2);
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 3);
    REQUIRE(fixture.proc->stream_stats().active_sources == 1);
    const auto base = PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 0);
    const auto level_one = PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 1);
    const auto level_two = PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 2);
    REQUIRE(base.valid());
    REQUIRE(level_one.valid());
    REQUIRE(level_two.valid());
    REQUIRE(base.source.source_id != level_one.source.source_id);
    REQUIRE(level_one.source.source_id != level_two.source.source_id);
    REQUIRE(level_one.sample_rate == 22050.0);
    REQUIRE(level_two.sample_rate == 11025.0);
}

TEST_CASE("PulpSampler invalid streamed mip sidecars fall back to the base",
          "[sampler][mip][stream][integration]") {
    TempSamplerWav source("mip_bundle_invalid", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);
    std::fstream manifest(sidecar.manifest_path, std::ios::binary | std::ios::in | std::ios::out);
    manifest.seekp(16);
    const char corrupt = static_cast<char>(0xff);
    REQUIRE(manifest.write(&corrupt, 1));
    manifest.close();
    SamplerFixture fixture;

    REQUIRE(fixture.proc->load_sample_file(source.path));
    REQUIRE(PulpSamplerTestAccess::published_stream_mip_count(*fixture.proc) == 0);
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 1);
    REQUIRE(PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 0).valid());
}

TEST_CASE("PulpSampler rolls back every partial streamed mip admission",
          "[sampler][mip][stream][integration][rollback]") {
    TempSamplerWav source("mip_bundle_rollback", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);
    SamplerFixture fixture;
    const auto resident_generation =
        PulpSamplerTestAccess::published_selection_generation(*fixture.proc);
    const auto memory_baseline = fixture.proc->stream_stats().memory;

    for (int admitted_members = 0; admitted_members <= 3; ++admitted_members) {
        CAPTURE(admitted_members);
        const auto attempts_before =
            PulpSamplerTestAccess::unpublished_rollback_attempts(*fixture.proc);
        PulpSamplerTestAccess::fail_after_stream_member_count(*fixture.proc, admitted_members);
        REQUIRE_FALSE(fixture.proc->load_sample_file(source.path));
        PulpSamplerTestAccess::fail_after_stream_member_count(*fixture.proc, -1);
        REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
                SamplerPublishedSourceKind::Resident);
        REQUIRE(PulpSamplerTestAccess::published_selection_generation(*fixture.proc) ==
                resident_generation);
        REQUIRE(wait_for_condition([&] {
            return PulpSamplerTestAccess::unpublished_rollback_count(*fixture.proc) == 0 &&
                   PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 0;
        }));
        REQUIRE(PulpSamplerTestAccess::unpublished_rollback_attempts(*fixture.proc) -
                    attempts_before ==
                static_cast<std::uint64_t>(admitted_members));
        const auto rolled_back = fixture.proc->stream_stats().memory;
        REQUIRE(rolled_back.current_preload_bytes ==
                memory_baseline.current_preload_bytes);
        REQUIRE(rolled_back.current_page_bytes == memory_baseline.current_page_bytes);
        REQUIRE(rolled_back.current_total_bytes == memory_baseline.current_total_bytes);
    }

    REQUIRE(fixture.proc->load_sample_file(source.path));
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 3);
}

TEST_CASE("PulpSampler selects and latches exact streamed mip octaves",
          "[sampler][mip][stream][integration][interpolation]") {
    TempSamplerWav source("mip_bundle_select", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(source.path));
    const auto level_one = PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 1);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 1);
    const auto active = PulpSamplerTestAccess::active_streamed_asset(*fixture.proc);
    REQUIRE(active.source.source_id == level_one.source.source_id);
    REQUIRE(PulpSamplerTestAccess::active_stream_interpolation(*fixture.proc).policy ==
            audio::SampleInterpolationPolicy::CubicHermite);
    REQUIRE_THAT(block.left.front(), WithinAbs(0.5f, 1.0e-6f));

    const auto token = active.source;
    fixture.store.set_value(kSamplerPitch, 1.0f);
    block.midi_in.clear();
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 1);
    REQUIRE(PulpSamplerTestAccess::active_streamed_asset(*fixture.proc).source.source_id ==
            token.source_id);
    REQUIRE(PulpSamplerTestAccess::active_stream_interpolation(*fixture.proc).policy ==
            audio::SampleInterpolationPolicy::RatioTrackingSinc);
}

TEST_CASE("PulpSampler preserves fractional streamed mip rates in playback",
          "[sampler][mip][stream][integration][rate]") {
    TempSamplerWav source("mip_bundle_fractional", 24000, 0.5f, 22050);
    TempSamplerMipSidecar sidecar(source);
    SamplerFixture fixture(512, 22050.0);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(source.path));

    SamplerProcessBlock block(64, 22050.0);
    block.midi_in.add(midi::MidiEvent::note_on(0, 84, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 2);
    REQUIRE(PulpSamplerTestAccess::active_streamed_asset(*fixture.proc).sample_rate == 5512.5);
    REQUIRE_THAT(PulpSamplerTestAccess::active_streamed_position(*fixture.proc),
                 WithinAbs(64.0, 1.0e-9));
}

TEST_CASE("PulpSampler excludes streamed mips from loops reverse and fractional ratios",
          "[sampler][mip][stream][integration][policy]") {
    TempSamplerWav source("mip_bundle_policy", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);

    for (const auto mode :
         {std::string_view{"loop"}, std::string_view{"reverse"}, std::string_view{"fractional"}}) {
        CAPTURE(mode);
        SamplerFixture fixture;
        fixture.store.set_value(kSamplerAttack, 0.0f);
        fixture.store.set_value(kSamplerDecay, 0.0f);
        fixture.store.set_value(kSamplerSustain, 100.0f);
        fixture.store.set_value(kSamplerInterpolation, 3.0f);
        fixture.store.set_value(kSamplerLoop, mode == "loop" ? 1.0f : 0.0f);
        fixture.store.set_value(kSamplerReverse, mode == "reverse" ? 1.0f : 0.0f);
        REQUIRE(fixture.proc->load_sample_file(source.path));
        const auto base = PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 0);

        SamplerProcessBlock block;
        block.midi_in.add(midi::MidiEvent::note_on(0, mode == "fractional" ? 73 : 72, 127));
        block.run(*fixture.proc);
        REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 0);
        REQUIRE(PulpSamplerTestAccess::active_streamed_asset(*fixture.proc).source.source_id ==
                base.source.source_id);
        REQUIRE_THAT(block.left.front(), WithinAbs(0.5f, 1.0e-6f));
    }
}

TEST_CASE("PulpSampler bounds mip construction without rejecting the base sample",
          "[sampler][mip]") {
    SamplerFixture fixture;
    std::vector<float> source(SamplerResidentMipStore::kMaximumBuildSamples + 1, 0.25f);
    REQUIRE(fixture.proc->load_sample(source.data(), static_cast<int>(source.size()), 48000.0f));
    REQUIRE(PulpSamplerTestAccess::resident_mips(*fixture.proc).level_count == 0);
    REQUIRE(fixture.proc->sample_length() == static_cast<int>(source.size()));
}

TEST_CASE("PulpSampler latches resident mips for up-pitched polynomial reads",
          "[sampler][mip][interpolation]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);

    const auto mips = PulpSamplerTestAccess::resident_mips(*fixture.proc);
    REQUIRE(mips.level_count > 0);
    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 1);
    const auto peak = *std::max_element(block.left.begin(), block.left.end(),
                                        [](float a, float b) { return std::abs(a) < std::abs(b); });
    REQUIRE(std::abs(peak) > 0.01f);

    const auto position_before_automation =
        PulpSamplerTestAccess::active_resident_position(*fixture.proc);
    fixture.store.set_value(kSamplerPitch, 1.0f);
    fixture.store.set_value(kSamplerInterpolation, 2.0f);
    block.midi_in.clear();
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 1);
    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::Linear);
    const auto automated_advance =
        PulpSamplerTestAccess::active_resident_position(*fixture.proc) - position_before_automation;
    REQUIRE_THAT(automated_advance, WithinAbs(512.0 * std::pow(2.0, 1.0 / 12.0), 1e-6));
}

TEST_CASE("PulpSampler uses ratio sinc between resident mip octaves",
          "[sampler][mip][interpolation]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 73, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 0);
    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::RatioTrackingSinc);
}

TEST_CASE("PulpSampler keeps loop boundaries on the base resident asset", "[sampler][mip][loop]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 0);
    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::RatioTrackingSinc);
}

TEST_CASE("PulpSampler keeps reverse one-shots on the phase-correct base asset",
          "[sampler][mip][reverse]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 0);
    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::RatioTrackingSinc);
}

TEST_CASE("PulpSampler keeps extreme resident notes audible when sinc coverage ends",
          "[sampler][interpolation][sinc]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 127, 127));
    block.run(*fixture.proc);

    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::CubicHermite);
    const auto peak =
        *std::max_element(block.left.begin(), block.left.end(),
                          [](float left, float right) { return std::abs(left) < std::abs(right); });
    REQUIRE(std::abs(peak) > 0.01f);
}
