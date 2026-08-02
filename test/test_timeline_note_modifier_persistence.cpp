#include "support/timeline_persistence_test_support.hpp"

namespace {

NoteModifier ratcheted_chance(std::uint64_t note, std::uint16_t probability,
                              std::uint16_t ratchet) {
    NoteModifier modifier;
    modifier.note_id = {note};
    modifier.probability = probability;
    modifier.ratchet_count = ratchet;
    return modifier;
}

constexpr std::uint64_t kModifierSeed = 1'234'567'890'123'456'789ull;

Project project_with_modifiers() {
    NoteModifier every_fourth;
    every_fourth.note_id = {7};
    every_fourth.condition = NoteConditionKind::EveryNth;
    every_fourth.condition_period = 4;
    every_fourth.condition_offset = 1;

    auto content =
        take(MidiContent::create({{{7}, {0}, {2}, 0xffff, 60, 0}, {{6}, {4}, {2}, 0x8000, 64, 1}},
                                 {every_fourth, ratcheted_chance(6, 0x4000, 3)}, kModifierSeed));
    auto clip = take(Clip::create({4}, {0}, {100}, std::move(content)));
    auto track = take(Track::create({3}, "track", {clip}));
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, {track}));
    return take(Project::create(ProjectInput{{1}, "project", 12, {2}, {}, {sequence}}));
}

const MidiContent& only_note_content(const Project& project) {
    const auto* sequence = project.find_sequence({2});
    REQUIRE(sequence != nullptr);
    return std::get<MidiContent>(sequence->tracks()[0].clips()[0].content());
}

} // namespace

TEST_CASE("Note modifiers survive a serialize and deserialize round trip",
          "[timeline][note-modifier][persistence]") {
    const auto registry = builtins();
    const auto encoded = take(serialize_project(project_with_modifiers(), registry));
    const auto decoded = take(deserialize_project(encoded.json, registry));

    const auto& content = only_note_content(decoded);
    REQUIRE(content.modifier_seed() == kModifierSeed);
    REQUIRE(content.modifiers().size() == 2);

    const auto* six = content.modifier_for({6});
    REQUIRE(six != nullptr);
    REQUIRE(six->probability == 0x4000);
    REQUIRE(six->ratchet_count == 3);
    REQUIRE(six->condition == NoteConditionKind::Always);

    const auto* seven = content.modifier_for({7});
    REQUIRE(seven != nullptr);
    REQUIRE(seven->condition == NoteConditionKind::EveryNth);
    REQUIRE(seven->condition_period == 4);
    REQUIRE(seven->condition_offset == 1);
    REQUIRE(seven->ratchet_count == 1);

    // Re-encoding the decoded project reproduces the exact canonical bytes.
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded.json);
}

TEST_CASE("Note content v1 upgrades to an empty modifier set and downgrades only when unused",
          "[timeline][note-modifier][persistence]") {
    const auto registry = builtins();
    const std::string v1 =
        R"({"data":{"notes":[]},"type_name":"pulp.timeline.content.notes","version":1})";
    const auto v2 =
        take(registry.migrate(SchemaDomain::Content, "pulp.timeline.content.notes", 1, 2, v1));
    REQUIRE(
        v2 ==
        R"({"data":{"modifier_seed":"0","modifiers":[],"notes":[]},"type_name":"pulp.timeline.content.notes","version":2})");
    REQUIRE(take(registry.migrate(SchemaDomain::Content, "pulp.timeline.content.notes", 2, 1,
                                 v2)) == v1);

    // A document that authors a modifier has no v1 spelling, so the downgrade
    // refuses rather than dropping the annotation and changing how it plays.
    const std::string populated =
        R"({"data":{"modifier_seed":"0","modifiers":[{"condition":"always","condition_offset":0,"condition_period":1,"note_id":"7","probability":16384,"ratchet_count":1}],"notes":[]},"type_name":"pulp.timeline.content.notes","version":2})";
    REQUIRE_FALSE(
        registry.migrate(SchemaDomain::Content, "pulp.timeline.content.notes", 2, 1, populated));

    // A non-zero seed is equally unrepresentable: it changes every draw.
    const std::string seeded =
        R"({"data":{"modifier_seed":"5","modifiers":[],"notes":[]},"type_name":"pulp.timeline.content.notes","version":2})";
    REQUIRE_FALSE(
        registry.migrate(SchemaDomain::Content, "pulp.timeline.content.notes", 2, 1, seeded));
}

TEST_CASE("A version-one note fixture upgrades and then re-saves canonically",
          "[timeline][note-modifier][persistence]") {
    std::ifstream stream(std::string(PULP_TIMELINE_FIXTURE_DIR) + "/v1/note-content.json",
                         std::ios::binary);
    REQUIRE(stream.good());
    const std::string fixture((std::istreambuf_iterator<char>(stream)),
                              std::istreambuf_iterator<char>());
    const auto registry = builtins();
    const auto decoded = take(deserialize_project(fixture, registry));

    const auto& content = only_note_content(decoded);
    REQUIRE(content.notes().size() == 2);
    // The upgrade is the identity on how the clip plays.
    REQUIRE(content.modifiers().empty());
    REQUIRE(content.modifier_seed() == 0);

    const auto encoded = take(serialize_project(decoded, registry));
    REQUIRE(encoded.json.find(R"("modifier_seed":"0","modifiers":[])") != std::string::npos);
    // Qualified by type name on purpose. This was a bare `"version":2`, which
    // named no type and so matched whichever envelope happened to sit at that
    // version -- the project's, not the note content this case is about. It
    // would have passed with the note content at any version at all, and it
    // broke when an unrelated project bump moved the envelope it was really
    // watching.
    REQUIRE(encoded.json.find(R"("type_name":"pulp.timeline.content.notes","version":3)") !=
            std::string::npos);
    const auto reloaded = take(deserialize_project(encoded.json, registry));
    REQUIRE(take(serialize_project(reloaded, registry)).json == encoded.json);
}

TEST_CASE("Malformed persisted note modifiers are refused rather than defaulted",
          "[timeline][note-modifier][persistence]") {
    const auto registry = builtins();
    const auto encoded = take(serialize_project(project_with_modifiers(), registry));

    auto corrupt = [&](std::string_view from, std::string_view to) {
        std::string mutated = encoded.json;
        const auto at = mutated.find(from);
        REQUIRE(at != std::string::npos);
        mutated.replace(at, from.size(), to);
        return mutated;
    };

    // A condition spelling the reader does not know must not silently become
    // "always" — that would change how the document plays without saying so.
    REQUIRE_FALSE(deserialize_project(corrupt(R"("condition":"every_nth")",
                                              R"("condition":"every_other")"),
                                      registry));
    // An offset at or past its period selects no pass at all.
    REQUIRE_FALSE(deserialize_project(
        corrupt(R"("condition_offset":1,"condition_period":4)",
                R"("condition_offset":9,"condition_period":4)"),
        registry));
    // A ratchet of zero would emit a note-on with no matching off.
    REQUIRE_FALSE(
        deserialize_project(corrupt(R"("ratchet_count":3)", R"("ratchet_count":0)"), registry));
    // A modifier naming a note that is not in the clip has nothing to modify.
    REQUIRE_FALSE(
        deserialize_project(corrupt(R"("note_id":"6")", R"("note_id":"4242")"), registry));
    // Probability is a 16-bit fraction; a wider value is not a stronger yes.
    REQUIRE_FALSE(
        deserialize_project(corrupt(R"("probability":16384)", R"("probability":70000)"), registry));
    // The seed is load-bearing for replay, so a missing one is not a zero.
    REQUIRE_FALSE(deserialize_project(
        corrupt(R"("modifier_seed":"1234567890123456789",)", ""), registry));
}
