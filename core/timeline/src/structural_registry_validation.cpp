#include "serialize_internal.hpp"
#include "track_schema_policy.hpp"

#include <algorithm>
#include <span>
#include <string_view>

namespace pulp::timeline::detail {

namespace {

bool has_contiguous_migration_path(std::span<const MigrationStep> steps,
                                   std::uint32_t source_version,
                                   std::uint32_t target_version) noexcept {
    auto version = source_version;
    while (version != target_version) {
        const auto found = std::find_if(steps.begin(), steps.end(), [version](const auto& step) {
            return step.from_version == version;
        });
        if (found == steps.end() || !found->migrate)
            return false;
        const auto next_version = version < target_version ? version + 1 : version - 1;
        if (found->to_version != next_version)
            return false;
        version = next_version;
    }
    return true;
}

} // namespace

std::optional<PersistenceErrorCode>
validate_structural_registry(const SchemaRegistry& registry) noexcept {
    struct ExpectedField {
        std::string_view name;
        SchemaValueKind kind;
        bool required = true;
    };
    struct RequiredSchema {
        SchemaDomain domain;
        std::string_view type_name;
        std::span<const ExpectedField> fields;
        std::uint32_t current_version = 1;
        std::uint32_t oldest_readable_version = 1;
    };

    static constexpr ExpectedField project_fields[] = {
        {"assets", SchemaValueKind::Array},
        {"id", SchemaValueKind::U64String},
        {"identities", SchemaValueKind::Array, false},
        {"meter_map", SchemaValueKind::Array, false},
        {"name", SchemaValueKind::String},
        {"next_item_id", SchemaValueKind::U64String},
        {"root_sequence_id", SchemaValueKind::U64String},
        {"sequences", SchemaValueKind::Array},
        {"tempo_map", SchemaValueKind::Array, false},
    };
    static constexpr ExpectedField asset_fields[] = {
        {"content_hash", SchemaValueKind::String}, {"frame_count", SchemaValueKind::U64String},
        {"id", SchemaValueKind::U64String},        {"locators", SchemaValueKind::Array},
        {"name", SchemaValueKind::String},         {"representations", SchemaValueKind::Array},
        {"sample_rate", SchemaValueKind::Object},  {"storage_policy", SchemaValueKind::String},
    };
    static constexpr ExpectedField representation_fields[] = {
        {"content_hash", SchemaValueKind::String},
        {"locators", SchemaValueKind::Array},
        {"role", SchemaValueKind::String},
        {"storage_policy", SchemaValueKind::String},
    };
    static constexpr ExpectedField sequence_fields[] = {
        {"absolute_duration", SchemaValueKind::Object},
        {"id", SchemaValueKind::U64String},
        {"musical_duration", SchemaValueKind::I64String},
        {"name", SchemaValueKind::String},
        {"tracks", SchemaValueKind::Array},
    };
    static constexpr ExpectedField track_fields[] = {
        {"active_take_lane_id", SchemaValueKind::U64String},
        {"automation_lanes", SchemaValueKind::Array},
        {"clips", SchemaValueKind::Array},
        {"device_chain", SchemaValueKind::Array},
        {"id", SchemaValueKind::U64String},
        {"name", SchemaValueKind::String},
        {"record_armed", SchemaValueKind::Boolean},
        {"take_lanes", SchemaValueKind::Array},
    };
    static constexpr ExpectedField device_placement_fields[] = {
        {"id", SchemaValueKind::U64String},
    };
    static constexpr ExpectedField take_lane_fields[] = {
        {"id", SchemaValueKind::U64String},
        {"name", SchemaValueKind::String},
        {"takes", SchemaValueKind::Array},
    };
    static constexpr ExpectedField take_fields[] = {
        {"asset_id", SchemaValueKind::U64String}, {"frame_count", SchemaValueKind::U64String},
        {"id", SchemaValueKind::U64String},       {"placement_start", SchemaValueKind::I64String},
        {"sample_rate", SchemaValueKind::Object}, {"source_start", SchemaValueKind::I64String},
    };
    static constexpr ExpectedField automation_lane_fields[] = {
        {"id", SchemaValueKind::U64String},
        {"points", SchemaValueKind::Array},
        {"target", SchemaValueKind::Object},
    };
    static constexpr ExpectedField automation_target_fields[] = {
        {"device_placement_id", SchemaValueKind::U64String},
        {"parameter_id", SchemaValueKind::U32},
    };
    static constexpr ExpectedField clip_fields[] = {
        {"content", SchemaValueKind::Object},
        {"fade_in_duration", SchemaValueKind::U64String, false},
        {"fade_out_duration", SchemaValueKind::U64String, false},
        {"gain_linear_bits", SchemaValueKind::U64String, false},
        {"id", SchemaValueKind::U64String},
        {"time_range", SchemaValueKind::Object},
    };
    static constexpr ExpectedField media_fields[] = {
        {"asset_id", SchemaValueKind::U64String},
        {"frame_count", SchemaValueKind::U64String},
        {"source_start", SchemaValueKind::I64String},
    };
    static constexpr ExpectedField notes_fields[] = {
        {"notes", SchemaValueKind::Array},
    };
    constexpr RequiredSchema required[] = {
        {SchemaDomain::Document, "pulp.timeline.project", project_fields},
        {SchemaDomain::Document, "pulp.timeline.asset", asset_fields},
        {SchemaDomain::AssetRepresentation, "pulp.timeline.asset_representation",
         representation_fields},
        {SchemaDomain::Document, "pulp.timeline.sequence", sequence_fields},
        {SchemaDomain::Document, track_schema_policy.type_name, track_fields,
         track_schema_policy.current_version, track_schema_policy.oldest_readable_version},
        {SchemaDomain::Document, "pulp.timeline.automation_lane", automation_lane_fields},
        {SchemaDomain::Document, "pulp.timeline.automation_target.device_parameter",
         automation_target_fields},
        {SchemaDomain::Document, "pulp.timeline.device_placement", device_placement_fields},
        {SchemaDomain::Document, "pulp.timeline.take_lane", take_lane_fields},
        {SchemaDomain::Document, "pulp.timeline.take", take_fields},
        {SchemaDomain::Document, "pulp.timeline.clip", clip_fields},
        {SchemaDomain::Content, "pulp.timeline.content.empty", {}},
        {SchemaDomain::Content, "pulp.timeline.content.media", media_fields},
        {SchemaDomain::Content, "pulp.timeline.content.notes", notes_fields},
    };
    for (const auto& expected : required) {
        const auto* schema = registry.find(expected.domain, expected.type_name);
        if (!schema)
            return PersistenceErrorCode::UnsupportedStructuralType;
        if (schema->current_version != expected.current_version)
            return PersistenceErrorCode::UnsupportedSchemaVersion;
        if (!has_contiguous_migration_path(schema->upgrades, expected.oldest_readable_version,
                                           expected.current_version) ||
            !has_contiguous_migration_path(schema->downgrades, expected.current_version,
                                           expected.oldest_readable_version))
            return PersistenceErrorCode::MigrationPathMissing;
        if (schema->fields.size() != expected.fields.size())
            return PersistenceErrorCode::InvalidSchema;
        for (std::size_t index = 0; index < expected.fields.size(); ++index) {
            const auto& actual = schema->fields[index];
            const auto& field = expected.fields[index];
            if (actual.name != field.name || actual.kind != field.kind ||
                actual.required != field.required || !actual.referenced_type.empty())
                return PersistenceErrorCode::InvalidSchema;
        }
    }
    return std::nullopt;
}

} // namespace pulp::timeline::detail
