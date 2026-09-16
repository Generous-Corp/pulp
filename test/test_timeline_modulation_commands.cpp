#include "support/timeline_persistence_test_support.hpp"
#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/transaction.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

Modulator lfo(ItemId id, std::string name = "wobble") {
    return Modulator{id, ModulatorKind::Lfo, std::move(name)};
}

MacroControl macro(ItemId id, float value, std::string name = "brightness") {
    return MacroControl{id, std::move(name), value};
}

ModulationRoute route_from(ItemId id, ItemId source, ModulationSourceKind kind) {
    return ModulationRoute{id, ModulationSourceRef{source, kind},
                           TrackMixerTarget{TrackMixerParameter::Gain}, 0.5f, true};
}

// Two tracks, so an assertion that an edit reached one track cannot pass by
// there being only one track to reach.
Project project_with(std::vector<Modulator> modulators, std::vector<MacroControl> macros,
                     std::vector<ModulationRoute> routes = {}) {
    TrackInput first;
    first.id = {3};
    first.name = "modulated";
    first.modulators = std::move(modulators);
    first.macros = std::move(macros);
    first.modulation_routes = std::move(routes);
    TrackInput second;
    second.id = {6};
    second.name = "plain";
    auto sequence = take(Sequence::create(
        {2}, "sequence", TickDuration{48}, {},
        {take(Track::create(std::move(first))), take(Track::create(std::move(second)))}));
    return take(Project::create(ProjectInput{{1}, "project", 40, {2}, {}, {sequence}}));
}

Transaction one(Command command) {
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1}, std::move(command)});
    return transaction;
}

const Track& modulated_track(const Project& project, ItemId track = {3}) {
    const auto* sequence = project.find_sequence({2});
    REQUIRE(sequence != nullptr);
    const auto* found = sequence->find_track(track);
    REQUIRE(found != nullptr);
    return *found;
}

std::string bits_of(float value) {
    return std::to_string(std::bit_cast<std::uint32_t>(value));
}

std::string modulator_envelope(std::string_view id, std::string_view kind, std::string_view name) {
    return std::string(R"({"data":{"id":")") + std::string(id) + R"(","kind":")" +
           std::string(kind) + R"(","name":")" + std::string(name) +
           R"("},"type_name":"pulp.timeline.modulator","version":1})";
}

std::string macro_envelope(std::string_view id, std::string_view name, float value) {
    return std::string(R"({"data":{"id":")") + std::string(id) + R"(","name":")" +
           std::string(name) + R"(","value_bits":")" + bits_of(value) +
           R"("},"type_name":"pulp.timeline.macro_control","version":1})";
}

std::string command_envelope(std::string_view type, std::string payload) {
    return std::string(R"({"data":)") + std::move(payload) + R"(,"type_name":")" +
           std::string(type) + R"(","version":1})";
}

} // namespace

TEST_CASE("a modulation source is authored and corrected in place and removed whole",
          "[timeline][modulation-command]") {
    const auto project = project_with({}, {});
    CHECK(modulated_track(project).modulators().empty());

    auto inserted =
        take(reduce_transaction(project, one(InsertModulator{{2}, {3}, lfo({40}, "slow")})));
    REQUIRE(modulated_track(inserted.project).modulators().size() == 1);
    CHECK(modulated_track(inserted.project).find_modulator({40})->name == "slow");
    // The second track is untouched, so the edit landed on the track it named.
    CHECK(modulated_track(inserted.project, {6}).modulators().empty());
    REQUIRE(inserted.dirty.items().size() == 1);
    CHECK(inserted.dirty.items()[0].item == ItemId{40});
    CHECK(inserted.dirty.items()[0].owner_track == ItemId{3});
    const auto* insert_inverse = std::get_if<RemoveModulator>(&inserted.inverses[0]);
    REQUIRE(insert_inverse != nullptr);
    CHECK(insert_inverse->modulator_id == ItemId{40});

    // A Modify reaches every mutable member, which is the whole point: a writer
    // that may not remove can still correct what it authored.
    auto corrected = take(reduce_transaction(
        inserted.project, one(SetModulator{{2},
                                           {3},
                                           {40},
                                           lfo({40}, "slow"),
                                           Modulator{{40}, ModulatorKind::Envelope, "attack"}})));
    const auto* changed = modulated_track(corrected.project).find_modulator({40});
    REQUIRE(changed != nullptr);
    CHECK(changed->kind == ModulatorKind::Envelope);
    CHECK(changed->name == "attack");
    // The inverse is the same command with the two sides swapped, so undo is
    // exact rather than reconstructed.
    const auto* set_inverse = std::get_if<SetModulator>(&corrected.inverses[0]);
    REQUIRE(set_inverse != nullptr);
    auto undone = take(reduce_transaction(corrected.project, one(*set_inverse)));
    CHECK(*modulated_track(undone.project).find_modulator({40}) == lfo({40}, "slow"));

    auto removed =
        take(reduce_transaction(corrected.project, one(RemoveModulator{{2}, {3}, {40}})));
    CHECK(modulated_track(removed.project).modulators().empty());
    // The inverse carries the declaration as it stood, not a default-constructed
    // stand-in with the same identity.
    const auto* remove_inverse = std::get_if<InsertModulator>(&removed.inverses[0]);
    REQUIRE(remove_inverse != nullptr);
    CHECK(remove_inverse->modulator.kind == ModulatorKind::Envelope);
    CHECK(remove_inverse->modulator.name == "attack");
}

TEST_CASE("a macro is authored and its position moves under a gate that covers only the position",
          "[timeline][modulation-command]") {
    const auto project = project_with({}, {});
    auto inserted =
        take(reduce_transaction(project, one(InsertMacro{{2}, {3}, macro({41}, 0.5f)})));
    REQUIRE(modulated_track(inserted.project).macros().size() == 1);
    CHECK(modulated_track(inserted.project).find_macro({41})->value == 0.5f);

    // The narrow command names no name, so a rename it did not conflict with
    // cannot abort it. That is the reason it exists beside SetMacro.
    auto renamed = take(reduce_transaction(
        inserted.project,
        one(SetMacro{{2}, {3}, {41}, macro({41}, 0.5f), macro({41}, 0.5f, "tone")})));
    auto moved =
        take(reduce_transaction(renamed.project, one(SetMacroValue{{2}, {3}, {41}, 0.5f, 0.25f})));
    const auto* after = modulated_track(moved.project).find_macro({41});
    REQUIRE(after != nullptr);
    CHECK(after->value == 0.25f);
    // The rename survived the move, so the narrow command wrote the one member
    // it names rather than replacing the macro.
    CHECK(after->name == "tone");

    const auto* value_inverse = std::get_if<SetMacroValue>(&moved.inverses[0]);
    REQUIRE(value_inverse != nullptr);
    CHECK(value_inverse->expected == 0.25f);
    CHECK(value_inverse->replacement == 0.5f);
    auto undone = take(reduce_transaction(moved.project, one(*value_inverse)));
    CHECK(modulated_track(undone.project).find_macro({41})->value == 0.5f);
    CHECK(modulated_track(undone.project).find_macro({41})->name == "tone");
}

TEST_CASE("a stale modulation expectation is refused and the document is unchanged",
          "[timeline][modulation-command]") {
    const auto project = project_with({lfo({10})}, {macro({11}, 0.5f)});

    auto stale_modulator = reduce_transaction(
        project, one(SetModulator{{2}, {3}, {10}, lfo({10}, "other"), lfo({10}, "next")}));
    REQUIRE_FALSE(stale_modulator);
    CHECK(stale_modulator.error().code == ConflictCode::ExpectedValueMismatch);

    auto stale_macro = reduce_transaction(
        project, one(SetMacro{{2}, {3}, {11}, macro({11}, 0.25f), macro({11}, 0.75f)}));
    REQUIRE_FALSE(stale_macro);
    CHECK(stale_macro.error().code == ConflictCode::ExpectedValueMismatch);

    auto stale_value =
        reduce_transaction(project, one(SetMacroValue{{2}, {3}, {11}, 0.25f, 0.75f}));
    REQUIRE_FALSE(stale_value);
    CHECK(stale_value.error().code == ConflictCode::ExpectedValueMismatch);

    // Control: the same three commands with the expectation the document holds
    // are admitted, so the refusals above are the gate rather than a malformed
    // address.
    CHECK(
        reduce_transaction(project, one(SetModulator{{2}, {3}, {10}, lfo({10}), lfo({10}, "next")}))
            .has_value());
    CHECK(reduce_transaction(project,
                             one(SetMacro{{2}, {3}, {11}, macro({11}, 0.5f), macro({11}, 0.75f)}))
              .has_value());
    CHECK(reduce_transaction(project, one(SetMacroValue{{2}, {3}, {11}, 0.5f, 0.75f})).has_value());

    // Nothing was written by any refusal: the reducer returns the error before
    // it edits, so the caller still holds the document it passed in.
    CHECK(modulated_track(project).find_modulator({10})->name == "wobble");
    CHECK(modulated_track(project).find_macro({11})->value == 0.5f);
}

TEST_CASE("a modulation edit may not move identity in either of the two ways it could",
          "[timeline][modulation-command]") {
    const auto project = project_with({lfo({10})}, {macro({11}, 0.5f)});

    // expected.id != replacement.id -- a removal and a creation wearing a
    // signature that declares neither.
    auto modulator_swap = reduce_transaction(
        project, one(SetModulator{{2}, {3}, {10}, lfo({10}), lfo({14}, "next")}));
    REQUIRE_FALSE(modulator_swap);
    CHECK(modulator_swap.error().code == ConflictCode::ModelInvariant);
    // expected.id != modulator_id -- the command names one item and gates on
    // another.
    auto modulator_mismatch = reduce_transaction(
        project, one(SetModulator{{2}, {3}, {14}, lfo({10}), lfo({10}, "next")}));
    REQUIRE_FALSE(modulator_mismatch);
    CHECK(modulator_mismatch.error().code == ConflictCode::ModelInvariant);

    auto macro_swap = reduce_transaction(
        project, one(SetMacro{{2}, {3}, {11}, macro({11}, 0.5f), macro({14}, 0.5f)}));
    REQUIRE_FALSE(macro_swap);
    CHECK(macro_swap.error().code == ConflictCode::ModelInvariant);
    auto macro_mismatch = reduce_transaction(
        project, one(SetMacro{{2}, {3}, {14}, macro({11}, 0.5f), macro({11}, 0.75f)}));
    REQUIRE_FALSE(macro_mismatch);
    CHECK(macro_mismatch.error().code == ConflictCode::ModelInvariant);

    // Control: the identical commands with identity held still are admitted.
    CHECK(reduce_transaction(project, one(SetModulator{{2}, {3}, {10}, lfo({10}), lfo({10}, "n")}))
              .has_value());
    CHECK(reduce_transaction(project,
                             one(SetMacro{{2}, {3}, {11}, macro({11}, 0.5f), macro({11}, 0.75f)}))
              .has_value());
}

TEST_CASE("a command addressed at the wrong kind of item is refused",
          "[timeline][modulation-command]") {
    const auto project = project_with({lfo({10})}, {macro({11}, 0.5f)});

    // A modulator command naming a macro's identity, and the reverse. Both
    // identities are live and owned by the same track, so the only thing
    // separating them is the kind the location carries -- which is the same
    // reason ModulationSourceRef carries its source's kind rather than
    // inferring it.
    auto modulator_at_macro = reduce_transaction(project, one(RemoveModulator{{2}, {3}, {11}}));
    REQUIRE_FALSE(modulator_at_macro);
    CHECK(modulator_at_macro.error().code == ConflictCode::WrongTargetKind);
    auto macro_at_modulator =
        reduce_transaction(project, one(SetMacroValue{{2}, {3}, {10}, 0.5f, 0.25f}));
    REQUIRE_FALSE(macro_at_modulator);
    CHECK(macro_at_modulator.error().code == ConflictCode::WrongTargetKind);

    // A command naming a track that does not own the item is refused on the
    // parent rather than admitted against the wrong track.
    auto wrong_track = reduce_transaction(project, one(RemoveModulator{{2}, {6}, {10}}));
    REQUIRE_FALSE(wrong_track);
    CHECK(wrong_track.error().code == ConflictCode::ParentMismatch);

    // Control: each command against its own kind on its own track is admitted,
    // so the three refusals are the address and not the command.
    CHECK(reduce_transaction(project, one(RemoveModulator{{2}, {3}, {10}})).has_value());
    CHECK(reduce_transaction(project, one(SetMacroValue{{2}, {3}, {11}, 0.5f, 0.25f})).has_value());
}

TEST_CASE("a source a route still reads cannot be removed", "[timeline][modulation-command]") {
    const auto wired = project_with({lfo({10})}, {macro({11}, 0.5f)},
                                    {route_from({12}, {10}, ModulationSourceKind::Modulator),
                                     route_from({13}, {11}, ModulationSourceKind::Macro)});
    auto orphan_modulator = reduce_transaction(wired, one(RemoveModulator{{2}, {3}, {10}}));
    REQUIRE_FALSE(orphan_modulator);
    CHECK(orphan_modulator.error().code == ConflictCode::ModelInvariant);
    auto orphan_macro = reduce_transaction(wired, one(RemoveMacro{{2}, {3}, {11}}));
    REQUIRE_FALSE(orphan_macro);
    CHECK(orphan_macro.error().code == ConflictCode::ModelInvariant);

    // Control: the same two removals against a document with no routes are
    // admitted, so the refusals are the dangling reference rather than removal
    // being unreachable.
    const auto unwired = project_with({lfo({10})}, {macro({11}, 0.5f)});
    CHECK(reduce_transaction(unwired, one(RemoveModulator{{2}, {3}, {10}})).has_value());
    CHECK(reduce_transaction(unwired, one(RemoveMacro{{2}, {3}, {11}})).has_value());
}

TEST_CASE("an out-of-range macro position and an undeclared modulator kind are refused",
          "[timeline][modulation-command]") {
    const auto project = project_with({}, {macro({11}, 0.5f)});

    // Range and NaN refusal lives in the model, reached through the same helper
    // Track::create applies, so this surfaces as a model failure rather than a
    // silently clamped macro.
    CHECK_FALSE(reduce_transaction(project, one(SetMacroValue{{2}, {3}, {11}, 0.5f, 1.5f})));
    CHECK_FALSE(reduce_transaction(
        project,
        one(SetMacroValue{{2}, {3}, {11}, 0.5f, std::numeric_limits<float>::quiet_NaN()})));
    CHECK_FALSE(reduce_transaction(project, one(InsertMacro{{2}, {3}, macro({44}, -0.25f)})));
    CHECK_FALSE(reduce_transaction(
        project,
        one(InsertModulator{{2}, {3}, Modulator{{44}, static_cast<ModulatorKind>(9), "bogus"}})));

    // Control: the in-range and declared forms of all four are admitted.
    CHECK(reduce_transaction(project, one(SetMacroValue{{2}, {3}, {11}, 0.5f, 1.0f})).has_value());
    CHECK(reduce_transaction(project, one(InsertMacro{{2}, {3}, macro({44}, 0.0f)})).has_value());
    CHECK(reduce_transaction(project, one(InsertModulator{{2}, {3}, lfo({44})})).has_value());
}

TEST_CASE("modulation commands round trip through their schema envelopes",
          "[timeline][modulation-command][persistence]") {
    const auto registry = builtins();
    const auto decode_one = [&](std::string json) {
        auto decoded = deserialize_commands("[" + std::move(json) + "]", registry);
        REQUIRE(decoded.has_value());
        return std::move(decoded).value()[0];
    };

    const auto insert = decode_one(
        command_envelope("pulp.timeline.command.insert_modulator",
                         R"({"modulator":)" + modulator_envelope("40", "envelope", "attack") +
                             R"(,"sequence_id":"2","track_id":"3"})"));
    const auto* inserted = std::get_if<InsertModulator>(&insert);
    REQUIRE(inserted != nullptr);
    CHECK(inserted->modulator.kind == ModulatorKind::Envelope);
    CHECK(inserted->modulator.name == "attack");

    const auto remove =
        decode_one(command_envelope("pulp.timeline.command.remove_modulator",
                                    R"({"modulator_id":"40","sequence_id":"2","track_id":"3"})"));
    REQUIRE(std::holds_alternative<RemoveModulator>(remove));

    const auto set = decode_one(command_envelope(
        "pulp.timeline.command.set_modulator",
        R"({"expected":)" + modulator_envelope("40", "lfo", "slow") +
            R"(,"modulator_id":"40","replacement":)" + modulator_envelope("40", "random", "drift") +
            R"(,"sequence_id":"2","track_id":"3"})"));
    const auto* edited = std::get_if<SetModulator>(&set);
    REQUIRE(edited != nullptr);
    CHECK(edited->expected.kind == ModulatorKind::Lfo);
    CHECK(edited->replacement.kind == ModulatorKind::Random);

    const auto insert_macro =
        decode_one(command_envelope("pulp.timeline.command.insert_macro",
                                    R"({"macro":)" + macro_envelope("41", "brightness", 0.1f) +
                                        R"(,"sequence_id":"2","track_id":"3"})"));
    const auto* macro_inserted = std::get_if<InsertMacro>(&insert_macro);
    REQUIRE(macro_inserted != nullptr);
    // Bit-exact, not approximately: the wire spells the float as its IEEE-754
    // bit pattern, and 0.1f is precisely the value a decimal spelling would
    // round away.
    CHECK(macro_inserted->macro.value == 0.1f);

    const auto remove_macro =
        decode_one(command_envelope("pulp.timeline.command.remove_macro",
                                    R"({"macro_id":"41","sequence_id":"2","track_id":"3"})"));
    REQUIRE(std::holds_alternative<RemoveMacro>(remove_macro));

    const auto set_macro = decode_one(command_envelope(
        "pulp.timeline.command.set_macro",
        R"({"expected":)" + macro_envelope("41", "brightness", 0.1f) +
            R"(,"macro_id":"41","replacement":)" + macro_envelope("41", "tone", 0.9f) +
            R"(,"sequence_id":"2","track_id":"3"})"));
    const auto* macro_edited = std::get_if<SetMacro>(&set_macro);
    REQUIRE(macro_edited != nullptr);
    CHECK(macro_edited->expected.name == "brightness");
    CHECK(macro_edited->replacement.value == 0.9f);

    const auto set_value = decode_one(command_envelope(
        "pulp.timeline.command.set_macro_value",
        R"({"expected_bits":")" + bits_of(0.1f) + R"(","macro_id":"41","replacement_bits":")" +
            bits_of(0.9f) + R"(","sequence_id":"2","track_id":"3"})"));
    const auto* value_edited = std::get_if<SetMacroValue>(&set_value);
    REQUIRE(value_edited != nullptr);
    CHECK(value_edited->expected == 0.1f);
    CHECK(value_edited->replacement == 0.9f);
}

TEST_CASE("a decoded macro position satisfies the gate the encoder wrote it for",
          "[timeline][modulation-command][persistence]") {
    const auto registry = builtins();
    // 0.1f has no exact decimal spelling at float width, so this is the value a
    // decimal wire format would lose. The document holds it, the command is
    // decoded from the wire, and the gate compares the two exactly.
    const auto project = project_with({}, {macro({11}, 0.1f)});
    auto decoded = deserialize_commands(
        "[" +
            command_envelope("pulp.timeline.command.set_macro_value",
                             R"({"expected_bits":")" + bits_of(0.1f) +
                                 R"(","macro_id":"11","replacement_bits":")" + bits_of(0.2f) +
                                 R"(","sequence_id":"2","track_id":"3"})") +
            "]",
        registry);
    REQUIRE(decoded.has_value());
    auto applied = reduce_transaction(project, one(decoded.value()[0]));
    REQUIRE(applied.has_value());
    CHECK(modulated_track(applied->project).find_macro({11})->value == 0.2f);

    // Negative control: the neighbouring bit pattern is the nearest float to
    // the same decimal, and it is refused -- so the acceptance above is the
    // exact value surviving and not a gate that accepts anything close.
    const auto neighbour =
        std::bit_cast<float>(std::bit_cast<std::uint32_t>(0.1f) + std::uint32_t{1});
    CHECK(neighbour != 0.1f);
    auto near_miss = deserialize_commands(
        "[" +
            command_envelope("pulp.timeline.command.set_macro_value",
                             R"({"expected_bits":")" + bits_of(neighbour) +
                                 R"(","macro_id":"11","replacement_bits":")" + bits_of(0.2f) +
                                 R"(","sequence_id":"2","track_id":"3"})") +
            "]",
        registry);
    REQUIRE(near_miss.has_value());
    auto refused = reduce_transaction(project, one(near_miss.value()[0]));
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ConflictCode::ExpectedValueMismatch);
}

TEST_CASE("decode refuses a modulation edit that moves identity",
          "[timeline][modulation-command][persistence]") {
    const auto registry = builtins();
    const auto modulator_edit = [&](std::string_view gate_id, std::string_view replacement_id,
                                    std::string_view named_id) {
        return "[" +
               command_envelope("pulp.timeline.command.set_modulator",
                                R"({"expected":)" + modulator_envelope(gate_id, "lfo", "slow") +
                                    R"(,"modulator_id":")" + std::string(named_id) +
                                    R"(","replacement":)" +
                                    modulator_envelope(replacement_id, "random", "drift") +
                                    R"(,"sequence_id":"2","track_id":"3"})") +
               "]";
    };
    // Control first: all three identities agreeing decodes, so each refusal
    // below is the disagreement and not the surrounding envelope.
    REQUIRE(deserialize_commands(modulator_edit("40", "40", "40"), registry).has_value());
    REQUIRE_FALSE(deserialize_commands(modulator_edit("40", "41", "40"), registry));
    REQUIRE_FALSE(deserialize_commands(modulator_edit("40", "40", "41"), registry));

    const auto macro_edit = [&](std::string_view gate_id, std::string_view replacement_id,
                                std::string_view named_id) {
        return "[" +
               command_envelope("pulp.timeline.command.set_macro",
                                R"({"expected":)" + macro_envelope(gate_id, "brightness", 0.1f) +
                                    R"(,"macro_id":")" + std::string(named_id) +
                                    R"(","replacement":)" +
                                    macro_envelope(replacement_id, "tone", 0.9f) +
                                    R"(,"sequence_id":"2","track_id":"3"})") +
               "]";
    };
    REQUIRE(deserialize_commands(macro_edit("41", "41", "41"), registry).has_value());
    REQUIRE_FALSE(deserialize_commands(macro_edit("41", "42", "41"), registry));
    REQUIRE_FALSE(deserialize_commands(macro_edit("41", "41", "42"), registry));
}

TEST_CASE("undo through a session restores a removed modulation source whole",
          "[timeline][modulation-command]") {
    const auto initial = project_with({lfo({10}, "slow")}, {macro({11}, 0.5f)});
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();

    auto committed = session->submit(
        writer, timeline_test::session_transaction(
                    writer, {}, {RemoveModulator{{2}, {3}, {10}}, RemoveMacro{{2}, {3}, {11}}}));
    REQUIRE(committed);
    CHECK(modulated_track(*session->snapshot()).modulators().empty());
    CHECK(modulated_track(*session->snapshot()).macros().empty());
    // Removal tombstones the identity rather than freeing it, which is why the
    // restoring inverse is reachable only through the session: the public
    // reduce_transaction refuses to insert an identity below next_item_id.
    CHECK_FALSE(session->snapshot()->locate({10})->active);
    auto public_restore =
        reduce_transaction(*session->snapshot(), one(InsertModulator{{2}, {3}, lfo({10}, "slow")}));
    REQUIRE_FALSE(public_restore);
    CHECK(public_restore.error().code == ConflictCode::IdentityNotAvailable);

    REQUIRE(session->undo(writer));
    const auto* restored = modulated_track(*session->snapshot()).find_modulator({10});
    REQUIRE(restored != nullptr);
    CHECK(restored->name == "slow");
    CHECK(restored->kind == ModulatorKind::Lfo);
    const auto* restored_macro = modulated_track(*session->snapshot()).find_macro({11});
    REQUIRE(restored_macro != nullptr);
    CHECK(restored_macro->value == 0.5f);
}

TEST_CASE("a writer denied removal may author and correct a modulator and a macro",
          "[timeline][modulation-command][capabilities]") {
    constexpr auto insert_modulator = command_authority_of<InsertModulator>();
    constexpr auto remove_modulator = command_authority_of<RemoveModulator>();
    constexpr auto set_modulator = command_authority_of<SetModulator>();
    constexpr auto set_macro_value = command_authority_of<SetMacroValue>();
    STATIC_REQUIRE(insert_modulator.command_class == CommandClass::Automation);
    STATIC_REQUIRE(insert_modulator.intent == CommandIntent::Create);
    STATIC_REQUIRE(remove_modulator.intent == CommandIntent::Remove);
    STATIC_REQUIRE(set_modulator.intent == CommandIntent::Modify);
    STATIC_REQUIRE(command_authority_of<InsertMacro>() == insert_modulator);
    STATIC_REQUIRE(set_macro_value.command_class == CommandClass::Automation);

    // No new class: adding one renumbers capability_bit and silently repurposes
    // every persisted and transmitted writer mask, so this is asserted rather
    // than assumed.
    STATIC_REQUIRE(kCommandClassCount == 11);
    // The literals are the point -- a class inserted ahead of these would shift
    // them and quietly change what an existing mask permits.
    STATIC_REQUIRE(capability_bit({CommandClass::Automation, CommandIntent::Create}) ==
                   std::uint64_t{1} << 6);
    STATIC_REQUIRE(capability_bit({CommandClass::Automation, CommandIntent::Modify}) ==
                   std::uint64_t{1} << 7);
    STATIC_REQUIRE(capability_bit({CommandClass::Automation, CommandIntent::Remove}) ==
                   std::uint64_t{1} << 8);

    // Automation gains a Modify command for the first time here. Automation
    // lanes themselves still have Insert and Remove and no Modify, so this does
    // not close that gap -- it only declines to add a ninth collection to it.
    const auto proposal = non_destructive_capabilities();
    CHECK(allows(proposal, insert_modulator));
    CHECK(allows(proposal, set_modulator));
    CHECK(allows(proposal, set_macro_value));
    CHECK(allows(proposal, command_authority_of<SetMacro>()));
    CHECK_FALSE(allows(proposal, remove_modulator));
    CHECK_FALSE(allows(proposal, command_authority_of<RemoveMacro>()));

    // Control: an unrestricted mask admits the removals, so the refusals above
    // are the destructive axis rather than a mask that refuses everything.
    CHECK(allows(unrestricted_capabilities(), remove_modulator));
    CHECK(allows(unrestricted_capabilities(), command_authority_of<RemoveMacro>()));

    // Driven rather than reasoned about: the proposal profile authors both
    // items and then changes every mutable member of each, holding only Create
    // and Modify.
    auto session = std::move(DocumentSession::create(project_with({}, {}))).value();
    auto writer = std::move(session->register_writer(proposal)).value();
    REQUIRE(session->submit(
        writer, timeline_test::session_transaction(writer, {},
                                                   {InsertModulator{{2}, {3}, lfo({40}, "slow")},
                                                    InsertMacro{{2}, {3}, macro({41}, 0.5f)}})));
    REQUIRE(session->submit(
        writer, timeline_test::session_transaction(
                    writer, session->revision(),
                    {SetModulator{{2},
                                  {3},
                                  {40},
                                  lfo({40}, "slow"),
                                  Modulator{{40}, ModulatorKind::StepSequence, "steps"}},
                     SetMacro{{2}, {3}, {41}, macro({41}, 0.5f), macro({41}, 0.25f, "tone")},
                     SetMacroValue{{2}, {3}, {41}, 0.25f, 0.75f}})));
    const auto& track = modulated_track(*session->snapshot());
    CHECK(track.find_modulator({40})->kind == ModulatorKind::StepSequence);
    CHECK(track.find_macro({41})->name == "tone");
    CHECK(track.find_macro({41})->value == 0.75f);

    auto denied = session->submit(
        writer, timeline_test::session_transaction(writer, session->revision(),
                                                   {RemoveModulator{{2}, {3}, {40}}}));
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == ConflictCode::CapabilityDenied);
}

TEST_CASE("modulation command identity and retained size answer for their own payloads",
          "[timeline][modulation-command]") {
    CHECK(equivalent(Command{InsertModulator{{2}, {3}, lfo({40})}},
                     Command{InsertModulator{{2}, {3}, lfo({40})}}));
    CHECK_FALSE(equivalent(Command{InsertModulator{{2}, {3}, lfo({40})}},
                           Command{InsertModulator{{2}, {3}, lfo({40}, "other")}}));
    CHECK(equivalent(Command{SetMacroValue{{2}, {3}, {41}, 0.5f, 0.25f}},
                     Command{SetMacroValue{{2}, {3}, {41}, 0.5f, 0.25f}}));
    CHECK_FALSE(equivalent(Command{SetMacroValue{{2}, {3}, {41}, 0.5f, 0.25f}},
                           Command{SetMacroValue{{2}, {3}, {41}, 0.5f, 0.75f}}));
    // A NaN expectation is the same authored command when it arrives twice.
    // Idempotency asks whether the same bytes arrived again, so the comparison
    // is on the bit pattern and `==` would answer no to both of these.
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(equivalent(Command{SetMacroValue{{2}, {3}, {41}, nan, 0.25f}},
                     Command{SetMacroValue{{2}, {3}, {41}, nan, 0.25f}}));

    // The name is the only heap allocation either item owns, and a gate carries
    // one on each side.
    const auto empty_names = retained_size(
        Command{SetMacro{{2}, {3}, {41}, macro({41}, 0.5f, ""), macro({41}, 0.5f, "")}});
    const auto long_names =
        retained_size(Command{SetMacro{{2},
                                       {3},
                                       {41},
                                       macro({41}, 0.5f, std::string(32, 'a')),
                                       macro({41}, 0.5f, std::string(48, 'b'))}});
    CHECK(long_names == empty_names + 80);
}
