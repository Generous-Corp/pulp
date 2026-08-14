#include "timeline_plugin_proof.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/plugin_view_host.hpp>

#include <algorithm>
#include <vector>

using namespace pulp;

namespace {

class RecordingPluginViewHost final : public view::PluginViewHost {
public:
    view::NativeViewHandle native_handle() override { return nullptr; }
    void attach_to_parent(view::NativeViewHandle) override {}
    void detach() override {}
    void repaint() override { ++repaint_count; }
    void set_size(std::uint32_t width, std::uint32_t height) override {
        size_ = {width, height};
    }
    Size get_size() const override { return size_; }

    int repaint_count = 0;

private:
    Size size_{};
};

examples::TimelinePluginProofView& proof_view(std::unique_ptr<view::View>& view) {
    auto* proof = dynamic_cast<examples::TimelinePluginProofView*>(view.get());
    REQUIRE(proof != nullptr);
    return *proof;
}

float pitch_y(std::uint8_t pitch) {
    auto projection = timeline_editor::PitchProjection::create(
        48, 71,
        timeline_editor::PixelSpan{0.0f, examples::kTimelinePluginProofViewHeight});
    REQUIRE(projection);
    return projection->y_at(pitch);
}

float tick_x(std::int64_t tick) {
    return examples::kTimelinePluginProofViewWidth * static_cast<float>(tick) /
           static_cast<float>(examples::kTimelinePluginProofClipTicks);
}

const timeline::MidiContent& midi_content(const timeline::Project& project) {
    const examples::TimelinePluginProofIds ids;
    const auto* sequence = project.find_sequence(ids.sequence);
    const auto* track = sequence ? sequence->find_track(ids.track) : nullptr;
    const auto* clip = track ? track->find_clip(ids.clip) : nullptr;
    REQUIRE(clip != nullptr);
    const auto* content = std::get_if<timeline::MidiContent>(&clip->content());
    REQUIRE(content != nullptr);
    return *content;
}

const timeline::NoteEvent* find_note(const timeline::Project& project, timeline::ItemId id) {
    const auto notes = midi_content(project).notes();
    const auto found = std::find_if(notes.begin(), notes.end(),
                                    [&](const auto& note) { return note.id == id; });
    return found == notes.end() ? nullptr : &*found;
}

std::vector<std::uint8_t> incompatible_state(bool wrong_content) {
    const examples::TimelinePluginProofIds ids;
    timeline::ClipContent content = timeline::EmptyContent{};
    if (!wrong_content) {
        auto midi = timeline::MidiContent::create({
            timeline::NoteEvent{ids.first_note, {0},
                                {examples::kTimelinePluginProofNoteTicks}, 1'000, 60, 0},
        });
        REQUIRE(midi);
        content = std::move(midi).value();
    }
    const auto duration = wrong_content ? examples::kTimelinePluginProofClipTicks
                                        : examples::kTimelinePluginProofClipTicks / 2;
    auto clip = timeline::Clip::create(ids.clip, {0}, {duration}, std::move(content));
    REQUIRE(clip);
    auto track = timeline::Track::create(ids.track, "Proof track", {std::move(clip).value()});
    REQUIRE(track);
    auto sequence = timeline::Sequence::create(
        ids.sequence, "Root",
        std::optional<timebase::TickDuration>{
            timebase::TickDuration{examples::kTimelinePluginProofClipTicks}},
        {std::move(track).value()});
    REQUIRE(sequence);
    timeline::ProjectInput input;
    input.id = ids.project;
    input.name = "incompatible";
    input.next_item_id = 7;
    input.root_sequence_id = ids.sequence;
    input.sequences.push_back(std::move(sequence).value());
    auto project = timeline::Project::create(std::move(input));
    REQUIRE(project);
    auto registry = timeline::make_builtin_timeline_registry();
    REQUIRE(registry);
    auto encoded = timeline::serialize_project(project.value(), registry.value());
    REQUIRE(encoded);
    return {encoded->json.begin(), encoded->json.end()};
}

std::vector<std::uint8_t> legacy_state(timebase::TickPosition clip_start,
                                       bool with_marker = false) {
    const examples::TimelinePluginProofIds ids;
    const timeline::ClipPlaybackProperties playback =
        with_marker
            ? timeline::ClipPlaybackProperties{0.5f, 10, 20,
                                               timeline::ClipFadeShape::EqualPower}
            : timeline::ClipPlaybackProperties{};
    auto clip = timeline::Clip::create(ids.clip, clip_start,
                                       {4 * timebase::kTicksPerQuarter},
                                       timeline::EmptyContent{}, playback);
    REQUIRE(clip);
    auto track = timeline::Track::create(ids.track, "Proof track", {std::move(clip).value()});
    REQUIRE(track);
    timeline::SequenceInput sequence_input;
    sequence_input.id = ids.sequence;
    sequence_input.name = "Root";
    sequence_input.musical_duration =
        timebase::TickDuration{8 * timebase::kTicksPerQuarter};
    sequence_input.tracks.push_back(std::move(track).value());
    if (with_marker)
        sequence_input.markers.push_back(
            timeline::SequenceMarker{{5}, "preserved", {timebase::kTicksPerQuarter}, {}});
    auto sequence = timeline::Sequence::create(std::move(sequence_input));
    REQUIRE(sequence);
    timeline::ProjectInput input;
    input.id = ids.project;
    input.name = "Timeline plugin proof";
    input.next_item_id = with_marker ? 6 : 5;
    input.root_sequence_id = ids.sequence;
    input.sequences.push_back(std::move(sequence).value());
    auto project = timeline::Project::create(std::move(input));
    REQUIRE(project);
    auto registry = timeline::make_builtin_timeline_registry();
    REQUIRE(registry);
    auto encoded = timeline::serialize_project(project.value(), registry.value());
    REQUIRE(encoded);
    return {encoded->json.begin(), encoded->json.end()};
}

} // namespace

TEST_CASE("timeline plugin proof owns a MIDI session and two piano-roll views") {
    examples::TimelinePluginProofProcessor processor;
    REQUIRE(processor.valid());
    REQUIRE(processor.document().revision == timeline::DocumentRevision{0});

    auto first_editor = processor.create_view();
    auto second_editor = processor.create_view();
    auto& first = proof_view(first_editor);
    auto& second = proof_view(second_editor);
    REQUIRE(first.piano_roll().notes().size() == 2);
    REQUIRE(second.piano_roll().notes().size() == 2);
    REQUIRE(first.bound_project() == processor.document().snapshot.get());
    REQUIRE(second.bound_project() == processor.document().snapshot.get());
    CHECK(first.bounds().width == examples::kTimelinePluginProofViewWidth);
    CHECK(first.bounds().height == examples::kTimelinePluginProofViewHeight);
    CHECK(first.bounds().encloses(first.piano_roll().bounds()));
    const auto first_note = first.piano_roll().note_rect(processor.ids().first_note);
    REQUIRE(first_note);
    CHECK(first_note->width == tick_x(examples::kTimelinePluginProofNoteTicks));
    CHECK(first.piano_roll().bounds().encloses(*first_note));
}

TEST_CASE("timeline plugin proof commits sequential note gestures and undo rebinds both views") {
    examples::TimelinePluginProofProcessor processor;
    const auto ids = processor.ids();
    auto first_editor = processor.create_view();
    auto second_editor = processor.create_view();
    auto& first = proof_view(first_editor);
    auto& second = proof_view(second_editor);
    RecordingPluginViewHost second_host;
    second.set_plugin_view_host(&second_host);
    second_host.repaint_count = 0;

    const auto note_ticks = examples::kTimelinePluginProofNoteTicks;
    const auto second_start = examples::kTimelinePluginProofSecondNoteStart;
    const auto inserted_start = 3 * timebase::kTicksPerQuarter / 2;
    first.simulate_drag({tick_x(second_start) + 5.0f, pitch_y(62)},
                        {tick_x(second_start + note_ticks) + 5.0f, pitch_y(64)}, 4);
    first.simulate_drag({tick_x(note_ticks), pitch_y(60)},
                        {tick_x(3 * note_ticks / 2), pitch_y(60)}, 4);
    first.simulate_click({tick_x(inserted_start), pitch_y(67)});

    REQUIRE(processor.document().revision != timeline::DocumentRevision{});
    const auto snapshot = processor.document().snapshot;
    REQUIRE(first.bound_project() == snapshot.get());
    REQUIRE(second.bound_project() == snapshot.get());
    const auto repaint_after_edits = second_host.repaint_count;
    REQUIRE(repaint_after_edits > 3);
    REQUIRE(first.piano_roll().notes().size() == 3);
    const auto* moved = find_note(*snapshot, ids.second_note);
    REQUIRE(moved != nullptr);
    CHECK(moved->start == timebase::TickPosition{second_start + note_ticks});
    CHECK(moved->pitch == 64);
    const auto* resized = find_note(*snapshot, ids.first_note);
    REQUIRE(resized != nullptr);
    CHECK(resized->duration == timebase::TickDuration{3 * note_ticks / 2});
    const auto inserted_id = timeline::ItemId{7};
    const auto* inserted = find_note(*snapshot, inserted_id);
    REQUIRE(inserted != nullptr);
    CHECK(inserted->start == timebase::TickPosition{inserted_start});
    CHECK(inserted->pitch == 67);

    REQUIRE(processor.undo());
    const auto undone = processor.document().snapshot;
    REQUIRE(first.bound_project() == undone.get());
    REQUIRE(second.bound_project() == undone.get());
    REQUIRE(second_host.repaint_count == repaint_after_edits + 1);
    REQUIRE(find_note(*undone, inserted_id) == nullptr);

    REQUIRE(processor.redo());
    const auto redone = processor.document().snapshot;
    REQUIRE(first.bound_project() == redone.get());
    REQUIRE(second.bound_project() == redone.get());
    REQUIRE(second_host.repaint_count == repaint_after_edits + 2);
    REQUIRE(first.piano_roll().notes().size() == 3);
    REQUIRE(second.piano_roll().notes().size() == 3);
    REQUIRE(find_note(*redone, inserted_id) != nullptr);
}

TEST_CASE("timeline plugin proof continuously moves and resizes as one undo group") {
    examples::TimelinePluginProofProcessor processor;
    const auto ids = processor.ids();
    auto first_editor = processor.create_view();
    auto second_editor = processor.create_view();
    auto& first = proof_view(first_editor);
    auto& second = proof_view(second_editor);

    const auto before_move = processor.serialize_plugin_state();
    const auto second_start = examples::kTimelinePluginProofSecondNoteStart;
    const auto note_ticks = examples::kTimelinePluginProofNoteTicks;
    first.simulate_drag({tick_x(second_start) + 5.0f, pitch_y(62)},
                        {tick_x(second_start + note_ticks) + 5.0f, pitch_y(64)}, 4);
    const auto after_move = processor.serialize_plugin_state();
    REQUIRE(after_move != before_move);
    REQUIRE(first.bound_project() == processor.document().snapshot.get());
    REQUIRE(second.bound_project() == processor.document().snapshot.get());
    CHECK(find_note(*processor.document().snapshot, ids.second_note)->start ==
          timebase::TickPosition{second_start + note_ticks});
    CHECK(find_note(*processor.document().snapshot, ids.second_note)->pitch == 64);
    const auto* moved_modifier =
        midi_content(*processor.document().snapshot).modifier_for(ids.second_note);
    REQUIRE(moved_modifier != nullptr);
    CHECK(*moved_modifier ==
          timeline::NoteModifier{.note_id = ids.second_note, .probability = 1'024});
    CHECK(midi_content(*processor.document().snapshot).modifier_seed() == 0xC0FFEE);

    REQUIRE(processor.undo());
    CHECK(processor.serialize_plugin_state() == before_move);
    REQUIRE(first.bound_project() == processor.document().snapshot.get());
    REQUIRE(second.bound_project() == processor.document().snapshot.get());
    REQUIRE(processor.redo());
    CHECK(processor.serialize_plugin_state() == after_move);
    REQUIRE(midi_content(*processor.document().snapshot).modifier_for(ids.second_note) !=
            nullptr);

    const auto before_resize = processor.serialize_plugin_state();
    first.simulate_drag({tick_x(note_ticks), pitch_y(60)},
                        {tick_x(3 * note_ticks / 2), pitch_y(60)}, 4);
    const auto after_resize = processor.serialize_plugin_state();
    REQUIRE(after_resize != before_resize);
    CHECK(find_note(*processor.document().snapshot, ids.first_note)->duration ==
          timebase::TickDuration{3 * note_ticks / 2});
    REQUIRE(processor.undo());
    CHECK(processor.serialize_plugin_state() == before_resize);
    REQUIRE(first.bound_project() == processor.document().snapshot.get());
    REQUIRE(second.bound_project() == processor.document().snapshot.get());
    REQUIRE(processor.redo());
    CHECK(processor.serialize_plugin_state() == after_resize);
}

TEST_CASE("timeline plugin proof cancel restores bytes and replacement resets provenance") {
    examples::TimelinePluginProofProcessor processor;
    auto first_editor = processor.create_view();
    auto second_editor = processor.create_view();
    auto& first = proof_view(first_editor);
    auto& second = proof_view(second_editor);
    const auto start = examples::kTimelinePluginProofSecondNoteStart;
    const auto note_ticks = examples::kTimelinePluginProofNoteTicks;

    const auto before_cancel = processor.serialize_plugin_state();
    first.piano_roll().on_mouse_down({tick_x(start) + 5.0f, pitch_y(62)});
    first.piano_roll().on_mouse_drag({tick_x(start + note_ticks / 2) + 5.0f,
                                     pitch_y(63)});
    first.piano_roll().on_mouse_drag({tick_x(start + note_ticks) + 5.0f,
                                     pitch_y(64)});
    REQUIRE(processor.serialize_plugin_state() != before_cancel);
    first.piano_roll().on_mouse_cancel({tick_x(start + note_ticks) + 5.0f,
                                       pitch_y(64)});
    CHECK(processor.serialize_plugin_state() == before_cancel);
    REQUIRE(first.bound_project() == processor.document().snapshot.get());
    REQUIRE(second.bound_project() == processor.document().snapshot.get());

    examples::TimelinePluginProofProcessor pristine;
    const auto replacement = pristine.serialize_plugin_state();
    first.piano_roll().on_mouse_down({tick_x(start) + 5.0f, pitch_y(62)});
    first.piano_roll().on_mouse_drag({tick_x(start + note_ticks / 2) + 5.0f,
                                     pitch_y(63)});
    first.piano_roll().on_mouse_drag({tick_x(start + note_ticks) + 5.0f,
                                     pitch_y(64)});
    REQUIRE(processor.serialize_plugin_state() != replacement);
    REQUIRE(processor.deserialize_plugin_state(replacement));
    CHECK(processor.serialize_plugin_state() == replacement);
    first.piano_roll().on_mouse_cancel({});
    CHECK(processor.serialize_plugin_state() == replacement);

    first.simulate_drag({tick_x(start) + 5.0f, pitch_y(62)},
                        {tick_x(start + note_ticks) + 5.0f, pitch_y(64)}, 4);
    REQUIRE(processor.serialize_plugin_state() != replacement);
    REQUIRE(first.bound_project() == processor.document().snapshot.get());
    REQUIRE(second.bound_project() == processor.document().snapshot.get());
}

TEST_CASE("timeline plugin proof rejects a stale note intent and keeps both views canonical") {
    examples::TimelinePluginProofProcessor processor;
    auto first_editor = processor.create_view();
    auto second_editor = processor.create_view();
    auto& first = proof_view(first_editor);
    auto& second = proof_view(second_editor);
    const auto ids = processor.ids();

    timeline_editor::NoteEditIntent stale;
    stale.kind = timeline_editor::NoteEditIntentKind::Move;
    stale.sequence_id = ids.sequence;
    stale.track_id = ids.track;
    stale.clip_id = ids.clip;
    stale.expected = timeline::NoteEvent{ids.first_note, {1}, {120}, 1'000, 60, 0};
    stale.replacement = timeline::NoteEvent{ids.first_note, {121}, {120}, 1'000, 60, 0};
    auto validated = timeline_editor::ValidatedNoteEditIntent::create(std::move(stale));
    REQUIRE(validated);
    REQUIRE(processor.submit_intent(validated.value()).status ==
            timeline_editor::IntentStatus::Rejected);
    REQUIRE(processor.document().revision == timeline::DocumentRevision{0});
    REQUIRE(first.bound_project() == processor.document().snapshot.get());
    REQUIRE(second.bound_project() == processor.document().snapshot.get());
    CHECK(find_note(*processor.document().snapshot, ids.first_note)->start ==
          timebase::TickPosition{0});
}

TEST_CASE("timeline plugin proof state load rebinds views and rejects incompatible projects") {
    examples::TimelinePluginProofProcessor source;
    auto source_editor = source.create_view();
    proof_view(source_editor).simulate_drag(
        {tick_x(examples::kTimelinePluginProofSecondNoteStart) + 5.0f, pitch_y(62)},
        {tick_x(examples::kTimelinePluginProofSecondNoteStart +
                examples::kTimelinePluginProofNoteTicks) + 5.0f,
         pitch_y(64)},
        4);
    const auto saved = source.serialize_plugin_state();
    REQUIRE_FALSE(saved.empty());

    examples::TimelinePluginProofProcessor restored;
    auto first_editor = restored.create_view();
    auto second_editor = restored.create_view();
    auto& first = proof_view(first_editor);
    auto& second = proof_view(second_editor);
    REQUIRE(restored.deserialize_plugin_state(saved));
    REQUIRE(restored.serialize_plugin_state() == saved);
    REQUIRE(first.bound_project() == restored.document().snapshot.get());
    REQUIRE(second.bound_project() == restored.document().snapshot.get());

    first.simulate_click({tick_x(3 * timebase::kTicksPerQuarter / 2), pitch_y(67)});
    REQUIRE(restored.document().revision == timeline::DocumentRevision{1});
    REQUIRE(first.piano_roll().notes().size() == 3);
    REQUIRE(second.piano_roll().notes().size() == 3);

    const auto before_bad_load = restored.serialize_plugin_state();
    REQUIRE_FALSE(restored.deserialize_plugin_state(incompatible_state(true)));
    REQUIRE(restored.serialize_plugin_state() == before_bad_load);
    REQUIRE_FALSE(restored.deserialize_plugin_state(incompatible_state(false)));
    REQUIRE(restored.serialize_plugin_state() == before_bad_load);
    const std::vector<std::uint8_t> malformed{'{', 'n', 'o'};
    REQUIRE_FALSE(restored.deserialize_plugin_state(malformed));
    REQUIRE(restored.serialize_plugin_state() == before_bad_load);

    REQUIRE(restored.undo());
    REQUIRE(first.bound_project() == restored.document().snapshot.get());
    REQUIRE(second.bound_project() == restored.document().snapshot.get());
    REQUIRE(first.piano_roll().notes().size() == 2);
    REQUIRE(second.piano_roll().notes().size() == 2);
    REQUIRE(restored.redo());
    REQUIRE(restored.serialize_plugin_state() == before_bad_load);
    REQUIRE(first.piano_roll().notes().size() == 3);
    REQUIRE(second.piano_roll().notes().size() == 3);
}

TEST_CASE("timeline plugin proof migrates legacy empty-clip state") {
    examples::TimelinePluginProofProcessor processor;
    auto editor = processor.create_view();
    auto& proof = proof_view(editor);
    const auto legacy_start = timebase::TickPosition{timebase::kTicksPerQuarter};

    REQUIRE(processor.deserialize_plugin_state(legacy_state(legacy_start)));
    REQUIRE(processor.document().revision == timeline::DocumentRevision{0});
    REQUIRE(proof.bound_project() == processor.document().snapshot.get());
    REQUIRE(proof.piano_roll().notes().size() == 2);
    const auto* sequence = processor.document().snapshot->find_sequence(processor.ids().sequence);
    const auto* track = sequence ? sequence->find_track(processor.ids().track) : nullptr;
    const auto* clip = track ? track->find_clip(processor.ids().clip) : nullptr;
    REQUIRE(clip != nullptr);
    const auto* range = std::get_if<timeline::MusicalTimeRange>(&clip->time_range());
    REQUIRE(range != nullptr);
    REQUIRE(range->start == legacy_start);
    REQUIRE(range->duration ==
            timebase::TickDuration{examples::kTimelinePluginProofClipTicks});

    proof.simulate_click({tick_x(3 * timebase::kTicksPerQuarter / 2), pitch_y(67)});
    REQUIRE(processor.document().revision == timeline::DocumentRevision{1});
    REQUIRE(proof.piano_roll().notes().size() == 3);

    examples::TimelinePluginProofProcessor enriched;
    REQUIRE(enriched.deserialize_plugin_state(legacy_state(legacy_start, true)));
    const auto* enriched_sequence =
        enriched.document().snapshot->find_sequence(enriched.ids().sequence);
    REQUIRE(enriched_sequence != nullptr);
    REQUIRE(enriched_sequence->markers().size() == 1);
    REQUIRE(enriched_sequence->markers().front().name == "preserved");
    const auto enriched_notes = midi_content(*enriched.document().snapshot).notes();
    REQUIRE(enriched_notes.size() == 2);
    REQUIRE(enriched_notes[0].id == timeline::ItemId{6});
    REQUIRE(enriched_notes[1].id == timeline::ItemId{7});
    REQUIRE(enriched.document().snapshot->next_item_id() == 8);
    const auto* enriched_track = enriched_sequence->find_track(enriched.ids().track);
    const auto* enriched_clip =
        enriched_track ? enriched_track->find_clip(enriched.ids().clip) : nullptr;
    REQUIRE(enriched_clip != nullptr);
    const timeline::ClipPlaybackProperties expected_playback{
        0.5f, 10, 20, timeline::ClipFadeShape::EqualPower};
    REQUIRE(enriched_clip->playback_properties() == expected_playback);
}

TEST_CASE("timeline plugin proof view detaches when its processor is destroyed first") {
    state::StateStore store;
    runtime::AliveToken owner_alive;
    auto processor = std::make_unique<examples::TimelinePluginProofProcessor>();
    processor->set_state_store(&store);
    processor->define_parameters(store);
    auto bridge = std::make_unique<format::ViewBridge>(
        *processor, store, owner_alive.capture());

    REQUIRE(bridge->open());
    bridge->notify_attached();
    auto released = bridge->release_view();
    REQUIRE(released != nullptr);
    auto& proof = proof_view(released);
    REQUIRE(proof.piano_roll().notes().size() == 2);

    owner_alive.retire();
    processor.reset();
    bridge->close();
    proof.simulate_click({tick_x(3 * timebase::kTicksPerQuarter / 2), pitch_y(67)});
    REQUIRE(proof.piano_roll().notes().size() == 2);
    released.reset();
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
