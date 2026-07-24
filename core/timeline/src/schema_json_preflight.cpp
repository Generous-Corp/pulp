#include <pulp/timeline/serialize.hpp>

#include "asset_schema_policy.hpp"
#include "bounded_increment.hpp"
#include "json_span_reader.hpp"
#include "schema_json_validation.hpp"
#include "serialize_internal.hpp"
#include "track_schema_policy.hpp"

#include <algorithm>
#include <limits>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::size_t offset = 0,
                                          std::uint64_t actual = 0, std::uint64_t limit = 0,
                                          std::string path = {}) {
    return runtime::Result<T, PersistenceError>(
        runtime::Err(PersistenceError{code, offset, actual, limit, std::move(path), std::nullopt}));
}

class StructuralScanner {
  public:
    StructuralScanner(std::string_view source, const DecodeLimits& limits,
                      const SchemaRegistry* registry = nullptr)
        : source_(source), limits_(limits), registry_(registry), reader_(source, limits) {}

    runtime::Result<StructuralPreflightSuccess, PersistenceError> run() {
        if (!scan())
            return failed();
        return runtime::Result<StructuralPreflightSuccess, PersistenceError>(
            runtime::Ok(StructuralPreflightSuccess{}));
    }

    runtime::Result<ProjectSnapshotSummary, PersistenceError> peek() {
        if (!scan())
            return runtime::Result<ProjectSnapshotSummary, PersistenceError>(runtime::Err(error_));
        return runtime::Result<ProjectSnapshotSummary, PersistenceError>(
            runtime::Ok(std::move(summary_)));
    }

  private:
    bool scan() {
        std::size_t position = 0;
        Span root;
        if (!skip_value(position, 0, root))
            return refine_json_error();
        skip_space(position);
        if (position != source_.size()) {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return refine_json_error();
        }
        if (!walk_project(root))
            return false;
        if (registry_)
            if (const auto error = detail::validate_json_syntax_and_limits(source_, limits_)) {
                error_ = *error;
                has_error_ = true;
                return false;
            }
        return true;
    }

    bool refine_json_error() {
        if (registry_ && error_.code == PersistenceErrorCode::InvalidJson)
            if (const auto error = detail::validate_json_syntax_and_limits(source_, limits_))
                error_ = *error;
        return false;
    }

    using Span = detail::JsonSpan;

    struct EnvelopeView {
        std::string type;
        std::uint32_t version = 0;
        Span data{};
        bool valid_shape = false;
    };

    std::string_view source_;
    const DecodeLimits& limits_;
    const SchemaRegistry* registry_;
    detail::JsonSpanReader reader_;
    PersistenceError error_;
    bool has_error_ = false;
    ProjectSnapshotCounts counts_;
    std::size_t audio_loop_points_ = 0;
    std::size_t audio_loop_tags_ = 0;
    std::size_t locators_ = 0;
    std::size_t representations_ = 0;
    ProjectSnapshotSummary summary_;

    runtime::Result<StructuralPreflightSuccess, PersistenceError> failed() const {
        return runtime::Result<StructuralPreflightSuccess, PersistenceError>(runtime::Err(error_));
    }

    void set_error(PersistenceErrorCode code, std::size_t offset, std::uint64_t actual = 0,
                   std::uint64_t limit = 0, std::string path = {}) {
        if (has_error_)
            return;
        has_error_ = true;
        error_ = PersistenceError{code, offset, actual, limit, std::move(path)};
    }

    bool adopt_reader_error() {
        if (reader_.has_error()) {
            const auto& error = reader_.error();
            set_error(error.code, error.byte_offset, error.actual, error.limit, error.path);
        }
        return false;
    }

    void skip_space(std::size_t& position) const noexcept {
        reader_.skip_space(position);
    }
    bool scan_string(std::size_t& position, std::string* captured = nullptr,
                     std::size_t capture_limit = 128) {
        return reader_.scan_string(position, captured, capture_limit) || adopt_reader_error();
    }
    bool skip_value(std::size_t& position, std::size_t depth, Span& span) {
        return reader_.skip_value(position, depth, span) || adopt_reader_error();
    }
    bool member(Span object, std::string_view wanted, Span& result, bool& found) {
        return reader_.member(object, wanted, result, found) || adopt_reader_error();
    }
    bool string_value(Span span, std::string& value) {
        return reader_.string_value(span, value) || adopt_reader_error();
    }
    bool u32_value(Span span, std::uint32_t& value) const noexcept {
        return reader_.u32_value(span, value);
    }

    bool exact_envelope_keys(Span object, bool& exact) {
        exact = false;
        std::size_t position = object.begin;
        skip_space(position);
        if (position >= object.end || source_[position++] != '{')
            return true;
        std::size_t members = 0;
        bool data = false;
        bool type = false;
        bool version = false;
        skip_space(position);
        if (position < object.end && source_[position] == '}')
            return true;
        for (;;) {
            skip_space(position);
            std::string key;
            if (!scan_string(position, &key))
                return false;
            skip_space(position);
            if (position >= object.end || source_[position++] != ':') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            Span value;
            if (!skip_value(position, 1, value))
                return false;
            ++members;
            if (key == "data")
                data = true;
            else if (key == "type_name")
                type = true;
            else if (key == "version")
                version = true;
            skip_space(position);
            if (position < object.end && source_[position] == '}')
                break;
            if (position >= object.end || source_[position++] != ',') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
        }
        exact = members == 3 && data && type && version;
        return true;
    }

    bool envelope(Span value, std::string& type, std::uint32_t& version, Span& data,
                  bool& valid_shape) {
        Span type_span;
        Span version_span;
        bool has_type = false;
        bool has_version = false;
        bool has_data = false;
        bool exact_keys = false;
        if (!exact_envelope_keys(value, exact_keys) ||
            !member(value, "type_name", type_span, has_type) ||
            !member(value, "version", version_span, has_version) ||
            !member(value, "data", data, has_data))
            return false;
        valid_shape = false;
        if (!has_type)
            return true;
        if (!string_value(type_span, type))
            return false;
        if (!has_version || !has_data)
            return true;
        valid_shape = exact_keys && u32_value(version_span, version) && data.begin < data.end &&
                      source_[data.begin] == '{';
        return true;
    }

    bool require_structural_shape(bool valid_shape, std::uint32_t version, const std::string& path,
                                  std::size_t offset, std::uint32_t minimum_version = 1,
                                  std::uint32_t maximum_version = 1) {
        if (!valid_shape) {
            set_error(PersistenceErrorCode::InvalidSchema, offset, 0, 0, path);
            return false;
        }
        if (version >= minimum_version && version <= maximum_version)
            return true;
        set_error(PersistenceErrorCode::UnsupportedSchemaVersion, offset, version, maximum_version,
                  path);
        return false;
    }

    enum ValueShape : std::uint8_t {
        StringShape = 1 << 0,
        ObjectShape = 1 << 1,
        ArrayShape = 1 << 2,
        NullShape = 1 << 3,
        NumberShape = 1 << 4,
        BooleanShape = 1 << 5,
    };

    bool has_shape(Span value, std::uint8_t allowed) const noexcept {
        if (value.begin >= value.end)
            return false;
        const auto first = source_[value.begin];
        const auto shape = first == '"'                                     ? StringShape
                           : first == '{'                                   ? ObjectShape
                           : first == '['                                   ? ArrayShape
                           : first == 'n'                                   ? NullShape
                           : first == 't' || first == 'f'                   ? BooleanShape
                           : first == '-' || (first >= '0' && first <= '9') ? NumberShape
                                                                            : 0;
        return (allowed & shape) != 0;
    }

    bool require_member(Span object, std::string_view name, std::uint8_t allowed,
                        const std::string& path,
                        PersistenceErrorCode missing_code = PersistenceErrorCode::InvalidSchema) {
        Span value;
        bool found = false;
        if (!member(object, name, value, found))
            return false;
        if (found && has_shape(value, allowed))
            return true;
        set_error(found ? PersistenceErrorCode::InvalidSchema : missing_code,
                  found ? value.begin : object.begin, 0, 0, path + "/" + std::string(name));
        return false;
    }

    template <typename ElementFn>
    bool governed_array(Span array, std::size_t& count, std::size_t maximum,
                        const std::string& path, ElementFn&& visit) {
        std::size_t position = array.begin;
        skip_space(position);
        if (position >= array.end || source_[position++] != '[') {
            set_error(PersistenceErrorCode::InvalidSchema, array.begin, 0, 0, path);
            return false;
        }
        skip_space(position);
        if (position < array.end && source_[position] == ']')
            return true;
        std::size_t index = 0;
        for (;;) {
            skip_space(position);
            const auto increment = detail::bounded_increment(count, maximum);
            if (!increment) {
                set_error(PersistenceErrorCode::LimitExceeded, position, increment.actual, maximum,
                          path);
                return false;
            }
            Span element;
            if (!skip_value(position, 1, element) || !visit(element, index))
                return false;
            ++index;
            skip_space(position);
            if (position < array.end && source_[position] == ']')
                return true;
            if (position >= array.end || source_[position++] != ',') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
        }
    }

    bool walk_project(Span value) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != "pulp.timeline.project") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, "", value.begin))
            return false;
        Span id;
        Span name;
        Span next;
        Span root;
        bool has_id = false;
        bool has_name = false;
        bool has_next = false;
        bool has_root = false;
        if (!member(data, "id", id, has_id) || !member(data, "name", name, has_name) ||
            !member(data, "next_item_id", next, has_next) ||
            !member(data, "root_sequence_id", root, has_root))
            return false;
        if (!has_id || !has_name || !has_next || !has_root || !has_shape(id, StringShape) ||
            !has_shape(name, StringShape) || !has_shape(next, StringShape) ||
            !has_shape(root, StringShape)) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0, "/data");
            return false;
        }
        Span assets;
        Span sequences;
        bool has_assets = false;
        bool has_sequences = false;
        if (!member(data, "assets", assets, has_assets) ||
            !member(data, "sequences", sequences, has_sequences))
            return false;
        if (!has_assets || !has_sequences) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0, "/data");
            return false;
        }
        if (has_assets &&
            !governed_array(assets, counts_.assets, limits_.max_assets, "/data/assets",
                            [&](Span element, std::size_t index) {
                                return walk_asset(element, "/data/assets/" + std::to_string(index));
                            }))
            return false;
        if (has_sequences &&
            !governed_array(sequences, counts_.sequences, limits_.max_sequences, "/data/sequences",
                            [&](Span element, std::size_t index) {
                                return walk_sequence(element,
                                                     "/data/sequences/" + std::to_string(index));
                            }))
            return false;
        if (!registry_)
            return true;

        std::uint64_t decoded_id = 0;
        std::uint64_t decoded_next = 0;
        std::uint64_t decoded_root = 0;
        std::string decoded_name;
        if (!reader_.canonical_u64(id, decoded_id, "/data/id") ||
            !reader_.decoded_string(name, decoded_name, "/data/name") ||
            !reader_.canonical_u64(next, decoded_next, "/data/next_item_id") ||
            !reader_.canonical_u64(root, decoded_root, "/data/root_sequence_id")) {
            adopt_reader_error();
            return false;
        }
        summary_.schema_version = version;
        summary_.project_id = ItemId{decoded_id};
        summary_.name = std::move(decoded_name);
        summary_.next_item_id = decoded_next;
        summary_.root_sequence_id = ItemId{decoded_root};
        summary_.counts = counts_;
        return true;
    }

    bool walk_asset(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != "pulp.timeline.asset") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin,
                                      detail::asset_schema_policy.oldest_readable_version,
                                      detail::asset_schema_policy.current_version))
            return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "content_hash", StringShape, data_path) ||
            !require_member(data, "frame_count", StringShape, data_path) ||
            !require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "name", StringShape, data_path) ||
            !require_member(data, "sample_rate", ObjectShape, data_path) ||
            !require_member(data, "storage_policy", StringShape, data_path))
            return false;
        Span locators;
        Span representations;
        bool has_locators = false;
        bool has_representations = false;
        if (!member(data, "locators", locators, has_locators) ||
            !member(data, "representations", representations, has_representations))
            return false;
        if (!has_locators || !has_representations) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0, path + "/data");
            return false;
        }
        if (has_locators &&
            !governed_array(locators, locators_, limits_.max_locators, path + "/data/locators",
                            [](Span, std::size_t) { return true; }))
            return false;
        if (has_representations &&
            !governed_array(representations, representations_, limits_.max_representations,
                            path + "/data/representations", [&](Span element, std::size_t index) {
                                return walk_representation(element, path +
                                                                        "/data/representations/" +
                                                                        std::to_string(index));
                            }))
            return false;
        Span loop_info;
        bool has_loop_info = false;
        if (!member(data, "loop_info", loop_info, has_loop_info))
            return false;
        if (!has_loop_info)
            return true;
        if (!detail::asset_schema_policy.supports_loop_info(version) ||
            !has_shape(loop_info, ObjectShape)) {
            set_error(PersistenceErrorCode::InvalidSchema, loop_info.begin, 0, 0,
                      data_path + "/loop_info");
            return false;
        }
        Span points;
        Span tags;
        bool has_points = false;
        bool has_tags = false;
        if (!member(loop_info, "points", points, has_points) ||
            !member(loop_info, "tags", tags, has_tags))
            return false;
        if (!has_points || !has_tags) {
            set_error(PersistenceErrorCode::InvalidSchema, loop_info.begin, 0, 0,
                      data_path + "/loop_info");
            return false;
        }
        if (!governed_array(points, audio_loop_points_, limits_.max_audio_loop_points,
                            data_path + "/loop_info/points",
                            [](Span, std::size_t) { return true; }))
            return false;
        return governed_array(tags, audio_loop_tags_, limits_.max_audio_loop_tags,
                              data_path + "/loop_info/tags",
                              [](Span tag, std::size_t) { return tag.begin < tag.end; });
    }

    bool walk_representation(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != "pulp.timeline.asset_representation") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin))
            return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "content_hash", StringShape, data_path) ||
            !require_member(data, "role", StringShape, data_path) ||
            !require_member(data, "storage_policy", StringShape, data_path))
            return false;
        Span locators;
        bool found = false;
        if (!member(data, "locators", locators, found))
            return false;
        if (!found) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/locators");
            return false;
        }
        return governed_array(locators, locators_, limits_.max_locators, path + "/data/locators",
                              [](Span, std::size_t) { return true; });
    }

    bool walk_sequence(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != "pulp.timeline.sequence") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin))
            return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "absolute_duration", ObjectShape | NullShape, data_path) ||
            !require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "musical_duration", StringShape | NullShape, data_path) ||
            !require_member(data, "name", StringShape, data_path))
            return false;
        Span tracks;
        bool found = false;
        if (!member(data, "tracks", tracks, found))
            return false;
        if (!found) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0, path + "/data/tracks");
            return false;
        }
        return governed_array(tracks, counts_.tracks, limits_.max_tracks, path + "/data/tracks",
                              [&](Span element, std::size_t index) {
                                  return walk_track(element,
                                                    path + "/data/tracks/" + std::to_string(index));
                              });
    }

    bool walk_track(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != detail::track_schema_policy.type_name) {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin,
                                      detail::track_schema_policy.oldest_readable_version,
                                      detail::track_schema_policy.current_version))
            return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "name", StringShape, data_path))
            return false;
        Span clips;
        Span devices;
        Span automation;
        Span take_lanes;
        Span freeze;
        Span record_armed;
        bool has_clips = false;
        bool has_devices = false;
        bool has_automation = false;
        bool has_take_lanes = false;
        bool has_freeze = false;
        bool has_record_armed = false;
        if (!member(data, "clips", clips, has_clips) ||
            !member(data, "device_chain", devices, has_devices) ||
            !member(data, "automation_lanes", automation, has_automation) ||
            !member(data, "take_lanes", take_lanes, has_take_lanes) ||
            !member(data, "freeze", freeze, has_freeze) ||
            !member(data, "record_armed", record_armed, has_record_armed))
            return false;
        const auto requires_devices = detail::track_schema_policy.requires_device_chain(version);
        const auto requires_automation = detail::track_schema_policy.requires_automation(version);
        const auto requires_takes = detail::track_schema_policy.requires_takes(version);
        const auto supports_freeze = detail::track_schema_policy.supports_freeze(version);
        if (!has_clips || requires_devices != has_devices ||
            requires_automation != has_automation || requires_takes != has_take_lanes ||
            requires_takes != has_record_armed || (!supports_freeze && has_freeze) ||
            (has_take_lanes && !has_shape(take_lanes, ArrayShape)) ||
            (has_record_armed && !has_shape(record_armed, BooleanShape))) {
            auto invalid_path = path + "/data/clips";
            if (has_clips) {
                if (!supports_freeze && has_freeze)
                    invalid_path = path + "/data/freeze";
                else if (requires_takes != has_record_armed ||
                         (has_record_armed && !has_shape(record_armed, BooleanShape)))
                    invalid_path = path + "/data/record_armed";
                else if (requires_takes != has_take_lanes)
                    invalid_path = path + "/data/take_lanes";
                else if (has_take_lanes && !has_shape(take_lanes, ArrayShape))
                    invalid_path = path + "/data/take_lanes";
                else if (requires_automation != has_automation)
                    invalid_path = path + "/data/automation_lanes";
                else
                    invalid_path = path + "/data/device_chain";
            }
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      std::move(invalid_path));
            return false;
        }
        if (has_freeze &&
            (!require_member(freeze, "asset_id", StringShape, data_path + "/freeze") ||
             !require_member(freeze, "frame_count", StringShape, data_path + "/freeze") ||
             !require_member(freeze, "placement_start", StringShape, data_path + "/freeze") ||
             !require_member(freeze, "render_plan_hash", StringShape, data_path + "/freeze") ||
             !require_member(freeze, "sample_rate", ObjectShape, data_path + "/freeze") ||
             !require_member(freeze, "source_start", StringShape, data_path + "/freeze")))
            return false;
        if (!governed_array(clips, counts_.clips, limits_.max_clips, path + "/data/clips",
                            [&](Span element, std::size_t index) {
                                return walk_clip(element,
                                                 path + "/data/clips/" + std::to_string(index));
                            }))
            return false;
        if (has_automation &&
            !governed_array(automation, counts_.automation_lanes, limits_.max_automation_lanes,
                            path + "/data/automation_lanes", [&](Span element, std::size_t index) {
                                return walk_automation_lane(element, path +
                                                                         "/data/automation_lanes/" +
                                                                         std::to_string(index));
                            }))
            return false;
        if (has_devices &&
            !governed_array(devices, counts_.device_placements, limits_.max_device_placements,
                            path + "/data/device_chain", [&](Span element, std::size_t index) {
                                return walk_device_placement(element, path + "/data/device_chain/" +
                                                                          std::to_string(index));
                            }))
            return false;
        return !has_take_lanes ||
               governed_array(take_lanes, counts_.take_lanes, limits_.max_take_lanes,
                              path + "/data/take_lanes", [&](Span element, std::size_t index) {
                                  return walk_take_lane(element, path + "/data/take_lanes/" +
                                                                     std::to_string(index));
                              });
    }

    bool walk_take_lane(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != "pulp.timeline.take_lane" ||
            !require_structural_shape(valid_shape, version, path, value.begin, 1, 2)) {
            if (!has_error_)
                set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        const auto data_path = path + "/data";
        if (!require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "name", StringShape, data_path) ||
            !require_member(data, "takes", ArrayShape, data_path))
            return false;
        Span takes;
        bool has_takes = false;
        if (!member(data, "takes", takes, has_takes) || !has_takes)
            return false;
        if (!governed_array(takes, counts_.takes, limits_.max_takes, data_path + "/takes",
                            [&](Span element, std::size_t index) {
                                return walk_take(element,
                                                 data_path + "/takes/" + std::to_string(index));
                            }))
            return false;
        Span comp;
        bool has_comp = false;
        if (!member(data, "comp_segments", comp, has_comp))
            return false;
        if ((version == 2) != has_comp) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      data_path + "/comp_segments");
            return false;
        }
        return !has_comp ||
               governed_array(
                   comp, counts_.take_comp_segments, limits_.max_take_comp_segments,
                   data_path + "/comp_segments", [&](Span segment, std::size_t index) {
                       const auto segment_path =
                           data_path + "/comp_segments/" + std::to_string(index);
                       return require_member(segment, "sample_count", StringShape, segment_path) &&
                              require_member(segment, "sample_rate", ObjectShape, segment_path) &&
                              require_member(segment, "start", StringShape, segment_path) &&
                              require_member(segment, "take_id", StringShape, segment_path);
                   });
    }

    bool walk_take(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != "pulp.timeline.take" ||
            !require_structural_shape(valid_shape, version, path, value.begin)) {
            if (!has_error_)
                set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        const auto data_path = path + "/data";
        return require_member(data, "asset_id", StringShape, data_path) &&
               require_member(data, "frame_count", StringShape, data_path) &&
               require_member(data, "id", StringShape, data_path) &&
               require_member(data, "placement_start", StringShape, data_path) &&
               require_member(data, "sample_rate", ObjectShape, data_path) &&
               require_member(data, "source_start", StringShape, data_path);
    }

    bool walk_automation_lane(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != "pulp.timeline.automation_lane" ||
            !require_structural_shape(valid_shape, version, path, value.begin)) {
            if (!has_error_)
                set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        const auto data_path = path + "/data";
        if (!require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "points", ArrayShape, data_path,
                            PersistenceErrorCode::MissingField) ||
            !require_member(data, "target", ObjectShape, data_path))
            return false;
        Span points;
        Span target;
        bool has_points = false;
        bool has_target = false;
        if (!member(data, "points", points, has_points) ||
            !member(data, "target", target, has_target) || !has_points || !has_target)
            return false;
        if (!governed_array(
                points, counts_.automation_points, limits_.max_automation_points,
                data_path + "/points", [&](Span point, std::size_t index) {
                    const auto point_path = data_path + "/points/" + std::to_string(index);
                    return require_member(point, "curvature_bits", StringShape, point_path) &&
                           require_member(point, "id", StringShape, point_path) &&
                           require_member(point, "interpolation", StringShape, point_path) &&
                           require_member(point, "position_ticks", StringShape, point_path) &&
                           require_member(point, "value_bits", StringShape, point_path);
                }))
            return false;
        EnvelopeView target_envelope;
        if (!envelope(target, target_envelope.type, target_envelope.version, target_envelope.data,
                      target_envelope.valid_shape))
            return false;
        if (target_envelope.type != "pulp.timeline.automation_target.device_parameter" ||
            !require_structural_shape(target_envelope.valid_shape, target_envelope.version,
                                      data_path + "/target", target.begin)) {
            if (!has_error_)
                set_error(PersistenceErrorCode::InvalidSchema, target.begin, 0, 0,
                          data_path + "/target");
            return false;
        }
        return require_member(target_envelope.data, "device_placement_id", StringShape,
                              data_path + "/target/data") &&
               require_member(target_envelope.data, "parameter_id", NumberShape,
                              data_path + "/target/data");
    }

    bool walk_device_placement(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != "pulp.timeline.device_placement") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin))
            return false;
        return require_member(data, "id", StringShape, path + "/data");
    }

    bool walk_clip(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape))
            return false;
        if (type != "pulp.timeline.clip") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin))
            return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "time_range", ObjectShape, data_path))
            return false;
        Span content;
        bool found = false;
        if (!member(data, "content", content, found))
            return false;
        if (!found) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/content");
            return false;
        }
        std::string content_type;
        Span content_data;
        if (!envelope(content, content_type, version, content_data, valid_shape))
            return false;
        if (!valid_shape) {
            set_error(PersistenceErrorCode::InvalidSchema, content.begin, 0, 0,
                      path + "/data/content");
            return false;
        }
        if (content_type == "pulp.timeline.content.empty" && version == 1)
            return true;
        if (content_type == "pulp.timeline.content.media" && version == 1) {
            const auto content_path = path + "/data/content/data";
            return require_member(content_data, "asset_id", StringShape, content_path) &&
                   require_member(content_data, "frame_count", StringShape, content_path) &&
                   require_member(content_data, "source_start", StringShape, content_path);
        }
        if (content_type != "pulp.timeline.content.notes" || version != 1) {
            const auto* schema =
                registry_ ? registry_->find(SchemaDomain::Content, content_type) : nullptr;
            const auto raw_size = content.end - content.begin;
            if (registry_ && (!schema || version > schema->current_version) &&
                raw_size > limits_.max_opaque_bytes) {
                set_error(PersistenceErrorCode::LimitExceeded, content.begin, raw_size,
                          limits_.max_opaque_bytes, path + "/data/content");
                return false;
            }
            return true;
        }
        Span notes;
        bool has_notes = false;
        if (!member(content_data, "notes", notes, has_notes))
            return false;
        if (!has_notes) {
            set_error(PersistenceErrorCode::InvalidSchema, content_data.begin, 0, 0,
                      path + "/data/content/data/notes");
            return false;
        }
        return governed_array(
            notes, counts_.notes, limits_.max_notes, path + "/data/content/data/notes",
            [&](Span note, std::size_t index) {
                const auto note_path = path + "/data/content/data/notes/" + std::to_string(index);
                if (!has_shape(note, ObjectShape)) {
                    set_error(PersistenceErrorCode::InvalidSchema, note.begin, 0, 0, note_path);
                    return false;
                }
                return require_member(note, "id", StringShape, note_path) &&
                       require_member(note, "start_ticks", StringShape, note_path) &&
                       require_member(note, "duration_ticks", StringShape, note_path) &&
                       require_member(note, "velocity", NumberShape, note_path) &&
                       require_member(note, "pitch", NumberShape, note_path) &&
                       require_member(note, "channel", NumberShape, note_path);
            });
    }
};

} // namespace
runtime::Result<StructuralPreflightSuccess, PersistenceError>
preflight_timeline_structure(std::string_view json, const DecodeLimits& limits) {
    if (json.size() > limits.max_input_bytes)
        return fail<StructuralPreflightSuccess>(PersistenceErrorCode::LimitExceeded, 0, json.size(),
                                                limits.max_input_bytes);
    return StructuralScanner(json, limits).run();
}
runtime::Result<ProjectSnapshotSummary, PersistenceError>
peek_project_summary(std::string_view json, const SchemaRegistry& registry,
                     const DecodeLimits& limits) {
    if (const auto invalid = detail::validate_structural_registry(registry))
        return fail<ProjectSnapshotSummary>(*invalid, 0, 0, 0, "/");
    if (json.size() > limits.max_input_bytes)
        return fail<ProjectSnapshotSummary>(PersistenceErrorCode::LimitExceeded, 0, json.size(),
                                            limits.max_input_bytes);
    return StructuralScanner(json, limits, &registry).peek();
}
} // namespace pulp::timeline
