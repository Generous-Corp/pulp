#include <pulp/platform/open_url.hpp>

#include "dialog_internal.hpp"

#include <cctype>
#include <mutex>
#include <utility>

namespace pulp::platform {

namespace {

std::mutex g_opener_mutex;
std::function<bool(const std::string&)> g_opener;

} // namespace

const char* to_string(OpenUrlResult result) noexcept {
    switch (result) {
    case OpenUrlResult::opened:
        return "opened";
    case OpenUrlResult::refused_scheme:
        return "refused_scheme";
    case OpenUrlResult::failed:
        return "failed";
    }
    return "unknown";
}

bool open_url_scheme_allowed(std::string_view url) noexcept {
    const auto colon = url.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    std::string scheme;
    for (char c : url.substr(0, colon))
        scheme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (scheme == "mailto")
        return url.size() > colon + 1;
    if (scheme != "http" && scheme != "https")
        return false;
    // A web URL needs an authority: "https://host".
    const auto rest = url.substr(colon + 1);
    return rest.size() > 2 && rest[0] == '/' && rest[1] == '/' && rest[2] != '/';
}

OpenUrlResult open_url(std::string_view url) {
    if (!open_url_scheme_allowed(url))
        return OpenUrlResult::refused_scheme;
    std::function<bool(const std::string&)> opener;
    {
        std::lock_guard<std::mutex> lock(g_opener_mutex);
        opener = g_opener;
    }
    const std::string text(url);
    const bool ok = opener ? opener(text) : detail::native_open_url(text);
    return ok ? OpenUrlResult::opened : OpenUrlResult::failed;
}

void set_url_opener(std::function<bool(const std::string&)> opener) {
    std::lock_guard<std::mutex> lock(g_opener_mutex);
    g_opener = std::move(opener);
}

} // namespace pulp::platform
