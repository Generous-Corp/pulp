#include <pulp/timeline/schema_json.hpp>

#include "schema_json_preflight_internal.hpp"

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::size_t offset,
                                          std::string path) {
    return runtime::Err(PersistenceError{code, offset, 0, 0, std::move(path), std::nullopt});
}

runtime::Result<JsonValue, PersistenceError> decode_scalar(std::string_view json,
                                                           std::string_view scalar,
                                                           const DecodeLimits& limits,
                                                           std::string path) {
    DecodeLimits scalar_limits;
    scalar_limits.max_input_bytes = scalar.size();
    scalar_limits.max_depth = 1;
    scalar_limits.max_total_values = 1;
    scalar_limits.max_array_elements = 0;
    scalar_limits.max_object_members = 0;
    scalar_limits.max_string_bytes = limits.max_string_bytes;
    auto parsed = parse_json(scalar, scalar_limits);
    const auto offset = static_cast<std::size_t>(scalar.data() - json.data());
    if (!parsed)
        return runtime::Err(PersistenceError{
            parsed.error().code,
            offset + parsed.error().byte_offset,
            parsed.error().actual,
            parsed.error().limit,
            std::move(path),
        });
    return runtime::Ok(parsed.value()->root());
}

runtime::Result<std::uint64_t, PersistenceError> decode_item_id(std::string_view json,
                                                                std::string_view scalar,
                                                                const JsonValue& value,
                                                                std::string path) {
    auto parsed = parse_canonical_u64_string(value, std::move(path));
    if (parsed)
        return parsed;
    auto error = parsed.error();
    error.byte_offset += static_cast<std::size_t>(scalar.data() - json.data());
    return runtime::Err(std::move(error));
}

} // namespace

runtime::Result<ProjectSummary, PersistenceError> peek_project_summary(std::string_view json,
                                                                       const DecodeLimits& limits) {
    auto scanned = detail::scan_project_summary(json, limits);
    if (!scanned)
        return runtime::Err(scanned.error());
    auto id = decode_scalar(json, scanned->id, limits, "/data/id");
    auto name = decode_scalar(json, scanned->name, limits, "/data/name");
    auto next = decode_scalar(json, scanned->next_item_id, limits, "/data/next_item_id");
    auto root = decode_scalar(json, scanned->root_sequence_id, limits, "/data/root_sequence_id");
    if (!id)
        return runtime::Err(id.error());
    if (!name)
        return runtime::Err(name.error());
    if (!next)
        return runtime::Err(next.error());
    if (!root)
        return runtime::Err(root.error());
    auto decoded_id = decode_item_id(json, scanned->id, id.value(), "/data/id");
    auto decoded_next =
        decode_item_id(json, scanned->next_item_id, next.value(), "/data/next_item_id");
    auto decoded_root =
        decode_item_id(json, scanned->root_sequence_id, root.value(), "/data/root_sequence_id");
    if (!decoded_id)
        return runtime::Err(decoded_id.error());
    if (!decoded_next)
        return runtime::Err(decoded_next.error());
    if (!decoded_root)
        return runtime::Err(decoded_root.error());
    if (name.value().kind != JsonValue::Kind::String) {
        const auto offset = static_cast<std::size_t>(scanned->name.data() - json.data());
        return fail<ProjectSummary>(PersistenceErrorCode::UnexpectedType, offset, "/data/name");
    }
    return runtime::Ok(ProjectSummary{
        ItemId{decoded_id.value()},
        std::move(name).value().scalar,
        decoded_next.value(),
        ItemId{decoded_root.value()},
        scanned->project_schema_version,
        scanned->asset_count,
        scanned->sequence_count,
        scanned->track_count,
        scanned->clip_count,
        scanned->note_count,
        scanned->device_placement_count,
        scanned->automation_lane_count,
        scanned->automation_point_count,
        scanned->take_lane_count,
        scanned->take_count,
        scanned->take_comp_segment_count,
    });
}

} // namespace pulp::timeline
