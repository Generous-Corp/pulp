#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <pulp/ship/installer.hpp>

using namespace pulp::ship;
using Catch::Matchers::ContainsSubstring;

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
