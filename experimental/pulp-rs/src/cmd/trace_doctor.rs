//! Trace readiness probes and doctor report rendering.

use std::io::Write;
use std::path::PathBuf;

use crate::cmd::trace::{io_err, no_inspector_hint, InspectorTalker};
use crate::cmd::trace_response::TraceResponse;
use crate::error::{CliError, Result};

/// Env var overriding the `trace_processor` binary path.
const TRACE_PROCESSOR_ENV: &str = "PULP_TRACE_PROCESSOR";

/// Which tier resolved a `trace_processor` binary (or that none did).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TraceProcessorSource {
    /// `$PULP_TRACE_PROCESSOR` pointed at an existing file.
    Env,
    /// The pinned, Pulp-fetched `trace_processor_shell` in the Pulp home.
    Pinned,
    /// Found on `$PATH`.
    Path,
    /// Not found anywhere.
    None,
}

/// Result of probing for a usable `trace_processor` / `trace_processor_shell`.
#[derive(Debug, Clone)]
pub struct TraceProcessorStatus {
    /// Resolved path, or `None` when unavailable.
    pub path: Option<PathBuf>,
    /// Which tier resolved it.
    pub source: TraceProcessorSource,
}

impl TraceProcessorStatus {
    /// Whether a usable binary was found.
    #[must_use]
    pub fn available(&self) -> bool {
        self.path.is_some()
    }

    fn source_str(&self) -> &'static str {
        match self.source {
            TraceProcessorSource::Env => "env",
            TraceProcessorSource::Pinned => "pinned",
            TraceProcessorSource::Path => "path",
            TraceProcessorSource::None => "none",
        }
    }
}

/// Probe for a usable `trace_processor` binary. Resolution order:
/// `$PULP_TRACE_PROCESSOR` (must point at an existing file) → the pinned,
/// Pulp-fetched `trace_processor_shell` in the Pulp home (populated by
/// `pulp trace fetch`) → a `trace_processor_shell` / `trace_processor` on
/// `$PATH`. The pinned tier makes offline query zero-install without a
/// surprise download: `query` uses it only once fetched.
#[must_use]
pub fn resolve_trace_processor() -> TraceProcessorStatus {
    if let Ok(v) = std::env::var(TRACE_PROCESSOR_ENV) {
        let p = PathBuf::from(&v);
        if p.is_file() {
            return TraceProcessorStatus {
                path: Some(p),
                source: TraceProcessorSource::Env,
            };
        }
    }
    if let Some(p) = crate::cmd::trace_fetch::pinned_binary_if_present() {
        return TraceProcessorStatus {
            path: Some(p),
            source: TraceProcessorSource::Pinned,
        };
    }
    for name in ["trace_processor_shell", "trace_processor"] {
        if let Some(p) = crate::proc::which(name) {
            return TraceProcessorStatus {
                path: Some(p),
                source: TraceProcessorSource::Path,
            };
        }
    }
    TraceProcessorStatus {
        path: None,
        source: TraceProcessorSource::None,
    }
}

/// Run `pulp trace doctor`: aggregate client-side readiness probes
/// (authenticated session reachability, `Trace.snapshot`,
/// `trace_processor` availability) with the inspector's own trace facts, then
/// print a report.
///
/// # Errors
///
/// Only writer failures ([`CliError::Io`]). An unreachable inspector or a
/// missing `trace_processor` is reported *in* the doctor output, not as an
/// error — surfacing that is the whole point of a doctor.
pub(crate) fn run_doctor<T: InspectorTalker>(
    port: u16,
    explicit_selection: Option<&crate::cmd::inspector::SessionSelection>,
    json: bool,
    talker: &T,
    out: &mut impl Write,
) -> Result<()> {
    let capabilities_response = match explicit_selection {
        Some(selection) => talker.call_selected(
            port,
            &selection.session_id,
            &selection.instance_id,
            &selection.publication_id,
            "Session.getCapabilities",
            "{}",
        ),
        None => talker.call(port, "Session.getCapabilities", "{}"),
    };
    let discovered_selection = capabilities_response.as_deref().ok().and_then(|response| {
        crate::cmd::inspector::exact_selection_from_capabilities(
            explicit_selection,
            response,
            "trace doctor",
        )
        .ok()
    });
    let selection = explicit_selection
        .filter(|selection| !selection.publication_id.is_empty())
        .cloned()
        .or(discovered_selection);
    let controls = selection.as_ref().and_then(|_| {
        capabilities_response
            .as_deref()
            .ok()
            .and_then(parse_capture_controls)
    });
    let snapshot_response = match selection.as_ref() {
        Some(selection) => talker.call_selected(
            port,
            &selection.session_id,
            &selection.instance_id,
            &selection.publication_id,
            "Trace.snapshot",
            "{}",
        ),
        None if capabilities_response.is_ok() => Err(CliError::Other(
            "Session.getCapabilities did not return a sessionId, instanceId, \
             and publicationId"
                .to_owned(),
        )),
        None => Err(CliError::Other(
            "an exact publication identity is required before Trace.snapshot".to_owned(),
        )),
    };
    let reachable = capabilities_response.is_ok() || snapshot_response.is_ok();
    let (snapshot, snapshot_error) = match snapshot_response {
        Ok(snapshot) => (Some(snapshot), None),
        Err(error) if reachable => (None, Some(error.to_string())),
        Err(_) => (None, None),
    };
    let tp = resolve_trace_processor();
    let report = build_doctor_report(
        port,
        reachable,
        controls,
        snapshot.as_deref(),
        snapshot_error.as_deref(),
        &tp,
        json,
    );
    write!(out, "{report}").map_err(io_err)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct CaptureControls {
    pub(crate) session: bool,
    pub(crate) trace: bool,
    pub(crate) controller_available: bool,
}

impl CaptureControls {
    pub(crate) fn ready(self) -> bool {
        self.session && self.trace && self.controller_available
    }
}

pub(crate) fn parse_capture_controls(response: &str) -> Option<CaptureControls> {
    let value: serde_json::Value = serde_json::from_str(response).ok()?;
    let effective = value.get("effective")?.as_array()?;
    let contains = |capability: &str| {
        effective
            .iter()
            .any(|value| value.as_str() == Some(capability))
    };
    Some(CaptureControls {
        session: contains("session.control"),
        trace: contains("trace.session.control"),
        controller_available: value.get("controller").is_none(),
    })
}

#[derive(serde::Serialize)]
struct DoctorReport<'a> {
    port: Option<u16>,
    inspector_reachable: bool,
    session_control_granted: Option<bool>,
    trace_control_granted: Option<bool>,
    controller_available: Option<bool>,
    compiled_in: Option<bool>,
    active: Option<bool>,
    trace_control_available: Option<bool>,
    last_trace_path: Option<&'a str>,
    snapshot_error: Option<&'a str>,
    trace_processor_available: bool,
    trace_processor_path: Option<&'a str>,
    trace_processor_source: &'static str,
    ready_to_capture: bool,
    ready_to_query: bool,
}

/// Build the doctor report (human text or `--json`). Pure: every probe
/// already ran in [`run_doctor`], so this is fully unit-testable.
///
/// `compiled_in` / `active` / `trace_control_available` / `last_trace_path`
/// come from the inspector's `Trace.snapshot`; they are `None` (JSON `null` /
/// "unknown") when the inspector is unreachable or the snapshot capability is
/// unavailable.
/// `ready_to_capture` needs a reachable inspector with tracing compiled in;
/// `ready_to_query` needs a `trace_processor` plus a captured trace.
#[must_use]
pub(crate) fn build_doctor_report(
    port: u16,
    reachable: bool,
    controls: Option<CaptureControls>,
    snapshot_json: Option<&str>,
    snapshot_error: Option<&str>,
    tp: &TraceProcessorStatus,
    json: bool,
) -> String {
    let snapshot = snapshot_json.and_then(TraceResponse::parse);
    let compiled_in = snapshot
        .as_ref()
        .and_then(|value| value.boolean("compiled_in"));
    let active = snapshot.as_ref().and_then(|value| value.boolean("active"));
    let trace_control_available = snapshot
        .as_ref()
        .and_then(|value| value.boolean("trace_control_available"));
    let last_trace_path = snapshot
        .as_ref()
        .and_then(|value| value.string("last_trace_path"))
        .map(str::to_owned);

    let ready_to_capture = reachable
        && compiled_in.unwrap_or(false)
        && trace_control_available.unwrap_or(false)
        && controls.is_some_and(CaptureControls::ready);
    let ready_to_query = tp.available() && last_trace_path.is_some();

    if json {
        let tp_path = tp.path.as_deref().and_then(std::path::Path::to_str);
        let report = DoctorReport {
            port: (port != 0).then_some(port),
            inspector_reachable: reachable,
            session_control_granted: controls.map(|value| value.session),
            trace_control_granted: controls.map(|value| value.trace),
            controller_available: controls.map(|value| value.controller_available),
            compiled_in,
            active,
            trace_control_available,
            last_trace_path: last_trace_path.as_deref(),
            snapshot_error,
            trace_processor_available: tp.available(),
            trace_processor_path: tp_path,
            trace_processor_source: tp.source_str(),
            ready_to_capture,
            ready_to_query,
        };
        let mut rendered = serde_json::to_string(&report)
            .unwrap_or_else(|_| "{\"serialization_error\":true}".to_owned());
        rendered.push('\n');
        return rendered;
    }

    let mut b = String::with_capacity(512);
    b.push_str("pulp trace doctor\n");
    let inspector_target = if port == 0 {
        "authenticated discovery".to_owned()
    } else {
        format!("port {port}")
    };
    b.push_str(&format!(
        "  inspector ({inspector_target}) ... {}\n",
        if reachable {
            "reachable"
        } else {
            "UNREACHABLE"
        }
    ));
    b.push_str(&format!(
        "  tracing compiled in ..... {}\n",
        match compiled_in {
            Some(true) => "yes",
            Some(false) => "NO (rebuild with -DPULP_TRACING=ON)",
            None if reachable => "unknown (Trace.snapshot unavailable)",
            None => "unknown (inspector unreachable)",
        }
    ));
    b.push_str(&format!(
        "  capture controls ........ {}\n",
        match controls {
            Some(value) if value.ready() => "granted",
            Some(value) if value.session && value.trace => "controller lease busy",
            Some(_) => "NOT GRANTED",
            None => "unknown (Session.getCapabilities unavailable)",
        }
    ));
    if let Some(error) = snapshot_error {
        b.push_str(&format!(
            "  trace snapshot .......... unavailable ({error})\n"
        ));
    }
    if let Some(a) = active {
        b.push_str(&format!(
            "  session active .......... {}\n",
            if a { "yes" } else { "no" }
        ));
    }
    b.push_str(&format!(
        "  trace control ............ {}\n",
        match trace_control_available {
            Some(true) => "available",
            Some(false) if active == Some(true) => "OWNED BY ANOTHER PUBLICATION",
            Some(false) => "unavailable",
            None => "unknown",
        }
    ));
    b.push_str(&format!(
        "  last trace .............. {}\n",
        last_trace_path.as_deref().unwrap_or("none captured yet")
    ));
    b.push_str(&format!(
        "  trace_processor ......... {}\n",
        match tp.source {
            TraceProcessorSource::Env => "found (via $PULP_TRACE_PROCESSOR)",
            TraceProcessorSource::Pinned => "found (pinned, fetched by Pulp)",
            TraceProcessorSource::Path => "found (on $PATH)",
            TraceProcessorSource::None =>
                "MISSING (run `pulp trace fetch` for the pinned build, \
                 set $PULP_TRACE_PROCESSOR, or install trace_processor_shell)",
        }
    ));
    b.push('\n');
    b.push_str(&format!(
        "  ready to capture a trace . {}\n",
        if ready_to_capture { "yes" } else { "no" }
    ));
    b.push_str(&format!(
        "  ready to query offline ... {}\n",
        if ready_to_query { "yes" } else { "no" }
    ));
    if !reachable {
        b.push('\n');
        b.push_str(&no_inspector_hint(port));
        b.push('\n');
    }
    b
}

#[cfg(test)]
#[path = "trace_doctor_tests.rs"]
mod tests;
