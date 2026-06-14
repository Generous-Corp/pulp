#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/au_info_plist.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace au = pulp::cli::au_info_plist;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        static std::atomic<int> seq{0};
        path = fs::temp_directory_path() /
               ("pulp-au-info-plist-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                std::to_string(seq.fetch_add(1)));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << body;
}

const char* valid_plist() {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
  <key>AudioComponents</key>
  <array>
    <dict>
      <key>type</key>
      <string>aumf</string>
      <key>subtype</key>
      <string>PHBn</string>
      <key>manufacturer</key>
      <string>Pulp</string>
    </dict>
  </array>
</dict>
</plist>
)";
}

}  // namespace

TEST_CASE("AU Info.plist parser derives host unique id",
          "[cli][au][info-plist]") {
    REQUIRE(au::parse_unique_id_from_text(valid_plist()) == "aumf:PHBn:Pulp");
}

TEST_CASE("AU Info.plist parser rejects incomplete metadata",
          "[cli][au][info-plist]") {
    REQUIRE(au::parse_unique_id_from_text("").empty());
    REQUIRE(au::parse_unique_id_from_text(
                "<plist><dict><key>CFBundleName</key><string>X</string></dict></plist>")
                .empty());
    REQUIRE(au::parse_unique_id_from_text(
                "<plist><dict><key>AudioComponents</key><array/></dict></plist>")
                .empty());
    REQUIRE(au::parse_unique_id_from_text(
                "<plist><dict><key>AudioComponents</key><array><dict>"
                "<key>type</key><string>aumf</string></array></dict></plist>")
                .empty());
    REQUIRE(au::parse_unique_id_from_text(
                "<plist><dict><key>AudioComponents</key><array><dict>"
                "<key>type</key><string>aumf</string>"
                "<key>subtype</key><string>PHBn</string>"
                "</dict></array></dict></plist>")
                .empty());
    REQUIRE(au::parse_unique_id_from_text(
                "<plist><dict><key>AudioComponents</key><array><dict>"
                "<key>subtype</key><string>PHBn</string>"
                "<key>manufacturer</key><string>Pulp</string>"
                "</dict></array></dict></plist>")
                .empty());
    REQUIRE(au::parse_unique_id_from_text(
                "<plist><dict><key>AudioComponents</key><array><dict>"
                "<key>type</key><string>aumf</string>"
                "<key>manufacturer</key><string>Pulp</string>"
                "</dict></array></dict></plist>")
                .empty());
}

TEST_CASE("AU Info.plist parser reads bundle Info.plist",
          "[cli][au][info-plist]") {
    TempDir tmp;
    const auto bundle = tmp.path / "HostBench.component";
    write_text(bundle / "Contents" / "Info.plist", valid_plist());
    REQUIRE(au::unique_id_from_bundle(bundle) == "aumf:PHBn:Pulp");
    REQUIRE(au::unique_id_from_bundle(tmp.path / "Missing.component").empty());
}
