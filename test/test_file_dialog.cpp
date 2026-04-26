// #301: FileDialog on non-Apple platforms routes through a host-
// registered Backend. Without one, calls return explicit nullopt
// so the JS bridge can distinguish "no backend installed" from
// "user cancelled the dialog".
//
// The backend API is public on every platform. Apple platforms
// ship a native built-in impl in file_dialog_mac.mm and the
// registration API is a no-op there.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/platform/popup_menu.hpp>

#include <string>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

using pulp::platform::FileDialog;
using pulp::platform::FileFilter;
using pulp::platform::PopupMenu;

TEST_CASE("FileDialog has_backend reflects native vs host-registered availability",
          "[platform][file-dialog][issue-301][issue-312][issue-316]") {
    // Clean host-registered state — test suite may run in any order.
    // Post-#312 + #316 (Codex P2s): has_backend() reports true
    // unconditionally ONLY on macOS, which ships file_dialog_mac.mm.
    // iOS and non-Apple reflect the host-registered state until their
    // native impls land.
    FileDialog::clear_backend();
#if defined(__APPLE__) && TARGET_OS_OSX
    REQUIRE(FileDialog::has_backend());
#else
    REQUIRE_FALSE(FileDialog::has_backend());
#endif
}

#if !defined(__APPLE__)
// Non-Apple: dialog methods route through the backend. Without one,
// every method returns an explicit no-selection. With a fake backend
// installed, calls reach the backend and its return value is passed
// back to the caller.

TEST_CASE("FileDialog non-Apple: no backend -> explicit no-selection",
          "[platform][file-dialog][issue-301]") {
    FileDialog::clear_backend();

    auto open = FileDialog::open_file("t", {}, "");
    REQUIRE_FALSE(open.has_value());

    auto many = FileDialog::open_files("t", {}, "");
    REQUIRE(many.empty());

    auto save = FileDialog::save_file("t", {}, "", "");
    REQUIRE_FALSE(save.has_value());

    auto folder = FileDialog::choose_folder("t", "");
    REQUIRE_FALSE(folder.has_value());
}

TEST_CASE("FileDialog non-Apple: installed backend without handlers fails closed",
          "[platform][file-dialog][issue-640]") {
    FileDialog::Backend b;
    FileDialog::set_backend(b);
    REQUIRE(FileDialog::has_backend());

    REQUIRE_FALSE(FileDialog::open_file("open", {}, "").has_value());
    REQUIRE(FileDialog::open_files("open-many", {}, "").empty());
    REQUIRE_FALSE(FileDialog::save_file("save", {}, "", "preset.pulp").has_value());
    REQUIRE_FALSE(FileDialog::choose_folder("folder", "").has_value());

    FileDialog::clear_backend();
    REQUIRE_FALSE(FileDialog::has_backend());
}

TEST_CASE("FileDialog non-Apple: backend routes through",
          "[platform][file-dialog][issue-301]") {
    FileDialog::Backend b;
    int open_calls = 0, open_many_calls = 0, save_calls = 0, folder_calls = 0;

    b.open_file = [&](const std::string& title, const std::vector<FileFilter>&,
                      const std::string&) {
        open_calls++;
        REQUIRE(title == "my-open");
        return std::optional<std::string>("/tmp/picked.wav");
    };
    b.open_files = [&](const std::string&, const std::vector<FileFilter>&,
                       const std::string&) {
        open_many_calls++;
        return std::vector<std::string>{"/tmp/a.wav", "/tmp/b.wav"};
    };
    b.save_file = [&](const std::string&, const std::vector<FileFilter>&,
                      const std::string&, const std::string& name) {
        save_calls++;
        REQUIRE(name == "preset.pulp");
        return std::optional<std::string>("/tmp/preset.pulp");
    };
    b.choose_folder = [&](const std::string&, const std::string&) {
        folder_calls++;
        return std::optional<std::string>("/tmp/out");
    };

    FileDialog::set_backend(b);
    REQUIRE(FileDialog::has_backend());

    auto open = FileDialog::open_file("my-open", {}, "");
    REQUIRE(open.has_value());
    REQUIRE(*open == "/tmp/picked.wav");
    REQUIRE(open_calls == 1);

    auto many = FileDialog::open_files("", {}, "");
    REQUIRE(many.size() == 2);
    REQUIRE(open_many_calls == 1);

    auto save = FileDialog::save_file("", {}, "", "preset.pulp");
    REQUIRE(save.has_value());
    REQUIRE(save_calls == 1);

    auto folder = FileDialog::choose_folder("", "");
    REQUIRE(folder.has_value());
    REQUIRE(folder_calls == 1);

    FileDialog::clear_backend();
    REQUIRE_FALSE(FileDialog::has_backend());

    // Post-clear calls fail closed (no fake-success).
    REQUIRE_FALSE(FileDialog::open_file("", {}, "").has_value());
    REQUIRE(open_calls == 1); // not called again
}
#endif // !defined(__APPLE__)

TEST_CASE("PopupMenu stores item metadata without showing native UI",
          "[platform][popup-menu][issue-640]") {
    PopupMenu menu;
    menu.add_item(7, "Enabled");
    menu.add_item(8, "Checked disabled", false, true);
    menu.add_separator();

    REQUIRE(menu.items().size() == 3);
    REQUIRE(menu.items()[0].id == 7);
    REQUIRE(menu.items()[0].label == "Enabled");
    REQUIRE(menu.items()[0].enabled);
    REQUIRE_FALSE(menu.items()[0].checked);
    REQUIRE_FALSE(menu.items()[0].is_separator);

    REQUIRE(menu.items()[1].id == 8);
    REQUIRE_FALSE(menu.items()[1].enabled);
    REQUIRE(menu.items()[1].checked);
    REQUIRE_FALSE(menu.items()[1].is_separator);

    REQUIRE(menu.items()[2].id == 0);
    REQUIRE(menu.items()[2].label.empty());
    REQUIRE(menu.items()[2].is_separator);
}

#if !defined(__APPLE__)
TEST_CASE("PopupMenu non-Apple stub returns no selection",
          "[platform][popup-menu][issue-640]") {
    PopupMenu menu;
    menu.add_item(1, "One");

    REQUIRE_FALSE(menu.show(10.0f, 20.0f).has_value());
    REQUIRE_FALSE(menu.show_at_view(nullptr).has_value());
}
#endif

TEST_CASE("FileDialog backend registration is safe to call on any platform",
          "[platform][file-dialog][issue-301][issue-312]") {
    // Just register + clear — must not throw or crash on any platform
    // even if the native impl is in the .mm file.
    FileDialog::Backend b;
    b.open_file = [](const std::string&, const std::vector<FileFilter>&,
                     const std::string&) {
        return std::optional<std::string>(std::nullopt);
    };
    FileDialog::set_backend(b);
    FileDialog::clear_backend();
    // Post-#312 + #316: has_backend() is true only on macOS (native
    // impl); iOS and non-Apple reflect host-registered state.
#if defined(__APPLE__) && TARGET_OS_OSX
    REQUIRE(FileDialog::has_backend());
#else
    REQUIRE_FALSE(FileDialog::has_backend());
#endif
}
