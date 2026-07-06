// cmd_design.cpp — pulp design command

#include "cli_common.hpp"
#include "design_binding.hpp"

#include <pulp/design/design_adherence.hpp>
#include <pulp/design/design_gallery.hpp>
#include <pulp/design/design_ledger.hpp>
#include <pulp/design/design_manifest.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_tokens.hpp>

#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <system_error>

namespace {

std::string read_text_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_text_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    f << content;
    return f.good();
}

// Build the design manifest from the standard source options: an explicit
// DESIGN.md or Theme JSON, else the built-in Ink & Signal system. Prints an
// error and returns false on an unreadable or tokenless source. Shared by
// `pulp design compile` and `pulp design lint-adherence`.
bool build_source_manifest(const fs::path& design_md, const fs::path& theme_json,
                           bool dark, pulp::design::DesignManifest& out) {
    const std::string appearance = dark ? "dark" : "light";
    if (!design_md.empty()) {
        auto text = read_text_file(design_md);
        if (text.empty()) { std::cerr << "Error: cannot read " << design_md << "\n"; return false; }
        auto parsed = pulp::view::parse_designmd(text);
        auto theme = pulp::view::ir_tokens_to_theme(parsed.ir.tokens);
        if (theme.colors.empty() && theme.dimensions.empty() && theme.strings.empty()) {
            std::cerr << "Error: no tokens parsed from " << design_md << "\n";
            return false;
        }
        out = pulp::design::compile_design_manifest(
            theme, pulp::design::catalog(), design_md.filename().string(), appearance);
        return true;
    }
    if (!theme_json.empty()) {
        auto theme = pulp::view::Theme::load_from_file(theme_json.string());
        if (theme.colors.empty() && theme.dimensions.empty() && theme.strings.empty()) {
            std::cerr << "Error: no tokens read from " << theme_json << "\n";
            return false;
        }
        out = pulp::design::compile_design_manifest(
            theme, pulp::design::catalog(), theme_json.filename().string(), appearance);
        return true;
    }
    out = pulp::design::compile_ink_signal_manifest(dark);
    return true;
}

// `pulp design lint <DESIGN.md>` — runs the seven Google design.md
// lint rules + section-order. Exit 0 if no error findings; exit 1
// if any error-severity finding is present.
int run_lint(const std::vector<std::string>& rest) {
    if (rest.empty()) {
        std::cerr << "Usage: pulp design lint <path/to/DESIGN.md>\n";
        return 1;
    }
    auto text = read_text_file(rest.front());
    if (text.empty()) {
        std::cerr << "Error: cannot read " << rest.front() << "\n";
        return 1;
    }
    auto parsed = pulp::view::parse_designmd(text);
    auto findings = pulp::view::lint_designmd(parsed);
    int errors = 0;
    for (const auto& d : findings) {
        const char* sev = (d.severity == pulp::view::DesignMdSeverity::error)   ? "error" :
                          (d.severity == pulp::view::DesignMdSeverity::warning) ? "warning" : "info";
        std::cout << "[" << sev << "] " << d.code
                  << " at " << (d.path.empty() ? "<root>" : d.path);
        if (d.line > 0) std::cout << " (line " << d.line << ":" << d.column << ")";
        std::cout << ": " << d.message << "\n";
        if (d.severity == pulp::view::DesignMdSeverity::error) ++errors;
    }
    std::cout << "Lint summary: " << findings.size() << " finding(s), "
              << errors << " error(s).\n";
    return errors > 0 ? 1 : 0;
}

// `pulp design diff <before.md> <after.md>` — token-level diff plus
// regression flag. Exit 0 if no regression; exit 1 if regression.
int run_diff(const std::vector<std::string>& rest) {
    if (rest.size() < 2) {
        std::cerr << "Usage: pulp design diff <before.md> <after.md>\n";
        return 1;
    }
    auto before_text = read_text_file(rest[0]);
    auto after_text  = read_text_file(rest[1]);
    if (before_text.empty() || after_text.empty()) {
        std::cerr << "Error: cannot read one of the input files\n";
        return 1;
    }
    auto before = pulp::view::parse_designmd(before_text);
    auto after  = pulp::view::parse_designmd(after_text);
    auto diff = pulp::view::diff_designmd(before, after);
    auto report = [](const char* group, const pulp::view::DesignMdTokenDiff& d) {
        std::cout << group << ": +" << d.added.size()
                  << " -" << d.removed.size()
                  << " ~" << d.modified.size() << "\n";
        for (const auto& k : d.added)    std::cout << "  + " << k << "\n";
        for (const auto& k : d.removed)  std::cout << "  - " << k << "\n";
        for (const auto& k : d.modified) std::cout << "  ~ " << k << "\n";
    };
    report("colors",     diff.colors);
    report("dimensions", diff.dimensions);
    report("strings",    diff.strings);
    std::cout << "regression: " << (diff.regression ? "true" : "false") << "\n";
    return diff.regression ? 1 : 0;
}

void print_compile_usage() {
    std::cout <<
        "Usage: pulp design compile [options]\n"
        "\n"
        "Compile the design contract (token allowlist + component contracts) into\n"
        "a machine-readable manifest and an LLM binding prompt. The default source\n"
        "is the built-in Ink & Signal system.\n"
        "\n"
        "Options:\n"
        "  --design-md <file>  Compile a project's DESIGN.md tokens instead\n"
        "  --theme <file.json> Compile a saved Theme JSON instead\n"
        "  --dark              Use the dark appearance (default: light)\n"
        "  -o, --out-dir <dir> Output directory (default: current directory)\n"
        "  --json              Emit only design-manifest.json\n"
        "  --prompt            Emit only design-binding-prompt.md\n"
        "  --stdout            Print to stdout instead of writing files\n"
        "  -h, --help          Show this help\n"
        "\n"
        "Outputs (in --out-dir): design-manifest.json, design-binding-prompt.md\n";
}

// `pulp design compile` — compile the design contract into
// design-manifest.json + design-binding-prompt.md. Deterministic; the manifest
// is the token/component allowlist the importer and adherence check share.
int run_compile(const std::vector<std::string>& rest) {
    fs::path out_dir = fs::current_path();
    fs::path design_md;
    fs::path theme_json;
    bool dark = false;
    bool to_stdout = false;
    bool only_json = false;
    bool only_prompt = false;

    for (size_t i = 0; i < rest.size(); ++i) {
        const std::string& a = rest[i];
        auto value_for = [&](const char* flag) -> std::string {
            if (i + 1 >= rest.size() || rest[i + 1].empty() || rest[i + 1][0] == '-') {
                std::cerr << "pulp design compile: " << flag << " requires a value\n";
                return {};
            }
            return rest[++i];
        };
        if (a == "-h" || a == "--help") { print_compile_usage(); return 0; }
        else if (a == "--dark") { dark = true; }
        else if (a == "--stdout") { to_stdout = true; }
        else if (a == "--json") { only_json = true; }
        else if (a == "--prompt") { only_prompt = true; }
        else if (a == "-o" || a == "--out-dir") {
            auto v = value_for("--out-dir"); if (v.empty()) return 2; out_dir = fs::absolute(v);
        } else if (a == "--design-md") {
            auto v = value_for("--design-md"); if (v.empty()) return 2; design_md = fs::absolute(v);
        } else if (a == "--theme") {
            auto v = value_for("--theme"); if (v.empty()) return 2; theme_json = fs::absolute(v);
        } else {
            std::cerr << "pulp design compile: unknown argument '" << a << "'\n";
            return 2;
        }
    }
    if (!design_md.empty() && !theme_json.empty()) {
        std::cerr << "pulp design compile: --design-md and --theme are mutually exclusive\n";
        return 2;
    }
    if (only_json && only_prompt) {
        std::cerr << "pulp design compile: --json and --prompt are mutually exclusive\n";
        return 2;
    }
    enum class Emit { both, json, prompt };
    const Emit emit = only_json ? Emit::json : only_prompt ? Emit::prompt : Emit::both;

    pulp::design::DesignManifest manifest;
    if (!build_source_manifest(design_md, theme_json, dark, manifest)) return 1;

    const std::string json = pulp::design::manifest_to_json(manifest);
    const std::string prompt = pulp::design::emit_binding_prompt(manifest);

    if (to_stdout) {
        if (emit != Emit::prompt) std::cout << json << "\n";
        if (emit != Emit::json) std::cout << prompt;
        return 0;
    }

    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (emit != Emit::prompt) {
        auto p = out_dir / "design-manifest.json";
        if (!write_text_file(p, json)) { std::cerr << "Error: cannot write " << p << "\n"; return 1; }
        std::cout << "Wrote " << p.string() << " (" << manifest.tokens.size()
                  << " tokens, " << manifest.components.size() << " components)\n";
    }
    if (emit != Emit::json) {
        auto p = out_dir / "design-binding-prompt.md";
        if (!write_text_file(p, prompt)) { std::cerr << "Error: cannot write " << p << "\n"; return 1; }
        std::cout << "Wrote " << p.string() << "\n";
    }
    return 0;
}

void print_adherence_usage() {
    std::cout <<
        "Usage: pulp design lint-adherence <ui.js> [options]\n"
        "\n"
        "Lint UI JS against the design contract: flags raw hex colors, unknown\n"
        "var(--token) references, and px literals that match a design token.\n"
        "\n"
        "Options:\n"
        "  --manifest <file>   Lint against a compiled design-manifest.json\n"
        "  --design-md <file>  Build the contract from a DESIGN.md instead\n"
        "  --theme <file.json> Build the contract from a saved Theme JSON instead\n"
        "  --dark              Use the dark appearance (default: light)\n"
        "  --strict            Exit non-zero on any finding, not just errors\n"
        "  -h, --help          Show this help\n"
        "\n"
        "With no source flag, lints against the built-in Ink & Signal system.\n"
        "Exit 0 when clean; 1 when an error-severity finding is present (or any\n"
        "finding under --strict).\n";
}

// `pulp design lint-adherence <ui.js>` — the mechanical backstop for the design
// contract. Flags raw colors, unknown tokens, and token-valued px literals.
int run_lint_adherence(const std::vector<std::string>& rest) {
    fs::path js_file;
    fs::path manifest_file;
    fs::path design_md;
    fs::path theme_json;
    bool dark = false;
    bool strict = false;

    for (size_t i = 0; i < rest.size(); ++i) {
        const std::string& a = rest[i];
        auto value_for = [&](const char* flag) -> std::string {
            if (i + 1 >= rest.size() || rest[i + 1].empty() || rest[i + 1][0] == '-') {
                std::cerr << "pulp design lint-adherence: " << flag << " requires a value\n";
                return {};
            }
            return rest[++i];
        };
        if (a == "-h" || a == "--help") { print_adherence_usage(); return 0; }
        else if (a == "--dark") { dark = true; }
        else if (a == "--strict") { strict = true; }
        else if (a == "--manifest") {
            auto v = value_for("--manifest"); if (v.empty()) return 2; manifest_file = fs::absolute(v);
        } else if (a == "--design-md") {
            auto v = value_for("--design-md"); if (v.empty()) return 2; design_md = fs::absolute(v);
        } else if (a == "--theme") {
            auto v = value_for("--theme"); if (v.empty()) return 2; theme_json = fs::absolute(v);
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "pulp design lint-adherence: unknown argument '" << a << "'\n";
            return 2;
        } else if (js_file.empty()) {
            js_file = fs::absolute(a);
        } else {
            std::cerr << "pulp design lint-adherence: unexpected extra argument '" << a << "'\n";
            return 2;
        }
    }
    if (js_file.empty()) {
        std::cerr << "Usage: pulp design lint-adherence <ui.js> [options]\n";
        return 2;
    }
    if (!manifest_file.empty() && (!design_md.empty() || !theme_json.empty())) {
        std::cerr << "pulp design lint-adherence: --manifest is mutually exclusive with "
                     "--design-md/--theme\n";
        return 2;
    }
    if (!design_md.empty() && !theme_json.empty()) {
        std::cerr << "pulp design lint-adherence: --design-md and --theme are mutually exclusive\n";
        return 2;
    }
    if (!fs::exists(js_file)) {
        std::cerr << "Error: cannot read " << js_file << "\n";
        return 1;
    }
    const std::string js = read_text_file(js_file);

    pulp::design::DesignManifest manifest;
    if (!manifest_file.empty()) {
        auto text = read_text_file(manifest_file);
        if (text.empty()) { std::cerr << "Error: cannot read " << manifest_file << "\n"; return 1; }
        manifest = pulp::design::manifest_from_json(text);
        if (manifest.tokens.empty()) {
            std::cerr << "Error: no tokens in " << manifest_file << "\n";
            return 1;
        }
    } else if (!build_source_manifest(design_md, theme_json, dark, manifest)) {
        return 1;
    }

    const auto findings = pulp::design::lint_adherence(js, manifest);
    int errors = 0;
    for (const auto& f : findings) {
        const char* sev = (f.severity == pulp::design::AdherenceSeverity::error)   ? "error"
                        : (f.severity == pulp::design::AdherenceSeverity::warning) ? "warning"
                                                                                   : "info";
        std::cout << "[" << sev << "] " << pulp::design::adherence_kind_name(f.kind) << " at "
                  << js_file.filename().string() << ":" << f.line << ":" << f.column << ": "
                  << f.message << "\n";
        if (f.severity == pulp::design::AdherenceSeverity::error) ++errors;
    }
    std::cout << "Adherence: " << findings.size() << " finding(s), " << errors << " error(s).\n";
    if (errors > 0) return 1;
    if (strict && !findings.empty()) return 1;
    return 0;
}

void print_record_usage() {
    std::cout <<
        "Usage: pulp design record [--ledger <file>] <operation>\n"
        "\n"
        "Maintain the project design ledger (default: .pulp-design-meta.json) — the\n"
        "CLI-owned record of emitted design assets, their versions, provenance, and\n"
        "review status. Only this command writes the ledger; skills read it on resume.\n"
        "\n"
        "Operations:\n"
        "  --name <n> --asset <path>   Record (or update) a named asset revision.\n"
        "      --source <s>            Provenance (fig, figma-plugin, claude, …)\n"
        "      --viewport <WxH>        Design viewport (e.g. 340x280)\n"
        "      --system <name>         Bound design system (repeatable)\n"
        "      --inherit-from <ver>    Parent version this revision derives from\n"
        "      --version <ver>         Target an explicit version (else auto v1, v2, …)\n"
        "      --status <s>            needs-review (default) | approved | changes-requested\n"
        "  --remove <name[@version]>   Drop an asset (all versions, or one revision).\n"
        "  --reconcile                 Drop entries whose file no longer exists on disk.\n"
        "  --list [--json]             Print the ledger (human table or JSON) and exit.\n";
}

// `pulp design record` — maintain the project design ledger. All file IO lives
// here; the pure ledger operations live in core/view/src/design_ledger.cpp.
int run_record(const std::vector<std::string>& rest) {
    fs::path ledger_path = ".pulp-design-meta.json";
    std::string name, asset, source, viewport, inherit_from, version, status_str;
    std::vector<std::string> systems;
    std::string remove_selector;
    bool do_reconcile = false, do_list = false, as_json = false;
    bool have_remove = false;

    auto need_value = [&](const std::string& flag, size_t& i) -> const std::string* {
        if (i + 1 >= rest.size()) {
            std::cerr << "pulp design record: " << flag << " requires a value\n";
            return nullptr;
        }
        return &rest[++i];
    };

    for (size_t i = 0; i < rest.size(); ++i) {
        const std::string& a = rest[i];
        if (a == "-h" || a == "--help") { print_record_usage(); return 0; }
        else if (a == "--reconcile") { do_reconcile = true; }
        else if (a == "--list") { do_list = true; }
        else if (a == "--json") { as_json = true; }
        else {
            const std::string* v = need_value(a, i);
            if (!v) return 2;
            if (a == "--ledger") ledger_path = *v;
            else if (a == "--name") name = *v;
            else if (a == "--asset") asset = *v;
            else if (a == "--source") source = *v;
            else if (a == "--viewport") viewport = *v;
            else if (a == "--system") systems.push_back(*v);
            else if (a == "--inherit-from") inherit_from = *v;
            else if (a == "--version") version = *v;
            else if (a == "--status") status_str = *v;
            else if (a == "--remove") { remove_selector = *v; have_remove = true; }
            else {
                std::cerr << "pulp design record: unknown argument '" << a << "'\n";
                return 2;
            }
        }
    }

    // Exactly one operation.
    const int ops = (!name.empty() || !asset.empty()) + have_remove + (do_reconcile ? 1 : 0)
                    + (do_list ? 1 : 0);
    if (ops == 0) {
        std::cerr << "pulp design record: no operation (see --help)\n";
        return 2;
    }
    if (ops > 1) {
        std::cerr << "pulp design record: choose one of record / --remove / --reconcile / --list\n";
        return 2;
    }

    // Load the current ledger (absent = empty; tolerant parse).
    pulp::design::DesignLedger ledger =
        pulp::design::parse_ledger(fs::exists(ledger_path) ? read_text_file(ledger_path) : std::string{});

    if (do_list) {
        if (as_json) {
            std::cout << pulp::design::ledger_to_json(ledger) << "\n";
        } else if (ledger.assets.empty()) {
            std::cout << "(empty ledger: " << ledger_path.string() << ")\n";
        } else {
            for (const auto& x : ledger.assets) {
                std::cout << x.name << "@" << x.version << "  [" << pulp::design::review_status_name(x.status)
                          << "]  " << x.path;
                if (!x.source.empty()) std::cout << "  src=" << x.source;
                if (!x.inherit_from.empty()) std::cout << "  <-" << x.inherit_from;
                std::cout << "\n";
            }
        }
        return 0;
    }

    if (do_reconcile) {
        auto removed = pulp::design::reconcile(
            ledger, [](const std::string& p) { return fs::exists(p); });
        if (!write_text_file(ledger_path, pulp::design::ledger_to_json(ledger))) {
            std::cerr << "Error: cannot write " << ledger_path.string() << "\n";
            return 1;
        }
        std::cout << "Reconciled: dropped " << removed.size() << " missing asset(s).\n";
        for (const auto& s : removed) std::cout << "  - " << s << "\n";
        return 0;
    }

    if (have_remove) {
        auto removed = pulp::design::remove_asset(ledger, remove_selector);
        if (removed.empty()) {
            std::cerr << "pulp design record: no asset matched '" << remove_selector << "'\n";
            return 1;
        }
        if (!write_text_file(ledger_path, pulp::design::ledger_to_json(ledger))) {
            std::cerr << "Error: cannot write " << ledger_path.string() << "\n";
            return 1;
        }
        std::cout << "Removed " << removed.size() << " asset(s).\n";
        for (const auto& s : removed) std::cout << "  - " << s << "\n";
        return 0;
    }

    // Record operation.
    if (name.empty()) {
        std::cerr << "pulp design record: --name is required to record\n";
        return 2;
    }
    if (!status_str.empty() && !pulp::design::review_status_from_name(status_str)) {
        std::cerr << "pulp design record: invalid --status '" << status_str
                  << "' (needs-review | approved | changes-requested)\n";
        return 2;
    }

    // When --version targets an EXISTING revision, this is an update: start from
    // the stored entry and overwrite only the fields the caller actually passed,
    // so a status-only change (e.g. approve v1) never wipes provenance. A fresh
    // revision (no --version, or a version not yet present) requires --asset.
    const pulp::design::LedgerAsset* existing = nullptr;
    if (!version.empty()) {
        for (const auto& a : ledger.assets)
            if (a.name == name && a.version == version) { existing = &a; break; }
    }
    if (!existing && asset.empty()) {
        std::cerr << "pulp design record: --asset is required for a new asset revision\n";
        return 2;
    }

    pulp::design::LedgerAsset incoming = existing ? *existing : pulp::design::LedgerAsset{};
    incoming.name = name;
    incoming.version = version;
    if (!asset.empty()) incoming.path = asset;
    if (!inherit_from.empty()) incoming.inherit_from = inherit_from;
    if (!source.empty()) incoming.source = source;
    if (!viewport.empty()) incoming.viewport = viewport;
    if (!systems.empty()) incoming.design_systems = systems;
    if (!status_str.empty()) incoming.status = *pulp::design::review_status_from_name(status_str);

    const auto& stored = pulp::design::upsert_asset(ledger, incoming);
    const auto stored_status = stored.status;
    const std::string stored_slug = stored.name + "@" + stored.version;
    if (!write_text_file(ledger_path, pulp::design::ledger_to_json(ledger))) {
        std::cerr << "Error: cannot write " << ledger_path.string() << "\n";
        return 1;
    }
    std::cout << "Recorded " << stored_slug << " [" << pulp::design::review_status_name(stored_status)
              << "] -> " << ledger_path.string() << "\n";
    return 0;
}
void print_gallery_usage() {
    std::cout <<
        "Usage: pulp design gallery [--root <dir>] [--out <dir>] [options]\n"
        "\n"
        "  Render every @dsCard-tagged source file under <root> into one review\n"
        "  artifact: a grouped HTML grid (gallery.html) + a JSON manifest\n"
        "  (gallery.json). Tag a file with a leading magic comment, e.g.:\n"
        "      // @dsCard group=knobs viewport=120x140\n"
        "      // @startingPoint\n"
        "\n"
        "  --root <dir>        Tree to scan for tagged *.js files (default: cwd).\n"
        "  --out <dir>         Where to write the artifact (default: <root>/.pulp-gallery).\n"
        "  --backend <name>    Screenshot backend: skia (default) | auto | coregraphics.\n"
        "  --screenshot <bin>  Path to the pulp-screenshot binary (default: auto-locate).\n"
        "  --no-render         Inventory only: emit the manifest/HTML with no PNGs.\n"
        "  --force             Re-render every card, ignoring the content-hash cache.\n"
        "  --json              Print the manifest to stdout and skip rendering.\n";
}

// Read a bounded prefix of a file — enough to hold the leading magic-comment
// block without slurping a large source body just to check for a tag.
std::string read_file_head(const fs::path& p, size_t max_bytes = 1024) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    std::string head(max_bytes, '\0');
    f.read(head.data(), static_cast<std::streamsize>(max_bytes));
    head.resize(static_cast<size_t>(f.gcount()));
    return head;
}

// A filesystem-safe, collision-resistant PNG name for one card: its path with
// non-alphanumerics folded to '_', then the viewport and content hash. Because
// the hash is in the name, unchanged content at an unchanged viewport maps to an
// already-existing file — that is the content-hash render cache.
std::string card_png_name(const pulp::design::GalleryCard& c) {
    std::string slug;
    for (char ch : c.file) slug += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    return slug + "_" + std::to_string(c.width) + "x" + std::to_string(c.height) + "_" +
           c.content_hash + ".png";
}

// Directories that never hold hand-authored cards and are expensive to walk.
bool is_skippable_dir(const std::string& name) {
    return name == ".git" || name == "build" || name == "node_modules" ||
           name == "external" || name == "planning" || name.rfind("build-", 0) == 0 ||
           name == ".pulp-gallery";
}

int run_gallery(const std::vector<std::string>& rest) {
    fs::path root, out_dir, screenshot_bin;
    std::string backend = "skia";
    bool no_render = false, json_only = false, force = false;

    for (size_t i = 0; i < rest.size(); ++i) {
        const std::string& a = rest[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= rest.size()) {
                std::cerr << "pulp design gallery: " << flag << " requires a value\n";
                return {};
            }
            return rest[++i];
        };
        if (a == "--help" || a == "-h") { print_gallery_usage(); return 0; }
        else if (a == "--root") root = next("--root");
        else if (a == "--out") out_dir = next("--out");
        else if (a == "--backend") backend = next("--backend");
        else if (a == "--screenshot") screenshot_bin = next("--screenshot");
        else if (a == "--no-render") no_render = true;
        else if (a == "--json") json_only = true;
        else if (a == "--force") force = true;
        else { std::cerr << "pulp design gallery: unknown option '" << a << "'\n"; return 2; }
    }

    std::error_code ec;
    if (root.empty()) root = fs::current_path();
    if (!fs::is_directory(root, ec)) {
        std::cerr << "Error: --root " << root << " is not a directory\n";
        return 1;
    }
    if (out_dir.empty()) out_dir = root / ".pulp-gallery";

    // Discover tagged cards. Walk the tree, skip noise dirs, parse each *.js head.
    // Status queries use the error_code overloads so a broken symlink or a status
    // race skips the entry instead of aborting the CLI.
    std::vector<pulp::design::GalleryCard> cards;
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        if (it->is_directory(sec) && is_skippable_dir(it->path().filename().string())) {
            it.disable_recursion_pending();
            continue;
        }
        if (sec || !it->is_regular_file(sec) || sec || it->path().extension() != ".js") continue;
        auto card = pulp::design::parse_gallery_card(read_file_head(it->path()),
                                                     fs::relative(it->path(), root).generic_string());
        if (!card) continue;
        card->content_hash = pulp::design::gallery_content_hash(read_text_file(it->path()));
        cards.push_back(std::move(*card));
    }
    if (ec) {
        // A traversal error mid-walk would otherwise emit a silently-partial
        // gallery as success. Fail loudly instead.
        std::cerr << "Error: scanning " << root << " failed: " << ec.message() << "\n";
        return 1;
    }
    pulp::design::sort_cards(cards);

    // Records the rendered (or cached) PNG path for each card, relative to out_dir.
    std::map<std::string, std::string> png_by_file;
    auto png_rel = [&](const pulp::design::GalleryCard& c) -> std::string {
        auto found = png_by_file.find(c.file);
        return found == png_by_file.end() ? std::string{} : found->second;
    };

    if (json_only) {
        // Fast inventory: manifest to stdout, no render, no files written.
        std::cout << pulp::design::gallery_manifest_json(cards, png_rel) << "\n";
        return 0;
    }

    fs::create_directories(out_dir, ec);
    fs::path cards_dir = out_dir / "cards";

    int rendered = 0, cached = 0, failed = 0;
    if (!no_render && !cards.empty()) {
        fs::create_directories(cards_dir, ec);
        // Locate pulp-screenshot next to this binary, then under <root>/build.
        if (screenshot_bin.empty()) {
            fs::path self_dir = current_executable_path().parent_path();
            for (const fs::path& cand : {platform_executable(self_dir / "pulp-screenshot"),
                                         platform_executable(root / "build" / "tools" /
                                                             "screenshot" / "pulp-screenshot")}) {
                if (fs::exists(cand)) { screenshot_bin = cand; break; }
            }
        }
        if (screenshot_bin.empty() || !fs::exists(screenshot_bin)) {
            std::cerr << "Error: pulp-screenshot not found; pass --screenshot <bin> or use "
                         "--no-render.\n";
            return 1;
        }
        // A PNG counts as a valid render (and a cache hit) only if it exists and
        // is non-empty — an interrupted or crashed render can leave a zero-byte
        // file, which must not be trusted forever.
        auto is_valid_png = [](const fs::path& p) {
            std::error_code fec;
            return fs::exists(p, fec) && fs::file_size(p, fec) > 0 && !fec;
        };
        for (const auto& c : cards) {
            fs::path png = cards_dir / card_png_name(c);
            std::string rel = (fs::path("cards") / card_png_name(c)).generic_string();
            // Cache key is the card's OWN bytes at its viewport (hash embedded in
            // the filename). It does NOT cover imported modules, assets, or theme
            // tokens the card pulls in — pass --force to re-render when those
            // change.
            if (!force && is_valid_png(png)) {
                png_by_file[c.file] = rel;
                ++cached;
                continue;
            }
            // Spawn the renderer with an argv array (no shell): the command
            // begins with a quoted path and carries quoted args, exactly the
            // shape std::system's `cmd /c` mis-parses on Windows.
            std::vector<std::string> args = {
                "--script",  (root / c.file).string(),
                "--output",  png.string(),
                "--width",   std::to_string(c.width),
                "--height",  std::to_string(c.height),
                "--backend", backend};
            auto result = pulp::platform::exec(screenshot_bin.string(), args, /*timeout_ms=*/120000);
            if (result.exit_code == 0 && is_valid_png(png)) {
                png_by_file[c.file] = rel;
                ++rendered;
            } else {
                std::cerr << "Warning: render failed for " << c.file;
                if (result.timed_out) std::cerr << " (timed out)";
                else std::cerr << " (exit " << result.exit_code << ")";
                std::cerr << "\n";
                fs::remove(png, ec);  // drop any partial/empty output so it is not cached
                ++failed;
            }
        }
    }

    if (!write_text_file(out_dir / "gallery.json",
                         pulp::design::gallery_manifest_json(cards, png_rel)) ||
        !write_text_file(out_dir / "gallery.html",
                         pulp::design::gallery_html(cards, png_rel))) {
        std::cerr << "Error: cannot write gallery artifact under " << out_dir << "\n";
        return 1;
    }

    std::cout << "Gallery: " << cards.size() << " card(s)";
    if (!no_render) std::cout << " (" << rendered << " rendered, " << cached << " cached, "
                              << failed << " failed)";
    std::cout << " -> " << (out_dir / "gallery.html").string() << "\n";
    return failed > 0 ? 1 : 0;
}

} // namespace

int cmd_design(const std::vector<std::string>& args) {
    // `pulp design lint` and `pulp design diff` operate on DESIGN.md
    // files and do NOT launch the live design tool. They short-circuit
    // before the design-tool build path.
    if (!args.empty()) {
        if (args[0] == "lint") {
            return run_lint(std::vector<std::string>(args.begin() + 1, args.end()));
        }
        if (args[0] == "diff") {
            return run_diff(std::vector<std::string>(args.begin() + 1, args.end()));
        }
        if (args[0] == "compile") {
            return run_compile(std::vector<std::string>(args.begin() + 1, args.end()));
        }
        if (args[0] == "lint-adherence") {
            return run_lint_adherence(std::vector<std::string>(args.begin() + 1, args.end()));
        }
        if (args[0] == "record") {
            return run_record(std::vector<std::string>(args.begin() + 1, args.end()));
        }
        if (args[0] == "gallery") {
            return run_gallery(std::vector<std::string>(args.begin() + 1, args.end()));
        }
    }

    fs::path cwd_root = find_project_root();
    fs::path build_dir;
    fs::path script_path;
    std::vector<std::string> pass_through;
    bool build_dir_explicit = false;
    std::string root_reason = cwd_root.empty() ? "" : "current checkout";
    std::string build_reason;
    std::string script_reason;

    bool watch_mode = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--watch" || args[i] == "-w") {
            watch_mode = true;
            continue;
        }
        if (args[i] == "--build-dir") {
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-')) {
                std::cerr << "pulp design: --build-dir requires a value\n";
                return 2;
            }
            build_dir = fs::absolute(args[++i]);
            build_dir_explicit = true;
            build_reason = "explicit --build-dir";
            continue;
        }
        if (args[i] == "--script") {
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-')) {
                std::cerr << "pulp design: --script requires a value\n";
                return 2;
            }
            script_path = fs::absolute(args[++i]);
            script_reason = "explicit --script";
            continue;
        }
        pass_through.push_back(args[i]);
    }

    if (script_path.empty() && !pass_through.empty() && !pass_through.front().empty()
        && pass_through.front()[0] != '-') {
        fs::path candidate = pass_through.front();
        auto ext = candidate.extension().string();
        if (ext == ".js" || ext == ".mjs" || ext == ".cjs") {
            script_path = candidate.is_absolute() ? candidate : fs::absolute(candidate);
            script_reason = "positional script argument";
            pass_through.erase(pass_through.begin());
        }
    }

    auto binary_build_dir = build_dir_from_current_binary();
    auto binary_root = cmake_home_directory(binary_build_dir);
    auto cache_root = cmake_home_directory(build_dir);
    pulp::cli::DesignBindingInput binding_input;
    binding_input.cwd_root = cwd_root;
    binding_input.build_dir = build_dir;
    binding_input.script_path = script_path;
    binding_input.script_root = script_path.empty() ? fs::path{} : find_project_root_from(script_path.parent_path());
    binding_input.build_dir_cache_root = cache_root;
    binding_input.binary_build_dir = binary_build_dir;
    binding_input.binary_root = binary_root;
    binding_input.build_dir_explicit = build_dir_explicit;
    binding_input.script_explicit = !script_reason.empty() && script_reason != "positional script argument";

    auto binding = pulp::cli::resolve_design_binding(binding_input);
    if (!binding.ok) {
        std::cerr << "Error: " << binding.error << "\n";
        return 1;
    }

    auto root = binding.root;
    build_dir = binding.build_dir;
    script_path = binding.script_path;
    root_reason = binding.root_reason;
    build_reason = binding.build_reason;
    script_reason = binding.script_reason;

    if (!fs::exists(script_path)) {
        std::cerr << "Error: design tool script not found at " << script_path << "\n";
        return 1;
    }

    std::cout << "Design root:  " << root << " (" << root_reason << ")\n";
    std::cout << "Build dir:    " << build_dir << " (" << build_reason << ")\n";
    std::cout << "Script:       " << script_path << " (" << script_reason << ")\n";

    int rc = ensure_repo_build_configured(root, build_dir);
    if (rc != 0) return rc;

    rc = run_with_spinner("cmake --build " + shell_quote(build_dir) + " --target pulp-design-tool",
                          "Building design tool");
    if (rc != 0) return rc;

    std::vector<fs::path> candidates = {
        platform_executable(build_dir / "tools" / "design" / "pulp-design"),
        platform_executable(build_dir / "examples" / "design-tool" / "pulp-design-tool"),
    };

    fs::path design_bin;
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            design_bin = candidate;
            break;
        }
    }

    if (design_bin.empty()) {
        std::cerr << "Error: pulp-design-tool not found after build in " << build_dir << "\n";
        return 1;
    }

    if (watch_mode) {
        // Watch mode: rebuild design tool on C++ changes, auto-relaunch.
        // JS hot-reload is handled by the design tool's internal HotReloader.
        WatchOptions opts;
        opts.root = root;
        opts.build_dir = build_dir;
        opts.build_args = {"--target", "pulp-design-tool"};
        opts.launch_target = design_bin.string();
        opts.launch_args = {script_path.string()};
        for (const auto& arg : pass_through) opts.launch_args.push_back(arg);
        return watch_loop(opts);
    }

    std::string cmd = shell_quote(design_bin) + " " + shell_quote(script_path);
    for (const auto& arg : pass_through) {
        cmd += " " + shell_quote(arg);
    }
    return run(cmd);
}
