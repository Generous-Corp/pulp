// mcp_gpu_tools.cpp -- CPU-only MCP projection of the installed GPU doctor.

#include "mcp_tools.hpp"

#include "mcp_json.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools_internal.hpp"

#include <choc/text/choc_JSON.h>
#include <pulp/platform/child_process.hpp>
#include <pulp_tooling/gpu_health/health_result.hpp>
#include <pulp_tooling/gpu_probe/probe_result.hpp>
#include <pulp_tooling/gpu_probe/recipes.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(evidence) +
           "}],\"isError\":true}";
}

std::string discovery_result(const std::string& json) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(json) +
           "}],\"structuredContent\":" + json + "}";
}

void validate_recipe_discovery(const choc::value::Value& result, const std::string& action,
                               const std::optional<std::string>& requested_recipe,
                               const std::optional<std::string>& requested_symptom) {
    if (!result.isObject() || !result.hasObjectMember("schema") || !result["schema"].isString() ||
        result["schema"].getString() != "pulp.gpu-recipes-discovery.v1" ||
        !result.hasObjectMember("catalog_revision") ||
        !(result["catalog_revision"].isInt32() || result["catalog_revision"].isInt64()) ||
        result["catalog_revision"].getInt64() != 1 || !result.hasObjectMember("recipes") ||
        !result["recipes"].isArray())
        throw std::runtime_error("discovery envelope does not match revision 1");

    const auto rows = result["recipes"];
    if (action == "show" && rows.size() != 1)
        throw std::runtime_error("show must return exactly one recipe");
    if (action == "list" && !requested_symptom && rows.size() != 4)
        throw std::runtime_error("unfiltered list must return the four catalog rows");

    std::set<std::string> ids;
    std::set<int64_t> indexes;
    for (uint32_t index = 0; index < rows.size(); ++index) {
        const auto row = rows[index];
        if (!row.isObject() || !row.hasObjectMember("callable") || !row["callable"].isBool() ||
            !row.hasObjectMember("recipe") || !row["recipe"].isObject())
            throw std::runtime_error("recipe row has an invalid wrapper");
        const auto recipe = row["recipe"];
        if (!recipe.hasObjectMember("id") || !recipe["id"].isString() ||
            recipe["id"].getString().empty() || !recipe.hasObjectMember("title") ||
            !recipe["title"].isString() || !recipe.hasObjectMember("summary") ||
            !recipe["summary"].isString() || !recipe.hasObjectMember("native_registry_index") ||
            !(recipe["native_registry_index"].isInt32() ||
              recipe["native_registry_index"].isInt64()) ||
            !recipe.hasObjectMember("symptoms") || !recipe["symptoms"].isArray() ||
            !recipe.hasObjectMember("availability") || !recipe["availability"].isObject())
            throw std::runtime_error("recipe row is missing required catalog fields");
        const std::string id{recipe["id"].getString()};
        const auto native_index = recipe["native_registry_index"].getInt64();
        if (native_index < 0 || native_index > 3 || !ids.insert(id).second ||
            !indexes.insert(native_index).second)
            throw std::runtime_error("recipe identifiers or registry indexes are incoherent");
        const auto* native = pulp::tooling::gpu_probe::find_recipe(id);
        if (row["callable"].getBool() != (native != nullptr))
            throw std::runtime_error("recipe callable value disagrees with the native registry");
        if (native) {
            const auto native_recipes = pulp::tooling::gpu_probe::recipes();
            const auto position = static_cast<int64_t>(native - native_recipes.data());
            if (position != native_index)
                throw std::runtime_error("recipe index disagrees with the native registry");
        }
        if (requested_recipe && id != *requested_recipe)
            throw std::runtime_error("show result does not match the requested recipe");
        if (requested_symptom) {
            bool found = false;
            const auto symptoms = recipe["symptoms"];
            for (uint32_t symptom_index = 0; symptom_index < symptoms.size(); ++symptom_index)
                found = found || (symptoms[symptom_index].isString() &&
                                  symptoms[symptom_index].getString() == *requested_symptom);
            if (!found)
                throw std::runtime_error("list result does not match the requested symptom");
        }
    }
    if (action == "list" && !requested_symptom) {
        for (const auto& native : pulp::tooling::gpu_probe::recipes())
            if (!ids.contains(std::string(native.id)))
                throw std::runtime_error("discovery omitted an active native recipe");
    }
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
        params.getView().visitObjectMembers(
            [&](std::string_view name, const choc::value::ValueView&) {
                if (name != "no_render" && !unknown) {
                    unknown = true;
                    unknown_name = name;
                }
            });
        if (unknown)
            return local_error(-1, "invalid-arguments", "unknown argument: " + unknown_name);
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
    case pulp::tooling::gpu_health::Verdict::pass:
        expected_status = 0;
        break;
    case pulp::tooling::gpu_health::Verdict::fail:
        expected_status = 1;
        break;
    case pulp::tooling::gpu_health::Verdict::unavailable:
    case pulp::tooling::gpu_health::Verdict::unverified:
        expected_status = 2;
        break;
    }
    if (run.status != expected_status)
        return local_error(run.status, "incoherent-cli-output",
                           "pulp-cpp doctor gpu exit status does not match its typed verdict",
                           run.output);

    const auto normalized = pulp::tooling::gpu_health::to_json(*evidence);

    return tool_result(run.status, normalized, run.status != 0);
}

std::string handle_gpu_probe(const std::string& params_json) {
    choc::value::Value params;
    try {
        params = choc::json::parse(params_json);
        if (!params.isObject())
            return local_error(-1, "invalid-arguments", "arguments must be an object");
        bool unknown = false;
        std::string unknown_name;
        params.getView().visitObjectMembers([&](std::string_view name,
                                                const choc::value::ValueView&) {
            if (name != "recipe" && name != "artifacts" && name != "negative_control" && !unknown) {
                unknown = true;
                unknown_name = name;
            }
        });
        if (unknown)
            return local_error(-1, "invalid-arguments", "unknown argument: " + unknown_name);
    } catch (...) {
        return local_error(-1, "invalid-arguments", "arguments must be valid JSON");
    }

    if (!params.hasObjectMember("recipe") || !params["recipe"].isString() ||
        params["recipe"].getString().empty())
        return local_error(-1, "invalid-arguments", "recipe is required");
    const auto recipe = std::string(params["recipe"].getString());
    if (pulp::tooling::gpu_probe::find_recipe(recipe) == nullptr)
        return local_error(-1, "invalid-arguments", "recipe is not in the closed catalog");
    if (!params.hasObjectMember("artifacts") || !params["artifacts"].isString() ||
        params["artifacts"].getString().empty())
        return local_error(-1, "invalid-arguments", "artifacts is required");
    const auto artifacts = std::string(params["artifacts"].getString());
    if (!std::filesystem::path(artifacts).is_absolute())
        return local_error(-1, "invalid-arguments", "artifacts must be an absolute path");
    if (params.hasObjectMember("negative_control") && !params["negative_control"].isBool())
        return local_error(-1, "invalid-arguments", "negative_control must be a boolean");
    const bool negative_control =
        params.hasObjectMember("negative_control") && params["negative_control"].getBool();

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

    std::vector<std::string> arguments{"gpu",         "probe",   "--recipe", recipe,
                                       "--artifacts", artifacts, "--json"};
    if (negative_control)
        arguments.emplace_back("--negative-control");
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 5 * 60 * 1000;
    options.max_output_bytes = 1 << 20;
    const auto process = pulp::platform::ChildProcess::run(cli.string(), arguments, options);
    const ExecResult run{process.timed_out || process.was_cancelled ? -1 : process.exit_code,
                         process.stdout_output};

    std::string parse_error;
    const auto evidence = pulp::tooling::gpu_probe::from_json(run.output, &parse_error);
    if (!evidence)
        return local_error(run.status, "malformed-cli-output",
                           "pulp-cpp gpu probe returned invalid v1 evidence: " + parse_error,
                           run.output);
    const auto* recipe_contract = pulp::tooling::gpu_probe::find_recipe(recipe);
    const auto expected_mutation =
        negative_control ? std::optional<std::string>{recipe_contract->negative_mutation}
                         : std::nullopt;
    if (evidence->recipe_id != recipe || evidence->mutation != expected_mutation)
        return local_error(run.status, "incoherent-cli-output",
                           "pulp-cpp gpu probe evidence does not match the request", run.output);
    const auto expected_status = pulp::tooling::gpu_probe::exit_code(*evidence);
    if (run.status != expected_status)
        return local_error(run.status, "incoherent-cli-output",
                           "pulp-cpp gpu probe exit status does not match its typed verdict",
                           run.output);

    return tool_result(run.status, pulp::tooling::gpu_probe::to_json(*evidence), run.status != 0);
}

std::string handle_gpu_recipes(const std::string& params_json) {
    choc::value::Value params;
    try {
        params = choc::json::parse(params_json);
        if (!params.isObject())
            return local_error(-1, "invalid-arguments", "arguments must be an object");
        bool unknown = false;
        std::string unknown_name;
        params.getView().visitObjectMembers(
            [&](std::string_view name, const choc::value::ValueView&) {
                if (name != "action" && name != "recipe" && name != "symptom" && !unknown) {
                    unknown = true;
                    unknown_name = name;
                }
            });
        if (unknown)
            return local_error(-1, "invalid-arguments", "unknown argument: " + unknown_name);
    } catch (...) {
        return local_error(-1, "invalid-arguments", "arguments must be valid JSON");
    }
    const auto string_argument = [&](const char* name) -> std::optional<std::string> {
        if (!params.hasObjectMember(name))
            return std::nullopt;
        if (!params[name].isString() || params[name].getString().empty())
            return std::string{};
        return std::string(params[name].getString());
    };
    const auto action_value = string_argument("action");
    const std::string action = action_value ? *action_value : "list";
    const auto recipe = string_argument("recipe");
    const auto symptom = string_argument("symptom");
    if (action.empty() || (action != "list" && action != "show"))
        return local_error(-1, "invalid-arguments", "action must be list or show");
    if (recipe && recipe->empty())
        return local_error(-1, "invalid-arguments", "recipe must be a non-empty string");
    if (symptom && symptom->empty())
        return local_error(-1, "invalid-arguments", "symptom must be a non-empty string");
    if (action == "show" && !recipe)
        return local_error(-1, "invalid-arguments", "recipe is required for show");
    if (action == "show" && symptom)
        return local_error(-1, "invalid-arguments", "symptom is valid only for list");
    if (action == "list" && recipe)
        return local_error(-1, "invalid-arguments", "recipe is valid only for show");

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

    std::vector<std::string> arguments{"gpu", "recipes", action};
    if (recipe)
        arguments.emplace_back(*recipe);
    if (symptom) {
        arguments.emplace_back("--symptom");
        arguments.emplace_back(*symptom);
    }
    arguments.emplace_back("--json");
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 30 * 1000;
    options.max_output_bytes = 1 << 20;
    const auto process = pulp::platform::ChildProcess::run(cli.string(), arguments, options);
    const int status = process.timed_out || process.was_cancelled ? -1 : process.exit_code;
    if (status != 0)
        return local_error(status, status == 2 ? "not-found" : "cli-failed",
                           "pulp-cpp GPU recipe discovery failed", process.stderr_output);
    try {
        const auto result = choc::json::parse(process.stdout_output);
        validate_recipe_discovery(result, action, recipe, symptom);
    } catch (...) {
        return local_error(status, "malformed-cli-output",
                           "pulp-cpp GPU recipe discovery returned invalid or incoherent data",
                           process.stdout_output);
    }
    return discovery_result(process.stdout_output);
}

} // namespace pulp_mcp
