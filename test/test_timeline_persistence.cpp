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

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_counter(const std::shared_ptr<const void>& value, BoundedJsonSink& output,
               const void*) noexcept {
    if (!value)
        return runtime::Result<SchemaWriteSuccess, PersistenceError>(
            runtime::Err(PersistenceError{PersistenceErrorCode::InvalidSchema}));
    const auto number = *static_cast<const std::int64_t*>(value.get());
    output.append("{\"value\":\"");
    output.append(std::to_string(number));
    output.append("\"}");
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(
        runtime::Ok(SchemaWriteSuccess{}));
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

runtime::Result<std::shared_ptr<const void>, PersistenceError>
decode_complex(const JsonValue&, const void*) noexcept {
    std::shared_ptr<const void> payload = std::make_shared<const int>(1);
    return runtime::Result<std::shared_ptr<const void>, PersistenceError>(
        runtime::Ok(std::move(payload)));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_complex(const std::shared_ptr<const void>&, BoundedJsonSink& output,
               const void*) noexcept {
    output.append(R"({"z":[null,true,false,2],"s":"value"})");
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(
        runtime::Ok(SchemaWriteSuccess{}));
}

SchemaRegistry registry_with_complex() {
    SchemaRegistryBuilder builder;
    REQUIRE(register_builtin_timeline_schemas(builder).has_value());
    TypeSchema schema;
    schema.type_name = "vendor.complex";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.codec = {{}, decode_complex, encode_complex};
    REQUIRE(builder.register_type(std::move(schema)).has_value());
    return take(std::move(builder).build());
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_decoys(const std::shared_ptr<const void>&, BoundedJsonSink& output,
              const void*) noexcept {
    output.append(R"({"clips":[{},{}],"locators":[{},{}],"notes":[{},{}],"representations":[{},{}],"tracks":[{},{}]})");
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(
        runtime::Ok(SchemaWriteSuccess{}));
}

SchemaRegistry registry_with_decoys() {
    SchemaRegistryBuilder builder;
    REQUIRE(register_builtin_timeline_schemas(builder).has_value());
    TypeSchema schema;
    schema.type_name = "vendor.decoys";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.codec = {{}, decode_complex, encode_decoys};
    REQUIRE(builder.register_type(std::move(schema)).has_value());
    return take(std::move(builder).build());
}

struct EncodeSpy {
    std::size_t calls = 0;
    std::size_t append_attempts = 0;
    std::size_t sink_maximum = 0;
    std::size_t sink_size = 0;
};

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_hostile(const std::shared_ptr<const void>&, BoundedJsonSink& output,
               const void* context) noexcept {
    auto& spy = *const_cast<EncodeSpy*>(static_cast<const EncodeSpy*>(context));
    ++spy.calls;
    spy.sink_maximum = output.maximum();
    for (std::size_t index = 0; index <= output.maximum(); ++index) {
        ++spy.append_attempts;
        output.append("x");
    }
    spy.sink_size = output.size();
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(
        runtime::Ok(SchemaWriteSuccess{}));
}

SchemaRegistry registry_with_hostile_encoder(std::shared_ptr<EncodeSpy> spy) {
    SchemaRegistryBuilder builder;
    REQUIRE(register_builtin_timeline_schemas(builder).has_value());
    TypeSchema schema;
    schema.type_name = "vendor.hostile";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.codec = {std::move(spy), decode_counter, encode_hostile};
    REQUIRE(builder.register_type(std::move(schema)).has_value());
    return take(std::move(builder).build());
}

enum class FieldMutation { Required, Kind, Reference };

SchemaRegistry structurally_modified_registry(std::string_view omitted,
                                               std::string_view version_changed = {},
                                               std::string_view field_changed = {},
                                               FieldMutation mutation = FieldMutation::Required) {
    const auto source = builtins();
    SchemaRegistryBuilder builder;
    for (const auto& original : source.types()) {
        if (original.type_name == omitted) continue;
        auto copy = original;
        if (copy.type_name == version_changed) copy.current_version = 2;
        if (copy.type_name == field_changed) {
            if (copy.fields.empty())
                copy.fields.emplace_back("unexpected", SchemaValueKind::String);
            else if (mutation == FieldMutation::Required)
                copy.fields.front().required = !copy.fields.front().required;
            else if (mutation == FieldMutation::Kind)
                copy.fields.front().kind = copy.fields.front().kind == SchemaValueKind::String
                                               ? SchemaValueKind::Object
                                               : SchemaValueKind::String;
            else
                copy.fields.front().referenced_type = "vendor.unexpected";
        }
        REQUIRE(builder.register_type(std::move(copy)).has_value());
    }
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

TEST_CASE("Timeline budgeted writer streams every JSON scalar and escape form") {
    const auto registry = registry_with_complex();
    auto registered = take(RegisteredContent::create_no_owned_ids(
        {"vendor.complex", 1}, std::make_shared<const int>(1)));
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
    auto project = take(Project::create(
        ProjectInput{{1}, escaped, 5, {2}, {}, {sequence}}));
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().json.find(
                R"("data":{"s":"value","z":[null,true,false,2]})") !=
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
    auto registered = take(RegisteredContent::create_no_owned_ids(
        {"vendor.hostile", 1}, std::make_shared<const int>(1)));
    constexpr std::size_t budget = 2'048;
    auto encoded = serialize_project(project_with(std::move(registered)), registry,
                                     SerializeOptions{budget});
    REQUIRE_FALSE(encoded.has_value());
    REQUIRE(encoded.error().code == PersistenceErrorCode::OutputLimitExceeded);
    REQUIRE(spy->calls == 1);
    REQUIRE(spy->sink_maximum < budget);
    REQUIRE(spy->sink_size == spy->sink_maximum);
    REQUIRE(spy->append_attempts == spy->sink_maximum + 1);
}

TEST_CASE("Timeline unknown content is retained byte for byte and marked opaque") {
    const std::string raw =
        "{ \"version\" : 7, \"data\" : {\"escaped\":\"\\u0061\",\"owned_id\":\"99\"}, "
        "\"type_name\" : \"vendor.future\" }";
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
    without_data.replace(raw_position, raw.size(),
                         R"({"type_name":"vendor.future","version":7})");
    auto rejected = deserialize_project(without_data, builtins());
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == PersistenceErrorCode::InvalidSchema);
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

    const auto complete = take(serialize_project(project_with(), registry));
    REQUIRE(serialize_project(project_with(), registry,
                              SerializeOptions{complete.json.size()})
                .has_value());
    auto one_byte_short = serialize_project(
        project_with(), registry, SerializeOptions{complete.json.size() - 1});
    REQUIRE_FALSE(one_byte_short.has_value());
    REQUIRE(one_byte_short.error().code == PersistenceErrorCode::OutputLimitExceeded);
    REQUIRE(one_byte_short.error().limit == complete.json.size() - 1);

    auto invalid_name = project_with();
    std::string bad_name(1, static_cast<char>(0xc0));
    auto invalid_sequence = take(Sequence::create({2}, bad_name, TickDuration{0}, {}));
    invalid_name = take(Project::create(
        ProjectInput{{1}, "valid", 3, {2}, {}, {std::move(invalid_sequence)}}));
    auto invalid_utf8 = serialize_project(invalid_name, registry);
    REQUIRE_FALSE(invalid_utf8.has_value());
    REQUIRE(invalid_utf8.error().code == PersistenceErrorCode::InvalidUtf8);
}

TEST_CASE("Timeline structural preflight applies quotas before scanning rejected elements") {
    const std::string expensive = "{\"payload\":\"" + std::string(256, 'x') + "\"}";
    const auto envelope = [](std::string_view type, std::string data,
                             std::string_view version = "1") {
        return "{\"data\":" + std::move(data) + ",\"type_name\":\"" +
               std::string(type) + "\",\"version\":" + std::string(version) + "}";
    };
    const auto project = [&](std::string data) {
        data.insert(1, R"("id":"1","name":"project","next_item_id":"2","root_sequence_id":"1",)");
        return envelope("pulp.timeline.project", std::move(data));
    };
    const auto asset = [&](std::string locators = "[]",
                           std::string representations = "[]") {
        return envelope("pulp.timeline.asset",
                        R"({"content_hash":")" + std::string(64, 'a') +
                            R"(","frame_count":"1","id":"1","name":"asset","sample_rate":{"denominator":"1","numerator":"48000"},"storage_policy":"external","locators":)" +
                            std::move(locators) +
                            ",\"representations\":" + std::move(representations) + "}");
    };
    const auto representation = [&](std::string locators = "[]") {
        return envelope("pulp.timeline.asset_representation",
                        R"({"content_hash":")" + std::string(64, 'b') +
                            R"(","role":"proxy","storage_policy":"embedded","locators":)" +
                            std::move(locators) + "}");
    };
    const auto empty_content = envelope("pulp.timeline.content.empty", "{}");
    const auto clip = [&](std::string content) {
        return envelope("pulp.timeline.clip",
                        R"({"id":"1","time_range":{"duration_ticks":"1","kind":"musical","start_ticks":"0"},"content":)" +
                            std::move(content) + "}");
    };
    const auto track = [&](std::string clips = "[]") {
        return envelope("pulp.timeline.track",
                        R"({"id":"1","name":"track","clips":)" +
                            std::move(clips) + "}");
    };
    const auto sequence = [&](std::string tracks = "[]") {
        return envelope("pulp.timeline.sequence",
                        R"({"absolute_duration":null,"id":"1","musical_duration":null,"name":"sequence","tracks":)" +
                            std::move(tracks) + "}");
    };
    const auto check = [&](std::string json, std::size_t DecodeLimits::*member,
                           std::string_view expected_path) {
        DecodeLimits limits;
        limits.max_string_bytes = 32;
        limits.*member = 1;
        auto parsed = preflight_timeline_structure(json, limits);
        REQUIRE_FALSE(parsed.has_value());
        REQUIRE(parsed.error().code == PersistenceErrorCode::LimitExceeded);
        REQUIRE(parsed.error().path == expected_path);
        REQUIRE(parsed.error().actual == 2);
        REQUIRE(parsed.error().limit == 1);
    };

    check(project("{\"assets\":[" + asset() + "," + expensive +
                  "],\"sequences\":[]}"),
          &DecodeLimits::max_assets, "/data/assets");
    check(project("{\"assets\":[],\"sequences\":[" + sequence() + "," +
                  expensive + "]}"),
          &DecodeLimits::max_sequences, "/data/sequences");
    check(project("{\"assets\":[],\"sequences\":[" +
                  sequence("[" + track() + "," + expensive + "]") + "]}"),
          &DecodeLimits::max_tracks, "/data/sequences/0/data/tracks");
    check(project("{\"assets\":[],\"sequences\":[" +
                  sequence("[" + track("[" + clip(empty_content) + "," + expensive + "]") +
                           "]") + "]}"),
          &DecodeLimits::max_clips,
          "/data/sequences/0/data/tracks/0/data/clips");
    const std::string valid_note =
        R"({"channel":0,"duration_ticks":"1","id":"1","pitch":60,"start_ticks":"0","velocity":1})";
    const auto note_content = envelope("pulp.timeline.content.notes",
                                       "{\"notes\":[" + valid_note + "," + expensive + "]}");
    check(project("{\"assets\":[],\"sequences\":[" +
                  sequence("[" + track("[" + clip(note_content) + "]") + "]") + "]}"),
          &DecodeLimits::max_notes,
          "/data/sequences/0/data/tracks/0/data/clips/0/data/content/data/notes");
    check(project("{\"assets\":[" + asset("[{}," + expensive + "]") +
                  "],\"sequences\":[]}"),
          &DecodeLimits::max_locators, "/data/assets/0/data/locators");
    check(project("{\"assets\":[" +
                  asset("[]", "[" + representation() + "," + expensive + "]") +
                  "],\"sequences\":[]}"),
          &DecodeLimits::max_representations, "/data/assets/0/data/representations");

    for (const auto malformed : {std::string_view("1.0"), std::string_view("\"1\"")}) {
        auto bad = project("{\"assets\":[],\"sequences\":[" +
                           envelope("pulp.timeline.sequence",
                                    "{\"tracks\":[" + expensive + "]}", malformed) + "]}");
        auto rejected = preflight_timeline_structure(bad, DecodeLimits{});
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error().code == PersistenceErrorCode::InvalidSchema);
    }
    auto scalar_data = project("{\"assets\":[],\"sequences\":[" +
                               envelope("pulp.timeline.sequence", "0") + "]}");
    auto rejected_scalar = preflight_timeline_structure(scalar_data, DecodeLimits{});
    REQUIRE_FALSE(rejected_scalar.has_value());
    REQUIRE(rejected_scalar.error().code == PersistenceErrorCode::InvalidSchema);

    const std::string malformed_unknown =
        R"({"data":0,"type_name":"vendor.unknown","version":"1"})";
    auto malformed_unknown_project = project("{\"assets\":[],\"sequences\":[" +
        sequence("[" + track("[" + clip(malformed_unknown) + "]") + "]") + "]}");
    auto rejected_unknown = preflight_timeline_structure(malformed_unknown_project,
                                                         DecodeLimits{});
    REQUIRE_FALSE(rejected_unknown.has_value());
    REQUIRE(rejected_unknown.error().code == PersistenceErrorCode::InvalidSchema);

    const auto unknown_terminal = envelope(
        "vendor.unknown",
        "{\"notes\":[{},{}],\"locators\":[{},{}],\"representations\":[{},{}]}");
    auto unknown_project = project("{\"assets\":[],\"sequences\":[" +
        sequence("[" + track("[" + clip(unknown_terminal) + "]") + "]") + "]}");
    DecodeLimits no_extension_quotas;
    no_extension_quotas.max_notes = 0;
    no_extension_quotas.max_locators = 0;
    no_extension_quotas.max_representations = 0;
    REQUIRE(preflight_timeline_structure(unknown_project, no_extension_quotas).has_value());

    const auto nul_decoy = project(
        "{\"assets\":[],\"ass\\u0000ets\":[" + expensive + "],\"sequences\":[]}");
    DecodeLimits no_assets;
    no_assets.max_assets = 0;
    REQUIRE(preflight_timeline_structure(nul_decoy, no_assets).has_value());

    const std::string malformed_slots[] = {
        project(R"({"assets":{},"sequences":[]})"),
        project(R"({"assets":[],"sequences":{}})"),
        project("{\"assets\":[" + asset("{}") + "],\"sequences\":[]}"),
        project("{\"assets\":[" + asset("[]", "{}") + "],\"sequences\":[]}"),
        project("{\"assets\":[],\"sequences\":[" + sequence("{}") + "]}"),
        project("{\"assets\":[],\"sequences\":[" +
                sequence("[" + track("{}") + "]") + "]}"),
        project("{\"assets\":[],\"sequences\":[" +
                sequence("[" + track("[{}]") + "]") + "]}"),
    };
    for (const auto& malformed : malformed_slots) {
        auto result = preflight_timeline_structure(malformed, DecodeLimits{});
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == PersistenceErrorCode::InvalidSchema);
    }
}

TEST_CASE("Timeline extension decoys do not consume structural quotas") {
    const std::string decoy_data =
        R"({"clips":[{},{}],"locators":[{},{}],"notes":[{},{}],"representations":[{},{}],"tracks":[{},{}]})";
    const std::string raw =
        "{\"data\":" + decoy_data +
        ",\"type_name\":\"vendor.opaque_decoys\",\"version\":1}";
    auto opaque = take(OpaqueContent::create({"vendor.opaque_decoys", 1}, raw));
    auto opaque_snapshot = take(serialize_project(project_with(opaque), builtins())).json;
    DecodeLimits exact;
    exact.max_assets = 1;
    exact.max_sequences = 1;
    exact.max_tracks = 1;
    exact.max_clips = 1;
    exact.max_notes = 0;
    exact.max_locators = 1;
    exact.max_representations = 1;
    REQUIRE(deserialize_project(opaque_snapshot, builtins(), exact).has_value());

    const auto registered_registry = registry_with_decoys();
    auto registered = take(RegisteredContent::create_no_owned_ids(
        {"vendor.decoys", 1}, std::make_shared<const int>(1)));
    auto registered_snapshot = take(serialize_project(project_with(registered),
                                                      registered_registry)).json;
    REQUIRE(deserialize_project(registered_snapshot, registered_registry, exact).has_value());

    auto empty_clip = take(Clip::create({4}, {0}, {10}, EmptyContent{}));
    auto track = take(Track::create({3}, "track", {empty_clip}));
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{10}, {track}));
    MediaAsset asset{{5}, "shared", 100, {48'000, 1}, hash('c'),
                     AssetStoragePolicy::External,
                     {{AssetLocatorKind::ExternalUri, "file:///source.wav"}},
                     {{"proxy", hash('d'), AssetStoragePolicy::Embedded,
                       {{AssetLocatorKind::PackageRelative, "proxy.wav"}}}}};
    auto shared = take(Project::create(ProjectInput{{1}, "shared", 6, {2}, {asset},
                                                    {sequence}}));
    auto shared_snapshot = take(serialize_project(shared, builtins())).json;
    exact.max_locators = 1;
    auto sharing = deserialize_project(shared_snapshot, builtins(), exact);
    REQUIRE_FALSE(sharing.has_value());
    REQUIRE(sharing.error().code == PersistenceErrorCode::LimitExceeded);
    REQUIRE(sharing.error().path == "/data/assets/0/data/representations/0/data/locators");
    REQUIRE(sharing.error().actual == 2);
}

TEST_CASE("Timeline structural preflight rejects missing required fields before DOM") {
    const auto envelope = [](std::string_view type, std::string data,
                             std::uint32_t version = 1) {
        return "{\"data\":" + std::move(data) + ",\"type_name\":\"" +
               std::string(type) + "\",\"version\":" + std::to_string(version) + "}";
    };
    const auto project = [&](std::string assets, std::string sequences,
                             std::string extra = {}) {
        return envelope("pulp.timeline.project",
                        "{\"assets\":" + std::move(assets) +
                            ",\"id\":\"1\",\"name\":\"project\",\"next_item_id\":\"2\","
                            "\"root_sequence_id\":\"1\",\"sequences\":" +
                            std::move(sequences) + std::move(extra) + "}");
    };
    const auto reject = [](const std::string& json) {
        auto result = preflight_timeline_structure(json, DecodeLimits{});
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == PersistenceErrorCode::InvalidSchema);
    };
    const auto erase_once = [](std::string value, std::string_view member) {
        const auto position = value.find(member);
        REQUIRE(position != std::string::npos);
        value.erase(position, member.size());
        return value;
    };
    const auto replace_once = [](std::string value, std::string_view from,
                                 std::string_view to) {
        const auto position = value.find(from);
        REQUIRE(position != std::string::npos);
        value.replace(position, from.size(), to);
        return value;
    };

    const auto root = project("[]", "[]", R"(,"sentinel":0)");
    for (const auto member : {
             std::string_view(R"("assets":[],)"),
             std::string_view(R"("id":"1",)"),
             std::string_view(R"("name":"project",)"),
             std::string_view(R"("next_item_id":"2",)"),
             std::string_view(R"("root_sequence_id":"1",)"),
             std::string_view(R"("sequences":[],)"),
         }) {
        INFO("missing_root_fields: " << member);
        reject(erase_once(root, member));
    }
    reject(replace_once(root, R"("id":"1")", R"("id":1)"));
    reject(replace_once(root, R"("name":"project")", R"("name":{})"));

    const std::string asset_data =
        R"({"content_hash":")" + std::string(64, 'a') +
        R"(","frame_count":"1","id":"5","locators":[],"name":"asset","representations":[],"sample_rate":{},"storage_policy":"external","sentinel":0})";
    const auto asset_project = project("[" + envelope("pulp.timeline.asset", asset_data) + "]", "[]");
    INFO("missing_asset_fields: content_hash");
    reject(erase_once(asset_project,
                      "\"content_hash\":\"" + std::string(64, 'a') + "\","));
    for (const auto member : {
             std::string_view(R"("frame_count":"1",)"),
             std::string_view(R"("id":"5",)"),
             std::string_view(R"("locators":[],)"),
             std::string_view(R"("name":"asset",)"),
             std::string_view(R"("representations":[],)"),
             std::string_view(R"("sample_rate":{},)"),
             std::string_view(R"("storage_policy":"external",)"),
        }) {
        INFO("missing_asset_fields: " << member);
        reject(erase_once(asset_project, member));
    }
    reject(replace_once(asset_project, R"("frame_count":"1")",
                        R"("frame_count":1)"));
    reject(replace_once(asset_project, R"("sample_rate":{})",
                        R"("sample_rate":"48000")"));

    const auto representation = envelope(
        "pulp.timeline.asset_representation",
        R"({"content_hash":")" + std::string(64, 'b') +
            R"(","locators":[],"role":"proxy","storage_policy":"embedded"})");
    auto representation_asset = asset_data;
    const auto representations = representation_asset.find(R"("representations":[])" );
    REQUIRE(representations != std::string::npos);
    representation_asset.replace(representations, std::string_view(R"("representations":[])" ).size(),
                                 "\"representations\":[" + representation + "]");
    const auto representation_project = project(
        "[" + envelope("pulp.timeline.asset", representation_asset) + "]", "[]");
    reject(erase_once(representation_project, R"("role":"proxy",)"));
    reject(replace_once(representation_project, R"("role":"proxy")",
                        R"("role":0)"));

    const auto empty = envelope("pulp.timeline.content.empty", "{}");
    const std::string clip_data =
        R"({"content":)" + empty +
        R"(,"id":"4","time_range":{},"sentinel":0})";
    const auto clip = envelope("pulp.timeline.clip", clip_data);
    const std::string track_data =
        "{\"clips\":[" + clip + R"(],"id":"3","name":"track","sentinel":0})";
    const auto track = envelope("pulp.timeline.track", track_data);
    const std::string sequence_data =
        std::string(R"({"absolute_duration":null,"id":"2","musical_duration":null,"name":"sequence","tracks":)") +
        "[" + track + R"(],"sentinel":0})";
    const auto sequence = envelope("pulp.timeline.sequence", sequence_data);
    const auto nested = project("[]", "[" + sequence + "]");
    for (const auto member : {
             std::string_view(R"("absolute_duration":null,)"),
             std::string_view(R"("musical_duration":null,)"),
             std::string_view(R"("name":"sequence",)"),
             std::string_view(R"("clips":[)"),
             std::string_view(R"("name":"track",)"),
             std::string_view(R"("content":)"),
             std::string_view(R"("time_range":{},)"),
         }) {
        INFO("missing_sequence_track_clip_fields: " << member);
        auto attacked = nested;
        if (member == R"("clips":[)" || member == R"("content":)") {
            const auto position = attacked.find(member);
            REQUIRE(position != std::string::npos);
            const auto value_end = member == R"("clips":[)"
                                       ? attacked.find("],\"id\":\"3\"", position)
                                       : attacked.find(",\"id\":\"4\"", position);
            REQUIRE(value_end != std::string::npos);
            const auto suffix = member == R"("clips":[)" ? 2u : 1u;
            attacked.erase(position, value_end - position + suffix);
        } else {
            attacked = erase_once(std::move(attacked), member);
        }
        reject(attacked);
    }

    reject(erase_once(nested, R"("id":"2",)"));
    reject(erase_once(nested, R"("id":"3",)"));
    reject(erase_once(nested, R"("id":"4",)"));
    reject(replace_once(nested, R"("absolute_duration":null)",
                        R"("absolute_duration":0)"));
    reject(replace_once(nested, R"("musical_duration":null)",
                        R"("musical_duration":0)"));
    reject(replace_once(nested, R"("id":"2")", R"("id":2)"));
    reject(replace_once(nested, R"("name":"track")", R"("name":[])"));
    reject(replace_once(nested, R"("id":"4")", R"("id":4)"));
    reject(replace_once(nested, R"("time_range":{})", R"("time_range":null)"));
}

TEST_CASE("Timeline future built-in notes content remains opaque and quota-terminal") {
    const std::string raw =
        R"({"data":{"notes":[{},{}]},"type_name":"pulp.timeline.content.notes","version":2})";
    auto opaque = take(OpaqueContent::create({"pulp.timeline.content.notes", 2}, raw));
    const auto registry = builtins();
    auto snapshot = take(serialize_project(project_with(std::move(opaque)), registry));
    DecodeLimits limits;
    limits.max_notes = 0;
    auto decoded = deserialize_project(snapshot.json, registry, limits);
    REQUIRE(decoded.has_value());
    const auto& content = decoded.value().sequences()[0].tracks()[0].clips()[0].content();
    REQUIRE(std::holds_alternative<OpaqueContent>(content));
    REQUIRE(std::get<OpaqueContent>(content).raw_json() == raw);
    auto reencoded = serialize_project(decoded.value(), registry);
    REQUIRE(reencoded.has_value());
    REQUIRE(reencoded.value().json == snapshot.json);
}

TEST_CASE("Timeline v1 notes reject malformed elements during structural preflight") {
    const auto envelope = [](std::string_view type, std::string data) {
        return "{\"data\":" + std::move(data) + ",\"type_name\":\"" +
               std::string(type) + "\",\"version\":1}";
    };
    const auto snapshot_with_note = [&](std::string note) {
        const auto content = envelope(
            "pulp.timeline.content.notes", "{\"notes\":[" + std::move(note) + "]}");
        const auto clip = envelope(
            "pulp.timeline.clip",
            R"({"content":)" + content +
                R"(,"id":"4","time_range":{"duration_ticks":"1","kind":"musical","start_ticks":"0"}})");
        const auto track = envelope(
            "pulp.timeline.track",
            "{\"clips\":[" + clip + R"(],"id":"3","name":"track"})");
        const auto sequence = envelope(
            "pulp.timeline.sequence",
            std::string(R"({"absolute_duration":null,"id":"2","musical_duration":"1","name":"sequence","tracks":)") +
                "[" + track + "]}");
        return envelope(
            "pulp.timeline.project",
            "{\"assets\":[],\"id\":\"1\",\"name\":\"project\",\"next_item_id\":\"8\","
            "\"root_sequence_id\":\"2\",\"sequences\":[" + sequence + "]}");
    };
    const auto reject = [&](std::string note) {
        auto result = preflight_timeline_structure(snapshot_with_note(std::move(note)),
                                                   DecodeLimits{});
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == PersistenceErrorCode::InvalidSchema);
    };
    const auto erase_once = [](std::string value, std::string_view member) {
        const auto position = value.find(member);
        REQUIRE(position != std::string::npos);
        value.erase(position, member.size());
        return value;
    };
    const auto replace_once = [](std::string value, std::string_view from,
                                 std::string_view to) {
        const auto position = value.find(from);
        REQUIRE(position != std::string::npos);
        value.replace(position, from.size(), to);
        return value;
    };

    const std::string note =
        R"({"channel":0,"duration_ticks":"1","id":"7","pitch":60,"start_ticks":"0","velocity":1,"sentinel":0})";
    REQUIRE(preflight_timeline_structure(snapshot_with_note(note), DecodeLimits{}).has_value());
    for (const auto member : {
             std::string_view(R"("channel":0,)"),
             std::string_view(R"("duration_ticks":"1",)"),
             std::string_view(R"("id":"7",)"),
             std::string_view(R"("pitch":60,)"),
             std::string_view(R"("start_ticks":"0",)"),
             std::string_view(R"("velocity":1,)"),
         }) {
        INFO("missing note field: " << member);
        reject(erase_once(note, member));
    }
    reject(replace_once(note, R"("id":"7")", R"("id":7)"));
    reject(replace_once(note, R"("start_ticks":"0")", R"("start_ticks":0)"));
    reject(replace_once(note, R"("duration_ticks":"1")", R"("duration_ticks":1)"));
    reject(replace_once(note, R"("velocity":1)", R"("velocity":"1")"));
    reject(replace_once(note, R"("pitch":60)", R"("pitch":"60")"));
    reject(replace_once(note, R"("channel":0)", R"("channel":"0")"));
    auto duplicate = preflight_timeline_structure(
        snapshot_with_note(R"({"id":"7","id":"8","start_ticks":"0","duration_ticks":"1","velocity":1,"pitch":60,"channel":0})"),
        DecodeLimits{});
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error().code == PersistenceErrorCode::DuplicateKey);
    reject("0");
}

TEST_CASE("Timeline persistence requires the complete compatible structural registry") {
    const auto project = project_with();
    const auto snapshot = take(serialize_project(project, builtins())).json;
    constexpr std::string_view structural_types[] = {
        "pulp.timeline.project",
        "pulp.timeline.asset",
        "pulp.timeline.asset_representation",
        "pulp.timeline.sequence",
        "pulp.timeline.track",
        "pulp.timeline.clip",
        "pulp.timeline.content.empty",
        "pulp.timeline.content.media",
        "pulp.timeline.content.notes",
    };
    for (const auto type_name : structural_types) {
        const auto incomplete = structurally_modified_registry(type_name);
        auto encode_missing = serialize_project(project, incomplete);
        REQUIRE_FALSE(encode_missing.has_value());
        REQUIRE(encode_missing.error().code ==
                PersistenceErrorCode::UnsupportedStructuralType);
        auto decode_missing = deserialize_project(snapshot, incomplete);
        REQUIRE_FALSE(decode_missing.has_value());
        REQUIRE(decode_missing.error().code ==
                PersistenceErrorCode::UnsupportedStructuralType);

        const auto conflicting = structurally_modified_registry({}, type_name);
        auto encode_conflict = serialize_project(project, conflicting);
        REQUIRE_FALSE(encode_conflict.has_value());
        REQUIRE(encode_conflict.error().code ==
                PersistenceErrorCode::UnsupportedSchemaVersion);
        auto decode_conflict = deserialize_project(snapshot, conflicting);
        REQUIRE_FALSE(decode_conflict.has_value());
        REQUIRE(decode_conflict.error().code ==
                PersistenceErrorCode::UnsupportedSchemaVersion);

        const auto incompatible_fields = structurally_modified_registry({}, {}, type_name);
        auto encode_fields = serialize_project(project, incompatible_fields);
        REQUIRE_FALSE(encode_fields.has_value());
        REQUIRE(encode_fields.error().code == PersistenceErrorCode::InvalidSchema);
        auto decode_fields = deserialize_project(snapshot, incompatible_fields);
        REQUIRE_FALSE(decode_fields.has_value());
        REQUIRE(decode_fields.error().code == PersistenceErrorCode::InvalidSchema);
    }
    for (const auto mutation : {FieldMutation::Kind, FieldMutation::Reference}) {
        const auto incompatible = structurally_modified_registry(
            {}, {}, "pulp.timeline.project", mutation);
        auto encode = serialize_project(project, incompatible);
        REQUIRE_FALSE(encode.has_value());
        REQUIRE(encode.error().code == PersistenceErrorCode::InvalidSchema);
        auto decode = deserialize_project(snapshot, incompatible);
        REQUIRE_FALSE(decode.has_value());
        REQUIRE(decode.error().code == PersistenceErrorCode::InvalidSchema);
    }
}
