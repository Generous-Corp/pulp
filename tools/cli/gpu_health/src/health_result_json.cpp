#include <pulp_tooling/gpu_health/health_result.hpp>

#include <choc/text/choc_JSON.h>

#include <array>
#include <exception>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace pulp::tooling::gpu_health {
namespace {

template <typename Enum, std::size_t Size>
std::optional<Enum> enum_from_string(
    std::string_view value,
    const std::array<std::pair<Enum, std::string_view>, Size>& entries) {
    for (const auto& [candidate, name] : entries)
        if (value == name) return candidate;
    return std::nullopt;
}

constexpr std::array kVerdicts{
    std::pair{ Verdict::pass, std::string_view{ "pass" } },
    std::pair{ Verdict::fail, std::string_view{ "fail" } },
    std::pair{ Verdict::unavailable, std::string_view{ "unavailable" } },
    std::pair{ Verdict::unverified, std::string_view{ "unverified" } },
};

constexpr std::array kStages{
    std::pair{ Stage::configuration, std::string_view{ "configuration" } },
    std::pair{ Stage::adapter, std::string_view{ "adapter" } },
    std::pair{ Stage::shader_compile, std::string_view{ "shader_compile" } },
    std::pair{ Stage::pipeline_create, std::string_view{ "pipeline_create" } },
    std::pair{ Stage::render, std::string_view{ "render" } },
    std::pair{ Stage::submit, std::string_view{ "submit" } },
    std::pair{ Stage::readback, std::string_view{ "readback" } },
    std::pair{ Stage::content, std::string_view{ "content" } },
    std::pair{ Stage::compute, std::string_view{ "compute" } },
    std::pair{ Stage::device_state, std::string_view{ "device_state" } },
};

constexpr std::array kAdapterClasses{
    std::pair{ AdapterClass::hardware, std::string_view{ "hardware" } },
    std::pair{ AdapterClass::software, std::string_view{ "software" } },
    std::pair{ AdapterClass::null_adapter, std::string_view{ "null" } },
    std::pair{ AdapterClass::unknown, std::string_view{ "unknown" } },
};

constexpr std::array kHealthStates{
    std::pair{ HealthState::healthy, std::string_view{ "healthy" } },
    std::pair{ HealthState::failed, std::string_view{ "failed" } },
    std::pair{ HealthState::unavailable, std::string_view{ "unavailable" } },
    std::pair{ HealthState::unverified, std::string_view{ "unverified" } },
    std::pair{ HealthState::lost, std::string_view{ "lost" } },
};

constexpr std::array kIdentityStatuses{
    std::pair{ IdentityStatus::authentic, std::string_view{ "authentic" } },
    std::pair{ IdentityStatus::unverified, std::string_view{ "unverified" } },
    std::pair{ IdentityStatus::unavailable, std::string_view{ "unavailable" } },
};

struct EvidenceCodeBinding {
    std::string_view code;
    Stage stage;
    Verdict verdict;
};

constexpr std::array kSpecificEvidenceCodeBindings{
    EvidenceCodeBinding{"gpu_compute_adapter_acquired", Stage::adapter, Verdict::pass},
    EvidenceCodeBinding{"gpu_compute_adapter_identity_unverified", Stage::adapter, Verdict::unverified},
    EvidenceCodeBinding{"gpu_compute_device_lost", Stage::device_state, Verdict::fail},
    EvidenceCodeBinding{"gpu_compute_execution_failed", Stage::compute, Verdict::fail},
    EvidenceCodeBinding{"gpu_compute_initialization_unavailable", Stage::adapter, Verdict::unavailable},
    EvidenceCodeBinding{"gpu_compute_not_built", Stage::configuration, Verdict::unavailable},
    EvidenceCodeBinding{"gpu_compute_oracle_mismatch", Stage::compute, Verdict::fail},
    EvidenceCodeBinding{"gpu_compute_oracle_passed", Stage::compute, Verdict::pass},
    EvidenceCodeBinding{"render_not_requested", Stage::configuration, Verdict::unverified},
    EvidenceCodeBinding{"renderer3d_adapter_unavailable", Stage::adapter, Verdict::unavailable},
    EvidenceCodeBinding{"renderer3d_blank_output", Stage::content, Verdict::fail},
    EvidenceCodeBinding{"renderer3d_content_floor_passed", Stage::content, Verdict::pass},
    EvidenceCodeBinding{"renderer3d_not_compiled", Stage::configuration, Verdict::unavailable},
    EvidenceCodeBinding{"renderer3d_setup_failed", Stage::pipeline_create, Verdict::fail},
    EvidenceCodeBinding{"renderer3d_readback_completed", Stage::readback, Verdict::pass},
    EvidenceCodeBinding{"renderer3d_readback_failed", Stage::readback, Verdict::fail},
    EvidenceCodeBinding{"renderer3d_render_completed", Stage::render, Verdict::pass},
    EvidenceCodeBinding{"renderer3d_submit_completed", Stage::submit, Verdict::pass},
    EvidenceCodeBinding{"skia_graphite_content_floor_passed", Stage::content, Verdict::pass},
    EvidenceCodeBinding{"skia_graphite_content_mismatch", Stage::content, Verdict::fail},
    EvidenceCodeBinding{"skia_graphite_frame_failed", Stage::render, Verdict::fail},
    EvidenceCodeBinding{"skia_graphite_readback_completed", Stage::readback, Verdict::pass},
    EvidenceCodeBinding{"skia_graphite_render_completed", Stage::render, Verdict::pass},
    EvidenceCodeBinding{"skia_graphite_unavailable", Stage::configuration, Verdict::unavailable},
    EvidenceCodeBinding{"wgsl.async_uncaptured_error", Stage::shader_compile, Verdict::fail},
};

bool evidence_code_matches(std::string_view code, Stage stage, Verdict verdict) {
    if (code.starts_with("gpu.")) {
        if (code == "gpu.adapter.null")
            return stage == Stage::adapter && verdict == Verdict::fail;
        return code == "gpu." + std::string(to_string(stage)) + "." +
                           std::string(to_string(verdict));
    }
    for (const auto& binding : kSpecificEvidenceCodeBindings)
        if (binding.code == code)
            return binding.stage == stage && binding.verdict == verdict;
    return false;
}

template <typename Enum, std::size_t Size>
std::string_view enum_to_string(
    Enum value,
    const std::array<std::pair<Enum, std::string_view>, Size>& entries) {
    for (const auto& [candidate, name] : entries)
        if (value == candidate) return name;
    return "unknown";
}

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

bool valid_utc_timestamp(std::string_view value) {
    const auto digit = [&](std::size_t index) {
        return index < value.size() && value[index] >= '0' && value[index] <= '9';
    };
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z' || !digit(0) || !digit(1) || !digit(2) ||
        !digit(3) || !digit(5) || !digit(6) || !digit(8) || !digit(9) ||
        !digit(11) || !digit(12) || !digit(14) || !digit(15) ||
        !digit(17) || !digit(18))
        return false;
    const auto two_digits = [&](std::size_t index) {
        return (value[index] - '0') * 10 + (value[index + 1] - '0');
    };
    const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
                     (value[2] - '0') * 10 + value[3] - '0';
    const int month = two_digits(5);
    const int day = two_digits(8);
    const int hour = two_digits(11);
    const int minute = two_digits(14);
    const int second = two_digits(17);
    if (year == 0 || month < 1 || month > 12 || hour > 23 || minute > 59 ||
        second > 59)
        return false;
    constexpr std::array days_per_month{ 31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31 };
    int maximum_day = days_per_month[static_cast<std::size_t>(month - 1)];
    if (month == 2 && (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0))
        maximum_day = 29;
    return day >= 1 && day <= maximum_day;
}

Verdict combined_verdict(bool any_fail, bool any_unavailable,
                         bool any_unverified) {
    if (any_fail) return Verdict::fail;
    if (any_unavailable) return Verdict::unavailable;
    if (any_unverified) return Verdict::unverified;
    return Verdict::pass;
}

template <typename T>
void set_optional(choc::value::Value& object, const char* key,
                  const std::optional<T>& value) {
    if (value.has_value())
        object.setMember(key, *value);
    else
        object.setMember(key, choc::value::Value());
}

void set_optional_u64(choc::value::Value& object, const char* key,
                      const std::optional<std::uint64_t>& value) {
    if (value.has_value())
        object.setMember(key, static_cast<std::int64_t>(*value));
    else
        object.setMember(key, choc::value::Value());
}

bool has_only_fields(const choc::value::ValueView& object,
                     std::span<const char* const> fields,
                     std::string_view path, std::string& error) {
    if (!object.isObject()) {
        error = std::string(path) + " must be an object";
        return false;
    }
    bool valid = true;
    object.visitObjectMembers([&](std::string_view member,
                                  const choc::value::ValueView&) {
        for (const auto* field : fields)
            if (member == field) return;
        if (valid) {
            error = std::string(path) + " has unknown member '" +
                    std::string(member) + "'";
            valid = false;
        }
    });
    return valid;
}

bool read_required_string(const choc::value::ValueView& object, const char* key,
                          std::string_view path, std::string& out,
                          std::string& error) {
    if (!object.hasObjectMember(key) || !object[key].isString()) {
        error = std::string(path) + key + " is required and must be a string";
        return false;
    }
    out = std::string(object[key].getString());
    return true;
}

bool read_required_bool(const choc::value::ValueView& object, const char* key,
                        std::string_view path, bool& out, std::string& error) {
    if (!object.hasObjectMember(key) || !object[key].isBool()) {
        error = std::string(path) + key + " is required and must be a boolean";
        return false;
    }
    out = object[key].getBool();
    return true;
}

bool read_required_u64(const choc::value::ValueView& object, const char* key,
                       std::string_view path, std::uint64_t& out,
                       std::string& error) {
    if (!object.hasObjectMember(key) ||
        !(object[key].isInt32() || object[key].isInt64())) {
        error = std::string(path) + key +
                " is required and must be a non-negative integer";
        return false;
    }
    const auto value = object[key].getInt64();
    if (value < 0) {
        error = std::string(path) + key + " must be non-negative";
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

std::string compact_json(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    bool in_string = false;
    bool escaped = false;
    for (const char character : input) {
        if (in_string) {
            output.push_back(character);
            if (escaped)
                escaped = false;
            else if (character == '\\')
                escaped = true;
            else if (character == '"')
                in_string = false;
        } else if (character == '"') {
            in_string = true;
            output.push_back(character);
        } else if (character != ' ' && character != '\n' && character != '\r' &&
                   character != '\t') {
            output.push_back(character);
        }
    }
    return output;
}

bool read_nullable_string(const choc::value::ValueView& object, const char* key,
                          std::string_view path,
                          std::optional<std::string>& out,
                          std::string& error) {
    if (!object.hasObjectMember(key)) {
        error = std::string(path) + key + " is required";
        return false;
    }
    const auto value = object[key];
    if (value.isVoid()) {
        out.reset();
        return true;
    }
    if (!value.isString()) {
        error = std::string(path) + key + " must be a string or null";
        return false;
    }
    out = std::string(value.getString());
    return true;
}

bool read_nullable_bool(const choc::value::ValueView& object, const char* key,
                        std::string_view path, std::optional<bool>& out,
                        std::string& error) {
    if (!object.hasObjectMember(key)) {
        error = std::string(path) + key + " is required";
        return false;
    }
    const auto value = object[key];
    if (value.isVoid()) {
        out.reset();
        return true;
    }
    if (!value.isBool()) {
        error = std::string(path) + key + " must be a boolean or null";
        return false;
    }
    out = value.getBool();
    return true;
}

bool read_nullable_u64(const choc::value::ValueView& object, const char* key,
                       std::string_view path, std::optional<std::uint64_t>& out,
                       std::string& error) {
    if (!object.hasObjectMember(key)) {
        error = std::string(path) + key + " is required";
        return false;
    }
    if (object[key].isVoid()) {
        out.reset();
        return true;
    }
    std::uint64_t value = 0;
    if (!read_required_u64(object, key, path, value, error)) return false;
    out = value;
    return true;
}

template <typename Enum, typename Parser>
bool read_required_enum(const choc::value::ValueView& object, const char* key,
                        std::string_view path, Enum& out, Parser parser,
                        std::string& error) {
    std::string text;
    if (!read_required_string(object, key, path, text, error)) return false;
    const auto parsed = parser(text);
    if (!parsed.has_value()) {
        error = std::string(path) + key + " has unsupported value '" + text + "'";
        return false;
    }
    out = *parsed;
    return true;
}

} // namespace

std::string_view to_string(Verdict value) { return enum_to_string(value, kVerdicts); }
std::string_view to_string(Stage value) { return enum_to_string(value, kStages); }
std::string_view to_string(AdapterClass value) {
    return enum_to_string(value, kAdapterClasses);
}
std::string_view to_string(HealthState value) {
    return enum_to_string(value, kHealthStates);
}
std::string_view to_string(IdentityStatus value) {
    return enum_to_string(value, kIdentityStatuses);
}

std::optional<Verdict> verdict_from_string(std::string_view value) {
    return enum_from_string(value, kVerdicts);
}
std::optional<Stage> stage_from_string(std::string_view value) {
    return enum_from_string(value, kStages);
}
std::optional<AdapterClass> adapter_class_from_string(std::string_view value) {
    return enum_from_string(value, kAdapterClasses);
}
std::optional<HealthState> health_state_from_string(std::string_view value) {
    return enum_from_string(value, kHealthStates);
}
std::optional<IdentityStatus> identity_status_from_string(std::string_view value) {
    return enum_from_string(value, kIdentityStatuses);
}

bool validate(const HealthResult& result, std::string* error) {
    const auto fail = [&](std::string message) {
        set_error(error, std::move(message));
        return false;
    };
    const bool is_v1 = result.schema == kSchemaV1 && result.version == kVersionV1;
    const bool is_v2 = result.schema == kSchema && result.version == kVersion;
    if (!is_v1 && !is_v2)
        return fail("schema and version must identify GPU-health result v1 or v2");
    if (result.run_id.empty() || result.run_id.size() > 128)
        return fail("run_id must contain 1..128 characters");
    if (is_v1 && !result.measured_at_utc.empty())
        return fail("measured_at_utc is not a v1 field");
    if (is_v2 && !valid_utc_timestamp(result.measured_at_utc))
        return fail("v2 measured_at_utc must be an exact valid Gregorian UTC timestamp");
    if (result.probes.empty() || result.probes.size() > 16)
        return fail("probes must contain 1..16 entries");
    if (result.recommendations.size() > 32)
        return fail("recommendations exceeds 32 entries");
    for (const auto& recommendation : result.recommendations)
        if (recommendation.empty() || recommendation.size() > 512)
            return fail("each recommendation must contain 1..512 characters");

    std::set<std::string> probe_ids;
    std::uint64_t next_sequence = 0;
    bool any_fail = false;
    bool any_unavailable = false;
    bool any_unverified = false;
    bool any_required = false;
    bool any_required_readback_pixels = false;
    bool any_required_authentic_identity = false;
    bool any_device_lost = false;

    for (const auto& probe : result.probes) {
        if (probe.probe_id.empty() || probe.probe_id.size() > 64)
            return fail("probe_id must contain 1..64 characters");
        if (!probe_ids.insert(probe.probe_id).second)
            return fail("probe_id values must be unique");
        if (probe.events.empty() || probe.events.size() > kStages.size())
            return fail("each probe must contain 1..10 events");

        if (probe.adapter.classification == AdapterClass::hardware &&
            probe.adapter.status != IdentityStatus::authentic)
            return fail("hardware adapter classification requires authentic identity");
        if (probe.adapter.status == IdentityStatus::authentic &&
            (!probe.adapter.backend.has_value() || probe.adapter.backend->empty()))
            return fail("authentic adapter identity requires a backend");
        if (probe.adapter.classification == AdapterClass::null_adapter &&
            probe.verdict == Verdict::pass)
            return fail("a null adapter cannot pass a probe");

        bool probe_fail = false;
        bool probe_unavailable = false;
        bool probe_unverified = false;
        std::set<Stage> seen_stages;
        for (const auto& event : probe.events) {
            if (event.sequence != next_sequence)
                return fail("event sequence must be globally contiguous from zero");
            ++next_sequence;
            if (!seen_stages.insert(event.stage).second)
                return fail("a probe cannot report the same stage twice");
            if (!is_known_evidence_code(event.code))
                return fail("event code is not registered by the GPU-health result contract");
            if (!evidence_code_matches(event.code, event.stage, event.verdict))
                return fail("event code does not match its registered stage and verdict");
            if (event.detail.empty() || event.detail.size() > 1024)
                return fail("event detail must contain 1..1024 characters");
            probe_fail |= event.verdict == Verdict::fail;
            probe_unavailable |= event.verdict == Verdict::unavailable;
            probe_unverified |= event.verdict == Verdict::unverified;
        }
        const auto derived_probe =
            combined_verdict(probe_fail, probe_unavailable, probe_unverified);
        if (probe.verdict != derived_probe)
            return fail("probe verdict does not match its event evidence");
        if (probe.required) {
            any_required = true;
            any_required_authentic_identity |=
                probe.adapter.status == IdentityStatus::authentic;
            any_fail |= probe.verdict == Verdict::fail;
            any_unavailable |= probe.verdict == Verdict::unavailable;
            any_unverified |= probe.verdict == Verdict::unverified;
        }

        const auto& measurements = probe.measurements;
        for (const auto value : { measurements.non_transparent_pixel_count,
                                  measurements.distinct_color_count,
                                  measurements.rgba_fingerprint })
            if (value.has_value() &&
                *value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return fail("integer measurements must fit signed 64-bit JSON values");
        if (measurements.readback_completed == false &&
            measurements.pixel_output_produced == true)
            return fail("pixel output cannot be proven when readback failed");
        if (measurements.content_floor_passed == true &&
            measurements.pixel_output_produced != true)
            return fail("content floor requires proven pixel output");
        if ((measurements.non_transparent_pixel_count.has_value() ||
             measurements.distinct_color_count.has_value() ||
             measurements.rgba_fingerprint.has_value()) &&
            measurements.pixel_output_produced != true)
            return fail("pixel measurements require proven pixel output");
        bool has_render_stage = false;
        bool has_compute_stage = false;
        for (const auto& evidence : probe.events) {
            has_render_stage = has_render_stage || evidence.stage == Stage::render ||
                               evidence.stage == Stage::submit ||
                               evidence.stage == Stage::readback ||
                               evidence.stage == Stage::content;
            has_compute_stage = has_compute_stage || evidence.stage == Stage::compute;
        }
        if (probe.verdict == Verdict::pass && has_render_stage &&
            (measurements.command_submitted != true ||
             measurements.readback_completed != true ||
             measurements.pixel_output_produced != true ||
             measurements.content_floor_passed != true))
            return fail("a passing render probe requires submit, readback, pixel, and content proof");
        if (probe.verdict == Verdict::pass && has_compute_stage &&
            (measurements.compute_initialized != true ||
             measurements.compute_oracle_passed != true))
            return fail("a passing compute probe requires initialization and oracle proof");
        any_required_readback_pixels |= probe.required &&
                                        measurements.readback_completed == true &&
                                        measurements.pixel_output_produced == true &&
                                        measurements.content_floor_passed == true;
        any_device_lost |= probe.required && measurements.device_lost == true;
    }

    if (!any_required) return fail("at least one probe must contribute to the top-level verdict");
    const auto derived = combined_verdict(any_fail, any_unavailable, any_unverified);
    if (result.verdict != derived)
        return fail("top-level verdict does not match probe evidence");
    if (!result.render_requested && result.verdict != Verdict::unverified)
        return fail("a no-render result must be unverified");
    if (result.verdict == Verdict::pass && !any_required_readback_pixels)
        return fail("a passing result requires readback, pixels, and content-floor proof");
    if (result.verdict == Verdict::pass && !any_required_authentic_identity)
        return fail("a passing result requires authentic adapter identity from a required probe");

    switch (result.verdict) {
        case Verdict::pass:
            if (result.health_state != HealthState::healthy)
                return fail("pass requires healthy state");
            break;
        case Verdict::fail:
            if (result.health_state != HealthState::failed &&
                result.health_state != HealthState::lost)
                return fail("fail requires failed or lost state");
            break;
        case Verdict::unavailable:
            if (result.health_state != HealthState::unavailable)
                return fail("unavailable verdict requires unavailable state");
            break;
        case Verdict::unverified:
            if (result.health_state != HealthState::unverified)
                return fail("unverified verdict requires unverified state");
            break;
    }
    if ((result.health_state == HealthState::lost) != any_device_lost)
        return fail("lost state and device_lost evidence must agree");
    if (error != nullptr) error->clear();
    return true;
}

std::string to_json(const HealthResult& result, bool pretty) {
    auto probes = choc::value::createEmptyArray();
    for (const auto& probe : result.probes) {
        auto adapter = choc::value::createObject("adapter");
        adapter.setMember("status", std::string(to_string(probe.adapter.status)));
        adapter.setMember("class", std::string(to_string(probe.adapter.classification)));
        set_optional(adapter, "backend", probe.adapter.backend);
        set_optional(adapter, "name", probe.adapter.name);
        set_optional(adapter, "vendor", probe.adapter.vendor);
        set_optional(adapter, "architecture", probe.adapter.architecture);
        set_optional(adapter, "device", probe.adapter.device);

        auto measurements = choc::value::createObject("measurements");
        set_optional(measurements, "command_submitted",
                     probe.measurements.command_submitted);
        set_optional(measurements, "readback_completed",
                     probe.measurements.readback_completed);
        set_optional(measurements, "pixel_output_produced",
                     probe.measurements.pixel_output_produced);
        set_optional(measurements, "content_floor_passed",
                     probe.measurements.content_floor_passed);
        set_optional(measurements, "compute_initialized",
                     probe.measurements.compute_initialized);
        set_optional(measurements, "compute_oracle_passed",
                     probe.measurements.compute_oracle_passed);
        set_optional(measurements, "device_lost", probe.measurements.device_lost);
        set_optional_u64(measurements, "non_transparent_pixel_count",
                         probe.measurements.non_transparent_pixel_count);
        set_optional_u64(measurements, "distinct_color_count",
                         probe.measurements.distinct_color_count);
        set_optional_u64(measurements, "rgba_fingerprint",
                         probe.measurements.rgba_fingerprint);

        auto events = choc::value::createEmptyArray();
        for (const auto& event : probe.events) {
            auto value = choc::value::createObject("event");
            value.setMember("sequence", static_cast<std::int64_t>(event.sequence));
            value.setMember("stage", std::string(to_string(event.stage)));
            value.setMember("verdict", std::string(to_string(event.verdict)));
            value.setMember("code", event.code);
            value.setMember("detail", event.detail);
            events.addArrayElement(std::move(value));
        }

        auto value = choc::value::createObject("probe");
        value.setMember("probe_id", probe.probe_id);
        value.setMember("required", probe.required);
        value.setMember("verdict", std::string(to_string(probe.verdict)));
        value.setMember("adapter", std::move(adapter));
        value.setMember("measurements", std::move(measurements));
        value.setMember("events", std::move(events));
        probes.addArrayElement(std::move(value));
    }

    auto recommendations = choc::value::createEmptyArray();
    for (const auto& recommendation : result.recommendations)
        recommendations.addArrayElement(recommendation);

    auto root = choc::value::createObject("");
    root.setMember("schema", result.schema);
    root.setMember("version", static_cast<std::int64_t>(result.version));
    root.setMember("run_id", result.run_id);
    if (result.schema == kSchema && result.version == kVersion)
        root.setMember("measured_at_utc", result.measured_at_utc);
    root.setMember("render_requested", result.render_requested);
    root.setMember("verdict", std::string(to_string(result.verdict)));
    root.setMember("health_state", std::string(to_string(result.health_state)));
    root.setMember("probes", std::move(probes));
    root.setMember("recommendations", std::move(recommendations));
    auto json = choc::json::toString(root, pretty);
    return pretty ? json : compact_json(json);
}

std::optional<HealthResult> from_json(std::string_view json, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<HealthResult> {
        set_error(error, std::move(message));
        return std::nullopt;
    };

    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (const choc::json::ParseError& exception) {
        return fail("JSON parse error at line " +
                    std::to_string(exception.lineAndColumn.line) + ", column " +
                    std::to_string(exception.lineAndColumn.column) + ": " +
                    exception.what());
    } catch (const std::exception& exception) {
        return fail(std::string("JSON parse error: ") + exception.what());
    }

    std::string parse_error;
    static constexpr std::array v1_root_fields{
        "schema", "version", "run_id", "render_requested", "verdict",
        "health_state", "probes", "recommendations",
    };
    HealthResult result;
    if (!read_required_string(root, "schema", "$.", result.schema, parse_error))
        return fail(std::move(parse_error));
    std::uint64_t version = 0;
    if (!read_required_u64(root, "version", "$.", version, parse_error) ||
        version > std::numeric_limits<std::uint32_t>::max())
        return fail(parse_error.empty() ? "$.version is too large" : std::move(parse_error));
    result.version = static_cast<std::uint32_t>(version);
    const bool is_v1 = result.schema == kSchemaV1 && result.version == kVersionV1;
    const bool is_v2 = result.schema == kSchema && result.version == kVersion;
    if (!is_v1 && !is_v2)
        return fail("$.schema and $.version must identify GPU-health result v1 or v2");
    static constexpr std::array v2_root_fields{
        "schema", "version", "run_id", "measured_at_utc", "render_requested", "verdict",
        "health_state", "probes", "recommendations",
    };
    if (is_v1 && !has_only_fields(root, v1_root_fields, "$", parse_error))
        return fail(std::move(parse_error));
    if (is_v2 && !has_only_fields(root, v2_root_fields, "$", parse_error))
        return fail(std::move(parse_error));
    if (!read_required_string(root, "run_id", "$.", result.run_id, parse_error))
        return fail(std::move(parse_error));
    if (is_v2 && !read_required_string(root, "measured_at_utc", "$.",
                                       result.measured_at_utc, parse_error))
        return fail(std::move(parse_error));
    if (!read_required_bool(root, "render_requested", "$.", result.render_requested,
                            parse_error) ||
        !read_required_enum(root, "verdict", "$.", result.verdict,
                            verdict_from_string, parse_error) ||
        !read_required_enum(root, "health_state", "$.", result.health_state,
                            health_state_from_string, parse_error))
        return fail(std::move(parse_error));

    if (!root.hasObjectMember("probes") || !root["probes"].isArray())
        return fail("$.probes is required and must be an array");
    const auto probes = root["probes"];
    result.probes.reserve(probes.size());
    static constexpr std::array probe_fields{
        "probe_id", "required", "verdict", "adapter", "measurements", "events",
    };
    static constexpr std::array adapter_fields{
        "status", "class", "backend", "name", "vendor", "architecture", "device",
    };
    static constexpr std::array measurement_fields{
        "command_submitted", "readback_completed", "pixel_output_produced",
        "content_floor_passed", "compute_initialized", "compute_oracle_passed",
        "device_lost", "non_transparent_pixel_count", "distinct_color_count",
        "rgba_fingerprint",
    };
    static constexpr std::array event_fields{
        "sequence", "stage", "verdict", "code", "detail",
    };

    for (std::uint32_t index = 0; index < probes.size(); ++index) {
        const auto value = probes[index];
        const auto path = "$.probes[" + std::to_string(index) + "]";
        if (!has_only_fields(value, probe_fields, path, parse_error))
            return fail(std::move(parse_error));
        ProbeEvidence probe;
        if (!read_required_string(value, "probe_id", path + ".", probe.probe_id,
                                  parse_error) ||
            !read_required_bool(value, "required", path + ".", probe.required,
                                parse_error) ||
            !read_required_enum(value, "verdict", path + ".", probe.verdict,
                                verdict_from_string, parse_error))
            return fail(std::move(parse_error));

        if (!value.hasObjectMember("adapter") || !value["adapter"].isObject())
            return fail(path + ".adapter is required and must be an object");
        const auto adapter = value["adapter"];
        if (!has_only_fields(adapter, adapter_fields, path + ".adapter", parse_error) ||
            !read_required_enum(adapter, "status", path + ".adapter.",
                                probe.adapter.status, identity_status_from_string,
                                parse_error) ||
            !read_required_enum(adapter, "class", path + ".adapter.",
                                probe.adapter.classification,
                                adapter_class_from_string, parse_error) ||
            !read_nullable_string(adapter, "backend", path + ".adapter.",
                                  probe.adapter.backend, parse_error) ||
            !read_nullable_string(adapter, "name", path + ".adapter.",
                                  probe.adapter.name, parse_error) ||
            !read_nullable_string(adapter, "vendor", path + ".adapter.",
                                  probe.adapter.vendor, parse_error) ||
            !read_nullable_string(adapter, "architecture", path + ".adapter.",
                                  probe.adapter.architecture, parse_error) ||
            !read_nullable_string(adapter, "device", path + ".adapter.",
                                  probe.adapter.device, parse_error))
            return fail(std::move(parse_error));

        if (!value.hasObjectMember("measurements") ||
            !value["measurements"].isObject())
            return fail(path + ".measurements is required and must be an object");
        const auto measurements = value["measurements"];
        auto& out = probe.measurements;
        if (!has_only_fields(measurements, measurement_fields,
                             path + ".measurements", parse_error) ||
            !read_nullable_bool(measurements, "command_submitted",
                                path + ".measurements.", out.command_submitted,
                                parse_error) ||
            !read_nullable_bool(measurements, "readback_completed",
                                path + ".measurements.", out.readback_completed,
                                parse_error) ||
            !read_nullable_bool(measurements, "pixel_output_produced",
                                path + ".measurements.", out.pixel_output_produced,
                                parse_error) ||
            !read_nullable_bool(measurements, "content_floor_passed",
                                path + ".measurements.", out.content_floor_passed,
                                parse_error) ||
            !read_nullable_bool(measurements, "compute_initialized",
                                path + ".measurements.", out.compute_initialized,
                                parse_error) ||
            !read_nullable_bool(measurements, "compute_oracle_passed",
                                path + ".measurements.", out.compute_oracle_passed,
                                parse_error) ||
            !read_nullable_bool(measurements, "device_lost",
                                path + ".measurements.", out.device_lost,
                                parse_error) ||
            !read_nullable_u64(measurements, "non_transparent_pixel_count",
                               path + ".measurements.",
                               out.non_transparent_pixel_count, parse_error) ||
            !read_nullable_u64(measurements, "distinct_color_count",
                               path + ".measurements.", out.distinct_color_count,
                               parse_error) ||
            !read_nullable_u64(measurements, "rgba_fingerprint",
                               path + ".measurements.", out.rgba_fingerprint,
                               parse_error))
            return fail(std::move(parse_error));

        if (!value.hasObjectMember("events") || !value["events"].isArray())
            return fail(path + ".events is required and must be an array");
        const auto events = value["events"];
        probe.events.reserve(events.size());
        for (std::uint32_t event_index = 0; event_index < events.size(); ++event_index) {
            const auto event_value = events[event_index];
            const auto event_path = path + ".events[" +
                                    std::to_string(event_index) + "]";
            if (!has_only_fields(event_value, event_fields, event_path, parse_error))
                return fail(std::move(parse_error));
            EvidenceEvent event;
            std::uint64_t sequence = 0;
            if (!read_required_u64(event_value, "sequence", event_path + ".",
                                   sequence, parse_error) ||
                sequence > std::numeric_limits<std::uint32_t>::max())
                return fail(parse_error.empty() ? event_path + ".sequence is too large"
                                                : std::move(parse_error));
            event.sequence = static_cast<std::uint32_t>(sequence);
            if (!read_required_enum(event_value, "stage", event_path + ".",
                                    event.stage, stage_from_string, parse_error) ||
                !read_required_enum(event_value, "verdict", event_path + ".",
                                    event.verdict, verdict_from_string, parse_error) ||
                !read_required_string(event_value, "code", event_path + ".",
                                      event.code, parse_error) ||
                !read_required_string(event_value, "detail", event_path + ".",
                                      event.detail, parse_error))
                return fail(std::move(parse_error));
            probe.events.push_back(std::move(event));
        }
        result.probes.push_back(std::move(probe));
    }

    if (!root.hasObjectMember("recommendations") ||
        !root["recommendations"].isArray())
        return fail("$.recommendations is required and must be an array");
    const auto recommendations = root["recommendations"];
    result.recommendations.reserve(recommendations.size());
    for (std::uint32_t index = 0; index < recommendations.size(); ++index) {
        if (!recommendations[index].isString())
            return fail("$.recommendations[" + std::to_string(index) +
                        "] must be a string");
        result.recommendations.emplace_back(recommendations[index].getString());
    }

    std::string validation_error;
    if (!validate(result, &validation_error)) return fail(std::move(validation_error));
    if (error != nullptr) error->clear();
    return result;
}

} // namespace pulp::tooling::gpu_health
