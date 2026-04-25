// parity_test.rs — Run `pulp-rs doctor --versions --json` against each
// fixture and compare to the captured expected.json from the C++ CLI.
//
// Two modes:
//   1. Default: load `expected.json` as the reference and diff the Rust
//      output against it.
//   2. `PULP_CLI_PATH=<path>` env: ALSO run that C++ binary against the
//      fixture and diff the Rust output against its live output —
//      useful when refreshing fixtures after a C++-side behavior
//      change.
//
// Compares via `serde_json::Value` equality, not byte equality — so
// whitespace, trailing newlines, etc. don't matter. The wire shape is
// what matters.

use std::path::{Path, PathBuf};
use std::process::Command;

use serde_json::Value;

const FIXTURES: &[&str] = &[
    "ok_plain",
    "sdk_ahead",
    "cli_min_ahead",
    "plugin_newer",
    "registered_projects",
];

// The C++ binary reports its own compile-time version — which at the
// time the expected.json files were captured was 0.37.0. Pin the Rust
// prototype to the same value via the prototype's override env so the
// CLI field matches exactly.
const CLI_VERSION: &str = "0.37.0";

fn fixture_dir(name: &str) -> PathBuf {
    let manifest = env!("CARGO_MANIFEST_DIR");
    PathBuf::from(manifest)
        .join("tests")
        .join("fixtures")
        .join(name)
}

fn run_pulp_rs(cwd: &Path, extra_env: &[(&str, &str)]) -> String {
    let bin = env!("CARGO_BIN_EXE_pulp-rs");
    let mut cmd = Command::new(bin);
    cmd.args(["doctor", "--versions", "--json"]);
    cmd.current_dir(cwd);
    // Ensure the prototype's CLI version matches the captured C++
    // fixtures. Also clear `PULP_HOME` unless the caller overrides it.
    cmd.env("PULP_RS_CLI_VERSION", CLI_VERSION);
    for (k, v) in extra_env {
        cmd.env(k, v);
    }
    let out = cmd.output().expect("failed to run pulp-rs");
    assert!(
        out.status.success(),
        "pulp-rs failed: stderr={}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8(out.stdout).expect("pulp-rs stdout not utf8")
}

/// Per-fixture extra env overrides (beyond `PULP_RS_CLI_VERSION`).
/// `registered_projects` needs `PULP_HOME` pointing at a planted
/// projects.json; everything else uses an isolated empty `PULP_HOME`.
fn env_for_fixture(name: &str) -> Vec<(String, String)> {
    let tmp = tempfile::tempdir().expect("tempdir");
    let home = tmp.path().to_path_buf();

    if name == "registered_projects" {
        let template = fixture_dir(name).join("projects.json.template");
        std::fs::copy(template, home.join("projects.json")).expect("copy template");
    }

    // Leak the tempdir so it survives until process exit — a test
    // cleanup wrapper would be more idiomatic but this keeps the
    // fixture plumbing small.
    std::mem::forget(tmp);

    vec![("PULP_HOME".to_string(), home.to_string_lossy().into_owned())]
}

fn normalise(mut v: Value, fixture: &str) -> Value {
    // `project_root` is an absolute path. It differs between:
    //   - the machine where expected.json was captured (`/Users/.../pulp-rs-proto/.../fixtures/ok_plain`)
    //   - the CI runner where this test runs (`/home/runner/.../fixtures/ok_plain` on Linux,
    //     `C:\...\fixtures\ok_plain` on Windows).
    // Strip everything before `fixtures/<name>` so the diff is about
    // SHAPE, not path. Handles forward + back slashes.
    let target = format!("fixtures/{fixture}");
    let target_back = format!("fixtures\\{fixture}");
    if let Some(obj) = v.as_object_mut() {
        if let Some(pr) = obj.get_mut("project_root") {
            if let Some(s) = pr.as_str() {
                let normalized = s.replace('\\', "/");
                if let Some(idx) = normalized.find(&target) {
                    let tail = &normalized[idx + target.len()..];
                    *pr = Value::String(format!("<FIXTURE:{fixture}>{tail}"));
                } else if let Some(idx) = s.find(&target_back) {
                    let tail = &s[idx + target_back.len()..].replace('\\', "/");
                    *pr = Value::String(format!("<FIXTURE:{fixture}>{tail}"));
                }
            }
        }
        // plugin_json_path has the same portability problem — a path
        // under `fixtures/<name>/...` becomes `<FIXTURE:name>/...`.
        if let Some(pj) = obj.get_mut("plugin_json_path") {
            if let Some(s) = pj.as_str() {
                if !s.is_empty() {
                    let normalized = s.replace('\\', "/");
                    if let Some(idx) = normalized.find(&target) {
                        let tail = &normalized[idx + target.len()..];
                        *pj = Value::String(format!("<FIXTURE:{fixture}>{tail}"));
                    }
                }
            }
        }
    }
    v
}

#[test]
fn fixtures_match_cpp_expected_json() {
    let mut failures = Vec::new();

    for name in FIXTURES {
        let dir = fixture_dir(name);
        let expected_path = dir.join("expected.json");
        if !expected_path.exists() {
            eprintln!("  [skip] {name}: no expected.json");
            continue;
        }

        let extra_env = env_for_fixture(name);
        let extra_env_refs: Vec<(&str, &str)> = extra_env
            .iter()
            .map(|(k, v)| (k.as_str(), v.as_str()))
            .collect();
        let rust_out = run_pulp_rs(&dir, &extra_env_refs);
        let rust_val: Value = serde_json::from_str(&rust_out)
            .unwrap_or_else(|e| panic!("rust output was not JSON for {name}: {e}\n{rust_out}"));

        let expected_raw = std::fs::read_to_string(&expected_path)
            .unwrap_or_else(|e| panic!("read {}: {e}", expected_path.display()));
        let expected_val: Value = serde_json::from_str(&expected_raw)
            .unwrap_or_else(|e| panic!("expected.json for {name} is not JSON: {e}"));

        let rust_norm = normalise(rust_val, name);
        let expected_norm = normalise(expected_val, name);

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
            eprintln!("--- expected (C++) ---\n{expected}");
            eprintln!("--- got (Rust) ---\n{got}");
        }
        panic!("{} of {} fixtures diverged", failures.len(), FIXTURES.len());
    }
}

/// Optional: compare Rust output against a live C++ invocation rather
/// than the captured expected.json. Skipped unless `PULP_CLI_PATH` is
/// set.
#[test]
fn fixtures_match_live_cpp_when_available() {
    let Some(cpp) = std::env::var_os("PULP_CLI_PATH") else {
        eprintln!("PULP_CLI_PATH not set — skipping live C++ parity check");
        return;
    };

    for name in FIXTURES {
        let dir = fixture_dir(name);
        let extra_env = env_for_fixture(name);
        let extra_env_refs: Vec<(&str, &str)> = extra_env
            .iter()
            .map(|(k, v)| (k.as_str(), v.as_str()))
            .collect();

        let rust_out = run_pulp_rs(&dir, &extra_env_refs);
        let rust_val: Value = serde_json::from_str(&rust_out).unwrap();

        let mut cpp_cmd = Command::new(&cpp);
        cpp_cmd.args(["doctor", "--versions", "--json"]);
        cpp_cmd.current_dir(&dir);
        for (k, v) in &extra_env {
            cpp_cmd.env(k, v);
        }
        let cpp_out = cpp_cmd.output().expect("run C++ pulp");
        assert!(cpp_out.status.success(), "C++ pulp failed on {name}");
        let cpp_val: Value = serde_json::from_slice(&cpp_out.stdout).unwrap();

        assert_eq!(
            normalise(rust_val, name),
            normalise(cpp_val, name),
            "fixture `{name}` diverged from live C++ output",
        );
    }
}
