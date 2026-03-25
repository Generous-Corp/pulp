// Linux packaging stubs for Pulp
// API surface for .deb and AppImage creation

#include <pulp/ship/codesign.hpp>

#if defined(__linux__)

#include <cstdlib>
#include <cstdio>

namespace pulp::ship {

static int exec_status(const std::string& cmd) {
    return WEXITSTATUS(std::system(cmd.c_str()));
}

// Linux has no code signing equivalent
SigningInfo check_codesign(const std::string&) { return {}; }
bool check_notarization(const std::string&) { return false; }
bool codesign(const std::string&, const std::string&, const std::string&) { return false; }
std::optional<std::string> notarize_submit(const std::string&, const std::string&,
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

#endif // __linux__
