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
bool notarize_staple(const std::string&) { return false; }
std::vector<std::string> list_signing_identities() { return {}; }
bool create_pkg(const std::string&, const std::string&, const std::string&,
                const std::string&, const std::string&) { return false; }
bool create_dmg(const std::string&, const std::string&, const std::string&) { return false; }
bool create_combined_pkg(const std::vector<InstallComponent>&, const std::string&,
                         const std::string&, const std::string&, const std::string&) { return false; }
std::string default_audio_entitlements() { return {}; }

} // namespace pulp::ship

#include <filesystem>
#include <fstream>

namespace pulp::ship {

bool create_deb(const std::string& plugin_name,
                const std::string& version,
                const std::string& build_dir,
                const std::string& output_path,
                const std::string& manufacturer) {
    namespace fs = std::filesystem;

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
            << "Architecture: amd64\n"
            << "Maintainer: " << manufacturer << "\n"
            << "Description: " << plugin_name << " audio plugin\n"
            << "Section: sound\n"
            << "Priority: optional\n";
    }

    // Copy plugin bundles to standard Linux locations
    auto copy_dir = [&](const std::string& subdir, const std::string& dest_suffix) {
        auto src = fs::path(build_dir) / subdir;
        if (fs::exists(src)) {
            auto dest = deb_root / "usr" / "lib" / dest_suffix;
            fs::create_directories(dest);
            for (auto& entry : fs::directory_iterator(src))
                fs::copy(entry.path(), dest / entry.path().filename(),
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
    };

    copy_dir("VST3", "vst3");
    copy_dir("CLAP", "clap");
    copy_dir("LV2", "lv2");

    // Build .deb
    std::string cmd = "dpkg-deb --build \"" + deb_root.string() + "\" \"" + output_path + "\"";
    int rc = exec_status(cmd);
    fs::remove_all(deb_root);
    return rc == 0;
}

bool create_tar_gz(const std::string& plugin_name,
                   const std::string& build_dir,
                   const std::string& output_path) {
    namespace fs = std::filesystem;
    std::string cmd = "tar czf \"" + output_path + "\" -C \"" + build_dir + "\"";
    if (fs::exists(fs::path(build_dir) / "VST3")) cmd += " VST3";
    if (fs::exists(fs::path(build_dir) / "CLAP")) cmd += " CLAP";
    if (fs::exists(fs::path(build_dir) / "LV2")) cmd += " LV2";
    return exec_status(cmd) == 0;
}

} // namespace pulp::ship

#endif // __linux__
