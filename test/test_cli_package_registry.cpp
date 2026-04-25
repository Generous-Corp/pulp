// test_cli_package_registry.cpp - deterministic tests for
// `tools/cli/package_registry.cpp`.
//
// Coverage tranche for issue #643. These tests stay on the pure local
// registry/lock/target/semver/query surface and intentionally avoid remote
// registry refresh or archive/install behavior.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/package_registry.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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

std::vector<std::string> target_strings(const std::vector<PlatformTarget>& targets) {
    std::vector<std::string> out;
    for (const auto& t : targets) out.push_back(t.to_string());
    return out;
}

fs::path write_registry_fixture(const fs::path& root) {
    auto path = root / "registry.json";
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
}

TEST_CASE("licenses, semver, and quality scoring classify local registry metadata",
          "[cli][package-registry][quality][issue-643]") {
    REQUIRE(check_license("MIT") == LicenseVerdict::allowed);
    REQUIRE(check_license("mit") == LicenseVerdict::allowed);
    REQUIRE(check_license("MPL-2.0") == LicenseVerdict::review_required);
    REQUIRE(check_license("GPL-3.0") == LicenseVerdict::rejected);
    REQUIRE(check_license("Proprietary") == LicenseVerdict::rejected);
    REQUIRE(std::string(license_verdict_label(LicenseVerdict::allowed)) == "allowed");
    REQUIRE(license_tier("GPL-3.0") == "restricted");
    REQUIRE(license_tier("SSPL-1.0") == "rejected");
    REQUIRE(license_explanation("AGPL-3.0").find("network service") != std::string::npos);
    REQUIRE(license_explanation("LGPL-2.1").find("dynamic linking") != std::string::npos);
    REQUIRE(license_explanation("MPL-2.0").find("file-level copyleft") != std::string::npos);

    auto parsed = SemVer::parse("v1.2.3-beta");
    REQUIRE(parsed);
    REQUIRE(parsed->major == 1);
    REQUIRE(parsed->minor == 2);
    REQUIRE(parsed->patch == 3);
    REQUIRE(parsed->pre == "beta");
    REQUIRE(SemVer::parse("1.2"));
    REQUIRE_FALSE(SemVer::parse("1.two.3"));

    auto release = *SemVer::parse("1.2.3");
    auto prerelease = *SemVer::parse("1.2.3-rc1");
    auto newer_patch = *SemVer::parse("1.2.4");
    REQUIRE(prerelease < release);
    REQUIRE(release < newer_patch);
    REQUIRE(newer_patch.compatible_with(*SemVer::parse("1.2.3")));
    REQUIRE_FALSE((*SemVer::parse("2.0.0")).compatible_with(*SemVer::parse("1.2.3")));

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

    auto missing = search(loaded.registry, "does-not-exist");
    REQUIRE(missing.empty());
}
