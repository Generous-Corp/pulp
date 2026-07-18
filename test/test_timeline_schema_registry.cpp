#include <pulp/timeline/schema_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace pulp::timeline;
namespace runtime = pulp::runtime;

namespace {

template <typename T>
concept HasCallbackStorageAccessor = requires(T& value) { value.output(); };

static_assert(!std::is_copy_constructible_v<BoundedJsonSink>);
static_assert(!std::is_copy_assignable_v<BoundedJsonSink>);
static_assert(!std::is_move_constructible_v<BoundedJsonSink>);
static_assert(!std::is_move_assignable_v<BoundedJsonSink>);
static_assert(!HasCallbackStorageAccessor<BoundedJsonSink>);

template <typename T, typename E>
T take_value(runtime::Result<T, E> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
replace_version(std::string_view source, BoundedJsonSink& output,
                const void* context) noexcept {
    const auto versions = static_cast<const std::pair<std::uint32_t, std::uint32_t>*>(context);
    const auto before = std::string("\"version\":") + std::to_string(versions->first);
    const auto after = std::string("\"version\":") + std::to_string(versions->second);
    const auto position = source.find(before);
    if (position == std::string::npos)
        return runtime::Result<SchemaWriteSuccess, PersistenceError>(
            runtime::Err(PersistenceError{PersistenceErrorCode::MigrationFailed}));
    if (!output.append(source.substr(0, position)) || !output.append(after) ||
        !output.append(source.substr(position + before.size())))
        return runtime::Result<SchemaWriteSuccess, PersistenceError>(
            runtime::Ok(SchemaWriteSuccess{}));
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(
        runtime::Ok(SchemaWriteSuccess{}));
}

MigrationStep migration(std::uint32_t from, std::uint32_t to) {
    return {from, to,
            std::make_shared<const std::pair<std::uint32_t, std::uint32_t>>(from, to),
            replace_version};
}

TypeSchema type(std::string name, std::uint32_t version = 1) {
    TypeSchema schema;
    schema.type_name = std::move(name);
    schema.domain = SchemaDomain::Content;
    schema.current_version = version;
    return schema;
}

struct MigrationSpy {
    std::size_t calls = 0;
    std::size_t append_attempts = 0;
    std::size_t sink_maximum = 0;
    std::size_t sink_size = 0;
    std::string output;
    bool hostile = false;
};

runtime::Result<SchemaWriteSuccess, PersistenceError>
scripted_migration(std::string_view, BoundedJsonSink& output,
                   const void* context) noexcept {
    auto& spy = *const_cast<MigrationSpy*>(static_cast<const MigrationSpy*>(context));
    ++spy.calls;
    spy.sink_maximum = output.maximum();
    if (spy.hostile) {
        for (std::size_t index = 0; index <= output.maximum(); ++index) {
            ++spy.append_attempts;
            output.append("x");
        }
    } else {
        output.append(spy.output);
    }
    spy.sink_size = output.size();
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(
        runtime::Ok(SchemaWriteSuccess{}));
}

} // namespace

TEST_CASE("Timeline schema registry is explicit immutable and deterministic") {
    SchemaRegistryBuilder builder;
    auto zed = type("vendor.zed");
    zed.fields = {{"z", SchemaValueKind::String}, {"a", SchemaValueKind::U64String}};
    REQUIRE(builder.register_type(std::move(zed)).has_value());
    REQUIRE(builder.register_type(type("vendor.alpha")).has_value());
    auto registry = std::move(builder).build();
    REQUIRE(registry.has_value());
    REQUIRE(registry.value().types().size() == 2);
    REQUIRE(registry.value().types()[0].type_name == "vendor.alpha");
    REQUIRE(registry.value().types()[1].fields[0].name == "a");
    REQUIRE(registry.value().find(SchemaDomain::Content, "vendor.zed") != nullptr);
    REQUIRE(registry.value().find(SchemaDomain::Document, "vendor.zed") == nullptr);
}

TEST_CASE("Timeline schema builder rejects ambiguous registrations") {
    SchemaRegistryBuilder duplicate_type;
    REQUIRE(duplicate_type.register_type(type("vendor.same")).has_value());
    auto duplicate = duplicate_type.register_type(type("vendor.same"));
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error().code == SchemaErrorCode::DuplicateType);

    SchemaRegistryBuilder duplicate_field;
    auto fields = type("vendor.fields");
    fields.fields = {{"same", SchemaValueKind::String},
                     {"same", SchemaValueKind::U32}};
    auto field_result = duplicate_field.register_type(std::move(fields));
    REQUIRE_FALSE(field_result.has_value());
    REQUIRE(field_result.error().code == SchemaErrorCode::DuplicateField);

    SchemaRegistryBuilder invalid_codec;
    auto codec = type("vendor.codec");
    codec.codec.decode = [](const JsonValue&, const void*) noexcept
        -> runtime::Result<std::shared_ptr<const void>, PersistenceError> {
        return runtime::Result<std::shared_ptr<const void>, PersistenceError>(
            runtime::Ok(std::shared_ptr<const void>{}));
    };
    auto codec_result = invalid_codec.register_type(std::move(codec));
    REQUIRE_FALSE(codec_result.has_value());
    REQUIRE(codec_result.error().code == SchemaErrorCode::InvalidCodec);

    SchemaRegistryBuilder invalid_identity;
    auto identity_result = invalid_identity.register_type(type("", 0));
    REQUIRE_FALSE(identity_result.has_value());
    REQUIRE(identity_result.error().code == SchemaErrorCode::InvalidIdentity);
}

TEST_CASE("Timeline schema migrations are bounded and verify every envelope") {
    SchemaRegistryBuilder builder;
    auto schema = type("vendor.migrating", 3);
    schema.upgrades = {migration(1, 2), migration(2, 3)};
    schema.downgrades = {migration(3, 2), migration(2, 1)};
    REQUIRE(builder.register_type(std::move(schema)).has_value());
    auto built = std::move(builder).build();
    REQUIRE(built.has_value());
    constexpr std::string_view source =
        R"({"data":{},"type_name":"vendor.migrating","version":1})";
    auto upgraded = built.value().migrate(SchemaDomain::Content, "vendor.migrating", 1, 3,
                                          source);
    REQUIRE(upgraded.has_value());
    REQUIRE(upgraded.value().find("\"version\":3") != std::string::npos);
    auto downgraded = built.value().migrate(SchemaDomain::Content, "vendor.migrating", 3, 1,
                                            upgraded.value());
    REQUIRE(downgraded.has_value());
    REQUIRE(downgraded.value() == source);

    DecodeLimits one_step;
    one_step.max_migration_steps = 1;
    auto bounded = built.value().migrate(SchemaDomain::Content, "vendor.migrating", 1, 3,
                                         source, one_step);
    REQUIRE_FALSE(bounded.has_value());
    REQUIRE(bounded.error().code == PersistenceErrorCode::LimitExceeded);

    auto missing = built.value().migrate(SchemaDomain::Content, "vendor.migrating", 1, 4,
                                         source);
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().code == PersistenceErrorCode::MigrationPathMissing);

    DecodeLimits exact;
    exact.max_input_bytes = source.size();
    REQUIRE(built.value()
                .migrate(SchemaDomain::Content, "vendor.migrating", 1, 1, source, exact)
                .has_value());
    REQUIRE(built.value()
                .migrate(SchemaDomain::Content, "vendor.migrating", 1, 2, source, exact)
                .has_value());

    DecodeLimits undersized;
    undersized.max_input_bytes = source.size() - 1;
    auto no_op_oversized = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 1, source, undersized);
    REQUIRE_FALSE(no_op_oversized.has_value());
    REQUIRE(no_op_oversized.error().code == PersistenceErrorCode::LimitExceeded);
    auto stepped_oversized = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 2, source, undersized);
    REQUIRE_FALSE(stepped_oversized.has_value());
    REQUIRE(stepped_oversized.error().code == PersistenceErrorCode::LimitExceeded);

    DecodeLimits zero;
    zero.max_input_bytes = 0;
    auto empty_no_op = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 1, {}, zero);
    REQUIRE_FALSE(empty_no_op.has_value());
    REQUIRE(empty_no_op.error().code == PersistenceErrorCode::InvalidJson);
    auto zero_rejects_nonempty = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 1, "x", zero);
    REQUIRE_FALSE(zero_rejects_nonempty.has_value());
    REQUIRE(zero_rejects_nonempty.error().code == PersistenceErrorCode::LimitExceeded);

    auto malformed = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 2, "{broken");
    REQUIRE_FALSE(malformed.has_value());
    REQUIRE(malformed.error().code == PersistenceErrorCode::InvalidJson);

    constexpr std::string_view duplicate =
        R"({"data":{},"type_name":"vendor.migrating","type_name":"vendor.migrating","version":1})";
    auto duplicate_source = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 2, duplicate);
    REQUIRE_FALSE(duplicate_source.has_value());
    // DuplicateKey rather than MigrationFailed proves the callback was never entered.
    REQUIRE(duplicate_source.error().code == PersistenceErrorCode::DuplicateKey);

    auto non_object = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 1, "[]");
    REQUIRE_FALSE(non_object.has_value());
    REQUIRE(non_object.error().code == PersistenceErrorCode::InvalidSchema);

    constexpr std::string_view wrong_type =
        R"({"data":{},"type_name":"vendor.other","version":1})";
    auto mismatched_type = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 1, wrong_type);
    REQUIRE_FALSE(mismatched_type.has_value());
    REQUIRE(mismatched_type.error().code == PersistenceErrorCode::InvalidSchema);

    constexpr std::string_view wrong_version =
        R"({"data":{},"type_name":"vendor.migrating","version":2})";
    auto mismatched_version = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 2, wrong_version);
    REQUIRE_FALSE(mismatched_version.has_value());
    REQUIRE(mismatched_version.error().code == PersistenceErrorCode::InvalidSchema);
}

TEST_CASE("Built-in timeline schemas require explicit registration") {
    auto builtins = make_builtin_timeline_registry();
    REQUIRE(builtins.has_value());
    REQUIRE(builtins.value().find(SchemaDomain::Document, "pulp.timeline.project") != nullptr);
    REQUIRE(builtins.value().find(SchemaDomain::Content,
                                  "pulp.timeline.content.notes") != nullptr);
    const auto* sequence = builtins.value().find(
        SchemaDomain::Document, "pulp.timeline.sequence");
    REQUIRE(sequence != nullptr);
    const auto required = [&](std::string_view name) {
        const auto field = std::find_if(
            sequence->fields.begin(), sequence->fields.end(),
            [&](const FieldSchema& candidate) { return candidate.name == name; });
        REQUIRE(field != sequence->fields.end());
        return field->required;
    };
    REQUIRE(required("absolute_duration"));
    REQUIRE(required("musical_duration"));
    REQUIRE(SchemaRegistry{}.types().empty());
}

TEST_CASE("Timeline migrations reject non-exact envelopes before the next callback") {
    constexpr std::string_view source =
        R"({"data":{},"type_name":"vendor.spy","version":1})";
    const std::vector<std::string> invalid_initial = {
        R"({"type_name":"vendor.spy","version":1})",
        R"({"data":0,"type_name":"vendor.spy","version":1})",
        R"({"data":{},"extra":0,"type_name":"vendor.spy","version":1})",
    };
    auto initial_spy = std::make_shared<MigrationSpy>();
    initial_spy->output =
        R"({"data":{},"type_name":"vendor.spy","version":2})";
    SchemaRegistryBuilder initial_builder;
    auto initial_schema = type("vendor.spy", 2);
    initial_schema.upgrades = {{1, 2, initial_spy, scripted_migration}};
    REQUIRE(initial_builder.register_type(std::move(initial_schema)).has_value());
    auto initial_registry = take_value(std::move(initial_builder).build());
    for (const auto& candidate : invalid_initial) {
        initial_spy->calls = 0;
        auto result = initial_registry.migrate(SchemaDomain::Content, "vendor.spy", 1, 2,
                                               candidate);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == PersistenceErrorCode::InvalidSchema);
        REQUIRE(initial_spy->calls == 0);
    }

    const std::vector<std::string> invalid_intermediate = {
        R"({"type_name":"vendor.spy","version":2})",
        R"({"data":0,"type_name":"vendor.spy","version":2})",
        R"({"data":{},"extra":0,"type_name":"vendor.spy","version":2})",
    };
    for (const auto& candidate : invalid_intermediate) {
        auto first = std::make_shared<MigrationSpy>();
        auto second = std::make_shared<MigrationSpy>();
        first->output = candidate;
        second->output =
            R"({"data":{},"type_name":"vendor.spy","version":3})";
        SchemaRegistryBuilder builder;
        auto schema = type("vendor.spy", 3);
        schema.upgrades = {{1, 2, first, scripted_migration},
                           {2, 3, second, scripted_migration}};
        REQUIRE(builder.register_type(std::move(schema)).has_value());
        auto registry = take_value(std::move(builder).build());
        auto result = registry.migrate(SchemaDomain::Content, "vendor.spy", 1, 3,
                                       source);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == PersistenceErrorCode::MigrationFailed);
        REQUIRE(first->calls == 1);
        REQUIRE(second->calls == 0);
    }
}

TEST_CASE("Timeline migration callbacks cannot grow output beyond their bounded sink") {
    constexpr std::string_view source =
        R"({"data":{},"type_name":"vendor.hostile","version":1})";
    auto spy = std::make_shared<MigrationSpy>();
    spy->hostile = true;
    SchemaRegistryBuilder builder;
    auto schema = type("vendor.hostile", 2);
    schema.upgrades = {{1, 2, spy, scripted_migration}};
    REQUIRE(builder.register_type(std::move(schema)).has_value());
    auto registry = take_value(std::move(builder).build());
    DecodeLimits limits;
    limits.max_input_bytes = source.size();
    auto result = registry.migrate(SchemaDomain::Content, "vendor.hostile", 1, 2,
                                   source, limits);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == PersistenceErrorCode::LimitExceeded);
    REQUIRE(spy->calls == 1);
    REQUIRE(spy->sink_maximum == source.size());
    REQUIRE(spy->sink_size == source.size());
    REQUIRE(spy->append_attempts == source.size() + 1);
}
