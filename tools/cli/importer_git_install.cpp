// SPDX-License-Identifier: MIT
//
// importer_git_install.cpp — see importer_git_install.hpp for the contract and
// the privacy invariant. This unit names no framework and no vendor; framework
// identity is DATA read from the cloned repo.

#include "importer_git_install.hpp"

#include "import_spi.hpp"
#include "import_terms.hpp"
#include "json_parser.hpp"
#include "tool_registry.hpp"

// The generated version header is only on the CLI binary target's include path;
// tests drive the SDK version through PULP_SDK_VERSION, so the include is
// optional (mirrors importer_install.cpp).
#if __has_include(<pulp_version_gen.h>)
#  include <pulp_version_gen.h>
#endif

#include <pulp/platform/child_process.hpp>

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

namespace pulp::cli::import_install {

namespace {

namespace pkg = pulp::cli::pkg;

// ── Console helpers ──

std::string c_green(const std::string& s) { return "\033[32m" + s + "\033[0m"; }
std::string c_red(const std::string& s) { return "\033[31m" + s + "\033[0m"; }
std::string c_dim(const std::string& s) { return "\033[2m" + s + "\033[0m"; }

// ── Small IO helpers ──

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool write_text(const fs::path& p, const std::string& body) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << body;
    return f.good();
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
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

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// The SDK version the running host reports. Env-first (the CLI exports it for
// subprocesses; tests set it) with a compiled fallback.
std::string host_sdk_version() {
    if (const char* v = std::getenv("PULP_SDK_VERSION"); v && *v) return v;
#ifdef PULP_SDK_VERSION_GENERATED
    return PULP_SDK_VERSION_GENERATED;
#else
    return "0.0.0";
#endif
}

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

// A unique staging directory on the SAME filesystem as the install root, so the
// final placement is an atomic rename rather than a cross-device copy.
fs::path make_staging_dir() {
    static std::atomic<uint64_t> seq{0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string name = "clone-" + std::to_string(static_cast<unsigned long long>(stamp)) +
                       "-" + std::to_string(seq.fetch_add(1));
    return pulp::cli::tools::tools_dir() / ".staging" / name;
}

}  // namespace

// ── tool.json parsing ──

ToolManifest parse_tool_manifest(const std::string& tool_json_text) {
    ToolManifest m;
    if (trim(tool_json_text).empty()) {
        m.error = "tool.json is empty";
        return m;
    }
    pkg::JsonParser parser{tool_json_text};
    auto root = parser.parse();
    if (root.type != pkg::JsonValue::Object) {
        m.error = "tool.json is not a JSON object";
        return m;
    }
    if (auto v = root.get("id")) m.id = v->as_string();
    if (auto v = root.get("category")) m.category = v->as_string();
    if (auto v = root.get("display_name")) m.display_name = v->as_string();
    if (auto v = root.get("pinned_version")) m.pinned_version = v->as_string();
    if (auto v = root.get("spi_min")) m.spi_min = v->as_int();
    if (auto v = root.get("spi_max")) m.spi_max = v->as_int();
    if (auto v = root.get("sdk_min")) m.sdk_min = v->as_string();
    if (auto v = root.get("sdk_max")) m.sdk_max = v->as_string();
    if (auto v = root.get("terms_version")) m.terms_version = v->as_string();
    if (auto v = root.get("terms_file")) m.terms_file = v->as_string();

    if (m.id.empty()) {
        m.error = "tool.json is missing a non-empty \"id\"";
        return m;
    }
    if (m.category != "importer") {
        m.error = "tool.json category is \"" + m.category +
                  "\", expected \"importer\"";
        return m;
    }
    m.parsed = true;
    return m;
}

// ── install ──

InstallResult install_from_git(const std::string& url,
                               bool accept_terms_flag,
                               bool force,
                               import_terms::GateIo io) {
    InstallResult r;
    std::error_code ec;

    if (trim(url).empty()) {
        r.error = "an importer URL is required";
        r.stage = InstallResult::Stage::Fetch;
        return r;
    }

    // 1. Clone with the user's own git. The clone is the ONLY place the SDK
    //    touches the URL, and its failure must stay indistinguishable from any
    //    other unreachable URL: we surface git's own stderr and a single
    //    URL-agnostic message. We never probe, classify, or record whether the
    //    repository exists or is public/private — we cannot, and must not.
    auto staging = make_staging_dir();
    fs::remove_all(staging, ec);
    fs::create_directories(staging.parent_path(), ec);

    auto clone = pulp::platform::exec(
        "git", {"clone", "--depth", "1", url, staging.string()}, 600000);

    if (clone.exit_code != 0) {
        fs::remove_all(staging, ec);
        // URL-agnostic, deterministic message. git's own words (which may differ
        // for a missing local path vs an unreachable remote) live only in
        // git_output, surfaced verbatim — the SDK adds no claim of its own.
        r.error = "could not fetch importer from the provided URL";
        r.git_output = clone.stderr_output.empty() ? clone.stdout_output
                                                    : clone.stderr_output;
        r.stage = InstallResult::Stage::Fetch;
        return r;
    }

    // 2. Read tool.json FROM THE CLONE (never from anything the SDK ships).
    auto tool_json_path = staging / "tool.json";
    if (!fs::exists(tool_json_path, ec)) {
        fs::remove_all(staging, ec);
        r.error = "the cloned repository has no tool.json at its root";
        r.stage = InstallResult::Stage::Manifest;
        return r;
    }
    auto manifest = parse_tool_manifest(read_text(tool_json_path));
    if (!manifest.parsed) {
        fs::remove_all(staging, ec);
        r.error = "invalid tool.json: " + manifest.error;
        r.stage = InstallResult::Stage::Manifest;
        return r;
    }
    r.importer_id = manifest.id;
    r.installed_version = manifest.pinned_version;

    // 3. Enforce the SPI version window loudly. The SDK speaks exactly one SPI
    //    version today (kSpiVersion); the window is the degenerate [v, v]. Refuse
    //    rather than half-install on a mismatch. Reuses the tested compat check
    //    (and its loud "upgrade Pulp" / "upgrade the importer" messages).
    const int sdk_spi = pulp::cli::import_spi::kSpiVersion;
    pulp::cli::tools::ToolDescriptor desc;
    desc.id = manifest.id;
    desc.category = "importer";
    desc.spi_min = manifest.spi_min;
    desc.spi_max = manifest.spi_max;
    desc.sdk_min = manifest.sdk_min;
    desc.sdk_max = manifest.sdk_max;
    auto compat = pulp::cli::tools::check_importer_compat(desc, host_sdk_version(),
                                                          sdk_spi, sdk_spi);
    if (!compat.ok) {
        fs::remove_all(staging, ec);
        r.error = compat.error;
        r.stage = InstallResult::Stage::SpiWindow;
        return r;
    }

    // 4. Accept-to-run terms gate. The terms body is DATA carried by the repo
    //    (its terms_file); the SDK surfaces it, hashes it, and records acceptance
    //    keyed by importer id + hash under ~/.pulp — a changed body re-prompts.
    import_terms::TermsDescriptor td;
    td.importer_id = manifest.id;
    td.terms_version = manifest.terms_version;
    td.vendor_id = manifest.id;
    if (!manifest.terms_file.empty()) {
        auto terms_path = staging / manifest.terms_file;
        if (!fs::exists(terms_path, ec)) {
            fs::remove_all(staging, ec);
            r.error = "tool.json names terms_file \"" + manifest.terms_file +
                      "\" but that file is not in the repository";
            r.stage = InstallResult::Stage::Terms;
            return r;
        }
        td.terms_text = read_text(terms_path);
    }

    const bool would_prompt =
        import_terms::has_terms(td) && !accept_terms_flag && io.interactive &&
        !import_terms::is_accepted(
            import_terms::load_acceptance_store(import_terms::acceptance_store_path()),
            td.importer_id, import_terms::terms_hash(td.terms_text));

    auto gate = import_terms::run_gate(td, import_terms::acceptance_store_path(),
                                       accept_terms_flag, iso_utc_now(), io);
    r.terms_prompted = would_prompt && (gate == import_terms::GateResult::Accepted ||
                                        gate == import_terms::GateResult::Declined ||
                                        gate == import_terms::GateResult::StoreError);
    switch (gate) {
        case import_terms::GateResult::Accepted:
        case import_terms::GateResult::NoTermsToAccept:
            break;  // proceed
        case import_terms::GateResult::Declined:
            fs::remove_all(staging, ec);
            r.error = "importer terms were not accepted";
            r.stage = InstallResult::Stage::Terms;
            return r;
        case import_terms::GateResult::NonInteractive:
            fs::remove_all(staging, ec);
            r.error = "importer terms require acceptance; re-run in a terminal or "
                      "pass --accept-importer-terms";
            r.stage = InstallResult::Stage::Terms;
            return r;
        case import_terms::GateResult::StoreError:
            fs::remove_all(staging, ec);
            r.error = "could not record importer-terms acceptance under ~/.pulp";
            r.stage = InstallResult::Stage::Terms;
            return r;
    }

    // 5. Place the tree at ~/.pulp/tools/<id>/ and write the install record.
    auto install_dir = pulp::cli::tools::tools_dir() / manifest.id;
    auto record_path =
        pulp::cli::tools::importer_records_dir() / (manifest.id + ".json");

    if (!force && fs::exists(record_path, ec) && fs::exists(install_dir, ec)) {
        // Idempotent: already installed and not forced. Discard the fresh clone.
        fs::remove_all(staging, ec);
        r.ok = true;
        r.install_dir = install_dir;
        r.record_path = record_path;
        return r;
    }

    fs::remove_all(install_dir, ec);
    fs::create_directories(install_dir.parent_path(), ec);
    fs::rename(staging, install_dir, ec);
    if (ec) {
        // Cross-device or racy rename — fall back to a recursive copy.
        ec.clear();
        fs::copy(staging, install_dir,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        fs::remove_all(staging, ec);
        if (fs::is_empty(install_dir, ec)) {
            r.error = "failed to place the importer under " + install_dir.string();
            r.stage = InstallResult::Stage::Io;
            return r;
        }
    }

    std::ostringstream rec;
    rec << "{\n"
        << "  \"id\": \"" << json_escape(manifest.id) << "\",\n"
        << "  \"version\": \"" << json_escape(manifest.pinned_version) << "\",\n"
        << "  \"source\": \"" << json_escape(url) << "\",\n"
        << "  \"install_dir\": \"" << json_escape(install_dir.string()) << "\",\n"
        << "  \"spi_min\": " << manifest.spi_min << ",\n"
        << "  \"spi_max\": " << manifest.spi_max << ",\n"
        << "  \"sdk_version\": \"" << json_escape(host_sdk_version()) << "\",\n"
        << "  \"terms_version\": \"" << json_escape(manifest.terms_version) << "\",\n"
        << "  \"skill_path\": \"\"\n"
        << "}\n";
    if (!write_text(record_path, rec.str())) {
        r.error = "installed the tree but failed to write the record at " +
                  record_path.string();
        r.stage = InstallResult::Stage::Io;
        return r;
    }

    r.ok = true;
    r.install_dir = install_dir;
    r.record_path = record_path;
    return r;
}

// ── CLI wrappers ──

int run_import_install(const std::vector<std::string>& args) {
    std::string url;
    bool accept_flag = false;
    bool force = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--accept-importer-terms") accept_flag = true;
        else if (a == "--force") force = true;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "pulp import install: unknown option '" << a << "'\n";
            return 2;
        } else if (url.empty()) {
            url = a;
        } else {
            std::cerr << "pulp import install: unexpected extra argument '" << a << "'\n";
            return 2;
        }
    }
    if (url.empty()) {
        std::cerr << "pulp import install: a git URL is required\n"
                     "Usage: pulp import install <url> [--accept-importer-terms] [--force]\n";
        return 2;
    }

#ifdef _WIN32
    const bool interactive = _isatty(_fileno(stdin)) != 0;
#else
    const bool interactive = ::isatty(STDIN_FILENO) != 0;
#endif
    import_terms::GateIo io{std::cin, std::cout, interactive};
    auto res = install_from_git(url, accept_flag, force, io);
    if (!res.ok) {
        std::cerr << c_red("✗") << " " << res.error << "\n";
        if (!res.git_output.empty()) {
            // git's own words, surfaced verbatim.
            std::cerr << c_dim(trim(res.git_output)) << "\n";
        }
        return 1;
    }
    std::cout << c_green("✓") << " Installed importer " << res.importer_id;
    if (!res.installed_version.empty()) std::cout << " " << res.installed_version;
    std::cout << "\n";
    std::cout << "  " << c_dim(res.install_dir.string()) << "\n";
    std::cout << "  " << c_dim("Run `pulp import detect <dir>` to use it.") << "\n";
    return 0;
}

int run_import_uninstall(const std::vector<std::string>& args) {
    std::string id;
    for (const auto& a : args) {
        if (!a.empty() && a[0] == '-') {
            std::cerr << "pulp import uninstall: unknown option '" << a << "'\n";
            return 2;
        }
        if (id.empty()) id = a;
        else {
            std::cerr << "pulp import uninstall: unexpected extra argument '" << a << "'\n";
            return 2;
        }
    }
    if (id.empty()) {
        std::cerr << "pulp import uninstall: an importer id is required\n"
                     "Usage: pulp import uninstall <id>\n";
        return 2;
    }
    if (pulp::cli::tools::uninstall_importer(id)) {
        std::cout << c_green("✓") << " Uninstalled importer " << id << "\n";
        return 0;
    }
    std::cerr << c_red("✗") << " importer '" << id << "' is not installed\n";
    return 1;
}

}  // namespace pulp::cli::import_install
