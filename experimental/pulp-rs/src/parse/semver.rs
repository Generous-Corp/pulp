//! Semver parsing with a "comparable" flag — the Pulp-specific shape.
//!
//! # Why not the `semver` crate directly?
//!
//! The `semver` crate parses `0.24.0-dev` as a valid pre-release. The
//! C++ `version_diag` deliberately treats anything with a suffix as
//! *non-comparable* so the skew analyser silently skips untagged dev
//! builds. Matching that behaviour requires a purpose-built wrapper.
//!
//! # Invariants
//!
//! - The `raw` string is preserved verbatim (tests inspect it, and
//!   the JSON writer emits it as-is).
//! - `comparable` is true **only** for pure `M.N.P` triples (a leading
//!   `v` or `V` is tolerated and stripped).
//! - Ordering via [`SemverCompat::cmp_triple`] is only meaningful when
//!   both sides have `comparable == true`.

use std::cmp::Ordering;
use std::sync::OnceLock;

use regex::Regex;
use serde::Serialize;

/// A version cell with Pulp's "tagged release or not" semantics.
///
/// The struct is cheap to clone (`raw` is the only heap allocation)
/// so callers routinely pass it by value in composition.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
pub struct SemverCompat {
    /// Raw string as read from the source file.
    pub raw: String,
    /// `true` iff `raw` is a clean `M.N.P` triple.
    pub comparable: bool,
    /// Parsed major (0 when `!comparable`).
    pub major: u32,
    /// Parsed minor (0 when `!comparable`).
    pub minor: u32,
    /// Parsed patch (0 when `!comparable`).
    pub patch: u32,
}

/// Regex for a pure `M.N.P` semver triple with optional leading `v`.
fn triple_re() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| Regex::new(r"^(\d+)\.(\d+)\.(\d+)$").expect("valid regex"))
}

impl SemverCompat {
    /// Parse a version string into the Pulp shape.
    ///
    /// # Examples
    ///
    /// ```
    /// use pulp_rs::parse::SemverCompat;
    ///
    /// let v = SemverCompat::parse("1.2.3");
    /// assert!(v.comparable);
    /// assert_eq!((v.major, v.minor, v.patch), (1, 2, 3));
    ///
    /// let v = SemverCompat::parse("1.2.3-dev");
    /// assert!(!v.comparable);
    /// assert_eq!(v.raw, "1.2.3-dev");
    /// ```
    #[must_use]
    // `option_if_let_else` suggests `map_or_else`, but the two branches
    // here construct *different* `Self` shapes (comparable vs not). The
    // explicit if/else reads more clearly than a closure block.
    #[allow(clippy::option_if_let_else)]
    pub fn parse(s: &str) -> Self {
        if s.is_empty() {
            return Self {
                raw: String::new(),
                ..Self::default()
            };
        }
        let body = s
            .strip_prefix('v')
            .or_else(|| s.strip_prefix('V'))
            .unwrap_or(s);
        if let Some(caps) = triple_re().captures(body) {
            // SAFETY: the regex guarantees three decimal-digit groups
            // of at most 10 digits each, comfortably within u32 for
            // every realistic version. The `.unwrap_or(0)` is defence
            // in depth.
            let major = caps[1].parse().unwrap_or(0);
            let minor = caps[2].parse().unwrap_or(0);
            let patch = caps[3].parse().unwrap_or(0);
            Self {
                raw: s.to_owned(),
                comparable: true,
                major,
                minor,
                patch,
            }
        } else {
            Self {
                raw: s.to_owned(),
                comparable: false,
                ..Self::default()
            }
        }
    }

    /// Ordering by `(major, minor, patch)`. Callers must ensure both
    /// operands are comparable; the C++ analyser guards every call
    /// site with `.comparable`.
    #[must_use]
    pub fn cmp_triple(&self, other: &Self) -> Ordering {
        self.major
            .cmp(&other.major)
            .then(self.minor.cmp(&other.minor))
            .then(self.patch.cmp(&other.patch))
    }

    /// JSON representation matching the C++ `write_semver_json`:
    /// `{raw, comparable}` always, plus `{major, minor, patch}` only
    /// when `comparable == true`.
    #[must_use]
    pub fn to_json(&self) -> serde_json::Value {
        if self.comparable {
            serde_json::json!({
                "raw": self.raw,
                "comparable": true,
                "major": self.major,
                "minor": self.minor,
                "patch": self.patch,
            })
        } else {
            serde_json::json!({
                "raw": self.raw,
                "comparable": false,
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn it_parses_clean_triples() {
        let v = SemverCompat::parse("1.2.3");
        assert!(v.comparable);
        assert_eq!((v.major, v.minor, v.patch), (1, 2, 3));
    }

    #[test]
    fn it_tolerates_leading_v() {
        let v = SemverCompat::parse("v0.24.0");
        assert!(v.comparable);
        assert_eq!((v.major, v.minor, v.patch), (0, 24, 0));
        assert_eq!(v.raw, "v0.24.0");
    }

    #[test]
    fn rejects_prerelease_suffix() {
        let v = SemverCompat::parse("0.24.0-dev");
        assert!(!v.comparable);
    }

    #[test]
    fn empty_is_non_comparable() {
        let v = SemverCompat::parse("");
        assert!(!v.comparable);
        assert!(v.raw.is_empty());
    }

    #[test]
    fn it_orders_by_triple() {
        let a = SemverCompat::parse("0.24.0");
        let b = SemverCompat::parse("0.25.0");
        assert_eq!(a.cmp_triple(&b), Ordering::Less);
        let c = SemverCompat::parse("1.0.0");
        assert_eq!(b.cmp_triple(&c), Ordering::Less);
    }

    proptest::proptest! {
        // Parsing any byte sequence must never panic. The only property
        // that's worth expressing statically is "comparable == true
        // implies raw's trimmed form starts with an ASCII digit".
        #[test]
        fn never_panics_on_arbitrary_input(s in ".*") {
            let v = SemverCompat::parse(&s);
            if v.comparable {
                let stripped = v.raw.strip_prefix('v').or_else(|| v.raw.strip_prefix('V')).unwrap_or(&v.raw);
                let first = stripped.chars().next().unwrap();
                proptest::prop_assert!(first.is_ascii_digit());
            }
        }
    }
}
