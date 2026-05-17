// motion_smoke_test.rs — end-to-end smoke tests for `pulp motion *`.
//
// These exercise the binary's argv parsing, exit codes, and the
// reachability gate. They deliberately do NOT spawn pulp-ui-preview
// (the only host that exposes a live motion inspector) because:
//
//   1. Spawning a GUI host with a window event loop is hostile to
//      headless CI;
//   2. The dispatcher's wire-protocol translation is already
//      covered by 34 unit tests with a RecordingTalker seam;
//   3. What the e2e layer can validate cheaply — exit codes,
//      help text bytes, the reachability hint, bad-usage errors —
//      is exactly what unit tests can't easily cover through the
//      library boundary.
//
// Pick a port that's unlikely to be bound in CI for the
// reachability assertions: 47654. If anything is genuinely
// listening there during the test run, the assertion would flake;
// that's an acceptable trade for not introducing an artificial
// "isolation port" infrastructure.

use std::process::Command;

const UNLIKELY_PORT: &str = "47654";

#[test]
fn motion_with_no_verb_prints_usage() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion"])
        .env_remove("PULP_INSPECTOR_PORT")
        .output()
        .expect("failed to run pulp binary");

    assert!(
        output.status.success(),
        "pulp motion (no args) should exit 0, got {:?}; stderr: {}",
        output.status.code(),
        String::from_utf8_lossy(&output.stderr),
    );

    let stdout = String::from_utf8_lossy(&output.stdout);
    // Usage banner anchors.
    assert!(stdout.contains("pulp motion"), "stdout: {stdout}");
    assert!(stdout.contains("record"), "stdout: {stdout}");
    assert!(stdout.contains("stop"), "stdout: {stdout}");
    assert!(stdout.contains("snapshot"), "stdout: {stdout}");
    assert!(stdout.contains("list-traces"), "stdout: {stdout}");
    assert!(stdout.contains("load-fixture"), "stdout: {stdout}");
    assert!(stdout.contains("scrub"), "stdout: {stdout}");
    assert!(stdout.contains("play"), "stdout: {stdout}");
    assert!(stdout.contains("pause"), "stdout: {stdout}");
    assert!(stdout.contains("cost"), "stdout: {stdout}");
}

#[test]
fn motion_unknown_verb_exits_two_with_supported_list() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "wat"])
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(
        output.status.code(),
        Some(2),
        "expected exit 2 on unknown verb; stderr: {}",
        String::from_utf8_lossy(&output.stderr),
    );
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("unknown subcommand"), "stderr: {stderr}");
    // Should hint at supported verbs.
    assert!(stderr.contains("supported"), "stderr: {stderr}");
}

#[test]
fn motion_snapshot_with_no_inspector_exits_one_with_hint() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "snapshot", "--port", UNLIKELY_PORT])
        .env_remove("PULP_INSPECTOR_PORT")
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(
        output.status.code(),
        Some(1),
        "expected exit 1 (no inspector); stderr: {}",
        String::from_utf8_lossy(&output.stderr),
    );
    let stderr = String::from_utf8_lossy(&output.stderr);
    // The clear "start the host" hint should be visible.
    assert!(
        stderr.contains("no inspector listening"),
        "stderr: {stderr}"
    );
    assert!(
        stderr.contains("PULP_MOTION_SERVER=1"),
        "stderr: {stderr}"
    );
    assert!(
        stderr.contains(UNLIKELY_PORT),
        "stderr: {stderr}"
    );
}

#[test]
fn motion_scrub_missing_frame_is_bad_usage() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "scrub"])
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("missing <FRAME>"), "stderr: {stderr}");
}

#[test]
fn motion_scrub_garbage_frame_is_bad_usage() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "scrub", "not-a-number"])
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("invalid frame"), "stderr: {stderr}");
}

#[test]
fn motion_cost_requires_action_arg() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "cost"])
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("enable|disable"), "stderr: {stderr}");
}

#[test]
fn motion_load_fixture_requires_path() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "load-fixture"])
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("missing <PATH>"), "stderr: {stderr}");
}

#[test]
fn motion_port_rejects_garbage_value() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "--port", "not-a-number", "snapshot"])
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("--port") || stderr.contains("invalid"),
        "stderr: {stderr}"
    );
}

#[test]
fn motion_env_port_overrides_default() {
    // We set PULP_INSPECTOR_PORT to an unlikely port and assert
    // the reachability hint surfaces THAT port (not 9147). Proves
    // the env override is wired through `resolve_port` end-to-end.
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "snapshot"])
        .env("PULP_INSPECTOR_PORT", UNLIKELY_PORT)
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains(UNLIKELY_PORT), "stderr: {stderr}");
    // Default port 9147 should NOT appear in the hint when the env
    // override is set.
    assert!(
        !stderr.contains("port 9147"),
        "env override should suppress default port in hint; stderr: {stderr}"
    );
}

#[test]
fn motion_record_default_view_name_is_timestamped() {
    // We can't easily verify the timestamped default view name end-
    // to-end because record needs an inspector. But we CAN verify
    // that the command parses fine and reaches the reachability
    // gate (exit 1), which is the next stop after parsing. Anything
    // wrong with the parser would exit 2.
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "record", "--port", UNLIKELY_PORT])
        .env_remove("PULP_INSPECTOR_PORT")
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(
        output.status.code(),
        Some(1),
        "parser should accept defaults; reachability gate should exit 1; stderr: {}",
        String::from_utf8_lossy(&output.stderr),
    );
}

#[test]
fn motion_record_accepts_out_view_fps_metrics() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args([
            "motion",
            "record",
            "--view",
            "Card",
            "--fps",
            "60",
            "--out",
            "/tmp/card.jsonl",
            "--metrics",
            "geometry:frame:card:minX,minY:window:presentation",
            "--port",
            UNLIKELY_PORT,
        ])
        .env_remove("PULP_INSPECTOR_PORT")
        .output()
        .expect("failed to run pulp binary");

    // Parser accepted; reachability gate exits 1. Parser error would
    // exit 2.
    assert_eq!(
        output.status.code(),
        Some(1),
        "stderr: {}",
        String::from_utf8_lossy(&output.stderr),
    );
}

#[test]
fn motion_record_rejects_unknown_record_flag() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args(["motion", "record", "--banana", "yellow"])
        .output()
        .expect("failed to run pulp binary");

    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("unknown argument"), "stderr: {stderr}");
}

#[test]
fn motion_stop_accepts_trace_id_arg() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    let output = Command::new(bin)
        .args([
            "motion",
            "stop",
            "--trace-id",
            "42",
            "--port",
            UNLIKELY_PORT,
        ])
        .env_remove("PULP_INSPECTOR_PORT")
        .output()
        .expect("failed to run pulp binary");

    // Parser accepts the trace_id; reachability gate exits 1.
    assert_eq!(
        output.status.code(),
        Some(1),
        "stderr: {}",
        String::from_utf8_lossy(&output.stderr),
    );
}

#[test]
fn motion_list_traces_alias_works() {
    let bin = env!("CARGO_BIN_EXE_pulp");

    // Both `list` and `list-traces` should resolve to the same
    // dispatch path. Anything else would exit 2 (unknown verb).
    for verb in &["list", "list-traces"] {
        let output = Command::new(bin)
            .args(["motion", verb, "--port", UNLIKELY_PORT])
            .env_remove("PULP_INSPECTOR_PORT")
            .output()
            .expect("failed to run pulp binary");
        assert_eq!(
            output.status.code(),
            Some(1),
            "alias `{verb}` should parse + hit reachability gate; stderr: {}",
            String::from_utf8_lossy(&output.stderr),
        );
    }
}
