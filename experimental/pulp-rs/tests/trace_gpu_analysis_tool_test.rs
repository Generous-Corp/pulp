//! Real trace_processor acceptance for the three named GPU questions.
//!
//! This test is deliberately fail-closed: the checked-in fixture contract is
//! not called green when the SDK-matched processor is absent. Run `pulp trace
//! fetch` before this focused test on a fresh machine.

use std::path::{Path, PathBuf};

use assert_cmd::Command;
use serde_json::Value;

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .expect("pulp-rs lives under experimental/")
        .to_path_buf()
}

fn trace_processor() -> PathBuf {
    if let Some(path) = std::env::var_os("PULP_TRACE_PROCESSOR") {
        let path = PathBuf::from(path);
        assert!(path.is_file(), "$PULP_TRACE_PROCESSOR is not a file");
        return path;
    }
    let platform = match (std::env::consts::OS, std::env::consts::ARCH) {
        ("macos", "aarch64") => "mac-arm64",
        ("macos", "x86_64") => "mac-amd64",
        ("linux", "x86_64") => "linux-amd64",
        ("linux", "aarch64") => "linux-arm64",
        (os, arch) => panic!("no pinned trace_processor fixture mapping for {os}/{arch}"),
    };
    let home = std::env::var_os("PULP_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".pulp")))
        .expect("PULP_HOME or HOME is required for the pinned processor");
    let path = home
        .join("tools/trace-processor/v57.2")
        .join(platform)
        .join("trace_processor_shell");
    assert!(
        path.is_file(),
        "SDK-matched trace_processor is required: run `pulp trace fetch` ({})",
        path.display()
    );
    path
}

fn run(question: &str, fixture: &str, expected_exit: i32) -> Value {
    let trace = repo_root().join("test/fixtures/perfetto-gpu").join(fixture);
    let output = Command::cargo_bin("pulp")
        .expect("pulp binary")
        .env("PULP_TRACE_PROCESSOR", trace_processor())
        .args(["trace", question, "--trace"])
        .arg(trace)
        .arg("--json")
        .output()
        .expect("run named trace analysis");
    assert_eq!(
        output.status.code(),
        Some(expected_exit),
        "stderr: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    serde_json::from_slice(&output.stdout).expect("typed JSON analysis output")
}

#[test]
fn healthy_fixture_preserves_question_specific_verdicts() {
    let startup = run("gpu-startup", "healthy.pftrace", 2);
    assert_eq!(startup["verdict"], "unverified");
    assert_eq!(startup["dominant_stage"], "pipeline-prepare");
    assert_eq!(startup["capture_complete"], true);
    assert!(startup["ui_correlation"]["open_command"].as_str().is_some());
    assert_eq!(
        startup["ui_correlation"]["search_terms"][0],
        "pipeline-prepare"
    );

    for question in ["gpu-health", "gpu-probe"] {
        let result = run(question, "healthy.pftrace", 0);
        assert_eq!(result["verdict"], "pass");
        assert_eq!(
            result["evidence_ids"][0],
            "0123456789abcdef0123456789abcdef"
        );
    }
}

#[test]
fn compile_failure_is_ranked_with_a_concrete_fix() {
    for question in ["gpu-health", "gpu-probe"] {
        let result = run(question, "compile-failure.pftrace", 1);
        assert_eq!(result["verdict"], "fail");
        assert_eq!(result["next_actions"][0]["code"], "fix-shader-compile");
        assert_eq!(
            result["evidence_ids"][0],
            "fedcba9876543210fedcba9876543210"
        );
    }
}

#[test]
fn incomplete_and_wrong_category_fixtures_are_unavailable() {
    let incomplete = run("gpu-startup", "incomplete.pftrace", 2);
    assert_eq!(incomplete["verdict"], "unavailable");
    assert_eq!(incomplete["unavailable_reason"], "incomplete-capture");

    let wrong = run("gpu-health", "wrong-category.pftrace", 2);
    assert_eq!(wrong["verdict"], "unavailable");
    assert_eq!(wrong["unavailable_reason"], "missing-question-category");
}

#[test]
fn blank_readback_failure_names_the_bounded_oracle_fix() {
    let result = run("gpu-probe", "blank-readback-failure.pftrace", 1);
    assert_eq!(result["verdict"], "fail");
    assert_eq!(result["capture_complete"], true);
    assert_eq!(result["dominant_stage"], "readback");
    assert_eq!(result["next_actions"][0]["code"], "inspect-readback-oracle");
    assert_eq!(result["evidence_ids"][0], "11111111111111111111111111111111");
}

#[test]
fn device_loss_names_the_recreation_fix() {
    let result = run("gpu-health", "device-loss.pftrace", 1);
    assert_eq!(result["verdict"], "fail");
    assert_eq!(result["capture_complete"], true);
    assert_eq!(result["dominant_stage"], "device-loss");
    assert_eq!(result["next_actions"][0]["code"], "recreate-lost-device");
    assert_eq!(result["evidence_ids"][0], "22222222222222222222222222222222");
}

#[test]
fn acquire_present_blocking_names_surface_blocking() {
    let result = run("gpu-startup", "acquire-present-blocking.pftrace", 2);
    assert_eq!(result["verdict"], "unverified");
    assert_eq!(result["capture_complete"], true);
    assert_eq!(result["dominant_stage"], "acquire");
    assert_eq!(result["next_actions"][0]["code"], "inspect-surface-blocking");
    assert_eq!(result["evidence_ids"][0], "33333333333333333333333333333333");
}

#[test]
fn first_frame_stall_ranks_pipeline_before_upload() {
    let result = run(
        "gpu-startup",
        "first-frame-pipeline-upload-stall.pftrace",
        2,
    );
    assert_eq!(result["verdict"], "unverified");
    assert_eq!(result["capture_complete"], true);
    assert_eq!(result["dominant_stage"], "pipeline-prepare");
    assert_eq!(
        result["next_actions"][0]["code"],
        "inspect-pipeline-signature"
    );
    assert_eq!(result["contributors"][1]["stage"], "resource-upload");
    assert_eq!(result["evidence_ids"][0], "44444444444444444444444444444444");
}
