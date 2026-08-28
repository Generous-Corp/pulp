#include <pulp_tooling/gpu_probe/probe_result.hpp>

#include <choc/text/choc_JSON.h>

#include <array>
#include <exception>
#include <limits>
#include <sstream>
#include <span>

namespace pulp::tooling::gpu_probe {
namespace {

template <typename T>
void set_optional(choc::value::Value& object, const char* name,
                  const std::optional<T>& value) {
    if (value)
        object.setMember(name, *value);
    else
        object.setMember(name, choc::value::Value{});
}

std::string compact_json(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    bool in_string = false;
    bool escaped = false;
    for (const char value : input) {
        if (in_string) {
            output.push_back(value);
            if (escaped)
                escaped = false;
            else if (value == '\\')
                escaped = true;
            else if (value == '"')
                in_string = false;
        } else if (value == '"') {
            in_string = true;
            output.push_back(value);
        } else if (value != ' ' && value != '\n' && value != '\r' && value != '\t') {
            output.push_back(value);
        }
    }
    return output;
}

bool has_only_fields(const choc::value::ValueView& object,
                     std::span<const char* const> fields,
                     std::string_view path, std::string& error) {
    if (!object.isObject()) {
        error = std::string(path) + " must be an object";
        return false;
    }
    bool valid = true;
    object.visitObjectMembers([&](std::string_view member, const choc::value::ValueView&) {
        for (const auto* field : fields)
            if (member == field) return;
        if (valid) {
            error = std::string(path) + " has unknown member '" + std::string(member) + "'";
            valid = false;
        }
    });
    return valid;
}

bool read_string(const choc::value::ValueView& object, const char* key,
                 std::string_view path, std::string& output, std::string& error) {
    if (!object.hasObjectMember(key) || !object[key].isString()) {
        error = std::string(path) + key + " is required and must be a string";
        return false;
    }
    output = std::string(object[key].getString());
    return true;
}

bool read_u64(const choc::value::ValueView& object, const char* key,
              std::string_view path, std::uint64_t& output, std::string& error) {
    if (!object.hasObjectMember(key) ||
        !(object[key].isInt32() || object[key].isInt64()) || object[key].getInt64() < 0) {
        error = std::string(path) + key + " is required and must be a non-negative integer";
        return false;
    }
    output = static_cast<std::uint64_t>(object[key].getInt64());
    return true;
}

bool read_double(const choc::value::ValueView& object, const char* key,
                 std::string_view path, double& output, std::string& error) {
    if (!object.hasObjectMember(key)) {
        error = std::string(path) + key + " is required and must be a number";
        return false;
    }
    const auto value = object[key];
    if (value.isFloat64()) output = value.getFloat64();
    else if (value.isFloat32()) output = value.getFloat32();
    else if (value.isInt32() || value.isInt64()) output = static_cast<double>(value.getInt64());
    else {
        error = std::string(path) + key + " is required and must be a number";
        return false;
    }
    return true;
}

bool read_bool(const choc::value::ValueView& object, const char* key,
               std::string_view path, bool& output, std::string& error) {
    if (!object.hasObjectMember(key) || !object[key].isBool()) {
        error = std::string(path) + key + " is required and must be a boolean";
        return false;
    }
    output = object[key].getBool();
    return true;
}

bool read_nullable_string(const choc::value::ValueView& object, const char* key,
                          std::string_view path, std::optional<std::string>& output,
                          std::string& error) {
    if (!object.hasObjectMember(key)) {
        error = std::string(path) + key + " is required";
        return false;
    }
    const auto value = object[key];
    if (value.isVoid()) output.reset();
    else if (value.isString()) output = std::string(value.getString());
    else {
        error = std::string(path) + key + " must be a string or null";
        return false;
    }
    return true;
}

bool read_nullable_double(const choc::value::ValueView& object, const char* key,
                          std::string_view path, std::optional<double>& output,
                          std::string& error) {
    if (!object.hasObjectMember(key)) {
        error = std::string(path) + key + " is required";
        return false;
    }
    if (object[key].isVoid()) {
        output.reset();
        return true;
    }
    double value = 0.0;
    if (!read_double(object, key, path, value, error)) return false;
    output = value;
    return true;
}

template <typename Enum, std::size_t Size>
bool read_enum(const choc::value::ValueView& object, const char* key,
               std::string_view path, Enum& output,
               const std::array<std::pair<std::string_view, Enum>, Size>& values,
               std::string& error) {
    std::string text;
    if (!read_string(object, key, path, text, error)) return false;
    for (const auto& [name, value] : values) {
        if (name == text) {
            output = value;
            return true;
        }
    }
    error = std::string(path) + key + " has unsupported value '" + text + "'";
    return false;
}

constexpr std::array kVerdicts{
    std::pair{std::string_view{"pass"}, Verdict::pass},
    std::pair{std::string_view{"fail"}, Verdict::fail},
    std::pair{std::string_view{"unavailable"}, Verdict::unavailable},
    std::pair{std::string_view{"unverified"}, Verdict::unverified},
};
constexpr std::array kAdapterPolicies{
    std::pair{std::string_view{"hardware-required"}, AdapterPolicy::hardware_required},
    std::pair{std::string_view{"hardware-preferred"}, AdapterPolicy::hardware_preferred},
    std::pair{std::string_view{"any-supported"}, AdapterPolicy::any_supported},
};
constexpr std::array kAdapterClasses{
    std::pair{std::string_view{"hardware"}, AdapterClass::hardware},
    std::pair{std::string_view{"software"}, AdapterClass::software},
    std::pair{std::string_view{"null"}, AdapterClass::null_adapter},
    std::pair{std::string_view{"unknown"}, AdapterClass::unknown},
};
constexpr std::array kIdentityStatuses{
    std::pair{std::string_view{"authentic"}, IdentityStatus::authentic},
    std::pair{std::string_view{"unverified"}, IdentityStatus::unverified},
    std::pair{std::string_view{"unavailable"}, IdentityStatus::unavailable},
};
constexpr std::array kArtifactKinds{
    std::pair{std::string_view{"json"}, ArtifactKind::json},
    std::pair{std::string_view{"image"}, ArtifactKind::image},
    std::pair{std::string_view{"numeric-samples"}, ArtifactKind::numeric_samples},
    std::pair{std::string_view{"trace"}, ArtifactKind::trace},
};

} // namespace

std::string to_json(const ProbeResult& result, bool pretty) {
    auto dimensions = choc::value::createObject("dimensions");
    dimensions.setMember("width", static_cast<std::int64_t>(result.dimensions.width));
    dimensions.setMember("height", static_cast<std::int64_t>(result.dimensions.height));
    dimensions.setMember("work_items",
                         static_cast<std::int64_t>(result.dimensions.work_items));

    auto tolerance = choc::value::createObject("tolerance");
    tolerance.setMember("absolute", result.tolerance.absolute);
    tolerance.setMember("relative", result.tolerance.relative);

    auto adapter = choc::value::createObject("adapter");
    adapter.setMember("status", std::string(to_string(result.adapter.status)));
    adapter.setMember("class", std::string(to_string(result.adapter.classification)));
    set_optional(adapter, "backend", result.adapter.backend);
    set_optional(adapter, "name", result.adapter.name);
    set_optional(adapter, "vendor", result.adapter.vendor);
    set_optional(adapter, "architecture", result.adapter.architecture);
    set_optional(adapter, "device", result.adapter.device);

    auto passes = choc::value::createEmptyArray();
    for (const auto& pass : result.passes) {
        auto value = choc::value::createObject("pass");
        value.setMember("sequence", static_cast<std::int64_t>(pass.sequence));
        value.setMember("name", pass.name);
        value.setMember("verdict", std::string(to_string(pass.verdict)));
        value.setMember("work_completed", pass.work_completed);
        set_optional(value, "expected", pass.expected);
        set_optional(value, "observed", pass.observed);
        set_optional(value, "absolute_error", pass.absolute_error);
        value.setMember("code", pass.code);
        passes.addArrayElement(std::move(value));
    }

    auto artifacts = choc::value::createEmptyArray();
    for (const auto& artifact : result.artifacts) {
        auto value = choc::value::createObject("artifact");
        value.setMember("name", artifact.name);
        value.setMember("kind", std::string(to_string(artifact.kind)));
        value.setMember("mime", artifact.mime);
        value.setMember("bytes", static_cast<std::int64_t>(artifact.bytes));
        value.setMember("sha256", artifact.sha256);
        artifacts.addArrayElement(std::move(value));
    }

    auto recommendations = choc::value::createEmptyArray();
    for (const auto& recommendation : result.recommendations)
        recommendations.addArrayElement(recommendation);

    auto root = choc::value::createObject("");
    root.setMember("schema", result.schema);
    root.setMember("version", static_cast<std::int64_t>(result.version));
    root.setMember("gpu_evidence_id", result.gpu_evidence_id);
    root.setMember("recipe_id", result.recipe_id);
    root.setMember("source_digest", result.source_digest);
    root.setMember("signature_digest", result.signature_digest);
    root.setMember("dimensions", std::move(dimensions));
    root.setMember("seed", static_cast<std::int64_t>(result.seed));
    root.setMember("clock", result.clock);
    root.setMember("input_format", result.input_format);
    root.setMember("output_format", result.output_format);
    root.setMember("encoding", result.encoding);
    root.setMember("tolerance", std::move(tolerance));
    root.setMember("adapter_policy", std::string(to_string(result.adapter_policy)));
    root.setMember("adapter", std::move(adapter));
    root.setMember("numeric_sample_count",
                   static_cast<std::int64_t>(result.numeric_sample_count));
    set_optional(root, "mutation", result.mutation);
    root.setMember("verdict", std::string(to_string(result.verdict)));
    root.setMember("passes", std::move(passes));
    root.setMember("artifacts", std::move(artifacts));
    root.setMember("recommendations", std::move(recommendations));

    auto json = choc::json::toString(root, pretty);
    return pretty ? json : compact_json(json);
}

std::optional<ProbeResult> from_json(std::string_view json, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<ProbeResult> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (const std::exception& exception) {
        return fail(std::string("JSON parse error: ") + exception.what());
    }
    static constexpr std::array root_fields{
        "schema", "version", "gpu_evidence_id", "recipe_id", "source_digest",
        "signature_digest", "dimensions", "seed", "clock", "input_format",
        "output_format", "encoding", "tolerance", "adapter_policy", "adapter",
        "numeric_sample_count", "mutation", "verdict", "passes", "artifacts",
        "recommendations",
    };
    std::string parse_error;
    if (!has_only_fields(root, root_fields, "$", parse_error)) return fail(parse_error);

    ProbeResult result;
    std::uint64_t integer = 0;
    if (!read_string(root, "schema", "$.", result.schema, parse_error) ||
        !read_u64(root, "version", "$.", integer, parse_error) ||
        integer > std::numeric_limits<std::uint32_t>::max())
        return fail(parse_error.empty() ? "$.version is too large" : parse_error);
    result.version = static_cast<std::uint32_t>(integer);
    if (!read_string(root, "gpu_evidence_id", "$.", result.gpu_evidence_id, parse_error) ||
        !read_string(root, "recipe_id", "$.", result.recipe_id, parse_error) ||
        !read_string(root, "source_digest", "$.", result.source_digest, parse_error) ||
        !read_string(root, "signature_digest", "$.", result.signature_digest, parse_error) ||
        !read_u64(root, "seed", "$.", result.seed, parse_error) ||
        !read_string(root, "clock", "$.", result.clock, parse_error) ||
        !read_string(root, "input_format", "$.", result.input_format, parse_error) ||
        !read_string(root, "output_format", "$.", result.output_format, parse_error) ||
        !read_string(root, "encoding", "$.", result.encoding, parse_error) ||
        !read_enum(root, "adapter_policy", "$.", result.adapter_policy,
                   kAdapterPolicies, parse_error) ||
        !read_u64(root, "numeric_sample_count", "$.", integer, parse_error) ||
        integer > std::numeric_limits<std::uint32_t>::max())
        return fail(parse_error.empty() ? "$.numeric_sample_count is too large" : parse_error);
    result.numeric_sample_count = static_cast<std::uint32_t>(integer);
    if (!read_nullable_string(root, "mutation", "$.", result.mutation, parse_error) ||
        !read_enum(root, "verdict", "$.", result.verdict, kVerdicts, parse_error))
        return fail(parse_error);

    static constexpr std::array dimension_fields{"width", "height", "work_items"};
    if (!root.hasObjectMember("dimensions") ||
        !has_only_fields(root["dimensions"], dimension_fields, "$.dimensions", parse_error))
        return fail(parse_error.empty() ? "$.dimensions is required" : parse_error);
    const auto dimensions = root["dimensions"];
    if (!read_u64(dimensions, "width", "$.dimensions.", integer, parse_error) ||
        integer > std::numeric_limits<std::uint32_t>::max()) return fail(parse_error);
    result.dimensions.width = static_cast<std::uint32_t>(integer);
    if (!read_u64(dimensions, "height", "$.dimensions.", integer, parse_error) ||
        integer > std::numeric_limits<std::uint32_t>::max()) return fail(parse_error);
    result.dimensions.height = static_cast<std::uint32_t>(integer);
    if (!read_u64(dimensions, "work_items", "$.dimensions.",
                  result.dimensions.work_items, parse_error)) return fail(parse_error);

    static constexpr std::array tolerance_fields{"absolute", "relative"};
    if (!root.hasObjectMember("tolerance") ||
        !has_only_fields(root["tolerance"], tolerance_fields, "$.tolerance", parse_error) ||
        !read_double(root["tolerance"], "absolute", "$.tolerance.",
                     result.tolerance.absolute, parse_error) ||
        !read_double(root["tolerance"], "relative", "$.tolerance.",
                     result.tolerance.relative, parse_error)) return fail(parse_error);

    static constexpr std::array adapter_fields{
        "status", "class", "backend", "name", "vendor", "architecture", "device"};
    if (!root.hasObjectMember("adapter") ||
        !has_only_fields(root["adapter"], adapter_fields, "$.adapter", parse_error))
        return fail(parse_error.empty() ? "$.adapter is required" : parse_error);
    const auto adapter = root["adapter"];
    if (!read_enum(adapter, "status", "$.adapter.", result.adapter.status,
                   kIdentityStatuses, parse_error) ||
        !read_enum(adapter, "class", "$.adapter.", result.adapter.classification,
                   kAdapterClasses, parse_error) ||
        !read_nullable_string(adapter, "backend", "$.adapter.", result.adapter.backend,
                              parse_error) ||
        !read_nullable_string(adapter, "name", "$.adapter.", result.adapter.name,
                              parse_error) ||
        !read_nullable_string(adapter, "vendor", "$.adapter.", result.adapter.vendor,
                              parse_error) ||
        !read_nullable_string(adapter, "architecture", "$.adapter.",
                              result.adapter.architecture, parse_error) ||
        !read_nullable_string(adapter, "device", "$.adapter.", result.adapter.device,
                              parse_error)) return fail(parse_error);

    if (!root.hasObjectMember("passes") || !root["passes"].isArray())
        return fail("$.passes is required and must be an array");
    static constexpr std::array pass_fields{
        "sequence", "name", "verdict", "work_completed", "expected", "observed",
        "absolute_error", "code"};
    const auto passes = root["passes"];
    for (std::uint32_t index = 0; index < passes.size(); ++index) {
        const auto value = passes[index];
        const auto path = "$.passes[" + std::to_string(index) + "]";
        if (!has_only_fields(value, pass_fields, path, parse_error)) return fail(parse_error);
        PassResult pass;
        if (!read_u64(value, "sequence", path + ".", integer, parse_error) ||
            integer > std::numeric_limits<std::uint32_t>::max()) return fail(parse_error);
        pass.sequence = static_cast<std::uint32_t>(integer);
        if (!read_string(value, "name", path + ".", pass.name, parse_error) ||
            !read_enum(value, "verdict", path + ".", pass.verdict, kVerdicts, parse_error) ||
            !read_bool(value, "work_completed", path + ".", pass.work_completed,
                       parse_error) ||
            !read_nullable_double(value, "expected", path + ".", pass.expected, parse_error) ||
            !read_nullable_double(value, "observed", path + ".", pass.observed, parse_error) ||
            !read_nullable_double(value, "absolute_error", path + ".", pass.absolute_error,
                                  parse_error) ||
            !read_string(value, "code", path + ".", pass.code, parse_error))
            return fail(parse_error);
        result.passes.push_back(std::move(pass));
    }

    if (!root.hasObjectMember("artifacts") || !root["artifacts"].isArray())
        return fail("$.artifacts is required and must be an array");
    static constexpr std::array artifact_fields{"name", "kind", "mime", "bytes", "sha256"};
    const auto artifacts = root["artifacts"];
    for (std::uint32_t index = 0; index < artifacts.size(); ++index) {
        const auto value = artifacts[index];
        const auto path = "$.artifacts[" + std::to_string(index) + "]";
        if (!has_only_fields(value, artifact_fields, path, parse_error)) return fail(parse_error);
        Artifact artifact;
        if (!read_string(value, "name", path + ".", artifact.name, parse_error) ||
            !read_enum(value, "kind", path + ".", artifact.kind, kArtifactKinds,
                       parse_error) ||
            !read_string(value, "mime", path + ".", artifact.mime, parse_error) ||
            !read_u64(value, "bytes", path + ".", artifact.bytes, parse_error) ||
            !read_string(value, "sha256", path + ".", artifact.sha256, parse_error))
            return fail(parse_error);
        result.artifacts.push_back(std::move(artifact));
    }

    if (!root.hasObjectMember("recommendations") || !root["recommendations"].isArray())
        return fail("$.recommendations is required and must be an array");
    const auto recommendations = root["recommendations"];
    for (std::uint32_t index = 0; index < recommendations.size(); ++index) {
        if (!recommendations[index].isString())
            return fail("$.recommendations[" + std::to_string(index) + "] must be a string");
        result.recommendations.emplace_back(recommendations[index].getString());
    }

    std::string validation_error;
    if (!validate(result, &validation_error)) return fail(validation_error);
    if (error) error->clear();
    return result;
}

std::string render_human(const ProbeResult& result) {
    std::ostringstream out;
    out << "GPU probe: " << result.recipe_id << '\n'
        << "  verdict: " << to_string(result.verdict) << '\n'
        << "  evidence: " << result.gpu_evidence_id << '\n'
        << "  adapter: " << to_string(result.adapter.status) << "/"
        << to_string(result.adapter.classification);
    if (result.adapter.backend) out << " (" << *result.adapter.backend << ")";
    out << '\n';
    for (const auto& pass : result.passes) {
        out << "  [" << to_string(pass.verdict) << "] " << pass.name
            << " — " << pass.code;
        if (pass.absolute_error) out << " (absolute error " << *pass.absolute_error << ")";
        out << '\n';
    }
    for (const auto& artifact : result.artifacts)
        out << "  artifact: " << artifact.name << " (" << artifact.bytes << " bytes)\n";
    for (const auto& recommendation : result.recommendations)
        out << "  next: " << recommendation << '\n';
    return out.str();
}

int exit_code(const ProbeResult& result) {
    switch (result.verdict) {
        case Verdict::pass: return 0;
        case Verdict::fail: return 1;
        case Verdict::unavailable:
        case Verdict::unverified: return 2;
    }
    return 2;
}

} // namespace pulp::tooling::gpu_probe
