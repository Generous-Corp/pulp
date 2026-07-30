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
    if (code == "node-unavailable" || code == "node-incompatible") {
        out << discovery.diagnostic.message << "\n"
            << "Install or update Node.js: " << kNodeDownloadUrl << "\n\n"
            << "Node.js launches the isolated browser-capture helper at import "
               "time only.\n"
            << "It is not embedded in your generated plugin.\n"
            << "Use --offline for the lower-fidelity static/runtime fallback.";
    } else if (code == "capture-runtime-unavailable") {
        out << discovery.diagnostic.message << "\n"
            << "Repair the Pulp installation with: pulp upgrade\n\n"
            << "The versioned browser-capture runtime must be installed beside "
               "pulp-import-design.\n"
            << "Use --offline for the lower-fidelity static/runtime fallback.";
    } else {
        out << discovery.diagnostic.message << "\n"
            << (code == "browser-incompatible"
                    ? "Install or update Google Chrome: "
                    : "Install Google Chrome: ")
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
    } else {
        install_url = kChromeDownloadUrl;
        remediation = code == "browser-incompatible"
            ? "update-browser"
            : "install-browser";
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
        << "\"offline_flag\":\"--offline\","
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
