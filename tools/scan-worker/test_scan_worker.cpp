#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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
