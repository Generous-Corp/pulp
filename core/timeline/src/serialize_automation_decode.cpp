#include "serialize_automation_decode.hpp"

#include <bit>
#include <limits>

namespace pulp::timeline::detail {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::string path,
                                          std::size_t offset = 0, std::uint64_t actual = 0,
                                          std::uint64_t limit = 0) {
    return runtime::Err(PersistenceError{code, offset, actual, limit, std::move(path)});
}

const JsonValue* member(const JsonValue& value, std::string_view name) noexcept {
    return value.kind == JsonValue::Kind::Object ? value.find(name) : nullptr;
}

runtime::Result<const JsonValue*, PersistenceError>
require_member(const JsonValue& value, std::string_view name, JsonValue::Kind kind,
               const std::string& path) {
    const auto* found = member(value, name);
    if (!found)
        return fail<const JsonValue*>(PersistenceErrorCode::MissingField,
                                      path + "/" + std::string(name), value.begin);
    if (found->kind != kind)
        return fail<const JsonValue*>(PersistenceErrorCode::UnexpectedType,
                                      path + "/" + std::string(name), found->begin);
    return runtime::Ok(found);
}

ModelError model_error(AutomationCurveError error) noexcept {
    const auto code = error.code == AutomationCurveErrorCode::InvalidPointId
                          ? ModelErrorCode::InvalidItemId
                      : error.code == AutomationCurveErrorCode::DuplicatePointId
                          ? ModelErrorCode::DuplicateItemId
                          : ModelErrorCode::InvalidSchemaIdentity;
    return {code, error.point, error.related_point};
}

ModelError model_error(AutomationLaneError error) noexcept {
    return {error.code == AutomationLaneErrorCode::InvalidLaneId
                ? ModelErrorCode::InvalidItemId
                : ModelErrorCode::MissingAutomationTarget,
            error.lane, error.related_item};
}

template <typename T>
runtime::Result<T, PersistenceError> model_fail(ModelError error, std::string path) {
    return runtime::Err(PersistenceError{PersistenceErrorCode::ModelRejected, 0, 0, 0,
                                         std::move(path), error});
}

} // namespace

runtime::Result<std::vector<AutomationLane>, PersistenceError>
decode_automation_lanes(const JsonValue& value, const DecodeLimits& limits, std::size_t& lane_count,
                        std::size_t& point_count, std::string path) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<std::vector<AutomationLane>>(PersistenceErrorCode::UnexpectedType,
                                                 std::move(path), value.begin);
    std::vector<AutomationLane> lanes;
    lanes.reserve(value.array.size());
    for (std::size_t lane_index = 0; lane_index < value.array.size(); ++lane_index) {
        const auto lane_path = path + "/" + std::to_string(lane_index);
        const auto& lane_value = value.array[lane_index];
        if (++lane_count > limits.max_automation_lanes)
            return fail<std::vector<AutomationLane>>(PersistenceErrorCode::LimitExceeded, lane_path,
                                                     lane_value.begin, lane_count,
                                                     limits.max_automation_lanes);
        auto lane_data =
            validate_exact_envelope(lane_value, "pulp.timeline.automation_lane", 1, lane_path);
        if (!lane_data)
            return runtime::Err(lane_data.error());
        auto id = require_member(*lane_data.value(), "id", JsonValue::Kind::String,
                                 lane_path + "/data");
        auto points = require_member(*lane_data.value(), "points", JsonValue::Kind::Array,
                                     lane_path + "/data");
        auto target = require_member(*lane_data.value(), "target", JsonValue::Kind::Object,
                                     lane_path + "/data");
        if (!id)
            return runtime::Err(id.error());
        if (!points)
            return runtime::Err(points.error());
        if (!target)
            return runtime::Err(target.error());
        auto decoded_id = parse_canonical_u64_string(*id.value(), lane_path + "/data/id");
        auto target_data =
            validate_exact_envelope(*target.value(),
                                    "pulp.timeline.automation_target.device_parameter", 1,
                                    lane_path + "/data/target");
        if (!decoded_id || !target_data)
            return fail<std::vector<AutomationLane>>(PersistenceErrorCode::InvalidSchema,
                                                     lane_path);
        const auto* placement = member(*target_data.value(), "device_placement_id");
        const auto* parameter = member(*target_data.value(), "parameter_id");
        if (!placement || !parameter)
            return fail<std::vector<AutomationLane>>(PersistenceErrorCode::MissingField,
                                                     lane_path + "/data/target/data");
        auto placement_id = parse_canonical_u64_string(
            *placement, lane_path + "/data/target/data/device_placement_id");
        auto parameter_id =
            parse_u32_number(*parameter, lane_path + "/data/target/data/parameter_id");
        if (!placement_id || !parameter_id)
            return fail<std::vector<AutomationLane>>(PersistenceErrorCode::InvalidNumber,
                                                     lane_path + "/data/target/data");

        std::vector<AutomationPoint> decoded_points;
        decoded_points.reserve(points.value()->array.size());
        for (std::size_t point_index = 0; point_index < points.value()->array.size();
             ++point_index) {
            const auto point_path = lane_path + "/data/points/" + std::to_string(point_index);
            const auto& point = points.value()->array[point_index];
            if (++point_count > limits.max_automation_points)
                return fail<std::vector<AutomationLane>>(PersistenceErrorCode::LimitExceeded,
                                                         point_path, point.begin, point_count,
                                                         limits.max_automation_points);
            const auto* point_id = member(point, "id");
            const auto* position = member(point, "position_ticks");
            const auto* value_bits = member(point, "value_bits");
            const auto* interpolation = member(point, "interpolation");
            const auto* curvature_bits = member(point, "curvature_bits");
            if (!point_id || !position || !value_bits || !interpolation || !curvature_bits ||
                interpolation->kind != JsonValue::Kind::String)
                return fail<std::vector<AutomationLane>>(PersistenceErrorCode::MissingField,
                                                         point_path);
            auto id_value = parse_canonical_u64_string(*point_id, point_path + "/id");
            auto position_value =
                parse_canonical_i64_string(*position, point_path + "/position_ticks");
            auto value_value = parse_canonical_u64_string(*value_bits, point_path + "/value_bits");
            auto curvature_value =
                parse_canonical_u64_string(*curvature_bits, point_path + "/curvature_bits");
            if (!id_value || !position_value || !value_value || !curvature_value ||
                value_value.value() > std::numeric_limits<std::uint32_t>::max() ||
                curvature_value.value() > std::numeric_limits<std::uint32_t>::max())
                return fail<std::vector<AutomationLane>>(PersistenceErrorCode::InvalidNumber,
                                                         point_path);
            AutomationInterpolation decoded_interpolation;
            if (interpolation->scalar == "hold")
                decoded_interpolation = AutomationInterpolation::Hold;
            else if (interpolation->scalar == "continuous")
                decoded_interpolation = AutomationInterpolation::Continuous;
            else
                return fail<std::vector<AutomationLane>>(PersistenceErrorCode::InvalidSchema,
                                                         point_path + "/interpolation");
            decoded_points.push_back(
                {{id_value.value()},
                 {position_value.value()},
                 std::bit_cast<float>(static_cast<std::uint32_t>(value_value.value())),
                 decoded_interpolation,
                 std::bit_cast<float>(static_cast<std::uint32_t>(curvature_value.value()))});
        }
        auto curve = AutomationCurve::create(std::move(decoded_points));
        if (!curve)
            return model_fail<std::vector<AutomationLane>>(model_error(curve.error()),
                                                           lane_path + "/data/points");
        auto lane = AutomationLane::create(
            {decoded_id.value()},
            DeviceParameterTarget{{placement_id.value()}, parameter_id.value()},
            std::move(curve).value());
        if (!lane)
            return model_fail<std::vector<AutomationLane>>(model_error(lane.error()), lane_path);
        lanes.push_back(std::move(lane).value());
    }
    return runtime::Ok(std::move(lanes));
}

} // namespace pulp::timeline::detail
