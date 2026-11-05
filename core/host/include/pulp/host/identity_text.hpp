#pragma once

#include <string>
#include <string_view>

namespace pulp::host {

/// Normalize one authored identity segment to NFC UTF-8 and reject malformed,
/// NUL-containing, or control-character input.  This narrow value-only helper
/// avoids pulling graph node types into identity consumers.
bool normalize_authored_identity_text(std::string_view input, std::string& output);

} // namespace pulp::host
