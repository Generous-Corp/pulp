#include <pulp/state/properties_file.hpp>
#include <choc/text/choc_JSON.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cctype>

namespace pulp::state {

namespace {

bool only_trailing_space(const std::string& text, std::size_t pos) {
    while (pos < text.size()) {
        if (!std::isspace(static_cast<unsigned char>(text[pos]))) return false;
        ++pos;
    }
    return true;
}

}  // namespace

// ── PropertiesFile ──────────────────────────────────────────────────────

bool PropertiesFile::load(std::string_view path) {
    path_ = std::string(path);

    std::ifstream file(path_);
    if (!file) return false;

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    if (content.empty()) {
        values_.clear();  // Clear any stale state when loading empty file
        return true;
    }

    try {
        auto json = choc::json::parse(content);

        if (!json.isObject()) return false;

        values_.clear();
        for (uint32_t i = 0; i < json.size(); ++i) {
            auto member = json.getObjectMemberAt(i);
            std::string key(member.name);

            if (member.value.isString())
                values_[key] = std::string(member.value.getString());
            else if (member.value.isBool())
                values_[key] = member.value.getBool() ? "true" : "false";
            else if (member.value.isInt32())
                values_[key] = std::to_string(member.value.getInt32());
            else if (member.value.isInt64())
                values_[key] = std::to_string(member.value.getInt64());
            else if (member.value.isFloat64())
                values_[key] = std::to_string(member.value.getFloat64());
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool PropertiesFile::save(std::string_view path) {
    path_ = std::string(path);
    return save_to(path_);
}

bool PropertiesFile::save() const {
    if (path_.empty()) return false;
    return save_to(path_);
}

bool PropertiesFile::save_to(const std::string& dest) const {
    auto parent = std::filesystem::path(dest).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    auto obj = choc::value::createObject("settings");
    for (auto& [key, value] : values_)
        obj.addMember(key, value);
    std::string json = choc::json::toString(obj, true);
    std::ofstream file(dest);
    if (!file) return false;
    file << json;
    return file.good();
}

std::optional<std::string> PropertiesFile::get_string(std::string_view key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

void PropertiesFile::set_string(std::string_view key, std::string_view value) {
    values_[std::string(key)] = std::string(value);
}

std::optional<int64_t> PropertiesFile::get_int(std::string_view key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    try {
        std::size_t consumed = 0;
        auto value = std::stoll(it->second, &consumed);
        if (!only_trailing_space(it->second, consumed)) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

void PropertiesFile::set_int(std::string_view key, int64_t value) {
    values_[std::string(key)] = std::to_string(value);
}

std::optional<double> PropertiesFile::get_double(std::string_view key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    try {
        std::size_t consumed = 0;
        auto value = std::stod(it->second, &consumed);
        if (!only_trailing_space(it->second, consumed)) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

void PropertiesFile::set_double(std::string_view key, double value) {
    values_[std::string(key)] = std::to_string(value);
}

std::optional<bool> PropertiesFile::get_bool(std::string_view key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second == "true" || it->second == "1";
}

void PropertiesFile::set_bool(std::string_view key, bool value) {
    values_[std::string(key)] = value ? "true" : "false";
}

void PropertiesFile::remove(std::string_view key) {
    auto it = values_.find(key);
    if (it != values_.end()) values_.erase(it);
}

bool PropertiesFile::contains(std::string_view key) const {
    return values_.find(key) != values_.end();
}

void PropertiesFile::clear() {
    values_.clear();
}

std::vector<std::string> PropertiesFile::keys() const {
    std::vector<std::string> result;
    result.reserve(values_.size());
    for (auto& [k, v] : values_)
        result.push_back(k);
    return result;
}

// ── ApplicationProperties ───────────────────────────────────────────────

ApplicationProperties::ApplicationProperties(std::string_view app_name)
    : app_name_(app_name) {}

void ApplicationProperties::load() {
    auto user_dir = user_settings_dir(app_name_);
    auto common_dir = common_settings_dir(app_name_);

    user_.load(user_dir + "/settings.json");
    common_.load(common_dir + "/settings.json");
}

void ApplicationProperties::save() {
    auto user_dir = user_settings_dir(app_name_);
    auto common_dir = common_settings_dir(app_name_);

    user_.save(user_dir + "/settings.json");
    common_.save(common_dir + "/settings.json");
}

std::string ApplicationProperties::user_settings_dir(std::string_view app_name) {
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + "/Library/Preferences/" + std::string(app_name);
    return "/tmp/" + std::string(app_name);
#elif defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata) return std::string(appdata) + "\\" + std::string(app_name);
    return "C:\\Users\\Public\\" + std::string(app_name);
#else
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + "/.config/" + std::string(app_name);
    return "/tmp/" + std::string(app_name);
#endif
}

std::string ApplicationProperties::common_settings_dir(std::string_view app_name) {
#ifdef __APPLE__
    return "/Library/Application Support/" + std::string(app_name);
#elif defined(_WIN32)
    const char* progdata = std::getenv("ProgramData");
    if (progdata) return std::string(progdata) + "\\" + std::string(app_name);
    return "C:\\ProgramData\\" + std::string(app_name);
#else
    return "/etc/" + std::string(app_name);
#endif
}

}  // namespace pulp::state
