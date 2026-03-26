// Linux clipboard stub
// Full implementation would use XCB/X11 selections or wl_clipboard for Wayland
// For headless/CI use: store in-process only

#include <pulp/platform/clipboard.hpp>

#include <string>
#include <vector>
#include <optional>
#include <map>
#include <mutex>

namespace pulp::platform {

namespace {
    std::mutex g_mutex;
    std::string g_text;
    bool g_has_text = false;
    std::map<std::string, std::vector<uint8_t>> g_data;
}

bool Clipboard::set_text(const std::string& text) {
    std::lock_guard lock(g_mutex);
    g_text = text;
    g_has_text = true;
    return true;
}

std::optional<std::string> Clipboard::get_text() {
    std::lock_guard lock(g_mutex);
    if (!g_has_text) return std::nullopt;
    return g_text;
}

bool Clipboard::has_text() {
    std::lock_guard lock(g_mutex);
    return g_has_text;
}

bool Clipboard::set_data(const std::string& type, const std::vector<uint8_t>& data) {
    std::lock_guard lock(g_mutex);
    g_data[type] = data;
    return true;
}

std::optional<std::vector<uint8_t>> Clipboard::get_data(const std::string& type) {
    std::lock_guard lock(g_mutex);
    auto it = g_data.find(type);
    if (it == g_data.end()) return std::nullopt;
    return it->second;
}

} // namespace pulp::platform
