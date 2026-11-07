#include <catch2/catch_test_macros.hpp>

#include <pulp/platform/special_dir.hpp>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

using namespace pulp::platform;
namespace fs = std::filesystem;

namespace {

SpecialDirInputs inputs_with(std::map<std::string, std::string> env) {
    SpecialDirInputs in;
    in.getenv = [env = std::move(env)](std::string_view name) -> std::optional<std::string> {
        auto it = env.find(std::string(name));
        if (it == env.end() || it->second.empty())
            return std::nullopt;
        return it->second;
    };
    return in;
}

std::string dir(SpecialDirOs os, SpecialDir kind, const SpecialDirInputs& in) {
    return special_dir_for(os, kind, in);
}

} // namespace

// ── String shapes, resolved for every OS on any host ─────────────────────────

TEST_CASE("special_dir_for macOS layout", "[special_dir]") {
    auto in = inputs_with({{"HOME", "/Users/ada"}, {"TMPDIR", "/var/folders/xy/T/"}});
    in.executable = "/Applications/Synth.app/Contents/MacOS/Synth";
    in.module = "/Library/Audio/Plug-Ins/Components/Fx.component/Contents/MacOS/Fx";
    const auto os = SpecialDirOs::macos;

    CHECK(dir(os, SpecialDir::home, in) == "/Users/ada");
    CHECK(dir(os, SpecialDir::documents, in) == "/Users/ada/Documents");
    CHECK(dir(os, SpecialDir::desktop, in) == "/Users/ada/Desktop");
    CHECK(dir(os, SpecialDir::music, in) == "/Users/ada/Music");
    CHECK(dir(os, SpecialDir::app_data, in) == "/Users/ada/Library/Application Support");
    CHECK(dir(os, SpecialDir::config, in) == "/Users/ada/Library/Application Support");
    CHECK(dir(os, SpecialDir::app_data_shared, in) == "/Library/Application Support");
    CHECK(dir(os, SpecialDir::cache, in) == "/Users/ada/Library/Caches");
    CHECK(dir(os, SpecialDir::logs, in) == "/Users/ada/Library/Logs");
    CHECK(dir(os, SpecialDir::user_presets, in) == "/Users/ada/Library/Audio/Presets");
    CHECK(dir(os, SpecialDir::temp, in) == "/var/folders/xy/T");
    CHECK(dir(os, SpecialDir::current_executable, in) == in.executable);
    CHECK(dir(os, SpecialDir::bundle_dir, in) ==
          "/Library/Audio/Plug-Ins/Components/Fx.component/Contents/Resources");
}

TEST_CASE("special_dir_for macOS app bundle and bare executable", "[special_dir]") {
    auto in = inputs_with({{"HOME", "/Users/ada"}});
    in.executable = "/Applications/Synth.app/Contents/MacOS/Synth";
    in.module = in.executable;
    CHECK(dir(SpecialDirOs::macos, SpecialDir::bundle_dir, in) ==
          "/Applications/Synth.app/Contents/Resources");

    // Innermost bundle wins: an app extension inside an app.
    in.module = "/Applications/Host.app/Contents/PlugIns/Fx.appex/Contents/MacOS/Fx";
    CHECK(dir(SpecialDirOs::macos, SpecialDir::bundle_dir, in) ==
          "/Applications/Host.app/Contents/PlugIns/Fx.appex/Contents/Resources");

    // Not in a bundle: the folder holding the module.
    in.module = "/usr/local/bin/tool";
    CHECK(dir(SpecialDirOs::macos, SpecialDir::bundle_dir, in) == "/usr/local/bin");

    // No module recorded: fall back to the executable.
    in.module.clear();
    in.executable = "/opt/tool/run";
    CHECK(dir(SpecialDirOs::macos, SpecialDir::bundle_dir, in) == "/opt/tool");
}

TEST_CASE("special_dir_for iOS layout", "[special_dir]") {
    auto in = inputs_with({{"HOME", "/var/mobile/Containers/Data/Application/ABC"}});
    in.module = "/private/var/containers/Bundle/Application/XYZ/Synth.app/Synth";
    const auto os = SpecialDirOs::ios;
    const std::string home = "/var/mobile/Containers/Data/Application/ABC";

    CHECK(dir(os, SpecialDir::home, in) == home);
    CHECK(dir(os, SpecialDir::documents, in) == home + "/Documents");
    CHECK(dir(os, SpecialDir::app_data, in) == home + "/Library/Application Support");
    CHECK(dir(os, SpecialDir::cache, in) == home + "/Library/Caches");
    CHECK(dir(os, SpecialDir::logs, in) == home + "/Library/Logs");
    CHECK(dir(os, SpecialDir::desktop, in).empty());
    CHECK(dir(os, SpecialDir::music, in).empty());
    CHECK(dir(os, SpecialDir::app_data_shared, in).empty());
    // iOS bundles are flat: resources sit at the bundle root.
    CHECK(dir(os, SpecialDir::bundle_dir, in) ==
          "/private/var/containers/Bundle/Application/XYZ/Synth.app");
}

TEST_CASE("special_dir_for Windows layout", "[special_dir]") {
    auto in = inputs_with({{"USERPROFILE", "C:\\Users\\Ada"},
                           {"APPDATA", "C:\\Users\\Ada\\AppData\\Roaming"},
                           {"LOCALAPPDATA", "C:\\Users\\Ada\\AppData\\Local"},
                           {"ProgramData", "D:\\ProgramData"},
                           {"TEMP", "C:\\Users\\Ada\\AppData\\Local\\Temp"}});
    in.executable = "C:\\Program Files\\Host\\host.exe";
    in.module = "C:\\Program Files\\Common Files\\VST3\\Fx.vst3\\Contents\\x86_64-win\\Fx.vst3";
    const auto os = SpecialDirOs::windows;

    CHECK(dir(os, SpecialDir::home, in) == "C:\\Users\\Ada");
    CHECK(dir(os, SpecialDir::documents, in) == "C:\\Users\\Ada\\Documents");
    CHECK(dir(os, SpecialDir::desktop, in) == "C:\\Users\\Ada\\Desktop");
    CHECK(dir(os, SpecialDir::music, in) == "C:\\Users\\Ada\\Music");
    CHECK(dir(os, SpecialDir::app_data, in) == "C:\\Users\\Ada\\AppData\\Roaming");
    CHECK(dir(os, SpecialDir::config, in) == "C:\\Users\\Ada\\AppData\\Roaming");
    CHECK(dir(os, SpecialDir::user_presets, in) == "C:\\Users\\Ada\\AppData\\Roaming");
    CHECK(dir(os, SpecialDir::app_data_shared, in) == "D:\\ProgramData");
    CHECK(dir(os, SpecialDir::cache, in) == "C:\\Users\\Ada\\AppData\\Local");
    CHECK(dir(os, SpecialDir::logs, in) == "C:\\Users\\Ada\\AppData\\Local\\Logs");
    CHECK(dir(os, SpecialDir::temp, in) == "C:\\Users\\Ada\\AppData\\Local\\Temp");
    CHECK(dir(os, SpecialDir::current_executable, in) == "C:\\Program Files\\Host\\host.exe");
    CHECK(dir(os, SpecialDir::bundle_dir, in) ==
          "C:\\Program Files\\Common Files\\VST3\\Fx.vst3\\Contents\\Resources");

    // A redirected Documents known folder wins over the derived path.
    in.known_documents = "D:\\Cloud\\Documents";
    CHECK(dir(os, SpecialDir::documents, in) == "D:\\Cloud\\Documents");
}

TEST_CASE("special_dir_for Windows fallbacks", "[special_dir]") {
    auto in = inputs_with({{"USERPROFILE", "C:\\Users\\Ada"}});
    in.module = "C:\\Tools\\app.exe";
    const auto os = SpecialDirOs::windows;
    CHECK(dir(os, SpecialDir::app_data, in) == "C:\\Users\\Ada\\AppData\\Roaming");
    CHECK(dir(os, SpecialDir::cache, in) == "C:\\Users\\Ada\\AppData\\Local");
    CHECK(dir(os, SpecialDir::app_data_shared, in) == "C:\\ProgramData");
    CHECK(dir(os, SpecialDir::temp, in) == "C:\\Users\\Ada\\AppData\\Local\\Temp");
    CHECK(dir(os, SpecialDir::bundle_dir, in) == "C:\\Tools");

    // No home at all: per-user kinds are unavailable rather than guessed.
    auto none = inputs_with({});
    CHECK(dir(os, SpecialDir::home, none).empty());
    CHECK(dir(os, SpecialDir::documents, none).empty());
    CHECK(dir(os, SpecialDir::cache, none).empty());
}

TEST_CASE("special_dir_for Linux follows XDG", "[special_dir]") {
    auto in = inputs_with({{"HOME", "/home/ada"}});
    in.executable = "/usr/bin/host";
    in.module = "/home/ada/.vst3/Fx.vst3/Contents/x86_64-linux/Fx.so";
    const auto os = SpecialDirOs::linux_desktop;

    CHECK(dir(os, SpecialDir::home, in) == "/home/ada");
    CHECK(dir(os, SpecialDir::documents, in) == "/home/ada/Documents");
    CHECK(dir(os, SpecialDir::app_data, in) == "/home/ada/.local/share");
    CHECK(dir(os, SpecialDir::config, in) == "/home/ada/.config");
    CHECK(dir(os, SpecialDir::user_presets, in) == "/home/ada/.config");
    CHECK(dir(os, SpecialDir::cache, in) == "/home/ada/.cache");
    CHECK(dir(os, SpecialDir::logs, in) == "/home/ada/.local/state/logs");
    CHECK(dir(os, SpecialDir::app_data_shared, in) == "/usr/share");
    CHECK(dir(os, SpecialDir::temp, in) == "/tmp");
    CHECK(dir(os, SpecialDir::bundle_dir, in) == "/home/ada/.vst3/Fx.vst3/Contents/Resources");

    auto xdg = inputs_with({{"HOME", "/home/ada"},
                            {"XDG_DATA_HOME", "/data"},
                            {"XDG_CONFIG_HOME", "/cfg"},
                            {"XDG_CACHE_HOME", "/cache"},
                            {"XDG_STATE_HOME", "/state"},
                            {"XDG_DOCUMENTS_DIR", "/docs"},
                            {"TMPDIR", "/run/user/1000/"}});
    CHECK(dir(os, SpecialDir::app_data, xdg) == "/data");
    CHECK(dir(os, SpecialDir::config, xdg) == "/cfg");
    CHECK(dir(os, SpecialDir::user_presets, xdg) == "/cfg");
    CHECK(dir(os, SpecialDir::cache, xdg) == "/cache");
    CHECK(dir(os, SpecialDir::logs, xdg) == "/state/logs");
    CHECK(dir(os, SpecialDir::documents, xdg) == "/docs");
    CHECK(dir(os, SpecialDir::temp, xdg) == "/run/user/1000");

    // The XDG spec says relative values are invalid and must be ignored.
    auto relative = inputs_with({{"HOME", "/home/ada"}, {"XDG_CONFIG_HOME", "cfg"}});
    CHECK(dir(os, SpecialDir::config, relative) == "/home/ada/.config");

    // A plain binary outside a bundle resolves to its own folder.
    in.module = "/usr/lib/libfx.so";
    CHECK(dir(os, SpecialDir::bundle_dir, in) == "/usr/lib");
}

TEST_CASE("special_dir_for Android lives under the app files directory", "[special_dir]") {
    SpecialDirInputs in = inputs_with({});
    const auto os = SpecialDirOs::android;

    // Until the Kotlin hook has supplied the files directory nothing resolves.
    for (auto kind : kAllSpecialDirs) {
        if (kind == SpecialDir::current_executable)
            continue;
        INFO(special_dir_name(kind));
        CHECK(dir(os, kind, in).empty());
    }

    in.android_files_dir = "/data/user/0/dev.pulp.app/files";
    in.android_cache_dir = "/data/user/0/dev.pulp.app/cache";
    CHECK(dir(os, SpecialDir::app_data, in) == "/data/user/0/dev.pulp.app/files");
    CHECK(dir(os, SpecialDir::documents, in) == "/data/user/0/dev.pulp.app/files/Documents");
    CHECK(dir(os, SpecialDir::logs, in) == "/data/user/0/dev.pulp.app/files/logs");
    CHECK(dir(os, SpecialDir::user_presets, in) == "/data/user/0/dev.pulp.app/files/Presets");
    CHECK(dir(os, SpecialDir::cache, in) == "/data/user/0/dev.pulp.app/cache");
    CHECK(dir(os, SpecialDir::temp, in) == "/data/user/0/dev.pulp.app/cache");
    CHECK(dir(os, SpecialDir::desktop, in).empty());
    CHECK(dir(os, SpecialDir::app_data_shared, in).empty());

    in.android_cache_dir.clear();
    CHECK(dir(os, SpecialDir::cache, in) == "/data/user/0/dev.pulp.app/files/cache");
}

TEST_CASE("special_dir_for Web uses an in-memory tree", "[special_dir]") {
    const SpecialDirInputs in = inputs_with({});
    const auto os = SpecialDirOs::web;
    CHECK(dir(os, SpecialDir::home, in) == "/home/web_user");
    CHECK(dir(os, SpecialDir::app_data, in) == "/home/web_user/.local/share");
    CHECK(dir(os, SpecialDir::cache, in) == "/home/web_user/.cache");
    CHECK(dir(os, SpecialDir::temp, in) == "/tmp");
    CHECK(dir(os, SpecialDir::bundle_dir, in) == "/");
    CHECK(dir(os, SpecialDir::current_executable, in).empty());
}

TEST_CASE("every kind has a unique stable name", "[special_dir]") {
    std::map<std::string, int> seen;
    for (auto kind : kAllSpecialDirs) {
        const auto name = special_dir_name(kind);
        CHECK(name != "unknown");
        CHECK(++seen[std::string(name)] == 1);
    }
}

// ── Host OS ──────────────────────────────────────────────────────────────────

TEST_CASE("special_dir resolves on the host", "[special_dir]") {
    clear_special_dir_overrides();

    const fs::path exe = special_dir(SpecialDir::current_executable);
    REQUIRE_FALSE(exe.empty());
    CHECK(exe.is_absolute());
    CHECK(fs::exists(exe));

    const fs::path bundle = special_dir(SpecialDir::bundle_dir);
    CHECK_FALSE(bundle.empty());

    const fs::path temp = special_dir(SpecialDir::temp);
    CHECK_FALSE(temp.empty());
    CHECK(fs::exists(temp));

#if !defined(_WIN32)
    if (const char* home = std::getenv("HOME"); home && *home) {
        CHECK(special_dir(SpecialDir::home) == fs::path(home));
        CHECK(special_dir(SpecialDir::cache).string().rfind(home, 0) == 0);
        CHECK(special_dir(SpecialDir::documents) == fs::path(home) / "Documents");
    }
#endif
}

TEST_CASE("special_dir overrides replace and restore resolution", "[special_dir]") {
    clear_special_dir_overrides();
    const fs::path real = special_dir(SpecialDir::logs);

    set_special_dir_override(SpecialDir::logs, fs::path("/sandbox/logs"));
    CHECK(special_dir(SpecialDir::logs) == fs::path("/sandbox/logs"));
    CHECK(special_dir(SpecialDir::cache) != fs::path("/sandbox/logs"));

    set_special_dir_override(SpecialDir::logs, {});
    CHECK(special_dir(SpecialDir::logs) == real);

    set_special_dir_override(SpecialDir::logs, fs::path("/a"));
    set_special_dir_override(SpecialDir::cache, fs::path("/b"));
    clear_special_dir_overrides();
    CHECK(special_dir(SpecialDir::logs) == real);
}
