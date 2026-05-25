#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef PULP_SCAN_WORKER_PATH
#error "PULP_SCAN_WORKER_PATH must point at the built pulp-scan-worker binary"
#endif

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

struct ScratchDir {
    fs::path path;

    explicit ScratchDir(const char* stem) {
        const auto counter =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path()
             / (std::string("pulp-scan-worker-test-") + stem + "-"
                + std::to_string(counter));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << body;
}

pulp::platform::ProcessResult run_worker(const std::vector<std::string>& args) {
    return pulp::platform::exec(PULP_SCAN_WORKER_PATH, args, 5000);
}

} // namespace

TEST_CASE("pulp-scan-worker reports usage when bundle path is missing",
          "[host][scan-worker][issue-493]") {
    auto result = run_worker({});
    REQUIRE(result.exit_code == 2);
    REQUIRE_THAT(result.stderr_output, ContainsSubstring("usage: pulp-scan-worker"));
}

TEST_CASE("pulp-scan-worker rejects unsupported bundle extensions",
          "[host][scan-worker][issue-493]") {
    ScratchDir scratch("unsupported");
    auto path = scratch.path / "not-a-plugin.txt";
    write_file(path, "not a plugin");

    auto result = run_worker({path.string()});
    REQUIRE(result.exit_code == 3);
    REQUIRE_THAT(result.stderr_output, ContainsSubstring("unsupported bundle extension"));
}

TEST_CASE("pulp-scan-worker emits JSON for a manifest-free VST3 bundle",
          "[host][scan-worker][issue-493]") {
    ScratchDir scratch("vst3");
    auto bundle = scratch.path / "Probe.vst3";
    fs::create_directories(bundle / "Contents" / "Resources");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Probe\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Probe\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"vst3\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(bundle.string()));
}

TEST_CASE("pulp-scan-worker reports VST3 moduleinfo identity and booleans",
          "[host][scan-worker][coverage][phase3]") {
    ScratchDir scratch("vst3-moduleinfo");
    auto bundle = scratch.path / "ModuleInfoProbe.vst3";
    fs::create_directories(bundle / "Contents" / "Resources");
    write_file(bundle / "Contents" / "Resources" / "moduleinfo.json", R"({
        "Classes": [
            {
                "Category": "Audio Module Class",
                "CID": "ABCDEF0123456789ABCDEF0123456789"
            }
        ]
    })");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE(result.stdout_output.find('\n') == result.stdout_output.size() - 1);
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"ModuleInfoProbe\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"manufacturer\":\"\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"version\":\"\""));
    REQUIRE_THAT(result.stdout_output,
                 ContainsSubstring("\"unique_id\":\"abcdef0123456789abcdef0123456789\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"is_instrument\":false"));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"is_effect\":true"));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"vst3\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(bundle.string()));
}

TEST_CASE("pulp-scan-worker filters sibling bundles from the same directory",
          "[host][scan-worker][coverage][phase3]") {
    ScratchDir scratch("vst3-siblings");
    auto target = scratch.path / "Target.vst3";
    auto sibling = scratch.path / "Sibling.vst3";
    fs::create_directories(target / "Contents" / "Resources");
    fs::create_directories(sibling / "Contents" / "Resources");

    auto result = run_worker({target.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE(result.stdout_output.find('\n') == result.stdout_output.size() - 1);
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Target\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Target\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(target.string()));
    REQUIRE(result.stdout_output.find("Sibling") == std::string::npos);
    REQUIRE(result.stdout_output.find(sibling.string()) == std::string::npos);
}

TEST_CASE("pulp-scan-worker emits fallback JSON for unreadable CLAP bundles",
          "[host][scan-worker][coverage][phase3]") {
    ScratchDir scratch("clap-fallback");
    auto bundle = scratch.path / "Fallback.clap";
    write_file(bundle, "not a dynamic library");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    REQUIRE(result.stdout_output.find("unsupported bundle extension") == std::string::npos);
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Fallback\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Fallback\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"is_instrument\":false"));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"is_effect\":true"));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring(bundle.string()));
}

TEST_CASE("pulp-scan-worker preserves fallback JSON fields for spaced CLAP paths",
          "[host][scan-worker][coverage][phase3]") {
#ifdef _WIN32
    SUCCEED("Windows path normalization is covered by the generic CLAP fallback case");
#else
    ScratchDir scratch("spaced-clap");
    auto bundle = scratch.path / "Space Name.clap";
    write_file(bundle, "not a dynamic library");

    auto result = run_worker({bundle.string()});
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"name\":\"Space Name\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"unique_id\":\"Space Name\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"format\":\"clap\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("\"path\":\""));
    REQUIRE_THAT(result.stdout_output, ContainsSubstring("Space Name.clap"));
    REQUIRE(result.stdout_output.find("Space%20Name") == std::string::npos);
    REQUIRE(result.stdout_output.back() == '\n');
#endif
}
