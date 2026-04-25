//! Snapshot tests via the [`insta`] crate.
//!
//! # Why
//!
//! The JSON shape emitted by `doctor --versions --json` and
//! `projects list --json` is part of this crate's contract. A byte-
//! diff against a checked-in snapshot catches accidental format
//! changes immediately.
//!
//! Updating a snapshot after a deliberate change is a one-liner:
//!
//! ```bash
//! cargo insta review          # interactive
//! cargo insta accept          # non-interactive (CI-driven fixes)
//! ```
//!
//! Snapshots live under `tests/snapshots/`.
//!
//! [`insta`]: https://insta.rs

use std::path::{Path, PathBuf};

use serde_json::Value;

fn fixture_dir(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join("projects")
        .join(name)
}

fn ensure_target_dirs() {
    for d in [
        "/tmp/pulp-rs-fixture/alpha-plugin",
        "/tmp/pulp-rs-fixture/active-plugin",
        "/tmp/pulp-rs-fixture/also-active",
        "/tmp/pulp-rs-fixture/forward-compat",
    ] {
        let _ = std::fs::create_dir_all(d);
    }
}

fn plant_registry(name: &str) -> tempfile::TempDir {
    ensure_target_dirs();
    let home = tempfile::tempdir().unwrap();
    let src = fixture_dir(name).join("projects.json");
    if src.exists() {
        std::fs::copy(&src, home.path().join("projects.json")).unwrap();
    }
    home
}

fn normalise_registry_field(mut v: Value) -> Value {
    if let Some(obj) = v.as_object_mut() {
        if obj.contains_key("registry") {
            obj.insert("registry".into(), Value::String("<REGISTRY>".into()));
        }
    }
    v
}

fn run(args: &[&str], home: &Path) -> Value {
    let mut cmd = assert_cmd::Command::cargo_bin("pulp-rs").unwrap();
    cmd.args(args);
    cmd.env("PULP_HOME", home);
    let output = cmd.output().expect("run pulp-rs");
    assert!(output.status.success(), "pulp-rs failed on {args:?}");
    serde_json::from_slice(&output.stdout).expect("JSON out")
}

#[test]
fn projects_list_json_snapshot_single_project() {
    let home = plant_registry("single_project");
    let v = normalise_registry_field(run(&["projects", "list", "--json"], home.path()));
    insta::assert_json_snapshot!("projects_list_single_project", v);
}

#[test]
fn projects_list_json_snapshot_multiple_with_stale() {
    let home = plant_registry("multiple_with_stale");
    let v = normalise_registry_field(run(&["projects", "list", "--json"], home.path()));
    insta::assert_json_snapshot!("projects_list_multiple_with_stale", v);
}

// ── Phase 5: config + upgrade snapshot coverage ─────────────────────────────

fn normalise_config_path_field(mut v: Value) -> Value {
    if let Some(obj) = v.as_object_mut() {
        if obj.contains_key("config_path") {
            obj.insert(
                "config_path".to_owned(),
                Value::String("<CONFIG_PATH>".to_owned()),
            );
        }
    }
    v
}

#[test]
fn config_list_json_snapshot_empty() {
    let home = tempfile::tempdir().unwrap();
    let v = normalise_config_path_field(run(&["config", "list", "--json"], home.path()));
    insta::assert_json_snapshot!("config_list_empty", v);
}

#[test]
fn config_list_json_snapshot_populated() {
    let home = tempfile::tempdir().unwrap();
    let src = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join("config")
        .join("populated")
        .join("config.toml");
    std::fs::copy(&src, home.path().join("config.toml")).unwrap();
    let v = normalise_config_path_field(run(&["config", "list", "--json"], home.path()));
    insta::assert_json_snapshot!("config_list_populated", v);
}

#[test]
fn upgrade_check_only_json_snapshot_disabled() {
    let home = tempfile::tempdir().unwrap();
    let mut cmd = assert_cmd::Command::cargo_bin("pulp-rs").unwrap();
    cmd.args(["upgrade", "--check-only", "--json"]);
    cmd.env("PULP_HOME", home.path())
        .env("PULP_RS_CLI_VERSION", "0.37.0")
        .env("PULP_UPDATE_CHECK_DISABLED", "1");
    let output = cmd.output().expect("run pulp-rs");
    assert!(output.status.success());
    let v: Value = serde_json::from_slice(&output.stdout).unwrap();
    insta::assert_json_snapshot!("upgrade_check_disabled", v);
}
