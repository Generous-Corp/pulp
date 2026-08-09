//! `pulp trace *` — canonical trace lifecycle control plus offline Perfetto
//! analysis utilities.
//!
//! # What this module ports
//!
//! Trace lifecycle commands delegate to the C++ canonical capability-control
//! client. Rust supplies no independent target-selection authority. The
//! remaining client-side utilities keep their existing offline behavior.
//!
//! Supported paths:
//!
//! | `pulp trace <verb>`                    | Backend                |
//! |----------------------------------------|------------------------|
//! | `start [--instance ID] [--categories …] [--ring-mb N]` | `Trace.startSession` |
//! | `stop [--instance ID]`                 | `Trace.stopSession`    |
//! | `query "<SQL>" --trace FILE.pftrace`   | `trace_processor` (offline) |
//! | `fetch`                                | pinned `trace_processor` download |
//!
//! # Why lifecycle delegates to `pulp-cpp inspect`
//!
//! The C++ adapter owns the canonical capability-control client. Reusing that
//! adapter keeps broker authentication and fail-closed behavior in one place.
//!
//! `start` and `stop` are default-denied when the broker cannot open a trusted,
//! consented control session. There is intentionally no legacy fallback.

use std::io::Write;
use std::path::{Path, PathBuf};

use crate::cmd::trace_open::OpenArgs;
use crate::error::{CliError, Result};

pub use crate::cmd::trace_dispatch::dispatch;
pub use crate::cmd::trace_doctor::{
    resolve_trace_processor, TraceProcessorSource, TraceProcessorStatus,
};

/// Output format for `pulp trace query`. JSON is the default because
/// it is the easiest for agents to parse; humans reach for `table`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum QueryFormat {
    /// Row objects as JSON — the agent-friendly default.
    #[default]
    Json,
    /// A plain aligned table for terminal reading.
    Table,
    /// Comma-separated values.
    Csv,
}

impl QueryFormat {
    /// The CLI token for this output format.
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            QueryFormat::Json => "json",
            QueryFormat::Table => "table",
            QueryFormat::Csv => "csv",
        }
    }

    /// Parse a `--format` value; `None` for an unrecognised token.
    fn parse(s: &str) -> Option<Self> {
        match s {
            "json" => Some(QueryFormat::Json),
            "table" => Some(QueryFormat::Table),
            "csv" => Some(QueryFormat::Csv),
            _ => None,
        }
    }
}

/// Parsed `pulp trace …` subcommand. One variant per verb; each
/// carries the already-parsed params so [`to_control_call`] is a
/// pure translation step (no re-parsing on the hot path).
#[derive(Debug, Clone)]
pub enum Sub {
    /// `pulp trace` with no verb — print the per-verb help blurb.
    Help,
    /// `pulp trace start [...]` — begin a Perfetto tracing session.
    Start(StartArgs),
    /// `pulp trace stop` — flush the session and print the `.pftrace`.
    Stop(StopArgs),
    /// `pulp trace query "<sql>" --trace <file.pftrace>`.
    Query(QueryArgs),
    /// `pulp trace doctor` — offline `trace_processor` readiness check.
    Doctor,
    /// `pulp trace open <file.pftrace>` — serve the trace from a loopback
    /// HTTP server and open it in the Perfetto UI.
    Open(OpenArgs),
    /// `pulp trace fetch` — download + SHA-verify the pinned
    /// `trace_processor_shell` into the Pulp home so `query --trace` works
    /// zero-install. Client-side, so [`dispatch`] runs `trace_fetch::run_fetch`.
    Fetch,
}

/// Shared flag state — flows into every [`dispatch`] call regardless
/// of verb.
#[derive(Debug, Clone, Default)]
pub struct GlobalFlags {
    /// `--json` — emit machine-readable output.
    pub json: bool,
}

/// `pulp trace start` flag set.
#[derive(Debug, Clone, Default)]
pub struct StartArgs {
    /// `--instance ID` — exact broker-owned live instance. Omission preserves
    /// the canonical opener's safe unambiguous-selection behavior.
    pub instance_id: Option<String>,
    /// `--categories dsp,render,…` — the span categories to record.
    /// Empty means "let the canonical host pick its default taxonomy".
    pub categories: Vec<String>,
    /// `--ring-mb N` — in-process ring size in mebibytes. `None` means
    /// the host's default (80MB); accepted values are 1 through 512.
    pub ring_mb: Option<u32>,
}

/// `pulp trace stop` flag set.
#[derive(Debug, Clone, Default)]
pub struct StopArgs {
    /// `--instance ID` — exact broker-owned live instance.
    pub instance_id: Option<String>,
}

/// `pulp trace query` flag set.
#[derive(Debug, Clone, Default)]
pub struct QueryArgs {
    /// Raw SQL passed as the first positional.
    pub sql: Option<String>,
    /// Output format; JSON by default.
    pub format: QueryFormat,
    /// True when `--format` was passed explicitly. Lets the offline path
    /// reject `--format json|csv` without misreading the JSON default.
    pub format_set: bool,
    /// `--trace FILE.pftrace` — run the SQL offline against a flushed trace
    /// via `trace_processor_shell`. The parser requires this option.
    pub trace: Option<PathBuf>,
}

/// Parse the post-`trace` argument slice into a [`Sub`] plus the
/// [`GlobalFlags`] that survived the parse.
///
/// # Errors
///
/// - [`CliError::UnknownSubcommand`] for an unrecognised verb.
/// - [`CliError::BadUsage`] when required positional / value
///   arguments are missing or malformed.
pub fn parse(args: &[String]) -> Result<(Sub, GlobalFlags)> {
    // Sweep the one supported global flag out first.
    let mut globals = GlobalFlags::default();
    let mut rest: Vec<String> = Vec::with_capacity(args.len());
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if a == "--json" {
            globals.json = true;
        } else {
            rest.push(a.clone());
        }
        i += 1;
    }
    let Some(verb) = rest.first() else {
        return Ok((Sub::Help, globals));
    };

    match verb.as_str() {
        "help" | "--help" | "-h" => Ok((Sub::Help, globals)),
        "start" => parse_start(&rest[1..]).map(|s| (s, globals)),
        "stop" => parse_stop(&rest[1..]).map(|s| (s, globals)),
        "query" => parse_query(&rest[1..]).map(|s| (s, globals)),
        "doctor" => no_args("doctor", &rest[1..]).map(|()| (Sub::Doctor, globals)),
        "fetch" => no_args("fetch", &rest[1..]).map(|()| (Sub::Fetch, globals)),
        "open" => parse_open(&rest[1..]).map(|s| (s, globals)),
        _ => Err(CliError::UnknownSubcommand),
    }
}

fn no_args(verb: &str, args: &[String]) -> Result<()> {
    if let Some(argument) = args.first() {
        return Err(CliError::BadUsage(format!(
            "pulp trace {verb}: unexpected argument `{argument}`"
        )));
    }
    Ok(())
}

fn parse_start(args: &[String]) -> Result<Sub> {
    let mut s = StartArgs::default();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--categories" | "--category" => {
                i += 1;
                let v = args.get(i).ok_or_else(|| {
                    CliError::BadUsage("--categories requires a value".to_owned())
                })?;
                s.categories = v
                    .split(',')
                    .map(str::trim)
                    .filter(|c| !c.is_empty())
                    .map(str::to_owned)
                    .collect();
            }
            "--instance" => {
                i += 1;
                let value = args
                    .get(i)
                    .ok_or_else(|| CliError::BadUsage("--instance requires a value".to_owned()))?;
                if value.is_empty() {
                    return Err(CliError::BadUsage(
                        "--instance requires a non-empty value".to_owned(),
                    ));
                }
                s.instance_id = Some(value.clone());
            }
            "--out" => {
                return Err(CliError::BadUsage(
                    "pulp trace start --out is unavailable: the controlled host owns \
                     the trace destination"
                        .to_owned(),
                ));
            }
            "--ring-mb" => {
                i += 1;
                let v = args
                    .get(i)
                    .ok_or_else(|| CliError::BadUsage("--ring-mb requires a value".to_owned()))?;
                let ring_mb = v.parse::<u32>().map_err(|_| {
                    CliError::BadUsage(format!("--ring-mb: invalid u32 value `{v}`"))
                })?;
                if !(1..=512).contains(&ring_mb) {
                    return Err(CliError::BadUsage(
                        "--ring-mb must be between 1 and 512".to_owned(),
                    ));
                }
                s.ring_mb = Some(ring_mb);
            }
            other => {
                return Err(CliError::BadUsage(format!(
                    "pulp trace start: unknown argument `{other}`"
                )));
            }
        }
        i += 1;
    }
    Ok(Sub::Start(s))
}

fn parse_stop(args: &[String]) -> Result<Sub> {
    let mut stop = StopArgs::default();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--instance" => {
                i += 1;
                let value = args
                    .get(i)
                    .ok_or_else(|| CliError::BadUsage("--instance requires a value".to_owned()))?;
                if value.is_empty() {
                    return Err(CliError::BadUsage(
                        "--instance requires a non-empty value".to_owned(),
                    ));
                }
                stop.instance_id = Some(value.clone());
            }
            other => {
                return Err(CliError::BadUsage(format!(
                    "pulp trace stop: unknown argument `{other}`"
                )));
            }
        }
        i += 1;
    }
    Ok(Sub::Stop(stop))
}

fn parse_query(args: &[String]) -> Result<Sub> {
    let mut q = QueryArgs::default();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--format" => {
                i += 1;
                let v = args
                    .get(i)
                    .ok_or_else(|| CliError::BadUsage("--format requires a value".to_owned()))?;
                q.format = QueryFormat::parse(v).ok_or_else(|| {
                    CliError::BadUsage(format!("--format: expected json|table|csv, got `{v}`"))
                })?;
                q.format_set = true;
            }
            "--trace" => {
                i += 1;
                let v = args
                    .get(i)
                    .ok_or_else(|| CliError::BadUsage("--trace requires a value".to_owned()))?;
                q.trace = Some(PathBuf::from(v));
            }
            other if other.starts_with("--") => {
                return Err(CliError::BadUsage(format!(
                    "pulp trace query: unknown argument `{other}`"
                )));
            }
            _ => {
                // First bare positional is the SQL string.
                if q.sql.is_some() {
                    return Err(CliError::BadUsage(
                        "pulp trace query: only one SQL string is allowed".to_owned(),
                    ));
                }
                q.sql = Some(args[i].clone());
            }
        }
        i += 1;
    }
    if q.sql.is_none() {
        return Err(CliError::BadUsage(
            "pulp trace query: missing SQL string".to_owned(),
        ));
    }
    if q.trace.is_none() {
        return Err(CliError::BadUsage(
            "pulp trace query: --trace <file.pftrace> is required; live query authority was removed"
                .to_owned(),
        ));
    }
    Ok(Sub::Query(q))
}

fn parse_open(args: &[String]) -> Result<Sub> {
    let mut file: Option<PathBuf> = None;
    let mut no_browser = false;
    let mut keep_alive_secs = OpenArgs::DEFAULT_KEEP_ALIVE_SECS;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--no-browser" => no_browser = true,
            "--keep-alive-seconds" => {
                i += 1;
                let v = args.get(i).ok_or_else(|| {
                    CliError::BadUsage("--keep-alive-seconds requires a value".to_owned())
                })?;
                keep_alive_secs = v.parse::<u64>().map_err(|_| {
                    CliError::BadUsage(format!("--keep-alive-seconds: invalid u64 value `{v}`"))
                })?;
            }
            other if other.starts_with("--") => {
                return Err(CliError::BadUsage(format!(
                    "pulp trace open: unknown argument `{other}`"
                )));
            }
            _ => {
                if file.is_some() {
                    return Err(CliError::BadUsage(
                        "pulp trace open: only one .pftrace file is allowed".to_owned(),
                    ));
                }
                file = Some(PathBuf::from(&args[i]));
            }
        }
        i += 1;
    }
    let file = file
        .ok_or_else(|| CliError::BadUsage("pulp trace open: missing <file.pftrace>".to_owned()))?;
    Ok(Sub::Open(OpenArgs {
        file,
        no_browser,
        keep_alive_secs,
    }))
}

/// Translate a [`Sub`] into the canonical control call surface —
/// `(method, params_json)`. Pure function: easy to unit test without
/// spawning anything.
#[must_use]
pub fn to_control_call(sub: &Sub) -> Option<(&'static str, String, Option<&str>)> {
    match sub {
        Sub::Help => None,
        Sub::Start(s) => Some((
            "Trace.startSession",
            build_start_params(s),
            s.instance_id.as_deref(),
        )),
        Sub::Stop(s) => Some((
            "Trace.stopSession",
            "{}".to_owned(),
            s.instance_id.as_deref(),
        )),
        Sub::Query(_) | Sub::Doctor | Sub::Open(_) | Sub::Fetch => None,
    }
}

/// Build the `Trace.startSession` params object. Pure string
/// composition so we don't pull `serde_json` into the CLI just for one
/// small object — the same choice `motion.rs` makes.
fn build_start_params(s: &StartArgs) -> String {
    let mut buf = String::with_capacity(128);
    buf.push('{');
    let mut first = true;
    if !s.categories.is_empty() {
        buf.push_str("\"categories\":[");
        for (idx, c) in s.categories.iter().enumerate() {
            if idx > 0 {
                buf.push(',');
            }
            buf.push('"');
            buf.push_str(&escape_json(c));
            buf.push('"');
        }
        buf.push(']');
        first = false;
    }
    if let Some(ring_mb) = s.ring_mb {
        if !first {
            buf.push(',');
        }
        buf.push_str("\"ring_mb\":");
        buf.push_str(&ring_mb.to_string());
    }
    buf.push('}');
    buf
}

/// Minimal JSON string escaper. We only escape backslashes and
/// double-quotes — everything else the user types makes it through
/// verbatim. The controlled host rejects anything that
/// isn't valid JSON afterwards, which gives a clearer error than a
/// partial escape would.
pub(crate) fn escape_json(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

pub use crate::cmd::inspector::InspectorTalker;

/// Production talker — shells out to `pulp-cpp
/// inspect --command METHOD --params JSON`. Captures stdout and
/// returns it verbatim.
#[derive(Debug, Default, Clone, Copy)]
pub struct SystemInspector;

impl InspectorTalker for SystemInspector {
    fn call(&self, method: &str, params_json: &str, instance_id: Option<&str>) -> Result<String> {
        crate::cmd::inspector::call("trace", method, params_json, instance_id)
    }
}

pub(crate) fn print_help(out: &mut impl Write) -> std::io::Result<()> {
    writeln!(
        out,
        "pulp trace — canonical trace capture and offline Perfetto analysis\n"
    )?;
    writeln!(out, "Usage: pulp trace <verb> [flags]\n")?;
    writeln!(out, "Lifecycle verbs:")?;
    writeln!(
        out,
        "  start [--instance ID] [--categories dsp,render,…] [--ring-mb 1..512]"
    )?;
    writeln!(
        out,
        "                                Begin a session (Trace.startSession)"
    )?;
    writeln!(
        out,
        "  stop [--instance ID]          Flush + print the .pftrace path (Trace.stopSession)"
    )?;
    writeln!(out)?;
    writeln!(out, "Offline analysis:")?;
    writeln!(
        out,
        "  query \"<sql>\" --trace FILE.pftrace        Run SQL offline via trace_processor"
    )?;
    writeln!(out)?;
    writeln!(out, "Readiness:")?;
    writeln!(
        out,
        "  doctor                        Check offline trace_processor readiness"
    )?;
    writeln!(
        out,
        "  fetch                         Download + SHA-verify the pinned trace_processor (zero-install offline query)"
    )?;
    writeln!(out)?;
    writeln!(out, "Viewing:")?;
    writeln!(
        out,
        "  open <file.pftrace> [--no-browser] [--keep-alive-seconds N]"
    )?;
    writeln!(
        out,
        "                                Serve the trace on loopback + open it in the Perfetto UI"
    )?;
    writeln!(out)?;
    writeln!(out, "Global flags:")?;
    writeln!(
        out,
        "  --json                        Print machine-readable output"
    )?;
    writeln!(
        out,
        "Live capture requires a broker-authorized canonical control session."
    )?;
    writeln!(
        out,
        "Use --instance ID to select one exact broker-owned live instance."
    )?;
    writeln!(out, "After the broker grants trace session control:")?;
    writeln!(out, "  pulp trace start --categories dsp,render")?;
    writeln!(out, "  pulp trace stop")?;
    Ok(())
}

#[inline]
pub(crate) fn io_err(e: std::io::Error) -> CliError {
    CliError::io(Path::new("<stdout>"), e)
}

#[cfg(test)]
#[path = "trace_parse_tests.rs"]
mod tests;
