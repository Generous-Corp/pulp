//! Offline trace-processor readiness probes and doctor rendering.

use std::io::Write;
use std::path::PathBuf;

use crate::cmd::trace::io_err;
use crate::error::Result;

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
/// `$PULP_TRACE_PROCESSOR` (must point at an existing file), the pinned
/// Pulp-fetched binary, then `trace_processor_shell` / `trace_processor` on
/// `$PATH`.
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

/// Run `pulp trace doctor` using only local trace-processor resolution.
pub(crate) fn run_doctor(json: bool, out: &mut impl Write) -> Result<()> {
    let status = resolve_trace_processor();
    write!(out, "{}", build_doctor_report(&status, json)).map_err(io_err)
}

#[derive(serde::Serialize)]
struct DoctorReport<'a> {
    trace_processor_available: bool,
    trace_processor_path: Option<&'a str>,
    trace_processor_source: &'static str,
    ready_to_query: bool,
}

/// Render the offline readiness report.
#[must_use]
pub(crate) fn build_doctor_report(tp: &TraceProcessorStatus, json: bool) -> String {
    if json {
        let report = DoctorReport {
            trace_processor_available: tp.available(),
            trace_processor_path: tp.path.as_deref().and_then(std::path::Path::to_str),
            trace_processor_source: tp.source_str(),
            ready_to_query: tp.available(),
        };
        let mut rendered = serde_json::to_string(&report)
            .unwrap_or_else(|_| "{\"serialization_error\":true}".to_owned());
        rendered.push('\n');
        return rendered;
    }

    let availability = match tp.source {
        TraceProcessorSource::Env => "found (via $PULP_TRACE_PROCESSOR)",
        TraceProcessorSource::Pinned => "found (pinned, fetched by Pulp)",
        TraceProcessorSource::Path => "found (on $PATH)",
        TraceProcessorSource::None => {
            "MISSING (run `pulp trace fetch` for the pinned build, set \
             $PULP_TRACE_PROCESSOR, or install trace_processor_shell)"
        }
    };
    format!(
        "pulp trace doctor\n  trace_processor ......... {availability}\n\n  ready to query offline ... {}\n",
        if tp.available() { "yes" } else { "no" }
    )
}

#[cfg(test)]
#[path = "trace_doctor_tests.rs"]
mod tests;
