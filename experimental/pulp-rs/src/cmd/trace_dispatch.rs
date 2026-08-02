//! Typed routing and response rendering for `pulp trace`.

use std::io::Write;

use crate::cmd::trace::{
    io_err, print_help, resolve_port, to_inspector_call, GlobalFlags, InspectorTalker, Sub,
};
use crate::cmd::trace_doctor::{resolve_trace_processor, run_doctor};
use crate::cmd::trace_open::run_open;
use crate::cmd::trace_query::run_offline_query;
use crate::cmd::trace_response::TraceResponse;
use crate::error::{CliError, Result};

/// Dispatch a parsed trace command to its client-side or inspector backend.
///
/// # Errors
///
/// Returns command, inspector, selection, or output failures.
pub fn dispatch<T: InspectorTalker>(
    sub: &Sub,
    flags: &GlobalFlags,
    talker: &T,
    out: &mut impl Write,
) -> Result<()> {
    if matches!(sub, Sub::Help) {
        return print_help(out).map_err(io_err);
    }
    if let Sub::Open(args) = sub {
        return run_open(args, flags.json, out);
    }
    if matches!(sub, Sub::Fetch) {
        return crate::cmd::trace_fetch::run_fetch(flags.json, out);
    }
    // `query --trace FILE` runs offline against a flushed `.pftrace` via
    // trace_processor; without `--trace` it falls through to the live
    // inspector `Trace.query` path below.
    if let Sub::Query(q) = sub {
        if q.trace.is_some() {
            return run_offline_query(q, &resolve_trace_processor(), flags.json, out);
        }
    }
    let port = resolve_port(flags)?;
    let explicit_selection = crate::cmd::inspector::explicit_selection(
        flags.session_id.as_deref(),
        flags.instance_id.as_deref(),
        flags.publication_id.as_deref(),
    );
    if matches!(sub, Sub::Doctor) {
        return run_doctor(port, explicit_selection.as_ref(), flags.json, talker, out);
    }
    let Some((method, params)) = to_inspector_call(sub) else {
        // The `Help` arm above already returned. This stays here so
        // adding a new `Sub` variant without a matching
        // `to_inspector_call` arm fails loudly instead of no-oping.
        return Err(CliError::Other(format!(
            "pulp trace: no inspector mapping for {sub:?}"
        )));
    };
    let selection_policy = match sub {
        Sub::Stop | Sub::Query(_) | Sub::Explain { .. } => {
            let message = match sub {
                Sub::Stop => {
                    concat!(
                        "pulp trace stop requires --session, --instance, and --publication",
                        " from the exact command printed by `pulp trace start`"
                    )
                }
                Sub::Query(_) => {
                    concat!(
                        "pulp trace query requires --session, --instance, and --publication",
                        " from the exact command printed by `pulp trace start`"
                    )
                }
                Sub::Explain { .. } => {
                    concat!(
                        "pulp trace explain requires --session, --instance, and --publication",
                        " from the exact command printed by `pulp trace start`"
                    )
                }
                _ => unreachable!(),
            };
            crate::cmd::inspector::PublicationSelectionPolicy::RequireExact(message)
        }
        Sub::Start(_) => crate::cmd::inspector::PublicationSelectionPolicy::DiscoverExact {
            command_name: "trace",
        },
        _ => crate::cmd::inspector::PublicationSelectionPolicy::Preserve,
    };
    let selection = crate::cmd::inspector::resolve_publication_selection(
        talker,
        port,
        explicit_selection,
        selection_policy,
    )?;
    let response =
        crate::cmd::inspector::call_through(talker, port, selection.as_ref(), method, &params)?;

    if flags.json {
        let rendered = if matches!(sub, Sub::Start(_)) {
            crate::cmd::inspector::attach_session_selection(&response, selection.as_ref())
        } else {
            response.trim_end().to_owned()
        };
        writeln!(out, "{rendered}").map_err(io_err)?;
    } else {
        write_pretty(out, sub, &response, selection.as_ref()).map_err(io_err)?;
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
    selection: Option<&crate::cmd::inspector::SessionSelection>,
) -> std::io::Result<()> {
    let trimmed = response.trim();
    let parsed = TraceResponse::parse(trimmed);
    match sub {
        Sub::Start(_) => {
            if parsed.as_ref().and_then(|value| value.boolean("ok")) == Some(false) {
                writeln!(out, "tracing did not start")?;
                if let Some(message) = parsed.as_ref().and_then(|value| value.string("message")) {
                    writeln!(out, "  {message}")?;
                } else {
                    writeln!(out, "  raw: {trimmed}")?;
                }
                return Ok(());
            }
            if let Some(path) = parsed.as_ref().and_then(|value| value.string("out_path")) {
                writeln!(out, "tracing started — writing to {path}")?;
            } else {
                writeln!(out, "tracing started")?;
                writeln!(out, "  raw: {trimmed}")?;
            }
            writeln!(
                out,
                "  stop with: pulp trace stop{}",
                crate::cmd::inspector::selection_cli_suffix(
                    selection.map(|value| value.session_id.as_str()),
                    selection.map(|value| value.instance_id.as_str()),
                    selection.map(|value| value.publication_id.as_str()),
                )
            )?;
        }
        Sub::Stop => {
            // The headline of `stop` is the `.pftrace` path — pull it
            // out so the user can hand it to ui.perfetto.dev.
            if let Some(path) = parsed.as_ref().and_then(|value| value.string("out_path")) {
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
            if let Some(text) = parsed
                .as_ref()
                .and_then(|value| value.string("explanation"))
            {
                writeln!(out, "{text}")?;
            } else {
                writeln!(out, "{trimmed}")?;
            }
        }
        Sub::Help => {
            writeln!(out, "{trimmed}")?;
        }
        // Doctor and Open are handled in dispatch() and never reach
        // write_pretty; these arms keep the match exhaustive.
        Sub::Doctor | Sub::Open(_) | Sub::Fetch => {
            writeln!(out, "{trimmed}")?;
        }
    }
    Ok(())
}

#[cfg(test)]
#[path = "trace_dispatch_tests.rs"]
mod tests;
