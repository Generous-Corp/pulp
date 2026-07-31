//! `pulp trace *` — agent-facing wrappers around the inspector
//! `Trace.*` Perfetto-tracing protocol.
//!
//! # What this module ports
//!
//! Every `pulp trace <verb>` subcommand is sugar over one
//! `pulp-cpp inspect --command Trace.<verb> --params <JSON>` call —
//! the same shape `pulp motion` uses for `Motion.*`. The MCP wrapper
//! (`tools/mcp/pulp_mcp.cpp`) uses the same routing pattern for the
//! `pulp_trace_*` tools; the CLI commands here keep the terminal Trace
//! surface aligned with the MCP tool surface.
//!
//! Subcommands (and the inspector method each one forwards to):
//!
//! | `pulp trace <verb>`                    | Inspector method       |
//! |----------------------------------------|------------------------|
//! | `start [--categories …] [--ring-mb N]` | `Trace.startSession`   |
//! | `stop`                                 | `Trace.stopSession`    |
//! | `query "<SQL>" [--format …]`           | `Trace.query`          |
//! | `query "<SQL>" --trace FILE.pftrace`   | `trace_processor` (offline) |
//! | `fetch`                                | pinned `trace_processor` download |
//! | `snapshot`                             | `Trace.snapshot`       |
//! | `explain "<question>"`                 | `Trace.explain`        |
//! | `slowest-frames` / `xruns` / …         | `Trace.query` (preset) |
//! | `query --preset <name>`                | `Trace.query` (preset) |
//!
//! The L0 preset verbs (`slowest-frames`, `xruns`, `dsp-hotspots`,
//! `layout-vs-paint`) are deterministic canned queries — each maps 1:1
//! onto a named trace-stdlib view via a `Trace.query` `preset` param,
//! so a novice gets a plain table with no SQL and no agent. `explain`
//! is the L1 one-shot: the inspector loads the investigation protocol
//! and returns a narrated root cause.
//!
//! # Why we delegate to `pulp-cpp inspect`
//!
//! The inspector socket uses a 4-byte little-endian length-prefix
//! frame (`core/events/src/interprocess_connection.cpp`), and the
//! C++ `pulp inspect --command METHOD --params JSON` path already
//! speaks it correctly, knows how to auto-discover the port, and
//! prints the parsed JSON response. Re-implementing length-prefix
//! framing + port discovery in Rust would duplicate logic that already
//! lives in the inspect adapter. The shell-out is what the MCP wrapper
//! does too.
//!
//! # Authenticated discovery (off-by-default ergonomics)
//!
//! `--session ID --instance ID --publication ID` selects one exact
//! publication. An explicit
//! `--port` or `PULP_INSPECTOR_PORT` is an additional discovery filter. The
//! C++ client performs authenticated ephemeral discovery and the real protocol
//! connection is the only connection opened. If no session is available it
//! prints a clear explanation that live capture requires an explicitly owned
//! source-checkout host which constructs `InspectorServer`, wires
//! `DomainHandler`, and publishes authenticated discovery. Normal Pulp hosts do
//! not start this endpoint, and `PULP_TRACE_SERVER` is not implemented.

use std::io::Write;
use std::path::{Path, PathBuf};

use crate::cmd::trace_open::OpenArgs;
use crate::error::{CliError, Result};

pub use crate::cmd::trace_dispatch::dispatch;
pub use crate::cmd::trace_doctor::{
    resolve_trace_processor, TraceProcessorSource, TraceProcessorStatus,
};

/// Optional explicit discovery filter understood by the wrapper.
pub const INSPECTOR_PORT_ENV: &str = crate::cmd::inspector::PORT_ENV;

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
    /// The wire token the inspector `Trace.query` param expects.
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
/// carries the already-parsed params so [`to_inspector_call`] is a
/// pure translation step (no re-parsing on the hot path).
#[derive(Debug, Clone)]
pub enum Sub {
    /// `pulp trace` with no verb — print the per-verb help blurb.
    Help,
    /// `pulp trace start [...]` — begin a Perfetto tracing session.
    Start(StartArgs),
    /// `pulp trace stop` — flush the session and print the `.pftrace`.
    Stop,
    /// `pulp trace query "<sql>"` / `query --preset <name>` / an L0
    /// preset verb. Exactly one of `sql` / `preset` is set.
    Query(QueryArgs),
    /// `pulp trace snapshot`.
    Snapshot,
    /// `pulp trace explain "<question>"` — the plain-English question
    /// forwarded to `Trace.explain`.
    Explain {
        /// The natural-language question the inspector investigates.
        question: String,
    },
    /// `pulp trace doctor` — readiness check. Aggregates client-side
    /// probes (inspector reachability, `trace_processor` availability)
    /// with the inspector's own `Trace.snapshot` facts. Not a single
    /// inspector call, so [`to_inspector_call`] returns `None` for it and
    /// [`dispatch`] runs it through [`run_doctor`].
    Doctor,
    /// `pulp trace open <file.pftrace>` — serve the trace from a loopback
    /// HTTP server and open it in the Perfetto UI. Client-side (no inspector
    /// call), so [`dispatch`] runs it through `trace_open::run_open`.
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
    /// `--json` — emit the raw inspector JSON instead of the
    /// pretty-printed default.
    pub json: bool,
    /// Optional explicit port. Falls back to `$PULP_INSPECTOR_PORT`;
    /// zero means authenticated auto-discovery.
    pub port: Option<u16>,
    /// Exact session identity forwarded as `pulp inspect --session`.
    pub session_id: Option<String>,
    /// Exact instance identity forwarded as `pulp inspect --instance`.
    pub instance_id: Option<String>,
    /// Non-reusable publication generation forwarded as `--publication`.
    pub publication_id: Option<String>,
}

/// `pulp trace start` flag set.
#[derive(Debug, Clone, Default)]
pub struct StartArgs {
    /// `--categories dsp,render,…` — the span categories to record.
    /// Empty means "let the inspector pick its default taxonomy".
    pub categories: Vec<String>,
    /// `--ring-mb N` — in-process ring size in mebibytes. `None` means
    /// the inspector's default (80MB); accepted values are 1 through 512.
    pub ring_mb: Option<u32>,
}

/// `pulp trace query` flag set. Exactly one of `sql` / `preset` is
/// `Some`; the parser enforces that.
#[derive(Debug, Clone, Default)]
pub struct QueryArgs {
    /// Raw SQL passed as the first positional.
    pub sql: Option<String>,
    /// A named trace-stdlib preset (`--preset` or an L0 verb).
    pub preset: Option<String>,
    /// Output format; JSON by default.
    pub format: QueryFormat,
    /// True when `--format` was passed explicitly. Lets the offline path
    /// reject `--format json|csv` without misreading the JSON default.
    pub format_set: bool,
    /// `--trace FILE.pftrace` — run the SQL offline against a flushed trace
    /// via `trace_processor_shell` instead of the live inspector. `None`
    /// keeps the default live-inspector `Trace.query` path.
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
    // Sweep top-level shared flags out first so `--json` / `--port`
    // work on either side of the verb, exactly like `pulp motion`.
    let mut globals = GlobalFlags::default();
    let mut rest: Vec<String> = Vec::with_capacity(args.len());
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if a == "--json" {
            globals.json = true;
        } else if a == "--port" {
            i += 1;
            let v = args
                .get(i)
                .ok_or_else(|| CliError::BadUsage("--port requires a value".to_owned()))?;
            let port = v
                .parse::<u16>()
                .map_err(|_| CliError::BadUsage(format!("--port: invalid u16 value `{v}`")))?;
            if port == 0 {
                return Err(CliError::BadUsage(
                    "--port must be between 1 and 65535".to_owned(),
                ));
            }
            globals.port = Some(port);
        } else if a == "--session" || a == "--instance" || a == "--publication" {
            i += 1;
            let v = args
                .get(i)
                .ok_or_else(|| CliError::BadUsage(format!("{a} requires a value")))?;
            if !crate::cmd::inspector::valid_session_identity(v) {
                return Err(CliError::BadUsage(format!(
                    "{a} must contain only ASCII letters, digits, `-`, or `_`"
                )));
            }
            if a == "--session" {
                globals.session_id = Some(v.clone());
            } else if a == "--instance" {
                globals.instance_id = Some(v.clone());
            } else {
                globals.publication_id = Some(v.clone());
            }
        } else {
            rest.push(a.clone());
        }
        i += 1;
    }
    if globals.session_id.is_some() != globals.instance_id.is_some() {
        return Err(CliError::BadUsage(
            "--session and --instance must be supplied together".to_owned(),
        ));
    }
    if globals.publication_id.is_some()
        && (globals.session_id.is_none() || globals.instance_id.is_none())
    {
        return Err(CliError::BadUsage(
            "--publication requires --session and --instance".to_owned(),
        ));
    }

    let Some(verb) = rest.first() else {
        return Ok((Sub::Help, globals));
    };

    match verb.as_str() {
        "help" | "--help" | "-h" => Ok((Sub::Help, globals)),
        "start" => parse_start(&rest[1..]).map(|s| (s, globals)),
        "stop" => no_args("stop", &rest[1..]).map(|()| (Sub::Stop, globals)),
        "query" => parse_query(&rest[1..]).map(|s| (s, globals)),
        "snapshot" => no_args("snapshot", &rest[1..]).map(|()| (Sub::Snapshot, globals)),
        "explain" => parse_explain(&rest[1..]).map(|s| (s, globals)),
        "doctor" => no_args("doctor", &rest[1..]).map(|()| (Sub::Doctor, globals)),
        "fetch" => no_args("fetch", &rest[1..]).map(|()| (Sub::Fetch, globals)),
        "open" => parse_open(&rest[1..]).map(|s| (s, globals)),
        // L0 preset verbs — sugar for `query --preset <verb>`.
        "slowest-frames" | "xruns" | "dsp-hotspots" | "layout-vs-paint" => {
            no_args(verb, &rest[1..]).map(|()| (preset_sub(verb), globals))
        }
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

/// Build a [`Sub::Query`] for a named preset verb.
fn preset_sub(name: &str) -> Sub {
    Sub::Query(QueryArgs {
        sql: None,
        preset: Some(name.to_owned()),
        format: QueryFormat::default(),
        ..QueryArgs::default()
    })
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
            "--out" => {
                return Err(CliError::BadUsage(
                    "pulp trace start --out is unavailable: authenticated \
                     inspector clients cannot choose a host filesystem path"
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

fn parse_query(args: &[String]) -> Result<Sub> {
    let mut q = QueryArgs::default();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--preset" => {
                i += 1;
                let v = args
                    .get(i)
                    .ok_or_else(|| CliError::BadUsage("--preset requires a value".to_owned()))?;
                q.preset = Some(v.clone());
            }
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
    if q.sql.is_some() && q.preset.is_some() {
        return Err(CliError::BadUsage(
            "pulp trace query: pass a SQL string OR --preset, not both".to_owned(),
        ));
    }
    if q.sql.is_none() && q.preset.is_none() {
        return Err(CliError::BadUsage(
            "pulp trace query: missing SQL string (or --preset <name>)".to_owned(),
        ));
    }
    Ok(Sub::Query(q))
}

fn parse_explain(args: &[String]) -> Result<Sub> {
    let question = args.first().ok_or_else(|| {
        CliError::BadUsage("pulp trace explain: missing \"<question>\"".to_owned())
    })?;
    Ok(Sub::Explain {
        question: question.clone(),
    })
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

/// Translate a [`Sub`] into the inspector call surface —
/// `(method, params_json)`. Pure function: easy to unit test without
/// spawning anything.
#[must_use]
pub fn to_inspector_call(sub: &Sub) -> Option<(&'static str, String)> {
    match sub {
        Sub::Help => None,
        Sub::Start(s) => Some(("Trace.startSession", build_start_params(s))),
        Sub::Stop => Some(("Trace.stopSession", "{}".to_owned())),
        Sub::Query(q) => Some(("Trace.query", build_query_params(q))),
        Sub::Snapshot => Some(("Trace.snapshot", "{}".to_owned())),
        Sub::Explain { question } => Some((
            "Trace.explain",
            format!("{{\"question\":\"{}\"}}", escape_json(question)),
        )),
        // Doctor and Open are client-side, not a single inspector call —
        // dispatch() runs them before reaching here.
        Sub::Doctor | Sub::Open(_) | Sub::Fetch => None,
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

/// Build the `Trace.query` params object. Exactly one of `sql` /
/// `preset` is present (the parser enforces it); `format` is always
/// emitted so the inspector never has to guess.
fn build_query_params(q: &QueryArgs) -> String {
    let mut buf = String::with_capacity(128);
    buf.push('{');
    if let Some(ref sql) = q.sql {
        buf.push_str("\"sql\":\"");
        buf.push_str(&escape_json(sql));
        buf.push('"');
    } else if let Some(ref preset) = q.preset {
        buf.push_str("\"preset\":\"");
        buf.push_str(&escape_json(preset));
        buf.push('"');
    }
    buf.push_str(",\"format\":\"");
    buf.push_str(q.format.as_str());
    buf.push_str("\"}");
    buf
}

/// Minimal JSON string escaper. We only escape backslashes and
/// double-quotes — everything else the user types makes it through
/// verbatim. The inspector's `choc::json::parse` rejects anything that
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
    fn call(&self, port: u16, method: &str, params_json: &str) -> Result<String> {
        crate::cmd::inspector::call("trace", port, method, params_json)
    }

    fn call_selected(
        &self,
        port: u16,
        session_id: &str,
        instance_id: &str,
        publication_id: &str,
        method: &str,
        params_json: &str,
    ) -> Result<String> {
        let selection = crate::cmd::inspector::SessionSelection {
            session_id: session_id.to_owned(),
            instance_id: instance_id.to_owned(),
            publication_id: publication_id.to_owned(),
        };
        crate::cmd::inspector::call_selected("trace", port, Some(&selection), method, params_json)
    }
}

/// The clear "no inspector" hint string surfaced after an authenticated
/// inspector request fails and in `pulp trace` help text.
pub(crate) fn no_inspector_hint(port: u16) -> String {
    let target = if port == 0 {
        "authenticated discovery".to_owned()
    } else {
        format!("port {port}")
    };
    format!(
        "pulp trace: no inspector available through {target}.\n\
         Live capture requires an explicitly owned source-checkout host that\n\
         constructs InspectorServer, wires DomainHandler, and publishes\n\
         authenticated discovery. Normal Pulp hosts do not start this endpoint;\n\
         PULP_TRACE_SERVER is not implemented.\n\
         (override the port with --port N or $PULP_INSPECTOR_PORT)."
    )
}

/// Resolve the explicit port filter from CLI flags + env. Zero delegates
/// selection to authenticated discovery. A configured but invalid environment
/// filter is an error rather than permission to select a different session.
pub fn resolve_port(flags: &GlobalFlags) -> Result<u16> {
    crate::cmd::inspector::resolve_port_from_env(flags.port)
}

pub(crate) fn print_help(out: &mut impl Write) -> std::io::Result<()> {
    writeln!(
        out,
        "pulp trace — wrappers around the inspector Trace.* protocol\n"
    )?;
    writeln!(out, "Usage: pulp trace <verb> [flags]\n")?;
    writeln!(out, "Lifecycle verbs:")?;
    writeln!(
        out,
        "  start [--categories dsp,render,…] [--ring-mb 1..512]"
    )?;
    writeln!(
        out,
        "                                Begin a session (Trace.startSession)"
    )?;
    writeln!(
        out,
        "  stop                          Flush + print the .pftrace path (Trace.stopSession)"
    )?;
    writeln!(out, "  snapshot                      Trace.snapshot")?;
    writeln!(out)?;
    writeln!(out, "Query verbs:")?;
    writeln!(
        out,
        "  query \"<sql>\" --trace FILE.pftrace        Run SQL offline via trace_processor"
    )?;
    writeln!(
        out,
        "  query \"<sql>\" [--format json|table|csv]   Reserved live command; currently unavailable"
    )?;
    writeln!(
        out,
        "  query --preset <name>         Reserved live command; currently unavailable"
    )?;
    writeln!(
        out,
        "  slowest-frames                Frames over the vsync budget, worst first"
    )?;
    writeln!(
        out,
        "  xruns                         Audio xrun / deadline-miss events"
    )?;
    writeln!(
        out,
        "  dsp-hotspots                  Per-node DSP cost, most expensive first"
    )?;
    writeln!(
        out,
        "  layout-vs-paint               One-row-per-category frame cost split"
    )?;
    writeln!(out)?;
    writeln!(out, "Investigation:")?;
    writeln!(
        out,
        "  explain \"<question>\"          Reserved live command; currently unavailable"
    )?;
    writeln!(out)?;
    writeln!(out, "Readiness:")?;
    writeln!(
        out,
        "  doctor                        Check inspector + tracing build + trace_processor readiness"
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
        "  --json                        Print the raw inspector JSON response"
    )?;
    writeln!(
        out,
        "  --port N                      Filter authenticated discovery by port (or use $PULP_INSPECTOR_PORT)"
    )?;
    writeln!(
        out,
        "  --session ID --instance ID --publication ID\n\
                                        Select one exact authenticated publication\n"
    )?;
    writeln!(
        out,
        "Live capture requires a custom host owning InspectorServer + DomainHandler."
    )?;
    writeln!(out, "After that host publishes discovery:")?;
    writeln!(out, "  pulp trace start --categories dsp,render")?;
    writeln!(
        out,
        "  pulp trace stop --session SESSION --instance INSTANCE --publication PUBLICATION"
    )?;
    writeln!(
        out,
        "  pulp trace explain \"why is my plugin slow to open?\" --session SESSION \
         --instance INSTANCE --publication PUBLICATION"
    )?;
    Ok(())
}

#[inline]
pub(crate) fn io_err(e: std::io::Error) -> CliError {
    CliError::io(Path::new("<stdout>"), e)
}

#[cfg(test)]
#[path = "trace_parse_tests.rs"]
mod tests;
