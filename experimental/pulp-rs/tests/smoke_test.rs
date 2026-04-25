// smoke_test.rs — integration smoke test.
//
// Two checks:
//   1. `doctor --versions --json` from the prototype's own cwd emits
//      valid JSON with all 9 required top-level keys.
//   2. The same command, run from the `ok_plain` fixture with an
//      isolated PULP_HOME, populates the `project_sdk` field — i.e.
//      the pulp.toml reader actually runs end-to-end.

use std::path::PathBuf;
use std::process::Command;

use serde_json::Value;

const REQUIRED_KEYS: &[&str] = &[
    "cli",
    "plugin",
    "plugin_min_cli",
    "plugin_json_path",
    "project_root",
    "project_sdk",
    "project_cli_min",
    "projects",
    "findings",
];

fn fixture_dir(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join(name)
}

#[test]
fn doctor_versions_json_has_required_shape() {
    let bin = env!("CARGO_BIN_EXE_pulp-rs");

    let output = Command::new(bin)
        .args(["doctor", "--versions", "--json"])
        .output()
        .expect("failed to run pulp-rs binary");

    assert!(
        output.status.success(),
        "pulp-rs exited with {:?}; stderr: {}",
        output.status.code(),
        String::from_utf8_lossy(&output.stderr),
    );

    let parsed: Value =
        serde_json::from_slice(&output.stdout).expect("pulp-rs stdout was not valid JSON");

    let obj = parsed
        .as_object()
        .expect("top-level JSON value must be an object");

    for key in REQUIRED_KEYS {
        assert!(
            obj.contains_key(*key),
            "missing required key `{key}` in doctor --versions --json output; got keys: {:?}",
            obj.keys().collect::<Vec<_>>(),
        );
    }
}

#[test]
fn doctor_populates_project_sdk_from_fixture() {
    let bin = env!("CARGO_BIN_EXE_pulp-rs");
    let dir = fixture_dir("ok_plain");
    let tmp_home = tempfile::tempdir().expect("tempdir");

    let output = Command::new(bin)
        .args(["doctor", "--versions", "--json"])
        .current_dir(&dir)
        .env("PULP_RS_CLI_VERSION", "0.38.0")
        .env("PULP_HOME", tmp_home.path())
        .output()
        .expect("failed to run pulp-rs binary");

    assert!(output.status.success());
    let v: Value = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(v["project_sdk"]["raw"], "0.38.0");
    assert_eq!(v["project_sdk"]["comparable"], true);
    assert_eq!(v["cli"]["raw"], "0.38.0");
    // Findings should include a single Info about compatibility.
    let findings = v["findings"].as_array().unwrap();
    assert!(findings.iter().any(|f| f["severity"] == "info"));
}
