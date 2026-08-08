use super::*;
use crate::cmd::inspector_test_support::RecordingTalker;
use crate::cmd::trace::*;

#[test]
fn dispatch_start_passes_only_method_and_params() {
    let talker = RecordingTalker::new(vec!["{\"out_path\":\"/tmp/pulp-9.pftrace\"}"]);
    let mut output = Vec::new();
    let sub = Sub::Start(StartArgs {
        categories: vec!["dsp".to_owned()],
        ring_mb: None,
    });
    dispatch(&sub, &GlobalFlags::default(), &talker, &mut output).unwrap();
    let calls = talker.calls.borrow();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].0, "Trace.startSession");
    assert!(calls[0].1.contains("\"categories\":[\"dsp\"]"));
    let rendered = String::from_utf8(output).unwrap();
    assert!(rendered.contains("tracing started"));
    assert!(rendered.contains("stop with: pulp trace stop"));
    assert!(!rendered.contains("--publication"));
}

#[test]
fn dispatch_start_json_prints_raw_control_response() {
    let talker = RecordingTalker::new(vec!["{\"ok\":true}"]);
    let mut output = Vec::new();
    dispatch(
        &Sub::Start(StartArgs::default()),
        &GlobalFlags { json: true },
        &talker,
        &mut output,
    )
    .unwrap();
    assert_eq!(String::from_utf8(output).unwrap(), "{\"ok\":true}\n");
}

#[test]
fn dispatch_stop_prints_pftrace_path() {
    let talker = RecordingTalker::new(vec!["{\"out_path\":\"/tmp/pulp-42.pftrace\"}"]);
    let mut output = Vec::new();
    dispatch(&Sub::Stop, &GlobalFlags::default(), &talker, &mut output).unwrap();
    assert_eq!(talker.calls.borrow()[0].0, "Trace.stopSession");
    assert_eq!(
        String::from_utf8(output).unwrap().trim(),
        "/tmp/pulp-42.pftrace"
    );
}

#[test]
fn dispatch_help_prints_usage_without_calling_control() {
    let talker = RecordingTalker::new(vec![]);
    let mut output = Vec::new();
    dispatch(&Sub::Help, &GlobalFlags::default(), &talker, &mut output).unwrap();
    let rendered = String::from_utf8(output).unwrap();
    assert!(rendered.contains("canonical trace capture"));
    assert!(!rendered.contains("endpoint discovery"));
    assert!(talker.calls.borrow().is_empty());
}
