#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/platform.hpp>
#include <pulp/platform/popup_menu.hpp>
#include <pulp/platform/win/registry.hpp>

using namespace pulp::platform;

TEST_CASE("Platform detection", "[platform]") {
    SECTION("Current OS is known") {
        REQUIRE(current_os != OS::Unknown);
    }

    SECTION("At least one platform flag is true") {
        REQUIRE((is_macos || is_ios || is_windows || is_linux || is_android));
    }

    SECTION("Desktop or mobile is set") {
        REQUIRE((is_desktop || is_mobile));
    }

    SECTION("Architecture is known") {
        REQUIRE(current_arch != Arch::Unknown);
    }

    SECTION("64-bit detection") {
        REQUIRE(is_64bit);  // We only target 64-bit
    }

    SECTION("Compiler is known") {
        REQUIRE(current_compiler != Compiler::Unknown);
    }

    SECTION("Endianness is consistent") {
        REQUIRE((is_little_endian || is_big_endian));
        REQUIRE(is_little_endian != is_big_endian);
    }
}

#ifdef __APPLE__
TEST_CASE("Apple-specific detection", "[platform]") {
    REQUIRE(is_apple);
    REQUIRE((is_macos || is_ios));
    REQUIRE((current_compiler == Compiler::AppleClang || current_compiler == Compiler::Clang));
}
#endif

TEST_CASE("PopupMenu records items and separators",
          "[platform][popup-menu][issue-640]") {
    PopupMenu menu;

    menu.add_item(10, "Open", true, false);
    menu.add_separator();
    menu.add_item(20, "Pinned", false, true);

    const auto& items = menu.items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].id == 10);
    REQUIRE(items[0].label == "Open");
    REQUIRE(items[0].enabled);
    REQUIRE_FALSE(items[0].checked);
    REQUIRE_FALSE(items[0].is_separator);

    REQUIRE(items[1].id == 0);
    REQUIRE(items[1].label.empty());
    REQUIRE(items[1].enabled);
    REQUIRE_FALSE(items[1].checked);
    REQUIRE(items[1].is_separator);

    REQUIRE(items[2].id == 20);
    REQUIRE(items[2].label == "Pinned");
    REQUIRE_FALSE(items[2].enabled);
    REQUIRE(items[2].checked);
    REQUIRE_FALSE(items[2].is_separator);
}

#if !defined(__APPLE__)
TEST_CASE("PopupMenu fallback returns no selection on non-Apple platforms",
          "[platform][popup-menu][issue-640]") {
    PopupMenu menu;
    menu.add_item(1, "Only action");

    REQUIRE_FALSE(menu.show(12.0f, 34.0f).has_value());
    REQUIRE_FALSE(menu.show_at_view(reinterpret_cast<void*>(0x1)).has_value());
}
#endif

#if !defined(_WIN32)
TEST_CASE("Windows registry helpers fail closed on non-Windows platforms",
          "[platform][registry][issue-640]") {
    using namespace pulp::platform::win;

    REQUIRE_FALSE(registry_read_string("HKCU", "Software\\Pulp", "InstallDir").has_value());
    REQUIRE_FALSE(registry_read_string("HKEY_LOCAL_MACHINE", "", "").has_value());
    REQUIRE_FALSE(registry_read_dword("HKLM", "Software\\Pulp", "Version").has_value());
    REQUIRE_FALSE(registry_read_dword("not-a-root", "", "").has_value());

    REQUIRE_FALSE(registry_write_string("HKCU", "Software\\Pulp", "InstallDir", "C:\\Pulp"));
    REQUIRE_FALSE(registry_write_string("not-a-root", "", "", ""));
    REQUIRE_FALSE(registry_write_dword("HKLM", "Software\\Pulp", "Version", 42));
    REQUIRE_FALSE(registry_write_dword("not-a-root", "", "", 0));
    REQUIRE_FALSE(registry_delete_value("HKCU", "Software\\Pulp", "InstallDir"));
    REQUIRE_FALSE(registry_delete_value("not-a-root", "", ""));
}
#endif
