#include <pulp/ship/codesign.hpp>
#include <pulp/platform/child_process.hpp>

#ifdef __APPLE__

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <array>
#include <filesystem>
#include <iostream>
#include <regex>
#include <sys/wait.h>
#include <unistd.h>

namespace pulp::ship {
namespace fs = std::filesystem;

namespace detail {

SigningInfo parse_codesign_details(const std::string& output) {
    SigningInfo info;
    std::regex id_re("Authority=(.+)");
    std::smatch match;
    if (std::regex_search(output, match, id_re))
        info.identity = match[1].str();

    std::regex team_re("TeamIdentifier=(.+)");
    if (std::regex_search(output, match, team_re))
        info.team_id = match[1].str();
    return info;
}

std::optional<std::string> parse_notarytool_submit_id(const std::string& output) {
    std::regex uuid_re(
        R"((^|\n)\s*id:\s+([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})(?:\s|$))");
    std::smatch match;
    if (std::regex_search(output, match, uuid_re))
        return match[2].str();
    return std::nullopt;
}

NotarizationStatus parse_notarytool_status(const std::string& output) {
    NotarizationStatus status;
    status.complete = output.find("Accepted") != std::string::npos
                   || output.find("Invalid") != std::string::npos
                   || output.find("Rejected") != std::string::npos;
    status.success = output.find("Accepted") != std::string::npos;
    status.message = output;
    return status;
}

std::vector<std::string> parse_signing_identities(const std::string& output) {
    std::vector<std::string> identities;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        auto start = line.find('"');
        auto end = line.rfind('"');
        if (start != std::string::npos && end > start)
            identities.push_back(line.substr(start + 1, end - start - 1));
    }
    return identities;
}

std::string disk_image_failure_hint(const std::string& output) {
    // `hdiutil create -srcfolder` builds the image by attaching a temporary volume,
    // copying into it, and unmounting it. Any DiskArbitration client may veto that
    // unmount, and hdiutil then reports the whole operation as "Resource busy" with
    // no clue as to who vetoed it. The veto lasts until that client is restarted, so
    // retrying never clears it. `diskutil unmount` on any mounted volume names the
    // offender ("Dissenter ..."); CoreSimulator's simdiskimaged is a common one.
    if (output.find("Resource busy") == std::string::npos
        && output.find("Device busy") == std::string::npos)
        return {};
    return "a process is vetoing volume unmounts machine-wide, so hdiutil cannot "
           "release the temporary volume it built. `diskutil unmount` on any mounted "
           "image names the dissenter; if it is CoreSimulator's simdiskimaged, "
           "restart it with `sudo killall -9 simdiskimaged`.";
}

} // namespace detail

static std::string exec_cmd(const std::string& cmd, int timeout_ms = 120000) {
    auto r = pulp::platform::exec("/bin/sh", {"-c", cmd}, timeout_ms);
    auto result = r.stdout_output;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static int exec_status(const std::string& cmd, int timeout_ms = 120000) {
    auto r = pulp::platform::exec("/bin/sh", {"-c", cmd}, timeout_ms);
    return r.exit_code;
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

namespace detail {

// On failure, `error` receives what hdiutil printed. Discarding it — as this used
// to — turns every DMG failure into a bare `false` and leaves the caller with
// nothing to say.
bool create_dmg_image(const std::string& source_path,
                      const std::string& output_path,
                      const std::string& volume_name,
                      std::string& error) {
    exec_status("rm -f \"" + output_path + "\"");

    std::string cmd = "hdiutil create"
        " -volname \"" + volume_name + "\""
        " -srcfolder \"" + source_path + "\""
        " -ov -format UDZO"
        " \"" + output_path + "\" 2>&1";
    auto r = pulp::platform::exec("/bin/sh", {"-c", cmd}, 120000);
    if (r.exit_code == 0 && file_exists(output_path))
        return true;

    error = r.stdout_output + r.stderr_output;
    return false;
}

} // namespace detail

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
    auto parsed = detail::parse_codesign_details(output);
    info.identity = std::move(parsed.identity);
    info.team_id = std::move(parsed.team_id);

    // Check notarization via spctl
    int spctl_status = exec_status("spctl --assess --type exec \"" + path + "\" 2>/dev/null");
    info.is_notarized = (spctl_status == 0);

    return info;
}

bool check_notarization(const std::string& path) {
    // A path that doesn't exist cannot be notarized — short-circuit before
    // consulting spctl. CI base images commonly disable Gatekeeper assessment
    // (`spctl --master-disable`), in which case `spctl --assess` returns 0 for
    // ANY argument (even a nonexistent path), so it can't be the sole signal.
    // (Surfaced by the Tart VM CI lane: the cirruslabs macOS bases ship with
    // assessment disabled, unlike a stock Mac where assess fails on a bad path.)
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    return exec_status("spctl --assess --type exec \"" + path + "\" 2>/dev/null") == 0;
}

bool codesign(const std::string& path, const std::string& identity,
              const std::string& entitlements) {
    std::string cmd = "codesign --force --sign \"" + identity + "\" --timestamp --options runtime";
    if (!entitlements.empty())
        cmd += " --entitlements \"" + entitlements + "\"";
    cmd += " \"" + path + "\"";
    auto r = pulp::platform::exec("/bin/sh", {"-c", cmd}, 120000);
    if (r.exit_code == 0) return true;

    auto output = r.stdout_output;
    if (!r.stderr_output.empty()) {
        if (!output.empty() && output.back() != '\n') output += '\n';
        output += r.stderr_output;
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    if (!output.empty())
        std::cerr << output << "\n";
    if (output.find("errSecInternalComponent") != std::string::npos) {
        std::cerr
            << "codesign: the signing identity was found, but this process "
               "could not access the private key.\n"
            << "  Configure the signing keychain partition list for Apple "
               "tools, or import the Developer ID certificate into a dedicated "
               "automation keychain.\n";
    }
    return false;
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

    // Notarization can take 5-15 minutes — use 20 min timeout
    auto output = exec_cmd(cmd, 1200000);
    return detail::parse_notarytool_submit_id(output);
}

std::optional<std::string> notarize_submit_asc(const std::string& path,
                                                const std::string& key_path,
                                                const std::string& key_id,
                                                const std::string& issuer_id) {
    // App Store Connect API key flow — Apple's preferred path. Note
    // notarytool reads the .p8 directly off disk; we never pass the
    // key contents on the command line.
    std::string cmd = "xcrun notarytool submit \"" + path + "\""
        + " --key \"" + key_path + "\""
        + " --key-id \"" + key_id + "\""
        + " --issuer \"" + issuer_id + "\""
        + " --wait 2>&1";
    auto output = exec_cmd(cmd, 1200000);
    return detail::parse_notarytool_submit_id(output);
}

NotarizationStatus notarize_check(const std::string& request_uuid) {
    auto output = exec_cmd("xcrun notarytool info " + request_uuid + " 2>&1");
    return detail::parse_notarytool_status(output);
}

NotarizationStatus notarize_check(const std::string& request_uuid,
                                  const std::string& apple_id,
                                  const std::string& team_id,
                                  const std::string& password) {
    auto output = exec_cmd("xcrun notarytool info " + request_uuid
        + " --apple-id \"" + apple_id + "\""
        + " --team-id \"" + team_id + "\""
        + " --password \"" + password + "\""
        + " 2>&1");
    return detail::parse_notarytool_status(output);
}

NotarizationStatus notarize_check_asc(const std::string& request_uuid,
                                      const std::string& key_path,
                                      const std::string& key_id,
                                      const std::string& issuer_id) {
    auto output = exec_cmd("xcrun notarytool info " + request_uuid
        + " --key \"" + key_path + "\""
        + " --key-id \"" + key_id + "\""
        + " --issuer \"" + issuer_id + "\""
        + " 2>&1");
    return detail::parse_notarytool_status(output);
}

bool notarize_staple(const std::string& path) {
    return exec_status("xcrun stapler staple \"" + path + "\" 2>/dev/null") == 0;
}

std::vector<std::string> list_signing_identities() {
    auto output = exec_cmd("security find-identity -v -p codesigning 2>/dev/null");
    return detail::parse_signing_identities(output);
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

// Reports on the last attempt only: the staged attempt failing while the direct one
// succeeds is a normal outcome, not something to warn about.
static void report_dmg_failure(const std::string& error) {
    std::cerr << "pulp: hdiutil create failed";
    if (!error.empty()) std::cerr << ": " << error;
    std::cerr << std::endl;
    const auto hint = detail::disk_image_failure_hint(error);
    if (!hint.empty()) std::cerr << "pulp: " << hint << std::endl;
}

bool create_dmg(const std::string& source_path,
                const std::string& output_path,
                const std::string& volume_name) {
    std::string error;
    auto direct = [&]() {
        error.clear();
        if (detail::create_dmg_image(source_path, output_path, volume_name, error))
            return true;
        report_dmg_failure(error);
        return false;
    };

    auto staging_dir = make_temp_dir("pulp-dmg-staging");
    if (!staging_dir)
        return direct();

    std::error_code ec;
    auto cleanup = [&]() {
        fs::remove_all(*staging_dir, ec);
    };

    auto staged_source = *staging_dir / fs::path(source_path).filename();
    fs::copy(source_path, staged_source,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (!ec) {
        fs::create_directory_symlink("/Applications", *staging_dir / "Applications", ec);
        if (detail::create_dmg_image(staging_dir->string(), output_path, volume_name, error)) {
            cleanup();
            return true;
        }
    }

    cleanup();
    return direct();
}

// Derive a human choice title from an install location when none was given,
// e.g. ".../Plug-Ins/VST3" -> "VST3", ".../Components" -> "Audio Unit".
static std::string derive_choice_title(const std::string& install_location,
                                       const std::string& path) {
    auto ends_with = [](const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (ends_with(install_location, "VST3")) return "VST3";
    if (ends_with(install_location, "Components")) return "Audio Unit (AU)";
    if (ends_with(install_location, "CLAP")) return "CLAP";
    if (ends_with(install_location, "Applications") || ends_with(path, ".app")) return "Application";
    // Fallback: last path component of the install location.
    auto slash = install_location.find_last_of('/');
    return slash == std::string::npos ? install_location : install_location.substr(slash + 1);
}

// Minimal XML escaping for titles embedded in the distribution document.
static std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

bool create_combined_pkg(const std::vector<InstallComponent>& components,
                         const std::string& output_path,
                         const std::string& identifier,
                         const std::string& version,
                         const std::string& signing_identity) {
    if (components.empty()) return false;

    // Build one component package per format, then combine them with a
    // distribution document that exposes each as a user-selectable choice
    // (the "Customize" pane). This is Pulp's default installer shape — a user
    // can install only the formats they want.
    //
    // Stage in an mkdtemp() directory (unique, 0700, creation checked) rather
    // than a predictable /tmp/...-<pid> path, so a concurrent build or a
    // pre-existing path can't collide or be pre-seeded.
    auto staging = make_temp_dir("pulp-pkg-staging");
    if (!staging) return false;
    const std::string tmp_dir = staging->string();

    std::string choices_outline;   // <line choice="..."/>
    std::string choice_defs;       // <choice .../> with nested <pkg-ref/>
    std::string pkg_refs;          // top-level <pkg-ref ...>file</pkg-ref>

    int idx = 0;
    for (const auto& comp : components) {
        const int n = ++idx;
        const std::string pkg_id = identifier + ".c" + std::to_string(n);
        const std::string pkg_file = "component_" + std::to_string(n) + ".pkg";
        const std::string pkg_path = tmp_dir + "/" + pkg_file;
        const std::string choice_id = "choice" + std::to_string(n);
        const std::string title = comp.title.empty()
            ? derive_choice_title(comp.install_location, comp.path)
            : comp.title;

        std::string cmd = "pkgbuild --component \"" + comp.path + "\""
            " --identifier \"" + pkg_id + "\""
            " --version \"" + version + "\""
            " --install-location \"" + comp.install_location + "\""
            " \"" + pkg_path + "\" 2>/dev/null";
        if (exec_status(cmd) != 0) {
            exec_status("rm -rf \"" + tmp_dir + "\"");
            return false;
        }

        choices_outline += "    <line choice=\"" + choice_id + "\"/>\n";
        choice_defs +=
            "  <choice id=\"" + choice_id + "\" title=\"" + xml_escape(title) +
            "\" visible=\"true\" start_selected=\"true\">\n"
            "    <pkg-ref id=\"" + pkg_id + "\"/>\n"
            "  </choice>\n";
        pkg_refs +=
            "  <pkg-ref id=\"" + pkg_id + "\" version=\"" + version +
            "\" onConclusion=\"none\">" + pkg_file + "</pkg-ref>\n";
    }

    const std::string dist =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<installer-gui-script minSpecVersion=\"2\">\n"
        "  <title>" + xml_escape(identifier) + "</title>\n"
        "  <options customize=\"allow\" require-scripts=\"false\""
        " hostArchitectures=\"arm64,x86_64\"/>\n"
        "  <choices-outline>\n" + choices_outline + "  </choices-outline>\n" +
        choice_defs + pkg_refs +
        "</installer-gui-script>\n";

    const std::string dist_path = tmp_dir + "/distribution.xml";
    {
        std::ofstream f(dist_path);
        if (!f) { exec_status("rm -rf \"" + tmp_dir + "\""); return false; }
        f << dist;
    }

    std::string cmd = "productbuild --distribution \"" + dist_path + "\""
        " --package-path \"" + tmp_dir + "\"";
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
std::optional<std::string> notarize_submit_asc(const std::string&, const std::string&, const std::string&, const std::string&) { return std::nullopt; }
NotarizationStatus notarize_check(const std::string&) { return {}; }
NotarizationStatus notarize_check(const std::string&, const std::string&, const std::string&, const std::string&) { return {}; }
NotarizationStatus notarize_check_asc(const std::string&, const std::string&, const std::string&, const std::string&) { return {}; }
bool notarize_staple(const std::string&) { return false; }
std::vector<std::string> list_signing_identities() { return {}; }
bool create_pkg(const std::string&, const std::string&, const std::string&, const std::string&, const std::string&) { return false; }
bool create_dmg(const std::string&, const std::string&, const std::string&) { return false; }
bool create_combined_pkg(const std::vector<InstallComponent>&, const std::string&, const std::string&, const std::string&, const std::string&) { return false; }
std::string default_audio_entitlements() { return {}; }
}

#endif
