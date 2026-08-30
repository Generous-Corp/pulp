// mcp_trace_tools.cpp -- closed offline GPU analysis through the installed CLI.

#include "mcp_tools.hpp"

#include "mcp_json.hpp"
#include "mcp_tools_internal.hpp"

#include <choc/text/choc_JSON.h>
#include <pulp/platform/child_process.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp_mcp {
namespace {

std::mutex g_executable_mutex;
std::filesystem::path g_mcp_executable;
std::optional<TraceAnalyzeProcessLimits> g_process_limits_for_testing;

std::string local_error(const std::string& code, const std::string& message,
                        const std::string& raw_output = {}) {
    auto structured = "{\"schema\":\"pulp.trace-gpu-analysis.v1\",\"verdict\":"
                      "\"unavailable\",\"capture_complete\":false,\"error\":{\"code\":" +
                      json_string(code) + ",\"message\":" + json_string(message);
    if (!raw_output.empty())
        structured += ",\"raw_output\":" + json_string(raw_output);
    structured += "}}";
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(structured) +
           "}],\"isError\":true}";
}

bool valid_question(const std::string& question) {
    return question == "gpu-startup" || question == "gpu-health" || question == "gpu-probe";
}

// The Rust analyzer owns trace_processor's complete process group (a Job Object
// on Windows), applies a 120-second deadline and a 4 MiB combined-output cap,
// then allows two seconds to terminate/reap it. This outer wrapper must not
// preempt that cleanup: ChildProcess cancellation only targets its direct child.
constexpr int kTraceAnalyzeTimeoutMs = 135 * 1000;
constexpr std::size_t kTraceAnalyzeMaximumOutputBytes = 8 << 20;
static_assert(kTraceAnalyzeTimeoutMs > (120 + 2) * 1000);
static_assert(kTraceAnalyzeMaximumOutputBytes > 4 * 1024 * 1024);

std::string trim_output(const std::string& value) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

int expected_status(std::string_view verdict) {
    if (verdict == "pass") return 0;
    if (verdict == "fail") return 1;
    if (verdict == "unavailable" || verdict == "unverified") return 2;
    return -1;
}

} // namespace

void configure_trace_analyze_executable(std::string executable_path) {
    std::lock_guard lock(g_executable_mutex);
    g_mcp_executable = std::move(executable_path);
}

TraceAnalyzeProcessLimits trace_analyze_process_limits() {
    std::lock_guard lock(g_executable_mutex);
    return g_process_limits_for_testing.value_or(
        TraceAnalyzeProcessLimits{kTraceAnalyzeTimeoutMs, kTraceAnalyzeMaximumOutputBytes});
}

void configure_trace_analyze_process_limits_for_testing(
    std::optional<TraceAnalyzeProcessLimits> limits) {
    std::lock_guard lock(g_executable_mutex);
    g_process_limits_for_testing = limits;
}

std::string handle_trace_analyze(const std::string& params_json) {
    choc::value::Value params;
    try {
        params = choc::json::parse(params_json);
        if (!params.isObject())
            return local_error("invalid-arguments", "arguments must be an object");
        std::string unknown;
        params.getView().visitObjectMembers(
            [&](std::string_view name, const choc::value::ValueView&) {
                if (name != "question" && name != "trace" && unknown.empty())
                    unknown = std::string(name);
            });
        if (!unknown.empty())
            return local_error("invalid-arguments", "unknown argument: " + unknown);
    } catch (...) {
        return local_error("invalid-arguments", "arguments must be valid JSON");
    }

    if (!params.hasObjectMember("question") || !params["question"].isString())
        return local_error("invalid-arguments", "question must be a string");
    if (!params.hasObjectMember("trace") || !params["trace"].isString())
        return local_error("invalid-arguments", "trace must be a string");
    const auto question = std::string(params["question"].getString());
    const auto trace = std::string(params["trace"].getString());
    if (!valid_question(question))
        return local_error("invalid-arguments",
                           "question must be gpu-startup, gpu-health, or gpu-probe");
    if (trace.empty())
        return local_error("invalid-arguments", "trace is required");
    if (trace.find('\0') != std::string::npos)
        return local_error("invalid-arguments", "trace must not contain a NUL byte");

    std::filesystem::path executable;
    {
        std::lock_guard lock(g_executable_mutex);
        executable = g_mcp_executable;
    }
    if (executable.empty())
        return local_error("cli-unavailable", "pulp-mcp executable identity is unavailable");
    const auto cli = sibling_pulp_path(executable);
    if (!std::filesystem::is_regular_file(cli))
        return local_error("cli-unavailable", "the sibling installed pulp executable was not found");

    const std::vector<std::string> arguments{"trace", question, "--trace", trace, "--json"};
    const auto limits = trace_analyze_process_limits();
    pulp::platform::ProcessOptions options;
    options.timeout_ms = limits.timeout_ms;
    options.max_output_bytes = limits.max_output_bytes;
    const auto process = pulp::platform::ChildProcess::run(cli.string(), arguments, options);
    const auto diagnostic_output = process.stdout_output.empty()
        ? process.stderr_output : process.stdout_output;
    if (process.timed_out)
        return local_error("cli-timeout", "pulp trace exceeded its bounded timeout",
                           diagnostic_output);
    if (process.output_limit_exceeded || process.was_cancelled)
        return local_error("cli-output-limit", "pulp trace exceeded its bounded output limit",
                           diagnostic_output);
    if (process.exit_code < 0)
        return local_error("cli-unavailable", "the sibling installed pulp executable could not run",
                           diagnostic_output);

    const auto normalized = trim_output(process.stdout_output);
    choc::value::Value result;
    try {
        result = choc::json::parse(normalized);
    } catch (...) {
        return local_error("malformed-cli-output",
                           "pulp trace returned invalid JSON", diagnostic_output);
    }
    if (!result.isObject())
        return local_error("malformed-cli-output",
                           "pulp trace returned a non-object result", diagnostic_output);
    if (!result.hasObjectMember("schema") || !result["schema"].isString() ||
        !result.hasObjectMember("question") || !result["question"].isString() ||
        !result.hasObjectMember("verdict") || !result["verdict"].isString() ||
        !result.hasObjectMember("capture_complete") ||
        !result["capture_complete"].isBool() ||
        !result.hasObjectMember("capture_integrity") ||
        !result["capture_integrity"].isObject() ||
        !result.hasObjectMember("scheduler_evidence_available") ||
        !result["scheduler_evidence_available"].isBool() ||
        !result.hasObjectMember("cold_start_contributors") ||
        !result["cold_start_contributors"].isArray() ||
        !result.hasObjectMember("steady_state_contributors") ||
        !result["steady_state_contributors"].isArray())
        return local_error("malformed-cli-output",
                           "pulp trace returned incomplete typed JSON", diagnostic_output);
    const auto schema = std::string(result["schema"].getString());
    const auto actual_question = std::string(result["question"].getString());
    const auto verdict = std::string(result["verdict"].getString());
    if (schema != "pulp.trace-gpu-analysis.v1" || actual_question != question)
        return local_error("incoherent-cli-output",
                           "pulp trace result does not match the requested v1 question",
                           diagnostic_output);
    const auto wanted_status = expected_status(verdict);
    if (wanted_status < 0 || process.exit_code != wanted_status)
        return local_error("incoherent-cli-output",
                           "pulp trace exit status does not match its typed verdict",
                           diagnostic_output);

    auto payload = "{\"content\":[{\"type\":\"text\",\"text\":" +
                   json_string(normalized) + "}],\"structuredContent\":" + normalized;
    if (process.exit_code != 0)
        payload += ",\"isError\":true";
    return payload + "}";
}

} // namespace pulp_mcp
