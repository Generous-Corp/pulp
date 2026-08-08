// pulp-mcp — MCP (Model Context Protocol) server for Pulp
// Exposes Pulp operations as tools via stdin/stdout JSON-RPC 2.0

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/control_inspector_client.hpp>
#endif
#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/service.hpp>

#include <choc/text/choc_JSON.h>

#include "mcp_compat.hpp"
#include "mcp_inspect_tools_internal.hpp"
#include "mcp_json.hpp"
#include "mcp_server.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools.hpp"
#include "pulp_mcp_version.h"
#if PULP_MCP_ENABLE_TIMELINE_TOOLS
#include "timeline_mcp_tools.h"
#endif

namespace fs = std::filesystem;

// JSON-RPC framing + field extraction live in mcp_json.hpp, and project
// SDK-compat resolution lives in mcp_compat.hpp. Pull the helpers into
// file scope so the rest of this TU keeps its existing unqualified call
// sites.
using pulp_mcp::compare_semver;
using pulp_mcp::compat_error_payload;
using pulp_mcp::extract_bool;
using pulp_mcp::extract_double;
using pulp_mcp::extract_int;
using pulp_mcp::extract_raw;
using pulp_mcp::extract_string;
using pulp_mcp::handle_compat;
using pulp_mcp::json_error;
using pulp_mcp::json_result;
using pulp_mcp::json_string;
using pulp_mcp::json_tool_payload;
using pulp_mcp::min_sdk_for_tool;
using pulp_mcp::resolve_project_sdk_version;
// Shell-execution + tool handlers live in mcp_shell.hpp /
// mcp_tools.{hpp,cpp}.
using pulp_mcp::exec;
using pulp_mcp::find_inspector_mcp_tool;
using pulp_mcp::find_project_root;
using pulp_mcp::handle_audio_compare;
using pulp_mcp::handle_audio_excerpt_find;
using pulp_mcp::handle_audio_model_activate;
using pulp_mcp::handle_audio_model_list;
using pulp_mcp::handle_audio_model_status;
using pulp_mcp::handle_audio_plugin_inspect;
using pulp_mcp::handle_audio_probe_json;
using pulp_mcp::handle_audio_read_bundle;
using pulp_mcp::handle_audio_render;
using pulp_mcp::handle_audio_scope;
using pulp_mcp::handle_build;
using pulp_mcp::handle_content;
using pulp_mcp::handle_content_install;
using pulp_mcp::handle_content_list;
using pulp_mcp::handle_content_preview;
using pulp_mcp::handle_content_remove;
using pulp_mcp::handle_content_rescan;
using pulp_mcp::handle_content_reveal;
using pulp_mcp::handle_content_update;
using pulp_mcp::handle_content_validate;
using pulp_mcp::handle_inspect_pending_requests;
using pulp_mcp::handle_kit;
using pulp_mcp::handle_kit_apply;
using pulp_mcp::handle_kit_init;
using pulp_mcp::handle_kit_inspect;
using pulp_mcp::handle_kit_pack;
using pulp_mcp::handle_kit_plan;
using pulp_mcp::handle_kit_publish_check;
using pulp_mcp::handle_kit_remove;
using pulp_mcp::handle_kit_search;
using pulp_mcp::handle_kit_validate;
using pulp_mcp::handle_kit_verify;
using pulp_mcp::handle_minos;
using pulp_mcp::handle_status;
using pulp_mcp::handle_test;
#if PULP_MCP_ENABLE_TIMELINE_TOOLS
using pulp_mcp::handle_timeline_tool;
#endif
using pulp_mcp::handle_validate;
using pulp_mcp::shell_quote;

namespace {

#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
struct InspectorCommandResult {
    pulp::inspect::InspectorClientResult client;
    std::string output;

    bool succeeded() const {
        return client.succeeded();
    }
};

InspectorCommandResult
format_inspector_command_result(pulp::inspect::InspectorClientResult result) {
    const auto output = result.response.is_error
                            ? "Error [" + result.response.error_code +
                                  "]: " + result.response.params_json +
                                  (result.response.error_data_json.empty() ||
                                           result.response.error_data_json == "{}"
                                       ? std::string{}
                                       : "\n" + result.response.error_data_json)
                            : result.response.params_json;
    return {std::move(result), output};
}

InspectorCommandResult run_inspector_command(const fs::path& root, const std::string& method,
                                             const std::string& params_json = "{}",
                                             const std::string& session_id = {},
                                             const std::string& instance_id = {},
                                             const std::string& publication_id = {}) {
    (void)root;
    auto result = pulp::inspect::request_inspector(method, params_json,
                                                   {session_id, instance_id, publication_id});
    return format_inspector_command_result(std::move(result));
}

InspectorCommandResult run_control_trace_command(const std::string& method,
                                                 const std::string& params_json = "{}") {
    return format_inspector_command_result(
        pulp::inspect::request_control_inspector(method, params_json));
}
#else
std::string inspector_component_unavailable_payload() {
    constexpr auto message = "Development Inspector unavailable: pulp-mcp was built with "
                             "PULP_ENABLE_INSPECTOR=OFF";
    return std::string("{\"content\":[{\"type\":\"text\",\"text\":") + json_string(message) +
           "}],\"isError\":true,\"structuredContent\":{\"ok\":false,"
           "\"error\":{\"code\":\"component_unavailable\",\"message\":" +
           json_string(message) + ",\"data\":{}}}}";
}

struct InspectorCommandResult {
    std::string output;

    bool succeeded() const {
        return false;
    }
};

InspectorCommandResult run_inspector_command(const fs::path&, const std::string&,
                                             const std::string& = "{}", const std::string& = {},
                                             const std::string& = {}, const std::string& = {}) {
    return {inspector_component_unavailable_payload()};
}
#endif

bool valid_inspector_identity(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
               (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
    });
}

struct InspectorSelectionFields {
    bool session_id = false;
    bool instance_id = false;
    bool publication_id = false;

    bool any() const {
        return session_id || instance_id || publication_id;
    }

    bool all() const {
        return session_id && instance_id && publication_id;
    }
};

struct InspectorToolArguments {
    choc::value::Value arguments;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    InspectorSelectionFields selection_fields;
};

struct InspectorSetParamArguments {
    std::uint32_t id = 0;
    double value = 0.0;
    bool normalized = false;
};

bool strict_json_lexemes(std::string_view json) {
    const auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    const auto is_delimiter = [](char c) {
        return c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };

    bool in_string = false;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const auto c = json[i];
        if (in_string) {
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c == '"') {
                in_string = false;
            } else if (c == '\\') {
                if (++i >= json.size())
                    return false;
                const auto escaped = json[i];
                if (escaped == 'u') {
                    if (i + 4 >= json.size())
                        return false;
                    for (int digit = 0; digit < 4; ++digit)
                        if (!is_hex(json[++i]))
                            return false;
                } else if (std::string_view("\"\\/bfnrt").find(escaped) == std::string_view::npos) {
                    return false;
                }
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '+')
            return false;
        if (c != '-' && (c < '0' || c > '9'))
            continue;

        std::size_t end = i;
        if (json[end] == '-' && (++end >= json.size() || json[end] < '0' || json[end] > '9'))
            return false;
        if (json[end] == '0') {
            ++end;
            if (end < json.size() && json[end] >= '0' && json[end] <= '9')
                return false;
        } else {
            while (end < json.size() && json[end] >= '0' && json[end] <= '9')
                ++end;
        }
        if (end < json.size() && json[end] == '.') {
            ++end;
            const auto fraction_start = end;
            while (end < json.size() && json[end] >= '0' && json[end] <= '9')
                ++end;
            if (end == fraction_start)
                return false;
        }
        if (end < json.size() && (json[end] == 'e' || json[end] == 'E')) {
            ++end;
            if (end < json.size() && (json[end] == '+' || json[end] == '-'))
                ++end;
            const auto exponent_start = end;
            while (end < json.size() && json[end] >= '0' && json[end] <= '9')
                ++end;
            if (end == exponent_start)
                return false;
        }
        if (end < json.size() && !is_delimiter(json[end]))
            return false;
        i = end - 1;
    }
    return !in_string;
}

bool has_one_complete_root_object(std::string_view json) {
    std::size_t start = 0;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start])))
        ++start;
    if (start == json.size() || json[start] != '{')
        return false;

    std::vector<char> delimiters;
    bool in_string = false;
    bool escaped = false;
    std::size_t end = start;
    for (; end < json.size(); ++end) {
        const auto c = json[end];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            delimiters.push_back(c);
        } else if (c == '}' || c == ']') {
            if (delimiters.empty() || (c == '}' && delimiters.back() != '{') ||
                (c == ']' && delimiters.back() != '['))
                return false;
            delimiters.pop_back();
            if (delimiters.empty()) {
                ++end;
                break;
            }
        }
    }
    if (in_string || !delimiters.empty())
        return false;
    while (end < json.size() && std::isspace(static_cast<unsigned char>(json[end])))
        ++end;
    return end == json.size();
}

bool has_unique_object_members(const choc::value::ValueView& object) {
    if (!object.isObject())
        return false;
    std::unordered_set<std::string> names;
    for (std::uint32_t i = 0; i < object.size(); ++i) {
        const auto member = object.getObjectMemberAt(i);
        if (!names.emplace(member.name).second)
            return false;
    }
    return true;
}

std::optional<InspectorToolArguments>
parse_inspector_tool_arguments(const choc::value::ValueView& request,
                               std::string_view expected_name, std::string& error) {
    try {
        if (!has_unique_object_members(request) || !request.hasObjectMember("params") ||
            !request["params"].isObject()) {
            error = "Error: malformed MCP tool request";
            return std::nullopt;
        }
        const auto params = request["params"];
        if (!has_unique_object_members(params) || !params.hasObjectMember("name") ||
            !params["name"].isString() || params["name"].getString() != expected_name ||
            (params.hasObjectMember("arguments") && !params["arguments"].isObject())) {
            error = "Error: malformed MCP inspector tool request";
            return std::nullopt;
        }
        const auto arguments = params.hasObjectMember("arguments")
                                   ? choc::value::Value(params["arguments"])
                                   : choc::value::createObject("");
        if (!has_unique_object_members(arguments)) {
            error = "Error: inspector tool arguments must not contain duplicate fields";
            return std::nullopt;
        }

        InspectorToolArguments parsed;
        parsed.arguments = choc::value::Value(arguments);
        const auto read_selector = [&](std::string_view name, std::string& value, bool& present) {
            present = arguments.hasObjectMember(std::string(name));
            if (!present)
                return true;
            const auto member = arguments[std::string(name)];
            if (!member.isString())
                return false;
            value = std::string(member.getString());
            return true;
        };
        if (!read_selector("session_id", parsed.session_id, parsed.selection_fields.session_id) ||
            !read_selector("instance_id", parsed.instance_id,
                           parsed.selection_fields.instance_id) ||
            !read_selector("publication_id", parsed.publication_id,
                           parsed.selection_fields.publication_id) ||
            (parsed.selection_fields.any() &&
             (!parsed.selection_fields.all() || !valid_inspector_identity(parsed.session_id) ||
              !valid_inspector_identity(parsed.instance_id) ||
              !valid_inspector_identity(parsed.publication_id)))) {
            error = "Error: session_id, instance_id, and publication_id "
                    "must be supplied together as exact safe identities";
            return std::nullopt;
        }

        return parsed;
    } catch (...) {
        error = "Error: malformed JSON in inspector tool request";
        return std::nullopt;
    }
}

std::optional<InspectorSetParamArguments>
parse_inspector_set_param_arguments(const InspectorToolArguments& tool, std::string& error) {
    const auto& arguments = tool.arguments;
    if (!arguments.hasObjectMember("id") || !arguments["id"].isInt()) {
        error = "Error: id must be an integer from 0 through 4294967295";
        return std::nullopt;
    }
    const auto raw_id = arguments["id"].getInt64();
    if (raw_id < 0 ||
        static_cast<std::uint64_t>(raw_id) > std::numeric_limits<std::uint32_t>::max()) {
        error = "Error: id must be an integer from 0 through 4294967295";
        return std::nullopt;
    }
    if (!arguments.hasObjectMember("value") ||
        (!arguments["value"].isInt() && !arguments["value"].isFloat())) {
        error = "Error: value must be a finite number";
        return std::nullopt;
    }

    InspectorSetParamArguments parsed;
    parsed.id = static_cast<std::uint32_t>(raw_id);
    parsed.value = arguments["value"].getWithDefault(0.0);
    if (!std::isfinite(parsed.value)) {
        error = "Error: value must be a finite number";
        return std::nullopt;
    }
    if (arguments.hasObjectMember("normalized")) {
        if (!arguments["normalized"].isBool()) {
            error = "Error: normalized must be a boolean";
            return std::nullopt;
        }
        parsed.normalized = arguments["normalized"].getWithDefault(false);
    }
    return parsed;
}

std::string inspector_argument_string(const InspectorToolArguments& tool, std::string_view name) {
    const auto key = std::string(name);
    if (!tool.arguments.hasObjectMember(key) || !tool.arguments[key].isString())
        return {};
    return std::string(tool.arguments[key].getString());
}

std::string inspector_argument_json(const InspectorToolArguments& tool, std::string_view name) {
    const auto key = std::string(name);
    if (!tool.arguments.hasObjectMember(key))
        return {};
    return choc::json::toString(tool.arguments[key], false);
}

bool has_inspector_selection(const std::string& session_id, const std::string& instance_id,
                             const std::string& publication_id) {
    return !session_id.empty() || !instance_id.empty() || !publication_id.empty();
}

bool valid_exact_inspector_selection(const std::string& session_id, const std::string& instance_id,
                                     const std::string& publication_id) {
    return valid_inspector_identity(session_id) && valid_inspector_identity(instance_id) &&
           valid_inspector_identity(publication_id);
}

struct InspectorSelection {
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
};

std::optional<InspectorSelection>
resolve_inspector_selection(const fs::path& root, const std::string& session_id = {},
                            const std::string& instance_id = {},
                            const std::string& publication_id = {}) {
    const bool has_explicit_selection =
        has_inspector_selection(session_id, instance_id, publication_id);
    if (has_explicit_selection) {
        if (!valid_exact_inspector_selection(session_id, instance_id, publication_id)) {
            return std::nullopt;
        }
        return InspectorSelection{session_id, instance_id, publication_id};
    }
#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
    auto capabilities = run_inspector_command(root, "Session.getCapabilities");
    if (!capabilities.succeeded())
        return std::nullopt;
    const auto& publication = *capabilities.client.publication;
    auto discovered_session_id = publication.session_id;
    auto discovered_instance_id = publication.instance_id;
    auto discovered_publication_id = publication.publication_id;
    if (!valid_inspector_identity(discovered_session_id) ||
        !valid_inspector_identity(discovered_instance_id) ||
        !valid_inspector_identity(discovered_publication_id))
        return std::nullopt;
    return InspectorSelection{std::move(discovered_session_id), std::move(discovered_instance_id),
                              std::move(discovered_publication_id)};
#else
    (void)root;
    return std::nullopt;
#endif
}

#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
std::string inspector_tool_payload(InspectorCommandResult command) {
    auto payload =
        "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(command.output) + "}]";
    if (command.succeeded()) {
        const auto& publication = *command.client.publication;
        payload += ",\"structuredContent\":{\"ok\":true,\"session\":{";
        payload += "\"session_id\":" + json_string(publication.session_id);
        payload += ",\"instance_id\":" + json_string(publication.instance_id);
        payload +=
            ",\"publication_id\":" + json_string(publication.publication_id) + "},\"result\":";
        try {
            (void)choc::json::parse(command.client.response.params_json);
            payload += command.client.response.params_json;
        } catch (...) {
            payload += json_string(command.client.response.params_json);
        }
        payload += "}";
    } else {
        const auto& response = command.client.response;
        payload += ",\"isError\":true,\"structuredContent\":{"
                   "\"ok\":false,\"error\":{\"code\":" +
                   json_string(response.error_code) +
                   ",\"message\":" + json_string(response.params_json) + ",\"data\":";
        if (response.error_data_json.empty()) {
            payload += "{}";
        } else {
            try {
                (void)choc::json::parse(response.error_data_json);
                payload += response.error_data_json;
            } catch (...) {
                payload += json_string(response.error_data_json);
            }
        }
        payload += "}}";
    }
    payload += "}";
    return payload;
}
#else
std::string inspector_tool_payload(InspectorCommandResult command) {
    return std::move(command.output);
}
#endif

std::string inspector_error_payload(const std::string& message) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) +
           "}],\"isError\":true,\"structuredContent\":{\"ok\":false,\"error\":{\"code\":"
           "\"invalid_arguments\",\"message\":" +
           json_string(message) + ",\"data\":{}}}}";
}

#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
choc::value::Value
inspector_publication_value(const pulp::inspect::InspectorDiscoveryRecord& record) {
    auto value = choc::value::createObject("");
    value.addMember("session_id", choc::value::createString(record.session_id));
    value.addMember("instance_id", choc::value::createString(record.instance_id));
    value.addMember("publication_id", choc::value::createString(record.publication_id));
    value.addMember("plugin_id", choc::value::createString(record.plugin_id));
    value.addMember("profile",
                    choc::value::createString(pulp::inspect::profile_id(record.profile)));
    value.addMember("endpoint", choc::value::createString(record.endpoint));
    value.addMember("protocol_version", choc::value::createString(record.protocol_version));
    return value;
}
#endif

std::string inspector_metadata_payload(const choc::value::Value& result) {
    const auto json = choc::json::toString(result, false);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(json) +
           "}],\"structuredContent\":{\"ok\":true,\"result\":" + json + "}}";
}

#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
std::string inspector_metadata_error_payload(const std::string& code, const std::string& message,
                                             const std::filesystem::path& runtime_directory) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) +
           "}],\"isError\":true,\"structuredContent\":{\"ok\":false,"
           "\"error\":{\"code\":" +
           json_string(code) + ",\"message\":" + json_string(message) +
           ",\"data\":{\"runtime_directory\":" + json_string(runtime_directory.string()) + "}}}}";
}
#endif

std::string inspector_profiles_payload() {
    auto profiles = choc::value::createEmptyArray();
    for (const auto profile :
         {pulp::inspect::InspectorProfile::Off, pulp::inspect::InspectorProfile::Observe,
          pulp::inspect::InspectorProfile::Develop}) {
        auto value = choc::value::createObject("");
        value.addMember("id", choc::value::createString(pulp::inspect::profile_id(profile)));
        auto capabilities = choc::value::createEmptyArray();
        for (const auto capability : pulp::inspect::profile_capabilities(profile)) {
            capabilities.addArrayElement(
                choc::value::createString(pulp::inspect::capability_id(capability)));
        }
        value.addMember("capabilities", capabilities);
        profiles.addArrayElement(value);
    }
    auto result = choc::value::createObject("");
    result.addMember("schema_version", choc::value::createInt32(1));
    result.addMember("profiles", profiles);
    return inspector_metadata_payload(result);
}

std::string inspector_list_payload() {
#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
    pulp::inspect::InspectorDiscoveryReader discovery;
    std::string discovery_issue;
    auto sessions = choc::value::createEmptyArray();
    for (const auto& record : discovery.list(&discovery_issue))
        sessions.addArrayElement(inspector_publication_value(record));
    if (!discovery_issue.empty())
        return inspector_metadata_error_payload("discovery_unavailable", discovery_issue,
                                                discovery.runtime_directory());
    auto result = choc::value::createObject("");
    result.addMember("schema_version", choc::value::createInt32(1));
    result.addMember("sessions", sessions);
    return inspector_metadata_payload(result);
#else
    return inspector_component_unavailable_payload();
#endif
}

std::string inspector_doctor_payload() {
#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
    pulp::inspect::InspectorDiscoveryReader discovery;
    std::string discovery_issue;
    const auto records = discovery.list(&discovery_issue);
    const bool ok = discovery_issue.empty();
    if (!ok)
        return inspector_metadata_error_payload("discovery_unavailable", discovery_issue,
                                                discovery.runtime_directory());
    auto result = choc::value::createObject("");
    result.addMember("schema_version", choc::value::createInt32(1));
    result.addMember("ok", choc::value::createBool(ok));
    result.addMember("runtime_directory",
                     choc::value::createString(discovery.runtime_directory().string()));
    result.addMember("session_count",
                     choc::value::createInt64(static_cast<std::int64_t>(records.size())));
    result.addMember("issues", choc::value::createEmptyArray());
    return inspector_metadata_payload(result);
#else
    return inspector_component_unavailable_payload();
#endif
}

} // namespace

// ── MCP Protocol Handler ─────────────────────────────────────────────────────

std::string pulp_mcp::server::tools_list_json() {
    std::string out;
    out.reserve(32 * 1024);
    out += R"JSON({"tools":[)JSON";
#if PULP_MCP_ENABLE_TIMELINE_TOOLS
    out += pulp_mcp::kTimelineMcpToolsArray;
    out += ",";
#endif
    out +=
        R"JSON({"name":"pulp_build","description":"Build the Pulp project (configure + compile)","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_test","description":"Run the Pulp test suite","inputSchema":{"type":"object","properties":{"filter":{"type":"string","description":"Test name filter (regex)"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_status","description":"Show Pulp project status","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_validate","description":"Run plugin format validators (CLAP, VST3/pluginval, AU, optional AAX). Use screenshot=true to save capture_view-backed editor PNGs under artifacts/screenshots/. Returns JSON report.","inputSchema":{"type":"object","properties":{"all":{"type":"boolean","description":"Run all validators including vstvalidator and full AAX validation"},"json":{"type":"boolean","description":"Return JSON report (default true via MCP)"},"screenshot":{"type":"boolean","description":"Also run pulp validate --screenshot and save capture_view-backed editor PNGs under artifacts/screenshots/"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit","description":"Umbrella wrapper for kit subcommands. Use search/validate/inspect/plan/verify/apply/remove/pack/init/publish for local Pulp kit manifests or .pulpkit archives; search also discovers verified local .pulpcontent archives; no package code is executed before apply/verify approval.","inputSchema":{"type":"object","required":["subcommand"],"properties":{"subcommand":{"type":"string","enum":["search","validate","inspect","show","plan","verify","apply","remove","uninstall","pack","publish","publish-check","init"],"description":"Kit subcommand to run"},"query":{"type":"string","description":"Search query for local package manifests or archives"},"root":{"type":"string","description":"Search root override for kit search"},"lane":{"type":"string","enum":["kit","content"],"description":"Optional kit search lane filter"},"path":{"type":"string","description":"Path for validate/inspect/plan/verify/apply/pack/publish; consuming commands accept .pulpkit archives"},"output":{"type":"string","description":"Archive output path for pack"},"registry_manifest":{"type":"string","description":"Optional local signed registry manifest for publish dry-run"},"yes":{"type":"boolean","description":"Required for apply/remove after reviewing the plan or ownership"},"strict":{"type":"boolean","description":"Validate in strict mode"},"id":{"type":"string","description":"Package id for init or installed kit id for remove"},"kit_id":{"type":"string","description":"Installed kit id for remove"},"kind":{"type":"string","description":"Kind filter for search, or kind for init"},"name":{"type":"string","description":"Display name for init"},"dir":{"type":"string","description":"Directory for init"},"force":{"type":"boolean","description":"Overwrite existing manifest for init"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_search","description":"Search local Pulp package manifests and verified local .pulpkit/.pulpcontent archives without executing package code. Results are classified into kit vs content lanes so arbitrary artifacts do not look like curated pulp add dependencies.","inputSchema":{"type":"object","properties":{"query":{"type":"string","description":"Optional search query"},"root":{"type":"string","description":"Directory, archive, or pulp.package.json file to search"},"kind":{"type":"string","description":"Optional package kind filter, e.g. ui-kit or content-pack"},"lane":{"type":"string","enum":["kit","content"],"description":"Optional trust/workflow lane filter"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_validate","description":"Validate a local kit/content directory, pulp.package.json, .pulpkit archive, or .pulpcontent archive without executing package code. Archives must include files.sha256.json and list every payload file. Template kits must include validation.generatedProjectDiffs review evidence.","inputSchema":{"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Path to a kit/content directory, pulp.package.json, .pulpkit archive, or .pulpcontent archive"},"strict":{"type":"boolean","description":"Promote review-required warnings to errors"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_inspect","description":"Inspect a local Pulp kit/content directory, manifest, .pulpkit archive, or .pulpcontent archive and return its JSON summary. No package code is executed; content-pack manifests are inspectable but cannot be planned/applied as kits.","inputSchema":{"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Path to a kit/content directory, pulp.package.json, .pulpkit archive, or .pulpcontent archive"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_plan","description":"Create a reviewable local kit composition plan from a directory, manifest, or .pulpkit archive without mutating the project or executing package code. Rejects content-pack manifests; use pulp_content_preview/install/update for data-only packs.","inputSchema":{"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Path to a kit directory, pulp.package.json, or .pulpkit archive"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_verify","description":"Run declared kit validation-profile checks after plan review. Accepts directories, manifests, or verified .pulpkit archives. Default mode is metadata-only and includes UI-kit integration preview artifacts when applicable; set execute_screenshots=true to explicitly render exported UI scripts through pulp-screenshot and compare expectedImage baselines when declared, honoring optional visualToleranceBytes.","inputSchema":{"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Path to a kit directory, pulp.package.json, or .pulpkit archive"},"execute_screenshots":{"type":"boolean","description":"Explicitly run Pulp screenshot profiles after plan review"},"screenshot_backend":{"type":"string","description":"Optional pulp-screenshot backend, e.g. auto, skia, gpu, coregraphics"},"screenshot_output_dir":{"type":"string","description":"Optional directory for rendered screenshot artifacts and logs"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_apply","description":"Apply a previously reviewed local kit plan from a directory, manifest, or .pulpkit archive. Rejects content-pack manifests; requires yes=true and still does not execute package code.","inputSchema":{"type":"object","required":["path","yes"],"properties":{"path":{"type":"string","description":"Path to a kit directory, pulp.package.json, or .pulpkit archive"},"yes":{"type":"boolean","description":"Must be true after plan review"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_remove","description":"Remove an installed kit using only constrained lock-recorded kit paths under pulp-kits/<kit-id>/ plus known generated lock/CMake files. Requires yes=true.","inputSchema":{"type":"object","required":["id","yes"],"properties":{"id":{"type":"string","description":"Installed kit id"},"yes":{"type":"boolean","description":"Must be true after ownership review"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_pack","description":"Pack a local kit/content manifest directory into a .pulpkit or .pulpcontent archive with files.sha256.json.","inputSchema":{"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Path to a kit directory or pulp.package.json"},"output":{"type":"string","description":"Archive output path"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_publish_check","description":"Run the metadata-only kit publish dry-run gate for a local kit directory, manifest, or .pulpkit archive. Rejects content-pack manifests; enforces strict manifest validation, license inventory, NOTICE-compatible license files via exports.licenses, human review, validation profiles, kind-specific evidence, and optional signed registry-manifest verification. Does not publish remotely.","inputSchema":{"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Path to a kit directory, pulp.package.json, or .pulpkit archive"},"registry_manifest":{"type":"string","description":"Optional local pulp-registry-manifest-v1 JSON to verify against the package manifest"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_kit_init","description":"Scaffold a local developer kit manifest. Writes metadata only; it does not install or apply anything.","inputSchema":{"type":"object","required":["kind","id"],"properties":{"kind":{"type":"string","enum":["source","ui-kit","template"],"description":"Developer fixture manifest kind"},"id":{"type":"string","description":"Package id"},"name":{"type":"string","description":"Display name"},"dir":{"type":"string","description":"Directory to write pulp.package.json into"},"force":{"type":"boolean","description":"Overwrite an existing manifest"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_content","description":"Umbrella wrapper for data-only content-pack subcommands. .pulpcontent archives must include files.sha256.json. Use validate/preview/list/reveal/rescan freely; install/update/remove require yes=true after review.","inputSchema":{"type":"object","required":["subcommand"],"properties":{"subcommand":{"type":"string","enum":["validate","preview","install","update","list","rescan","remove","uninstall","reveal"],"description":"Content subcommand to run"},"path":{"type":"string","description":"Path for validate/preview/install/update"},"plugin_runtime":{"type":"string","description":"Path to trusted pulp.plugin-runtime.json for preview"},"plugin":{"type":"string","description":"Target plugin id"},"id":{"type":"string","description":"Installed content package id for remove/reveal"},"package_id":{"type":"string","description":"Installed content package id for remove/reveal"},"version":{"type":"string","description":"Optional content package version"},"root":{"type":"string","description":"Optional content data root override"},"yes":{"type":"boolean","description":"Required for install/update/remove after review"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_content_validate","description":"Validate a local .pulpcontent archive or content-pack directory without executing package code. Archives must include files.sha256.json, every payload file must be listed there, and hashes must match.","inputSchema":{"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Path to .pulpcontent archive or content-pack directory"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_content_preview","description":"Preview content-pack compatibility and reload policy against a trusted plugin runtime manifest without installing anything. Archives must include files.sha256.json.","inputSchema":{"type":"object","required":["path","plugin_runtime"],"properties":{"path":{"type":"string","description":"Path to .pulpcontent archive or content-pack directory"},"plugin_runtime":{"type":"string","description":"Path to trusted pulp.plugin-runtime.json"},"plugin":{"type":"string","description":"Optional expected target plugin id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_content_install","description":"Install a validated data-only content pack for a plugin. .pulpcontent archives must include files.sha256.json. Requires yes=true after reviewing the target.","inputSchema":{"type":"object","required":["path","plugin","yes"],"properties":{"path":{"type":"string","description":"Path to .pulpcontent archive or content-pack directory"},"plugin":{"type":"string","description":"Target plugin id"},"root":{"type":"string","description":"Optional content data root override"},"yes":{"type":"boolean","description":"Must be true after install target review"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_content_update","description":"Update a data-only content pack from an explicit local path. .pulpcontent archives must include files.sha256.json. Requires yes=true after review; rolls back a replaced local version on failure.","inputSchema":{"type":"object","required":["path","plugin","yes"],"properties":{"path":{"type":"string","description":"Path to .pulpcontent archive or content-pack directory"},"plugin":{"type":"string","description":"Target plugin id"},"root":{"type":"string","description":"Optional content data root override"},"yes":{"type":"boolean","description":"Must be true after update target review"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_content_list","description":"List installed data-only content packs.","inputSchema":{"type":"object","properties":{"plugin":{"type":"string","description":"Optional plugin id filter"},"root":{"type":"string","description":"Optional content data root override"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_content_rescan","description":"Rebuild the installed content index from local content-pack manifests. Metadata-only; no package code is executed.","inputSchema":{"type":"object","properties":{"root":{"type":"string","description":"Optional content data root override"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_content_remove","description":"Remove an installed content pack by package id/plugin/version. Requires yes=true.","inputSchema":{"type":"object","required":["id","plugin","yes"],"properties":{"id":{"type":"string","description":"Installed content package id"},"plugin":{"type":"string","description":"Target plugin id"},"version":{"type":"string","description":"Optional content package version"},"root":{"type":"string","description":"Optional content data root override"},"yes":{"type":"boolean","description":"Must be true after installed content review"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_content_reveal","description":"Return the local install path for an installed content pack.","inputSchema":{"type":"object","required":["id","plugin"],"properties":{"id":{"type":"string","description":"Installed content package id"},"plugin":{"type":"string","description":"Target plugin id"},"version":{"type":"string","description":"Optional content package version"},"root":{"type":"string","description":"Optional content data root override"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_model_list","description":"List registered audio models and their current install state.","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_model_status","description":"Show the configured audio model and whether its recorded checkpoint is loadable now.","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_model_activate","description":"Activate an installed audio model by logical id.","inputSchema":{"type":"object","required":["model_id"],"properties":{"model_id":{"type":"string","description":"Registered audio model id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_excerpt_find","description":"Run WAV-first excerpt-find with the deterministic null backend and create a bundle.","inputSchema":{"type":"object","required":["text","input_path"],"properties":{"text":{"type":"string","description":"Natural language excerpt query"},"input_path":{"type":"string","description":"File or directory path to scan"},"model_id":{"type":"string","description":"Optional registered audio model id"},"recursive":{"type":"boolean","description":"Recurse into input directories"},"top":{"type":"integer","description":"Maximum ranked results to return"},"window_ms":{"type":"integer","description":"Excerpt window size in milliseconds"},"hop_ms":{"type":"integer","description":"Window hop size in milliseconds"},"min_score":{"type":"number","description":"Minimum deterministic stub score threshold"},"max_candidates_per_file":{"type":"integer","description":"Per-file candidate cap before global ranking"},"bundle_out":{"type":"string","description":"Directory to create excerpt bundles in"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_read_bundle","description":"Read an excerpt-find artifact bundle and return parsed manifest/result summary.","inputSchema":{"type":"object","required":["bundle_path"],"properties":{"bundle_path":{"type":"string","description":"Path to an excerpt-find bundle directory"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_probe_json","description":"Run a standalone through `pulp run --audio-probe-json` and return live output-boundary probe metrics JSON. This uses the existing pulp-mcp server as a one-shot wrapper; it does not start a second MCP server or persistent live socket.","inputSchema":{"type":"object","properties":{"target":{"type":"string","description":"Optional standalone target name to pass to `pulp run`"},"frames":{"type":"integer","description":"Frame delay before reading the probe snapshot (default 90)"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_scope","description":"Return versioned audio scope JSON from a live standalone target or a speakerless offline WAV input. Live target mode opens the audio device; input_wav mode does not emit sound.","inputSchema":{"type":"object","properties":{"target":{"type":"string","description":"Optional standalone target name to pass to `pulp audio scope` for live capture"},"input_wav":{"type":"string","description":"Optional WAV path for speakerless offline scope analysis"},"png_path":{"type":"string","description":"Optional PNG artifact path for offline WAV scope traces"},"frames":{"type":"integer","description":"Frame delay before reading the live scope window (default 90)"},"window":{"type":"integer","description":"Acquisition window in samples (default 2048)"},"trigger":{"type":"string","description":"Trigger mode: none, raw, off, rising-zero"},"channel":{"type":"integer","description":"Source channel index"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_plugin_inspect","description":"Load a third-party plugin in an isolated worker and return its host-visible metadata, buses, latency, tail, and complete parameter API. This inspects observable host contracts, not the plugin's private algorithm or native editor UI.","inputSchema":{"type":"object","required":["plugin"],"properties":{"plugin":{"type":"string","description":"Path to the plugin bundle"},"format":{"type":"string","description":"Plugin format: clap, vst3, au, auv3, or lv2 (default clap)"},"id":{"type":"string","description":"Descriptor/component identity when the bundle contains more than one plugin"},"sample_rate":{"type":"number","description":"Preparation sample rate in Hz (default 48000)"},"block":{"type":"integer","description":"Preparation block size (default 512)"},"warmup_ms":{"type":"integer","description":"Discarded initialization pre-roll while native host events are pumped (default 1000 for AU, 0 otherwise)"},"timeout_ms":{"type":"integer","description":"Isolated worker timeout in milliseconds (default at least 30000 and scales with warmup_ms)"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_render","description":"Render a plugin bundle offline through `pulp audio render` and return the audio-analysis metrics JSON (peak/RMS/DC/clip/NaN per channel). No DAW, no audio device, no sound. Drives an explicit plugin bundle with a test signal plus optional parameter/MIDI automation. Pass exactly one of duration_ms / duration_frames. param and midi take a SINGLE token each (use the CLI for multiple). Set latency:true to PROVE the plugin's reported latency matches the delay actually in its output — returns latency evidence instead of metrics, and reports an ERROR if the claim is disproven or cannot be proven.","inputSchema":{"type":"object","required":["plugin"],"properties":{"plugin":{"type":"string","description":"Path to the plugin bundle to render"},"out":{"type":"string","description":"Optional output WAV path to keep; omitted → a temp WAV is rendered and discarded, only the metrics JSON is returned"},"duration_ms":{"type":"integer","description":"Render length in milliseconds (exactly one of duration_ms / duration_frames)"},"duration_frames":{"type":"integer","description":"Render length in frames (exactly one of duration_ms / duration_frames)"},"format":{"type":"string","description":"Plugin format: clap, vst3, au, auv3, lv2 (default clap)"},"id":{"type":"string","description":"Descriptor URI / unique-id (LV2, multi-plugin CLAP bundles)"},"sample_rate":{"type":"number","description":"Render sample rate in Hz (default 48000)"},"block":{"type":"integer","description":"Max block size in frames (default 512)"},"in_channels":{"type":"integer","description":"Input bus channel count (default 2; 0 for instruments)"},"out_channels":{"type":"integer","description":"Output bus channel count (default 2)"},"input":{"type":"string","description":"Input WAV path (used as-is at sample_rate, no resampling)"},"input_signal":{"type":"string","description":"Generated input: silence, sine:<hz>[,<dbfs>], noise[:<seed>], or impulse[:<frame>]. A latency proof needs noise (delayed-null) or impulse (marker) — silence and sine cannot reveal a delay."},"warmup_ms":{"type":"integer","description":"Discarded initialization pre-roll while native host events are pumped (default 1000 for AU, 0 otherwise)"},"initial_param":{"type":"string","description":"One plain-domain id=value applied after warm-up and before settling; use the CLI for multiple"},"settle_ms":{"type":"integer","description":"Discarded processing after the initial parameter write (default 250 for AU when initial_param is set)"},"timeout_ms":{"type":"integer","description":"Isolated render-worker timeout in milliseconds"},"tail_ms":{"type":"integer","description":"Silent-input tail appended after the requested stimulus duration"},"wav_format":{"type":"string","description":"Output encoding: int16, int24, or float32 (default int16)"},"param":{"type":"string","description":"A single parameter change id=value[@frame]; value is the PLAIN native domain, @frame is sample-accurate"},"midi":{"type":"string","description":"A single note event note:<note>,<vel>,<on>[,<off>]"},"latency":{"type":"boolean","description":"Prove the plugin's reported latency against its rendered audio. Requires an input_signal of noise or impulse. Returns latency evidence (reported vs measured samples, delta, verdict) instead of metrics; an unproven or disproven claim comes back as an error, never a silent pass."},"latency_policy":{"type":"string","description":"delayed-null (default with noise): null the output against the input delayed by D, sweeping D — needs the plugin in a pass-through/bypass/fully-dry mode, which you arrange with param. marker (default with impulse): locate the single onset; for plugins that reshape the signal."},"latency_tolerance":{"type":"integer","description":"Samples of drift allowed between the reported and measured latency (default 0)"},"latency_intrinsic":{"type":"integer","description":"Delay the plugin adds that is NOT latency (e.g. leading silence in a known IR). Subtracted before comparing. marker policy only."},"latency_expect":{"type":"integer","description":"The latency this plugin is SUPPOSED to have, pinned independently of what it reports. Without it the proof is self-consistency only: a plugin whose true delay AND report both grew together still passes, because the host compensates it correctly — it just got slower. Set this to catch that."}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_audio_compare","description":"Advisory, agent-facing before/after judgment between two WAVs (measure -> compare -> judge) via `pulp audio compare`. Delegates to the opt-in Audio Quality Lab tool (no DSP in the CLI); level-matches, runs one measurement axis, and returns the typed quality_lab.compare.v1 report JSON (evidence envelope + verdict: regression_suspected | material_change_detected | no_material_change_detected | inconclusive | invalid). Advisory, never a gate. Requires the tool: `pulp tool install audio-quality-lab`.","inputSchema":{"type":"object","required":["reference","candidate"],"properties":{"reference":{"type":"string","description":"Path to the reference (before) WAV"},"candidate":{"type":"string","description":"Path to the candidate (after) WAV"},"profile":{"type":"string","description":"Measurement axis: tonal-balance (LTAS centroid), added-hf (band-relative >=8kHz dB), noise-roughness (HNR drop), graininess (spectral-flux rise), stereo-width (side/mid + phase; needs 2-channel input), or transient-integrity (per-onset attack smear; needs percussive material). Default tonal-balance"},"reference_role":{"type":"string","description":"peer (neutral A/B, default) or golden (reference is known-good; enables regression_suspected)"},"align":{"type":"string","description":"Time-align before measuring: none (default), latency (trim a constant delay/offset so a pure shift is not read as a material change), varispeed:R (undo a declared tape-style speed change by resampling to the reference time base), stretch:R (a declared pitch-preserving time-stretch — measure warp-invariant axes directly, warp-normalize graininess + corroboration), pitch:S (a declared duration-preserving pitch shift in semitones — compensate tonal-balance for the expected centroid move), or ratio:auto (ESTIMATE a uniform stretch ratio, double-gated). Refuses if the audio does not support the request"},"threshold":{"type":"number","description":"Materiality override (finite positive; the per-axis valid range is enforced by the tool); defaults to the axis's own default"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_screenshot","description":"Render a built-in demo or JavaScript UI fixture to PNG (base64). For project plugin screenshots, use pulp_validate with screenshot=true or the CLI's pulp run --headless --screenshot path.","inputSchema":{"type":"object","properties":{"script":{"type":"string","description":"Path to JS UI script"},"width":{"type":"integer","description":"Width in points (default 400)"},"height":{"type":"integer","description":"Height in points (default 300)"},"theme":{"type":"string","description":"Theme: dark, light, pro_audio"},"demo":{"type":"boolean","description":"Render built-in demo UI"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_simulate_click","description":"Simulate a mouse click at coordinates on a demo UI and return the view tree JSON","inputSchema":{"type":"object","properties":{"x":{"type":"number","description":"X coordinate"},"y":{"type":"number","description":"Y coordinate"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_get_view_tree","description":"Get the view tree as JSON for a demo UI","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_create","description":"Scaffold a new plugin project from templates","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Plugin name"},"type":{"type":"string","enum":["effect","instrument"],"description":"Plugin type"},"manufacturer":{"type":"string","description":"Manufacturer name"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_docs_check","description":"Validate docs consistency against the codebase","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_docs_search","description":"Search local docs for a query string","inputSchema":{"type":"object","properties":{"query":{"type":"string","description":"Search query"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_profiles","description":"Installed in-process client that lists the stable Development Inspector profiles and their declared capabilities.","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_list","description":"Installed in-process client that discovers live owner-private inspector publications and returns their exact session, instance, and non-reusable publication identities.","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_capabilities","description":"Installed in-process client that authenticates to one exact inspector publication and returns its available and effective capabilities with structured errors.","inputSchema":{"type":"object","required":["session_id","instance_id","publication_id"],"properties":{"session_id":{"type":"string","description":"Exact session id from pulp_inspect_list"},"instance_id":{"type":"string","description":"Exact instance id from pulp_inspect_list"},"publication_id":{"type":"string","description":"Exact non-reusable publication id from pulp_inspect_list"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_doctor","description":"Installed in-process client that reports inspector discovery-directory readiness and the current live-publication count.","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_dom","description":"Installed DOM.getDocument client for one exact explicitly activated Development Inspector publication.","inputSchema":{"type":"object","properties":{"session_id":{"type":"string","description":"Exact session id from pulp_inspect_list"},"instance_id":{"type":"string","description":"Exact instance id from pulp_inspect_list"},"publication_id":{"type":"string","description":"Exact non-reusable publication id from pulp_inspect_list"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_params","description":"Installed State.getParameters client for one exact explicitly activated Development Inspector publication.","inputSchema":{"type":"object","properties":{"session_id":{"type":"string","description":"Exact session id from pulp_inspect_list"},"instance_id":{"type":"string","description":"Exact instance id from pulp_inspect_list"},"publication_id":{"type":"string","description":"Exact non-reusable publication id from pulp_inspect_list"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_value_channels","description":"Installed value-channel catalog client for one exact Development Inspector publication. Independent live telemetry is not wired.","inputSchema":{"type":"object","properties":{"session_id":{"type":"string","description":"Exact session id from pulp_inspect_list"},"instance_id":{"type":"string","description":"Exact instance id from pulp_inspect_list"},"publication_id":{"type":"string","description":"Exact non-reusable publication id from pulp_inspect_list"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_set_param","description":"Installed typed State.setParameter client for one exact publication whose profile grants session.control and state.write. The shared client acquires and releases a same-connection controller lease.","inputSchema":{"type":"object","required":["id","value"],"properties":{"id":{"type":"integer","description":"Parameter id from pulp_inspect_params"},"value":{"type":"number","description":"New value (raw, or a 0..1 position when normalized=true)"},"normalized":{"type":"boolean","description":"Treat value as a 0..1 normalized position (default false)"},"session_id":{"type":"string","description":"Exact session id from pulp_inspect_list"},"instance_id":{"type":"string","description":"Exact instance id from pulp_inspect_list"},"publication_id":{"type":"string","description":"Exact non-reusable publication id from pulp_inspect_list"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_inject_midi","description":"Installed typed Test.injectMidi client for one exact standalone publication. A note_on is held for the required bounded duration and paired with note_off on the same controller lease.","inputSchema":{"type":"object","required":["kind","channel","note","session_id","instance_id","publication_id"],"properties":{"kind":{"type":"string","enum":["note_on","note_off"]},"channel":{"type":"integer","minimum":1,"maximum":16},"note":{"type":"integer","minimum":0,"maximum":127},"velocity":{"type":"integer","minimum":0,"maximum":127,"description":"Required for note_on; defaults to zero for note_off"},"duration_ms":{"type":"integer","minimum":1,"maximum":2000,"description":"Required bounded hold for note_on; invalid for note_off"},"session_id":{"type":"string","description":"Exact session id from pulp_inspect_list"},"instance_id":{"type":"string","description":"Exact instance id from pulp_inspect_list"},"publication_id":{"type":"string","description":"Exact non-reusable publication id from pulp_inspect_list"}},"additionalProperties":false}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_set_transport","description":"Installed typed Test.setTransport client for one exact standalone publication. Applies one coherent transport update through the normal host control path under a same-connection controller lease.","inputSchema":{"type":"object","required":["session_id","instance_id","publication_id"],"properties":{"playing":{"type":"boolean"},"position_samples":{"type":"integer","minimum":0},"tempo_bpm":{"type":"number","minimum":20,"maximum":400},"session_id":{"type":"string","description":"Exact session id from pulp_inspect_list"},"instance_id":{"type":"string","description":"Exact instance id from pulp_inspect_list"},"publication_id":{"type":"string","description":"Exact non-reusable publication id from pulp_inspect_list"}},"additionalProperties":false}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_screenshot","description":"Installed in-process whole-window capture client for an explicitly activated standalone whose selected host supports deterministic back-buffer capture, or an explicit custom host. Capture.screenshotNode remains unavailable.","inputSchema":{"type":"object","properties":{"session_id":{"type":"string","description":"Optional exact session id; requires instance_id and publication_id"},"instance_id":{"type":"string","description":"Optional exact instance id; requires session_id and publication_id"},"publication_id":{"type":"string","description":"Optional exact non-reusable publication id; requires session_id and instance_id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_evaluate","description":"Installed in-process Runtime.evaluate client for an explicit custom host only. Standalone profiles do not grant runtime.eval; high-risk evaluation requires separate host wiring and enablement.","inputSchema":{"type":"object","properties":{"expression":{"type":"string","description":"JS expression to evaluate"},"session_id":{"type":"string","description":"Optional exact session id; requires instance_id and publication_id"},"instance_id":{"type":"string","description":"Optional exact instance id; requires session_id and publication_id"},"publication_id":{"type":"string","description":"Optional exact non-reusable publication id; requires session_id and instance_id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_performance","description":"Installed in-process Performance.getMetrics client for an explicitly activated standalone or explicit custom host. Individual metrics report unavailable when the selected session has no corresponding render source.","inputSchema":{"type":"object","properties":{"session_id":{"type":"string","description":"Optional exact session id; requires instance_id and publication_id"},"instance_id":{"type":"string","description":"Optional exact instance id; requires session_id and publication_id"},"publication_id":{"type":"string","description":"Optional exact non-reusable publication id; requires session_id and instance_id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_audio","description":"Installed in-process Audio.getConfig client for an explicitly activated standalone from `pulp run --inspect`, or an explicit custom host.","inputSchema":{"type":"object","properties":{"session_id":{"type":"string","description":"Optional exact session id; requires instance_id and publication_id"},"instance_id":{"type":"string","description":"Optional exact instance id; requires session_id and publication_id"},"publication_id":{"type":"string","description":"Optional exact non-reusable publication id; requires session_id and instance_id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_inspect_pending_requests","description":"Read the pull-based agent-request queue (.pulp-design-requests.json) for a design project: the not-yet-consumed free-text requests a human raised from the running design's send-to-agent affordance. Returns a JSON array of pending requests, each with id, text, design, screen, editmode_state, screenshot_path, created_at, and consumed. An empty or absent queue returns an empty array, not an error.","inputSchema":{"type":"object","properties":{"project_dir":{"type":"string","description":"Design project directory containing .pulp-design-requests.json (defaults to the enclosing Pulp project root)"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_motion_start_trace","description":"Experimental source-checkout custom-fixture client for Motion.startTrace; normal Pulp launches provide no endpoint. Resolves and pins one authenticated publication, then returns trace_id plus the exact session_id, instance_id, and non-reusable publication_id required for follow-up mutations.","inputSchema":{"type":"object","required":["view_name","metrics"],"properties":{"view_name":{"type":"string","description":"Human-readable trace name attached to all emitted events"},"fps":{"type":"integer","description":"Target sample rate in frames per second (default 15)"},"metrics":{"type":"array","description":"Metric probes. Each item is {kind:'geometry'|'scroll-geometry', name, node_id, properties?, space?, source?}.","items":{"type":"object","required":["kind"],"properties":{"kind":{"type":"string","enum":["geometry","scroll-geometry","scrollGeometry"]},"name":{"type":"string"},"node_id":{"type":"string"},"properties":{"type":"array","items":{"type":"string"}},"space":{"type":"string"},"source":{"type":"string"}}}},"session_id":{"type":"string","description":"Optional exact session id; requires instance_id and publication_id"},"instance_id":{"type":"string","description":"Optional exact instance id; requires session_id and publication_id"},"publication_id":{"type":"string","description":"Optional exact non-reusable publication id; requires session_id and instance_id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_motion_stop_trace","description":"Experimental source-checkout custom-fixture client for Motion.stopTrace; normal Pulp launches provide no endpoint. Releases a fixture-owned trace on the exact publication selected by start.","inputSchema":{"type":"object","required":["trace_id","session_id","instance_id","publication_id"],"properties":{"trace_id":{"type":"integer","description":"trace_id returned by pulp_motion_start_trace"},"session_id":{"type":"string","description":"Exact session_id returned by pulp_motion_start_trace"},"instance_id":{"type":"string","description":"Exact instance_id returned by pulp_motion_start_trace"},"publication_id":{"type":"string","description":"Non-reusable publication_id returned by pulp_motion_start_trace"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_motion_snapshot","description":"Experimental source-checkout custom-fixture client for Motion.snapshot; normal Pulp launches provide no endpoint.","inputSchema":{"type":"object","properties":{"session_id":{"type":"string","description":"Optional exact session id; requires instance_id and publication_id"},"instance_id":{"type":"string","description":"Optional exact instance id; requires session_id and publication_id"},"publication_id":{"type":"string","description":"Optional exact non-reusable publication id; requires session_id and instance_id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_motion_list_traces","description":"Experimental source-checkout custom-fixture client for Motion.listTraces; normal Pulp launches provide no endpoint.","inputSchema":{"type":"object","properties":{"session_id":{"type":"string","description":"Optional exact session id; requires instance_id and publication_id"},"instance_id":{"type":"string","description":"Optional exact instance id; requires session_id and publication_id"},"publication_id":{"type":"string","description":"Optional exact non-reusable publication id; requires session_id and instance_id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_motion_scrub_to","description":"Experimental source-checkout custom-fixture client for Motion.scrubTo on one exact session; normal Pulp launches provide no endpoint.","inputSchema":{"type":"object","required":["frame","session_id","instance_id","publication_id"],"properties":{"frame":{"type":"integer","description":"Target playhead frame (>= 0)"},"session_id":{"type":"string","description":"Exact session_id returned by pulp_motion_start_trace"},"instance_id":{"type":"string","description":"Exact instance_id returned by pulp_motion_start_trace"},"publication_id":{"type":"string","description":"Non-reusable publication_id returned by pulp_motion_start_trace"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_motion_play","description":"Experimental source-checkout custom-fixture client for Motion.play on one exact session; normal Pulp launches provide no endpoint.","inputSchema":{"type":"object","required":["session_id","instance_id","publication_id"],"properties":{"session_id":{"type":"string","description":"Exact session_id returned by pulp_motion_start_trace"},"instance_id":{"type":"string","description":"Exact instance_id returned by pulp_motion_start_trace"},"publication_id":{"type":"string","description":"Non-reusable publication_id returned by pulp_motion_start_trace"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_motion_pause","description":"Experimental source-checkout custom-fixture client for Motion.pause on one exact session; normal Pulp launches provide no endpoint.","inputSchema":{"type":"object","required":["session_id","instance_id","publication_id"],"properties":{"session_id":{"type":"string","description":"Exact session_id returned by pulp_motion_start_trace"},"instance_id":{"type":"string","description":"Exact instance_id returned by pulp_motion_start_trace"},"publication_id":{"type":"string","description":"Non-reusable publication_id returned by pulp_motion_start_trace"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_motion_enable_cost","description":"Experimental source-checkout custom-fixture client for Motion.enableCost on one exact session; normal Pulp launches provide no endpoint.","inputSchema":{"type":"object","required":["session_id","instance_id","publication_id"],"properties":{"session_id":{"type":"string","description":"Exact authenticated session id"},"instance_id":{"type":"string","description":"Exact authenticated instance id"},"publication_id":{"type":"string","description":"Exact non-reusable publication id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_motion_disable_cost","description":"Experimental source-checkout custom-fixture client for Motion.disableCost on one exact session; normal Pulp launches provide no endpoint.","inputSchema":{"type":"object","required":["session_id","instance_id","publication_id"],"properties":{"session_id":{"type":"string","description":"Exact authenticated session id"},"instance_id":{"type":"string","description":"Exact authenticated instance id"},"publication_id":{"type":"string","description":"Exact non-reusable publication id"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_trace_start","description":"Begin trace capture through the canonical capability-control client. The broker owns authenticated target selection, consent, grants, and receipts; legacy Inspector publication and raw host/port selectors are not accepted.","inputSchema":{"type":"object","properties":{"categories":{"type":"array","description":"Span categories to record (e.g. dsp, render, gpu, text, js, layout). Empty lets the host pick its default taxonomy.","items":{"type":"string"}},"ring_mb":{"type":"integer","minimum":1,"maximum":512,"description":"In-process ring size in mebibytes (default 80; range 1 through 512)"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_trace_stop","description":"Stop trace capture through the canonical capability-control client. The broker resolves the authorized target; legacy Inspector publication and raw host/port selectors are not accepted.","inputSchema":{"type":"object","properties":{}}},)JSON";
    out +=
        R"JSON({"name":"pulp_minos","description":"Measure the minimum OS a built binary needs, read straight from the artifact (macOS deployment target / Linux glibc symbol version / Windows PE subsystem). Point it at any Mach-O / ELF / PE / static archive or a plugin bundle's inner binary. The floor of a binary is the MAX minimum among everything linked into it. The multi-repo consumer sweep (rebuild every downstream project and compare floors) is CLI-only: `pulp minos sweep`, because it clones and builds many repositories.","inputSchema":{"type":"object","required":["binary"],"properties":{"binary":{"type":"string","description":"Path to a built binary: a .dylib/.so/.dll, a .a static archive, an executable, or a plugin bundle's inner binary (e.g. Foo.vst3/Contents/MacOS/Foo)"}}}},)JSON";
    out +=
        R"JSON({"name":"pulp_compat","description":"Report pulp-mcp / MCP protocol / project SDK versions plus per-tool min_sdk_version floors so clients can pre-filter their tool list. Use this once at startup to detect SDK skew.","inputSchema":{"type":"object","properties":{}}})JSON";
    out += R"JSON(]})JSON";
    if (!pulp_mcp::decorate_inspector_mcp_tool_descriptions(out))
        return R"JSON({"tools":[],"error":"invalid inspector MCP registry"})JSON";
    return out;
}

// MCP spec: stdio transport messages are line-delimited and MUST NOT
// contain embedded `\n` or `\r`. Several response builders use
// multi-line R"JSON(...)" raw strings for readability. Strip those
// here so every wire-bound response is a single line.
//
// Applied at the bottom of handle_request, so the contract holds for every
// caller. Pinned by `test/test_mcp_server.cpp` "MCP wire output never
// contains embedded newlines" [issue-2091].
static std::string compact_for_wire(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '\n' && c != '\r')
            out += c;
    }
    return out;
}

static std::string handle_request_raw(const std::string& json);

std::string pulp_mcp::server::handle_request(const std::string& json) {
    return compact_for_wire(handle_request_raw(json));
}

static std::string handle_request_raw(const std::string& json) {
    choc::value::Value request;
    try {
        if (!strict_json_lexemes(json) || !has_one_complete_root_object(json))
            return json_error("null", -32700, "Parse error");
        request = choc::json::parse(json);
    } catch (...) {
        return json_error("null", -32700, "Parse error");
    }
    if (!has_unique_object_members(request))
        return json_error("null", -32600, "Invalid Request");

    std::string method;
    if (request.hasObjectMember("method") && request["method"].isString())
        method = std::string(request["method"].getString());
    std::string id = "null";
    if (request.hasObjectMember("id"))
        id = choc::json::toString(request["id"], false);

    if (method == "initialize") {
        // serverInfo.version tracks the SDK/CLI release. It is wired to
        // PROJECT_VERSION via tools/mcp/pulp_mcp_version.h.in so
        // doctor/launcher can see real drift between an old installed
        // pulp-mcp and a newer plugin.
        std::string payload =
            std::string(
                R"JSON({"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"pulp-mcp","version":")JSON") +
            PULP_MCP_SERVER_VERSION + std::string(R"JSON("}})JSON");
        return json_result(id, payload);
    }

    if (method == "notifications/initialized") {
        return {}; // No response for notifications
    }

    if (method == "tools/list") {
        return json_result(id, pulp_mcp::server::tools_list_json());
    }

    if (method == "tools/call") {
        if (!request.hasObjectMember("params") || !request["params"].isObject() ||
            !has_unique_object_members(request["params"]))
            return json_error(id, -32602, "Invalid tools/call params");
        const auto params = request["params"];
        if (!params.hasObjectMember("name") || !params["name"].isString())
            return json_error(id, -32602, "Invalid tools/call name");
        const auto name = std::string(params["name"].getString());
        std::string args_json = "{}";
        if (params.hasObjectMember("arguments")) {
            if (!params["arguments"].isObject())
                return json_error(id, -32602, "Invalid tools/call arguments");
            args_json = choc::json::toString(params["arguments"], false);
        }

        // Per-tool feature detection. If the tool declares a min_sdk floor
        // and the project pins an older SDK, return a structured error result
        // with `isError: true` so the LLM gets actionable upgrade guidance
        // instead of silently running the newer behavior. `pulp_compat` is
        // exempt — clients invoke it
        // *to* discover skew and must always be able to read the
        // matrix. Tools left out of the table default to "0.0.0" so
        // existing behavior is unchanged.
        if (name != "pulp_compat") {
            auto min_sdk = min_sdk_for_tool(name);
            if (min_sdk != "0.0.0") {
                auto project_sdk = resolve_project_sdk_version();
                if (!project_sdk.empty() && compare_semver(project_sdk, min_sdk) < 0) {
                    return json_result(id, compat_error_payload(name, min_sdk, project_sdk));
                }
                // If we couldn't resolve the project SDK at all, fall open.
                // The launcher has already started; gating on "no project
                // root" would make pulp-mcp unusable from `/tmp` or similar.
            }
        }

        std::string result;
        const auto* inspector_tool = find_inspector_mcp_tool(name);
        const bool inspector_capabilities_tool = name == "pulp_inspect_capabilities";
        const bool installed_inspector_tool =
            inspector_tool != nullptr && name.starts_with("pulp_inspect_");
        std::optional<InspectorToolArguments> inspector_arguments;
        if (inspector_tool != nullptr || inspector_capabilities_tool) {
            std::string parse_error;
            inspector_arguments = parse_inspector_tool_arguments(request, name, parse_error);
            if (!inspector_arguments && !installed_inspector_tool && !inspector_capabilities_tool &&
                find_project_root().empty()) {
                return json_result(id, "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in "
                                       "a Pulp project\"}]}");
            }
            if (!inspector_arguments)
                return json_result(id, inspector_error_payload(parse_error));
            const bool discovers_and_pins_publication = name == "pulp_motion_start_trace";
            const bool uses_canonical_control =
                name == "pulp_trace_start" || name == "pulp_trace_stop";
            if (!discovers_and_pins_publication && !uses_canonical_control &&
                !inspector_arguments->selection_fields.all()) {
                if (!installed_inspector_tool && !inspector_capabilities_tool &&
                    find_project_root().empty()) {
                    return json_result(id, "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not "
                                           "in a Pulp project\"}]}");
                }
                return json_result(id,
                                   inspector_error_payload(
                                       "Error: session_id, instance_id, and publication_id "
                                       "are required to select one exact inspector publication"));
            }
        }
        if (name == "pulp_inspect_profiles") {
            result = inspector_profiles_payload();
        } else if (name == "pulp_inspect_list") {
            result = inspector_list_payload();
        } else if (name == "pulp_inspect_doctor") {
            result = inspector_doctor_payload();
        } else if (name == "pulp_inspect_capabilities") {
            auto command = run_inspector_command(
                {}, std::string(pulp::inspect::methods::kSessionGetCapabilities), "{}",
                inspector_arguments->session_id, inspector_arguments->instance_id,
                inspector_arguments->publication_id);
            result = inspector_tool_payload(std::move(command));
        } else if (name == "pulp_compat")
            result = handle_compat();
        else if (name == "pulp_build")
            result = handle_build(args_json);
        else if (name == "pulp_test")
            result = handle_test(args_json);
        else if (name == "pulp_status")
            result = handle_status(args_json);
        else if (name == "pulp_validate")
            result = handle_validate(args_json);
        else if (name == "pulp_minos")
            result = handle_minos(args_json);
        else if (name == "pulp_kit")
            result = handle_kit(args_json);
        else if (name == "pulp_kit_search")
            result = handle_kit_search(args_json);
        else if (name == "pulp_kit_validate")
            result = handle_kit_validate(args_json);
        else if (name == "pulp_kit_inspect")
            result = handle_kit_inspect(args_json);
        else if (name == "pulp_kit_plan")
            result = handle_kit_plan(args_json);
        else if (name == "pulp_kit_verify")
            result = handle_kit_verify(args_json);
        else if (name == "pulp_kit_apply")
            result = handle_kit_apply(args_json);
        else if (name == "pulp_kit_remove")
            result = handle_kit_remove(args_json);
        else if (name == "pulp_kit_pack")
            result = handle_kit_pack(args_json);
        else if (name == "pulp_kit_publish_check")
            result = handle_kit_publish_check(args_json);
        else if (name == "pulp_kit_init")
            result = handle_kit_init(args_json);
        else if (name == "pulp_content")
            result = handle_content(args_json);
        else if (name == "pulp_content_validate")
            result = handle_content_validate(args_json);
        else if (name == "pulp_content_preview")
            result = handle_content_preview(args_json);
        else if (name == "pulp_content_install")
            result = handle_content_install(args_json);
        else if (name == "pulp_content_update")
            result = handle_content_update(args_json);
        else if (name == "pulp_content_list")
            result = handle_content_list(args_json);
        else if (name == "pulp_content_rescan")
            result = handle_content_rescan(args_json);
        else if (name == "pulp_content_remove")
            result = handle_content_remove(args_json);
        else if (name == "pulp_content_reveal")
            result = handle_content_reveal(args_json);
        else if (name == "pulp_audio_model_list")
            result = handle_audio_model_list(args_json);
        else if (name == "pulp_audio_model_status")
            result = handle_audio_model_status(args_json);
        else if (name == "pulp_audio_model_activate")
            result = handle_audio_model_activate(args_json);
        else if (name == "pulp_audio_excerpt_find")
            result = handle_audio_excerpt_find(args_json);
        else if (name == "pulp_audio_read_bundle")
            result = handle_audio_read_bundle(args_json);
        else if (name == "pulp_audio_probe_json")
            result = handle_audio_probe_json(args_json);
        else if (name == "pulp_audio_scope")
            result = handle_audio_scope(args_json);
        else if (name == "pulp_audio_plugin_inspect")
            result = handle_audio_plugin_inspect(args_json);
        else if (name == "pulp_audio_render")
            result = handle_audio_render(args_json);
        else if (name == "pulp_audio_compare")
            result = handle_audio_compare(args_json);
#if PULP_MCP_ENABLE_TIMELINE_TOOLS
        else if (auto timeline = handle_timeline_tool(name, args_json))
            result = std::move(*timeline);
#endif
        else if (name == "pulp_screenshot" || name == "pulp_simulate_click" ||
                 name == "pulp_get_view_tree") {
            // These tools delegate to pulp-screenshot binary
            auto root = find_project_root();
            if (root.empty()) {
                result =
                    "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto screenshot_bin = root / "build" / "tools" / "screenshot" / "pulp-screenshot";
                if (name == "pulp_screenshot") {
                    auto demo = extract_string(args_json, "demo");
                    auto script = extract_string(args_json, "script");
                    std::string cmd = shell_quote(screenshot_bin.string()) + " --base64";
                    if (!script.empty())
                        cmd += " --script " + shell_quote(script);
                    else
                        cmd += " --demo";
                    auto theme = extract_string(args_json, "theme");
                    if (!theme.empty())
                        cmd += " --theme " + shell_quote(theme);
                    auto output = exec(cmd + " 2>/dev/null");
                    result = "{\"content\":[{\"type\":\"image\",\"data\":\"" + output +
                             "\",\"mimeType\":\"image/png\"}]}";
                } else {
                    // simulate_click and get_view_tree: run screenshot in demo mode, capture view tree
                    std::string cmd = shell_quote(screenshot_bin.string()) +
                                      " --demo --output /dev/null 2>/dev/null";
                    exec(cmd);
                    result = "{\"content\":[{\"type\":\"text\",\"text\":\"View tree and event "
                             "simulation available via pulp-screenshot --demo\"}]}";
                }
            }
        } else if (name == "pulp_create") {
            auto root = find_project_root();
            if (root.empty()) {
                result =
                    "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto plugin_name = extract_string(args_json, "name");
                auto plugin_type = extract_string(args_json, "type");
                auto manufacturer = extract_string(args_json, "manufacturer");
                if (plugin_name.empty()) {
                    result =
                        "{\"content\":[{\"type\":\"text\",\"text\":\"Error: name is required\"}]}";
                } else {
                    std::string cmd =
                        "python3 " + shell_quote((root / "tools" / "create-project.py").string());
                    cmd += " " + shell_quote(plugin_name);
                    if (!plugin_type.empty())
                        cmd += " --type " + shell_quote(plugin_type);
                    if (!manufacturer.empty())
                        cmd += " --manufacturer " + shell_quote(manufacturer);
                    auto output = exec(cmd + " 2>&1");
                    result =
                        "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
                }
            }
        } else if (name == "pulp_docs_check") {
            auto root = find_project_root();
            if (root.empty()) {
                result =
                    "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto output = exec(
                    "bash " + shell_quote((root / "tools" / "check-docs.sh").string()) + " 2>&1");
                result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
            }
        } else if (name == "pulp_docs_search") {
            auto root = find_project_root();
            auto query = extract_string(args_json, "query");
            if (root.empty()) {
                result =
                    "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else if (query.empty()) {
                result =
                    "{\"content\":[{\"type\":\"text\",\"text\":\"Error: query is required\"}]}";
            } else {
                auto output = exec(shell_quote(pulp_mcp::resolve_cli_binary(root).string()) +
                                   " docs search " + shell_quote(query) + " 2>&1");
                result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
            }
        }
        // Motion inspector tools use the shared in-process client. Each tool
        // maps to one of the
        // Motion.* protocol methods routed by MotionInspector::handle
        // (inspect/src/motion_inspector.cpp) and MotionScrubber::handle
        // (inspect/src/motion_scrubber.cpp). Off-by-default semantics:
        // tools that operate on global state (snapshot, listTraces,
        // pause, disableCost) return clean payloads even when nothing
        // is active; tools that require prior state (scrubTo / play
        // without a loaded fixture, stopTrace with an unknown id) get
        // a structured inspector error which we propagate verbatim.
        // pulp_motion_start_trace flips Coordinator::tracing_enabled
        // on attach (matches motion_inspector.cpp:~265), so callers
        // don't need to pre-arm tracing.
        else if (inspector_tool != nullptr && name.starts_with("pulp_motion_")) {
            std::string inspector_method(inspector_tool->method);
            std::string inspector_params = "{}";
            if (name == "pulp_motion_start_trace") {
                const auto view_name = inspector_argument_string(*inspector_arguments, "view_name");
                const auto metrics = inspector_argument_json(*inspector_arguments, "metrics");
                const auto fps = inspector_argument_json(*inspector_arguments, "fps");
                inspector_params = "{\"view_name\":" + json_string(view_name) +
                                   ",\"metrics\":" + (metrics.empty() ? "null" : metrics);
                if (!fps.empty())
                    inspector_params += ",\"fps\":" + fps;
                inspector_params += "}";
            } else if (name == "pulp_motion_stop_trace") {
                auto trace_id_raw = inspector_argument_json(*inspector_arguments, "trace_id");
                if (trace_id_raw.empty())
                    trace_id_raw = "0";
                inspector_params = std::string("{\"trace_id\":") + trace_id_raw + "}";
            } else if (name == "pulp_motion_scrub_to") {
                auto frame_raw = inspector_argument_json(*inspector_arguments, "frame");
                if (frame_raw.empty())
                    frame_raw = "0";
                inspector_params = std::string("{\"frame\":") + frame_raw + "}";
            }

            auto root = find_project_root();
            if (root.empty()) {
                result =
                    "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto session_id = inspector_arguments->session_id;
                auto instance_id = inspector_arguments->instance_id;
                auto publication_id = inspector_arguments->publication_id;
                const auto selection_fields = inspector_arguments->selection_fields;
                if (selection_fields.any() &&
                    (!selection_fields.all() ||
                     !valid_exact_inspector_selection(session_id, instance_id, publication_id))) {
                    result = inspector_error_payload(
                        "Error: session_id, instance_id, and publication_id "
                        "must be supplied together as exact safe identities");
                }
                if (result.empty() && name == "pulp_motion_start_trace") {
                    auto selection =
                        resolve_inspector_selection(root, session_id, instance_id, publication_id);
                    if (!selection) {
                        result = inspector_error_payload(
                            "Error: could not resolve a safe exact inspector "
                            "publication before Motion.startTrace");
                    } else {
                        session_id = selection->session_id;
                        instance_id = selection->instance_id;
                        publication_id = selection->publication_id;
                    }
                }
                const bool requires_selection =
                    name == "pulp_motion_start_trace" || name == "pulp_motion_stop_trace" ||
                    name == "pulp_motion_scrub_to" || name == "pulp_motion_play" ||
                    name == "pulp_motion_pause" || name == "pulp_motion_enable_cost" ||
                    name == "pulp_motion_disable_cost";
                if (result.empty() && requires_selection &&
                    !valid_exact_inspector_selection(session_id, instance_id, publication_id)) {
                    result = inspector_error_payload(
                        "Error: session_id, instance_id, and publication_id "
                        "must be the exact safe identities returned by the "
                        "start tool");
                }
                if (result.empty()) {
                    auto command = run_inspector_command(root, inspector_method, inspector_params,
                                                         session_id, instance_id, publication_id);
                    if (command.succeeded() && name == "pulp_motion_start_trace") {
                        command.output +=
                            "\nExact selection: {\"session_id\":" + json_string(session_id) +
                            ",\"instance_id\":" + json_string(instance_id) +
                            ",\"publication_id\":" + json_string(publication_id) + "}";
                    }
                    result = inspector_tool_payload(std::move(command));
                }
            }
        }
        // Perfetto lifecycle is control-plane-only. No trace tool may enter
        // project discovery or the legacy Inspector publication path.
        else if (inspector_tool != nullptr && name.starts_with("pulp_trace_")) {
            std::string inspector_method(inspector_tool->method);
            std::string inspector_params = "{}";
            if (name == "pulp_trace_start") {
                const auto categories = inspector_argument_json(*inspector_arguments, "categories");
                const auto ring_mb = inspector_argument_json(*inspector_arguments, "ring_mb");
                inspector_params = "{";
                if (!categories.empty())
                    inspector_params += "\"categories\":" + categories;
                if (!ring_mb.empty()) {
                    if (inspector_params.size() != 1)
                        inspector_params += ",";
                    inspector_params += "\"ring_mb\":" + ring_mb;
                }
                inspector_params += "}";
            }

            if (inspector_arguments->selection_fields.any()) {
                result = inspector_error_payload(
                    "Error: canonical trace lifecycle does not accept legacy "
                    "session_id, instance_id, or publication_id selectors");
            } else {
                result = inspector_tool_payload(
                    run_control_trace_command(inspector_method, inspector_params));
            }
        }
        // Pending agent-request queue — an in-process read of the
        // pulp::inspect queue core (no CLI shell-out, no audio device).
        // Kept as its own branch because it resolves its own project dir
        // and returns a structured JSON array rather than proxying the
        // inspector wire.
        else if (name == "pulp_inspect_pending_requests")
            result = handle_inspect_pending_requests(args_json);
        // Inspector parameter mutation uses a typed protocol payload. Kept
        // separate from the read-only inspector
        // tools below because those pass no arguments; this one carries id/value.
        else if (inspector_tool != nullptr && name == "pulp_inspect_set_param") {
            std::string parse_error;
            const auto parsed =
                parse_inspector_set_param_arguments(*inspector_arguments, parse_error);
            if (!parsed) {
                result = inspector_error_payload(parse_error);
            } else {
                std::string params_json =
                    std::string("{\"id\":") + std::to_string(parsed->id) +
                    ",\"value\":" + std::to_string(parsed->value) +
                    ",\"normalized\":" + (parsed->normalized ? "true" : "false") + "}";
                auto command = run_inspector_command({}, std::string(inspector_tool->method),
                                                     params_json, inspector_arguments->session_id,
                                                     inspector_arguments->instance_id,
                                                     inspector_arguments->publication_id);
                result = inspector_tool_payload(std::move(command));
            }
        } else if (inspector_tool != nullptr && name == "pulp_inspect_inject_midi") {
            std::string parse_error;
            const auto parsed = pulp_mcp::detail::parse_inspector_midi_arguments(
                inspector_arguments->arguments, parse_error);
            if (!parsed) {
                result = inspector_error_payload(parse_error);
            } else {
#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
                pulp::inspect::MidiTestInput input;
                input.kind = parsed->kind == "note_on" ? pulp::inspect::MidiTestInputKind::NoteOn
                                                       : pulp::inspect::MidiTestInputKind::NoteOff;
                input.channel = parsed->channel;
                input.note = parsed->note;
                input.velocity = parsed->velocity;
                auto command = format_inspector_command_result(pulp::inspect::inject_inspector_midi(
                    input, parsed->hold_duration,
                    {inspector_arguments->session_id, inspector_arguments->instance_id,
                     inspector_arguments->publication_id},
                    std::chrono::seconds(5)));
#else
                auto command = run_inspector_command({}, std::string(inspector_tool->method));
#endif
                result = inspector_tool_payload(std::move(command));
            }
        } else if (inspector_tool != nullptr && name == "pulp_inspect_set_transport") {
            std::string parse_error;
            const auto parsed = pulp_mcp::detail::parse_inspector_transport_arguments(
                inspector_arguments->arguments, parse_error);
            if (!parsed) {
                result = inspector_error_payload(parse_error);
            } else {
#if PULP_MCP_ENABLE_INSPECTOR_CLIENT
                auto command =
                    format_inspector_command_result(pulp::inspect::set_inspector_transport(
                        {.playing = parsed->playing,
                         .position_samples = parsed->position_samples,
                         .tempo_bpm = parsed->tempo_bpm},
                        {inspector_arguments->session_id, inspector_arguments->instance_id,
                         inspector_arguments->publication_id}));
#else
                auto command = run_inspector_command({}, std::string(inspector_tool->method));
#endif
                result = inspector_tool_payload(std::move(command));
            }
        }
        // Inspector tools authenticate and connect through the shared typed
        // client; no CLI subprocess or shell-built JSON crosses this seam.
        else if (inspector_tool != nullptr) {
            std::string inspector_method(inspector_tool->method);
            std::string inspector_params = "{}";
            if (name == "pulp_inspect_evaluate") {
                auto expr = inspector_argument_string(*inspector_arguments, "expression");
                if (!expr.empty()) {
                    auto params_json = std::string("{\"expression\":") + json_string(expr) + "}";
                    inspector_params = params_json;
                }
            }

            const auto& session_id = inspector_arguments->session_id;
            const auto& instance_id = inspector_arguments->instance_id;
            const auto& publication_id = inspector_arguments->publication_id;
            const auto selection_fields = inspector_arguments->selection_fields;
            if (selection_fields.any() &&
                (!selection_fields.all() ||
                 !valid_exact_inspector_selection(session_id, instance_id, publication_id))) {
                result =
                    inspector_error_payload("Error: session_id, instance_id, and publication_id "
                                            "must be supplied together as exact safe identities");
            } else {
                auto command = run_inspector_command({}, inspector_method, inspector_params,
                                                     session_id, instance_id, publication_id);
                result = inspector_tool_payload(std::move(command));
            }
        } else
            return json_error(id, -32601, "Unknown tool: " + name);

        return json_result(id, result);
    }

    if (method == "ping") {
        return json_result(id, "{}");
    }

    return json_error(id, -32601, "Method not found: " + method);
}

// ── Main: stdio JSON-RPC transport ───────────────────────────────────────────

int pulp_mcp::server::run(int argc, char* argv[]) {
    // Flag-only invocations short-circuit the JSON-RPC loop so the
    // release-CLI smoke gate and `pulp doctor` can probe the binary
    // without speaking MCP framing. Keep this list narrow — anything
    // that consumes stdin must fall through to the loop below.
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--version" || arg == "-V") {
            std::cout << "pulp-mcp " << PULP_MCP_SERVER_VERSION << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "pulp-mcp " << PULP_MCP_SERVER_VERSION << "\n"
                      << "MCP (Model Context Protocol) server for Pulp.\n"
                      << "Speaks JSON-RPC 2.0 over stdin/stdout — normally\n"
                      << "invoked by .mcp.json using the PULP_MCP_BINARY path.\n"
                      << "\n"
                      << "Flags:\n"
                      << "  --version, -V   Print version and exit\n"
                      << "  --help, -h      Show this help\n";
            return 0;
        }
        std::cerr << "pulp-mcp: unknown flag '" << arg << "'. Try --help.\n";
        return 2;
    }

    // MCP spec: messages on the stdio transport are delimited by
    // newlines and MUST NOT contain embedded `\n` or `\r`. The
    // newline-stripping contract lives inside `handle_request` itself,
    // so callers here just pass through.

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;

        // MCP uses Content-Length header framing
        if (line.find("Content-Length:") == 0) {
            int length = std::stoi(line.substr(15));
            std::getline(std::cin, line); // empty line
            std::string body(length, '\0');
            std::cin.read(body.data(), length);

            auto response = handle_request(body);
            if (!response.empty()) {
                std::cout << "Content-Length: " << response.size() << "\r\n\r\n" << response;
                std::cout.flush();
            }
            continue;
        }

        // Also handle bare JSON (for simpler testing AND the
        // newline-delimited MCP stdio transport that Claude Code uses).
        auto response = handle_request(line);
        if (!response.empty()) {
            std::cout << response << "\n";
            std::cout.flush();
        }
    }

    return 0;
}
