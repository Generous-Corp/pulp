use super::*;
use crate::cmd::inspector_test_support::{s, RecordingTalker};
use crate::cmd::trace::*;

#[test]
fn dispatch_start_passes_method_and_params() {
    let t = RecordingTalker::new(vec!["{\"out_path\":\"/tmp/pulp-9.pftrace\"}"]);
    let mut buf: Vec<u8> = Vec::new();
    let sub = Sub::Start(StartArgs {
        categories: vec!["dsp".to_owned()],
        ring_mb: None,
    });
    dispatch(&sub, &GlobalFlags::default(), &t, &mut buf).unwrap();
    let calls = t.calls.borrow();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].0, 0);
    assert_eq!(calls[0].1, "Trace.startSession");
    assert!(calls[0].2.contains("\"categories\":[\"dsp\"]"));
    assert_eq!(t.selections.borrow()[0], None);
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("/tmp/pulp-9.pftrace"), "{out}");
    assert!(out.contains("pulp trace stop"), "{out}");
}

#[test]
fn dispatch_start_rejects_legacy_exact_selection() {
    let talker = RecordingTalker::new(vec![]);
    let flags = GlobalFlags {
        session_id: Some("session-a".to_owned()),
        instance_id: Some("instance-b".to_owned()),
        publication_id: Some("publication-c".to_owned()),
        ..GlobalFlags::default()
    };
    let mut output = Vec::new();
    let error = dispatch(
        &Sub::Start(StartArgs::default()),
        &flags,
        &talker,
        &mut output,
    )
    .unwrap_err();
    assert!(
        error.to_string().contains("canonical capability control"),
        "{error}"
    );
    assert!(talker.calls.borrow().is_empty());
}

#[test]
fn dispatch_start_failure_does_not_print_a_stop_command() {
    let talker = RecordingTalker::new(vec![
        "{\"compiled_in\":false,\"active\":false,\"ok\":false,\
         \"message\":\"Tracing is not compiled in\"}",
    ]);
    let flags = GlobalFlags::default();
    let mut output = Vec::new();
    dispatch(
        &Sub::Start(StartArgs::default()),
        &flags,
        &talker,
        &mut output,
    )
    .unwrap();
    let output = String::from_utf8(output).unwrap();
    assert!(output.contains("tracing did not start"), "{output}");
    assert!(output.contains("Tracing is not compiled in"), "{output}");
    assert!(!output.contains("pulp trace stop"), "{output}");
}

#[test]
fn dispatch_start_json_surfaces_resolved_exact_selection() {
    let talker = RecordingTalker::new(vec!["{\"out_path\":\"/tmp/pulp-9.pftrace\"}"]);
    let flags = GlobalFlags {
        json: true,
        ..GlobalFlags::default()
    };
    let mut output = Vec::new();
    dispatch(
        &Sub::Start(StartArgs::default()),
        &flags,
        &talker,
        &mut output,
    )
    .unwrap();
    let value: serde_json::Value = serde_json::from_slice(&output).unwrap();
    assert_eq!(value["out_path"], "/tmp/pulp-9.pftrace");
}

#[test]
fn dispatch_live_command_rejects_removed_exact_session_selection() {
    let talker = RecordingTalker::new(vec![]);
    let error = parse(&s(&[
        "snapshot",
        "--session",
        "session-a",
        "--instance",
        "instance-b",
    ]))
    .unwrap_err();
    assert!(error.to_string().contains("removed"), "{error}");
    assert!(talker.calls.borrow().is_empty());
}

#[test]
fn dispatch_stop_prints_pftrace_path() {
    let t = RecordingTalker::new(vec!["{\"out_path\":\"/tmp/pulp-42.pftrace\"}"]);
    let mut buf: Vec<u8> = Vec::new();
    let flags = GlobalFlags::default();
    dispatch(&Sub::Stop, &flags, &t, &mut buf).unwrap();
    assert_eq!(t.calls.borrow()[0].1, "Trace.stopSession");
    let out = String::from_utf8(buf).unwrap();
    assert_eq!(out.trim(), "/tmp/pulp-42.pftrace");
}

#[test]
fn dispatch_stop_uses_canonical_selection_without_legacy_identifiers() {
    let talker = RecordingTalker::new(vec!["{\"out_path\":\"/tmp/pulp.pftrace\"}"]);
    let mut output = Vec::new();
    dispatch(&Sub::Stop, &GlobalFlags::default(), &talker, &mut output).unwrap();
    assert_eq!(talker.calls.borrow()[0].1, "Trace.stopSession");
    assert_eq!(talker.selections.borrow()[0], None);
}

#[test]
fn dispatch_live_followups_require_exact_selection() {
    let talker = RecordingTalker::new(vec![]);
    for sub in [
        Sub::Query(QueryArgs {
            sql: Some("select 1".to_owned()),
            ..QueryArgs::default()
        }),
        Sub::Explain {
            question: "why slow?".to_owned(),
        },
    ] {
        let mut output = Vec::new();
        let error = dispatch(&sub, &GlobalFlags::default(), &talker, &mut output).unwrap_err();
        assert!(matches!(error, CliError::BadUsage(_)), "{error}");
    }
    assert!(talker.calls.borrow().is_empty());
}

#[test]
fn dispatch_query_preset_verb_has_no_legacy_route() {
    let t = RecordingTalker::new(vec![]);
    let error = parse(&s(&[
        "slowest-frames",
        "--session",
        "session-a",
        "--instance",
        "instance-b",
        "--publication",
        "publication-c",
    ]))
    .unwrap_err();
    assert!(error.to_string().contains("removed"), "{error}");
    assert!(t.calls.borrow().is_empty());
}

#[test]
fn dispatch_json_flag_does_not_restore_live_query() {
    let t = RecordingTalker::new(vec![]);
    let mut buf: Vec<u8> = Vec::new();
    let flags = GlobalFlags {
        json: true,
        port: None,
        session_id: Some("session-a".to_owned()),
        instance_id: Some("instance-b".to_owned()),
        publication_id: Some("publication-c".to_owned()),
        ..GlobalFlags::default()
    };
    let sub = Sub::Query(QueryArgs {
        sql: Some("SELECT name FROM slice".to_owned()),
        preset: None,
        format: QueryFormat::Json,
        ..QueryArgs::default()
    });
    assert!(dispatch(&sub, &flags, &t, &mut buf).is_err());
    assert!(t.calls.borrow().is_empty());
}

#[test]
fn dispatch_explain_has_no_legacy_route() {
    let t = RecordingTalker::new(vec![]);
    let mut buf: Vec<u8> = Vec::new();
    let sub = Sub::Explain {
        question: "why slow to open?".to_owned(),
    };
    let flags = GlobalFlags {
        session_id: Some("session-a".to_owned()),
        instance_id: Some("instance-b".to_owned()),
        publication_id: Some("publication-c".to_owned()),
        ..GlobalFlags::default()
    };
    assert!(dispatch(&sub, &flags, &t, &mut buf).is_err());
    assert!(t.calls.borrow().is_empty());
}

#[test]
fn dispatch_help_prints_usage_without_calling_inspector() {
    let t = RecordingTalker::new(vec![]);
    let mut buf: Vec<u8> = Vec::new();
    dispatch(&Sub::Help, &GlobalFlags::default(), &t, &mut buf).unwrap();
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("pulp trace — wrappers"));
    assert!(t.calls.borrow().is_empty());
}

#[test]
fn no_inspector_hint_names_the_explicit_host_ownership() {
    let s = no_inspector_hint(9200);
    assert!(s.contains("port 9200"), "{s}");
    assert!(s.contains("constructs InspectorServer"), "{s}");
    assert!(s.contains("wires DomainHandler"), "{s}");
    assert!(s.contains("PULP_TRACE_SERVER is not implemented"), "{s}");
}
