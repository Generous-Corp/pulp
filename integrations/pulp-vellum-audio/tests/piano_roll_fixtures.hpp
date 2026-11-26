#pragma once

// Fixtures shared by the piano roll tests: projects with known notes, a session
// that lowers emitted intents onto the document, and canonical-JSON helpers.

#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline_editor/scripted_ui_host.hpp>
#include <pulp/vellum_audio/piano_roll_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace piano_roll_test {

using namespace pulp::timeline;
using namespace pulp::timeline_editor;
using namespace pulp::vellum_audio;
using namespace timeline_test;
using ::vellum::view::HitMetrics;
using ::vellum::view::MouseButton;
using ::vellum::view::PointerType;

// One pixel per tick horizontally and twenty pixels per semitone vertically, so
// every expected value below is readable without arithmetic. The clip runs
// [0, 960) and the roll shows pitches 48..71, highest at the top.
constexpr std::int64_t kClipTicks = 960;
constexpr std::uint8_t kLowPitch = 48;
constexpr std::uint8_t kHighPitch = 71;
constexpr float kRollHeight = 480.0f; // 24 rows of 20px
constexpr ItemId kSequence{3};
constexpr ItemId kTrack{4};
constexpr ItemId kClip{5};
constexpr ItemId kNoteA{10};
constexpr ItemId kNoteB{11};
constexpr ItemId kNoteC{12};
constexpr std::size_t kDenseNoteCount = 10'000;

inline std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline PianoRollLayout layout_over(std::int64_t visible_start, std::int64_t visible_ticks,
                                   float pixel_extent) {
    auto time =
        TickProjection::create({visible_start}, {visible_ticks}, PixelSpan{0.0f, pixel_extent});
    REQUIRE(time);
    auto pitch = PitchProjection::create(kLowPitch, kHighPitch, PixelSpan{0.0f, kRollHeight});
    REQUIRE(pitch);
    return PianoRollLayout{std::move(time).value(), std::move(pitch).value(), {}};
}

/// The whole clip, one pixel per tick.
inline PianoRollLayout unit_layout() {
    return layout_over(0, kClipTicks, static_cast<float>(kClipTicks));
}

/// Three notes, canonically ordered by start: A at 0 (pitch 60), B at 240
/// (pitch 62), C at 480 (pitch 64). Each is 120 ticks long.
inline Project make_note_project() {
    auto content = MidiContent::create({
        NoteEvent{kNoteA, {0}, {120}, 1000, 60, 0},
        NoteEvent{kNoteB, {240}, {120}, 1000, 62, 0},
        NoteEvent{kNoteC, {480}, {120}, 1000, 64, 0},
    });
    REQUIRE(content);
    auto authored = Clip::create(kClip, {0}, {kClipTicks}, std::move(content).value());
    REQUIRE(authored);
    auto track = Track::create(kTrack, "track", {std::move(authored).value()});
    REQUIRE(track);
    auto sequence = Sequence::create(kSequence, "sequence", TickDuration{kClipTicks},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project =
        Project::create({{1}, "project", 100, kSequence, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

inline Project make_modified_note_project() {
    std::vector<NoteEvent> notes{
        NoteEvent{kNoteA, {0}, {120}, 1000, 60, 0},
        NoteEvent{kNoteB, {240}, {120}, 1000, 62, 0},
        NoteEvent{kNoteC, {480}, {120}, 1000, 64, 0},
    };
    std::vector<NoteModifier> modifiers{
        NoteModifier{kNoteB, 4096, 2, 1, 2, NoteConditionKind::EveryNth},
    };
    auto content = MidiContent::create(std::move(notes), std::move(modifiers), 0xBEEF);
    REQUIRE(content);
    auto authored = Clip::create(kClip, {0}, {kClipTicks}, std::move(content).value());
    REQUIRE(authored);
    auto track = Track::create(kTrack, "track", {std::move(authored).value()});
    REQUIRE(track);
    auto sequence = Sequence::create(kSequence, "sequence", TickDuration{kClipTicks},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project =
        Project::create({{1}, "project", 100, kSequence, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

inline Project make_dense_note_project(bool include_long_note = false) {
    constexpr std::int64_t stride = 10;
    constexpr std::uint64_t first_note_id = 1'000;
    std::vector<NoteEvent> notes;
    notes.reserve(kDenseNoteCount + (include_long_note ? 1 : 0));
    constexpr auto clip_ticks = static_cast<std::int64_t>(kDenseNoteCount) * stride;
    if (include_long_note)
        notes.push_back({{first_note_id - 1}, {0}, {clip_ticks}, 1'000, 60, 0});
    for (std::size_t index = 0; index < kDenseNoteCount; ++index) {
        notes.push_back({{first_note_id + index},
                         {static_cast<std::int64_t>(index) * stride},
                         {4},
                         1'000,
                         60,
                         0});
    }

    auto content = MidiContent::create(std::move(notes));
    REQUIRE(content);
    auto authored = Clip::create(kClip, {0}, {clip_ticks}, std::move(content).value());
    REQUIRE(authored);
    auto track = Track::create(kTrack, "track", {std::move(authored).value()});
    REQUIRE(track);
    auto sequence = Sequence::create(kSequence, "sequence", TickDuration{clip_ticks},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1},
                                    "project",
                                    first_note_id + kDenseNoteCount,
                                    kSequence,
                                    {},
                                    {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

inline Project make_negative_start_note_project() {
    auto content = MidiContent::create({
        NoteEvent{{20}, {-20}, {30}, 1'000, 60, 0},
    });
    REQUIRE(content);
    auto authored = Clip::create(kClip, {0}, {kClipTicks}, std::move(content).value());
    REQUIRE(authored);
    auto track = Track::create(kTrack, "track", {std::move(authored).value()});
    REQUIRE(track);
    auto sequence = Sequence::create(kSequence, "sequence", TickDuration{kClipTicks},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project =
        Project::create({{1}, "project", 21, kSequence, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

inline PianoRollView& configure(PianoRollView& view, const Project& project,
                                NoteEditIntentHost& host) {
    view.set_bounds({0, 0, static_cast<float>(kClipTicks), kRollHeight});
    view.set_clip(&project, kSequence, kTrack, kClip);
    view.set_layout(unit_layout());
    view.set_host(&host);
    view.set_hit_metrics(HitMetrics::for_pointer(PointerType::mouse));
    view.set_continuous_gestures(true);
    return view;
}

/// Lattice y of a pitch's row centre, which is where a gesture aims.
inline float y_of(std::uint8_t pitch) {
    auto projection = PitchProjection::create(kLowPitch, kHighPitch, PixelSpan{0.0f, kRollHeight});
    REQUIRE(projection);
    return projection->y_at(pitch);
}

/// Plays the role a product shell plays: owns the session, lowers whatever the
/// view emitted against the CURRENT note array, and commits it.
struct Session {
    std::unique_ptr<DocumentSession> session;
    WriterToken writer;

    static Session create(Project project) {
        auto owned = DocumentSession::create(std::move(project));
        REQUIRE(owned);
        auto session = std::move(owned).value();
        auto writer = session->register_writer();
        REQUIRE(writer);
        return Session{std::move(session), std::move(writer).value()};
    }

    const Project& project() const {
        return *session->snapshot();
    }

    std::span<const NoteEvent> notes() const {
        const auto& content =
            project().find_sequence(kSequence)->find_track(kTrack)->find_clip(kClip)->content();
        return std::get<MidiContent>(content).notes();
    }

    /// Lowers and commits one intent. Returns whether the document took it.
    bool apply(const ValidatedNoteEditIntent& intent) {
        EditIntentIdentity identity;
        identity.transaction_id = writer.allocate_transaction_id();
        identity.command_id = writer.allocate_command_id();
        identity.expected_revision = session->revision();

        const auto current = notes();
        auto lowered = lower_note_edit_intent(intent, current, identity);
        if (!lowered)
            return false;
        return static_cast<bool>(session->submit(writer, std::move(lowered).value()));
    }

    /// Lowers one continuous stream against each newly committed note array,
    /// keeping every phase in one document undo group.
    bool apply_continuous(std::span<const ValidatedNoteEditIntent> intents) {
        const auto group = writer.allocate_undo_group_id();
        for (const auto& intent : intents) {
            EditIntentIdentity identity;
            identity.transaction_id = writer.allocate_transaction_id();
            identity.command_id = writer.allocate_command_id();
            identity.expected_revision = session->revision();
            identity.undo_group = group;

            auto lowered = lower_note_edit_intent(intent, notes(), identity);
            if (!lowered || !session->submit(writer, std::move(lowered).value()))
                return false;
        }
        return true;
    }
};

class CallbackNoteHost final : public NoteEditIntentHost {
  public:
    using Callback = std::function<IntentResult(const ValidatedNoteEditIntent&)>;

    UiPlayhead playhead() const noexcept override {
        return {};
    }
    AuditionResult begin_audition(const AuditionRequest&) noexcept override {
        return {};
    }
    void end_audition(AuditionHandle) noexcept override {}

    IntentResult submit_intent(const ValidatedNoteEditIntent& intent) noexcept override {
        intents.push_back(intent);
        return callback ? callback(intent) : IntentResult{};
    }

    Callback callback;
    std::vector<ValidatedNoteEditIntent> intents;
};

inline SchemaRegistry builtin_registry() {
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    return std::move(registry).value();
}

/// The clip's notes after a round trip through canonical JSON. This is the
/// acceptance vehicle: an edit that only exists in memory is not an edit the
/// document kept.
inline std::vector<NoteEvent> notes_after_round_trip(const Project& project) {
    const auto registry = builtin_registry();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    auto decoded = deserialize_project(encoded.value().json, registry);
    REQUIRE(decoded);
    const auto& content =
        decoded.value().find_sequence(kSequence)->find_track(kTrack)->find_clip(kClip)->content();
    const auto restored = std::get<MidiContent>(content).notes();
    return std::vector<NoteEvent>(restored.begin(), restored.end());
}

inline std::string canonical_project(const Project& project) {
    const auto registry = builtin_registry();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    return std::move(encoded).value().json;
}

struct CanonicalHistory {
    std::string baseline;
    std::string edited;
    std::string undone;
    std::string redone;
    std::vector<ValidatedNoteEditIntent> intents;
};

inline CanonicalHistory exercise_device_drag(PointerType pointer_type) {
    auto session = Session::create(make_note_project());
    const auto baseline = canonical_project(session.project());
    std::vector<ValidatedNoteEditIntent> intents;
    {
        ScriptedUiHost<ValidatedNoteEditIntent> host;
        PianoRollView view;
        configure(view, session.project(), host);
        view.set_hit_metrics(HitMetrics::for_pointer(pointer_type));

        ::vellum::view::View::SimulatedPointer pointer;
        pointer.type = pointer_type;
        view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4, pointer);

        REQUIRE(host.intents().size() == 4);
        intents.assign(host.intents().begin(), host.intents().end());
    }

    REQUIRE(intents.front().value().kind == NoteEditIntentKind::Move);
    REQUIRE(session.apply_continuous(intents));
    const auto edited = canonical_project(session.project());

    REQUIRE(session.session->undo(session.writer));
    const auto undone = canonical_project(session.project());
    REQUIRE(session.session->redo(session.writer));
    const auto redone = canonical_project(session.project());
    return {baseline, edited, undone, redone, std::move(intents)};
}

inline const NoteEvent* find_note(std::span<const NoteEvent> notes, ItemId id) {
    const auto found =
        std::find_if(notes.begin(), notes.end(), [&](const NoteEvent& n) { return n.id == id; });
    return found == notes.end() ? nullptr : &*found;
}

/// A factory that mints one fixed identity, which is what a document's id
/// allocator does for a real shell. The id must be at or above the project's
/// next_item_id, `plan_identity_insert` rejects anything below it as
/// `IdentityNotAvailable`, which is precisely why a view cannot invent one.
inline PianoRollView::NoteFactory factory_for(ItemId id, std::int64_t duration) {
    return [id, duration](pulp::timebase::TickPosition start,
                          std::uint8_t pitch) -> std::optional<NoteEvent> {
        return NoteEvent{id, start, {duration}, 900, pitch, 0};
    };
}

} // namespace piano_roll_test
