// mcp_trace_tools.cpp -- closed offline GPU analysis through the installed CLI.

#include "mcp_tools.hpp"

#include "mcp_json.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools_internal.hpp"

#include <choc/text/choc_JSON.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace pulp_mcp {
namespace {

std::mutex g_executable_mutex;
std::filesystem::path g_mcp_executable;

std::string local_error(const std::string& code, const std::string& message,
                        const std::string& raw_output = {}) {
    auto structured = "{\"schema\":\"pulp.trace-gpu-analysis.v1\",\"verdict\":"
                      "\"unavailable\",\"capture_complete\":false,\"error\":{\"code\":" +
                      json_string(code) + ",\"message\":" + json_string(message);
    if (!raw_output.empty())
        structured += ",\"raw_output\":" + json_string(raw_output);
    structured += "}}";
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(structured) +
           "}],\"isError\":true,\"structuredContent\":" + structured + "}";
}

bool valid_question(const std::string& question) {
    return question == "gpu-startup" || question == "gpu-health" || question == "gpu-probe";
}

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

    const auto question = extract_string(params_json, "question");
    const auto trace = extract_string(params_json, "trace");
    if (!valid_question(question))
        return local_error("invalid-arguments",
                           "question must be gpu-startup, gpu-health, or gpu-probe");
    if (trace.empty())
        return local_error("invalid-arguments", "trace is required");

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

    const auto command = shell_quote(cli.string()) + " trace " + question + " --trace " +
                         shell_quote(trace) + " --json 2>&1";
    const auto run = exec_with_status(command);
    const auto normalized = trim_output(run.output);
    choc::value::Value result;
    try {
        result = choc::json::parse(normalized);
    } catch (...) {
        return local_error("malformed-cli-output",
                           "pulp trace returned invalid JSON", run.output);
    }
    if (!result.isObject())
        return local_error("malformed-cli-output",
                           "pulp trace returned a non-object result", run.output);
    const auto schema = extract_string(normalized, "schema");
    const auto actual_question = extract_string(normalized, "question");
    const auto verdict = extract_string(normalized, "verdict");
    if (schema != "pulp.trace-gpu-analysis.v1" || actual_question != question)
        return local_error("incoherent-cli-output",
                           "pulp trace result does not match the requested v1 question",
                           run.output);
    const auto wanted_status = expected_status(verdict);
    if (wanted_status < 0 || run.status != wanted_status)
        return local_error("incoherent-cli-output",
                           "pulp trace exit status does not match its typed verdict",
                           run.output);

    auto payload = "{\"content\":[{\"type\":\"text\",\"text\":" +
                   json_string(normalized) + "}],\"structuredContent\":" + normalized;
    if (run.status != 0)
        payload += ",\"isError\":true";
    return payload + "}";
}

} // namespace pulp_mcp
