// pulp import — read an existing audio-plugin project read-only and emit a
// Pulp migration scaffold.
//
// Framework importers are vendor-specific add-on tools that live in their own
// private repos. The Pulp SDK owns only the generalised substrate: a discovery
// index of known frameworks (DATA, the one place real markers/vendor names
// appear), a JSON-over-stdio SPI to drive an installed importer, and the
// emission step (the SDK writes files; the importer only proposes a plan).
//
// This translation unit names NO framework and NO vendor: framework identity
// is runtime DATA loaded from tools/import/known-frameworks.json. The detection
// engine, the SPI runner, and the tool-registry importer fields are the entire
// SDK surface.
//
// Subcommands:
//   pulp import detect <dir>
//   pulp import inspect --from <fw> <dir> [opts]
//   pulp import emit    --from <fw> <dir> --output <out> [opts]
//   pulp import <dir>                       (alias for detect)

#include "cli_common.hpp"
#include "import_detect.hpp"
#include "import_spi.hpp"
#include "tool_registry.hpp"

#include <pulp/platform/child_process.hpp>

#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace det = pulp::cli::import_detect;
namespace spi = pulp::cli::import_spi;

// ── JSON string escaping for request payloads ──

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// ── Index resolution ──

det::KnownFrameworks load_known_frameworks(std::string* index_path_out) {
    fs::path start = fs::current_path();
    fs::path exe_dir = current_executable_path().parent_path();
    fs::path idx = det::find_index(start, exe_dir);
    if (index_path_out) *index_path_out = idx.string();
    if (idx.empty()) {
        det::KnownFrameworks kf;
        kf.error = "could not locate tools/import/known-frameworks.json "
                   "(set PULP_KNOWN_FRAMEWORKS to override)";
        return kf;
    }
    return det::load_index(idx);
}

const det::FrameworkEntry* find_framework(const det::KnownFrameworks& kf,
                                          const std::string& id) {
    for (const auto& fw : kf.frameworks)
        if (fw.framework_id == id) return &fw;
    return nullptr;
}

// ── Install-hint text ──

void print_install_hint(const std::string& framework_id,
                        const std::string& importer_tool_id,
                        const fs::path& dir) {
    std::cerr << "\nNo importer for '" << framework_id
              << "' is installed. To proceed:\n"
              << "  pulp tool install " << importer_tool_id << "\n"
              << "  pulp import inspect --from " << framework_id << " "
              << dir.string() << "\n";
}

// ── Importer resolution ──
//
// Returns the importer invocation, or nullopt with `hint_*` populated so the
// caller can print the install hint. An explicit --importer-cmd always wins
// (used by tests and power users); otherwise we resolve the tool registry
// entry whose `frameworks` list contains the requested framework and locate
// its binary.

struct ResolvedImporter {
    spi::ImporterInvocation invocation;
    int spi_min = 0;
    int spi_max = 0;
};

std::optional<ResolvedImporter> resolve_importer(
    const std::string& framework_id,
    const det::FrameworkEntry* fw_entry,
    const std::string& importer_cmd_override,
    std::string* hint_tool_id) {
    ResolvedImporter r;
    if (fw_entry) { r.spi_min = fw_entry->spi_min; r.spi_max = fw_entry->spi_max; }

    if (!importer_cmd_override.empty()) {
        r.invocation.command_line = importer_cmd_override;
        return r;
    }

    // Tool-registry resolution. Find the registry, find the importer tool that
    // declares this framework, locate its binary.
    auto reg_path = [&]() -> fs::path {
        fs::path cwd = fs::current_path();
        while (true) {
            auto p = cwd / "tools" / "packages" / "tool-registry.json";
            if (fs::exists(p)) return p;
            if (cwd.has_parent_path() && cwd.parent_path() != cwd)
                cwd = cwd.parent_path();
            else break;
        }
        return {};
    }();

    std::string declared_tool_id =
        fw_entry ? fw_entry->importer_tool_id : std::string{};

    if (!reg_path.empty()) {
        auto [reg, err] = pulp::cli::tools::load_tool_registry(reg_path);
        if (err.empty()) {
            // Prefer a tool that declares the framework; fall back to the
            // index's importer_tool_id.
            const pulp::cli::tools::ToolDescriptor* chosen = nullptr;
            for (auto& [id, tool] : reg.tools) {
                for (auto& f : tool.frameworks) {
                    if (f == framework_id) { chosen = &tool; break; }
                }
                if (chosen) break;
            }
            if (!chosen && !declared_tool_id.empty()) {
                auto it = reg.tools.find(declared_tool_id);
                if (it != reg.tools.end()) chosen = &it->second;
            }
            if (chosen) {
                if (chosen->spi_min || chosen->spi_max) {
                    r.spi_min = chosen->spi_min;
                    r.spi_max = chosen->spi_max;
                }
                auto loc = pulp::cli::tools::locate_tool(*chosen);
                if (loc.found) {
                    r.invocation.argv = {loc.path.string()};
                    return r;
                }
                if (hint_tool_id) *hint_tool_id = chosen->id;
                return std::nullopt;
            }
        }
    }

    if (hint_tool_id) *hint_tool_id = declared_tool_id;
    return std::nullopt;
}

// ── detect ──

int run_detect(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        std::cerr << "pulp import detect: not a directory: " << dir.string() << "\n";
        return 1;
    }

    std::string index_path;
    auto kf = load_known_frameworks(&index_path);
    if (!kf.error.empty()) {
        std::cerr << "pulp import detect: " << kf.error << "\n";
        return 1;
    }

    auto candidates = det::detect(dir, kf);
    if (candidates.empty()) {
        std::cout << "No known framework detected in " << dir.string() << "\n";
        std::cout << "(scanned against " << kf.frameworks.size()
                  << " known frameworks from " << index_path << ")\n";
        return 0;
    }

    std::cout << "Detected framework candidates in " << dir.string() << ":\n\n";
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        std::printf("  %zu. %s  (confidence %.0f%%)\n", i + 1,
                    c.framework_id.c_str(), c.confidence * 100.0);
        if (!c.display_name.empty())
            std::cout << "     " << c.display_name << "\n";
        for (const auto& e : c.evidence)
            std::cout << "     - " << e << "\n";
        std::cout << "\n";
    }

    const auto& top = candidates.front();
    std::cout << "Next steps:\n";
    std::cout << "  pulp tool install " << top.importer_tool_id << "\n";
    std::cout << "  pulp import inspect --from " << top.framework_id << " "
              << dir.string() << "\n";
    return 0;
}

// ── shared inspect/emit option parsing ──

struct ImportOptions {
    std::string from;          // framework id (required for inspect/emit)
    fs::path dir;              // project dir (required)
    fs::path output;           // emit: output dir
    fs::path ir_out;           // inspect: -o IR.json
    fs::path report;           // --report MD
    std::string framework_path;
    std::vector<std::string> extra_includes;
    std::string importer_cmd;
};

// Build the analyze payload JSON object literal.
std::string build_analyze_payload(const ImportOptions& o) {
    std::string p = "{\"project_dir\":\"" + json_escape(fs::absolute(o.dir).string()) + "\"";
    if (!o.framework_path.empty())
        p += ",\"framework_path\":\"" + json_escape(o.framework_path) + "\"";
    if (!o.extra_includes.empty()) {
        p += ",\"options\":{\"extra_includes\":[";
        for (size_t i = 0; i < o.extra_includes.size(); ++i) {
            if (i) p += ",";
            p += "\"" + json_escape(o.extra_includes[i]) + "\"";
        }
        p += "]}";
    }
    p += "}";
    return p;
}

void print_diagnostics(const spi::SpiResponse& resp) {
    for (const auto& d : resp.diagnostics) {
        std::cerr << "  [" << (d.severity.empty() ? "info" : d.severity) << "] ";
        if (!d.code.empty()) std::cerr << d.code << ": ";
        std::cerr << d.message << "\n";
    }
}

// Drive the importer's `analyze` verb and return the ProjectIR JSON (the
// response `result`), or empty on failure (message already printed).
std::optional<std::string> run_analyze(const ImportOptions& o,
                                        const det::KnownFrameworks& kf) {
    const det::FrameworkEntry* fw = find_framework(kf, o.from);
    if (!fw) {
        std::cerr << "pulp import: unknown framework '" << o.from
                  << "'. Run `pulp import detect " << o.dir.string()
                  << "` to list candidates.\n";
        return std::nullopt;
    }

    std::string hint_tool_id;
    auto resolved = resolve_importer(o.from, fw, o.importer_cmd, &hint_tool_id);
    if (!resolved) {
        print_install_hint(o.from,
                           hint_tool_id.empty() ? fw->importer_tool_id : hint_tool_id,
                           o.dir);
        return std::nullopt;
    }

    std::string request =
        spi::build_request("analyze", "analyze-1", build_analyze_payload(o));
    auto resp = spi::run(resolved->invocation, request);

    if (!resp.transport_ok) {
        std::cerr << "pulp import: importer transport error: "
                  << resp.transport_error << "\n";
        return std::nullopt;
    }

    if (auto vmsg = spi::check_version(resp.spi_version, resolved->spi_min,
                                       resolved->spi_max);
        !vmsg.empty()) {
        std::cerr << "pulp import: " << vmsg << "\n";
        return std::nullopt;
    }

    if (!resp.ok) {
        std::cerr << "pulp import: importer reported failure";
        if (!resp.error_code.empty()) std::cerr << " (" << resp.error_code << ")";
        std::cerr << ": " << resp.error_message << "\n";
        print_diagnostics(resp);
        return std::nullopt;
    }

    print_diagnostics(resp);
    if (resp.result_json.empty()) {
        std::cerr << "pulp import: importer returned ok but no ProjectIR result\n";
        return std::nullopt;
    }
    return resp.result_json;
}

bool write_text(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    return f.good();
}

// ── inspect ──

int run_inspect(const ImportOptions& o) {
    if (o.from.empty()) {
        std::cerr << "pulp import inspect: --from <framework> is required\n";
        return 2;
    }
    std::error_code ec;
    if (!fs::is_directory(o.dir, ec)) {
        std::cerr << "pulp import inspect: not a directory: " << o.dir.string() << "\n";
        return 1;
    }

    auto kf = load_known_frameworks(nullptr);
    if (!kf.error.empty()) {
        std::cerr << "pulp import inspect: " << kf.error << "\n";
        return 1;
    }

    auto ir = run_analyze(o, kf);
    if (!ir) return 1;  // resolution / transport / importer failure (hint printed)

    if (!o.ir_out.empty()) {
        if (!write_text(o.ir_out, *ir + "\n")) {
            std::cerr << "pulp import inspect: cannot write " << o.ir_out.string() << "\n";
            return 1;
        }
        std::cout << "Wrote ProjectIR to " << o.ir_out.string() << "\n";
    } else {
        std::cout << *ir << "\n";
    }

    if (!o.report.empty()) {
        std::string md = "# Import report: " + o.from + "\n\n"
                         "Source: " + fs::absolute(o.dir).string() + "\n\n"
                         "ProjectIR captured via SPI `analyze`. See the JSON for the\n"
                         "full inventory; emission (`pulp import emit`) materialises the\n"
                         "Pulp migration scaffold.\n";
        if (!write_text(o.report, md))
            std::cerr << "pulp import inspect: warning: cannot write report "
                      << o.report.string() << "\n";
        else
            std::cout << "Wrote report to " << o.report.string() << "\n";
    }
    return 0;
}

// ── emit ──

int run_emit(const ImportOptions& o) {
    if (o.from.empty()) {
        std::cerr << "pulp import emit: --from <framework> is required\n";
        return 2;
    }
    if (o.output.empty()) {
        std::cerr << "pulp import emit: --output <dir> is required\n";
        return 2;
    }
    std::error_code ec;
    if (!fs::is_directory(o.dir, ec)) {
        std::cerr << "pulp import emit: not a directory: " << o.dir.string() << "\n";
        return 1;
    }

    auto kf = load_known_frameworks(nullptr);
    if (!kf.error.empty()) {
        std::cerr << "pulp import emit: " << kf.error << "\n";
        return 1;
    }

    // analyze → ProjectIR is real in this slice. The plan+emit materialisation
    // (SDK-writes-files) is deferred to the next slice; we persist the IR so
    // the work isn't lost and print a clear "next slice" message.
    auto ir = run_analyze(o, kf);
    if (!ir) return 1;

    fs::create_directories(o.output, ec);
    fs::path ir_path = o.output / "project-import-ir.json";
    if (!write_text(ir_path, *ir + "\n")) {
        std::cerr << "pulp import emit: cannot write " << ir_path.string() << "\n";
        return 1;
    }
    std::cout << "Wrote ProjectIR to " << ir_path.string() << "\n";
    std::cout << "\nNote: scaffold emission (plan + SDK file materialisation) is\n"
                 "implemented in the next slice. This slice produces a validated\n"
                 "ProjectIR via the SPI `analyze` verb; `pulp import inspect` is the\n"
                 "fully-realised path today.\n";
    return 0;
}

// ── option parsing ──

void print_usage() {
    std::cout <<
        "Usage: pulp import <command> [options]\n\n"
        "Commands:\n"
        "  detect <dir>                 Rank known-framework candidates for a project\n"
        "  inspect --from <fw> <dir>    Run an importer's SPI analyze → write ProjectIR\n"
        "  emit    --from <fw> <dir> --output <out>\n"
        "                               analyze → ProjectIR (scaffold emission: next slice)\n"
        "  <dir>                        Alias for `detect <dir>`\n\n"
        "inspect / emit options:\n"
        "  --from <framework>           Framework id (see `pulp import detect`)\n"
        "  --framework-path <path>      The user's own framework checkout (read-only)\n"
        "  --extra-include <dir>        Extra include dir for the importer (repeatable)\n"
        "  -o, --output-ir <file>       inspect: write ProjectIR JSON to <file>\n"
        "  --report <file.md>           inspect: write a human report\n"
        "  --output <dir>               emit: scaffold output directory\n"
        "  --importer-cmd <cmd>         Override importer resolution with a command\n";
}

}  // namespace

int cmd_import(const std::vector<std::string>& args) {
    if (args.empty()) {
        print_usage();
        return 0;
    }

    const std::string& first = args[0];
    if (first == "-h" || first == "--help" || first == "help") {
        print_usage();
        return 0;
    }

    // Subcommand detection. `detect`, `inspect`, `emit` are explicit; any other
    // first token that is an existing path is treated as `detect <dir>`.
    std::string sub;
    size_t arg_start = 1;
    if (first == "detect" || first == "inspect" || first == "emit") {
        sub = first;
    } else {
        sub = "detect";
        arg_start = 0;  // first token is the directory
    }

    ImportOptions o;
    std::vector<std::string> positionals;
    for (size_t i = arg_start; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= args.size()) {
                std::cerr << "pulp import: " << flag << " requires a value\n";
                return std::string{};
            }
            return args[++i];
        };
        if (a == "--from") o.from = need("--from");
        else if (a == "--framework-path") o.framework_path = need("--framework-path");
        else if (a == "--extra-include") o.extra_includes.push_back(need("--extra-include"));
        else if (a == "-o" || a == "--output-ir") o.ir_out = need(a.c_str());
        else if (a == "--report") o.report = need("--report");
        else if (a == "--output") o.output = need("--output");
        else if (a == "--importer-cmd") o.importer_cmd = need("--importer-cmd");
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "pulp import: unknown option '" << a << "'\n";
            return 2;
        } else {
            positionals.push_back(a);
        }
    }

    if (!positionals.empty()) o.dir = positionals.front();
    if (o.dir.empty()) o.dir = fs::current_path();

    if (sub == "detect") return run_detect(o.dir);
    if (sub == "inspect") return run_inspect(o);
    if (sub == "emit") return run_emit(o);

    print_usage();
    return 2;
}
