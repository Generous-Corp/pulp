//! Version diagnostics — the engine behind `pulp doctor --versions`.
//!
//! # Responsibilities
//!
//! - Discover the active project root (standalone or source-tree).
//! - Snapshot version numbers from `pulp.toml`, `CMakeLists.txt`, and
//!   `.claude-plugin/plugin.json`.
//! - Walk the registered-projects registry.
//! - Compose a `findings[]` list using the same rules as the C++
//!   `VersionReport::analyze()`.
//! - Render the JSON shape the C++ `render_report_json` emits.
//!
//! # Module split
//!
//! ```text
//!   mod.rs       ── VersionDiag struct + collect() orchestrator
//!   findings.rs  ── rule engine (rule 1, 1b, 2a, 2b, 3)
//!   emit.rs      ── JSON writer that matches C++ key order byte-for-byte
//! ```
//!
//! The C++ reference lumps all of this into
//! `tools/cli/version_diag.cpp`; we split so each piece is
//! independently testable and the rule list has a single obvious home.

mod emit;
mod findings;

pub use emit::emit_json;
pub use findings::{analyze, Finding, Inputs, ProjectEntry, Severity};

use std::path::{Path, PathBuf};

use crate::error::Result;
use crate::parse::SemverCompat;
use crate::registry;

/// Fully-resolved diagnostic snapshot.
///
/// Direct counterpart of the C++ `VersionReport` plus the composed
/// `findings[]` list.
#[derive(Debug, Default)]
pub struct VersionDiag {
    /// Installed CLI version (we lie a bit: this is actually
    /// `PULP_RS_CLI_VERSION` env override or the Cargo package
    /// version, since the prototype isn't bound to the C++ `PULP_SDK_VERSION`).
    pub cli: SemverCompat,
    /// Plugin `version` field.
    pub plugin: SemverCompat,
    /// Plugin `min_cli_version` field.
    pub plugin_min_cli: SemverCompat,
    /// Path the plugin JSON was read from (empty if none).
    pub plugin_json_path: PathBuf,
    /// Project root we resolved against.
    pub project_root: PathBuf,
    /// Project SDK version (from `pulp.toml` or `CMakeLists.txt`).
    pub project_sdk: SemverCompat,
    /// Project `cli_min_version` (from `pulp.toml`).
    pub project_cli_min: SemverCompat,
    /// Registered projects (excluding the active one).
    pub projects: Vec<ProjectEntry>,
    /// Composed diagnostic findings.
    pub findings: Vec<Finding>,
}

/// Walk upward from `start` looking for:
///
/// 1. A standalone project root (`pulp.toml` present, no `core/`).
/// 2. A source-tree root (`CMakeLists.txt` present with `core/`).
///
/// Returns `(root, is_standalone)`. Mirrors
/// `resolve_active_project_root` in `version_diag.cpp`.
#[must_use]
pub fn resolve_active_project_root(start: &Path) -> (Option<PathBuf>, bool) {
    let mut dir = Some(start);
    while let Some(d) = dir {
        if d.join("pulp.toml").is_file() && !d.join("core").is_dir() {
            return (Some(d.to_path_buf()), true);
        }
        dir = d.parent();
    }
    let mut dir = Some(start);
    while let Some(d) = dir {
        if d.join("CMakeLists.txt").is_file() && d.join("core").is_dir() {
            return (Some(d.to_path_buf()), false);
        }
        dir = d.parent();
    }
    (None, false)
}

/// Determine the CLI version for reporting purposes.
///
/// Precedence:
/// 1. `PULP_RS_CLI_VERSION` env — only used in tests and benches.
/// 2. `CARGO_PKG_VERSION` — the prototype's own version.
fn cli_version_string() -> String {
    if let Ok(v) = std::env::var("PULP_RS_CLI_VERSION") {
        if !v.is_empty() {
            return v;
        }
    }
    env!("CARGO_PKG_VERSION").to_owned()
}

/// Runtime overrides that let callers inject values that would
/// otherwise come from environment probing.
///
/// The prod entry point [`collect`] constructs this from the
/// environment; tests pass it explicitly to avoid process-wide env
/// mutation (which races badly with concurrent test runs).
#[derive(Debug, Default, Clone)]
pub struct CollectOpts {
    /// Override the CLI version string. Empty = use env / package.
    pub cli_version: Option<String>,
    /// Override the registry path. `None` = use ambient
    /// `registry::registry_path()`.
    pub registry_path: Option<PathBuf>,
}

/// Orchestrator — snapshot every version input under `cwd` and compose
/// findings. Direct port of the `cmd_doctor` side of the C++ code path.
///
/// # Errors
///
/// Returns an error only if the caller's `cwd` lookup fails, which
/// really only happens in a broken process. Every I/O failure inside
/// the read path is handled by returning an empty version string, to
/// match the C++ "diagnostic never critical" rule.
pub fn collect(cwd: &Path) -> Result<VersionDiag> {
    collect_with(cwd, &CollectOpts::default())
}

/// Same as [`collect`] but with explicit overrides for test/bench use.
///
/// # Errors
///
/// See [`collect`].
pub fn collect_with(cwd: &Path, opts: &CollectOpts) -> Result<VersionDiag> {
    let cli_raw = opts.cli_version.clone().unwrap_or_else(cli_version_string);
    let mut d = VersionDiag {
        cli: SemverCompat::parse(&cli_raw),
        ..Default::default()
    };

    let (root, is_standalone) = resolve_active_project_root(cwd);
    if let Some(ref r) = root {
        d.project_root.clone_from(r);

        let sdk_raw = if is_standalone {
            crate::parse::PulpToml::read(r)
                .and_then(|t| t.sdk_version().map(str::to_owned))
                .unwrap_or_default()
        } else {
            crate::parse::cmake::read(r).unwrap_or_default()
        };
        d.project_sdk = SemverCompat::parse(&sdk_raw);

        let cli_min_raw = crate::parse::PulpToml::read(r)
            .and_then(|t| t.cli_min_version().map(str::to_owned))
            .unwrap_or_default();
        d.project_cli_min = SemverCompat::parse(&cli_min_raw);
    }

    // Quirk parity: the C++ cmd_doctor passes an EMPTY repo root to
    // `locate_plugin_json` when the active project is standalone,
    // which skips the in-repo lookup step and falls through to the
    // user-global candidates. Preserved here for byte-equal output.
    let repo_root_for_plugin = if is_standalone { None } else { root.as_deref() };
    let plugin_json =
        crate::parse::plugin_json::locate(repo_root_for_plugin, None).unwrap_or_default();
    if !plugin_json.as_os_str().is_empty() {
        d.plugin_json_path.clone_from(&plugin_json);
        if let Some(pj) = crate::parse::PluginJson::read(&plugin_json) {
            d.plugin = SemverCompat::parse(pj.version.as_deref().unwrap_or_default());
            d.plugin_min_cli =
                SemverCompat::parse(pj.min_cli_version.as_deref().unwrap_or_default());
        }
    }

    // Registered projects (dedup against the active root).
    let reg_path = opts.registry_path.clone().or_else(registry::registry_path);
    if let Some(reg_path) = reg_path {
        let list = registry::read(&reg_path);
        let active_str = root
            .as_ref()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        for p in list {
            if !active_str.is_empty() && paths_equivalent(&p.path, &active_str) {
                continue;
            }
            d.projects.push(ProjectEntry::from_registry(&p));
        }
    }

    d.findings = analyze(&Inputs {
        cli: &d.cli,
        plugin_min_cli: &d.plugin_min_cli,
        project_sdk: &d.project_sdk,
        project_cli_min: &d.project_cli_min,
        projects: &d.projects,
    });

    Ok(d)
}

/// Path equivalence without canonicalisation — just normalise
/// separators so Windows registries written with forward slashes
/// compare equal on a back-slash cwd.
fn paths_equivalent(a: &str, b: &str) -> bool {
    a == b || a.replace('\\', "/") == b.replace('\\', "/")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn write_file(path: &Path, body: &str) {
        let mut f = std::fs::File::create(path).expect("create");
        f.write_all(body.as_bytes()).expect("write");
    }

    fn opts(cli_version: &str, registry_path: PathBuf) -> CollectOpts {
        CollectOpts {
            cli_version: Some(cli_version.to_owned()),
            registry_path: Some(registry_path),
        }
    }

    #[test]
    fn it_reports_standalone_project_when_pulp_toml_present() {
        let td = tempfile::tempdir().unwrap();
        write_file(
            &td.path().join("pulp.toml"),
            "sdk_version = \"0.38.0\"\ncli_min_version = \"0.37.0\"\n",
        );
        let home = tempfile::tempdir().unwrap();
        let reg = home.path().join("projects.json");

        let d = collect_with(td.path(), &opts("0.38.0", reg)).unwrap();
        assert!(d.project_sdk.comparable);
        assert_eq!(d.project_sdk.raw, "0.38.0");
        assert_eq!(d.project_cli_min.raw, "0.37.0");
        let infos = d
            .findings
            .iter()
            .filter(|f| f.severity == Severity::Info)
            .count();
        assert_eq!(infos, 1);
    }

    #[test]
    fn it_warns_when_cli_min_ahead_of_cli() {
        let td = tempfile::tempdir().unwrap();
        write_file(
            &td.path().join("pulp.toml"),
            "sdk_version = \"0.40.0\"\ncli_min_version = \"0.40.0\"\n",
        );
        let home = tempfile::tempdir().unwrap();
        let reg = home.path().join("projects.json");

        let d = collect_with(td.path(), &opts("0.37.0", reg)).unwrap();
        let warns = d
            .findings
            .iter()
            .filter(|f| f.severity == Severity::Warn)
            .count();
        assert!(warns >= 2);
    }
}
