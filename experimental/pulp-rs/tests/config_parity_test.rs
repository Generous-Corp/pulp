// config_parity_test.rs — `pulp-rs config <list|get|set>` parity.
//
// The C++ `pulp config` command has no `--json` flag today, so the
// Rust port introduces one. The expected.json files here pin the
// shape for that new JSON lane. The human lane's parity is a simpler
// substring check since the C++ and Rust writers compose each line
// identically.

mod common;

use std::fs;

use serde_json::Value;

use common::{fixture_dir, replace_field};

const FIXTURES: &[&str] = &["empty", "partial", "populated"];

/// Plant a temp `PULP_HOME` with the fixture's config.toml (if any)
/// and return the tempdir handle + the expected config path.
fn plant(name: &str) -> tempfile::TempDir {
    let home = tempfile::tempdir().expect("tempdir");
    let src = fixture_dir("config", name).join("config.toml");
    if src.exists() {
        fs::copy(&src, home.path().join("config.toml")).expect("copy fixture");
    }
    home
}

fn run(args: &[&str], home: &std::path::Path) -> std::process::Output {
    let mut cmd = assert_cmd::Command::cargo_bin("pulp").expect("binary");
    cmd.args(args).env("PULP_HOME", home);
    cmd.output().expect("run pulp-rs")
}

#[test]
fn list_json_matches_expected_shape() {
    let mut failures = Vec::new();

    for name in FIXTURES {
        let home = plant(name);
        let output = run(&["config", "list", "--json"], home.path());
        assert!(
            output.status.success(),
            "pulp-rs config list --json failed on {name}: stderr={}",
            String::from_utf8_lossy(&output.stderr)
        );

        let mut rust_val: Value = serde_json::from_slice(&output.stdout).expect("JSON output");
        replace_field(&mut rust_val, "config_path", "<CONFIG_PATH>");

        let expected_path = fixture_dir("config", name).join("expected.json");
        let expected_val: Value = serde_json::from_str(
            &fs::read_to_string(&expected_path)
                .unwrap_or_else(|e| panic!("read {}: {e}", expected_path.display())),
        )
        .expect("expected.json is valid JSON");

        if rust_val != expected_val {
            failures.push((
                name,
                serde_json::to_string_pretty(&expected_val).unwrap(),
                serde_json::to_string_pretty(&rust_val).unwrap(),
            ));
        }
    }

    if !failures.is_empty() {
        for (name, expected, got) in &failures {
            eprintln!("=== fixture `{name}` diverged ===");
            eprintln!("--- expected ---\n{expected}");
            eprintln!("--- got ---\n{got}");
        }
        panic!("{} of {} fixtures diverged", failures.len(), FIXTURES.len());
    }
}

#[test]
fn list_human_lane_reports_keys_with_resolved_values() {
    let home = plant("partial");
    let output = run(&["config", "list"], home.path());
    assert!(output.status.success());
    let s = String::from_utf8(output.stdout).unwrap();
    assert!(s.contains("Pulp config ("));
    assert!(s.contains("update.mode = manual"));
    assert!(s.contains("update.channel = stable")); // defaulted
}

#[test]
fn get_prints_value_or_empty_line() {
    let home = plant("partial");

    // A set key.
    let output = run(&["config", "get", "update.mode"], home.path());
    assert!(output.status.success());
    assert_eq!(String::from_utf8(output.stdout).unwrap(), "manual\n");

    // An unset key — prints empty line, still exit 0.
    let output = run(&["config", "get", "update.channel"], home.path());
    assert!(output.status.success());
    assert_eq!(String::from_utf8(output.stdout).unwrap(), "\n");
}

#[test]
fn set_round_trips_and_preserves_comments() {
    let home = plant("partial");
    let output = run(&["config", "set", "update.mode", "auto"], home.path());
    assert!(output.status.success(), "set failed");

    let body = fs::read_to_string(home.path().join("config.toml")).unwrap();
    assert!(body.contains("# Pulp config"));
    assert!(body.contains("mode = \"auto\""));

    let output = run(&["config", "get", "update.mode"], home.path());
    assert_eq!(String::from_utf8(output.stdout).unwrap(), "auto\n");
}

#[test]
fn unknown_subcommand_exits_two() {
    let home = tempfile::tempdir().unwrap();
    let output = run(&["config", "bogus"], home.path());
    assert_eq!(output.status.code(), Some(2));
}

#[test]
fn set_rejects_unknown_key_with_exit_two() {
    let home = tempfile::tempdir().unwrap();
    let output = run(&["config", "set", "update.nope", "x"], home.path());
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8(output.stderr).unwrap();
    assert!(stderr.contains("unknown config key"));
}

#[test]
fn set_rejects_bad_value_with_exit_two() {
    let home = tempfile::tempdir().unwrap();
    let output = run(&["config", "set", "update.mode", "bogus"], home.path());
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8(output.stderr).unwrap();
    assert!(stderr.contains("auto, prompt, manual, off"));
}
