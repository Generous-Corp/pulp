#include <pulp/timeline/schema_registry.hpp>

#include "track_schema_migrations.hpp"
#include "track_schema_policy.hpp"
#include "take_lane_schema_migrations.hpp"

#include <algorithm>
#include <limits>

namespace pulp::timeline {

bool BoundedJsonSink::append(std::string_view bytes) {
    if (failed_)
        return false;
    if (bytes.size() > remaining()) {
        failed_ = true;
        const auto room = std::numeric_limits<std::uint64_t>::max() - output_.size();
        attempted_ = bytes.size() > room ? std::numeric_limits<std::uint64_t>::max()
                                         : output_.size() + bytes.size();
        return false;
    }
    output_.append(bytes);
    return true;
}

PersistenceError BoundedJsonSink::error(std::string path) const {
    return PersistenceError{overflow_code_, 0, attempted_, maximum_, std::move(path)};
}

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
        if (step.from_version == 0 || step.to_version == 0 || step.from_version > current_version ||
            step.to_version > current_version)
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
    const auto found = std::lower_bound(
        steps.begin(), steps.end(), from,
        [](const MigrationStep& step, std::uint32_t value) { return step.from_version < value; });
    return found != steps.end() && found->from_version == from ? &*found : nullptr;
}

TypeSchema builtin(std::string name, SchemaDomain domain, std::initializer_list<FieldSchema> fields,
                   std::uint32_t version = 1) {
    TypeSchema schema;
    schema.type_name = std::move(name);
    schema.domain = domain;
    schema.current_version = version;
    schema.fields.assign(fields.begin(), fields.end());
    return schema;
}

} // namespace

struct SchemaRegistry::Impl {
    std::vector<TypeSchema> types;
};

const TypeSchema* SchemaRegistry::find(SchemaDomain domain,
                                       std::string_view type_name) const noexcept {
    if (!impl_)
        return nullptr;
    const auto found =
        std::lower_bound(impl_->types.begin(), impl_->types.end(), std::pair(domain, type_name),
                         [](const TypeSchema& schema, const auto& wanted) {
                             if (schema.domain != wanted.first)
                                 return schema.domain < wanted.first;
                             return schema.type_name < wanted.second;
                         });
    return found != impl_->types.end() && found->domain == domain && found->type_name == type_name
               ? &*found
               : nullptr;
}

std::span<const TypeSchema> SchemaRegistry::types() const noexcept {
    return impl_ ? std::span<const TypeSchema>(impl_->types) : std::span<const TypeSchema>();
}

runtime::Result<std::string, PersistenceError>
SchemaRegistry::encode_registered(SchemaDomain domain, const SchemaIdentity& identity,
                                  const std::shared_ptr<const void>& value,
                                  std::size_t maximum_bytes) const {
    const auto* schema = find(domain, identity.type_name);
    if (!schema || !schema->codec.encode || identity.version != schema->current_version)
        return persistence_fail<std::string>(PersistenceErrorCode::InvalidSchema);
    BoundedJsonSink sink(maximum_bytes, PersistenceErrorCode::OutputLimitExceeded);
    auto encoded = schema->codec.encode(value, sink, schema->codec.context.get());
    if (sink.failed())
        return runtime::Result<std::string, PersistenceError>(runtime::Err(sink.error()));
    if (!encoded)
        return runtime::Result<std::string, PersistenceError>(runtime::Err(encoded.error()));
    return runtime::Result<std::string, PersistenceError>(
        runtime::Ok(std::string(sink.stored_output())));
}

runtime::Result<RegisteredContent, PersistenceError>
SchemaRegistry::create_registered_no_owned_ids(const SchemaIdentity& identity,
                                               std::shared_ptr<const void> value,
                                               std::size_t maximum_bytes) const {
    const auto* schema = find(SchemaDomain::Content, identity.type_name);
    if (!schema || identity.version != schema->current_version || !value || !schema->codec.encode ||
        !schema->codec.retained_size)
        return persistence_fail<RegisteredContent>(PersistenceErrorCode::InvalidSchema);
    auto encoded = encode_registered(SchemaDomain::Content, identity, value, maximum_bytes);
    if (!encoded)
        return runtime::Result<RegisteredContent, PersistenceError>(runtime::Err(encoded.error()));
    DecodeLimits limits;
    limits.max_input_bytes = maximum_bytes;
    auto parsed = parse_json(encoded.value(), limits);
    if (!parsed || parsed.value()->root().kind != JsonValue::Kind::Object)
        return persistence_fail<RegisteredContent>(PersistenceErrorCode::InvalidSchema);
    auto canonical = canonicalize_json(parsed.value()->root());
    if (!canonical || canonical.value().size() > maximum_bytes)
        return persistence_fail<RegisteredContent>(PersistenceErrorCode::OutputLimitExceeded);
    const auto payload_bytes = schema->codec.retained_size(value, schema->codec.context.get());
    const auto retained =
        payload_bytes > std::numeric_limits<std::size_t>::max() - canonical.value().size()
            ? std::numeric_limits<std::size_t>::max()
            : payload_bytes + canonical.value().size();
    return runtime::Ok(
        RegisteredContent(identity, std::move(value), std::move(canonical).value(), retained));
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
    auto initial = parse_json(source_envelope, limits);
    if (!initial)
        return runtime::Result<std::string, PersistenceError>(runtime::Err(initial.error()));
    auto initial_envelope =
        validate_exact_envelope(initial.value()->root(), type_name, source_version, "",
                                PersistenceErrorCode::InvalidSchema);
    if (!initial_envelope)
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
        BoundedJsonSink sink(limits.max_input_bytes, PersistenceErrorCode::LimitExceeded);
        auto migrated = step->migrate(current, sink, step->context.get());
        if (sink.failed())
            return runtime::Result<std::string, PersistenceError>(runtime::Err(sink.error()));
        if (!migrated)
            return runtime::Result<std::string, PersistenceError>(runtime::Err(migrated.error()));
        std::string next(sink.stored_output());
        auto parsed = parse_json(next, limits);
        if (!parsed)
            return persistence_fail<std::string>(PersistenceErrorCode::MigrationFailed);
        auto next_envelope =
            validate_exact_envelope(parsed.value()->root(), type_name, step->to_version, "",
                                    PersistenceErrorCode::MigrationFailed);
        if (!next_envelope)
            return persistence_fail<std::string>(PersistenceErrorCode::MigrationFailed);
        current = std::move(next);
        version = step->to_version;
    }
    return runtime::Result<std::string, PersistenceError>(runtime::Ok(std::move(current)));
}

runtime::Result<SchemaRegistration, SchemaError>
SchemaRegistryBuilder::register_type(TypeSchema schema) {
    if (!SchemaIdentity{schema.type_name, schema.current_version}.valid())
        return schema_fail<SchemaRegistration>(SchemaErrorCode::InvalidIdentity, schema.type_name,
                                               schema.current_version);
    const auto codec_members = static_cast<unsigned>(schema.codec.decode != nullptr) +
                               static_cast<unsigned>(schema.codec.encode != nullptr) +
                               static_cast<unsigned>(schema.codec.retained_size != nullptr);
    if (codec_members != 0 && codec_members != 3)
        return schema_fail<SchemaRegistration>(SchemaErrorCode::InvalidCodec, schema.type_name,
                                               schema.current_version);
    for (const auto& existing : types_)
        if (existing.domain == schema.domain && existing.type_name == schema.type_name)
            return schema_fail<SchemaRegistration>(SchemaErrorCode::DuplicateType, schema.type_name,
                                                   schema.current_version);
    std::sort(schema.fields.begin(), schema.fields.end(),
              [](const FieldSchema& lhs, const FieldSchema& rhs) { return lhs.name < rhs.name; });
    if (std::any_of(schema.fields.begin(), schema.fields.end(),
                    [](const FieldSchema& field) { return field.name.empty(); }))
        return schema_fail<SchemaRegistration>(SchemaErrorCode::InvalidIdentity, schema.type_name,
                                               schema.current_version);
    if (std::adjacent_find(schema.fields.begin(), schema.fields.end(),
                           [](const FieldSchema& lhs, const FieldSchema& rhs) {
                               return lhs.name == rhs.name;
                           }) != schema.fields.end())
        return schema_fail<SchemaRegistration>(SchemaErrorCode::DuplicateField, schema.type_name,
                                               schema.current_version);
    if (!validate_steps(schema.upgrades, true, schema.current_version) ||
        !validate_steps(schema.downgrades, false, schema.current_version))
        return schema_fail<SchemaRegistration>(SchemaErrorCode::InvalidMigration, schema.type_name,
                                               schema.current_version);
    types_.push_back(std::move(schema));
    return runtime::Result<SchemaRegistration, SchemaError>(runtime::Ok(SchemaRegistration{}));
}

runtime::Result<SchemaRegistry, SchemaError> SchemaRegistryBuilder::build() && {
    std::sort(types_.begin(), types_.end(), [](const TypeSchema& lhs, const TypeSchema& rhs) {
        if (lhs.domain != rhs.domain)
            return lhs.domain < rhs.domain;
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
                               {"identities", SchemaValueKind::Array, false},
                               {"meter_map", SchemaValueKind::Array, false},
                               {"name", SchemaValueKind::String},
                               {"next_item_id", SchemaValueKind::U64String},
                               {"root_sequence_id", SchemaValueKind::U64String},
                               {"sequences", SchemaValueKind::Array},
                               {"tempo_map", SchemaValueKind::Array, false}}));
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
                              {{"absolute_duration", SchemaValueKind::Object},
                               {"id", SchemaValueKind::U64String},
                               {"musical_duration", SchemaValueKind::I64String},
                               {"name", SchemaValueKind::String},
                               {"tracks", SchemaValueKind::Array}}));
    auto track = builtin(std::string(detail::track_schema_policy.type_name), SchemaDomain::Document,
                         {{"active_take_lane_id", SchemaValueKind::U64String},
                          {"automation_lanes", SchemaValueKind::Array},
                          {"clips", SchemaValueKind::Array},
                          {"device_chain", SchemaValueKind::Array},
                          {"freeze", SchemaValueKind::Object, false},
                          {"id", SchemaValueKind::U64String},
                          {"name", SchemaValueKind::String},
                          {"record_armed", SchemaValueKind::Boolean},
                          {"take_lanes", SchemaValueKind::Array}},
                         detail::track_schema_policy.current_version);
    track.upgrades.push_back({1, 2, {}, detail::migrate_track_v1_to_v2});
    track.upgrades.push_back({2, 3, {}, detail::migrate_track_v2_to_v3});
    track.upgrades.push_back({3, 4, {}, detail::migrate_track_v3_to_v4});
    track.upgrades.push_back({4, 5, {}, detail::migrate_track_v4_to_v5});
    track.upgrades.push_back({5, 6, {}, detail::migrate_track_v5_to_v6});
    track.downgrades.push_back({6, 5, {}, detail::migrate_track_v6_to_v5});
    track.downgrades.push_back({5, 4, {}, detail::migrate_track_v5_to_v4});
    track.downgrades.push_back({4, 3, {}, detail::migrate_track_v4_to_v3});
    track.downgrades.push_back({3, 2, {}, detail::migrate_track_v3_to_v2});
    track.downgrades.push_back({2, 1, {}, detail::migrate_track_v2_to_v1});
    schemas.push_back(std::move(track));
    schemas.push_back(builtin("pulp.timeline.automation_lane", SchemaDomain::Document,
                              {{"id", SchemaValueKind::U64String},
                               {"points", SchemaValueKind::Array},
                               {"target", SchemaValueKind::Object}}));
    schemas.push_back(builtin("pulp.timeline.automation_target.device_parameter",
                              SchemaDomain::Document,
                              {{"device_placement_id", SchemaValueKind::U64String},
                               {"parameter_id", SchemaValueKind::U32}}));
    schemas.push_back(builtin("pulp.timeline.device_placement", SchemaDomain::Document,
                              {{"id", SchemaValueKind::U64String}}));
    auto take_lane = builtin("pulp.timeline.take_lane", SchemaDomain::Document,
                             {{"comp_segments", SchemaValueKind::Array},
                              {"id", SchemaValueKind::U64String},
                              {"name", SchemaValueKind::String},
                              {"takes", SchemaValueKind::Array}},
                             2);
    take_lane.upgrades.push_back({1, 2, {}, detail::migrate_take_lane_v1_to_v2});
    take_lane.downgrades.push_back({2, 1, {}, detail::migrate_take_lane_v2_to_v1});
    schemas.push_back(std::move(take_lane));
    schemas.push_back(builtin("pulp.timeline.take", SchemaDomain::Document,
                              {{"asset_id", SchemaValueKind::U64String},
                               {"frame_count", SchemaValueKind::U64String},
                               {"id", SchemaValueKind::U64String},
                               {"placement_start", SchemaValueKind::I64String},
                               {"sample_rate", SchemaValueKind::Object},
                               {"source_start", SchemaValueKind::I64String}}));
    schemas.push_back(builtin("pulp.timeline.clip", SchemaDomain::Document,
                              {{"content", SchemaValueKind::Object},
                               {"fade_in_duration", SchemaValueKind::U64String, false},
                               {"fade_out_duration", SchemaValueKind::U64String, false},
                               {"gain_linear_bits", SchemaValueKind::U64String, false},
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
        if (!result)
            return result;
    }
    return runtime::Result<SchemaRegistration, SchemaError>(runtime::Ok(SchemaRegistration{}));
}

runtime::Result<SchemaRegistry, SchemaError> make_builtin_timeline_registry() {
    SchemaRegistryBuilder builder;
    auto registered = register_builtin_timeline_schemas(builder);
    if (!registered)
        return schema_fail<SchemaRegistry>(registered.error().code, registered.error().type_name,
                                           registered.error().version);
    return std::move(builder).build();
}

} // namespace pulp::timeline
