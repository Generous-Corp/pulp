// mcp_gpu_tools.cpp -- CPU-only MCP projection of the installed GPU doctor.

#include "mcp_tools.hpp"

#include "mcp_json.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools_internal.hpp"

#include <pulp_tooling/gpu_health/health_result.hpp>
#include <pulp_tooling/gpu_probe/probe_result.hpp>
#include <choc/text/choc_JSON.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace pulp_mcp {
namespace {

std::mutex g_executable_mutex;
std::filesystem::path g_mcp_executable;

std::string tool_result(int exit_code, const std::string& evidence_json, bool is_error) {
    const auto structured =
        "{\"exit_code\":" + std::to_string(exit_code) + ",\"evidence\":" + evidence_json + "}";
    auto payload = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(evidence_json) + "}]";
    if (is_error)
        payload += ",\"isError\":true";
    return payload + ",\"structuredContent\":" + structured + "}";
}

std::string local_error(int, const std::string& code, const std::string& message,
                        const std::string& raw_output = {}) {
    auto evidence = "{\"ok\":false,\"error\":{\"code\":" + json_string(code) +
                    ",\"message\":" + json_string(message);
    if (!raw_output.empty())
        evidence += ",\"raw_output\":" + json_string(raw_output);
    evidence += "}}";
    return "{\"content\":[{\"type\":\"text\",\"text\":" +
           json_string(evidence) + "}],\"isError\":true}";
}

} // namespace

void configure_gpu_doctor_executable(std::string executable_path) {
    std::lock_guard lock(g_executable_mutex);
    g_mcp_executable = std::move(executable_path);
}

std::string handle_gpu_doctor(const std::string& params_json) {
    try {
        const auto params = choc::json::parse(params_json);
        if (!params.isObject())
            return local_error(-1, "invalid-arguments", "arguments must be an object");
        bool unknown = false;
        std::string unknown_name;
        params.getView().visitObjectMembers([&](std::string_view name, const choc::value::ValueView&) {
            if (name != "no_render" && !unknown) {
                unknown = true;
                unknown_name = name;
            }
        });
        if (unknown)
            return local_error(-1, "invalid-arguments",
                               "unknown argument: " + unknown_name);
    } catch (...) {
        return local_error(-1, "invalid-arguments", "arguments must be valid JSON");
    }
    const auto no_render_token = extract_raw(params_json, "no_render");
    if (!no_render_token.empty() && no_render_token != "true" && no_render_token != "false") {
        return local_error(-1, "invalid-arguments", "no_render must be a boolean");
    }

    std::filesystem::path executable;
    {
        std::lock_guard lock(g_executable_mutex);
        executable = g_mcp_executable;
    }
    if (executable.empty()) {
        return local_error(-1, "cli-unavailable", "pulp-mcp executable identity is unavailable");
    }

    // Installed MCP and native CLI releases are a matched pair. Resolve the
    // adjacent installed binary or its deterministic sibling build-tree
    // output so cwd/PATH cannot silently select a different Pulp release.
    const auto cli = sibling_pulp_cpp_path(executable);
    if (!std::filesystem::is_regular_file(cli)) {
        return local_error(-1, "cli-unavailable",
                           "the sibling installed pulp-cpp executable was not found");
    }

    auto command = shell_quote(cli.string()) + " doctor gpu --json";
    if (no_render_token == "true")
        command += " --no-render";
    const auto run = exec_with_status(command);

    std::string parse_error;
    const auto evidence = pulp::tooling::gpu_health::from_json(run.output, &parse_error);
    if (!evidence.has_value())
        return local_error(run.status, "malformed-cli-output",
                           "pulp-cpp doctor gpu returned invalid v1 evidence: " + parse_error,
                           run.output);

    const bool expected_render_requested = no_render_token != "true";
    if (evidence->render_requested != expected_render_requested)
        return local_error(run.status, "incoherent-cli-output",
                           "pulp-cpp doctor gpu evidence does not match the requested render mode",
                           run.output);

    int expected_status = 1;
    switch (evidence->verdict) {
        case pulp::tooling::gpu_health::Verdict::pass: expected_status = 0; break;
        case pulp::tooling::gpu_health::Verdict::fail: expected_status = 1; break;
        case pulp::tooling::gpu_health::Verdict::unavailable:
        case pulp::tooling::gpu_health::Verdict::unverified: expected_status = 2; break;
    }
    if (run.status != expected_status)
        return local_error(run.status, "incoherent-cli-output",
                           "pulp-cpp doctor gpu exit status does not match its typed verdict",
                           run.output);

    const auto normalized = pulp::tooling::gpu_health::to_json(*evidence);

    return tool_result(run.status, normalized, run.status != 0);
}

std::string handle_gpu_probe(const std::string& params_json) {
    try {
        const auto params = choc::json::parse(params_json);
        if (!params.isObject())
            return local_error(-1, "invalid-arguments", "arguments must be an object");
        bool unknown = false;
        std::string unknown_name;
        params.getView().visitObjectMembers([&](std::string_view name,
                                                 const choc::value::ValueView&) {
            if (name != "recipe" && name != "artifacts" &&
                name != "negative_control" && !unknown) {
                unknown = true;
                unknown_name = name;
            }
        });
        if (unknown)
            return local_error(-1, "invalid-arguments", "unknown argument: " + unknown_name);
    } catch (...) {
        return local_error(-1, "invalid-arguments", "arguments must be valid JSON");
    }

    const auto recipe = extract_string(params_json, "recipe");
    const auto artifacts = extract_string(params_json, "artifacts");
    const auto negative_token = extract_raw(params_json, "negative_control");
    if (recipe.empty())
        return local_error(-1, "invalid-arguments", "recipe is required");
    if (pulp::tooling::gpu_probe::find_recipe(recipe) == nullptr)
        return local_error(-1, "invalid-arguments", "recipe is not in the closed catalog");
    if (artifacts.empty())
        return local_error(-1, "invalid-arguments", "artifacts is required");
    if (!std::filesystem::path(artifacts).is_absolute())
        return local_error(-1, "invalid-arguments", "artifacts must be an absolute path");
    if (!negative_token.empty() && negative_token != "true" && negative_token != "false")
        return local_error(-1, "invalid-arguments", "negative_control must be a boolean");

    std::filesystem::path executable;
    {
        std::lock_guard lock(g_executable_mutex);
        executable = g_mcp_executable;
    }
    if (executable.empty())
        return local_error(-1, "cli-unavailable", "pulp-mcp executable identity is unavailable");
    const auto cli = sibling_pulp_cpp_path(executable);
    if (!std::filesystem::is_regular_file(cli))
        return local_error(-1, "cli-unavailable",
                           "the sibling installed pulp-cpp executable was not found");

    auto command = shell_quote(cli.string()) + " gpu probe --recipe " + shell_quote(recipe) +
                   " --artifacts " + shell_quote(artifacts) + " --json";
    if (negative_token == "true") command += " --negative-control";
    const auto run = exec_with_status(command);

    std::string parse_error;
    const auto evidence = pulp::tooling::gpu_probe::from_json(run.output, &parse_error);
    if (!evidence)
        return local_error(run.status, "malformed-cli-output",
                           "pulp-cpp gpu probe returned invalid v1 evidence: " + parse_error,
                           run.output);
    if (evidence->recipe_id != recipe ||
        evidence->mutation.has_value() != (negative_token == "true"))
        return local_error(run.status, "incoherent-cli-output",
                           "pulp-cpp gpu probe evidence does not match the request", run.output);
    const auto expected_status = pulp::tooling::gpu_probe::exit_code(*evidence);
    if (run.status != expected_status)
        return local_error(run.status, "incoherent-cli-output",
                           "pulp-cpp gpu probe exit status does not match its typed verdict",
                           run.output);

    return tool_result(run.status, pulp::tooling::gpu_probe::to_json(*evidence),
                       run.status != 0);
}

} // namespace pulp_mcp
