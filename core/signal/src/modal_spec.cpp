// ModalSpec <-> JSON. This is the only translation unit in core/signal that
// includes a JSON parser, which is what keeps the header-only pulp-signal
// INTERFACE target free of a choc dependency (see modal_spec.hpp, "Layering").

#include <pulp/signal/modal_spec.hpp>

#include <choc/text/choc_JSON.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace pulp::signal {
namespace {

// ── Field names ───────────────────────────────────────────────────────────
// Listed once each; the writer, the reader and the unknown-field check all
// read from these, so a field cannot be emitted under one name and looked up
// under another.

constexpr const char* kSchemaVersion = "schema_version";
constexpr const char* kName = "name";
constexpr const char* kDescription = "description";
constexpr const char* kModes = "modes";
constexpr const char* kExcitation = "excitation";
constexpr const char* kStrikeMap = "strike_map";
constexpr const char* kPickupMap = "pickup_map";
constexpr const char* kTolerances = "tolerances";

constexpr const char* kFreqHz = "freq_hz";
constexpr const char* kT60S = "t60_s";
constexpr const char* kGain = "gain";

constexpr const char* kContactS = "contact_s";
constexpr const char* kVelocity = "velocity";

constexpr const char* kGridPoints = "grid_points";
constexpr const char* kWeights = "weights";

constexpr const char* kFreqCents = "freq_cents";
constexpr const char* kT60Rel = "t60_rel";
constexpr const char* kGainRel = "gain_rel";
constexpr const char* kVerifySeconds = "verify_seconds";

/// Ceiling on a shape map's grid resolution. Nothing physical needs this many
/// samples of a mode shape; the bound is here to keep an untrusted count out
/// of a size computation.
constexpr int kMaxGridPoints = 1 << 20;

// ── Reading ───────────────────────────────────────────────────────────────

/// Record every member of `object` whose name is not in `known` as an ignored
/// unknown field, under the dotted `path` prefix.
void collect_unknown(const choc::value::ValueView& object, std::string_view path,
                     std::span<const char* const> known,
                     ModalSpecDiagnostics* diagnostics) {
    if (diagnostics == nullptr || !object.isObject()) return;
    object.visitObjectMembers([&](std::string_view member,
                                  const choc::value::ValueView&) {
        for (const char* k : known)
            if (member == k) return;
        std::string full;
        if (!path.empty()) {
            full.append(path);
            full.push_back('.');
        }
        full.append(member);
        diagnostics->unknown_fields.push_back(std::move(full));
    });
}

/// choc keeps JSON ints and floats as distinct types, so `1` and `1.0` arrive
/// as different values for the same authored quantity. Every number in a spec
/// is conceptually real-valued, so accept either.
bool read_number(const choc::value::ValueView& v, double& out) {
    if (v.isFloat32() || v.isFloat64()) {
        out = v.getFloat64();
        return true;
    }
    if (v.isInt32() || v.isInt64()) {
        out = static_cast<double>(v.getInt64());
        return true;
    }
    return false;
}

/// Read an optional number. Absent leaves `out` untouched (the caller's
/// default stands); present-but-not-a-number is an error, never a silent
/// fallback to the default.
bool read_optional_number(const choc::value::ValueView& object, const char* field,
                          std::string_view path, double& out, std::string& error) {
    const auto v = object[field];
    if (v.isVoid()) return true;
    if (!read_number(v, out)) {
        error = std::string(path) + field + " must be a number";
        return false;
    }
    return true;
}

bool read_required_number(const choc::value::ValueView& object, const char* field,
                          std::string_view path, double& out, std::string& error) {
    const auto v = object[field];
    if (v.isVoid()) {
        error = std::string(path) + field + " is required";
        return false;
    }
    if (!read_number(v, out)) {
        error = std::string(path) + field + " must be a number";
        return false;
    }
    return true;
}

/// Read a required count-like field. JSON has one numeric type and a spec is
/// an untrusted file, so `1e300` and `2.5` both reach here for a field that
/// must be a whole number in `[min, max]`. Narrowing a double that does not
/// fit an int is undefined behaviour, so the range is checked *before* the
/// cast rather than after.
bool read_required_int(const choc::value::ValueView& object, const char* field,
                       std::string_view path, int min, int max, int& out,
                       std::string& error) {
    double raw = 0.0;
    if (!read_required_number(object, field, path, raw, error)) return false;
    if (!std::isfinite(raw) || raw != std::floor(raw)) {
        error = std::string(path) + field + " must be a whole number";
        return false;
    }
    if (raw < static_cast<double>(min) || raw > static_cast<double>(max)) {
        error = std::string(path) + field + " must be between " + std::to_string(min) +
                " and " + std::to_string(max);
        return false;
    }
    out = static_cast<int>(raw);
    return true;
}

bool read_optional_string(const choc::value::ValueView& object, const char* field,
                          std::string_view path, std::string& out,
                          std::string& error) {
    const auto v = object[field];
    if (v.isVoid()) return true;
    if (!v.isString()) {
        error = std::string(path) + field + " must be a string";
        return false;
    }
    out = std::string(v.getString());
    return true;
}

bool read_shape_map(const choc::value::ValueView& root, const char* field,
                    ModalShapeMap& out, ModalSpecDiagnostics* diagnostics,
                    std::string& error) {
    const auto v = root[field];
    if (v.isVoid()) return true;
    if (!v.isObject()) {
        error = std::string(field) + " must be an object";
        return false;
    }

    static constexpr std::array<const char*, 2> known{kGridPoints, kWeights};
    collect_unknown(v, field, known, diagnostics);

    const std::string path = std::string(field) + ".";
    // Upper bound is arbitrary but finite: it exists so a bad grid_points
    // cannot make the expected-weights product overflow before validation
    // gets to compare it against the array that is actually present.
    if (!read_required_int(v, kGridPoints, path, 0, kMaxGridPoints, out.grid_points,
                           error))
        return false;

    const auto weights = v[kWeights];
    if (!weights.isArray()) {
        error = path + kWeights + " is required and must be an array";
        return false;
    }
    out.weights.clear();
    out.weights.reserve(weights.size());
    for (uint32_t i = 0; i < weights.size(); ++i) {
        double w = 0.0;
        if (!read_number(weights[i], w)) {
            error = path + kWeights + "[" + std::to_string(i) + "] must be a number";
            return false;
        }
        out.weights.push_back(static_cast<float>(w));
    }
    return true;
}

// ── Writing ───────────────────────────────────────────────────────────────

/// Emit a float as the shortest decimal that reads back as the same float.
///
/// The obvious `static_cast<double>(v)` is exact but unreadable: it widens the
/// float to a double and prints *that*, so an authored `1.6` re-saves as
/// `1.600000023841858`. A spec is meant to be read, diffed and reviewed, and a
/// format that churns every number the first time a tool touches it fails at
/// that regardless of how exact it is.
///
/// `to_chars` gives the shortest decimal that round-trips as a float, and the
/// decimal is then converted back by the same parser that will read the file,
/// so the stored double is by construction the one a load produces — no
/// locale-sensitive `strtod`, and no second decimal converter that could
/// disagree with choc's by a ULP. The load path narrows that double to float,
/// which recovers `v` exactly: the double is within half a double-ULP of a
/// decimal that already sits strictly inside `v`'s float rounding interval, so
/// it cannot round to a different float.
choc::value::Value float_value(float v) {
    // to_chars would emit "inf"/"nan", which is not JSON and would throw back
    // out of parseValue. A non-finite spec is rejected by validate_modal_spec;
    // this keeps the writer total anyway rather than throwing from to_json.
    if (!std::isfinite(v)) return choc::value::Value(static_cast<double>(v));
    // One byte reserved for the terminator: choc's parser walks a UTF8Pointer
    // and scans for a NUL, so a bare (pointer, length) view over an
    // unterminated buffer runs it into whatever follows on the stack.
    char buf[40];
    const auto result = std::to_chars(buf, buf + sizeof buf - 1, v);
    if (result.ec != std::errc{}) return choc::value::Value(static_cast<double>(v));
    *result.ptr = '\0';
    return choc::json::parseValue(
        std::string_view(buf, static_cast<std::size_t>(result.ptr - buf)));
}

choc::value::Value shape_map_to_value(const ModalShapeMap& map) {
    auto weights = choc::value::createEmptyArray();
    for (float w : map.weights) weights.addArrayElement(float_value(w));
    return choc::json::create(kGridPoints, map.grid_points, kWeights, weights);
}

} // namespace

// ── Validation ────────────────────────────────────────────────────────────

bool validate_modal_spec(const ModalSpec& spec, std::string& error) {
    error.clear();

    if (spec.schema_version < 1) {
        error = "schema_version must be at least 1";
        return false;
    }
    if (spec.schema_version > kModalSpecSchemaVersion) {
        error = "schema_version " + std::to_string(spec.schema_version) +
                " is newer than this build reads (" +
                std::to_string(kModalSpecSchemaVersion) + ")";
        return false;
    }
    if (spec.modes.empty()) {
        error = "modes must hold at least one mode";
        return false;
    }

    for (std::size_t m = 0; m < spec.modes.size(); ++m) {
        const auto& mode = spec.modes[m];
        const std::string at = "modes[" + std::to_string(m) + "].";
        if (!std::isfinite(mode.freq_hz) || mode.freq_hz <= 0.0f) {
            error = at + "freq_hz must be finite and greater than 0";
            return false;
        }
        if (!std::isfinite(mode.t60_s) || mode.t60_s <= 0.0f) {
            error = at + "t60_s must be finite and greater than 0";
            return false;
        }
        if (!std::isfinite(mode.gain)) {
            error = at + "gain must be finite";
            return false;
        }
    }

    if (!std::isfinite(spec.excitation.contact_s) || spec.excitation.contact_s < 0.0) {
        error = "excitation.contact_s must be finite and not negative";
        return false;
    }
    if (!std::isfinite(spec.excitation.velocity)) {
        error = "excitation.velocity must be finite";
        return false;
    }

    // A shape map is indexed [mode * grid_points + g] for every mode, so a
    // short table is not a partially-specified map to fill in — it is a
    // silent out-of-bounds read for the modes past its end.
    const auto check_map = [&](const ModalShapeMap& map, const char* name) {
        if (map.grid_points == 0 && map.weights.empty()) return true;
        if (map.grid_points < 2) {
            error = std::string(name) +
                    ".grid_points must be at least 2 (a map needs two points to "
                    "interpolate between)";
            return false;
        }
        const std::size_t expected = spec.modes.size() *
                                     static_cast<std::size_t>(map.grid_points);
        if (map.weights.size() != expected) {
            error = std::string(name) + ".weights has " +
                    std::to_string(map.weights.size()) + " entries, expected " +
                    std::to_string(expected) + " (" + std::to_string(spec.modes.size()) +
                    " modes x " + std::to_string(map.grid_points) + " grid points)";
            return false;
        }
        for (std::size_t i = 0; i < map.weights.size(); ++i) {
            if (!std::isfinite(map.weights[i])) {
                error = std::string(name) + ".weights[" + std::to_string(i) +
                        "] must be finite";
                return false;
            }
        }
        return true;
    };
    if (!check_map(spec.strike_map, kStrikeMap)) return false;
    if (!check_map(spec.pickup_map, kPickupMap)) return false;

    const auto check_tolerance = [&](double value, const char* name) {
        if (!std::isfinite(value) || value <= 0.0) {
            error = std::string("tolerances.") + name +
                    " must be finite and greater than 0";
            return false;
        }
        return true;
    };
    if (!check_tolerance(spec.tolerances.freq_cents, kFreqCents)) return false;
    if (!check_tolerance(spec.tolerances.t60_rel, kT60Rel)) return false;
    if (!check_tolerance(spec.tolerances.gain_rel, kGainRel)) return false;
    if (!check_tolerance(spec.tolerances.verify_seconds, kVerifySeconds)) return false;

    return true;
}

// ── Parsing ───────────────────────────────────────────────────────────────

std::optional<ModalSpec> parse_modal_spec(std::string_view json,
                                          ModalSpecDiagnostics* diagnostics) {
    if (diagnostics != nullptr) {
        diagnostics->error.clear();
        diagnostics->unknown_fields.clear();
    }
    const auto fail = [&](std::string message) -> std::optional<ModalSpec> {
        if (diagnostics != nullptr) diagnostics->error = std::move(message);
        return std::nullopt;
    };

    choc::value::Value root;
    // ParseError is caught by name rather than as a std::exception base: any
    // -fno-rtti static library co-linked into the same binary can emit a
    // local std::exception typeinfo that shadows libc++'s, and Apple compares
    // type_info by address, so a base-class catch silently misses.
    try {
        root = choc::json::parse(json);
    } catch (const choc::json::ParseError& e) {
        return fail("JSON parse error at line " +
                    std::to_string(e.lineAndColumn.line) + ", column " +
                    std::to_string(e.lineAndColumn.column) + ": " + e.what());
    } catch (const std::exception& e) {
        return fail(std::string("JSON parse error: ") + e.what());
    }

    if (!root.isObject()) return fail("root value must be a JSON object");

    static constexpr std::array<const char*, 8> known_root{
        kSchemaVersion, kName, kDescription, kModes,
        kExcitation,    kStrikeMap, kPickupMap, kTolerances};
    collect_unknown(root, "", known_root, diagnostics);

    ModalSpec spec;
    std::string error;

    // Any int is readable here; the "newer than this build" check below is
    // what rejects an unreadable version, and it owns that message.
    if (!read_required_int(root, kSchemaVersion, "", 1, std::numeric_limits<int>::max(),
                           spec.schema_version, error))
        return fail(std::move(error));
    // Version-gate before reading anything else: a newer schema may have
    // redefined a field this build would otherwise misread as v1.
    if (spec.schema_version > kModalSpecSchemaVersion) {
        return fail("schema_version " + std::to_string(spec.schema_version) +
                    " is newer than this build reads (" +
                    std::to_string(kModalSpecSchemaVersion) + ")");
    }

    if (!read_optional_string(root, kName, "", spec.name, error))
        return fail(std::move(error));
    if (!read_optional_string(root, kDescription, "", spec.description, error))
        return fail(std::move(error));

    const auto modes = root[kModes];
    if (!modes.isArray()) return fail("modes is required and must be an array");
    spec.modes.reserve(modes.size());
    static constexpr std::array<const char*, 3> known_mode{kFreqHz, kT60S, kGain};
    for (uint32_t i = 0; i < modes.size(); ++i) {
        const auto entry = modes[i];
        const std::string path = std::string(kModes) + "[" + std::to_string(i) + "]";
        if (!entry.isObject()) return fail(path + " must be an object");
        collect_unknown(entry, path, known_mode, diagnostics);

        const std::string field_path = path + ".";
        double freq = 0.0, t60 = 0.0, gain = 0.0;
        if (!read_required_number(entry, kFreqHz, field_path, freq, error))
            return fail(std::move(error));
        if (!read_required_number(entry, kT60S, field_path, t60, error))
            return fail(std::move(error));
        if (!read_required_number(entry, kGain, field_path, gain, error))
            return fail(std::move(error));
        spec.modes.push_back(ModalMode{static_cast<float>(freq),
                                       static_cast<float>(t60),
                                       static_cast<float>(gain)});
    }

    const auto excitation = root[kExcitation];
    if (!excitation.isVoid()) {
        if (!excitation.isObject()) return fail("excitation must be an object");
        static constexpr std::array<const char*, 2> known{kContactS, kVelocity};
        collect_unknown(excitation, kExcitation, known, diagnostics);
        if (!read_optional_number(excitation, kContactS, "excitation.",
                                  spec.excitation.contact_s, error))
            return fail(std::move(error));
        if (!read_optional_number(excitation, kVelocity, "excitation.",
                                  spec.excitation.velocity, error))
            return fail(std::move(error));
    }

    if (!read_shape_map(root, kStrikeMap, spec.strike_map, diagnostics, error))
        return fail(std::move(error));
    if (!read_shape_map(root, kPickupMap, spec.pickup_map, diagnostics, error))
        return fail(std::move(error));

    const auto tolerances = root[kTolerances];
    if (!tolerances.isVoid()) {
        if (!tolerances.isObject()) return fail("tolerances must be an object");
        static constexpr std::array<const char*, 4> known{kFreqCents, kT60Rel, kGainRel,
                                                          kVerifySeconds};
        collect_unknown(tolerances, kTolerances, known, diagnostics);
        if (!read_optional_number(tolerances, kFreqCents, "tolerances.",
                                  spec.tolerances.freq_cents, error))
            return fail(std::move(error));
        if (!read_optional_number(tolerances, kT60Rel, "tolerances.",
                                  spec.tolerances.t60_rel, error))
            return fail(std::move(error));
        if (!read_optional_number(tolerances, kGainRel, "tolerances.",
                                  spec.tolerances.gain_rel, error))
            return fail(std::move(error));
        if (!read_optional_number(tolerances, kVerifySeconds, "tolerances.",
                                  spec.tolerances.verify_seconds, error))
            return fail(std::move(error));
    }

    if (!validate_modal_spec(spec, error)) return fail(std::move(error));
    return spec;
}

// ── Serializing ───────────────────────────────────────────────────────────

std::string to_json(const ModalSpec& spec, bool pretty) {
    auto modes = choc::value::createEmptyArray();
    for (const auto& mode : spec.modes) {
        modes.addArrayElement(choc::json::create(kFreqHz, float_value(mode.freq_hz),
                                                 kT60S, float_value(mode.t60_s),
                                                 kGain, float_value(mode.gain)));
    }

    auto root = choc::json::create(
        kSchemaVersion, spec.schema_version,
        kName, spec.name,
        kDescription, spec.description,
        kModes, modes,
        kExcitation, choc::json::create(kContactS, spec.excitation.contact_s,
                                        kVelocity, spec.excitation.velocity),
        kTolerances, choc::json::create(kFreqCents, spec.tolerances.freq_cents,
                                        kT60Rel, spec.tolerances.t60_rel,
                                        kGainRel, spec.tolerances.gain_rel,
                                        kVerifySeconds, spec.tolerances.verify_seconds));

    // Absent maps stay absent rather than round-tripping as an empty object:
    // "no map" is a meaningful state (position has no effect, gains verbatim),
    // and an empty grid_points/weights pair would read as a malformed map.
    if (!spec.strike_map.empty())
        root.addMember(kStrikeMap, shape_map_to_value(spec.strike_map));
    if (!spec.pickup_map.empty())
        root.addMember(kPickupMap, shape_map_to_value(spec.pickup_map));

    return choc::json::toString(root, pretty);
}

bool operator==(const ModalSpec& a, const ModalSpec& b) {
    const auto modes_equal = [](const ModalMode& x, const ModalMode& y) {
        return x.freq_hz == y.freq_hz && x.t60_s == y.t60_s && x.gain == y.gain;
    };
    const auto maps_equal = [](const ModalShapeMap& x, const ModalShapeMap& y) {
        return x.grid_points == y.grid_points && x.weights == y.weights;
    };
    return a.schema_version == b.schema_version && a.name == b.name &&
           a.description == b.description &&
           a.modes.size() == b.modes.size() &&
           std::equal(a.modes.begin(), a.modes.end(), b.modes.begin(), modes_equal) &&
           a.excitation.contact_s == b.excitation.contact_s &&
           a.excitation.velocity == b.excitation.velocity &&
           maps_equal(a.strike_map, b.strike_map) &&
           maps_equal(a.pickup_map, b.pickup_map) &&
           a.tolerances.freq_cents == b.tolerances.freq_cents &&
           a.tolerances.t60_rel == b.tolerances.t60_rel &&
           a.tolerances.gain_rel == b.tolerances.gain_rel &&
           a.tolerances.verify_seconds == b.tolerances.verify_seconds;
}

} // namespace pulp::signal
