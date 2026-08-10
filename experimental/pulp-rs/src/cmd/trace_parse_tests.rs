use super::*;

fn s(values: &[&str]) -> Vec<String> {
    values.iter().map(|value| (*value).to_owned()).collect()
}

#[test]
fn parse_help_aliases() {
    for args in [vec![], s(&["help"]), s(&["--help"]), s(&["-h"])] {
        let (sub, _) = parse(&args).unwrap();
        assert!(matches!(sub, Sub::Help));
    }
}

#[test]
fn parse_global_json_in_any_position() {
    let (_, before) = parse(&s(&["--json", "doctor"])).unwrap();
    let (_, after) = parse(&s(&["doctor", "--json"])).unwrap();
    assert!(before.json && after.json);
}

#[test]
fn legacy_authority_selectors_are_unknown_arguments() {
    for args in [
        s(&["start", "--port", "9200"]),
        s(&["stop", "--session", "session-a"]),
        s(&["doctor", "--publication", "publication-a"]),
    ] {
        assert!(parse(&args).is_err());
    }
}

#[test]
fn exact_instance_is_lifecycle_only() {
    let (start, _) = parse(&s(&["start", "--instance", "instance-a"])).unwrap();
    let Sub::Start(start) = start else {
        panic!("expected start")
    };
    assert_eq!(start.instance_id.as_deref(), Some("instance-a"));

    let (stop, _) = parse(&s(&["stop", "--instance", "instance-a"])).unwrap();
    let Sub::Stop(stop) = stop else {
        panic!("expected stop")
    };
    assert_eq!(stop.instance_id.as_deref(), Some("instance-a"));

    for args in [
        s(&[
            "query",
            "SELECT 1",
            "--trace",
            "/tmp/a.pftrace",
            "--instance",
            "instance-a",
        ]),
        s(&["doctor", "--instance", "instance-a"]),
        s(&["fetch", "--instance", "instance-a"]),
        s(&["open", "/tmp/a.pftrace", "--instance", "instance-a"]),
    ] {
        assert!(parse(&args).is_err());
    }
}

#[test]
fn removed_live_verbs_are_unknown() {
    for verb in [
        "snapshot",
        "explain",
        "slowest-frames",
        "xruns",
        "dsp-hotspots",
        "layout-vs-paint",
    ] {
        assert!(matches!(
            parse(&s(&[verb])),
            Err(CliError::UnknownSubcommand)
        ));
    }
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
    let Sub::Start(args) = sub else {
        panic!("expected start")
    };
    assert_eq!(args.categories, vec!["dsp", "render", "gpu"]);
    assert_eq!(args.ring_mb, Some(128));
}

#[test]
fn parse_start_rejects_bad_ring_and_client_output() {
    for value in ["0", "513", "big"] {
        assert!(parse(&s(&["start", "--ring-mb", value])).is_err());
    }
    assert!(parse(&s(&["start", "--out", "/tmp/client.pftrace"])).is_err());
}

#[test]
fn parse_query_requires_sql_and_trace() {
    for args in [
        s(&["query"]),
        s(&["query", "SELECT 1"]),
        s(&["query", "--trace", "/tmp/a.pftrace"]),
        s(&["query", "--preset", "xruns", "--trace", "/tmp/a.pftrace"]),
    ] {
        assert!(parse(&args).is_err());
    }

    let (sub, _) = parse(&s(&[
        "query",
        "SELECT name FROM slice",
        "--trace",
        "/tmp/a.pftrace",
        "--format",
        "table",
    ]))
    .unwrap();
    let Sub::Query(args) = sub else {
        panic!("expected query")
    };
    assert_eq!(args.sql.as_deref(), Some("SELECT name FROM slice"));
    assert_eq!(args.trace.as_deref(), Some(Path::new("/tmp/a.pftrace")));
    assert_eq!(args.format, QueryFormat::Table);
    assert!(args.format_set);
}

#[test]
fn control_calls_are_exactly_start_and_stop() {
    assert_eq!(
        to_control_call(&Sub::Start(StartArgs::default()))
            .unwrap()
            .0,
        "Trace.startSession"
    );
    assert_eq!(
        to_control_call(&Sub::Stop(StopArgs::default())).unwrap().0,
        "Trace.stopSession"
    );
    assert!(to_control_call(&Sub::Doctor).is_none());
    assert!(to_control_call(&Sub::Fetch).is_none());
    assert!(to_control_call(&Sub::Help).is_none());
}

#[test]
fn build_start_params_includes_only_set_fields() {
    assert_eq!(build_start_params(&StartArgs::default()), "{}");
    let params = build_start_params(&StartArgs {
        instance_id: Some("instance-a".to_owned()),
        categories: vec!["dsp".to_owned(), "render".to_owned()],
        ring_mb: Some(64),
    });
    assert!(params.contains("\"categories\":[\"dsp\",\"render\"]"));
    assert!(params.contains("\"ring_mb\":64"));
    assert!(!params.contains("instance-a"));
}

#[test]
fn escape_json_handles_quotes_and_backslashes() {
    assert_eq!(escape_json("a\"b"), "a\\\"b");
    assert_eq!(escape_json("a\\b"), "a\\\\b");
}

#[test]
fn parse_offline_utility_verbs() {
    assert!(matches!(parse(&s(&["doctor"])).unwrap().0, Sub::Doctor));
    assert!(matches!(parse(&s(&["fetch"])).unwrap().0, Sub::Fetch));
    let (sub, _) = parse(&s(&["open", "/tmp/x.pftrace", "--no-browser"])).unwrap();
    let Sub::Open(args) = sub else {
        panic!("expected open")
    };
    assert_eq!(args.file, Path::new("/tmp/x.pftrace"));
    assert!(args.no_browser);
}
