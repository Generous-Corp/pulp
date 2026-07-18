#include <pulp/timeline/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::timeline;
using namespace pulp::timebase;
namespace runtime = pulp::runtime;

namespace {

template <typename T, typename E>
T take(runtime::Result<T, E> value) {
    REQUIRE(value.has_value());
    return std::move(value).value();
}

ContentHash hash(char digit) {
    return *ContentHash::from_hex(std::string(64, digit));
}

Project project_with(ClipContent content = EmptyContent{}) {
    auto clip = take(Clip::create({4}, {0}, {100}, std::move(content)));
    auto track = take(Track::create({3}, "track", {clip}));
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, {track}));
    MediaAsset asset{{5}, "missing-is-valid.wav", 1'000, {48'000, 1}, hash('a'),
                     AssetStoragePolicy::PreferEmbedded, {},
                     {{"proxy", hash('b'), AssetStoragePolicy::Embedded,
                       {{AssetLocatorKind::PackageRelative, "media/proxy.wav"}}}}};
    return take(Project::create(ProjectInput{{1}, "project", 6, {2}, {asset}, {sequence}}));
}

Project mixed_project() {
    auto media = take(Clip::create({4}, {0}, {10}, MediaRef{{2}, {25}, 10}));
    auto note_content = take(NoteContent::create(
        {{{7}, {4}, {2}, 0x8000, 64, 1}, {{6}, {1}, {2}, 0xffff, 60, 0}}));
    auto notes = take(Clip::create({5}, {20}, {10}, std::move(note_content)));
    auto musical_track = take(Track::create({3}, "musical", {notes, media}));
    auto musical_sequence =
        take(Sequence::create({8}, "root", TickDuration{100}, {musical_track}));

    auto absolute =
        take(Clip::create_absolute({10}, {0}, 48'000, {48'000, 1}, EmptyContent{}));
    auto absolute_track = take(Track::create({9}, "absolute", {absolute}));
    auto absolute_sequence = take(Sequence::create(
        {11}, "fixed", std::nullopt, AbsoluteTimelineDuration{48'000, {48'000, 1}},
        {absolute_track}));

    MediaAsset asset{{2}, "source.wav", 1'000, {48'000, 1}, hash('c'),
                     AssetStoragePolicy::External,
                     {{AssetLocatorKind::ExternalUri, "file:///source.wav"}}, {}};
    return take(Project::create(ProjectInput{{1}, "mixed", 12, {8}, {asset},
                                             {absolute_sequence, musical_sequence}}));
}

SchemaRegistry builtins() {
    return take(make_builtin_timeline_registry());
}

runtime::Result<std::shared_ptr<const void>, PersistenceError>
decode_counter(const JsonValue& data, const void*) noexcept {
    const auto* value = data.find("value");
    if (!value)
        return runtime::Result<std::shared_ptr<const void>, PersistenceError>(
            runtime::Err(PersistenceError{PersistenceErrorCode::MissingField}));
    auto parsed = parse_canonical_i64_string(*value, "/value");
    if (!parsed)
        return runtime::Result<std::shared_ptr<const void>, PersistenceError>(
            runtime::Err(parsed.error()));
    std::shared_ptr<const void> payload = std::make_shared<const std::int64_t>(parsed.value());
    return runtime::Result<std::shared_ptr<const void>, PersistenceError>(
        runtime::Ok(std::move(payload)));
}

runtime::Result<std::string, PersistenceError>
encode_counter(const std::shared_ptr<const void>& value, const void*) noexcept {
    if (!value)
        return runtime::Result<std::string, PersistenceError>(
            runtime::Err(PersistenceError{PersistenceErrorCode::InvalidSchema}));
    const auto number = *static_cast<const std::int64_t*>(value.get());
    return runtime::Result<std::string, PersistenceError>(
        runtime::Ok(std::string("{\"value\":\"") + std::to_string(number) + "\"}"));
}

SchemaRegistry registry_with_counter() {
    SchemaRegistryBuilder builder;
    REQUIRE(register_builtin_timeline_schemas(builder).has_value());
    TypeSchema schema;
    schema.type_name = "vendor.counter";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.fields = {{"value", SchemaValueKind::I64String}};
    schema.codec = {{}, decode_counter, encode_counter};
    REQUIRE(builder.register_type(std::move(schema)).has_value());
    return take(std::move(builder).build());
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

TEST_CASE("Timeline version-one fixture remains readable and canonical") {
    std::ifstream stream(std::string(PULP_TIMELINE_FIXTURE_DIR) + "/v1/minimal.json",
                         std::ios::binary);
    REQUIRE(stream.good());
    const std::string fixture((std::istreambuf_iterator<char>(stream)),
                              std::istreambuf_iterator<char>());
    auto decoded = deserialize_project(fixture, builtins());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().name() == "fixture");
    auto encoded = serialize_project(decoded.value(), builtins());
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().json + "\n" == fixture);
}

TEST_CASE("Timeline canonical snapshots preserve full-width values as decimal strings") {
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{0}, {}));
    auto project = take(Project::create(ProjectInput{
        {1}, "wide", std::numeric_limits<std::uint64_t>::max(), {2}, {}, {sequence}}));
    auto encoded = serialize_project(project, builtins());
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().json.find(
                "\"next_item_id\":\"18446744073709551615\"") != std::string::npos);

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
    const auto& note_content =
        std::get<NoteContent>(musical->tracks()[0].clips()[1].content());
    REQUIRE(note_content.notes().size() == 2);
    REQUIRE(note_content.notes()[0].id == ItemId{6});
    const auto* absolute = decoded.value().find_sequence({11});
    REQUIRE(absolute != nullptr);
    REQUIRE(absolute->tracks()[0].clips()[0].time_anchor() == ClipTimeAnchor::Absolute);
    REQUIRE(take(serialize_project(decoded.value(), registry)).json == encoded.value().json);
}

TEST_CASE("Timeline registered content uses the explicit typed codec") {
    const auto registry = registry_with_counter();
    auto registered = take(RegisteredContent::create_no_owned_ids(
        {"vendor.counter", 1}, std::make_shared<const std::int64_t>(-42)));
    auto encoded = serialize_project(project_with(registered), registry);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().json.find(
                R"("data":{"value":"-42"},"type_name":"vendor.counter")") !=
            std::string::npos);
    auto decoded = deserialize_project(encoded.value().json, registry);
    REQUIRE(decoded.has_value());
    const auto& content = decoded.value().sequences()[0].tracks()[0].clips()[0].content();
    const auto& result = std::get<RegisteredContent>(content);
    REQUIRE(*result.value_as<std::int64_t>() == -42);
    REQUIRE(take(serialize_project(decoded.value(), registry)).json == encoded.value().json);
}

TEST_CASE("Timeline unknown content is retained byte for byte and marked opaque") {
    const std::string raw =
        "{ \"version\" : 7, \"data\" : {\"escaped\":\"\\u0061\",\"owned_id\":\"99\"}, "
        "\"type_name\" : \"vendor.future\" }";
    auto opaque = take(OpaqueContent::create({"vendor.future", 7}, raw));
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
    without_data.replace(raw_position, raw.size(),
                         R"({"type_name":"vendor.future","version":7})");
    auto rejected = deserialize_project(without_data, builtins());
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == PersistenceErrorCode::MissingField);
}

TEST_CASE("Timeline JSON rejects ambiguous malformed and oversized input") {
    DecodeLimits limits;
    REQUIRE_FALSE(parse_json(R"({"a":1,"a":2})", limits).has_value());
    REQUIRE(parse_json(R"({"a":1,"a":2})", limits).error().code ==
            PersistenceErrorCode::DuplicateKey);
    std::string wide_object = "{";
    for (std::size_t index = 0; index < limits.max_object_members - 1; ++index) {
        if (index != 0)
            wide_object.push_back(',');
        wide_object += "\"k" + std::to_string(index) + "\":0";
    }
    wide_object += ",\"k0\":1}";
    auto duplicate_at_limit = parse_json(wide_object, limits);
    REQUIRE_FALSE(duplicate_at_limit.has_value());
    REQUIRE(duplicate_at_limit.error().code == PersistenceErrorCode::DuplicateKey);
    REQUIRE_FALSE(parse_json(R"("\uD800")", limits).has_value());
    REQUIRE(parse_json(R"("\uD800")", limits).error().code ==
            PersistenceErrorCode::InvalidUtf8);

    std::string invalid_utf8{"\""};
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8.push_back('"');
    auto utf8 = parse_json(invalid_utf8, limits);
    REQUIRE_FALSE(utf8.has_value());
    REQUIRE(utf8.error().code == PersistenceErrorCode::InvalidUtf8);

    limits.max_depth = 1;
    auto deep = parse_json("[[[]]]", limits);
    REQUIRE_FALSE(deep.has_value());
    REQUIRE(deep.error().code == PersistenceErrorCode::LimitExceeded);

    limits = {};
    limits.max_string_bytes = 3;
    auto long_string = parse_json(R"("four")", limits);
    REQUIRE_FALSE(long_string.has_value());
    REQUIRE(long_string.error().code == PersistenceErrorCode::LimitExceeded);

    limits = {};
    limits.max_input_bytes = 2;
    auto input = parse_json("null", limits);
    REQUIRE_FALSE(input.has_value());
    REQUIRE(input.error().code == PersistenceErrorCode::LimitExceeded);
}

TEST_CASE("Timeline decoder enforces canonical integers model validity and object limits") {
    const auto registry = builtins();
    auto snapshot = take(serialize_project(project_with(), registry)).json;

    auto noncanonical = snapshot;
    const auto id = noncanonical.find("\"id\":\"1\"");
    REQUIRE(id != std::string::npos);
    noncanonical.replace(id, std::string("\"id\":\"1\"").size(), "\"id\":\"01\"");
    auto rejected_number = deserialize_project(noncanonical, registry);
    REQUIRE_FALSE(rejected_number.has_value());
    REQUIRE(rejected_number.error().code == PersistenceErrorCode::InvalidNumber);

    auto invalid_model = snapshot;
    const auto root = invalid_model.find("\"root_sequence_id\":\"2\"");
    REQUIRE(root != std::string::npos);
    invalid_model.replace(root, std::string("\"root_sequence_id\":\"2\"").size(),
                          "\"root_sequence_id\":\"99\"");
    auto rejected_model = deserialize_project(invalid_model, registry);
    REQUIRE_FALSE(rejected_model.has_value());
    REQUIRE(rejected_model.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(rejected_model.error().model_error.has_value());

    DecodeLimits limits;
    limits.max_clips = 0;
    auto bounded = deserialize_project(snapshot, registry, limits);
    REQUIRE_FALSE(bounded.has_value());
    REQUIRE(bounded.error().code == PersistenceErrorCode::LimitExceeded);

    auto output = serialize_project(project_with(), registry, SerializeOptions{8});
    REQUIRE_FALSE(output.has_value());
    REQUIRE(output.error().code == PersistenceErrorCode::OutputLimitExceeded);

    auto invalid_name = project_with();
    std::string bad_name(1, static_cast<char>(0xc0));
    auto invalid_sequence = take(Sequence::create({2}, bad_name, TickDuration{0}, {}));
    invalid_name = take(Project::create(
        ProjectInput{{1}, "valid", 3, {2}, {}, {std::move(invalid_sequence)}}));
    auto invalid_utf8 = serialize_project(invalid_name, registry);
    REQUIRE_FALSE(invalid_utf8.has_value());
    REQUIRE(invalid_utf8.error().code == PersistenceErrorCode::InvalidUtf8);
}
