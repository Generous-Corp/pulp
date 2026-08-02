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

/** @addtogroup timeline_persistence
 * @{
 */

/// Stable category for a JSON, schema, quota, migration, or model rejection.
enum class PersistenceErrorCode : std::uint8_t {
    InvalidJson,               ///< JSON syntax or document shape is invalid.
    InvalidUtf8,               ///< A required string is not valid UTF-8.
    DuplicateKey,              ///< An object repeats a member name.
    LimitExceeded,             ///< An input, structure, or migration quota was exceeded.
    MissingField,              ///< A required schema field is absent.
    UnexpectedType,            ///< A JSON value has the wrong kind.
    InvalidNumber,             ///< A numeric value is malformed or out of range.
    InvalidSchema,             ///< A schema identity, envelope, or codec contract is invalid.
    UnsupportedStructuralType, ///< Structural preflight encountered an unknown typed envelope.
    UnsupportedSchemaVersion,  ///< No supported decoder version accepts the envelope.
    MigrationPathMissing,      ///< Registered migrations do not connect source to target.
    MigrationFailed,           ///< A migration returned or produced invalid output.
    ModelRejected,             ///< The decoded value violates a Timeline model invariant.
    OutputLimitExceeded,       ///< Generated JSON exceeded its output ceiling.
};

/// Structured persistence failure.
///
/// byte_offset locates syntax/encoding failures in the original input. actual
/// and limit describe quota failures. path is a JSON Pointer-like model path,
/// and model_error carries the underlying model invariant rejection when code
/// is ModelRejected. Fields not applicable to a code remain zero or empty.
struct PersistenceError {
    /// Constructs an InvalidJson error with no source location.
    PersistenceError() = default;
    /// Constructs a structured persistence error from stage-specific details.
    ///
    /// @param error_code Failure category.
    /// @param offset Source byte offset, when applicable.
    /// @param observed Observed size or count for a quota failure.
    /// @param maximum Configured ceiling for a quota failure.
    /// @param error_path JSON Pointer-like diagnostic path.
    /// @param rejected_model Underlying model failure for ModelRejected.
    explicit PersistenceError(PersistenceErrorCode error_code, std::size_t offset = 0,
                              std::uint64_t observed = 0, std::uint64_t maximum = 0,
                              std::string error_path = {},
                              std::optional<ModelError> rejected_model = {})
        : code(error_code), byte_offset(offset), actual(observed), limit(maximum),
          path(std::move(error_path)), model_error(rejected_model) {}

    PersistenceErrorCode code = PersistenceErrorCode::InvalidJson; ///< Failure category.
    std::size_t byte_offset = 0; ///< Source byte offset, when available.
    std::uint64_t actual = 0;   ///< Observed size or count for a quota failure.
    std::uint64_t limit = 0;    ///< Configured ceiling for a quota failure.
    std::string path;           ///< JSON Pointer-like path to the rejected value.
    std::optional<ModelError> model_error; ///< Underlying ModelRejected detail.
};

/// Parser, migration, opaque-payload, and Timeline structure quotas.
///
/// All byte counts measure bytes, not Unicode scalar values. Container counts
/// apply per container unless named as totals. The same limits object should be
/// passed through structural preflight, parsing, migrations, and model decode
/// so no later stage widens admission.
struct DecodeLimits {
    /// Maximum complete input size.
    std::size_t max_input_bytes = 1024ull * 1024ull * 1024ull;
    /// Maximum JSON container nesting depth.
    std::size_t max_depth = 64;
    /// Maximum total DOM values.
    std::size_t max_total_values = 30'000'000;
    /// Maximum elements in one JSON array.
    std::size_t max_array_elements = 10'000'000;
    /// Maximum members in one JSON object.
    std::size_t max_object_members = 4'096;
    /// Maximum decoded bytes in one string or key.
    std::size_t max_string_bytes = 16ull * 1024ull * 1024ull;
    /// Maximum raw bytes retained by one opaque envelope.
    std::size_t max_opaque_bytes = 64ull * 1024ull * 1024ull;
    /// Maximum adjacent migration callbacks per value.
    std::size_t max_migration_steps = 64;
    /// Maximum media assets in one project.
    std::size_t max_assets = 100'000;
    /// Maximum loop slice points across all assets.
    std::size_t max_audio_loop_points = 1'000'000;
    /// Maximum loop tags across all assets.
    std::size_t max_audio_loop_tags = 100'000;
    /// Maximum project sequences.
    std::size_t max_sequences = 100'000;
    /// Maximum tracks across all sequences.
    std::size_t max_tracks = 10'000;
    /// Maximum clips across all tracks.
    std::size_t max_clips = 100'000;
    /// Maximum note events across all note content.
    std::size_t max_notes = 5'000'000;
    /// Maximum asset locators.
    std::size_t max_locators = 1'000'000;
    /// Maximum alternate asset representations.
    std::size_t max_representations = 1'000'000;
    /// Maximum device placements across all tracks.
    std::size_t max_device_placements = 100'000;
    /// Maximum automation lanes across all tracks.
    std::size_t max_automation_lanes = 100'000;
    /// Maximum automation points across all lanes.
    std::size_t max_automation_points = 5'000'000;
    /// Maximum modulators across all tracks.
    std::size_t max_modulators = 100'000;
    /// Maximum macro controls across all tracks.
    std::size_t max_macro_controls = 100'000;
    /// Maximum modulation routes across all tracks.
    std::size_t max_modulation_routes = 1'000'000;
    /// Maximum take lanes across all tracks.
    std::size_t max_take_lanes = 100'000;
    /// Maximum takes across all take lanes.
    std::size_t max_takes = 5'000'000;
    /// Maximum take-comp segments across all lanes.
    std::size_t max_take_comp_segments = 5'000'000;
    /// Maximum sequence markers.
    std::size_t max_markers = 100'000;
    /// Maximum sequence regions.
    std::size_t max_regions = 100'000;
    /// Maximum launch scenes.
    std::size_t max_scenes = 100'000;
    /// Maximum launch slots across all scenes.
    std::size_t max_slots = 1'000'000;
    /// Maximum chord/scale events.
    std::size_t max_chord_scale_events = 1'000'000;
    /// Maximum groove-template steps.
    std::size_t max_groove_steps = 100'000;
    /// Maximum controller/expression lanes across all MIDI content.
    std::size_t max_midi_lanes = 100'000;
    /// Maximum controller points across all lanes.
    std::size_t max_midi_lane_points = 5'000'000;

    /// Returns tighter ceilings suitable for memory-constrained web runtimes.
    static DecodeLimits web_defaults() noexcept;
};

/// Owning JSON DOM node with source byte bounds.
///
/// Strings and object keys are decoded UTF-8. Number preserves the lexical
/// representation in scalar so canonical numeric validation remains explicit.
struct JsonValue {
    /// JSON value category.
    enum class Kind : std::uint8_t {
        Null,    ///< JSON null.
        Boolean, ///< JSON true or false.
        Number,  ///< JSON number with its lexical spelling in scalar.
        String,  ///< Decoded JSON string in scalar.
        Array,   ///< Ordered child values in array.
        Object,  ///< Ordered decoded key/value pairs in object.
    };

    /// Value category.
    Kind kind = Kind::Null;
    /// Boolean payload when kind is Boolean.
    bool boolean = false;
    /// Decoded string or original number spelling.
    std::string scalar;
    /// Child values when kind is Array.
    std::vector<JsonValue> array;
    /// Decoded key/value pairs when kind is Object.
    std::vector<std::pair<std::string, JsonValue>> object;
    /// Inclusive source byte offset.
    std::size_t begin = 0;
    /// Exclusive source byte offset.
    std::size_t end = 0;

    /// Finds an exact member on an object.
    ///
    /// @param key Decoded member name.
    /// @return A pointer valid for this DOM's lifetime, or nullptr for a
    ///         non-object or missing key.
    const JsonValue* find(std::string_view key) const noexcept;
};

/// Parsed JSON tree that owns the source bytes referenced by node offsets.
///
/// The object is immutable after parsing and safe for concurrent reads. Views
/// returned by source() and raw() remain valid while this ParsedJson lives.
class ParsedJson {
  public:
    /// Returns the root node.
    const JsonValue& root() const noexcept {
        return root_;
    }
    /// Returns the complete owned source.
    std::string_view source() const noexcept {
        return *source_;
    }
    /// Returns the original byte slice for a node from this parse.
    ///
    /// Provenance is checked by node identity, not inferred from byte offsets:
    /// `value` must be the exact root() object or one of its exact descendants.
    /// A copied node, a manually constructed node, or a node owned by another
    /// ParsedJson is rejected even when its bounds fit source().
    ///
    /// @param value Node belonging to this parse tree.
    /// @return The node's original source bytes, or an empty view when provenance
    ///         or source bounds fail validation. The function never throws.
    std::string_view raw(const JsonValue& value) const noexcept;

  private:
    // Parameters are named here to match the free function's declaration.
    // Doxygen attaches that function's `@param` block to this friend
    // declaration, and unnamed parameters make the names unresolvable —
    // a strict docs build then fails on `argument 'json' ... is not found`.
    friend runtime::Result<std::shared_ptr<const ParsedJson>, PersistenceError>
    parse_json(std::string_view json, const DecodeLimits& limits);

    ParsedJson() = default;
    ParsedJson(const ParsedJson&) = delete;
    ParsedJson& operator=(const ParsedJson&) = delete;
    ParsedJson(ParsedJson&&) = delete;
    ParsedJson& operator=(ParsedJson&&) = delete;

    std::shared_ptr<const std::string> source_;
    JsonValue root_;
};

/// Parses one complete UTF-8 JSON value into an owning immutable document.
///
/// Duplicate object keys are rejected. Input bytes are copied, and all parser
/// depth, value, container, and string ceilings are enforced before success.
///
/// @param json Complete JSON text, borrowed for the call.
/// @param limits Parser resource ceilings.
/// @return An owning immutable parse tree or a structured parse error.
runtime::Result<std::shared_ptr<const ParsedJson>, PersistenceError>
parse_json(std::string_view json, const DecodeLimits& limits = {});

/// Validates an exact extension or migration envelope.
///
/// value must contain only `data`, `type_name`, and `version`; `data` must be
/// an object, and identity fields must equal the expected values.
///
/// @param value Candidate envelope.
/// @param expected_type Required type_name value.
/// @param expected_version Required version value.
/// @param path Diagnostic path for the envelope.
/// @param failure_code Error category returned for envelope mismatch.
/// @return A pointer to `data` owned by value, or failure_code at path.
runtime::Result<const JsonValue*, PersistenceError>
validate_exact_envelope(const JsonValue& value, std::string_view expected_type,
                        std::uint32_t expected_version, std::string path = {},
                        PersistenceErrorCode failure_code = PersistenceErrorCode::InvalidSchema);

/// Empty success value returned by structural preflight.
struct StructuralPreflightSuccess {};
/// Performs an allocation-light, schema-aware Timeline quota scan.
///
/// Only arrays reached through supported structural envelopes contribute to
/// the named Timeline item ceilings. This pass does not replace full JSON
/// parsing or model validation.
///
/// @param json Complete Timeline JSON, borrowed for the call.
/// @param limits Input and Timeline structure ceilings.
/// @return Success or the first syntax, structure, or quota failure.
runtime::Result<StructuralPreflightSuccess, PersistenceError>
preflight_timeline_structure(std::string_view json, const DecodeLimits& limits);

/// Serializes a DOM value as deterministic JSON with sorted object keys.
///
/// Strings are escaped as needed and numbers must already use an accepted JSON
/// lexical form.
///
/// @param value Complete DOM subtree.
/// @return Canonical JSON or InvalidJson for an invalid DOM value.
runtime::Result<std::string, PersistenceError> canonicalize_json(const JsonValue& value);
/// Quotes and escapes a byte sequence as a JSON string token.
///
/// The function escapes JSON syntax and control bytes but does not validate
/// Unicode. Callers producing a JSON document must validate UTF-8 first.
///
/// @param value Unquoted bytes to escape.
/// @return A quoted JSON string token.
std::string quote_json_string(std::string_view value);
/// Validates a complete byte sequence as UTF-8.
///
/// @param value Bytes to validate.
/// @return Whether the complete sequence is valid UTF-8.
bool is_valid_utf8(std::string_view value) noexcept;

/// Parses a canonical decimal unsigned integer stored as a JSON string.
///
/// Leading zeroes, signs, non-digits, and overflow are rejected.
///
/// @param value JSON string node to parse.
/// @param path Diagnostic path for a rejection.
/// @return The parsed integer or a typed numeric error.
runtime::Result<std::uint64_t, PersistenceError> parse_canonical_u64_string(const JsonValue& value,
                                                                            std::string path = {});
/// Parses a canonical decimal signed integer stored as a JSON string.
///
/// Redundant leading zeroes, a leading plus, negative zero, non-digits, and
/// overflow are rejected.
///
/// @param value JSON string node to parse.
/// @param path Diagnostic path for a rejection.
/// @return The parsed integer or a typed numeric error.
runtime::Result<std::int64_t, PersistenceError> parse_canonical_i64_string(const JsonValue& value,
                                                                           std::string path = {});
/// Parses an integral JSON number representable as uint32_t.
///
/// Fractions, exponents, signs, noncanonical leading zeroes, and overflow are
/// rejected.
///
/// @param value JSON number node to parse.
/// @param path Diagnostic path for a rejection.
/// @return The parsed integer or a typed numeric error.
runtime::Result<std::uint32_t, PersistenceError> parse_u32_number(const JsonValue& value,
                                                                  std::string path = {});

/// @}

} // namespace pulp::timeline
