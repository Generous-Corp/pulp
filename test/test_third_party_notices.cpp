#include <catch2/catch_test_macros.hpp>
#include <pulp/format/third_party_notices.hpp>

#include "support/unique_temp_dir.hpp"

#include <filesystem>
#include <fstream>

TEST_CASE("third-party notices parse and load from a bundle", "[format][notices]") {
    const pulp::test::ScopedTempDir root_dir("pulp-third-party-notices");
    const auto root = root_dir.path;
    const auto resources = root / "Demo.vst3" / "Contents" / "Resources";
    std::filesystem::create_directories(resources);
    const std::string document = R"JSON({"schema":"pulp.third-party-notices.v1","product":"Demo","target":"Demo_VST3","format":"VST3","notices":[{"name":"Skia","version":"m158","license":"BSD-3-Clause","copyright":"Google","source":"https://skia.org","license_text":"licenses/skia.txt","sha256":"abc","required_by":["pulp-format-core"],"feature":""}],"sdks_without_manifest":[],"unattributed_targets":[]})JSON";
    std::ofstream(resources / "ThirdPartyNotices.json") << document;

    const auto parsed = pulp::format::parse_third_party_notices(document);
    REQUIRE(parsed.available());
    REQUIRE(parsed.notices.size() == 1);
    REQUIRE(parsed.notices.front().name == "Skia");
    REQUIRE(pulp::format::load_third_party_notices(root / "Demo.vst3").notices.size() == 1);

    std::filesystem::remove(resources / "ThirdPartyNotices.json");
    const auto missing = pulp::format::load_third_party_notices(root / "Demo.vst3");
    REQUIRE_FALSE(missing.available());
    REQUIRE(missing.reason.find("missing") != std::string::npos);
}
