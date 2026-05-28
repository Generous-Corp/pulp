// Windows code signing stubs for Pulp
// Uses signtool.exe when available

#include <pulp/ship/codesign.hpp>
#include <pulp/platform/child_process.hpp>

#ifdef _WIN32

#include <cstdlib>
#include <cstdio>
#include <sstream>

namespace pulp::ship {

static std::string exec_cmd(const std::string& cmd) {
    auto r = pulp::platform::exec("cmd", {"/c", cmd}, 120000);
    auto result = r.stdout_output;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static int exec_status(const std::string& cmd) {
    auto r = pulp::platform::exec("cmd", {"/c", cmd}, 120000);
    return r.exit_code;
}

SigningInfo check_codesign(const std::string& path) {
    SigningInfo info;
    // Use signtool verify
    int status = exec_status("signtool verify /pa \"" + path + "\" >nul 2>&1");
    info.is_signed = (status == 0);
    info.is_valid = info.is_signed;
    if (!info.is_signed) info.error = "Not signed or signtool not available";
    return info;
}

bool check_notarization(const std::string&) { return false; }

bool codesign(const std::string& path, const std::string& identity,
              const std::string& entitlements) {
    (void)entitlements; // Not used on Windows
    std::string cmd = "signtool sign /n \"" + identity + "\" /t http://timestamp.digicert.com"
        " /fd sha256 \"" + path + "\" >nul 2>&1";
    return exec_status(cmd) == 0;
}

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

std::vector<std::string> list_signing_identities() {
    // List certificates in the Windows certificate store
    std::vector<std::string> identities;
    auto output = exec_cmd("certutil -store My 2>nul");
    // Parse certificate names (simplified)
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("Subject:") != std::string::npos) {
            auto pos = line.find("CN=");
            if (pos != std::string::npos) {
                identities.push_back(line.substr(pos + 3));
            }
        }
    }
    return identities;
}

bool create_pkg(const std::string&, const std::string&, const std::string&,
                const std::string&, const std::string&) {
    return false; // macOS only
}

bool create_dmg(const std::string&, const std::string&, const std::string&) {
    return false; // macOS only
}

bool create_combined_pkg(const std::vector<InstallComponent>&, const std::string&,
                         const std::string&, const std::string&, const std::string&) {
    return false; // macOS only
}

std::string default_audio_entitlements() { return {}; }

} // namespace pulp::ship

#endif // _WIN32
