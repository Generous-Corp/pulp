#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/note_transform.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;
namespace runtime = pulp::runtime;

namespace {

runtime::Result<std::vector<NoteEvent>, NoteTransformCallbackFailure>
octave_echo(std::span<const NoteEvent> notes, std::string_view params, std::uint64_t seed,
            const void*) noexcept {
    if (params != R"({"interval":12})")
        return runtime::Err(
            NoteTransformCallbackFailure{NoteTransformCallbackError::InvalidParameters});
    std::vector<NoteEvent> result;
    for (const auto& note : notes) {
        auto original = note;
        original.velocity = static_cast<std::uint16_t>(seed);
        result.push_back(original);
        auto echo = note;
        echo.id = {};
        echo.start.value += kTicksPerQuarter / 8;
        echo.pitch = static_cast<std::uint8_t>(echo.pitch + 12);
        result.push_back(echo);
    }
    return runtime::Ok(std::move(result));
}

runtime::Result<std::vector<NoteEvent>, NoteTransformCallbackFailure>
foreign_identity(std::span<const NoteEvent> notes, std::string_view, std::uint64_t,
                 const void*) noexcept {
    auto result = std::vector<NoteEvent>(notes.begin(), notes.end());
    result[0].id = {999};
    return runtime::Ok(std::move(result));
}

runtime::Result<std::vector<NoteEvent>, NoteTransformCallbackFailure>
duplicate_identity(std::span<const NoteEvent> notes, std::string_view, std::uint64_t,
                   const void*) noexcept {
    return runtime::Ok(std::vector<NoteEvent>{notes[0], notes[0]});
}

runtime::Result<std::vector<NoteEvent>, NoteTransformCallbackFailure>
remove_all(std::span<const NoteEvent>, std::string_view, std::uint64_t, const void*) noexcept {
    return runtime::Ok(std::vector<NoteEvent>{});
}

runtime::Result<std::vector<NoteEvent>, NoteTransformCallbackFailure>
fill_standalone_output_limit(std::span<const NoteEvent>, std::string_view, std::uint64_t,
                             const void*) noexcept {
    return runtime::Ok(std::vector<NoteEvent>(NoteTransformRegistry::kMaxDurableCommandNotes));
}

runtime::Result<std::vector<NoteEvent>, NoteTransformCallbackFailure>
accept_fractional_parameters(std::span<const NoteEvent> notes, std::string_view params,
                             std::uint64_t, const void*) noexcept {
    if (params != R"({"amount":0.5,"ratio":0.001})")
        return runtime::Err(
            NoteTransformCallbackFailure{NoteTransformCallbackError::InvalidParameters});
    return runtime::Ok(std::vector<NoteEvent>(notes.begin(), notes.end()));
}

runtime::Result<std::vector<NoteEvent>, NoteTransformCallbackFailure>
accept_equivalent_numbers(std::span<const NoteEvent> notes, std::string_view params, std::uint64_t,
                          const void*) noexcept {
    if (params != R"({"amount":100})")
        return runtime::Err(
            NoteTransformCallbackFailure{NoteTransformCallbackError::InvalidParameters});
    return runtime::Ok(std::vector<NoteEvent>(notes.begin(), notes.end()));
}

const NoteContent& notes(const Project& project) {
    return std::get<NoteContent>(clip(project).content());
}

} // namespace

TEST_CASE("Note transform preview lowers once to an ordinary undoable note edit") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    NoteTransformRegistry registry;
    REQUIRE_FALSE(registry.register_transform({{"vendor.note.octave_echo", 1}, octave_echo, {}}));

    const ApplyNoteTransform request{
        {3}, {4}, {5}, {"vendor.note.octave_echo", 1}, R"({ "interval" : 12 })", 2222};
    auto preview = registry.preview(session->current(), writer, request);
    REQUIRE(preview);

    // Preview is speculative: only the returned snapshot changed.
    REQUIRE(notes(*session->snapshot()).notes().size() == 1);
    REQUIRE(notes(preview->snapshot).notes().size() == 2);
    REQUIRE(notes(preview->snapshot).notes()[0].id == ItemId{6});
    REQUIRE(notes(preview->snapshot).notes()[0].velocity == 2222);
    REQUIRE(notes(preview->snapshot).notes()[1].id == ItemId{7});
    REQUIRE(notes(preview->snapshot).notes()[1].pitch == 72);
    REQUIRE(preview->snapshot.next_item_id() == 8);
    REQUIRE(preview->transaction.commands.size() == 1);
    REQUIRE(std::holds_alternative<ReplaceNoteContent>(preview->transaction.commands[0].command));
    REQUIRE(preview->dirty.items().size() == 1);
    REQUIRE(preview->dirty.items()[0].item == ItemId{5});

    // Commit submits the exact previewed transaction; undo/redo use ordinary
    // inverse commands and preserve the identity directory.
    auto committed = session->submit(writer, std::move(preview->transaction));
    REQUIRE(committed);
    REQUIRE(same_project(preview->snapshot, *committed->snapshot));
    REQUIRE(notes(*session->snapshot()).notes().size() == 2);
    REQUIRE(session->snapshot()->locate({7})->active);
    REQUIRE(session->undo(writer));
    REQUIRE(notes(*session->snapshot()).notes().size() == 1);
    REQUIRE_FALSE(session->snapshot()->locate({7})->active);
    REQUIRE(session->redo(writer));
    REQUIRE(notes(*session->snapshot()).notes().size() == 2);
    REQUIRE(session->snapshot()->locate({7})->active);
}

TEST_CASE("Note transform registry and output identities fail closed") {
    NoteTransformRegistry registry;
    REQUIRE(registry.register_transform({{}, octave_echo, {}})->code ==
            NoteTransformErrorCode::InvalidRegistration);
    REQUIRE_FALSE(registry.register_transform({{"vendor.note.foreign", 1}, foreign_identity, {}}));
    REQUIRE_FALSE(registry.register_transform({{"vendor.note.octave_echo", 1}, octave_echo, {}}));
    REQUIRE_FALSE(
        registry.register_transform({{"vendor.note.duplicate", 1}, duplicate_identity, {}}));
    REQUIRE(registry.register_transform({{"vendor.note.foreign", 1}, foreign_identity, {}})->code ==
            NoteTransformErrorCode::DuplicateRegistration);

    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto rejected = registry.preview(session->current(), writer,
                                     {{3}, {4}, {5}, {"vendor.note.foreign", 1}, "{}", 0});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == NoteTransformErrorCode::ForeignOutputIdentity);
    auto bad_params = registry.preview(session->current(), writer,
                                       {{3}, {4}, {5}, {"vendor.note.octave_echo", 1}, "{}", 0});
    REQUIRE_FALSE(bad_params);
    REQUIRE(bad_params.error().code == NoteTransformErrorCode::CallbackFailed);
    REQUIRE(bad_params.error().callback_error->code ==
            NoteTransformCallbackError::InvalidParameters);
    auto malformed = registry.preview(session->current(), writer,
                                      {{3}, {4}, {5}, {"vendor.note.octave_echo", 1}, "{", 0});
    REQUIRE_FALSE(malformed);
    REQUIRE(malformed.error().code == NoteTransformErrorCode::InvalidParameters);
    REQUIRE(malformed.error().persistence_error->code == PersistenceErrorCode::InvalidJson);
    auto duplicate = registry.preview(session->current(), writer,
                                      {{3}, {4}, {5}, {"vendor.note.duplicate", 1}, "{}", 0});
    REQUIRE_FALSE(duplicate);
    REQUIRE(duplicate.error().code == NoteTransformErrorCode::DuplicateOutputIdentity);
    REQUIRE(session->revision().value == 0);
}

TEST_CASE("Note transform parameters accept and canonicalize fractional JSON numbers") {
    NoteTransformRegistry registry;
    REQUIRE_FALSE(registry.register_transform(
        {{"vendor.note.fractional", 1}, accept_fractional_parameters, {}}));
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto preview = registry.preview(session->current(), writer,
                                    {{3},
                                     {4},
                                     {5},
                                     {"vendor.note.fractional", 1},
                                     R"({ "ratio": 1E-003, "amount": 0.5000 })",
                                     0});
    REQUIRE(preview);
    REQUIRE(same_project(preview->snapshot, *session->snapshot()));
}

TEST_CASE("Equivalent JSON numbers reach note transforms as identical canonical bytes") {
    NoteTransformRegistry registry;
    REQUIRE_FALSE(
        registry.register_transform({{"vendor.note.numeric", 1}, accept_equivalent_numbers, {}}));
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    for (const auto parameters : {R"({"amount":100})", R"({"amount":1e2})", R"({"amount":10e1})",
                                  R"({"amount":1000e-1})"}) {
        auto preview = registry.preview(session->current(), writer,
                                        {{3}, {4}, {5}, {"vendor.note.numeric", 1}, parameters, 0});
        REQUIRE(preview);
        REQUIRE(same_project(preview->snapshot, *session->snapshot()));
    }

    auto huge = parse_json("1e999999999999999999999999");
    auto equivalent_huge = parse_json("10e999999999999999999999998");
    auto negative_zero = parse_json("-0.000e999999999999999999999999");
    REQUIRE(huge);
    REQUIRE(equivalent_huge);
    REQUIRE(negative_zero);
    auto canonical_huge = canonicalize_json(huge.value()->root());
    auto canonical_equivalent_huge =
        canonicalize_json(equivalent_huge.value()->root());
    auto canonical_negative_zero = canonicalize_json(negative_zero.value()->root());
    REQUIRE(canonical_huge);
    REQUIRE(canonical_equivalent_huge);
    REQUIRE(canonical_negative_zero);
    REQUIRE(canonical_huge.value() == canonical_equivalent_huge.value());
    REQUIRE(canonical_negative_zero.value() == "0");
}

TEST_CASE("A zero-output note transform deactivates and undo restores note identities") {
    NoteTransformRegistry registry;
    REQUIRE_FALSE(registry.register_transform({{"vendor.note.remove_all", 1}, remove_all, {}}));
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto preview = registry.preview(session->current(), writer,
                                    {{3}, {4}, {5}, {"vendor.note.remove_all", 1}, "{}", 0});
    REQUIRE(preview);
    REQUIRE(notes(preview->snapshot).notes().empty());
    REQUIRE_FALSE(preview->snapshot.locate({6})->active);
    REQUIRE(session->submit(writer, std::move(preview->transaction)));
    REQUIRE(notes(*session->snapshot()).notes().empty());
    REQUIRE_FALSE(session->snapshot()->locate({6})->active);
    REQUIRE(session->undo(writer));
    REQUIRE(notes(*session->snapshot()).notes().size() == 1);
    REQUIRE(session->snapshot()->locate({6})->active);
}

TEST_CASE("Note transforms reserve persistence quota for the expected note content") {
    NoteTransformRegistry registry;
    REQUIRE_FALSE(registry.register_transform(
        {{"vendor.note.output_limit", 1}, fill_standalone_output_limit, {}}));
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();

    // The callback returns the standalone output ceiling, but the durable
    // ReplaceNoteContent payload must also carry the clip's existing note.
    auto preview = registry.preview(
        session->current(), writer,
        {{3}, {4}, {5}, {"vendor.note.output_limit", 1}, "{}", 0});
    REQUIRE_FALSE(preview);
    REQUIRE(preview.error().code == NoteTransformErrorCode::OutputLimitExceeded);
}

TEST_CASE("ReplaceNoteContent canonicalizes programmatic input before building its inverse") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto original = notes(*session->snapshot()).notes()[0];
    auto later = original;
    later.start.value = kTicksPerQuarter / 8;
    NoteEvent earlier{{7}, {0}, {kTicksPerQuarter / 8}, 2000, 72, 0};
    auto edit = session_transaction(
        writer, {}, {ReplaceNoteContent{{3}, {4}, {5}, {original}, {later, earlier}}});
    REQUIRE(session->submit(writer, std::move(edit)));
    REQUIRE(notes(*session->snapshot()).notes()[0].id == ItemId{7});
    REQUIRE(notes(*session->snapshot()).notes()[1].id == ItemId{6});
    REQUIRE(session->undo(writer));
    REQUIRE(notes(*session->snapshot()).notes().size() == 1);
    REQUIRE(notes(*session->snapshot()).notes()[0].id == ItemId{6});
    REQUIRE_FALSE(session->snapshot()->locate({7})->active);
}

TEST_CASE("A note transform preview becomes stale instead of changing its meaning") {
    NoteTransformRegistry registry;
    REQUIRE_FALSE(registry.register_transform({{"vendor.note.octave_echo", 1}, octave_echo, {}}));
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto intervening_writer = std::move(session->register_writer()).value();
    auto preview = registry.preview(
        session->current(), writer,
        {{3}, {4}, {5}, {"vendor.note.octave_echo", 1}, R"({"interval":12})", 2222});
    REQUIRE(preview);
    auto intervening = session_transaction(intervening_writer, {},
                                           {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 3333}});
    REQUIRE(session->submit(intervening_writer, std::move(intervening)));
    auto stale = session->submit(writer, std::move(preview->transaction));
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error().code == ConflictCode::StaleRevision);
    REQUIRE(notes(*session->snapshot()).notes().size() == 1);
    REQUIRE(notes(*session->snapshot()).notes()[0].velocity == 3333);
}
