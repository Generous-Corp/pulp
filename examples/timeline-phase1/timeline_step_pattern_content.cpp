#include "timeline_step_pattern_content.hpp"

#include <pulp/timeline/schema_json.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>

namespace pulp::examples::timeline_phase1 {
namespace {

using timeline::BoundedJsonSink;
using timeline::JsonValue;
using timeline::PersistenceError;
using timeline::PersistenceErrorCode;
using timeline::SchemaWriteSuccess;

bool cells_equal(const state::StepCell& left, const state::StepCell& right) noexcept {
    return left.flags == right.flags && left.velocity == right.velocity &&
           left.probability == right.probability &&
           left.pitch_offset == right.pitch_offset &&
           left.gate_ticks == right.gate_ticks && left.ratchet == right.ratchet &&
           left.reserved == right.reserved;
}

bool cell_is_wire_valid(const state::StepCell& cell) noexcept {
    return cell.velocity <= 127 && cell.probability <= 127 && cell.reserved == 0;
}

bool lane_is_default(
    const std::array<state::StepCell, state::kStepCount>& lane) noexcept {
    const state::StepCell default_cell{};
    return std::all_of(lane.begin(), lane.end(), [&](const auto& cell) {
        return cells_equal(cell, default_cell);
    });
}

template <class T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code,
                                          std::string path = {}) {
    return runtime::Result<T, PersistenceError>(
        runtime::Err(PersistenceError{code, 0, 0, 0, std::move(path)}));
}

runtime::Result<std::uint32_t, PersistenceError>
required_u32(const JsonValue& object, std::string_view name) {
    const auto* value = object.find(name);
    if (!value)
        return fail<std::uint32_t>(PersistenceErrorCode::MissingField,
                                   "/" + std::string(name));
    return timeline::parse_u32_number(*value, "/" + std::string(name));
}

runtime::Result<std::shared_ptr<const void>, PersistenceError>
decode_step_pattern(const JsonValue& data, const void*) noexcept {
    if (data.kind != JsonValue::Kind::Object)
        return fail<std::shared_ptr<const void>>(PersistenceErrorCode::UnexpectedType);
    auto schema_version = required_u32(data, "schema_version");
    auto active_pattern = required_u32(data, "active_pattern");
    auto lane_count = required_u32(data, "active_lane_count");
    auto pattern_count = required_u32(data, "active_pattern_count");
    const auto* lengths = data.find("lengths");
    const auto* cells = data.find("cells");
    if (!schema_version || !active_pattern || !lane_count || !pattern_count ||
        !lengths || !cells || lengths->kind != JsonValue::Kind::Array ||
        cells->kind != JsonValue::Kind::Array)
        return fail<std::shared_ptr<const void>>(PersistenceErrorCode::InvalidSchema);
    if (schema_version.value() != kStepPatternSchemaVersion || lane_count.value() == 0 ||
        lane_count.value() > state::kLaneCount || pattern_count.value() == 0 ||
        pattern_count.value() > state::kPatternCount ||
        active_pattern.value() >= pattern_count.value() ||
        lengths->array.size() != pattern_count.value())
        return fail<std::shared_ptr<const void>>(PersistenceErrorCode::InvalidSchema);

    StepPatternDocument document;
    auto& snapshot = document.snapshot;
    snapshot.schema_version = schema_version.value();
    snapshot.active_pattern = static_cast<std::uint8_t>(active_pattern.value());
    snapshot.active_lane_count = static_cast<std::uint8_t>(lane_count.value());
    snapshot.active_pattern_count = static_cast<std::uint8_t>(pattern_count.value());

    for (std::size_t index = 0; index < lengths->array.size(); ++index) {
        auto length = timeline::parse_u32_number(lengths->array[index]);
        if (!length || length.value() > state::kStepCount)
            return fail<std::shared_ptr<const void>>(PersistenceErrorCode::InvalidSchema,
                                                     "/lengths");
        snapshot.patterns[index].length = static_cast<std::uint8_t>(length.value());
    }
    const auto expected_cells = static_cast<std::size_t>(pattern_count.value()) *
                                static_cast<std::size_t>(lane_count.value()) *
                                state::kStepCount;
    if (cells->array.size() != expected_cells)
        return fail<std::shared_ptr<const void>>(PersistenceErrorCode::InvalidSchema,
                                                 "/cells");

    std::size_t cell_index = 0;
    for (std::uint8_t pattern_index = 0; pattern_index < snapshot.active_pattern_count;
         ++pattern_index) {
        auto& pattern = snapshot.patterns[pattern_index];
        for (std::uint8_t lane = 0; lane < snapshot.active_lane_count; ++lane) {
            for (std::uint8_t step = 0; step < state::kStepCount; ++step) {
                const auto& encoded = cells->array[cell_index++];
                if (encoded.kind != JsonValue::Kind::Array || encoded.array.size() != 6)
                    return fail<std::shared_ptr<const void>>(
                        PersistenceErrorCode::InvalidSchema, "/cells");
                auto flags = timeline::parse_u32_number(encoded.array[0]);
                auto velocity = timeline::parse_u32_number(encoded.array[1]);
                auto probability = timeline::parse_u32_number(encoded.array[2]);
                auto pitch = timeline::parse_canonical_i64_string(encoded.array[3]);
                auto gate = timeline::parse_u32_number(encoded.array[4]);
                auto ratchet = timeline::parse_u32_number(encoded.array[5]);
                if (!flags || !velocity || !probability || !pitch || !gate || !ratchet ||
                    flags.value() > 255 || velocity.value() > 127 ||
                    probability.value() > 127 || pitch.value() < -128 ||
                    pitch.value() > 127 || gate.value() > 65535 || ratchet.value() > 255)
                    return fail<std::shared_ptr<const void>>(
                        PersistenceErrorCode::InvalidSchema, "/cells");
                auto& cell = pattern.lanes[lane][step];
                cell.flags = static_cast<std::uint8_t>(flags.value());
                cell.velocity = static_cast<std::uint8_t>(velocity.value());
                cell.probability = static_cast<std::uint8_t>(probability.value());
                cell.pitch_offset = static_cast<std::int8_t>(pitch.value());
                cell.gate_ticks = static_cast<std::uint16_t>(gate.value());
                cell.ratchet = static_cast<std::uint8_t>(ratchet.value());
            }
        }
    }
    if (!step_pattern_snapshot_is_canonical(snapshot))
        return fail<std::shared_ptr<const void>>(PersistenceErrorCode::InvalidSchema);
    std::shared_ptr<const void> result =
        std::make_shared<const StepPatternDocument>(std::move(document));
    return runtime::Result<std::shared_ptr<const void>, PersistenceError>(
        runtime::Ok(std::move(result)));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_step_pattern(const std::shared_ptr<const void>& value, BoundedJsonSink& output,
                    const void*) noexcept {
    if (!value)
        return fail<SchemaWriteSuccess>(PersistenceErrorCode::InvalidSchema);
    const auto& snapshot =
        static_cast<const StepPatternDocument*>(value.get())->snapshot;
    if (!step_pattern_snapshot_is_canonical(snapshot))
        return fail<SchemaWriteSuccess>(PersistenceErrorCode::InvalidSchema);

    const auto number = [&](std::uint32_t value) { output.append(std::to_string(value)); };
    output.append("{\"schema_version\":");
    number(kStepPatternSchemaVersion);
    output.append(",\"active_pattern\":");
    number(snapshot.active_pattern);
    output.append(",\"active_lane_count\":");
    number(snapshot.active_lane_count);
    output.append(",\"active_pattern_count\":");
    number(snapshot.active_pattern_count);
    output.append(",\"lengths\":[");
    for (std::uint8_t pattern = 0; pattern < snapshot.active_pattern_count; ++pattern) {
        if (pattern != 0)
            output.append(",");
        number(snapshot.patterns[pattern].length);
    }
    output.append("],\"cells\":[");
    bool first = true;
    for (std::uint8_t pattern_index = 0; pattern_index < snapshot.active_pattern_count;
         ++pattern_index) {
        const auto& pattern = snapshot.patterns[pattern_index];
        if (pattern.length > state::kStepCount)
            return fail<SchemaWriteSuccess>(PersistenceErrorCode::InvalidSchema);
        for (std::uint8_t lane = 0; lane < snapshot.active_lane_count; ++lane) {
            for (std::uint8_t step = 0; step < state::kStepCount; ++step) {
                if (!first)
                    output.append(",");
                first = false;
                const auto& cell = pattern.lanes[lane][step];
                output.append("[");
                number(cell.flags);
                output.append(",");
                number(cell.velocity);
                output.append(",");
                number(cell.probability);
                output.append(",\"");
                output.append(std::to_string(cell.pitch_offset));
                output.append("\",");
                number(cell.gate_ticks);
                output.append(",");
                number(cell.ratchet);
                output.append("]");
            }
        }
    }
    output.append("]}");
    return runtime::Result<SchemaWriteSuccess, PersistenceError>(
        runtime::Ok(SchemaWriteSuccess{}));
}

std::size_t retained_step_pattern(const std::shared_ptr<const void>&,
                                  const void*) noexcept {
    return sizeof(StepPatternDocument);
}

} // namespace

bool step_pattern_snapshot_is_canonical(const state::Snapshot& snapshot) noexcept {
    if (snapshot.schema_version != kStepPatternSchemaVersion ||
        snapshot.active_lane_count == 0 ||
        snapshot.active_lane_count > state::kLaneCount ||
        snapshot.active_pattern_count == 0 ||
        snapshot.active_pattern_count > state::kPatternCount ||
        snapshot.active_pattern >= snapshot.active_pattern_count)
        return false;

    for (std::uint8_t pattern_index = 0; pattern_index < state::kPatternCount;
         ++pattern_index) {
        const auto& pattern = snapshot.patterns[pattern_index];
        if (pattern_index >= snapshot.active_pattern_count) {
            if (pattern.length != state::kStepCount)
                return false;
            for (const auto& lane : pattern.lanes)
                if (!lane_is_default(lane))
                    return false;
            continue;
        }
        if (pattern.length > state::kStepCount)
            return false;
        for (std::uint8_t lane = 0; lane < state::kLaneCount; ++lane) {
            if (lane >= snapshot.active_lane_count) {
                if (!lane_is_default(pattern.lanes[lane]))
                    return false;
                continue;
            }
            for (const auto& cell : pattern.lanes[lane])
                if (!cell_is_wire_valid(cell))
                    return false;
        }
    }
    return true;
}

std::optional<timeline::SchemaRegistry> make_step_pattern_registry() {
    timeline::SchemaRegistryBuilder builder;
    if (!timeline::register_builtin_timeline_schemas(builder))
        return std::nullopt;
    timeline::TypeSchema schema;
    schema.type_name = kStepPatternSchemaName;
    schema.domain = timeline::SchemaDomain::Content;
    schema.current_version = kStepPatternSchemaVersion;
    schema.fields = {{"active_lane_count", timeline::SchemaValueKind::U32},
                     {"active_pattern", timeline::SchemaValueKind::U32},
                     {"active_pattern_count", timeline::SchemaValueKind::U32},
                     {"cells", timeline::SchemaValueKind::Array},
                     {"lengths", timeline::SchemaValueKind::Array},
                     {"schema_version", timeline::SchemaValueKind::U32}};
    schema.codec = {{}, decode_step_pattern, encode_step_pattern, retained_step_pattern};
    if (!builder.register_type(std::move(schema)))
        return std::nullopt;
    auto registry = std::move(builder).build();
    if (!registry)
        return std::nullopt;
    return std::move(registry).value();
}

std::optional<timeline::RegisteredContent>
make_registered_step_pattern(const state::Snapshot& snapshot,
                             const timeline::SchemaRegistry& registry) {
    auto value = std::make_shared<const StepPatternDocument>(StepPatternDocument{snapshot});
    auto content = registry.create_registered_no_owned_ids(
        {kStepPatternSchemaName, kStepPatternSchemaVersion}, std::move(value),
        2u * 1024u * 1024u);
    if (!content)
        return std::nullopt;
    return std::move(content).value();
}

} // namespace pulp::examples::timeline_phase1
