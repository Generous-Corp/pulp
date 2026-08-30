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
    assert!(PROBE_SQL.contains("FROM candidates AS unbound_tooling"));
    assert!(PROBE_SQL.contains("unbound_tooling.evidence_id IS NULL"));
    assert!(PROBE_SQL.contains("unbound_tooling.name GLOB 'gpu_probe*'"));
    assert!(PROBE_SQL.contains("unbound_tooling.name GLOB 'gpu_readback*'"));
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

#[cfg(unix)]
#[test]
fn trace_replacement_during_snapshot_is_rejected_before_processor_launch() {
    let root = tempfile::tempdir().unwrap();
    let trace = root.path().join("trace.pftrace");
    let replacement = root.path().join("replacement.pftrace");
    std::fs::write(&trace, b"original-trace").unwrap();
    std::fs::write(&replacement, b"replacement-trace").unwrap();

    let error = snapshot_trace_input_with_hook(&trace, MAX_TRACE_BYTES, || {
        std::fs::rename(&replacement, &trace).unwrap();
    })
    .unwrap_err();

    assert!(format!("{error}").contains("changed while it was being snapshotted"));
}

#[cfg(unix)]
#[test]
fn processor_receives_only_the_private_trace_snapshot() {
    let root = tempfile::tempdir().unwrap();
    let trace = root.path().join("caller-trace.pftrace");
    let observed_path = root.path().join("processor-path");
    let observed_bytes = root.path().join("processor-bytes");
    let processor = executable_script(
        root.path(),
        "snapshot-observer.sh",
        &format!(
            "#!/bin/sh\nprintf '%s' \"$3\" > {}\ncat \"$3\" > {}\nprintf '%s\\n' '__PULP_GPU_INTEGRITY__0|0|0|0'\n",
            shell_quote(&observed_path.to_string_lossy()),
            shell_quote(&observed_bytes.to_string_lossy()),
        ),
    );
    std::fs::write(&trace, b"caller-owned-trace").unwrap();
    let args = GpuAnalysisArgs {
        question: GpuQuestion::GpuHealth,
        trace: trace.clone(),
    };
    let tp = TraceProcessorStatus {
        path: Some(processor),
        source: TraceProcessorSource::Env,
    };

    let status = run_gpu_analysis_with_processor(&args, &tp, true, &mut Vec::new()).unwrap();
    assert_eq!(status, TraceCommandStatus::Unavailable);
    let processor_path = PathBuf::from(std::fs::read_to_string(&observed_path).unwrap());
    assert_ne!(processor_path, trace);
    assert_eq!(
        std::fs::read(&observed_bytes).unwrap(),
        b"caller-owned-trace"
    );
    assert!(
        !processor_path.exists(),
        "the private trace snapshot survived analysis"
    );
}

#[test]
fn trace_growth_during_snapshot_is_rejected_at_the_copy_boundary() {
    let trace = tempfile::NamedTempFile::new().unwrap();
    std::fs::write(trace.path(), b"tiny").unwrap();

    let error = snapshot_trace_input_with_hook(trace.path(), 8, || {
        let writer = OpenOptions::new().append(true).open(trace.path()).unwrap();
        writer.set_len(9).unwrap();
    })
    .unwrap_err();

    assert!(format!("{error}").contains("exceeded the 8 byte analysis limit"));
}

#[cfg(unix)]
#[test]
fn symlinked_trace_input_is_rejected() {
    use std::os::unix::fs::symlink;

    let root = tempfile::tempdir().unwrap();
    let target = root.path().join("target.pftrace");
    let link = root.path().join("trace.pftrace");
    std::fs::write(&target, b"trace").unwrap();
    symlink(&target, &link).unwrap();

    let error = snapshot_trace_input(&link).unwrap_err();
    assert!(format!("{error}").contains("non-symlink regular file"));
}

#[cfg(unix)]
#[test]
fn fifo_trace_input_is_rejected_without_blocking_before_fstat() {
    use std::ffi::CString;
    use std::os::unix::ffi::OsStrExt;
    use std::sync::mpsc;

    let root = tempfile::tempdir().unwrap();
    let fifo = root.path().join("trace.pftrace");
    let fifo_c = CString::new(fifo.as_os_str().as_bytes()).unwrap();
    assert_eq!(unsafe { libc::mkfifo(fifo_c.as_ptr(), 0o600) }, 0);

    let (sender, receiver) = mpsc::channel();
    std::thread::spawn(move || {
        sender
            .send(snapshot_trace_input(&fifo).map(|_| ()))
            .unwrap();
    });
    let result = receiver
        .recv_timeout(Duration::from_secs(1))
        .expect("opening a FIFO must remain nonblocking");
    assert!(format!("{}", result.unwrap_err()).contains("non-symlink regular file"));
}

#[cfg(unix)]
#[test]
fn unix_trace_open_source_contract_opens_nofollow_nonblocking_before_fstat() {
    let source = include_str!("trace_gpu_analysis.rs");
    assert!(source.contains("libc::O_NOFOLLOW | libc::O_NONBLOCK"));
    assert!(source.contains("let file = options.open(path)?;\n    let (identity, len"));
    assert!(!source.contains("std::fs::symlink_metadata(path)"));
    assert!(!source.contains("File::open(path)"));
}

#[cfg(windows)]
#[test]
fn windows_trace_file_ids_distinguish_same_sized_regular_files() {
    let root = tempfile::tempdir().unwrap();
    let first = root.path().join("first.pftrace");
    let second = root.path().join("second.pftrace");
    std::fs::write(&first, b"same").unwrap();
    std::fs::write(&second, b"same").unwrap();

    let (_, first_identity, first_len) = open_trace_source(&first).unwrap();
    let (_, second_identity, second_len) = open_trace_source(&second).unwrap();
    assert_eq!(first_len, second_len);
    assert_ne!(first_identity, second_identity);
}

#[cfg(windows)]
#[test]
fn windows_reparse_trace_input_is_rejected_when_symlinks_are_available() {
    use std::os::windows::fs::symlink_file;

    let root = tempfile::tempdir().unwrap();
    let target = root.path().join("target.pftrace");
    let link = root.path().join("link.pftrace");
    std::fs::write(&target, b"trace").unwrap();
    if symlink_file(&target, &link).is_err() {
        return;
    }

    let error = snapshot_trace_input(&link).unwrap_err();
    assert!(format!("{error}").contains("non-symlink regular file"));
}

#[cfg(not(windows))]
#[test]
fn windows_trace_open_source_contract_is_handle_identity_and_reparse_safe() {
    let source = include_str!("trace_gpu_analysis.rs");
    assert!(source.contains("FILE_FLAG_OPEN_REPARSE_POINT"));
    assert!(source.contains("FILE_FLAG_BACKUP_SEMANTICS"));
    assert!(source.contains("FILE_FLAG_SEQUENTIAL_SCAN"));
    assert!(source.contains("security_qos_flags(SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION)"));
    assert!(source.contains("FILE_ATTRIBUTE_REPARSE_POINT"));
    assert!(source.contains("GetFileInformationByHandleEx"));
    assert!(source.contains("FILE_ID_INFO_CLASS"));
    assert!(source.contains("FILE_ATTRIBUTE_TAG_INFO_CLASS"));
    assert!(source.contains("VolumeSerialNumber"));
    assert!(source.contains("file_id: identity.FileId"));
    assert!(source.contains("identity.FileId == [0xff; 16]"));
    assert!(source.contains("opened_identity != final_path_identity"));
    assert!(!source.contains("left.modified().ok() == right.modified().ok()"));
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
        pace_disconnected_processor_poll(true, true, wait, |duration| sleeps.push(duration));
    }
    assert_eq!(sleeps, vec![wait; 3]);

    // Once try_wait observes exit, the same disconnected-reader state must not
    // add another interval. Connected readers are already paced by recv_timeout.
    pace_disconnected_processor_poll(true, false, wait, |duration| sleeps.push(duration));
    pace_disconnected_processor_poll(false, true, wait, |duration| sleeps.push(duration));
    assert_eq!(sleeps, vec![wait; 3]);
}

#[cfg(unix)]
#[test]
fn bounded_processor_wires_disconnected_readers_to_poll_pacing() {
    let root = tempfile::tempdir().unwrap();
    let processor = executable_script(
        root.path(),
        "closed-stream-processor.sh",
        "#!/bin/sh\nexec 1>&- 2>&-\nsleep 0.2\nexit 0\n",
    );
    let sql = root.path().join("query.sql");
    let trace = root.path().join("trace.pftrace");
    std::fs::write(&sql, b"SELECT 1;").unwrap();
    std::fs::write(&trace, b"trace").unwrap();

    DISCONNECTED_PROCESSOR_POLL_PACE_COUNT.with(|count| count.set(0));
    let output = run_processor_bounded(
        &processor,
        &sql,
        &trace,
        ProcessorLimits {
            deadline: Duration::from_secs(2),
            max_output_bytes: 1024,
        },
    )
    .unwrap();
    assert!(output.status.success());
    let paced_polls = DISCONNECTED_PROCESSOR_POLL_PACE_COUNT.with(|count| count.get());
    assert!(
        paced_polls > 0,
        "the production collector skipped pacing after both readers disconnected"
    );
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
