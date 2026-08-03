#pragma once

#include <string_view>

namespace pulp::timeline {

/// Validates a complete byte sequence as UTF-8 without throwing.
///
/// @param value Bytes to validate.
/// @return Whether the complete sequence is valid UTF-8.
bool is_valid_utf8(std::string_view value) noexcept;

} // namespace pulp::timeline
