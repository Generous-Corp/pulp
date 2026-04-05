#include <pulp/ship/codesign.hpp>

#ifdef __APPLE__

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <array>
#include <filesystem>
#include <regex>
#include <sys/wait.h>
#include <unistd.h>

namespace pulp::ship {
namespace fs = std::filesystem;

static std::string exec_cmd(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        result += buf;
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static int exec_status(const std::string& cmd) {
    int ret = system(cmd.c_str());
    if (ret == -1)
        return -1;
    if (WIFEXITED(ret))
        return WEXITSTATUS(ret);
    return ret;
}

static std::optional<fs::path> make_temp_dir(const std::string& prefix) {
    auto base = fs::temp_directory_path() / (prefix + "-XXXXXX");
    std::string templ = base.string();
    if (mkdtemp(templ.data()) == nullptr)
        return std::nullopt;
    return fs::path(templ);
}

static bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(path), ec) && fs::file_size(fs::path(path), ec) > 0;
}

static bool create_dmg_image(const std::string& source_path,
                             const std::string& output_path,
                             const std::string& volume_name) {
    exec_status("rm -f \"" + output_path + "\"");

    std::string cmd = "hdiutil create"
        " -volname \"" + volume_name + "\""
        " -srcfolder \"" + source_path + "\""
        " -ov -format UDZO"
        " \"" + output_path + "\" >/dev/null 2>&1";
    return exec_status(cmd) == 0 && file_exists(output_path);
}

SigningInfo check_codesign(const std::string& path) {
    SigningInfo info;

    // Check signature validity
    int status = exec_status("codesign --verify --deep --strict \"" + path + "\" 2>/dev/null");
    info.is_signed = (status == 0);
    info.is_valid = info.is_signed;

    if (!info.is_signed) {
        info.error = "Not signed or invalid signature";
        return info;
    }

    // Get signing identity
    auto output = exec_cmd("codesign -dvvv \"" + path + "\" 2>&1");
    std::regex id_re("Authority=(.+)");
    std::smatch match;
    if (std::regex_search(output, match, id_re)) {
        info.identity = match[1].str();
    }

    std::regex team_re("TeamIdentifier=(.+)");
    if (std::regex_search(output, match, team_re)) {
        info.team_id = match[1].str();
    }

    // Check notarization via spctl
    int spctl_status = exec_status("spctl --assess --type exec \"" + path + "\" 2>/dev/null");
    info.is_notarized = (spctl_status == 0);

    return info;
}

bool check_notarization(const std::string& path) {
    return exec_status("spctl --assess --type exec \"" + path + "\" 2>/dev/null") == 0;
}

bool codesign(const std::string& path, const std::string& identity,
              const std::string& entitlements) {
    std::string cmd = "codesign --force --sign \"" + identity + "\" --timestamp --options runtime";
    if (!entitlements.empty())
        cmd += " --entitlements \"" + entitlements + "\"";
    cmd += " \"" + path + "\" 2>/dev/null";
    return exec_status(cmd) == 0;
}

std::optional<std::string> notarize_submit(const std::string& path,
                                           const std::string& apple_id,
                                           const std::string& team_id,
                                           const std::string& password) {
    std::string cmd = "xcrun notarytool submit \"" + path + "\""
        + " --apple-id \"" + apple_id + "\""
        + " --team-id \"" + team_id + "\""
        + " --password \"" + password + "\""
        + " --wait 2>&1";

    auto output = exec_cmd(cmd);
    // Extract request UUID
    std::regex uuid_re("id: ([0-9a-f-]+)");
    std::smatch match;
    if (std::regex_search(output, match, uuid_re))
        return match[1].str();

    return std::nullopt;
}

NotarizationStatus notarize_check(const std::string& request_uuid) {
    NotarizationStatus status;
    auto output = exec_cmd("xcrun notarytool info " + request_uuid + " 2>&1");
    status.complete = output.find("Accepted") != std::string::npos
                   || output.find("Invalid") != std::string::npos
                   || output.find("Rejected") != std::string::npos;
    status.success = output.find("Accepted") != std::string::npos;
    status.message = output;
    return status;
}

bool notarize_staple(const std::string& path) {
    return exec_status("xcrun stapler staple \"" + path + "\" 2>/dev/null") == 0;
}

std::vector<std::string> list_signing_identities() {
    std::vector<std::string> identities;
    auto output = exec_cmd("security find-identity -v -p codesigning 2>/dev/null");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        auto start = line.find('"');
        auto end = line.rfind('"');
        if (start != std::string::npos && end > start) {
            identities.push_back(line.substr(start + 1, end - start - 1));
        }
    }
    return identities;
}

bool create_pkg(const std::string& component_path,
                const std::string& output_path,
                const std::string& identifier,
                const std::string& version,
                const std::string& signing_identity) {
    std::string cmd = "pkgbuild --component \"" + component_path + "\""
        + " --identifier \"" + identifier + "\""
        + " --version \"" + version + "\""
        + " --install-location /Library/Audio/Plug-Ins/";

    if (!signing_identity.empty())
        cmd += " --sign \"" + signing_identity + "\"";

    cmd += " \"" + output_path + "\" 2>/dev/null";
    return exec_status(cmd) == 0;
}

bool create_dmg(const std::string& source_path,
                const std::string& output_path,
                const std::string& volume_name) {
    auto staging_dir = make_temp_dir("pulp-dmg-staging");
    if (!staging_dir)
        return create_dmg_image(source_path, output_path, volume_name);

    std::error_code ec;
    auto cleanup = [&]() {
        fs::remove_all(*staging_dir, ec);
    };

    auto staged_source = *staging_dir / fs::path(source_path).filename();
    fs::copy(source_path, staged_source,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (!ec) {
        fs::create_directory_symlink("/Applications", *staging_dir / "Applications", ec);
        if (create_dmg_image(staging_dir->string(), output_path, volume_name)) {
            cleanup();
            return true;
        }
    }

    cleanup();
    return create_dmg_image(source_path, output_path, volume_name);
}

bool create_combined_pkg(const std::vector<InstallComponent>& components,
                         const std::string& output_path,
                         const std::string& identifier,
                         const std::string& version,
                         const std::string& signing_identity) {
    // Build individual component packages, then combine with productbuild
    std::string tmp_dir = "/tmp/pulp-pkg-staging-" + std::to_string(getpid());
    exec_status("mkdir -p \"" + tmp_dir + "\"");

    std::vector<std::string> pkg_paths;
    int idx = 0;
    for (const auto& comp : components) {
        std::string pkg_name = tmp_dir + "/component_" + std::to_string(idx++) + ".pkg";
        std::string cmd = "pkgbuild --component \"" + comp.path + "\""
            " --identifier \"" + identifier + ".c" + std::to_string(idx) + "\""
            " --version \"" + version + "\""
            " --install-location \"" + comp.install_location + "\""
            " \"" + pkg_name + "\" 2>/dev/null";
        if (exec_status(cmd) != 0) {
            exec_status("rm -rf \"" + tmp_dir + "\"");
            return false;
        }
        pkg_paths.push_back(pkg_name);
    }

    // Combine with productbuild
    std::string cmd = "productbuild";
    for (const auto& p : pkg_paths)
        cmd += " --package \"" + p + "\"";
    if (!signing_identity.empty())
        cmd += " --sign \"" + signing_identity + "\"";
    cmd += " \"" + output_path + "\" 2>/dev/null";
    bool ok = exec_status(cmd) == 0;

    exec_status("rm -rf \"" + tmp_dir + "\"");
    return ok;
}

std::string default_audio_entitlements() {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.device.audio-input</key>
    <true/>
    <key>com.apple.security.network.client</key>
    <true/>
</dict>
</plist>
)";
}

} // namespace pulp::ship

#else // !__APPLE__

namespace pulp::ship {
SigningInfo check_codesign(const std::string&) { return {}; }
bool check_notarization(const std::string&) { return false; }
bool codesign(const std::string&, const std::string&, const std::string&) { return false; }
std::optional<std::string> notarize_submit(const std::string&, const std::string&, const std::string&, const std::string&) { return std::nullopt; }
NotarizationStatus notarize_check(const std::string&) { return {}; }
bool notarize_staple(const std::string&) { return false; }
std::vector<std::string> list_signing_identities() { return {}; }
bool create_pkg(const std::string&, const std::string&, const std::string&, const std::string&, const std::string&) { return false; }
bool create_dmg(const std::string&, const std::string&, const std::string&) { return false; }
bool create_combined_pkg(const std::vector<InstallComponent>&, const std::string&, const std::string&, const std::string&, const std::string&) { return false; }
std::string default_audio_entitlements() { return {}; }
}

#endif
