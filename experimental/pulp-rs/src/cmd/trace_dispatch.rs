//! Typed routing and response rendering for `pulp trace`.

use std::io::Write;

use crate::cmd::trace::{io_err, print_help, to_control_call, GlobalFlags, InspectorTalker, Sub};
use crate::cmd::trace_doctor::{resolve_trace_processor, run_doctor};
use crate::cmd::trace_open::run_open;
use crate::cmd::trace_query::run_offline_query;
use crate::cmd::trace_response::TraceResponse;
use crate::error::Result;

/// Dispatch a parsed trace command to canonical control or an offline backend.
///
/// # Errors
///
/// Returns command, control-client, or output failures.
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
    if let Sub::Query(q) = sub {
        return run_offline_query(q, &resolve_trace_processor(), flags.json, out);
    }
    if matches!(sub, Sub::Doctor) {
        return run_doctor(flags.json, out);
    }
    if matches!(sub, Sub::Start(_) | Sub::Stop(_)) {
        let Some((method, params, instance_id)) = to_control_call(sub) else {
            unreachable!("trace lifecycle has a canonical method")
        };
        let response = talker.call(method, &params, instance_id)?;
        if flags.json {
            writeln!(out, "{}", response.trim_end()).map_err(io_err)?;
        } else {
            write_pretty(out, sub, &response).map_err(io_err)?;
        }
        return Ok(());
    }
    unreachable!("all trace subcommands return through canonical or offline paths")
}

/// Pretty-printer for lifecycle responses. Falls back to raw JSON when the
/// canonical control response does not have the expected shape.
fn write_pretty(out: &mut impl Write, sub: &Sub, response: &str) -> std::io::Result<()> {
    let trimmed = response.trim();
    let parsed = TraceResponse::parse(trimmed);
    match sub {
        Sub::Start(start) => {
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
            if let Some(instance_id) = &start.instance_id {
                writeln!(out, "  stop with: pulp trace stop --instance {instance_id}")?;
            } else {
                writeln!(out, "  stop with: pulp trace stop")?;
            }
        }
        Sub::Stop(_) => {
            // The headline of `stop` is the `.pftrace` path — pull it
            // out so the user can hand it to ui.perfetto.dev.
            if let Some(path) = parsed.as_ref().and_then(|value| value.string("out_path")) {
                writeln!(out, "{path}")?;
            } else {
                writeln!(out, "{trimmed}")?;
            }
        }
        Sub::Query(_) => writeln!(out, "{trimmed}")?,
        Sub::Help => {
            writeln!(out, "{trimmed}")?;
        }
        Sub::Doctor | Sub::Open(_) | Sub::Fetch => {
            writeln!(out, "{trimmed}")?;
        }
    }
    Ok(())
}

#[cfg(test)]
#[path = "trace_dispatch_tests.rs"]
mod tests;
