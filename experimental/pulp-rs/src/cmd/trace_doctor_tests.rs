use super::*;
use crate::cmd::inspector_test_support::{s, RecordingTalker};
use crate::cmd::trace::{dispatch, parse, GlobalFlags, Sub};

fn tp(source: TraceProcessorSource, path: Option<&str>) -> TraceProcessorStatus {
    TraceProcessorStatus {
        path: path.map(PathBuf::from),
        source,
    }
}

#[test]
fn doctor_report_is_offline_only() {
    let status = tp(
        TraceProcessorSource::Path,
        Some("/usr/bin/trace_processor_shell"),
    );
    let human = build_doctor_report(&status, false);
    assert!(human.contains("trace_processor ......... found (on $PATH)"));
    assert!(human.contains("ready to query offline ... yes"));
    assert!(!human.contains("inspector"));
    assert!(!human.contains("publication"));
}

#[test]
fn doctor_report_json_shape() {
    let status = tp(TraceProcessorSource::Env, Some("/opt/tp"));
    let json = build_doctor_report(&status, true);
    let value: serde_json::Value = serde_json::from_str(json.trim()).unwrap();
    assert_eq!(value["trace_processor_available"], true);
    assert_eq!(value["trace_processor_path"], "/opt/tp");
    assert_eq!(value["trace_processor_source"], "env");
    assert_eq!(value["ready_to_query"], true);
    assert_eq!(value.as_object().unwrap().len(), 4);
}

#[test]
fn doctor_report_missing_processor_is_actionable() {
    let status = tp(TraceProcessorSource::None, None);
    let human = build_doctor_report(&status, false);
    assert!(human.contains("pulp trace fetch"));
    assert!(human.contains("ready to query offline ... no"));
}

#[test]
fn dispatch_doctor_never_calls_control_transport() {
    let talker = RecordingTalker::new(vec!["should not be consumed"]);
    let mut output = Vec::new();
    dispatch(&Sub::Doctor, &GlobalFlags::default(), &talker, &mut output).unwrap();
    assert!(String::from_utf8(output)
        .unwrap()
        .contains("pulp trace doctor"));
    assert!(talker.calls.borrow().is_empty());
}

#[test]
fn removed_live_doctor_selectors_are_not_parsed_as_authority() {
    for args in [
        s(&["doctor", "--port", "9200"]),
        s(&["doctor", "--session", "session-a"]),
        s(&["doctor", "--publication", "publication-a"]),
    ] {
        assert!(parse(&args).is_err());
    }
}

#[test]
fn resolve_trace_processor_honors_env_override() {
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
