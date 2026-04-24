// findings.rs — Composition rules for the `findings[]` array.
//
// Direct port of `VersionReport::analyze()` in
// `tools/cli/version_diag.cpp`. The rules, their order, and the exact
// message strings must match so the JSON lane is byte-comparable.
//
// Rules (reproduced from the C++ comment block):
//   1. project_cli_min > cli                       -> Warn "Project requires CLI >= ..."
//   1b. plugin_min_cli > cli                       -> Warn "Claude plugin requires ..."
//   2a. project_sdk > cli                          -> Warn "Project SDK is ..."
//   2b. project_sdk <= cli (and comparable)        -> Info "CLI vX is compatible with ..."
//   3. per-project entries:
//       missing_on_disk                            -> Warn "Registered project '...' no longer exists"
//       p.cli_min > cli                            -> Warn "Project '...' requires CLI >= ..."
//       p.sdk > cli                                -> Warn "Project '...' SDK is ..."
//
// The rendered order in the emitted array is: (1), (1b), (2a OR 2b),
// then per-project (missing | cli_min | sdk), in registry order.

use std::cmp::Ordering;

use serde::Serialize;

use crate::projects_registry::Project;
use crate::semver_compat::SemverCompat;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Severity {
    Info,
    Warn,
}

#[derive(Debug, Clone, Serialize)]
pub struct Finding {
    pub severity: Severity,
    pub message: String,
}

/// One registered or ancestor-scanned project's version snapshot.
/// Matches the C++ `ProjectEntry`.
#[derive(Debug, Clone, Default)]
pub struct ProjectEntry {
    pub path: String,
    pub name: String,
    pub sdk: SemverCompat,
    pub cli_min: SemverCompat,
    pub missing_on_disk: bool,
    pub scanned: bool,
}

impl ProjectEntry {
    pub fn from_registry(p: &Project) -> Self {
        let path_buf = std::path::PathBuf::from(&p.path);
        let missing = !path_buf.exists();
        let mut e = ProjectEntry {
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
            // Prefer pulp.toml sdk_version, fall back to CMakeLists.txt VERSION.
            let sdk_raw = crate::pulp_toml::PulpToml::read(&path_buf)
                .and_then(|t| t.sdk_version().map(str::to_string))
                .or_else(|| crate::cmake_version::read(&path_buf))
                .unwrap_or_default();
            e.sdk = SemverCompat::parse(&sdk_raw);

            let cli_min_raw = crate::pulp_toml::PulpToml::read(&path_buf)
                .and_then(|t| t.cli_min_version().map(str::to_string))
                .unwrap_or_default();
            e.cli_min = SemverCompat::parse(&cli_min_raw);
        }
        e
    }
}

/// Mirrors `VersionReport::analyze()`. Not quite identical struct shape
/// — the rendered report pulls these into an enclosing struct; for the
/// analyzer we only need the inputs.
pub struct Inputs<'a> {
    pub cli: &'a SemverCompat,
    pub plugin_min_cli: &'a SemverCompat,
    pub project_sdk: &'a SemverCompat,
    pub project_cli_min: &'a SemverCompat,
    pub projects: &'a [ProjectEntry],
}

pub fn analyze(inputs: &Inputs<'_>) -> Vec<Finding> {
    let mut out = Vec::new();

    // Rule 1: project_cli_min > cli
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

    // Rule 1b: plugin_min_cli > cli
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

    // Rule 2: project_sdk > cli -> Warn, else (comparable) Info.
    if inputs.cli.comparable && inputs.project_sdk.comparable {
        let cmp = inputs.project_sdk.cmp_triple(inputs.cli);
        if cmp == Ordering::Greater {
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
            std::path::PathBuf::from(&p.path)
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
        let pcm = sv("0.20.0");
        let empty = SemverCompat::default();
        let inp = Inputs {
            cli: &cli,
            plugin_min_cli: &empty,
            project_sdk: &empty,
            project_cli_min: &pcm,
            projects: &[],
        };
        let f = analyze(&inp);
        assert!(f.is_empty());
    }

    #[test]
    fn warns_when_plugin_min_cli_exceeds_cli() {
        let cli = sv("0.37.0");
        let pmc = sv("0.38.0");
        let empty = SemverCompat::default();
        let inp = Inputs {
            cli: &cli,
            plugin_min_cli: &pmc,
            project_sdk: &empty,
            project_cli_min: &empty,
            projects: &[],
        };
        let f = analyze(&inp);
        assert_eq!(f.len(), 1);
        assert_eq!(f[0].severity, Severity::Warn);
        assert!(f[0].message.contains("Claude plugin requires"));
    }

    #[test]
    fn info_when_project_sdk_matches_cli() {
        let cli = sv("0.24.0");
        let sdk = sv("0.24.0");
        let empty = SemverCompat::default();
        let inp = Inputs {
            cli: &cli,
            plugin_min_cli: &empty,
            project_sdk: &sdk,
            project_cli_min: &empty,
            projects: &[],
        };
        let f = analyze(&inp);
        assert_eq!(f.len(), 1);
        assert_eq!(f[0].severity, Severity::Info);
    }

    #[test]
    fn skips_silently_when_cli_untagged() {
        let cli = sv("0.22.0-dev");
        let pcm = sv("0.25.0");
        let empty = SemverCompat::default();
        let inp = Inputs {
            cli: &cli,
            plugin_min_cli: &empty,
            project_sdk: &empty,
            project_cli_min: &pcm,
            projects: &[],
        };
        assert!(analyze(&inp).is_empty());
    }

    #[test]
    fn warns_on_missing_on_disk_registered_project() {
        let cli = sv("0.38.0");
        let empty = SemverCompat::default();
        let p = ProjectEntry {
            path: "/tmp/absent".to_string(),
            name: "absent".to_string(),
            missing_on_disk: true,
            ..Default::default()
        };
        let inp = Inputs {
            cli: &cli,
            plugin_min_cli: &empty,
            project_sdk: &empty,
            project_cli_min: &empty,
            projects: std::slice::from_ref(&p),
        };
        let f = analyze(&inp);
        assert_eq!(f.len(), 1);
        assert!(f[0].message.contains("no longer exists"));
    }
}
