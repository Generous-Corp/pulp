#include <pulp/timeline/schema_codegen.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace pulp::timeline;
namespace runtime = pulp::runtime;

namespace {

std::string emit() {
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry.has_value());
    auto manifest = emit_schema_manifest(registry.value());
    REQUIRE(manifest.has_value());
    return std::move(manifest).value();
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("schema manifest is deterministic across emissions", "[timeline][codegen]") {
    // Same registry, freshly built each time, must yield byte-identical output —
    // this is the invariant the drift gate depends on.
    const std::string first = emit();
    const std::string second = emit();
    REQUIRE(first == second);
    REQUIRE_FALSE(first.empty());
}

TEST_CASE("schema manifest is canonical JSON", "[timeline][codegen]") {
    const std::string manifest = emit();
    auto parsed = parse_json(manifest);
    REQUIRE(parsed.has_value());
    // Canonicalizing an already-canonical document is the identity.
    auto recanonical = canonicalize_json(parsed.value()->root());
    REQUIRE(recanonical.has_value());
    REQUIRE(recanonical.value() == manifest);
}

TEST_CASE("schema manifest projects every registered type", "[timeline][codegen]") {
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry.has_value());
    auto manifest = emit_schema_manifest(registry.value());
    REQUIRE(manifest.has_value());

    auto parsed = parse_json(manifest.value());
    REQUIRE(parsed.has_value());
    const JsonValue& root = parsed.value()->root();
    const JsonValue* defs = root.find("$defs");
    REQUIRE(defs != nullptr);
    REQUIRE(defs->kind == JsonValue::Kind::Object);

    const auto types = registry.value().types();
    REQUIRE_FALSE(types.empty());
    REQUIRE(defs->object.size() == types.size());

    for (const auto& type : types) {
        const JsonValue* entry = defs->find(type.type_name);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->kind == JsonValue::Kind::Object);

        const JsonValue* version = entry->find("x-pulp-current-version");
        REQUIRE(version != nullptr);
        REQUIRE(version->scalar == std::to_string(type.current_version));

        const JsonValue* domain = entry->find("x-pulp-domain");
        REQUIRE(domain != nullptr);

        const JsonValue* properties = entry->find("properties");
        REQUIRE(properties != nullptr);
        REQUIRE(properties->kind == JsonValue::Kind::Object);
        REQUIRE(properties->object.size() == type.fields.size());
        for (const auto& field : type.fields)
            REQUIRE(properties->find(field.name) != nullptr);
    }
}

TEST_CASE("schema manifest carries migration edges", "[timeline][codegen]") {
    // The track type is the only built-in with registered migrations; its edges
    // must survive the projection so downstream generators see version paths.
    const std::string manifest = emit();
    REQUIRE(contains(manifest, "x-pulp-migrations"));
    REQUIRE(contains(manifest, "\"upgrades\""));
    REQUIRE(contains(manifest, "\"downgrades\""));
    // Track upgrades 1->2 and 2->3 are registered.
    REQUIRE(contains(manifest, "\"from\":1,\"to\":2"));
    REQUIRE(contains(manifest, "\"from\":2,\"to\":3"));
}

TEST_CASE("schema manifest is self-identifying", "[timeline][codegen]") {
    const std::string manifest = emit();
    REQUIRE(contains(manifest, "pulp-timeline-schema-emit"));
    REQUIRE(contains(manifest, "x-pulp-manifest-version"));
    REQUIRE(contains(manifest, "json-schema.org/draft/2020-12/schema"));
}

namespace {

SchemaRegistry build_custom() {
    // A field carrying a referenced_type is the only way to exercise $ref
    // emission — no built-in type sets one.
    TypeSchema referrer;
    referrer.type_name = "pulp.test.referrer";
    referrer.domain = SchemaDomain::Document;
    referrer.current_version = 1;
    referrer.fields.emplace_back("link", SchemaValueKind::Object, true, "pulp.test.target");

    TypeSchema target;
    target.type_name = "pulp.test.target";
    target.domain = SchemaDomain::Document;
    target.current_version = 1;
    target.fields.emplace_back("flag", SchemaValueKind::Boolean, false);

    SchemaRegistryBuilder builder;
    REQUIRE(builder.register_type(std::move(referrer)).has_value());
    REQUIRE(builder.register_type(std::move(target)).has_value());
    auto built = std::move(builder).build();
    REQUIRE(built.has_value());
    return std::move(built).value();
}

} // namespace

TEST_CASE("schema manifest emits $ref for referenced types", "[timeline][codegen]") {
    const SchemaRegistry registry = build_custom();
    auto manifest = emit_schema_manifest(registry);
    REQUIRE(manifest.has_value());
    REQUIRE(contains(manifest.value(), "#/$defs/pulp.test.target"));
    // A non-required boolean field must not appear in a required set; the target
    // has only that field, so it carries no "required" key at all.
    auto parsed = parse_json(manifest.value());
    REQUIRE(parsed.has_value());
    const JsonValue* defs = parsed.value()->root().find("$defs");
    REQUIRE(defs != nullptr);
    const JsonValue* target = defs->find("pulp.test.target");
    REQUIRE(target != nullptr);
    REQUIRE(target->find("required") == nullptr);
}

TEST_CASE("schema manifest fails closed when it exceeds the byte budget",
          "[timeline][codegen]") {
    const SchemaRegistry registry = build_custom();
    auto manifest = emit_schema_manifest(registry, 16);
    REQUIRE_FALSE(manifest.has_value());
    REQUIRE(manifest.error().code == PersistenceErrorCode::OutputLimitExceeded);
}
