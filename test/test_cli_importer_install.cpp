// test_cli_importer_install.cpp — deterministic tests for the framework-importer
// install mechanism (`tools/cli/importer_install.cpp`, plan item #19).
//
// Everything here runs against a MOCK LOCAL PACKAGE built in a temp dir and
// installed via the `--from <path>` source — no network, no real importer
// binary, no system libclang. Coverage:
//   * SHA-256 known-answer vectors (the hand-rolled digest).
//   * sha256_file_hex over a real file.
//   * version-window enforcement (sdk + spi) with loud messages.
//   * install from a checksummed local package: succeeds, records, drops skill.
//   * checksum mismatch refuses.
//   * sdk/spi out-of-window refuses.
//   * uninstall removes skill + record + install tree.
//   * `pulp tool install/uninstall` dispatch + `pulp add` alias hit the path.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/tool_registry.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pulp/platform/child_process.hpp>

namespace fs = std::filesystem;
using namespace pulp::cli::tools;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        const int n = seq.fetch_add(1);
        path = base / ("pulp-importer-install-test-" +
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
        set(value);
    }
    ~ScopedEnv() {
#ifdef _WIN32
        _putenv_s(key.c_str(), had ? old.c_str() : "");
#else
        if (had) setenv(key.c_str(), old.c_str(), 1);
        else unsetenv(key.c_str());
#endif
    }
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }
};

struct ScopedCurrentPath {
    fs::path old = fs::current_path();
    explicit ScopedCurrentPath(const fs::path& p) { fs::current_path(p); }
    ~ScopedCurrentPath() { std::error_code ec; fs::current_path(old, ec); }
};

struct ScopedOutput {
    std::ostringstream out;
    std::ostringstream err;
    std::streambuf* old_out = std::cout.rdbuf(out.rdbuf());
    std::streambuf* old_err = std::cerr.rdbuf(err.rdbuf());
    ~ScopedOutput() {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
    }
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << body;
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7] = {};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

// Build a mock importer package (tar.gz) at <out>, containing an entrypoint
// file and a SKILL.md. Returns the package path. Uses the system tar, which is
// present on all CI platforms.
fs::path build_mock_package(const fs::path& staging, const fs::path& out,
                            const std::string& skill_body) {
    write_file(staging / "bin" / "importer", "#!/bin/sh\necho mock-importer\n");
    write_file(staging / "SKILL.md", skill_body);
    write_file(staging / "libclang" / "VERSION", "bundled-libclang-vX\n");

    // tar -czf <out> -C <staging> .
    auto r = pulp::platform::exec(
        "tar", {"-czf", out.string(), "-C", staging.string(), "."}, 60000);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(out));
    return out;
}

// A registry fixture with a single importer descriptor pointing at a local
// package via importer_artifacts (url_template carries a local path so the
// non --from path also works in the alias test).
ToolDescriptor make_importer(const std::string& sha,
                             const std::string& sdk_min = "0.0.0",
                             const std::string& sdk_max = "999.0.0",
                             int spi_min = 0, int spi_max = 0) {
    ToolDescriptor t;
    t.id = "framework-x-importer";
    t.display_name = "Framework X Importer";
    t.category = "importer";
    t.description = "Imports Framework X projects";
    t.license = "MIT";
    t.install_method = "importer_package";
    t.pinned_version = "1.4.2";
    t.frameworks = {"framework-x"};
    t.spi_min = spi_min;
    t.spi_max = spi_max;
    t.sdk_min = sdk_min;
    t.sdk_max = sdk_max;
    t.capabilities = {"detect", "inspect", "emit"};
    t.skill_source = "SKILL.md";
    t.skill_name = "framework-x-importer";
    t.terms_version = "2026-06-07";
    t.vendor_id = "framework-x";
    ImporterArtifact a;
    a.archive_format = "tar.gz";
    a.sha256 = sha;
    t.importer_artifacts[current_platform_key()] = a;
    return t;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

}  // namespace

TEST_CASE("sha256 matches known answer vectors", "[cli][importer][sha256][issue-19]") {
    TempDir tmp;
    // FIPS 180-4 well-known vectors.
    write_file(tmp.path / "empty", "");
    REQUIRE(sha256_file_hex(tmp.path / "empty") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    write_file(tmp.path / "abc", "abc");
    REQUIRE(sha256_file_hex(tmp.path / "abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // 1,000,000 'a' characters -> the classic long vector.
    {
        std::ofstream f(tmp.path / "million", std::ios::binary);
        std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) f << chunk;
    }
    REQUIRE(sha256_file_hex(tmp.path / "million") ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    REQUIRE(sha256_file_hex(tmp.path / "does-not-exist").empty());
}

TEST_CASE("semver3 parses and orders", "[cli][importer][semver][issue-19]") {
    Semver3 a, b;
    REQUIRE(parse_semver3("0.332.1", a));
    REQUIRE(a.major == 0);
    REQUIRE(a.minor == 332);
    REQUIRE(a.patch == 1);

    REQUIRE(parse_semver3("1.0.0-rc.2", b));  // pre-release suffix ignored
    REQUIRE(b.major == 1);
    REQUIRE(b.minor == 0);
    REQUIRE(b.patch == 0);
    REQUIRE(a < b);
    REQUIRE(a <= a);

    Semver3 junk;
    REQUIRE_FALSE(parse_semver3("not-a-version", junk));
}

TEST_CASE("version-window check rejects out-of-range sdk and non-overlapping spi",
          "[cli][importer][version-window][issue-19]") {
    auto tool = make_importer("00", "0.300.0", "0.400.0", 0, 1);

    // In-window.
    REQUIRE(check_importer_compat(tool, "0.350.0", 0, 0).ok);
    REQUIRE(check_importer_compat(tool, "0.300.0", 0, 0).ok);  // inclusive lo
    REQUIRE(check_importer_compat(tool, "0.400.0", 1, 1).ok);  // inclusive hi

    // SDK too old.
    auto r_old = check_importer_compat(tool, "0.299.9", 0, 0);
    REQUIRE_FALSE(r_old.ok);
    REQUIRE(r_old.error.find("requires Pulp SDK >=") != std::string::npos);
    REQUIRE(r_old.error.find("upgrade Pulp") != std::string::npos);

    // SDK too new.
    auto r_new = check_importer_compat(tool, "0.500.0", 0, 0);
    REQUIRE_FALSE(r_new.ok);
    REQUIRE(r_new.error.find("supports Pulp SDK <=") != std::string::npos);
    REQUIRE(r_new.error.find("upgrade the importer") != std::string::npos);

    // SPI window does not overlap: importer speaks [0,1], SDK speaks [5,5].
    auto r_spi = check_importer_compat(tool, "0.350.0", 5, 5);
    REQUIRE_FALSE(r_spi.ok);
    REQUIRE(r_spi.error.find("import-SPI") != std::string::npos);
    REQUIRE(r_spi.error.find("upgrade the importer") != std::string::npos);

    // SPI window: importer ahead of SDK -> "upgrade Pulp".
    auto tool2 = make_importer("00", "0.300.0", "0.400.0", 7, 9);
    auto r_spi2 = check_importer_compat(tool2, "0.350.0", 0, 0);
    REQUIRE_FALSE(r_spi2.ok);
    REQUIRE(r_spi2.error.find("upgrade Pulp") != std::string::npos);
}

TEST_CASE("install from a checksummed local package records and drops the skill",
          "[cli][importer][install][issue-19]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};

    auto pkg = build_mock_package(tmp.path / "staging", tmp.path / "framework-x.tar.gz",
                                  "# Framework X importer skill\nUse `pulp import`.\n");
    auto sha = sha256_file_hex(pkg);
    REQUIRE(sha.size() == 64);

    auto tool = make_importer(sha);
    auto res = install_importer(tool, "0.350.0", 0, 0, pkg.string(), /*force=*/false);
    INFO(res.error);
    REQUIRE(res.ok);
    REQUIRE(res.installed_version == "1.4.2");

    // Install tree unpacked.
    REQUIRE(fs::exists(res.install_dir / "bin" / "importer"));
    REQUIRE(fs::exists(res.install_dir / "libclang" / "VERSION"));

    // Skill dropped into ~/.agents/skills (honoring PULP_HOME).
    REQUIRE(fs::exists(res.skill_path));
    REQUIRE(res.skill_path == skills_dir() / "framework-x-importer" / "SKILL.md");
    std::ifstream sk(res.skill_path);
    std::string sk_body{std::istreambuf_iterator<char>(sk),
                        std::istreambuf_iterator<char>()};
    REQUIRE(sk_body.find("Framework X importer skill") != std::string::npos);

    // Record written under ~/.pulp/importers.
    REQUIRE(fs::exists(res.record_path));
    REQUIRE(res.record_path == importer_records_dir() / "framework-x-importer.json");
    std::ifstream rf(res.record_path);
    std::string rec{std::istreambuf_iterator<char>(rf), std::istreambuf_iterator<char>()};
    REQUIRE(rec.find("\"version\": \"1.4.2\"") != std::string::npos);
    REQUIRE(rec.find("\"sha256\": \"" + sha + "\"") != std::string::npos);
    REQUIRE(rec.find("\"terms_version\": \"2026-06-07\"") != std::string::npos);
    REQUIRE(lower(rec).find("\"sdk_version\": \"0.350.0\"") != std::string::npos);

    // Idempotent re-install (no force) is a no-op success.
    auto again = install_importer(tool, "0.350.0", 0, 0, pkg.string(), false);
    REQUIRE(again.ok);
    REQUIRE(again.skill_path == res.skill_path);
}

TEST_CASE("install refuses on checksum mismatch", "[cli][importer][checksum][issue-19]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};

    auto pkg = build_mock_package(tmp.path / "staging", tmp.path / "framework-x.tar.gz",
                                  "# skill\n");
    // Wrong sha — the all-zero digest never matches a real archive.
    auto tool = make_importer(std::string(64, '0'));

    ScopedOutput out;
    auto res = install_importer(tool, "0.350.0", 0, 0, pkg.string(), false);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("checksum mismatch") != std::string::npos);
    REQUIRE(res.error.find("refusing to install") != std::string::npos);

    // Nothing recorded, nothing installed, no skill.
    REQUIRE_FALSE(fs::exists(importer_records_dir() / "framework-x-importer.json"));
    REQUIRE_FALSE(fs::exists(skills_dir() / "framework-x-importer" / "SKILL.md"));
}

TEST_CASE("install refuses when sdk/spi out of window", "[cli][importer][version-window][issue-19]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};

    auto pkg = build_mock_package(tmp.path / "staging", tmp.path / "framework-x.tar.gz",
                                  "# skill\n");
    auto sha = sha256_file_hex(pkg);
    auto tool = make_importer(sha, "0.300.0", "0.400.0", 0, 0);

    // SDK too new — refuse before touching the package.
    auto res = install_importer(tool, "0.500.0", 0, 0, pkg.string(), false);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("supports Pulp SDK <=") != std::string::npos);
    REQUIRE_FALSE(fs::exists(importer_records_dir() / "framework-x-importer.json"));
}

TEST_CASE("uninstall removes the skill, record, and install tree",
          "[cli][importer][uninstall][issue-19]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};

    auto pkg = build_mock_package(tmp.path / "staging", tmp.path / "framework-x.tar.gz",
                                  "# skill\n");
    auto tool = make_importer(sha256_file_hex(pkg));
    auto res = install_importer(tool, "0.350.0", 0, 0, pkg.string(), false);
    REQUIRE(res.ok);

    REQUIRE(fs::exists(res.skill_path));
    REQUIRE(fs::exists(res.record_path));

    REQUIRE(uninstall_importer("framework-x-importer"));
    REQUIRE_FALSE(fs::exists(res.skill_path));
    REQUIRE_FALSE(fs::exists(skills_dir() / "framework-x-importer"));
    REQUIRE_FALSE(fs::exists(res.record_path));
    REQUIRE_FALSE(fs::exists(tools_dir() / "framework-x-importer"));

    // Second uninstall is a no-op (nothing to remove).
    REQUIRE_FALSE(uninstall_importer("framework-x-importer"));
}

// ── End-to-end: registry on disk + `pulp tool` / `pulp add` dispatch ──

namespace {

// Materialize a tool-registry.json under a repo dir whose importer artifact
// url_template points at the mock package via a local path, then chdir there
// so find_tool_registry_path() locates it.
fs::path write_repo_registry(const fs::path& repo, const std::string& sha,
                             const fs::path& pkg) {
    auto reg = repo / "tools" / "packages" / "tool-registry.json";
    std::string platform = current_platform_key();
    std::string body = std::string(R"({
  "schema_version": 4,
  "tools": {
    "framework-x-importer": {
      "display_name": "Framework X Importer",
      "category": "importer",
      "description": "Imports Framework X projects",
      "license": "MIT",
      "install_method": "importer_package",
      "pinned_version": "1.4.2",
      "frameworks": ["framework-x"],
      "spi_min": 0,
      "spi_max": 0,
      "sdk_min": "0.0.0",
      "sdk_max": "999.0.0",
      "capabilities": ["detect", "inspect", "emit"],
      "skill_source": "SKILL.md",
      "skill_name": "framework-x-importer",
      "terms_version": "2026-06-07",
      "vendor_id": "framework-x",
      "importer_artifacts": {
        ")") + platform + R"(": {
          "url_template": "file://)" + json_escape(pkg.string()) + R"(",
          "archive_format": "tar.gz",
          "sha256": ")" + sha + R"("
        }
      }
    }
  }
}
)";
    write_file(reg, body);
    return reg;
}

}  // namespace

TEST_CASE("pulp tool install/uninstall dispatch installs the importer end-to-end",
          "[cli][importer][command][issue-19]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    ScopedEnv sdk{"PULP_SDK_VERSION", "0.350.0"};  // drive the version-window check

    auto pkg = build_mock_package(tmp.path / "staging", tmp.path / "framework-x.tar.gz",
                                  "# skill\n");
    auto repo = tmp.path / "repo";
    write_repo_registry(repo, sha256_file_hex(pkg), pkg);
    ScopedCurrentPath cwd{repo};

    {
        ScopedOutput out;
        REQUIRE(cmd_tool({"install", "framework-x-importer"}) == 0);
        REQUIRE(out.out.str().find("Installed importer Framework X Importer 1.4.2") !=
                std::string::npos);
    }
    REQUIRE(fs::exists(skills_dir() / "framework-x-importer" / "SKILL.md"));
    REQUIRE(fs::exists(importer_records_dir() / "framework-x-importer.json"));

    {
        ScopedOutput out;
        REQUIRE(cmd_tool({"uninstall", "framework-x-importer"}) == 0);
        REQUIRE(out.out.str().find("Uninstalled importer framework-x-importer") !=
                std::string::npos);
    }
    REQUIRE_FALSE(fs::exists(skills_dir() / "framework-x-importer"));
}

TEST_CASE("pulp tool install --from drives the local package path",
          "[cli][importer][command][issue-19]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    ScopedEnv sdk{"PULP_SDK_VERSION", "0.350.0"};

    auto pkg = build_mock_package(tmp.path / "staging", tmp.path / "framework-x.tar.gz",
                                  "# skill\n");
    auto repo = tmp.path / "repo";
    // Registry artifact url is bogus; --from supplies the real package.
    write_repo_registry(repo, sha256_file_hex(pkg), fs::path("/nonexistent/pkg.tar.gz"));
    ScopedCurrentPath cwd{repo};

    ScopedOutput out;
    REQUIRE(cmd_tool({"install", "framework-x-importer", "--from", pkg.string()}) == 0);
    REQUIRE(fs::exists(importer_records_dir() / "framework-x-importer.json"));
}

TEST_CASE("pulp add <importer> alias routes through the importer install path",
          "[cli][importer][add-alias][issue-19]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    ScopedEnv sdk{"PULP_SDK_VERSION", "0.350.0"};

    auto pkg = build_mock_package(tmp.path / "staging", tmp.path / "framework-x.tar.gz",
                                  "# skill\n");
    auto repo = tmp.path / "repo";
    write_repo_registry(repo, sha256_file_hex(pkg), pkg);
    ScopedCurrentPath cwd{repo};

    // `pulp add framework-x-importer` calls try_add_importer_alias first (see
    // package_commands_add.cpp); a match installs and short-circuits the normal
    // audio-package resolution. Exercise that exact resolver here.
    ScopedOutput out;
    auto rc = try_add_importer_alias("framework-x-importer", "", false);
    REQUIRE(rc.has_value());
    REQUIRE(*rc == 0);
    REQUIRE(out.out.str().find("Installed importer Framework X Importer") !=
            std::string::npos);
    REQUIRE(fs::exists(skills_dir() / "framework-x-importer" / "SKILL.md"));

    // The --from override is honored through the alias too.
    auto alt = build_mock_package(tmp.path / "staging2",
                                  tmp.path / "framework-x-alt.tar.gz", "# skill-alt\n");
    write_repo_registry(repo, sha256_file_hex(alt), fs::path("/nonexistent/pkg.tar.gz"));
    auto rc2 = try_add_importer_alias("framework-x-importer", alt.string(), true);
    REQUIRE(rc2.has_value());
    REQUIRE(*rc2 == 0);
}

TEST_CASE("non-importer ids do not route through the importer path",
          "[cli][importer][add-alias][issue-19]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", (tmp.path / "home").string()};
    auto repo = tmp.path / "repo";
    auto pkg = tmp.path / "pkg.tar.gz";
    write_repo_registry(repo, std::string(64, '0'), pkg);
    ScopedCurrentPath cwd{repo};

    // A name that isn't an importer returns nullopt from the alias resolver.
    auto rc = try_add_importer_alias("not-an-importer", "", false);
    REQUIRE_FALSE(rc.has_value());
}
