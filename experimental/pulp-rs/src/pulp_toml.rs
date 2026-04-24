// pulp_toml.rs — Read `pulp.toml` scalar values (sdk_version,
// cli_min_version).
//
// The C++ side uses a hand-rolled line scanner (not a real TOML parser)
// with a key-boundary check to avoid substring-matching keys and a
// `#` comment strip. We use the real `toml` crate here because:
//   1. it's already in Cargo.toml;
//   2. a real parser handles the key-boundary case for free;
//   3. the file is tiny so there's no perf concern.
//
// We return a string for each field, keeping parse-to-Semver in the
// caller to match the C++ code path exactly (the C++ version also
// reads raw → parse_semver).

use std::fs;
use std::path::Path;

use serde::Deserialize;

/// Pulp's standalone-project `pulp.toml` shape — we only care about a
/// couple of scalar fields under `[pulp]` and at the top level. Future
/// forward-compatible additions don't need this struct — anything we
/// don't read gets ignored by #[serde(deny_unknown_fields = false)].
#[derive(Debug, Default, Deserialize)]
pub struct PulpToml {
    // The real project template puts these at top level. Some examples
    // in the C++ tests put them under `[pulp]`. Handle both layouts.
    #[serde(default)]
    pub sdk_version: Option<String>,
    #[serde(default)]
    pub cli_min_version: Option<String>,

    #[serde(default)]
    pub pulp: Option<PulpSection>,
}

#[derive(Debug, Default, Deserialize)]
pub struct PulpSection {
    #[serde(default)]
    pub sdk_version: Option<String>,
    #[serde(default)]
    pub cli_min_version: Option<String>,
}

impl PulpToml {
    pub fn read(project_root: &Path) -> Option<Self> {
        let path = project_root.join("pulp.toml");
        let body = fs::read_to_string(&path).ok()?;
        toml::from_str(&body).ok()
    }

    /// Prefer top-level `sdk_version` over `[pulp] sdk_version` —
    /// matches the cmd_create.cpp writer output which puts it at the
    /// top level.
    pub fn sdk_version(&self) -> Option<&str> {
        self.sdk_version
            .as_deref()
            .or_else(|| self.pulp.as_ref().and_then(|p| p.sdk_version.as_deref()))
    }

    pub fn cli_min_version(&self) -> Option<&str> {
        self.cli_min_version
            .as_deref()
            .or_else(|| {
                self.pulp
                    .as_ref()
                    .and_then(|p| p.cli_min_version.as_deref())
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn write(path: &Path, body: &str) {
        let mut f = std::fs::File::create(path).expect("create");
        f.write_all(body.as_bytes()).expect("write");
    }

    #[test]
    fn reads_top_level_fields() {
        let td = tempfile::tempdir().unwrap();
        write(
            &td.path().join("pulp.toml"),
            "sdk_version = \"0.24.0\"\ncli_min_version = \"0.22.0\"\n",
        );
        let t = PulpToml::read(td.path()).unwrap();
        assert_eq!(t.sdk_version(), Some("0.24.0"));
        assert_eq!(t.cli_min_version(), Some("0.22.0"));
    }

    #[test]
    fn reads_pulp_section_fields() {
        let td = tempfile::tempdir().unwrap();
        write(
            &td.path().join("pulp.toml"),
            "[pulp]\nsdk_version = \"0.24.0\"\ncli_min_version = \"0.22.0\"\n",
        );
        let t = PulpToml::read(td.path()).unwrap();
        assert_eq!(t.sdk_version(), Some("0.24.0"));
        assert_eq!(t.cli_min_version(), Some("0.22.0"));
    }

    #[test]
    fn ignores_commented_values() {
        // The real TOML parser strips comments natively, so this
        // exercises the same "commented example" case the C++ tests
        // worry about.
        let td = tempfile::tempdir().unwrap();
        write(
            &td.path().join("pulp.toml"),
            "sdk_version = \"0.24.0\"\n# cli_min_version = \"0.22.0\"\n",
        );
        let t = PulpToml::read(td.path()).unwrap();
        assert_eq!(t.sdk_version(), Some("0.24.0"));
        assert_eq!(t.cli_min_version(), None);
    }

    #[test]
    fn missing_file_returns_none() {
        let td = tempfile::tempdir().unwrap();
        assert!(PulpToml::read(td.path()).is_none());
    }
}
