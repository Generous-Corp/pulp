#pragma once

/// @file host_version.hpp
/// Host major/minor/patch version detection on top of `HostType`.
///
/// Some DAW accommodations are version-keyed (Cubase 9 state-blob size
/// quirk differs from Cubase 10 async-resize quirk). `HostVersion`
/// captures the host's reported version string in a comparable form so
/// `host_quirks::make_quirks_for(HostType, HostVersion)` can branch.

#include <pulp/format/host_type.hpp>

#include <optional>
#include <string_view>

namespace pulp::format {

/// Semver-ish host version.
///
/// Pulp doesn't try to parse every DAW's idiosyncratic version string —
/// just enough to distinguish major/minor releases for quirk dispatch.
/// `patch` is best-effort; missing components default to 0.
struct HostVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    constexpr HostVersion() = default;
    constexpr HostVersion(int maj, int min = 0, int pat = 0)
        : major(maj), minor(min), patch(pat) {}

    /// True when this version is >= the comparison target.
    constexpr bool is_at_least(int maj, int min = 0, int pat = 0) const {
        if (major != maj) return major > maj;
        if (minor != min) return minor > min;
        return patch >= pat;
    }

    /// True when this version is strictly less than the comparison target.
    constexpr bool is_before(int maj, int min = 0, int pat = 0) const {
        return !is_at_least(maj, min, pat);
    }

    /// Returns true if all components are zero (unknown / undetected).
    constexpr bool is_unknown() const {
        return major == 0 && minor == 0 && patch == 0;
    }
};

/// Best-effort parse of a version string like "10.0.50", "12.0", or
/// "Pro Tools 2024.6". Returns `std::nullopt` if no leading numeric
/// component is found.
std::optional<HostVersion> parse_host_version(std::string_view version_string);

/// Detect the running host's major/minor version, best-effort.
///
/// On macOS, attempts to read the host process's `Info.plist`
/// (`CFBundleShortVersionString`). On Windows / Linux, attempts the
/// platform's version-resource / `--version` introspection. Returns
/// an empty (zero-valued) `HostVersion` when detection fails — callers
/// should treat that as "unknown" and avoid version-gated quirks.
HostVersion detect_host_version(HostType type);

/// Combined accessor: detect both type and version in one call.
struct HostInfo {
    HostType type = HostType::Unknown;
    HostVersion version;
};

HostInfo detect_host_info();

}  // namespace pulp::format
