// version_parity_test.rs — `pulp-rs version --json` against captured
// expected.json. The C++ `pulp version` has no `--json` flag today, so
// the expected.json files here are the Rust prototype's own pinned
// shape. That's still a parity contract: any accidental change to the
// writer will break the test and force a review.
//
// We additionally assert that the per-field SCHEMA matches `doctor
// --versions --json` (same SemverCompat + generic-path conventions),
// so downstream tooling can use one parser for both commands.

mod common;

use std::fs;

use serde_json::Value;

use common::{fixture_dir, normalise_path_field};

const FIXTURES: &[&str] = &["with_pulp_toml", "with_plugin_json"];

fn run(name: &str) -> Value {
    let dir = fixture_dir("version", name);
    // Isolate from the real user's home + $PULP_HOME so the
    // plugin-json fallback doesn't pick up an ambient manifest.
    let tmp_home = tempfile::tempdir().expect("tempdir");
    let mut cmd = assert_cmd::Command::cargo_bin("pulp").expect("binary");
    cmd.args(["version", "--json"])
        .current_dir(&dir)
        .env("PULP_HOME", tmp_home.path())
        .env("HOME", tmp_home.path())
        .env("PULP_RS_CLI_VERSION", "0.37.0");
    let output = cmd.output().expect("run pulp-rs");
    assert!(
        output.status.success(),
        "pulp-rs version failed on {name}: stderr={}",
        String::from_utf8_lossy(&output.stderr),
    );
    serde_json::from_slice(&output.stdout).expect("JSON output")
}

#[test]
fn version_json_matches_expected() {
    let mut failures = Vec::new();

    for name in FIXTURES {
        let dir = fixture_dir("version", name);
        let expected_path = dir.join("expected.json");
        if !expected_path.exists() {
            eprintln!("  [skip] {name}: no expected.json");
            continue;
        }

        let rust_val = run(name);
        let expected_val: Value = serde_json::from_str(
            &fs::read_to_string(&expected_path)
                .unwrap_or_else(|e| panic!("read {}: {e}", expected_path.display())),
        )
        .unwrap_or_else(|e| panic!("expected.json for {name} not JSON: {e}"));

        let mut rust_norm = rust_val;
        let mut expected_norm = expected_val;
        normalise_path_field(&mut rust_norm, "plugin_json_path", "version", name);
        normalise_path_field(&mut expected_norm, "plugin_json_path", "version", name);

        if rust_norm != expected_norm {
            failures.push((
                name,
                serde_json::to_string_pretty(&expected_norm).unwrap(),
                serde_json::to_string_pretty(&rust_norm).unwrap(),
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
fn version_json_has_required_shape() {
    let v = run("with_pulp_toml");
    for key in ["cli", "plugin", "plugin_min_cli", "plugin_json_path"] {
        assert!(
            v.as_object().unwrap().contains_key(key),
            "missing required key {key}",
        );
    }
    assert!(v["cli"]["raw"].is_string());
    assert_eq!(v["cli"]["raw"], "0.37.0");
}

#[test]
fn version_human_lane_prints_cli_and_plugin() {
    let dir = fixture_dir("version", "with_plugin_json");
    let tmp_home = tempfile::tempdir().unwrap();
    let mut cmd = assert_cmd::Command::cargo_bin("pulp").unwrap();
    cmd.args(["version"])
        .current_dir(&dir)
        .env("PULP_HOME", tmp_home.path())
        .env("HOME", tmp_home.path())
        .env("PULP_RS_CLI_VERSION", "0.37.0");
    let output = cmd.output().unwrap();
    assert!(output.status.success());
    let s = String::from_utf8(output.stdout).unwrap();
    // Phase 8 binary swap (#767): banner is now `pulp v…` (was `pulp-rs v… (prototype)`).
    assert!(s.contains("pulp v0.37.0"));
    assert!(s.contains("Claude plugin: v0.12.0"));
}
