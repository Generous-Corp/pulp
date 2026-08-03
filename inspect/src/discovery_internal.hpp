#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect::discovery_detail {

inline constexpr std::uintmax_t kMaxDiscoveryRecordBytes = 1024;

std::int64_t unix_ms_now();
bool safe_component(std::string_view value);
bool valid_loopback_endpoint(std::string_view endpoint);
std::string discovery_file_stem(std::string_view session_id,
                                std::string_view instance_id);
std::optional<std::string> process_start_identity(std::int64_t process_id);

} // namespace pulp::inspect::discovery_detail
