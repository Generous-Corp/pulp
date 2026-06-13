// mcp_tools.cpp — MCP tool-call handlers for pulp-mcp.
//
// Extracted from tools/mcp/pulp_mcp.cpp in the 2026-05 Phase 6 (B4)
// refactor. See mcp_tools.hpp for the dispatch surface. Each handler
// shells out to the `pulp` CLI (build/test/validate) or queries the
// bundled audio services (model store, excerpt service), then wraps
// the result via json_tool_payload.

#include "mcp_tools.hpp"
#include "mcp_json.hpp"
#include "mcp_shell.hpp"

#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/service.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace pulp_mcp {

namespace {

std::string trim_copy(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string strip_quotes_copy(const std::string& s) {
    if (s.size() >= 2
        && ((s.front() == '"' && s.back() == '"')
            || (s.front() == '\'' && s.back() == '\'')))
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
        if (fs::exists(candidate)) return candidate;
    }
    return build_dir / "pulp";
}

struct ProbeJsonTemp {
    fs::path directory;
    fs::path json_path;
};

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
            fs::permissions(dir, fs::perms::owner_all,
                            fs::perm_options::replace, ec);
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
    if (!file.is_open()) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool normalize_structured_json(const std::string& text,
                               std::string& normalized,
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
    if (home.empty()) return {};
    std::ifstream f(home / "config.toml");
    if (!f.is_open()) return {};
    std::string line;
    std::string current_section;
    while (std::getline(f, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        const auto trimmed = trim_copy(line);
        if (trimmed.empty()) continue;
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = trim_copy(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }
        if (current_section != section) continue;
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        if (trim_copy(trimmed.substr(0, eq)) != key) continue;
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
            return "Import design defaults: invalid (import_design.default_emit must be one of: js, ir-json, cpp from env:PULP_IMPORT_DESIGN_DEFAULT_EMIT)\n";
        }
    } else if (auto configured = read_config_value("import_design", "default_emit");
               !configured.empty()) {
        if (!apply_emit(configured, "config:import_design.default_emit")) {
            return "Import design defaults: invalid (import_design.default_emit must be one of: js, ir-json, cpp from config:import_design.default_emit)\n";
        }
    }
    if (const char* env = std::getenv("PULP_IMPORT_DESIGN_DEFAULT_MODE"); env && *env) {
        if (!apply_mode(env, "env:PULP_IMPORT_DESIGN_DEFAULT_MODE")) {
            return "Import design defaults: invalid (import_design.default_mode must be one of: live, baked from env:PULP_IMPORT_DESIGN_DEFAULT_MODE)\n";
        }
    } else if (auto configured = read_config_value("import_design", "default_mode");
               !configured.empty()) {
        if (!apply_mode(configured, "config:import_design.default_mode")) {
            return "Import design defaults: invalid (import_design.default_mode must be one of: live, baked from config:import_design.default_mode)\n";
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

    return "Import design defaults: --mode " + mode + " (" + mode_source
        + "), --emit " + emit + " (" + emit_source + ")\n";
}

} // namespace

std::string handle_build(const std::string& /*params_json*/) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto build_dir = root / "build";
    std::string output;

    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        output += exec("cmake -B " + shell_quote(build_dir.string()) +
                       " -S " + shell_quote(root.string()) + " 2>&1");
    }
    output += exec("cmake --build " + shell_quote(build_dir.string()) + " 2>&1");

    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_test(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto build_dir = root / "build";
    std::string cmd = "ctest --test-dir " + shell_quote(build_dir.string()) + " --output-on-failure";

    auto filter = extract_string(params_json, "filter");
    if (!filter.empty()) cmd += " -R " + shell_quote(filter);

    auto output = exec(cmd + " 2>&1");
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_status(const std::string& /*params_json*/) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::ostringstream out;
    out << "Pulp Project: " << root.string() << "\n";

    auto branch = exec("git -C " + shell_quote(root.string()) + " branch --show-current 2>/dev/null");
    if (!branch.empty()) {
        while (!branch.empty() && branch.back() == '\n') branch.pop_back();
        out << "Branch: " << branch << "\n";
    }

    auto build_dir = root / "build";
    out << "Build: " << (fs::exists(build_dir / "CMakeCache.txt") ? "configured" : "not configured") << "\n";
    out << import_design_defaults_line();

    int src = 0, hdr = 0, tests = 0;
    for (auto& e : fs::recursive_directory_iterator(root / "core")) {
        auto ext = e.path().extension().string();
        if (ext == ".cpp" || ext == ".mm") ++src;
        if (ext == ".hpp" || ext == ".h") ++hdr;
    }
    if (fs::exists(root / "test")) {
        for (auto& e : fs::directory_iterator(root / "test")) {
            if (e.path().extension() == ".cpp") ++tests;
        }
    }
    out << "Sources: " << src << " impl, " << hdr << " headers, " << tests << " test files\n";

    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(out.str()) + "}]}";
}

std::string handle_validate(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = shell_quote((root / "build" / "tools" / "cli" / "pulp").string()) +
        " validate --json";
    if (extract_bool(params_json, "all", false)) {
        cmd += " --all";
    }
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_audio_model_status(const std::string& /*params_json*/) {
    auto status = pulp::tools::audio::query_model_status();
    return json_tool_payload(pulp::tools::audio::to_json(status));
}

std::string handle_audio_model_list(const std::string& /*params_json*/) {
    auto result = pulp::tools::audio::list_models();
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

std::string handle_audio_model_activate(const std::string& params_json) {
    auto model_id = extract_string(params_json, "model_id");
    if (model_id.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: model_id is required\"}]}";
    }

    auto result = pulp::tools::audio::activate_model(model_id);
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

std::string handle_audio_read_bundle(const std::string& params_json) {
    auto bundle_path = extract_string(params_json, "bundle_path");
    if (bundle_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: bundle_path is required\"}]}";
    }

    auto bundle = pulp::tools::audio::read_excerpt_bundle(bundle_path);
    return json_tool_payload(pulp::tools::audio::to_json(bundle));
}


std::string handle_audio_excerpt_find(const std::string& params_json) {
    auto text = extract_string(params_json, "text");
    auto input_path = extract_string(params_json, "input_path");
    if (text.empty() || input_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: text and input_path are required\"}]}";
    }

    pulp::tools::audio::ExcerptFindRequest request;
    request.text = text;
    request.input_path = input_path;
    request.model_id = extract_string(params_json, "model_id");
    request.recursive = extract_bool(params_json, "recursive", false);
    request.top_k = static_cast<std::size_t>(extract_int(params_json, "top", 5));
    request.window_ms = static_cast<uint64_t>(extract_int(params_json, "window_ms", 1500));
    request.hop_ms = static_cast<uint64_t>(extract_int(params_json, "hop_ms", 250));
    request.min_score = extract_double(params_json, "min_score", 0.0);
    request.max_candidates_per_file =
        static_cast<std::size_t>(extract_int(params_json, "max_candidates_per_file", 3));
    request.bundle_out = extract_string(params_json, "bundle_out");

    auto result = pulp::tools::audio::run_excerpt_find(request);
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

std::string handle_audio_probe_json(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
    }

    int frames = 90;
    auto frames_raw = extract_raw(params_json, "frames");
    if (!frames_raw.empty() && frames_raw != "null") {
        frames = extract_int(params_json, "frames", -1);
        if (frames <= 0) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: frames must be a positive integer\"}]}";
        }
    }

    auto target = extract_string(params_json, "target");
    if (!target.empty() && target.front() == '-') {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: target must be a standalone target name, not an option\"}]}";
    }
    std::string temp_error;
    auto temp = make_private_probe_json_temp(temp_error);
    if (temp.json_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string("Error: " + temp_error) + "}]}";
    }
    auto output_path = temp.json_path;

    std::string cmd = shell_quote(source_build_cli_path(root).string()) + " run";
    if (!target.empty()) cmd += " " + shell_quote(target);
    cmd += " --audio-probe-json " + shell_quote(output_path.string());
    cmd += " --frames " + std::to_string(frames);
    cmd += " 2>&1";

    auto output = exec(cmd);
    auto probe_json = read_text_file(output_path);
    std::error_code remove_ec;
    fs::remove_all(temp.directory, remove_ec);

    if (probe_json.empty()) {
        std::string message = "Error: pulp run did not write audio probe JSON";
        if (!output.empty()) message += "\n" + output;
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) + "}]}";
    }
    std::string normalized_json;
    std::string parse_error;
    if (!normalize_structured_json(probe_json, normalized_json, parse_error)) {
        std::string message = "Error: " + parse_error + "\n" + probe_json;
        if (!output.empty()) message += "\n" + output;
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) + "}]}";
    }

    return json_tool_payload(normalized_json);
}

std::string handle_audio_scope(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
    }

    int frames = 90;
    auto frames_raw = extract_raw(params_json, "frames");
    if (!frames_raw.empty() && frames_raw != "null") {
        frames = extract_int(params_json, "frames", -1);
        if (frames <= 0) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: frames must be a positive integer\"}]}";
        }
    }

    int window = 2048;
    auto window_raw = extract_raw(params_json, "window");
    if (!window_raw.empty() && window_raw != "null") {
        window = extract_int(params_json, "window", -1);
        if (window <= 0) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: window must be a positive integer\"}]}";
        }
    }

    int channel = 0;
    auto channel_raw = extract_raw(params_json, "channel");
    if (!channel_raw.empty() && channel_raw != "null") {
        channel = extract_int(params_json, "channel", -1);
        if (channel < 0) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: channel must be a non-negative integer\"}]}";
        }
    }

    auto trigger = extract_string(params_json, "trigger");
    if (trigger.empty()) trigger = "rising-zero";
    auto normalized_trigger = trigger;
    std::replace(normalized_trigger.begin(), normalized_trigger.end(), '_', '-');
    if (normalized_trigger != "none" && normalized_trigger != "off"
        && normalized_trigger != "raw" && normalized_trigger != "rising-zero") {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: trigger must be one of none, raw, off, rising-zero\"}]}";
    }

    auto target = extract_string(params_json, "target");
    if (!target.empty() && target.front() == '-') {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: target must be a standalone target name, not an option\"}]}";
    }

    auto input_wav = extract_string(params_json, "input_wav");
    auto png_path = extract_string(params_json, "png_path");
    if (!input_wav.empty() && !target.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: target and input_wav are mutually exclusive\"}]}";
    }
    if (!png_path.empty() && input_wav.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: png_path is only supported with input_wav\"}]}";
    }

    std::string temp_error;
    auto temp = make_private_probe_json_temp(temp_error);
    if (temp.json_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string("Error: " + temp_error) + "}]}";
    }
    auto output_path = temp.directory / "scope.json";

    std::string cmd = shell_quote(source_build_cli_path(root).string()) + " audio scope";
    if (!target.empty()) cmd += " " + shell_quote(target);
    if (!input_wav.empty()) cmd += " --input-wav " + shell_quote(input_wav);
    cmd += " --json " + shell_quote(output_path.string());
    cmd += " --frames " + std::to_string(frames);
    cmd += " --window " + std::to_string(window);
    cmd += " --trigger " + shell_quote(trigger);
    cmd += " --channel " + std::to_string(channel);
    if (!png_path.empty()) cmd += " --png " + shell_quote(png_path);
    cmd += " 2>&1";

    auto output = exec(cmd);
    auto scope_json = read_text_file(output_path);
    std::error_code remove_ec;
    fs::remove_all(temp.directory, remove_ec);

    if (scope_json.empty()) {
        std::string message = "Error: pulp audio scope did not write scope JSON";
        if (!output.empty()) message += "\n" + output;
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) + "}]}";
    }

    std::string normalized_json;
    std::string parse_error;
    if (!normalize_structured_json(scope_json, normalized_json, parse_error)) {
        std::string message = "Error: " + parse_error + "\n" + scope_json;
        if (!output.empty()) message += "\n" + output;
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) + "}]}";
    }

    return json_tool_payload(normalized_json);
}

}  // namespace pulp_mcp
