#pragma once

/// @file open_url.hpp
/// Open a web or mail link in the user's default application.

#include <functional>
#include <string>
#include <string_view>

namespace pulp::platform {

enum class OpenUrlResult {
    /// Handed to the default browser or mail client.
    opened,
    /// Not an http, https or mailto URL; nothing was opened. Local files and
    /// script URLs are refused so editor content cannot launch them.
    refused_scheme,
    /// The platform call failed, or this platform has no way to open URLs.
    failed,
};

/// Short stable name ("opened", "refused_scheme", "failed").
const char* to_string(OpenUrlResult result) noexcept;

/// Whether `url`'s scheme is one open_url accepts (http, https, mailto; case
/// insensitive).
bool open_url_scheme_allowed(std::string_view url) noexcept;

/// Open `url` (macOS: NSWorkspace). Callable from the main thread.
OpenUrlResult open_url(std::string_view url);

/// Replaces the platform opener (for tests). It receives URLs that passed the
/// scheme check and returns whether it opened them. Empty restores the
/// platform opener.
void set_url_opener(std::function<bool(const std::string& url)> opener);

} // namespace pulp::platform
