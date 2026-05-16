#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <pulp/ship/installer.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace pulp::ship;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

static InstallerConfig make_test_config() {
    InstallerConfig config;
    config.product_name = "TestPlugin";
    config.publisher = "TestCorp";
    config.version = "1.2.3";
    config.output_path = "TestPlugin-1.2.3-setup.exe";
    config.plugins = {
        {"/path/to/TestPlugin.vst3", "", "vst3"},
        {"/path/to/TestPlugin.clap", "", "clap"},
    };
    return config;
}

struct ScopedTempDir {
    fs::path path;

    ScopedTempDir() {
        path = fs::temp_directory_path() / ("pulp-nsis-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

TEST_CASE("NSIS script contains product metadata", "[ship][installer]") {
    auto config = make_test_config();
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("!define PRODUCT_NAME \"TestPlugin\""));
    REQUIRE_THAT(script, ContainsSubstring("!define PRODUCT_VERSION \"1.2.3\""));
    REQUIRE_THAT(script, ContainsSubstring("!define PRODUCT_PUBLISHER \"TestCorp\""));
}

TEST_CASE("NSIS script has VST3 and CLAP sections", "[ship][installer]") {
    auto config = make_test_config();
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("TestPlugin (VST3)"));
    REQUIRE_THAT(script, ContainsSubstring("TestPlugin (CLAP)"));
    REQUIRE_THAT(script, ContainsSubstring("VST3"));
    REQUIRE_THAT(script, ContainsSubstring("CLAP"));
}

TEST_CASE("NSIS script includes uninstaller", "[ship][installer]") {
    auto config = make_test_config();
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("Section \"Uninstall\""));
    REQUIRE_THAT(script, ContainsSubstring("WriteUninstaller"));
    REQUIRE_THAT(script, ContainsSubstring("UninstallString"));
}

TEST_CASE("NSIS script registers uninstall metadata", "[ship][installer]") {
    auto config = make_test_config();
    auto script = generate_nsis_script(config);

    const auto uninstall_key =
        "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TestPlugin";

    REQUIRE_THAT(script, ContainsSubstring(
        std::string("WriteRegStr HKLM \"") + uninstall_key + "\" \"DisplayName\" \"TestPlugin\""));
    REQUIRE_THAT(script, ContainsSubstring(
        std::string("WriteRegStr HKLM \"") + uninstall_key + "\" \"DisplayVersion\" \"1.2.3\""));
    REQUIRE_THAT(script, ContainsSubstring(
        std::string("WriteRegStr HKLM \"") + uninstall_key + "\" \"Publisher\" \"TestCorp\""));
}

TEST_CASE("NSIS script per-user install uses LOCALAPPDATA", "[ship][installer]") {
    auto config = make_test_config();
    config.per_user_install = true;
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("$LOCALAPPDATA"));
    REQUIRE_THAT(script, ContainsSubstring("RequestExecutionLevel user"));
}

TEST_CASE("NSIS script admin install uses PROGRAMFILES and HKLM", "[ship][installer]") {
    auto config = make_test_config();
    config.per_user_install = false;
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("$PROGRAMFILES"));
    REQUIRE_THAT(script, ContainsSubstring("RequestExecutionLevel admin"));
    REQUIRE_THAT(script, ContainsSubstring("InstallDirRegKey HKLM"));
    REQUIRE_THAT(script, ContainsSubstring("WriteRegStr HKLM"));
}

TEST_CASE("NSIS script per-user install uses HKCU registry", "[ship][installer]") {
    auto config = make_test_config();
    config.per_user_install = true;
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("InstallDirRegKey HKCU"));
    REQUIRE_THAT(script, ContainsSubstring("WriteRegStr HKCU"));
}

TEST_CASE("NSIS script deletes uninstall registry keys from selected hive", "[ship][installer]") {
    const auto uninstall_key =
        "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TestPlugin";

    auto admin = make_test_config();
    admin.per_user_install = false;
    auto admin_script = generate_nsis_script(admin);

    REQUIRE_THAT(admin_script, ContainsSubstring("DeleteRegKey HKLM \"Software\\TestCorp\\TestPlugin\""));
    REQUIRE_THAT(admin_script, ContainsSubstring(
        std::string("DeleteRegKey HKLM \"") + uninstall_key + "\""));

    auto user = make_test_config();
    user.per_user_install = true;
    auto user_script = generate_nsis_script(user);

    REQUIRE_THAT(user_script, ContainsSubstring("DeleteRegKey HKCU \"Software\\TestCorp\\TestPlugin\""));
    REQUIRE_THAT(user_script, ContainsSubstring(
        std::string("DeleteRegKey HKCU \"") + uninstall_key + "\""));
}

TEST_CASE("NSIS script includes directory selection page", "[ship][installer]") {
    auto config = make_test_config();
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("MUI_PAGE_DIRECTORY"));
}

TEST_CASE("NSIS script includes license page when path set", "[ship][installer]") {
    auto config = make_test_config();
    config.license_path = "/path/to/LICENSE.txt";
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("MUI_PAGE_LICENSE"));
    REQUIRE_THAT(script, ContainsSubstring("LICENSE.txt"));
}

TEST_CASE("NSIS script includes icon when set", "[ship][installer]") {
    auto config = make_test_config();
    config.icon_path = "/path/to/icon.ico";
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("MUI_ICON"));
    REQUIRE_THAT(script, ContainsSubstring("icon.ico"));
}

TEST_CASE("NSIS script includes publisher URL when set", "[ship][installer]") {
    auto config = make_test_config();
    config.url = "https://testcorp.com";
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("PRODUCT_WEB_SITE"));
    REQUIRE_THAT(script, ContainsSubstring("https://testcorp.com"));
}

TEST_CASE("NSIS script recurses into directory bundles", "[ship][installer]") {
    ScopedTempDir temp;
    auto bundle = temp.path / "TestPlugin.vst3";
    fs::create_directories(bundle);
    std::ofstream(bundle / "Contents.txt") << "bundle";

    auto config = make_test_config();
    config.plugins = {{bundle.string(), "", "vst3"}};
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("File /r \"" + bundle.string() + "\""));
    REQUIRE_THAT(script, ContainsSubstring("RMDir /r \"$COMMONFILES\\VST3\\TestPlugin.vst3\""));
}

TEST_CASE("NSIS script maps standalone and unknown formats to INSTDIR", "[ship][installer]") {
    auto config = make_test_config();
    config.plugins = {
        {"C:/build/TestPlugin.exe", "", "standalone"},
        {"C:/build/TestPlugin.lv2", "", "lv2"},
    };
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("Section \"TestPlugin (Standalone)\""));
    REQUIRE_THAT(script, ContainsSubstring("Section \"TestPlugin (lv2)\""));
    REQUIRE_THAT(script, ContainsSubstring("SetOutPath \"$INSTDIR\""));
    REQUIRE_THAT(script, ContainsSubstring("RMDir /r \"$INSTDIR\\TestPlugin.exe\""));
    REQUIRE_THAT(script, ContainsSubstring("RMDir /r \"$INSTDIR\\TestPlugin.lv2\""));
}

TEST_CASE("NSIS script handles empty plugin list with only post and uninstall sections",
          "[ship][installer][coverage][issue-644]") {
    auto config = make_test_config();
    config.plugins.clear();
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("Name \"TestPlugin\""));
    REQUIRE_THAT(script, ContainsSubstring("Section \"Uninstall\""));
    REQUIRE_THAT(script, ContainsSubstring("Section \"-Post\""));
    REQUIRE(script.find("TestPlugin (VST3)") == std::string::npos);
    REQUIRE(script.find("TestPlugin (CLAP)") == std::string::npos);
}

TEST_CASE("NSIS script honors configured output path and install root",
          "[ship][installer][coverage][issue-644]") {
    auto config = make_test_config();
    config.output_path = "dist/TestPlugin Setup.exe";
    config.publisher = "Acme Audio";
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("OutFile \"dist/TestPlugin Setup.exe\""));
    REQUIRE_THAT(script, ContainsSubstring("InstallDir \"$PROGRAMFILES\\Acme Audio\\TestPlugin\""));
    REQUIRE_THAT(script, ContainsSubstring("InstallDirRegKey HKLM \"Software\\Acme Audio\\TestPlugin\""));
}

TEST_CASE("NSIS script installs file plugins without recursive flag",
          "[ship][installer][coverage][issue-644]") {
    ScopedTempDir temp;
    auto plugin = temp.path / "TestPlugin.clap";
    std::ofstream(plugin) << "plugin";

    auto config = make_test_config();
    config.plugins = {{plugin.string(), "", "clap"}};
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("File \"" + plugin.string() + "\""));
    REQUIRE(script.find("File /r \"" + plugin.string() + "\"") == std::string::npos);
    REQUIRE_THAT(script, ContainsSubstring("RMDir /r \"$COMMONFILES\\CLAP\\TestPlugin.clap\""));
}

TEST_CASE("NSIS script uses per-user plugin common directories",
          "[ship][installer][coverage][issue-644]") {
    auto config = make_test_config();
    config.per_user_install = true;
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("SetOutPath \"$LOCALAPPDATA\\Programs\\Common\\VST3\""));
    REQUIRE_THAT(script, ContainsSubstring("SetOutPath \"$LOCALAPPDATA\\Programs\\Common\\CLAP\""));
    REQUIRE_THAT(script, ContainsSubstring("RMDir /r \"$LOCALAPPDATA\\Programs\\Common\\VST3\\TestPlugin.vst3\""));
    REQUIRE_THAT(script, ContainsSubstring("RMDir /r \"$LOCALAPPDATA\\Programs\\Common\\CLAP\\TestPlugin.clap\""));
}

TEST_CASE("NSIS script keeps install_dir field non-authoritative for format destinations",
          "[ship][installer][coverage][issue-644]") {
    auto config = make_test_config();
    config.plugins = {
        {"/tmp/custom/TestPlugin.vst3", "C:/Ignored/VST3", "vst3"},
        {"/tmp/custom/TestPlugin.clap", "C:/Ignored/CLAP", "clap"},
    };
    auto script = generate_nsis_script(config);

    REQUIRE_THAT(script, ContainsSubstring("SetOutPath \"$COMMONFILES\\VST3\""));
    REQUIRE_THAT(script, ContainsSubstring("SetOutPath \"$COMMONFILES\\CLAP\""));
    REQUIRE(script.find("C:/Ignored") == std::string::npos);
}
