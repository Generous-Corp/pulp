#include "serialize_internal.hpp"

#include <span>
#include <string_view>

namespace pulp::timeline::detail {

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
        bool requires_round_trip_migration = false;
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
        {"clips", SchemaValueKind::Array},
        {"device_chain", SchemaValueKind::Array},
        {"id", SchemaValueKind::U64String},
        {"name", SchemaValueKind::String},
    };
    static constexpr ExpectedField device_placement_fields[] = {
        {"id", SchemaValueKind::U64String},
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
        {SchemaDomain::Document, "pulp.timeline.track", track_fields, 2, true},
        {SchemaDomain::Document, "pulp.timeline.device_placement", device_placement_fields},
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
        if (expected.requires_round_trip_migration &&
            (schema->upgrades.size() != 1 || schema->downgrades.size() != 1 ||
             schema->upgrades[0].from_version != 1 || schema->upgrades[0].to_version != 2 ||
             !schema->upgrades[0].migrate || schema->downgrades[0].from_version != 2 ||
             schema->downgrades[0].to_version != 1 || !schema->downgrades[0].migrate))
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
