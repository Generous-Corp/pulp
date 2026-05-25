#pragma once

// Runtime convenience wrapper for child processes.

#include <pulp/platform/child_process.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace pulp::runtime {

using ProcessResult = pulp::platform::ProcessResult;

/// Launch a child process and wait for it to complete.
/// Returns nullopt if the process could not be started.
std::optional<ProcessResult> run_process(
    std::string_view executable,
    const std::vector<std::string>& args = {},
    std::string_view working_dir = "",
    int timeout_ms = 0  // 0 = no timeout
);

}  // namespace pulp::runtime
