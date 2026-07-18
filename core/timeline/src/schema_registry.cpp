#include <pulp/timeline/schema_registry.hpp>

#include <algorithm>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, SchemaError> schema_fail(SchemaErrorCode code, std::string type_name,
                                            std::uint32_t version = 0) {
    return runtime::Result<T, SchemaError>(
        runtime::Err(SchemaError{code, std::move(type_name), version}));
}

template <typename T>
runtime::Result<T, PersistenceError> persistence_fail(PersistenceErrorCode code) {
    return runtime::Result<T, PersistenceError>(runtime::Err(PersistenceError{code}));
}

bool migration_less(const MigrationStep& lhs, const MigrationStep& rhs) noexcept {
    return lhs.from_version < rhs.from_version;
}

bool validate_steps(std::vector<MigrationStep>& steps, bool upgrading,
                    std::uint32_t current_version) {
    std::sort(steps.begin(), steps.end(), migration_less);
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto& step = steps[index];
        if (!step.migrate)
            return false;
        if (step.from_version == 0 || step.to_version == 0 ||
            step.from_version > current_version || step.to_version > current_version)
            return false;
        if (upgrading ? step.to_version != step.from_version + 1
                      : step.from_version != step.to_version + 1)
            return false;
        if (index != 0 && steps[index - 1].from_version == step.from_version)
            return false;
    }
    return true;
}

const MigrationStep* find_step(const std::vector<MigrationStep>& steps,
                               std::uint32_t from) noexcept {
    const auto found = std::lower_bound(steps.begin(), steps.end(), from,
                                        [](const MigrationStep& step, std::uint32_t value) {
                                            return step.from_version < value;
                                        });
    return found != steps.end() && found->from_version == from ? &*found : nullptr;
}

TypeSchema builtin(std::string name, SchemaDomain domain,
                   std::initializer_list<FieldSchema> fields) {
    TypeSchema schema;
    schema.type_name = std::move(name);
    schema.domain = domain;
    schema.current_version = 1;
    schema.fields.assign(fields.begin(), fields.end());
    return schema;
}

} // namespace

struct SchemaRegistry::Impl {
    std::vector<TypeSchema> types;
};

const TypeSchema* SchemaRegistry::find(SchemaDomain domain,
                                       std::string_view type_name) const noexcept {
    if (!impl_) return nullptr;
    const auto found = std::lower_bound(
        impl_->types.begin(), impl_->types.end(), std::pair(domain, type_name),
        [](const TypeSchema& schema, const auto& wanted) {
            if (schema.domain != wanted.first)
                return schema.domain < wanted.first;
            return schema.type_name < wanted.second;
        });
    return found != impl_->types.end() && found->domain == domain &&
                   found->type_name == type_name
               ? &*found
               : nullptr;
}

std::span<const TypeSchema> SchemaRegistry::types() const noexcept {
    return impl_ ? std::span<const TypeSchema>(impl_->types) : std::span<const TypeSchema>();
}

runtime::Result<std::string, PersistenceError>
SchemaRegistry::migrate(SchemaDomain domain, std::string_view type_name,
                        std::uint32_t source_version, std::uint32_t target_version,
                        std::string_view source_envelope, const DecodeLimits& limits) const {
    if (source_envelope.size() > limits.max_input_bytes)
        return persistence_fail<std::string>(PersistenceErrorCode::LimitExceeded);
    const auto* schema = find(domain, type_name);
    if (!schema)
        return persistence_fail<std::string>(PersistenceErrorCode::InvalidSchema);
    if (source_version == target_version)
        return runtime::Result<std::string, PersistenceError>(
            runtime::Ok(std::string(source_envelope)));

    std::string current(source_envelope);
    auto version = source_version;
    std::size_t steps = 0;
    while (version != target_version) {
        if (++steps > limits.max_migration_steps)
            return persistence_fail<std::string>(PersistenceErrorCode::LimitExceeded);
        const auto& candidates = version < target_version ? schema->upgrades : schema->downgrades;
        const auto* step = find_step(candidates, version);
        if (!step || (version < target_version ? step->to_version > target_version
                                               : step->to_version < target_version))
            return persistence_fail<std::string>(PersistenceErrorCode::MigrationPathMissing);
        auto migrated = step->migrate(current, step->context.get());
        if (!migrated)
            return migrated;
        if (migrated.value().size() > limits.max_input_bytes)
            return persistence_fail<std::string>(PersistenceErrorCode::LimitExceeded);
        auto parsed = parse_json(migrated.value(), limits);
        if (!parsed || parsed.value()->root().kind != JsonValue::Kind::Object)
            return persistence_fail<std::string>(PersistenceErrorCode::MigrationFailed);
        const auto* migrated_type = parsed.value()->root().find("type_name");
        const auto* migrated_version = parsed.value()->root().find("version");
        if (!migrated_type || migrated_type->kind != JsonValue::Kind::String ||
            migrated_type->scalar != type_name || !migrated_version)
            return persistence_fail<std::string>(PersistenceErrorCode::MigrationFailed);
        auto decoded_version = parse_u32_number(*migrated_version, "/version");
        if (!decoded_version || decoded_version.value() != step->to_version)
            return persistence_fail<std::string>(PersistenceErrorCode::MigrationFailed);
        current = std::move(migrated).value();
        version = step->to_version;
    }
    return runtime::Result<std::string, PersistenceError>(runtime::Ok(std::move(current)));
}

runtime::Result<SchemaRegistration, SchemaError>
SchemaRegistryBuilder::register_type(TypeSchema schema) {
    if (!SchemaIdentity{schema.type_name, schema.current_version}.valid())
        return schema_fail<SchemaRegistration>(SchemaErrorCode::InvalidIdentity,
                                               schema.type_name, schema.current_version);
    if ((schema.codec.decode == nullptr) != (schema.codec.encode == nullptr))
        return schema_fail<SchemaRegistration>(SchemaErrorCode::InvalidCodec,
                                               schema.type_name, schema.current_version);
    for (const auto& existing : types_)
        if (existing.domain == schema.domain && existing.type_name == schema.type_name)
            return schema_fail<SchemaRegistration>(SchemaErrorCode::DuplicateType,
                                                   schema.type_name, schema.current_version);
    std::sort(schema.fields.begin(), schema.fields.end(), [](const FieldSchema& lhs,
                                                             const FieldSchema& rhs) {
        return lhs.name < rhs.name;
    });
    if (std::any_of(schema.fields.begin(), schema.fields.end(), [](const FieldSchema& field) {
            return field.name.empty();
        }))
        return schema_fail<SchemaRegistration>(SchemaErrorCode::InvalidIdentity,
                                               schema.type_name, schema.current_version);
    if (std::adjacent_find(schema.fields.begin(), schema.fields.end(),
                           [](const FieldSchema& lhs, const FieldSchema& rhs) {
                               return lhs.name == rhs.name;
                           }) != schema.fields.end())
        return schema_fail<SchemaRegistration>(SchemaErrorCode::DuplicateField,
                                               schema.type_name, schema.current_version);
    if (!validate_steps(schema.upgrades, true, schema.current_version) ||
        !validate_steps(schema.downgrades, false, schema.current_version))
        return schema_fail<SchemaRegistration>(SchemaErrorCode::InvalidMigration,
                                               schema.type_name, schema.current_version);
    types_.push_back(std::move(schema));
    return runtime::Result<SchemaRegistration, SchemaError>(runtime::Ok(SchemaRegistration{}));
}

runtime::Result<SchemaRegistry, SchemaError> SchemaRegistryBuilder::build() && {
    std::sort(types_.begin(), types_.end(), [](const TypeSchema& lhs, const TypeSchema& rhs) {
        if (lhs.domain != rhs.domain) return lhs.domain < rhs.domain;
        return lhs.type_name < rhs.type_name;
    });
    auto impl = std::make_shared<SchemaRegistry::Impl>();
    impl->types = std::move(types_);
    return runtime::Result<SchemaRegistry, SchemaError>(
        runtime::Ok(SchemaRegistry(std::move(impl))));
}

runtime::Result<SchemaRegistration, SchemaError>
register_builtin_timeline_schemas(SchemaRegistryBuilder& builder) {
    std::vector<TypeSchema> schemas;
    schemas.push_back(builtin("pulp.timeline.project", SchemaDomain::Document,
                              {{"assets", SchemaValueKind::Array},
                               {"id", SchemaValueKind::U64String},
                               {"name", SchemaValueKind::String},
                               {"next_item_id", SchemaValueKind::U64String},
                               {"root_sequence_id", SchemaValueKind::U64String},
                               {"sequences", SchemaValueKind::Array}}));
    schemas.push_back(builtin("pulp.timeline.asset", SchemaDomain::Document,
                              {{"content_hash", SchemaValueKind::String},
                               {"frame_count", SchemaValueKind::U64String},
                               {"id", SchemaValueKind::U64String},
                               {"locators", SchemaValueKind::Array},
                               {"name", SchemaValueKind::String},
                               {"representations", SchemaValueKind::Array},
                               {"sample_rate", SchemaValueKind::Object},
                               {"storage_policy", SchemaValueKind::String}}));
    schemas.push_back(builtin("pulp.timeline.asset_representation",
                              SchemaDomain::AssetRepresentation,
                              {{"content_hash", SchemaValueKind::String},
                               {"locators", SchemaValueKind::Array},
                               {"role", SchemaValueKind::String},
                               {"storage_policy", SchemaValueKind::String}}));
    schemas.push_back(builtin("pulp.timeline.sequence", SchemaDomain::Document,
                              {{"absolute_duration", SchemaValueKind::Object, false},
                               {"id", SchemaValueKind::U64String},
                               {"musical_duration", SchemaValueKind::I64String, false},
                               {"name", SchemaValueKind::String},
                               {"tracks", SchemaValueKind::Array}}));
    schemas.push_back(builtin("pulp.timeline.track", SchemaDomain::Document,
                              {{"clips", SchemaValueKind::Array},
                               {"id", SchemaValueKind::U64String},
                               {"name", SchemaValueKind::String}}));
    schemas.push_back(builtin("pulp.timeline.clip", SchemaDomain::Document,
                              {{"content", SchemaValueKind::Object},
                               {"id", SchemaValueKind::U64String},
                               {"time_range", SchemaValueKind::Object}}));
    schemas.push_back(builtin("pulp.timeline.content.empty", SchemaDomain::Content, {}));
    schemas.push_back(builtin("pulp.timeline.content.media", SchemaDomain::Content,
                              {{"asset_id", SchemaValueKind::U64String},
                               {"frame_count", SchemaValueKind::U64String},
                               {"source_start", SchemaValueKind::I64String}}));
    schemas.push_back(builtin("pulp.timeline.content.notes", SchemaDomain::Content,
                              {{"notes", SchemaValueKind::Array}}));
    for (auto& schema : schemas) {
        auto result = builder.register_type(std::move(schema));
        if (!result) return result;
    }
    return runtime::Result<SchemaRegistration, SchemaError>(runtime::Ok(SchemaRegistration{}));
}

runtime::Result<SchemaRegistry, SchemaError> make_builtin_timeline_registry() {
    SchemaRegistryBuilder builder;
    auto registered = register_builtin_timeline_schemas(builder);
    if (!registered)
        return schema_fail<SchemaRegistry>(registered.error().code,
                                           registered.error().type_name,
                                           registered.error().version);
    return std::move(builder).build();
}

} // namespace pulp::timeline
