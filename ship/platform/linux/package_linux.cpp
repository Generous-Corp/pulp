// Linux packaging stubs for Pulp
// API surface for .deb and AppImage creation

#include <pulp/ship/codesign.hpp>
#include <pulp/platform/child_process.hpp>

#if defined(__linux__)

#include <cstdlib>
#include <cstdio>

namespace pulp::ship {

static int exec_status(const std::string& cmd) {
    auto r = pulp::platform::exec("/bin/sh", {"-c", cmd}, 60000);
    return r.exit_code;
}

// Linux has no code signing equivalent
SigningInfo check_codesign(const std::string&) { return {}; }
bool check_notarization(const std::string&) { return false; }
bool codesign(const std::string&, const std::string&, const std::string&) { return false; }
std::optional<std::string> notarize_submit(const std::string&, const std::string&,
                                           const std::string&, const std::string&) {
    return std::nullopt;
}
std::optional<std::string> notarize_submit_asc(const std::string&, const std::string&,
                                               const std::string&, const std::string&) {
    return std::nullopt;
}
NotarizationStatus notarize_check(const std::string&) { return {}; }
NotarizationStatus notarize_check(const std::string&, const std::string&,
                                  const std::string&, const std::string&) {
    return {};
}
NotarizationStatus notarize_check_asc(const std::string&, const std::string&,
                                      const std::string&, const std::string&) {
    return {};
}
bool notarize_staple(const std::string&) { return false; }
std::vector<std::string> list_signing_identities() { return {}; }
bool create_pkg(const std::string&, const std::string&, const std::string&,
                const std::string&, const std::string&) { return false; }
bool create_dmg(const std::string&, const std::string&, const std::string&) { return false; }
bool create_combined_pkg(const std::vector<InstallComponent>&, const std::string&,
                         const std::string&, const std::string&, const std::string&) { return false; }
std::string default_audio_entitlements() { return {}; }

} // namespace pulp::ship

#include <pulp/ship/installer.hpp>

#include <filesystem>
#include <fstream>

namespace pulp::ship {

namespace {

// Every entry of the VST3/CLAP/LV2 directories under build_dir: the set the
// list-less packagers have always shipped.
std::vector<std::string> all_bundles(const std::string& build_dir) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    for (const char* sub : {"VST3", "CLAP", "LV2"}) {
        auto dir = fs::path(build_dir) / sub;
        std::error_code ec;
        if (!fs::exists(dir, ec))
            continue;
        for (auto& entry : fs::directory_iterator(dir))
            out.push_back(entry.path().string());
    }
    return out;
}

// "vst3"/"clap"/"lv2" install suffix for a bundle's format directory, or empty.
std::string install_suffix(const std::filesystem::path& bundle) {
    const auto format = bundle.parent_path().filename().string();
    if (format == "VST3")
        return "vst3";
    if (format == "CLAP")
        return "clap";
    if (format == "LV2")
        return "lv2";
    return {};
}

} // namespace

bool create_deb(const std::string& plugin_name, const std::string& version,
                const std::string& build_dir, const std::string& output_path,
                const std::string& manufacturer, const std::vector<std::string>& bundles) {
    namespace fs = std::filesystem;
    if (bundles.empty())
        return false;
    for (auto& b : bundles)
        if (install_suffix(fs::path(b)).empty())
            return false;

    auto deb_root = fs::path(build_dir) / "deb-staging";
    fs::remove_all(deb_root);
    auto debian_dir = deb_root / "DEBIAN";
    fs::create_directories(debian_dir);

    // Write control file
    {
        std::ofstream ctl(debian_dir / "control");
        if (!ctl) return false;
        ctl << "Package: " << plugin_name << "\n"
            << "Version: " << version << "\n"
            << "Architecture: " << debian_architecture() << "\n"
            << "Maintainer: " << manufacturer << "\n"
            << "Description: " << plugin_name << " audio plugin\n"
            << "Section: sound\n"
            << "Priority: optional\n";
    }

    // Copy exactly the listed bundles to standard Linux locations
    for (auto& b : bundles) {
        fs::path src(b);
        auto dest = deb_root / "usr" / "lib" / install_suffix(src);
        fs::create_directories(dest);
        fs::copy(src, dest / src.filename(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }

    // Build .deb
    std::string cmd = "dpkg-deb --build \"" + deb_root.string() + "\" \"" + output_path + "\"";
    int rc = exec_status(cmd);
    fs::remove_all(deb_root);
    return rc == 0;
}

bool create_deb(const std::string& plugin_name, const std::string& version,
                const std::string& build_dir, const std::string& output_path,
                const std::string& manufacturer) {
    return create_deb(plugin_name, version, build_dir, output_path, manufacturer,
                      all_bundles(build_dir));
}

bool create_tar_gz(const std::string& plugin_name, const std::string& build_dir,
                   const std::string& output_path, const std::vector<std::string>& bundles) {
    (void)plugin_name;
    namespace fs = std::filesystem;
    if (bundles.empty())
        return false;
    std::string cmd = "tar czf \"" + output_path + "\" -C \"" + build_dir + "\"";
    for (auto& b : bundles) {
        fs::path src(b);
        if (install_suffix(src).empty())
            return false;
        cmd += " \"" + (fs::path(src.parent_path().filename()) / src.filename()).string() + "\"";
    }
    return exec_status(cmd) == 0;
}

bool create_tar_gz(const std::string& plugin_name, const std::string& build_dir,
                   const std::string& output_path) {
    return create_tar_gz(plugin_name, build_dir, output_path, all_bundles(build_dir));
}

namespace {

// appimagetool's ARCH value for the architecture this code was compiled for
// (distinct from Debian's naming used by debian_architecture()).
const char* appimage_arch() {
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__arm__)
    return "armhf";
#elif defined(__i386__)
    return "i686";
#else
    return "x86_64";
#endif
}

// A 1×1 transparent PNG (67 bytes). appimagetool requires an icon matching the
// .desktop Icon= key; we ship this placeholder when the caller provides none so
// the build never fails purely for lack of an icon.
const unsigned char kPlaceholderPng[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
    0x89,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x00,0x01,0x00,0x00,
    0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,
    0x42,0x60,0x82};

}  // namespace

bool create_appimage(const std::string& app_name,
                     const std::string& version,
                     const std::string& exe_path,
                     const std::string& output_path,
                     const std::string& icon_path) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // The standalone binary must exist; everything else we synthesize.
    if (app_name.empty() || !fs::exists(exe_path)) return false;

    auto appdir = fs::path(output_path).parent_path() / (app_name + ".AppDir");
    fs::remove_all(appdir, ec);
    auto bindir = appdir / "usr" / "bin";
    fs::create_directories(bindir, ec);
    if (ec) return false;

    // usr/bin/<app> — the standalone executable.
    auto dest_exe = bindir / app_name;
    fs::copy_file(exe_path, dest_exe, fs::copy_options::overwrite_existing, ec);
    if (ec) { fs::remove_all(appdir, ec); return false; }
    fs::permissions(dest_exe,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);

    // AppRun — the entry point AppImage executes; resolves its own dir so the
    // bundle is relocatable.
    {
        std::ofstream apprun(appdir / "AppRun");
        if (!apprun) { fs::remove_all(appdir, ec); return false; }
        apprun << "#!/bin/sh\n"
               << "HERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\n"
               << "exec \"$HERE/usr/bin/" << app_name << "\" \"$@\"\n";
    }
    fs::permissions(appdir / "AppRun",
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);

    // Top-level desktop entry (appimagetool requires one; Icon= must match the
    // icon basename at the AppDir root).
    {
        std::ofstream desktop(appdir / (app_name + ".desktop"));
        if (!desktop) { fs::remove_all(appdir, ec); return false; }
        desktop << "[Desktop Entry]\n"
                << "Type=Application\n"
                << "Name=" << app_name << "\n"
                << "Exec=" << app_name << "\n"
                << "Icon=" << app_name << "\n"
                << "Categories=AudioVideo;Audio;\n"
                << "Terminal=false\n"
                << "Comment=" << app_name << " " << version << "\n";
    }

    // Icon: a caller-supplied .png, else the placeholder. .DirIcon is the
    // AppImage thumbnail; appimagetool also wants <Icon>.png at the root.
    auto root_icon = appdir / (app_name + ".png");
    if (!icon_path.empty() && fs::exists(icon_path)) {
        fs::copy_file(icon_path, root_icon, fs::copy_options::overwrite_existing, ec);
    } else {
        std::ofstream png(root_icon, std::ios::binary);
        png.write(reinterpret_cast<const char*>(kPlaceholderPng), sizeof(kPlaceholderPng));
    }
    fs::copy_file(root_icon, appdir / ".DirIcon", fs::copy_options::overwrite_existing, ec);

    // appimagetool turns the AppDir into a single-file AppImage. Honest-fail
    // when it isn't installed (it is not vendored).
    std::string cmd = "ARCH=" + std::string(appimage_arch()) +
                      " appimagetool --no-appstream \"" + appdir.string() + "\" \"" +
                      output_path + "\"";
    const int rc = exec_status(cmd);
    fs::remove_all(appdir, ec);
    return rc == 0;
}

} // namespace pulp::ship

#endif // __linux__
