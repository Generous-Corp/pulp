#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/schema_json.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::timeline {

enum class SchemaDomain : std::uint8_t {
    Document,
    Content,
    AssetRepresentation,
    Command,
    Diagnostic,
};

enum class SchemaValueKind : std::uint8_t {
    Boolean,
    U32,
    I64String,
    U64String,
    String,
    Object,
    Array,
};

struct FieldSchema {
    FieldSchema() = default;
    FieldSchema(std::string field_name, SchemaValueKind value_kind,
                bool is_required = true, std::string reference = {})
        : name(std::move(field_name)), kind(value_kind), required(is_required),
          referenced_type(std::move(reference)) {}

    std::string name;
    SchemaValueKind kind = SchemaValueKind::String;
    bool required = true;
    std::string referenced_type;
};

using SchemaDecodeFn = runtime::Result<std::shared_ptr<const void>, PersistenceError> (*)(
    const JsonValue& data, const void* context) noexcept;
using SchemaEncodeFn = runtime::Result<std::string, PersistenceError> (*)(
    const std::shared_ptr<const void>& value, const void* context) noexcept;
using SchemaMigrationFn = runtime::Result<std::string, PersistenceError> (*)(
    std::string_view source_envelope, const void* context) noexcept;

struct SchemaCodec {
    std::shared_ptr<const void> context;
    SchemaDecodeFn decode = nullptr;
    SchemaEncodeFn encode = nullptr;
};

struct MigrationStep {
    std::uint32_t from_version = 0;
    std::uint32_t to_version = 0;
    std::shared_ptr<const void> context;
    SchemaMigrationFn migrate = nullptr;
};

struct TypeSchema {
    std::string type_name;
    SchemaDomain domain = SchemaDomain::Document;
    std::uint32_t current_version = 0;
    std::vector<FieldSchema> fields;
    SchemaCodec codec;
    std::vector<MigrationStep> upgrades;
    std::vector<MigrationStep> downgrades;
};

enum class SchemaErrorCode : std::uint8_t {
    InvalidIdentity,
    DuplicateType,
    DuplicateField,
    InvalidCodec,
    InvalidMigration,
    DuplicateMigration,
};

struct SchemaError {
    SchemaErrorCode code = SchemaErrorCode::InvalidIdentity;
    std::string type_name;
    std::uint32_t version = 0;
};

struct SchemaRegistration {};

class SchemaRegistry {
  public:
    SchemaRegistry() = default;

    const TypeSchema* find(SchemaDomain domain, std::string_view type_name) const noexcept;
    std::span<const TypeSchema> types() const noexcept;

    runtime::Result<std::string, PersistenceError>
    migrate(SchemaDomain domain, std::string_view type_name, std::uint32_t source_version,
            std::uint32_t target_version, std::string_view source_envelope,
            const DecodeLimits& limits = {}) const;

  private:
    friend class SchemaRegistryBuilder;
    struct Impl;
    explicit SchemaRegistry(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<const Impl> impl_;
};

class SchemaRegistryBuilder {
  public:
    runtime::Result<SchemaRegistration, SchemaError> register_type(TypeSchema schema);
    runtime::Result<SchemaRegistry, SchemaError> build() &&;

  private:
    std::vector<TypeSchema> types_;
};

runtime::Result<SchemaRegistration, SchemaError>
register_builtin_timeline_schemas(SchemaRegistryBuilder& builder);
runtime::Result<SchemaRegistry, SchemaError> make_builtin_timeline_registry();

} // namespace pulp::timeline
