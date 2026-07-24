#include "support/timeline_persistence_test_support.hpp"

namespace {

std::string erase_parent_ids(std::string snapshot) {
    constexpr std::string_view prefix = ",\"parent_id\":\"";
    std::size_t search_from = 0;
    auto begin = snapshot.find(prefix, search_from);
    while (begin != std::string::npos) {
        const auto end = snapshot.find('"', begin + prefix.size());
        REQUIRE(end != std::string::npos);
        snapshot.erase(begin, end - begin + 1);
        search_from = begin;
        begin = snapshot.find(prefix, search_from);
    }
    return snapshot;
}

} // namespace

TEST_CASE("Timeline snapshots round trip canonically with durable asset identity") {
    const auto registry = builtins();
    const auto original = project_with();
    auto first = serialize_project(original, registry);
    REQUIRE(first.has_value());
    REQUIRE_FALSE(first.value().has_opaque_objects);
    REQUIRE(first.value().json.find("\"id\":\"1\"") != std::string::npos);
    REQUIRE(first.value().json.find(std::string(64, 'a')) != std::string::npos);
    REQUIRE(first.value().json.find("media/proxy.wav") != std::string::npos);

    auto decoded = deserialize_project(first.value().json, registry);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().assets()[0].locators.empty());
    REQUIRE(decoded.value().assets()[0].representations[0].role == "proxy");
    auto second = serialize_project(decoded.value(), registry);
    REQUIRE(second.has_value());
    REQUIRE(second.value().json == first.value().json);
}

TEST_CASE("Timeline project summary peeks without constructing the editable document") {
    const auto snapshot = take(serialize_project(mixed_project(), builtins())).json;
    auto escaped_name = snapshot;
    const auto name = escaped_name.find(R"("name":"mixed")");
    REQUIRE(name != std::string::npos);
    escaped_name.replace(name, std::string_view(R"("name":"mixed")").size(),
                         R"("name":"m\u00edxed")");

    const auto nodes_before = Project::identity_stats().nodes_created;
    const auto registry = builtins();
    const auto summary = take(peek_project_summary(escaped_name, registry));
    const auto nodes_after = Project::identity_stats().nodes_created;

    REQUIRE(summary.project_id == ItemId{1});
    REQUIRE(summary.name == "m\u00edxed");
    REQUIRE(summary.next_item_id == 12);
    REQUIRE(summary.root_sequence_id == ItemId{8});
    REQUIRE(summary.schema_version == 1);
    REQUIRE(summary.counts.assets == 1);
    REQUIRE(summary.counts.sequences == 2);
    REQUIRE(summary.counts.tracks == 2);
    REQUIRE(summary.counts.clips == 3);
    REQUIRE(summary.counts.notes == 2);
    REQUIRE(summary.counts.device_placements == 0);
    REQUIRE(summary.counts.automation_lanes == 0);
    REQUIRE(summary.counts.automation_points == 0);
    REQUIRE(summary.counts.take_lanes == 0);
    REQUIRE(summary.counts.takes == 0);
    REQUIRE(summary.counts.take_comp_segments == 0);
    REQUIRE(nodes_after == nodes_before);

    auto invalid_id = escaped_name;
    const auto id = invalid_id.find(R"("id":"1")");
    REQUIRE(id != std::string::npos);
    invalid_id.replace(id, std::string_view(R"("id":"1")").size(), R"("id":"01")");
    const auto rejected = peek_project_summary(invalid_id, registry);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == PersistenceErrorCode::InvalidNumber);
    REQUIRE(rejected.error().path == "/data/id");
    REQUIRE(rejected.error().byte_offset == id + std::string_view(R"("id":)").size());
}

TEST_CASE("Timeline project summary counts nested authored structures") {
    auto clip = take(Clip::create({7}, {0}, {100}, EmptyContent{}));
    auto curve = take(AutomationCurve::create(
        {{{10}, {0}, 0.25f}, {{11}, {50}, 0.75f}}));
    auto automation =
        take(AutomationLane::create({9}, DeviceParameterTarget{{8}, 42}, std::move(curve)));
    auto recording =
        take(Take::create({13}, MediaRef{{20}, {0}, 100}, {0}, RationalRate{48'000, 1}));
    auto take_lane = take(TakeLane::create(
        {12}, "recording", {recording},
        {{.take_id = {13}, .range = {{0}, 50, {48'000, 1}}}}));
    auto track = take(Track::create(TrackInput{
        .id = {6},
        .name = "authored",
        .clips = {clip},
        .device_chain = {{{8}}},
        .automation_lanes = {automation},
        .take_lanes = {take_lane},
    }));
    auto sequence = take(Sequence::create({5}, "root", TickDuration{100}, {track}));
    MediaAsset asset{{20},
                     "take.wav",
                     100,
                     {48'000, 1},
                     hash('d'),
                     AssetStoragePolicy::External,
                     {},
                     {}};
    auto project =
        take(Project::create(ProjectInput{{1}, "counts", 21, {5}, {asset}, {sequence}}));

    const auto registry = builtins();
    const auto summary =
        take(peek_project_summary(take(serialize_project(project, registry)).json, registry));
    REQUIRE(summary.counts.assets == 1);
    REQUIRE(summary.counts.sequences == 1);
    REQUIRE(summary.counts.tracks == 1);
    REQUIRE(summary.counts.clips == 1);
    REQUIRE(summary.counts.notes == 0);
    REQUIRE(summary.counts.device_placements == 1);
    REQUIRE(summary.counts.automation_lanes == 1);
    REQUIRE(summary.counts.automation_points == 2);
    REQUIRE(summary.counts.take_lanes == 1);
    REQUIRE(summary.counts.takes == 1);
    REQUIRE(summary.counts.take_comp_segments == 1);
}

TEST_CASE("Timeline identity persistence derives immediate parents from legacy snapshots") {
    const auto registry = builtins();
    const auto encoded = take(serialize_project(mixed_project(), registry));
    const auto legacy = erase_parent_ids(encoded.json);
    REQUIRE(legacy.find("\"parent_id\"") == std::string::npos);

    const auto decoded = take(deserialize_project(legacy, registry));
    REQUIRE(decoded.locate({1})->parent_id == ItemId{});
    REQUIRE(decoded.locate({2})->parent_id == ItemId{1});
    REQUIRE(decoded.locate({8})->parent_id == ItemId{1});
    REQUIRE(decoded.locate({3})->parent_id == ItemId{8});
    REQUIRE(decoded.locate({5})->parent_id == ItemId{3});
    REQUIRE(decoded.locate({6})->parent_id == ItemId{5});

    const auto canonical = take(serialize_project(decoded, registry));
    REQUIRE(canonical.json == encoded.json);
}

TEST_CASE("Timeline schema-v1 owns and canonically replays editable tempo and meter maps") {
    auto clip = take(Clip::create({4}, {0}, {100}, EmptyContent{}));
    auto track = take(Track::create({3}, "track", {clip}));
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, {track}));
    const std::array tempo_points{
        TempoPoint{{0}, 90.0, TempoCurve::LinearInTicks},
        TempoPoint{{4 * kTicksPerQuarter}, 135.0, TempoCurve::Constant},
    };
    const std::array meter_points{
        MeterPoint{{0}, {4, 4}},
        MeterPoint{{8 * kTicksPerQuarter}, {3, 4}},
    };
    auto project = take(Project::create(ProjectInput{{1},
                                                     "maps",
                                                     5,
                                                     {2},
                                                     {},
                                                     {sequence},
                                                     take(TempoMap::create(tempo_points)),
                                                     take(MeterMap::create(meter_points))}));
    const auto registry = builtins();
    const auto encoded = take(serialize_project(project, registry));
    REQUIRE(encoded.json.find("\"tempo_map\"") != std::string::npos);
    REQUIRE(encoded.json.find("\"meter_map\"") != std::string::npos);
    const auto decoded = take(deserialize_project(encoded.json, registry));
    REQUIRE(decoded.tempo_map() == project.tempo_map());
    REQUIRE(decoded.meter_map() == project.meter_map());
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded.json);

    auto malformed = encoded.json;
    const auto denominator = malformed.find("\"denominator\":4");
    REQUIRE(denominator != std::string::npos);
    malformed.replace(denominator, std::string("\"denominator\":4").size(), "\"denominator\":3");
    REQUIRE_FALSE(deserialize_project(malformed, registry));
}

TEST_CASE("Timeline snapshots preserve clip gain and fades while old payloads default them") {
    const auto registry = builtins();
    const ClipPlaybackProperties playback{0.375f, 12, 24};
    auto clip = take(Clip::create({4}, {0}, {100}, EmptyContent{}, playback));
    auto track = take(Track::create({3}, "track", {clip}));
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, {track}));
    auto project = take(Project::create(ProjectInput{{1}, "playback", 5, {2}, {}, {sequence}}));

    auto encoded = take(serialize_project(project, registry));
    REQUIRE(encoded.json.find("\"gain_linear_bits\":\"1052770304\"") != std::string::npos);
    auto decoded = take(deserialize_project(encoded.json, registry));
    REQUIRE(decoded.find_sequence({2})->find_track({3})->find_clip({4})->playback_properties() ==
            playback);
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded.json);

    auto old_payload = encoded.json;
    for (const std::string field : {
             ",\"fade_in_duration\":\"12\"",
             ",\"fade_out_duration\":\"24\"",
             ",\"gain_linear_bits\":\"1052770304\"",
         }) {
        const auto position = old_payload.find(field);
        REQUIRE(position != std::string::npos);
        old_payload.erase(position, field.size());
    }
    auto old_decoded = take(deserialize_project(old_payload, registry));
    REQUIRE(
        old_decoded.find_sequence({2})->find_track({3})->find_clip({4})->playback_properties() ==
        ClipPlaybackProperties{});
}

TEST_CASE("Timeline version-one fixture remains readable and canonical") {
    std::ifstream stream(std::string(PULP_TIMELINE_FIXTURE_DIR) + "/v1/minimal.json",
                         std::ios::binary);
    REQUIRE(stream.good());
    const std::string fixture((std::istreambuf_iterator<char>(stream)),
                              std::istreambuf_iterator<char>());
    auto decoded = deserialize_project(fixture, builtins());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().name() == "fixture");
    REQUIRE(decoded.value().tempo_map() == TempoMap{});
    REQUIRE(decoded.value().meter_map() == MeterMap{});
    auto encoded = serialize_project(decoded.value(), builtins());
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().json.find("\"identities\":[") != std::string::npos);
    auto canonical = deserialize_project(encoded.value().json, builtins());
    REQUIRE(canonical.has_value());
    REQUIRE(take(serialize_project(canonical.value(), builtins())).json == encoded.value().json);
}

TEST_CASE("Timeline canonical snapshots preserve full-width values as decimal strings") {
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{0}, {}));
    auto project = take(Project::create(
        ProjectInput{{1}, "wide", std::numeric_limits<std::uint64_t>::max(), {2}, {}, {sequence}}));
    auto encoded = serialize_project(project, builtins());
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().json.find("\"next_item_id\":\"18446744073709551615\"") !=
            std::string::npos);

    auto decoded = deserialize_project(encoded.value().json, builtins());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().next_item_id() == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("Timeline snapshots round trip media notes and absolute time") {
    const auto registry = builtins();
    auto encoded = serialize_project(mixed_project(), registry);
    REQUIRE(encoded.has_value());
    auto decoded = deserialize_project(encoded.value().json, registry);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().sequences().size() == 2);
    const auto* musical = decoded.value().find_sequence({8});
    REQUIRE(musical != nullptr);
    REQUIRE(std::holds_alternative<MediaRef>(musical->tracks()[0].clips()[0].content()));
    const auto& note_content = std::get<NoteContent>(musical->tracks()[0].clips()[1].content());
    REQUIRE(note_content.notes().size() == 2);
    REQUIRE(note_content.notes()[0].id == ItemId{6});
    const auto* absolute = decoded.value().find_sequence({11});
    REQUIRE(absolute != nullptr);
    REQUIRE(absolute->tracks()[0].clips()[0].time_anchor() == ClipTimeAnchor::Absolute);
    REQUIRE(take(serialize_project(decoded.value(), registry)).json == encoded.value().json);
}

TEST_CASE("Timeline registered content uses the explicit typed codec") {
    const auto registry = registry_with_counter();
    auto registered = take(registry.create_registered_no_owned_ids(
        {"vendor.counter", 1}, std::make_shared<const std::int64_t>(-42), 1024));
    const auto original = project_with(registered);
    auto encoded = serialize_project(original, registry);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().json.find(R"("data":{"value":"-42"},"type_name":"vendor.counter")") !=
            std::string::npos);
    auto decoded = deserialize_project(encoded.value().json, registry);
    REQUIRE(decoded.has_value());
    const auto& content = decoded.value().sequences()[0].tracks()[0].clips()[0].content();
    const auto& result = std::get<RegisteredContent>(content);
    REQUIRE(*result.value_as<std::int64_t>() == -42);
    REQUIRE(equivalent(original.sequences()[0].tracks()[0].clips()[0],
                       decoded.value().sequences()[0].tracks()[0].clips()[0]));
    REQUIRE(take(serialize_project(decoded.value(), registry)).json == encoded.value().json);
}

TEST_CASE("Timeline budgeted writer streams every JSON scalar and escape form") {
    const auto registry = registry_with_complex();
    auto registered = take(registry.create_registered_no_owned_ids(
        {"vendor.complex", 1}, std::make_shared<const int>(1), 1024));
    auto clip = take(Clip::create({4}, {0}, {10}, std::move(registered)));
    std::string escaped = "quote\" slash\\";
    escaped.push_back('\b');
    escaped.push_back('\f');
    escaped.push_back('\n');
    escaped.push_back('\r');
    escaped.push_back('\t');
    escaped.push_back('\x01');
    auto track = take(Track::create({3}, escaped, {clip}));
    auto sequence = take(Sequence::create({2}, escaped, TickDuration{10}, {track}));
    auto project = take(Project::create(ProjectInput{{1}, escaped, 5, {2}, {}, {sequence}}));
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().json.find(R"("data":{"s":"value","z":[null,true,false,2]})") !=
            std::string::npos);
    REQUIRE(encoded.value().json.find("\\\"") != std::string::npos);
    REQUIRE(encoded.value().json.find("\\\\") != std::string::npos);
    REQUIRE(encoded.value().json.find("\\b") != std::string::npos);
    REQUIRE(encoded.value().json.find("\\f") != std::string::npos);
    REQUIRE(encoded.value().json.find("\\n") != std::string::npos);
    REQUIRE(encoded.value().json.find("\\r") != std::string::npos);
    REQUIRE(encoded.value().json.find("\\t") != std::string::npos);
    REQUIRE(encoded.value().json.find("\\u0001") != std::string::npos);
}

TEST_CASE("Timeline registered encoders cannot exceed the remaining output sink") {
    auto spy = std::make_shared<EncodeSpy>();
    const auto registry = registry_with_hostile_encoder(spy);
    constexpr std::size_t budget = 2'048;
    auto registered = registry.create_registered_no_owned_ids(
        {"vendor.hostile", 1}, std::make_shared<const int>(1), budget);
    REQUIRE_FALSE(registered.has_value());
    REQUIRE(registered.error().code == PersistenceErrorCode::OutputLimitExceeded);
    REQUIRE(spy->calls == 1);
    REQUIRE(spy->sink_maximum == budget);
    REQUIRE(spy->sink_size == spy->sink_maximum);
    REQUIRE(spy->append_attempts == spy->sink_maximum + 1);
}

TEST_CASE("Timeline unknown content is retained byte for byte and marked opaque") {
    std::ifstream fixture_stream(std::string(PULP_TIMELINE_FIXTURE_DIR) +
                                     "/v1/unknown-content-envelope.json",
                                 std::ios::binary);
    REQUIRE(fixture_stream.good());
    std::string raw((std::istreambuf_iterator<char>(fixture_stream)),
                    std::istreambuf_iterator<char>());
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r'))
        raw.pop_back();
    OpaqueContentLimits opaque_limits;
    opaque_limits.max_input_bytes = raw.size();
    opaque_limits.max_opaque_bytes = raw.size();
    auto opaque = take(OpaqueContent::create({"vendor.future", 7}, raw, opaque_limits));
    REQUIRE(opaque.validation_limits() == opaque_limits);
    auto first = serialize_project(project_with(opaque), builtins());
    REQUIRE(first.has_value());
    REQUIRE(first.value().has_opaque_objects);
    REQUIRE(first.value().json.find(raw) != std::string::npos);

    auto decoded = deserialize_project(first.value().json, builtins());
    REQUIRE(decoded.has_value());
    const auto& content = decoded.value().sequences()[0].tracks()[0].clips()[0].content();
    REQUIRE(std::get<OpaqueContent>(content).raw_json() == raw);
    auto second = serialize_project(decoded.value(), builtins());
    REQUIRE(second.has_value());
    REQUIRE(second.value().json == first.value().json);
    REQUIRE(second.value().has_opaque_objects);

    auto without_data = first.value().json;
    const auto raw_position = without_data.find(raw);
    REQUIRE(raw_position != std::string::npos);
    without_data.replace(raw_position, raw.size(), R"({"type_name":"vendor.future","version":7})");
    auto rejected = deserialize_project(without_data, builtins());
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == PersistenceErrorCode::InvalidSchema);
}
