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

fn run_path(question: &str, trace: &Path, expected_exit: i32) -> Value {
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

fn run(question: &str, fixture: &str, expected_exit: i32) -> Value {
    let trace = repo_root().join("test/fixtures/perfetto-gpu").join(fixture);
    run_path(question, &trace, expected_exit)
}

#[test]
fn healthy_fixture_preserves_question_specific_verdicts() {
    let startup = run("gpu-startup", "healthy.pftrace", 2);
    assert_eq!(startup["verdict"], "unverified");
    assert_eq!(startup["dominant_stage"], "pipeline-prepare");
    assert_eq!(startup["capture_complete"], true);
    assert_eq!(startup["observed_categories"], serde_json::json!(["gpu"]));
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
fn categories_are_scoped_to_one_evidence_process_instance() {
    let evidence = "abababababababababababababababab";
    // Reuse the nonce deliberately: process identity, not nonce uniqueness,
    // must keep these unrelated categories out of the named answer.
    let unrelated = evidence;
    let trace = tempfile::NamedTempFile::new().expect("cross-process category trace");
    let fixture = serde_json::json!({"traceEvents": [
        {"name":"gpu_health_transition","cat":"gpu","ph":"X","ts":1000,
         "dur":20,"pid":51,"tid":51,"args":{"debug.gpu_evidence_id":evidence,
         "debug.health_state":"healthy","debug.sequence":1}},
        {"name":"gpu_probe_scoped","cat":"gpu","ph":"X","ts":1100,
         "dur":40,"pid":51,"tid":51,"args":{"debug.gpu_evidence_id":evidence,
         "debug.health_state":"healthy","debug.sequence":2}},
        {"name":"unrelated_render","cat":"render","ph":"X","ts":1200,
         "dur":10,"pid":52,"tid":52,"args":{"debug.gpu_evidence_id":unrelated}},
        {"name":"unrelated_text","cat":"text","ph":"X","ts":1210,
         "dur":10,"pid":52,"tid":52,"args":{"debug.gpu_evidence_id":unrelated}},
        {"name":"unrelated_js","cat":"js","ph":"X","ts":1220,
         "dur":10,"pid":52,"tid":52,"args":{"debug.gpu_evidence_id":unrelated}},
        {"name":"unrelated_layout","cat":"layout","ph":"X","ts":1230,
         "dur":10,"pid":52,"tid":52,"args":{"debug.gpu_evidence_id":unrelated}}
    ]});
    std::fs::write(trace.path(), serde_json::to_vec(&fixture).unwrap()).unwrap();

    for question in ["gpu-health", "gpu-probe"] {
        let result = run_path(question, trace.path(), 0);
        assert_eq!(result["evidence_ids"], serde_json::json!([evidence]));
        assert_eq!(result["observed_categories"], serde_json::json!(["gpu"]));
        assert_eq!(result["category_scope"]["evidence_id"], evidence);
        assert_eq!(result["category_scope"]["process_pid"], 51);
        assert!(result["category_scope"]["process_upid"].as_i64().is_some());
    }

    let positive = tempfile::NamedTempFile::new().expect("same-process category trace");
    let events = fixture["traceEvents"]
        .as_array()
        .unwrap()
        .iter()
        .take(2)
        .cloned()
        .chain(
            ["render", "text", "js", "layout"]
                .into_iter()
                .enumerate()
                .map(|(index, category)| {
                    serde_json::json!({
                        "name": format!("scoped_{category}"), "cat": category, "ph": "X",
                        "ts": 1300 + index * 10, "dur": 5, "pid": 51, "tid": 51,
                        "args": {"debug.gpu_evidence_id": evidence}
                    })
                }),
        )
        .collect::<Vec<_>>();
    std::fs::write(
        positive.path(),
        serde_json::to_vec(&serde_json::json!({"traceEvents": events})).unwrap(),
    )
    .unwrap();
    let result = run_path("gpu-health", positive.path(), 0);
    assert_eq!(
        result["observed_categories"],
        serde_json::json!(["gpu", "js", "layout", "render", "text"])
    );
    assert_eq!(result["category_scope"]["evidence_id"], evidence);
    assert_eq!(result["category_scope"]["process_pid"], 51);
}

#[test]
fn duplicate_evidence_id_across_question_processes_is_unavailable() {
    let evidence = "acacacacacacacacacacacacacacacac";
    let trace = tempfile::NamedTempFile::new().expect("ambiguous evidence trace");
    let fixture = serde_json::json!({"traceEvents": [
        {"name":"gpu_health_transition_first","cat":"gpu","ph":"X","ts":1000,
         "dur":20,"pid":81,"tid":81,"args":{"debug.gpu_evidence_id":evidence,
         "debug.health_state":"healthy","debug.sequence":1}},
        {"name":"gpu_probe_first","cat":"gpu","ph":"X","ts":1100,
         "dur":20,"pid":81,"tid":81,"args":{"debug.gpu_evidence_id":evidence,
         "debug.health_state":"healthy","debug.sequence":2}},
        {"name":"gpu_health_transition_second","cat":"gpu","ph":"X","ts":1200,
         "dur":20,"pid":82,"tid":82,"args":{"debug.gpu_evidence_id":evidence,
         "debug.health_state":"healthy","debug.sequence":1}},
        {"name":"gpu_probe_second","cat":"gpu","ph":"X","ts":1300,
         "dur":20,"pid":82,"tid":82,"args":{"debug.gpu_evidence_id":evidence,
         "debug.health_state":"healthy","debug.sequence":2}}
    ]});
    std::fs::write(trace.path(), serde_json::to_vec(&fixture).unwrap()).unwrap();

    for question in ["gpu-health", "gpu-probe"] {
        let result = run_path(question, trace.path(), 2);
        assert_eq!(result["verdict"], "unavailable");
        assert_eq!(result["capture_complete"], false);
        assert_eq!(result["unavailable_reason"], "invalid-evidence-correlation");
        assert_eq!(result["category_scope"], serde_json::Value::Null);
    }
}

#[test]
fn causal_probe_failure_diagnostic_cannot_be_laundered_by_healthy_state() {
    let evidence = "efefefefefefefefefefefefefefefef";
    let trace = tempfile::NamedTempFile::new().expect("probe oracle mismatch trace");
    let fixture = serde_json::json!({"traceEvents": [{
        "name": "gpu_probe_renderer3d_hardcoded_cube", "cat": "gpu", "ph": "X",
        "ts": 1000, "dur": 20, "pid": 61, "tid": 61,
        "args": {"debug.gpu_evidence_id": evidence,
                 "debug.health_state": "healthy",
                 "debug.diagnostic_code": "cpu_oracle_mismatch",
                 "debug.sequence": 1}
    }]});
    std::fs::write(trace.path(), serde_json::to_vec(&fixture).unwrap()).unwrap();

    let result = run_path("gpu-probe", trace.path(), 1);
    assert_eq!(result["verdict"], "fail");
    assert_eq!(result["evidence_ids"], serde_json::json!([evidence]));
    assert_eq!(result["next_actions"][0]["code"], "inspect-readback-oracle");
}

#[test]
fn generic_untagged_backend_spans_cannot_poison_or_supply_a_probe_cohort() {
    let evidence = "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";
    let trace = tempfile::NamedTempFile::new().expect("untagged backend trace");
    let fixture = serde_json::json!({"traceEvents": [
        {"name":"gpu_submit_internal","cat":"gpu","ph":"X","ts":900,
         "dur":5,"pid":71,"tid":71},
        {"name":"gpu_probe_scoped","cat":"gpu","ph":"X","ts":1000,
         "dur":20,"pid":71,"tid":71,"args":{"debug.gpu_evidence_id":evidence,
         "debug.health_state":"healthy","debug.sequence":1}},
        {"name":"scoped_render","cat":"render","ph":"X","ts":1030,
         "dur":5,"pid":71,"tid":71,"args":{"debug.gpu_evidence_id":evidence}},
        {"name":"scoped_text","cat":"text","ph":"X","ts":1040,
         "dur":5,"pid":71,"tid":71,"args":{"debug.gpu_evidence_id":evidence}},
        {"name":"scoped_js","cat":"js","ph":"X","ts":1050,
         "dur":5,"pid":71,"tid":71,"args":{"debug.gpu_evidence_id":evidence}},
        {"name":"scoped_layout","cat":"layout","ph":"X","ts":1060,
         "dur":5,"pid":71,"tid":71,"args":{"debug.gpu_evidence_id":evidence}}
    ]});
    std::fs::write(trace.path(), serde_json::to_vec(&fixture).unwrap()).unwrap();
    let result = run_path("gpu-probe", trace.path(), 0);
    assert_eq!(result["verdict"], "pass");
    assert_eq!(result["evidence_ids"], serde_json::json!([evidence]));
    assert_eq!(
        result["observed_categories"],
        serde_json::json!(["gpu", "js", "layout", "render", "text"])
    );

    let mixed = tempfile::NamedTempFile::new().expect("mixed tagged probe trace");
    let other = "edededededededededededededededed";
    let mixed_fixture = serde_json::json!({"traceEvents": [
        {"name":"gpu_probe_a","cat":"gpu","ph":"X","ts":1000,
         "dur":20,"pid":71,"tid":71,"args":{"debug.gpu_evidence_id":evidence,
         "debug.health_state":"healthy","debug.sequence":1}},
        {"name":"gpu_probe_b","cat":"gpu","ph":"X","ts":1030,
         "dur":20,"pid":71,"tid":71,"args":{"debug.gpu_evidence_id":other,
         "debug.health_state":"healthy","debug.sequence":2}}
    ]});
    std::fs::write(mixed.path(), serde_json::to_vec(&mixed_fixture).unwrap()).unwrap();
    let result = run_path("gpu-probe", mixed.path(), 2);
    assert_eq!(result["verdict"], "unavailable");
    assert!(result["evidence_ids"].as_array().unwrap().is_empty());
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
    assert_eq!(
        incomplete["next_actions"][0]["code"],
        "complete-and-flush-capture"
    );

    let wrong = run("gpu-health", "wrong-category.pftrace", 2);
    assert_eq!(wrong["verdict"], "unavailable");
    assert_eq!(wrong["unavailable_reason"], "missing-question-category");

    let render_only = run("gpu-startup", "render-only.pftrace", 2);
    assert_eq!(render_only["verdict"], "unavailable");
    assert_eq!(
        render_only["unavailable_reason"],
        "missing-question-category"
    );
}

#[test]
fn healthy_diagnostic_codes_do_not_become_failures() {
    for question in ["gpu-health", "gpu-probe"] {
        let result = run(question, "healthy-diagnostic.pftrace", 0);
        assert_eq!(result["verdict"], "pass");
        assert_eq!(result["capture_complete"], true);
    }
}

#[test]
fn unavailable_and_unverified_health_states_never_become_pass() {
    for question in ["gpu-health", "gpu-probe"] {
        let unavailable = run(question, "unavailable-state.pftrace", 2);
        assert_eq!(unavailable["verdict"], "unavailable");
        assert_eq!(unavailable["unavailable_reason"], "reported-unavailable");
        assert_eq!(unavailable["capture_complete"], true);

        let unverified = run(question, "unverified-state.pftrace", 2);
        assert_eq!(unverified["verdict"], "unverified");
        assert_eq!(unverified["capture_complete"], true);
    }
}

#[test]
fn short_non_pass_state_survives_the_bounded_contributor_limit() {
    use std::fmt::Write as _;

    let trace = std::env::temp_dir().join(format!(
        "pulp-gpu-analysis-over-limit-{}.pftrace",
        std::process::id()
    ));
    let mut events = String::from("{\"traceEvents\":[");
    for index in 0..17 {
        if index > 0 {
            events.push(',');
        }
        write!(events, "{{\"name\":\"gpu_health_transition_{index}\",\"cat\":\"gpu\",\"ph\":\"X\",\"ts\":{},\"dur\":{},\"pid\":13,\"tid\":13,\"args\":{{\"debug.gpu_evidence_id\":\"99999999999999999999999999999999\",\"debug.health_state\":\"healthy\",\"debug.sequence\":{index}}}}},{{\"name\":\"gpu_probe_over_limit_{index}\",\"cat\":\"gpu\",\"ph\":\"X\",\"ts\":{},\"dur\":{},\"pid\":13,\"tid\":13,\"args\":{{\"debug.gpu_evidence_id\":\"99999999999999999999999999999999\",\"debug.health_state\":\"healthy\",\"debug.sequence\":{index}}}}}", 1000 + index * 20, 1000 + index, 1010 + index * 20, 1000 + index).unwrap();
    }
    events.push(',');
    events.push_str("{\"name\":\"gpu_health_transition_short\",\"cat\":\"gpu\",\"ph\":\"X\",\"ts\":5000,\"dur\":1,\"pid\":13,\"tid\":13,\"args\":{\"debug.gpu_evidence_id\":\"99999999999999999999999999999999\",\"debug.health_state\":\"unavailable\",\"debug.sequence\":18}},{\"name\":\"gpu_probe_short\",\"cat\":\"gpu\",\"ph\":\"X\",\"ts\":5010,\"dur\":1,\"pid\":13,\"tid\":13,\"args\":{\"debug.gpu_evidence_id\":\"99999999999999999999999999999999\",\"debug.health_state\":\"unavailable\",\"debug.sequence\":18}}]}");
    std::fs::write(&trace, events).unwrap();
    for question in ["gpu-health", "gpu-probe"] {
        let result = run_path(question, &trace, 2);
        assert_eq!(result["verdict"], "unavailable");
        assert_eq!(result["unavailable_reason"], "reported-unavailable");
    }
    std::fs::remove_file(trace).unwrap();
}

#[test]
fn blank_readback_failure_names_the_bounded_oracle_fix() {
    let result = run("gpu-probe", "blank-readback-failure.pftrace", 1);
    assert_eq!(result["verdict"], "fail");
    assert_eq!(result["capture_complete"], true);
    assert_eq!(result["dominant_stage"], "readback");
    assert_eq!(result["next_actions"][0]["code"], "inspect-readback-oracle");
    assert_eq!(
        result["evidence_ids"][0],
        "11111111111111111111111111111111"
    );
}

#[test]
fn device_loss_names_the_recreation_fix() {
    let result = run("gpu-health", "device-loss.pftrace", 1);
    assert_eq!(result["verdict"], "fail");
    assert_eq!(result["capture_complete"], true);
    assert_eq!(result["dominant_stage"], "device-loss");
    assert_eq!(result["next_actions"][0]["code"], "recreate-lost-device");
    assert_eq!(
        result["evidence_ids"][0],
        "22222222222222222222222222222222"
    );
}

#[test]
fn acquire_present_without_scheduler_data_refuses_to_infer_blocking() {
    let result = run("gpu-startup", "acquire-present-wall-time-only.pftrace", 2);
    assert_eq!(result["verdict"], "unverified");
    assert_eq!(result["capture_complete"], true);
    assert_eq!(result["dominant_stage"], "acquire");
    assert_eq!(
        result["next_actions"][0]["code"],
        "capture-scheduler-evidence"
    );
    assert_eq!(result["scheduler_evidence_available"], false);
    assert_eq!(result["contributors"][0]["execution_state"], "unavailable");
    assert!(result["contributors"][0].get("cpu_running_ns").is_none());
    assert_eq!(
        result["evidence_ids"][0],
        "33333333333333333333333333333333"
    );
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
    assert_eq!(
        result["evidence_ids"][0],
        "44444444444444444444444444444444"
    );
}

#[test]
fn startup_rejects_multiple_tagged_first_visible_lifecycles() {
    let first_evidence = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    let later_evidence = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    let trace = tempfile::NamedTempFile::new().expect("mixed startup trace");
    let fixture = serde_json::json!({
        "traceEvents": [
            {"name":"gpu_pipeline_prepare","cat":"gpu","ph":"X","ts":1000,
             "dur":700,"pid":20,"tid":20,"args":{"debug.gpu_evidence_id":first_evidence,
             "debug.health_state":"healthy","debug.sequence":1,"debug.frame_index":0}},
            {"name":"gpu_resource_upload","cat":"gpu","ph":"X","ts":1200,
             "dur":200,"pid":20,"tid":20,"args":{"debug.gpu_evidence_id":first_evidence,
             "debug.health_state":"healthy","debug.sequence":2,"debug.frame_index":0}},
            {"name":"frame","cat":"render","ph":"X","ts":1700,"dur":50,
             "pid":20,"tid":20,"args":{"debug.gpu_evidence_id":first_evidence,
             "debug.health_state":"healthy","debug.sequence":3,"debug.frame_index":0}},
            {"name":"gpu_present_runtime","cat":"gpu","ph":"X","ts":2000,
             "dur":30000,"pid":20,"tid":20,"args":{"debug.gpu_evidence_id":first_evidence,
             "debug.health_state":"healthy","debug.sequence":4,"debug.frame_index":1}},
            {"name":"gpu_pipeline_prepare_future_window","cat":"gpu","ph":"X","ts":500,
             "dur":40000,"pid":20,"tid":20,"args":{"debug.gpu_evidence_id":later_evidence,
             "debug.health_state":"healthy","debug.sequence":1,"debug.frame_index":0}},
            {"name":"frame_later","cat":"render","ph":"X","ts":4500,"dur":50,
             "pid":20,"tid":20,"args":{"debug.gpu_evidence_id":later_evidence,
             "debug.health_state":"healthy","debug.sequence":2,"debug.frame_index":0}}
        ]
    });
    std::fs::write(trace.path(), serde_json::to_vec(&fixture).unwrap()).unwrap();

    let result = run_path("gpu-startup", trace.path(), 2);
    assert_eq!(result["verdict"], "unavailable");
    assert_eq!(result["capture_complete"], false);
    assert_eq!(result["unavailable_reason"], "missing-question-category");
    assert_eq!(result["contributors"], serde_json::json!([]));
    assert_eq!(result["evidence_ids"], serde_json::json!([]));
}

#[test]
fn startup_infers_only_pre_first_frame_unindexed_setup_as_cold() {
    let evidence = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    let trace = tempfile::NamedTempFile::new().expect("phase trace");
    let fixture = serde_json::json!({
        "traceEvents": [
            {"name":"gpu_pipeline_prepare","cat":"gpu","ph":"X","ts":500,
             "dur":300,"pid":30,"tid":30,"args":{"debug.gpu_evidence_id":evidence,
             "debug.health_state":"healthy","debug.sequence":1}},
            {"name":"frame","cat":"render","ph":"X","ts":1000,"dur":100,
             "pid":30,"tid":30,"args":{"debug.gpu_evidence_id":evidence,
             "debug.health_state":"healthy","debug.sequence":2,"debug.frame_index":0}},
            {"name":"gpu_acquire_late_unindexed","cat":"gpu","ph":"X","ts":2000,
             "dur":900,"pid":30,"tid":30,"args":{"debug.gpu_evidence_id":evidence,
             "debug.health_state":"healthy","debug.sequence":3}},
            {"name":"gpu_present_steady","cat":"gpu","ph":"X","ts":3000,"dur":80,
             "pid":30,"tid":30,"args":{"debug.gpu_evidence_id":evidence,
             "debug.health_state":"healthy","debug.sequence":4,"debug.frame_index":1}}
        ]
    });
    std::fs::write(trace.path(), serde_json::to_vec(&fixture).unwrap()).unwrap();

    let result = run_path("gpu-startup", trace.path(), 2);
    assert_eq!(result["verdict"], "unverified");
    assert_eq!(result["dominant_stage"], "pipeline-prepare");
    let contributors = result["contributors"].as_array().unwrap();
    let late = contributors
        .iter()
        .find(|row| row["stage"] == "acquire")
        .expect("late unindexed acquire");
    assert_eq!(late["timing_phase"], "unknown");
    assert_eq!(
        result["cold_start_contributors"].as_array().unwrap().len(),
        2
    );
    assert_eq!(
        result["steady_state_contributors"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
}

#[test]
fn steady_only_startup_trace_fails_closed() {
    let evidence = "ffffffffffffffffffffffffffffffff";
    let trace = tempfile::NamedTempFile::new().expect("steady-only trace");
    let fixture = serde_json::json!({
        "traceEvents": [
            {"name":"gpu_present_steady","cat":"gpu","ph":"X","ts":1000,
             "dur":800,"pid":31,"tid":31,"args":{"debug.gpu_evidence_id":evidence,
             "debug.health_state":"healthy","debug.sequence":1,"debug.frame_index":1}},
            {"name":"frame_steady","cat":"render","ph":"X","ts":1900,"dur":100,
             "pid":31,"tid":31,"args":{"debug.gpu_evidence_id":evidence,
             "debug.health_state":"healthy","debug.sequence":2,"debug.frame_index":1}}
        ]
    });
    std::fs::write(trace.path(), serde_json::to_vec(&fixture).unwrap()).unwrap();

    let result = run_path("gpu-startup", trace.path(), 2);
    assert_eq!(result["verdict"], "unavailable");
    assert_eq!(result["capture_complete"], false);
    assert_eq!(result["unavailable_reason"], "missing-cold-start-window");
    assert_eq!(
        result["next_actions"][0]["code"],
        "capture-cold-start-window"
    );
    assert_eq!(result["cold_start_contributors"], serde_json::json!([]));
    assert_eq!(
        result["steady_state_contributors"]
            .as_array()
            .unwrap()
            .len(),
        2
    );
}

#[test]
fn zero_byte_never_flushed_capture_is_typed_unavailable() {
    let trace = tempfile::NamedTempFile::new().expect("empty capture");
    let result = run_path("gpu-startup", trace.path(), 2);
    assert_eq!(result["verdict"], "unavailable");
    assert_eq!(result["capture_complete"], false);
    assert_eq!(
        result["unavailable_reason"],
        "empty-or-never-flushed-capture"
    );
    assert_eq!(result["capture_integrity"]["slice_count"], 0);
    assert_eq!(
        result["next_actions"][0]["code"],
        "record-and-flush-capture"
    );
}

#[test]
fn processor_reported_truncated_capture_is_typed_unavailable() {
    let result = run("gpu-startup", "truncated-json.pftrace", 2);
    assert_eq!(result["verdict"], "unavailable");
    assert_eq!(result["capture_complete"], false);
    assert_eq!(result["unavailable_reason"], "truncated-capture");
    assert_eq!(
        result["capture_integrity"]["processor_reported_truncated"],
        true
    );
    assert_eq!(
        result["next_actions"][0]["code"],
        "recapture-complete-artifact"
    );
}

#[test]
fn startup_ignores_generic_untagged_backend_candidates() {
    let evidence = "abababababababababababababababab";
    let trace = tempfile::NamedTempFile::new().expect("uncorrelated startup trace");
    let fixture = serde_json::json!({
        "traceEvents": [
            {"name":"gpu_pipeline_prepare","cat":"gpu","ph":"X","ts":1000,
             "dur":700,"pid":22,"tid":22,"args":{"debug.gpu_evidence_id":evidence,
             "debug.health_state":"healthy","debug.sequence":1,"debug.frame_index":0}},
            {"name":"frame","cat":"render","ph":"X","ts":1700,"dur":50,
             "pid":22,"tid":22,"args":{"debug.gpu_evidence_id":evidence,
             "debug.health_state":"healthy","debug.sequence":2,"debug.frame_index":0}},
            {"name":"gpu_shader_compile_uncorrelated","cat":"gpu","ph":"X","ts":900,
             "dur":40000,"pid":22,"tid":22,"args":{"debug.health_state":"healthy",
             "debug.sequence":0,"debug.frame_index":0}}
        ]
    });
    std::fs::write(trace.path(), serde_json::to_vec(&fixture).unwrap()).unwrap();

    let result = run_path("gpu-startup", trace.path(), 2);
    assert_eq!(result["verdict"], "unverified");
    assert_eq!(result["capture_complete"], true);
    assert_eq!(result["evidence_ids"], serde_json::json!([evidence]));
    assert!(result["contributors"].as_array().unwrap().iter().all(
        |row| row["evidence_id"] == evidence
    ));
}

#[test]
fn health_and_probe_reject_mixed_evidence_before_the_row_cap() {
    let first_evidence = "cccccccccccccccccccccccccccccccc";
    let second_evidence = "dddddddddddddddddddddddddddddddd";
    let trace = tempfile::NamedTempFile::new().expect("mixed evidence trace");
    let mut events = Vec::new();
    for index in 0..17 {
        events.push(serde_json::json!({
            "name": format!("gpu_health_transition_{index}"), "cat": "gpu", "ph": "X",
            "ts": 1000 + index * 20, "dur": 1000 + index, "pid": 21, "tid": 21,
            "args": {"debug.gpu_evidence_id": first_evidence,
                     "debug.health_state": "healthy", "debug.sequence": index}
        }));
        events.push(serde_json::json!({
            "name": format!("gpu_probe_{index}"), "cat": "gpu", "ph": "X",
            "ts": 1010 + index * 20, "dur": 1000 + index, "pid": 21, "tid": 21,
            "args": {"debug.gpu_evidence_id": first_evidence,
                     "debug.health_state": "healthy", "debug.sequence": index}
        }));
    }
    events.push(serde_json::json!({
        "name": "gpu_health_transition_other", "cat": "gpu", "ph": "X",
        "ts": 5000, "dur": 1, "pid": 21, "tid": 21,
        "args": {"debug.gpu_evidence_id": second_evidence,
                 "debug.health_state": "healthy", "debug.sequence": 1}
    }));
    events.push(serde_json::json!({
        "name": "gpu_probe_other", "cat": "gpu", "ph": "X",
        "ts": 5010, "dur": 1, "pid": 21, "tid": 21,
        "args": {"debug.gpu_evidence_id": second_evidence,
                 "debug.health_state": "healthy", "debug.sequence": 1}
    }));
    std::fs::write(
        trace.path(),
        serde_json::to_vec(&serde_json::json!({"traceEvents": events})).unwrap(),
    )
    .unwrap();

    for question in ["gpu-health", "gpu-probe"] {
        let singleton = run(question, "healthy.pftrace", 0);
        assert_eq!(
            singleton["evidence_ids"],
            serde_json::json!(["0123456789abcdef0123456789abcdef"])
        );

        let mixed = run_path(question, trace.path(), 2);
        assert_eq!(mixed["verdict"], "unavailable");
        assert_eq!(mixed["capture_complete"], false);
        assert_eq!(mixed["unavailable_reason"], "missing-question-category");
        assert_eq!(mixed["observed_categories"], serde_json::json!([]));
        assert_eq!(mixed["category_scope"], serde_json::Value::Null);
        assert_eq!(mixed["contributors"], serde_json::json!([]));
        assert_eq!(mixed["evidence_ids"], serde_json::json!([]));
    }
}
