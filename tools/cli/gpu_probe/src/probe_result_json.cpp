#include <pulp_tooling/gpu_probe/probe_result.hpp>

#include <choc/text/choc_JSON.h>

#include <sstream>

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
    return result.verdict == Verdict::pass ? 0 : 1;
}

} // namespace pulp::tooling::gpu_probe
