// test_cli_import_install.cpp — deterministic tests for the URL-driven importer
// install path (`tools/cli/importer_git_install.cpp`).
//
// Everything here installs from a REAL LOCAL GIT REPOSITORY built in a temp dir
// (git clones a local path with the user's own git) — no network, no shipped
// registry, no per-platform artifact. Coverage:
//   * tool.json parsing (id / category / spi window / terms fields).
//   * successful install of a local importer through the interactive terms gate,
//     with the tree placed under ~/.pulp/tools/<id>/ and a record written.
//   * SPI-window rejection (importer ahead of the SDK) refuses without installing.
//   * terms acceptance is recorded and re-prompts when the terms text changes.
//   * uninstall removes the tree + record.
//   * PRIVACY INVARIANT: an unreachable URL fails with a fixed, URL-agnostic
//     SDK message that reveals nothing about existence or public/private status,
//     and is byte-identical across different unreachable URLs.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/import_terms.hpp"
#include "../tools/cli/importer_git_install.hpp"
#include "../tools/cli/tool_registry.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <pulp/platform/child_process.hpp>

namespace fs = std::filesystem;
namespace ii = pulp::cli::import_install;
namespace it = pulp::cli::import_terms;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> seq{0};
        const int n = seq.fetch_add(1);
        path = fs::temp_directory_path() /
               ("pulp-import-install-test-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                std::to_string(n));
        fs::create_directories(path);
        std::error_code ec;
        auto canon = fs::weakly_canonical(path, ec);
        if (!ec) path = canon;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ScopedEnv {
    std::string key;
    bool had = false;
    std::string old;
    ScopedEnv(std::string k, const std::string& value) : key(std::move(k)) {
        if (const char* e = std::getenv(key.c_str())) { had = true; old = e; }
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }
    ~ScopedEnv() {
#ifdef _WIN32
        _putenv_s(key.c_str(), had ? old.c_str() : "");
#else
        if (had) setenv(key.c_str(), old.c_str(), 1);
        else unsetenv(key.c_str());
#endif
    }
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << body;
}

void git(const fs::path& repo, const std::vector<std::string>& args) {
    std::vector<std::string> full = {"-C", repo.string(),
                                     "-c", "user.email=test@pulp.invalid",
                                     "-c", "user.name=Pulp Test",
                                     "-c", "commit.gpgsign=false",
                                     "-c", "init.defaultBranch=main"};
    for (const auto& a : args) full.push_back(a);
    auto r = pulp::platform::exec("git", full, 60000);
    INFO("git " << (args.empty() ? "" : args[0]) << " -> " << r.exit_code
                << "\n" << r.stderr_output);
    REQUIRE(r.exit_code == 0);
}

std::string default_tool_json(int spi_min = 0, int spi_max = 0) {
    return std::string("{\n") +
           "  \"id\": \"example-importer\",\n" +
           "  \"display_name\": \"Example Importer\",\n" +
           "  \"category\": \"importer\",\n" +
           "  \"pinned_version\": \"0.1.0\",\n" +
           "  \"spi_min\": " + std::to_string(spi_min) + ",\n" +
           "  \"spi_max\": " + std::to_string(spi_max) + ",\n" +
           "  \"terms_version\": \"1\",\n" +
           "  \"terms_file\": \"TERMS.md\"\n" +
           "}\n";
}

// Build a real local git repo that looks like an add-on importer, and return
// its path (usable directly as a `git clone` source).
fs::path make_importer_repo(const fs::path& dir, const std::string& tool_json,
                            const std::string& terms_body) {
    write_file(dir / "tool.json", tool_json);
    write_file(dir / "TERMS.md", terms_body);
    write_file(dir / "known-frameworks.json",
               "{\n  \"schema\": \"pulp.import.known_frameworks.v0\",\n"
               "  \"frameworks\": []\n}\n");
    git(dir, {"init", "-q"});
    git(dir, {"add", "-A"});
    git(dir, {"commit", "-q", "-m", "importer"});
    return dir;
}

}  // namespace

TEST_CASE("parse_tool_manifest reads the importer fields", "[cli][import-install]") {
    auto ok = ii::parse_tool_manifest(default_tool_json(0, 3));
    REQUIRE(ok.parsed);
    REQUIRE(ok.id == "example-importer");
    REQUIRE(ok.category == "importer");
    REQUIRE(ok.spi_min == 0);
    REQUIRE(ok.spi_max == 3);
    REQUIRE(ok.terms_file == "TERMS.md");
    REQUIRE(ok.terms_version == "1");

    auto no_id = ii::parse_tool_manifest("{\"category\":\"importer\"}");
    REQUIRE_FALSE(no_id.parsed);
    REQUIRE(no_id.error.find("id") != std::string::npos);

    auto wrong_cat = ii::parse_tool_manifest("{\"id\":\"x\",\"category\":\"binary\"}");
    REQUIRE_FALSE(wrong_cat.parsed);
    REQUIRE(wrong_cat.error.find("importer") != std::string::npos);

    auto not_obj = ii::parse_tool_manifest("[]");
    REQUIRE_FALSE(not_obj.parsed);
}

TEST_CASE("install from a local git repo through the terms gate records + installs",
          "[cli][import-install]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    ScopedEnv sdk{"PULP_SDK_VERSION", "0.350.0"};

    auto repo = make_importer_repo(tmp.path / "repo", default_tool_json(),
                                   "# Terms\nAccept me.\n");

    std::istringstream in("accept\n");
    std::ostringstream out;
    auto res = ii::install_from_git(repo.string(), /*accept_flag=*/false,
                                    /*force=*/false, it::GateIo{in, out, true});
    INFO(res.error);
    REQUIRE(res.ok);
    REQUIRE(res.importer_id == "example-importer");
    REQUIRE(res.terms_prompted);
    REQUIRE(out.str().find("Importer Terms of Use") != std::string::npos);

    // Tree placed under ~/.pulp/tools/<id>/ with the repo contents.
    REQUIRE(res.install_dir == pulp::cli::tools::tools_dir() / "example-importer");
    REQUIRE(fs::exists(res.install_dir / "tool.json"));
    REQUIRE(fs::exists(res.install_dir / "known-frameworks.json"));

    // Install record written under ~/.pulp/importers/.
    REQUIRE(fs::exists(res.record_path));
    std::ifstream rf(res.record_path);
    std::string rec{std::istreambuf_iterator<char>(rf), std::istreambuf_iterator<char>()};
    REQUIRE(rec.find("\"id\": \"example-importer\"") != std::string::npos);
    REQUIRE(rec.find("\"terms_version\": \"1\"") != std::string::npos);

    // Acceptance persisted, keyed by the terms hash.
    auto store = it::load_acceptance_store(it::acceptance_store_path());
    REQUIRE(it::is_accepted(store, "example-importer",
                            it::terms_hash("# Terms\nAccept me.\n")));
}

TEST_CASE("install refuses when the importer SPI window is outside the SDK's",
          "[cli][import-install]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    ScopedEnv sdk{"PULP_SDK_VERSION", "0.350.0"};

    // Importer speaks SPI v5..v9; the SDK speaks v0 — no overlap.
    auto repo = make_importer_repo(tmp.path / "repo", default_tool_json(5, 9),
                                   "# Terms\n");

    std::istringstream in("accept\n");
    std::ostringstream out;
    auto res = ii::install_from_git(repo.string(), false, false,
                                    it::GateIo{in, out, true});
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.stage == ii::InstallResult::Stage::SpiWindow);
    REQUIRE(res.error.find("import-SPI") != std::string::npos);
    // Nothing installed, and the terms prompt never ran (we refuse before it).
    REQUIRE_FALSE(fs::exists(pulp::cli::tools::tools_dir() / "example-importer"));
    REQUIRE(out.str().find("Importer Terms of Use") == std::string::npos);
}

TEST_CASE("changed terms re-prompt on reinstall", "[cli][import-install]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    ScopedEnv sdk{"PULP_SDK_VERSION", "0.350.0"};

    auto repo = make_importer_repo(tmp.path / "repo", default_tool_json(),
                                   "# Terms\nversion one\n");

    // First install: interactive accept.
    {
        std::istringstream in("accept\n");
        std::ostringstream out;
        auto res = ii::install_from_git(repo.string(), false, false,
                                        it::GateIo{in, out, true});
        REQUIRE(res.ok);
        REQUIRE(res.terms_prompted);
    }

    // Reinstall (force) with EMPTY input: already accepted (same hash) → the gate
    // passes through without reading input or prompting.
    {
        std::istringstream in("");
        std::ostringstream out;
        auto res = ii::install_from_git(repo.string(), false, /*force=*/true,
                                        it::GateIo{in, out, true});
        REQUIRE(res.ok);
        REQUIRE_FALSE(res.terms_prompted);
        REQUIRE(out.str().find("Importer Terms of Use") == std::string::npos);
    }

    // Change the terms body (new commit) → the hash changes → the gate must
    // re-prompt. With empty input the prompt is declined and install is refused.
    write_file(repo / "TERMS.md", "# Terms\nversion TWO — materially different\n");
    git(repo, {"add", "-A"});
    git(repo, {"commit", "-q", "-m", "update terms"});
    {
        std::istringstream in("");  // decline by not typing "accept"
        std::ostringstream out;
        auto res = ii::install_from_git(repo.string(), false, true,
                                        it::GateIo{in, out, true});
        REQUIRE_FALSE(res.ok);
        REQUIRE(res.stage == ii::InstallResult::Stage::Terms);
        REQUIRE(out.str().find("Importer Terms of Use") != std::string::npos);
    }
}

TEST_CASE("the --accept-importer-terms flag skips the prompt for CI",
          "[cli][import-install]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    ScopedEnv sdk{"PULP_SDK_VERSION", "0.350.0"};

    auto repo = make_importer_repo(tmp.path / "repo", default_tool_json(),
                                   "# Terms\nCI accept\n");

    std::istringstream in("");
    std::ostringstream out;
    // Non-interactive + flag: installs without prompting, and still records.
    auto res = ii::install_from_git(repo.string(), /*accept_flag=*/true, false,
                                    it::GateIo{in, out, false});
    REQUIRE(res.ok);
    REQUIRE_FALSE(res.terms_prompted);
    auto store = it::load_acceptance_store(it::acceptance_store_path());
    REQUIRE(it::is_accepted(store, "example-importer",
                            it::terms_hash("# Terms\nCI accept\n")));
}

TEST_CASE("uninstall removes the tree and record", "[cli][import-install]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    ScopedEnv sdk{"PULP_SDK_VERSION", "0.350.0"};

    auto repo = make_importer_repo(tmp.path / "repo", default_tool_json(),
                                   "# Terms\n");
    std::istringstream in("");
    std::ostringstream out;
    auto res = ii::install_from_git(repo.string(), true, false,
                                    it::GateIo{in, out, false});
    REQUIRE(res.ok);
    REQUIRE(fs::exists(res.install_dir));
    REQUIRE(fs::exists(res.record_path));

    REQUIRE(ii::run_import_uninstall({"example-importer"}) == 0);
    REQUIRE_FALSE(fs::exists(res.install_dir));
    REQUIRE_FALSE(fs::exists(res.record_path));

    // Second uninstall is a no-op failure (nothing to remove).
    REQUIRE(ii::run_import_uninstall({"example-importer"}) == 1);
}

// ── Privacy invariant ──

TEST_CASE("an unreachable URL leaks nothing about the repository",
          "[cli][import-install][privacy]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    ScopedEnv sdk{"PULP_SDK_VERSION", "0.350.0"};

    auto fetch = [&](const std::string& url) {
        std::istringstream in("");
        std::ostringstream out;
        return ii::install_from_git(url, true, false, it::GateIo{in, out, false});
    };

    // Two unreachable URLs the SDK cannot tell apart — one that "looks" like a
    // private repo, one that "looks" public. The SDK only knows the URL string.
    auto a = fetch((tmp.path / "does-not-exist-a").string());
    auto b = fetch((tmp.path / "does-not-exist-b").string());

    REQUIRE_FALSE(a.ok);
    REQUIRE_FALSE(b.ok);
    REQUIRE(a.stage == ii::InstallResult::Stage::Fetch);
    REQUIRE(b.stage == ii::InstallResult::Stage::Fetch);

    // The SDK-authored message is URL-agnostic and byte-identical across URLs.
    REQUIRE(a.error == b.error);
    REQUIRE(a.error == "could not fetch importer from the provided URL");

    // It reveals nothing about existence or visibility.
    for (const char* leak : {"private", "public", "exists", "does not exist",
                             "not found", "permission"}) {
        REQUIRE(a.error.find(leak) == std::string::npos);
    }

    // git's own words are surfaced separately (not hidden), so a human still sees
    // the real reason — but that is git's output, not an SDK claim.
    REQUIRE_FALSE(a.git_output.empty());

    // Nothing was installed.
    REQUIRE_FALSE(fs::exists(pulp::cli::tools::tools_dir() / "example-importer"));
    REQUIRE_FALSE(fs::exists(pulp::cli::tools::importer_records_dir()));
}
