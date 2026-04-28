//! Phase 4 integration tests — `pulp-rs projects list`.
//!
//! Covers both lanes:
//!
//! - **Human output** diff against `expected_human.txt` captured from
//!   the C++ `pulp projects list` binary. The `Registry:` path varies
//!   per machine, so both sides normalise the first line to
//!   `Registry: <REGISTRY>` before diff.
//! - **JSON output** diff against `expected.json`, which pins the
//!   shape introduced by the Rust port (C++ has no `--json` flag for
//!   `projects list` today).
//!
//! We use [`assert_cmd`] + [`predicates`] for subprocess invocation —
//! cleaner errors than `std::process::Command` when an assertion
//! fails.

use std::fs;
use std::path::{Path, PathBuf};

use predicates::prelude::*;

const FIXTURES: &[&str] = &[
    "empty_registry",
    "single_project",
    "multiple_with_stale",
    "malformed_registry",
];

/// Target directories that every parity fixture expects to exist.
/// Populating them here means `missing_on_disk` behaves predictably
/// regardless of the order tests run in.
const FIXTURE_TARGET_DIRS: &[&str] = &[
    "/tmp/pulp-rs-fixture/alpha-plugin",
    "/tmp/pulp-rs-fixture/active-plugin",
    "/tmp/pulp-rs-fixture/also-active",
    "/tmp/pulp-rs-fixture/forward-compat",
];

fn fixture_dir(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join("projects")
        .join(name)
}

fn ensure_target_dirs() {
    for d in FIXTURE_TARGET_DIRS {
        let _ = fs::create_dir_all(d);
    }
}

fn plant_registry(fixture: &str) -> tempfile::TempDir {
    ensure_target_dirs();
    let home = tempfile::tempdir().expect("tempdir");
    let src = fixture_dir(fixture).join("projects.json");
    if src.exists() {
        fs::copy(&src, home.path().join("projects.json")).expect("copy fixture");
    }
    home
}

/// Replace the machine-specific `Registry: /abs/.../projects.json`
/// line with the placeholder `<REGISTRY>` so diffs are portable.
fn normalise_human(text: &str) -> String {
    text.lines()
        .enumerate()
        .map(|(i, line)| {
            if i == 0 && line.starts_with("Registry: ") {
                "Registry: <REGISTRY>".to_owned()
            } else {
                line.to_owned()
            }
        })
        .collect::<Vec<_>>()
        .join("\n")
        + "\n"
}

/// Replace the `"registry"` field in a JSON value with `<REGISTRY>`
/// before comparison.
fn normalise_json(mut v: serde_json::Value) -> serde_json::Value {
    if let Some(obj) = v.as_object_mut() {
        if obj.get("registry").is_some() {
            obj.insert(
                "registry".to_owned(),
                serde_json::Value::String("<REGISTRY>".to_owned()),
            );
        }
    }
    v
}

fn run_pulp_rs(args: &[&str], pulp_home: &Path) -> std::process::Output {
    let mut cmd = assert_cmd::Command::cargo_bin("pulp-rs").expect("pulp-rs binary");
    cmd.args(args);
    cmd.env("PULP_HOME", pulp_home);
    cmd.env_remove("NO_COLOR"); // irrelevant because stdout is piped
    cmd.output().expect("run pulp-rs")
}

#[test]
fn projects_list_human_matches_cpp_on_all_fixtures() {
    for name in FIXTURES {
        let home = plant_registry(name);
        let output = run_pulp_rs(&["projects", "list"], home.path());
        assert!(
            output.status.success(),
            "pulp-rs projects list failed on {name}: stderr={}",
            String::from_utf8_lossy(&output.stderr)
        );
        let stdout = String::from_utf8(output.stdout).expect("utf8");
        let rendered = normalise_human(&stdout);

        let expected_path = fixture_dir(name).join("expected_human.txt");
        let expected = fs::read_to_string(&expected_path)
            .unwrap_or_else(|e| panic!("read {}: {e}", expected_path.display()));

        assert!(
            rendered == expected,
            "human output diverged for fixture `{name}`\n--- expected (C++) ---\n{expected}\n--- got (Rust) ---\n{rendered}"
        );
    }
}

#[test]
fn projects_list_json_matches_expected_on_all_fixtures() {
    for name in FIXTURES {
        let home = plant_registry(name);
        let output = run_pulp_rs(&["projects", "list", "--json"], home.path());
        assert!(output.status.success(), "pulp-rs failed on {name}");

        let rust_val: serde_json::Value =
            serde_json::from_slice(&output.stdout).expect("valid JSON");

        let expected_path = fixture_dir(name).join("expected.json");
        let expected_val: serde_json::Value =
            serde_json::from_str(&fs::read_to_string(&expected_path).expect("read"))
                .expect("valid expected.json");

        let rn = normalise_json(rust_val);
        let en = normalise_json(expected_val);
        assert_eq!(
            rn,
            en,
            "JSON shape diverged for fixture `{name}`\n--- expected ---\n{}\n--- got ---\n{}",
            serde_json::to_string_pretty(&en).unwrap(),
            serde_json::to_string_pretty(&rn).unwrap(),
        );
    }
}

#[test]
fn projects_list_reports_registry_path_in_human_lane() {
    let home = plant_registry("empty_registry");
    let output = run_pulp_rs(&["projects", "list"], home.path());
    assert!(output.status.success());
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(stdout.contains("projects.json"));
    assert!(stdout.contains("(no projects registered)"));
}

#[test]
fn projects_unknown_subcommand_exits_two() {
    let home = tempfile::tempdir().unwrap();
    let mut cmd = assert_cmd::Command::cargo_bin("pulp-rs").unwrap();
    cmd.args(["projects", "nope"]);
    cmd.env("PULP_HOME", home.path());
    cmd.assert().code(predicate::eq(2));
}
