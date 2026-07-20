#include <pulp/timeline/schema_json.hpp>

#include "track_schema_policy.hpp"
#include "sequence_schema_policy.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::size_t offset = 0,
                                          std::uint64_t actual = 0,
                                          std::uint64_t limit = 0,
                                          std::string path = {}) {
    return runtime::Result<T, PersistenceError>(runtime::Err(
        PersistenceError{code, offset, actual, limit, std::move(path), std::nullopt}));
}

class StructuralScanner {
  public:
    StructuralScanner(std::string_view source, const DecodeLimits& limits)
        : source_(source), limits_(limits) {}

    runtime::Result<StructuralPreflightSuccess, PersistenceError> run() {
        std::size_t position = 0;
        Span root;
        if (!skip_value(position, 0, root)) return failed();
        skip_space(position);
        if (position != source_.size()) {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return failed();
        }
        if (!walk_project(root)) return failed();
        return runtime::Result<StructuralPreflightSuccess, PersistenceError>(
            runtime::Ok(StructuralPreflightSuccess{}));
    }

  private:
    struct Span {
        std::size_t begin = 0;
        std::size_t end = 0;
    };

    struct EnvelopeView {
        std::string type;
        std::uint32_t version = 0;
        Span data{};
        bool valid_shape = false;
    };

    std::string_view source_;
    const DecodeLimits& limits_;
    PersistenceError error_;
    bool has_error_ = false;
    std::size_t assets_ = 0;
    std::size_t sequences_ = 0;
    std::size_t tracks_ = 0;
    std::size_t clips_ = 0;
    std::size_t notes_ = 0;
    std::size_t device_placements_ = 0;
    std::size_t automation_lanes_ = 0;
    std::size_t automation_points_ = 0;
    std::size_t sequence_markers_ = 0;
    std::size_t sequence_regions_ = 0;
    std::size_t locators_ = 0;
    std::size_t representations_ = 0;

    runtime::Result<StructuralPreflightSuccess, PersistenceError> failed() const {
        return runtime::Result<StructuralPreflightSuccess, PersistenceError>(
            runtime::Err(error_));
    }

    void set_error(PersistenceErrorCode code, std::size_t offset,
                   std::uint64_t actual = 0, std::uint64_t limit = 0,
                   std::string path = {}) {
        if (has_error_) return;
        has_error_ = true;
        error_ = PersistenceError{code, offset, actual, limit, std::move(path)};
    }

    void skip_space(std::size_t& position) const noexcept {
        while (position < source_.size()) {
            const auto value = source_[position];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
            ++position;
        }
    }

    static int hex_digit(char value) noexcept {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    bool scan_string(std::size_t& position, std::string* captured = nullptr,
                     std::size_t capture_limit = 128) {
        if (position >= source_.size() || source_[position] != '"') {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return false;
        }
        ++position;
        while (position < source_.size()) {
            const auto byte = static_cast<unsigned char>(source_[position++]);
            if (byte == '"') return true;
            if (byte < 0x20) {
                set_error(PersistenceErrorCode::InvalidJson, position - 1);
                return false;
            }
            if (byte != '\\') {
                if (captured && captured->size() < capture_limit)
                    captured->push_back(static_cast<char>(byte));
                continue;
            }
            if (position >= source_.size()) {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            const auto escape = source_[position++];
            char decoded = '\0';
            bool has_decoded_byte = true;
            switch (escape) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u': {
                    if (source_.size() - position < 4) {
                        set_error(PersistenceErrorCode::InvalidJson, position);
                        return false;
                    }
                    std::uint32_t codepoint = 0;
                    for (int index = 0; index < 4; ++index) {
                        const auto digit = hex_digit(source_[position++]);
                        if (digit < 0) {
                            set_error(PersistenceErrorCode::InvalidJson, position - 1);
                            return false;
                        }
                        codepoint = codepoint * 16 + static_cast<std::uint32_t>(digit);
                    }
                    if (codepoint <= 0x7f) decoded = static_cast<char>(codepoint);
                    else {
                        has_decoded_byte = false;
                        if (captured) captured->resize(capture_limit);
                    }
                    break;
                }
                default:
                    set_error(PersistenceErrorCode::InvalidJson, position - 1);
                    return false;
            }
            if (captured && has_decoded_byte && captured->size() < capture_limit)
                captured->push_back(decoded);
        }
        set_error(PersistenceErrorCode::InvalidJson, position);
        return false;
    }

    bool skip_value(std::size_t& position, std::size_t depth, Span& span) {
        skip_space(position);
        if (depth > limits_.max_depth) {
            set_error(PersistenceErrorCode::LimitExceeded, position, depth,
                      limits_.max_depth);
            return false;
        }
        span.begin = position;
        if (position >= source_.size()) {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return false;
        }
        const auto first = source_[position];
        if (first == '"') {
            if (!scan_string(position)) return false;
        } else if (first == '{') {
            ++position;
            skip_space(position);
            if (position < source_.size() && source_[position] == '}') {
                ++position;
            } else {
                for (;;) {
                    skip_space(position);
                    if (!scan_string(position)) return false;
                    skip_space(position);
                    if (position >= source_.size() || source_[position++] != ':') {
                        set_error(PersistenceErrorCode::InvalidJson, position);
                        return false;
                    }
                    Span child;
                    if (!skip_value(position, depth + 1, child)) return false;
                    skip_space(position);
                    if (position < source_.size() && source_[position] == '}') {
                        ++position;
                        break;
                    }
                    if (position >= source_.size() || source_[position++] != ',') {
                        set_error(PersistenceErrorCode::InvalidJson, position);
                        return false;
                    }
                }
            }
        } else if (first == '[') {
            ++position;
            skip_space(position);
            if (position < source_.size() && source_[position] == ']') {
                ++position;
            } else {
                for (;;) {
                    Span child;
                    if (!skip_value(position, depth + 1, child)) return false;
                    skip_space(position);
                    if (position < source_.size() && source_[position] == ']') {
                        ++position;
                        break;
                    }
                    if (position >= source_.size() || source_[position++] != ',') {
                        set_error(PersistenceErrorCode::InvalidJson, position);
                        return false;
                    }
                }
            }
        } else if (source_.substr(position, 4) == "true" ||
                   source_.substr(position, 4) == "null") {
            position += 4;
        } else if (source_.substr(position, 5) == "false") {
            position += 5;
        } else {
            if (first == '-') ++position;
            if (position >= source_.size()) {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            if (source_[position] == '0') {
                ++position;
            } else if (source_[position] >= '1' && source_[position] <= '9') {
                while (position < source_.size() && source_[position] >= '0' &&
                       source_[position] <= '9') ++position;
            } else {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            if (position < source_.size() && source_[position] == '.') {
                ++position;
                if (position >= source_.size() || source_[position] < '0' ||
                    source_[position] > '9') {
                    set_error(PersistenceErrorCode::InvalidJson, position);
                    return false;
                }
                while (position < source_.size() && source_[position] >= '0' &&
                       source_[position] <= '9') ++position;
            }
            if (position < source_.size() &&
                (source_[position] == 'e' || source_[position] == 'E')) {
                ++position;
                if (position < source_.size() &&
                    (source_[position] == '+' || source_[position] == '-')) ++position;
                if (position >= source_.size() || source_[position] < '0' ||
                    source_[position] > '9') {
                    set_error(PersistenceErrorCode::InvalidJson, position);
                    return false;
                }
                while (position < source_.size() && source_[position] >= '0' &&
                       source_[position] <= '9') ++position;
            }
        }
        span.end = position;
        return true;
    }

    bool member(Span object, std::string_view wanted, Span& result, bool& found) {
        found = false;
        std::size_t position = object.begin;
        skip_space(position);
        if (position >= object.end || source_[position++] != '{') return true;
        skip_space(position);
        if (position < object.end && source_[position] == '}') return true;
        for (;;) {
            skip_space(position);
            std::string key;
            if (!scan_string(position, &key)) return false;
            skip_space(position);
            if (position >= object.end || source_[position++] != ':') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            Span value;
            if (!skip_value(position, 1, value)) return false;
            if (key == wanted) {
                if (found) {
                    set_error(PersistenceErrorCode::DuplicateKey, object.begin);
                    return false;
                }
                found = true;
                result = value;
            }
            skip_space(position);
            if (position < object.end && source_[position] == '}') break;
            if (position >= object.end || source_[position++] != ',') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
        }
        return true;
    }

    bool string_value(Span span, std::string& value) {
        value.clear();
        std::size_t position = span.begin;
        if (!scan_string(position, &value)) return false;
        return position == span.end;
    }

    bool u32_value(Span span, std::uint32_t& value) const noexcept {
        const auto raw = source_.substr(span.begin, span.end - span.begin);
        const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == raw.data() + raw.size();
    }

    bool exact_envelope_keys(Span object, bool& exact) {
        exact = false;
        std::size_t position = object.begin;
        skip_space(position);
        if (position >= object.end || source_[position++] != '{') return true;
        std::size_t members = 0;
        bool data = false;
        bool type = false;
        bool version = false;
        skip_space(position);
        if (position < object.end && source_[position] == '}') return true;
        for (;;) {
            skip_space(position);
            std::string key;
            if (!scan_string(position, &key)) return false;
            skip_space(position);
            if (position >= object.end || source_[position++] != ':') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            Span value;
            if (!skip_value(position, 1, value)) return false;
            ++members;
            if (key == "data") data = true;
            else if (key == "type_name") type = true;
            else if (key == "version") version = true;
            skip_space(position);
            if (position < object.end && source_[position] == '}') break;
            if (position >= object.end || source_[position++] != ',') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
        }
        exact = members == 3 && data && type && version;
        return true;
    }

    bool envelope(Span value, std::string& type, std::uint32_t& version,
                  Span& data, bool& valid_shape) {
        Span type_span;
        Span version_span;
        bool has_type = false;
        bool has_version = false;
        bool has_data = false;
        bool exact_keys = false;
        if (!exact_envelope_keys(value, exact_keys) ||
            !member(value, "type_name", type_span, has_type) ||
            !member(value, "version", version_span, has_version) ||
            !member(value, "data", data, has_data)) return false;
        valid_shape = false;
        if (!has_type) return true;
        if (!string_value(type_span, type)) return false;
        if (!has_version || !has_data) return true;
        valid_shape = exact_keys && u32_value(version_span, version) &&
                      data.begin < data.end &&
                      source_[data.begin] == '{';
        return true;
    }

    bool require_structural_shape(bool valid_shape, std::uint32_t version,
                                  const std::string& path, std::size_t offset,
                                  std::uint32_t minimum_version = 1,
                                  std::uint32_t maximum_version = 1) {
        if (!valid_shape) {
            set_error(PersistenceErrorCode::InvalidSchema, offset, 0, 0, path);
            return false;
        }
        if (version >= minimum_version && version <= maximum_version) return true;
        set_error(PersistenceErrorCode::UnsupportedSchemaVersion, offset, version,
                  maximum_version, path);
        return false;
    }

    enum ValueShape : std::uint8_t {
        StringShape = 1 << 0,
        ObjectShape = 1 << 1,
        ArrayShape = 1 << 2,
        NullShape = 1 << 3,
        NumberShape = 1 << 4,
    };

    bool has_shape(Span value, std::uint8_t allowed) const noexcept {
        if (value.begin >= value.end) return false;
        const auto first = source_[value.begin];
        const auto shape = first == '"' ? StringShape
                           : first == '{' ? ObjectShape
                           : first == '[' ? ArrayShape
                           : first == 'n' ? NullShape
                           : first == '-' || (first >= '0' && first <= '9') ? NumberShape
                                          : 0;
        return (allowed & shape) != 0;
    }

    bool require_member(Span object, std::string_view name, std::uint8_t allowed,
                        const std::string& path,
                        PersistenceErrorCode missing_code = PersistenceErrorCode::InvalidSchema) {
        Span value;
        bool found = false;
        if (!member(object, name, value, found)) return false;
        if (found && has_shape(value, allowed)) return true;
        set_error(found ? PersistenceErrorCode::InvalidSchema : missing_code,
                  found ? value.begin : object.begin, 0, 0,
                  path + "/" + std::string(name));
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
        if (position < array.end && source_[position] == ']') return true;
        std::size_t index = 0;
        for (;;) {
            skip_space(position);
            if (count >= maximum) {
                set_error(PersistenceErrorCode::LimitExceeded, position, count + 1,
                          maximum, path);
                return false;
            }
            ++count;
            Span element;
            if (!skip_value(position, 1, element) || !visit(element, index)) return false;
            ++index;
            skip_space(position);
            if (position < array.end && source_[position] == ']') return true;
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
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.project") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, "", value.begin)) return false;
        if (!require_member(data, "id", StringShape, "/data") ||
            !require_member(data, "name", StringShape, "/data") ||
            !require_member(data, "next_item_id", StringShape, "/data") ||
            !require_member(data, "root_sequence_id", StringShape, "/data")) return false;
        Span assets;
        Span sequences;
        bool has_assets = false;
        bool has_sequences = false;
        if (!member(data, "assets", assets, has_assets) ||
            !member(data, "sequences", sequences, has_sequences)) return false;
        if (!has_assets || !has_sequences) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0, "/data");
            return false;
        }
        if (has_assets && !governed_array(
                assets, assets_, limits_.max_assets, "/data/assets",
                [&](Span element, std::size_t index) {
                    return walk_asset(element, "/data/assets/" + std::to_string(index));
                })) return false;
        return !has_sequences || governed_array(
            sequences, sequences_, limits_.max_sequences, "/data/sequences",
            [&](Span element, std::size_t index) {
                return walk_sequence(element, "/data/sequences/" + std::to_string(index));
            });
    }

    bool walk_asset(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.asset") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "content_hash", StringShape, data_path) ||
            !require_member(data, "frame_count", StringShape, data_path) ||
            !require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "name", StringShape, data_path) ||
            !require_member(data, "sample_rate", ObjectShape, data_path) ||
            !require_member(data, "storage_policy", StringShape, data_path)) return false;
        Span locators;
        Span representations;
        bool has_locators = false;
        bool has_representations = false;
        if (!member(data, "locators", locators, has_locators) ||
            !member(data, "representations", representations, has_representations)) return false;
        if (!has_locators || !has_representations) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0, path + "/data");
            return false;
        }
        if (has_locators && !governed_array(
                locators, locators_, limits_.max_locators, path + "/data/locators",
                [](Span, std::size_t) { return true; })) return false;
        return !has_representations || governed_array(
            representations, representations_, limits_.max_representations,
            path + "/data/representations",
            [&](Span element, std::size_t index) {
                return walk_representation(
                    element, path + "/data/representations/" + std::to_string(index));
            });
    }

    bool walk_representation(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.asset_representation") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "content_hash", StringShape, data_path) ||
            !require_member(data, "role", StringShape, data_path) ||
            !require_member(data, "storage_policy", StringShape, data_path)) return false;
        Span locators;
        bool found = false;
        if (!member(data, "locators", locators, found)) return false;
        if (!found) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/locators");
            return false;
        }
        return governed_array(
            locators, locators_, limits_.max_locators, path + "/data/locators",
            [](Span, std::size_t) { return true; });
    }

    bool walk_sequence(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != detail::sequence_schema_policy.type_name) {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin,
                                      detail::sequence_schema_policy.oldest_readable_version,
                                      detail::sequence_schema_policy.current_version)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "absolute_duration", ObjectShape | NullShape, data_path) ||
            !require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "musical_duration", StringShape | NullShape, data_path) ||
            !require_member(data, "name", StringShape, data_path)) return false;
        Span tracks;
        Span markers;
        Span regions;
        bool has_tracks = false;
        bool has_markers = false;
        bool has_regions = false;
        if (!member(data, "tracks", tracks, has_tracks) ||
            !member(data, "markers", markers, has_markers) ||
            !member(data, "regions", regions, has_regions)) return false;
        const bool has_annotations = detail::sequence_has_annotations(version);
        if (!has_tracks) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/tracks");
            return false;
        }
        if (has_annotations && !has_markers) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/markers");
            return false;
        }
        if (has_annotations && !has_regions) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/regions");
            return false;
        }
        if (!has_annotations && has_markers) {
            set_error(PersistenceErrorCode::InvalidSchema, markers.begin, 0, 0,
                      path + "/data/markers");
            return false;
        }
        if (!has_annotations && has_regions) {
            set_error(PersistenceErrorCode::InvalidSchema, regions.begin, 0, 0,
                      path + "/data/regions");
            return false;
        }
        if (has_markers && !governed_array(
            markers, sequence_markers_, limits_.max_sequence_markers, path + "/data/markers",
            [&](Span element, std::size_t index) {
                const auto item_path = path + "/data/markers/" + std::to_string(index);
                std::string marker_type;
                std::uint32_t marker_version = 0;
                Span marker_data;
                bool marker_shape = false;
                if (!envelope(element, marker_type, marker_version, marker_data, marker_shape))
                    return false;
                if (marker_type != "pulp.timeline.sequence_marker") {
                    set_error(PersistenceErrorCode::InvalidSchema, element.begin, 0, 0,
                              item_path);
                    return false;
                }
                return require_structural_shape(marker_shape, marker_version, item_path,
                                                element.begin) &&
                       require_member(marker_data, "id", StringShape, item_path + "/data") &&
                       require_member(marker_data, "name", StringShape, item_path + "/data") &&
                       require_member(marker_data, "point", ObjectShape, item_path + "/data") &&
                       require_member(marker_data, "type", StringShape, item_path + "/data");
            })) return false;
        if (has_regions && !governed_array(
            regions, sequence_regions_, limits_.max_sequence_regions, path + "/data/regions",
            [&](Span element, std::size_t index) {
                const auto item_path = path + "/data/regions/" + std::to_string(index);
                std::string region_type;
                std::uint32_t region_version = 0;
                Span region_data;
                bool region_shape = false;
                if (!envelope(element, region_type, region_version, region_data, region_shape))
                    return false;
                if (region_type != "pulp.timeline.sequence_region") {
                    set_error(PersistenceErrorCode::InvalidSchema, element.begin, 0, 0,
                              item_path);
                    return false;
                }
                return require_structural_shape(region_shape, region_version, item_path,
                                                element.begin) &&
                       require_member(region_data, "id", StringShape, item_path + "/data") &&
                       require_member(region_data, "name", StringShape, item_path + "/data") &&
                       require_member(region_data, "range", ObjectShape, item_path + "/data");
            })) return false;
        return governed_array(
            tracks, tracks_, limits_.max_tracks, path + "/data/tracks",
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
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != detail::track_schema_policy.type_name) {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin,
                                      detail::track_schema_policy.oldest_readable_version,
                                      detail::track_schema_policy.current_version)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "name", StringShape, data_path)) return false;
        Span clips;
        Span devices;
        Span automation;
        bool has_clips = false;
        bool has_devices = false;
        bool has_automation = false;
        if (!member(data, "clips", clips, has_clips) ||
            !member(data, "device_chain", devices, has_devices) ||
            !member(data, "automation_lanes", automation, has_automation)) return false;
        if (!has_clips ||
            (!detail::track_has_device_chain(version) && has_devices) ||
            (detail::track_has_device_chain(version) && !has_devices) ||
            (!detail::track_has_automation_lanes(version) && has_automation) ||
            (detail::track_has_automation_lanes(version) && !has_automation)) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + (has_clips ? (detail::track_has_automation_lanes(version)
                                               ? "/data/automation_lanes"
                                               : "/data/device_chain")
                                        : "/data/clips"));
            return false;
        }
        if (!governed_array(
            clips, clips_, limits_.max_clips, path + "/data/clips",
            [&](Span element, std::size_t index) {
                return walk_clip(element,
                                 path + "/data/clips/" + std::to_string(index));
            })) return false;
        if (has_automation && !governed_array(
            automation, automation_lanes_, limits_.max_automation_lanes,
            path + "/data/automation_lanes", [&](Span element, std::size_t index) {
                return walk_automation_lane(
                    element, path + "/data/automation_lanes/" + std::to_string(index));
            })) return false;
        return !has_devices || governed_array(
            devices, device_placements_, limits_.max_device_placements,
            path + "/data/device_chain", [&](Span element, std::size_t index) {
                return walk_device_placement(
                    element, path + "/data/device_chain/" + std::to_string(index));
            });
    }

    bool walk_automation_lane(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
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
            !require_member(data, "target", ObjectShape, data_path)) return false;
        Span points;
        Span target;
        bool has_points = false;
        bool has_target = false;
        if (!member(data, "points", points, has_points) ||
            !member(data, "target", target, has_target) || !has_points || !has_target)
            return false;
        if (!governed_array(points, automation_points_, limits_.max_automation_points,
                            data_path + "/points", [&](Span point, std::size_t index) {
                                const auto point_path = data_path + "/points/" +
                                                        std::to_string(index);
                                return require_member(point, "curvature_bits", StringShape,
                                                      point_path) &&
                                       require_member(point, "id", StringShape, point_path) &&
                                       require_member(point, "interpolation", StringShape,
                                                      point_path) &&
                                       require_member(point, "position_ticks", StringShape,
                                                      point_path) &&
                                       require_member(point, "value_bits", StringShape,
                                                      point_path);
                            })) return false;
        EnvelopeView target_envelope;
        if (!envelope(target, target_envelope.type, target_envelope.version,
                      target_envelope.data, target_envelope.valid_shape)) return false;
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
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.device_placement") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin)) return false;
        return require_member(data, "id", StringShape, path + "/data");
    }

    bool walk_clip(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.clip") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "time_range", ObjectShape, data_path)) return false;
        Span content;
        bool found = false;
        if (!member(data, "content", content, found)) return false;
        if (!found) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/content");
            return false;
        }
        std::string content_type;
        Span content_data;
        if (!envelope(content, content_type, version, content_data, valid_shape)) return false;
        if (!valid_shape) {
            set_error(PersistenceErrorCode::InvalidSchema, content.begin, 0, 0,
                      path + "/data/content");
            return false;
        }
        if (content_type == "pulp.timeline.content.empty" && version == 1) return true;
        if (content_type == "pulp.timeline.content.media" && version == 1) {
            const auto content_path = path + "/data/content/data";
            return require_member(content_data, "asset_id", StringShape, content_path) &&
                   require_member(content_data, "frame_count", StringShape, content_path) &&
                   require_member(content_data, "source_start", StringShape, content_path);
        }
        if (content_type != "pulp.timeline.content.notes" || version != 1) return true;
        Span notes;
        bool has_notes = false;
        if (!member(content_data, "notes", notes, has_notes)) return false;
        if (!has_notes) {
            set_error(PersistenceErrorCode::InvalidSchema, content_data.begin, 0, 0,
                      path + "/data/content/data/notes");
            return false;
        }
        return governed_array(
            notes, notes_, limits_.max_notes, path + "/data/content/data/notes",
            [&](Span note, std::size_t index) {
                const auto note_path = path + "/data/content/data/notes/" +
                                       std::to_string(index);
                if (!has_shape(note, ObjectShape)) {
                    set_error(PersistenceErrorCode::InvalidSchema, note.begin, 0, 0,
                              note_path);
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
        return fail<StructuralPreflightSuccess>(PersistenceErrorCode::LimitExceeded, 0,
                                                json.size(), limits.max_input_bytes);
    return StructuralScanner(json, limits).run();
}

} // namespace pulp::timeline
