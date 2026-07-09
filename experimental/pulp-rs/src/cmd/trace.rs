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
//! | `start [--categories …] [--out FILE]…` | `Trace.startSession`   |
//! | `stop`                                 | `Trace.stopSession`    |
//! | `query "<SQL>" [--format …]`           | `Trace.query`          |
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
//! # Reachability gate (off-by-default ergonomics)
//!
//! Every verb runs a quick `TcpStream::connect` against
//! `127.0.0.1:<port>` first (default 9147, override via
//! `PULP_INSPECTOR_PORT`). If nothing is listening we print a clear
//! "no inspector running — start with `PULP_TRACE_SERVER=1
//! ./build/examples/ui-preview/pulp-ui-preview`" message and exit 1.
//! This catches the most common user mistake (forgetting to launch the
//! host with tracing enabled) without making the user wait for the C++
//! binary's own discovery + connect cycle to fail.

use std::io::Write;
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;

use crate::error::{CliError, Result};

/// Default inspector port — matches `inspect/src/inspector_server.cpp`
/// `kDefaultPort` (and `motion::DEFAULT_INSPECTOR_PORT`). The C++ side
/// also honours `PULP_INSPECTOR_PORT`; we honour the same env var here
/// so a non-default host stays reachable.
pub const DEFAULT_INSPECTOR_PORT: u16 = 9147;

/// Env var that overrides [`DEFAULT_INSPECTOR_PORT`]. Same name the
/// C++ inspector server reads on startup.
pub const INSPECTOR_PORT_ENV: &str = "PULP_INSPECTOR_PORT";

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
}

/// Shared flag state — flows into every [`dispatch`] call regardless
/// of verb.
#[derive(Debug, Clone, Default)]
pub struct GlobalFlags {
    /// `--json` — emit the raw inspector JSON instead of the
    /// pretty-printed default.
    pub json: bool,
    /// Optional explicit port. Falls back to
    /// `$PULP_INSPECTOR_PORT`, then [`DEFAULT_INSPECTOR_PORT`].
    pub port: Option<u16>,
}

/// `pulp trace start` flag set.
#[derive(Debug, Clone, Default)]
pub struct StartArgs {
    /// `--categories dsp,render,…` — the span categories to record.
    /// Empty means "let the inspector pick its default taxonomy".
    pub categories: Vec<String>,
    /// `--out FILE.pftrace` — explicit output path for the flushed
    /// trace. Empty means the inspector chooses a temp path.
    pub out: Option<PathBuf>,
    /// `--ring-mb N` — in-process ring size in mebibytes. `None` means
    /// the inspector's default (80MB).
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
            let v = args.get(i).ok_or_else(|| {
                CliError::BadUsage("--port requires a value".to_owned())
            })?;
            globals.port = Some(v.parse::<u16>().map_err(|_| {
                CliError::BadUsage(format!("--port: invalid u16 value `{v}`"))
            })?);
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
        "stop" => Ok((Sub::Stop, globals)),
        "query" => parse_query(&rest[1..]).map(|s| (s, globals)),
        "snapshot" => Ok((Sub::Snapshot, globals)),
        "explain" => parse_explain(&rest[1..]).map(|s| (s, globals)),
        "doctor" => Ok((Sub::Doctor, globals)),
        // L0 preset verbs — sugar for `query --preset <verb>`.
        "slowest-frames" | "xruns" | "dsp-hotspots" | "layout-vs-paint" => {
            Ok((preset_sub(verb), globals))
        }
        _ => Err(CliError::UnknownSubcommand),
    }
}

/// Build a [`Sub::Query`] for a named preset verb.
fn preset_sub(name: &str) -> Sub {
    Sub::Query(QueryArgs {
        sql: None,
        preset: Some(name.to_owned()),
        format: QueryFormat::default(),
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
                    CliError::BadUsage(
                        "--categories requires a value".to_owned(),
                    )
                })?;
                s.categories = v
                    .split(',')
                    .map(str::trim)
                    .filter(|c| !c.is_empty())
                    .map(str::to_owned)
                    .collect();
            }
            "--out" => {
                i += 1;
                let v = args.get(i).ok_or_else(|| {
                    CliError::BadUsage("--out requires a path".to_owned())
                })?;
                s.out = Some(PathBuf::from(v));
            }
            "--ring-mb" => {
                i += 1;
                let v = args.get(i).ok_or_else(|| {
                    CliError::BadUsage("--ring-mb requires a value".to_owned())
                })?;
                s.ring_mb = Some(v.parse::<u32>().map_err(|_| {
                    CliError::BadUsage(format!(
                        "--ring-mb: invalid u32 value `{v}`"
                    ))
                })?);
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
                let v = args.get(i).ok_or_else(|| {
                    CliError::BadUsage("--preset requires a value".to_owned())
                })?;
                q.preset = Some(v.clone());
            }
            "--format" => {
                i += 1;
                let v = args.get(i).ok_or_else(|| {
                    CliError::BadUsage("--format requires a value".to_owned())
                })?;
                q.format = QueryFormat::parse(v).ok_or_else(|| {
                    CliError::BadUsage(format!(
                        "--format: expected json|table|csv, got `{v}`"
                    ))
                })?;
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
                        "pulp trace query: only one SQL string is allowed"
                            .to_owned(),
                    ));
                }
                q.sql = Some(args[i].clone());
            }
        }
        i += 1;
    }
    if q.sql.is_some() && q.preset.is_some() {
        return Err(CliError::BadUsage(
            "pulp trace query: pass a SQL string OR --preset, not both"
                .to_owned(),
        ));
    }
    if q.sql.is_none() && q.preset.is_none() {
        return Err(CliError::BadUsage(
            "pulp trace query: missing SQL string (or --preset <name>)"
                .to_owned(),
        ));
    }
    Ok(Sub::Query(q))
}

fn parse_explain(args: &[String]) -> Result<Sub> {
    let question = args.first().ok_or_else(|| {
        CliError::BadUsage(
            "pulp trace explain: missing \"<question>\"".to_owned(),
        )
    })?;
    Ok(Sub::Explain {
        question: question.clone(),
    })
}

/// Translate a [`Sub`] into the inspector call surface —
/// `(method, params_json)`. Pure function: easy to unit test without
/// spawning anything.
#[must_use]
pub fn to_inspector_call(sub: &Sub) -> Option<(&'static str, String)> {
    match sub {
        Sub::Help => None,
        Sub::Start(s) => {
            Some(("Trace.startSession", build_start_params(s)))
        }
        Sub::Stop => Some(("Trace.stopSession", "{}".to_owned())),
        Sub::Query(q) => Some(("Trace.query", build_query_params(q))),
        Sub::Snapshot => Some(("Trace.snapshot", "{}".to_owned())),
        Sub::Explain { question } => Some((
            "Trace.explain",
            format!("{{\"question\":\"{}\"}}", escape_json(question)),
        )),
        // Doctor is a client-side aggregation, not a single inspector
        // call — dispatch() runs it via run_doctor() before reaching here.
        Sub::Doctor => None,
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
    if let Some(ref out) = s.out {
        if !first {
            buf.push(',');
        }
        buf.push_str("\"out_path\":\"");
        buf.push_str(&escape_json(&out.to_string_lossy()));
        buf.push('"');
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
fn escape_json(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

/// Trait so tests can swap out the inspector "talker" without spawning
/// a real `pulp-cpp` subprocess. Production code uses the
/// [`SystemInspector`] impl below.
pub trait InspectorTalker {
    /// Send `method` + `params_json` to the inspector and return the
    /// raw response body (the inspector's JSON result).
    ///
    /// # Errors
    ///
    /// Implementations return [`CliError::Other`] with a human message
    /// on transport failures (binary not on PATH, port not listening,
    /// command exited non-zero).
    fn call(&self, port: u16, method: &str, params_json: &str)
        -> Result<String>;
}

/// Production talker — checks reachability via `TcpStream::connect`
/// against `127.0.0.1:<port>` first, then shells out to `pulp-cpp
/// inspect --command METHOD --params JSON`. Captures stdout and
/// returns it verbatim.
#[derive(Debug, Default, Clone, Copy)]
pub struct SystemInspector;

impl InspectorTalker for SystemInspector {
    fn call(
        &self,
        port: u16,
        method: &str,
        params_json: &str,
    ) -> Result<String> {
        // Reachability probe — fail fast with a clear message before
        // the C++ binary's slower discovery+connect cycle would.
        if !inspector_reachable(port) {
            return Err(CliError::Other(no_inspector_hint(port)));
        }
        let bin = resolve_inspect_binary().ok_or_else(|| {
            CliError::Other(
                "pulp trace: could not find `pulp-cpp` or `pulp` binary \
                 on PATH (needed to talk to the inspector). Install / \
                 build the CLI first."
                    .to_owned(),
            )
        })?;
        let output = Command::new(&bin)
            .arg("inspect")
            .arg("--port")
            .arg(port.to_string())
            .arg("--command")
            .arg(method)
            .arg("--params")
            .arg(params_json)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .output()
            .map_err(|e| {
                CliError::Other(format!(
                    "pulp trace: failed to spawn {}: {}",
                    bin.display(),
                    e
                ))
            })?;
        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr).into_owned();
            return Err(CliError::Other(format!(
                "pulp trace: `{} inspect --command {}` exited with {:?}: {}",
                bin.display(),
                method,
                output.status.code(),
                stderr.trim(),
            )));
        }
        Ok(String::from_utf8_lossy(&output.stdout).into_owned())
    }
}

/// Resolve the inspect-capable binary. Preference order:
///
/// 1. `pulp-cpp` on `$PATH` (post-cutover install layout).
/// 2. A local in-tree dev build.
/// 3. `pulp` on `$PATH`, relying on its fallthrough to `pulp-cpp`.
fn resolve_inspect_binary() -> Option<PathBuf> {
    if let Some(p) = crate::proc::which("pulp-cpp") {
        return Some(p);
    }
    for candidate in [
        "build/tools/cli/pulp-cpp",
        "build/tools/cli/pulp",
        "build/pulp",
    ] {
        let p = PathBuf::from(candidate);
        if p.is_file() {
            return Some(p);
        }
    }
    crate::proc::which("pulp")
}

/// Probe whether something is listening on `127.0.0.1:<port>`. Short
/// timeout (250ms) so the wait is invisible to the user when the
/// inspector is alive on the local box.
#[must_use]
pub fn inspector_reachable(port: u16) -> bool {
    let addr = format!("127.0.0.1:{port}");
    let parsed = match addr.parse() {
        Ok(p) => p,
        Err(_) => return false,
    };
    TcpStream::connect_timeout(&parsed, Duration::from_millis(250)).is_ok()
}

/// The clear "no inspector" hint string — surfaced both on
/// reachability failure and in `pulp trace` help text.
fn no_inspector_hint(port: u16) -> String {
    format!(
        "pulp trace: no inspector listening on port {port}.\n\
         Start the host with the tracing server enabled, e.g.:\n  \
         PULP_TRACE_SERVER=1 ./build/examples/ui-preview/pulp-ui-preview\n\
         (override the port with --port N or $PULP_INSPECTOR_PORT)."
    )
}

/// Resolve the effective port from CLI flags + env.
#[must_use]
pub fn resolve_port(flags: &GlobalFlags) -> u16 {
    if let Some(p) = flags.port {
        return p;
    }
    if let Ok(v) = std::env::var(INSPECTOR_PORT_ENV) {
        if let Ok(p) = v.parse::<u16>() {
            return p;
        }
    }
    DEFAULT_INSPECTOR_PORT
}

/// Dispatch a parsed [`Sub`] against an [`InspectorTalker`]. Pure glue
/// — pulls the inspector call, optionally prints the inspector JSON or
/// the pretty-printed form, and returns.
///
/// # Errors
///
/// Surfaces any [`CliError`] the talker emits, plus [`CliError::Io`]
/// on writer failure.
pub fn dispatch<T: InspectorTalker>(
    sub: &Sub,
    flags: &GlobalFlags,
    talker: &T,
    out: &mut impl Write,
) -> Result<()> {
    let port = resolve_port(flags);
    if matches!(sub, Sub::Help) {
        return print_help(out).map_err(io_err);
    }
    if matches!(sub, Sub::Doctor) {
        return run_doctor(port, flags.json, talker, out);
    }
    let Some((method, params)) = to_inspector_call(sub) else {
        // The `Help` arm above already returned. This stays here so
        // adding a new `Sub` variant without a matching
        // `to_inspector_call` arm fails loudly instead of no-oping.
        return Err(CliError::Other(format!(
            "pulp trace: no inspector mapping for {sub:?}"
        )));
    };
    let response = talker.call(port, method, &params)?;

    if flags.json {
        writeln!(out, "{}", response.trim_end()).map_err(io_err)?;
    } else {
        write_pretty(out, sub, &response).map_err(io_err)?;
    }
    Ok(())
}

/// Pretty-printer per verb. Falls back to the raw JSON when the
/// response doesn't look like the expected shape — the inspector is
/// the source of truth, we don't try to second-guess it.
fn write_pretty(
    out: &mut impl Write,
    sub: &Sub,
    response: &str,
) -> std::io::Result<()> {
    let trimmed = response.trim();
    match sub {
        Sub::Start(_) => {
            if let Some(path) = extract_str(trimmed, "out_path") {
                writeln!(out, "tracing started — writing to {path}")?;
                writeln!(out, "  stop with: pulp trace stop")?;
            } else {
                writeln!(out, "tracing started")?;
                writeln!(out, "  raw: {trimmed}")?;
            }
        }
        Sub::Stop => {
            // The headline of `stop` is the `.pftrace` path — pull it
            // out so the user can hand it to ui.perfetto.dev.
            if let Some(path) = extract_str(trimmed, "out_path") {
                writeln!(out, "{path}")?;
            } else {
                writeln!(out, "{trimmed}")?;
            }
        }
        Sub::Query(_) => {
            // Query results are data — print the inspector body as-is
            // (JSON by default, or the pre-formatted table/csv the
            // inspector rendered).
            writeln!(out, "{trimmed}")?;
        }
        Sub::Snapshot => {
            writeln!(out, "Trace subsystem snapshot")?;
            writeln!(out, "  raw: {trimmed}")?;
        }
        Sub::Explain { .. } => {
            // The narrated answer lives in `explanation`; surface it
            // as prose, not JSON, since that is the L1 product.
            if let Some(text) = extract_str(trimmed, "explanation") {
                writeln!(out, "{text}")?;
            } else {
                writeln!(out, "{trimmed}")?;
            }
        }
        Sub::Help => {
            writeln!(out, "{trimmed}")?;
        }
        // Doctor is handled in dispatch() via run_doctor() and never
        // reaches write_pretty; this arm keeps the match exhaustive.
        Sub::Doctor => {
            writeln!(out, "{trimmed}")?;
        }
    }
    Ok(())
}

/// Tiny grep for `"<key>":"<value>"` in a flat JSON object — used by
/// `write_pretty` to pull the `.pftrace` path / narrated explanation.
/// We don't pull serde_json in here because the response shapes are
/// stable and a substring match is plenty for a pretty-print. Handles
/// the `\"` / `\\` escapes the inspector emits.
fn extract_str(json: &str, key: &str) -> Option<String> {
    let needle = format!("\"{key}\":");
    let idx = json.find(&needle)?;
    let tail = &json[idx + needle.len()..];
    let tail = tail.trim_start();
    let mut chars = tail.char_indices();
    // Expect an opening quote.
    if chars.next().map(|(_, c)| c) != Some('"') {
        return None;
    }
    let mut result = String::new();
    let mut escaped = false;
    for (_, c) in chars {
        if escaped {
            result.push(c);
            escaped = false;
        } else if c == '\\' {
            escaped = true;
        } else if c == '"' {
            return Some(result);
        } else {
            result.push(c);
        }
    }
    None
}

/// Extract a boolean field `"<key>":true|false` from a flat JSON object.
/// Companion to [`extract_str`] with the same rationale (stable inspector
/// response shapes make a substring match plenty).
fn extract_bool(json: &str, key: &str) -> Option<bool> {
    let needle = format!("\"{key}\":");
    let idx = json.find(&needle)?;
    let tail = json[idx + needle.len()..].trim_start();
    if tail.starts_with("true") {
        Some(true)
    } else if tail.starts_with("false") {
        Some(false)
    } else {
        None
    }
}

/// Env var overriding the `trace_processor` binary path.
const TRACE_PROCESSOR_ENV: &str = "PULP_TRACE_PROCESSOR";

/// Which tier resolved a `trace_processor` binary (or that none did).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TraceProcessorSource {
    /// `$PULP_TRACE_PROCESSOR` pointed at an existing file.
    Env,
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
            TraceProcessorSource::Path => "path",
            TraceProcessorSource::None => "none",
        }
    }
}

/// Probe for a usable `trace_processor` binary. Resolution order:
/// `$PULP_TRACE_PROCESSOR` (must point at an existing file) → a
/// `trace_processor_shell` / `trace_processor` on `$PATH`. The pinned
/// in-tree binary tier lands with the offline-query slice.
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
/// (inspector reachability, `trace_processor` availability) with the
/// inspector's own `Trace.snapshot` facts, then print a report.
///
/// # Errors
///
/// Only writer failures ([`CliError::Io`]). An unreachable inspector or a
/// missing `trace_processor` is reported *in* the doctor output, not as an
/// error — surfacing that is the whole point of a doctor.
pub fn run_doctor<T: InspectorTalker>(
    port: u16,
    json: bool,
    talker: &T,
    out: &mut impl Write,
) -> Result<()> {
    let reachable = inspector_reachable(port);
    // Only ask the inspector for its snapshot when we know it is up;
    // otherwise the talker's own reachability probe would just re-fail.
    let snapshot = if reachable {
        talker.call(port, "Trace.snapshot", "{}").ok()
    } else {
        None
    };
    let tp = resolve_trace_processor();
    let report =
        build_doctor_report(port, reachable, snapshot.as_deref(), &tp, json);
    write!(out, "{report}").map_err(io_err)
}

fn json_bool_opt(v: Option<bool>) -> &'static str {
    match v {
        Some(true) => "true",
        Some(false) => "false",
        None => "null",
    }
}

fn json_str_opt(v: Option<&str>) -> String {
    match v {
        Some(s) => format!("\"{}\"", escape_json(s)),
        None => "null".to_owned(),
    }
}

/// Build the doctor report (human text or `--json`). Pure: every probe
/// already ran in [`run_doctor`], so this is fully unit-testable.
///
/// `compiled_in` / `active` / `last_trace_path` come from the inspector's
/// `Trace.snapshot`; they are `None` (JSON `null` / "unknown") when the
/// inspector is unreachable. `ready_to_capture` needs a reachable inspector
/// with tracing compiled in; `ready_to_query` needs a `trace_processor`
/// plus a captured trace to run over.
#[must_use]
pub fn build_doctor_report(
    port: u16,
    reachable: bool,
    snapshot_json: Option<&str>,
    tp: &TraceProcessorStatus,
    json: bool,
) -> String {
    let compiled_in = snapshot_json.and_then(|s| extract_bool(s, "compiled_in"));
    let active = snapshot_json.and_then(|s| extract_bool(s, "active"));
    let last_trace_path =
        snapshot_json.and_then(|s| extract_str(s, "last_trace_path"));

    let ready_to_capture = reachable && compiled_in.unwrap_or(false);
    let ready_to_query = tp.available() && last_trace_path.is_some();

    if json {
        let tp_path = tp.path.as_deref().and_then(std::path::Path::to_str);
        return format!(
            "{{\"port\":{port},\
             \"inspector_reachable\":{reachable},\
             \"compiled_in\":{},\
             \"active\":{},\
             \"last_trace_path\":{},\
             \"trace_processor_available\":{},\
             \"trace_processor_path\":{},\
             \"trace_processor_source\":\"{}\",\
             \"ready_to_capture\":{ready_to_capture},\
             \"ready_to_query\":{ready_to_query}}}\n",
            json_bool_opt(compiled_in),
            json_bool_opt(active),
            json_str_opt(last_trace_path.as_deref()),
            tp.available(),
            json_str_opt(tp_path),
            tp.source_str(),
        );
    }

    let mut b = String::with_capacity(512);
    b.push_str("pulp trace doctor\n");
    b.push_str(&format!(
        "  inspector (port {port}) ... {}\n",
        if reachable { "reachable" } else { "UNREACHABLE" }
    ));
    b.push_str(&format!(
        "  tracing compiled in ..... {}\n",
        match compiled_in {
            Some(true) => "yes",
            Some(false) => "NO (rebuild with -DPULP_TRACING=ON)",
            None => "unknown (inspector unreachable)",
        }
    ));
    if let Some(a) = active {
        b.push_str(&format!(
            "  session active .......... {}\n",
            if a { "yes" } else { "no" }
        ));
    }
    b.push_str(&format!(
        "  last trace .............. {}\n",
        last_trace_path.as_deref().unwrap_or("none captured yet")
    ));
    b.push_str(&format!(
        "  trace_processor ......... {}\n",
        match tp.source {
            TraceProcessorSource::Env => "found (via $PULP_TRACE_PROCESSOR)",
            TraceProcessorSource::Path => "found (on $PATH)",
            TraceProcessorSource::None =>
                "MISSING (set $PULP_TRACE_PROCESSOR or install trace_processor_shell)",
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

fn print_help(out: &mut impl Write) -> std::io::Result<()> {
    writeln!(
        out,
        "pulp trace — wrappers around the inspector Trace.* protocol\n"
    )?;
    writeln!(out, "Usage: pulp trace <verb> [flags]\n")?;
    writeln!(out, "Lifecycle verbs:")?;
    writeln!(
        out,
        "  start [--categories dsp,render,…] [--out FILE.pftrace] [--ring-mb N]"
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
        "  query \"<sql>\" [--format json|table|csv]   Run SQL (Trace.query; JSON default)"
    )?;
    writeln!(
        out,
        "  query --preset <name>         Run a named trace-stdlib preset"
    )?;
    writeln!(
        out,
        "  slowest-frames                Frames over the vsync budget, worst first"
    )?;
    writeln!(out, "  xruns                         Audio xrun / deadline-miss events")?;
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
        "  explain \"<question>\"          One-shot narrated root cause (Trace.explain)"
    )?;
    writeln!(out)?;
    writeln!(out, "Readiness:")?;
    writeln!(
        out,
        "  doctor                        Check inspector + tracing build + trace_processor readiness"
    )?;
    writeln!(out)?;
    writeln!(out, "Global flags:")?;
    writeln!(out, "  --json                        Print the raw inspector JSON response")?;
    writeln!(
        out,
        "  --port N                      Override the inspector port (default 9147 / $PULP_INSPECTOR_PORT)\n"
    )?;
    writeln!(
        out,
        "Example: PULP_TRACE_SERVER=1 ./build/examples/ui-preview/pulp-ui-preview &"
    )?;
    writeln!(out, "         pulp trace start --categories dsp,render")?;
    writeln!(out, "         pulp trace stop        # → /tmp/pulp-<ts>.pftrace")?;
    writeln!(
        out,
        "         pulp trace explain \"why is my plugin slow to open?\""
    )?;
    Ok(())
}

#[inline]
fn io_err(e: std::io::Error) -> CliError {
    CliError::io(Path::new("<stdout>"), e)
}

#[cfg(test)]
mod tests {
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
    fn parse_unknown_verb_is_unknown() {
        let err = parse(&s(&["blarg"])).unwrap_err();
        assert!(matches!(err, CliError::UnknownSubcommand));
    }

    #[test]
    fn parse_start_collects_categories_out_and_ring() {
        let (sub, _) = parse(&s(&[
            "start",
            "--categories",
            "dsp, render ,gpu",
            "--out",
            "/tmp/x.pftrace",
            "--ring-mb",
            "128",
        ]))
        .unwrap();
        let Sub::Start(a) = sub else {
            panic!("expected start")
        };
        assert_eq!(a.categories, vec!["dsp", "render", "gpu"]);
        assert_eq!(a.out.as_deref(), Some(Path::new("/tmp/x.pftrace")));
        assert_eq!(a.ring_mb, Some(128));
    }

    #[test]
    fn parse_start_defaults_are_empty() {
        let (sub, _) = parse(&s(&["start"])).unwrap();
        let Sub::Start(a) = sub else { panic!() };
        assert!(a.categories.is_empty());
        assert!(a.out.is_none());
        assert!(a.ring_mb.is_none());
    }

    #[test]
    fn parse_start_rejects_bad_ring_mb() {
        let err = parse(&s(&["start", "--ring-mb", "big"])).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn parse_query_takes_sql_positional() {
        let (sub, _) =
            parse(&s(&["query", "SELECT name FROM slice"])).unwrap();
        let Sub::Query(q) = sub else { panic!() };
        assert_eq!(q.sql.as_deref(), Some("SELECT name FROM slice"));
        assert!(q.preset.is_none());
        assert_eq!(q.format, QueryFormat::Json);
    }

    #[test]
    fn parse_query_format_flag() {
        let (sub, _) =
            parse(&s(&["query", "SELECT 1", "--format", "table"])).unwrap();
        let Sub::Query(q) = sub else { panic!() };
        assert_eq!(q.format, QueryFormat::Table);
    }

    #[test]
    fn parse_query_rejects_bad_format() {
        let err =
            parse(&s(&["query", "SELECT 1", "--format", "yaml"])).unwrap_err();
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
        let err =
            parse(&s(&["query", "SELECT 1", "--preset", "xruns"])).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn parse_query_rejects_missing_sql_and_preset() {
        let err = parse(&s(&["query"])).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn parse_preset_verbs_map_to_query() {
        for name in
            &["slowest-frames", "xruns", "dsp-hotspots", "layout-vs-paint"]
        {
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
            to_inspector_call(&Sub::Start(StartArgs::default())).unwrap().0,
            "Trace.startSession"
        );
        assert_eq!(
            to_inspector_call(&Sub::Stop).unwrap().0,
            "Trace.stopSession"
        );
        assert_eq!(
            to_inspector_call(&Sub::Query(QueryArgs {
                sql: Some("SELECT 1".to_owned()),
                preset: None,
                format: QueryFormat::Json,
            }))
            .unwrap()
            .0,
            "Trace.query"
        );
        assert_eq!(
            to_inspector_call(&Sub::Snapshot).unwrap().0,
            "Trace.snapshot"
        );
        assert_eq!(
            to_inspector_call(&Sub::Explain {
                question: "why?".to_owned()
            })
            .unwrap()
            .0,
            "Trace.explain"
        );
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
            out: Some(PathBuf::from("/tmp/x.pftrace")),
            ring_mb: Some(64),
        });
        assert!(p.contains("\"categories\":[\"dsp\",\"render\"]"));
        assert!(p.contains("\"out_path\":\"/tmp/x.pftrace\""));
        assert!(p.contains("\"ring_mb\":64"));
    }

    #[test]
    fn build_query_params_carries_sql_and_format() {
        let p = build_query_params(&QueryArgs {
            sql: Some("SELECT name FROM slice".to_owned()),
            preset: None,
            format: QueryFormat::Csv,
        });
        assert!(p.contains("\"sql\":\"SELECT name FROM slice\""));
        assert!(p.contains("\"format\":\"csv\""));
        assert!(!p.contains("preset"));
    }

    #[test]
    fn build_query_params_carries_preset() {
        let p = build_query_params(&QueryArgs {
            sql: None,
            preset: Some("slowest-frames".to_owned()),
            format: QueryFormat::Json,
        });
        assert!(p.contains("\"preset\":\"slowest-frames\""));
        assert!(p.contains("\"format\":\"json\""));
        assert!(!p.contains("\"sql\""));
    }

    #[test]
    fn explain_params_escape_the_question() {
        let (_m, p) = to_inspector_call(&Sub::Explain {
            question: "why is \"x\" slow?".to_owned(),
        })
        .unwrap();
        assert!(p.contains("\\\""), "expected escaped quote in {p}");
    }

    #[test]
    fn resolve_port_prefers_explicit() {
        let g = GlobalFlags {
            json: false,
            port: Some(1234),
        };
        assert_eq!(resolve_port(&g), 1234);
    }

    #[test]
    fn extract_str_reads_flat_string_field() {
        let body = "{\"out_path\":\"/tmp/pulp-1.pftrace\",\"n\":3}";
        assert_eq!(
            extract_str(body, "out_path").as_deref(),
            Some("/tmp/pulp-1.pftrace")
        );
        assert_eq!(extract_str(body, "missing"), None);
    }

    #[test]
    fn extract_str_handles_escaped_quotes() {
        let body = "{\"explanation\":\"the \\\"lead\\\" voice dominates\"}";
        assert_eq!(
            extract_str(body, "explanation").as_deref(),
            Some("the \"lead\" voice dominates")
        );
    }

    #[test]
    fn escape_json_handles_quotes_and_backslashes() {
        assert_eq!(escape_json("a\"b"), "a\\\"b");
        assert_eq!(escape_json("a\\b"), "a\\\\b");
    }

    /// Test-only talker that records calls and returns canned
    /// responses. Lets us exercise `dispatch` without a real
    /// `pulp-cpp` binary or a live inspector.
    struct RecordingTalker {
        responses: std::cell::RefCell<Vec<String>>,
        calls: std::cell::RefCell<Vec<(u16, String, String)>>,
    }

    impl RecordingTalker {
        fn new(responses: Vec<&str>) -> Self {
            Self {
                responses: std::cell::RefCell::new(
                    responses.into_iter().map(str::to_owned).collect(),
                ),
                calls: std::cell::RefCell::new(Vec::new()),
            }
        }
    }

    impl InspectorTalker for RecordingTalker {
        fn call(
            &self,
            port: u16,
            method: &str,
            params: &str,
        ) -> Result<String> {
            self.calls.borrow_mut().push((
                port,
                method.to_owned(),
                params.to_owned(),
            ));
            let mut r = self.responses.borrow_mut();
            if r.is_empty() {
                Ok("{}".to_owned())
            } else {
                Ok(r.remove(0))
            }
        }
    }

    #[test]
    fn dispatch_start_passes_method_and_params() {
        let t = RecordingTalker::new(vec![
            "{\"out_path\":\"/tmp/pulp-9.pftrace\"}",
        ]);
        let mut buf: Vec<u8> = Vec::new();
        let sub = Sub::Start(StartArgs {
            categories: vec!["dsp".to_owned()],
            out: None,
            ring_mb: None,
        });
        dispatch(&sub, &GlobalFlags::default(), &t, &mut buf).unwrap();
        let calls = t.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].1, "Trace.startSession");
        assert!(calls[0].2.contains("\"categories\":[\"dsp\"]"));
        let out = String::from_utf8(buf).unwrap();
        assert!(out.contains("/tmp/pulp-9.pftrace"), "{out}");
    }

    #[test]
    fn dispatch_stop_prints_pftrace_path() {
        let t = RecordingTalker::new(vec![
            "{\"out_path\":\"/tmp/pulp-42.pftrace\"}",
        ]);
        let mut buf: Vec<u8> = Vec::new();
        dispatch(&Sub::Stop, &GlobalFlags::default(), &t, &mut buf).unwrap();
        assert_eq!(t.calls.borrow()[0].1, "Trace.stopSession");
        let out = String::from_utf8(buf).unwrap();
        assert_eq!(out.trim(), "/tmp/pulp-42.pftrace");
    }

    #[test]
    fn dispatch_query_preset_verb_routes_to_trace_query() {
        let t = RecordingTalker::new(vec!["[]"]);
        let mut buf: Vec<u8> = Vec::new();
        let (sub, flags) = parse(&s(&["slowest-frames"])).unwrap();
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
        };
        let sub = Sub::Query(QueryArgs {
            sql: Some("SELECT name FROM slice".to_owned()),
            preset: None,
            format: QueryFormat::Json,
        });
        dispatch(&sub, &flags, &t, &mut buf).unwrap();
        let out = String::from_utf8(buf).unwrap();
        assert!(out.contains("[{\"name\":\"process\"}]"), "{out}");
    }

    #[test]
    fn dispatch_explain_prints_narrated_prose() {
        let t = RecordingTalker::new(vec![
            "{\"explanation\":\"Root cause: font-atlas build.\"}",
        ]);
        let mut buf: Vec<u8> = Vec::new();
        let sub = Sub::Explain {
            question: "why slow to open?".to_owned(),
        };
        dispatch(&sub, &GlobalFlags::default(), &t, &mut buf).unwrap();
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
    fn no_inspector_hint_mentions_port_and_trace_server_knob() {
        let s = no_inspector_hint(9200);
        assert!(s.contains("port 9200"), "{s}");
        assert!(s.contains("PULP_TRACE_SERVER=1"));
    }

    #[test]
    fn inspector_reachable_returns_quickly_for_unused_port() {
        let _ = inspector_reachable(1);
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
    fn extract_bool_reads_true_false_and_missing() {
        let body = "{\"compiled_in\":true,\"active\":false}";
        assert_eq!(extract_bool(body, "compiled_in"), Some(true));
        assert_eq!(extract_bool(body, "active"), Some(false));
        assert_eq!(extract_bool(body, "missing"), None);
    }

    fn tp(source: TraceProcessorSource, path: Option<&str>) -> TraceProcessorStatus {
        TraceProcessorStatus {
            path: path.map(PathBuf::from),
            source,
        }
    }

    #[test]
    fn doctor_report_all_green_is_ready() {
        let snap = "{\"compiled_in\":true,\"active\":false,\
                    \"last_trace_path\":\"/tmp/x.pftrace\"}";
        let status = tp(TraceProcessorSource::Path, Some("/usr/bin/trace_processor_shell"));
        let human = build_doctor_report(9147, true, Some(snap), &status, false);
        assert!(human.contains("inspector (port 9147) ... reachable"), "{human}");
        assert!(human.contains("tracing compiled in ..... yes"), "{human}");
        assert!(human.contains("last trace .............. /tmp/x.pftrace"), "{human}");
        assert!(human.contains("ready to capture a trace . yes"), "{human}");
        assert!(human.contains("ready to query offline ... yes"), "{human}");
    }

    #[test]
    fn doctor_report_unreachable_marks_unknowns_and_prints_hint() {
        let status = tp(TraceProcessorSource::None, None);
        let human = build_doctor_report(9200, false, None, &status, false);
        assert!(human.contains("inspector (port 9200) ... UNREACHABLE"), "{human}");
        assert!(human.contains("tracing compiled in ..... unknown"), "{human}");
        assert!(human.contains("ready to capture a trace . no"), "{human}");
        // The "how do I start the server" hint is surfaced.
        assert!(human.contains("PULP_TRACE_SERVER=1"), "{human}");
    }

    #[test]
    fn doctor_report_not_compiled_in_blocks_capture() {
        let snap = "{\"compiled_in\":false,\"active\":false}";
        let status = tp(TraceProcessorSource::Path, Some("/usr/bin/trace_processor_shell"));
        let human = build_doctor_report(9147, true, Some(snap), &status, false);
        assert!(human.contains("tracing compiled in ..... NO"), "{human}");
        assert!(human.contains("ready to capture a trace . no"), "{human}");
        // No trace yet, so offline query is not ready even with the binary.
        assert!(human.contains("ready to query offline ... no"), "{human}");
    }

    #[test]
    fn doctor_report_json_shape() {
        let snap = "{\"compiled_in\":true,\"active\":true,\
                    \"last_trace_path\":\"/tmp/y.pftrace\"}";
        let status = tp(TraceProcessorSource::Env, Some("/opt/tp"));
        let json = build_doctor_report(9147, true, Some(snap), &status, true);
        // Parses as one flat JSON object with the readiness contract.
        let v: serde_json::Value = serde_json::from_str(json.trim()).unwrap();
        assert_eq!(v["port"], 9147);
        assert_eq!(v["inspector_reachable"], true);
        assert_eq!(v["compiled_in"], true);
        assert_eq!(v["active"], true);
        assert_eq!(v["last_trace_path"], "/tmp/y.pftrace");
        assert_eq!(v["trace_processor_available"], true);
        assert_eq!(v["trace_processor_path"], "/opt/tp");
        assert_eq!(v["trace_processor_source"], "env");
        assert_eq!(v["ready_to_capture"], true);
        assert_eq!(v["ready_to_query"], true);
    }

    #[test]
    fn doctor_report_json_nulls_when_unreachable() {
        let status = tp(TraceProcessorSource::None, None);
        let json = build_doctor_report(9147, false, None, &status, true);
        let v: serde_json::Value = serde_json::from_str(json.trim()).unwrap();
        assert!(v["compiled_in"].is_null());
        assert!(v["last_trace_path"].is_null());
        assert!(v["trace_processor_path"].is_null());
        assert_eq!(v["trace_processor_source"], "none");
        assert_eq!(v["ready_to_query"], false);
    }

    #[test]
    fn resolve_trace_processor_honors_env_override() {
        // A unique env var owned solely by trace-doctor, so no other test
        // reads it — setting it here does not race another reader.
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
    fn dispatch_doctor_reports_unreachable_without_a_call_mapping() {
        // Port 1 is never a live inspector, so this is deterministic:
        // run_doctor prints the report; no Trace.* call mapping is needed.
        let t = RecordingTalker::new(vec![]);
        let mut buf: Vec<u8> = Vec::new();
        let flags = GlobalFlags { json: false, port: Some(1) };
        dispatch(&Sub::Doctor, &flags, &t, &mut buf).unwrap();
        let out = String::from_utf8(buf).unwrap();
        assert!(out.contains("pulp trace doctor"), "{out}");
        assert!(out.contains("UNREACHABLE"), "{out}");
    }
}
