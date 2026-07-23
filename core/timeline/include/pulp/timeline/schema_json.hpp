#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::timeline {

enum class PersistenceErrorCode : std::uint8_t {
    InvalidJson,
    InvalidUtf8,
    DuplicateKey,
    LimitExceeded,
    MissingField,
    UnexpectedType,
    InvalidNumber,
    InvalidSchema,
    UnsupportedStructuralType,
    UnsupportedSchemaVersion,
    MigrationPathMissing,
    MigrationFailed,
    ModelRejected,
    OutputLimitExceeded,
};

struct PersistenceError {
    PersistenceError() = default;
    explicit PersistenceError(PersistenceErrorCode error_code, std::size_t offset = 0,
                              std::uint64_t observed = 0, std::uint64_t maximum = 0,
                              std::string error_path = {},
                              std::optional<ModelError> rejected_model = {})
        : code(error_code), byte_offset(offset), actual(observed), limit(maximum),
          path(std::move(error_path)), model_error(rejected_model) {}

    PersistenceErrorCode code = PersistenceErrorCode::InvalidJson;
    std::size_t byte_offset = 0;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
    std::string path;
    std::optional<ModelError> model_error;
};

struct DecodeLimits {
    std::size_t max_input_bytes = 1024ull * 1024ull * 1024ull;
    std::size_t max_depth = 64;
    std::size_t max_total_values = 30'000'000;
    std::size_t max_array_elements = 10'000'000;
    std::size_t max_object_members = 4'096;
    std::size_t max_string_bytes = 16ull * 1024ull * 1024ull;
    std::size_t max_opaque_bytes = 64ull * 1024ull * 1024ull;
    std::size_t max_migration_steps = 64;
    std::size_t max_assets = 100'000;
    std::size_t max_audio_loop_points = 1'000'000;
    std::size_t max_audio_loop_tags = 100'000;
    std::size_t max_sequences = 100'000;
    std::size_t max_tracks = 10'000;
    std::size_t max_clips = 100'000;
    std::size_t max_notes = 5'000'000;
    std::size_t max_locators = 1'000'000;
    std::size_t max_representations = 1'000'000;
    std::size_t max_device_placements = 100'000;
    std::size_t max_automation_lanes = 100'000;
    std::size_t max_automation_points = 5'000'000;
    std::size_t max_take_lanes = 100'000;
    std::size_t max_takes = 5'000'000;
    std::size_t max_take_comp_segments = 5'000'000;

    static DecodeLimits web_defaults() noexcept;
};

struct JsonValue {
    enum class Kind : std::uint8_t { Null, Boolean, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    std::string scalar;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::size_t begin = 0;
    std::size_t end = 0;

    const JsonValue* find(std::string_view key) const noexcept;
};

class ParsedJson {
  public:
    const JsonValue& root() const noexcept {
        return root_;
    }
    std::string_view source() const noexcept {
        return *source_;
    }
    std::string_view raw(const JsonValue& value) const noexcept;

  private:
    friend runtime::Result<std::shared_ptr<const ParsedJson>, PersistenceError>
    parse_json(std::string_view, const DecodeLimits&);
    std::shared_ptr<const std::string> source_;
    JsonValue root_;
};

runtime::Result<std::shared_ptr<const ParsedJson>, PersistenceError>
parse_json(std::string_view json, const DecodeLimits& limits = {});

// Exact extension/migration envelope contract. The object must contain only
// data, type_name and version; data must itself be an object.
runtime::Result<const JsonValue*, PersistenceError>
validate_exact_envelope(const JsonValue& value, std::string_view expected_type,
                        std::uint32_t expected_version, std::string path = {},
                        PersistenceErrorCode failure_code = PersistenceErrorCode::InvalidSchema);

// Allocation-light schema-aware quota pass used before the generic DOM parser.
// Only arrays reached through supported structural envelopes are counted.
struct StructuralPreflightSuccess {};
runtime::Result<StructuralPreflightSuccess, PersistenceError>
preflight_timeline_structure(std::string_view json, const DecodeLimits& limits);

runtime::Result<std::string, PersistenceError> canonicalize_json(const JsonValue& value);
std::string quote_json_string(std::string_view value);
bool is_valid_utf8(std::string_view value) noexcept;

runtime::Result<std::uint64_t, PersistenceError> parse_canonical_u64_string(const JsonValue& value,
                                                                            std::string path = {});
runtime::Result<std::int64_t, PersistenceError> parse_canonical_i64_string(const JsonValue& value,
                                                                           std::string path = {});
runtime::Result<std::uint32_t, PersistenceError> parse_u32_number(const JsonValue& value,
                                                                  std::string path = {});

} // namespace pulp::timeline
