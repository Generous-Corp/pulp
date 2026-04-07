#include <pulp/runtime/i18n.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace pulp::runtime {

// ── LocalisedStrings ────────────────────────────────────────────────────────

bool LocalisedStrings::load_strings_file(std::string_view path) {
    std::ifstream file(std::string(path));
    if (!file.is_open()) return false;

    std::string line;
    while (std::getline(file, line)) {
        // Apple .strings format: "key" = "value";
        auto kstart = line.find('"');
        if (kstart == std::string::npos) continue;
        auto kend = line.find('"', kstart + 1);
        if (kend == std::string::npos) continue;

        auto vstart = line.find('"', kend + 1);
        if (vstart == std::string::npos) continue;
        auto vend = line.find('"', vstart + 1);
        if (vend == std::string::npos) continue;

        strings_[line.substr(kstart + 1, kend - kstart - 1)] =
            line.substr(vstart + 1, vend - vstart - 1);
    }
    return true;
}

bool LocalisedStrings::load_po_file(std::string_view path) {
    std::ifstream file(std::string(path));
    if (!file.is_open()) return false;

    std::string line, msgid, msgstr;
    bool in_msgid = false, in_msgstr = false;

    auto commit = [&]() {
        if (!msgid.empty() && !msgstr.empty())
            strings_[msgid] = msgstr;
        msgid.clear();
        msgstr.clear();
    };

    while (std::getline(file, line)) {
        if (line.starts_with("msgid \"")) {
            commit();
            in_msgid = true; in_msgstr = false;
            msgid = line.substr(7, line.size() - 8);
        } else if (line.starts_with("msgstr \"")) {
            in_msgid = false; in_msgstr = true;
            msgstr = line.substr(8, line.size() - 9);
        } else if (line.starts_with("\"") && line.ends_with("\"")) {
            auto content = line.substr(1, line.size() - 2);
            if (in_msgid) msgid += content;
            else if (in_msgstr) msgstr += content;
        }
    }
    commit();
    return true;
}

bool LocalisedStrings::load_json_file(std::string_view path) {
    std::ifstream file(std::string(path));
    if (!file.is_open()) return false;

    // Minimal JSON object parser for flat {"key":"value"} objects
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    size_t pos = 0;
    auto skip_ws = [&]() {
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
               content[pos] == '\r' || content[pos] == '\t'))
            ++pos;
    };
    auto read_string = [&]() -> std::string {
        if (pos >= content.size() || content[pos] != '"') return {};
        ++pos;
        std::string result;
        while (pos < content.size() && content[pos] != '"') {
            if (content[pos] == '\\' && pos + 1 < content.size()) {
                ++pos;
                if (content[pos] == 'n') result += '\n';
                else if (content[pos] == 't') result += '\t';
                else result += content[pos];
            } else {
                result += content[pos];
            }
            ++pos;
        }
        if (pos < content.size()) ++pos;  // skip closing quote
        return result;
    };

    skip_ws();
    if (pos >= content.size() || content[pos] != '{') return false;
    ++pos;

    while (pos < content.size()) {
        skip_ws();
        if (content[pos] == '}') break;
        if (content[pos] == ',') { ++pos; continue; }

        auto key = read_string();
        skip_ws();
        if (pos < content.size() && content[pos] == ':') ++pos;
        skip_ws();
        auto value = read_string();

        if (!key.empty()) strings_[std::move(key)] = std::move(value);
    }

    return true;
}

void LocalisedStrings::add(std::string_view key, std::string_view value) {
    strings_[std::string(key)] = std::string(value);
}

std::string LocalisedStrings::translate(std::string_view key) const {
    auto it = strings_.find(key);
    return it != strings_.end() ? it->second : std::string(key);
}

std::string LocalisedStrings::translate(std::string_view key,
                                         const std::vector<std::string>& args) const {
    std::string result = translate(key);

    // Replace {0}, {1}, {2}, ... with arguments
    for (size_t i = 0; i < args.size(); ++i) {
        std::string placeholder = "{" + std::to_string(i) + "}";
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.size(), args[i]);
            pos += args[i].size();
        }
    }

    return result;
}

bool LocalisedStrings::has(std::string_view key) const {
    return strings_.find(key) != strings_.end();
}

LocalisedStrings& LocalisedStrings::instance() {
    static LocalisedStrings s_instance;
    return s_instance;
}

std::string LocalisedStrings::system_locale() {
    // Platform-specific locale detection
#if defined(__APPLE__)
    // Use CFLocale on Apple; simplified to env var fallback
    if (const char* lang = std::getenv("LANG")) {
        std::string locale(lang);
        auto dot = locale.find('.');
        if (dot != std::string::npos) locale = locale.substr(0, dot);
        auto underscore = locale.find('_');
        if (underscore != std::string::npos) locale = locale.substr(0, underscore);
        return locale;
    }
#elif defined(_WIN32)
    // Simplified: use GetUserDefaultLocaleName in a real implementation
    if (const char* lang = std::getenv("LANG")) return std::string(lang);
#else
    if (const char* lang = std::getenv("LANG")) {
        std::string locale(lang);
        auto dot = locale.find('.');
        if (dot != std::string::npos) locale = locale.substr(0, dot);
        auto underscore = locale.find('_');
        if (underscore != std::string::npos) locale = locale.substr(0, underscore);
        return locale;
    }
#endif
    return "en";
}

}  // namespace pulp::runtime
