#include "test_pulp_sampler_support.hpp"

TEST_CASE("PulpSampler descriptor", "[sampler]") {
    PulpSamplerProcessor proc;
    auto d = proc.descriptor();
    REQUIRE(d.name == "PulpSampler");
    REQUIRE(d.category == format::PluginCategory::Instrument);
    REQUIRE(d.accepts_midi);
    REQUIRE(d.input_buses.empty());
    REQUIRE(d.output_buses.size() == 1);
}

TEST_CASE("PulpSampler checked prepare reports every public failure and rolls back",
          "[sampler][api][prepare]") {
    auto prepare = [](PulpSamplerProcessor& processor,
                      format::PrepareContext context = {}) {
        context.input_channels = 0;
        context.output_channels = 2;
        processor.prepare(context);
    };

    SECTION("invalid host") {
        PulpSamplerProcessor processor;
        format::PrepareContext context;
        context.sample_rate = 0.0;
        prepare(processor, context);
        REQUIRE(processor.prepare_result().status ==
                PulpSamplerPrepareStatus::InvalidHostConfiguration);
        REQUIRE(PulpSamplerTestAccess::fully_released(processor));
    }
    SECTION("resource limit") {
        PulpSamplerProcessor processor;
        format::PrepareContext context;
        context.resource_limits.max_voices = PulpSamplerProcessor::kMaxVoices - 1;
        prepare(processor, context);
        REQUIRE(processor.prepare_result().status ==
                PulpSamplerPrepareStatus::ResourceLimitExceeded);
        REQUIRE(processor.prepare_result().exceeded_limit ==
                format::PrepareResourceLimit::Voices);
        REQUIRE(PulpSamplerTestAccess::fully_released(processor));
    }
    SECTION("memory budget") {
        const auto estimate = SamplerStreamingRuntime::estimate_prepare(48000.0f, 512);
        PulpSamplerProcessor processor({estimate.required_streaming_memory_bytes - 1});
        prepare(processor);
        REQUIRE(processor.prepare_result().status ==
                PulpSamplerPrepareStatus::StreamingMemoryBudgetTooSmall);
        REQUIRE(PulpSamplerTestAccess::fully_released(processor));
    }
    SECTION("service unavailable") {
        PulpSamplerProcessor processor;
        PulpSamplerTestAccess::fail_next_service_prepare(processor);
        prepare(processor);
        REQUIRE(processor.prepare_result().status ==
                PulpSamplerPrepareStatus::StreamingServiceUnavailable);
        REQUIRE(PulpSamplerTestAccess::fully_released(processor));
    }
    SECTION("allocation failure") {
        PulpSamplerProcessor processor;
        PulpSamplerTestAccess::fail_next_prepare_allocation(processor);
        prepare(processor);
        REQUIRE(processor.prepare_result().status ==
                PulpSamplerPrepareStatus::AllocationFailure);
        REQUIRE(PulpSamplerTestAccess::fully_released(processor));
    }
    SECTION("thread startup failure retries cleanly") {
        PulpSamplerProcessor processor;
        PulpSamplerTestAccess::fail_next_thread_start(processor);
        prepare(processor);
        REQUIRE(processor.prepare_result().status ==
                PulpSamplerPrepareStatus::AllocationFailure);
        REQUIRE(PulpSamplerTestAccess::fully_released(processor));
        prepare(processor);
        REQUIRE(processor.prepare_result().prepared());
    }
    SECTION("resident bank failure retries cleanly") {
        PulpSamplerProcessor processor;
        PulpSamplerTestAccess::fail_next_resident_prepare(processor);
        prepare(processor);
        REQUIRE(processor.prepare_result().status ==
                PulpSamplerPrepareStatus::AllocationFailure);
        REQUIRE(PulpSamplerTestAccess::fully_released(processor));
        prepare(processor);
        REQUIRE(processor.prepare_result().prepared());
    }
    SECTION("mip coefficient failure retries cleanly") {
        PulpSamplerProcessor processor;
        PulpSamplerTestAccess::fail_next_mip_prepare(processor);
        prepare(processor);
        REQUIRE(processor.prepare_result().status ==
                PulpSamplerPrepareStatus::AllocationFailure);
        REQUIRE(PulpSamplerTestAccess::fully_released(processor));
        prepare(processor);
        REQUIRE(processor.prepare_result().prepared());
    }
}

TEST_CASE("PulpSampler resource estimates enforce exact checked limits",
          "[sampler][api][prepare][resources]") {
    format::PrepareContext context;
    context.sample_rate = 48000.0;
    context.max_buffer_size = 512;
    context.input_channels = 0;
    context.output_channels = 2;
    PulpSamplerProcessor reference;
    const auto usage = reference.estimate_prepare_resources(context);
    REQUIRE(usage.persistent_bytes > 46'080'000u);
    REQUIRE(usage.block_scratch_bytes ==
            512u * (PulpSamplerProcessor::kMaxOutputChannels +
                    PulpSamplerProcessor::kMaxSampleChannels) * sizeof(float) +
                512u * sizeof(std::uint8_t));
    REQUIRE(usage.total_bytes() >= usage.persistent_bytes);

    auto require_limit = [&](format::PrepareResourceLimit expected,
                             auto set_limit) {
        PulpSamplerProcessor exact;
        auto exact_context = context;
        set_limit(exact_context, false);
        exact.prepare(exact_context);
        REQUIRE(exact.prepare_result().prepared());
        PulpSamplerProcessor under;
        auto under_context = context;
        set_limit(under_context, true);
        under.prepare(under_context);
        REQUIRE(under.prepare_result().status ==
                PulpSamplerPrepareStatus::ResourceLimitExceeded);
        REQUIRE(under.prepare_result().exceeded_limit == expected);
        REQUIRE(PulpSamplerTestAccess::fully_released(under));
    };
    require_limit(format::PrepareResourceLimit::PersistentBytes,
                  [&](auto& value, bool under) {
        value.resource_limits.max_persistent_bytes =
            usage.persistent_bytes - static_cast<std::size_t>(under);
    });
    require_limit(format::PrepareResourceLimit::BlockScratchBytes,
                  [&](auto& value, bool under) {
        value.resource_limits.max_block_scratch_bytes =
            usage.block_scratch_bytes - static_cast<std::size_t>(under);
    });
    require_limit(format::PrepareResourceLimit::TotalBytes,
                  [&](auto& value, bool under) {
        value.resource_limits.max_total_bytes =
            usage.total_bytes() - static_cast<std::size_t>(under);
    });

    PulpSamplerProcessor below_resident;
    auto below_context = context;
    below_context.resource_limits.max_persistent_bytes = 46'079'999u;
    below_resident.prepare(below_context);
    REQUIRE(below_resident.prepare_result().status ==
            PulpSamplerPrepareStatus::ResourceLimitExceeded);

    PulpSamplerProcessor unlimited({std::numeric_limits<std::uint64_t>::max()});
    const auto saturated = unlimited.estimate_prepare_resources(context);
    REQUIRE(saturated.persistent_bytes ==
            std::numeric_limits<std::size_t>::max());
    REQUIRE(saturated.total_bytes() ==
            std::numeric_limits<std::size_t>::max());
    unlimited.prepare(context);
    REQUIRE(unlimited.prepare_result().prepared());
}

TEST_CASE("PulpSampler rejects host rates that cannot narrow to float",
          "[sampler][api][prepare]") {
    const double invalid_rates[] = {
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::denorm_min(),
    };
    for (const auto rate : invalid_rates) {
        PulpSamplerProcessor processor;
        format::PrepareContext context;
        context.sample_rate = rate;
        context.input_channels = 0;
        context.output_channels = 2;
        processor.prepare(context);
        REQUIRE(processor.prepare_result().status ==
                PulpSamplerPrepareStatus::InvalidHostConfiguration);
        REQUIRE(PulpSamplerTestAccess::fully_released(processor));
    }
}

TEST_CASE("PulpSampler public config load and diagnostics are coherent",
          "[sampler][api][diagnostics]") {
    const auto derived = SamplerStreamingRuntime::estimate_prepare(48000.0f, 512);
    const auto configured = derived.required_streaming_memory_bytes + 4096;
    PulpSamplerProcessor processor;
    REQUIRE(processor.set_config({configured}));
    format::PrepareContext context;
    context.sample_rate = 48000.0;
    context.max_buffer_size = 512;
    context.input_channels = 0;
    context.output_channels = 2;
    const auto estimate = processor.estimate_prepare_resources(context);
    REQUIRE(estimate.persistent_bytes > configured);
    processor.prepare(context);
    REQUIRE(processor.prepare_result().prepared());
    REQUIRE_FALSE(processor.set_config({configured + 1}));
    const auto prepared = processor.prepare_result();
    REQUIRE(prepared.required_streaming_memory_bytes ==
            derived.required_streaming_memory_bytes);
    REQUIRE(prepared.configured_streaming_memory_bytes == configured);
    const auto before = processor.diagnostics();
    REQUIRE(before.streaming_memory_capacity_bytes == configured);
    REQUIRE(before.current_streaming_memory_bytes <= configured);
    REQUIRE(before.peak_streaming_memory_bytes <= configured);

    TempSamplerWav wav("public_api", 100000, 0.25f, 48000);
    const auto loaded = processor.load_sample_file_result(wav.path);
    REQUIRE(loaded.loaded());
    REQUIRE(processor.last_load_result().status == PulpSamplerLoadStatus::Ok);
    const auto diagnostics = processor.diagnostics();
    REQUIRE(diagnostics.last_load.loaded());
    REQUIRE(diagnostics.preload.sufficient());
    REQUIRE(diagnostics.preload.page_frames > 0);
    REQUIRE(diagnostics.streaming_memory_capacity_bytes == configured);
}

TEST_CASE("PulpSampler envelope lifetime survives voice reset without double count",
          "[sampler][api][diagnostics][envelope]") {
    TempSamplerWav wav("envelope_lifetime", 100000, 0.25f);
    SamplerFixture fixture;
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    PulpSamplerTestAccess::seed_and_harvest_envelope(*fixture.proc, 37);
    PulpSamplerTestAccess::publish_envelope(*fixture.proc);
    const auto first = fixture.proc->diagnostics().envelope.lifetime.starved_frames;
    REQUIRE(first == 37);
    PulpSamplerTestAccess::publish_envelope(*fixture.proc);
    REQUIRE(fixture.proc->diagnostics().envelope.lifetime.starved_frames == first);
}

TEST_CASE("PulpSampler serializes two loaders with coherent diagnostics publication",
          "[sampler][api][diagnostics][thread]") {
    TempSamplerWav first_wav("diagnostics_concurrent_first", 100000, 0.25f);
    TempSamplerWav second_wav("diagnostics_concurrent_second", 100000, 0.5f);
    SamplerFixture fixture;
    const auto load_before =
        PulpSamplerTestAccess::control_load_counts(*fixture.proc);
    const auto diagnostics_before =
        PulpSamplerTestAccess::control_diagnostics_counts(*fixture.proc);
    PulpSamplerTestAccess::pause_file_stage(*fixture.proc, true);

    PulpSamplerLoadResult first_loaded, second_loaded;
    PulpSamplerDiagnostics snapshot;
    std::thread first_loader, second_loader, observer;
    auto cleanup = runtime::make_scope_guard([&] {
        PulpSamplerTestAccess::pause_file_stage(*fixture.proc, false);
        if (first_loader.joinable()) first_loader.join();
        if (second_loader.joinable()) second_loader.join();
        if (observer.joinable()) observer.join();
    });

    first_loader = std::thread([&] {
        first_loaded = fixture.proc->load_sample_file_result(first_wav.path);
    });
    REQUIRE(wait_for_condition(
        [&] { return PulpSamplerTestAccess::file_stage_paused(*fixture.proc); }));
    second_loader = std::thread([&] {
        second_loaded = fixture.proc->load_sample_file_result(second_wav.path);
    });
    REQUIRE(wait_for_condition([&] {
        return PulpSamplerTestAccess::control_load_counts(*fixture.proc).first ==
               load_before.first + 2;
    }));
    REQUIRE(PulpSamplerTestAccess::control_load_counts(*fixture.proc).second ==
            load_before.second + 1);

    observer = std::thread([&] { snapshot = fixture.proc->diagnostics(); });
    REQUIRE(wait_for_condition([&] {
        return PulpSamplerTestAccess::control_diagnostics_counts(
                   *fixture.proc).first == diagnostics_before.first + 1;
    }));
    REQUIRE(PulpSamplerTestAccess::control_diagnostics_counts(
                *fixture.proc).second == diagnostics_before.second);

    PulpSamplerTestAccess::pause_file_stage(*fixture.proc, false);
    first_loader.join();
    second_loader.join();
    observer.join();
    REQUIRE(first_loaded.loaded());
    REQUIRE(second_loaded.loaded());
    REQUIRE((snapshot.snapshot_epoch & 1u) == 0);
    REQUIRE(snapshot.snapshot_epoch != 0);
    REQUIRE(snapshot.last_load.selection_generation != 0);
    REQUIRE(snapshot.last_load.selection_generation ==
            snapshot.preload.selection_generation);
    cleanup.dismiss();
}

TEST_CASE("PulpSampler serializes load against release without owner lifetime races",
          "[sampler][api][diagnostics][thread]") {
    TempSamplerWav wav("diagnostics_release_overlap", 100000, 0.25f);
    SamplerFixture fixture;
    const auto release_before =
        PulpSamplerTestAccess::control_release_counts(*fixture.proc);
    PulpSamplerTestAccess::pause_file_stage(*fixture.proc, true);

    PulpSamplerLoadResult loaded;
    std::thread loader, releaser;
    auto cleanup = runtime::make_scope_guard([&] {
        PulpSamplerTestAccess::pause_file_stage(*fixture.proc, false);
        if (loader.joinable()) loader.join();
        if (releaser.joinable()) releaser.join();
    });
    loader = std::thread([&] {
        loaded = fixture.proc->load_sample_file_result(wav.path);
    });
    REQUIRE(wait_for_condition(
        [&] { return PulpSamplerTestAccess::file_stage_paused(*fixture.proc); }));
    releaser = std::thread([&] { fixture.proc->release(); });
    REQUIRE(wait_for_condition([&] {
        return PulpSamplerTestAccess::control_release_counts(*fixture.proc).first ==
               release_before.first + 1;
    }));
    REQUIRE(PulpSamplerTestAccess::control_release_counts(*fixture.proc).second ==
            release_before.second);

    PulpSamplerTestAccess::pause_file_stage(*fixture.proc, false);
    loader.join();
    releaser.join();
    REQUIRE(loaded.loaded());
    REQUIRE(PulpSamplerTestAccess::fully_released(*fixture.proc));
    const auto snapshot = fixture.proc->diagnostics();
    REQUIRE(snapshot.prepare.status == PulpSamplerPrepareStatus::NotPrepared);
    REQUIRE(snapshot.last_load.status == PulpSamplerLoadStatus::NotAttempted);
    REQUIRE(snapshot.preload.selection_generation == 0);
    cleanup.dismiss();
}

TEST_CASE("PulpSampler envelope diagnostics aggregate live voices and saturate",
          "[sampler][api][diagnostics][envelope]") {
    TempSamplerWav wav("envelope_aggregate", 100000, 0.25f);
    SamplerFixture fixture;
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.midi_in.add(midi::MidiEvent::note_on(0, 64, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::mark_all_streamed_voices(*fixture.proc, 11) == 2);
    PulpSamplerTestAccess::publish_envelope(*fixture.proc);
    auto envelope = fixture.proc->diagnostics().envelope;
    REQUIRE(envelope.active_streamed_voices == 2);
    REQUIRE(envelope.ready_voices + envelope.fading_out_voices +
                envelope.silent_voices + envelope.recovering_voices == 2);
    REQUIRE(envelope.lifetime.starved_frames == 22);

    PulpSamplerTestAccess::set_envelope_lifetime_starved(
        *fixture.proc, std::numeric_limits<std::uint64_t>::max() - 5);
    PulpSamplerTestAccess::seed_and_harvest_envelope(*fixture.proc, 11);
    PulpSamplerTestAccess::publish_envelope(*fixture.proc);
    REQUIRE(fixture.proc->diagnostics().envelope.lifetime.starved_frames ==
            std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("PulpSampler envelope lifetime survives natural EOS contract reset and steal",
          "[sampler][api][diagnostics][envelope]") {
    SECTION("natural EOS") {
        TempSamplerWav wav("envelope_eos", 4096, 0.25f);
        SamplerFixture fixture;
        REQUIRE(fixture.proc->load_sample_file(wav.path));
        SamplerProcessBlock block;
        block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
        block.run(*fixture.proc);
        REQUIRE(PulpSamplerTestAccess::mark_all_streamed_voices(*fixture.proc, 7) == 1);
        block.midi_in.clear();
        for (int index = 0; index < 20; ++index) block.run(*fixture.proc);
        REQUIRE(fixture.proc->diagnostics().envelope.lifetime.starved_frames >= 7);
    }
    SECTION("contract rejection") {
        TempSamplerWav wav("envelope_contract", 100000, 0.25f);
        SamplerFixture fixture;
        REQUIRE(fixture.proc->load_sample_file(wav.path));
        SamplerProcessBlock block;
        block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
        block.run(*fixture.proc);
        REQUIRE(PulpSamplerTestAccess::mark_all_streamed_voices(*fixture.proc, 9) == 1);
        REQUIRE(PulpSamplerTestAccess::invalidate_active_stream_preload_contract(
            *fixture.proc));
        block.midi_in.clear();
        block.run(*fixture.proc);
        REQUIRE(fixture.proc->diagnostics().envelope.lifetime.starved_frames >= 9);
    }
    SECTION("steal") {
        TempSamplerWav wav("envelope_steal", 100000, 0.25f);
        SamplerFixture fixture;
        REQUIRE(fixture.proc->load_sample_file(wav.path));
        SamplerProcessBlock block;
        for (int note = 60; note < 68; ++note)
            block.midi_in.add(midi::MidiEvent::note_on(0, note, 127));
        block.run(*fixture.proc);
        REQUIRE(PulpSamplerTestAccess::mark_all_streamed_voices(*fixture.proc, 1) == 8);
        block.midi_in.clear();
        block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
        block.run(*fixture.proc);
        REQUIRE(fixture.proc->diagnostics().envelope.lifetime.starved_frames >= 1);
    }
}

namespace {
/// Render a parameter-id list for a Catch2 INFO line, so a mismatch names the
/// parameter that drifted instead of only reporting two counts.
std::string ids_to_string(const std::vector<state::ParamID>& ids) {
    std::string out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(static_cast<unsigned long long>(ids[i]));
    }
    return out;
}
}  // namespace

TEST_CASE("PulpSampler registers exactly its declared parameter set", "[sampler]") {
    SamplerFixture f;
    // Assert the SET, not just the count. A bare count fails as "10 != 9",
    // which says nothing about which parameter moved; the heritage clock ratio
    // was added while this assertion still read 9, and the example only builds
    // on lanes that compile examples, so nothing caught it before it landed.
    const state::ParamID expected[] = {
        kSamplerGain,          kSamplerAttack,        kSamplerDecay,
        kSamplerSustain,       kSamplerRelease,       kSamplerPitch,
        kSamplerLoop,          kSamplerReverse,       kSamplerInterpolation,
        kSamplerHeritageClockRatio,
    };
    const auto params = f.store.all_params();

    std::vector<state::ParamID> actual;
    actual.reserve(params.size());
    for (const auto& info : params) actual.push_back(info.id);

    std::vector<state::ParamID> wanted(std::begin(expected), std::end(expected));
    INFO("registered ids: " << ids_to_string(actual));
    INFO("declared ids:   " << ids_to_string(wanted));
    REQUIRE(actual == wanted);
}

TEST_CASE("PulpSampler exposes each scalar interpolation policy", "[sampler]") {
    SamplerFixture f;
    const audio::SampleInterpolationPolicy expected[] = {
        audio::SampleInterpolationPolicy::Hold,
        audio::SampleInterpolationPolicy::Nearest,
        audio::SampleInterpolationPolicy::Linear,
        audio::SampleInterpolationPolicy::CubicHermite,
        audio::SampleInterpolationPolicy::CubicLagrange,
        audio::SampleInterpolationPolicy::RatioTrackingSinc,
    };
    for (std::size_t index = 0; index < std::size(expected); ++index) {
        f.store.set_value(kSamplerInterpolation, static_cast<float>(index));
        REQUIRE(PulpSamplerTestAccess::interpolation_policy(*f.proc) == expected[index]);
    }
    const auto sinc = PulpSamplerTestAccess::sinc_bank(*f.proc);
    REQUIRE(sinc.valid());
    const auto maximum_consumption = SamplerStreamingRuntime::maximum_pitch_ratio() *
                                     SamplerStreamingRuntime::maximum_source_sample_rate() /
                                     44100.0;
    REQUIRE(sinc.select(maximum_consumption).valid());
}
