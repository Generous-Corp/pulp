use super::*;
use crate::cmd::trace::{TraceProcessorSource, TraceProcessorStatus};

#[test]
fn parses_ranked_rows_and_preserves_evidence() {
    let output = "\"__PULP_GPU_ROW__706970656C696E652D70726570617265|800000|3031323334353637383961626364656630313233343536373839616263646566||6865616C746879|1|0|636F6C64|-1|0|0|0\"\n";
    let rows = parse_rows(output).unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].stage, "pipeline-prepare");
    assert_eq!(
        rows[0].evidence_id.as_deref(),
        Some("0123456789abcdef0123456789abcdef")
    );
    assert_eq!(rows[0].timing_phase, "cold");
    assert_eq!(rows[0].cpu_running_ns, None);
}

#[test]
fn parses_capture_integrity_counts() {
    let integrity = parse_integrity("__PULP_GPU_INTEGRITY__12|1|3|4\n").unwrap();
    assert_eq!(integrity.slice_count, 12);
    assert_eq!(integrity.incomplete_slice_count, 1);
    assert_eq!(integrity.data_loss_count, 3);
    assert_eq!(integrity.no_flush_count, 4);
}

#[test]
fn parses_bounded_trace_categories_without_trusting_stage_names() {
    let evidence = "3031323334353637383961626364656630313233343536373839616263646566";
    let output = format!(
        "__PULP_GPU_CATEGORY__677075|{evidence}|7|42\n\
         __PULP_GPU_CATEGORY__72656E6465722C74657874|{evidence}|7|42\n\
         __PULP_GPU_CATEGORY__2E2E2F756E73616665|{evidence}|7|42\n"
    );
    let scopes = parse_category_scopes(&output);
    assert_eq!(scopes.len(), 2);
    assert_eq!(scopes[0].categories, vec!["gpu"]);
    assert_eq!(scopes[1].categories, vec!["render", "text"]);
    assert!(scopes.iter().all(|scope| {
        scope.evidence_id == "0123456789abcdef0123456789abcdef"
            && scope.process_upid == 7
            && scope.process_pid == 42
    }));
}

#[test]
fn category_scope_excludes_other_evidence_and_rejects_cross_process_reuse() {
    let evidence = "0123456789abcdef0123456789abcdef";
    let row = RawRow {
        stage: "probe".to_owned(),
        duration_ns: 42,
        evidence_id: Some(evidence.to_owned()),
        diagnostic_code: None,
        health_state: Some("healthy".to_owned()),
        sequence: Some(1),
        frame_index: None,
        timing_phase: "not-applicable".to_owned(),
        cpu_running_ns: None,
        scheduler_evidence: false,
        incomplete: false,
        failure: false,
    };
    let own = RawCategoryScope {
        categories: vec!["gpu".to_owned()],
        evidence_id: evidence.to_owned(),
        process_upid: 7,
        process_pid: 42,
    };
    let unrelated = RawCategoryScope {
        categories: vec!["js".to_owned(), "layout".to_owned(), "render".to_owned()],
        evidence_id: "ffffffffffffffffffffffffffffffff".to_owned(),
        process_upid: 8,
        process_pid: 43,
    };
    let (categories, scope, ambiguous) =
        correlated_categories(std::slice::from_ref(&row), &[own.clone(), unrelated]);
    assert_eq!(categories, vec!["gpu"]);
    assert_eq!(scope.unwrap().process_upid, 7);
    assert!(!ambiguous);

    let reused_in_other_process = RawCategoryScope {
        categories: vec!["text".to_owned()],
        evidence_id: evidence.to_owned(),
        process_upid: 9,
        process_pid: 44,
    };
    let (categories, scope, ambiguous) =
        correlated_categories(&[row], &[own, reused_in_other_process]);
    assert!(categories.is_empty());
    assert!(scope.is_none());
    assert!(ambiguous);
}

#[test]
fn empty_and_incomplete_captures_fail_closed() {
    let empty = result_from_rows(GpuQuestion::GpuHealth, Path::new("/tmp/a.pftrace"), vec![]);
    assert_eq!(empty.verdict, "unavailable");
    assert_eq!(
        empty.unavailable_reason,
        Some("empty-or-never-flushed-capture")
    );

    let incomplete = RawRow {
        stage: "pipeline-prepare".to_owned(),
        duration_ns: -1,
        evidence_id: Some("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_owned()),
        diagnostic_code: None,
        health_state: Some("healthy".to_owned()),
        sequence: Some(1),
        frame_index: None,
        timing_phase: "cold".to_owned(),
        cpu_running_ns: None,
        scheduler_evidence: false,
        incomplete: true,
        failure: false,
    };
    let result = result_from_rows(
        GpuQuestion::GpuStartup,
        Path::new("/tmp/a.pftrace"),
        vec![incomplete],
    );
    assert_eq!(result.unavailable_reason, Some("incomplete-capture"));
    assert!(!result.capture_complete);
}

#[test]
fn scheduler_attribution_and_data_loss_are_not_inferred_from_wall_time() {
    let row = RawRow {
        stage: "acquire".to_owned(),
        duration_ns: 1_000,
        evidence_id: Some("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_owned()),
        diagnostic_code: None,
        health_state: Some("healthy".to_owned()),
        sequence: Some(1),
        frame_index: Some(0),
        timing_phase: "cold".to_owned(),
        cpu_running_ns: Some(100),
        scheduler_evidence: true,
        incomplete: false,
        failure: false,
    };
    let measured = result_from_rows(
        GpuQuestion::GpuStartup,
        Path::new("/tmp/measured.pftrace"),
        vec![row.clone()],
    );
    assert_eq!(measured.contributors[0].execution_state, "wait-dominated");
    assert_eq!(measured.contributors[0].cpu_running_ns, Some(100));
    assert_eq!(measured.contributors[0].non_running_ns, Some(900));
    assert_eq!(measured.next_actions[0].code, "inspect-surface-blocking");

    let lost = result_from_rows_and_categories(
        GpuQuestion::GpuStartup,
        Path::new("/tmp/lost.pftrace"),
        vec![row],
        Vec::new(),
        CaptureIntegrity {
            slice_count: 1,
            incomplete_slice_count: 0,
            data_loss_count: 2,
            no_flush_count: 0,
            processor_reported_truncated: false,
        },
    );
    assert_eq!(lost.verdict, "unavailable");
    assert_eq!(lost.unavailable_reason, Some("trace-data-loss"));
    assert!(!lost.capture_complete);
}

#[test]
fn startup_is_unverified_until_a_versioned_budget_exists() {
    let row = RawRow {
        stage: "pipeline-prepare".to_owned(),
        duration_ns: 800_000,
        evidence_id: Some("0123456789abcdef0123456789abcdef".to_owned()),
        diagnostic_code: None,
        health_state: Some("healthy".to_owned()),
        sequence: Some(1),
        frame_index: Some(0),
        timing_phase: "cold".to_owned(),
        cpu_running_ns: None,
        scheduler_evidence: false,
        incomplete: false,
        failure: false,
    };
    let result = result_from_rows(
        GpuQuestion::GpuStartup,
        Path::new("/tmp/a.pftrace"),
        vec![row],
    );
    assert_eq!(result.verdict, "unverified");
    assert!(result.capture_complete);
}

#[test]
fn render_only_startup_capture_fails_closed() {
    let row = RawRow {
        stage: "frame".to_owned(),
        duration_ns: 300_000,
        evidence_id: Some("55555555555555555555555555555555".to_owned()),
        diagnostic_code: None,
        health_state: Some("healthy".to_owned()),
        sequence: Some(1),
        frame_index: Some(0),
        timing_phase: "cold".to_owned(),
        cpu_running_ns: None,
        scheduler_evidence: false,
        incomplete: false,
        failure: false,
    };
    let result = result_from_rows(
        GpuQuestion::GpuStartup,
        Path::new("/tmp/render-only.pftrace"),
        vec![row],
    );
    assert_eq!(result.verdict, "unavailable");
    assert_eq!(result.unavailable_reason, Some("missing-question-category"));
    assert!(!result.capture_complete);
}

#[test]
fn perfetto_open_command_shell_quotes_untrusted_paths() {
    let result = result_from_rows(
        GpuQuestion::GpuHealth,
        Path::new("/tmp/a; touch PWNED ' $(false).pftrace"),
        vec![],
    );
    assert_eq!(
        result.ui_correlation.open_command,
        "pulp trace open -- '/tmp/a; touch PWNED '\"'\"' $(false).pftrace'"
    );
}

#[cfg(unix)]
#[test]
fn exclusive_sql_creation_does_not_follow_symlinks() {
    use std::os::unix::fs::symlink;

    let root = std::env::temp_dir().join(format!(
        "pulp-gpu-analysis-exclusive-{}",
        std::process::id()
    ));
    let _ = std::fs::remove_dir(&root);
    std::fs::create_dir(&root).unwrap();
    let victim = root.join("victim.txt");
    let candidate = root.join("candidate.sql");
    std::fs::write(&victim, b"preserve-me").unwrap();
    symlink(&victim, &candidate).unwrap();

    let error = create_exclusive_sql(&candidate, "SELECT 1;").unwrap_err();
    assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
    assert_eq!(std::fs::read(&victim).unwrap(), b"preserve-me");

    std::fs::remove_file(candidate).unwrap();
    std::fs::remove_file(victim).unwrap();
    std::fs::remove_dir(root).unwrap();
}

#[test]
fn probe_rejects_malformed_evidence_correlation() {
    let row = RawRow {
        stage: "readback".to_owned(),
        duration_ns: 42,
        evidence_id: Some("not-a-bounded-evidence-id".to_owned()),
        diagnostic_code: None,
        health_state: Some("healthy".to_owned()),
        sequence: Some(1),
        frame_index: Some(0),
        timing_phase: "not-applicable".to_owned(),
        cpu_running_ns: None,
        scheduler_evidence: false,
        incomplete: false,
        failure: false,
    };
    let result = result_from_rows(
        GpuQuestion::GpuProbe,
        Path::new("/tmp/a.pftrace"),
        vec![row],
    );
    assert_eq!(result.verdict, "unavailable");
    assert_eq!(
        result.unavailable_reason,
        Some("invalid-evidence-correlation")
    );
}

#[test]
fn health_and_probe_never_launder_non_pass_states() {
    for question in [GpuQuestion::GpuHealth, GpuQuestion::GpuProbe] {
        for (state, verdict, reason) in [
            (
                Some("unavailable"),
                "unavailable",
                Some("reported-unavailable"),
            ),
            (Some("unverified"), "unverified", None),
            (None, "unavailable", Some("invalid-health-state")),
        ] {
            let row = RawRow {
                stage: "probe".to_owned(),
                duration_ns: 42,
                evidence_id: Some("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_owned()),
                diagnostic_code: None,
                health_state: state.map(str::to_owned),
                sequence: Some(1),
                frame_index: Some(0),
                timing_phase: "not-applicable".to_owned(),
                cpu_running_ns: None,
                scheduler_evidence: false,
                incomplete: false,
                failure: false,
            };
            let result = result_from_rows(question, Path::new("/tmp/a.pftrace"), vec![row]);
            assert_eq!(result.verdict, verdict);
            assert_eq!(result.unavailable_reason, reason);
            assert_ne!(result.verdict, "pass");
            if state.is_some() {
                assert!(result.capture_complete);
            }
        }
    }
}

#[test]
fn checked_in_gpu_views_keep_the_safe_sql_contract() {
    for sql in [STARTUP_SQL, HEALTH_SQL, PROBE_SQL] {
        assert!(sql.contains("CREATE OR REPLACE PERFETTO VIEW"));
        assert!(sql.contains(" GLOB "));
        assert!(sql.contains("EXTRACT_ARG"));
        assert!(sql.contains("dur >= 0"));
        assert!(!sql.contains(" LIKE "));
    }
    assert!(STARTUP_SQL.contains("thread_state"));
    assert!(STARTUP_SQL.contains("tt.utid"));
    assert!(STARTUP_SQL.contains("measured_state_coverage_ns = dur"));
    assert!(STARTUP_SQL.contains("'cold'"));
    assert!(STARTUP_SQL.contains("'steady'"));
    assert!(STARTUP_SQL.contains("'unknown'"));
}

#[cfg(unix)]
#[test]
fn processor_failure_remains_an_error() {
    use std::os::unix::fs::PermissionsExt;
    let root = std::env::temp_dir().join(format!("pulp-gpu-analysis-{}", std::process::id()));
    let trace = root.with_extension("pftrace");
    let processor = root.with_extension("sh");
    std::fs::write(&trace, b"trace").unwrap();
    std::fs::write(&processor, b"#!/bin/sh\necho broken 1>&2\nexit 7\n").unwrap();
    let mut permissions = std::fs::metadata(&processor).unwrap().permissions();
    permissions.set_mode(0o755);
    std::fs::set_permissions(&processor, permissions).unwrap();
    let args = GpuAnalysisArgs {
        question: GpuQuestion::GpuHealth,
        trace: trace.clone(),
    };
    let tp = TraceProcessorStatus {
        path: Some(processor.clone()),
        source: TraceProcessorSource::Env,
    };
    let error = run_gpu_analysis_with_processor(&args, &tp, true, &mut Vec::new()).unwrap_err();
    let _ = std::fs::remove_file(trace);
    let _ = std::fs::remove_file(processor);
    assert!(format!("{error}").contains("exited with 7"));
}

#[test]
fn oversized_trace_is_rejected_before_processor_launch() {
    let trace = tempfile::NamedTempFile::new().unwrap();
    trace.as_file().set_len(MAX_TRACE_BYTES + 1).unwrap();
    let args = GpuAnalysisArgs {
        question: GpuQuestion::GpuHealth,
        trace: trace.path().to_path_buf(),
    };
    let tp = TraceProcessorStatus {
        path: None,
        source: TraceProcessorSource::None,
    };
    let error = run_gpu_analysis_with_processor(&args, &tp, true, &mut Vec::new()).unwrap_err();
    let message = format!("{error}");
    assert!(message.contains("maximum supported analysis input"));
    assert!(message.contains(&(MAX_TRACE_BYTES + 1).to_string()));
    assert!(message.contains(&MAX_TRACE_BYTES.to_string()));
}

#[test]
fn production_processor_limits_are_explicit() {
    assert_eq!(MAX_TRACE_BYTES, 512 * 1024 * 1024);
    assert_eq!(MAX_PROCESSOR_OUTPUT_BYTES, 4 * 1024 * 1024);
    assert_eq!(PROCESSOR_DEADLINE, Duration::from_secs(120));
}

#[test]
fn disconnected_readers_keep_live_child_polling_bounded_without_delaying_exit() {
    let wait = Duration::from_millis(17);
    let mut sleeps = Vec::new();

    // Model three successive live-child polls after both output readers close.
    // The injected sleeper makes the cadence proof deterministic: each poll
    // consumes exactly one wait interval instead of relying on wall-clock timing.
    for _ in 0..3 {
        pace_disconnected_processor_poll(true, true, wait, |duration| {
            sleeps.push(duration)
        });
    }
    assert_eq!(sleeps, vec![wait; 3]);

    // Once try_wait observes exit, the same disconnected-reader state must not
    // add another interval. Connected readers are already paced by recv_timeout.
    pace_disconnected_processor_poll(true, false, wait, |duration| sleeps.push(duration));
    pace_disconnected_processor_poll(false, true, wait, |duration| sleeps.push(duration));
    assert_eq!(sleeps, vec![wait; 3]);
}

#[cfg(unix)]
fn executable_script(root: &Path, name: &str, body: &str) -> PathBuf {
    use std::os::unix::fs::PermissionsExt;

    let path = root.join(name);
    std::fs::write(&path, body).unwrap();
    let mut permissions = std::fs::metadata(&path).unwrap().permissions();
    permissions.set_mode(0o755);
    std::fs::set_permissions(&path, permissions).unwrap();
    path
}

#[cfg(unix)]
#[test]
fn hanging_processor_hits_deadline_and_terminates_descendants() {
    let root = tempfile::tempdir().unwrap();
    let heartbeat = root.path().join("descendant-heartbeat");
    let ready = root.path().join("descendant-ready");
    let processor = executable_script(
        root.path(),
        "hanging-processor.sh",
        &format!(
            "#!/bin/sh\n(printf x >> {0}; touch {1}; while :; do printf x >> {0}; sleep 0.01; done) &\nwhile [ ! -f {1} ]; do sleep 0.01; done\nwait\n",
            shell_quote(&heartbeat.to_string_lossy()),
            shell_quote(&ready.to_string_lossy()),
        ),
    );
    let sql = root.path().join("query.sql");
    let trace = root.path().join("trace.pftrace");
    std::fs::write(&heartbeat, b"p").unwrap();
    std::fs::write(&sql, b"SELECT 1;").unwrap();
    std::fs::write(&trace, b"trace").unwrap();

    let error = run_processor_bounded(
        &processor,
        &sql,
        &trace,
        ProcessorLimits {
            deadline: Duration::from_secs(1),
            max_output_bytes: 16 * 1024,
        },
    )
    .unwrap_err();
    assert!(format!("{error}").contains("analysis deadline"));
    let stopped_at = std::fs::metadata(&heartbeat).unwrap().len();
    assert!(stopped_at > 1, "the descendant heartbeat never ran");
    thread::sleep(Duration::from_millis(150));
    assert_eq!(
        std::fs::metadata(&heartbeat).unwrap().len(),
        stopped_at,
        "a descendant survived process-group termination"
    );
}

#[cfg(unix)]
#[test]
fn flooding_processor_hits_output_cap_and_terminates_descendants() {
    let root = tempfile::tempdir().unwrap();
    let heartbeat = root.path().join("flood-heartbeat");
    let processor = executable_script(
        root.path(),
        "flooding-processor.sh",
        &format!(
            "#!/bin/sh\n(while :; do printf '0123456789abcdef'; printf x >> {}; done) &\nwait\n",
            shell_quote(&heartbeat.to_string_lossy())
        ),
    );
    let sql = root.path().join("query.sql");
    let trace = root.path().join("trace.pftrace");
    std::fs::write(&sql, b"SELECT 1;").unwrap();
    std::fs::write(&trace, b"trace").unwrap();

    let error = run_processor_bounded(
        &processor,
        &sql,
        &trace,
        ProcessorLimits {
            deadline: Duration::from_secs(2),
            max_output_bytes: 16 * 1024,
        },
    )
    .unwrap_err();
    assert!(format!("{error}").contains("output exceeded the 16384 byte limit"));
    let stopped_at = std::fs::metadata(&heartbeat).unwrap().len();
    thread::sleep(Duration::from_millis(150));
    assert_eq!(
        std::fs::metadata(&heartbeat).unwrap().len(),
        stopped_at,
        "a flooding descendant survived process-group termination"
    );
}
