// mcp_tools_internal.cpp — Shared implementation details for MCP tool handlers.

#include "mcp_tools_internal.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace pulp_mcp {

std::string trim_copy(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string strip_quotes_copy(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

std::string normalize_pref(std::string value) {
    value = strip_quotes_copy(trim_copy(value));
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool valid_import_design_mode(const std::string& value) {
    return value == "live" || value == "baked";
}

bool valid_import_design_emit(const std::string& value) {
    return value == "js" || value == "ir-json" || value == "cpp";
}

fs::path source_build_cli_path(const fs::path& root) {
    const auto build_dir = root / "build";
    const auto cli_dir = build_dir / "tools" / "cli";
    std::vector<fs::path> candidates = {
        cli_dir / "pulp-cpp",
        cli_dir / "pulp-cpp.exe",
    };

    for (const char* config : {"Release", "RelWithDebInfo", "Debug", "MinSizeRel"}) {
        candidates.push_back(cli_dir / config / "pulp-cpp");
        candidates.push_back(cli_dir / config / "pulp-cpp.exe");
    }

    candidates.push_back(build_dir / "pulp");
    candidates.push_back(build_dir / "pulp.exe");

    candidates.push_back(cli_dir / "pulp");
    candidates.push_back(cli_dir / "pulp.exe");
    for (const char* config : {"Release", "RelWithDebInfo", "Debug", "MinSizeRel"}) {
        candidates.push_back(cli_dir / config / "pulp");
        candidates.push_back(cli_dir / config / "pulp.exe");
    }

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate))
            return candidate;
    }
    return build_dir / "pulp";
}

std::string random_temp_suffix() {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve(32);
    for (int word = 0; word < 4; ++word) {
        std::uint32_t value = rd();
        for (int shift = 28; shift >= 0; shift -= 4)
            out.push_back(hex[(value >> shift) & 0x0f]);
    }
    return out;
}

ProbeJsonTemp make_private_probe_json_temp(std::string& error) {
    const auto base = fs::temp_directory_path();
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto dir = base / ("pulp-mcp-audio-probe-" + random_temp_suffix());
#if defined(_WIN32)
        std::error_code ec;
        if (fs::create_directory(dir, ec)) {
            fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
            return {dir, dir / "probe.json"};
        }
#else
        if (::mkdir(dir.c_str(), S_IRWXU) == 0)
            return {dir, dir / "probe.json"};
        if (errno != EEXIST) {
            error = "failed to create private temp directory for audio probe JSON";
            return {};
        }
#endif
    }
    error = "failed to create private temp directory for audio probe JSON";
    return {};
}

std::string read_text_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool normalize_structured_json(const std::string& text, std::string& normalized,
                               std::string& error) {
    try {
        auto parsed = choc::json::parse(text);
        if (!parsed.isObject() && !parsed.isArray()) {
            error = "audio probe JSON root must be an object or array";
            return false;
        }
        normalized = choc::json::toString(parsed, false);
        return true;
    } catch (const std::exception& e) {
        error = std::string("failed to parse audio probe JSON: ") + e.what();
        return false;
    } catch (...) {
        error = "failed to parse audio probe JSON";
        return false;
    }
}

fs::path pulp_home_path() {
    if (const char* home = std::getenv("PULP_HOME"); home && *home)
        return fs::path(home);
#ifdef _WIN32
    if (const char* home = std::getenv("USERPROFILE"); home && *home)
        return fs::path(home) / ".pulp";
#else
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".pulp";
#endif
    return {};
}

std::string read_config_value(const std::string& section, const std::string& key) {
    const auto home = pulp_home_path();
    if (home.empty())
        return {};
    std::ifstream f(home / "config.toml");
    if (!f.is_open())
        return {};
    std::string line;
    std::string current_section;
    while (std::getline(f, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos)
            line = line.substr(0, comment);
        const auto trimmed = trim_copy(line);
        if (trimmed.empty())
            continue;
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = trim_copy(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }
        if (current_section != section)
            continue;
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos)
            continue;
        if (trim_copy(trimmed.substr(0, eq)) != key)
            continue;
        return strip_quotes_copy(trim_copy(trimmed.substr(eq + 1)));
    }
    return {};
}

std::string import_design_defaults_line() {
    std::string mode = "live";
    std::string emit = "js";
    std::string mode_source = "built-in";
    std::string emit_source = "built-in";

    auto apply_emit = [&](std::string raw, std::string source) -> bool {
        raw = normalize_pref(std::move(raw));
        if (!valid_import_design_emit(raw)) {
            return false;
        }
        emit = std::move(raw);
        emit_source = std::move(source);
        return true;
    };
    auto apply_mode = [&](std::string raw, std::string source) -> bool {
        raw = normalize_pref(std::move(raw));
        if (!valid_import_design_mode(raw)) {
            return false;
        }
        mode = std::move(raw);
        mode_source = std::move(source);
        return true;
    };

    if (const char* env = std::getenv("PULP_IMPORT_DESIGN_DEFAULT_EMIT"); env && *env) {
        if (!apply_emit(env, "env:PULP_IMPORT_DESIGN_DEFAULT_EMIT")) {
            return "Import design defaults: invalid (import_design.default_emit must be one of: "
                   "js, ir-json, cpp from env:PULP_IMPORT_DESIGN_DEFAULT_EMIT)\n";
        }
    } else if (auto configured = read_config_value("import_design", "default_emit");
               !configured.empty()) {
        if (!apply_emit(configured, "config:import_design.default_emit")) {
            return "Import design defaults: invalid (import_design.default_emit must be one of: "
                   "js, ir-json, cpp from config:import_design.default_emit)\n";
        }
    }
    if (const char* env = std::getenv("PULP_IMPORT_DESIGN_DEFAULT_MODE"); env && *env) {
        if (!apply_mode(env, "env:PULP_IMPORT_DESIGN_DEFAULT_MODE")) {
            return "Import design defaults: invalid (import_design.default_mode must be one of: "
                   "live, baked from env:PULP_IMPORT_DESIGN_DEFAULT_MODE)\n";
        }
    } else if (auto configured = read_config_value("import_design", "default_mode");
               !configured.empty()) {
        if (!apply_mode(configured, "config:import_design.default_mode")) {
            return "Import design defaults: invalid (import_design.default_mode must be one of: "
                   "live, baked from config:import_design.default_mode)\n";
        }
    }

    if (emit_source == "built-in" && mode == "baked") {
        emit = "ir-json";
        emit_source = "implied by " + mode_source;
    }
    if (mode_source == "built-in" && (emit == "ir-json" || emit == "cpp")) {
        mode = "baked";
        mode_source = "implied by " + emit_source;
    }

    return "Import design defaults: --mode " + mode + " (" + mode_source + "), --emit " + emit +
           " (" + emit_source + ")\n";
}

} // namespace pulp_mcp
