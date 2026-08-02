#include "support/timeline_schema_version.hpp"
#include "timeline_command_test_helpers.hpp"

#include <pulp/interchange/census.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace timeline_test;
using timeline_test_support::current_schema_version;
using timeline_test_support::track_version_stamp;

namespace {

std::uint32_t current_track_version() {
    return current_schema_version("pulp.timeline.track");
}

template <typename T, typename E> T take_result(pulp::runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

// One placed device for routes and lanes to address. The chain is identity-only
// today, which is all a parameter target references.
constexpr ItemId kDevice{20};
constexpr ItemId kOtherDevice{21};

ModulationRoute device_route(ItemId id, ItemId source, ModulationSourceKind kind,
                             std::uint32_t param, float depth, ItemId placement = kDevice) {
    return ModulationRoute{id, {source, kind}, DeviceParameterTarget{placement, param}, depth,
                           true};
}

TrackInput base_track(std::vector<Modulator> modulators, std::vector<MacroControl> macros,
                      std::vector<ModulationRoute> routes,
                      std::vector<AutomationLane> lanes = {}) {
    return TrackInput{.id = {4},
                      .name = "track",
                      .clips = {make_note_clip({5}, {6}, 0)},
                      .device_chain = {DevicePlacement{kDevice}, DevicePlacement{kOtherDevice}},
                      .automation_lanes = std::move(lanes),
                      .modulators = std::move(modulators),
                      .macros = std::move(macros),
                      .modulation_routes = std::move(routes)};
}

pulp::runtime::Result<Track, ModelError> make_track(TrackInput input) {
    return Track::create(std::move(input));
}

Project project_of(Track track) {
    auto sequence = take_result(
        Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter}, {std::move(track)}));
    return take_result(Project::create({{1}, "project", 100, {3}, {}, {std::move(sequence)}}));
}

const Track& track_of(const Project& value) {
    return *value.find_sequence({3})->find_track({4});
}

SchemaRegistry registry() {
    return take_result(make_builtin_timeline_registry());
}

std::string serialized(const Project& project) {
    return take_result(serialize_project(project, registry())).json;
}

AutomationLane device_lane(ItemId lane_id, ItemId point_id, std::uint32_t param, float first,
                           float second) {
    auto curve = AutomationCurve::create(
        {AutomationPoint{point_id, {0}, first, AutomationInterpolation::Continuous, 0.0f},
         AutomationPoint{{point_id.value + 1}, {kTicksPerQuarter}, second,
                         AutomationInterpolation::Continuous, 0.0f}});
    REQUIRE(curve);
    return take_result(AutomationLane::create(lane_id, DeviceParameterTarget{kDevice, param},
                                              std::move(curve).value()));
}

// The track envelope inside a serialized project, isolated so a migration can be
// driven against exactly what the encoder writes rather than a hand-typed shape.
std::string track_envelope(const std::string& document) {
    const auto tracks = document.find("\"tracks\":[");
    REQUIRE(tracks != std::string::npos);
    const auto begin = tracks + std::string("\"tracks\":[").size();
    std::size_t depth = 0;
    for (auto index = begin; index < document.size(); ++index) {
        if (document[index] == '{')
            ++depth;
        else if (document[index] == '}' && --depth == 0)
            return document.substr(begin, index + 1 - begin);
    }
    FAIL("no track envelope found");
    return {};
}

} // namespace

TEST_CASE("Modulation route round trips distinct from automation lanes",
          "[timeline][modulation]") {
    // One device parameter carries both an automation lane, which authors its
    // base value, and a modulation route, which offsets whatever base is in
    // force. A document that collapsed the two would lose one of them here.
    const auto authored = project_of(take_result(make_track(base_track(
        {Modulator{{30}, ModulatorKind::Lfo, "wobble"}}, {MacroControl{{31}, "brightness", 0.75f}},
        {device_route({40}, {30}, ModulationSourceKind::Modulator, 7, -0.5f),
         device_route({41}, {31}, ModulationSourceKind::Macro, 7, 0.25f),
         ModulationRoute{{42},
                         {{31}, ModulationSourceKind::Macro},
                         TrackMixerTarget{TrackMixerParameter::Pan},
                         1.0f,
                         false}},
        {device_lane({50}, {51}, 7, 0.25f, 0.75f)}))));

    const auto document = serialized(authored);
    REQUIRE(document.find("pulp.timeline.modulation_route") != std::string::npos);
    REQUIRE(document.find("pulp.timeline.modulator") != std::string::npos);
    REQUIRE(document.find("pulp.timeline.macro_control") != std::string::npos);
    // The envelope the track declares, qualified by its type name so this never
    // silently watches whichever schema happens to sit at that number.
    REQUIRE(document.find(track_version_stamp()) != std::string::npos);

    const auto restored = take_result(deserialize_project(document, registry()));
    const auto& track = track_of(restored);

    REQUIRE(track.modulators().size() == 1);
    REQUIRE(track.modulators()[0] == Modulator{{30}, ModulatorKind::Lfo, "wobble"});
    REQUIRE(track.macros().size() == 1);
    REQUIRE(track.macros()[0] == MacroControl{{31}, "brightness", 0.75f});

    REQUIRE(track.modulation_routes().size() == 3);
    const auto* modulator_route = track.find_modulation_route({40});
    REQUIRE(modulator_route != nullptr);
    REQUIRE(modulator_route->source == ModulationSourceRef{{30}, ModulationSourceKind::Modulator});
    REQUIRE(modulator_route->depth == -0.5f);
    REQUIRE(modulator_route->enabled);
    REQUIRE(std::get<DeviceParameterTarget>(modulator_route->target) ==
            DeviceParameterTarget{kDevice, 7});

    // The disabled route keeps identity, depth, and target, so re-enabling it
    // restores exactly what was authored rather than a default.
    const auto* mixer_route = track.find_modulation_route({42});
    REQUIRE(mixer_route != nullptr);
    REQUIRE_FALSE(mixer_route->enabled);
    REQUIRE(mixer_route->depth == 1.0f);
    REQUIRE(std::get<TrackMixerTarget>(mixer_route->target) ==
            TrackMixerTarget{TrackMixerParameter::Pan});

    // The lane on the same parameter is untouched by any of it.
    REQUIRE(track.automation_lanes().size() == 1);
    const auto* lane = track.find_automation_lane({50});
    REQUIRE(lane != nullptr);
    REQUIRE(std::get<DeviceParameterTarget>(lane->target()) == DeviceParameterTarget{kDevice, 7});
    REQUIRE(lane->curve().points().size() == 2);

    // Re-saving must reproduce the same bytes, or the document is not a fixed
    // point and every later comparison is against a moving target.
    REQUIRE(serialized(restored) == document);
}

TEST_CASE("A track authoring no modulation writes no modulation members",
          "[timeline][modulation]") {
    // Absence is how the canonical form spells the empty collection. A track
    // that never touched modulation must not grow three empty arrays.
    const auto document = serialized(project_of(take_result(make_track(base_track({}, {}, {})))));
    REQUIRE(document.find("\"modulators\":") == std::string::npos);
    REQUIRE(document.find("\"macros\":") == std::string::npos);
    REQUIRE(document.find("\"modulation_routes\":") == std::string::npos);

    const auto restored = take_result(deserialize_project(document, registry()));
    REQUIRE(track_of(restored).modulators().empty());
    REQUIRE(track_of(restored).macros().empty());
    REQUIRE(track_of(restored).modulation_routes().empty());
    REQUIRE(serialized(restored) == document);
}

TEST_CASE("Macro control fans out to device parameters in census", "[timeline][modulation]") {
    // One macro reaching three device parameters is three routes against one
    // macro. An export that can bind a control to a single destination loses
    // two of them, and only the ratio says so.
    const auto project = project_of(take_result(make_track(
        base_track({Modulator{{30}, ModulatorKind::Envelope, "env"}},
                   {MacroControl{{31}, "brightness", 0.5f}},
                   {device_route({40}, {31}, ModulationSourceKind::Macro, 1, 1.0f),
                    device_route({41}, {31}, ModulationSourceKind::Macro, 2, 0.5f),
                    device_route({42}, {31}, ModulationSourceKind::Macro, 3, 0.25f, kOtherDevice),
                    ModulationRoute{{43},
                                    {{30}, ModulationSourceKind::Modulator},
                                    TrackMixerTarget{TrackMixerParameter::Gain},
                                    0.1f,
                                    true}}))));

    const auto census = pulp::interchange::census(project);
    REQUIRE(census.count(pulp::interchange::Concept::ModulationMacro) == 1);
    REQUIRE(census.count(pulp::interchange::Concept::ModulationModulator) == 1);
    REQUIRE(census.count(pulp::interchange::Concept::ModulationRouteDeviceParam) == 3);
    REQUIRE(census.count(pulp::interchange::Concept::ModulationRouteTrackMixer) == 1);

    // The route concepts are separate atoms from the automation ones. A census
    // that reported modulation as automation would let an export claim it
    // carried the routes because it carries lanes.
    REQUIRE_FALSE(census.contains(pulp::interchange::Concept::AutomationDeviceParam));
    REQUIRE_FALSE(census.contains(pulp::interchange::Concept::AutomationTrackGain));

    const auto owners = census.owners(pulp::interchange::Concept::ModulationRouteDeviceParam);
    REQUIRE(owners.size() == 3);
}

TEST_CASE("A modulation route must name a source the track owns", "[timeline][modulation]") {
    SECTION("no such identity") {
        const auto failed = make_track(base_track(
            {Modulator{{30}, ModulatorKind::Lfo, "wobble"}}, {},
            {device_route({40}, {99}, ModulationSourceKind::Modulator, 1, 0.5f)}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::MissingModulationSource);
        REQUIRE(failed.error().related_item == ItemId{99});
    }
    SECTION("right identity, wrong kind") {
        // The macro exists and holds the named ID. A route declaring a
        // modulator must not be satisfied by it.
        const auto failed = make_track(
            base_track({}, {MacroControl{{31}, "brightness", 0.5f}},
                       {device_route({40}, {31}, ModulationSourceKind::Modulator, 1, 0.5f)}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::MissingModulationSource);
    }
    SECTION("matching kind is accepted") {
        REQUIRE(make_track(base_track({}, {MacroControl{{31}, "brightness", 0.5f}},
                                      {device_route({40}, {31}, ModulationSourceKind::Macro, 1,
                                                    0.5f)})));
    }
}

TEST_CASE("A modulation route must name a device placement the track holds",
          "[timeline][modulation]") {
    const auto failed = make_track(
        base_track({Modulator{{30}, ModulatorKind::Lfo, "wobble"}}, {},
                   {device_route({40}, {30}, ModulationSourceKind::Modulator, 1, 0.5f, {77})}));
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().code == ModelErrorCode::MissingModulationTarget);
    REQUIRE(failed.error().related_item == ItemId{77});
}

TEST_CASE("Modulation depth and macro value are bounded", "[timeline][modulation]") {
    const auto modulator = Modulator{{30}, ModulatorKind::Lfo, "wobble"};
    SECTION("depth above the maximum") {
        const auto failed = make_track(base_track(
            {modulator}, {}, {device_route({40}, {30}, ModulationSourceKind::Modulator, 1, 1.5f)}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::InvalidModulationRoute);
    }
    SECTION("depth below the minimum") {
        const auto failed = make_track(
            base_track({modulator}, {},
                       {device_route({40}, {30}, ModulationSourceKind::Modulator, 1, -1.5f)}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::InvalidModulationRoute);
    }
    SECTION("NaN depth") {
        const auto failed = make_track(
            base_track({modulator}, {},
                       {device_route({40}, {30}, ModulationSourceKind::Modulator, 1,
                                     std::numeric_limits<float>::quiet_NaN())}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::InvalidModulationRoute);
    }
    SECTION("the boundary values themselves are admitted") {
        REQUIRE(make_track(base_track(
            {modulator}, {},
            {device_route({40}, {30}, ModulationSourceKind::Modulator, 1,
                          -kMaximumModulationDepth),
             device_route({41}, {30}, ModulationSourceKind::Modulator, 2,
                          kMaximumModulationDepth)})));
    }
    SECTION("macro value out of range") {
        const auto failed = make_track(base_track({}, {MacroControl{{31}, "hot", 1.5f}}, {}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::InvalidMacroControl);
        REQUIRE(failed.error().item == ItemId{31});
    }
    SECTION("NaN macro value") {
        const auto failed = make_track(base_track(
            {}, {MacroControl{{31}, "hot", std::numeric_limits<float>::quiet_NaN()}}, {}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::InvalidMacroControl);
    }
}

TEST_CASE("Modulation summing is what distinguishes a route from a lane",
          "[timeline][modulation]") {
    const auto modulator = Modulator{{30}, ModulatorKind::Lfo, "wobble"};
    const auto macro = MacroControl{{31}, "brightness", 0.5f};
    SECTION("two sources may reach one parameter, because their offsets sum") {
        REQUIRE(make_track(base_track(
            {modulator}, {macro},
            {device_route({40}, {30}, ModulationSourceKind::Modulator, 7, 0.5f),
             device_route({41}, {31}, ModulationSourceKind::Macro, 7, -0.25f)})));
    }
    SECTION("two lanes may not, because a base value has no sum") {
        const auto failed = make_track(base_track(
            {}, {}, {}, {device_lane({50}, {51}, 7, 0.0f, 1.0f),
                         device_lane({60}, {61}, 7, 0.0f, 1.0f)}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::DuplicateAutomationTarget);
    }
    SECTION("one source may not reach one parameter twice") {
        // Two depths for one connection with no rule saying which wins.
        const auto failed = make_track(base_track(
            {modulator}, {},
            {device_route({40}, {30}, ModulationSourceKind::Modulator, 7, 0.5f),
             device_route({41}, {30}, ModulationSourceKind::Modulator, 7, 0.25f)}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::InvalidModulationRoute);
    }
}

TEST_CASE("Modulation identities are disjoint from every other track identity",
          "[timeline][modulation]") {
    SECTION("a route may not reuse a modulator's identity") {
        const auto failed = make_track(base_track(
            {Modulator{{30}, ModulatorKind::Lfo, "wobble"}}, {},
            {device_route({30}, {30}, ModulationSourceKind::Modulator, 1, 0.5f)}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::DuplicateItemId);
    }
    SECTION("a macro may not reuse a clip's identity") {
        const auto failed = make_track(base_track({}, {MacroControl{{5}, "brightness", 0.5f}}, {}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::DuplicateItemId);
    }
    SECTION("a modulator may not reuse an automation lane's identity") {
        const auto failed =
            make_track(base_track({Modulator{{50}, ModulatorKind::Lfo, "wobble"}}, {}, {},
                                  {device_lane({50}, {51}, 7, 0.0f, 1.0f)}));
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().code == ModelErrorCode::DuplicateItemId);
    }
}

TEST_CASE("routes_from_source reports one source's complete fan-out", "[timeline][modulation]") {
    const auto track = take_result(make_track(
        base_track({Modulator{{30}, ModulatorKind::Lfo, "wobble"}},
                   {MacroControl{{31}, "brightness", 0.5f}},
                   {device_route({40}, {31}, ModulationSourceKind::Macro, 1, 1.0f),
                    device_route({41}, {30}, ModulationSourceKind::Modulator, 2, 0.5f),
                    device_route({42}, {31}, ModulationSourceKind::Macro, 3, 0.25f)})));
    const auto macro_routes = track.routes_from_source({31});
    REQUIRE(macro_routes.size() == 2);
    REQUIRE(macro_routes[0].id == ItemId{40});
    REQUIRE(macro_routes[1].id == ItemId{42});
    REQUIRE(track.routes_from_source({30}).size() == 1);
    REQUIRE(track.routes_from_source({99}).empty());
    REQUIRE(track.routes_from_source({}).empty());
}

TEST_CASE("A v7 track upgrades to v8 and a modulating v8 track refuses to downgrade",
          "[timeline][modulation][migration]") {
    const auto schemas = registry();
    const auto plain =
        track_envelope(serialized(project_of(take_result(make_track(base_track({}, {}, {}))))));
    REQUIRE(plain.find(track_version_stamp()) != std::string::npos);
    const auto current = current_track_version();
    const auto previous = current - 1;

    // A track with no modulation says the same thing at v7 as at v8, because
    // absence is how v8 spells the empty collections. The pair is lossless.
    const auto downgraded = take_result(
        schemas.migrate(SchemaDomain::Document, "pulp.timeline.track", current, previous, plain, {}));
    REQUIRE(downgraded.find("\"version\":" + std::to_string(previous)) != std::string::npos);
    REQUIRE(take_result(schemas.migrate(SchemaDomain::Document, "pulp.timeline.track", previous,
                                        current, downgraded, {})) == plain);

    // A track that authored modulation has no v7 spelling. Dropping it would
    // silently unwire the document, so the downgrade refuses rather than losing
    // what drives the parameters.
    const auto modulating = track_envelope(serialized(project_of(take_result(make_track(
        base_track({Modulator{{30}, ModulatorKind::Lfo, "wobble"}}, {},
                   {device_route({40}, {30}, ModulationSourceKind::Modulator, 1, 0.5f)}))))));
    REQUIRE(modulating.find("\"modulators\":") != std::string::npos);
    REQUIRE_FALSE(
        schemas.migrate(SchemaDomain::Document, "pulp.timeline.track", current, previous, modulating, {}));

    SECTION("a macro alone also blocks the downgrade") {
        const auto with_macro = track_envelope(serialized(project_of(
            take_result(make_track(base_track({}, {MacroControl{{31}, "brightness", 0.5f}}, {}))))));
        REQUIRE_FALSE(
            schemas.migrate(SchemaDomain::Document, "pulp.timeline.track", current, previous, with_macro, {}));
    }
}

TEST_CASE("A pre-modulation document opens and reads as empty collections",
          "[timeline][modulation][migration]") {
    // The v8 encoder is the only writer, so a genuine v7 track envelope is
    // manufactured by downgrading one the encoder wrote and splicing it back.
    // Hand-typing the shape would produce a fixture that agrees with the parser
    // for whatever reason the parser happens to hold.
    const auto document =
        serialized(project_of(take_result(make_track(base_track({}, {}, {})))));
    const auto v8_track = track_envelope(document);
    const auto v7_track = take_result(
        registry().migrate(SchemaDomain::Document, "pulp.timeline.track", current_track_version(),
                           current_track_version() - 1, v8_track, {}));

    const auto at = document.find(v8_track);
    REQUIRE(at != std::string::npos);
    auto older = document;
    older.replace(at, v8_track.size(), v7_track);
    REQUIRE(older.find("\"type_name\":\"pulp.timeline.track\",\"version\":" +
                       std::to_string(current_track_version() - 1)) != std::string::npos);

    const auto restored = take_result(deserialize_project(older, registry()));
    REQUIRE(track_of(restored).modulators().empty());
    REQUIRE(track_of(restored).macros().empty());
    REQUIRE(track_of(restored).modulation_routes().empty());
}

TEST_CASE("A pre-modulation document may not carry modulation members",
          "[timeline][modulation][migration]") {
    // A v7 envelope that carries a v8 member is a document claiming a version
    // whose writer could not have produced it. Accepting it would make the
    // version stamp advisory. The envelope is built by restamping one the v8
    // encoder wrote, so the smuggled member lands exactly where a real one
    // would rather than wherever a hand-typed string search happened to point.
    const auto document = serialized(project_of(take_result(
        make_track(base_track({}, {MacroControl{{31}, "brightness", 0.5f}}, {})))));
    REQUIRE(document.find("\"macros\":[") != std::string::npos);

    const std::string stamp = track_version_stamp();
    const auto at = document.find(stamp);
    REQUIRE(at != std::string::npos);
    auto smuggled = document;
    smuggled.replace(at, stamp.size(),
                     "\"type_name\":\"pulp.timeline.track\",\"version\":" +
                         std::to_string(current_track_version() - 1));

    REQUIRE_FALSE(deserialize_project(smuggled, registry()));

    // The control: the same bytes with the stamp left alone are accepted, so
    // the refusal above is the version disagreement and not some other damage
    // the edit did to the document.
    REQUIRE(deserialize_project(document, registry()));
}

TEST_CASE("Remapping rewrites modulation identities and both of a route's references",
          "[timeline][modulation]") {
    // A route carries three identities: its own, its source's, and the device
    // placement its target names. A remap that rewrote only some of them would
    // leave a route pointing at an identity the copy does not contain, so the
    // rebuilt track is re-validated here through the same Track::create the
    // model uses.
    const auto original = take_result(make_track(
        base_track({Modulator{{30}, ModulatorKind::Random, "drift"}},
                   {MacroControl{{31}, "brightness", 0.5f}},
                   {device_route({40}, {30}, ModulationSourceKind::Modulator, 7, -0.75f),
                    device_route({41}, {31}, ModulationSourceKind::Macro, 8, 0.5f, kOtherDevice)})));

    ItemIdAllocator allocator(1000);
    const auto remapped = take_result(remap_ids(original, allocator));
    const auto& track = remapped.track;

    REQUIRE(track.modulators().size() == 1);
    REQUIRE(track.modulators()[0].id == *remapped.ids.find({30}));
    // Authored state rides across unchanged; only identities move.
    REQUIRE(track.modulators()[0].kind == ModulatorKind::Random);
    REQUIRE(track.modulators()[0].name == "drift");
    REQUIRE(track.macros().size() == 1);
    REQUIRE(track.macros()[0].id == *remapped.ids.find({31}));
    REQUIRE(track.macros()[0].value == 0.5f);

    REQUIRE(track.modulation_routes().size() == 2);
    const auto* from_modulator = track.find_modulation_route(*remapped.ids.find({40}));
    REQUIRE(from_modulator != nullptr);
    REQUIRE(from_modulator->source.id == *remapped.ids.find({30}));
    REQUIRE(from_modulator->source.kind == ModulationSourceKind::Modulator);
    REQUIRE(from_modulator->depth == -0.75f);
    REQUIRE(std::get<DeviceParameterTarget>(from_modulator->target) ==
            DeviceParameterTarget{*remapped.ids.find(kDevice), 7});

    const auto* from_macro = track.find_modulation_route(*remapped.ids.find({41}));
    REQUIRE(from_macro != nullptr);
    REQUIRE(from_macro->source.id == *remapped.ids.find({31}));
    REQUIRE(from_macro->source.kind == ModulationSourceKind::Macro);
    REQUIRE(std::get<DeviceParameterTarget>(from_macro->target) ==
            DeviceParameterTarget{*remapped.ids.find(kOtherDevice), 8});

    // None of the new identities may collide with the ones they replaced.
    for (const auto& route : track.modulation_routes())
        REQUIRE(track.find_modulation_route(route.id) != nullptr);

    // Every modulation identity is owned, so the project directory locates each
    // one under the track that holds it.
    const auto project = project_of(original);
    const auto remapped_project = take_result(remap_ids(project, 2000));
    const auto modulator_id = *remapped_project.ids.find({30});
    const auto route_id = *remapped_project.ids.find({40});
    REQUIRE(remapped_project.project.locate(modulator_id)->kind == ItemKind::Modulator);
    REQUIRE(remapped_project.project.locate(*remapped_project.ids.find({31}))->kind ==
            ItemKind::MacroControl);
    REQUIRE(remapped_project.project.locate(route_id)->kind == ItemKind::ModulationRoute);
    REQUIRE(remapped_project.project.locate(route_id)->parent_id ==
            *remapped_project.ids.find({4}));
}

TEST_CASE("A track carrying both a tuning and modulation reads both back",
          "[timeline][modulation][migration]") {
    // Tuning and the three modulation collections were added by two independent
    // slices that each APPENDED a member to the preflight's positionally-indexed
    // request array. Neither slice's own tests can catch the two being wired to
    // each other's span, because each exercises a track that carries only its
    // own member. This is the case that separates them: if the indices crossed,
    // the tuning walk would run over an array and refuse, or a modulation
    // collection would be scored against an object.
    auto input = base_track({Modulator{{30}, ModulatorKind::Lfo, "wobble"}},
                            {MacroControl{{31}, "brightness", 0.25f}},
                            {device_route({40}, {30}, ModulationSourceKind::Modulator, 3, 0.75f),
                             device_route({41}, {31}, ModulationSourceKind::Macro, 4, -0.5f)});
    input.tuning = TuningReference{TuningSystem::EqualTemperament, 432'000, std::nullopt,
                                   std::nullopt};
    const auto document = serialized(project_of(take_result(make_track(std::move(input)))));
    REQUIRE(document.find("\"tuning\":") != std::string::npos);
    REQUIRE(document.find("\"modulators\":") != std::string::npos);
    REQUIRE(document.find("\"macros\":") != std::string::npos);
    REQUIRE(document.find("\"modulation_routes\":") != std::string::npos);

    const auto restored = take_result(deserialize_project(document, registry()));
    const auto& track = track_of(restored);
    REQUIRE(track.tuning().has_value());
    REQUIRE(track.tuning()->reference_pitch_millihertz == 432'000);
    REQUIRE(track.modulators().size() == 1);
    REQUIRE(track.macros().size() == 1);
    REQUIRE(track.modulation_routes().size() == 2);
    REQUIRE(serialized(restored) == document);
}
