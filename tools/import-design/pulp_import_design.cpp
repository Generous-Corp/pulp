#include <pulp/view/design_import.hpp>
#include <pulp/view/design_export.hpp>
#include <pulp/view/design_fidelity_ledger.hpp>
#include <pulp/view/recognition_resolver.hpp>
#include <pulp/view/widget_skin_derive.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/inspector.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/state/store.hpp>
#include "import_detect.hpp"
#include "fig_lane.hpp"
#include "envelope_merge.hpp"
#include "figma_url.hpp"
#include "render_artifact_path.hpp"
#include "sprite_skins.hpp"
#include <miniz.h>
// getpid() is POSIX-only via <unistd.h>; MSVC ships an equivalent
// `_getpid` declaration in <process.h>. Wrap both to keep the
// `pid_kind` lookup portable.
#if defined(_WIN32)
#  include <process.h>
#  define pulp_getpid _getpid
#else
#  include <unistd.h>
#  define pulp_getpid getpid
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <cstring>
#include <filesystem>
#include <functional>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::view;
using namespace pulp::state;

namespace {

enum class ArtifactEmit {
    js,
    ir_json,
    cpp,
    swiftui
};

enum class RuntimeMode {
    live,
    baked
};

enum class SnapshotSemantics {
    fail,
    warn,
    accept
};

const char* artifact_emit_name(ArtifactEmit emit) {
    switch (emit) {
        case ArtifactEmit::js:      return "js";
        case ArtifactEmit::ir_json: return "ir-json";
        case ArtifactEmit::cpp:     return "cpp";
        case ArtifactEmit::swiftui: return "swiftui";
    }
    return "js";
}

const char* runtime_mode_name(RuntimeMode mode) {
    switch (mode) {
        case RuntimeMode::live:  return "live";
        case RuntimeMode::baked: return "baked";
    }
    return "live";
}

std::string trim_copy(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool looks_like_serialized_design_ir(const std::string& content) {
    const auto trimmed = trim_copy(content);
    return !trimmed.empty()
        && trimmed.front() == '{'
        && trimmed.find("\"version\"") != std::string::npos
        && trimmed.find("\"root\"") != std::string::npos;
}

// Detect a Figma-plugin export envelope (the `.pulp.json` the Pulp Figma plugin
// and the headless REST exporter emit), so `--from figma` doesn't silently feed
// it to parse_figma_json — which reads none of its structure and produces an
// empty root-only import. Keyed on the envelope's stable identity fields:
// format_version `...-figma-plugin-v1` or provenance.adapter "figma-plugin".
bool looks_like_figma_plugin_export(const std::string& content) {
    const auto trimmed = trim_copy(content);
    if (trimmed.empty() || trimmed.front() != '{') return false;
    return trimmed.find("figma-plugin-v1") != std::string::npos
        || trimmed.find("\"adapter\": \"figma-plugin\"") != std::string::npos
        || trimmed.find("\"adapter\":\"figma-plugin\"") != std::string::npos;
}

std::string strip_quotes_copy(const std::string& s) {
    if (s.size() >= 2
        && ((s.front() == '"' && s.back() == '"')
            || (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

std::string normalize_pref_value(std::string value) {
    value = strip_quotes_copy(trim_copy(value));
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::optional<ArtifactEmit> parse_artifact_emit_pref(const std::string& raw) {
    const auto value = normalize_pref_value(raw);
    if (value == "js") return ArtifactEmit::js;
    if (value == "ir-json") return ArtifactEmit::ir_json;
    if (value == "cpp") return ArtifactEmit::cpp;
    if (value == "swiftui") return ArtifactEmit::swiftui;
    return std::nullopt;
}

std::optional<RuntimeMode> parse_runtime_mode_pref(const std::string& raw) {
    const auto value = normalize_pref_value(raw);
    if (value == "live") return RuntimeMode::live;
    if (value == "baked") return RuntimeMode::baked;
    return std::nullopt;
}

std::optional<bool> parse_knob_style_pref(const std::string& raw) {
    const auto value = normalize_pref_value(raw);
    if (value == "sprite") return false;
    if (value == "silver" || value == "default" || value == "standard" || value == "auto")
        return true;
    return std::nullopt;
}

std::optional<bool> parse_skin_style_pref(const std::string& raw) {
    const auto value = normalize_pref_value(raw);
    if (value == "default" || value == "plain") return false;
    if (value == "skin" || value == "skinned") return true;
    return std::nullopt;
}

std::optional<std::string> consume_value_option(int& i, int argc, char* argv[], std::string_view name) {
    const std::string_view arg(argv[i]);
    if (arg == name) return i + 1 < argc ? std::optional<std::string>{argv[++i]} : std::nullopt;
    if (arg.size() <= name.size() || arg.compare(0, name.size(), name) != 0 || arg[name.size()] != '=')
        return std::nullopt;
    return std::string(arg.substr(name.size() + 1));
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool is_native_widget_node(const IRNode& node) {
    if (node.audio_widget != AudioWidgetType::none) return true;
    const auto type = lower_copy(node.type);
    return type == "button" || type == "text_button" || type == "toggle_button" ||
           type == "input" || type == "slider" || type == "range" ||
           type == "knob" || type == "fader" || type == "meter" ||
           type == "xy_pad" || type == "xypad" || type == "waveform" ||
           type == "spectrum" || type == "textarea" || type == "text_editor" ||
           type == "checkbox" || type == "canvas" || type == "image" ||
           type == "img" || type == "path" || type == "svg_path" ||
           type == "rect" || type == "svg_rect" || type == "line" ||
           type == "svg_line";
}

struct ElementCounts {
    size_t nodes = 0;
    size_t text = 0;
    size_t containers = 0;
    size_t widgets = 0;
};

ElementCounts count_design_ir_elements(const IRNode& root) {
    ElementCounts counts;
    std::function<void(const IRNode&)> visit = [&](const IRNode& node) {
        counts.nodes++;
        const auto type = lower_copy(node.type);
        if (is_native_widget_node(node)) counts.widgets++;
        else if (type == "text" || type == "label" || type == "span" || type == "p") counts.text++;
        else if (!node.children.empty() || type == "frame" || type == "view" ||
                 type == "div" || type == "section") counts.containers++;
        for (const auto& child : node.children) visit(child);
    };
    visit(root);
    return counts;
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

std::string read_import_design_config_value(const std::string& section,
                                            const std::string& key) {
    const auto home = pulp_home_path();
    if (home.empty()) return {};
    const auto path = home / "config.toml";
    if (!fs::exists(path)) return {};

    std::ifstream f(path);
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

struct DefaultSelection {
    ArtifactEmit emit = ArtifactEmit::js;
    RuntimeMode mode = RuntimeMode::live;
    std::string emit_source = "built-in";
    std::string mode_source = "built-in";
    std::string error;
};

DefaultSelection resolve_import_design_defaults(ArtifactEmit cli_emit,
                                                RuntimeMode cli_mode,
                                                bool emit_explicit,
                                                bool mode_explicit) {
    DefaultSelection out;
    out.emit = cli_emit;
    out.mode = cli_mode;
    if (emit_explicit) out.emit_source = "cli";
    if (mode_explicit) out.mode_source = "cli";

    auto apply_emit = [&](const std::string& raw, const std::string& source) -> bool {
        auto parsed = parse_artifact_emit_pref(raw);
        if (!parsed) {
            out.error = "invalid import-design default emit '" + raw + "' from " + source
                + " (expected js, ir-json, or cpp)";
            return false;
        }
        out.emit = *parsed;
        out.emit_source = source;
        return true;
    };
    auto apply_mode = [&](const std::string& raw, const std::string& source) -> bool {
        auto parsed = parse_runtime_mode_pref(raw);
        if (!parsed) {
            out.error = "invalid import-design default mode '" + raw + "' from " + source
                + " (expected live or baked)";
            return false;
        }
        out.mode = *parsed;
        out.mode_source = source;
        return true;
    };

    if (!emit_explicit) {
        if (const char* env = std::getenv("PULP_IMPORT_DESIGN_DEFAULT_EMIT"); env && *env) {
            if (!apply_emit(env, "env:PULP_IMPORT_DESIGN_DEFAULT_EMIT")) return out;
        } else if (auto configured = read_import_design_config_value("import_design", "default_emit");
                   !configured.empty()) {
            if (!apply_emit(configured, "config:import_design.default_emit")) return out;
        }
    }

    if (!mode_explicit) {
        if (const char* env = std::getenv("PULP_IMPORT_DESIGN_DEFAULT_MODE"); env && *env) {
            if (!apply_mode(env, "env:PULP_IMPORT_DESIGN_DEFAULT_MODE")) return out;
        } else if (auto configured = read_import_design_config_value("import_design", "default_mode");
                   !configured.empty()) {
            if (!apply_mode(configured, "config:import_design.default_mode")) return out;
        }
    }

    if (!emit_explicit && out.emit_source == "built-in"
        && !mode_explicit && out.mode == RuntimeMode::baked) {
        out.emit = ArtifactEmit::ir_json;
        out.emit_source = "implied by " + out.mode_source;
    }
    if (!mode_explicit && out.mode_source == "built-in"
        && !emit_explicit
        && (out.emit == ArtifactEmit::ir_json || out.emit == ArtifactEmit::cpp
            || out.emit == ArtifactEmit::swiftui)) {
        out.mode = RuntimeMode::baked;
        out.mode_source = "implied by " + out.emit_source;
    }

    return out;
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string prefix) {
        auto base = fs::temp_directory_path();
        std::random_device rd;
        std::mt19937_64 rng(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();

        for (int attempt = 0; attempt < 100; ++attempt) {
            std::ostringstream name;
            name << prefix << "-" << std::hex << tick << "-" << dist(rng);
            auto candidate = base / name.str();
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                path_ = std::move(candidate);
                active_ = true;
                return;
            }
            if (ec && !fs::exists(candidate)) {
                throw std::runtime_error("failed to create temporary directory: " + ec.message());
            }
        }
        throw std::runtime_error("failed to allocate a unique temporary directory");
    }

    ~ScopedTempDir() {
        if (active_ && !path_.empty()) {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    bool active_ = false;
};

bool has_disallowed_url_char(const std::string& url) {
    for (unsigned char c : url) {
        if (c <= 0x20 || c == 0x7f) return true;
        switch (c) {
            case '\'':
            case '"':
            case '`':
            case '<':
            case '>':
            case '|':
            case '\\':
                return true;
            default:
                break;
        }
    }
    return false;
}

bool has_disallowed_file_char(const std::string& value) {
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

bool has_url_shell_metachar(const std::string& value) {
    for (unsigned char c : value) {
        if (c <= 0x20 || c == 0x7f) return true;
        switch (c) {
            case '\'':
            case '"':
            case '`':
            case ';':
            case '|':
            case '<':
            case '>':
            case '$':
            case '\\':
            case '(':
            case ')':
            case '*':
            case '[':
            case ']':
            case '{':
            case '}':
            case '!':
                return true;
            default:
                break;
        }
    }
    return false;
}

bool is_supported_http_url(const std::string& url) {
    return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

bool fetch_url_to_file(const std::string& url, const fs::path& output_path) {
    if (!is_supported_http_url(url)) {
        std::cerr << "Error: --url must start with http:// or https://\n";
        return false;
    }
    // A figma.com scene URL can never import through the unauthenticated fetch
    // below, so fail fast naming the lanes that work rather than surfacing a
    // bare curl exit code. Rule + message live in figma_url.hpp.
    if (pulp::import_design::is_figma_app_url(url)) {
        std::cerr << pulp::import_design::figma_app_url_error();
        return false;
    }
    if (has_disallowed_url_char(url)) {
        std::cerr << "Error: --url contains characters that are not accepted by the import fetcher\n";
        return false;
    }

    auto curl = pulp::platform::find_on_path("curl");
    if (!curl) {
        std::cerr << "Error: curl not found on PATH; pass --file <path> or install curl\n";
        return false;
    }

    pulp::platform::ProcessOptions opts;
    opts.timeout_ms = 30000;
    opts.max_output_bytes = 64 * 1024;
    auto result = pulp::platform::ChildProcess::run(
        curl->string(),
        {"-fsSL", "--max-time", "30", "--output", output_path.string(), url},
        opts);
    if (result.timed_out) {
        std::cerr << "Error: timed out fetching URL: " << url << "\n";
        return false;
    }
    if (result.exit_code != 0) {
        std::cerr << "Error: failed to fetch URL: " << url << "\n";
        if (!result.stderr_output.empty()) std::cerr << result.stderr_output;
        else if (!result.stdout_output.empty()) std::cerr << result.stdout_output;
        return false;
    }
    return true;
}

const char* diagnostic_severity_name(ImportDiagnosticSeverity severity) {
    switch (severity) {
        case ImportDiagnosticSeverity::info: return "info";
        case ImportDiagnosticSeverity::warning: return "warning";
        case ImportDiagnosticSeverity::error: return "error";
    }
    return "warning";
}

bool parse_positive_int_arg(const char* flag, const std::string& value, int& out) {
    try {
        size_t parsed_len = 0;
        const long parsed = std::stol(value, &parsed_len, 10);
        if (parsed_len != value.size() || parsed <= 0
            || parsed > std::numeric_limits<int>::max()) {
            std::cerr << "Error: " << flag << " requires a positive integer value\n";
            return false;
        }
        out = static_cast<int>(parsed);
        return true;
    } catch (...) {
        std::cerr << "Error: " << flag << " requires a positive integer value\n";
        return false;
    }
}

bool parse_asset_hash_arg(const std::string& value,
                          std::unordered_map<std::string, std::string>& expected_hash_by_uri) {
    const auto sep = value.rfind('=');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= value.size()) {
        std::cerr << "Error: --asset-hash requires <uri=sha256-hex>\n";
        return false;
    }
    auto uri = value.substr(0, sep);
    auto hash = value.substr(sep + 1);
    constexpr std::string_view prefix = "sha256:";
    if (hash.rfind(prefix, 0) == 0)
        hash = hash.substr(prefix.size());
    expected_hash_by_uri[std::move(uri)] = std::move(hash);
    return true;
}

const char* snapshot_semantics_name(SnapshotSemantics semantics) {
    switch (semantics) {
        case SnapshotSemantics::fail:   return "fail";
        case SnapshotSemantics::warn:   return "warn";
        case SnapshotSemantics::accept: return "accept";
    }
    return "fail";
}

std::string join_tokens(const std::vector<std::string>& tokens) {
    std::ostringstream out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) out << ", ";
        out << tokens[i];
    }
    return out.str();
}

std::string current_utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

ImportDiagnostic make_cli_diagnostic(ImportDiagnosticSeverity severity,
                                     ImportDiagnosticKind kind,
                                     std::string code,
                                     std::string path,
                                     std::string message) {
    ImportDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.kind = kind;
    diagnostic.code = std::move(code);
    diagnostic.path = std::move(path);
    diagnostic.message = std::move(message);
    return diagnostic;
}

void print_asset_manifest_diagnostics(const IRAssetManifest& manifest) {
    for (const auto& asset : manifest.assets) {
        for (const auto& diagnostic : asset.diagnostics) {
            std::cerr << "[" << diagnostic_severity_name(diagnostic.severity)
                      << "] " << diagnostic.code << " at "
                      << (diagnostic.path.empty() ? asset.original_uri : diagnostic.path)
                      << ": " << diagnostic.message << "\n";
        }
    }
}

void print_import_diagnostics(const std::vector<ImportDiagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == ImportDiagnosticSeverity::info) continue;
        std::cerr << "[" << diagnostic_severity_name(diagnostic.severity)
                  << "] " << diagnostic.code << " at "
                  << (diagnostic.path.empty() ? "<root>" : diagnostic.path)
                  << ": " << diagnostic.message << "\n";
    }
}

void print_designmd_diagnostics(const std::vector<DesignMdDiagnostic>& diagnostics) {
    for (const auto& d : diagnostics) {
        const char* sev = (d.severity == DesignMdSeverity::error)   ? "error" :
                          (d.severity == DesignMdSeverity::warning) ? "warning" : "info";
        std::cerr << "[" << sev << "] " << d.code
                  << " at " << (d.path.empty() ? "<root>" : d.path);
        if (d.line > 0) std::cerr << " (line " << d.line << ":" << d.column << ")";
        std::cerr << ": " << d.message << "\n";
    }
}

bool has_blocking_asset_diagnostic(const IRAssetManifest& manifest) {
    for (const auto& asset : manifest.assets) {
        for (const auto& diagnostic : asset.diagnostics) {
            if (diagnostic.severity == ImportDiagnosticSeverity::error
                || diagnostic.code == "asset-network-fetch-disabled") {
                return true;
            }
        }
    }
    return false;
}

DesignIrAssetOptions make_asset_options(const std::string& input_file,
                                        const std::string& input_url,
                                        bool allow_network_fetch,
                                        int asset_timeout_ms,
                                        const std::string& asset_cache_dir,
                                        const std::unordered_map<std::string, std::string>& expected_asset_hashes) {
    DesignIrAssetOptions asset_options;
    asset_options.allow_network_fetch = allow_network_fetch;
    asset_options.network_timeout_ms = asset_timeout_ms;
    if (!asset_cache_dir.empty()) asset_options.cache_directory = asset_cache_dir;
    if (!input_url.empty()) asset_options.base_url = input_url;
    if (!input_file.empty()) {
        std::error_code ec;
        auto input_path = fs::weakly_canonical(fs::path(input_file), ec);
        if (ec) input_path = fs::absolute(fs::path(input_file), ec);
        asset_options.base_directory = ec ? fs::path(input_file).parent_path()
                                          : input_path.parent_path();
    }
    asset_options.expected_hash_by_uri = expected_asset_hashes;
    return asset_options;
}

struct CppOutputPaths {
    fs::path source;
    fs::path header;
    fs::path binding_manifest;
    std::string include_name;
};

CppOutputPaths resolve_cpp_output_paths(const std::string& output_file) {
    fs::path requested(output_file.empty() ? "imported_ui.cpp" : output_file);
    std::error_code ec;
    const bool existing_dir = fs::is_directory(requested, ec);
    const auto ext = requested.extension().string();
    CppOutputPaths paths;
    if (existing_dir || ext.empty()) {
        paths.source = requested / "imported_ui.cpp";
        paths.header = requested / "imported_ui.hpp";
    } else if (ext == ".hpp" || ext == ".hh" || ext == ".h") {
        paths.header = requested;
        paths.source = requested;
        paths.source.replace_extension(".cpp");
    } else {
        paths.source = requested;
        paths.header = requested;
        paths.header.replace_extension(".hpp");
    }
    paths.binding_manifest = paths.source;
    paths.binding_manifest.replace_extension(".bindings.json");
    paths.include_name = paths.header.filename().string();
    return paths;
}

// Tailwind formats re-parse DESIGN.md for section context, so they are gated to
// `--from designmd` (generalizing them to any source is Workstream A2). Callers
// must reject these before reaching the theme-only exporter below.
bool is_tailwind_format(const std::string& format) {
    return format == "tailwind" || format == "json-tailwind" ||
           format == "css-tailwind";
}

// Resolve a token-export body for a theme-based `--format` value. W3C DTCG is
// the default; `css-variables` emits CSS custom properties (base → :root,
// `.dark`-suffixed → @media prefers-color-scheme). Only `w3c`/`css-variables`
// reach here — Tailwind formats are dispatched (designmd) or rejected upstream
// via is_tailwind_format(), so this never silently downgrades Tailwind to W3C.
std::string export_theme_tokens(const std::string& format,
                                const pulp::view::Theme& theme) {
    if (format == "css-variables")
        return pulp::view::export_css_variables(theme);
    return pulp::view::export_w3c_tokens(theme);
}

struct SwiftOutputPaths {
    fs::path view;             // <RootView>.swift
    fs::path theme;            // <RootView>Theme.swift (sibling)
    fs::path binding_manifest; // <RootView>.bindings.json
    std::string root_view_name;
    std::string theme_type_name;  // <RootView>Theme
};

// Mirror resolve_cpp_output_paths for `--emit swiftui`. A directory or
// extensionless --output yields ImportedPulpView.swift inside it; a .swift
// file path is the view itself, with <RootView>Theme.swift as a sibling and the
// binding manifest beside the view.
SwiftOutputPaths resolve_swift_output_paths(const std::string& output_file) {
    fs::path requested(output_file.empty() ? "ImportedPulpView.swift" : output_file);
    std::error_code ec;
    const bool existing_dir = fs::is_directory(requested, ec);
    const auto ext = requested.extension().string();
    SwiftOutputPaths paths;
    if (existing_dir || ext.empty()) {
        paths.view = requested / "ImportedPulpView.swift";
    } else {
        paths.view = requested;
    }
    paths.root_view_name = paths.view.stem().string();
    if (paths.root_view_name.empty()) paths.root_view_name = "ImportedPulpView";
    // Theme artifact + type are derived per-view (`<RootView>Theme`) so two
    // SwiftUI imports never clobber a shared PulpTheme.swift on disk nor emit a
    // duplicate `enum PulpTheme` / dynamic-color symbol when compiled into one
    // Swift target. The theme file (<RootView>Theme.swift) is
    // always distinct from the view (<RootView>.swift), so no path collision.
    paths.theme_type_name = paths.root_view_name + "Theme";
    paths.theme = paths.view.parent_path() / (paths.root_view_name + "Theme.swift");
    paths.binding_manifest = paths.view;
    paths.binding_manifest.replace_extension(".bindings.json");
    return paths;
}

} // namespace

static void print_usage() {
    std::cout << "pulp import-design — Import designs from external tools into Pulp\n\n";
    std::cout << "Usage:\n";
    std::cout << "  pulp import-design --from <source> [options]\n\n";
    std::cout << "Sources:\n";
    std::cout << "  figma, figma-plugin  Figma JSON/normalized IR, or Pulp plugin envelope\n";
    std::cout << "  fig      Local Figma .fig save file, decoded offline (no account/network)\n";
    std::cout << "  stitch   Google Stitch screen HTML or normalized IR file\n";
    std::cout << "  v0       v0.dev TSX/Tailwind output\n";
    std::cout << "  pencil   Pencil/OpenPencil node JSON or .pen export\n";
    std::cout << "  claude   Anthropic Claude Design — manually-exported standalone HTML\n";
    std::cout << "  designmd Google DESIGN.md design-system spec (tokens only)\n";
    std::cout << "  jsx      Precompiled React JSX runtime bundle for live pass-through or baked snapshots\n\n";
    std::cout << "Options:\n";
    std::cout << "  --from <source>   Design source (required)\n";
    std::cout << "  --file <path>     Input file path. Repeatable with --from figma-plugin:\n";
    std::cout << "                    one already-exported envelope per state captures a\n";
    std::cout << "                    multi-state design into one view. Order sets the frame\n";
    std::cout << "                    index a \"swap <n>\" button targets (the first --file is\n";
    std::cout << "                    frame 0).\n";
    std::cout << "  --url <url>       URL that serves design JSON/HTML directly (e.g. a v0 share\n";
    std::cout << "                    link). Fetched unauthenticated; a figma.com file URL does\n";
    std::cout << "                    NOT work — see 'Importing from Figma' below.\n";
    std::cout << "  --frame <name>    Frame/artboard to import (Figma; guid or name for --from fig).\n";
    std::cout << "                    Repeatable: give it once per state to capture a multi-state\n";
    std::cout << "                    design into one view. Order sets the frame index a \"swap <n>\"\n";
    std::cout << "                    button targets (the first --frame is frame 0).\n";
    std::cout << "  --page <name>     Restrict frame lookup to one page (--from fig)\n";
    std::cout << "  --outline         List pages/frames of a .fig file and exit (--from fig)\n";
    std::cout << "  --json            With --outline, emit the inventory as JSON\n";
    std::cout << "  --screen <name>   Screen to import (Stitch)\n";
    std::cout << "  --output <path>   Destination file for the primary artifact (default: ui.js)\n";
    std::cout << "  --emit {js|ir-json|cpp|swiftui}\n";
    std::cout << "                    Primary artifact kind (built-in default: js). cpp and\n";
    std::cout << "                    swiftui are baked-only; swiftui emits native SwiftUI\n";
    std::cout << "                    (a View + PulpTheme.swift + binding manifest)\n";
    std::cout << "  --mode {live|baked}\n";
    std::cout << "                    Runtime model (built-in default: live; baked emits IR or C++ artifacts)\n";
    std::cout << "  --snapshot-semantics {fail|warn|accept}\n";
    std::cout << "                    JSX baked snapshot policy (default: fail)\n";
    std::cout << "  --allow-network-fetch\n";
    std::cout << "                    Allow DesignIR asset-manifest HTTP fetches at import time\n";
    std::cout << "  --asset-cache <path>\n";
    std::cout << "                    Asset cache directory (default: PULP_IMPORT_ASSET_CACHE or user cache)\n";
    std::cout << "  --asset-timeout-ms <ms>\n";
    std::cout << "                    Per-request asset fetch timeout (default: 30000)\n";
    std::cout << "  --asset-hash <uri=sha256>\n";
    std::cout << "                    Expected asset content hash; may be repeated\n";
    std::cout << "  --tokens <path>   Output token file (default: tokens.json; theme.css for css-variables)\n";
    std::cout << "  --format {w3c|css-variables|tailwind|json-tailwind|css-tailwind}\n";
    std::cout << "                    Token export format (default: w3c). css-variables emits CSS\n";
    std::cout << "                    custom properties (.dark modes → @media prefers-color-scheme);\n";
    std::cout << "                    tailwind variants currently require --from designmd\n";
    std::cout << "  --dry-run         Show generated code without writing files\n";
    std::cout << "  --no-tokens       Skip token extraction\n";
    std::cout << "  --no-comments     Omit comments from generated code\n";
    std::cout << "  --web-compat      Use DOM API instead of native Pulp API\n";
    std::cout << "  --validate        Render generated JS and validate layout\n";
    std::cout << "  --screenshot-backend {skia|coregraphics}\n";
    std::cout << "                    Render backend for --validate (default: skia). Only the\n";
    std::cout << "                    Skia backend composites file-backed images; coregraphics\n";
    std::cout << "                    draws an image's filename placeholder (not faithful).\n";
    std::cout << "  --knob-style {silver|sprite|auto|standard|default}\n";
    std::cout << "  --fader-style {skin|skinned|default|plain}\n";
    std::cout << "  --meter-style {skin|skinned|default|plain}\n";
    std::cout << "  --strict-fidelity Fail (exit 4) if the import-time fidelity self-check\n";
    std::cout << "                    finds a skewed / unverifiable sprite (always warns)\n";
    std::cout << "  --fidelity-report <file>  Write the run's fidelity findings as a JSON ledger\n";
    std::cout << "                    (named taxonomy + per-kind counts) — a diffable contract\n";
    std::cout << "  --dump-layout <file>  Write the laid-out view tree as JSON (implies\n";
    std::cout << "                    --validate): per view its anchor, source node id, and\n";
    std::cout << "                    absolute bounds in design px. Feed it to\n";
    std::cout << "                    tools/import-design/layout_parity.py alongside the\n";
    std::cout << "                    source's own solved rects (--from fig writes those to\n";
    std::cout << "                    geometry.json) to diff placement per node.\n";
    std::cout << "  --reference <png> Compare render against a reference screenshot\n";
    std::cout << "  --diff <png>      Save visual diff image\n";
    std::cout << "  --import-report <path>  Write the per-control resolution report (JSON) — rung,\n";
    std::cout << "                    confidence, conflicts, verification — for review or a CI gate\n";
    std::cout << "  --fail-on-unresolved    Exit nonzero (2) when a control is conflicted or inert\n";
    std::cout << "  --fail-below <pct>      Exit nonzero (5) when --reference similarity is below\n";
    std::cout << "                    <pct>, given as a percentage 0-100 (e.g. 85, not 0.85).\n";
    std::cout << "                    Without this flag the similarity is advisory and the exit\n";
    std::cout << "                    code is unchanged, at any similarity.\n";
    std::cout << "  --recognition-manifest <path>\n";
    std::cout << "                    User recognition manifest (flat library-manifest shape) mapping\n";
    std::cout << "                    your OWN Figma component-set keys / name prefixes to Pulp control\n";
    std::cout << "                    kinds; merged OVER the built-in Pulp Figma Library so the importer\n";
    std::cout << "                    wires controls on third-party designs. figma / figma-plugin only.\n";
    std::cout << "  --param-binding-manifest <path>\n";
    std::cout << "                    JSON object mapping a Figma node id to a host-param key\n";
    std::cout << "                    (e.g. {\"10:42\": \"filter.cutoff\"}). Binds DESCRIPTIVELY-named\n";
    std::cout << "                    geometry controls (a knob layer named \"Cutoff\", not a\n";
    std::cout << "                    param: sigil) by their stamped source_node_id. A layer-name\n";
    std::cout << "                    sigil still wins; the manifest never overwrites one.\n";
    std::cout << "  --render-size WxH Render dimensions (default: the design's canvas size)\n";
    std::cout << "  --bridge-output <path>  Path to write bridge handler scaffold (default: bridge_handlers.cpp,\n";
    std::cout << "                          only emitted for --from claude)\n";
    std::cout << "  --no-bridge-scaffold    Skip bridge handler scaffold (claude only)\n";
    std::cout << "  --classnames <path>     Output classname → style map (default: classnames.json,\n";
    std::cout << "                          only emitted for Claude static classname extraction)\n";
    std::cout << "  --emit classnames       Legacy sidecar: force-emit classnames.json (claude)\n";
    std::cout << "  --no-emit-classnames    Skip classname emission (claude only)\n";
    std::cout << "  --shortcuts <path>      Output keyboard-shortcut manifest (default: shortcuts.json)\n";
    std::cout << "  --no-import-shortcuts   Skip keyboard shortcut auto-import (default: import)\n";
    std::cout << "  --no-default-shortcuts  Skip platform-convention defaults (Settings=Cmd+,, etc.) (default: enabled)\n";
    std::cout << "  --execute-bundle  Run the bundled React app in a headless JS engine and\n";
    std::cout << "                    walk the materialized DOM (--from claude only).\n";
    std::cout << "                    Falls back to the static parser on any harness failure.\n";
    std::cout << "  --export-tokens   Export a Pulp theme (from --file theme JSON, or the built-in\n";
    std::cout << "                    dark theme when no input) in the --format token format.\n";
    std::cout << "  --detect-only     Detect (source, format-version, parser-version) for\n";
    std::cout << "                    --file or --directory <path> against compat.json without\n";
    std::cout << "                    parsing. Prints match counts and confidence.\n";
    std::cout << "  --directory <p>   Path to a directory export (alternative to --file).\n";
    std::cout << "  --compat <path>   compat.json override (default: discover from cwd / repo root).\n";
    std::cout << "  --report-new-format\n";
    std::cout << "                    Emit a fingerprint-diff JSON suitable for hand-editing\n";
    std::cout << "                    into a new compat.json[imports/<source>/detected-formats]\n";
    std::cout << "                    entry. Implies --detect-only.\n";
    std::cout << "  --help            Show this help\n\n";
    std::cout << "Preferences:\n";
    std::cout << "  Built-in default is --mode live --emit js (live runtime import).\n";
    std::cout << "  Persistent defaults: pulp config set import_design.default_mode live|baked\n";
    std::cout << "                       pulp config set import_design.default_emit js|ir-json|cpp\n";
    std::cout << "  Environment overrides: PULP_IMPORT_DESIGN_DEFAULT_MODE, PULP_IMPORT_DESIGN_DEFAULT_EMIT\n";
    std::cout << "  Each CLI flag overrides its matching preference. If only default_mode=baked is set, default_emit\n";
    std::cout << "  becomes ir-json unless explicitly configured.\n\n";
    std::cout << "Importing from Figma:\n";
    std::cout << "  There is no authenticated Figma fetch in this CLI, so a figma.com file URL\n";
    std::cout << "  cannot be imported with --url. Use one of these lanes instead (local first):\n";
    std::cout << "    1. Figma desktop MCP — get_design_context/get_metadata for inspection.\n";
    std::cout << "    2. 'Design for Pulp' Figma desktop plugin — exports a .pulp.zip envelope;\n";
    std::cout << "       import it with --from figma-plugin --file <export>.pulp.zip\n";
    std::cout << "    3. --from fig --file design.fig — decodes a local .fig save file offline.\n";
    std::cout << "    4. tools/import-design/figma_rest_export.py --token <pat> — headless/CI\n";
    std::cout << "       fallback; hits the Figma REST API and emits the same envelope.\n\n";
    std::cout << "Examples:\n";
    std::cout << "  pulp import-design --from figma --file design.json\n";
    std::cout << "  pulp import-design --from figma-plugin --file design.pulp.zip --frame 'Plugin UI'\n";
    std::cout << "  pulp import-design --from stitch --file screen.html --screen 'Main'\n";
    std::cout << "  pulp import-design --from v0 --url 'https://v0.dev/t/abc123' --output my-ui.js\n";
    std::cout << "  pulp import-design --from pencil --file design.json --dry-run\n";
    std::cout << "  pulp import-design --from pencil --file design.json --validate --reference source.png\n";
    std::cout << "  pulp import-design --from claude --file design.html\n";
    std::cout << "  pulp import-design --from fig --file design.fig --outline\n";
    std::cout << "  pulp import-design --from fig --file design.fig --frame 'Main' --output ui.js\n";
    std::cout << "  pulp import-design --from figma-plugin --file typing.pulp.json --file piano.pulp.json --emit cpp --output kbd.cpp\n";
    std::cout << "  pulp import-design --from figma --file design.json --format css-variables --tokens theme.css\n";
    std::cout << "  pulp import-design --export-tokens --format css-variables   # built-in dark theme → theme.css\n";
    std::cout << "  pulp import-design --from jsx --file bundle.js --mode live --emit js --output live-ui.js\n";
    std::cout << "  pulp import-design --from jsx --file bundle.js --mode baked --emit cpp --output imported_ui.cpp\n";
    std::cout << "  pulp import-design --from figma --file design.json --mode baked --emit swiftui --output ImportedPulpView.swift\n";
}

// ── Layout dump (--dump-layout) ─────────────────────────────────────────────
//
// Serialize the laid-out view tree: per view, its anchor, the source node id it
// came from, and its absolute bounds in design px.
//
// This exists so a layout bug cannot hide. The auto-layout regression that
// motivated it — flex emitted into `style`, where nothing reads it — still
// rendered SOMETHING for every node, so every test stayed green and only a
// human squinting at a screenshot ever noticed. A source that ships its own
// solved rects (a .fig does, for auto-layout children too) can be diffed
// against these bounds by node id: no pixels, no thresholds fighting
// anti-aliasing, and a failure names a node and an exact delta.
//
// The join key the VIEW tree carries is its anchor (set_anchor_id, populated by
// the bridge's setAnchor call). The SOURCE keys its rects by its own node id.
// The IR is the only place both live side by side, so it supplies the mapping —
// which is also why this is a map lookup rather than string surgery on the
// anchor's "<adapter>:<node-id>" shape: a Figma guid contains colons of its own.

void collect_anchor_source_ids(const pulp::view::IRNode& node,
                               std::map<std::string, std::string>& out) {
    if (node.stable_anchor_id && !node.stable_anchor_id->empty() &&
        node.source_node_id && !node.source_node_id->empty()) {
        out.emplace(*node.stable_anchor_id, *node.source_node_id);
    }
    for (const auto& child : node.children) collect_anchor_source_ids(child, out);
}

void collect_layout_dump(const pulp::view::View& view,
                         const std::map<std::string, std::string>& source_ids,
                         choc::value::Value& views) {
    const auto& anchor = view.anchor_id();
    if (!anchor.empty()) {
        auto entry = choc::value::createObject("");
        entry.addMember("anchor_id", choc::value::createString(anchor));
        if (auto it = source_ids.find(anchor); it != source_ids.end())
            entry.addMember("node_id", choc::value::createString(it->second));
        entry.addMember("type",
            choc::value::createString(pulp::view::ViewInspector::type_name(view)));
        entry.addMember("visible", choc::value::createBool(view.visible()));
        // Absolute, so the numbers are in the same space as a source's own
        // frame-relative rects. A view's own bounds() are parent-relative and
        // would make every nested node look misplaced by its ancestors' offsets.
        auto abs = pulp::view::ViewInspector::absolute_bounds(view);
        entry.addMember("x", choc::value::createFloat64(abs.x));
        entry.addMember("y", choc::value::createFloat64(abs.y));
        entry.addMember("width", choc::value::createFloat64(abs.width));
        entry.addMember("height", choc::value::createFloat64(abs.height));
        views.addArrayElement(entry);
    }
    for (size_t i = 0; i < view.child_count(); ++i)
        collect_layout_dump(*view.child_at(i), source_ids, views);
}

// Bridge-handler scaffold body lives in core/view/src/design_import.cpp
// (`render_claude_bridge_scaffold`) so it can be unit-tested directly
// from the design_import test target — coverage doesn't follow CLI
// subprocess invocations, so keeping the body here would leave it
// uncovered. The CLI only calls into the library function below.

static std::string read_file(const std::string& path) {
    // Binary mode — the input may be a .pulp.zip (handled upstream by
    // extract_pulp_zip_if_present) or a JSON file with multi-byte UTF-8
    // sequences. std::ifstream default text mode is fine on POSIX but the
    // explicit ios::binary documents intent and survives any future
    // Windows port.
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "Error: cannot open file: " << path << "\n";
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ── .pulp.zip auto-unpack ───────────────────────────────────────────────
//
// The Pulp Figma plugin's "Export to Pulp" button emits a `.pulp.zip` that
// contains scene.pulp.json + assets/*.png. Asking users to manually unzip
// before running `pulp import-design --from figma-plugin` is a UX wart;
// detect a ZIP magic header (PK\x03\x04) or `.zip` extension on the input
// file, unpack it, and swap input_file for the path to scene.pulp.json
// inside.
//
// Asset paths in the envelope are relative (`assets/...`), and the rest
// of the import pipeline resolves them against
// `fs::path(input_file).parent_path()`. For real output artifacts we
// therefore extract to a durable sidecar directory next to the output
// file. Dry-run paths still use a scoped temp dir.

struct PulpZipExtraction {
    fs::path temp_dir;          // root of the extracted archive
    fs::path scene_json_path;   // resolved location of scene.pulp.json
    fs::path scene_rel_path;    // scene path relative to temp_dir/final_dir
    fs::path final_dir;         // durable sidecar target for real outputs
    fs::path backup_dir;        // previous marked sidecar during replacement
    bool cleanup_on_destroy = true;
    bool committed = false;
    bool finalized = false;

    PulpZipExtraction() = default;
    PulpZipExtraction(const PulpZipExtraction&) = delete;
    PulpZipExtraction& operator=(const PulpZipExtraction&) = delete;
    PulpZipExtraction(PulpZipExtraction&& other) noexcept {
        *this = std::move(other);
    }
    PulpZipExtraction& operator=(PulpZipExtraction&& other) noexcept {
        if (this == &other) return *this;
        cleanup_owned();
        temp_dir = std::move(other.temp_dir);
        scene_json_path = std::move(other.scene_json_path);
        scene_rel_path = std::move(other.scene_rel_path);
        final_dir = std::move(other.final_dir);
        backup_dir = std::move(other.backup_dir);
        cleanup_on_destroy = other.cleanup_on_destroy;
        committed = other.committed;
        finalized = other.finalized;

        other.cleanup_on_destroy = false;
        other.committed = false;
        other.finalized = true;
        other.temp_dir.clear();
        other.final_dir.clear();
        other.backup_dir.clear();
        return *this;
    }

    ~PulpZipExtraction() {
        cleanup_owned();
    }

    void cleanup_owned() noexcept {
        if (committed && !finalized && !final_dir.empty()) {
            std::error_code ec;
            fs::remove_all(final_dir, ec);
            if (ec) {
                std::cerr << "Warning: could not remove incomplete asset sidecar "
                          << final_dir << ": " << ec.message() << "\n";
            }
            if (!backup_dir.empty()) {
                ec.clear();
                fs::rename(backup_dir, final_dir, ec);
                if (ec) {
                    std::cerr << "Warning: could not restore previous asset sidecar "
                              << backup_dir << " → " << final_dir << ": "
                              << ec.message() << "\n";
                }
            }
            return;
        }
        if (cleanup_on_destroy && !temp_dir.empty()) {
            std::error_code ec;
            fs::remove_all(temp_dir, ec);
            if (ec) {
                std::cerr << "Warning: could not remove temporary import assets "
                          << temp_dir << ": " << ec.message() << "\n";
            }
        }
        if (finalized && !backup_dir.empty()) {
            std::error_code ec;
            fs::remove_all(backup_dir, ec);
            if (ec) {
                std::cerr << "Warning: could not remove previous asset sidecar backup "
                          << backup_dir << ": " << ec.message() << "\n";
            }
        }
    }
};

static bool looks_like_pulp_zip(const std::string& path) {
    // ZIP magic bytes (RFC: local file header signature 0x04034b50,
    // stored little-endian → "PK\x03\x04"). Cheap and authoritative —
    // catches the `.pulp.zip` case and also any `.zip` someone renamed.
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    char magic[4]{};
    f.read(magic, sizeof(magic));
    if (!f) return false;
    return magic[0] == 'P' && magic[1] == 'K' &&
           magic[2] == '\x03' && magic[3] == '\x04';
}

/// Make a unique temp directory under /tmp (or %TEMP%). Returns an empty
/// path on failure.
static fs::path make_temp_dir() {
    std::error_code ec;
    auto tmp_root = fs::temp_directory_path(ec);
    if (ec) return {};
    // Names like pulp-import-design-<pid>-<rng>.
    std::random_device rd;
    auto suffix = std::to_string(static_cast<unsigned long>(::pulp_getpid()))
                + "-"
                + std::to_string(static_cast<uint32_t>(rd()));
    auto dir = tmp_root / ("pulp-import-design-" + suffix);
    fs::create_directories(dir, ec);
    if (ec) return {};
    return dir;
}

static fs::path make_unique_dir_near(const fs::path& target) {
    auto parent = target.parent_path();
    if (parent.empty()) parent = fs::current_path();

    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) return {};

    auto leaf = target.filename().string();
    if (leaf.empty() || leaf == "." || leaf == "..")
        leaf = "pulp-import-design-assets";

    std::random_device rd;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto candidate = parent / (leaf + ".tmp-" +
                                   std::to_string(static_cast<unsigned long>(::pulp_getpid())) +
                                   "-" +
                                   std::to_string(static_cast<uint32_t>(rd())));
        fs::create_directory(candidate, ec);
        if (!ec) return candidate;
        if (!fs::exists(candidate)) return {};
        ec.clear();
    }
    return {};
}

static fs::path make_unique_sibling_path(const fs::path& target,
                                         const std::string& suffix) {
    auto parent = target.parent_path();
    if (parent.empty()) parent = fs::current_path();

    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) return {};

    auto leaf = target.filename().string();
    if (leaf.empty() || leaf == "." || leaf == "..")
        leaf = "pulp-import-design-assets";

    std::random_device rd;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto candidate = parent / (leaf + suffix + "-" +
                                   std::to_string(static_cast<unsigned long>(::pulp_getpid())) +
                                   "-" +
                                   std::to_string(static_cast<uint32_t>(rd())));
        const bool exists = fs::exists(candidate, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (!exists) return candidate;
        ec.clear();
    }
    return {};
}

static fs::path zip_asset_sidecar_dir_for_output(const std::string& output_file) {
    fs::path output(output_file.empty() ? "ui.js" : output_file);
    auto parent = output.parent_path();
    if (parent.empty()) parent = fs::current_path();

    auto leaf = output.filename().string();
    if (leaf.empty() || leaf == "." || leaf == "..") leaf = "ui.js";
    return parent / (leaf + ".assets");
}

static fs::path zip_asset_sidecar_dir_for_import_output(const std::string& output_file,
                                                        ArtifactEmit emit) {
    if (emit == ArtifactEmit::cpp)
        return zip_asset_sidecar_dir_for_output(resolve_cpp_output_paths(output_file).source.string());
    return zip_asset_sidecar_dir_for_output(output_file);
}

static constexpr const char* kZipSidecarMarker = ".pulp-import-design-sidecar-v1";

static bool write_zip_sidecar_marker(const fs::path& dir) {
    std::ofstream f(dir / kZipSidecarMarker);
    if (!f.is_open()) return false;
    f << "managed-by=pulp-import-design\n";
    f.close();
    return static_cast<bool>(f);
}

static bool is_marked_zip_sidecar(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec)
        && fs::is_regular_file(dir / kZipSidecarMarker, ec);
}

static bool commit_pulp_zip_sidecar(PulpZipExtraction& extraction) {
    if (extraction.final_dir.empty() || extraction.committed)
        return true;
    if (extraction.scene_rel_path.empty()) {
        std::cerr << "Error: cannot persist ZIP assets without a scene path\n";
        return false;
    }
    if (!write_zip_sidecar_marker(extraction.temp_dir)) {
        std::cerr << "Error: could not mark asset sidecar "
                  << extraction.temp_dir << "\n";
        return false;
    }

    std::error_code ec;
    if (fs::exists(extraction.final_dir, ec)) {
        if (!is_marked_zip_sidecar(extraction.final_dir)) {
            std::cerr << "Error: refusing to replace unmarked asset sidecar "
                      << extraction.final_dir
                      << ". Remove or rename that directory, or rerun with a different "
                         "--output path.\n";
            return false;
        }
        extraction.backup_dir = make_unique_sibling_path(extraction.final_dir, ".backup");
        if (extraction.backup_dir.empty()) {
            std::cerr << "Error: could not allocate backup path for "
                      << extraction.final_dir << "\n";
            return false;
        }
        fs::rename(extraction.final_dir, extraction.backup_dir, ec);
        if (ec) {
            std::cerr << "Error: could not backup asset sidecar "
                      << extraction.final_dir << ": " << ec.message() << "\n";
            extraction.backup_dir.clear();
            return false;
        }
    }

    fs::rename(extraction.temp_dir, extraction.final_dir, ec);
    if (ec) {
        std::cerr << "Error: could not move extracted assets to "
                  << extraction.final_dir << ": " << ec.message() << "\n";
        if (!extraction.backup_dir.empty()) {
            std::error_code restore_ec;
            fs::rename(extraction.backup_dir, extraction.final_dir, restore_ec);
            if (restore_ec) {
                std::cerr << "Error: could not restore previous asset sidecar "
                          << extraction.backup_dir << " → "
                          << extraction.final_dir << ": "
                          << restore_ec.message() << "\n";
            }
            extraction.backup_dir.clear();
        }
        return false;
    }

    extraction.temp_dir = extraction.final_dir;
    extraction.scene_json_path = extraction.final_dir / extraction.scene_rel_path;
    extraction.cleanup_on_destroy = false;
    extraction.committed = true;
    return true;
}

static void finalize_pulp_zip_sidecar(PulpZipExtraction& extraction) {
    if (extraction.final_dir.empty())
        return;
    if (!extraction.backup_dir.empty()) {
        std::error_code ec;
        fs::remove_all(extraction.backup_dir, ec);
        extraction.backup_dir.clear();
    }
    extraction.finalized = true;
    extraction.committed = false;
    extraction.cleanup_on_destroy = false;
}

struct StagedTextFile {
    fs::path final_path;
    fs::path temp_path;
    fs::path backup_path;
    bool installed = false;
};

static void cleanup_staged_text_file(StagedTextFile& staged) {
    std::error_code ec;
    if (!staged.temp_path.empty()) {
        fs::remove(staged.temp_path, ec);
        if (ec) {
            std::cerr << "Warning: could not remove staged output "
                      << staged.temp_path << ": " << ec.message() << "\n";
        }
    }
}

static void rollback_staged_text_files(std::vector<StagedTextFile>& staged) {
    for (auto it = staged.rbegin(); it != staged.rend(); ++it) {
        std::error_code ec;
        if (it->installed) {
            fs::remove(it->final_path, ec);
            if (ec) {
                std::cerr << "Warning: could not remove incomplete output "
                          << it->final_path << ": " << ec.message() << "\n";
            }
            it->installed = false;
        }
        if (!it->backup_path.empty()) {
            ec.clear();
            fs::rename(it->backup_path, it->final_path, ec);
            if (ec) {
                std::cerr << "Warning: could not restore previous output "
                          << it->backup_path << " → " << it->final_path
                          << ": " << ec.message() << "\n";
            }
            it->backup_path.clear();
        }
        cleanup_staged_text_file(*it);
    }
}

static bool stage_text_file(const std::string& path,
                            const std::string& content,
                            StagedTextFile& staged) {
    staged = {};
    staged.final_path = fs::path(path);
    if (staged.final_path.empty()) {
        std::cerr << "Error: cannot write file: empty output path\n";
        return false;
    }

    auto parent = staged.final_path.parent_path();
    if (parent.empty()) parent = fs::current_path();

    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        std::cerr << "Error: cannot create parent directory for "
                  << staged.final_path << ": " << ec.message() << "\n";
        return false;
    }

    if (fs::is_directory(staged.final_path, ec)) {
        std::cerr << "Error: cannot write file over directory: "
                  << staged.final_path << "\n";
        return false;
    }
    ec.clear();

    staged.temp_path = make_unique_sibling_path(staged.final_path, ".tmp-write");
    if (staged.temp_path.empty()) {
        std::cerr << "Error: cannot allocate staged output path for "
                  << staged.final_path << "\n";
        return false;
    }

    std::ofstream f(staged.temp_path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "Error: cannot write file: " << staged.final_path << "\n";
        cleanup_staged_text_file(staged);
        return false;
    }
    f << content;
    f.close();
    if (!f) {
        std::cerr << "Error: failed to write file completely: "
                  << staged.final_path << "\n";
        cleanup_staged_text_file(staged);
        return false;
    }
    return true;
}

static bool commit_staged_text_files(std::vector<StagedTextFile>& staged) {
    std::error_code ec;

    for (auto& file : staged) {
        if (fs::is_directory(file.final_path, ec)) {
            std::cerr << "Error: cannot write file over directory: "
                      << file.final_path << "\n";
            rollback_staged_text_files(staged);
            return false;
        }
        ec.clear();

        if (fs::exists(file.final_path, ec)) {
            file.backup_path = make_unique_sibling_path(file.final_path, ".backup-write");
            if (file.backup_path.empty()) {
                std::cerr << "Error: cannot allocate backup path for "
                          << file.final_path << "\n";
                rollback_staged_text_files(staged);
                return false;
            }
            fs::rename(file.final_path, file.backup_path, ec);
            if (ec) {
                std::cerr << "Error: could not backup existing output "
                          << file.final_path << ": " << ec.message() << "\n";
                file.backup_path.clear();
                rollback_staged_text_files(staged);
                return false;
            }
        } else if (ec) {
            std::cerr << "Error: cannot inspect output path "
                      << file.final_path << ": " << ec.message() << "\n";
            rollback_staged_text_files(staged);
            return false;
        }
        ec.clear();
    }

    for (auto& file : staged) {
        fs::rename(file.temp_path, file.final_path, ec);
        if (ec) {
            std::cerr << "Error: could not install staged output "
                      << file.final_path << ": " << ec.message() << "\n";
            rollback_staged_text_files(staged);
            return false;
        }
        file.temp_path.clear();
        file.installed = true;
        ec.clear();
    }

    for (auto& file : staged) {
        if (!file.backup_path.empty()) {
            fs::remove(file.backup_path, ec);
            if (ec) {
                std::cerr << "Warning: could not remove previous output backup "
                          << file.backup_path << ": " << ec.message() << "\n";
            }
            file.backup_path.clear();
            ec.clear();
        }
    }

    return true;
}

static bool write_files_atomically(
    const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<StagedTextFile> staged;
    staged.reserve(files.size());
    for (const auto& [path, content] : files) {
        StagedTextFile file;
        if (!stage_text_file(path, content, file)) {
            cleanup_staged_text_file(file);
            rollback_staged_text_files(staged);
            return false;
        }
        staged.push_back(std::move(file));
    }
    return commit_staged_text_files(staged);
}

/// If `input_file` points at a Pulp-flavoured ZIP, extract it to a fresh
/// temp dir or durable output sidecar and return the path information.
/// Otherwise return std::nullopt.
/// On extraction failure the returned PulpZipExtraction.temp_dir is empty
/// and the caller should fall back to treating input_file as raw JSON
/// (with a useful error already printed to stderr).
static std::optional<PulpZipExtraction>
extract_pulp_zip_if_present(const std::string& input_file,
                            const fs::path& durable_extract_dir = {}) {
    if (!looks_like_pulp_zip(input_file)) return std::nullopt;

    PulpZipExtraction out;
    const bool persist_extraction = !durable_extract_dir.empty();
    fs::path final_extract_dir;
    if (persist_extraction) {
        final_extract_dir = durable_extract_dir;
        out.temp_dir = make_unique_dir_near(final_extract_dir);
        if (out.temp_dir.empty()) {
            std::cerr << "Error: could not create temporary asset sidecar near "
                      << final_extract_dir << "\n";
            return out;
        }
    } else {
        out.temp_dir = make_temp_dir();
        if (out.temp_dir.empty()) {
            std::cerr << "Error: could not create temp directory for "
                      << input_file << "\n";
            return out;  // empty temp_dir signals "tried but failed"
        }
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, input_file.c_str(), 0)) {
        std::cerr << "Error: not a valid ZIP archive: " << input_file
                  << "\n";
        return out;
    }

    // Bomb / over-quota caps. Realistic Pulp exports are a few MB of JSON
    // + a few hundred KB of PNG/SVG assets; cap well above that but well
    // below "fills /tmp on a CI runner". Numbers chosen to be ~10× any
    // realistic plugin export.
    constexpr std::uint64_t kMaxTotalUncompressed   = 256ull * 1024 * 1024;  // 256 MB
    constexpr std::uint64_t kMaxPerFileUncompressed =  64ull * 1024 * 1024;  //  64 MB
    constexpr mz_uint       kMaxFileCount           = 10000;

    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    if (n > kMaxFileCount) {
        std::cerr << "Error: ZIP " << input_file << " has " << n
                  << " entries (>" << kMaxFileCount
                  << "); refusing to extract\n";
        mz_zip_reader_end(&zip);
        return out;
    }
    std::uint64_t total_uncompressed = 0;
    std::string scene_candidate;
    for (mz_uint i = 0; i < n; ++i) {
        // mz_zip_reader_get_filename truncates silently when name_buf is
        // too small. A malicious archive can stuff a 2-KB entry name like
        // "<1020 safe chars>/../../../etc/passwd": the truncated string
        // sails past our `..` substring check, but the central directory
        // still holds the FULL name and mz_zip_reader_extract_to_file
        // happily writes outside temp_dir. Probe required size first and
        // reject anything that wouldn't fit (or exceeds a sane POSIX-y
        // path limit) BEFORE we ever read the name.
        const mz_uint name_size = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
        char name_buf[1024]{};
        if (name_size == 0 || name_size > sizeof(name_buf)) {
            std::cerr << "Error: ZIP " << input_file << " entry " << i
                      << " has oversized filename (" << name_size
                      << " bytes); refusing\n";
            mz_zip_reader_end(&zip);
            return out;
        }
        mz_zip_reader_get_filename(&zip, i, name_buf, sizeof(name_buf));
        const std::string entry_name(name_buf);
        if (entry_name.empty()) continue;

        // Skip directory entries (trailing slash).
        if (entry_name.back() == '/') continue;

        // Per-file + running total uncompressed-size caps (zip-bomb guard).
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            std::cerr << "Error: ZIP " << input_file << " entry "
                      << entry_name << " has unreadable stat; refusing\n";
            mz_zip_reader_end(&zip);
            return out;
        }
        if (stat.m_uncomp_size > kMaxPerFileUncompressed) {
            std::cerr << "Error: ZIP " << input_file << " entry "
                      << entry_name << " uncompressed size "
                      << stat.m_uncomp_size << " > " << kMaxPerFileUncompressed
                      << " bytes; refusing\n";
            mz_zip_reader_end(&zip);
            return out;
        }
        total_uncompressed += stat.m_uncomp_size;
        if (total_uncompressed > kMaxTotalUncompressed) {
            std::cerr << "Error: ZIP " << input_file
                      << " total uncompressed size > "
                      << kMaxTotalUncompressed << " bytes; refusing\n";
            mz_zip_reader_end(&zip);
            return out;
        }

        // Path-safety: refuse anything that could escape temp_dir.
        // (a) `..` anywhere — catches `a/../../etc/x` and trailing `..`.
        // (b) entry that resolves absolute under std::filesystem rules —
        //     catches POSIX `/foo`, Windows `C:\foo`, UNC `\\srv\sh\x`,
        //     and any platform-specific oddity we'd otherwise miss.
        // (c) Windows drive-relative `C:foo` — `is_absolute()` does NOT
        //     consider this absolute on Linux (where this CLI parses
        //     archives Windows authors may have produced), so guard
        //     explicitly: any entry whose second character is `:` after
        //     a single alphabetic drive letter.
        bool unsafe = false;
        const char* unsafe_reason = "";
        if (entry_name.find("..") != std::string::npos) {
            unsafe = true; unsafe_reason = "contains '..'";
        } else if (fs::path(entry_name).is_absolute()) {
            unsafe = true; unsafe_reason = "absolute path";
        } else if (entry_name.size() >= 2 &&
                   ((entry_name[0] >= 'A' && entry_name[0] <= 'Z') ||
                    (entry_name[0] >= 'a' && entry_name[0] <= 'z')) &&
                   entry_name[1] == ':') {
            unsafe = true; unsafe_reason = "drive-relative Windows path";
        } else if (!entry_name.empty() &&
                   (entry_name[0] == '/' || entry_name[0] == '\\')) {
            // Belt + braces — `is_absolute()` on macOS / Linux already
            // catches `/`, but this also covers `\foo` on a Unix host
            // where fs::path treats `\` as a regular character.
            unsafe = true; unsafe_reason = "leading slash";
        }
        if (unsafe) {
            std::cerr << "Error: refusing unsafe zip entry (" << unsafe_reason
                      << "): " << entry_name << "\n";
            mz_zip_reader_end(&zip);
            return out;
        }

        const fs::path dest = out.temp_dir / entry_name;
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        if (ec) {
            std::cerr << "Error: could not mkdir for " << dest << ": "
                      << ec.message() << "\n";
            mz_zip_reader_end(&zip);
            return out;
        }
        if (!mz_zip_reader_extract_to_file(&zip, i, dest.string().c_str(), 0)) {
            std::cerr << "Error: failed to extract " << entry_name
                      << " from " << input_file << "\n";
            mz_zip_reader_end(&zip);
            return out;
        }

        // Identify the IR envelope. The plugin uses scene.pulp.json by
        // convention; accept `scene.json` and `design.json` as fallbacks
        // so older or hand-authored archives still work.
        const auto fname = fs::path(entry_name).filename().string();
        if (scene_candidate.empty()) {
            if (fname == "scene.pulp.json" ||
                fname == "scene.json"      ||
                fname == "design.json") {
                scene_candidate = dest.string();
            }
        }
    }
    mz_zip_reader_end(&zip);

    if (scene_candidate.empty()) {
        std::cerr << "Error: ZIP " << input_file
                  << " contains no scene.pulp.json / scene.json / design.json\n";
        return out;
    }

    if (persist_extraction) {
        std::error_code ec;
        const auto rel_scene = fs::relative(scene_candidate, out.temp_dir, ec);
        if (ec || rel_scene.empty()) {
            std::cerr << "Error: could not resolve ZIP scene path for "
                      << scene_candidate << "\n";
            out.scene_json_path.clear();
            return out;
        }
        out.final_dir = final_extract_dir;
        out.scene_rel_path = rel_scene;
        out.scene_json_path = fs::path(scene_candidate);
    } else {
        out.scene_json_path = fs::path(scene_candidate);
    }
    return out;
}

static bool write_file(const std::string& path, const std::string& content) {
    return write_files_atomically({{path, content}});
}

// ── CLI options ────────────────────────────────────────────────────────────
// Every flag the CLI accepts, with the defaults the parse loop has always
// applied. These were main()'s locals; the run's mutable outcome state
// (fidelity_failed / similarity_failed) deliberately stays in main().
struct CliOptions {
    std::string source_str;
    std::string input_file;
    // --file: repeatable. Two or more paths capture a MULTI-STATE design from
    // already-exported envelopes — one per state, in the order given — which is
    // how a lane that exports a faithful frame at a time (the Figma REST
    // faithful-vector export, the Figma plugin) reaches multi-state capture.
    // input_file stays the first, so every single-file path is untouched.
    std::vector<std::string> input_files;
    std::string input_url;           // --url: Figma file URL or v0 share link
    // --frame: Figma frame/artboard name. Repeatable — two or more capture a
    // multi-state design into one DesignFrameView, in the order given, so a
    // `swap` element's target_frame is an index into this list.
    std::vector<std::string> frame_names;
    std::string screen_name;         // --screen: Stitch screen name
    std::string page_name;           // --page: Figma page name (scopes the .fig lane)
    bool outline_mode = false;       // --outline: read-only page/frame inventory (fig lane)
    bool outline_json = false;       // --json: emit the outline as JSON
    std::string output_file = "ui.js";
    std::string tokens_file = "tokens.json";
    std::string export_format = "w3c";
    std::string reference_image;     // --reference: PNG of source design for validation
    std::string diff_output;         // --diff: output path for visual diff image
    std::string import_report_path;  // --import-report: write the P7 resolution report JSON here
    bool fail_on_unresolved = false; // --fail-on-unresolved: nonzero exit if a control is conflicted/inert
    // --fail-below <pct>: opt-in gate turning a low --reference similarity into
    // a nonzero exit. Negative means "not requested" — absent the flag the
    // similarity stays advisory and the exit code is unchanged, so existing
    // callers that only read the printed number keep working.
    float fail_below_pct = -1.0f;
    bool dry_run = false;
    bool include_tokens = true;
    bool include_comments = true;
    bool export_tokens_mode = false;
    bool validate = false;           // --validate: render + compare after import
    std::string dump_layout_path;    // --dump-layout <file>: write the laid-out view tree as JSON
    bool strict_fidelity = false;    // --strict-fidelity: fail on a fidelity self-check finding
    std::string fidelity_report_path; // --fidelity-report <file>: write the JSON fidelity ledger
    bool use_web_compat = false;     // --web-compat: use DOM API instead of native
    bool preview_mode = false;       // --preview: minimal widget style for design comparison
    // figma-plugin lane only: @sprite/@silver node suffixes override per knob.
    //
    // `use_silver_knobs` no longer means "always paint Pulp's knob". It is the
    // FALLBACK for a knob the design gives us nothing to draw. A knob the
    // designer actually drew — captured art or vector geometry — keeps their
    // art; substituting our own would overwrite the design we were asked to
    // import. `--knob-style silver` is the explicit opt-out for anyone who
    // wants our widget regardless.
    bool use_silver_knobs = true;    // fallback only; sprite via --knob-style=sprite
    bool skin_faders = true;         // plain via --fader-style=default
    bool skin_meters = true;         // plain via --meter-style=default
    bool debug_json = false;         // --debug: output JSON report with all metrics
    std::string debug_output;        // --debug-output: path for JSON report
    // Fallback only. --validate defaults to the DESIGN's own canvas size (set
    // below once the IR is parsed); these apply solely when the root declares no
    // size. A fixed 340x280 default silently rendered a 1004x672 design into a
    // third of its area, so every --validate screenshot and every similarity
    // score compared a squeezed render against a full-size reference. A
    // verification default that reshapes what it verifies is worse than none.
    int render_width = 340;
    int render_height = 280;
    bool render_size_explicit = false;  // --render-size overrides the canvas
    // --validate backend: Skia is faithful for file-backed images; CoreGraphics
    // renders filename placeholders and is an explicit escape hatch.
    pulp::view::ScreenshotBackend screenshot_backend =
        pulp::view::ScreenshotBackend::skia;
    std::string bridge_output = "bridge_handlers.cpp";  // claude scaffold output
    bool bridge_output_explicit = false;                 // bridge output was set explicitly
    bool emit_bridge_scaffold = true;                    // default on for --from claude
    bool execute_bundle = false;                         // native-runtime path
    std::string classnames_output = "classnames.json";   // claude classname map
    bool classnames_output_explicit = false;             // classname output was set explicitly
    bool emit_classnames = true;                          // default on for --from claude
    std::string shortcuts_output = "shortcuts.json";
    bool shortcuts_output_explicit = false;
    bool import_shortcuts = true;
    bool default_shortcuts = true;
    bool output_explicit = false;                         // output path was set explicitly
    bool tokens_file_explicit = false;                    // tokens file was set explicitly
    // Versioned detect surface.
    bool detect_only = false;
    bool report_new_format = false;
    std::string input_directory;                          // --directory: alternative to --file
    std::string compat_override;                          // --compat: explicit compat.json path
    ArtifactEmit artifact_emit = ArtifactEmit::js;
    RuntimeMode runtime_mode = RuntimeMode::live;
    bool artifact_emit_explicit = false;
    bool runtime_mode_explicit = false;
    SnapshotSemantics snapshot_semantics = SnapshotSemantics::fail;
    bool allow_network_fetch = false;
    int asset_timeout_ms = 30000;
    std::string asset_cache_dir;
    std::unordered_map<std::string, std::string> expected_asset_hashes;
    // User recognition mappings are merged over the built-in Figma library.
    std::string recognition_manifest_path;
    // Out-of-band host-param bindings: figma node id → param_key, applied to the
    // parsed IR's geometry-detected controls (a layer-name sigil still wins).
    std::string param_binding_manifest_path;
};

// Parse argv into `opt`, keeping the historical flag handling, defaults,
// error messages, and exit codes exactly. Returns the process exit code when
// parsing ends the run (--help, a bad flag value, or an inconsistent flag
// combination); std::nullopt to continue with the import pipeline.
static std::optional<int> parse_cli_args(int argc, char* argv[], CliOptions& opt) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            opt.source_str = argv[++i];
        } else if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            opt.input_files.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            opt.input_url = argv[++i];
        } else if (std::strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
            opt.frame_names.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            opt.screen_name = argv[++i];
        } else if (std::strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            opt.page_name = argv[++i];
        } else if (std::strcmp(argv[i], "--outline") == 0) {
            opt.outline_mode = true;
        } else if (std::strcmp(argv[i], "--json") == 0) {
            opt.outline_json = true;
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            opt.output_file = argv[++i];
            opt.output_explicit = true;
        } else if (std::strcmp(argv[i], "--tokens") == 0 && i + 1 < argc) {
            opt.tokens_file = argv[++i];
            opt.tokens_file_explicit = true;
        } else if (std::strcmp(argv[i], "--dry-run") == 0) {
            opt.dry_run = true;
        } else if (std::strcmp(argv[i], "--no-tokens") == 0) {
            opt.include_tokens = false;
        } else if (std::strcmp(argv[i], "--no-comments") == 0) {
            opt.include_comments = false;
        } else if (std::strcmp(argv[i], "--export-tokens") == 0) {
            opt.export_tokens_mode = true;
        } else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            opt.export_format = argv[++i];
        } else if (std::strcmp(argv[i], "--web-compat") == 0) {
            opt.use_web_compat = true;
        } else if (std::strcmp(argv[i], "--validate") == 0) {
            opt.validate = true;
        } else if (std::strcmp(argv[i], "--strict-fidelity") == 0) {
            opt.strict_fidelity = true;
        } else if (std::strcmp(argv[i], "--fidelity-report") == 0 && i + 1 < argc) {
            opt.fidelity_report_path = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-layout") == 0 && i + 1 < argc) {
            // The dump is taken after the layout pass, which only runs under
            // --validate. Implying it (as --reference does) beats asking for
            // both and silently writing nothing when the caller forgets one.
            opt.dump_layout_path = argv[++i];
            opt.validate = true;
        } else if (std::strcmp(argv[i], "--reference") == 0 && i + 1 < argc) {
            opt.reference_image = argv[++i];
            opt.validate = true;
        } else if (std::strcmp(argv[i], "--diff") == 0 && i + 1 < argc) {
            opt.diff_output = argv[++i];
        } else if (std::strcmp(argv[i], "--import-report") == 0 && i + 1 < argc) {
            opt.import_report_path = argv[++i];
        } else if (std::strcmp(argv[i], "--recognition-manifest") == 0 && i + 1 < argc) {
            opt.recognition_manifest_path = argv[++i];
        } else if (std::strcmp(argv[i], "--param-binding-manifest") == 0 && i + 1 < argc) {
            opt.param_binding_manifest_path = argv[++i];
        } else if (std::strcmp(argv[i], "--fail-on-unresolved") == 0) {
            opt.fail_on_unresolved = true;
        } else if (std::strcmp(argv[i], "--fail-below") == 0) {
            // Unit is PERCENT (0-100), matching the "Similarity: NN%" line this
            // gates on. A fraction like 0.85 is rejected rather than silently
            // read as 0.85% — a threshold that parses too low never fires, which
            // is precisely the silent-pass failure this flag exists to end.
            //
            // The missing-value case is checked BEFORE consuming argv: gating on
            // `&& i + 1 < argc` would drop a valueless `--fail-below` through to
            // the arg loop's silent fallthrough, disabling the gate while still
            // reading as enforcement. `--fail-below $UNSET_VAR` in CI must be a
            // hard error, not a pass.
            if (i + 1 >= argc) {
                std::cerr << "Error: --fail-below requires a value (percent, 0-100)\n";
                return 2;
            }
            const std::string raw = argv[++i];
            std::size_t consumed = 0;
            float pct = 0.0f;
            try {
                pct = std::stof(raw, &consumed);
            } catch (const std::exception&) {
                consumed = 0;
            }
            if (consumed != raw.size()) {
                std::cerr << "Error: --fail-below expects a number, got '" << raw << "'\n";
                return 2;
            }
            if (pct > 0.0f && pct < 1.0f) {
                std::cerr << "Error: --fail-below takes a percentage (0-100), not a fraction; "
                          << "'" << raw << "' is ambiguous. Did you mean "
                          << static_cast<int>(pct * 100.0f) << "?\n";
                return 2;
            }
            if (pct < 0.0f || pct > 100.0f) {
                std::cerr << "Error: --fail-below must be between 0 and 100, got '" << raw << "'\n";
                return 2;
            }
            opt.fail_below_pct = pct;
        } else if (std::strcmp(argv[i], "--render-size") == 0 && i + 1 < argc) {
            // Parse WxH
            std::string sz = argv[++i];
            auto x = sz.find('x');
            if (x != std::string::npos) {
                opt.render_width = std::stoi(sz.substr(0, x));
                opt.render_height = std::stoi(sz.substr(x + 1));
                opt.render_size_explicit = true;
            }
        } else if (std::strcmp(argv[i], "--screenshot-backend") == 0 && i + 1 < argc) {
            std::string b = argv[++i];
            if (b == "skia") {
                opt.screenshot_backend = pulp::view::ScreenshotBackend::skia;
            } else if (b == "coregraphics" || b == "cg") {
                opt.screenshot_backend = pulp::view::ScreenshotBackend::coregraphics;
            } else {
                std::cerr << "Error: --screenshot-backend must be skia or coregraphics\n";
                return 2;
            }
        } else if (std::strcmp(argv[i], "--preview") == 0) {
            opt.preview_mode = true;
        } else if (auto value = consume_value_option(i, argc, argv, "--knob-style")) {
            if (auto parsed = parse_knob_style_pref(*value)) opt.use_silver_knobs = *parsed;
            else {
                std::cerr << "Error: --knob-style must be silver, sprite, auto, standard, or default\n";
                return 2;
            }
        } else if (auto value = consume_value_option(i, argc, argv, "--fader-style")) {
            if (auto parsed = parse_skin_style_pref(*value)) opt.skin_faders = *parsed;
            else {
                std::cerr << "Error: --fader-style must be skin, skinned, default, or plain\n";
                return 2;
            }
        } else if (auto value = consume_value_option(i, argc, argv, "--meter-style")) {
            if (auto parsed = parse_skin_style_pref(*value)) opt.skin_meters = *parsed;
            else {
                std::cerr << "Error: --meter-style must be skin, skinned, default, or plain\n";
                return 2;
            }
        } else if (std::strcmp(argv[i], "--debug") == 0) {
            opt.debug_json = true;
        } else if (std::strcmp(argv[i], "--debug-output") == 0 && i + 1 < argc) {
            opt.debug_output = argv[++i];
            opt.debug_json = true;
        } else if (std::strcmp(argv[i], "--bridge-output") == 0 && i + 1 < argc) {
            opt.bridge_output = argv[++i];
            opt.bridge_output_explicit = true;
        } else if (std::strcmp(argv[i], "--no-bridge-scaffold") == 0) {
            opt.emit_bridge_scaffold = false;
        } else if (std::strcmp(argv[i], "--execute-bundle") == 0) {
            opt.execute_bundle = true;
        } else if (std::strcmp(argv[i], "--classnames") == 0 && i + 1 < argc) {
            opt.classnames_output = argv[++i];
            opt.classnames_output_explicit = true;
        } else if (std::strcmp(argv[i], "--emit") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --emit requires a value: js, ir-json, cpp, swiftui, or classnames\n";
                return 2;
            }
            std::string what = argv[++i];
            if (what == "js") {
                opt.artifact_emit = ArtifactEmit::js;
                opt.artifact_emit_explicit = true;
            } else if (what == "ir-json") {
                opt.artifact_emit = ArtifactEmit::ir_json;
                opt.artifact_emit_explicit = true;
            } else if (what == "cpp") {
                opt.artifact_emit = ArtifactEmit::cpp;
                opt.artifact_emit_explicit = true;
            } else if (what == "swiftui") {
                opt.artifact_emit = ArtifactEmit::swiftui;
                opt.artifact_emit_explicit = true;
            } else if (what == "classnames") {
                opt.emit_classnames = true;
            } else {
                std::cerr << "Error: unsupported --emit value '" << what
                          << "' (expected js, ir-json, cpp, swiftui, or classnames)\n";
                return 2;
            }
        } else if (std::strcmp(argv[i], "--mode") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --mode requires a value: live or baked\n";
                return 2;
            }
            std::string mode = argv[++i];
            if (mode == "live") {
                opt.runtime_mode = RuntimeMode::live;
                opt.runtime_mode_explicit = true;
            } else if (mode == "baked") {
                opt.runtime_mode = RuntimeMode::baked;
                opt.runtime_mode_explicit = true;
            } else {
                std::cerr << "Error: unsupported --mode value '" << mode
                          << "' (expected live or baked)\n";
                return 2;
            }
        } else if (std::strcmp(argv[i], "--snapshot-semantics") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --snapshot-semantics requires a value: fail, warn, or accept\n";
                return 2;
            }
            std::string semantics = argv[++i];
            if (semantics == "fail") {
                opt.snapshot_semantics = SnapshotSemantics::fail;
            } else if (semantics == "warn") {
                opt.snapshot_semantics = SnapshotSemantics::warn;
            } else if (semantics == "accept") {
                opt.snapshot_semantics = SnapshotSemantics::accept;
            } else {
                std::cerr << "Error: unsupported --snapshot-semantics value '" << semantics
                          << "' (expected fail, warn, or accept)\n";
                return 2;
            }
        } else if (std::strcmp(argv[i], "--allow-network-fetch") == 0) {
            opt.allow_network_fetch = true;
        } else if (std::strcmp(argv[i], "--asset-cache") == 0 && i + 1 < argc) {
            opt.asset_cache_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--asset-timeout-ms") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --asset-timeout-ms requires a value\n";
                return 2;
            }
            if (!parse_positive_int_arg("--asset-timeout-ms", argv[++i], opt.asset_timeout_ms))
                return 2;
        } else if (std::strcmp(argv[i], "--asset-hash") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --asset-hash requires <uri=sha256-hex>\n";
                return 2;
            }
            if (!parse_asset_hash_arg(argv[++i], opt.expected_asset_hashes))
                return 2;
        } else if (std::strcmp(argv[i], "--no-emit-classnames") == 0) {
            opt.emit_classnames = false;
        } else if (std::strcmp(argv[i], "--shortcuts") == 0 && i + 1 < argc) {
            opt.shortcuts_output = argv[++i];
            opt.shortcuts_output_explicit = true;
        } else if (std::strcmp(argv[i], "--no-import-shortcuts") == 0) {
            opt.import_shortcuts = false;
        } else if (std::strcmp(argv[i], "--no-default-shortcuts") == 0) {
            opt.default_shortcuts = false;
        } else if (std::strcmp(argv[i], "--detect-only") == 0) {
            opt.detect_only = true;
        } else if (std::strcmp(argv[i], "--report-new-format") == 0) {
            opt.report_new_format = true;
            opt.detect_only = true;
        } else if (std::strcmp(argv[i], "--directory") == 0 && i + 1 < argc) {
            opt.input_directory = argv[++i];
        } else if (std::strcmp(argv[i], "--compat") == 0 && i + 1 < argc) {
            opt.compat_override = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    // --fail-below gates the --reference similarity, so without a reference it
    // could never fire. Rejecting that up front keeps a CI gate from reading as
    // enforced while silently passing everything.
    if (opt.fail_below_pct >= 0.0f && opt.reference_image.empty()) {
        std::cerr << "Error: --fail-below requires --reference <png> (nothing to compare against)\n";
        return 2;
    }

    return std::nullopt;
}

// Reference-free fidelity self-check: surface any sprite the importer could
// not prove it sized faithfully. Always reported as warnings; with
// --strict-fidelity a finding makes the import exit non-zero.
// Informational findings (e.g. a below-native-minimum widget codegen clamps
// up) are surfaced as warnings but never gate the import — only "hard"
// findings trip --strict-fidelity (see count_strict_fidelity_failures).
//
// --fidelity-report: persist the run's findings as a machine-readable ledger
// (a durable, diffable fidelity contract). An explicitly-requested diagnostic
// artifact, so it is written regardless of --strict-fidelity or --dry-run —
// matching the --import-report convention; --dry-run only suppresses the
// primary generated code + token files.
//
// Sets `fidelity_failed` when --strict-fidelity has a hard finding. Returns
// the process exit code when the run must stop (ledger write failure);
// std::nullopt to continue.
static std::optional<int> run_fidelity_selfcheck(
    const std::vector<pulp::view::FidelityIssue>& fidelity_issues,
    bool strict_fidelity,
    const std::string& fidelity_report_path,
    DesignSource source,
    const std::string& output_file,
    bool& fidelity_failed) {
    for (const auto& fi : fidelity_issues) {
        std::cerr << "fidelity: [" << fi.kind << "] " << fi.node_name
                  << " (" << fi.node_id << "): " << fi.detail
                  << (fi.informational ? "  [informational]" : "") << "\n";
    }
    const std::size_t hard_findings =
        pulp::view::count_strict_fidelity_failures(fidelity_issues);
    if (strict_fidelity && hard_findings > 0) {
        std::cerr << "fidelity: " << hard_findings
                  << " issue(s); failing due to --strict-fidelity\n";
        fidelity_failed = true;
    }

    if (!fidelity_report_path.empty()) {
        pulp::view::FidelityLedgerMeta meta;
        meta.source = design_source_name(source);
        meta.output = output_file;
        if (!write_file(fidelity_report_path,
                        pulp::view::fidelity_ledger_json(fidelity_issues, meta))) {
            std::cerr << "Error: cannot write fidelity report to " << fidelity_report_path << "\n";
            return 1;
        }
        std::cerr << "fidelity: wrote ledger to " << fidelity_report_path << "\n";
    }
    return std::nullopt;
}

int main(int argc, char* argv[]) {
    CliOptions cli;
    if (auto parse_exit = parse_cli_args(argc, argv, cli)) return *parse_exit;

    bool fidelity_failed = false;    // set when strict_fidelity + at least one finding
    bool similarity_failed = false;  // set when --fail-below + reference similarity under the bar

    // Local references into the parsed options. The pipeline below reads and
    // mutates these names exactly as it did when they were main()'s own locals
    // (the emit-dependent output defaults, the multi-state input_file rewrite,
    // and --validate's design-size render default all land in the same storage).
    auto& source_str = cli.source_str;
    auto& input_file = cli.input_file;
    auto& input_files = cli.input_files;
    auto& input_url = cli.input_url;
    auto& frame_names = cli.frame_names;
    auto& screen_name = cli.screen_name;
    auto& page_name = cli.page_name;
    auto& outline_mode = cli.outline_mode;
    auto& outline_json = cli.outline_json;
    auto& output_file = cli.output_file;
    auto& tokens_file = cli.tokens_file;
    auto& export_format = cli.export_format;
    auto& reference_image = cli.reference_image;
    auto& diff_output = cli.diff_output;
    auto& import_report_path = cli.import_report_path;
    auto& fail_on_unresolved = cli.fail_on_unresolved;
    auto& fail_below_pct = cli.fail_below_pct;
    auto& dry_run = cli.dry_run;
    auto& include_tokens = cli.include_tokens;
    auto& include_comments = cli.include_comments;
    auto& export_tokens_mode = cli.export_tokens_mode;
    auto& validate = cli.validate;
    auto& dump_layout_path = cli.dump_layout_path;
    auto& strict_fidelity = cli.strict_fidelity;
    auto& fidelity_report_path = cli.fidelity_report_path;
    auto& use_web_compat = cli.use_web_compat;
    auto& preview_mode = cli.preview_mode;
    auto& use_silver_knobs = cli.use_silver_knobs;
    auto& skin_faders = cli.skin_faders;
    auto& skin_meters = cli.skin_meters;
    auto& debug_json = cli.debug_json;
    auto& debug_output = cli.debug_output;
    auto& render_width = cli.render_width;
    auto& render_height = cli.render_height;
    auto& render_size_explicit = cli.render_size_explicit;
    auto& screenshot_backend = cli.screenshot_backend;
    auto& bridge_output = cli.bridge_output;
    auto& bridge_output_explicit = cli.bridge_output_explicit;
    auto& emit_bridge_scaffold = cli.emit_bridge_scaffold;
    auto& execute_bundle = cli.execute_bundle;
    auto& classnames_output = cli.classnames_output;
    auto& classnames_output_explicit = cli.classnames_output_explicit;
    auto& emit_classnames = cli.emit_classnames;
    auto& shortcuts_output = cli.shortcuts_output;
    auto& shortcuts_output_explicit = cli.shortcuts_output_explicit;
    auto& import_shortcuts = cli.import_shortcuts;
    auto& default_shortcuts = cli.default_shortcuts;
    auto& output_explicit = cli.output_explicit;
    auto& tokens_file_explicit = cli.tokens_file_explicit;
    auto& detect_only = cli.detect_only;
    auto& report_new_format = cli.report_new_format;
    auto& input_directory = cli.input_directory;
    auto& compat_override = cli.compat_override;
    auto& artifact_emit = cli.artifact_emit;
    auto& runtime_mode = cli.runtime_mode;
    auto& artifact_emit_explicit = cli.artifact_emit_explicit;
    auto& runtime_mode_explicit = cli.runtime_mode_explicit;
    auto& snapshot_semantics = cli.snapshot_semantics;
    auto& allow_network_fetch = cli.allow_network_fetch;
    auto& asset_timeout_ms = cli.asset_timeout_ms;
    auto& asset_cache_dir = cli.asset_cache_dir;
    auto& expected_asset_hashes = cli.expected_asset_hashes;
    auto& recognition_manifest_path = cli.recognition_manifest_path;
    auto& param_binding_manifest_path = cli.param_binding_manifest_path;

    DefaultSelection default_selection;
    if (!export_tokens_mode && !detect_only) {
        default_selection = resolve_import_design_defaults(
            artifact_emit,
            runtime_mode,
            artifact_emit_explicit,
            runtime_mode_explicit);
        if (!default_selection.error.empty()) {
            std::cerr << "Error: " << default_selection.error << "\n";
            return 2;
        }
        artifact_emit = default_selection.emit;
        runtime_mode = default_selection.mode;
    }

    if (artifact_emit == ArtifactEmit::cpp && !output_explicit)
        output_file = "imported_ui.cpp";
    if (artifact_emit == ArtifactEmit::swiftui && !output_explicit)
        output_file = "ImportedPulpView.swift";

    // --format css-variables emits a CSS file, so its sidecar defaults to
    // theme.css rather than tokens.json (the W3C default). The leaf name also
    // feeds the sidecar anchoring below.
    const char* tokens_default_leaf =
        (export_format == "css-variables") ? "theme.css" : "tokens.json";
    if (export_format == "css-variables" && !tokens_file_explicit)
        tokens_file = tokens_default_leaf;

    // Reject unknown --format values up front with a helpful message rather
    // than silently falling back to W3C. Tailwind variants stay source-gated
    // to DESIGN.md at the write site; the rest are theme-based.
    if (export_format != "w3c" && export_format != "css-variables" &&
        export_format != "tailwind" && export_format != "json-tailwind" &&
        export_format != "css-tailwind") {
        std::cerr << "Error: unsupported --format value '" << export_format
                  << "' (expected: w3c, css-variables, tailwind, json-tailwind, css-tailwind)\n";
        return 2;
    }

    // When the user passes --output <dir>/ui.js, anchor the sidecar files
    // (bridge_handlers.cpp, classnames.json,
    // tokens.json) to the same directory so they don't scatter to cwd.
    // Only applies when the sidecar flag wasn't given explicitly.
    if (output_explicit) {
        fs::path out_dir = fs::path(output_file).parent_path();
        if (!out_dir.empty()) {
            auto anchor = [&](std::string& slot, const char* leaf) {
                slot = (out_dir / leaf).string();
            };
            if (!bridge_output_explicit)     anchor(bridge_output,     "bridge_handlers.cpp");
            if (!classnames_output_explicit) anchor(classnames_output, "classnames.json");
            if (!shortcuts_output_explicit)  anchor(shortcuts_output,  "shortcuts.json");
            if (!tokens_file_explicit)       anchor(tokens_file,       tokens_default_leaf);
        }
    }

    // Every path below reads the first --file; a second one only means
    // multi-state capture, handled once the source is known.
    if (!input_files.empty()) input_file = input_files.front();

    // Export-tokens mode: read a Pulp theme JSON and export in --format.
    if (export_tokens_mode) {
        // Tailwind formats need DESIGN.md section context that --export-tokens
        // (a flat theme → tokens path) does not have. Reject rather than
        // silently emit W3C under the requested-but-unhonored format name.
        if (is_tailwind_format(export_format)) {
            std::cerr << "Error: --format " << export_format
                      << " requires an import with --from designmd; "
                         "--export-tokens supports w3c and css-variables only\n";
            return 2;
        }
        if (input_file.empty()) {
            // No input = export the built-in dark theme
            auto theme = Theme::dark();
            auto body = export_theme_tokens(export_format, theme);
            if (dry_run) {
                std::cout << body;
                return 0;
            }
            if (!write_file(tokens_file, body)) return 1;
            std::cout << "Exported " << (theme.colors.size() + theme.dimensions.size() + theme.strings.size())
                      << " tokens → " << tokens_file << " (format=" << export_format << ")\n";
            return 0;
        }
        // Read theme JSON → export in the requested token format
        auto content = read_file(input_file);
        if (content.empty()) return 1;
        auto theme = Theme::from_json(content);
        auto body = export_theme_tokens(export_format, theme);
        if (dry_run) {
            std::cout << body;
            return 0;
        }
        if (!write_file(tokens_file, body)) return 1;
        std::cout << "Exported " << (theme.colors.size() + theme.dimensions.size() + theme.strings.size())
                  << " tokens → " << tokens_file << " (format=" << export_format << ")\n";
        return 0;
    }

    // ── Versioned detect-only path ──────────────────────────────────────
    // Runs against compat.json without invoking the source parsers.
    if (detect_only) {
        namespace det = pulp::import_detect;

        std::string scan_path = input_file.empty() ? input_directory : input_file;
        if (scan_path.empty()) {
            std::cerr << "Error: --detect-only requires --file <path> or --directory <path>\n";
            return 1;
        }
        if (!fs::exists(scan_path)) {
            std::cerr << "Error: path does not exist: " << scan_path << "\n";
            return 1;
        }

        // Resolve compat.json — explicit override > walk parents > cwd.
        fs::path compat_path;
        if (!compat_override.empty()) {
            compat_path = compat_override;
        } else {
            fs::path start = fs::is_directory(scan_path)
                ? fs::path(scan_path)
                : fs::path(scan_path).parent_path();
            if (start.empty()) start = fs::current_path();
            compat_path = det::find_compat_json(start);
            if (compat_path.empty())
                compat_path = det::find_compat_json(fs::current_path());
        }
        if (compat_path.empty() || !fs::exists(compat_path)) {
            std::cerr << "Error: compat.json not found"
                         " (pass --compat <path> or run from a Pulp checkout)\n";
            return 1;
        }

        auto manifest_text = read_file(compat_path.string());
        auto manifest = det::parse_compat_json(manifest_text);
        if (!manifest) {
            std::cerr << "Error: malformed compat.json at " << compat_path << "\n";
            return 1;
        }

        auto snap = det::snapshot_input(scan_path);
        auto result = det::detect(*manifest, snap);

        if (report_new_format) {
            auto report = det::build_new_format_report(*manifest, snap, result);
            std::cout << det::render_new_format_json(report);
            return 0;
        }

        if (result.source.empty()) {
            std::cout << "no detected source for " << scan_path << "\n";
            std::cout << "  compat.json: " << compat_path.string() << " (schema "
                      << manifest->compat_schema_version << ")\n";
            return 2;  // distinct from generic failure (1)
        }

        std::cout << "detected source: " << result.source << "\n";
        std::cout << "  format-version: " << result.format_version << "\n";
        std::cout << "  parser-version: " << result.parser_version << "\n";
        std::cout << "  fingerprint match: " << result.matched_clauses
                  << "/" << result.total_clauses;
        if (!result.matched_kinds.empty()) {
            std::cout << " (";
            for (size_t i = 0; i < result.matched_kinds.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << result.matched_kinds[i];
            }
            std::cout << ")";
        }
        std::cout << "\n";
        std::cout << "  confidence: " << result.confidence_pct << "%\n";

        if (result.confidence_pct < 80) {
            std::cout << "warning: confidence below 80% — this export may be a newer\n"
                      << "         format-version than Pulp recognizes. Pulp will use\n"
                      << "         the most-recent matching parser; gaps surface in\n"
                      << "         import-report.json. To file a new format detector:\n"
                      << "  pulp import-design --file " << scan_path
                      << " --report-new-format\n";
        }
        return 0;
    }

    if (source_str.empty()) {
        std::cerr << "Error: --from <source> is required\n";
        print_usage();
        return 1;
    }

    // `--from fig`: decode a local Figma save file offline. The lane either
    // prints a read-only outline and returns, or rewrites source_str/input_file
    // to the decoded figma-plugin envelope and lets the rest of the pipeline run.
    // The scratch directory it writes is removed when main returns.
    std::string fig_scratch_dir;
    struct FigScratchCleanup {
        const std::string& dir;
        ~FigScratchCleanup() {
            if (!dir.empty()) {
                std::error_code ec;
                fs::remove_all(dir, ec);
            }
        }
    } fig_scratch_cleanup{fig_scratch_dir};

    // Both multi-value flags are validated against the source the USER named,
    // before the fig lane runs — it rewrites source_str to "figma-plugin" once it
    // has decoded, at which point a `--from fig` run is no longer distinguishable
    // from a real figma-plugin one.
    //
    // `--file` names the design to read, so repeating it is only meaningful for a
    // source whose input IS one already-exported state (figma-plugin envelopes);
    // `--from fig` takes one .fig and selects states from it with --frame.
    if (input_files.size() > 1 && source_str != "figma-plugin") {
        std::cerr << "Error: --file is repeatable only with --from figma-plugin (got "
                  << input_files.size() << " --file values with --from " << source_str << ")\n";
        std::cerr << "       Repeated --file merges one already-exported envelope per state "
                     "into a\n"
                     "       multi-state design; other sources parse a single file.\n";
        return 2;
    }

    // `--frame` selects a frame from ONE design file, which only the .fig lane
    // does per-frame; every other source resolves a single frame per run. Repeat
    // it elsewhere and the extra states would be dropped without a word.
    if (frame_names.size() > 1 && source_str != "fig") {
        std::cerr << "Error: --frame is repeatable only with --from fig (got "
                  << frame_names.size() << " --frame values with --from "
                  << source_str << ")\n";
        std::cerr << "       To capture a multi-state design from another source, export "
                     "each state to its\n"
                     "       own envelope and pass a --file per state: --from figma-plugin "
                     "--file a.pulp.json --file b.pulp.json\n";
        return 2;
    }

    pulp::import_design::fig::LaneArgs fig_args{
        source_str, input_file, frame_names, page_name, outline_mode, outline_json};
    fig_args.created_tmp_dir = &fig_scratch_dir;
    std::string fig_geometry_file;
    fig_args.geometry_file = &fig_geometry_file;
    if (auto fig_code = pulp::import_design::fig::handle(fig_args)) {
        return *fig_code;
    }

    auto source = parse_design_source(source_str);
    if (!source) {
        std::cerr << "Error: unknown source '" << source_str << "'\n";
        std::cerr << "Valid sources: fig, figma, figma-plugin, stitch, v0, pencil, claude, designmd, jsx\n";
        return 1;
    }

    // Multi-state capture from pre-exported envelopes: one --file per state, in
    // capture order, merged into a single envelope whose root carries the rest as
    // alternate_frames. This is the multi-state surface for the lanes that export
    // one faithful frame per run — the Figma REST faithful-vector export and the
    // Figma plugin both write this envelope — and it is deliberately a merge of
    // finished exports rather than a new decode path, so it inherits whatever
    // render mode those exporters produced.
    if (input_files.size() > 1) {
        namespace id = pulp::import_design;
        const fs::path scratch = id::make_scratch_dir("states", input_files.front());
        std::error_code sec;
        fs::create_directories(scratch, sec);
        if (sec) {
            std::cerr << "Error: could not create scratch dir " << scratch << ": "
                      << sec.message() << "\n";
            return 1;
        }
        fig_scratch_dir = scratch.string();   // removed when main returns

        std::vector<fs::path> envelopes;
        envelopes.reserve(input_files.size());
        for (const auto& f : input_files) {
            if (!fs::exists(f)) {
                std::cerr << "Error: --file " << f << " does not exist\n";
                return 1;
            }
            envelopes.emplace_back(f);
        }
        const fs::path merged = scratch / "scene.pulp.json";
        if (auto err = id::merge_frame_envelopes(envelopes, scratch, merged)) return *err;
        input_file = merged.string();
    }

    // Tailwind formats are gated to DESIGN.md (they re-parse it for section
    // context — see the designmd dispatch in the token-write block). On any
    // other source they would silently fall through to W3C while reporting the
    // requested format, so reject up front. Generalizing Tailwind to all
    // sources is Workstream A2.
    if (is_tailwind_format(export_format) && *source != DesignSource::designmd) {
        std::cerr << "Error: --format " << export_format
                  << " currently requires --from designmd (got --from "
                  << source_str << ")\n";
        return 2;
    }

    if (input_file.empty() && input_url.empty()) {
        std::cerr << "Error: --file <path> or --url <url> is required\n";
        return 1;
    }

    if (!input_file.empty() && has_disallowed_file_char(input_file)) {
        std::cerr << "Error: --file contains control characters that are not accepted\n";
        return 2;
    }
    if (!input_url.empty() && has_url_shell_metachar(input_url)) {
        std::cerr << "Error: --url contains shell metacharacters that are not accepted\n";
        return 2;
    }

    if (runtime_mode == RuntimeMode::baked) {
        if (artifact_emit == ArtifactEmit::js) {
            std::cerr << "Error: --mode baked requires --emit ir-json, --emit cpp, or --emit swiftui\n";
            std::cerr << "       effective defaults: --mode " << runtime_mode_name(runtime_mode)
                      << " (" << default_selection.mode_source << "), --emit "
                      << artifact_emit_name(artifact_emit) << " ("
                      << default_selection.emit_source << ")\n";
            return 2;
        }
    } else if (artifact_emit == ArtifactEmit::cpp || artifact_emit == ArtifactEmit::swiftui) {
        std::cerr << "Error: --emit " << artifact_emit_name(artifact_emit)
                  << " requires --mode baked\n";
        std::cerr << "       effective defaults: --mode " << runtime_mode_name(runtime_mode)
                  << " (" << default_selection.mode_source << "), --emit "
                  << artifact_emit_name(artifact_emit) << " ("
                  << default_selection.emit_source << ")\n";
        return 2;
    }
    if (runtime_mode == RuntimeMode::baked) {
        if (*source == DesignSource::designmd
            && (artifact_emit == ArtifactEmit::cpp || artifact_emit == ArtifactEmit::swiftui)) {
            std::cerr << "Error: DESIGN.md is a token spec and cannot emit a baked "
                      << (artifact_emit == ArtifactEmit::swiftui ? "SwiftUI view" : "C++ view")
                      << "\n";
            return 2;
        }
    }

    // --url without --file: fetch the URL content via argv-safe curl.
    std::string fetched_tmp;
    std::unique_ptr<ScopedTempDir> fetched_tmp_dir;
    if (input_file.empty() && !input_url.empty()) {
        try {
            fetched_tmp_dir = std::make_unique<ScopedTempDir>("pulp-import-design");
            fetched_tmp = (fetched_tmp_dir->path() / "download.html").string();
        } catch (const std::exception& e) {
            std::cerr << "Error: failed to prepare URL fetch workspace: " << e.what() << "\n";
            return 1;
        }
        if (!fetch_url_to_file(input_url, fetched_tmp)) return 1;
        input_file = fetched_tmp;
        std::cout << "Fetched " << input_url << " → " << fetched_tmp << "\n";
    }

    auto t_start = std::chrono::steady_clock::now();

    // If the user passed a .pulp.zip (or any ZIP with a Pulp envelope
    // inside), unpack it transparently so `--file` always behaves the
    // same regardless of whether the plugin shipped a JSON or a bundle.
    // Real output artifacts need durable asset paths, so extract beside
    // the output file. Dry-run keeps using an RAII temp dir.
    std::optional<PulpZipExtraction> pulp_zip_keepalive;
    const fs::path durable_zip_extract_dir =
        dry_run ? fs::path{} : zip_asset_sidecar_dir_for_import_output(output_file, artifact_emit);
    if (auto extracted = extract_pulp_zip_if_present(input_file, durable_zip_extract_dir)) {
        if (extracted->scene_json_path.empty()) {
            // Tried to extract but failed; the helper already wrote a
            // useful stderr line and we should not silently fall back to
            // the truncated text-read path.
            return 1;
        }
        std::cout << "Unpacked " << input_file << " → "
                  << extracted->temp_dir;
        if (!extracted->final_dir.empty())
            std::cout << " (assets staged for generated output)";
        std::cout << "\n";
        input_file = extracted->scene_json_path.string();
        pulp_zip_keepalive = std::move(*extracted);
    }

    // Read input
    auto content = read_file(input_file);
    if (content.empty()) return 1;

    if (*source == DesignSource::jsx
        && runtime_mode == RuntimeMode::live
        && artifact_emit == ArtifactEmit::js) {
        if (validate || debug_json || !diff_output.empty()) {
            std::cerr << "Error: --from jsx --mode live --emit js writes the precompiled bundle verbatim "
                         "and does not support --validate, --reference, --diff, or --debug; use "
                         "--mode baked --emit ir-json|cpp for import validation or debug reports\n";
            return 2;
        }
        if (dry_run) {
            std::cout << content;
            return 0;
        }
        if (!write_file(output_file, content)) return 1;
        std::cout << "Wrote " << output_file << " (JSX live bundle)\n";
        return 0;
    }

    // Parse based on source
    DesignIR ir;
    bool parsed_serialized_design_ir = false;
    std::string runtime_error;  // captures --execute-bundle fallback reason
    try {
        if (runtime_mode == RuntimeMode::baked &&
            (artifact_emit == ArtifactEmit::ir_json || artifact_emit == ArtifactEmit::cpp ||
             artifact_emit == ArtifactEmit::swiftui) &&
            !looks_like_figma_plugin_export(content) &&
            looks_like_serialized_design_ir(content)) {
            ir = parse_design_ir_json(content);
            parsed_serialized_design_ir = true;
        } else {
            switch (*source) {
                case DesignSource::figma:
                    // Guardrail: a Figma-plugin export envelope passed to
                    // `--from figma` would otherwise be fed
                    // to parse_figma_json, which finds none of its structure and
                    // silently yields an empty root-only import. Auto-route to
                    // the plugin parser and tell the user once.
                    if (looks_like_figma_plugin_export(content)) {
                        std::cerr << "note: input is a Figma-plugin export envelope; "
                                     "using the figma-plugin parser. Pass "
                                     "--from figma-plugin to silence this notice.\n";
                        ir = parse_figma_plugin_json(content);
                    } else {
                        ir = parse_figma_json(content);
                    }
                    break;
                case DesignSource::figma_plugin: ir = parse_figma_plugin_json(content); break;
                case DesignSource::stitch: ir = parse_stitch_html(content); break;
                case DesignSource::v0:     ir = parse_v0_tsx(content); break;
                case DesignSource::pencil: ir = parse_pencil_json(content); break;
                case DesignSource::claude:
                    if (execute_bundle) {
                        ClaudeRuntimeOptions ropts;
                        ropts.error_out = &runtime_error;
                        // Allow up to 16 MB for the largest realistic Claude
                        // exports (3.1 MB Spectr app + 1.1 MB react-dom +
                        // 0.1 MB react with growth headroom).
                        ropts.max_total_js_bytes = 16 * 1024 * 1024;
                        ropts.runtime_snapshot_viewport_width = render_width;
                        ropts.runtime_snapshot_viewport_height = render_height;
                        ir = parse_claude_html_with_runtime(content, ropts);
                    } else {
                        ir = parse_claude_html(content);
                    }
                    break;
                case DesignSource::designmd: {
                    // DESIGN.md is a system spec, not a screen — parse the
                    // frontmatter into tokens and walk the body for section
                    // ordering. No UI tree is scaffolded; the dispatch below
                    // suppresses the ui.js write for this source.
                    auto pr = parse_designmd(content);
                    ir = std::move(pr.ir);
                    // Hard fail on any error-severity diagnostic (e.g. duplicate
                    // section heading, malformed YAML). Exit code 3 reserved
                    // for parse errors per the integration plan.
                    for (const auto& d : pr.diagnostics) {
                        if (d.severity == DesignMdSeverity::error) {
                            print_designmd_diagnostics(pr.diagnostics);
                            return 3;
                        }
                    }
                    break;
                }
                case DesignSource::jsx:
                    if (runtime_mode != RuntimeMode::baked ||
                        (artifact_emit != ArtifactEmit::ir_json &&
                         artifact_emit != ArtifactEmit::cpp &&
                         artifact_emit != ArtifactEmit::swiftui)) {
                        std::cerr << "Error: --from jsx is currently wired only for"
                                     " --mode baked --emit ir-json, --emit cpp, or --emit swiftui\n";
                        return 2;
                    } else {
                        const auto dynamic_scan = detect_jsx_snapshot_dynamic_apis(content);
                        if (dynamic_scan.has_dynamic_apis()
                            && snapshot_semantics == SnapshotSemantics::fail) {
                            std::cerr << "Error: JSX baked snapshot uses dynamic APIs ("
                                      << join_tokens(dynamic_scan.tokens) << "). "
                                      << "Rerun with --snapshot-semantics warn or accept to proceed.\n";
                            return 2;
                        }

                    auto bundle = parse_jsx_react(content, fs::path(input_file).stem().string());
                    if (!bundle) {
                        std::cerr << "Error: --from jsx expected a precompiled JSX runtime bundle\n";
                        return 1;
                    }
                    auto envelope = synthesize_runtime_envelope(*bundle);
                    ClaudeRuntimeOptions ropts;
                    ropts.error_out = &runtime_error;
                    ropts.max_total_js_bytes = 16 * 1024 * 1024;
                    ropts.runtime_snapshot_viewport_width = render_width;
                    ropts.runtime_snapshot_viewport_height = render_height;
                    ir = parse_claude_html_with_runtime(envelope, ropts);
                    const auto fallback_reason = !runtime_error.empty()
                        ? runtime_error
                        : ir.fallback_reason;
                    const bool captured_runtime =
                        ir.capture_method == "runtime_snapshot" ||
                        ir.capture_method == "runtime_native_snapshot";
                    if (!fallback_reason.empty() || !captured_runtime) {
                        std::cerr << "Error: JSX baked runtime snapshot failed";
                        if (!fallback_reason.empty()) std::cerr << ": " << fallback_reason;
                        std::cerr << "\n";
                        return 1;
                    }
                    const bool native_snapshot = ir.capture_method == "runtime_native_snapshot";
                    ir.source = DesignSource::jsx;
                    ir.capture_method = native_snapshot ? "runtime_native_snapshot" : "runtime_snapshot";
                    if (ir.settle_rounds <= 0) ir.settle_rounds = 4;
                    ir.source_adapter = "jsx-runtime";
                    ir.source_version = "1";
                    if (ir.root.provenance) {
                        ir.root.provenance->adapter = "jsx-runtime";
                        ir.root.provenance->version = "1";
                    } else {
                        ir.root.provenance = IRProvenance{"jsx-runtime", "1", {}};
                    }
                    ir.root.confidence = IRConfidence::pass;
                    ir.root.source_adapter = "jsx-runtime";
                    ir.root.source_version = "1";
                    if (native_snapshot)
                        ir.root.attributes["snapshotSource"] = "native-view";
                    ir.root.attributes["snapshotSemantics"] = snapshot_semantics_name(snapshot_semantics);
                    if (dynamic_scan.has_dynamic_apis()
                        && snapshot_semantics == SnapshotSemantics::warn) {
                        ir.diagnostics.push_back(make_cli_diagnostic(
                            ImportDiagnosticSeverity::warning,
                            ImportDiagnosticKind::snapshot_semantics_warning,
                            "snapshot-dynamic-api",
                            "<source>",
                            "JSX baked snapshot uses dynamic APIs: "
                                + join_tokens(dynamic_scan.tokens)));
                    }
                }
                break;
        }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing " << design_source_name(*source) << " input: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Error parsing " << design_source_name(*source)
                  << " input: parser threw an unknown exception\n";
        return 1;
    }

    if (pulp_zip_keepalive && !pulp_zip_keepalive->final_dir.empty()) {
        if (!commit_pulp_zip_sidecar(*pulp_zip_keepalive)) return 1;
        input_file = pulp_zip_keepalive->scene_json_path.string();
        std::cout << "Persisted ZIP assets → " << pulp_zip_keepalive->final_dir << "\n";
    }

    if (execute_bundle && !runtime_error.empty()) {
        // Surface the harness-fallback reason so users can tell when the
        // bundle eval lane bailed out vs. produced a real materialized IR.
        std::cout << "[execute-bundle] runtime fallback: " << runtime_error << "\n";
    } else if (execute_bundle) {
        std::cout << "[execute-bundle] runtime path produced the IR (no fallback)\n";
    }

    if (!parsed_serialized_design_ir)
        ir.source = *source;
    ir.source_file = input_url.empty() ? input_file : input_url;
    if (ir.imported_at.empty()) ir.imported_at = current_utc_timestamp();
    if (ir.capture_method.empty()) ir.capture_method = "adapter_parse";
    if (ir.source_adapter.empty()) ir.source_adapter = source_str;
    if (ir.source_version.empty()) ir.source_version = "1";
    print_import_diagnostics(ir.diagnostics);

    // Clean up temp file from URL fetch
    if (!fetched_tmp.empty()) fs::remove(fetched_tmp);

    // Store frame/screen selection metadata
    if (!frame_names.empty()) {
        // Frame 0 keeps the plain "frame" attribute so a single-state import's
        // metadata is unchanged; a multi-state capture additionally records the
        // full ordered list, which is the frame index a swap target names.
        ir.root.attributes["frame"] = frame_names.front();
        if (frame_names.size() > 1) {
            std::string all;
            for (std::size_t i = 0; i < frame_names.size(); ++i)
                all += (i ? "," : "") + frame_names[i];
            ir.root.attributes["frames"] = all;
        }
    }
    if (!screen_name.empty()) ir.root.attributes["screen"] = screen_name;

    // ── Extensible key-based recognition manifest merge ──────────────────────
    // Re-resolve each Figma component instance's component-set key / name prefix
    // against the MERGED recognition table (built-in Pulp Figma Library + the
    // user-supplied --recognition-manifest, the latter merged OVER the former),
    // and stamp `audio_widget` on any instance that matched but the in-Figma TS
    // plugin did not already recognize. This wires controls on a THIRD-PARTY
    // design whose instances carry their own component-set keys.
    //
    // Only the figma sources carry the component identity the resolver reads
    // (parse_ir_node stamps figmaComponentKey / figmaMainComponentName from the
    // envelope's `figma` block). For every other source the resolver is empty of
    // matchable nodes, so this is a no-op there.
    //
    // Strictly additive: NO --recognition-manifest and a design with no
    // resolvable third-party keys leaves behavior exactly as before (the built-in
    // library path stays authoritative; an already-stamped audio_widget is never
    // overridden). Unmatched-but-present component instances are surfaced as an
    // import diagnostic — never guessed into a wrong kind.
    if (*source == DesignSource::figma_plugin || *source == DesignSource::figma) {
        auto resolver = pulp::view::RecognitionResolver::with_builtin_library();
        if (!recognition_manifest_path.empty()) {
            const std::string manifest_json = read_file(recognition_manifest_path);
            if (manifest_json.empty()) {
                std::cerr << "Error: could not read --recognition-manifest "
                          << recognition_manifest_path << "\n";
                return 2;
            }
            std::string parse_err;
            auto user_source = pulp::view::RecognitionResolver::parse_manifest_json(
                manifest_json, "user-manifest", &parse_err);
            if (!user_source) {
                std::cerr << "Error: invalid --recognition-manifest "
                          << recognition_manifest_path << ": " << parse_err << "\n";
                return 2;
            }
            resolver.add_source(std::move(*user_source));
        }

        // Installed-package custom controls: gather each installed
        // package's `design_controls` fragment and add it as its own source,
        // MERGED OVER the built-in library and the user manifest. With no
        // custom-control package installed this contributes nothing, so behavior
        // is unchanged. Discovery walks up from the design's directory for a
        // packages.lock.json / registry.json pair (a project root).
        {
            // Anchor discovery at the OUTPUT location (the user's project), not
            // the input file (which may be a temp/extracted export). Fall back to
            // cwd when no output directory is set (dry-run / stdout).
            fs::path search_start = fs::path(output_file).parent_path();
            if (search_start.empty()) search_start = fs::current_path();
            auto pkg_sources =
                pulp::view::discover_package_design_controls(search_start);
            for (auto& w : pkg_sources.warnings)
                std::cerr << "recognition: " << w << "\n";
            for (auto& s : pkg_sources.sources) {
                const auto entry_count = s.entries.size();
                const std::string pkg_name = s.name;
                resolver.add_source(std::move(s));
                std::cerr << "recognition: merged " << entry_count
                          << " custom control"
                          << (entry_count == 1 ? "" : "s") << " from package '"
                          << pkg_name << "'\n";
            }
        }

        std::vector<pulp::view::UnmatchedComponent> unmatched;
        const int wired = pulp::view::apply_recognition_resolver(
            ir.root, resolver, &unmatched);
        // Materialize half: turn resolved custom-control matches (stamped as the
        // `recognitionFactoryId` node attribute) into kind=custom interactive
        // elements the native materializer builds — or renders inert + diagnoses
        // when the factory isn't registered (never a silent knob).
        pulp::view::materialize_recognized_custom_controls(ir.root);
        if (wired > 0)
            std::cerr << "recognition: wired " << wired
                      << " control" << (wired == 1 ? "" : "s")
                      << " from the recognition manifest set\n";
        // Surface present-but-unmatched component instances so they are SEEN
        // (a candidate for a --recognition-manifest entry), never silently
        // rendered inert (P7 never-silent-knob). Recorded in the IR diagnostics
        // AND printed directly to stderr — the shared print helper drops `info`
        // severity, and a missing knob the user must notice is not "info noise".
        for (const auto& u : unmatched) {
            ir.diagnostics.push_back(make_cli_diagnostic(
                ImportDiagnosticSeverity::info,
                ImportDiagnosticKind::unknown,
                "unmapped-component",
                "<component>",
                "component '" + u.name + "' (key " + u.component_key +
                    ") is present in the design but not mapped by any "
                    "recognition manifest; add it to --recognition-manifest "
                    "to wire it as a control"));
            std::cerr << "recognition: unmapped component '" << u.name
                      << "' (key " << u.component_key << ") — present in the "
                      << "design but mapped by no recognition manifest; add it "
                      << "to --recognition-manifest to wire it as a control\n";
        }
    }

    // Out-of-band host-param binding: apply a --param-binding-manifest (figma
    // node id → param_key) to the parsed IR. This binds DESCRIPTIVELY-named
    // geometry controls (the common Figma case — a knob layer named "Cutoff")
    // that carry provenance but no layer-name sigil; an explicit sigil still wins.
    // Applied BEFORE the import report so its provenance reflects the bound state.
    if (!param_binding_manifest_path.empty()) {
        const std::string manifest_json = read_file(param_binding_manifest_path);
        if (manifest_json.empty()) {
            std::cerr << "Error: could not read --param-binding-manifest "
                      << param_binding_manifest_path << "\n";
            return 2;
        }
        std::string parse_err;
        auto bindings = pulp::view::parse_param_binding_manifest_json(
            manifest_json, &parse_err);
        if (!bindings) {
            std::cerr << "Error: invalid --param-binding-manifest "
                      << param_binding_manifest_path << ": " << parse_err << "\n";
            return 2;
        }
        const int bound =
            pulp::view::apply_param_binding_manifest(ir.root, *bindings);
        std::cerr << "param-binding: bound " << bound << " control"
                  << (bound == 1 ? "" : "s") << " from "
                  << param_binding_manifest_path << "\n";
    }

    // P7 import report — surface every interactive control's resolution provenance
    // (rung / confidence / conflicts / verification) for EVERY output mode (codegen
    // and DesignIR-v1 alike), so a low-confidence or conflicted control is SEEN at
    // import time. Printed to stderr (stdout may carry dry-run JSON);
    // --import-report writes the machine-readable JSON a CI gate can threshold;
    // --fail-on-unresolved makes a conflicted/inert control a nonzero exit.
    // P7 render-placement verification (structural): flag overlays that can't
    // render (degenerate extent) or fall entirely outside the frame, BEFORE the
    // report collects verification_pass — so the report and the gate see it.
    apply_placement_verification(ir.root,
                                 ir.root.style.width.value_or(0.0f),
                                 ir.root.style.height.value_or(0.0f));
    // Swap-target verification: a swap-link button whose target frame was never
    // captured would render as a button that silently does nothing, so flag it
    // here — before the report collects verification_pass — and let the same
    // --import-report / --fail-on-unresolved channel carry it.
    apply_swap_target_verification(ir.root);

    // Captured states nobody can render are a hard error, not a diagnostic. Only
    // a faithful_svg node lowers alternate_frames to DesignFrameView::add_frame,
    // so on any other node the extra states are dropped and the import "succeeds"
    // with a single frame — the user asked for N states and silently got one.
    // Refusing here is what keeps that from shipping as a working command.
    if (const auto dropped = find_unrenderable_alternate_frames(ir.root); !dropped.empty()) {
        std::size_t total = 0;
        for (const auto& d : dropped) total += d.alternates;
        std::cerr << "Error: " << total << " captured state"
                  << (total == 1 ? "" : "s") << " cannot be rendered and would be "
                     "dropped:\n";
        for (const auto& d : dropped)
            std::cerr << "       - " << d.node_name << ": " << d.alternates
                      << " alternate frame" << (d.alternates == 1 ? "" : "s")
                      << " — " << d.reason << "\n";
        std::cerr << "       Multi-state capture needs a faithful-vector export. The Figma "
                     "REST lane produces one:\n"
                     "         python3 tools/import-design/figma_rest_export.py --file-key "
                     "<KEY> --node <A> --out a.pulp.json --faithful-vector\n"
                     "         python3 tools/import-design/figma_rest_export.py --file-key "
                     "<KEY> --node <B> --out b.pulp.json --faithful-vector\n"
                     "         pulp import-design --from figma-plugin --file a.pulp.json "
                     "--file b.pulp.json ...\n";
        return 2;
    }
    const auto import_report = collect_import_report(ir.root);
    if (!import_report.controls.empty())
        std::cerr << import_report_to_text(import_report);
    if (!import_report_path.empty() &&
        !write_file(import_report_path, import_report_to_json(import_report)))
        std::cerr << "warning: could not write import report to "
                  << import_report_path << "\n";
    const int report_exit = (fail_on_unresolved && !import_report.ok()) ? 2 : 0;

    if (artifact_emit == ArtifactEmit::ir_json) {
        const auto asset_options = make_asset_options(input_file,
                                                      input_url,
                                                      allow_network_fetch,
                                                      asset_timeout_ms,
                                                      asset_cache_dir,
                                                      expected_asset_hashes);
        refresh_design_ir_asset_manifest(ir, asset_options);
        print_asset_manifest_diagnostics(ir.asset_manifest);
        if (has_blocking_asset_diagnostic(ir.asset_manifest)) return 1;

        const auto ir_json = serialize_design_ir(ir);
        if (dry_run) {
            std::cout << ir_json << "\n";
            return report_exit;
        }
        if (!write_file(output_file, ir_json)) return 1;
        if (pulp_zip_keepalive) finalize_pulp_zip_sidecar(*pulp_zip_keepalive);
        std::cout << "Wrote " << output_file << " (DesignIR v1, "
                  << ir.asset_manifest.assets.size() << " asset"
                  << (ir.asset_manifest.assets.size() == 1 ? "" : "s")
                  << ")\n";
        return report_exit;
    }

    if (artifact_emit == ArtifactEmit::cpp) {
        const auto asset_options = make_asset_options(input_file,
                                                      input_url,
                                                      allow_network_fetch,
                                                      asset_timeout_ms,
                                                      asset_cache_dir,
                                                      expected_asset_hashes);
        refresh_design_ir_asset_manifest(ir, asset_options);
        print_asset_manifest_diagnostics(ir.asset_manifest);
        if (has_blocking_asset_diagnostic(ir.asset_manifest)) return 1;
        enrich_imported_image_asset_metadata(
            ir,
            ir.asset_manifest,
            asset_options.base_directory.string());

        const auto paths = resolve_cpp_output_paths(output_file);
        CppExportOptions cpp_opts;
        cpp_opts.header_filename = paths.include_name;
        cpp_opts.include_comments = include_comments;
        cpp_opts.emit_named_tokens = include_tokens;
        cpp_opts.emit_asset_constants = true;

        const auto cpp = generate_pulp_cpp(ir, ir.asset_manifest, cpp_opts);
        if (dry_run) {
            std::cout << "=== Generated Pulp C++ header (" << paths.header.string() << ") ===\n\n";
            std::cout << cpp.header;
            std::cout << "\n=== Generated Pulp C++ source (" << paths.source.string() << ") ===\n\n";
            std::cout << cpp.source;
            std::cout << "\n=== Generated Pulp C++ binding manifest (" << paths.binding_manifest.string() << ") ===\n\n";
            std::cout << cpp.binding_manifest;
            return report_exit;   // honor --fail-on-unresolved on the cpp dry-run path
        }

        if (!write_files_atomically({
                {paths.header.string(), cpp.header},
                {paths.source.string(), cpp.source},
                {paths.binding_manifest.string(), cpp.binding_manifest},
            })) {
            return 1;
        }
        if (pulp_zip_keepalive) finalize_pulp_zip_sidecar(*pulp_zip_keepalive);

        const auto counts = count_design_ir_elements(ir.root);

        std::cout << "Wrote " << paths.source.string() << ", "
                  << paths.header.string() << ", and "
                  << paths.binding_manifest.string() << " (" << counts.nodes << " elements: "
                  << counts.containers << " containers, " << counts.widgets << " widgets, "
                  << counts.text << " labels, " << ir.asset_manifest.assets.size() << " asset"
                  << (ir.asset_manifest.assets.size() == 1 ? "" : "s") << ")\n";
        return report_exit;   // honor --fail-on-unresolved on the cpp write path
    }

    if (artifact_emit == ArtifactEmit::swiftui) {
        const auto asset_options = make_asset_options(input_file,
                                                      input_url,
                                                      allow_network_fetch,
                                                      asset_timeout_ms,
                                                      asset_cache_dir,
                                                      expected_asset_hashes);
        refresh_design_ir_asset_manifest(ir, asset_options);
        print_asset_manifest_diagnostics(ir.asset_manifest);
        if (has_blocking_asset_diagnostic(ir.asset_manifest)) return 1;

        const auto paths = resolve_swift_output_paths(output_file);
        SwiftExportOptions swift_opts;
        swift_opts.root_view_name = paths.root_view_name;
        swift_opts.theme_type_name = paths.theme_type_name;
        swift_opts.include_comments = include_comments;
        swift_opts.emit_theme = include_tokens;
        swift_opts.emit_binding_manifest = true;

        // B2 fidelity: the SwiftUI lowering reports each divergence a SwiftUI
        // stack cannot reproduce (flex-wrap, justify distribution, align-
        // stretch, absolute position, grid, skew/matrix transforms, per-side
        // borders, multi/inset shadows). Surface them like the JS path and let
        // --strict-fidelity gate on the non-informational ones.
        std::vector<pulp::view::FidelityIssue> swift_fidelity;
        swift_opts.fidelity_report = &swift_fidelity;

        const auto swift = generate_pulp_swift(ir, ir.asset_manifest, swift_opts);

        for (const auto& fi : swift_fidelity) {
            std::cerr << "fidelity: [" << fi.kind << "] " << fi.node_name
                      << " (" << fi.node_id << "): " << fi.detail
                      << (fi.informational ? "  [informational]" : "") << "\n";
        }
        const std::size_t swift_hard =
            pulp::view::count_strict_fidelity_failures(swift_fidelity);
        const bool swift_fidelity_failed = strict_fidelity && swift_hard > 0;
        if (swift_fidelity_failed)
            std::cerr << "fidelity: " << swift_hard
                      << " issue(s); failing due to --strict-fidelity\n";

        if (dry_run) {
            std::cout << "=== Generated SwiftUI view (" << paths.view.string() << ") ===\n\n";
            std::cout << swift.view_source;
            if (!swift.theme_source.empty()) {
                std::cout << "\n=== Generated PulpTheme (" << paths.theme.string() << ") ===\n\n";
                std::cout << swift.theme_source;
            }
            std::cout << "\n=== SwiftUI binding manifest (" << paths.binding_manifest.string() << ") ===\n\n";
            std::cout << swift.binding_manifest;
            return swift_fidelity_failed ? 4 : 0;
        }
        if (swift_fidelity_failed) return 4;

        if (!write_file(paths.view.string(), swift.view_source)) return 1;
        if (!swift.theme_source.empty() &&
            !write_file(paths.theme.string(), swift.theme_source)) return 1;
        if (!write_file(paths.binding_manifest.string(), swift.binding_manifest)) return 1;

        const auto counts = count_design_ir_elements(ir.root);
        std::cout << "Wrote " << paths.view.string();
        if (!swift.theme_source.empty()) std::cout << ", " << paths.theme.string();
        std::cout << ", and " << paths.binding_manifest.string()
                  << " (" << counts.nodes << " elements: "
                  << counts.containers << " containers, " << counts.widgets << " widgets, "
                  << counts.text << " labels)\n";
        return 0;
    }

    // Generate Pulp JS
    CodeGenOptions opts;
    opts.mode = use_web_compat ? CodeGenMode::web_compat : CodeGenMode::bridge_native_js;
    opts.include_tokens = include_tokens;
    opts.include_comments = include_comments;
    opts.preview_mode = preview_mode;
    opts.use_silver_knobs = use_silver_knobs;
    opts.skin_faders = skin_faders;
    opts.skin_meters = skin_meters;

    // Auto-import keyboard shortcuts from the source.
    // Default-on. Source-agnostic helper: the extractor takes a raw
    // TSX/JS/HTML string and regex-scans for `e.key === '…'` patterns,
    // so all source types (claude, v0, figma code blobs, stitch inline
    // JS, pencil) can route through the same call without per-source
    // branching here.
    std::vector<DetectedShortcut> detected_shortcuts;
    DefaultShortcutScan default_scan;
    if (import_shortcuts) {
        detected_shortcuts = extract_keyboard_shortcuts(content, input_file);

        // Default shortcuts only fire when the developer's React source has a
        // high-confidence match. `apply_default_shortcuts` lowers
        // accepted DefaultShortcutCandidates into the same DetectedShortcut
        // form so they ride V2's codegen path with no fork. Suppressed
        // chord-by-chord against `detected_shortcuts` so an extracted
        // binding always wins.
        //
        // The import CLI runs at build time, but the generated ui.js ships to
        // many platforms (mac standalone, win
        // standalone, plugin hosts on either). Emit BOTH macOS and
        // Win/Linux variants — at runtime only the chord matching the
        // physical key press fires its registerShortcut entry, so the
        // user gets the right native binding on each platform without
        // platform detection at codegen time. Mirrors the V2 dual emit
        // for `metaKey||ctrlKey` (per-platform handlers, exact-mask
        // match on the bridge side).
        if (default_shortcuts) {
            default_scan = detect_default_shortcuts(content, detected_shortcuts);
            auto mac_defaults = apply_default_shortcuts(
                default_scan.accepted, TargetPlatform::macos);
            auto win_defaults = apply_default_shortcuts(
                default_scan.accepted, TargetPlatform::win_linux);
            for (auto& d : mac_defaults) detected_shortcuts.push_back(std::move(d));
            // Skip Win/Linux variants whose chord (key + mask) already
            // came in via the mac pass — happens for keys without a
            // platform delta (e.g. bare `?` for cheatsheet emits the
            // same binding under both platforms).
            for (auto& d : win_defaults) {
                bool dup = false;
                for (const auto& existing : detected_shortcuts) {
                    if (existing.key == d.key && existing.modifiers == d.modifiers) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) detected_shortcuts.push_back(std::move(d));
            }
        }

        opts.shortcuts = detected_shortcuts;
    }

    // Sprite/knob/fader/meter skin + asset-path resolution (sprite_skins.cpp):
    // hoists captured knob body art onto recognized knobs, resolves asset_ref →
    // absolute asset_path, stamps true PNG dims / opaque-core / asset-bleed
    // metadata, derives sampled fader+meter skins, and resolves bundled fonts.
    pulp::import_design::resolve_sprite_skins(
        ir, input_file, use_silver_knobs, skin_faders, skin_meters);

    std::vector<pulp::view::FidelityIssue> fidelity_issues;
    opts.fidelity_report = &fidelity_issues;
    auto js = generate_pulp_js(ir, opts);

    if (auto fidelity_exit = run_fidelity_selfcheck(fidelity_issues,
                                                    strict_fidelity,
                                                    fidelity_report_path,
                                                    *source,
                                                    output_file,
                                                    fidelity_failed))
        return *fidelity_exit;

    if (dry_run) {
        std::cout << "=== Generated Pulp JS (" << design_source_name(*source) << " → " << output_file << ") ===\n\n";
        std::cout << js;

        if (include_tokens && (!ir.tokens.colors.empty() || !ir.tokens.dimensions.empty())) {
            auto theme = ir_tokens_to_theme(ir.tokens);
            auto body = export_theme_tokens(export_format, theme);
            const char* label = (export_format == "css-variables")
                                    ? "CSS Variables" : "W3C Design Tokens";
            std::cout << "\n=== " << label << " (" << tokens_file << ") ===\n\n";
            std::cout << body;
        }
        // --dry-run still honors --strict-fidelity: a harness that imports with
        // both must see the non-zero exit, not a silent success.
        return fidelity_failed ? 4 : 0;
    }

    auto t_codegen = std::chrono::steady_clock::now();

    // Write output files. DESIGN.md describes a system, not a screen —
    // there is no UI tree to scaffold, so skip the ui.js write entirely
    // and emit only tokens.json. Future work may add a `--with-scaffold`
    // flag once name-based widget detection is consistent across sources.
    if (*source != DesignSource::designmd) {
        if (!write_file(output_file, js)) return 1;

        // Emit a <output>.meta.json sidecar with the root frame's canvas
        // size + design source. Lets downstream renderers (pulp-screenshot,
        // tools/scripts/render-figma-import.sh) auto-pick --width/--height
        // instead of requiring the caller to remember them.
        float root_w = ir.root.style.width.value_or(0.0f);
        float root_h = ir.root.style.height.value_or(0.0f);
        if (root_w > 0.0f && root_h > 0.0f) {
            fs::path meta_path = fs::path(output_file).string() + ".meta.json";
            std::ostringstream meta;
            meta << "{\n"
                 << "  \"canvas\": { \"width\": " << static_cast<int>(root_w)
                 << ", \"height\": " << static_cast<int>(root_h) << " },\n"
                 << "  \"source\": \"" << design_source_name(*source) << "\",\n"
                 << "  \"script\": \"" << fs::path(output_file).filename().string() << "\"\n"
                 << "}\n";
            (void)write_file(meta_path.string(), meta.str());
        }
        if (pulp_zip_keepalive) finalize_pulp_zip_sidecar(*pulp_zip_keepalive);
    }

    // Count elements by type
    const auto counts = count_design_ir_elements(ir.root);

    auto t_write = std::chrono::steady_clock::now();
    if (*source == DesignSource::designmd) {
        std::cout << "DESIGN.md → tokens only (no ui.js; system spec, not screen)";
    } else {
        std::cout << "Wrote " << output_file << " (" << counts.nodes << " elements: "
                  << counts.containers << " containers, " << counts.widgets << " widgets, "
                  << counts.text << " labels";
    }

    // Write tokens (W3C DTCG by default; --format json-tailwind /
    // css-tailwind selects Tailwind v3 JSON or v4 CSS, but only when the
    // source is `designmd` because the parser-produced section/
    // diagnostic context is required for sensible Tailwind shape).
    if (include_tokens && (!ir.tokens.colors.empty() || !ir.tokens.dimensions.empty() || !ir.tokens.strings.empty())) {
        std::string body;
        if ((export_format == "json-tailwind" || export_format == "tailwind" ||
             export_format == "css-tailwind") && *source == DesignSource::designmd) {
            auto pr = parse_designmd(content);
            body = (export_format == "css-tailwind")
                       ? export_tailwind_v4_css(pr)
                       : export_tailwind_v3_json(pr);
        } else {
            auto theme = ir_tokens_to_theme(ir.tokens);
            body = export_theme_tokens(export_format, theme);
        }
        if (write_file(tokens_file, body)) {
            size_t token_count = ir.tokens.colors.size() + ir.tokens.dimensions.size() + ir.tokens.strings.size();
            std::cout << ", " << token_count << " tokens → " << tokens_file
                      << " (format=" << export_format << ")";
        }
    }

    if (*source == DesignSource::designmd) {
        std::cout << "\n";
    } else {
        std::cout << ")\n";
    }

    // Bridge handler scaffold for Claude Design imports.
    // Only emitted for --from claude; other sources keep their existing
    // output shape unchanged.
    if (*source == DesignSource::claude && emit_bridge_scaffold) {
        const auto scaffold = render_claude_bridge_scaffold(output_file);
        if (write_file(bridge_output, scaffold)) {
            std::cout << "Wrote " << bridge_output
                      << " (bridge handler scaffold — edit add_handler() entries to wire your editor's messages)\n";
        }
    }

    // Classnames artifact for Claude Design imports.
    // Spectr's `tools/extract-html-bundle/extract.mjs` emits the same
    // map by hand; pulling it into the CLI lets `@pulp/css-adapt`
    // consume the file directly without a separate Node-side pass.
    // Only emitted for --from claude; default on, opt-out via
    // --no-emit-classnames.
    if (*source == DesignSource::claude && emit_classnames) {
        auto rules = extract_claude_classnames(content);
        const auto classnames_json = serialize_claude_classnames(rules);
        if (write_file(classnames_output, classnames_json)) {
            std::cout << "Wrote " << classnames_output
                      << " (" << rules.size() << " class rule"
                      << (rules.size() == 1 ? "" : "s")
                      << " — feed to @pulp/css-adapt or dom-adapter)\n";
        }
    }

    // Shortcuts manifest alongside classnames. Mirror shape so a reviewer can
    // audit what the auto-import will bind. The
    // generated ui.js already contains the matching registerShortcut(...)
    // calls; this file is for human/CI audit.
    if (import_shortcuts && !detected_shortcuts.empty()) {
        const auto shortcuts_json = serialize_detected_shortcuts(detected_shortcuts);
        if (write_file(shortcuts_output, shortcuts_json)) {
            // `default_scan.accepted` is the count of UI surfaces matched
            // (one per Settings/Help/Cheatsheet/…). Each accepted surface
            // emits up to TWO actual bindings (mac chord + win/linux
            // variant) so the count of default-tagged DetectedShortcuts
            // can be up to 2× the accepted-surfaces count.
            size_t default_count = 0;
            for (const auto& s : detected_shortcuts) {
                if (s.pattern.rfind("default:", 0) == 0) ++default_count;
            }
            const size_t extracted_count = detected_shortcuts.size() - default_count;
            std::cout << "Wrote " << shortcuts_output
                      << " (" << detected_shortcuts.size() << " shortcut"
                      << (detected_shortcuts.size() == 1 ? "" : "s")
                      << " — " << extracted_count << " extracted, "
                      << default_count << " platform-default"
                      << " — bound natively via registerShortcut())\n";
        }
    }

    // Diagnostic dump of the defaults scan alongside the bound manifest.
    // Writes even when no defaults fired, so a reviewer can see
    // *why* (collisions, low confidence). Mirror naming convention.
    if (import_shortcuts && default_shortcuts &&
        (!default_scan.accepted.empty() || !default_scan.collisions.empty())) {
        std::string defaults_path = shortcuts_output;
        const auto dot = defaults_path.rfind('.');
        defaults_path = (dot == std::string::npos)
            ? defaults_path + ".defaults.json"
            : defaults_path.substr(0, dot) + ".defaults.json";
        const auto defaults_json = serialize_default_shortcut_scan(default_scan);
        if (write_file(defaults_path, defaults_json)) {
            std::cout << "Wrote " << defaults_path
                      << " (" << default_scan.accepted.size() << " accepted, "
                      << default_scan.collisions.size() << " collisions"
                      << " — Phase A source-matched defaults)\n";
        }
    }

    // Native-react detection (heuristic shared with the lib so tests can
    // exercise it directly; see
    // design_import.hpp::looks_like_bundler_entry). When the static
    // parser produces only a handful of elements AND the HTML looks
    // like a JS-bundler entry, the user almost certainly wanted to run
    // the bundle directly. Soft warning — we still wrote ui.js.
    if (*source == DesignSource::claude && counts.nodes <= 12 &&
        looks_like_bundler_entry(content)) {
        std::cerr << "\n"
                  << "Note: this HTML looks like a JS-bundler entry "
                  << "(mount-point + script tag). The static parser "
                  << "only captured the placeholder chrome ("
                  << counts.nodes << " element"
                  << (counts.nodes == 1 ? "" : "s")
                  << ").\n"
                  << "      For native-react / @pulp/react bundles, run "
                  << "the bundle directly:\n"
                  << "          pulp-design-tool --script <bundle>.js\n"
                  << "      (the bundle IS the import artifact — the "
                  << "static HTML pass is for hand-authored Claude "
                  << "Design pages.)\n\n";
    }

    // Screenshot naming convention: {design-name}-{source}-render.png
    auto design_name = fs::path(output_file).stem().string();
    auto source_lower = std::string(design_source_name(*source));
    std::transform(source_lower.begin(), source_lower.end(), source_lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // ── Validation: render generated JS and compare with reference ──────
    if (validate) {
        std::cout << "Validating render...\n";

        // Honesty guard: --validate renders the generated JS (the native-
        // materialized widget tree), NOT the embedded faithful SVG. For a
        // faithful_svg scene the two are different renders — the widget tree can
        // diverge badly (missing custom controls, placeholder art) while the 1:1
        // faithful SVG is pixel-perfect. Reporting the native-materialize
        // similarity without saying so has read as a failed import when the
        // faithful render was fine. Detect faithful nodes and label the number.
        std::function<bool(const pulp::view::IRNode&)> scene_has_faithful =
            [&](const pulp::view::IRNode& node) -> bool {
            if (node.render_mode == pulp::view::NodeRenderMode::faithful_svg)
                return true;
            for (const auto& child : node.children)
                if (scene_has_faithful(child)) return true;
            return false;
        };
        const bool faithful_scene = scene_has_faithful(ir.root);
        if (faithful_scene) {
            std::cout <<
                "  NOTE: this scene uses faithful_svg render mode. --validate renders the\n"
                "  native-materialized widget tree, NOT the 1:1 faithful SVG. The similarity\n"
                "  below is native-materialize fidelity and will UNDERSTATE the true faithful\n"
                "  render. Verify the faithful render with pulp-svg-probe on the embedded SVG\n"
                "  (extract the data:image/svg+xml payload from the scene JSON first).\n";
        }

        // Render at the design's own size unless the caller asked otherwise, so
        // the render and its reference are the same shape by default.
        if (!render_size_explicit) {
            const int design_w = static_cast<int>(ir.root.style.width.value_or(0.0f));
            const int design_h = static_cast<int>(ir.root.style.height.value_or(0.0f));
            if (design_w > 0 && design_h > 0) {
                render_width = design_w;
                render_height = design_h;
            }
        }

        // Render the generated JS headlessly
        View render_root;
        render_root.set_theme(Theme::dark());
        render_root.flex().direction = FlexDirection::column;
        StateStore render_store;
        ScriptEngine render_engine;
        WidgetBridge render_bridge(render_engine, render_root, render_store);
        try {
            render_bridge.load_script(js);
        } catch (const std::exception& e) {
            std::cerr << "Validation error: generated JS failed to load: " << e.what() << "\n";
            return 1;
        }

        auto rendered_png = render_to_png(render_root,
            static_cast<uint32_t>(render_width),
            static_cast<uint32_t>(render_height), 2.0f, screenshot_backend);

        if (rendered_png.empty()) {
            std::cerr << "Validation error: headless render failed\n";
            return 1;
        }

        auto rendered_path = pulp::import_design::render_artifact_path(output_file, design_name + "-" + source_lower + "-render.png");
        {
            std::ofstream f(rendered_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(rendered_png.data()),
                    static_cast<std::streamsize>(rendered_png.size()));
        }
        std::cout << "Rendered → " << rendered_path << " (" << render_width << "x" << render_height << ")\n";

        // Dump the laid-out tree. AFTER the render, deliberately: render_to_png
        // is what runs the layout pass, so dumping before it would serialize a
        // tree of zero-sized views that all agree with each other and with
        // nothing real.
        if (!dump_layout_path.empty()) {
            std::map<std::string, std::string> source_ids;
            collect_anchor_source_ids(ir.root, source_ids);
            auto views = choc::value::createEmptyArray();
            collect_layout_dump(render_root, source_ids, views);

            auto dump = choc::value::createObject("");
            dump.addMember("units", choc::value::createString("design-px"));
            dump.addMember("render_width", choc::value::createInt64(render_width));
            dump.addMember("render_height", choc::value::createInt64(render_height));
            dump.addMember("views", views);

            std::ofstream f(dump_layout_path);
            if (!f) {
                std::cerr << "Error: could not write --dump-layout file "
                          << dump_layout_path << "\n";
                return 1;
            }
            f << choc::json::toString(dump, true) << "\n";
            std::cout << "Layout dump → " << dump_layout_path << " ("
                      << views.size() << " anchored view(s))\n";

            // A dump alone proves nothing — it needs the source's own rects to
            // be diffed against. `--from fig` decodes those into a scratch
            // directory this process deletes on the way out, so copy them next
            // to the dump: that makes the one-shot `--from fig … --dump-layout`
            // produce BOTH halves of a parity run, which is the only reason
            // either file exists.
            if (!fig_geometry_file.empty()) {
                fs::path sidecar = dump_layout_path;
                sidecar.replace_extension();
                sidecar += ".geometry.json";
                std::error_code copy_ec;
                fs::copy_file(fig_geometry_file, sidecar,
                              fs::copy_options::overwrite_existing, copy_ec);
                if (copy_ec) {
                    std::cerr << "Warning: could not copy the source's geometry to "
                              << sidecar << ": " << copy_ec.message() << "\n";
                } else {
                    std::cout << "Source geometry → " << sidecar.string() << "\n";
                }
            }
            // An anchorless dump is useless and looks like a passing parity run,
            // so say what happened rather than writing an empty file quietly.
            if (views.size() == 0) {
                std::cout << "  NOTE: no view carries an anchor, so this dump has nothing to "
                             "join on.\n  Generated JS emits setAnchor() only with comments "
                             "enabled and an IR that\n  resolved anchors — check --no-comments "
                             "and the source's node ids.\n";
            }
        }

        // Compare with reference if provided
        if (!reference_image.empty()) {
            auto result = compare_screenshot_files(reference_image, rendered_path);
            if (!result.valid) {
                std::cerr << "Comparison error: " << result.error << "\n";
                return 1;
            }

            std::cout << "Similarity: " << static_cast<int>(result.similarity * 100) << "% ("
                      << result.diff_pixels << "/" << result.total_pixels << " pixels differ, "
                      << "mean error: " << result.mean_error << ")\n";

            // The advisory label uses the shared default; --fail-below, when
            // given, sets both the label's bar and the exit gate so one number
            // drives what is printed and what is enforced.
            const float gate = fail_below_pct >= 0.0f
                ? fail_below_pct / 100.0f : pulp::view::kDefaultSimilarityThreshold;
            if (result.passes(gate)) {
                std::cout << "Validation: PASS\n";
            } else {
                std::cout << "Validation: NEEDS REVIEW (similarity below "
                          << static_cast<int>(gate * 100.0f) << "%)\n";
                // Only --fail-below turns this into a failure. Absent the flag
                // the exit code stays whatever the rest of the run decided.
                if (fail_below_pct >= 0.0f) similarity_failed = true;
            }

            // Always generate diff image when reference is provided
            // Use --diff path if given, otherwise auto-generate alongside render
            auto actual_diff_path = diff_output.empty()
                ? pulp::import_design::render_artifact_path(output_file, design_name + "-" + source_lower + "-diff.png") : diff_output;
            {
                auto ref_bytes = [&]() -> std::vector<uint8_t> {
                    std::ifstream f(reference_image, std::ios::binary);
                    return {std::istreambuf_iterator<char>(f), {}};
                }();
                auto diff_png = generate_diff_image(ref_bytes, rendered_png);
                if (!diff_png.empty()) {
                    std::ofstream f(actual_diff_path, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(diff_png.data()),
                            static_cast<std::streamsize>(diff_png.size()));
                    std::cout << "Diff image → " << actual_diff_path << "\n";
                }
            }
        }
    }

    // ── Debug JSON report ────────────────────────────────────────────────
    if (debug_json) {
        auto t_end = std::chrono::steady_clock::now();
        auto ms_total = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
        auto ms_codegen = std::chrono::duration_cast<std::chrono::milliseconds>(t_codegen - t_start).count();
        auto ms_write = std::chrono::duration_cast<std::chrono::milliseconds>(t_write - t_codegen).count();
        auto ms_post_codegen = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_codegen).count();
        auto ms_validation = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_write).count();

        std::ostringstream dbg;
        dbg << "{\n";
        dbg << "  \"source\": \"" << design_source_name(*source) << "\",\n";
        dbg << "  \"input_file\": \"" << input_file << "\",\n";
        dbg << "  \"output_file\": \"" << output_file << "\",\n";
        dbg << "  \"mode\": \"" << (use_web_compat ? "web_compat" : "bridge_native_js") << "\",\n";
        dbg << "  \"elements\": {\n";
        dbg << "    \"total\": " << counts.nodes << ",\n";
        dbg << "    \"containers\": " << counts.containers << ",\n";
        dbg << "    \"widgets\": " << counts.widgets << ",\n";
        dbg << "    \"labels\": " << counts.text << "\n";
        dbg << "  },\n";
        dbg << "  \"tokens\": {\n";
        dbg << "    \"colors\": " << ir.tokens.colors.size() << ",\n";
        dbg << "    \"dimensions\": " << ir.tokens.dimensions.size() << ",\n";
        dbg << "    \"strings\": " << ir.tokens.strings.size() << "\n";
        dbg << "  },\n";
        dbg << "  \"timing_ms\": " << ms_total << ",\n";
        dbg << "  \"timing_codegen_ms\": " << ms_codegen << ",\n";
        dbg << "  \"timing_write_ms\": " << ms_write << ",\n";
        dbg << "  \"timing_post_codegen_ms\": " << ms_post_codegen << ",\n";
        dbg << "  \"timing_validation_ms\": " << ms_validation << ",\n";
        dbg << "  \"render_size\": \"" << render_width << "x" << render_height << "\",\n";
        dbg << "  \"js_bytes\": " << js.size() << ",\n";

        // Validation results if available
        if (validate && !reference_image.empty()) {
            auto result = compare_screenshot_files(reference_image, pulp::import_design::render_artifact_path(output_file, design_name + "-" + source_lower + "-render.png"));
            dbg << "  \"validation\": {\n";
            dbg << "    \"reference\": \"" << reference_image << "\",\n";
            dbg << "    \"similarity_pct\": " << static_cast<int>(result.similarity * 100) << ",\n";
            dbg << "    \"diff_pixels\": " << result.diff_pixels << ",\n";
            dbg << "    \"total_pixels\": " << result.total_pixels << ",\n";
            dbg << "    \"mean_error\": " << result.mean_error << ",\n";
            // Same bar as the printed verdict: --fail-below when given, else the
            // shared default. A hardcoded 0.70 here meant the debug JSON could
            // report "pass": true for a render the very same run printed as
            // NEEDS REVIEW — one tool, one run, two answers.
            const float dbg_gate = fail_below_pct >= 0.0f
                ? fail_below_pct / 100.0f : pulp::view::kDefaultSimilarityThreshold;
            dbg << "    \"pass\": " << (result.passes(dbg_gate) ? "true" : "false") << "\n";
            dbg << "  },\n";
        }

        // List unprocessed/unsupported elements
        dbg << "  \"gaps\": [\n";
        bool first_gap = true;
        std::function<void(const IRNode&)> find_gaps = [&](const IRNode& n) {
            // Shapes that aren't audio widgets (not translated to Pulp widgets)
            if ((n.type == "ellipse" || n.type == "rectangle" || n.type == "path" ||
                 n.type == "polygon" || n.type == "line") &&
                n.audio_widget == AudioWidgetType::none &&
                !is_native_widget_node(n)) {
                if (!first_gap) dbg << ",\n";
                first_gap = false;
                dbg << "    {\"type\": \"" << n.type << "\", \"name\": \"" << n.name
                    << "\", \"reason\": \"shape not mapped to widget\"}";
            }
            for (auto& c : n.children) find_gaps(c);
        };
        find_gaps(ir.root);
        dbg << "\n  ]\n";

        dbg << "}\n";

        auto report = dbg.str();
        if (!debug_output.empty()) {
            write_file(debug_output, report);
            std::cout << "Debug report → " << debug_output << "\n";
        } else {
            std::cout << "\n" << report;
        }
    }

    // --strict-fidelity: a self-check finding fails the import (distinct exit
    // code so callers/harness can tell it apart from a parse/IO error).
    if (pulp_zip_keepalive) finalize_pulp_zip_sidecar(*pulp_zip_keepalive);
    if (fidelity_failed) return 4;
    // --fail-below: the render missed the reference bar. Distinct from 4
    // (import-time self-check) because this is a render-vs-reference verdict a
    // caller may want to triage differently.
    if (similarity_failed) return 5;
    return report_exit;  // 0, or 2 under --fail-on-unresolved with a conflicted/inert control
}
