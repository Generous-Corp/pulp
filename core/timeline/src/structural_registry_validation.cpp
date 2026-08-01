#include "asset_schema_policy.hpp"
#include "clip_schema_policy.hpp"
#include "project_schema_policy.hpp"
#include "sequence_schema_policy.hpp"
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
        constexpr ExpectedField(std::string_view field_name, SchemaValueKind field_kind,
                                bool is_required = true,
                                std::string_view reference = {}) noexcept
            : name(field_name), kind(field_kind), required(is_required),
              referenced_type(reference) {}

        std::string_view name;
        SchemaValueKind kind;
        bool required = true;
        std::string_view referenced_type;
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
        {"session_start", SchemaValueKind::Object, false},
        {"tempo_map", SchemaValueKind::Array, false},
    };
    static constexpr ExpectedField asset_fields[] = {
        {"content_hash", SchemaValueKind::String}, {"frame_count", SchemaValueKind::U64String},
        {"id", SchemaValueKind::U64String},        {"locators", SchemaValueKind::Array},
        {"loop_info", SchemaValueKind::Object, false},
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
        {"chord_scale_lane", SchemaValueKind::Array},
        {"groove", SchemaValueKind::Object, true, "pulp.timeline.groove_template"},
        {"id", SchemaValueKind::U64String},
        {"markers", SchemaValueKind::Array},
        {"musical_duration", SchemaValueKind::I64String},
        {"name", SchemaValueKind::String},
        {"regions", SchemaValueKind::Array},
        {"scenes", SchemaValueKind::Array},
        {"track_order", SchemaValueKind::Array},
        {"tracks", SchemaValueKind::Array},
    };
    static constexpr ExpectedField chord_scale_event_fields[] = {
        {"chord_quality", SchemaValueKind::String},
        {"chord_root", SchemaValueKind::U32},
        {"position", SchemaValueKind::I64String},
        {"scale_mode", SchemaValueKind::String},
        {"scale_root", SchemaValueKind::U32},
    };
    static constexpr ExpectedField groove_template_fields[] = {
        {"name", SchemaValueKind::String},
        {"step", SchemaValueKind::I64String},
        {"steps", SchemaValueKind::Array, true, "pulp.timeline.groove_step"},
        {"swing_denominator", SchemaValueKind::I64String},
        {"swing_grid", SchemaValueKind::I64String},
        {"swing_numerator", SchemaValueKind::I64String},
        {"timing_strength", SchemaValueKind::U32},
        {"velocity_strength", SchemaValueKind::U32},
    };
    static constexpr ExpectedField groove_step_fields[] = {
        {"timing_offset", SchemaValueKind::I64String},
        {"velocity_scale", SchemaValueKind::U32},
    };
    static constexpr ExpectedField marker_fields[] = {
        {"color", SchemaValueKind::U32, false},
        {"id", SchemaValueKind::U64String},
        {"name", SchemaValueKind::String},
        {"position", SchemaValueKind::I64String},
    };
    static constexpr ExpectedField region_fields[] = {
        {"color", SchemaValueKind::U32, false},   {"duration", SchemaValueKind::I64String},
        {"id", SchemaValueKind::U64String},       {"name", SchemaValueKind::String},
        {"position", SchemaValueKind::I64String},
    };
    static constexpr ExpectedField scene_fields[] = {
        {"id", SchemaValueKind::U64String},
        {"name", SchemaValueKind::String},
        {"slots", SchemaValueKind::Array},
    };
    static constexpr ExpectedField slot_fields[] = {
        {"clip_id", SchemaValueKind::U64String},
        {"follow", SchemaValueKind::Object},
        {"id", SchemaValueKind::U64String},
        {"launch_quantize", SchemaValueKind::Object},
    };
    static constexpr ExpectedField track_fields[] = {
        {"active_take_lane_id", SchemaValueKind::U64String},
        {"automation_lanes", SchemaValueKind::Array},
        {"clips", SchemaValueKind::Array},
        {"device_chain", SchemaValueKind::Array},
        {"freeze", SchemaValueKind::Object, false},
        {"id", SchemaValueKind::U64String},
        {"mixer", SchemaValueKind::Object, false},
        {"name", SchemaValueKind::String},
        {"record_armed", SchemaValueKind::Boolean},
        {"take_lanes", SchemaValueKind::Array},
    };
    static constexpr ExpectedField device_placement_fields[] = {
        {"id", SchemaValueKind::U64String},
    };
    static constexpr ExpectedField take_lane_fields[] = {
        {"comp_segments", SchemaValueKind::Array},
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
    static constexpr ExpectedField track_mixer_target_fields[] = {
        {"parameter", SchemaValueKind::String},
    };
    static constexpr ExpectedField clip_fields[] = {
        {"content", SchemaValueKind::Object},
        {"fade_in_duration", SchemaValueKind::U64String, false},
        {"fade_out_duration", SchemaValueKind::U64String, false},
        {"fade_shape", SchemaValueKind::String},
        {"gain_linear_bits", SchemaValueKind::U64String, false},
        {"id", SchemaValueKind::U64String},
        {"time_conform", SchemaValueKind::String},
        {"time_range", SchemaValueKind::Object},
    };
    static constexpr ExpectedField media_fields[] = {
        {"asset_id", SchemaValueKind::U64String},
        {"frame_count", SchemaValueKind::U64String},
        {"source_start", SchemaValueKind::I64String},
    };
    static constexpr ExpectedField notes_fields[] = {
        {"lanes", SchemaValueKind::Array},
        {"modifier_seed", SchemaValueKind::U64String},
        {"modifiers", SchemaValueKind::Array},
        {"notes", SchemaValueKind::Array},
    };
    static constexpr ExpectedField sequence_ref_fields[] = {
        {"sequence_id", SchemaValueKind::U64String},
        {"source_start", SchemaValueKind::I64String},
    };
    constexpr RequiredSchema required[] = {
        {SchemaDomain::Document, project_schema_policy.type_name, project_fields,
         project_schema_policy.current_version, project_schema_policy.oldest_readable_version},
        {SchemaDomain::Document, asset_schema_policy.type_name, asset_fields,
         asset_schema_policy.current_version, asset_schema_policy.oldest_readable_version},
        {SchemaDomain::AssetRepresentation, "pulp.timeline.asset_representation",
         representation_fields},
        {SchemaDomain::Document, sequence_schema_policy.type_name, sequence_fields,
         sequence_schema_policy.current_version, sequence_schema_policy.oldest_readable_version},
        {SchemaDomain::Document, "pulp.timeline.chord_scale_event", chord_scale_event_fields},
        {SchemaDomain::Document, "pulp.timeline.groove_template", groove_template_fields},
        {SchemaDomain::Document, "pulp.timeline.groove_step", groove_step_fields},
        {SchemaDomain::Document, "pulp.timeline.marker", marker_fields},
        {SchemaDomain::Document, "pulp.timeline.region", region_fields},
        {SchemaDomain::Document, "pulp.timeline.scene", scene_fields},
        {SchemaDomain::Document, "pulp.timeline.slot", slot_fields},
        {SchemaDomain::Document, track_schema_policy.type_name, track_fields,
         track_schema_policy.current_version, track_schema_policy.oldest_readable_version},
        {SchemaDomain::Document, "pulp.timeline.automation_lane", automation_lane_fields},
        {SchemaDomain::Document, "pulp.timeline.automation_target.device_parameter",
         automation_target_fields},
        {SchemaDomain::Document, "pulp.timeline.automation_target.track_mixer",
         track_mixer_target_fields},
        {SchemaDomain::Document, "pulp.timeline.device_placement", device_placement_fields},
        {SchemaDomain::Document, "pulp.timeline.take_lane", take_lane_fields, 2, 1},
        {SchemaDomain::Document, "pulp.timeline.take", take_fields},
        {SchemaDomain::Document, clip_schema_policy.type_name, clip_fields,
         clip_schema_policy.current_version, clip_schema_policy.oldest_readable_version},
        {SchemaDomain::Content, "pulp.timeline.content.empty", {}},
        {SchemaDomain::Content, "pulp.timeline.content.media", media_fields},
        {SchemaDomain::Content, "pulp.timeline.content.notes", notes_fields, 3, 1},
        {SchemaDomain::Content, "pulp.timeline.content.sequence_ref", sequence_ref_fields},
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
                actual.required != field.required ||
                actual.referenced_type != field.referenced_type)
                return PersistenceErrorCode::InvalidSchema;
        }
    }
    return std::nullopt;
}

} // namespace pulp::timeline::detail
