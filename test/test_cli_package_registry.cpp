// test_cli_package_registry.cpp - deterministic tests for
// `tools/cli/package_registry.cpp`.
//
// Coverage tranche for issue #643. These tests stay on the pure local
// registry/lock/target/semver/query surface and intentionally avoid remote
// registry refresh or archive/install behavior.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/package_registry.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::cli::pkg;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        const int n = seq.fetch_add(1);
        path = base / ("pulp-cli-package-registry-test-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                       "-" + std::to_string(n));
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

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << body;
}

struct EnvVarGuard {
    std::string name;
    std::optional<std::string> previous;

    EnvVarGuard(std::string key, std::optional<std::string> value)
        : name(std::move(key)) {
        if (auto* old = std::getenv(name.c_str())) previous = old;
        set(value);
    }

    ~EnvVarGuard() { set(previous); }

    void set(const std::optional<std::string>& value) const {
#ifdef _WIN32
        _putenv_s(name.c_str(), value ? value->c_str() : "");
#else
        if (value) {
            setenv(name.c_str(), value->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
#endif
    }
};

std::vector<std::string> target_strings(const std::vector<PlatformTarget>& targets) {
    std::vector<std::string> out;
    for (const auto& t : targets) out.push_back(t.to_string());
    return out;
}

fs::path write_registry_fixture(const fs::path& root,
                                const std::string& filename = "registry.json") {
    auto path = root / filename;
    write_file(path, R"({
  "registry_version": 2,
  "packages": {
    "synth-core": {
      "name": "Core Synth",
      "version": "1.4.2",
      "description": "Polyphonic synth toolkit",
      "license": "MIT",
      "category": "DSP",
      "url": "https://example.com/synth-core",
      "fetch": {
        "method": "FetchContent",
        "git_repository": "https://example.com/synth-core.git",
        "git_tag": "v1.4.2"
      },
      "cmake": {
        "targets": ["synth::core"],
        "header_only": false,
        "include_dir": "include"
      },
      "platforms": {
        "macOS": {"architectures": ["arm64", "x64"], "notes": "native"},
        "Windows": {"architectures": ["x64"]},
        "Linux": {"architectures": ["x64"]}
      },
      "rt_safe": true,
      "tags": ["synth", "oscillator"],
      "provides": ["oscillator", "voice"],
      "overlaps_with_builtin": {
        "pulp/signal/oscillator.hpp": "basic oscillator"
      },
      "unique_value": "voice allocation helpers",
      "alternatives": ["filter-kit"],
      "verification": {
        "last_verified": "2026-04-22",
        "verified_version": "1.4.2",
        "build_status": {
          "macOS": "pass",
          "Windows": "pass",
          "Linux": "fail"
        }
      }
    },
    "filter-kit": {
      "name": "Filter Kit",
      "version": "0.9.0",
      "description": "Filter design helpers",
      "license": "MPL-2.0",
      "category": "Analysis",
      "url": "https://example.com/filter-kit",
      "fetch": {
        "method": "header-only",
        "git_repository": "https://example.com/filter-kit.git",
        "git_tag": "v0.9.0"
      },
      "cmake": {
        "targets": ["filter::kit"],
        "header_only": true,
        "include_dir": "include/filter"
      },
      "platforms": {
        "macOS": {"architectures": ["arm64"]}
      },
      "tags": ["filter", "analysis"],
      "provides": ["filter"],
      "verification": {
        "last_verified": "2026-04-20",
        "verified_version": "0.9.0",
        "build_status": {
          "macOS": "pass"
        }
      }
    }
  }
}
)");
    return path;
}

PackageDescriptor quality_fixture() {
    PackageDescriptor pkg;
    pkg.license = "MIT";
    pkg.platforms["macOS"].architectures = {"arm64", "x64"};
    pkg.platforms["Windows"].architectures = {"x64"};
    pkg.platforms["Linux"].architectures = {"x64"};
    pkg.verification.last_verified = "2026-04-22";
    pkg.verification.build_status = {
        {"macOS", "pass"},
        {"Windows", "pass"},
        {"Linux", "fail"},
    };
    return pkg;
}

}  // namespace

TEST_CASE("package registry parses descriptors and reports malformed roots",
          "[cli][package-registry][issue-643]") {
    TempDir tmp;
    auto registry_path = write_registry_fixture(tmp.path);

    auto loaded = load_registry(registry_path);
    REQUIRE(loaded.error.empty());
    REQUIRE(loaded.registry.version == 2);
    REQUIRE(loaded.registry.packages.size() == 2);

    const auto& synth = loaded.registry.packages.at("synth-core");
    REQUIRE(synth.id == "synth-core");
    REQUIRE(synth.name == "Core Synth");
    REQUIRE(synth.version == "1.4.2");
    REQUIRE(synth.fetch.method == "FetchContent");
    REQUIRE(synth.fetch.git_repository == "https://example.com/synth-core.git");
    REQUIRE(synth.cmake.targets == std::vector<std::string>{"synth::core"});
    REQUIRE_FALSE(synth.cmake.header_only);
    REQUIRE(synth.platforms.at("macOS").architectures == std::vector<std::string>{"arm64", "x64"});
    REQUIRE(synth.platforms.at("macOS").notes == "native");
    REQUIRE(synth.rt_safe);
    REQUIRE(synth.tags == std::vector<std::string>{"synth", "oscillator"});
    REQUIRE(synth.provides == std::vector<std::string>{"oscillator", "voice"});
    REQUIRE(synth.overlaps_with_builtin.at("pulp/signal/oscillator.hpp") == "basic oscillator");
    REQUIRE(synth.unique_value == "voice allocation helpers");
    REQUIRE(synth.alternatives == std::vector<std::string>{"filter-kit"});
    REQUIRE(synth.verification.verified_version == "1.4.2");
    REQUIRE(synth.verification.build_status.at("Linux") == "fail");

    auto missing = load_registry(tmp.path / "missing.json");
    REQUIRE(missing.registry.packages.empty());
    REQUIRE(missing.error.find("Cannot read registry file") != std::string::npos);

    write_file(tmp.path / "array.json", "[]");
    auto malformed = load_registry(tmp.path / "array.json");
    REQUIRE(malformed.registry.packages.empty());
    REQUIRE(malformed.error == "Registry file is not a valid JSON object");
}

TEST_CASE("package lock files round-trip escaped local package metadata",
          "[cli][package-registry][lock][issue-643]") {
    TempDir tmp;
    auto lock_path = tmp.path / "packages.lock.json";

    LockFile lock;
    lock.version = 3;
    lock.packages["filter-kit"] = {
        "0.9.0",
        "https://example.com/filter-kit.git",
        "sha256:abc\\def",
        "v0.9.0",
    };
    lock.packages["quoted"] = {
        "1.0.0",
        "https://example.com/quote.git",
        "quote\"and\nnewline",
        "abc123",
    };

    REQUIRE(save_lock_file(lock_path, lock));
    auto loaded = load_lock_file(lock_path);
    REQUIRE(loaded.version == 3);
    REQUIRE(loaded.packages.size() == 2);
    REQUIRE(loaded.packages.at("filter-kit").integrity == "sha256:abc\\def");
    REQUIRE(loaded.packages.at("quoted").integrity == "quote\"and\nnewline");
    REQUIRE(loaded.packages.at("quoted").commit == "abc123");

    auto missing = load_lock_file(tmp.path / "missing-lock.json");
    REQUIRE(missing.version == 1);
    REQUIRE(missing.packages.empty());

    write_file(tmp.path / "bad-lock.json", "[]");
    auto malformed = load_lock_file(tmp.path / "bad-lock.json");
    REQUIRE(malformed.version == 1);
    REQUIRE(malformed.packages.empty());
}

TEST_CASE("remote registry helpers use environment cache paths and fresh cache hits",
          "[cli][package-registry][remote][cache][issue-643]") {
    TempDir tmp;

#ifdef _WIN32
    {
        EnvVarGuard appdata("LOCALAPPDATA", tmp.path.string());
        REQUIRE(default_cache_dir() == tmp.path / "Pulp");
    }
    {
        EnvVarGuard no_appdata("LOCALAPPDATA", std::nullopt);
        REQUIRE(default_cache_dir().filename() == "pulp-cache");
    }
#else
    {
        EnvVarGuard home("HOME", tmp.path.string());
        REQUIRE(default_cache_dir() == tmp.path / ".pulp");
    }
    {
        EnvVarGuard no_home("HOME", std::nullopt);
        REQUIRE(default_cache_dir().filename() == "pulp-cache");
    }
#endif

    auto cache_file = write_registry_fixture(tmp.path, "registry-cache.json");
    REQUIRE(fs::exists(cache_file));

    auto loaded = load_remote_registry(
        "https://127.0.0.1:9/pulp-registry-should-not-be-fetched.json",
        tmp.path,
        24);
    REQUIRE(loaded.error.empty());
    REQUIRE(loaded.registry.packages.size() == 2);
    REQUIRE(loaded.registry.packages.at("synth-core").name == "Core Synth");

    auto stale = load_remote_registry(
        "https://127.0.0.1:9/pulp-registry-stale-cache-fallback.json",
        tmp.path,
        0);
    REQUIRE(stale.error.empty());
    REQUIRE(stale.registry.packages.size() == 2);

    TempDir missing_cache;
    auto refresh = refresh_remote_registry(
        "https://127.0.0.1:9/pulp-registry-refresh-failure.json",
        missing_cache.path);
    REQUIRE(refresh.registry.packages.empty());
    REQUIRE(refresh.error.find("Failed to fetch remote registry") != std::string::npos);
}

TEST_CASE("project targets parse, default, expand platforms, and rewrite TOML",
          "[cli][package-registry][targets][issue-643]") {
    TempDir tmp;

    REQUIRE(target_strings(default_targets()) ==
            std::vector<std::string>{"macOS-arm64", "Windows-x64", "Linux-x64"});
    REQUIRE(PlatformTarget::parse("macOS-arm64")->to_string() == "macOS-arm64");
    REQUIRE(PlatformTarget::parse("Android-arm64-v8a")->to_string() == "Android-arm64-v8a");
    REQUIRE_FALSE(PlatformTarget::parse("macOS"));
    REQUIRE_FALSE(PlatformTarget::parse("macOS-mips"));
    REQUIRE_FALSE(PlatformTarget::parse("-x64"));
    REQUIRE(is_valid_target({"WASM", "wasm32"}));
    REQUIRE_FALSE(is_valid_target({"BeOS", "x64"}));

    REQUIRE(target_strings(read_project_targets(tmp.path)) ==
            std::vector<std::string>{"macOS-arm64", "Windows-x64", "Linux-x64"});

    write_file(tmp.path / "pulp.toml", R"([project]
platforms = [
  "macOS",
  "Android",
  "WASM"
]

[plugin]
name = "Demo"
)");
    REQUIRE(target_strings(read_project_targets(tmp.path)) ==
            std::vector<std::string>{"macOS-arm64", "Android-arm64-v8a", "WASM-wasm32"});

    REQUIRE(write_project_targets(
        tmp.path,
        {PlatformTarget{"Linux", "x64"}, PlatformTarget{"iOS", "arm64"}}));
    REQUIRE(target_strings(read_project_targets(tmp.path)) ==
            std::vector<std::string>{"Linux-x64", "iOS-arm64"});

    auto rewritten = std::ifstream(tmp.path / "pulp.toml");
    std::string body{std::istreambuf_iterator<char>(rewritten), std::istreambuf_iterator<char>()};
    REQUIRE(body.find("targets = [") != std::string::npos);
    REQUIRE(body.find("\"Linux-x64\"") != std::string::npos);
    REQUIRE(body.find("[plugin]") != std::string::npos);

    TempDir no_project;
    write_file(no_project.path / "pulp.toml", "[plugin]\nname = \"Demo\"\n");
    REQUIRE(write_project_targets(no_project.path, {PlatformTarget{"Windows", "x64"}}));
    REQUIRE(target_strings(read_project_targets(no_project.path)) ==
            std::vector<std::string>{"Windows-x64"});

    TempDir empty_project;
    REQUIRE(write_project_targets(empty_project.path, {}));
    REQUIRE(target_strings(read_project_targets(empty_project.path)) ==
            std::vector<std::string>{"macOS-arm64", "Windows-x64", "Linux-x64"});
}

TEST_CASE("project target parsing falls back for malformed target TOML",
          "[cli][package-registry][targets][issue-643]") {
    TempDir invalid_values;
    write_file(invalid_values.path / "pulp.toml", R"([project]
targets = [
  "BeOS-x64",
  "macOS-mips"
]
)");
    REQUIRE(target_strings(read_project_targets(invalid_values.path)) ==
            std::vector<std::string>{"macOS-arm64", "Windows-x64", "Linux-x64"});

    TempDir unterminated;
    write_file(unterminated.path / "pulp.toml", R"([project]
targets = [
  "Linux-x64"
)");
    REQUIRE(target_strings(read_project_targets(unterminated.path)) ==
            std::vector<std::string>{"macOS-arm64", "Windows-x64", "Linux-x64"});

    TempDir wrong_section;
    write_file(wrong_section.path / "pulp.toml", R"([plugin]
targets = ["Linux-x64"]
)");
    REQUIRE(target_strings(read_project_targets(wrong_section.path)) ==
            std::vector<std::string>{"macOS-arm64", "Windows-x64", "Linux-x64"});
}

TEST_CASE("project targets cover alternate valid architectures and platform rewrite",
          "[cli][package-registry][targets][coverage][phase3-batch757]") {
    auto linux_x86_64 = PlatformTarget::parse("Linux-x86_64");
    REQUIRE(linux_x86_64);
    REQUIRE(linux_x86_64->to_string() == "Linux-x86_64");

    auto windows_x86 = PlatformTarget::parse("Windows-x86");
    REQUIRE(windows_x86);
    REQUIRE(windows_x86->to_string() == "Windows-x86");

    auto ios_arm64 = PlatformTarget::parse("iOS-arm64");
    REQUIRE(ios_arm64);
    REQUIRE(ios_arm64->to_string() == "iOS-arm64");

    REQUIRE(is_valid_target({"macOS", "x86_64"}));
    REQUIRE(is_valid_target({"WASM", "x64"}));
    REQUIRE(is_valid_target({"Android", "arm64-v8a"}));
    REQUIRE_FALSE(is_valid_target({"Linux", "armv7"}));
    REQUIRE_FALSE(PlatformTarget::parse("macOS-arm64-extra"));
    REQUIRE_FALSE(PlatformTarget::parse("macOS-"));
    REQUIRE_FALSE(PlatformTarget::parse(""));

    TempDir tmp;
    write_file(tmp.path / "pulp.toml", R"([project]
name = "Demo"
platforms = [
  "macOS",
  "Linux"
]

[plugin]
name = "DemoPlugin"
)");

    REQUIRE(write_project_targets(
        tmp.path,
        {PlatformTarget{"Android", "arm64-v8a"}, PlatformTarget{"WASM", "wasm32"}}));
    REQUIRE(target_strings(read_project_targets(tmp.path)) ==
            std::vector<std::string>{"Android-arm64-v8a", "WASM-wasm32"});

    auto rewritten = std::ifstream(tmp.path / "pulp.toml");
    std::string body{std::istreambuf_iterator<char>(rewritten), std::istreambuf_iterator<char>()};
    auto project_pos = body.find("[project]");
    auto targets_pos = body.find("targets = [");
    auto plugin_pos = body.find("[plugin]");
    REQUIRE(project_pos != std::string::npos);
    REQUIRE(targets_pos != std::string::npos);
    REQUIRE(plugin_pos != std::string::npos);
    REQUIRE(project_pos < targets_pos);
    REQUIRE(targets_pos < plugin_pos);
    REQUIRE(body.find("platforms = [") == std::string::npos);
    REQUIRE(body.find("\"Android-arm64-v8a\"") != std::string::npos);
    REQUIRE(body.find("\"WASM-wasm32\"") != std::string::npos);
    REQUIRE(body.find("[plugin]") != std::string::npos);
}

TEST_CASE("project target parsing prefers targets over later platforms key",
          "[cli][package-registry][targets][coverage]") {
    TempDir tmp;
    write_file(tmp.path / "pulp.toml", R"([project]
targets = ["Linux-x64"]
platforms = ["macOS"]
)");

    REQUIRE(target_strings(read_project_targets(tmp.path)) ==
            std::vector<std::string>{"Linux-x64"});
}

TEST_CASE("lock files tolerate sparse package entries and escaped package ids",
          "[cli][package-registry][lock][coverage][phase3-batch757]") {
    TempDir tmp;
    write_file(tmp.path / "sparse-lock.json", R"({
  "lockfile_version": 4,
  "packages": {
    "bare": {},
    "scope/pkg": {
      "version": "2.3.4"
    }
  }
}
)");

    auto sparse = load_lock_file(tmp.path / "sparse-lock.json");
    REQUIRE(sparse.version == 4);
    REQUIRE(sparse.packages.size() == 2);
    REQUIRE(sparse.packages.at("bare").version.empty());
    REQUIRE(sparse.packages.at("scope/pkg").version == "2.3.4");
    REQUIRE(sparse.packages.at("scope/pkg").resolved.empty());

    LockFile lock;
    lock.packages["quote\"id"] = {"1.0.0", "https://example.com/pkg.git", "sha", "commit"};
    REQUIRE(save_lock_file(tmp.path / "escaped-lock.json", lock));
    auto escaped = load_lock_file(tmp.path / "escaped-lock.json");
    REQUIRE(escaped.packages.at("quote\"id").resolved == "https://example.com/pkg.git");
}

TEST_CASE("licenses, semver, and quality scoring classify local registry metadata",
          "[cli][package-registry][quality][issue-643]") {
    REQUIRE(check_license("MIT") == LicenseVerdict::allowed);
    REQUIRE(check_license("mit") == LicenseVerdict::allowed);
    REQUIRE(check_license("MPL-2.0") == LicenseVerdict::review_required);
    REQUIRE(check_license("GPL-3.0") == LicenseVerdict::rejected);
    REQUIRE(check_license("Proprietary") == LicenseVerdict::rejected);
    REQUIRE(std::string(license_verdict_label(LicenseVerdict::allowed)) == "allowed");
    REQUIRE(std::string(license_verdict_label(LicenseVerdict::review_required)) ==
            "review required");
    REQUIRE(std::string(license_verdict_label(LicenseVerdict::rejected)) == "rejected");
    REQUIRE(std::string(license_verdict_label(static_cast<LicenseVerdict>(99))) == "unknown");
    REQUIRE(license_tier("Apache-2.0") == "allowed");
    REQUIRE(license_tier("Custom-1.0") == "review");
    REQUIRE(license_tier("GPL-3.0") == "restricted");
    REQUIRE(license_tier("SSPL-1.0") == "rejected");
    REQUIRE(license_explanation("AGPL-3.0").find("network service") != std::string::npos);
    REQUIRE(license_explanation("LGPL-2.1").find("dynamic linking") != std::string::npos);
    REQUIRE(license_explanation("MPL-2.0").find("file-level copyleft") != std::string::npos);
    REQUIRE(license_explanation("Custom-1.0").find("compatibility") != std::string::npos);

    auto parsed = SemVer::parse("v1.2.3-beta");
    REQUIRE(parsed);
    REQUIRE(parsed->major == 1);
    REQUIRE(parsed->minor == 2);
    REQUIRE(parsed->patch == 3);
    REQUIRE(parsed->pre == "beta");
    REQUIRE(SemVer::parse("1.2"));
    auto major_only = SemVer::parse("V2");
    REQUIRE(major_only);
    REQUIRE(major_only->major == 2);
    REQUIRE(major_only->minor == 0);
    REQUIRE(major_only->patch == 0);
    REQUIRE(*major_only == *SemVer::parse("2.0.0"));
    REQUIRE_FALSE(SemVer::parse(""));
    REQUIRE_FALSE(SemVer::parse("1.two.3"));
    REQUIRE_FALSE(SemVer::parse("1x.2.3"));
    REQUIRE_FALSE(SemVer::parse("1.2.3.4"));
    REQUIRE_FALSE(SemVer::parse("1..3"));

    auto release = *SemVer::parse("1.2.3");
    auto prerelease = *SemVer::parse("1.2.3-rc1");
    auto newer_patch = *SemVer::parse("1.2.4");
    REQUIRE(prerelease < release);
    REQUIRE(release < newer_patch);
    REQUIRE((*SemVer::parse("1.2.3-alpha")) < (*SemVer::parse("1.2.3-beta")));
    REQUIRE(newer_patch.compatible_with(*SemVer::parse("1.2.3")));
    REQUIRE_FALSE((*SemVer::parse("2.0.0")).compatible_with(*SemVer::parse("1.2.3")));
    REQUIRE_FALSE((*SemVer::parse("1.1.9")).compatible_with(*SemVer::parse("1.2.0")));

    auto q = compute_quality(quality_fixture());
    REQUIRE(q.license == 25);
    REQUIRE(q.platforms == 24);
    REQUIRE(q.verification == 16);
    REQUIRE(q.maintenance == 15);
    REQUIRE(q.total == 80);
    REQUIRE(q.tier == "official");

    PackageDescriptor experimental;
    experimental.license = "GPL-3.0";
    experimental.platforms["Android"].architectures = {"arm64-v8a"};
    auto low = compute_quality(experimental);
    REQUIRE(low.total == 0);
    REQUIRE(low.tier == "experimental");

    PackageDescriptor community = quality_fixture();
    community.license = "MPL-2.0";
    community.verification.build_status = {{"macOS", "pass"}, {"Linux", "fail"}};
    auto mid = compute_quality(community);
    REQUIRE(mid.license == 10);
    REQUIRE(mid.verification == 12);
    REQUIRE(mid.tier == "community");
}

TEST_CASE("semver and quality cover boundary tiers",
          "[cli][package-registry][quality][coverage][phase3-batch757]") {
    auto rc1 = *SemVer::parse("1.2.3-rc1");
    auto rc2 = *SemVer::parse("1.2.3-rc2");
    auto next_minor = *SemVer::parse("1.3.0");
    REQUIRE(rc1 < rc2);
    REQUIRE(rc2 < next_minor);
    REQUIRE(next_minor.compatible_with(*SemVer::parse("1.2.9")));
    REQUIRE_FALSE((*SemVer::parse("1.2.8")).compatible_with(*SemVer::parse("1.2.9")));
    auto dotted_prerelease = SemVer::parse("1.2.-3");
    REQUIRE(dotted_prerelease);
    REQUIRE(dotted_prerelease->major == 1);
    REQUIRE(dotted_prerelease->minor == 2);
    REQUIRE(dotted_prerelease->patch == 0);
    REQUIRE(dotted_prerelease->pre == "3");
    REQUIRE(SemVer::parse("1.2.3-"));
    REQUIRE_FALSE(SemVer::parse("v"));

    PackageDescriptor capped = quality_fixture();
    capped.platforms["iOS"].architectures = {"arm64"};
    capped.platforms["Android"].architectures = {"arm64-v8a"};
    capped.platforms["WASM"].architectures = {"wasm32"};
    auto capped_score = compute_quality(capped);
    REQUIRE(capped_score.platforms == 24);
    REQUIRE(capped_score.tier == "official");

    PackageDescriptor unverified;
    unverified.license = "MIT-0";
    unverified.platforms["macOS"].architectures = {"arm64"};
    auto unverified_score = compute_quality(unverified);
    REQUIRE(unverified_score.license == 25);
    REQUIRE(unverified_score.platforms == 8);
    REQUIRE(unverified_score.verification == 0);
    REQUIRE(unverified_score.maintenance == 0);
    REQUIRE(unverified_score.tier == "experimental");
}

TEST_CASE("package registry queries rank search hits and detect unsupported targets",
          "[cli][package-registry][search][issue-643]") {
    TempDir tmp;
    auto loaded = load_registry(write_registry_fixture(tmp.path));
    REQUIRE(loaded.error.empty());

    const auto& synth = loaded.registry.packages.at("synth-core");
    auto unsupported = unsupported_targets(
        synth,
        {PlatformTarget{"macOS", "arm64"},
         PlatformTarget{"macOS", "x86"},
         PlatformTarget{"Linux", "x64"},
         PlatformTarget{"Android", "arm64-v8a"}});
    REQUIRE(target_strings(unsupported) ==
            std::vector<std::string>{"macOS-x86", "Android-arm64-v8a"});

    auto exact = search(loaded.registry, "synth-core");
    REQUIRE(exact.size() == 1);
    REQUIRE(exact.front()->id == "synth-core");

    auto synth_hits = search(loaded.registry, "synth");
    REQUIRE_FALSE(synth_hits.empty());
    REQUIRE(synth_hits.front()->id == "synth-core");

    auto analysis_hits = search(loaded.registry, "analysis");
    REQUIRE_FALSE(analysis_hits.empty());
    REQUIRE(analysis_hits.front()->id == "filter-kit");

    auto voice_hits = search(loaded.registry, "voice");
    REQUIRE_FALSE(voice_hits.empty());
    REQUIRE(voice_hits.front()->id == "synth-core");

    auto missing = search(loaded.registry, "does-not-exist");
    REQUIRE(missing.empty());
}

TEST_CASE("package registry search covers category description and no-platform fallbacks",
          "[cli][package-registry][search][coverage][phase3-batch757]") {
    TempDir tmp;
    auto loaded = load_registry(write_registry_fixture(tmp.path));
    REQUIRE(loaded.error.empty());

    auto upper = search(loaded.registry, "SYNTH");
    REQUIRE_FALSE(upper.empty());
    REQUIRE(upper.front()->id == "synth-core");

    auto empty = search(loaded.registry, "");
    REQUIRE(empty.size() == loaded.registry.packages.size());
    REQUIRE(empty[0]->id == "filter-kit");
    REQUIRE(empty[1]->id == "synth-core");

    auto description_hits = search(loaded.registry, "polyphonic");
    REQUIRE_FALSE(description_hits.empty());
    REQUIRE(description_hits.front()->id == "synth-core");

    auto category_hits = search(loaded.registry, "DSP");
    REQUIRE_FALSE(category_hits.empty());
    REQUIRE(category_hits.front()->id == "synth-core");

    const auto& filter = loaded.registry.packages.at("filter-kit");
    auto unsupported = unsupported_targets(
        filter,
        {PlatformTarget{"macOS", "arm64"},
         PlatformTarget{"macOS", "x64"},
         PlatformTarget{"Windows", "x64"}});
    REQUIRE(target_strings(unsupported) ==
            std::vector<std::string>{"macOS-x64", "Windows-x64"});
}

TEST_CASE("remote registry retries corrupt fresh cache before stale fallback",
          "[cli][package-registry][remote][cache][coverage][phase3-batch757]") {
    TempDir tmp;
    write_file(tmp.path / "registry-cache.json", "[]");

    auto corrupt = load_remote_registry(
        "https://127.0.0.1:9/pulp-registry-corrupt-cache.json",
        tmp.path,
        24);
    REQUIRE(corrupt.registry.packages.empty());
    REQUIRE(corrupt.error == "Registry file is not a valid JSON object");

    write_registry_fixture(tmp.path, "registry-cache.json");
    auto stale = load_remote_registry(
        "https://127.0.0.1:9/pulp-registry-stale-after-failure.json",
        tmp.path,
        0);
    REQUIRE(stale.error.empty());
    REQUIRE(stale.registry.packages.size() == 2);
}
