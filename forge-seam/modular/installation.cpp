#include "forge/installation.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace forge_modular {

namespace {

std::string trimmed(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

/// The dotted integers of a stamp. "0.12.8" -> {0, 12, 8}. A field that does
/// not begin with a digit stops the parse, so "0.13.0-rc1" reads as {0,13,0}
/// and a release candidate does not sort above the release.
std::vector<long> fields(const std::string& stamp) {
    std::vector<long> out;
    const auto suffix = stamp.find_first_of("-+");
    const auto core = stamp.substr(0, suffix);
    std::size_t at = 0;
    while (at <= core.size()) {
        const auto dot = core.find('.', at);
        const auto part = core.substr(at, dot == std::string::npos
                                               ? std::string::npos : dot - at);
        char* end = nullptr;
        errno = 0;
        const long v = std::strtol(part.c_str(), &end, 10);
        if (end == part.c_str() || *end != '\0' || errno == ERANGE)
            return {};
        out.push_back(v);
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
    return out;
}

std::string prerelease(const std::string& stamp) {
    const auto dash = stamp.find('-');
    if (dash == std::string::npos) return {};
    const auto plus = stamp.find('+', dash + 1);
    return stamp.substr(dash + 1, plus == std::string::npos
                                      ? std::string::npos : plus - dash - 1);
}

std::vector<std::string> prerelease_identifiers(const std::string& stamp) {
    std::vector<std::string> out;
    const auto pre = prerelease(stamp);
    std::string part;
    for (char c : pre) {
        if (c == '.') {
            if (!part.empty()) out.push_back(std::move(part));
            part.clear();
            continue;
        }
        if (!part.empty() && std::isdigit(static_cast<unsigned char>(c)) !=
                                 std::isdigit(static_cast<unsigned char>(part.back()))) {
            out.push_back(std::move(part));
            part.clear();
        }
        part += c;
    }
    if (!part.empty()) out.push_back(std::move(part));
    return out;
}

bool numeric_identifier(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
}

int compare_numeric_identifier(std::string a, std::string b) {
    const auto first_a = a.find_first_not_of('0');
    const auto first_b = b.find_first_not_of('0');
    a = first_a == std::string::npos ? "0" : a.substr(first_a);
    b = first_b == std::string::npos ? "0" : b.substr(first_b);
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    if (a == b) return 0;
    return a < b ? -1 : 1;
}

}  // namespace

int compare_stamps(const std::string& a, const std::string& b) {
    const auto fa = fields(trimmed(a));
    const auto fb = fields(trimmed(b));
    // Unparseable is the oldest thing there is, which is what makes an
    // unstamped directory unable to outrank a release without a special case.
    if (fa.empty() && fb.empty()) return 0;
    if (fa.empty()) return -1;
    if (fb.empty()) return 1;
    for (std::size_t i = 0; i < std::max(fa.size(), fb.size()); ++i) {
        const long x = i < fa.size() ? fa[i] : 0;
        const long y = i < fb.size() ? fb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    const auto pa = prerelease(trimmed(a));
    const auto pb = prerelease(trimmed(b));
    if (pa.empty() && pb.empty()) return 0;
    if (pa.empty()) return 1;
    if (pb.empty()) return -1;
    const auto ia = prerelease_identifiers(trimmed(a));
    const auto ib = prerelease_identifiers(trimmed(b));
    for (std::size_t i = 0; i < std::min(ia.size(), ib.size()); ++i) {
        if (ia[i] == ib[i]) continue;
        const bool na = numeric_identifier(ia[i]);
        const bool nb = numeric_identifier(ib[i]);
        if (na && nb) return compare_numeric_identifier(ia[i], ib[i]);
        if (na != nb) return na ? -1 : 1;
        return ia[i] < ib[i] ? -1 : 1;
    }
    if (ia.size() == ib.size()) return 0;
    return ia.size() < ib.size() ? -1 : 1;
}

std::string stamp_file_name() { return "VERSION"; }

std::pair<std::string, std::string> parse_stamp(const std::string& text) {
    std::istringstream in(text);
    std::string version, packaged;
    std::getline(in, version);
    std::getline(in, packaged);
    return {trimmed(version), trimmed(packaged)};
}

ToolchainPick choose_toolchain(const ToolchainCandidate& override_dir,
                               const ToolchainCandidate& installed,
                               const ToolchainCandidate& bundled,
                               const ToolchainCandidate& checkout) {
    // An explicit choice beats everything. A developer who names a directory
    // means it, and no version arithmetic should be able to argue.
    if (override_dir.usable)
        return {override_dir.path, override_dir.stamp,
                "chosen by FORGE_MODULAR_TOOLS"};

    if (installed.usable && bundled.usable) {
        // Strictly newer, so an equal stamp leaves the installed copy in
        // charge and hand edits there keep running.
        if (compare_stamps(bundled.stamp, installed.stamp) > 0) {
            const std::string had = installed.stamp.empty() ? "unstamped"
                                                            : installed.stamp;
            return {bundled.path, bundled.stamp,
                    "this release (" + bundled.stamp + ") is newer than the "
                    "installed copy (" + had + ")"};
        }
        return {installed.path, installed.stamp, "installed copy"};
    }
    if (installed.usable) return {installed.path, installed.stamp,
                                  "installed copy"};
    if (bundled.usable) return {bundled.path, bundled.stamp,
                                "shipped inside this application"};
    if (checkout.usable)
        return {checkout.path, checkout.stamp, "source checkout"};
    return {"", "", "generator unavailable"};
}

std::string describe_age(std::time_t written, std::time_t now) {
    if (written <= 0) return "never built";
    const auto seconds = now - written;
    if (seconds < 0) return "just now";
    if (seconds < 90) return "just now";
    if (seconds < 90 * 60)
        return std::to_string(seconds / 60) + " minutes ago";
    if (seconds < 36 * 3600)
        return std::to_string(seconds / 3600) + " hours ago";
    const auto days = seconds / (24 * 3600);
    return std::to_string(days) + (days == 1 ? " day ago" : " days ago");
}

std::vector<std::pair<std::string, std::string>> details_rows(
    const AppDetails& d, std::time_t now) {
    std::vector<std::pair<std::string, std::string>> out;
    out.push_back({"Version", d.app_version.empty() ? "unknown"
                                                    : d.app_version});
    if (!d.packaged_at.empty()) out.push_back({"Packaged", d.packaged_at});
    // FIRST among the things that vary, because it is the one that would have
    // made a whole day of shadowed fixes obvious in seconds.
    out.push_back({"Generator in use", d.toolchain_path});
    out.push_back({"Generator version",
                   (d.toolchain_stamp.empty() ? std::string("unstamped")
                                              : d.toolchain_stamp) +
                       " — " + d.toolchain_reason});
    out.push_back({"Library index",
                   d.index_written == 0
                       ? std::string("never built")
                       : std::to_string(d.index_plugins) + " plugins, " +
                             std::to_string(d.index_modules) + " modules, " +
                             describe_age(d.index_written, now)});
    out.push_back({"Rack SDK", d.sdk_path.empty() ? "not installed"
                                                  : d.sdk_path});
    out.push_back({"VCV library sign-in",
                   d.signed_in ? "found" : "not found"});
    return out;
}

std::string details_text(const AppDetails& d, std::time_t now) {
    std::string out;
    for (const auto& [label, value] : details_rows(d, now))
        out += label + ": " + value + "\n";
    return out;
}

}  // namespace forge_modular
