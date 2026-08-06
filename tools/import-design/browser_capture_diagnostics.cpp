// SPDX-License-Identifier: MIT
#include "browser_capture_backend.hpp"
#include "browser_capture_diagnostics.hpp"

#include <sstream>

namespace pulp::import_design::browser_capture {

namespace {

constexpr std::string_view kChromeDownloadUrl =
    "https://www.google.com/chrome/";
constexpr std::string_view kNodeDownloadUrl =
    "https://nodejs.org/en/download";

}  // namespace

namespace detail {

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0xf]);
                    out.push_back(kHex[c & 0xf]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

}  // namespace detail

std::string browser_unavailable_human(
    const BrowserDiscoveryResult& discovery) {
    std::ostringstream out;
    const auto& code = discovery.diagnostic.code;
    const bool node_failure =
        code == "node-unavailable" || code == "node-incompatible";
    if (node_failure) {
        out << discovery.diagnostic.message << "\n"
            << "Install or update Node.js: " << kNodeDownloadUrl << "\n";
        if (code == "node-unavailable") {
            out << "\n"
                << "An app launched from Finder or Explorer inherits a minimal "
                   "PATH, so a Node.js\n"
                << "installed by Homebrew or a version manager is invisible "
                   "through PATH alone. Pulp\n"
                << "also looks in the standard install locations directly; "
                   "none of them held a\n"
                << "Node.js either.\n";
        }
        out << "\n"
            << "Node.js launches the isolated browser-capture helper at import "
               "time only.\n"
            << "It is not embedded in your generated plugin.\n"
            << "Use --offline for the lower-fidelity static/runtime fallback.";
        const auto report = node_search_report(discovery.node);
        if (!report.empty()) out << "\n\n" << report;
        return out.str();
    }
    if (code == "capture-runtime-unavailable") {
        out << discovery.diagnostic.message << "\n"
            << "Repair the Pulp installation with: pulp upgrade\n\n"
            << "The versioned browser-capture runtime must be installed beside "
               "pulp-import-design.\n"
            << "Use --offline for the lower-fidelity static/runtime fallback.";
    } else if (code == "managed-browser-unavailable") {
        out << discovery.diagnostic.message << "\n"
            << "Install the pinned browser with: "
               "pulp tool install chrome-for-testing\n"
            << "Or select an existing installation with: "
               "pulp config set import_design.browser system\n\n"
            << "Pulp never downloads a browser during import.";
    } else if (code == "browser-mode-invalid") {
        out << discovery.diagnostic.message << "\n"
            << "Choose one of: auto, managed, system.\n"
            << "Example: pulp config set import_design.browser auto";
    } else if (code == "browser-version-unreadable") {
        // Reading the version has been seen to fail transiently and then
        // succeed on the same browser, so the first advice is to retry, not to
        // replace a browser that may be perfectly fine.
        out << discovery.diagnostic.message << "\n"
            << "Retry the import; the probe already retried once.\n"
            << "If it persists, run the browser's --version by hand to see "
               "what it reports,\n"
            << "or select a known-good installation with: "
               "pulp import-design --browser <path>\n\n"
            << "Use --offline for the lower-fidelity static/runtime fallback.";
    } else {
        out << discovery.diagnostic.message << "\n"
            << "Install Pulp's pinned browser with: "
               "pulp tool install chrome-for-testing\n"
            << "Or "
            << (code == "browser-incompatible"
                    ? "install or update system Google Chrome: "
                    : "install system Google Chrome: ")
            << kChromeDownloadUrl << "\n\n"
            << "Pulp launches it with a temporary isolated profile to evaluate "
               "layout and make\n"
            << "the reference image. Chrome is not embedded in your generated "
               "plugin.\n"
            << "Use --offline for the lower-fidelity static/runtime fallback.";
    }
    if (!discovery.probes.empty()) {
        out << "\n\nChecked:";
        for (const auto& probe : discovery.probes) {
            out << "\n  " << browser_origin_name(probe.candidate.origin)
                << ": " << probe.candidate.executable.string()
                << " — "
                << (probe.failure.empty() ? "incompatible" : probe.failure);
        }
    }
    return out.str();
}

std::string browser_unavailable_json(
    const BrowserDiscoveryResult& discovery) {
    const auto& code = discovery.diagnostic.code;
    std::string_view install_url;
    std::string_view remediation = "install-browser";
    if (code == "node-unavailable" || code == "node-incompatible") {
        install_url = kNodeDownloadUrl;
        remediation = "install-node-22";
    } else if (code == "capture-runtime-unavailable") {
        remediation = "pulp-upgrade";
    } else if (code == "managed-browser-unavailable") {
        remediation = "pulp-tool-install-chrome-for-testing";
    } else if (code == "browser-mode-invalid") {
        remediation = "set-browser-mode";
    } else if (code == "browser-version-unreadable") {
        remediation = "retry-or-select-browser";
    } else {
        install_url = kChromeDownloadUrl;
        remediation = code == "browser-incompatible"
            ? "install-managed-or-update-system-browser"
            : "install-managed-or-system-browser";
    }
    std::ostringstream out;
    out << "{"
        << "\"code\":\""
        << detail::json_escape(
               code.empty() ? "browser-unavailable" : code)
        << "\","
        << "\"message\":\""
        << detail::json_escape(
               discovery.diagnostic.message.empty()
                   ? "No compatible Google Chrome or Chromium installation "
                     "was found."
                   : discovery.diagnostic.message)
        << "\","
        << "\"install_url\":\"" << install_url << "\","
        << "\"remediation\":\"" << remediation << "\","
        << "\"managed_install_command\":\""
        << ((code == "browser-unavailable" ||
             code == "browser-incompatible" ||
             code == "managed-browser-unavailable")
                ? "pulp tool install chrome-for-testing"
                : "")
        << "\","
        << "\"system_mode_command\":\""
        << (code == "managed-browser-unavailable"
                ? "pulp config set import_design.browser system"
                : "")
        << "\","
        << "\"offline_flag\":\"--offline\","
        << "\"node\":{"
        << "\"path\":\""
        << detail::json_escape(
               discovery.node.executable
                   ? discovery.node.executable->string()
                   : std::string{})
        << "\","
        << "\"version\":\"" << detail::json_escape(discovery.node.version)
        << "\","
        << "\"source\":\"" << detail::json_escape(discovery.node.source)
        << "\","
        << "\"searched\":[";
    for (std::size_t i = 0; i < discovery.node.searched.size(); ++i) {
        if (i != 0) out << ",";
        const auto& location = discovery.node.searched[i];
        out << "{\"source\":\"" << detail::json_escape(location.source)
            << "\",\"location\":\""
            << detail::json_escape(location.location) << "\"}";
    }
    out << "],\"attempts\":[";
    for (std::size_t i = 0; i < discovery.node.attempts.size(); ++i) {
        if (i != 0) out << ",";
        const auto& attempt = discovery.node.attempts[i];
        out << "{\"source\":\"" << detail::json_escape(attempt.source)
            << "\",\"path\":\""
            << detail::json_escape(attempt.executable.string())
            << "\",\"version\":\"" << detail::json_escape(attempt.version)
            << "\",\"failure\":\"" << detail::json_escape(attempt.failure)
            << "\"}";
    }
    out << "]},"
        << "\"probes\":[";
    for (std::size_t i = 0; i < discovery.probes.size(); ++i) {
        if (i != 0) out << ",";
        const auto& probe = discovery.probes[i];
        out << "{"
            << "\"origin\":\""
            << browser_origin_name(probe.candidate.origin) << "\","
            << "\"path\":\""
            << detail::json_escape(probe.candidate.executable.string())
            << "\","
            << "\"compatible\":"
            << (probe.compatible ? "true" : "false") << ","
            << "\"product\":\"" << detail::json_escape(probe.product)
            << "\","
            << "\"version\":\"" << detail::json_escape(probe.version)
            << "\","
            << "\"failure\":\"" << detail::json_escape(probe.failure)
            << "\""
            << "}";
    }
    out << "]}";
    return out.str();
}

}  // namespace pulp::import_design::browser_capture
