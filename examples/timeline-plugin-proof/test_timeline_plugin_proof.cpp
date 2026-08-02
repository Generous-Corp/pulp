#include "timeline_plugin_proof.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace pulp;

TEST_CASE("timeline plugin proof owns a session and native editor") {
    examples::TimelinePluginProofProcessor processor;
    REQUIRE(processor.valid());
    REQUIRE(processor.document().revision == timeline::DocumentRevision{0});

    auto editor = processor.create_view();
    REQUIRE(editor != nullptr);
    REQUIRE(dynamic_cast<examples::TimelinePluginProofView*>(editor.get()) != nullptr);
}

TEST_CASE("timeline plugin proof publishes host playhead and emits silence") {
    examples::TimelinePluginProofProcessor processor;
    std::vector<float> input_left(16, 0.25f), input_right(16, -0.25f);
    std::vector<float> output_left(16, 1.0f), output_right(16, 1.0f);
    const float* input_channels[]{input_left.data(), input_right.data()};
    float* output_channels[]{output_left.data(), output_right.data()};
    audio::BufferView<const float> input(input_channels, 2, 16);
    audio::BufferView<float> output(output_channels, 2, 16);
    midi::MidiBuffer midi_in, midi_out;
    format::ProcessContext context{48'000.0, 16};
    context.is_playing = true;
    context.tempo_bpm = 132.0;
    context.position_beats = 2.5;

    processor.process(output, input, midi_in, midi_out, context);

    REQUIRE(std::ranges::all_of(output_left, [](float sample) { return sample == 0.0f; }));
    REQUIRE(std::ranges::all_of(output_right, [](float sample) { return sample == 0.0f; }));
    const auto playhead = processor.playhead();
    REQUIRE(playhead.sequence == 1);
    REQUIRE(playhead.position ==
            timebase::TickPosition{5 * timebase::kTicksPerQuarter / 2});
    REQUIRE(playhead.state == timeline_editor::UiTransportState::Playing);
    REQUIRE(playhead.tempo_bpm == 132.0);
}

TEST_CASE("timeline plugin proof submits an editor intent to its document session") {
    examples::TimelinePluginProofProcessor processor;
    const auto ids = processor.ids();
    const timeline::MusicalTimeRange original{
        timebase::TickPosition{0},
        timebase::TickDuration{4 * timebase::kTicksPerQuarter}};
    const timeline::MusicalTimeRange moved{
        timebase::TickPosition{timebase::kTicksPerQuarter}, original.duration};

    timeline_editor::EditIntent intent;
    intent.kind = timeline_editor::EditIntentKind::Move;
    intent.sequence_id = ids.sequence;
    intent.track_id = ids.track;
    intent.clip_id = ids.clip;
    intent.expected_range = original;
    intent.replacement_range = moved;

    const auto result = processor.submit_intent(intent);
    REQUIRE(result.status == timeline_editor::IntentStatus::Accepted);
    REQUIRE(result.sequence != 0);
    REQUIRE(processor.document().revision == timeline::DocumentRevision{1});

    const auto document = processor.document();
    const auto* sequence = document.snapshot->find_sequence(ids.sequence);
    const auto* track = sequence ? sequence->find_track(ids.track) : nullptr;
    const auto* clip = track ? track->find_clip(ids.clip) : nullptr;
    REQUIRE(clip != nullptr);
    const auto* actual = std::get_if<timeline::MusicalTimeRange>(&clip->time_range());
    REQUIRE(actual != nullptr);
    REQUIRE(actual->start == moved.start);
    REQUIRE(actual->duration == moved.duration);
}

TEST_CASE("timeline plugin proof round trips its project through plugin state") {
    examples::TimelinePluginProofProcessor source;
    const auto ids = source.ids();
    timeline_editor::EditIntent intent;
    intent.kind = timeline_editor::EditIntentKind::Move;
    intent.sequence_id = ids.sequence;
    intent.track_id = ids.track;
    intent.clip_id = ids.clip;
    intent.expected_range = timeline::MusicalTimeRange{
        timebase::TickPosition{0},
        timebase::TickDuration{4 * timebase::kTicksPerQuarter}};
    intent.replacement_range = timeline::MusicalTimeRange{
        timebase::TickPosition{2 * timebase::kTicksPerQuarter},
        timebase::TickDuration{4 * timebase::kTicksPerQuarter}};
    REQUIRE(source.submit_intent(intent).status == timeline_editor::IntentStatus::Accepted);

    const auto saved = source.serialize_plugin_state();
    REQUIRE_FALSE(saved.empty());
    examples::TimelinePluginProofProcessor restored;
    REQUIRE(restored.deserialize_plugin_state(saved));
    REQUIRE(restored.serialize_plugin_state() == saved);

    const auto before_bad_load = restored.serialize_plugin_state();
    const std::vector<std::uint8_t> malformed{'{', 'n', 'o'};
    REQUIRE_FALSE(restored.deserialize_plugin_state(malformed));
    REQUIRE(restored.serialize_plugin_state() == before_bad_load);
}
