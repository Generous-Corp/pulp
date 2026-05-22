#include <pulp/view/design_import.hpp>
#include <pulp/view/design_export.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/state/store.hpp>
#include "import_detect.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <cstring>
#include <filesystem>
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
    cpp
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
    return std::nullopt;
}

std::optional<RuntimeMode> parse_runtime_mode_pref(const std::string& raw) {
    const auto value = normalize_pref_value(raw);
    if (value == "live") return RuntimeMode::live;
    if (value == "baked") return RuntimeMode::baked;
    return std::nullopt;
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
        && (out.emit == ArtifactEmit::ir_json || out.emit == ArtifactEmit::cpp)) {
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
    paths.include_name = paths.header.filename().string();
    return paths;
}

} // namespace

static void print_usage() {
    std::cout << "pulp import-design — Import designs from external tools into Pulp\n\n";
    std::cout << "Usage:\n";
    std::cout << "  pulp import-design --from <source> [options]\n\n";
    std::cout << "Sources:\n";
    std::cout << "  figma    Figma export JSON or MCP data\n";
    std::cout << "  stitch   Google Stitch screen HTML or MCP data\n";
    std::cout << "  v0       v0.dev TSX/Tailwind output\n";
    std::cout << "  pencil   Pencil/OpenPencil node JSON or .pen export\n";
    std::cout << "  claude   Anthropic Claude Design — manually-exported standalone HTML\n";
    std::cout << "  designmd Google DESIGN.md design-system spec (tokens only)\n";
    std::cout << "  jsx      Precompiled React JSX runtime bundle for live pass-through or baked snapshots\n\n";
    std::cout << "Options:\n";
    std::cout << "  --from <source>   Design source (required)\n";
    std::cout << "  --file <path>     Input file path\n";
    std::cout << "  --url <url>       Design URL (Figma file URL or v0 share link)\n";
    std::cout << "  --frame <name>    Frame/artboard to import (Figma)\n";
    std::cout << "  --screen <name>   Screen to import (Stitch)\n";
    std::cout << "  --output <path>   Destination file for the primary artifact (default: ui.js)\n";
    std::cout << "  --emit {js|ir-json|cpp}\n";
    std::cout << "                    Primary artifact kind (built-in default: js)\n";
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
    std::cout << "  --tokens <path>   Output W3C token file (default: tokens.json)\n";
    std::cout << "  --dry-run         Show generated code without writing files\n";
    std::cout << "  --no-tokens       Skip token extraction\n";
    std::cout << "  --no-comments     Omit comments from generated code\n";
    std::cout << "  --web-compat      Use DOM API instead of native Pulp API\n";
    std::cout << "  --validate        Render generated JS and validate layout\n";
    std::cout << "  --reference <png> Compare render against a reference screenshot\n";
    std::cout << "  --diff <png>      Save visual diff image\n";
    std::cout << "  --render-size WxH Render dimensions (default: 340x280)\n";
    std::cout << "  --bridge-output <path>  Path to write bridge handler scaffold (default: bridge_handlers.cpp,\n";
    std::cout << "                          only emitted for --from claude)\n";
    std::cout << "  --no-bridge-scaffold    Skip bridge handler scaffold (claude only)\n";
    std::cout << "  --classnames <path>     Output classname → style map (default: classnames.json,\n";
    std::cout << "                          only emitted for --from claude — pulp #1035)\n";
    std::cout << "  --emit classnames       Legacy sidecar: force-emit classnames.json (claude)\n";
    std::cout << "  --no-emit-classnames    Skip classname emission (claude only)\n";
    std::cout << "  --shortcuts <path>      Output keyboard-shortcut manifest (default: shortcuts.json)\n";
    std::cout << "  --no-import-shortcuts   Skip keyboard shortcut auto-import (default: import)\n";
    std::cout << "  --no-default-shortcuts  Skip platform-convention defaults (Settings=Cmd+,, etc.) (default: enabled)\n";
    std::cout << "  --execute-bundle  Run the bundled React app in a headless JS engine and\n";
    std::cout << "                    walk the materialized DOM (--from claude only).\n";
    std::cout << "                    Falls back to the static parser on any harness failure.\n";
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
    std::cout << "Examples:\n";
    std::cout << "  pulp import-design --from figma --file design.json\n";
    std::cout << "  pulp import-design --from figma --url 'https://figma.com/design/...' --frame 'Plugin UI'\n";
    std::cout << "  pulp import-design --from stitch --file screen.html --screen 'Main'\n";
    std::cout << "  pulp import-design --from v0 --url 'https://v0.dev/t/abc123' --output my-ui.js\n";
    std::cout << "  pulp import-design --from pencil --file design.json --dry-run\n";
    std::cout << "  pulp import-design --from pencil --file design.json --validate --reference source.png\n";
    std::cout << "  pulp import-design --from claude --file design.html\n";
    std::cout << "  pulp import-design --from jsx --file bundle.js --mode live --emit js --output live-ui.js\n";
    std::cout << "  pulp import-design --from jsx --file bundle.js --mode baked --emit cpp --output imported_ui.cpp\n";
}

// Bridge-handler scaffold body lives in core/view/src/design_import.cpp
// (`render_claude_bridge_scaffold`) so it can be unit-tested directly
// from the design_import test target — coverage doesn't follow CLI
// subprocess invocations, so keeping the body here would leave it
// uncovered. The CLI only calls into the library function below.

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "Error: cannot open file: " << path << "\n";
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_file(const std::string& path, const std::string& content) {
    // Create parent directories if needed
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "Error: cannot write file: " << path << "\n";
        return false;
    }
    f << content;
    return true;
}

int main(int argc, char* argv[]) {
    std::string source_str;
    std::string input_file;
    std::string input_url;           // --url: Figma file URL or v0 share link
    std::string frame_name;          // --frame: Figma frame/artboard name
    std::string screen_name;         // --screen: Stitch screen name
    std::string output_file = "ui.js";
    std::string tokens_file = "tokens.json";
    std::string export_format = "w3c";
    std::string reference_image;     // --reference: PNG of source design for validation
    std::string diff_output;         // --diff: output path for visual diff image
    bool dry_run = false;
    bool include_tokens = true;
    bool include_comments = true;
    bool export_tokens_mode = false;
    bool validate = false;           // --validate: render + compare after import
    bool use_web_compat = false;     // --web-compat: use DOM API instead of native
    bool preview_mode = false;       // --preview: minimal widget style for design comparison
    bool debug_json = false;         // --debug: output JSON report with all metrics
    std::string debug_output;        // --debug-output: path for JSON report
    int render_width = 340;
    int render_height = 280;
    std::string bridge_output = "bridge_handlers.cpp";  // claude scaffold output
    bool bridge_output_explicit = false;                 // pulp friction-fix #4
    bool emit_bridge_scaffold = true;                    // default on for --from claude
    bool execute_bundle = false;                         // pulp #468 native-runtime path
    std::string classnames_output = "classnames.json";   // pulp #1035 — claude classname map
    bool classnames_output_explicit = false;             // pulp friction-fix #4
    bool emit_classnames = true;                          // default on for --from claude
    // pulp #2116 V2 — keyboard shortcuts auto-imported from source.
    // Default-on; opt out with --no-import-shortcuts.
    std::string shortcuts_output = "shortcuts.json";
    bool shortcuts_output_explicit = false;
    bool import_shortcuts = true;
    // Phase A defaults — auto-bind platform conventions (Cmd+, → Settings,
    // etc.) when the source has a high-confidence component match. Default-on;
    // `--no-default-shortcuts` opts out without affecting the source-extracted
    // path above.
    bool default_shortcuts = true;
    bool output_explicit = false;                         // pulp friction-fix #4
    bool tokens_file_explicit = false;                    // pulp friction-fix #4
    // pulp #1031 — versioned detect surface
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

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            source_str = argv[++i];
        } else if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            input_url = argv[++i];
        } else if (std::strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
            frame_name = argv[++i];
        } else if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            screen_name = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = argv[++i];
            output_explicit = true;
        } else if (std::strcmp(argv[i], "--tokens") == 0 && i + 1 < argc) {
            tokens_file = argv[++i];
            tokens_file_explicit = true;
        } else if (std::strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (std::strcmp(argv[i], "--no-tokens") == 0) {
            include_tokens = false;
        } else if (std::strcmp(argv[i], "--no-comments") == 0) {
            include_comments = false;
        } else if (std::strcmp(argv[i], "--export-tokens") == 0) {
            export_tokens_mode = true;
        } else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            export_format = argv[++i];
        } else if (std::strcmp(argv[i], "--web-compat") == 0) {
            use_web_compat = true;
        } else if (std::strcmp(argv[i], "--validate") == 0) {
            validate = true;
        } else if (std::strcmp(argv[i], "--reference") == 0 && i + 1 < argc) {
            reference_image = argv[++i];
            validate = true;
        } else if (std::strcmp(argv[i], "--diff") == 0 && i + 1 < argc) {
            diff_output = argv[++i];
        } else if (std::strcmp(argv[i], "--render-size") == 0 && i + 1 < argc) {
            // Parse WxH
            std::string sz = argv[++i];
            auto x = sz.find('x');
            if (x != std::string::npos) {
                render_width = std::stoi(sz.substr(0, x));
                render_height = std::stoi(sz.substr(x + 1));
            }
        } else if (std::strcmp(argv[i], "--preview") == 0) {
            preview_mode = true;
        } else if (std::strcmp(argv[i], "--debug") == 0) {
            debug_json = true;
        } else if (std::strcmp(argv[i], "--debug-output") == 0 && i + 1 < argc) {
            debug_output = argv[++i];
            debug_json = true;
        } else if (std::strcmp(argv[i], "--bridge-output") == 0 && i + 1 < argc) {
            bridge_output = argv[++i];
            bridge_output_explicit = true;
        } else if (std::strcmp(argv[i], "--no-bridge-scaffold") == 0) {
            emit_bridge_scaffold = false;
        } else if (std::strcmp(argv[i], "--execute-bundle") == 0) {
            execute_bundle = true;
        } else if (std::strcmp(argv[i], "--classnames") == 0 && i + 1 < argc) {
            classnames_output = argv[++i];
            classnames_output_explicit = true;
        } else if (std::strcmp(argv[i], "--emit") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --emit requires a value: js, ir-json, cpp, or classnames\n";
                return 2;
            }
            std::string what = argv[++i];
            if (what == "js") {
                artifact_emit = ArtifactEmit::js;
                artifact_emit_explicit = true;
            } else if (what == "ir-json") {
                artifact_emit = ArtifactEmit::ir_json;
                artifact_emit_explicit = true;
            } else if (what == "cpp") {
                artifact_emit = ArtifactEmit::cpp;
                artifact_emit_explicit = true;
            } else if (what == "classnames") {
                emit_classnames = true;
            } else {
                std::cerr << "Error: unsupported --emit value '" << what
                          << "' (expected js, ir-json, cpp, or classnames)\n";
                return 2;
            }
        } else if (std::strcmp(argv[i], "--mode") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --mode requires a value: live or baked\n";
                return 2;
            }
            std::string mode = argv[++i];
            if (mode == "live") {
                runtime_mode = RuntimeMode::live;
                runtime_mode_explicit = true;
            } else if (mode == "baked") {
                runtime_mode = RuntimeMode::baked;
                runtime_mode_explicit = true;
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
                snapshot_semantics = SnapshotSemantics::fail;
            } else if (semantics == "warn") {
                snapshot_semantics = SnapshotSemantics::warn;
            } else if (semantics == "accept") {
                snapshot_semantics = SnapshotSemantics::accept;
            } else {
                std::cerr << "Error: unsupported --snapshot-semantics value '" << semantics
                          << "' (expected fail, warn, or accept)\n";
                return 2;
            }
        } else if (std::strcmp(argv[i], "--allow-network-fetch") == 0) {
            allow_network_fetch = true;
        } else if (std::strcmp(argv[i], "--asset-cache") == 0 && i + 1 < argc) {
            asset_cache_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--asset-timeout-ms") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --asset-timeout-ms requires a value\n";
                return 2;
            }
            if (!parse_positive_int_arg("--asset-timeout-ms", argv[++i], asset_timeout_ms))
                return 2;
        } else if (std::strcmp(argv[i], "--asset-hash") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --asset-hash requires <uri=sha256-hex>\n";
                return 2;
            }
            if (!parse_asset_hash_arg(argv[++i], expected_asset_hashes))
                return 2;
        } else if (std::strcmp(argv[i], "--no-emit-classnames") == 0) {
            emit_classnames = false;
        } else if (std::strcmp(argv[i], "--shortcuts") == 0 && i + 1 < argc) {
            shortcuts_output = argv[++i];
            shortcuts_output_explicit = true;
        } else if (std::strcmp(argv[i], "--no-import-shortcuts") == 0) {
            import_shortcuts = false;
        } else if (std::strcmp(argv[i], "--no-default-shortcuts") == 0) {
            default_shortcuts = false;
        } else if (std::strcmp(argv[i], "--detect-only") == 0) {
            detect_only = true;
        } else if (std::strcmp(argv[i], "--report-new-format") == 0) {
            report_new_format = true;
            detect_only = true;
        } else if (std::strcmp(argv[i], "--directory") == 0 && i + 1 < argc) {
            input_directory = argv[++i];
        } else if (std::strcmp(argv[i], "--compat") == 0 && i + 1 < argc) {
            compat_override = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

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

    // pulp friction-fix #4 — when the user passes --output <dir>/ui.js,
    // anchor the sidecar files (bridge_handlers.cpp, classnames.json,
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
            if (!tokens_file_explicit)       anchor(tokens_file,       "tokens.json");
        }
    }

    // Export-tokens mode: read a Pulp theme JSON and export as W3C tokens
    if (export_tokens_mode) {
        if (input_file.empty()) {
            // No input = export the built-in dark theme
            auto theme = Theme::dark();
            auto w3c = export_w3c_tokens(theme);
            if (dry_run) {
                std::cout << w3c;
                return 0;
            }
            if (!write_file(tokens_file, w3c)) return 1;
            std::cout << "Exported " << (theme.colors.size() + theme.dimensions.size() + theme.strings.size())
                      << " tokens → " << tokens_file << "\n";
            return 0;
        }
        // Read theme JSON → export as W3C
        auto content = read_file(input_file);
        if (content.empty()) return 1;
        auto theme = Theme::from_json(content);
        auto w3c = export_w3c_tokens(theme);
        if (dry_run) {
            std::cout << w3c;
            return 0;
        }
        if (!write_file(tokens_file, w3c)) return 1;
        std::cout << "Exported " << (theme.colors.size() + theme.dimensions.size() + theme.strings.size())
                  << " tokens → " << tokens_file << "\n";
        return 0;
    }

    // ── pulp #1031 — versioned detect-only path ─────────────────────────
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
                      << "         format-version than Pulp recognises. Pulp will use\n"
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

    auto source = parse_design_source(source_str);
    if (!source) {
        std::cerr << "Error: unknown source '" << source_str << "'\n";
        std::cerr << "Valid sources: figma, stitch, v0, pencil, claude, designmd, jsx\n";
        return 1;
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
            std::cerr << "Error: --mode baked requires --emit ir-json or --emit cpp\n";
            std::cerr << "       effective defaults: --mode " << runtime_mode_name(runtime_mode)
                      << " (" << default_selection.mode_source << "), --emit "
                      << artifact_emit_name(artifact_emit) << " ("
                      << default_selection.emit_source << ")\n";
            return 2;
        }
    } else if (artifact_emit == ArtifactEmit::cpp) {
        std::cerr << "Error: --emit cpp requires --mode baked\n";
        std::cerr << "       effective defaults: --mode " << runtime_mode_name(runtime_mode)
                  << " (" << default_selection.mode_source << "), --emit "
                  << artifact_emit_name(artifact_emit) << " ("
                  << default_selection.emit_source << ")\n";
        return 2;
    }
    if (runtime_mode == RuntimeMode::baked) {
        if (*source == DesignSource::designmd && artifact_emit == ArtifactEmit::cpp) {
            std::cerr << "Error: DESIGN.md is a token spec and cannot emit a baked C++ view\n";
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
    std::string runtime_error;  // captures --execute-bundle fallback reason
    try {
        switch (*source) {
            case DesignSource::figma:  ir = parse_figma_json(content); break;
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
                    (artifact_emit != ArtifactEmit::ir_json && artifact_emit != ArtifactEmit::cpp)) {
                    std::cerr << "Error: --from jsx is currently wired only for"
                                 " --mode baked --emit ir-json or --emit cpp\n";
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
    } catch (const std::exception& e) {
        std::cerr << "Error parsing " << design_source_name(*source) << " input: " << e.what() << "\n";
        return 1;
    }

    if (execute_bundle && !runtime_error.empty()) {
        // Surface the harness-fallback reason so users can tell when the
        // bundle eval lane bailed out vs. produced a real materialized IR.
        std::cout << "[execute-bundle] runtime fallback: " << runtime_error << "\n";
    } else if (execute_bundle) {
        std::cout << "[execute-bundle] runtime path produced the IR (no fallback)\n";
    }

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
    if (!frame_name.empty()) ir.root.attributes["frame"] = frame_name;
    if (!screen_name.empty()) ir.root.attributes["screen"] = screen_name;

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
            return 0;
        }
        if (!write_file(output_file, ir_json)) return 1;
        std::cout << "Wrote " << output_file << " (DesignIR v1, "
                  << ir.asset_manifest.assets.size() << " asset"
                  << (ir.asset_manifest.assets.size() == 1 ? "" : "s")
                  << ")\n";
        return 0;
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
            return 0;
        }

        if (!write_file(paths.header.string(), cpp.header)) return 1;
        if (!write_file(paths.source.string(), cpp.source)) return 1;

        size_t node_count = 0, text_count = 0, container_count = 0, widget_count = 0;
        std::function<void(const IRNode&)> count_nodes = [&](const IRNode& n) {
            node_count++;
            if (n.audio_widget != AudioWidgetType::none) widget_count++;
            else if (n.type == "text" || n.type == "label") text_count++;
            else if (!n.children.empty() || n.type == "frame") container_count++;
            for (auto& c : n.children) count_nodes(c);
        };
        count_nodes(ir.root);

        std::cout << "Wrote " << paths.source.string() << " and "
                  << paths.header.string() << " (" << node_count << " elements: "
                  << container_count << " containers, " << widget_count << " widgets, "
                  << text_count << " labels, " << ir.asset_manifest.assets.size() << " asset"
                  << (ir.asset_manifest.assets.size() == 1 ? "" : "s") << ")\n";
        return 0;
    }

    // Generate Pulp JS
    CodeGenOptions opts;
    opts.mode = use_web_compat ? CodeGenMode::web_compat : CodeGenMode::bridge_native_js;
    opts.include_tokens = include_tokens;
    opts.include_comments = include_comments;
    opts.preview_mode = preview_mode;

    // pulp #2116 V2 — auto-import keyboard shortcuts from the source.
    // Default-on. Source-agnostic helper: the extractor takes a raw
    // TSX/JS/HTML string and regex-scans for `e.key === '…'` patterns,
    // so all source types (claude, v0, figma code blobs, stitch inline
    // JS, pencil) can route through the same call without per-source
    // branching here.
    std::vector<DetectedShortcut> detected_shortcuts;
    DefaultShortcutScan default_scan;
    if (import_shortcuts) {
        detected_shortcuts = extract_keyboard_shortcuts(content, input_file);

        // Phase A defaults — only fire when the developer's React source
        // has a high-confidence match. `apply_default_shortcuts` lowers
        // accepted DefaultShortcutCandidates into the same DetectedShortcut
        // form so they ride V2's codegen path with no fork. Suppressed
        // chord-by-chord against `detected_shortcuts` so an extracted
        // binding always wins.
        //
        // Codex P2 on #2128: the import CLI runs at build time, but the
        // generated ui.js ships to many platforms (mac standalone, win
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

    auto js = generate_pulp_js(ir, opts);

    if (dry_run) {
        std::cout << "=== Generated Pulp JS (" << design_source_name(*source) << " → " << output_file << ") ===\n\n";
        std::cout << js;

        if (include_tokens && (!ir.tokens.colors.empty() || !ir.tokens.dimensions.empty())) {
            auto theme = ir_tokens_to_theme(ir.tokens);
            auto w3c = export_w3c_tokens(theme);
            std::cout << "\n=== W3C Design Tokens (" << tokens_file << ") ===\n\n";
            std::cout << w3c;
        }
        return 0;
    }

    auto t_codegen = std::chrono::steady_clock::now();

    // Write output files. DESIGN.md describes a system, not a screen —
    // there is no UI tree to scaffold, so skip the ui.js write entirely
    // and emit only tokens.json. Phase 3 may add a `--with-scaffold` flag
    // once name-based widget detection is consistent across sources.
    if (*source != DesignSource::designmd) {
        if (!write_file(output_file, js)) return 1;
    }

    // Count elements by type
    size_t node_count = 0, text_count = 0, container_count = 0, widget_count = 0;
    std::function<void(const IRNode&)> count_nodes = [&](const IRNode& n) {
        node_count++;
        if (n.audio_widget != AudioWidgetType::none) widget_count++;
        else if (n.type == "text" || n.type == "label") text_count++;
        else if (!n.children.empty() || n.type == "frame") container_count++;
        for (auto& c : n.children) count_nodes(c);
    };
    count_nodes(ir.root);

    auto t_write = std::chrono::steady_clock::now();
    if (*source == DesignSource::designmd) {
        std::cout << "DESIGN.md → tokens only (no ui.js; system spec, not screen)";
    } else {
        std::cout << "Wrote " << output_file << " (" << node_count << " elements: "
                  << container_count << " containers, " << widget_count << " widgets, "
                  << text_count << " labels";
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
            body = export_w3c_tokens(theme);
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

    // Bridge handler scaffold for Claude Design imports (pulp #709).
    // Only emitted for --from claude; other sources keep their existing
    // output shape unchanged.
    if (*source == DesignSource::claude && emit_bridge_scaffold) {
        const auto scaffold = render_claude_bridge_scaffold(output_file);
        if (write_file(bridge_output, scaffold)) {
            std::cout << "Wrote " << bridge_output
                      << " (bridge handler scaffold — edit add_handler() entries to wire your editor's messages)\n";
        }
    }

    // Classnames artifact for Claude Design imports (pulp #1035).
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

    // pulp #2116 V2 — shortcuts manifest alongside classnames. Mirror
    // shape so a reviewer can audit what the auto-import will bind. The
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

    // Phase A — diagnostic dump of the defaults scan alongside the bound
    // manifest. Writes even when no defaults fired, so a reviewer can see
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

    // pulp friction-fix #3 — native-react detection (heuristic shared
    // with the lib so tests can exercise it directly; see
    // design_import.hpp::looks_like_bundler_entry). When the static
    // parser produces only a handful of elements AND the HTML looks
    // like a JS-bundler entry, the user almost certainly wanted to run
    // the bundle directly. Soft warning — we still wrote ui.js.
    if (*source == DesignSource::claude && node_count <= 12 &&
        looks_like_bundler_entry(content)) {
        std::cerr << "\n"
                  << "Note: this HTML looks like a JS-bundler entry "
                  << "(mount-point + script tag). The static parser "
                  << "only captured the placeholder chrome ("
                  << node_count << " element"
                  << (node_count == 1 ? "" : "s")
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
            static_cast<uint32_t>(render_height), 2.0f);

        if (rendered_png.empty()) {
            std::cerr << "Validation error: headless render failed\n";
            return 1;
        }

        auto rendered_path = design_name + "-" + source_lower + "-render.png";
        {
            std::ofstream f(rendered_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(rendered_png.data()),
                    static_cast<std::streamsize>(rendered_png.size()));
        }
        std::cout << "Rendered → " << rendered_path << " (" << render_width << "x" << render_height << ")\n";

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

            if (result.passes(0.70f)) {
                std::cout << "Validation: PASS\n";
            } else {
                std::cout << "Validation: NEEDS REVIEW (similarity below 70%)\n";
            }

            // Always generate diff image when reference is provided
            // Use --diff path if given, otherwise auto-generate alongside render
            auto actual_diff_path = diff_output.empty()
                ? (design_name + "-" + source_lower + "-diff.png") : diff_output;
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
        dbg << "    \"total\": " << node_count << ",\n";
        dbg << "    \"containers\": " << container_count << ",\n";
        dbg << "    \"widgets\": " << widget_count << ",\n";
        dbg << "    \"labels\": " << text_count << "\n";
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
            auto result = compare_screenshot_files(reference_image, design_name + "-" + source_lower + "-render.png");
            dbg << "  \"validation\": {\n";
            dbg << "    \"reference\": \"" << reference_image << "\",\n";
            dbg << "    \"similarity_pct\": " << static_cast<int>(result.similarity * 100) << ",\n";
            dbg << "    \"diff_pixels\": " << result.diff_pixels << ",\n";
            dbg << "    \"total_pixels\": " << result.total_pixels << ",\n";
            dbg << "    \"mean_error\": " << result.mean_error << ",\n";
            dbg << "    \"pass\": " << (result.passes(0.70f) ? "true" : "false") << "\n";
            dbg << "  },\n";
        }

        // List unprocessed/unsupported elements
        dbg << "  \"gaps\": [\n";
        bool first_gap = true;
        std::function<void(const IRNode&)> find_gaps = [&](const IRNode& n) {
            // Shapes that aren't audio widgets (not translated to Pulp widgets)
            if ((n.type == "ellipse" || n.type == "rectangle" || n.type == "path" ||
                 n.type == "polygon" || n.type == "line") &&
                n.audio_widget == AudioWidgetType::none) {
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

    return 0;
}
