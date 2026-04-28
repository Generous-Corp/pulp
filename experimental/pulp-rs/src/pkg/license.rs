//! License-policy helpers — mirrors `package_registry.cpp::check_license`.
//!
//! # Policy
//!
//! The C++ CLI classifies every SPDX identifier as one of:
//!
//! - **`allowed`** — safe to use without prompt.
//! - **`review_required`** — MPL-2.0 and any not-explicitly-allowed id.
//! - **`rejected`** — GPL-family + AGPL + SSPL + proprietary.
//!
//! Rejected splits into two tiers:
//!
//! - `restricted` — copyleft (GPL/LGPL/AGPL). Overridable via
//!   `pulp add --accept-license <SPDX>`.
//! - `rejected` — SSPL / `proprietary`. Non-overridable.
//!
//! The case-insensitive match on the allow-list and the prefix rule on
//! the reject-list come straight from the C++ implementation.

/// Classification buckets mirrored from the C++ `LicenseVerdict` enum.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LicenseVerdict {
    /// Compatible with the MIT-licensed Pulp core.
    Allowed,
    /// Manual review required before use.
    ReviewRequired,
    /// Incompatible with MIT distribution.
    Rejected,
}

impl LicenseVerdict {
    /// Human-readable tag matching `license_verdict_label()` in the C++
    /// reference.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Self::Allowed => "allowed",
            Self::ReviewRequired => "review required",
            Self::Rejected => "rejected",
        }
    }
}

/// Classify an SPDX identifier.
///
/// Case-insensitive match on the allow-list, prefix match on the
/// reject-list, and SSPL / `proprietary` fall through to `Rejected`.
#[must_use]
pub fn check(spdx_id: &str) -> LicenseVerdict {
    const ALLOWED: &[&str] = &[
        "MIT",
        "MIT-0",
        "BSD-2-Clause",
        "BSD-3-Clause",
        "Apache-2.0",
        "ISC",
        "zlib",
        "BSL-1.0",
        "Unlicense",
        "CC0-1.0",
    ];
    const REJECTED_PREFIXES: &[&str] = &["GPL", "LGPL", "AGPL", "SSPL"];

    let lower = spdx_id.to_ascii_lowercase();

    for a in ALLOWED {
        if a.eq_ignore_ascii_case(spdx_id) {
            return LicenseVerdict::Allowed;
        }
    }
    for p in REJECTED_PREFIXES {
        if lower.starts_with(&p.to_ascii_lowercase()) {
            return LicenseVerdict::Rejected;
        }
    }
    if lower == "mpl-2.0" {
        return LicenseVerdict::ReviewRequired;
    }
    if lower == "proprietary" {
        return LicenseVerdict::Rejected;
    }
    LicenseVerdict::ReviewRequired
}

/// Sub-tier of a rejected verdict so the `pulp add` UX can split
/// copyleft (overridable) from `SSPL`/`proprietary` (non-overridable).
///
/// Returns one of:
///
/// - `"allowed"` / `"review"` for non-rejected ids.
/// - `"rejected"` for SSPL / `proprietary` — non-overridable.
/// - `"restricted"` for every other rejected id (copyleft family).
#[must_use]
pub fn tier(spdx_id: &str) -> &'static str {
    match check(spdx_id) {
        LicenseVerdict::Allowed => "allowed",
        LicenseVerdict::ReviewRequired => "review",
        LicenseVerdict::Rejected => {
            let lower = spdx_id.to_ascii_lowercase();
            if lower == "sspl-1.0" || lower == "proprietary" {
                "rejected"
            } else {
                "restricted"
            }
        }
    }
}

/// Long-form explanation used when `pulp add` refuses a copyleft
/// package. Strings are copied verbatim from the C++
/// `license_explanation()` helper so parity tests can grep them.
#[must_use]
pub fn explanation(spdx_id: &str) -> &'static str {
    let lower = spdx_id.to_ascii_lowercase();
    if lower.starts_with("agpl") {
        return "AGPL requires that if you distribute software linking this library, \
or provide it as a network service, the complete source of your \
application must be available under AGPL. This affects YOUR plugin, \
not Pulp itself.";
    }
    if lower.starts_with("gpl") {
        return "GPL requires that software you distribute which links this library \
also be distributed under GPL. This affects YOUR plugin binary, \
not Pulp itself. If your plugin is GPL-licensed, this is fine.";
    }
    if lower.starts_with("lgpl") {
        return "LGPL allows use via dynamic linking without affecting your license. \
Static linking requires your code to be LGPL-compatible. For audio \
plugins, dynamic linking is complex — consult your legal advisor.";
    }
    if lower == "mpl-2.0" {
        return "MPL-2.0 is file-level copyleft. Modifications to MPL-licensed files \
must stay MPL, but your own code remains under your chosen license.";
    }
    "This license may have compatibility implications. Review before distributing."
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allow_list_is_case_insensitive() {
        assert_eq!(check("MIT"), LicenseVerdict::Allowed);
        assert_eq!(check("mit"), LicenseVerdict::Allowed);
        assert_eq!(check("Apache-2.0"), LicenseVerdict::Allowed);
        assert_eq!(check("apache-2.0"), LicenseVerdict::Allowed);
    }

    #[test]
    fn gpl_family_rejected_by_prefix() {
        assert_eq!(check("GPL-2.0"), LicenseVerdict::Rejected);
        assert_eq!(check("GPL-3.0"), LicenseVerdict::Rejected);
        assert_eq!(check("LGPL-2.1"), LicenseVerdict::Rejected);
        assert_eq!(check("AGPL-3.0"), LicenseVerdict::Rejected);
        assert_eq!(check("SSPL-1.0"), LicenseVerdict::Rejected);
    }

    #[test]
    fn mpl_requires_review() {
        assert_eq!(check("MPL-2.0"), LicenseVerdict::ReviewRequired);
    }

    #[test]
    fn proprietary_is_rejected() {
        assert_eq!(check("proprietary"), LicenseVerdict::Rejected);
    }

    #[test]
    fn unknown_falls_through_to_review() {
        assert_eq!(check("CUSTOM-LICENSE-42"), LicenseVerdict::ReviewRequired);
    }

    #[test]
    fn tier_splits_copyleft_from_sspl() {
        assert_eq!(tier("GPL-3.0"), "restricted");
        assert_eq!(tier("AGPL-3.0"), "restricted");
        assert_eq!(tier("LGPL-2.1"), "restricted");
        assert_eq!(tier("SSPL-1.0"), "rejected");
        assert_eq!(tier("proprietary"), "rejected");
        assert_eq!(tier("MIT"), "allowed");
        assert_eq!(tier("MPL-2.0"), "review");
    }

    #[test]
    fn explanation_selects_family_by_prefix() {
        assert!(explanation("AGPL-3.0").contains("AGPL"));
        assert!(explanation("GPL-3.0").contains("GPL"));
        assert!(explanation("LGPL-2.1").contains("LGPL"));
        assert!(explanation("MPL-2.0").contains("file-level"));
        assert!(explanation("CUSTOM").contains("Review before"));
    }
}
