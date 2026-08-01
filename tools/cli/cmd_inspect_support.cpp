#include "cmd_inspect_support.hpp"

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <system_error>

namespace pulp::cli::inspect_detail {

using namespace pulp::inspect;

std::string trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}
static bool g_inspect_color = true;
std::string color_bold()  { return g_inspect_color ? "\033[1m"  : ""; }
std::string color_cyan()  { return g_inspect_color ? "\033[36m" : ""; }
std::string color_green() { return g_inspect_color ? "\033[32m" : ""; }
std::string color_red()   { return g_inspect_color ? "\033[31m" : ""; }
std::string color_reset() { return g_inspect_color ? "\033[0m"  : ""; }
void print_json_error(std::string_view code,
                      std::string_view message,
                      std::string_view data_json) {
    auto error = choc::value::createObject("");
    error.addMember("code", choc::value::createString(code));
    error.addMember("message", choc::value::createString(message));
    try {
        error.addMember("data", choc::json::parse(data_json));
    } catch (...) {
        auto data = choc::value::createObject("");
        data.addMember("raw", choc::value::createString(data_json));
        error.addMember("data", std::move(data));
    }
    auto envelope = choc::value::createObject("");
    envelope.addMember(
        "schemaVersion",
        choc::value::createString("pulp.inspect.error.v1"));
    envelope.addMember("error", std::move(error));
    std::cerr << choc::json::toString(envelope, false) << "\n";
}

void print_cli_error(bool json,
                     std::string_view code,
                     std::string_view message) {
    if (json) {
        print_json_error(code, message);
        return;
    }
    std::cerr << "Error";
    if (!code.empty())
        std::cerr << " [" << code << "]";
    std::cerr << ": " << message << "\n";
}

bool require_arg_value(const std::vector<std::string>& args,
                       std::size_t& index,
                       const char* flag,
                       std::string& output,
                       bool json) {
    if (index + 1 >= args.size()) {
        print_cli_error(json, "invalid_arguments",
                        std::string(flag) + " requires a value");
        return false;
    }
    output = args[++index];
    if (output.empty()) {
        print_cli_error(json, "invalid_arguments",
                        std::string(flag) + " requires a non-empty value");
        return false;
    }
    return true;
}

bool parse_port(std::string_view text, int& output) {
    int value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value <= 0 || value > 65535)
        return false;
    output = value;
    return true;
}

bool parse_parameter_id(std::string_view text, std::int64_t& output) {
    std::int64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 0 ||
        value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
        return false;
    output = value;
    return true;
}

bool parse_nonnegative_int64(std::string_view text, std::int64_t& output) {
    std::int64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 0)
        return false;
    output = value;
    return true;
}

bool parse_parameter_value(std::string_view text, double& output) {
    const std::string owned(text);
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtod(owned.c_str(), &end);
    if (errno == ERANGE || end != owned.c_str() + owned.size() ||
        end == owned.c_str() || !std::isfinite(value))
        return false;
    output = value;
    return true;
}

void print_error(const InspectorMessage& response, bool json) {
    if (json) {
        print_json_error(
            response.error_code.empty() ? "protocol_error"
                                        : response.error_code,
            response.params_json,
            response.error_data_json.empty() ? "{}"
                                             : response.error_data_json);
        return;
    }
    std::cerr << "Error";
    if (!response.error_code.empty())
        std::cerr << " [" << response.error_code << "]";
    std::cerr << ": " << response.params_json << "\n";
    if (!response.error_data_json.empty() &&
        response.error_data_json != "{}")
        std::cerr << response.error_data_json << "\n";
}

void print_failure(const InspectorClientFailure& failure, bool json) {
    if (json) {
        print_json_error(
            failure.code.empty() ? "client_failure" : failure.code,
            failure.message,
            failure.data_json.empty() ? "{}" : failure.data_json);
        return;
    }
    std::cerr << "Error";
    if (!failure.code.empty())
        std::cerr << " [" << failure.code << "]";
    std::cerr << ": " << failure.message << "\n";
    if (!failure.data_json.empty() && failure.data_json != "{}")
        std::cerr << failure.data_json << "\n";
}

choc::value::Value discovery_json(const InspectorDiscoveryRecord& record) {
    auto value = choc::value::createObject("");
    value.addMember("sessionId", choc::value::createString(record.session_id));
    value.addMember("instanceId", choc::value::createString(record.instance_id));
    value.addMember("publicationId",
                    choc::value::createString(record.publication_id));
    value.addMember("pluginId", choc::value::createString(record.plugin_id));
    value.addMember("endpoint", choc::value::createString(record.endpoint));
    value.addMember("profile",
                    choc::value::createString(profile_id(record.profile)));
    value.addMember("processId", record.process_id);
    value.addMember("expiresAtUnixMs", record.expires_at_unix_ms);
    return value;
}

std::string profiles_json() {
    auto result = choc::value::createObject("");
    result.addMember("schemaVersion",
                     choc::value::createString("pulp.inspect.profiles.v1"));
    constexpr std::uint32_t kProfileCount = 4;
    auto profiles = choc::value::createArray(
        kProfileCount, [&] (std::uint32_t index) {
            const auto profile = static_cast<InspectorProfile>(index);
            auto value = choc::value::createObject("");
            value.addMember(
                "id", choc::value::createString(profile_id(profile)));
            auto capabilities = choc::value::createArray(
                static_cast<std::uint32_t>(
                    profile_capabilities(profile).size()),
                [profile] (std::uint32_t capability_index) {
                    return choc::value::createString(capability_id(
                        profile_capabilities(profile)[capability_index]));
                });
            value.addMember("capabilities", std::move(capabilities));
            return value;
        });
    result.addMember("profiles", std::move(profiles));
    return choc::json::toString(result, false);
}

std::optional<choc::value::Value> parse_json_object(
    std::string_view payload,
    std::string_view method,
    bool json) {
    try {
        auto value = choc::json::parse(payload);
        if (value.isObject())
            return value;
    } catch (...) {
    }
    print_cli_error(json, "invalid_response",
                    std::string(method) +
                        " returned a non-object JSON response");
    return std::nullopt;
}

void print_capability_list(
    const std::vector<InspectorCapability>& capabilities,
    std::string_view label) {
    std::cout << "  " << label << ":";
    if (capabilities.empty()) {
        std::cout << " none\n";
        return;
    }
    std::cout << "\n";
    for (const auto capability : capabilities)
        std::cout << "    " << capability_id(capability) << "\n";
}

std::string attach_publication_id(
    std::string response_json,
    std::string_view publication_id) {
    try {
        auto value = choc::json::parse(response_json);
        if (!value.isObject())
            return response_json;
        value.addMember(
            "publicationId",
            choc::value::createString(publication_id));
        return choc::json::toString(value, false);
    } catch (...) {
        return response_json;
    }
}

} // namespace pulp::cli::inspect_detail
