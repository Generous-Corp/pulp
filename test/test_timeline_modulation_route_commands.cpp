#include "support/timeline_persistence_test_support.hpp"
#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/transaction.hpp>

#include <bit>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

Modulator lfo(ItemId id, std::string name = "wobble") {
    return Modulator{id, ModulatorKind::Lfo, std::move(name)};
}

MacroControl macro_control(ItemId id, float value = 0.5f, std::string name = "brightness") {
    return MacroControl{id, std::move(name), value};
}

ModulationRoute mixer_route(ItemId id, ItemId source, ModulationSourceKind kind, float depth = 0.5f,
                            bool enabled = true,
                            TrackMixerParameter parameter = TrackMixerParameter::Gain) {
    return ModulationRoute{id, ModulationSourceRef{source, kind}, TrackMixerTarget{parameter},
                           depth, enabled};
}

// Two tracks and a device chain, so an assertion that an edit reached one track
// cannot pass by there being only one track to reach, and a device-parameter
// target has a placement to name.
Project project_with_routes(std::vector<Modulator> modulators, std::vector<MacroControl> macros,
                            std::vector<ModulationRoute> routes) {
    TrackInput first;
    first.id = {3};
    first.name = "modulated";
    first.device_chain = {DevicePlacement{{8}}};
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

// One modulator and one macro on the track, so a route can name either and a
// kind disagreement is reachable without inventing an identity.
Project wired_project(std::vector<ModulationRoute> routes = {}) {
    return project_with_routes({lfo({10})}, {macro_control({11})}, std::move(routes));
}

Transaction one(Command command) {
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1}, std::move(command)});
    return transaction;
}

const Track& routed_track(const Project& project, ItemId track = {3}) {
    const auto* sequence = project.find_sequence({2});
    REQUIRE(sequence != nullptr);
    const auto* found = sequence->find_track(track);
    REQUIRE(found != nullptr);
    return *found;
}

std::string bits_of(float value) {
    return std::to_string(std::bit_cast<std::uint32_t>(value));
}

std::string route_envelope(std::string_view id, std::string_view source_id,
                           std::string_view source_kind, float depth, bool enabled) {
    return std::string(R"({"data":{"depth_bits":")") + bits_of(depth) + R"(","enabled":)" +
           (enabled ? "true" : "false") + R"(,"id":")" + std::string(id) + R"(","source_id":")" +
           std::string(source_id) + R"(","source_kind":")" + std::string(source_kind) +
           R"(","target":{"data":{"parameter":"gain"},)"
           R"("type_name":"pulp.timeline.automation_target.track_mixer","version":1}},)"
           R"("type_name":"pulp.timeline.modulation_route","version":1})";
}

std::string command_envelope(std::string_view type, std::string payload) {
    return std::string(R"({"data":)") + std::move(payload) + R"(,"type_name":")" +
           std::string(type) + R"(","version":1})";
}

std::string snapshot_of(const Project& project, const SchemaRegistry& registry) {
    return take(serialize_project(project, registry)).json;
}

} // namespace

TEST_CASE("a modulation route is authored and rewired in place and removed whole",
          "[timeline][modulation-route-command]") {
    const auto project = wired_project();
    CHECK(routed_track(project).modulation_routes().empty());

    auto inserted = take(reduce_transaction(
        project, one(InsertModulationRoute{
                     {2}, {3}, mixer_route({40}, {10}, ModulationSourceKind::Modulator, 0.5f)})));
    REQUIRE(routed_track(inserted.project).modulation_routes().size() == 1);
    CHECK(routed_track(inserted.project).find_modulation_route({40})->depth == 0.5f);
    // The second track is untouched, so the edit landed on the track it named.
    CHECK(routed_track(inserted.project, {6}).modulation_routes().empty());
    REQUIRE(inserted.dirty.items().size() == 1);
    CHECK(inserted.dirty.items()[0].item == ItemId{40});
    CHECK(inserted.dirty.items()[0].owner_track == ItemId{3});
    const auto* insert_inverse = std::get_if<RemoveModulationRoute>(&inserted.inverses[0]);
    REQUIRE(insert_inverse != nullptr);
    CHECK(insert_inverse->route_id == ItemId{40});

    // Every mutable member at once: the source it reads, the parameter it
    // reaches, its depth, and its bypass. A route has no performed field, so
    // one gate covering all four is the whole vocabulary a correction needs.
    const auto rewired_value =
        ModulationRoute{{40},
                        ModulationSourceRef{{11}, ModulationSourceKind::Macro},
                        DeviceParameterTarget{{8}, 7},
                        -0.25f,
                        false};
    auto rewired = take(reduce_transaction(
        inserted.project,
        one(SetModulationRoute{{2},
                               {3},
                               {40},
                               mixer_route({40}, {10}, ModulationSourceKind::Modulator, 0.5f),
                               rewired_value})));
    const auto* changed = routed_track(rewired.project).find_modulation_route({40});
    REQUIRE(changed != nullptr);
    CHECK(*changed == rewired_value);

    // The inverse is the same command with the two sides swapped, so undo is
    // exact rather than reconstructed.
    const auto* set_inverse = std::get_if<SetModulationRoute>(&rewired.inverses[0]);
    REQUIRE(set_inverse != nullptr);
    auto undone = take(reduce_transaction(rewired.project, one(*set_inverse)));
    CHECK(*routed_track(undone.project).find_modulation_route({40}) ==
          mixer_route({40}, {10}, ModulationSourceKind::Modulator, 0.5f));

    auto removed =
        take(reduce_transaction(rewired.project, one(RemoveModulationRoute{{2}, {3}, {40}})));
    CHECK(routed_track(removed.project).modulation_routes().empty());
    // Removing a route leaves both sources standing: nothing reads a route, so
    // this is the one modulation removal no other member can refuse.
    CHECK(routed_track(removed.project).modulators().size() == 1);
    CHECK(routed_track(removed.project).macros().size() == 1);
    // The inverse carries the connection as it stood -- bypass included, so a
    // route the author had silenced does not come back live.
    const auto* remove_inverse = std::get_if<InsertModulationRoute>(&removed.inverses[0]);
    REQUIRE(remove_inverse != nullptr);
    CHECK(remove_inverse->route == rewired_value);
    CHECK_FALSE(remove_inverse->route.enabled);
}

TEST_CASE("a route command round trips a document back to the bytes it started from",
          "[timeline][modulation-route-command][persistence]") {
    const auto registry = builtins();
    const auto live = mixer_route({20}, {10}, ModulationSourceKind::Modulator, 0.25f, false);
    const auto project = wired_project({live});
    // The sequence envelope rather than the whole snapshot, because a project
    // also carries next_item_id, and identity allocation is monotonic by design:
    // an insert consumes an identity that an undo does not hand back. Everything
    // a route command authors lives inside this envelope.
    const auto before = sequence_envelope(snapshot_of(project, registry));

    const std::vector<Command> forward{
        InsertModulationRoute{{2}, {3}, mixer_route({41}, {11}, ModulationSourceKind::Macro, 1.0f)},
        SetModulationRoute{{2},
                           {3},
                           {20},
                           live,
                           mixer_route({20}, {11}, ModulationSourceKind::Macro, -1.0f, true)},
    };
    for (const auto& command : forward) {
        auto applied = reduce_transaction(project, one(command));
        REQUIRE(applied.has_value());
        const auto changed = sequence_envelope(snapshot_of(applied->project, registry));
        // Control: the forward command actually moved the document, so the
        // equality below is a restoration rather than a command that did
        // nothing in both directions.
        CHECK(changed != before);
        auto undone = reduce_transaction(applied->project, one(applied->inverses[0]));
        REQUIRE(undone.has_value());
        CHECK(sequence_envelope(snapshot_of(undone->project, registry)) == before);
    }
    // The third command is a removal, whose inverse reinstates a tombstoned
    // identity and is therefore reachable only through a session. It is driven
    // whole in "undo through a session restores a removed route with its bypass
    // intact", which asserts every field including the bypass.
}

TEST_CASE("a stale route expectation is refused and the document is unchanged",
          "[timeline][modulation-route-command]") {
    const auto live = mixer_route({20}, {10}, ModulationSourceKind::Modulator, 0.5f, true);
    const auto project = wired_project({live});

    // Each of the four members differs in turn, so the gate is proved to cover
    // the whole route rather than the depth someone expected it to check.
    const std::vector<ModulationRoute> stale{
        mixer_route({20}, {11}, ModulationSourceKind::Macro, 0.5f, true),
        mixer_route({20}, {10}, ModulationSourceKind::Modulator, 0.25f, true),
        mixer_route({20}, {10}, ModulationSourceKind::Modulator, 0.5f, false),
        mixer_route({20}, {10}, ModulationSourceKind::Modulator, 0.5f, true,
                    TrackMixerParameter::Pan),
    };
    for (const auto& expectation : stale) {
        auto refused = reduce_transaction(
            project, one(SetModulationRoute{
                         {2},
                         {3},
                         {20},
                         expectation,
                         mixer_route({20}, {10}, ModulationSourceKind::Modulator, 1.0f)}));
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ConflictCode::ExpectedValueMismatch);
    }

    // Control: the same command with the expectation the document holds is
    // admitted, so the four refusals are the gate rather than a bad address.
    CHECK(reduce_transaction(project,
                             one(SetModulationRoute{
                                 {2},
                                 {3},
                                 {20},
                                 live,
                                 mixer_route({20}, {10}, ModulationSourceKind::Modulator, 1.0f)}))
              .has_value());

    // Nothing was written by any refusal: the reducer returns before it edits.
    CHECK(*routed_track(project).find_modulation_route({20}) == live);
}

TEST_CASE("a route edit may not move identity in either of the two ways it could",
          "[timeline][modulation-route-command]") {
    const auto live = mixer_route({20}, {10}, ModulationSourceKind::Modulator);
    const auto project = wired_project({live});

    // expected.id != replacement.id -- a removal and a creation wearing a
    // signature that declares neither.
    auto swap = reduce_transaction(
        project,
        one(SetModulationRoute{
            {2}, {3}, {20}, live, mixer_route({44}, {10}, ModulationSourceKind::Modulator, 1.0f)}));
    REQUIRE_FALSE(swap);
    CHECK(swap.error().code == ConflictCode::ModelInvariant);

    // expected.id != route_id -- the command names one route and gates on
    // another.
    auto mismatch = reduce_transaction(
        project,
        one(SetModulationRoute{
            {2}, {3}, {44}, live, mixer_route({20}, {10}, ModulationSourceKind::Modulator, 1.0f)}));
    REQUIRE_FALSE(mismatch);
    CHECK(mismatch.error().code == ConflictCode::ModelInvariant);

    // Control: the identical command with identity held still is admitted.
    CHECK(reduce_transaction(project,
                             one(SetModulationRoute{
                                 {2},
                                 {3},
                                 {20},
                                 live,
                                 mixer_route({20}, {10}, ModulationSourceKind::Modulator, 1.0f)}))
              .has_value());
}

TEST_CASE("a route command addressed at the wrong kind of item or the wrong track is refused",
          "[timeline][modulation-route-command]") {
    const auto live = mixer_route({20}, {10}, ModulationSourceKind::Modulator);
    const auto project = wired_project({live});

    // A route command naming a modulator's identity. Both identities are live
    // and owned by the same track, so the only thing separating them is the
    // kind the location carries.
    auto at_modulator = reduce_transaction(project, one(RemoveModulationRoute{{2}, {3}, {10}}));
    REQUIRE_FALSE(at_modulator);
    CHECK(at_modulator.error().code == ConflictCode::WrongTargetKind);

    // And the reverse, so the separation is not one-directional.
    auto modulator_at_route = reduce_transaction(project, one(RemoveModulator{{2}, {3}, {20}}));
    REQUIRE_FALSE(modulator_at_route);
    CHECK(modulator_at_route.error().code == ConflictCode::WrongTargetKind);

    auto wrong_track = reduce_transaction(project, one(RemoveModulationRoute{{2}, {6}, {20}}));
    REQUIRE_FALSE(wrong_track);
    CHECK(wrong_track.error().code == ConflictCode::ParentMismatch);

    // Control: the route command against its own kind on its own track is
    // admitted, and so is the modulator command once no route reads that
    // source -- so the three refusals above are the address and not the command.
    CHECK(reduce_transaction(project, one(RemoveModulationRoute{{2}, {3}, {20}})).has_value());
    CHECK(reduce_transaction(wired_project(), one(RemoveModulator{{2}, {3}, {10}})).has_value());
}

TEST_CASE("a route naming an absent source or the wrong source kind is refused both ways in",
          "[timeline][modulation-route-command]") {
    const auto live = mixer_route({20}, {10}, ModulationSourceKind::Modulator);
    const auto project = wired_project({live});

    // The track holds modulator 10 and macro 11 and nothing else.
    const auto absent = mixer_route({41}, {99}, ModulationSourceKind::Modulator);
    // Identity 11 is live, but it is a macro -- and the route says modulator.
    // ModulationSourceRef carries the kind explicitly for exactly this case, so
    // a macro can never quietly stand in for a modulator that shared its ID.
    const auto wrong_kind = mixer_route({41}, {11}, ModulationSourceKind::Modulator);

    CHECK_FALSE(reduce_transaction(project, one(InsertModulationRoute{{2}, {3}, absent})));
    CHECK_FALSE(reduce_transaction(project, one(InsertModulationRoute{{2}, {3}, wrong_kind})));

    for (const auto& replacement : {absent, wrong_kind}) {
        auto edited = replacement;
        edited.id = {20};
        auto refused =
            reduce_transaction(project, one(SetModulationRoute{{2}, {3}, {20}, live, edited}));
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ConflictCode::ModelInvariant);
    }

    // Control: the same two routes with the source and kind the track actually
    // holds are admitted on both paths, so the refusals are the reference and
    // not the shape of the command.
    const auto sound = mixer_route({41}, {11}, ModulationSourceKind::Macro);
    CHECK(reduce_transaction(project, one(InsertModulationRoute{{2}, {3}, sound})).has_value());
    auto sound_edit = sound;
    sound_edit.id = {20};
    CHECK(reduce_transaction(project, one(SetModulationRoute{{2}, {3}, {20}, live, sound_edit}))
              .has_value());
}

TEST_CASE("a route depth outside the declared maximum is refused on both paths",
          "[timeline][modulation-route-command]") {
    const auto live = mixer_route({20}, {10}, ModulationSourceKind::Modulator);
    const auto project = wired_project({live});
    const auto over = kMaximumModulationDepth + 0.5f;
    // Every inserted route here reaches Pan while the seeded one reaches Gain.
    // A second route from the same source to the same parameter is its own
    // refusal, and it would make the control below fail for a reason that has
    // nothing to do with depth.

    CHECK_FALSE(reduce_transaction(
        project, one(InsertModulationRoute{{2},
                                           {3},
                                           mixer_route({41}, {10}, ModulationSourceKind::Modulator,
                                                       over, true, TrackMixerParameter::Pan)})));
    CHECK_FALSE(reduce_transaction(
        project, one(InsertModulationRoute{{2},
                                           {3},
                                           mixer_route({41}, {10}, ModulationSourceKind::Modulator,
                                                       -over, true, TrackMixerParameter::Pan)})));
    CHECK_FALSE(reduce_transaction(
        project,
        one(SetModulationRoute{{2},
                               {3},
                               {20},
                               live,
                               mixer_route({20}, {10}, ModulationSourceKind::Modulator, over)})));

    // Control: the endpoints of the declared range are admitted, so the three
    // refusals are the range and not a rule that refuses every depth.
    CHECK(reduce_transaction(
              project, one(InsertModulationRoute{
                           {2},
                           {3},
                           mixer_route({41}, {10}, ModulationSourceKind::Modulator,
                                       kMaximumModulationDepth, true, TrackMixerParameter::Pan)}))
              .has_value());
    CHECK(reduce_transaction(project, one(SetModulationRoute{
                                          {2},
                                          {3},
                                          {20},
                                          live,
                                          mixer_route({20}, {10}, ModulationSourceKind::Modulator,
                                                      -kMaximumModulationDepth)}))
              .has_value());
}

TEST_CASE("route commands round trip through their schema envelopes",
          "[timeline][modulation-route-command][persistence]") {
    const auto registry = builtins();
    const auto decode_one = [&](std::string json) {
        auto decoded = deserialize_commands("[" + std::move(json) + "]", registry);
        REQUIRE(decoded.has_value());
        return std::move(decoded).value()[0];
    };

    const auto insert = decode_one(
        command_envelope("pulp.timeline.command.insert_modulation_route",
                         R"({"route":)" + route_envelope("40", "10", "modulator", 0.25f, false) +
                             R"(,"sequence_id":"2","track_id":"3"})"));
    const auto* inserted = std::get_if<InsertModulationRoute>(&insert);
    REQUIRE(inserted != nullptr);
    CHECK(inserted->route.source.kind == ModulationSourceKind::Modulator);
    CHECK(inserted->route.depth == 0.25f);
    // The bypass survives the wire as authored, which is what makes the inverse
    // of a removal restore a silenced route silenced.
    CHECK_FALSE(inserted->route.enabled);

    const auto remove =
        decode_one(command_envelope("pulp.timeline.command.remove_modulation_route",
                                    R"({"route_id":"40","sequence_id":"2","track_id":"3"})"));
    REQUIRE(std::holds_alternative<RemoveModulationRoute>(remove));

    const auto set = decode_one(command_envelope(
        "pulp.timeline.command.set_modulation_route",
        R"({"expected":)" + route_envelope("40", "10", "modulator", 0.25f, false) +
            R"(,"replacement":)" + route_envelope("40", "11", "macro", -1.0f, true) +
            R"(,"route_id":"40","sequence_id":"2","track_id":"3"})"));
    const auto* edited = std::get_if<SetModulationRoute>(&set);
    REQUIRE(edited != nullptr);
    CHECK(edited->expected.source.kind == ModulationSourceKind::Modulator);
    CHECK(edited->replacement.source.kind == ModulationSourceKind::Macro);
    CHECK(edited->replacement.depth == -1.0f);
    CHECK(edited->replacement.enabled);

    // An unknown source-kind spelling fails closed rather than defaulting to
    // one of the two the enum declares.
    CHECK_FALSE(deserialize_commands(
        "[" +
            command_envelope("pulp.timeline.command.insert_modulation_route",
                             R"({"route":)" + route_envelope("40", "10", "envelope", 0.25f, true) +
                                 R"(,"sequence_id":"2","track_id":"3"})") +
            "]",
        registry));
}

TEST_CASE("a decoded route depth satisfies the gate the encoder wrote it for",
          "[timeline][modulation-route-command][persistence]") {
    const auto registry = builtins();
    // 0.1f has no exact decimal spelling at float width, so this is the value a
    // decimal wire format would lose. The document holds it, the command is
    // decoded from the wire, and the gate compares the two exactly.
    const auto live = mixer_route({20}, {10}, ModulationSourceKind::Modulator, 0.1f);
    const auto project = wired_project({live});
    const auto edit = [&](float gate) {
        return "[" +
               command_envelope(
                   "pulp.timeline.command.set_modulation_route",
                   R"({"expected":)" + route_envelope("20", "10", "modulator", gate, true) +
                       R"(,"replacement":)" + route_envelope("20", "10", "modulator", 0.2f, true) +
                       R"(,"route_id":"20","sequence_id":"2","track_id":"3"})") +
               "]";
    };

    auto decoded = deserialize_commands(edit(0.1f), registry);
    REQUIRE(decoded.has_value());
    auto applied = reduce_transaction(project, one(decoded.value()[0]));
    REQUIRE(applied.has_value());
    CHECK(routed_track(applied->project).find_modulation_route({20})->depth == 0.2f);

    // Negative control: the neighbouring bit pattern is the nearest float to
    // the same decimal, and it is refused -- so the acceptance above is the
    // exact value surviving and not a gate that accepts anything close.
    const auto neighbour =
        std::bit_cast<float>(std::bit_cast<std::uint32_t>(0.1f) + std::uint32_t{1});
    CHECK(neighbour != 0.1f);
    auto near_miss = deserialize_commands(edit(neighbour), registry);
    REQUIRE(near_miss.has_value());
    auto refused = reduce_transaction(project, one(near_miss.value()[0]));
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ConflictCode::ExpectedValueMismatch);
}

TEST_CASE("decode refuses a route edit that moves identity",
          "[timeline][modulation-route-command][persistence]") {
    const auto registry = builtins();
    const auto edit = [&](std::string_view gate_id, std::string_view replacement_id,
                          std::string_view named_id) {
        return "[" +
               command_envelope("pulp.timeline.command.set_modulation_route",
                                R"({"expected":)" +
                                    route_envelope(gate_id, "10", "modulator", 0.25f, true) +
                                    R"(,"replacement":)" +
                                    route_envelope(replacement_id, "11", "macro", 0.5f, true) +
                                    R"(,"route_id":")" + std::string(named_id) +
                                    R"(","sequence_id":"2","track_id":"3"})") +
               "]";
    };
    // Control first: all three identities agreeing decodes, so each refusal
    // below is the disagreement and not the surrounding envelope.
    REQUIRE(deserialize_commands(edit("40", "40", "40"), registry).has_value());
    REQUIRE_FALSE(deserialize_commands(edit("40", "41", "40"), registry));
    REQUIRE_FALSE(deserialize_commands(edit("40", "40", "41"), registry));
}

TEST_CASE("undo through a session restores a removed route with its bypass intact",
          "[timeline][modulation-route-command]") {
    const auto silenced = mixer_route({20}, {10}, ModulationSourceKind::Modulator, -0.75f, false);
    auto session = std::move(DocumentSession::create(wired_project({silenced}))).value();
    auto writer = std::move(session->register_writer()).value();

    auto committed = session->submit(
        writer,
        timeline_test::session_transaction(writer, {}, {RemoveModulationRoute{{2}, {3}, {20}}}));
    REQUIRE(committed);
    CHECK(routed_track(*session->snapshot()).modulation_routes().empty());
    // Removal tombstones the identity rather than freeing it, which is why the
    // restoring inverse is reachable only through the session: the public
    // reduce_transaction refuses to insert an identity below next_item_id.
    CHECK_FALSE(session->snapshot()->locate({20})->active);
    auto public_restore =
        reduce_transaction(*session->snapshot(), one(InsertModulationRoute{{2}, {3}, silenced}));
    REQUIRE_FALSE(public_restore);
    CHECK(public_restore.error().code == ConflictCode::IdentityNotAvailable);

    REQUIRE(session->undo(writer));
    const auto* restored = routed_track(*session->snapshot()).find_modulation_route({20});
    REQUIRE(restored != nullptr);
    CHECK(*restored == silenced);
}

TEST_CASE("a writer denied removal may author and rewire a route",
          "[timeline][modulation-route-command][capabilities]") {
    constexpr auto insert_route = command_authority_of<InsertModulationRoute>();
    constexpr auto remove_route = command_authority_of<RemoveModulationRoute>();
    constexpr auto set_route = command_authority_of<SetModulationRoute>();
    STATIC_REQUIRE(insert_route.command_class == CommandClass::Automation);
    STATIC_REQUIRE(insert_route.intent == CommandIntent::Create);
    STATIC_REQUIRE(remove_route.intent == CommandIntent::Remove);
    STATIC_REQUIRE(set_route.intent == CommandIntent::Modify);
    STATIC_REQUIRE(insert_route == command_authority_of<InsertModulator>());

    // No new class: adding one renumbers capability_bit and silently repurposes
    // every persisted and transmitted writer mask, so this is asserted rather
    // than assumed.
    STATIC_REQUIRE(kCommandClassCount == 11);
    STATIC_REQUIRE(capability_bit({CommandClass::Automation, CommandIntent::Create}) ==
                   std::uint64_t{1} << 6);
    STATIC_REQUIRE(capability_bit({CommandClass::Automation, CommandIntent::Modify}) ==
                   std::uint64_t{1} << 7);
    STATIC_REQUIRE(capability_bit({CommandClass::Automation, CommandIntent::Remove}) ==
                   std::uint64_t{1} << 8);

    const auto proposal = non_destructive_capabilities();
    CHECK(allows(proposal, insert_route));
    CHECK(allows(proposal, set_route));
    CHECK_FALSE(allows(proposal, remove_route));
    // Control: an unrestricted mask admits the removal, so the refusal above is
    // the destructive axis rather than a mask that refuses everything.
    CHECK(allows(unrestricted_capabilities(), remove_route));

    // Driven rather than reasoned about: the proposal profile authors a source,
    // a macro, and a route, then changes every mutable member of the route,
    // holding only Create and Modify.
    auto session = std::move(DocumentSession::create(wired_project())).value();
    auto writer = std::move(session->register_writer(proposal)).value();
    REQUIRE(session->submit(
        writer,
        timeline_test::session_transaction(
            writer, {},
            {InsertModulationRoute{
                {2}, {3}, mixer_route({40}, {10}, ModulationSourceKind::Modulator, 0.5f)}})));
    REQUIRE(session->submit(
        writer, timeline_test::session_transaction(
                    writer, session->revision(),
                    {SetModulationRoute{
                        {2},
                        {3},
                        {40},
                        mixer_route({40}, {10}, ModulationSourceKind::Modulator, 0.5f),
                        ModulationRoute{{40},
                                        ModulationSourceRef{{11}, ModulationSourceKind::Macro},
                                        DeviceParameterTarget{{8}, 3},
                                        -1.0f,
                                        false}}})));
    const auto* rewired = routed_track(*session->snapshot()).find_modulation_route({40});
    REQUIRE(rewired != nullptr);
    CHECK(rewired->source.kind == ModulationSourceKind::Macro);
    CHECK(rewired->depth == -1.0f);
    CHECK_FALSE(rewired->enabled);
    CHECK(std::holds_alternative<DeviceParameterTarget>(rewired->target));

    auto denied = session->submit(
        writer, timeline_test::session_transaction(writer, session->revision(),
                                                   {RemoveModulationRoute{{2}, {3}, {40}}}));
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == ConflictCode::CapabilityDenied);
}

TEST_CASE("route command identity and retained size answer for their own payloads",
          "[timeline][modulation-route-command]") {
    const auto live = mixer_route({40}, {10}, ModulationSourceKind::Modulator, 0.5f);
    CHECK(equivalent(Command{InsertModulationRoute{{2}, {3}, live}},
                     Command{InsertModulationRoute{{2}, {3}, live}}));
    // A bypass difference is a different authored command, so idempotency does
    // not collapse a silencing edit onto the edit that preceded it.
    CHECK_FALSE(equivalent(
        Command{InsertModulationRoute{{2}, {3}, live}},
        Command{InsertModulationRoute{
            {2}, {3}, mixer_route({40}, {10}, ModulationSourceKind::Modulator, 0.5f, false)}}));
    CHECK_FALSE(
        equivalent(Command{InsertModulationRoute{{2}, {3}, live}},
                   Command{InsertModulationRoute{
                       {2}, {3}, mixer_route({40}, {11}, ModulationSourceKind::Macro, 0.5f)}}));
    CHECK(equivalent(Command{RemoveModulationRoute{{2}, {3}, {40}}},
                     Command{RemoveModulationRoute{{2}, {3}, {40}}}));
    CHECK_FALSE(equivalent(Command{RemoveModulationRoute{{2}, {3}, {40}}},
                           Command{RemoveModulationRoute{{2}, {3}, {41}}}));
    CHECK(equivalent(Command{SetModulationRoute{{2}, {3}, {40}, live, live}},
                     Command{SetModulationRoute{{2}, {3}, {40}, live, live}}));

    // A route owns no heap storage, so its retained size is its struct size and
    // nothing else. Measured against a command that does own storage, so the
    // claim is not "retained_size returns a constant".
    CHECK(retained_size(Command{InsertModulationRoute{{2}, {3}, live}}) ==
          sizeof(InsertModulationRoute));
    CHECK(retained_size(Command{RemoveModulationRoute{{2}, {3}, {40}}}) ==
          sizeof(RemoveModulationRoute));
    CHECK(retained_size(Command{SetModulationRoute{{2}, {3}, {40}, live, live}}) ==
          sizeof(SetModulationRoute));
    CHECK(retained_size(Command{InsertModulator{{2}, {3}, lfo({40}, std::string(32, 'a'))}}) >
          sizeof(InsertModulator));
}
