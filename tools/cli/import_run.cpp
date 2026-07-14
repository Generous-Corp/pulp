// pulp import — SPI-verb orchestration (detect / inspect / emit).
//
// See import_run.hpp for the role of this unit. Framework importers are
// vendor-specific add-on tools that live in their own private repos. The Pulp
// SDK owns only the generalised substrate: a discovery index of known
// frameworks (DATA, the one place real markers/vendor names appear), a
// JSON-over-stdio SPI to drive an installed importer, and the emission step
// (the SDK writes files; the importer only proposes a plan).
//
// This translation unit names NO framework and NO vendor: framework identity
// is runtime DATA loaded from tools/import/known-frameworks.json.

#include "import_run.hpp"

#include "import_detect.hpp"
#include "import_emit.hpp"
#include "import_emit_scan.hpp"
#include "import_spi.hpp"
#include "import_terms.hpp"
#include "tool_registry.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace pulp::cli::import_run {

namespace {

namespace det = pulp::cli::import_detect;
namespace reg = pulp::cli::tools;
namespace spi = pulp::cli::import_spi;
namespace ie = pulp::cli::import_emit;
namespace ies = pulp::cli::import_emit_scan;

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
    det::KnownFrameworks kf;
    if (!idx.empty()) kf = det::load_index(idx);

    // The SDK's own index ships EMPTY on purpose. Detection markers belong to an
    // add-on importer, which contributes its own index when it is installed. Merge
    // those in — and note that with nothing installed this adds nothing, so
    // detection matches nothing and reveals nothing about what could have matched.
    auto installed = det::merge_installed_indices(reg::pulp_home() / "tools");
    if (!installed.error.empty()) {
        if (!kf.error.empty()) kf.error += "; ";
        kf.error += installed.error;
    }
    for (auto& fw : installed.frameworks) kf.frameworks.push_back(std::move(fw));

    return kf;
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

// ── shared inspect/emit helpers ──

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

// Resolve the importer for `o.from`, printing the install hint and returning
// nullopt when no importer is resolvable.
std::optional<ResolvedImporter> resolve_for(const ImportOptions& o,
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
    return resolved;
}

// Drive one SPI verb against an already-resolved importer and return the raw
// `result` JSON, or nullopt on failure (message already printed). Shared by
// analyze and emit so transport/version/ok handling lives in one place.
std::optional<std::string> run_verb(const ResolvedImporter& resolved,
                                    const std::string& verb,
                                    const std::string& id,
                                    const std::string& payload_json,
                                    const char* result_label) {
    std::string request = spi::build_request(verb, id, payload_json);
    auto resp = spi::run(resolved.invocation, request);

    if (!resp.transport_ok) {
        std::cerr << "pulp import: importer transport error: "
                  << resp.transport_error << "\n";
        return std::nullopt;
    }
    if (auto vmsg = spi::check_version(resp.spi_version, resolved.spi_min,
                                       resolved.spi_max);
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
        std::cerr << "pulp import: importer returned ok but no " << result_label
                  << " result\n";
        return std::nullopt;
    }
    return resp.result_json;
}

// Drive the importer's `analyze` verb and return the ProjectIR JSON (the
// response `result`), or empty on failure (message already printed).
std::optional<std::string> run_analyze(const ImportOptions& o,
                                        const det::KnownFrameworks& kf) {
    auto resolved = resolve_for(o, kf);
    if (!resolved) return std::nullopt;
    return run_verb(*resolved, "analyze", "analyze-1",
                    build_analyze_payload(o), "ProjectIR");
}

bool write_text(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    return f.good();
}

// A UTC ISO-8601 timestamp the SDK stamps into the provenance marker. The
// importer never sees the host clock; the SDK supplies the time-of-emit.
std::string iso_utc_now() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// A cheap, order-independent hash of a directory's contents so the provenance
// marker records which source tree produced the scaffold. FNV-1a over relative
// paths + sizes; read-only and bounded (skips common build/vendor dirs).
std::string source_dir_hash(const fs::path& dir) {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    auto mix = [&](const std::string& s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    };
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        for (auto it = fs::recursive_directory_iterator(
                 dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const std::string name = it->path().filename().string();
            if (name == ".git" || name == "build" || name == "node_modules") {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            mix(fs::relative(it->path(), dir, ec).generic_string());
            mix(std::to_string(static_cast<long long>(it->file_size(ec))));
        }
    }
    char out[19];
    std::snprintf(out, sizeof(out), "0x%016llx",
                  static_cast<unsigned long long>(h));
    return out;
}

// Build the emit-verb payload: the analyzed ProjectIR (embedded verbatim), the
// source project dir (so the importer can resolve portable-core copy sources),
// the output dir, and the SDK-supplied options (the host clock, never the
// importer's). `ir_json` is a JSON object literal from the analyze result.
std::string build_emit_payload(const ImportOptions& o, const std::string& ir_json,
                               const std::string& emitted_at) {
    std::string p = "{\"project_ir\":" + ir_json;
    p += ",\"project_dir\":\"" + json_escape(fs::absolute(o.dir).string()) + "\"";
    p += ",\"output_dir\":\"" + json_escape(fs::absolute(o.output).string()) + "\"";
    p += ",\"options\":{\"source_dir\":\"" +
         json_escape(fs::absolute(o.dir).string()) + "\"";
    p += ",\"emitted_at\":\"" + json_escape(emitted_at) + "\"}";
    p += "}";
    return p;
}

// Serialise the SDK-written provenance marker. Records importer identity, the
// framework, the SPI version this SDK spoke, the caller-supplied timestamp, and
// a hash of the source tree. JSON hand-built (small, fixed shape).
std::string build_provenance_marker(const ie::Manifest& m,
                                    const std::string& framework,
                                    const std::string& emitted_at,
                                    const std::string& src_hash,
                                    const std::vector<ie::WriteAction>& actions) {
    std::string j = "{\n";
    j += "  \"schema\": \"pulp.import.provenance.v0\",\n";
    j += "  \"importer_id\": \"" + json_escape(m.importer_id) + "\",\n";
    j += "  \"framework\": \"" + json_escape(framework) + "\",\n";
    j += "  \"spi_version\": " + std::to_string(spi::kSpiVersion) + ",\n";
    j += "  \"emitted_at\": \"" + json_escape(emitted_at) + "\",\n";
    j += "  \"source_dir_hash\": \"" + json_escape(src_hash) + "\",\n";
    j += "  \"files\": [\n";
    for (size_t i = 0; i < actions.size(); ++i) {
        const auto& a = actions[i];
        j += "    {\"path\": \"" + json_escape(a.file->path) +
             "\", \"provenance\": \"" + ie::provenance_name(a.provenance) + "\"}";
        j += (i + 1 < actions.size()) ? ",\n" : "\n";
    }
    j += "  ]\n}\n";
    return j;
}

// ── IMPORTER_TERMS accept-to-run gate ──
//
// The terms body is DATA the add-on importer carries on its tool-registry
// descriptor. Resolve it for `o.from`; an explicit --importer-cmd has no
// registry entry, so a --importer-terms-text override supplies the body there.

namespace terms = pulp::cli::import_terms;

// Find the tool-registry path by walking up from the cwd (mirrors the search in
// resolve_importer; kept tiny + local).
fs::path find_tool_registry() {
    fs::path cwd = fs::current_path();
    while (true) {
        auto p = cwd / "tools" / "packages" / "tool-registry.json";
        if (fs::exists(p)) return p;
        if (cwd.has_parent_path() && cwd.parent_path() != cwd)
            cwd = cwd.parent_path();
        else break;
    }
    return {};
}

// Build the TermsDescriptor for `o.from`. Priority: an explicit
// --importer-terms-text override, else the resolved tool-registry descriptor.
// `importer_id` is the importer tool id (the acceptance store key), falling back
// to the framework id when no tool is resolvable.
terms::TermsDescriptor resolve_terms(const ImportOptions& o,
                                     const det::FrameworkEntry* fw_entry) {
    terms::TermsDescriptor td;
    td.importer_id =
        fw_entry && !fw_entry->importer_tool_id.empty()
            ? fw_entry->importer_tool_id
            : o.from;

    if (!o.terms_text.empty()) {
        td.terms_text = o.terms_text;
        td.terms_version = o.terms_version;
        return td;
    }

    auto reg_path = find_tool_registry();
    if (reg_path.empty()) return td;
    auto [reg, err] = pulp::cli::tools::load_tool_registry(reg_path);
    if (!err.empty()) return td;

    const pulp::cli::tools::ToolDescriptor* chosen = nullptr;
    for (auto& [id, tool] : reg.tools)
        for (auto& f : tool.frameworks)
            if (f == o.from) { chosen = &tool; break; }
    if (!chosen && fw_entry && !fw_entry->importer_tool_id.empty()) {
        auto it = reg.tools.find(fw_entry->importer_tool_id);
        if (it != reg.tools.end()) chosen = &it->second;
    }
    if (chosen) {
        td.importer_id = chosen->id;
        td.terms_text = chosen->terms_text;
        td.terms_version = chosen->terms_version;
        td.vendor_id = chosen->vendor_id;
    }
    return td;
}

// Enforce the accept-to-run gate before any importer is driven. Returns true to
// proceed; on a hard stop prints a clear message and the caller returns nonzero.
bool ensure_terms_accepted(const ImportOptions& o, const det::KnownFrameworks& kf,
                           const char* subcommand) {
    const det::FrameworkEntry* fw = find_framework(kf, o.from);
    terms::TermsDescriptor td = resolve_terms(o, fw);

    terms::GateIo io{std::cin, std::cout, is_tty()};
    auto result = terms::run_gate(td, terms::acceptance_store_path(),
                                  o.accept_importer_terms, iso_utc_now(), io);

    switch (result) {
        case terms::GateResult::Accepted:
        case terms::GateResult::NoTermsToAccept:
            return true;
        case terms::GateResult::Declined:
            std::cerr << "pulp import " << subcommand
                      << ": importer terms were not accepted. Aborting.\n";
            return false;
        case terms::GateResult::NonInteractive:
            std::cerr << "pulp import " << subcommand
                      << ": importer '" << td.importer_id
                      << "' requires accepting its terms of use, but no terminal is\n"
                         "available to prompt. Re-run with --accept-importer-terms to\n"
                         "accept them non-interactively (e.g. in CI).\n";
            return false;
        case terms::GateResult::StoreError:
            std::cerr << "pulp import " << subcommand
                      << ": could not record terms acceptance under "
                      << terms::acceptance_store_path().string()
                      << " (check permissions / $PULP_HOME).\n";
            return false;
    }
    return false;
}

}  // namespace

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

    // Accept-to-run IMPORTER_TERMS gate (before any importer is driven).
    if (!ensure_terms_accepted(o, kf, "inspect")) return 1;

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

    // Accept-to-run IMPORTER_TERMS gate (before any importer is driven).
    if (!ensure_terms_accepted(o, kf, "emit")) return 1;

    auto resolved = resolve_for(o, kf);
    if (!resolved) return 1;  // install hint already printed

    // analyze → ProjectIR, then emit → EmissionManifest. The importer proposes
    // the files; the SDK writes + gates them.
    auto ir = run_verb(*resolved, "analyze", "analyze-1",
                       build_analyze_payload(o), "ProjectIR");
    if (!ir) return 1;

    const std::string emitted_at = iso_utc_now();
    auto manifest_json = run_verb(*resolved, "emit", "emit-1",
                                  build_emit_payload(o, *ir, emitted_at),
                                  "EmissionManifest");
    if (!manifest_json) return 1;

    ie::Manifest manifest = ie::parse_manifest(*manifest_json);
    if (!manifest.ok) {
        std::cerr << "pulp import emit: malformed emission manifest: "
                  << manifest.parse_error << "\n";
        return 1;
    }

    // Clean-room OUTPUT gate: reject framework SOURCE / vendor banners in any
    // generated file before writing it. copied-user-file is the user's own DSP
    // and is exempt. The denylist is DATA from the known-frameworks index.
    auto denylist = ies::denylist_from_known_frameworks(kf);
    auto scan = ies::scan_manifest(manifest, denylist);
    if (!scan.clean) {
        std::cerr << "pulp import emit: clean-room output scan FAILED — the "
                     "importer proposed framework source in generated files:\n";
        for (const auto& hit : scan.hits)
            std::cerr << "  - " << hit.path << ": contains \"" << hit.token << "\"\n";
        std::cerr << "Refusing to materialise. This is a clean-room safety net: "
                     "generated output must not embed framework source.\n";
        return 1;
    }

    // Compute the write plan (pure validation: no path may escape --output).
    ie::WritePlan plan = ie::compute_write_plan(manifest, fs::absolute(o.output));
    if (!plan.ok) {
        std::cerr << "pulp import emit: " << plan.error << "\n";
        return 1;
    }

    fs::create_directories(o.output, ec);

    // Materialise each file: write inline content, or copy a verbatim source.
    int written = 0, copied = 0;
    for (const auto& a : plan.actions) {
        if (a.dest.has_parent_path())
            fs::create_directories(a.dest.parent_path(), ec);
        if (a.is_copy) {
            fs::copy_file(a.source, a.dest,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "pulp import emit: cannot copy " << a.source.string()
                          << " -> " << a.dest.string() << ": " << ec.message() << "\n";
                return 1;
            }
            ++copied;
        } else {
            if (!write_text(a.dest, a.file->content)) {
                std::cerr << "pulp import emit: cannot write " << a.dest.string() << "\n";
                return 1;
            }
            ++written;
        }
    }

    // The SDK owns migration_status.json (the importer's rich status document)
    // and the provenance marker — both written by the SDK, not the importer.
    if (!manifest.migration_status_json.empty()) {
        if (!write_text(o.output / "migration_status.json",
                        manifest.migration_status_json + "\n")) {
            std::cerr << "pulp import emit: cannot write migration_status.json\n";
            return 1;
        }
    }
    const std::string framework =
        manifest.framework.empty() ? o.from : manifest.framework;
    if (!write_text(o.output / ".pulp-import-provenance.json",
                    build_provenance_marker(manifest, framework, emitted_at,
                                            source_dir_hash(fs::absolute(o.dir)),
                                            plan.actions))) {
        std::cerr << "pulp import emit: cannot write provenance marker\n";
        return 1;
    }

    // Summary.
    std::cout << "Materialised Pulp migration scaffold in " << o.output.string()
              << "\n";
    std::cout << "  files written:  " << written << " generated, " << copied
              << " copied verbatim\n";
    std::cout << "  clean-room scan: passed (" << scan.scanned_files
              << " generated files, " << scan.exempt_files << " user-file copies "
                 "exempt)\n";
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
        return s.empty() ? std::string("(none)") : s;
    };
    std::cout << "  formats:        " << join(manifest.formats) << "\n";
    std::cout << "  deferred:       " << join(manifest.deferred_formats) << "\n";
    std::cout << "  unresolved:     " << ie::format_unresolved_summary(manifest)
              << "\n";
    if (!manifest.verdict.empty())
        std::cout << "  verdict:        " << manifest.verdict << "\n";
    std::cout << "\nNext steps:\n";
    std::cout << "  1. Build it:   cmake -S " << o.output.string()
              << " -B " << (o.output / "build").string() << " && cmake --build "
              << (o.output / "build").string() << "\n";
    std::cout << "  2. " << ie::format_parity_review_step(manifest) << "\n";
    std::cout << "  3. Implement the missing parity work in src/ and rebuild/test; "
                 "any TODO(import) markers are importer-emitted hints.\n";
    return 0;
}

}  // namespace pulp::cli::import_run
