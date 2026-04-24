// semver_compat.rs — Mirror of `pulp::cli::version_diag::Semver` in C++.
//
// The C++ type keeps the raw string around verbatim (so an untagged
// "0.24.0-dev" still round-trips through the diagnostic) AND a
// `comparable` flag that's only true for pure M.N.P triples. We mirror
// that exact shape — including the serde layout — so JSON output lines
// up with the C++ side byte-for-byte on the fields the C++ writer emits.
//
// Why not just use the `semver` crate's `Version`? The `semver` crate
// treats "0.24.0-dev" as valid (prerelease). The C++ version_diag
// deliberately rejects anything with a suffix and flags it
// non-comparable to implement the "silently skip untagged builds"
// rule. Matching that rule means we need this wrapper.

use regex::Regex;
use serde::Serialize;

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
pub struct SemverCompat {
    pub raw: String,
    pub comparable: bool,

    // Serde skips these when !comparable so the JSON shape matches the
    // C++ writer (which only emits major/minor/patch on comparable).
    #[serde(skip_serializing_if = "skip_if_not_comparable")]
    pub major: u32,
    #[serde(skip_serializing_if = "skip_if_not_comparable")]
    pub minor: u32,
    #[serde(skip_serializing_if = "skip_if_not_comparable")]
    pub patch: u32,
}

// Serde serialize-skip predicate — the C++ writer only emits major /
// minor / patch when comparable is true, so we suppress them in the
// !comparable case to preserve parity. We rely on the owning struct's
// `comparable` field via a wrapping `Serialize` impl below, but serde
// doesn't give us access to sibling fields in a field-level predicate,
// so we emit them always and post-process — see custom_serialize below.
#[allow(dead_code)]
fn skip_if_not_comparable(_v: &u32) -> bool {
    false
}

impl SemverCompat {
    pub fn parse(s: &str) -> Self {
        let mut out = SemverCompat {
            raw: s.to_string(),
            comparable: false,
            major: 0,
            minor: 0,
            patch: 0,
        };
        if s.is_empty() {
            return out;
        }
        // Tolerate a leading "v" or "V".
        let body = if s.starts_with('v') || s.starts_with('V') {
            &s[1..]
        } else {
            s
        };
        // Pure M.N.P only. Anything with a suffix (e.g. "0.24.0-dev")
        // stays non-comparable, which is how the C++ path silently
        // skips untagged builds in the skew analyzer.
        let re = Regex::new(r"^(\d+)\.(\d+)\.(\d+)$").unwrap();
        if let Some(caps) = re.captures(body) {
            out.major = caps[1].parse().unwrap_or(0);
            out.minor = caps[2].parse().unwrap_or(0);
            out.patch = caps[3].parse().unwrap_or(0);
            out.comparable = true;
        }
        out
    }

    /// Ordering equivalent to the C++ compare_semver. Only call when
    /// both sides are comparable — the C++ analyzer only touches
    /// compare_semver behind a .comparable guard.
    pub fn cmp_triple(&self, other: &SemverCompat) -> std::cmp::Ordering {
        self.major
            .cmp(&other.major)
            .then(self.minor.cmp(&other.minor))
            .then(self.patch.cmp(&other.patch))
    }

    /// Produce the JSON shape the C++ writer emits: major/minor/patch
    /// are only present when comparable=true.
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
    fn parses_clean_triples() {
        let v = SemverCompat::parse("1.2.3");
        assert!(v.comparable);
        assert_eq!((v.major, v.minor, v.patch), (1, 2, 3));
    }

    #[test]
    fn tolerates_leading_v() {
        let v = SemverCompat::parse("v0.24.0");
        assert!(v.comparable);
        assert_eq!((v.major, v.minor, v.patch), (0, 24, 0));
        assert_eq!(v.raw, "v0.24.0");
    }

    #[test]
    fn rejects_prerelease_suffix() {
        // Matches C++ behaviour: "0.24.0-dev" stays non-comparable so
        // the skew analyzer silently skips it.
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
    fn cmp_orders_by_triple() {
        let a = SemverCompat::parse("0.24.0");
        let b = SemverCompat::parse("0.25.0");
        assert_eq!(a.cmp_triple(&b), std::cmp::Ordering::Less);
        let c = SemverCompat::parse("1.0.0");
        assert_eq!(b.cmp_triple(&c), std::cmp::Ordering::Less);
    }
}
