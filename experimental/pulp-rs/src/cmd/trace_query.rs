//! `pulp trace query "<sql>" --trace <file.pftrace>` — run SQL against a
//! flushed trace **offline**, without a live inspector session.
//!
//! The default `pulp trace query "<sql>"` (no `--trace`) forwards to the
//! running inspector's `Trace.query`, which reads the in-process live
//! session. Once a session has been flushed to a `.pftrace`, there is no
//! inspector to ask — so `--trace <file>` shells out to Perfetto's
//! `trace_processor_shell` and runs the SQL against the saved file.
//!
//! Resolution reuses [`crate::cmd::trace::resolve_trace_processor`]
//! (`$PULP_TRACE_PROCESSOR` → `$PATH`); a missing binary is reported with an
//! actionable pointer at `pulp trace doctor`, never a bare spawn failure.
//!
//! We invoke `trace_processor_shell -q <sql-file> <trace>` — the batch
//! query-file flag with the trace as the trailing positional (the
//! conventional gflags form; a leading positional is rejected by some
//! versions). `-q` has been stable across `trace_processor_shell` releases
//! far longer than the newer `query` subcommand / `--query-string`
//! spellings. Offline results are the tool's native aligned-table output;
//! `--format json|csv` stays a live-path affordance until a pinned
//! `trace_processor` lands (tracked separately), so we reject that combination
//! loudly rather than silently returning a table the caller did not ask for.

use std::fmt::Write as _;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::cmd::trace::{QueryArgs, QueryFormat, TraceProcessorStatus};
use crate::error::{CliError, Result};

fn io_err(e: std::io::Error) -> CliError {
    CliError::io(Path::new("<trace-query>"), e)
}

/// Build the `trace_processor_shell` argument vector for a batch query.
/// Pure so the command shape is unit-testable without spawning anything:
/// `-q <sql-file> <trace-file>` (trace as the trailing positional).
#[must_use]
pub fn build_trace_processor_args(trace: &Path, sql_file: &Path) -> Vec<String> {
    vec![
        "-q".to_owned(),
        sql_file.to_string_lossy().into_owned(),
        trace.to_string_lossy().into_owned(),
    ]
}

/// Path for the temporary SQL file handed to `-q`. Unique per call — keyed on
/// pid *and* a monotonic counter — so concurrent offline queries in one
/// process (and parallel tests) never share or delete each other's file.
fn sql_temp_path() -> PathBuf {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQ: AtomicU64 = AtomicU64::new(0);
    let n = SEQ.fetch_add(1, Ordering::Relaxed);
    let mut p = std::env::temp_dir();
    p.push(format!("pulp-trace-query-{}-{n}.sql", std::process::id()));
    p
}

/// Escape a string for embedding as a JSON string value (the `--json`
/// envelope). Minimal: quotes, backslash, and control chars.
fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out
}

/// Run an offline query against a flushed `.pftrace`.
///
/// `tp` is injected (not re-resolved here) so callers — and tests — control
/// which binary is used. The caller has already established that
/// `args.trace` is `Some`.
///
/// # Errors
///
/// - [`CliError::BadUsage`] for an offline-incompatible request: a missing
///   trace file, a `--preset` (presets run against the live inspector), or
///   `--format json|csv` (offline returns the native table).
/// - [`CliError::Other`] when `trace_processor` is unavailable (with an
///   actionable pointer at `pulp trace doctor`) or exits non-zero.
/// - [`CliError::Io`] on temp-file or writer failure.
pub fn run_offline_query(
    args: &QueryArgs,
    tp: &TraceProcessorStatus,
    json: bool,
    out: &mut impl Write,
) -> Result<()> {
    let trace = args.trace.as_ref().ok_or_else(|| {
        CliError::Other(
            "pulp trace query: run_offline_query called without --trace".to_owned(),
        )
    })?;

    // Presets are the inspector's trace-stdlib views; their SQL text lives in
    // the inspector, not client-side. Offline needs a raw SQL string.
    if args.preset.is_some() {
        return Err(CliError::BadUsage(
            "pulp trace query: --preset runs against the live inspector; \
             pass a raw SQL string with --trace"
                .to_owned(),
        ));
    }
    let sql = args.sql.as_ref().ok_or_else(|| {
        CliError::BadUsage(
            "pulp trace query: --trace needs a SQL string".to_owned(),
        )
    })?;

    // Offline output is trace_processor's native aligned table. `--format
    // json|csv` is honored only on the live path for now; reject the combo
    // rather than hand back an unrequested table.
    if args.format_set && args.format != QueryFormat::Table {
        return Err(CliError::BadUsage(format!(
            "pulp trace query: --format {} is not supported with --trace \
             (offline queries return trace_processor's native table); \
             omit --format or use the live inspector path",
            args.format.as_str()
        )));
    }

    if !trace.is_file() {
        return Err(CliError::BadUsage(format!(
            "pulp trace query: trace file not found: {}",
            trace.display()
        )));
    }

    let Some(tp_path) = tp.path.as_ref() else {
        return Err(CliError::Other(
            "pulp trace query: trace_processor not found — run \
             `pulp trace doctor`, install Perfetto's trace_processor_shell, \
             or set $PULP_TRACE_PROCESSOR"
                .to_owned(),
        ));
    };

    let sql_file = sql_temp_path();
    std::fs::write(&sql_file, sql.as_bytes())
        .map_err(|e| CliError::io(&sql_file, e))?;

    let tp_args = build_trace_processor_args(trace, &sql_file);
    let result = Command::new(tp_path).args(&tp_args).output();
    // Clean up the temp SQL file regardless of how the run went.
    let _ = std::fs::remove_file(&sql_file);
    let output = result.map_err(|e| {
        CliError::Other(format!(
            "pulp trace query: failed to run {}: {e}",
            tp_path.display()
        ))
    })?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(CliError::Other(format!(
            "pulp trace query: trace_processor exited with {} — {}",
            output
                .status
                .code()
                .map_or_else(|| "signal".to_owned(), |c| c.to_string()),
            stderr.trim()
        )));
    }

    let stdout = String::from_utf8_lossy(&output.stdout);
    if json {
        writeln!(
            out,
            "{{\"trace\":\"{}\",\"output\":\"{}\"}}",
            json_escape(&trace.to_string_lossy()),
            json_escape(&stdout)
        )
        .map_err(io_err)?;
    } else {
        write!(out, "{stdout}").map_err(io_err)?;
        if !stdout.ends_with('\n') {
            writeln!(out).map_err(io_err)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cmd::trace::TraceProcessorSource;

    fn q(sql: Option<&str>, trace: Option<&str>) -> QueryArgs {
        QueryArgs {
            sql: sql.map(str::to_owned),
            preset: None,
            format: QueryFormat::default(),
            format_set: false,
            trace: trace.map(PathBuf::from),
        }
    }

    fn tp_at(path: Option<&str>) -> TraceProcessorStatus {
        match path {
            Some(p) => TraceProcessorStatus {
                path: Some(PathBuf::from(p)),
                source: TraceProcessorSource::Env,
            },
            None => TraceProcessorStatus {
                path: None,
                source: TraceProcessorSource::None,
            },
        }
    }

    #[test]
    fn build_args_are_query_file_then_trace_positional() {
        let a = build_trace_processor_args(
            Path::new("/t/run.pftrace"),
            Path::new("/tmp/q.sql"),
        );
        assert_eq!(a, ["-q", "/tmp/q.sql", "/t/run.pftrace"]);
    }

    #[test]
    fn json_escape_handles_quotes_newlines_controls() {
        assert_eq!(json_escape("a\"b\\c\n\t"), "a\\\"b\\\\c\\n\\t");
        assert_eq!(json_escape("\u{0001}"), "\\u0001");
    }

    #[test]
    fn preset_with_trace_is_rejected() {
        let mut a = q(None, Some("/t/run.pftrace"));
        a.preset = Some("xruns".to_owned());
        let mut buf = Vec::new();
        let err = run_offline_query(&a, &tp_at(Some("/bin/true")), false, &mut buf)
            .unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
        assert!(format!("{err}").contains("live inspector"));
    }

    #[test]
    fn json_format_with_trace_is_rejected() {
        let mut a = q(Some("select 1"), Some("/t/run.pftrace"));
        a.format = QueryFormat::Json;
        a.format_set = true;
        let mut buf = Vec::new();
        let err = run_offline_query(&a, &tp_at(Some("/bin/true")), false, &mut buf)
            .unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
        assert!(format!("{err}").contains("native table"));
    }

    #[test]
    fn missing_trace_file_is_rejected_before_spawn() {
        let a = q(Some("select 1"), Some("/no/such/run.pftrace"));
        let mut buf = Vec::new();
        let err = run_offline_query(&a, &tp_at(Some("/bin/true")), false, &mut buf)
            .unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
        assert!(format!("{err}").contains("trace file not found"));
    }

    #[test]
    fn missing_trace_processor_points_at_doctor() {
        // Use a real existing file for the trace so we reach the tp check.
        let mut trace = std::env::temp_dir();
        trace.push(format!("pulp-tq-fixture-{}.pftrace", std::process::id()));
        std::fs::write(&trace, b"PFTRACE").unwrap();
        let a = q(Some("select 1"), Some(trace.to_str().unwrap()));
        let mut buf = Vec::new();
        let err =
            run_offline_query(&a, &tp_at(None), false, &mut buf).unwrap_err();
        let _ = std::fs::remove_file(&trace);
        assert!(matches!(err, CliError::Other(_)));
        assert!(format!("{err}").contains("pulp trace doctor"));
    }

    // The spawn/stream/exit-code paths use a fake `trace_processor` shell
    // script, so they exercise the real Command plumbing without Perfetto.
    #[cfg(unix)]
    fn write_fake_tp(body: &str, name: &str) -> PathBuf {
        use std::os::unix::fs::PermissionsExt;
        let mut p = std::env::temp_dir();
        p.push(format!("pulp-fake-tp-{}-{}.sh", std::process::id(), name));
        std::fs::write(&p, body).unwrap();
        let mut perms = std::fs::metadata(&p).unwrap().permissions();
        perms.set_mode(0o755);
        std::fs::set_permissions(&p, perms).unwrap();
        p
    }

    #[cfg(unix)]
    fn write_fixture_trace(name: &str) -> PathBuf {
        let mut t = std::env::temp_dir();
        t.push(format!("pulp-tq-{}-{}.pftrace", std::process::id(), name));
        std::fs::write(&t, b"PFTRACE").unwrap();
        t
    }

    #[cfg(unix)]
    #[test]
    fn spawn_streams_native_table_on_success() {
        // Fake tp echoes a table and exits 0. Verifies we pass `-q <sqlfile>`
        // as $1/$2 and the trace as the trailing positional $3, and stream
        // stdout through verbatim.
        let tp = write_fake_tp(
            "#!/bin/sh\necho \"flag:$1\"\ntest -f \"$2\" && echo \"sql-file-exists\"\necho \"trace:$3\"\n",
            "ok",
        );
        let trace = write_fixture_trace("ok");
        let a = q(Some("select count(*) from slice"), trace.to_str());
        let mut buf = Vec::new();
        run_offline_query(&a, &tp_at(tp.to_str()), false, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        let _ = std::fs::remove_file(&tp);
        let _ = std::fs::remove_file(&trace);
        assert!(s.contains("flag:-q"), "{s}");
        assert!(s.contains("sql-file-exists"), "{s}");
        assert!(s.contains(&format!("trace:{}", trace.display())), "{s}");
    }

    #[cfg(unix)]
    #[test]
    fn spawn_json_envelope_wraps_output() {
        let tp = write_fake_tp("#!/bin/sh\nprintf 'row1\\nrow2\\n'\n", "json");
        let trace = write_fixture_trace("json");
        let a = q(Some("select 1"), trace.to_str());
        let mut buf = Vec::new();
        run_offline_query(&a, &tp_at(tp.to_str()), true, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        let _ = std::fs::remove_file(&tp);
        let _ = std::fs::remove_file(&trace);
        assert!(s.contains("\"output\":\"row1\\nrow2\\n\""), "{s}");
        assert!(s.contains("\"trace\":"), "{s}");
    }

    #[cfg(unix)]
    #[test]
    fn spawn_nonzero_exit_surfaces_stderr() {
        let tp = write_fake_tp(
            "#!/bin/sh\necho 'syntax error near FROM' 1>&2\nexit 3\n",
            "fail",
        );
        let trace = write_fixture_trace("fail");
        let a = q(Some("select bogus"), trace.to_str());
        let mut buf = Vec::new();
        let err = run_offline_query(&a, &tp_at(tp.to_str()), false, &mut buf)
            .unwrap_err();
        let _ = std::fs::remove_file(&tp);
        let _ = std::fs::remove_file(&trace);
        let msg = format!("{err}");
        assert!(msg.contains("exited with 3"), "{msg}");
        assert!(msg.contains("syntax error near FROM"), "{msg}");
    }
}
