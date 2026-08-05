use super::*;
use crate::cmd::inspector_test_support::{s, RecordingTalker};
use crate::cmd::trace::{dispatch, parse, GlobalFlags, InspectorTalker, Sub};

#[test]
fn capture_controls_come_from_effective_capabilities() {
    let controls =
        parse_capture_controls("{\"effective\":[\"session.control\",\"trace.session.control\"]}")
            .unwrap();
    assert!(controls.ready());

    let observe =
        parse_capture_controls("{\"effective\":[\"session.describe\",\"diagnostics.read\"]}")
            .unwrap();
    assert!(!observe.ready());
    let busy = parse_capture_controls(
        "{\"effective\":[\"session.control\",\"trace.session.control\"],\
         \"controller\":\"another-client\"}",
    )
    .unwrap();
    assert!(!busy.ready());
    assert!(parse_capture_controls("{}").is_none());
}

fn tp(source: TraceProcessorSource, path: Option<&str>) -> TraceProcessorStatus {
    TraceProcessorStatus {
        path: path.map(PathBuf::from),
        source,
    }
}

fn controls() -> Option<CaptureControls> {
    Some(CaptureControls {
        session: true,
        trace: true,
        controller_available: true,
    })
}

#[test]
fn doctor_report_all_green_is_ready() {
    let snap = "{\"compiled_in\":true,\"active\":false,\
                \"trace_control_available\":true,\
                \"last_trace_path\":\"/tmp/x.pftrace\"}";
    let status = tp(
        TraceProcessorSource::Path,
        Some("/usr/bin/trace_processor_shell"),
    );
    let human = build_doctor_report(9147, true, controls(), Some(snap), None, &status, false);
    assert!(
        human.contains("inspector (port 9147) ... reachable"),
        "{human}"
    );
    assert!(human.contains("tracing compiled in ..... yes"), "{human}");
    assert!(
        human.contains("last trace .............. /tmp/x.pftrace"),
        "{human}"
    );
    assert!(human.contains("ready to capture a trace . yes"), "{human}");
    assert!(human.contains("ready to query offline ... yes"), "{human}");
}

#[test]
fn doctor_report_unreachable_marks_unknowns_and_prints_hint() {
    let status = tp(TraceProcessorSource::None, None);
    let human = build_doctor_report(9200, false, None, None, None, &status, false);
    assert!(
        human.contains("inspector (port 9200) ... UNREACHABLE"),
        "{human}"
    );
    assert!(
        human.contains("tracing compiled in ..... unknown"),
        "{human}"
    );
    assert!(human.contains("ready to capture a trace . no"), "{human}");
    assert!(human.contains("constructs InspectorServer"), "{human}");
    assert!(
        human.contains("PULP_TRACE_SERVER is not implemented"),
        "{human}"
    );
}

#[test]
fn doctor_report_distinguishes_snapshot_denial_from_unreachable() {
    let status = tp(TraceProcessorSource::None, None);
    let human = build_doctor_report(
        9200,
        true,
        None,
        None,
        Some("capability_denied"),
        &status,
        false,
    );
    assert!(
        human.contains("inspector (port 9200) ... reachable"),
        "{human}"
    );
    assert!(
        human.contains("trace snapshot .......... unavailable (capability_denied)"),
        "{human}"
    );
    assert!(!human.contains("no inspector available"), "{human}");

    let json = build_doctor_report(
        9200,
        true,
        None,
        None,
        Some("capability_denied"),
        &status,
        true,
    );
    let value: serde_json::Value = serde_json::from_str(json.trim()).unwrap();
    assert_eq!(value["inspector_reachable"], true);
    assert_eq!(value["snapshot_error"], "capability_denied");
}

#[test]
fn doctor_report_not_compiled_in_blocks_capture() {
    let snap = "{\"compiled_in\":false,\"active\":false,\
                \"trace_control_available\":true}";
    let status = tp(
        TraceProcessorSource::Path,
        Some("/usr/bin/trace_processor_shell"),
    );
    let human = build_doctor_report(9147, true, controls(), Some(snap), None, &status, false);
    assert!(human.contains("tracing compiled in ..... NO"), "{human}");
    assert!(human.contains("ready to capture a trace . no"), "{human}");
    // No trace yet, so offline query is not ready even with the binary.
    assert!(human.contains("ready to query offline ... no"), "{human}");
}

#[test]
fn doctor_report_requires_both_capture_controls() {
    let snap = "{\"compiled_in\":true,\"active\":false,\
                \"trace_control_available\":true}";
    let status = tp(TraceProcessorSource::None, None);
    let controls = Some(CaptureControls {
        session: true,
        trace: false,
        controller_available: true,
    });
    let json = build_doctor_report(9147, true, controls, Some(snap), None, &status, true);
    let value: serde_json::Value = serde_json::from_str(json.trim()).unwrap();
    assert_eq!(value["session_control_granted"], true);
    assert_eq!(value["trace_control_granted"], false);
    assert_eq!(value["ready_to_capture"], false);
}

#[test]
fn doctor_report_rejects_a_busy_controller_lease() {
    let snap = "{\"compiled_in\":true,\"active\":false,\
                \"trace_control_available\":true}";
    let status = tp(TraceProcessorSource::None, None);
    let controls = Some(CaptureControls {
        session: true,
        trace: true,
        controller_available: false,
    });
    let json = build_doctor_report(9147, true, controls, Some(snap), None, &status, true);
    let value: serde_json::Value = serde_json::from_str(json.trim()).unwrap();
    assert_eq!(value["controller_available"], false);
    assert_eq!(value["ready_to_capture"], false);
}

#[test]
fn doctor_report_rejects_trace_owned_by_another_publication() {
    let snap = "{\"compiled_in\":true,\"active\":true,\
                \"trace_control_available\":false}";
    let status = tp(TraceProcessorSource::None, None);
    let human = build_doctor_report(9147, true, controls(), Some(snap), None, &status, false);
    assert!(
        human.contains("trace control ............ OWNED BY ANOTHER PUBLICATION"),
        "{human}"
    );
    assert!(human.contains("ready to capture a trace . no"), "{human}");
}

#[test]
fn doctor_report_json_shape() {
    let snap = "{\"compiled_in\":true,\"active\":true,\
                \"trace_control_available\":true,\
                \"last_trace_path\":\"/tmp/y.pftrace\"}";
    let status = tp(TraceProcessorSource::Env, Some("/opt/tp"));
    let json = build_doctor_report(9147, true, controls(), Some(snap), None, &status, true);
    // Parses as one flat JSON object with the readiness contract.
    let v: serde_json::Value = serde_json::from_str(json.trim()).unwrap();
    assert_eq!(v["port"], 9147);
    assert_eq!(v["inspector_reachable"], true);
    assert_eq!(v["compiled_in"], true);
    assert_eq!(v["active"], true);
    assert_eq!(v["trace_control_available"], true);
    assert_eq!(v["last_trace_path"], "/tmp/y.pftrace");
    assert_eq!(v["trace_processor_available"], true);
    assert_eq!(v["trace_processor_path"], "/opt/tp");
    assert_eq!(v["trace_processor_source"], "env");
    assert_eq!(v["ready_to_capture"], true);
    assert_eq!(v["ready_to_query"], true);
}

#[test]
fn doctor_report_json_escapes_control_characters() {
    let snap = "{\"compiled_in\":true,\"active\":false,\
                \"trace_control_available\":true,\
                \"last_trace_path\":\"/tmp/line\\ntrace.pftrace\"}";
    let status = tp(TraceProcessorSource::Env, Some("/tmp/trace\nprocessor"));
    let json = build_doctor_report(
        9147,
        true,
        controls(),
        Some(snap),
        Some("snapshot\twarning"),
        &status,
        true,
    );
    let value: serde_json::Value = serde_json::from_str(json.trim()).unwrap();
    assert_eq!(value["last_trace_path"], "/tmp/line\ntrace.pftrace");
    assert_eq!(value["snapshot_error"], "snapshot\twarning");
    assert_eq!(value["trace_processor_path"], "/tmp/trace\nprocessor");
}

#[test]
fn doctor_report_json_nulls_when_unreachable() {
    let status = tp(TraceProcessorSource::None, None);
    let json = build_doctor_report(9147, false, None, None, None, &status, true);
    let v: serde_json::Value = serde_json::from_str(json.trim()).unwrap();
    assert!(v["compiled_in"].is_null());
    assert!(v["trace_control_available"].is_null());
    assert!(v["last_trace_path"].is_null());
    assert!(v["trace_processor_path"].is_null());
    assert_eq!(v["trace_processor_source"], "none");
    assert_eq!(v["ready_to_query"], false);
}

#[test]
fn doctor_report_labels_authenticated_discovery() {
    let status = tp(TraceProcessorSource::None, None);
    let human = build_doctor_report(0, false, None, None, None, &status, false);
    assert!(human.contains("inspector (authenticated discovery)"));
    let json = build_doctor_report(0, false, None, None, None, &status, true);
    let value: serde_json::Value = serde_json::from_str(&json).unwrap();
    assert!(value["port"].is_null());
}

#[test]
fn resolve_trace_processor_honors_env_override() {
    // Serialize with the pinned-tier test: both drive resolution off the
    // process-global PULP_* env vars.
    let _g = crate::cmd::trace_fetch::ENV_MUTEX
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    let mut f = std::env::temp_dir();
    f.push("pulp-doctor-test-tp");
    std::fs::write(&f, b"#!/bin/sh\n").unwrap();
    std::env::set_var("PULP_TRACE_PROCESSOR", &f);
    let status = resolve_trace_processor();
    std::env::remove_var("PULP_TRACE_PROCESSOR");
    let _ = std::fs::remove_file(&f);
    assert_eq!(status.source, TraceProcessorSource::Env);
    assert_eq!(status.path.as_deref(), Some(f.as_path()));
}

#[test]
fn resolve_prefers_pinned_over_path_when_env_unset() {
    let Some(key) = crate::cmd::trace_fetch::host_platform_key() else {
        return;
    };
    let pin = crate::cmd::trace_fetch::pin_for(key).unwrap();
    let _g = crate::cmd::trace_fetch::ENV_MUTEX
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    let prev_tp = std::env::var_os("PULP_TRACE_PROCESSOR");
    let prev_home = std::env::var_os("PULP_HOME");
    std::env::remove_var("PULP_TRACE_PROCESSOR");
    let home = std::env::temp_dir().join(format!("pulp-resolve-pin-{}", std::process::id()));
    let cached = crate::cmd::trace_fetch::pinned_cache_path_under(&home, pin);
    std::fs::create_dir_all(cached.parent().unwrap()).unwrap();
    std::fs::write(&cached, b"pinned tp").unwrap();
    std::env::set_var("PULP_HOME", &home);
    let status = resolve_trace_processor();
    match prev_tp {
        Some(v) => std::env::set_var("PULP_TRACE_PROCESSOR", v),
        None => std::env::remove_var("PULP_TRACE_PROCESSOR"),
    }
    match prev_home {
        Some(v) => std::env::set_var("PULP_HOME", v),
        None => std::env::remove_var("PULP_HOME"),
    }
    let _ = std::fs::remove_dir_all(&home);
    assert_eq!(status.source, TraceProcessorSource::Pinned);
    assert_eq!(status.path.as_deref(), Some(cached.as_path()));
}

#[test]
fn dispatch_doctor_uses_the_authenticated_protocol_connection() {
    let t = RecordingTalker::new(vec![
        "{\"sessionId\":\"session-a\",\"instanceId\":\"instance-b\",\
         \"publicationId\":\"publication-c\",\
         \"effective\":[\"session.control\",\"trace.session.control\"]}",
        "{}",
    ]);
    let mut buf: Vec<u8> = Vec::new();
    let flags = GlobalFlags {
        json: false,
        port: Some(1),
        ..GlobalFlags::default()
    };
    dispatch(&Sub::Doctor, &flags, &t, &mut buf).unwrap();
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("pulp trace doctor"), "{out}");
    assert!(out.contains("reachable"), "{out}");
    assert_eq!(
        &*t.calls.borrow(),
        &[
            (1, "Session.getCapabilities".to_owned(), "{}".to_owned()),
            (1, "Trace.snapshot".to_owned(), "{}".to_owned()),
        ]
    );
    assert_eq!(
        &*t.selections.borrow(),
        &[
            None,
            Some(crate::cmd::inspector::SessionSelection {
                session_id: "session-a".to_owned(),
                instance_id: "instance-b".to_owned(),
                publication_id: "publication-c".to_owned(),
            }),
        ]
    );
}

#[test]
fn dispatch_doctor_upgrades_partial_selection_for_snapshot() {
    let talker = RecordingTalker::new(vec![
        "{\"sessionId\":\"session-a\",\"instanceId\":\"instance-b\",\
         \"publicationId\":\"publication-c\",\
         \"effective\":[\"session.control\",\"trace.session.control\"]}",
        "{}",
    ]);
    let mut output = Vec::new();
    let (sub, flags) = parse(&s(&[
        "doctor",
        "--session",
        "session-a",
        "--instance",
        "instance-b",
    ]))
    .unwrap();
    dispatch(&sub, &flags, &talker, &mut output).unwrap();
    let partial = Some(crate::cmd::inspector::SessionSelection {
        session_id: "session-a".to_owned(),
        instance_id: "instance-b".to_owned(),
        publication_id: String::new(),
    });
    let exact = Some(crate::cmd::inspector::SessionSelection {
        session_id: "session-a".to_owned(),
        instance_id: "instance-b".to_owned(),
        publication_id: "publication-c".to_owned(),
    });
    assert_eq!(&*talker.selections.borrow(), &[partial, exact]);
}

#[test]
fn dispatch_doctor_keeps_reachability_when_snapshot_is_denied() {
    struct SnapshotDeniedTalker;
    impl InspectorTalker for SnapshotDeniedTalker {
        fn call(&self, _port: u16, method: &str, _params: &str) -> Result<String> {
            if method == "Session.getCapabilities" {
                Ok("{\"sessionId\":\"session-a\",\
                     \"instanceId\":\"instance-b\",\
                     \"publicationId\":\"publication-c\"}"
                    .to_owned())
            } else {
                Err(CliError::Other("capability_denied".to_owned()))
            }
        }

        fn call_selected(
            &self,
            port: u16,
            _session_id: &str,
            _instance_id: &str,
            _publication_id: &str,
            method: &str,
            params: &str,
        ) -> Result<String> {
            self.call(port, method, params)
        }
    }

    let mut output = Vec::new();
    run_doctor(9200, None, false, &SnapshotDeniedTalker, &mut output).unwrap();
    let human = String::from_utf8(output).unwrap();
    assert!(
        human.contains("inspector (port 9200) ... reachable"),
        "{human}"
    );
    assert!(human.contains("capability_denied"), "{human}");
    assert!(!human.contains("no inspector available"), "{human}");
}

#[test]
fn dispatch_doctor_does_not_snapshot_without_an_exact_session_identity() {
    let talker = RecordingTalker::new(vec![
        "{\"effective\":[\"session.control\",\"trace.session.control\"]}",
        "{\"compiled_in\":true,\"active\":true}",
    ]);
    let mut output = Vec::new();
    run_doctor(9200, None, true, &talker, &mut output).unwrap();

    let value: serde_json::Value = serde_json::from_slice(&output).unwrap();
    assert_eq!(value["inspector_reachable"], true);
    assert!(value["compiled_in"].is_null());
    assert_eq!(value["ready_to_capture"], false);
    assert!(value["snapshot_error"]
        .as_str()
        .unwrap()
        .contains("sessionId, instanceId, and publicationId"));
    assert_eq!(talker.calls.borrow().len(), 1);
}

#[test]
fn dispatch_doctor_does_not_use_partial_selection_after_capability_denial() {
    struct CapabilitiesDeniedTalker;
    impl InspectorTalker for CapabilitiesDeniedTalker {
        fn call(&self, _port: u16, method: &str, _params: &str) -> Result<String> {
            if method == "Trace.snapshot" {
                Ok("{\"compiled_in\":true}".to_owned())
            } else {
                Err(CliError::Other("capability_denied".to_owned()))
            }
        }

        fn call_selected(
            &self,
            port: u16,
            _session_id: &str,
            _instance_id: &str,
            _publication_id: &str,
            method: &str,
            params: &str,
        ) -> Result<String> {
            self.call(port, method, params)
        }
    }

    let mut output = Vec::new();
    let selection = crate::cmd::inspector::SessionSelection {
        session_id: "session-a".to_owned(),
        instance_id: "instance-b".to_owned(),
        publication_id: String::new(),
    };
    run_doctor(
        9200,
        Some(&selection),
        true,
        &CapabilitiesDeniedTalker,
        &mut output,
    )
    .unwrap();
    let value: serde_json::Value = serde_json::from_slice(&output).unwrap();
    assert_eq!(value["inspector_reachable"], false);
    assert!(value["compiled_in"].is_null());
    assert!(value["session_control_granted"].is_null());
    assert_eq!(value["ready_to_capture"], false);
}
