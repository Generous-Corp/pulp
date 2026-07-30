// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace pulp::import_design::browser_capture::detail {

/// Escape one string value for the backend's small diagnostic JSON writers.
std::string json_escape(std::string_view value);

}  // namespace pulp::import_design::browser_capture::detail
