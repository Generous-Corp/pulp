#include "support/timeline_graph_binding_test_support.hpp"

#include <pulp/format/playback_context_projection.hpp>
#include <pulp/sequence/sequence_processor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <optional>
#include <string>

namespace {

class UmpOutputAttachment {
  public:
    UmpOutputAttachment(midi::MidiBuffer& output, std::size_t capacity) : output_(output) {
        ump_.reserve(capacity);
        ump_.set_realtime_capacity_limit(true);
        output_.attach_ump(&ump_);
    }
    ~UmpOutputAttachment() {
        output_.attach_ump(nullptr);
    }
    UmpOutputAttachment(const UmpOutputAttachment&) = delete;
    UmpOutputAttachment& operator=(const UmpOutputAttachment&) = delete;

    const midi::UmpBuffer& ump() const noexcept {
        return ump_;
    }

  private:
    midi::MidiBuffer& output_;
    midi::UmpBuffer ump_;
};


std::shared_ptr<const Project> stretch_latency_alignment_project(std::uint64_t source_frames) {
    ClipPlaybackProperties playback;
    auto impulse = take(
        Clip::create_absolute({100}, {0}, 512, {48'000, 1}, MediaRef{{3}, {0}, 512}, playback));
    auto stretch =
        take(Clip::create({101}, {0}, {kTicksPerQuarter}, MediaRef{{4}, {0}, source_frames},
                          playback, TimeConform::Stretch));

    NoteEvent note;
    note.id = {103};
    note.start = {};
    note.duration = {kTicksPerQuarter};
    note.velocity = 0xffff;
    note.pitch = 60;
    auto notes = take(MidiContent::create({note}));
    auto note_clip = take(Clip::create({102}, {0}, {2 * kTicksPerQuarter}, std::move(notes)));
    auto impulse_track =
        take(Track::create({10}, "latency alignment impulse", {std::move(impulse)}));
    auto stretch_track = take(Track::create({11}, "latency declaration", {std::move(stretch)}));
    auto note_track = take(Track::create({12}, "latency alignment note", {std::move(note_clip)}));
    auto sequence =
        take(Sequence::create({2}, "root", TickDuration{10 * kTicksPerQuarter},
                              std::vector<Track>{std::move(impulse_track), std::move(stretch_track),
                                                 std::move(note_track)}));
    const auto impulse_hash = ContentHash::from_hex(std::string(64, 'a'));
    const auto stretch_hash = ContentHash::from_hex(std::string(64, 'b'));
    REQUIRE(impulse_hash);
    REQUIRE(stretch_hash);
    MediaAsset impulse_asset{
        .id = {3},
        .name = "impulse.wav",
        .frame_count = source_frames,
        .sample_rate = {48'000, 1},
        .content_hash = *impulse_hash,
    };
    MediaAsset stretch_asset{
        .id = {4},
        .name = "silent-stretch.wav",
        .frame_count = source_frames,
        .sample_rate = {48'000, 1},
        .content_hash = *stretch_hash,
    };
    return std::make_shared<const Project>(
        take(Project::create(ProjectInput{{1},
                                          "stretch latency",
                                          1'000,
                                          {2},
                                          {impulse_asset, stretch_asset},
                                          {std::move(sequence)}})));
}

constexpr std::uint32_t kTempoChangeSampleRate = 4'800;
constexpr std::uint64_t kTempoChangeSourceFrames = 2'400;
constexpr std::int64_t kTempoChangeClipTicks = kTicksPerQuarter / 2;
constexpr std::int64_t kTempoChangeAlignmentTick = 3 * kTicksPerQuarter / 8;

std::shared_ptr<const CompiledTempoMap> tempo_change_alignment_map() {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{kTicksPerQuarter}, 180.0, TempoCurve::Constant},
    };
    return std::make_shared<const CompiledTempoMap>(
        take(CompiledTempoMap::compile(points, RationalRate{kTempoChangeSampleRate, 1})));
}

std::shared_ptr<const Project> tempo_change_alignment_project(TimeConform conform) {
    ClipPlaybackProperties playback;
    auto audio_clip =
        take(Clip::create({200}, {0}, {kTempoChangeClipTicks},
                          MediaRef{{3}, {0}, kTempoChangeSourceFrames}, playback, conform));

    NoteEvent note;
    note.id = {202};
    note.start = {kTempoChangeAlignmentTick};
    note.duration = {kTicksPerQuarter / 16};
    note.velocity = 0xffff;
    note.pitch = 64;
    auto notes = take(MidiContent::create({note}));
    auto note_clip =
        take(Clip::create({201}, {0}, {kTempoChangeClipTicks}, std::move(notes)));
    auto audio_track = take(Track::create({20}, "tempo-conformed audio", {std::move(audio_clip)}));
    auto note_track = take(Track::create({21}, "tempo-aligned MIDI", {std::move(note_clip)}));
    auto sequence =
        take(Sequence::create({2}, "root", TickDuration{kTempoChangeClipTicks},
                              std::vector<Track>{std::move(audio_track), std::move(note_track)}));

    const auto hash = ContentHash::from_hex(std::string(64, 'c'));
    REQUIRE(hash);
    ProjectInput input;
    input.id = {1};
    input.name = conform == TimeConform::Stretch ? "tempo-change acceptance"
                                                 : "tempo-change negative control";
    input.next_item_id = 1'000;
    input.root_sequence_id = {2};
    input.assets = {{
        .id = {3},
        .name = "midpoint-transient.wav",
        .frame_count = kTempoChangeSourceFrames,
        .sample_rate = {kTempoChangeSampleRate, 1},
        .content_hash = *hash,
    }};
    input.sequences = {std::move(sequence)};
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{kTicksPerQuarter}, 180.0, TempoCurve::Constant},
    };
    input.tempo_map = take(TempoMap::create(points));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

} // namespace

TEST_CASE("embedded sequence processor aligns MIDI with Stretch-declared audio latency") {
    constexpr std::uint64_t source_frames = 24'000;
    std::vector<float> impulse(source_frames, 0.0f);
    impulse[0] = 1.0f;
    std::vector<float> stretch(source_frames, 0.05f);
    const auto impulse_hash = ContentHash::from_hex(std::string(64, 'a'));
    const auto stretch_hash = ContentHash::from_hex(std::string(64, 'b'));
    REQUIRE(impulse_hash);
    REQUIRE(stretch_hash);
    const auto assets = take(DecodedAudioAssetPool::create(
        {DecodedAudioAsset{{3}, audio_data(std::move(impulse)), *impulse_hash, {}},
         DecodedAudioAsset{{4}, audio_data(std::move(stretch)), *stretch_hash, {}}}));
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(stretch_latency_alignment_project(source_frames), map, assets, 1);

    sequence::SequenceProcessorConfig processor_config;
    processor_config.output_channels = 1;
    sequence::SequenceProcessor embedded(programs.store, processor_config);
    embedded.prepare({
        .sample_rate = 48'000.0,
        .max_buffer_size = 128,
        .input_channels = 0,
        .output_channels = 1,
    });
    REQUIRE(embedded.ready());
    const auto latency = embedded.latency_samples();
    REQUIRE(latency > 0);

    // A poisoned same-generation runtime is replaceable from the still-live
    // store publication. The exact publication pointer changes, so the queued
    // pre-failure note cannot leak from the discarded runtime generation.
    {
        sequence::SequenceProcessor recovery(programs.store, processor_config);
        recovery.prepare({
            .sample_rate = 48'000.0,
            .max_buffer_size = 128,
            .input_channels = 0,
            .output_channels = 1,
        });
        REQUIRE(recovery.ready());
        Buffer recovery_input(1, 128);
        Buffer recovery_output(1, 128);
        auto recovery_view = recovery_output.view();
        midi::MidiBuffer recovery_midi_in;
        midi::MidiBuffer recovery_midi_out;
        recovery_midi_out.reserve(16);
        recovery_midi_out.set_realtime_capacity_limit(true);
        UmpOutputAttachment recovery_ump(recovery_midi_out, 16);
        const auto recovery_context = [](std::int64_t position) {
            format::ProcessContext context;
            context.sample_rate = 48'000.0;
            context.num_samples = 128;
            context.is_playing = true;
            context.position_samples = position;
            context.position_beats = static_cast<double>(position) / 24'000.0;
            context.tempo_bpm = 120.0;
            context.transport_validity.set(format::TransportField::BeatPosition);
            context.transport_validity.set(format::TransportField::Tempo);
            context.transport_validity.set(format::TransportField::SamplePosition);
            return context;
        };
        auto first = recovery_context(0);
        recovery.process(recovery_view, recovery_input.const_view(), recovery_midi_in,
                         recovery_midi_out, first);
        REQUIRE(recovery.status() == sequence::SequenceProcessorStatus::Ready);
        REQUIRE(recovery_midi_out.empty());
        recovery.force_realtime_stretch_failure_for_test();
        auto rejected = recovery_context(128);
        recovery.process(recovery_view, recovery_input.const_view(), recovery_midi_in,
                         recovery_midi_out, rejected);
        REQUIRE(recovery.status() == sequence::SequenceProcessorStatus::RenderFailed);
        REQUIRE(recovery_midi_out.empty());
        REQUIRE(recovery.adopt_latest_program());
        auto resumed = recovery_context(256);
        recovery.process(recovery_view, recovery_input.const_view(), recovery_midi_in,
                         recovery_midi_out, resumed);
        REQUIRE(recovery.status() == sequence::SequenceProcessorStatus::Ready);
        REQUIRE(recovery_midi_out.empty());
    }

    {
        sequence::SequenceProcessor missing_sidecar(programs.store, processor_config);
        missing_sidecar.prepare({
            .sample_rate = 48'000.0,
            .max_buffer_size = 128,
            .input_channels = 0,
            .output_channels = 1,
        });
        REQUIRE(missing_sidecar.ready());
        Buffer input(1, 128);
        Buffer output(1, 128);
        auto output_view = output.view();
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out_without_ump;
        midi_out_without_ump.reserve(16);
        midi_out_without_ump.set_realtime_capacity_limit(true);
        format::ProcessContext context;
        context.sample_rate = 48'000.0;
        context.num_samples = 128;
        context.is_playing = true;
        context.position_samples = 0;
        context.position_beats = 0.0;
        context.tempo_bpm = 120.0;
        context.transport_validity.set(format::TransportField::BeatPosition);
        context.transport_validity.set(format::TransportField::Tempo);
        context.transport_validity.set(format::TransportField::SamplePosition);
        missing_sidecar.process(output_view, input.const_view(), midi_in, midi_out_without_ump,
                                context);
        REQUIRE(missing_sidecar.status() == sequence::SequenceProcessorStatus::RenderFailed);
        REQUIRE(midi_out_without_ump.empty());
    }

    {
        sequence::SequenceProcessor stopped(programs.store, processor_config);
        stopped.prepare({
            .sample_rate = 48'000.0,
            .max_buffer_size = 128,
            .input_channels = 0,
            .output_channels = 1,
        });
        REQUIRE(stopped.ready());
        Buffer input(1, 128);
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;
        midi_out.reserve(16);
        midi_out.set_realtime_capacity_limit(true);
        UmpOutputAttachment ump_out(midi_out, 16);
        const auto context_at = [](std::int64_t position, bool playing) {
            format::ProcessContext context;
            context.sample_rate = 48'000.0;
            context.num_samples = 128;
            context.is_playing = playing;
            context.position_samples = position;
            context.position_beats = static_cast<double>(position) / 24'000.0;
            context.tempo_bpm = 120.0;
            context.transport_validity.set(format::TransportField::BeatPosition);
            context.transport_validity.set(format::TransportField::Tempo);
            context.transport_validity.set(format::TransportField::SamplePosition);
            return context;
        };
        const auto warm_blocks = (static_cast<std::uint64_t>(latency) + 255u) / 128u + 2u;
        bool heard_audio = false;
        for (std::uint64_t block = 0; block < warm_blocks; ++block) {
            Buffer output(1, 128);
            auto output_view = output.view();
            auto context = context_at(static_cast<std::int64_t>(block * 128u), true);
            stopped.process(output_view, input.const_view(), midi_in, midi_out, context);
            REQUIRE(stopped.status() == sequence::SequenceProcessorStatus::Ready);
            heard_audio =
                heard_audio || std::any_of(output.storage[0].begin(), output.storage[0].end(),
                                           [](float sample) { return std::abs(sample) > 1.0e-6f; });
        }
        REQUIRE(heard_audio);

        Buffer stopped_output(1, 128);
        auto stopped_view = stopped_output.view();
        auto stop_context = context_at(static_cast<std::int64_t>(warm_blocks * 128u), false);
        stopped.process(stopped_view, input.const_view(), midi_in, midi_out, stop_context);
        REQUIRE(stopped.status() == sequence::SequenceProcessorStatus::Ready);
        REQUIRE(std::all_of(stopped_output.storage[0].begin(), stopped_output.storage[0].end(),
                            [](float sample) { return sample == 0.0f; }));
        REQUIRE(std::count_if(midi_out.begin(), midi_out.end(), [](const auto& event) {
                    return event.is_note_off() && event.sample_offset == 0;
                }) == 1);
        REQUIRE(std::none_of(midi_out.begin(), midi_out.end(),
                             [](const auto& event) { return event.is_note_on(); }));
        REQUIRE(std::count_if(ump_out.ump().begin(), ump_out.ump().end(), [](const auto& event) {
                    return (event.packet.status() & 0xf0u) == 0x80u && event.sample_offset == 0;
                }) == 1);

        // Restart must be indistinguishable from a fresh processor at the same
        // transport jump through the complete block containing the exact
        // latency boundary, not merely floor(latency / block) silent blocks.
        sequence::SequenceProcessor fresh(programs.store, processor_config);
        fresh.prepare({
            .sample_rate = 48'000.0,
            .max_buffer_size = 128,
            .input_channels = 0,
            .output_channels = 1,
        });
        REQUIRE(fresh.ready());
        midi::MidiBuffer fresh_midi_out;
        fresh_midi_out.reserve(16);
        fresh_midi_out.set_realtime_capacity_limit(true);
        UmpOutputAttachment fresh_ump(fresh_midi_out, 16);
        std::optional<std::uint64_t> restarted_audio_sample;
        std::optional<std::uint64_t> fresh_audio_sample;
        std::optional<std::uint64_t> restarted_note_sample;
        std::optional<std::uint64_t> fresh_note_sample;
        const auto restart_blocks = (static_cast<std::uint64_t>(latency) + 255u) / 128u;
        for (std::uint64_t block = 0; block < restart_blocks; ++block) {
            Buffer output(1, 128);
            Buffer fresh_output(1, 128);
            auto output_view = output.view();
            auto fresh_view = fresh_output.view();
            auto context = context_at(static_cast<std::int64_t>(block * 128u), true);
            context.transport_jump = block == 0;
            stopped.process(output_view, input.const_view(), midi_in, midi_out, context);
            fresh.process(fresh_view, input.const_view(), midi_in, fresh_midi_out, context);
            REQUIRE(stopped.status() == sequence::SequenceProcessorStatus::Ready);
            REQUIRE(fresh.status() == sequence::SequenceProcessorStatus::Ready);
            REQUIRE(output.storage == fresh_output.storage);
            REQUIRE(midi_out.size() == fresh_midi_out.size());
            REQUIRE(ump_out.ump().size() == fresh_ump.ump().size());
            for (std::size_t frame = 0; frame < 128; ++frame) {
                if (!restarted_audio_sample && std::abs(output.storage[0][frame]) > 0.5f)
                    restarted_audio_sample = block * 128u + frame;
                if (!fresh_audio_sample && std::abs(fresh_output.storage[0][frame]) > 0.5f)
                    fresh_audio_sample = block * 128u + frame;
            }
            for (const auto& event : midi_out)
                if (!restarted_note_sample && event.is_note_on())
                    restarted_note_sample = block * 128u + event.sample_offset;
            for (const auto& event : fresh_midi_out)
                if (!fresh_note_sample && event.is_note_on())
                    fresh_note_sample = block * 128u + event.sample_offset;
        }
        REQUIRE(restarted_audio_sample == fresh_audio_sample);
        REQUIRE(restarted_note_sample == fresh_note_sample);
        REQUIRE(restarted_audio_sample == std::optional<std::uint64_t>{latency});
        REQUIRE(restarted_note_sample == restarted_audio_sample);
    }

    Buffer silence(1, 128);
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_out.reserve(16);
    midi_out.set_realtime_capacity_limit(true);
    UmpOutputAttachment midi_ump(midi_out, 16);
    std::optional<std::uint64_t> impulse_sample;
    std::optional<std::uint64_t> note_sample;
    std::optional<std::uint64_t> ump_note_sample;
    const auto block_count = (static_cast<std::uint64_t>(latency) + 255u) / 128u;
    for (std::uint64_t block = 0; block < block_count; ++block) {
        Buffer audio_output(1, 128);
        auto audio_view = audio_output.view();
        format::ProcessContext context;
        context.sample_rate = 48'000.0;
        context.num_samples = 128;
        context.is_playing = true;
        context.position_samples = static_cast<std::int64_t>(block * 128u);
        context.position_beats = static_cast<double>(context.position_samples) / 24'000.0;
        context.tempo_bpm = 120.0;
        context.transport_validity.set(format::TransportField::BeatPosition);
        context.transport_validity.set(format::TransportField::Tempo);
        context.transport_validity.set(format::TransportField::SamplePosition);

        std::size_t allocations = 0;
        {
            test::ScopedRtProcessProbe probe;
            embedded.process(audio_view, silence.const_view(), midi_in, midi_out, context);
            allocations = probe.allocation_count();
        }
        REQUIRE(allocations == 0);
        INFO("block " << block << " processor status " << static_cast<int>(embedded.status())
                      << " audio status "
                      << static_cast<int>(embedded.last_observation().audio_status));
        REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::Ready);
        for (std::size_t frame = 0; frame < 128; ++frame) {
            if (!impulse_sample && std::abs(audio_output.storage[0][frame]) > 0.5f)
                impulse_sample = block * 128u + frame;
        }
        for (const auto& event : midi_out) {
            if (!note_sample && event.is_note_on())
                note_sample = block * 128u + static_cast<std::uint32_t>(event.sample_offset);
        }
        for (const auto& event : midi_ump.ump()) {
            if (!ump_note_sample)
                ump_note_sample = block * 128u + static_cast<std::uint32_t>(event.sample_offset);
        }
    }

    REQUIRE(impulse_sample);
    REQUIRE(note_sample);
    REQUIRE(ump_note_sample);
    REQUIRE(*impulse_sample == static_cast<std::uint64_t>(latency));
    REQUIRE(*note_sample == *impulse_sample);
    REQUIRE(*ump_note_sample == *impulse_sample);
}

TEST_CASE("tempo-map change aligns audio and MIDI with executable reverted-conform control",
          "[sequence][realtime-stretch][acceptance]") {
    std::vector<float> source(kTempoChangeSourceFrames, 0.0f);
    constexpr std::size_t transient_center = 3 * kTempoChangeSourceFrames / 4;
    constexpr std::size_t transient_radius = 400;
    for (std::size_t frame = transient_center - transient_radius;
         frame <= transient_center + transient_radius; ++frame) {
        const auto phase = static_cast<double>(frame - (transient_center - transient_radius)) /
                           static_cast<double>(2 * transient_radius);
        const auto envelope = std::sin(std::numbers::pi * phase);
        source[frame] = static_cast<float>(
            envelope * std::sin(2.0 * std::numbers::pi * 440.0 *
                                static_cast<double>(frame - transient_center) /
                                kTempoChangeSampleRate));
    }
    const auto hash = ContentHash::from_hex(std::string(64, 'c'));
    REQUIRE(hash);
    auto decoded_audio = std::make_shared<audio::AudioFileData>();
    decoded_audio->sample_rate = kTempoChangeSampleRate;
    decoded_audio->channels = {std::move(source)};
    const auto assets = take(DecodedAudioAssetPool::create(
        {DecodedAudioAsset{{3}, std::move(decoded_audio), *hash, {}}}));
    const auto map = tempo_change_alignment_map();
    const auto alignment_sample =
        static_cast<std::uint64_t>(map->ticks_to_samples({kTempoChangeAlignmentTick}).value);
    REQUIRE(alignment_sample > 1'300);
    REQUIRE(alignment_sample < 1'400);

    struct Observation {
        std::uint64_t audio_peak_sample = 0;
        std::optional<std::uint64_t> note_sample;
        float audio_peak = 0.0f;
        std::uint32_t latency = 0;
    };
    const auto observe = [&](TimeConform conform) {
        ProgramHarness programs;
        ProgramCompileRequest request;
        request.project = tempo_change_alignment_project(conform);
        request.sequence_id = {2};
        request.tempo_map = map;
        request.document_revision = 1;
        request.dirty.all = true;
        request.audio_assets = assets;
        REQUIRE(programs.compiler.submit(std::move(request)));
        INFO("compile code " << static_cast<int>(programs.compiler.status().last_error.code)
                             << " audio detail "
                             << static_cast<int>(
                                    programs.compiler.status().last_error.audio_detail)
                             << " stretch detail "
                             << static_cast<int>(
                                    programs.compiler.status().last_error.offline_stretch_detail));
        REQUIRE_FALSE(programs.compiler.status().has_error);

        sequence::SequenceProcessorConfig processor_config;
        processor_config.output_channels = 1;
        sequence::SequenceProcessor processor(programs.store, processor_config);
        processor.prepare({
            .sample_rate = static_cast<double>(kTempoChangeSampleRate),
            .max_buffer_size = 128,
            .input_channels = 0,
            .output_channels = 1,
        });
        REQUIRE(processor.ready());

        Observation observation;
        observation.latency = static_cast<std::uint32_t>(processor.latency_samples());
        Buffer silence(1, 128);
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;
        midi_out.reserve(16);
        midi_out.set_realtime_capacity_limit(true);
        UmpOutputAttachment midi_ump(midi_out, 16);
        const auto render_end =
            std::max(alignment_sample + observation.latency + 1'024u,
                     static_cast<std::uint64_t>(transient_center + 512u));
        for (std::uint64_t position = 0; position < render_end; position += 128u) {
            Buffer output(1, 128);
            auto output_view = output.view();
            const auto tick = map->samples_to_ticks(
                {static_cast<std::int64_t>(position)});
            format::ProcessContext context;
            context.sample_rate = static_cast<double>(kTempoChangeSampleRate);
            context.num_samples = 128;
            context.is_playing = true;
            context.position_samples = static_cast<std::int64_t>(position);
            context.position_beats =
                static_cast<double>(tick.value) / static_cast<double>(kTicksPerQuarter);
            context.tempo_bpm = map->tempo_at_tick(tick);
            context.transport_jump = position == 0;
            context.transport_validity.set(format::TransportField::BeatPosition);
            context.transport_validity.set(format::TransportField::Tempo);
            context.transport_validity.set(format::TransportField::SamplePosition);
            processor.process(output_view, silence.const_view(), midi_in, midi_out, context);
            INFO("conform " << static_cast<int>(conform) << " position " << position
                            << " status " << static_cast<int>(processor.status()));
            REQUIRE(processor.status() == sequence::SequenceProcessorStatus::Ready);

            for (std::size_t frame = 0; frame < output.storage[0].size(); ++frame) {
                const auto magnitude = std::abs(output.storage[0][frame]);
                if (magnitude > observation.audio_peak) {
                    observation.audio_peak = magnitude;
                    observation.audio_peak_sample = position + frame;
                }
            }
            for (const auto& event : midi_out) {
                if (!observation.note_sample && event.is_note_on() && event.note() == 64)
                    observation.note_sample = position + event.sample_offset;
            }
        }
        REQUIRE(observation.audio_peak > 0.01f);
        REQUIRE(observation.note_sample);
        return observation;
    };

    const auto conformed = observe(TimeConform::Stretch);
    const auto negative = observe(TimeConform::None);
    const auto distance = [](std::uint64_t left, std::uint64_t right) {
        return left > right ? left - right : right - left;
    };
    INFO("alignment sample " << alignment_sample << " conformed latency " << conformed.latency
                             << " audio peak " << conformed.audio_peak_sample << " note "
                             << (conformed.note_sample ? *conformed.note_sample : 0)
                             << " negative audio peak " << negative.audio_peak_sample
                             << " negative note "
                             << (negative.note_sample ? *negative.note_sample : 0));

    // The acceptance oracle crosses Project -> compiler -> SequenceProcessor ->
    // realtime audio and MIDI outputs. The transient's source midpoint is the
    // chosen ramp tick: Stretch must move it from native sample 1,800 to the
    // document's earlier tempo-ramp sample, while the processor applies one declared
    // latency to both outputs.
    REQUIRE(conformed.latency > 0);
    REQUIRE(distance(*conformed.note_sample, alignment_sample + conformed.latency) <= 1);
    const auto mapped_transient_half_width = static_cast<std::uint64_t>(std::ceil(
        static_cast<double>(transient_radius) * 60.0 /
        map->tempo_at_tick({kTempoChangeAlignmentTick})));
    REQUIRE(mapped_transient_half_width < transient_radius);
    REQUIRE(distance(conformed.audio_peak_sample, *conformed.note_sample) <=
            mapped_transient_half_width + 2);

    // Executable negative control for the production mutation
    // `TimeConform::Stretch -> TimeConform::None` (or bypassing the compiled
    // stretch artifact in track_audio_program_compiler.cpp): inputs differ
    // only at that conform seam, while the asset, tempo map, processor and
    // detector are identical. Reverting conform wiring therefore makes the
    // positive assertion above fail rather than allowing a vacuous silence pass.
    REQUIRE(negative.latency == 0);
    REQUIRE(distance(*negative.note_sample, alignment_sample) <= 1);
    REQUIRE(distance(negative.audio_peak_sample, *negative.note_sample) > 300);
}
