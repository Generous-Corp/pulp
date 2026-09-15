#include "support/timeline_persistence_test_support.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/transaction.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

TuningReference equal_temperament(std::uint32_t pitch_millihertz) {
    return TuningReference{TuningSystem::EqualTemperament, pitch_millihertz, {}, {}};
}

TuningReference scala_tuning() {
    return TuningReference{TuningSystem::Scala, kDefaultReferencePitchMillihertz, hash('c'), {}};
}

SequenceRegion region(SectionRole role, std::string name = "section") {
    return SequenceRegion{{15}, std::move(name), TickPosition{0}, TickDuration{48}, {}, role};
}

// Two tracks, so an assertion that a track override reached one track cannot
// pass by there being only one to reach.
Project project_with(std::optional<TuningReference> project_tuning,
                     std::optional<TuningReference> track_tuning,
                     std::vector<SequenceRegion> regions = {}) {
    auto clip = take(Clip::create({4}, {0}, {48}, EmptyContent{}));
    TrackInput first;
    first.id = {3};
    first.name = "tuned";
    first.clips = {clip};
    first.tuning = std::move(track_tuning);
    TrackInput second;
    second.id = {6};
    second.name = "untuned";
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{48}, {},
                                          {take(Track::create(std::move(first))),
                                           take(Track::create(std::move(second)))},
                                          {}, std::move(regions)));
    ProjectInput input{{1}, "project", 40, {2}, {}, {sequence}};
    input.tuning = std::move(project_tuning);
    return take(Project::create(std::move(input)));
}

Transaction one(Command command) {
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1}, std::move(command)});
    return transaction;
}

const SequenceRegion& only_region(const Project& project) {
    const auto* sequence = project.find_sequence({2});
    REQUIRE(sequence != nullptr);
    REQUIRE(sequence->regions().size() == 1);
    return sequence->regions()[0];
}

const std::optional<TuningReference>& track_tuning_of(const Project& project, ItemId track) {
    const auto* sequence = project.find_sequence({2});
    REQUIRE(sequence != nullptr);
    const auto* found = sequence->find_track(track);
    REQUIRE(found != nullptr);
    return found->tuning();
}

} // namespace

TEST_CASE("project tuning is authored, replaced, and cleared under an exact gate",
          "[timeline][tuning-command]") {
    const auto project = project_with({}, {});
    REQUIRE_FALSE(project.tuning().has_value());

    // Absent is a value the gate compares, not a wildcard: this states "the
    // project names no tuning" and writes one.
    auto authored = take(reduce_transaction(
        project, one(SetProjectTuning{{}, equal_temperament(432'000)})));
    REQUIRE(authored.project.tuning().has_value());
    CHECK(authored.project.tuning()->reference_pitch_millihertz == 432'000u);
    REQUIRE(authored.dirty.items().size() == 1);
    CHECK(authored.dirty.items()[0].item == project.id());

    auto replaced = take(reduce_transaction(
        authored.project,
        one(SetProjectTuning{equal_temperament(432'000), equal_temperament(415'000)})));
    CHECK(replaced.project.tuning()->reference_pitch_millihertz == 415'000u);

    // Clearing is an authored act on a value the project keeps, not a removal
    // of anything the document owns, so it reduces through the same Modify.
    auto cleared = take(reduce_transaction(
        replaced.project, one(SetProjectTuning{equal_temperament(415'000), {}})));
    CHECK_FALSE(cleared.project.tuning().has_value());

    // Every inverse reduces publicly, including the one that restores absence:
    // no identity is created or retired by any of the three, so none needs a
    // session to undo.
    const auto* inverse = std::get_if<SetProjectTuning>(&cleared.inverses[0]);
    REQUIRE(inverse != nullptr);
    auto undone = take(reduce_transaction(cleared.project, one(*inverse)));
    CHECK(undone.project.tuning() == replaced.project.tuning());
}

TEST_CASE("a track override applies to its own track and leaves the project's alone",
          "[timeline][tuning-command]") {
    const auto project = project_with(equal_temperament(440'000), {});
    auto reduced = take(reduce_transaction(
        project, one(SetTrackTuning{{2}, {3}, {}, equal_temperament(432'000)})));

    REQUIRE(track_tuning_of(reduced.project, {3}).has_value());
    CHECK(track_tuning_of(reduced.project, {3})->reference_pitch_millihertz == 432'000u);
    // The other track states nothing, so it still plays the project's tuning,
    // and the project's own statement is untouched.
    CHECK_FALSE(track_tuning_of(reduced.project, {6}).has_value());
    CHECK(reduced.project.tuning()->reference_pitch_millihertz == 440'000u);
    REQUIRE(reduced.dirty.items().size() == 1);
    CHECK(reduced.dirty.items()[0].item == ItemId{3});
    CHECK(reduced.dirty.items()[0].owner_sequence == ItemId{2});

    // Clearing the override hands the track back to the project rather than
    // silencing it.
    const auto* inverse = std::get_if<SetTrackTuning>(&reduced.inverses[0]);
    REQUIRE(inverse != nullptr);
    auto undone = take(reduce_transaction(reduced.project, one(*inverse)));
    CHECK_FALSE(track_tuning_of(undone.project, {3}).has_value());
    CHECK(undone.project.tuning()->reference_pitch_millihertz == 440'000u);
}

TEST_CASE("a stale tuning expectation is refused and the document is unchanged",
          "[timeline][tuning-command]") {
    const auto project = project_with(equal_temperament(440'000), equal_temperament(432'000));

    auto stale_project = reduce_transaction(
        project, one(SetProjectTuning{equal_temperament(415'000), equal_temperament(444'000)}));
    REQUIRE_FALSE(stale_project.has_value());
    CHECK(stale_project.error().code == ConflictCode::ExpectedValueMismatch);

    // Expecting absence where a value stands is refused rather than treated as
    // "no opinion", which would let a writer overwrite an override it never saw.
    auto stale_absent =
        reduce_transaction(project, one(SetTrackTuning{{2}, {3}, {}, equal_temperament(444'000)}));
    REQUIRE_FALSE(stale_absent.has_value());
    CHECK(stale_absent.error().code == ConflictCode::ExpectedValueMismatch);

    CHECK(project.tuning()->reference_pitch_millihertz == 440'000u);
    CHECK(track_tuning_of(project, {3})->reference_pitch_millihertz == 432'000u);

    // Control: the same commands with the expectation the document actually
    // holds are admitted, so the refusals above are the stale gate rather than
    // a reducer that refuses every tuning edit.
    CHECK(reduce_transaction(
              project, one(SetProjectTuning{equal_temperament(440'000), equal_temperament(444'000)}))
              .has_value());
    CHECK(reduce_transaction(project, one(SetTrackTuning{{2}, {3}, equal_temperament(432'000),
                                                         equal_temperament(444'000)}))
              .has_value());
}

TEST_CASE("an inconsistent tuning is refused by the model's own helper",
          "[timeline][tuning-command]") {
    const auto project = project_with({}, {});

    // A non-Scala system carrying a payload hash. The refusal is the shared
    // valid_tuning_reference rule reached through the model, so the error
    // carries the model's own reason rather than a restated one.
    TuningReference stray = equal_temperament(440'000);
    stray.scale_content = hash('d');
    REQUIRE_FALSE(valid_tuning_reference(stray));
    auto refused = reduce_transaction(project, one(SetProjectTuning{{}, stray}));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConflictCode::ModelInvariant);
    REQUIRE(refused.error().model_error.has_value());
    CHECK(refused.error().model_error->code == ModelErrorCode::InvalidTuningReference);

    // A reference pitch below the admitted floor, refused the same way on the
    // track command.
    const auto silent = equal_temperament(kMinReferencePitchMillihertz - 1);
    auto too_low = reduce_transaction(project, one(SetTrackTuning{{2}, {3}, {}, silent}));
    REQUIRE_FALSE(too_low.has_value());
    REQUIRE(too_low.error().model_error.has_value());
    CHECK(too_low.error().model_error->code == ModelErrorCode::InvalidTuningReference);

    // Control: a Scala reference that names its payload is admitted on both, so
    // the refusals are the inconsistency rather than the systems involved.
    CHECK(reduce_transaction(project, one(SetProjectTuning{{}, scala_tuning()})).has_value());
    CHECK(reduce_transaction(project, one(SetTrackTuning{{2}, {3}, {}, scala_tuning()}))
              .has_value());
}

TEST_CASE("a region's section role is corrected in place and the edit inverts exactly",
          "[timeline][region-command]") {
    const auto project = project_with({}, {}, {region(SectionRole::Verse)});
    CHECK(only_region(project).role == SectionRole::Verse);

    auto reduced = take(reduce_transaction(
        project, one(SetRegion{{2}, region(SectionRole::Verse), region(SectionRole::Chorus)})));
    CHECK(only_region(reduced.project).role == SectionRole::Chorus);
    // Identity survives the correction, which is what makes this a Modify and
    // not a removal followed by a creation.
    CHECK(only_region(reduced.project).id == ItemId{15});
    REQUIRE(reduced.dirty.items().size() == 1);
    CHECK(reduced.dirty.items()[0].item == ItemId{15});

    // The inverse reduces through the public entry point because nothing was
    // retired: a region edit that keeps identity plans no identity mutation.
    const auto* inverse = std::get_if<SetRegion>(&reduced.inverses[0]);
    REQUIRE(inverse != nullptr);
    auto undone = take(reduce_transaction(reduced.project, one(*inverse)));
    CHECK(only_region(undone.project) == only_region(project));
}

TEST_CASE("a region edit gates on every authored member, role included",
          "[timeline][region-command]") {
    const auto project = project_with({}, {}, {region(SectionRole::Verse)});

    // The gate compares the whole region. An expectation that is right about
    // name, span, and identity but wrong about role is a statement about a
    // document that does not exist -- and it is the case a member-by-member
    // comparison that forgot the role would wrongly admit.
    auto stale_role = reduce_transaction(
        project, one(SetRegion{{2}, region(SectionRole::Chorus), region(SectionRole::Drop)}));
    REQUIRE_FALSE(stale_role.has_value());
    CHECK(stale_role.error().code == ConflictCode::ExpectedValueMismatch);

    auto stale_name = reduce_transaction(
        project, one(SetRegion{{2}, region(SectionRole::Verse, "renamed"),
                               region(SectionRole::Drop)}));
    REQUIRE_FALSE(stale_name.has_value());
    CHECK(stale_name.error().code == ConflictCode::ExpectedValueMismatch);
    CHECK(only_region(project).role == SectionRole::Verse);

    // Control: the expectation the document holds is admitted, so both
    // refusals are the stale member rather than a reducer refusing every edit.
    CHECK(reduce_transaction(
              project, one(SetRegion{{2}, region(SectionRole::Verse), region(SectionRole::Drop)}))
              .has_value());
}

TEST_CASE("a region edit may not swap identity and may not name a region the sequence lacks",
          "[timeline][region-command]") {
    const auto project = project_with({}, {}, {region(SectionRole::Verse)});

    auto swapped = region(SectionRole::Chorus);
    swapped.id = {16};
    auto refused =
        reduce_transaction(project, one(SetRegion{{2}, region(SectionRole::Verse), swapped}));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConflictCode::ExpectedValueMismatch);

    auto absent = region(SectionRole::Verse);
    absent.id = {17};
    auto missing = reduce_transaction(project, one(SetRegion{{2}, absent, absent}));
    REQUIRE_FALSE(missing.has_value());

    // Control: the identity the sequence does own, unswapped, is admitted.
    CHECK(reduce_transaction(
              project, one(SetRegion{{2}, region(SectionRole::Verse), region(SectionRole::Drop)}))
              .has_value());
}

TEST_CASE("tuning and region commands round trip through their schema envelopes",
          "[timeline][tuning-command][region-command][persistence]") {
    const auto registry = builtins();
    const std::string project_tuning =
        R"({"data":{"expected":null,"replacement":{"keyboard_map_content":null,)"
        R"("reference_pitch_millihertz":432000,"scale_content":null,)"
        R"("system":"equal_temperament"}},)"
        R"("type_name":"pulp.timeline.command.set_project_tuning","version":1})";
    auto decoded_project = deserialize_commands("[" + project_tuning + "]", registry);
    REQUIRE(decoded_project.has_value());
    const auto* project_edit = std::get_if<SetProjectTuning>(&decoded_project.value()[0]);
    REQUIRE(project_edit != nullptr);
    CHECK_FALSE(project_edit->expected.has_value());
    REQUIRE(project_edit->replacement.has_value());
    CHECK(project_edit->replacement->reference_pitch_millihertz == 432'000u);

    const std::string track_tuning =
        R"({"data":{"expected":{"keyboard_map_content":null,)"
        R"("reference_pitch_millihertz":440000,"scale_content":null,)"
        R"("system":"equal_temperament"},"sequence_id":"2","track_id":"3"},)"
        R"("type_name":"pulp.timeline.command.set_track_tuning","version":1})";
    auto decoded_track = deserialize_commands("[" + track_tuning + "]", registry);
    REQUIRE(decoded_track.has_value());
    const auto* track_edit = std::get_if<SetTrackTuning>(&decoded_track.value()[0]);
    REQUIRE(track_edit != nullptr);
    CHECK(track_edit->track_id == ItemId{3});
    REQUIRE(track_edit->expected.has_value());
    // An omitted replacement is the claim "clear it", which is why clearing is
    // expressible on the wire at all.
    CHECK_FALSE(track_edit->replacement.has_value());

    const auto region_envelope = [](std::string_view role) {
        return std::string(R"({"data":{"duration":"48","id":"15","name":"section",)"
                           R"("position":"0","role":)") +
               std::string(role) + R"(},"type_name":"pulp.timeline.region","version":1})";
    };
    const std::string region_edit =
        R"({"data":{"expected":)" + region_envelope(R"("verse")") + R"(,"replacement":)" +
        region_envelope(R"("chorus")") +
        R"(,"sequence_id":"2"},"type_name":"pulp.timeline.command.set_region","version":1})";
    auto decoded_region = deserialize_commands("[" + region_edit + "]", registry);
    REQUIRE(decoded_region.has_value());
    const auto* edit = std::get_if<SetRegion>(&decoded_region.value()[0]);
    REQUIRE(edit != nullptr);
    CHECK(edit->expected.role == SectionRole::Verse);
    CHECK(edit->replacement.role == SectionRole::Chorus);
}

TEST_CASE("decode refuses a region edit that swaps identity",
          "[timeline][region-command][persistence]") {
    const auto registry = builtins();
    const auto envelope = [](std::string_view replacement_id) {
        return std::string(
                   R"([{"data":{"expected":{"data":{"duration":"48","id":"15","name":"section",)"
                   R"("position":"0","role":"verse"},"type_name":"pulp.timeline.region",)"
                   R"("version":1},"replacement":{"data":{"duration":"48","id":")") +
               std::string(replacement_id) +
               R"(","name":"section","position":"0","role":"chorus"},)"
               R"("type_name":"pulp.timeline.region","version":1},"sequence_id":"2"},)"
               R"("type_name":"pulp.timeline.command.set_region","version":1}])";
    };
    // Control first: the same payload with matching identities decodes, so the
    // refusal below is the swap and not the surrounding envelope.
    REQUIRE(deserialize_commands(envelope("15"), registry).has_value());
    // Refused at the wire as well as in the reducer, because the model is handed
    // one region and cannot see that the caller named a different one.
    REQUIRE_FALSE(deserialize_commands(envelope("16"), registry));
}

TEST_CASE("a writer denied removal may still correct a region's role and retune",
          "[timeline][tuning-command][region-command][capabilities]") {
    constexpr auto project_tuning = command_authority_of<SetProjectTuning>();
    constexpr auto track_tuning = command_authority_of<SetTrackTuning>();
    constexpr auto region_edit = command_authority_of<SetRegion>();
    STATIC_REQUIRE(project_tuning.command_class == CommandClass::Timing);
    STATIC_REQUIRE(project_tuning.intent == CommandIntent::Modify);
    STATIC_REQUIRE(track_tuning.command_class == CommandClass::Timing);
    STATIC_REQUIRE(region_edit.command_class == CommandClass::Annotation);
    STATIC_REQUIRE(region_edit.intent == CommandIntent::Modify);

    // No new class: adding one renumbers capability_bit and silently breaks
    // every persisted and transmitted writer mask, so this is asserted rather
    // than assumed.
    STATIC_REQUIRE(kCommandClassCount == 11);
    // The literals are the point: a class inserted ahead of these would shift
    // them and silently repurpose every persisted and transmitted mask bit.
    STATIC_REQUIRE(capability_bit({CommandClass::Annotation, CommandIntent::Modify}) ==
                   std::uint64_t{1} << 25);
    STATIC_REQUIRE(capability_bit({CommandClass::Timing, CommandIntent::Modify}) ==
                   std::uint64_t{1} << 28);
    STATIC_REQUIRE(capability_bit({CommandClass::Asset, CommandIntent::Remove}) ==
                   std::uint64_t{1} << 32);

    // This is the defect the family closes. A proposal-profile writer holds
    // every class and no destructive intent, so before SetRegion existed it
    // could author a region carrying a role and never correct it: the only
    // correction was remove-then-insert, and the remove is the axis that
    // profile is denied.
    const auto proposal = non_destructive_capabilities();
    CHECK(allows(proposal, region_edit));
    CHECK(allows(proposal, project_tuning));
    CHECK(allows(proposal, track_tuning));
    CHECK_FALSE(allows(proposal, command_authority_of<RemoveRegion>()));

    // Control: a mask denying nothing admits the removal, so the refusal above
    // is the destructive axis rather than a mask that refuses everything.
    CHECK(allows(unrestricted_capabilities(), command_authority_of<RemoveRegion>()));

    // Retuning sits on one bit, so a mask can forbid it without also forbidding
    // track renames -- which is the reason Track was not widened instead.
    const auto no_retune =
        deny(unrestricted_capabilities(), CommandClass::Timing, CommandIntent::Modify);
    CHECK_FALSE(allows(no_retune, project_tuning));
    CHECK(allows(no_retune, command_authority_of<SetTrackName>()));
}
