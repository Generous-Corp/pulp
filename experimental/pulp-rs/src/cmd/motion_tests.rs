use super::*;
use crate::cmd::inspector_test_support::{s, RecordingTalker};

#[test]
fn parse_empty_yields_help() {
    let (sub, _g) = parse(&[]).unwrap();
    assert!(matches!(sub, Sub::Help));
}

#[test]
fn parse_help_aliases() {
    for a in &["help", "--help", "-h"] {
        let (sub, _) = parse(&s(&[a])).unwrap();
        assert!(matches!(sub, Sub::Help), "{a}");
    }
}

#[test]
fn parse_global_json_in_any_position() {
    let (_sub, g) = parse(&s(&["--json", "snapshot"])).unwrap();
    assert!(g.json);
    let (_sub, g) = parse(&s(&["snapshot", "--json"])).unwrap();
    assert!(g.json);
}

#[test]
fn parse_port_override() {
    let (_sub, g) = parse(&s(&["--port", "9200", "snapshot"])).unwrap();
    assert_eq!(g.port, Some(9200));
}

#[test]
fn parse_port_rejects_garbage() {
    let err = parse(&s(&["--port", "nope", "snapshot"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_port_rejects_zero() {
    let err = parse(&s(&["--port", "0", "snapshot"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_exact_publication_selection_accepts_all_identifiers() {
    let (_, flags) = parse(&s(&[
        "play",
        "--session",
        "session-a",
        "--instance",
        "instance-b",
        "--publication",
        "publication-c",
    ]))
    .unwrap();
    assert_eq!(flags.session_id.as_deref(), Some("session-a"));
    assert_eq!(flags.instance_id.as_deref(), Some("instance-b"));
    assert_eq!(flags.publication_id.as_deref(), Some("publication-c"));

    for args in [
        s(&["play", "--session", "session-a"]),
        s(&["play", "--instance", "instance-b"]),
        s(&["play", "--publication", "publication-c"]),
        s(&["play", "--session", "session a", "--instance", "instance-b"]),
    ] {
        let err = parse(&args).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }
}

#[test]
fn zero_argument_verbs_reject_unconsumed_arguments() {
    for args in [
        s(&["play", "--session-id", "silently-ignored"]),
        s(&["pause", "extra"]),
        s(&["snapshot", "extra"]),
        s(&["list-traces", "extra"]),
        s(&["scrub", "1", "extra"]),
        s(&["cost", "enable", "extra"]),
    ] {
        let err = parse(&args).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }
}

#[test]
fn parse_unknown_verb_is_unknown() {
    let err = parse(&s(&["blarg"])).unwrap_err();
    assert!(matches!(err, CliError::UnknownSubcommand));
}

#[test]
fn parse_record_default_view_name_is_timestamped() {
    let (sub, _) = parse(&s(&["record"])).unwrap();
    let Sub::Record(r) = sub else {
        panic!("expected record")
    };
    assert!(r.view_name.starts_with("motion-"));
    assert!(r.out.is_none());
}

#[test]
fn parse_record_with_out_and_view() {
    let (sub, _) = parse(&s(&["record", "--view", "Card", "--out", "card.jsonl"])).unwrap();
    let Sub::Record(r) = sub else {
        panic!("expected record")
    };
    assert_eq!(r.view_name, "Card");
    assert_eq!(r.out.as_deref(), Some(Path::new("card.jsonl")));
}

#[test]
fn parse_record_metrics_collects_multiple() {
    let (sub, _) = parse(&s(&[
        "record",
        "--view",
        "Card",
        "--metrics",
        "geometry:frame:card:minX,minY:window:presentation",
        "--metrics",
        "scroll-geometry:scroll:scrollview",
    ]))
    .unwrap();
    let Sub::Record(r) = sub else { panic!() };
    assert_eq!(r.metrics.len(), 2);
}

#[test]
fn parse_stop_with_trace_id() {
    let (sub, _) = parse(&s(&["stop", "--trace-id", "7"])).unwrap();
    assert!(matches!(sub, Sub::Stop { trace_id: Some(7) }));
}

#[test]
fn parse_stop_without_trace_id_defaults_to_none() {
    let (sub, _) = parse(&s(&["stop"])).unwrap();
    assert!(matches!(sub, Sub::Stop { trace_id: None }));
}

#[test]
fn parse_scrub_requires_frame_argument() {
    let err = parse(&s(&["scrub"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_scrub_rejects_non_numeric_frame() {
    let err = parse(&s(&["scrub", "foo"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_scrub_accepts_negative_frame() {
    // Inspector itself enforces frame >= 0 — the CLI passes
    // through whatever the user typed.
    let (sub, _) = parse(&s(&["scrub", "-1"])).unwrap();
    assert!(matches!(sub, Sub::Scrub { frame: -1 }));
}

#[test]
fn load_fixture_is_explicitly_unavailable() {
    let err = parse(&s(&["load-fixture", "/tmp/a.jsonl"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(message)
            if message.contains("unavailable")
                && message.contains("server-side filesystem path")));
}

#[test]
fn parse_cost_recognises_enable_disable_aliases() {
    let (sub, _) = parse(&s(&["cost", "enable"])).unwrap();
    assert!(matches!(sub, Sub::Cost { enable: true }));
    let (sub, _) = parse(&s(&["cost", "on"])).unwrap();
    assert!(matches!(sub, Sub::Cost { enable: true }));
    let (sub, _) = parse(&s(&["cost", "disable"])).unwrap();
    assert!(matches!(sub, Sub::Cost { enable: false }));
    let (sub, _) = parse(&s(&["cost", "off"])).unwrap();
    assert!(matches!(sub, Sub::Cost { enable: false }));
}

#[test]
fn parse_cost_rejects_unknown_action() {
    let err = parse(&s(&["cost", "toggle"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_list_traces_alias() {
    let (sub, _) = parse(&s(&["list"])).unwrap();
    assert!(matches!(sub, Sub::ListTraces));
    let (sub, _) = parse(&s(&["list-traces"])).unwrap();
    assert!(matches!(sub, Sub::ListTraces));
}

#[test]
fn to_inspector_call_methods_match_protocol() {
    let mk_record = || {
        Sub::Record(RecordArgs {
            view_name: "Card".to_owned(),
            fps: Some(30),
            out: None,
            metrics: vec![],
        })
    };
    assert_eq!(
        to_inspector_call(&mk_record()).unwrap().0,
        "Motion.startTrace"
    );
    assert_eq!(
        to_inspector_call(&Sub::Stop { trace_id: Some(1) })
            .unwrap()
            .0,
        "Motion.stopTrace"
    );
    assert_eq!(
        to_inspector_call(&Sub::Snapshot).unwrap().0,
        "Motion.snapshot"
    );
    assert_eq!(
        to_inspector_call(&Sub::ListTraces).unwrap().0,
        "Motion.listTraces"
    );
    assert_eq!(
        to_inspector_call(&Sub::Scrub { frame: 5 }).unwrap().0,
        "Motion.scrubTo"
    );
    assert_eq!(to_inspector_call(&Sub::Play).unwrap().0, "Motion.play");
    assert_eq!(to_inspector_call(&Sub::Pause).unwrap().0, "Motion.pause");
    assert_eq!(
        to_inspector_call(&Sub::Cost { enable: true }).unwrap().0,
        "Motion.enableCost"
    );
    assert_eq!(
        to_inspector_call(&Sub::Cost { enable: false }).unwrap().0,
        "Motion.disableCost"
    );
    assert!(to_inspector_call(&Sub::Help).is_none());
}

#[test]
fn to_inspector_call_stop_defaults_trace_id_to_zero() {
    let (_m, p) = to_inspector_call(&Sub::Stop { trace_id: None }).unwrap();
    assert_eq!(p, "{\"trace_id\":0}");
}

#[test]
fn build_start_trace_params_includes_view_name_and_default_probe() {
    let r = RecordArgs {
        view_name: "Card".to_owned(),
        fps: Some(60),
        out: None,
        metrics: vec![],
    };
    let p = build_start_trace_params(&r);
    assert!(p.contains("\"view_name\":\"Card\""));
    assert!(p.contains("\"fps\":60"));
    assert!(p.contains("\"kind\":\"geometry\""));
    assert!(p.contains("\"node_id\":\"Card\""));
    assert!(p.contains("\"properties\":[\"minX\",\"minY\",\"width\",\"height\"]"));
}

#[test]
fn build_start_trace_params_passes_user_metrics_verbatim_when_json() {
    let r = RecordArgs {
        view_name: "X".to_owned(),
        fps: None,
        out: None,
        metrics: vec!["{\"kind\":\"value\",\"name\":\"opacity\"}".to_owned()],
    };
    let p = build_start_trace_params(&r);
    assert!(p.contains("\"kind\":\"value\""));
    assert!(p.contains("\"name\":\"opacity\""));
    // No default geometry probe when user passed at least one
    // explicit --metrics.
    assert!(!p.contains("\"properties\":[\"minX\""));
}

#[test]
fn metric_spec_short_form_round_trips() {
    let j = metric_spec_to_json("geometry:frame:card:minX,minY:window:presentation");
    assert!(j.contains("\"kind\":\"geometry\""));
    assert!(j.contains("\"name\":\"frame\""));
    assert!(j.contains("\"node_id\":\"card\""));
    assert!(j.contains("\"properties\":[\"minX\",\"minY\"]"));
    assert!(j.contains("\"space\":\"window\""));
    assert!(j.contains("\"source\":\"presentation\""));
}

#[test]
fn extract_int_finds_trace_id() {
    let body = "{\"trace_id\":42,\"other\":7}";
    assert_eq!(extract_int(body, "trace_id"), Some(42));
    assert_eq!(extract_int(body, "other"), Some(7));
    assert_eq!(extract_int(body, "missing"), None);
}

#[test]
fn dispatch_forwards_exact_session_selection() {
    let talker = RecordingTalker::new(vec!["{}"]);
    let flags = GlobalFlags {
        session_id: Some("session-a".to_owned()),
        instance_id: Some("instance-b".to_owned()),
        publication_id: Some("publication-c".to_owned()),
        ..GlobalFlags::default()
    };
    let mut output = Vec::new();
    dispatch(&Sub::Play, &flags, &talker, &mut output).unwrap();
    assert_eq!(
        talker.selections.borrow().as_slice(),
        &[Some(crate::cmd::inspector::SessionSelection {
            session_id: "session-a".to_owned(),
            instance_id: "instance-b".to_owned(),
            publication_id: "publication-c".to_owned(),
        })]
    );
    assert_eq!(talker.calls.borrow()[0].1, "Motion.play");
}

#[test]
fn dispatch_snapshot_passes_method_through() {
    let t = RecordingTalker::new(vec!["{\"tracing_enabled\":true,\"emitted_events\":4}"]);
    let mut buf: Vec<u8> = Vec::new();
    dispatch(&Sub::Snapshot, &GlobalFlags::default(), &t, &mut buf).unwrap();
    let calls = t.calls.borrow();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].0, 0);
    assert_eq!(calls[0].1, "Motion.snapshot");
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("tracing_enabled"));
}

#[test]
fn dispatch_record_extracts_trace_id_in_pretty_mode() {
    let t = RecordingTalker::new(vec![
        "{\"sessionId\":\"session-a\",\"instanceId\":\"instance-b\",\
         \"publicationId\":\"publication-c\"}",
        "{\"trace_id\":3}",
    ]);
    let mut buf: Vec<u8> = Vec::new();
    let sub = Sub::Record(RecordArgs {
        view_name: "Card".to_owned(),
        fps: Some(30),
        out: None,
        metrics: vec![],
    });
    dispatch(&sub, &GlobalFlags::default(), &t, &mut buf).unwrap();
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("trace_id=3"), "{out}");
    assert!(
        out.contains(
            "pulp motion stop --trace-id 3 --session session-a \
         --instance instance-b --publication publication-c"
        ),
        "{out}"
    );
    assert_eq!(t.calls.borrow()[0].1, "Session.getCapabilities");
    assert_eq!(t.calls.borrow()[1].1, "Motion.startTrace");
    assert_eq!(
        t.selections.borrow()[1],
        Some(crate::cmd::inspector::SessionSelection {
            session_id: "session-a".to_owned(),
            instance_id: "instance-b".to_owned(),
            publication_id: "publication-c".to_owned(),
        })
    );
}

#[test]
fn dispatch_record_preserves_exact_selection_in_stop_hint() {
    let talker = RecordingTalker::new(vec!["{\"trace_id\":3}"]);
    let flags = GlobalFlags {
        session_id: Some("session-a".to_owned()),
        instance_id: Some("instance-b".to_owned()),
        publication_id: Some("publication-c".to_owned()),
        ..GlobalFlags::default()
    };
    let mut output = Vec::new();
    dispatch(
        &Sub::Record(RecordArgs {
            view_name: "Card".to_owned(),
            fps: Some(30),
            out: None,
            metrics: vec![],
        }),
        &flags,
        &talker,
        &mut output,
    )
    .unwrap();
    let output = String::from_utf8(output).unwrap();
    assert!(
        output.contains(
            "pulp motion stop --trace-id 3 --session session-a \
         --instance instance-b --publication publication-c"
        ),
        "{output}"
    );
}

#[test]
fn dispatch_record_json_surfaces_resolved_exact_selection() {
    let talker = RecordingTalker::new(vec![
        "{\"sessionId\":\"session-a\",\"instanceId\":\"instance-b\",\
         \"publicationId\":\"publication-c\"}",
        "{\"trace_id\":3}",
    ]);
    let flags = GlobalFlags {
        json: true,
        ..GlobalFlags::default()
    };
    let mut output = Vec::new();
    dispatch(
        &Sub::Record(RecordArgs {
            view_name: "Card".to_owned(),
            fps: None,
            out: None,
            metrics: vec![],
        }),
        &flags,
        &talker,
        &mut output,
    )
    .unwrap();
    let value: serde_json::Value = serde_json::from_slice(&output).unwrap();
    assert_eq!(value["sessionId"], "session-a");
    assert_eq!(value["instanceId"], "instance-b");
    assert_eq!(value["trace_id"], 3);
}

#[test]
fn dispatch_json_flag_prints_raw_response() {
    let t = RecordingTalker::new(vec!["{\"trace_ids\":[1,2,3]}"]);
    let mut buf: Vec<u8> = Vec::new();
    let flags = GlobalFlags {
        json: true,
        port: None,
        ..GlobalFlags::default()
    };
    dispatch(&Sub::ListTraces, &flags, &t, &mut buf).unwrap();
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("{\"trace_ids\":[1,2,3]}"), "{out}");
}

#[test]
fn dispatch_record_with_out_prints_sidecar_hint() {
    let t = RecordingTalker::new(vec![
        "{\"sessionId\":\"session-a\",\"instanceId\":\"instance-b\",\
         \"publicationId\":\"publication-c\"}",
        "{\"trace_id\":9}",
    ]);
    let mut buf: Vec<u8> = Vec::new();
    let sub = Sub::Record(RecordArgs {
        view_name: "Card".to_owned(),
        fps: None,
        out: Some(PathBuf::from("/tmp/card.jsonl")),
        metrics: vec![],
    });
    dispatch(&sub, &GlobalFlags::default(), &t, &mut buf).unwrap();
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("make_fixture_sink"), "{out}");
    assert!(out.contains("/tmp/card.jsonl"), "{out}");
}

#[test]
fn dispatch_stateful_followups_require_exact_selection_before_calling_inspector() {
    let talker = RecordingTalker::new(vec![]);
    for sub in [
        Sub::Stop { trace_id: Some(3) },
        Sub::Scrub { frame: 3 },
        Sub::Play,
        Sub::Pause,
        Sub::Cost { enable: true },
        Sub::Cost { enable: false },
    ] {
        let mut output = Vec::new();
        let error = dispatch(&sub, &GlobalFlags::default(), &talker, &mut output).unwrap_err();
        assert!(matches!(error, CliError::BadUsage(_)), "{error}");
    }
    assert!(talker.calls.borrow().is_empty());
}

#[test]
fn dispatch_help_prints_usage_without_calling_inspector() {
    let t = RecordingTalker::new(vec![]);
    let mut buf: Vec<u8> = Vec::new();
    dispatch(&Sub::Help, &GlobalFlags::default(), &t, &mut buf).unwrap();
    let out = String::from_utf8(buf).unwrap();
    assert!(out.contains("pulp motion — wrappers"));
    assert!(t.calls.borrow().is_empty());
}

#[test]
fn escape_json_handles_quotes_and_backslashes() {
    assert_eq!(escape_json("a\"b"), "a\\\"b");
    assert_eq!(escape_json("a\\b"), "a\\\\b");
}
