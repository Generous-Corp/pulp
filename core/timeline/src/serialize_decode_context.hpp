#pragma once

#include "bounded_increment.hpp"
#include "serialize_decode_support.hpp"

#include <pulp/timeline/serialize.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace pulp::timeline::detail {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::string path = {},
                                          std::size_t byte_offset = 0, std::uint64_t actual = 0,
                                          std::uint64_t limit = 0) {
    return decode_fail<T>(code, std::move(path), byte_offset, actual, limit);
}

template <typename T>
runtime::Result<T, PersistenceError> model_fail(ModelError error, std::string path) {
    PersistenceError failure;
    failure.code = PersistenceErrorCode::ModelRejected;
    failure.path = std::move(path);
    failure.model_error = error;
    return runtime::Result<T, PersistenceError>(runtime::Err(std::move(failure)));
}

struct DecodeContext {
    explicit DecodeContext(const DecodeLimits& bounded_limits) noexcept : limits(bounded_limits) {}

    const DecodeLimits& limits;
    ProjectSnapshotCounts counts;
    std::size_t audio_loop_points = 0;
    std::size_t audio_loop_tags = 0;
};

struct StructuralData {
    const JsonValue* data = nullptr;
    std::uint32_t version = 0;
};

runtime::Result<ItemKind, PersistenceError> decode_item_kind(std::string_view value,
                                                             std::string path);
runtime::Result<const JsonValue*, PersistenceError>
required(const JsonValue& object_value, std::string_view name, std::string path);
runtime::Result<std::string, PersistenceError>
string_field(const JsonValue& object_value, std::string_view name, std::string path);
runtime::Result<StructuralData, PersistenceError>
data_for_versions(const JsonValue& value, std::string_view expected_type,
                  std::uint32_t minimum_version, std::uint32_t maximum_version, std::string path);
runtime::Result<const JsonValue*, PersistenceError>
data_for(const JsonValue& value, std::string_view expected_type, std::string path);
runtime::Result<timebase::RationalRate, PersistenceError> decode_rate(const JsonValue& value,
                                                                      std::string path);
runtime::Result<timebase::TempoMap, PersistenceError> decode_tempo_map(const JsonValue& value,
                                                                       std::string path);
runtime::Result<timebase::MeterMap, PersistenceError> decode_meter_map(const JsonValue& value,
                                                                       std::string path);

runtime::Result<MediaAsset, PersistenceError> decode_asset(const JsonValue& value, std::string path,
                                                           DecodeContext& context);
runtime::Result<Clip, PersistenceError>
decode_clip(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
            const SchemaRegistry& registry, DecodeContext& context, std::string path);
runtime::Result<Take, PersistenceError> decode_take(const JsonValue& value, DecodeContext& context,
                                                    std::string path);
runtime::Result<TakeLane, PersistenceError>
decode_take_lane(const JsonValue& value, DecodeContext& context, std::string path);
runtime::Result<SequenceMarker, PersistenceError>
decode_marker(const JsonValue& value, DecodeContext& context, std::string path);
runtime::Result<SequenceRegion, PersistenceError>
decode_region(const JsonValue& value, DecodeContext& context, std::string path);
runtime::Result<Scene, PersistenceError>
decode_scene(const JsonValue& value, DecodeContext& context, std::string path);
runtime::Result<Slot, PersistenceError>
decode_slot(const JsonValue& value, DecodeContext& context, std::string path);
// A null value decodes as an empty lane, which is what a pre-lane sequence
// version means. `lane_path` is the full diagnostic path of the array itself.
runtime::Result<ChordScaleLane, PersistenceError>
decode_chord_scale_lane(const JsonValue* value, DecodeContext& context, std::string lane_path);
// A null value decodes as the groove that states no feel, which is what a
// pre-groove sequence version means. `groove_path` is the full diagnostic path
// of the object itself.
runtime::Result<GrooveTemplate, PersistenceError>
decode_groove(const JsonValue* value, DecodeContext& context, std::string groove_path);

runtime::Result<Sequence, PersistenceError>
decode_sequence(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
                const SchemaRegistry& registry, DecodeContext& context, std::string path);

} // namespace pulp::timeline::detail
