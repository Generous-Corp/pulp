#pragma once

#include <pulp/inspect/client_session.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::cli::inspect_detail {

std::string trim(const std::string& value);
std::string color_bold();
std::string color_cyan();
std::string color_green();
std::string color_red();
std::string color_reset();

void print_json_error(std::string_view code,
                      std::string_view message,
                      std::string_view data_json = "{}");
void print_cli_error(bool json,
                     std::string_view code,
                     std::string_view message);
bool require_arg_value(const std::vector<std::string>& args,
                       std::size_t& index,
                       const char* flag,
                       std::string& output,
                       bool json);
bool parse_port(std::string_view text, int& output);
bool parse_parameter_id(std::string_view text, std::int64_t& output);
bool parse_nonnegative_int64(std::string_view text, std::int64_t& output);
bool parse_parameter_value(std::string_view text, double& output);
void print_error(const inspect::InspectorMessage& response, bool json);
void print_failure(const inspect::InspectorClientFailure& failure, bool json);
choc::value::Value discovery_json(
    const inspect::InspectorDiscoveryRecord& record);
std::string profiles_json();
std::optional<choc::value::Value> parse_json_object(
    std::string_view payload,
    std::string_view method,
    bool json);
void print_capability_list(
    const std::vector<inspect::InspectorCapability>& capabilities,
    std::string_view label);
std::string attach_publication_id(std::string response_json,
                                  std::string_view publication_id);

} // namespace pulp::cli::inspect_detail
