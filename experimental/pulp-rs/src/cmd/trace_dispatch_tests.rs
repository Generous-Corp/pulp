use super::*;
use crate::cmd::inspector_test_support::{s, RecordingTalker};
use crate::cmd::trace::*;

#[test]
fn dispatch_start_passes_method_and_params() {
    let t = RecordingTalker::new(vec![
        "{\"sessionId\":\"session-a\",\"instanceId\":\"instance-b\",\
         \"publicationId\":\"publication-c\"}",
        "{\"out_path\":\"/tmp/pulp-9.pftrace\"}",
    ]);
    let mut buf: Vec<u8> = Vec::new();
    let sub = Sub::Start(StartArgs {
        categories: vec!["dsp".to_owned()],
        ring_mb: None,
    });
    dispatch(&sub, &GlobalFlags::default(), &t, &mut buf).unwrap();
    let calls = t.calls.borrow();
    assert_eq!(calls.len(), 2);
    assert_eq!(calls[0].0, 0);
    assert_eq!(calls[0].1, "Session.getCapabilities");
    assert_eq!(calls[1].1, "Trace.startSession");
    assert!(calls[1].2.contains("\"categories\":[\"dsp\"]"));
    assert_eq!(
        t.selections.borrow()[1],
        Some(crate::cmd::inspector::SessionSelection {
            session_id: "session-a".to_owned(),
            instance_id: "instance-b".to_owned(),
            publication_id: "publication-c".to_owned(),
        })
    );
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("/tmp/pulp-9.pftrace"), "{out}");
    assert!(
        out.contains(
            "pulp trace stop --session session-a --instance instance-b \
         --publication publication-c"
        ),
        "{out}"
    );
}

#[test]
fn dispatch_start_preserves_exact_selection_in_stop_hint() {
    let talker = RecordingTalker::new(vec!["{\"compiled_in\":true,\"active\":true,\"ok\":true}"]);
    let flags = GlobalFlags {
        session_id: Some("session-a".to_owned()),
        instance_id: Some("instance-b".to_owned()),
        publication_id: Some("publication-c".to_owned()),
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
    let output = String::from_utf8(output).unwrap();
    assert!(
        output.contains(
            "pulp trace stop --session session-a --instance instance-b \
         --publication publication-c"
        ),
        "{output}"
    );
}

#[test]
fn dispatch_start_failure_does_not_print_a_stop_command() {
    let talker = RecordingTalker::new(vec![
        "{\"compiled_in\":false,\"active\":false,\"ok\":false,\
         \"message\":\"Tracing is not compiled in\"}",
    ]);
    let flags = GlobalFlags {
        session_id: Some("session-a".to_owned()),
        instance_id: Some("instance-b".to_owned()),
        publication_id: Some("publication-c".to_owned()),
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
    let output = String::from_utf8(output).unwrap();
    assert!(output.contains("tracing did not start"), "{output}");
    assert!(output.contains("Tracing is not compiled in"), "{output}");
    assert!(!output.contains("pulp trace stop"), "{output}");
}

#[test]
fn dispatch_start_json_surfaces_resolved_exact_selection() {
    let talker = RecordingTalker::new(vec![
        "{\"sessionId\":\"session-a\",\"instanceId\":\"instance-b\",\
         \"publicationId\":\"publication-c\"}",
        "{\"out_path\":\"/tmp/pulp-9.pftrace\"}",
    ]);
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
    assert_eq!(value["sessionId"], "session-a");
    assert_eq!(value["instanceId"], "instance-b");
    assert_eq!(value["out_path"], "/tmp/pulp-9.pftrace");
}

#[test]
fn dispatch_live_command_uses_exact_session_selection() {
    let talker = RecordingTalker::new(vec!["{}"]);
    let mut output = Vec::new();
    let (sub, flags) = parse(&s(&[
        "snapshot",
        "--session",
        "session-a",
        "--instance",
        "instance-b",
    ]))
    .unwrap();
    dispatch(&sub, &flags, &talker, &mut output).unwrap();
    assert_eq!(
        &*talker.selections.borrow(),
        &[Some(crate::cmd::inspector::SessionSelection {
            session_id: "session-a".to_owned(),
            instance_id: "instance-b".to_owned(),
            publication_id: String::new(),
        })]
    );
}

#[test]
fn dispatch_stop_prints_pftrace_path() {
    let t = RecordingTalker::new(vec!["{\"out_path\":\"/tmp/pulp-42.pftrace\"}"]);
    let mut buf: Vec<u8> = Vec::new();
    let flags = GlobalFlags {
        session_id: Some("session-a".to_owned()),
        instance_id: Some("instance-b".to_owned()),
        publication_id: Some("publication-c".to_owned()),
        ..GlobalFlags::default()
    };
    dispatch(&Sub::Stop, &flags, &t, &mut buf).unwrap();
    assert_eq!(t.calls.borrow()[0].1, "Trace.stopSession");
    let out = String::from_utf8(buf).unwrap();
    assert_eq!(out.trim(), "/tmp/pulp-42.pftrace");
}

#[test]
fn dispatch_stop_requires_exact_selection_before_calling_inspector() {
    let talker = RecordingTalker::new(vec![]);
    let mut output = Vec::new();
    let error = dispatch(&Sub::Stop, &GlobalFlags::default(), &talker, &mut output).unwrap_err();
    assert!(matches!(error, CliError::BadUsage(_)), "{error}");
    assert!(talker.calls.borrow().is_empty());
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
fn dispatch_query_preset_verb_routes_to_trace_query() {
    let t = RecordingTalker::new(vec!["[]"]);
    let mut buf: Vec<u8> = Vec::new();
    let (sub, flags) = parse(&s(&[
        "slowest-frames",
        "--session",
        "session-a",
        "--instance",
        "instance-b",
        "--publication",
        "publication-c",
    ]))
    .unwrap();
    dispatch(&sub, &flags, &t, &mut buf).unwrap();
    let calls = t.calls.borrow();
    assert_eq!(calls[0].1, "Trace.query");
    assert!(calls[0].2.contains("\"preset\":\"slowest-frames\""));
}

#[test]
fn dispatch_json_flag_prints_raw_response() {
    let t = RecordingTalker::new(vec!["[{\"name\":\"process\"}]"]);
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
    dispatch(&sub, &flags, &t, &mut buf).unwrap();
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("[{\"name\":\"process\"}]"), "{out}");
}

#[test]
fn dispatch_explain_prints_narrated_prose() {
    let t = RecordingTalker::new(vec!["{\"explanation\":\"Root cause: font-atlas build.\"}"]);
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
    dispatch(&sub, &flags, &t, &mut buf).unwrap();
    assert_eq!(t.calls.borrow()[0].1, "Trace.explain");
    let out = String::from_utf8(buf).unwrap();
    assert_eq!(out.trim(), "Root cause: font-atlas build.");
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
