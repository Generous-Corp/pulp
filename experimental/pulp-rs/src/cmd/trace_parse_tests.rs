use super::*;

fn s(strs: &[&str]) -> Vec<String> {
    strs.iter().map(|x| (*x).to_owned()).collect()
}

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
fn parse_port_override_is_removed() {
    let error = parse(&s(&["--port", "9200", "snapshot"])).unwrap_err();
    assert!(error.to_string().contains("removed"), "{error}");
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
fn parse_exact_publication_selection_is_removed() {
    let error = parse(&s(&[
        "snapshot",
        "--session",
        "session-a",
        "--instance",
        "instance-b",
        "--publication",
        "publication-c",
    ]))
    .unwrap_err();
    assert!(error.to_string().contains("removed"), "{error}");

    for args in [
        s(&["snapshot", "--session", "session-a"]),
        s(&["snapshot", "--instance", "instance-b"]),
        s(&["snapshot", "--publication", "publication-c"]),
        s(&[
            "snapshot",
            "--session",
            "session a",
            "--instance",
            "instance-b",
        ]),
    ] {
        let error = parse(&args).unwrap_err();
        assert!(matches!(error, CliError::BadUsage(_)));
    }
}

#[test]
fn parse_zero_argument_live_verbs_reject_trailing_arguments() {
    for verb in ["stop", "snapshot", "doctor", "fetch", "slowest-frames"] {
        let error = parse(&s(&[verb, "extra"])).unwrap_err();
        assert!(matches!(error, CliError::BadUsage(_)), "{verb}");
    }
}

#[test]
fn parse_unknown_verb_is_unknown() {
    let err = parse(&s(&["blarg"])).unwrap_err();
    assert!(matches!(err, CliError::UnknownSubcommand));
}

#[test]
fn parse_start_collects_categories_and_ring() {
    let (sub, _) = parse(&s(&[
        "start",
        "--categories",
        "dsp, render ,gpu",
        "--ring-mb",
        "128",
    ]))
    .unwrap();
    let Sub::Start(a) = sub else {
        panic!("expected start")
    };
    assert_eq!(a.categories, vec!["dsp", "render", "gpu"]);
    assert_eq!(a.ring_mb, Some(128));
}

#[test]
fn parse_start_defaults_are_empty() {
    let (sub, _) = parse(&s(&["start"])).unwrap();
    let Sub::Start(a) = sub else { panic!() };
    assert!(a.categories.is_empty());
    assert!(a.ring_mb.is_none());
}

#[test]
fn parse_start_rejects_bad_ring_mb() {
    let err = parse(&s(&["start", "--ring-mb", "big"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
    for value in ["0", "513", "4294967295"] {
        let err = parse(&s(&["start", "--ring-mb", value])).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }
}

#[test]
fn parse_start_rejects_client_selected_output_path() {
    let err = parse(&s(&["start", "--out", "/tmp/client-selected.pftrace"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_query_takes_sql_positional() {
    let (sub, _) = parse(&s(&["query", "SELECT name FROM slice"])).unwrap();
    let Sub::Query(q) = sub else { panic!() };
    assert_eq!(q.sql.as_deref(), Some("SELECT name FROM slice"));
    assert!(q.preset.is_none());
    assert_eq!(q.format, QueryFormat::Json);
}

#[test]
fn parse_query_format_flag() {
    let (sub, _) = parse(&s(&["query", "SELECT 1", "--format", "table"])).unwrap();
    let Sub::Query(q) = sub else { panic!() };
    assert_eq!(q.format, QueryFormat::Table);
    assert!(q.format_set);
}

#[test]
fn parse_query_default_format_is_not_marked_set() {
    let (sub, _) = parse(&s(&["query", "SELECT 1"])).unwrap();
    let Sub::Query(q) = sub else { panic!() };
    assert!(!q.format_set);
    assert!(q.trace.is_none());
}

#[test]
fn parse_query_trace_flag_sets_offline_path() {
    let (sub, _) = parse(&s(&["query", "SELECT 1", "--trace", "/t/run.pftrace"])).unwrap();
    let Sub::Query(q) = sub else { panic!() };
    assert_eq!(
        q.trace.as_deref(),
        Some(std::path::Path::new("/t/run.pftrace"))
    );
    assert_eq!(q.sql.as_deref(), Some("SELECT 1"));
}

#[test]
fn parse_query_trace_requires_a_value() {
    let err = parse(&s(&["query", "SELECT 1", "--trace"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_query_rejects_bad_format() {
    let err = parse(&s(&["query", "SELECT 1", "--format", "yaml"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_query_preset_flag() {
    let (sub, _) = parse(&s(&["query", "--preset", "xruns"])).unwrap();
    let Sub::Query(q) = sub else { panic!() };
    assert_eq!(q.preset.as_deref(), Some("xruns"));
    assert!(q.sql.is_none());
}

#[test]
fn parse_query_rejects_sql_and_preset_together() {
    let err = parse(&s(&["query", "SELECT 1", "--preset", "xruns"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_query_rejects_missing_sql_and_preset() {
    let err = parse(&s(&["query"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_preset_verbs_map_to_query() {
    for name in &["slowest-frames", "xruns", "dsp-hotspots", "layout-vs-paint"] {
        let (sub, _) = parse(&s(&[name])).unwrap();
        let Sub::Query(q) = sub else {
            panic!("{name} should be a query")
        };
        assert_eq!(q.preset.as_deref(), Some(*name));
    }
}

#[test]
fn parse_explain_requires_a_question() {
    let err = parse(&s(&["explain"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
    let (sub, _) = parse(&s(&["explain", "why slow?"])).unwrap();
    assert!(matches!(sub, Sub::Explain { .. }));
}

#[test]
fn to_inspector_call_methods_match_protocol() {
    assert_eq!(
        to_inspector_call(&Sub::Start(StartArgs::default()))
            .unwrap()
            .0,
        "Trace.startSession"
    );
    assert_eq!(
        to_inspector_call(&Sub::Stop).unwrap().0,
        "Trace.stopSession"
    );
    assert!(to_inspector_call(&Sub::Query(QueryArgs {
        sql: Some("SELECT 1".to_owned()),
        preset: None,
        format: QueryFormat::Json,
        ..QueryArgs::default()
    }))
    .is_none());
    assert!(to_inspector_call(&Sub::Snapshot).is_none());
    assert!(to_inspector_call(&Sub::Explain {
        question: "why?".to_owned()
    })
    .is_none());
    assert!(to_inspector_call(&Sub::Help).is_none());
}

#[test]
fn build_start_params_includes_only_set_fields() {
    // Empty → empty object (inspector picks its defaults).
    let p = build_start_params(&StartArgs::default());
    assert_eq!(p, "{}");
    // All fields set.
    let p = build_start_params(&StartArgs {
        categories: vec!["dsp".to_owned(), "render".to_owned()],
        ring_mb: Some(64),
    });
    assert!(p.contains("\"categories\":[\"dsp\",\"render\"]"));
    assert!(p.contains("\"ring_mb\":64"));
}

#[test]
fn explain_params_escape_the_question() {
    assert!(to_inspector_call(&Sub::Explain {
        question: "why is \"x\" slow?".to_owned(),
    })
    .is_none());
}

#[test]
fn escape_json_handles_quotes_and_backslashes() {
    assert_eq!(escape_json("a\"b"), "a\\\"b");
    assert_eq!(escape_json("a\\b"), "a\\\\b");
}

#[test]
fn parse_doctor_verb() {
    let (sub, _) = parse(&s(&["doctor"])).unwrap();
    assert!(matches!(sub, Sub::Doctor));
}

#[test]
fn doctor_has_no_single_inspector_call() {
    assert!(to_inspector_call(&Sub::Doctor).is_none());
}

#[test]
fn parse_fetch_verb() {
    let (sub, _) = parse(&s(&["fetch"])).unwrap();
    assert!(matches!(sub, Sub::Fetch));
}

#[test]
fn fetch_has_no_single_inspector_call() {
    assert!(to_inspector_call(&Sub::Fetch).is_none());
}

#[test]
fn parse_open_verb_defaults() {
    let (sub, _) = parse(&s(&["open", "/tmp/x.pftrace"])).unwrap();
    let Sub::Open(a) = sub else {
        panic!("expected open")
    };
    assert_eq!(a.file, Path::new("/tmp/x.pftrace"));
    assert!(!a.no_browser);
    assert_eq!(a.keep_alive_secs, OpenArgs::DEFAULT_KEEP_ALIVE_SECS);
}

#[test]
fn parse_open_collects_flags() {
    let (sub, _) = parse(&s(&[
        "open",
        "/tmp/y.pftrace",
        "--no-browser",
        "--keep-alive-seconds",
        "3",
    ]))
    .unwrap();
    let Sub::Open(a) = sub else {
        panic!("expected open")
    };
    assert!(a.no_browser);
    assert_eq!(a.keep_alive_secs, 3);
}

#[test]
fn parse_open_requires_a_file() {
    let err = parse(&s(&["open", "--no-browser"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn parse_open_rejects_a_second_file() {
    let err = parse(&s(&["open", "a.pftrace", "b.pftrace"])).unwrap_err();
    assert!(matches!(err, CliError::BadUsage(_)));
}

#[test]
fn open_has_no_single_inspector_call() {
    let sub = Sub::Open(OpenArgs {
        file: PathBuf::from("/tmp/x.pftrace"),
        no_browser: true,
        keep_alive_secs: 0,
    });
    assert!(to_inspector_call(&sub).is_none());
}
