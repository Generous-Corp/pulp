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

/** @addtogroup timeline_persistence
 * @{
 */

/// Namespace in which a schema type name is unique.
enum class SchemaDomain : std::uint8_t {
    Document,
    Content,
    AssetRepresentation,
    Command,
    Diagnostic,
};

/// JSON representation declared for a registered field.
enum class SchemaValueKind : std::uint8_t {
    Boolean,
    U32,
    I64String,
    U64String,
    String,
    Object,
    Array,
};

/// Empty success value returned by bounded schema writers.
struct SchemaWriteSuccess {};

/// Append-only JSON output sink with an enforced byte ceiling.
///
/// Callback output is accepted only through this incrementally checked sink;
/// the callback ABI has no unbounded string-returning path. The first append
/// that would exceed the ceiling fails atomically, records the attempted total,
/// and makes every later append fail.
class BoundedJsonSink {
  public:
    /// Creates an empty sink with a maximum output size and overflow code.
    explicit BoundedJsonSink(std::size_t maximum, PersistenceErrorCode overflow_code =
                                                      PersistenceErrorCode::OutputLimitExceeded)
        : maximum_(maximum), overflow_code_(overflow_code) {}

    BoundedJsonSink(const BoundedJsonSink&) = delete;
    BoundedJsonSink& operator=(const BoundedJsonSink&) = delete;
    BoundedJsonSink(BoundedJsonSink&&) = delete;
    BoundedJsonSink& operator=(BoundedJsonSink&&) = delete;

    /// Appends all bytes or none of them when the ceiling would be exceeded.
    bool append(std::string_view bytes);
    /// Reports whether an append has exceeded the ceiling.
    bool failed() const noexcept {
        return failed_;
    }
    /// Returns the number of bytes accepted so far.
    std::size_t size() const noexcept {
        return output_.size();
    }
    /// Returns the configured byte ceiling.
    std::size_t maximum() const noexcept {
        return maximum_;
    }
    /// Returns the number of bytes that can still be accepted.
    std::size_t remaining() const noexcept {
        return maximum_ - output_.size();
    }
    /// Describes the recorded overflow and associates it with path.
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

/// Public field metadata for one registered schema type.
struct FieldSchema {
    /// Constructs an empty, invalid field descriptor for aggregate assembly.
    FieldSchema() = default;
    /// Constructs field metadata and takes ownership of its names.
    FieldSchema(std::string field_name, SchemaValueKind value_kind, bool is_required = true,
                std::string reference = {})
        : name(std::move(field_name)), kind(value_kind), required(is_required),
          referenced_type(std::move(reference)) {}

    std::string name;
    SchemaValueKind kind = SchemaValueKind::String;
    bool required = true;
    std::string referenced_type;
};

/// Decodes schema data into an immutable type-erased value.
///
/// The returned shared object owns its value independently of the parsed JSON.
using SchemaDecodeFn = runtime::Result<std::shared_ptr<const void>, PersistenceError> (*)(
    const JsonValue& data, const void* context) noexcept;
/// Encodes a type-erased value into a quota-enforcing JSON sink.
using SchemaEncodeFn = runtime::Result<SchemaWriteSuccess, PersistenceError> (*)(
    const std::shared_ptr<const void>& value, BoundedJsonSink& output,
    const void* context) noexcept;
/// Estimates bytes retained by a decoded type-erased value.
using SchemaRetainedSizeFn = std::size_t (*)(const std::shared_ptr<const void>& value,
                                             const void* context) noexcept;
/// Rewrites one exact schema envelope into a quota-enforcing sink.
using SchemaMigrationFn = runtime::Result<SchemaWriteSuccess, PersistenceError> (*)(
    std::string_view source_envelope, BoundedJsonSink& output, const void* context) noexcept;

/// Immutable callback bundle and shared context for a registered type.
///
/// A registry retains context for its lifetime. Decode, encode, and
/// retained_size must either all be present or all be null.
struct SchemaCodec {
    std::shared_ptr<const void> context;
    SchemaDecodeFn decode = nullptr;
    SchemaEncodeFn encode = nullptr;
    SchemaRetainedSizeFn retained_size = nullptr;
};

/// One adjacent version transition for a schema type.
///
/// Registration accepts only nonzero, one-version upgrade or downgrade edges
/// within the type's current-version range.
struct MigrationStep {
    std::uint32_t from_version = 0;
    std::uint32_t to_version = 0;
    std::shared_ptr<const void> context;
    SchemaMigrationFn migrate = nullptr;
};

/// Complete registration contract for one domain-qualified schema type.
///
/// Builder registration sorts fields and migration edges, validates callback
/// completeness, and transfers ownership of all strings, vectors, and shared
/// callback contexts into the immutable registry.
struct TypeSchema {
    std::string type_name;
    SchemaDomain domain = SchemaDomain::Document;
    std::uint32_t current_version = 0;
    std::vector<FieldSchema> fields;
    SchemaCodec codec;
    std::vector<MigrationStep> upgrades;
    std::vector<MigrationStep> downgrades;
};

/// Reason a TypeSchema cannot be registered.
enum class SchemaErrorCode : std::uint8_t {
    InvalidIdentity,
    DuplicateType,
    DuplicateField,
    InvalidCodec,
    InvalidMigration,
    DuplicateMigration,
};

/// Schema-registration failure with the affected type and version.
struct SchemaError {
    SchemaErrorCode code = SchemaErrorCode::InvalidIdentity;
    std::string type_name;
    std::uint32_t version = 0;
};

/// Empty success value returned after registering a schema.
struct SchemaRegistration {};

/// Immutable, shareable registry of Timeline schema contracts.
///
/// A default-constructed registry is empty. Registries produced by a builder
/// share immutable backing storage and support concurrent lookup. Concurrent
/// encode, decode, or migration calls additionally require the registered
/// callbacks and their shared contexts to be thread-safe.
class SchemaRegistry {
  public:
    /// Constructs an empty registry.
    SchemaRegistry() = default;

    /// Finds a type by exact domain and name.
    ///
    /// @return A pointer valid for the registry backing-store lifetime, or nullptr.
    const TypeSchema* find(SchemaDomain domain, std::string_view type_name) const noexcept;
    /// Returns all types sorted by domain and type name.
    std::span<const TypeSchema> types() const noexcept;
    /// Opaque identity shared by copies of this immutable registry.
    std::shared_ptr<const void> identity() const noexcept {
        return impl_;
    }

    /// Migrates an exact envelope along registered adjacent version edges.
    ///
    /// Each intermediate result is bounded, parsed, and validated for the
    /// expected type and version. Equal source and target versions return a copy
    /// of the validated input without canonicalizing it.
    runtime::Result<std::string, PersistenceError>
    migrate(SchemaDomain domain, std::string_view type_name, std::uint32_t source_version,
            std::uint32_t target_version, std::string_view source_envelope,
            const DecodeLimits& limits = {}) const;

    /// Encodes a current-version registered value under a byte ceiling.
    ///
    /// The returned string is the codec's output; callers that require
    /// canonical JSON should use create_registered_no_owned_ids.
    runtime::Result<std::string, PersistenceError>
    encode_registered(SchemaDomain domain, const SchemaIdentity& identity,
                      const std::shared_ptr<const void>& value, std::size_t maximum_bytes) const;
    /// Creates registered Content after bounded encode and canonical validation.
    ///
    /// The value must be non-null and use the type's current version. The
    /// returned RegisteredContent shares ownership of value and stores
    /// canonical object JSON plus a saturated retained-size estimate. This
    /// helper is for content that owns no Timeline ItemIds.
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

/// Mutable, single-owner accumulator for constructing a SchemaRegistry.
///
/// Registering and building are not thread-safe. build() consumes the builder.
class SchemaRegistryBuilder {
  public:
    /// Validates and takes ownership of one schema registration.
    ///
    /// Domain/name pairs and field names must be unique; codecs must be complete;
    /// migration edges must be unique, adjacent, and directionally valid.
    runtime::Result<SchemaRegistration, SchemaError> register_type(TypeSchema schema);
    /// Sorts the accumulated registrations and consumes them into a registry.
    runtime::Result<SchemaRegistry, SchemaError> build() &&;

  private:
    std::vector<TypeSchema> types_;
};

/// Registers the built-in document, content, command, and diagnostic schemas.
///
/// Registration stops at and returns the first builder error.
runtime::Result<SchemaRegistration, SchemaError>
register_builtin_timeline_schemas(SchemaRegistryBuilder& builder);
/// Constructs a registry containing exactly the built-in Timeline schemas.
runtime::Result<SchemaRegistry, SchemaError> make_builtin_timeline_registry();

/// @}

} // namespace pulp::timeline
