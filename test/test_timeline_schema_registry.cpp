#include <pulp/timeline/schema_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace pulp::timeline;
namespace runtime = pulp::runtime;

namespace {

runtime::Result<std::string, PersistenceError>
replace_version(std::string_view source, const void* context) noexcept {
    const auto versions = static_cast<const std::pair<std::uint32_t, std::uint32_t>*>(context);
    const auto before = std::string("\"version\":") + std::to_string(versions->first);
    const auto after = std::string("\"version\":") + std::to_string(versions->second);
    std::string output(source);
    const auto position = output.find(before);
    if (position == std::string::npos)
        return runtime::Result<std::string, PersistenceError>(
            runtime::Err(PersistenceError{PersistenceErrorCode::MigrationFailed}));
    output.replace(position, before.size(), after);
    return runtime::Result<std::string, PersistenceError>(runtime::Ok(std::move(output)));
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
    REQUIRE(built.value()
                .migrate(SchemaDomain::Content, "vendor.migrating", 1, 1, {}, zero)
                .has_value());
    auto zero_rejects_nonempty = built.value().migrate(
        SchemaDomain::Content, "vendor.migrating", 1, 1, "x", zero);
    REQUIRE_FALSE(zero_rejects_nonempty.has_value());
    REQUIRE(zero_rejects_nonempty.error().code == PersistenceErrorCode::LimitExceeded);
}

TEST_CASE("Built-in timeline schemas require explicit registration") {
    auto builtins = make_builtin_timeline_registry();
    REQUIRE(builtins.has_value());
    REQUIRE(builtins.value().find(SchemaDomain::Document, "pulp.timeline.project") != nullptr);
    REQUIRE(builtins.value().find(SchemaDomain::Content,
                                  "pulp.timeline.content.notes") != nullptr);
    REQUIRE(SchemaRegistry{}.types().empty());
}
