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

struct SchemaWriteSuccess {};

// Callback output is accepted only through this incrementally checked sink.
// The callback ABI deliberately has no string-returning compatibility path.
class BoundedJsonSink {
  public:
    explicit BoundedJsonSink(std::size_t maximum, PersistenceErrorCode overflow_code =
                                                      PersistenceErrorCode::OutputLimitExceeded)
        : maximum_(maximum), overflow_code_(overflow_code) {}

    BoundedJsonSink(const BoundedJsonSink&) = delete;
    BoundedJsonSink& operator=(const BoundedJsonSink&) = delete;
    BoundedJsonSink(BoundedJsonSink&&) = delete;
    BoundedJsonSink& operator=(BoundedJsonSink&&) = delete;

    bool append(std::string_view bytes);
    bool failed() const noexcept {
        return failed_;
    }
    std::size_t size() const noexcept {
        return output_.size();
    }
    std::size_t maximum() const noexcept {
        return maximum_;
    }
    std::size_t remaining() const noexcept {
        return maximum_ - output_.size();
    }
    PersistenceError error(std::string path = {}) const;

  private:
    friend class SchemaRegistry;
    const std::string& stored_output() const noexcept {
        return output_;
    }
    const std::size_t maximum_ = 0;
    const PersistenceErrorCode overflow_code_ = PersistenceErrorCode::OutputLimitExceeded;
    std::string output_;
    bool failed_ = false;
    std::uint64_t attempted_ = 0;
};

struct FieldSchema {
    FieldSchema() = default;
    FieldSchema(std::string field_name, SchemaValueKind value_kind, bool is_required = true,
                std::string reference = {})
        : name(std::move(field_name)), kind(value_kind), required(is_required),
          referenced_type(std::move(reference)) {}

    std::string name;
    SchemaValueKind kind = SchemaValueKind::String;
    bool required = true;
    std::string referenced_type;
};

using SchemaDecodeFn = runtime::Result<std::shared_ptr<const void>, PersistenceError> (*)(
    const JsonValue& data, const void* context) noexcept;
using SchemaEncodeFn = runtime::Result<SchemaWriteSuccess, PersistenceError> (*)(
    const std::shared_ptr<const void>& value, BoundedJsonSink& output,
    const void* context) noexcept;
using SchemaRetainedSizeFn = std::size_t (*)(const std::shared_ptr<const void>& value,
                                             const void* context) noexcept;
using SchemaMigrationFn = runtime::Result<SchemaWriteSuccess, PersistenceError> (*)(
    std::string_view source_envelope, BoundedJsonSink& output, const void* context) noexcept;

struct SchemaCodec {
    std::shared_ptr<const void> context;
    SchemaDecodeFn decode = nullptr;
    SchemaEncodeFn encode = nullptr;
    SchemaRetainedSizeFn retained_size = nullptr;
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

    runtime::Result<std::string, PersistenceError>
    encode_registered(SchemaDomain domain, const SchemaIdentity& identity,
                      const std::shared_ptr<const void>& value, std::size_t maximum_bytes) const;
    runtime::Result<RegisteredContent, PersistenceError>
    create_registered_no_owned_ids(const SchemaIdentity& identity,
                                   std::shared_ptr<const void> value,
                                   std::size_t maximum_bytes) const;

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
