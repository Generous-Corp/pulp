// SPDX-License-Identifier: MIT
#include "import_emit_scan.hpp"

#include <algorithm>
#include <cctype>

namespace pulp::cli::import_emit_scan {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

std::vector<std::string> denylist_from_known_frameworks(
    const import_detect::KnownFrameworks& index) {
    std::vector<std::string> tokens;
    for (const auto& fw : index.frameworks) {
        for (const auto& m : fw.detection) {
            // Only content markers are output tells. A file_glob marker is a
            // SOURCE project-layout path pattern, not generated code, so it
            // would only cause false positives on filenames.
            if (m.type != import_detect::MarkerType::ContentMatch) continue;
            if (m.pattern.empty()) continue;
            std::string lc = to_lower(m.pattern);
            if (std::find(tokens.begin(), tokens.end(), lc) == tokens.end())
                tokens.push_back(std::move(lc));
        }
    }
    return tokens;
}

ScanResult scan_manifest(const import_emit::Manifest& manifest,
                         const std::vector<std::string>& denylist) {
    ScanResult result;
    for (const auto& f : manifest.files) {
        // The user's own DSP, copied verbatim — never a clean-room concern.
        if (f.provenance == import_emit::Provenance::CopiedUserFile) {
            ++result.exempt_files;
            continue;
        }
        ++result.scanned_files;
        const std::string haystack = to_lower(f.content);
        for (const auto& token : denylist) {
            if (token.empty()) continue;
            if (haystack.find(token) != std::string::npos) {
                result.clean = false;
                result.hits.push_back({f.path, token});
            }
        }
    }
    return result;
}

}  // namespace pulp::cli::import_emit_scan
