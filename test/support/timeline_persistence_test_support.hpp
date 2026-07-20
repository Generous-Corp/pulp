#pragma once

#include <pulp/timeline/command.hpp>
#include <pulp/timeline/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::timeline;
using namespace pulp::timebase;
namespace runtime = pulp::runtime;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> value) {
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
    MediaAsset asset{{5},
                     "missing-is-valid.wav",
                     1'000,
                     {48'000, 1},
                     hash('a'),
                     AssetStoragePolicy::PreferEmbedded,
                     {},
                     {{"proxy",
                       hash('b'),
                       AssetStoragePolicy::Embedded,
                       {{AssetLocatorKind::PackageRelative, "media/proxy.wav"}}}}};
    return take(Project::create(ProjectInput{{1}, "project", 6, {2}, {asset}, {sequence}}));
}

Project mixed_project() {
    auto media = take(Clip::create({4}, {0}, {10}, MediaRef{{2}, {25}, 10}));
    auto note_content =
        take(NoteContent::create({{{7}, {4}, {2}, 0x8000, 64, 1}, {{6}, {1}, {2}, 0xffff, 60, 0}}));
    auto notes = take(Clip::create({5}, {20}, {10}, std::move(note_content)));
    auto musical_track = take(Track::create({3}, "musical", {notes, media}));
    auto musical_sequence = take(Sequence::create({8}, "root", TickDuration{100}, {musical_track}));

    auto absolute = take(Clip::create_absolute({10}, {0}, 48'000, {48'000, 1}, EmptyContent{}));
    auto absolute_track = take(Track::create({9}, "absolute", {absolute}));
    auto absolute_sequence =
        take(Sequence::create({11}, "fixed", std::nullopt,
                              AbsoluteTimelineDuration{48'000, {48'000, 1}}, {absolute_track}));

    MediaAsset asset{{2},
                     "source.wav",
                     1'000,
                     {48'000, 1},
                     hash('c'),
                     AssetStoragePolicy::External,
                     {{AssetLocatorKind::ExternalUri, "file:///source.wav"}},
                     {}};
    return take(Project::create(
        ProjectInput{{1}, "mixed", 12, {8}, {asset}, {absolute_sequence, musical_sequence}}));
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
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(runtime::Ok(SchemaWriteSuccess{}));
}

std::size_t retained_i64(const std::shared_ptr<const void>&, const void*) noexcept {
    return sizeof(std::int64_t);
}

std::size_t retained_int(const std::shared_ptr<const void>&, const void*) noexcept {
    return sizeof(int);
}

SchemaRegistry registry_with_counter() {
    SchemaRegistryBuilder builder;
    REQUIRE(register_builtin_timeline_schemas(builder).has_value());
    TypeSchema schema;
    schema.type_name = "vendor.counter";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.fields = {{"value", SchemaValueKind::I64String}};
    schema.codec = {{}, decode_counter, encode_counter, retained_i64};
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
encode_complex(const std::shared_ptr<const void>&, BoundedJsonSink& output, const void*) noexcept {
    output.append(R"({"z":[null,true,false,2],"s":"value"})");
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(runtime::Ok(SchemaWriteSuccess{}));
}

SchemaRegistry registry_with_complex() {
    SchemaRegistryBuilder builder;
    REQUIRE(register_builtin_timeline_schemas(builder).has_value());
    TypeSchema schema;
    schema.type_name = "vendor.complex";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.codec = {{}, decode_complex, encode_complex, retained_int};
    REQUIRE(builder.register_type(std::move(schema)).has_value());
    return take(std::move(builder).build());
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_decoys(const std::shared_ptr<const void>&, BoundedJsonSink& output, const void*) noexcept {
    output.append(
        R"({"clips":[{},{}],"locators":[{},{}],"notes":[{},{}],"representations":[{},{}],"tracks":[{},{}]})");
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(runtime::Ok(SchemaWriteSuccess{}));
}

SchemaRegistry registry_with_decoys() {
    SchemaRegistryBuilder builder;
    REQUIRE(register_builtin_timeline_schemas(builder).has_value());
    TypeSchema schema;
    schema.type_name = "vendor.decoys";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.codec = {{}, decode_complex, encode_decoys, retained_int};
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
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(runtime::Ok(SchemaWriteSuccess{}));
}

SchemaRegistry registry_with_hostile_encoder(std::shared_ptr<EncodeSpy> spy) {
    SchemaRegistryBuilder builder;
    REQUIRE(register_builtin_timeline_schemas(builder).has_value());
    TypeSchema schema;
    schema.type_name = "vendor.hostile";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.codec = {std::move(spy), decode_counter, encode_hostile, retained_int};
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
        if (original.type_name == omitted)
            continue;
        auto copy = original;
        if (copy.type_name == version_changed)
            ++copy.current_version;
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
