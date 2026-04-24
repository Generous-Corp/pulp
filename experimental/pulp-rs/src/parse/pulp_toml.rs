//! Read `pulp.toml` scalar fields.
//!
//! # Source of truth
//!
//! The C++ side in `cli_common.cpp` uses a hand-rolled line scanner
//! (no real TOML parser) so its test-binary link surface stays small.
//! We use the real `toml` crate because:
//!
//! 1. It's already in `Cargo.toml`.
//! 2. It handles key-boundary edge cases for free — the C++ version
//!    has a dedicated helper just to stop `cli_min_version` from
//!    matching `min_cli_version` via substring search.
//! 3. Real `pulp.toml` files are tiny; throughput doesn't matter here.
//!
//! # Schema
//!
//! Two layouts both accepted:
//!
//! ```toml
//! # Top-level (what `pulp create` writes today).
//! sdk_version = "0.24.0"
//! cli_min_version = "0.22.0"
//! ```
//!
//! ```toml
//! # Under [pulp] section (some C++ test fixtures).
//! [pulp]
//! sdk_version = "0.24.0"
//! cli_min_version = "0.22.0"
//! ```
//!
//! The top-level form wins when both are present.

use std::fs;
use std::path::Path;

use serde::Deserialize;

/// Parsed `pulp.toml` — only the fields the diagnostic cares about.
///
/// Any other key in the file is silently ignored, which is how we
/// tolerate forward-compatible additions (new `[build]`, `[ship]`, …
/// sections) without a parser bump.
#[derive(Debug, Default, Deserialize)]
pub struct PulpToml {
    /// `sdk_version` at the top level.
    #[serde(default)]
    pub sdk_version: Option<String>,

    /// `cli_min_version` at the top level.
    #[serde(default)]
    pub cli_min_version: Option<String>,

    /// `sdk_path` hint — an absolute path to a prebuilt SDK directory
    /// (the one `pulp cache fetch` downloads into, or a local override
    /// the user points at). Checked by `pulp status` on standalone
    /// projects.
    #[serde(default)]
    pub sdk_path: Option<String>,

    /// `sdk_checkout` hint — absolute path to a local SDK source
    /// checkout. Same purpose as `sdk_path` but for when the user is
    /// iterating on the SDK itself rather than consuming a release
    /// build.
    #[serde(default)]
    pub sdk_checkout: Option<String>,

    /// `[pulp]` section overrides (secondary layout).
    #[serde(default)]
    pub pulp: Option<PulpSection>,
}

/// `[pulp]` section fields. Same shape as the top-level forms.
#[derive(Debug, Default, Deserialize)]
pub struct PulpSection {
    /// See [`PulpToml::sdk_version`].
    #[serde(default)]
    pub sdk_version: Option<String>,
    /// See [`PulpToml::cli_min_version`].
    #[serde(default)]
    pub cli_min_version: Option<String>,
    /// See [`PulpToml::sdk_path`].
    #[serde(default)]
    pub sdk_path: Option<String>,
    /// See [`PulpToml::sdk_checkout`].
    #[serde(default)]
    pub sdk_checkout: Option<String>,
}

impl PulpToml {
    /// Read `project_root/pulp.toml`.
    ///
    /// Missing file or malformed TOML both yield `None` — the
    /// diagnostic is never critical, so we never panic here.
    ///
    /// # Examples
    ///
    /// ```
    /// use pulp_rs::parse::PulpToml;
    ///
    /// let dir = tempfile::tempdir().unwrap();
    /// std::fs::write(
    ///     dir.path().join("pulp.toml"),
    ///     "sdk_version = \"0.24.0\"\n",
    /// ).unwrap();
    ///
    /// let t = PulpToml::read(dir.path()).unwrap();
    /// assert_eq!(t.sdk_version(), Some("0.24.0"));
    /// ```
    #[must_use]
    pub fn read(project_root: &Path) -> Option<Self> {
        let body = fs::read_to_string(project_root.join("pulp.toml")).ok()?;
        toml::from_str(&body).ok()
    }

    /// Parse an already-loaded body — handy for benchmarks that want
    /// to isolate parse time from I/O.
    ///
    /// Named `parse_body` rather than implementing `FromStr` because
    /// the `toml::Error` type doesn't match the `FromStr::Err` bound
    /// that the rest of the codebase would want anyway.
    ///
    /// # Errors
    ///
    /// Returns the `toml` crate's parse error verbatim when `body`
    /// is not valid TOML.
    pub fn parse_body(body: &str) -> std::result::Result<Self, toml::de::Error> {
        toml::from_str(body)
    }

    /// Top-level `sdk_version` wins; fall back to `[pulp].sdk_version`.
    #[must_use]
    pub fn sdk_version(&self) -> Option<&str> {
        self.sdk_version
            .as_deref()
            .or_else(|| self.pulp.as_ref().and_then(|p| p.sdk_version.as_deref()))
    }

    /// Top-level `cli_min_version` wins; fall back to
    /// `[pulp].cli_min_version`.
    #[must_use]
    pub fn cli_min_version(&self) -> Option<&str> {
        self.cli_min_version.as_deref().or_else(|| {
            self.pulp
                .as_ref()
                .and_then(|p| p.cli_min_version.as_deref())
        })
    }

    /// Top-level `sdk_path` wins; fall back to `[pulp].sdk_path`.
    #[must_use]
    pub fn sdk_path(&self) -> Option<&str> {
        self.sdk_path
            .as_deref()
            .or_else(|| self.pulp.as_ref().and_then(|p| p.sdk_path.as_deref()))
    }

    /// Top-level `sdk_checkout` wins; fall back to
    /// `[pulp].sdk_checkout`.
    #[must_use]
    pub fn sdk_checkout(&self) -> Option<&str> {
        self.sdk_checkout
            .as_deref()
            .or_else(|| self.pulp.as_ref().and_then(|p| p.sdk_checkout.as_deref()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn write_file(path: &Path, body: &str) {
        let mut f = std::fs::File::create(path).expect("create");
        f.write_all(body.as_bytes()).expect("write");
    }

    #[test]
    fn it_reads_top_level_fields() {
        let td = tempfile::tempdir().unwrap();
        write_file(
            &td.path().join("pulp.toml"),
            "sdk_version = \"0.24.0\"\ncli_min_version = \"0.22.0\"\n",
        );
        let t = PulpToml::read(td.path()).unwrap();
        assert_eq!(t.sdk_version(), Some("0.24.0"));
        assert_eq!(t.cli_min_version(), Some("0.22.0"));
    }

    #[test]
    fn it_reads_pulp_section_fields() {
        let td = tempfile::tempdir().unwrap();
        write_file(
            &td.path().join("pulp.toml"),
            "[pulp]\nsdk_version = \"0.24.0\"\ncli_min_version = \"0.22.0\"\n",
        );
        let t = PulpToml::read(td.path()).unwrap();
        assert_eq!(t.sdk_version(), Some("0.24.0"));
        assert_eq!(t.cli_min_version(), Some("0.22.0"));
    }

    #[test]
    fn it_ignores_commented_values() {
        let td = tempfile::tempdir().unwrap();
        write_file(
            &td.path().join("pulp.toml"),
            "sdk_version = \"0.24.0\"\n# cli_min_version = \"0.22.0\"\n",
        );
        let t = PulpToml::read(td.path()).unwrap();
        assert_eq!(t.sdk_version(), Some("0.24.0"));
        assert_eq!(t.cli_min_version(), None);
    }

    #[test]
    fn it_prefers_top_level_over_section() {
        let td = tempfile::tempdir().unwrap();
        write_file(
            &td.path().join("pulp.toml"),
            "sdk_version = \"0.40.0\"\n[pulp]\nsdk_version = \"0.24.0\"\n",
        );
        let t = PulpToml::read(td.path()).unwrap();
        assert_eq!(t.sdk_version(), Some("0.40.0"));
    }

    #[test]
    fn missing_file_returns_none() {
        let td = tempfile::tempdir().unwrap();
        assert!(PulpToml::read(td.path()).is_none());
    }

    proptest::proptest! {
        // Fuzz the parser with arbitrary strings. Every input must
        // either parse (and sdk_version / cli_min_version are either
        // None or produce stringy output) or fail gracefully — never
        // panic.
        #[test]
        fn never_panics_on_arbitrary_input(input in ".*") {
            let parsed = PulpToml::parse_body(&input);
            if let Ok(t) = parsed {
                // getters must not panic either
                let _ = t.sdk_version();
                let _ = t.cli_min_version();
            }
        }
    }
}
