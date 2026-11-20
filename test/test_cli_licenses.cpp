#include <catch2/catch_test_macros.hpp>

#include "support/unique_temp_dir.hpp"
#include "test_cli_shellout_helpers.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

TEST_CASE("licenses CLI prints JSON and checks manifest gaps", "[cli][licenses]") {
#if !defined(PULP_CLI_BINARY)
    SKIP("CLI shell-out binary is not configured");
#else
    const pulp::test::ScopedTempDir root_dir("pulp-cli-licenses");
    const auto root = root_dir.path;
    const auto resources = root / "Demo.vst3" / "Contents" / "Resources";
    std::filesystem::create_directories(resources);
    std::ofstream(resources / "ThirdPartyNotices.json")
        << R"JSON({"schema":"pulp.third-party-notices.v1","product":"Demo","format":"VST3","notices":[],"sdks_without_manifest":["Vellum"],"unattributed_targets":[]})JSON";
    const auto result = pulp_test_cli::run_pulp({"licenses", (root / "Demo.vst3").string(), "--json"});
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_output.find("pulp.third-party-notices.v1") != std::string::npos);
    const auto checked = pulp_test_cli::run_pulp({"licenses", (root / "Demo.vst3").string(), "--check"});
    REQUIRE(checked.exit_code != 0);
#endif
}
