// FileChooser widget tests — macOS plan item 6.2.
//
// Coverage:
//   * Builder accessors (title / initial directory / default name /
//     filters / allow_multiple) round-trip correctly and the fluent
//     `set_*` / `add_*` chain returns `*this`.
//   * `clear_filters()` empties the list and stays chainable.
//   * `add_extension_filter(desc, exts)` is a shorthand for `add_filter()`.
//   * On non-Apple platforms (where `set_backend()` is honored), the
//     dialog methods forward title / filters / initial-dir / default-name
//     to the platform backend and route the platform result back through
//     the user's callback. macOS routes through NSOpenPanel directly, so
//     `open()` / `save()` / `choose_folder()` are not invoked on Apple
//     here — those need a UI-test rig.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/file_chooser.hpp>
#include <pulp/platform/file_dialog.hpp>

#include <optional>
#include <string>
#include <vector>

using pulp::platform::FileDialog;
using pulp::platform::FileFilter;
using pulp::view::FileChooser;

TEST_CASE("FileChooser defaults are empty / single-select",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileChooser c;
    REQUIRE(c.title().empty());
    REQUIRE(c.initial_directory().empty());
    REQUIRE(c.default_name().empty());
    REQUIRE(c.filters().empty());
    REQUIRE_FALSE(c.allow_multiple());
}

TEST_CASE("FileChooser builders mutate and return *this for chaining",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileChooser c;
    auto& ref = c.set_title("Pick")
                 .set_initial_directory("/Users/me")
                 .set_default_name("preset.pulp")
                 .set_allow_multiple(true);
    REQUIRE(&ref == &c);
    REQUIRE(c.title() == "Pick");
    REQUIRE(c.initial_directory() == "/Users/me");
    REQUIRE(c.default_name() == "preset.pulp");
    REQUIRE(c.allow_multiple());
}

TEST_CASE("FileChooser add_filter preserves insertion order and shape",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileChooser c;
    c.add_filter({"Audio", "wav;flac"});
    c.add_extension_filter("Preset", "pulp");

    REQUIRE(c.filters().size() == 2);
    REQUIRE(c.filters()[0].description == "Audio");
    REQUIRE(c.filters()[0].extensions == "wav;flac");
    REQUIRE(c.filters()[1].description == "Preset");
    REQUIRE(c.filters()[1].extensions == "pulp");
}

TEST_CASE("FileChooser clear_filters resets to empty and stays chainable",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileChooser c;
    c.add_extension_filter("Audio", "wav").add_extension_filter("Preset", "pulp");
    REQUIRE(c.filters().size() == 2);

    auto& ref = c.clear_filters();
    REQUIRE(&ref == &c);
    REQUIRE(c.filters().empty());

    c.clear_filters().add_extension_filter("Reloaded", "wav");
    REQUIRE(c.filters().size() == 1);
    REQUIRE(c.filters()[0].description == "Reloaded");
}

TEST_CASE("FileChooser null callback discards results without crashing",
          "[file-chooser][issue-2026-05-24-6.2]") {
    // Even when the user passes a null callback, the dialog calls must
    // not invoke an empty std::function. On macOS the call still spawns
    // NSOpenPanel, so we gate that path off here — the non-Apple stub
    // returns no-selection synchronously which is what we want to test.
#if !defined(__APPLE__)
    FileDialog::clear_backend();
    FileChooser c;
    c.open(nullptr);          // no callback set; must not crash
    c.save(nullptr);
    c.choose_folder(nullptr);
    SUCCEED("no-callback invocations completed without throwing");
#else
    SUCCEED("skipped on Apple: open/save/choose_folder spawn NSOpenPanel");
#endif
}

#if !defined(__APPLE__)
// On non-Apple platforms `set_backend()` actually replaces the dialog
// implementations, so we can verify FileChooser forwards every builder
// field to the platform layer and returns the backend's result through
// the user's callback.

TEST_CASE("FileChooser open forwards title/filters/initial-dir to backend",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileDialog::Backend b;
    int open_calls = 0;
    b.open_file = [&](const std::string& title,
                      const std::vector<FileFilter>& filters,
                      const std::string& default_path) {
        ++open_calls;
        REQUIRE(title == "Pick preset");
        REQUIRE(filters.size() == 1);
        REQUIRE(filters[0].description == "Preset");
        REQUIRE(filters[0].extensions == "pulp");
        REQUIRE(default_path == "/presets");
        return std::optional<std::string>("/presets/lead.pulp");
    };
    FileDialog::set_backend(b);

    FileChooser c;
    c.set_title("Pick preset")
     .set_initial_directory("/presets")
     .add_extension_filter("Preset", "pulp");

    std::vector<std::string> got;
    c.open([&](std::vector<std::string> paths) { got = std::move(paths); });

    REQUIRE(open_calls == 1);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0] == "/presets/lead.pulp");

    FileDialog::clear_backend();
}

TEST_CASE("FileChooser open routes through open_files when allow_multiple",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileDialog::Backend b;
    int open_files_calls = 0;
    int open_file_calls = 0;
    b.open_file = [&](const std::string&, const std::vector<FileFilter>&,
                      const std::string&) {
        ++open_file_calls;
        return std::optional<std::string>(std::nullopt);
    };
    b.open_files = [&](const std::string&, const std::vector<FileFilter>&,
                       const std::string&) {
        ++open_files_calls;
        return std::vector<std::string>{"/a.wav", "/b.wav", "/c.wav"};
    };
    FileDialog::set_backend(b);

    FileChooser c;
    c.set_allow_multiple(true);

    std::vector<std::string> got;
    c.open([&](std::vector<std::string> paths) { got = std::move(paths); });

    REQUIRE(open_files_calls == 1);
    REQUIRE(open_file_calls == 0);
    REQUIRE(got.size() == 3);
    REQUIRE(got[0] == "/a.wav");
    REQUIRE(got[2] == "/c.wav");

    FileDialog::clear_backend();
}

TEST_CASE("FileChooser open delivers empty vector on user cancel",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileDialog::Backend b;
    b.open_file = [](const std::string&, const std::vector<FileFilter>&,
                     const std::string&) {
        return std::optional<std::string>(std::nullopt);
    };
    FileDialog::set_backend(b);

    FileChooser c;
    bool called = false;
    std::vector<std::string> got{"stale"};  // ensure callback overwrites it
    c.open([&](std::vector<std::string> paths) {
        called = true;
        got = std::move(paths);
    });

    REQUIRE(called);
    REQUIRE(got.empty());

    FileDialog::clear_backend();
}

TEST_CASE("FileChooser save forwards default_name and uses Save title default",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileDialog::Backend b;
    int save_calls = 0;
    b.save_file = [&](const std::string& title,
                      const std::vector<FileFilter>& filters,
                      const std::string& default_path,
                      const std::string& default_name) {
        ++save_calls;
        REQUIRE(title == "Save");            // default applied when title_ empty
        REQUIRE(filters.empty());
        REQUIRE(default_path == "/exports");
        REQUIRE(default_name == "lead.pulp");
        return std::optional<std::string>("/exports/lead.pulp");
    };
    FileDialog::set_backend(b);

    FileChooser c;
    c.set_initial_directory("/exports").set_default_name("lead.pulp");

    std::vector<std::string> got;
    c.save([&](std::vector<std::string> paths) { got = std::move(paths); });

    REQUIRE(save_calls == 1);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0] == "/exports/lead.pulp");

    FileDialog::clear_backend();
}

TEST_CASE("FileChooser choose_folder ignores filters and yields Choose Folder default",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileDialog::Backend b;
    int folder_calls = 0;
    b.choose_folder = [&](const std::string& title,
                          const std::string& default_path) {
        ++folder_calls;
        REQUIRE(title == "Choose Folder");
        REQUIRE(default_path == "/Users/me");
        return std::optional<std::string>("/Users/me/Picked");
    };
    FileDialog::set_backend(b);

    FileChooser c;
    c.set_initial_directory("/Users/me")
     .add_extension_filter("ignored", "wav");

    std::vector<std::string> got;
    c.choose_folder([&](std::vector<std::string> paths) {
        got = std::move(paths);
    });

    REQUIRE(folder_calls == 1);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0] == "/Users/me/Picked");

    FileDialog::clear_backend();
}

TEST_CASE("FileChooser open uses Open title default when title_ is empty",
          "[file-chooser][issue-2026-05-24-6.2]") {
    FileDialog::Backend b;
    std::string seen_title;
    b.open_file = [&](const std::string& title,
                      const std::vector<FileFilter>&, const std::string&) {
        seen_title = title;
        return std::optional<std::string>(std::nullopt);
    };
    FileDialog::set_backend(b);

    FileChooser c;
    c.open(nullptr);
    REQUIRE(seen_title == "Open");

    FileDialog::clear_backend();
}

#endif // !defined(__APPLE__)
