#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/platform.hpp>
#include <pulp/platform/popup_menu.hpp>
#include <pulp/platform/progress_parser.hpp>
#include <pulp/platform/win/registry.hpp>

#include <vector>

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

TEST_CASE("Platform detection flags are mutually consistent",
          "[platform][coverage][phase3]") {
    const int os_flags =
        static_cast<int>(is_macos) +
        static_cast<int>(is_ios) +
        static_cast<int>(is_windows) +
        static_cast<int>(is_linux) +
        static_cast<int>(is_android);
    REQUIRE(os_flags == 1);

    REQUIRE(is_apple == (is_macos || is_ios));
    REQUIRE(is_desktop == (is_macos || is_windows || is_linux));
    REQUIRE(is_mobile == (is_ios || is_android));
    REQUIRE_FALSE((is_desktop && is_mobile));

    switch (current_os) {
        case OS::macOS:   REQUIRE(is_macos); break;
        case OS::iOS:     REQUIRE(is_ios); break;
        case OS::Windows: REQUIRE(is_windows); break;
        case OS::Linux:   REQUIRE(is_linux); break;
        case OS::Android: REQUIRE(is_android); break;
        case OS::Unknown: FAIL("current OS should be known in supported test lanes"); break;
    }
}

TEST_CASE("Architecture detection flags are mutually consistent",
          "[platform][coverage][phase3]") {
    REQUIRE(is_arm == (current_arch == Arch::ARM64 || current_arch == Arch::ARM32));
    REQUIRE(is_x86 == (current_arch == Arch::x86_64 || current_arch == Arch::x86));
    REQUIRE(is_64bit == (current_arch == Arch::x86_64 || current_arch == Arch::ARM64));
    REQUIRE_FALSE((is_arm && is_x86));

    switch (current_arch) {
        case Arch::x86_64:
            REQUIRE(is_x86);
            REQUIRE(is_64bit);
            break;
        case Arch::ARM64:
            REQUIRE(is_arm);
            REQUIRE(is_64bit);
            break;
        case Arch::ARM32:
            REQUIRE(is_arm);
            REQUIRE_FALSE(is_64bit);
            break;
        case Arch::x86:
            REQUIRE(is_x86);
            REQUIRE_FALSE(is_64bit);
            break;
        case Arch::Unknown:
            FAIL("current architecture should be known in supported test lanes");
            break;
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

TEST_CASE("ProgressParser accepts payloads, empty payloads, and type-only lines",
          "[platform][progress][coverage][issue-649]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) {
        events.push_back(e);
    });

    parser.feed_line("PROGRESS:DOWNLOAD_START:https://example.test/file.zip");
    parser.feed_line("PROGRESS:OVERALL:42");
    parser.feed_line("PROGRESS:EMPTY:");
    parser.feed_line("PROGRESS:TYPE_ONLY");

    REQUIRE(events.size() == 4);
    REQUIRE(events[0].type == "DOWNLOAD_START");
    REQUIRE(events[0].payload == "https://example.test/file.zip");
    REQUIRE(events[1].type == "OVERALL");
    REQUIRE(events[1].payload == "42");
    REQUIRE(events[2].type == "EMPTY");
    REQUIRE(events[2].payload.empty());
    REQUIRE(events[3].type == "TYPE_ONLY");
    REQUIRE(events[3].payload.empty());
}

TEST_CASE("ProgressParser ignores non-progress lines and tolerates empty callbacks",
          "[platform][progress][coverage][issue-649]") {
    int calls = 0;
    ProgressParser parser([&](const ProgressEvent&) { ++calls; });

    parser.feed_line("");
    parser.feed_line("progress:LOWERCASE:ignored");
    parser.feed_line("INFO:PROGRESS:OVERALL:1");
    parser.feed_line("PROGRESSISH:OVERALL:1");
    REQUIRE(calls == 0);

    ProgressParser empty_callback({});
    empty_callback.feed_line("PROGRESS:OVERALL:100");
    empty_callback.feed_line("PROGRESS:TYPE_ONLY");
    SUCCEED("empty callbacks are inert");
}

TEST_CASE("ProgressParser preserves payload colons and empty event types",
          "[platform][progress][coverage][phase3]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) {
        events.push_back(e);
    });

    parser.feed_line("PROGRESS:URL:https://example.test/a:b:c");
    parser.feed_line("PROGRESS::payload-with-empty-type");
    parser.feed_line("PROGRESS:");

    REQUIRE(events.size() == 3);
    REQUIRE(events[0].type == "URL");
    REQUIRE(events[0].payload == "https://example.test/a:b:c");
    REQUIRE(events[1].type.empty());
    REQUIRE(events[1].payload == "payload-with-empty-type");
    REQUIRE(events[2].type.empty());
    REQUIRE(events[2].payload.empty());
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
