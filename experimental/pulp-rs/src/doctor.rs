// doctor.rs — `pulp doctor --versions --json` port.
//
// Phase 2: real output. Mirrors the C++ path:
//   cmd_doctor.cpp (orchestration)  ->  Rust doctor::run/collect
//   version_diag.cpp (analyze+JSON) ->  Rust findings + emit_json
//
// Design choices for parity:
//   - Keys emitted in the exact order the C++ writer uses:
//       cli, plugin, plugin_min_cli, plugin_json_path, project_root,
//       project_sdk, project_cli_min, projects, findings.
//   - Semver value shape matches `write_semver_json`: {raw, comparable}
//     plus {major, minor, patch} only when comparable.
//   - Projects entry shape matches `render_report_json` projects[]:
//       {path, name, sdk, cli_min, missing_on_disk, scanned}.
//
// Divergences from the C++:
//   - We accept a PULP_RS_CLI_VERSION env override for testing (the
//     C++ side uses a compile-time `PULP_SDK_VERSION` macro). In normal
//     use we fall back to the built-in version, which for Phase 2 is
//     just the Cargo package version.
//   - The `project_root` / `plugin_json_path` are rendered as
//     forward-slash strings to match `generic_string()` on the C++ side.

use std::path::{Path, PathBuf};

use anyhow::Result;
use serde_json::{json, Value};

use crate::findings::{analyze, Finding, Inputs, ProjectEntry};
use crate::projects_registry;
use crate::semver_compat::SemverCompat;

/// The fully-resolved diagnostic snapshot. Mirrors C++ `VersionReport`
/// plus the composed `findings[]` list so the caller can emit either
/// the JSON lane or (future) a human-readable one.
#[derive(Debug, Default)]
pub struct VersionDiag {
    pub cli: SemverCompat,
    pub plugin: SemverCompat,
    pub plugin_min_cli: SemverCompat,
    pub plugin_json_path: PathBuf,
    pub project_root: PathBuf,
    pub project_sdk: SemverCompat,
    pub project_cli_min: SemverCompat,
    pub projects: Vec<ProjectEntry>,
    pub findings: Vec<Finding>,
}

/// Walk upward from `start` looking for either a standalone-project
/// root (pulp.toml without core/) or a source-tree root (CMakeLists.txt
/// with core/). Returns `(root, is_standalone)`. Mirrors the two
/// helpers `find_standalone_root` + `find_project_root` + chaining
/// `resolve_active_project_root` on the C++ side.
pub fn resolve_active_project_root(start: &Path) -> (Option<PathBuf>, bool) {
    let mut dir: Option<&Path> = Some(start);
    // Pass 1 — standalone: pulp.toml + NO core/
    while let Some(d) = dir {
        if d.join("pulp.toml").is_file() && !d.join("core").is_dir() {
            return (Some(d.to_path_buf()), true);
        }
        dir = d.parent();
    }
    // Pass 2 — source-tree: CMakeLists.txt + core/
    let mut dir: Option<&Path> = Some(start);
    while let Some(d) = dir {
        if d.join("CMakeLists.txt").is_file() && d.join("core").is_dir() {
            return (Some(d.to_path_buf()), false);
        }
        dir = d.parent();
    }
    (None, false)
}

/// Return the CLI version the diagnostic should report on.
fn cli_version_string() -> String {
    if let Ok(v) = std::env::var("PULP_RS_CLI_VERSION") {
        if !v.is_empty() {
            return v;
        }
    }
    // Use our own package version by default — this is a prototype, not
    // wired to PULP_SDK_VERSION. Parity tests override via env.
    env!("CARGO_PKG_VERSION").to_string()
}

/// Orchestrator: read all four data sources + compose findings.
///
/// Accepts a `cwd` for testability. The top-level `run()` passes
/// `std::env::current_dir()`.
pub fn collect(cwd: &Path) -> Result<VersionDiag> {
    let mut d = VersionDiag {
        cli: SemverCompat::parse(&cli_version_string()),
        ..Default::default()
    };

    let (root, is_standalone) = resolve_active_project_root(cwd);
    if let Some(ref r) = root {
        d.project_root = r.clone();

        // Project SDK: pulp.toml sdk_version for standalone; CMakeLists
        // VERSION otherwise.
        let sdk_raw = if is_standalone {
            crate::pulp_toml::PulpToml::read(r)
                .and_then(|t| t.sdk_version().map(str::to_string))
                .unwrap_or_default()
        } else {
            crate::cmake_version::read(r).unwrap_or_default()
        };
        d.project_sdk = SemverCompat::parse(&sdk_raw);

        let cli_min_raw = crate::pulp_toml::PulpToml::read(r)
            .and_then(|t| t.cli_min_version().map(str::to_string))
            .unwrap_or_default();
        d.project_cli_min = SemverCompat::parse(&cli_min_raw);
    }

    // Plugin JSON lookup. Mirror the C++ quirk exactly: when the
    // active project is STANDALONE (pulp.toml without core/), the C++
    // cmd_doctor.cpp passes an EMPTY repo root to `locate_plugin_json`,
    // which skips the <repo>/.claude-plugin step and falls through to
    // the user-global locations. Source-tree projects pass the real
    // repo root.
    let repo_root_for_plugin = if is_standalone { None } else { root.as_deref() };
    let plugin_json = crate::plugin_json::locate(repo_root_for_plugin, None).unwrap_or_default();
    if !plugin_json.as_os_str().is_empty() {
        d.plugin_json_path = plugin_json.clone();
        if let Some(pj) = crate::plugin_json::PluginJson::read(&plugin_json) {
            d.plugin = SemverCompat::parse(pj.version.as_deref().unwrap_or(""));
            d.plugin_min_cli = SemverCompat::parse(pj.min_cli_version.as_deref().unwrap_or(""));
        }
    }

    // Registered projects (dedup against active root).
    if let Some(reg_path) = projects_registry::registry_path() {
        let list = projects_registry::read(&reg_path);
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

    // Compose findings.
    d.findings = analyze(&Inputs {
        cli: &d.cli,
        plugin_min_cli: &d.plugin_min_cli,
        project_sdk: &d.project_sdk,
        project_cli_min: &d.project_cli_min,
        projects: &d.projects,
    });

    Ok(d)
}

/// Cheap equivalence check — doesn't canonicalize, just normalises
/// separators for the common case (Windows) where registry entries
/// might have been written with forward slashes.
fn paths_equivalent(a: &str, b: &str) -> bool {
    a == b || a.replace('\\', "/") == b.replace('\\', "/")
}

/// Emit the C++-compatible JSON shape. Field order matches
/// `render_report_json` exactly so byte-for-byte diff is possible
/// after a serde_json round-trip.
pub fn emit_json(d: &VersionDiag) -> String {
    let projects: Vec<Value> = d
        .projects
        .iter()
        .map(|p| {
            json!({
                "path": generic(&PathBuf::from(&p.path)),
                "name": p.name,
                "sdk": p.sdk.to_json(),
                "cli_min": p.cli_min.to_json(),
                "missing_on_disk": p.missing_on_disk,
                "scanned": p.scanned,
            })
        })
        .collect();

    let findings: Vec<Value> = d
        .findings
        .iter()
        .map(|f| {
            json!({
                "severity": match f.severity {
                    crate::findings::Severity::Warn => "warn",
                    crate::findings::Severity::Info => "info",
                },
                "message": f.message,
            })
        })
        .collect();

    // Use a serde_json::Map to preserve key order (serde_json's map
    // preserves insertion order).
    let mut obj = serde_json::Map::new();
    obj.insert("cli".to_string(), d.cli.to_json());
    obj.insert("plugin".to_string(), d.plugin.to_json());
    obj.insert("plugin_min_cli".to_string(), d.plugin_min_cli.to_json());
    obj.insert(
        "plugin_json_path".to_string(),
        json!(generic(&d.plugin_json_path)),
    );
    obj.insert("project_root".to_string(), json!(generic(&d.project_root)));
    obj.insert("project_sdk".to_string(), d.project_sdk.to_json());
    obj.insert("project_cli_min".to_string(), d.project_cli_min.to_json());
    obj.insert("projects".to_string(), Value::Array(projects));
    obj.insert("findings".to_string(), Value::Array(findings));

    serde_json::to_string_pretty(&Value::Object(obj))
        .unwrap_or_else(|_| "{}".to_string())
}

/// Normalise a PathBuf into forward-slash form (mirrors C++
/// `generic_string()`). Empty PathBuf → empty string — the C++ writer
/// does the same via its empty-path generic_string().
fn generic(p: &Path) -> String {
    if p.as_os_str().is_empty() {
        return String::new();
    }
    let s = p.to_string_lossy().into_owned();
    s.replace('\\', "/")
}

/// Public entry point used by `main.rs`.
pub fn run(versions: bool, json: bool) -> Result<()> {
    // Phase 2: the `--versions --json` path is the only one ported.
    // Other combinations still print a stub so the CLI stays
    // behaviour-compatible with the Phase 1 surface.
    if versions && json {
        let cwd = std::env::current_dir()?;
        let diag = collect(&cwd)?;
        println!("{}", emit_json(&diag));
        return Ok(());
    }
    if versions {
        println!("pulp-rs doctor --versions (human lane not ported in Phase 2)");
        return Ok(());
    }
    println!("pulp-rs doctor (stub — Phase 2 only ports --versions --json)");
    Ok(())
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
    fn collect_reports_standalone_project() {
        let td = tempfile::tempdir().unwrap();
        write(
            &td.path().join("pulp.toml"),
            "sdk_version = \"0.38.0\"\ncli_min_version = \"0.37.0\"\n",
        );
        // Override so findings generation is deterministic.
        std::env::set_var("PULP_RS_CLI_VERSION", "0.38.0");
        // Empty PULP_HOME so we don't see real registered projects.
        let home = tempfile::tempdir().unwrap();
        std::env::set_var("PULP_HOME", home.path());

        let d = collect(td.path()).unwrap();
        assert!(d.project_sdk.comparable);
        assert_eq!(d.project_sdk.raw, "0.38.0");
        assert_eq!(d.project_cli_min.raw, "0.37.0");
        // CLI 0.38.0 vs SDK 0.38.0 => Info "compatible".
        let infos = d
            .findings
            .iter()
            .filter(|f| f.severity == crate::findings::Severity::Info)
            .count();
        assert_eq!(infos, 1);
    }

    #[test]
    fn collect_warns_on_cli_min_ahead() {
        let td = tempfile::tempdir().unwrap();
        write(
            &td.path().join("pulp.toml"),
            "sdk_version = \"0.40.0\"\ncli_min_version = \"0.40.0\"\n",
        );
        std::env::set_var("PULP_RS_CLI_VERSION", "0.37.0");
        let home = tempfile::tempdir().unwrap();
        std::env::set_var("PULP_HOME", home.path());

        let d = collect(td.path()).unwrap();
        // Expect two warnings: cli_min_version (rule 1) + project SDK (rule 2a).
        let warns = d
            .findings
            .iter()
            .filter(|f| f.severity == crate::findings::Severity::Warn)
            .count();
        assert!(warns >= 2);
    }

    #[test]
    fn emit_json_has_required_keys() {
        let td = tempfile::tempdir().unwrap();
        write(&td.path().join("pulp.toml"), "sdk_version = \"0.38.0\"\n");
        std::env::set_var("PULP_RS_CLI_VERSION", "0.38.0");
        let home = tempfile::tempdir().unwrap();
        std::env::set_var("PULP_HOME", home.path());

        let d = collect(td.path()).unwrap();
        let s = emit_json(&d);
        let v: Value = serde_json::from_str(&s).unwrap();
        let obj = v.as_object().unwrap();
        for k in [
            "cli",
            "plugin",
            "plugin_min_cli",
            "plugin_json_path",
            "project_root",
            "project_sdk",
            "project_cli_min",
            "projects",
            "findings",
        ] {
            assert!(obj.contains_key(k), "missing key: {k}");
        }
    }
}
