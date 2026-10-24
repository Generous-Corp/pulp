// release_checksum.hpp — verify a downloaded Pulp release archive against
// the release's SHA256SUMS manifest before anything extracts it.
//
// The release workflow publishes one `SHA256SUMS` asset per GitHub Release,
// in `shasum -a 256` format, covering every archive on that release. The
// CLI download paths (`pulp upgrade`, `pulp sdk install`) fetch that
// manifest next to the archive and refuse to extract an archive whose
// digest differs from its manifest entry. The manifest is verified whenever
// it is present; the floor in tools/scripts/release_product_matrix.json
// governs only absence. It names the first release whose publish step
// guaranteed the manifest, so at or above it a missing manifest is refused,
// while an older manifest-less release installs with a loud warning.
//
// Header-only so tests can drive the verifier without the CLI's link deps;
// the only dependency is pulp::runtime's streaming SHA-256.

#pragma once

#include <pulp/runtime/crypto.hpp>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::cli::release_checksum {

namespace fs = std::filesystem;

inline constexpr std::string_view kManifestAssetName = "SHA256SUMS";

// Largest archive the verifier hashes. Release archives are tens of MB;
// the bound keeps a hostile download from pinning the hasher forever.
inline constexpr std::uint64_t kMaxArchiveBytes = std::uint64_t{4} << 30;

inline std::string lowercase_hex(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline bool is_hex_digest(std::string_view s) {
    if (s.size() != 64)
        return false;
    for (unsigned char c : s)
        if (!std::isxdigit(c))
            return false;
    return true;
}

struct ManifestLookup {
    std::string digest; // lowercase hex when found
    std::string error;  // non-empty when the manifest cannot vouch for the asset
    bool found() const {
        return !digest.empty() && error.empty();
    }
};

// Find the manifest entry whose filename equals `asset` exactly. A malformed
// line, a duplicate entry, or a missing entry is an error: the manifest must
// vouch for exactly one set of bytes under that name.
inline ManifestLookup manifest_digest_for(std::string_view manifest, std::string_view asset) {
    ManifestLookup out;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= manifest.size()) {
        const auto end = manifest.find('\n', pos);
        std::string_view line = manifest.substr(
            pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? manifest.size() + 1 : end + 1;
        ++line_no;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.empty())
            continue;
        // "<64 hex>  <name>" or "<64 hex> *<name>" (binary-mode marker).
        if (line.size() < 64 + 2 + 1 || !is_hex_digest(line.substr(0, 64)) || line[64] != ' ' ||
            (line[65] != ' ' && line[65] != '*')) {
            out.error = "malformed SHA256SUMS line " + std::to_string(line_no);
            out.digest.clear();
            return out;
        }
        const std::string_view name = line.substr(66);
        if (name != asset)
            continue;
        if (!out.digest.empty()) {
            out.error = "SHA256SUMS lists " + std::string(asset) + " more than once";
            out.digest.clear();
            return out;
        }
        out.digest = lowercase_hex(line.substr(0, 64));
    }
    if (out.digest.empty())
        out.error = "SHA256SUMS has no entry for " + std::string(asset);
    return out;
}

// True when `version` (M.m.p, optional leading v, optional -pre suffix) is at
// or above `floor`. Unparsable input counts as at-or-above so a malformed
// version never unlocks the legacy path.
inline bool version_at_or_above(std::string_view version, std::string_view floor) {
    auto parse = [](std::string_view s, long (&parts)[3]) {
        if (!s.empty() && (s.front() == 'v' || s.front() == 'V'))
            s.remove_prefix(1);
        if (auto dash = s.find('-'); dash != std::string_view::npos)
            s = s.substr(0, dash);
        int idx = 0;
        std::size_t pos = 0;
        while (idx < 3) {
            auto dot = s.find('.', pos);
            std::string_view tok =
                s.substr(pos, dot == std::string_view::npos ? std::string_view::npos : dot - pos);
            if (tok.empty())
                return false;
            long value = 0;
            for (unsigned char c : tok) {
                if (!std::isdigit(c))
                    return false;
                value = value * 10 + (c - '0');
            }
            parts[idx++] = value;
            if (dot == std::string_view::npos)
                break;
            pos = dot + 1;
        }
        return idx == 3;
    };
    long v[3] = {0, 0, 0};
    long f[3] = {0, 0, 0};
    if (!parse(version, v) || !parse(floor, f))
        return true;
    for (int i = 0; i < 3; ++i) {
        if (v[i] != f[i])
            return v[i] > f[i];
    }
    return true;
}

enum class Status {
    verified,          // digest matches the manifest entry
    refused,           // do not extract; `message` says why
    unverified_legacy, // pre-floor release without a manifest; caller warns loudly
};

struct Outcome {
    Status status = Status::refused;
    std::string digest;  // observed lowercase hex (verified) or empty
    std::string message; // user-facing explanation
    bool ok() const {
        return status != Status::refused;
    }
};

// Decide whether `archive` (published as `asset` on release `version`) may be
// extracted. `manifest` is the SHA256SUMS text fetched for that release, or
// nullopt when it could not be fetched.
inline Outcome verify_release_archive(const fs::path& archive, std::string_view asset,
                                      std::string_view version, std::string_view floor,
                                      const std::optional<std::string>& manifest) {
    Outcome out;
    const std::string where = "release v" + std::string(version);
    if (!manifest) {
        if (version_at_or_above(version, floor)) {
            out.status = Status::refused;
            out.message = "could not fetch SHA256SUMS for " + where +
                          " (the release is missing it, or the download failed); releases from v" +
                          std::string(floor) + " onward must carry one, so " + std::string(asset) +
                          " was not extracted";
            return out;
        }
        out.status = Status::unverified_legacy;
        out.message = where + " predates the SHA256SUMS manifest (floor v" + std::string(floor) +
                      "); " + std::string(asset) + " was NOT verified before extraction";
        return out;
    }
    const auto lookup = manifest_digest_for(*manifest, asset);
    if (!lookup.found()) {
        out.status = Status::refused;
        out.message =
            lookup.error + " on " + where + "; " + std::string(asset) + " was not extracted";
        return out;
    }
    const auto observed = pulp::runtime::sha256_file_hex(archive, kMaxArchiveBytes);
    if (!observed) {
        out.status = Status::refused;
        out.message = "could not hash " + archive.string() +
                      " (unreadable or larger than the verifier bound)";
        return out;
    }
    const std::string observed_hex = lowercase_hex(*observed);
    if (observed_hex != lookup.digest) {
        out.status = Status::refused;
        out.message = "SHA-256 mismatch for " + std::string(asset) + " on " + where +
                      ": expected " + lookup.digest + ", got " + observed_hex +
                      "; the archive was not extracted";
        return out;
    }
    out.status = Status::verified;
    out.digest = observed_hex;
    out.message = "Verified SHA-256: " + observed_hex;
    return out;
}

} // namespace pulp::cli::release_checksum
