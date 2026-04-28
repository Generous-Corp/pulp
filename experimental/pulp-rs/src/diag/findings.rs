//! Rule engine for the `findings[]` array.
//!
//! # Source of truth
//!
//! Direct port of `VersionReport::analyze()` in
//! `tools/cli/version_diag.cpp`. The rules, their order, and the exact
//! message strings match so the JSON lane is byte-comparable.
//!
//! # Rules (reproduced from the C++ comment block)
//!
//! | # | Condition                                        | Severity | Message shape                                         |
//! |---|--------------------------------------------------|----------|-------------------------------------------------------|
//! | 1  | `project_cli_min > cli`                          | Warn     | `"Project requires CLI >= v{}..."`                    |
//! | 1b | `plugin_min_cli > cli`                           | Warn     | `"Claude plugin requires CLI >= v{}..."`              |
//! | 2a | `project_sdk > cli`                              | Warn     | `"Project SDK is v{} but installed CLI is v{}..."`    |
//! | 2b | `project_sdk <= cli` (and both comparable)       | Info     | `"CLI v{} is compatible with project SDK v{}"`        |
//! | 3  | per registered project: missing / `cli_min` / sdk  | Warn     | (three distinct shapes)                               |
//!
//! The emitted order is `1, 1b, (2a|2b), then [3] in registry order`.

use std::cmp::Ordering;
use std::path::PathBuf;

use serde::Serialize;

use crate::parse::SemverCompat;
use crate::registry::Project;

/// Severity tag carried by every [`Finding`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Severity {
    /// "everything lines up" observation.
    Info,
    /// Actionable mismatch.
    Warn,
}

/// One diagnostic finding. The JSON writer emits these in
/// `{severity, message}` order.
#[derive(Debug, Clone, Serialize)]
pub struct Finding {
    /// Severity tag.
    pub severity: Severity,
    /// User-facing message.
    pub message: String,
}

/// One registered-or-ancestor project's version snapshot. Matches
/// `ProjectEntry` in the C++ `version_diag.cpp`.
#[derive(Debug, Clone, Default)]
pub struct ProjectEntry {
    /// Absolute path as it appears in the registry.
    pub path: String,
    /// Display name (basename fallback).
    pub name: String,
    /// Project SDK version (`sdk_version` or `CMakeLists.txt VERSION`).
    pub sdk: SemverCompat,
    /// Project CLI minimum (`cli_min_version` in `pulp.toml`).
    pub cli_min: SemverCompat,
    /// True iff the path no longer exists on disk.
    pub missing_on_disk: bool,
    /// Sentinel for future `--scan-parents` mode — always `false` today.
    pub scanned: bool,
}

impl ProjectEntry {
    /// Derive a [`ProjectEntry`] from a registry [`Project`]. Reads
    /// `pulp.toml` / `CMakeLists.txt` from the path to populate
    /// version fields when the directory still exists.
    #[must_use]
    pub fn from_registry(p: &Project) -> Self {
        let path_buf = PathBuf::from(&p.path);
        let missing = !path_buf.exists();
        let mut e = Self {
            path: p.path.clone(),
            name: if p.name.is_empty() {
                path_buf
                    .file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default()
            } else {
                p.name.clone()
            },
            sdk: SemverCompat::default(),
            cli_min: SemverCompat::default(),
            missing_on_disk: missing,
            scanned: false,
        };
        if !missing {
            let sdk_raw = crate::parse::PulpToml::read(&path_buf)
                .and_then(|t| t.sdk_version().map(str::to_owned))
                .or_else(|| crate::parse::cmake::read(&path_buf))
                .unwrap_or_default();
            e.sdk = SemverCompat::parse(&sdk_raw);

            let cli_min_raw = crate::parse::PulpToml::read(&path_buf)
                .and_then(|t| t.cli_min_version().map(str::to_owned))
                .unwrap_or_default();
            e.cli_min = SemverCompat::parse(&cli_min_raw);
        }
        e
    }
}

/// Input bundle for [`analyze`] — every field is borrowed so the
/// caller can hand the function a transient view of its own state
/// struct.
pub struct Inputs<'a> {
    /// Installed CLI version.
    pub cli: &'a SemverCompat,
    /// Plugin `min_cli_version` (empty `SemverCompat` if absent).
    pub plugin_min_cli: &'a SemverCompat,
    /// Project SDK version.
    pub project_sdk: &'a SemverCompat,
    /// Project `cli_min_version`.
    pub project_cli_min: &'a SemverCompat,
    /// Registered project entries (active root already de-duplicated).
    pub projects: &'a [ProjectEntry],
}

/// Apply the rule engine; return the composed `findings` list.
#[must_use]
pub fn analyze(inputs: &Inputs<'_>) -> Vec<Finding> {
    let mut out = Vec::new();

    // Rule 1: project_cli_min > cli.
    if inputs.cli.comparable
        && inputs.project_cli_min.comparable
        && inputs.project_cli_min.cmp_triple(inputs.cli) == Ordering::Greater
    {
        out.push(Finding {
            severity: Severity::Warn,
            message: format!(
                "Project requires CLI >= v{} but installed CLI is v{} — run `pulp upgrade`",
                inputs.project_cli_min.raw, inputs.cli.raw
            ),
        });
    }

    // Rule 1b: plugin_min_cli > cli.
    if inputs.cli.comparable
        && inputs.plugin_min_cli.comparable
        && inputs.plugin_min_cli.cmp_triple(inputs.cli) == Ordering::Greater
    {
        out.push(Finding {
            severity: Severity::Warn,
            message: format!(
                "Claude plugin requires CLI >= v{} but installed CLI is v{} — run `pulp upgrade`",
                inputs.plugin_min_cli.raw, inputs.cli.raw
            ),
        });
    }

    // Rule 2: project_sdk > cli → Warn, else (comparable) → Info.
    if inputs.cli.comparable && inputs.project_sdk.comparable {
        if inputs.project_sdk.cmp_triple(inputs.cli) == Ordering::Greater {
            out.push(Finding {
                severity: Severity::Warn,
                message: format!(
                    "Project SDK is v{} but installed CLI is v{} — consider `pulp upgrade`",
                    inputs.project_sdk.raw, inputs.cli.raw
                ),
            });
        } else {
            out.push(Finding {
                severity: Severity::Info,
                message: format!(
                    "CLI v{} is compatible with project SDK v{}",
                    inputs.cli.raw, inputs.project_sdk.raw
                ),
            });
        }
    }

    // Rule 3: per-project entries.
    for p in inputs.projects {
        let label = if p.name.is_empty() {
            PathBuf::from(&p.path)
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default()
        } else {
            p.name.clone()
        };

        if p.missing_on_disk {
            out.push(Finding {
                severity: Severity::Warn,
                message: format!(
                    "Registered project '{}' at {} no longer exists — run `pulp projects remove {}` to forget it",
                    label, p.path, p.path
                ),
            });
            continue;
        }

        if inputs.cli.comparable
            && p.cli_min.comparable
            && p.cli_min.cmp_triple(inputs.cli) == Ordering::Greater
        {
            out.push(Finding {
                severity: Severity::Warn,
                message: format!(
                    "Project '{}' requires CLI >= v{} but installed CLI is v{} — run `pulp upgrade`",
                    label, p.cli_min.raw, inputs.cli.raw
                ),
            });
        }
        if inputs.cli.comparable
            && p.sdk.comparable
            && p.sdk.cmp_triple(inputs.cli) == Ordering::Greater
        {
            out.push(Finding {
                severity: Severity::Warn,
                message: format!(
                    "Project '{}' SDK is v{} but installed CLI is v{} — consider `pulp upgrade`",
                    label, p.sdk.raw, inputs.cli.raw
                ),
            });
        }
    }

    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sv(s: &str) -> SemverCompat {
        SemverCompat::parse(s)
    }

    fn empty_inputs(cli: &SemverCompat) -> Inputs<'_> {
        static EMPTY: std::sync::OnceLock<SemverCompat> = std::sync::OnceLock::new();
        let e = EMPTY.get_or_init(SemverCompat::default);
        Inputs {
            cli,
            plugin_min_cli: e,
            project_sdk: e,
            project_cli_min: e,
            projects: &[],
        }
    }

    #[test]
    fn warns_when_cli_min_exceeds_cli() {
        let cli = sv("0.22.0");
        let pcm = sv("0.24.0");
        let empty = SemverCompat::default();
        let inp = Inputs {
            cli: &cli,
            plugin_min_cli: &empty,
            project_sdk: &empty,
            project_cli_min: &pcm,
            projects: &[],
        };
        let f = analyze(&inp);
        assert_eq!(f.len(), 1);
        assert_eq!(f[0].severity, Severity::Warn);
        assert!(f[0].message.contains("Project requires CLI >= v0.24.0"));
    }

    #[test]
    fn silent_when_cli_min_satisfied() {
        let cli = sv("0.22.0");
        let mut inp = empty_inputs(&cli);
        let pcm = sv("0.20.0");
        inp.project_cli_min = &pcm;
        assert!(analyze(&inp).is_empty());
    }

    #[test]
    fn warns_when_plugin_min_cli_exceeds_cli() {
        let cli = sv("0.37.0");
        let pmc = sv("0.38.0");
        let mut inp = empty_inputs(&cli);
        inp.plugin_min_cli = &pmc;
        let f = analyze(&inp);
        assert_eq!(f.len(), 1);
        assert!(f[0].message.contains("Claude plugin requires"));
    }

    #[test]
    fn info_when_project_sdk_matches_cli() {
        let cli = sv("0.24.0");
        let sdk = sv("0.24.0");
        let mut inp = empty_inputs(&cli);
        inp.project_sdk = &sdk;
        let f = analyze(&inp);
        assert_eq!(f.len(), 1);
        assert_eq!(f[0].severity, Severity::Info);
    }

    #[test]
    fn skips_silently_when_cli_untagged() {
        let cli = sv("0.22.0-dev");
        let pcm = sv("0.25.0");
        let mut inp = empty_inputs(&cli);
        inp.project_cli_min = &pcm;
        assert!(analyze(&inp).is_empty());
    }

    #[test]
    fn warns_on_missing_on_disk_registered_project() {
        let cli = sv("0.38.0");
        let p = ProjectEntry {
            path: "/tmp/absent".to_owned(),
            name: "absent".to_owned(),
            missing_on_disk: true,
            ..Default::default()
        };
        let mut inp = empty_inputs(&cli);
        inp.projects = std::slice::from_ref(&p);
        let f = analyze(&inp);
        assert_eq!(f.len(), 1);
        assert!(f[0].message.contains("no longer exists"));
    }
}
