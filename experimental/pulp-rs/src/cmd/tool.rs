//! `pulp-rs tool` — list / info / install / uninstall / path / run / doctor.
//!
//! # Runtime shape
//!
//! Everything **except** `install` is ported Rust-native:
//!
//! | Subcommand  | Status      | Notes                                        |
//! |-------------|-------------|----------------------------------------------|
//! | `list`      | Ported      | Two-column table; color-aware.               |
//! | `info`      | Ported      | Tool metadata text / JSON.                   |
//! | `uninstall` | Ported      | `rm -rf` of `$PULP_HOME/tools/<id>`.          |
//! | `path`      | Ported      | Prints absolute path via `locate_tool`.      |
//! | `run`       | Ported      | Exec via `Spawner`.                          |
//! | `doctor`    | Ported      | Health report per tool.                      |
//! | `install`   | Delegated   | Uses `pulp-cpp` when available; otherwise    |
//! |             |             | prints the Rust fallback notice.             |
//! | `update`    | Ported +    | Resolves/persists the user version override  |
//! |             | Delegated   | (Rust), delegates the re-fetch to `pulp-cpp`.|
//!
//! `install`/`update` carry a user version override (`--version`, or the
//! `PULP_TOOL_<ID>_VERSION` env var) that takes precedence over the registry
//! `pinned_version` and survives Pulp pin bumps. See [`crate::tool_version`].
//!
//! The install branch still dispatches (so `pulp tool install <id>`
//! delegates to `pulp-cpp` when available, or returns a clean "not
//! yet ported" exit code instead of an "unknown subcommand" error).
//! See `tool_registry.cpp` for the reference.

use std::io::Write;

use crate::color;
use crate::error::{CliError, Result};
use crate::proc::{Invocation, Spawner};
use crate::tool_registry::{
    self, current_platform_key, locate_tool, uninstall_tool, ToolRegistry,
};
use crate::tool_version::{self, ActiveVersion};

mod tool_doctor;
use tool_doctor::doctor;

mod tool_status;
use tool_status::{status_label, tool_available_on_platform};

/// Parsed subcommand token.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Sub {
    /// `pulp tool` (no sub) — print help.
    Help,
    /// `list`.
    List,
    /// `info <id> [--json]`.
    Info {
        /// Tool id to inspect.
        id: String,
        /// Emit JSON instead of text.
        json: bool,
    },
    /// `install <id>` / `install --all` / `install <id> --force` /
    /// `install <id> --version <v>`.
    Install {
        /// Tool id to install, `None` when `--all` is set.
        id: Option<String>,
        /// `--all`.
        all: bool,
        /// `--force`.
        force: bool,
        /// `--version <v>` — pin this tool to a user-chosen version
        /// (persisted as a durable override; overrides the registry pin).
        version: Option<String>,
    },
    /// `update <id> [--version <v>]` — re-install a managed tool at the
    /// latest registry pin, or at an explicit user-chosen version.
    Update {
        /// Tool id to update.
        id: String,
        /// `--version <v>` — pin to this version (durable user override).
        /// When absent, `update` re-syncs to the registry pin and clears
        /// any prior user override.
        version: Option<String>,
    },
    /// `uninstall <id>`.
    Uninstall(String),
    /// `path <id>`.
    Path(String),
    /// `run <id> [args...]`.
    Run {
        /// Tool id.
        id: String,
        /// Arguments forwarded to the tool.
        args: Vec<String>,
    },
    /// `doctor [id] [--run]`.
    Doctor {
        /// Optional tool id to inspect.
        id: Option<String>,
        /// Execute the tool's smoke check when a target is provided.
        run: bool,
    },
}

/// Parse the tail into a [`Sub`].
///
/// # Errors
///
/// Returns [`CliError::UnknownSubcommand`] on unknown subcommand
/// tokens and [`CliError::BadUsage`] on missing required positional
/// arguments.
pub fn parse_sub(args: &[String]) -> Result<Sub> {
    let Some(first) = args.first() else {
        return Ok(Sub::Help);
    };
    match first.as_str() {
        "list" => Ok(Sub::List),
        "info" => {
            let mut id = None;
            let mut json = false;
            for a in &args[1..] {
                if a == "--json" {
                    json = true;
                } else if id.is_none() {
                    id = Some(a.clone());
                } else {
                    return Err(CliError::BadUsage(
                        "Usage: pulp tool info <tool-id> [--json]".to_owned(),
                    ));
                }
            }
            let id = id.ok_or_else(|| {
                CliError::BadUsage("Usage: pulp tool info <tool-id> [--json]".to_owned())
            })?;
            Ok(Sub::Info { id, json })
        }
        "install" => {
            let usage = "Usage: pulp tool install <tool-id> [--force] [--version <version>]";
            let mut id = None;
            let mut all = false;
            let mut force = false;
            let mut version = None;
            let mut i = 1;
            while i < args.len() {
                match args[i].as_str() {
                    "--all" => all = true,
                    "--force" => force = true,
                    "--version" => {
                        i += 1;
                        version = Some(
                            args.get(i)
                                .cloned()
                                .ok_or_else(|| CliError::BadUsage(usage.to_owned()))?,
                        );
                    }
                    _ => id = Some(args[i].clone()),
                }
                i += 1;
            }
            if !all && id.is_none() {
                return Err(CliError::BadUsage(usage.to_owned()));
            }
            Ok(Sub::Install { id, all, force, version })
        }
        "update" => {
            let usage = "Usage: pulp tool update <tool-id> [--version <version>]";
            let mut id = None;
            let mut version = None;
            let mut i = 1;
            while i < args.len() {
                match args[i].as_str() {
                    "--version" => {
                        i += 1;
                        version = Some(
                            args.get(i)
                                .cloned()
                                .ok_or_else(|| CliError::BadUsage(usage.to_owned()))?,
                        );
                    }
                    other if other.starts_with("--") => {
                        return Err(CliError::BadUsage(usage.to_owned()));
                    }
                    _ if id.is_none() => id = Some(args[i].clone()),
                    _ => return Err(CliError::BadUsage(usage.to_owned())),
                }
                i += 1;
            }
            let id = id.ok_or_else(|| CliError::BadUsage(usage.to_owned()))?;
            Ok(Sub::Update { id, version })
        }
        "uninstall" => {
            let id = args.get(1).cloned().ok_or_else(|| {
                CliError::BadUsage("Usage: pulp tool uninstall <tool-id>".to_owned())
            })?;
            Ok(Sub::Uninstall(id))
        }
        "path" => {
            let id = args
                .get(1)
                .cloned()
                .ok_or_else(|| CliError::BadUsage("Usage: pulp tool path <tool-id>".to_owned()))?;
            Ok(Sub::Path(id))
        }
        "run" => {
            let id = args.get(1).cloned().ok_or_else(|| {
                CliError::BadUsage("Usage: pulp tool run <tool-id> [args...]".to_owned())
            })?;
            let rest = args.get(2..).map(<[String]>::to_vec).unwrap_or_default();
            Ok(Sub::Run { id, args: rest })
        }
        "doctor" => {
            let mut id = None;
            let mut run = false;
            for a in &args[1..] {
                if a == "--run" {
                    run = true;
                } else if id.is_none() {
                    id = Some(a.clone());
                } else {
                    return Err(CliError::BadUsage(
                        "Usage: pulp tool doctor [tool-id] [--run]".to_owned(),
                    ));
                }
            }
            if run && id.is_none() {
                return Err(CliError::BadUsage(
                    "Usage: pulp tool doctor [tool-id] [--run]".to_owned(),
                ));
            }
            Ok(Sub::Doctor { id, run })
        }
        _ => Err(CliError::UnknownSubcommand),
    }
}

/// Dispatched from `main`.
///
/// # Errors
///
/// Surfaces registry load / spawn / filesystem failures.
pub fn run<S: Spawner>(sub: &Sub, spawner: &S, out: &mut impl Write) -> Result<i32> {
    if matches!(sub, Sub::Help) {
        return print_help(out).map(|()| 0);
    }

    let cwd = std::env::current_dir().map_err(|e| CliError::io("<cwd>", e))?;
    // Repo checkout wins (a Pulp dev's edits show without a rebuild); installed
    // users fall back to the registry compiled into the binary, so `pulp tool`
    // works from any directory, not only inside a Pulp source tree.
    let (reg, _reg_path) = tool_registry::resolve_registry(&cwd)?;

    // Resolve friendly aliases (e.g. "perfetto" → "trace-processor") once, at
    // the dispatch boundary, so every verb — including install's trace-processor
    // short-circuit and uninstall's managed-dir lookup — sees the canonical id.
    match sub {
        Sub::Help => unreachable!(), // handled above
        Sub::List => list(&reg, out),
        Sub::Info { id, json } => info(&reg, &reg.canonical_id(id), *json, out),
        Sub::Install { id, all, force: _, version } => {
            let canon = id.as_ref().map(|i| reg.canonical_id(i));
            install(&reg, canon.as_deref(), *all, version.as_deref(), out)
        }
        Sub::Update { id, version } => {
            update(&reg, &reg.canonical_id(id), version.as_deref(), out)
        }
        Sub::Uninstall(id) => uninstall(&reg.canonical_id(id), out),
        Sub::Path(id) => path(&reg, &reg.canonical_id(id), out),
        Sub::Run { id, args } => run_tool(&reg, &reg.canonical_id(id), args, spawner, out),
        Sub::Doctor { id, run } => {
            let canon = id.as_ref().map(|i| reg.canonical_id(i));
            doctor(&reg, canon.as_deref(), *run, spawner, out)
        }
    }
}

fn io(e: std::io::Error) -> CliError {
    CliError::io("<stdout>", e)
}

/// Write the usage banner, byte-parity with C++ `cmd_tool` empty-args path.
///
/// # Errors
///
/// Surfaces write failures.
pub fn print_help(out: &mut impl Write) -> Result<()> {
    out.write_all(
        b"Usage: pulp tool <command> [options]\n\n\
        Commands:\n\
        \x20 list                    Show available and installed tools\n\
        \x20 info <tool> [--json]    Show install/package metadata for one tool\n\
        \x20 install <tool>          Download and install a tool\n\
        \x20 install <tool> --version <v>  Install and pin to a user-chosen version\n\
        \x20 install --all           Install all tools for current platform\n\
        \x20 update <tool>           Re-install a tool at the latest registry pin\n\
        \x20 update <tool> --version <v>   Update and pin to a user-chosen version\n\
        \x20 uninstall <tool>        Remove a pulp-managed tool\n\
        \x20 path <tool>             Print path to a tool's binary\n\
        \x20 run <tool> [args]       Run a tool with arguments\n\
        \x20 doctor [tool] [--run]   Check tool health\n\
        \n\
        A user version override (set via --version, or the\n\
        PULP_TOOL_<ID>_VERSION env var) takes precedence over the registry\n\
        pin and survives Pulp registry-pin bumps.\n",
    )
    .map_err(io)
}

fn list(reg: &ToolRegistry, out: &mut impl Write) -> Result<i32> {
    let platform = current_platform_key();
    writeln!(
        out,
        "Available tools {dim}({platform}){reset}:\n",
        dim = color::dim(),
        reset = color::reset()
    )
    .map_err(io)?;

    for (id, tool) in &reg.tools {
        let loc = locate_tool(tool);
        let status = status_label(tool, &loc, platform);
        let pad = 20usize.saturating_sub(id.chars().count()).max(1);
        writeln!(
            out,
            "  {id}{padding}{status}  {dim}{desc}{reset}",
            padding = " ".repeat(pad),
            desc = tool.description,
            dim = color::dim(),
            reset = color::reset()
        )
        .map_err(io)?;
    }
    Ok(0)
}

fn info(reg: &ToolRegistry, id: &str, json: bool, out: &mut impl Write) -> Result<i32> {
    let Some(tool) = reg.tools.get(id) else {
        writeln!(
            out,
            "{red}✗{reset} Tool '{id}' not found",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    };
    let platform = current_platform_key();
    let loc = locate_tool(tool);
    let active = tool_version::resolve_active(tool);
    if json {
        let body = serde_json::json!({
            "id": tool.id,
            "display_name": tool.display_name,
            "category": tool.category,
            "description": tool.description,
            "install_method": tool.install_method,
            "pip_package": tool.pip_package,
            "source_dir": tool.source_dir,
            "module": tool.module,
            "install_scope": tool.install_scope,
            "distribution_lane": tool.distribution_lane,
            "package_format": tool.package_format,
            "artifact_status": tool.artifact_status,
            "artifact_policy": tool.artifact_policy,
            "artifact_pack_command": tool.artifact_pack_command,
            "artifact_pack_npm_script": tool.artifact_pack_npm_script,
            "artifact_verify_command": tool.artifact_verify_command,
            "artifact_manifest_schema": tool.artifact_manifest_schema,
            "pinned_version": tool.pinned_version,
            "active_version": active.version,
            "active_version_source": active.source.label(),
            "bundleable": tool.bundleable,
            "managed_by_pulp": tool.managed_by_pulp,
            "platform": platform,
            "available_on_platform": tool_available_on_platform(tool, platform),
            "installed": loc.found,
            "location_source": loc.source,
            "path": loc.path.to_string_lossy(),
        });
        let rendered = serde_json::to_string(&body).unwrap_or_else(|_| "{}".to_owned());
        writeln!(out, "{rendered}").map_err(io)?;
        return Ok(0);
    }

    let name = if tool.display_name.is_empty() {
        id
    } else {
        &tool.display_name
    };
    writeln!(out, "{name} {}({id}){}\n", color::dim(), color::reset()).map_err(io)?;
    if !tool.description.is_empty() {
        writeln!(out, "{}\n", tool.description).map_err(io)?;
    }
    writeln!(out, "Install method: {}", tool.install_method).map_err(io)?;
    if !tool.source_dir.is_empty() {
        writeln!(out, "Source dir: {}", tool.source_dir).map_err(io)?;
    }
    if !tool.module.is_empty() {
        writeln!(out, "Run module: {}", tool.module).map_err(io)?;
    }
    if !tool.install_scope.is_empty() {
        writeln!(out, "Install scope: {}", tool.install_scope).map_err(io)?;
    }
    if !tool.distribution_lane.is_empty() {
        writeln!(out, "Distribution lane: {}", tool.distribution_lane).map_err(io)?;
    }
    if !tool.package_format.is_empty() {
        writeln!(out, "Package format: {}", tool.package_format).map_err(io)?;
    }
    if !tool.artifact_status.is_empty() {
        writeln!(out, "Artifact status: {}", tool.artifact_status).map_err(io)?;
    }
    if !tool.artifact_policy.is_empty() {
        writeln!(out, "Artifact policy: {}", tool.artifact_policy).map_err(io)?;
    }
    if !tool.artifact_pack_command.is_empty() {
        writeln!(out, "Artifact pack command: {}", tool.artifact_pack_command).map_err(io)?;
    }
    if !tool.artifact_pack_npm_script.is_empty() {
        writeln!(
            out,
            "Artifact pack npm script: {}",
            tool.artifact_pack_npm_script
        )
        .map_err(io)?;
    }
    if !tool.artifact_verify_command.is_empty() {
        writeln!(
            out,
            "Artifact verify command: {}",
            tool.artifact_verify_command
        )
        .map_err(io)?;
    }
    if !tool.artifact_manifest_schema.is_empty() {
        writeln!(
            out,
            "Artifact manifest schema: {}",
            tool.artifact_manifest_schema
        )
        .map_err(io)?;
    }
    if !tool.pinned_version.is_empty() {
        writeln!(out, "Pinned version: {}", tool.pinned_version).map_err(io)?;
    }
    if !active.version.is_empty() {
        writeln!(
            out,
            "Active version: {} ({})",
            active.version,
            active.source.label()
        )
        .map_err(io)?;
    }
    writeln!(out, "Platform: {platform}").map_err(io)?;
    writeln!(
        out,
        "Available: {}",
        if tool_available_on_platform(tool, platform) {
            "yes"
        } else {
            "no"
        }
    )
    .map_err(io)?;
    if loc.found {
        writeln!(
            out,
            "Installed: yes ({}, {})",
            loc.source,
            loc.path.display()
        )
        .map_err(io)?;
    } else {
        writeln!(out, "Installed: no").map_err(io)?;
    }
    Ok(0)
}

/// `install trace-processor` — the Perfetto query tool is a bare (non-archive)
/// binary, so the built-in verified fetcher downloads + SHA-256-checks it into
/// the same `~/.pulp` location `pulp trace` resolves. No tar/zip delegation to
/// `pulp-cpp` is needed. This is the registry-surfaced peer of `pulp trace
/// fetch`; both share the fetch code and destination.
fn install_trace_processor(version: Option<&str>, out: &mut impl Write) -> Result<i32> {
    if let Some(v) = version {
        if v != crate::cmd::trace_fetch::PINNED_VERSION {
            writeln!(
                out,
                "{dim}note:{reset} trace-processor is pinned to {pin}; ignoring --version {v}",
                dim = color::dim(),
                reset = color::reset(),
                pin = crate::cmd::trace_fetch::PINNED_VERSION,
            )
            .map_err(io)?;
        }
    }
    crate::cmd::trace_fetch::run_fetch(false, out)?;
    Ok(0)
}

fn install(
    reg: &ToolRegistry,
    id: Option<&str>,
    all: bool,
    version: Option<&str>,
    out: &mut impl Write,
) -> Result<i32> {
    if id == Some("trace-processor") {
        return install_trace_processor(version, out);
    }
    // `--all` sweeps every managed tool, but trace-processor is a bare binary
    // installed only by the SHA-256-verified fetcher: the generic archive
    // installer (Rust delegates it to pulp-cpp) cannot verify it and would try
    // to untar a raw binary. Fetch it here so the delegated sweep finds it
    // already present and skips it, instead of failing on "cannot extract".
    if all && reg.tools.contains_key("trace-processor") {
        install_trace_processor(None, out)?;
    }
    // A `--version` is a durable user pin: record it before installing so
    // resolution (and the re-fetch below) honor it, and so it survives a
    // future Pulp registry-pin bump.
    if let (Some(id), Some(v)) = (id, version) {
        tool_version::set_override(id, v)?;
        writeln!(
            out,
            "{green}✓{reset} Pinned {id} to {v} {dim}(user override){reset}",
            green = color::green(),
            reset = color::reset(),
            dim = color::dim(),
        )
        .map_err(io)?;
    }
    if let Some(id) = id {
        if let Some(tool) = reg.tools.get(id) {
            let active = tool_version::resolve_active(tool);
            report_active_version(id, &active, out)?;
        }
    }

    // Archive download + tar/zip/xz extraction + xattr cleanup is
    // ~500 LOC of new deps (tar + flate2 + zip). Delegate to pulp-cpp
    // when present; print the stub when it's not on PATH so
    // CI/sandboxed callers see a clear error. `--version` is stripped
    // from the delegated argv (the C++ installer does not accept it) and
    // forwarded via env instead.
    let id_for_env = id.filter(|_| version.is_some());
    let argv = strip_version_flag(crate::fallthrough::current_argv_tail());
    delegate_install(id_for_env, &argv, out)
}

/// `update <id> [--version <v>]` — re-install a managed tool.
fn update(
    reg: &ToolRegistry,
    id: &str,
    version: Option<&str>,
    out: &mut impl Write,
) -> Result<i32> {
    let Some(tool) = reg.tools.get(id) else {
        writeln!(
            out,
            "{red}✗{reset} Tool '{id}' not found",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    };
    if !tool.managed_by_pulp {
        writeln!(
            out,
            "{red}✗{reset} {id} is not a pulp-managed tool; nothing to update",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }

    match version {
        Some(v) => {
            // Explicit version → durable user pin.
            tool_version::set_override(id, v)?;
            writeln!(
                out,
                "{green}✓{reset} Pinned {id} to {v} {dim}(user override){reset}",
                green = color::green(),
                reset = color::reset(),
                dim = color::dim(),
            )
            .map_err(io)?;
        }
        None => {
            // Bare update re-syncs to the registry pin, dropping any prior
            // user override so the shipped pin becomes active.
            if let Some(prev) = tool_version::clear_override(id)? {
                writeln!(
                    out,
                    "{dim}note:{reset} cleared user override {prev}; tracking registry pin {pin}",
                    dim = color::dim(),
                    reset = color::reset(),
                    pin = tool.pinned_version,
                )
                .map_err(io)?;
            }
        }
    }

    let active = tool_version::resolve_active(tool);
    report_active_version(id, &active, out)?;

    if id == "trace-processor" {
        // Bare binary: re-sync via the verified fetcher (idempotent — fetches
        // only when the pinned artifact is absent), not the pulp-cpp installer.
        return install_trace_processor(None, out);
    }

    // Re-fetch/re-install at the resolved version (delegated to pulp-cpp,
    // forwarding the version via env).
    let argv = vec![
        "tool".to_owned(),
        "install".to_owned(),
        id.to_owned(),
        "--force".to_owned(),
    ];
    delegate_install(Some(id), &argv, out)
}

/// Print the active version + provenance line shared by install / update.
fn report_active_version(id: &str, active: &ActiveVersion, out: &mut impl Write) -> Result<()> {
    if active.version.is_empty() {
        return Ok(());
    }
    writeln!(
        out,
        "Active version for {id}: {} {dim}({}){reset}",
        active.version,
        active.source.label(),
        dim = color::dim(),
        reset = color::reset(),
    )
    .map_err(io)
}

/// Strip a `--version <value>` pair from a delegated argv. The C++ installer
/// reads the version from the registry / env, not this flag.
fn strip_version_flag(tail: Vec<String>) -> Vec<String> {
    let mut kept = Vec::with_capacity(tail.len());
    let mut skip_value = false;
    for a in tail {
        if skip_value {
            skip_value = false;
            continue;
        }
        if a == "--version" {
            skip_value = true;
            continue;
        }
        kept.push(a);
    }
    kept
}

/// Delegate the archive fetch to `pulp-cpp`, forwarding the resolved version
/// via env for the named tool (a durable override is the source of truth; the
/// env var lets a C++ install honor it without touching the committed registry).
fn delegate_install(id_for_env: Option<&str>, argv: &[String], out: &mut impl Write) -> Result<i32> {
    // Forward the resolved version to the `pulp-cpp` child via env. Gated on
    // fallthrough being live so a disabled/sandboxed run (and the unit suite)
    // never mutates process-wide env — the durable override file remains the
    // source of truth either way.
    if !crate::fallthrough::is_fallthrough_disabled() {
        if let Some(id) = id_for_env {
            let resolved = tool_version::env_override(id)
                .or_else(|| tool_version::read_overrides().get(id).cloned());
            if let Some(v) = resolved.filter(|v| !v.is_empty()) {
                std::env::set_var(tool_version::env_var_name(id), v);
            }
        }
    }
    match crate::fallthrough::delegate(argv)? {
        crate::fallthrough::Outcome::Delegated(rc) => Ok(rc),
        crate::fallthrough::Outcome::Disabled | crate::fallthrough::Outcome::NotFound => {
            writeln!(
                out,
                "pulp-rs tool install: archive download + extraction not ported; \
                 install pulp-cpp to enable."
            )
            .map_err(io)?;
            Ok(1)
        }
    }
}

fn uninstall(id: &str, out: &mut impl Write) -> Result<i32> {
    // uninstall_tool validates the id and confines the delete to the managed
    // tree; it returns the exact directory removed so we can name it — deleting
    // silently is the wrong default.
    match uninstall_tool(id)? {
        Some(removed) => {
            writeln!(
                out,
                "{green}✓{reset} Uninstalled {id} {dim}(removed {}){reset}",
                removed.display(),
                green = color::green(),
                reset = color::reset(),
                dim = color::dim(),
            )
            .map_err(io)?;
            Ok(0)
        }
        None => {
            writeln!(
                out,
                "{red}✗{reset} {id} is not installed (pulp-managed); nothing removed",
                red = color::red(),
                reset = color::reset()
            )
            .map_err(io)?;
            Ok(1)
        }
    }
}

fn path(reg: &ToolRegistry, id: &str, out: &mut impl Write) -> Result<i32> {
    let Some(tool) = reg.tools.get(id) else {
        writeln!(
            out,
            "{red}✗{reset} Tool '{id}' not found",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    };
    let loc = locate_tool(tool);
    if loc.found {
        writeln!(out, "{}", loc.path.display()).map_err(io)?;
        Ok(0)
    } else {
        writeln!(
            out,
            "{red}✗{reset} {id} not installed",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        Ok(1)
    }
}

fn run_tool<S: Spawner>(
    reg: &ToolRegistry,
    id: &str,
    args: &[String],
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let Some(tool) = reg.tools.get(id) else {
        writeln!(
            out,
            "{red}✗{reset} Tool '{id}' not found",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    };
    let loc = locate_tool(tool);
    if !loc.found {
        writeln!(
            out,
            "{red}✗{reset} {id} not installed. Run: pulp tool install {id}",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }
    let inv = Invocation::new(loc.path.to_string_lossy().into_owned()).args(args.iter().cloned());
    spawner.run(&inv)
}

// Helper used only in tests to resolve fixtures.
#[cfg(test)]
#[allow(dead_code)]
pub(crate) fn registry_at(p: &std::path::Path) -> Result<ToolRegistry> {
    tool_registry::load(p)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::EnvVarGuard;
    use crate::tool_registry::load;
    use std::fs;
    use std::path::Path;

    fn plant_project(body: &str) -> tempfile::TempDir {
        let td = tempfile::tempdir().unwrap();
        fs::create_dir_all(td.path().join("core")).unwrap();
        fs::write(td.path().join("CMakeLists.txt"), "project(Demo)\n").unwrap();
        let reg_path = td
            .path()
            .join("tools")
            .join("packages")
            .join("tool-registry.json");
        fs::create_dir_all(reg_path.parent().unwrap()).unwrap();
        fs::write(&reg_path, body).unwrap();
        td
    }

    fn registry_body() -> &'static str {
        r#"{
            "schema_version": 1,
            "tools": {
                "uv": {
                    "display_name": "UV",
                    "description": "Python package manager",
                    "install_method": "binary_download",
                    "pinned_version": "0.4.0",
                    "binary_sources": {
                        "macOS-arm64": {"url_template":"x","archive_format":"tar.gz","binary_name":"uv"},
                        "macOS-x64": {"url_template":"x","archive_format":"tar.gz","binary_name":"uv"},
                        "Windows-x64": {"url_template":"x","archive_format":"zip","binary_name":"uv.exe"},
                        "Linux-x64": {"url_template":"x","archive_format":"tar.gz","binary_name":"uv"},
                        "Linux-arm64": {"url_template":"x","archive_format":"tar.gz","binary_name":"uv"}
                    }
                }
            }
        }"#
    }

    fn info_registry_body() -> &'static str {
        r#"{
            "schema_version": 1,
            "tools": {
                "video-proof": {
                    "display_name": "Video Proof",
                    "description": "npm package fixture",
                    "install_method": "npm_package",
                    "npm_package_root": "tools/local-ci",
                    "npm_default_script": "smoke-video-proof",
                    "pinned_version": "0.0.0",
                    "install_scope": "machine",
                    "distribution_lane": "tool_addon",
                    "package_format": "not_pulp_add",
                    "artifact_status": "source_tree_iteration",
                    "artifact_policy": "Keep video proof tooling outside projects.",
                    "artifact_pack_command": "python3 tools/local-ci/pack_video_proof_tool.py --json",
                    "artifact_pack_npm_script": "npm --prefix tools/local-ci run pack-video-proof-tool -- --json",
                    "artifact_verify_command": "python3 tools/local-ci/pack_video_proof_tool.py --verify <manifest> --json",
                    "artifact_manifest_schema": "pulp.video-proof-tool-package.v1"
                }
            }
        }"#
    }

    #[test]
    fn parse_list_simple() {
        assert_eq!(parse_sub(&["list".into()]).unwrap(), Sub::List);
    }

    #[test]
    fn parse_info_captures_id_and_json_flag() {
        let s = parse_sub(&["info".into(), "video-proof".into(), "--json".into()]).unwrap();
        assert_eq!(
            s,
            Sub::Info {
                id: "video-proof".to_owned(),
                json: true,
            }
        );
    }

    #[test]
    fn parse_install_requires_id_or_all() {
        assert!(parse_sub(&["install".into()]).is_err());
        let s = parse_sub(&["install".into(), "uv".into()]).unwrap();
        assert!(matches!(
            s,
            Sub::Install {
                id: Some(_),
                all: false,
                ..
            }
        ));
        let s = parse_sub(&["install".into(), "--all".into()]).unwrap();
        assert!(matches!(s, Sub::Install { all: true, .. }));
    }

    #[test]
    fn parse_run_captures_tail() {
        let s = parse_sub(&["run".into(), "uv".into(), "pip".into(), "list".into()]).unwrap();
        if let Sub::Run { id, args } = s {
            assert_eq!(id, "uv");
            assert_eq!(args, vec!["pip", "list"]);
        } else {
            panic!("wrong variant");
        }
    }

    #[test]
    fn parse_path_missing_id_errors() {
        assert!(parse_sub(&["path".into()]).is_err());
    }

    #[test]
    fn parse_unknown_subcommand() {
        assert!(matches!(
            parse_sub(&["install-all".into()]),
            Err(CliError::UnknownSubcommand)
        ));
    }

    #[test]
    fn list_renders_rows() {
        let td = plant_project(registry_body());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let mut buf = Vec::new();
        list(&reg, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Available tools"));
        assert!(s.contains("uv"));
        assert!(s.contains("Python package manager"));
    }

    #[test]
    fn info_renders_json_metadata() {
        let td = plant_project(info_registry_body());
        let home = td.path().join("pulp-home");
        let _home_guard = EnvVarGuard::set("PULP_HOME", home.to_str().unwrap());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let mut buf = Vec::new();
        let rc = info(&reg, "video-proof", true, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let value: serde_json::Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(value["id"], "video-proof");
        assert_eq!(value["display_name"], "Video Proof");
        assert_eq!(value["install_method"], "npm_package");
        assert_eq!(value["install_scope"], "machine");
        assert_eq!(value["distribution_lane"], "tool_addon");
        assert_eq!(value["package_format"], "not_pulp_add");
        assert_eq!(value["artifact_status"], "source_tree_iteration");
        assert_eq!(
            value["artifact_manifest_schema"],
            "pulp.video-proof-tool-package.v1"
        );
        assert_eq!(value["available_on_platform"], true);
        assert_eq!(value["installed"], false);
    }

    #[test]
    fn info_renders_text_metadata() {
        let td = plant_project(info_registry_body());
        let home = td.path().join("pulp-home");
        let _home_guard = EnvVarGuard::set("PULP_HOME", home.to_str().unwrap());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let mut buf = Vec::new();
        let rc = info(&reg, "video-proof", false, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Install method: npm_package"));
        assert!(s.contains("Install scope: machine"));
        assert!(s.contains("Distribution lane: tool_addon"));
        assert!(s.contains("Package format: not_pulp_add"));
        assert!(s.contains("Artifact status: source_tree_iteration"));
        assert!(s.contains("Artifact manifest schema: pulp.video-proof-tool-package.v1"));
        assert!(s.contains("Available: yes"));
        assert!(s.contains("Installed: no"));
    }

    #[test]
    fn doctor_reports_zero_issues_when_nothing_marked_unavailable() {
        let td = plant_project(registry_body());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = doctor(&reg, None, false, &spawner, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        // UV is "available" or "installed" for current platform.
        assert!(s.contains("UV"));
        assert_eq!(rc, 0);
    }

    #[test]
    fn doctor_target_reports_install_hint_for_missing_tool() {
        let td = plant_project(info_registry_body());
        let home = td.path().join("pulp-home");
        let _home_guard = EnvVarGuard::set("PULP_HOME", home.to_str().unwrap());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = doctor(&reg, Some("video-proof"), false, &spawner, &mut buf).unwrap();
        assert_eq!(rc, 1);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Video Proof"));
        assert!(s.contains("pulp tool install video-proof"));
        assert!(spawner.calls.borrow().is_empty());
    }

    #[test]
    fn doctor_target_unknown_tool_errors_without_health_header() {
        let td = plant_project(info_registry_body());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = doctor(&reg, Some("does-not-exist"), false, &spawner, &mut buf).unwrap();
        assert_eq!(rc, 1);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Tool 'does-not-exist' not found"));
        assert!(!s.contains("Tool Health"));
        assert!(spawner.calls.borrow().is_empty());
    }

    #[test]
    fn doctor_target_reports_smoke_hint_for_installed_npm_tool() {
        let td = plant_project(info_registry_body());
        let home = td.path().join("pulp-home");
        let _home_guard = EnvVarGuard::set("PULP_HOME", home.to_str().unwrap());
        let wrapper = home
            .join("tools")
            .join("npm-packages")
            .join("video-proof")
            .join(if cfg!(windows) { "run.bat" } else { "run.sh" });
        fs::create_dir_all(wrapper.parent().unwrap()).unwrap();
        fs::write(&wrapper, "stub").unwrap();
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = doctor(&reg, Some("video-proof"), false, &spawner, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Video Proof"));
        assert!(s.contains("pulp tool doctor video-proof --run"));
        assert!(spawner.calls.borrow().is_empty());
    }

    #[test]
    fn doctor_target_run_execs_located_wrapper() {
        let td = plant_project(info_registry_body());
        let home = td.path().join("pulp-home");
        let _home_guard = EnvVarGuard::set("PULP_HOME", home.to_str().unwrap());
        let wrapper = home
            .join("tools")
            .join("npm-packages")
            .join("video-proof")
            .join(if cfg!(windows) { "run.bat" } else { "run.sh" });
        fs::create_dir_all(wrapper.parent().unwrap()).unwrap();
        fs::write(&wrapper, "stub").unwrap();
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = doctor(&reg, Some("video-proof"), true, &spawner, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Video Proof smoke check passed"));
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].program, wrapper.to_string_lossy().into_owned());
        assert!(calls[0].args.is_empty());
    }

    #[test]
    fn uninstall_reports_missing_entry() {
        // Intentionally no prior install — should return rc=1.
        let mut buf = Vec::new();
        // Use a guaranteed-not-installed id.
        let rc = uninstall("__pulp_rs_tool_nope_34f9b7__", &mut buf).unwrap();
        assert_eq!(rc, 1);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("is not installed"));
    }

    #[test]
    fn install_prints_stub_notice() {
        let _guard = EnvVarGuard::set(crate::fallthrough::DISABLE_ENV, "1");
        let td = plant_project(registry_body());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let mut buf = Vec::new();
        let rc = install(&reg, Some("uv"), false, None, &mut buf).unwrap();
        assert_eq!(rc, 1);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("not ported"));
    }

    #[test]
    fn print_help_mentions_all_subcommands() {
        let mut buf = Vec::new();
        print_help(&mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        for cmd in [
            "list",
            "info",
            "install",
            "uninstall",
            "path",
            "run",
            "doctor",
        ] {
            assert!(s.contains(cmd), "missing {cmd}");
        }
    }

    // ── tool.rs parse_sub edge coverage ───────────────────────────

    #[test]
    fn parse_sub_no_args_returns_help() {
        let s = parse_sub(&[]).unwrap();
        assert!(matches!(s, Sub::Help));
    }

    #[test]
    fn parse_sub_list() {
        let s = parse_sub(&["list".to_owned()]).unwrap();
        assert!(matches!(s, Sub::List));
    }

    #[test]
    fn parse_sub_info_requires_id() {
        let err = parse_sub(&["info".to_owned()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool info"));
    }

    #[test]
    fn parse_sub_info_rejects_extra_positional() {
        let err = parse_sub(&["info".to_owned(), "uv".to_owned(), "extra".to_owned()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool info"));
    }

    #[test]
    fn parse_sub_doctor() {
        let s = parse_sub(&["doctor".to_owned()]).unwrap();
        assert!(matches!(
            s,
            Sub::Doctor {
                id: None,
                run: false
            }
        ));
    }

    #[test]
    fn parse_sub_doctor_accepts_target_and_run_flag() {
        let s = parse_sub(&[
            "doctor".to_owned(),
            "video-proof".to_owned(),
            "--run".to_owned(),
        ])
        .unwrap();
        match s {
            Sub::Doctor { id, run } => {
                assert_eq!(id.as_deref(), Some("video-proof"));
                assert!(run);
            }
            other => panic!("expected Doctor, got {other:?}"),
        }
    }

    #[test]
    fn parse_sub_doctor_accepts_run_before_target() {
        let s = parse_sub(&[
            "doctor".to_owned(),
            "--run".to_owned(),
            "video-proof".to_owned(),
        ])
        .unwrap();
        match s {
            Sub::Doctor { id, run } => {
                assert_eq!(id.as_deref(), Some("video-proof"));
                assert!(run);
            }
            other => panic!("expected Doctor, got {other:?}"),
        }
    }

    #[test]
    fn parse_sub_doctor_rejects_run_without_target() {
        let err = parse_sub(&["doctor".to_owned(), "--run".to_owned()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool doctor"));
    }

    #[test]
    fn parse_sub_doctor_rejects_extra_positional() {
        let err = parse_sub(&[
            "doctor".to_owned(),
            "video-proof".to_owned(),
            "extra".to_owned(),
        ])
        .unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool doctor"));
    }

    #[test]
    fn parse_sub_install_with_id_only() {
        let s = parse_sub(&["install".to_owned(), "uv".to_owned()]).unwrap();
        match s {
            Sub::Install { id, all, force, version } => {
                assert_eq!(id.as_deref(), Some("uv"));
                assert!(!all);
                assert!(!force);
                assert!(version.is_none());
            }
            other => panic!("expected Install, got {other:?}"),
        }
    }

    #[test]
    fn parse_sub_install_with_all_and_force() {
        let s = parse_sub(&[
            "install".to_owned(),
            "--all".to_owned(),
            "--force".to_owned(),
        ])
        .unwrap();
        match s {
            Sub::Install { id, all, force, version } => {
                assert!(id.is_none());
                assert!(all);
                assert!(force);
                assert!(version.is_none());
            }
            other => panic!("expected Install, got {other:?}"),
        }
    }

    #[test]
    fn parse_sub_install_no_id_no_all_errors() {
        let err = parse_sub(&["install".to_owned()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool install"));
    }

    #[test]
    fn parse_sub_uninstall_requires_id() {
        let err = parse_sub(&["uninstall".to_owned()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool uninstall"));
    }

    #[test]
    fn parse_sub_path_requires_id() {
        let err = parse_sub(&["path".to_owned()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool path"));
    }

    #[test]
    fn parse_sub_path_captures_id() {
        let s = parse_sub(&["path".to_owned(), "uv".to_owned()]).unwrap();
        match s {
            Sub::Path(id) => assert_eq!(id, "uv"),
            other => panic!("expected Path, got {other:?}"),
        }
    }

    #[test]
    fn parse_sub_run_requires_id() {
        let err = parse_sub(&["run".to_owned()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool run"));
    }

    #[test]
    fn parse_sub_run_captures_id_and_args() {
        let s = parse_sub(&[
            "run".to_owned(),
            "uv".to_owned(),
            "pip".to_owned(),
            "install".to_owned(),
            "rich".to_owned(),
        ])
        .unwrap();
        match s {
            Sub::Run { id, args } => {
                assert_eq!(id, "uv");
                assert_eq!(args, vec!["pip", "install", "rich"]);
            }
            other => panic!("expected Run, got {other:?}"),
        }
    }

    #[test]
    fn parse_sub_unknown_top_level_errors() {
        let err = parse_sub(&["nonsense".to_owned()]).unwrap_err();
        assert!(matches!(err, CliError::UnknownSubcommand) || err.to_string().contains("unknown"));
    }

    // ── update verb + version-override parsing ────────────────────

    fn managed_registry_body() -> &'static str {
        r#"{
            "schema_version": 1,
            "tools": {
                "uv": {
                    "display_name": "UV",
                    "description": "Python package manager",
                    "install_method": "binary_download",
                    "pinned_version": "0.6.14",
                    "managed_by_pulp": true,
                    "binary_sources": {
                        "macOS-arm64": {"url_template":"x","archive_format":"tar.gz","binary_name":"uv"},
                        "macOS-x64": {"url_template":"x","archive_format":"tar.gz","binary_name":"uv"},
                        "Windows-x64": {"url_template":"x","archive_format":"zip","binary_name":"uv.exe"},
                        "Linux-x64": {"url_template":"x","archive_format":"tar.gz","binary_name":"uv"},
                        "Linux-arm64": {"url_template":"x","archive_format":"tar.gz","binary_name":"uv"}
                    }
                }
            }
        }"#
    }

    #[test]
    fn parse_update_captures_id() {
        let s = parse_sub(&["update".into(), "uv".into()]).unwrap();
        assert_eq!(
            s,
            Sub::Update {
                id: "uv".to_owned(),
                version: None,
            }
        );
    }

    #[test]
    fn parse_update_captures_id_and_version() {
        let s = parse_sub(&["update".into(), "uv".into(), "--version".into(), "0.7.0".into()])
            .unwrap();
        assert_eq!(
            s,
            Sub::Update {
                id: "uv".to_owned(),
                version: Some("0.7.0".to_owned()),
            }
        );
    }

    #[test]
    fn parse_update_version_flag_before_id() {
        let s = parse_sub(&["update".into(), "--version".into(), "1.0".into(), "uv".into()])
            .unwrap();
        assert_eq!(
            s,
            Sub::Update {
                id: "uv".to_owned(),
                version: Some("1.0".to_owned()),
            }
        );
    }

    #[test]
    fn parse_update_requires_id() {
        let err = parse_sub(&["update".into()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool update"));
    }

    #[test]
    fn parse_update_version_needs_value() {
        let err = parse_sub(&["update".into(), "uv".into(), "--version".into()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool update"));
    }

    #[test]
    fn parse_update_rejects_extra_positional() {
        let err =
            parse_sub(&["update".into(), "uv".into(), "extra".into()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool update"));
    }

    #[test]
    fn parse_install_captures_version() {
        let s = parse_sub(&["install".into(), "uv".into(), "--version".into(), "0.7.0".into()])
            .unwrap();
        match s {
            Sub::Install { id, version, .. } => {
                assert_eq!(id.as_deref(), Some("uv"));
                assert_eq!(version.as_deref(), Some("0.7.0"));
            }
            other => panic!("expected Install, got {other:?}"),
        }
    }

    #[test]
    fn parse_install_version_needs_value() {
        let err = parse_sub(&["install".into(), "uv".into(), "--version".into()]).unwrap_err();
        assert!(err.to_string().contains("Usage: pulp tool install"));
    }

    #[test]
    fn strip_version_flag_removes_pair() {
        let out = strip_version_flag(vec![
            "tool".into(),
            "install".into(),
            "uv".into(),
            "--version".into(),
            "0.7.0".into(),
            "--force".into(),
        ]);
        assert_eq!(out, vec!["tool", "install", "uv", "--force"]);
    }

    #[test]
    fn update_with_version_persists_user_override() {
        let td = plant_project(managed_registry_body());
        let home = td.path().join("pulp-home");
        let _env = EnvVarGuard::set_many(&[
            (crate::fallthrough::DISABLE_ENV, Some("1")),
            ("PULP_HOME", Some(home.to_str().unwrap())),
            ("PULP_TOOL_UV_VERSION", None),
        ]);
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();

        let mut buf = Vec::new();
        // Delegate is disabled → re-install prints the stub; the override
        // persistence + reporting still happens first.
        let _ = update(&reg, "uv", Some("0.7.0"), &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Pinned uv to 0.7.0"), "{s}");
        assert!(s.contains("Active version for uv: 0.7.0"), "{s}");
        assert!(s.contains("user override"), "{s}");
        assert_eq!(
            tool_version::read_overrides().get("uv").map(String::as_str),
            Some("0.7.0")
        );
    }

    #[test]
    fn update_bare_clears_prior_override_and_tracks_pin() {
        let td = plant_project(managed_registry_body());
        let home = td.path().join("pulp-home");
        let _env = EnvVarGuard::set_many(&[
            (crate::fallthrough::DISABLE_ENV, Some("1")),
            ("PULP_HOME", Some(home.to_str().unwrap())),
            ("PULP_TOOL_UV_VERSION", None),
        ]);
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();

        tool_version::set_override("uv", "0.9.9").unwrap();
        let mut buf = Vec::new();
        let _ = update(&reg, "uv", None, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("cleared user override 0.9.9"), "{s}");
        assert!(s.contains("Active version for uv: 0.6.14"), "{s}");
        assert!(s.contains("registry pin"), "{s}");
        assert!(tool_version::read_overrides().get("uv").is_none());
    }

    #[test]
    fn update_unknown_tool_errors() {
        let td = plant_project(managed_registry_body());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let mut buf = Vec::new();
        let rc = update(&reg, "does-not-exist", None, &mut buf).unwrap();
        assert_eq!(rc, 1);
        assert!(String::from_utf8(buf).unwrap().contains("not found"));
    }

    #[test]
    fn update_rejects_unmanaged_tool() {
        // `registry_body`'s uv has no managed_by_pulp flag (defaults false).
        let td = plant_project(registry_body());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let mut buf = Vec::new();
        let rc = update(&reg, "uv", None, &mut buf).unwrap();
        assert_eq!(rc, 1);
        assert!(String::from_utf8(buf)
            .unwrap()
            .contains("not a pulp-managed tool"));
    }

    #[test]
    fn info_json_surfaces_active_version_and_source() {
        let td = plant_project(managed_registry_body());
        let home = td.path().join("pulp-home");
        let _env = EnvVarGuard::set_many(&[
            ("PULP_HOME", Some(home.to_str().unwrap())),
            ("PULP_TOOL_UV_VERSION", None),
        ]);
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();

        let mut buf = Vec::new();
        info(&reg, "uv", true, &mut buf).unwrap();
        let value: serde_json::Value = serde_json::from_slice(&buf).unwrap();
        assert_eq!(value["active_version"], "0.6.14");
        assert_eq!(value["active_version_source"], "registry pin");

        tool_version::set_override("uv", "0.7.0").unwrap();
        let mut buf2 = Vec::new();
        info(&reg, "uv", true, &mut buf2).unwrap();
        let value2: serde_json::Value = serde_json::from_slice(&buf2).unwrap();
        assert_eq!(value2["active_version"], "0.7.0");
        assert_eq!(value2["active_version_source"], "user override");
    }

    #[test]
    fn help_mentions_update_verb() {
        let mut buf = Vec::new();
        print_help(&mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("update <tool>"));
        assert!(s.contains("PULP_TOOL_<ID>_VERSION"));
    }

    #[test]
    fn run_help_short_circuits_without_registry() {
        // Help is the only sub that doesn't need a tool-registry.json
        // — make sure that bypass works.
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = run(&Sub::Help, &spawner, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let s = String::from_utf8(buf).unwrap();
        assert!(
            s.contains("Usage: pulp tool"),
            "missing usage in help: {s:?}"
        );
    }

    // ── trace-processor: the verified-fetcher install path ─────────

    /// Plant the pinned host binary under `home` so `run_fetch` short-circuits
    /// to "already present" (no network). Returns false on a host with no pin.
    fn plant_trace_processor(home: &Path) -> bool {
        use crate::cmd::trace_fetch;
        let Some(key) = trace_fetch::host_platform_key() else {
            return false;
        };
        let pin = trace_fetch::pin_for(key).unwrap();
        let path = trace_fetch::pinned_cache_path_under(home, pin);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, "stub-binary").unwrap();
        true
    }

    #[test]
    fn install_trace_processor_reports_already_present() {
        let td = tempfile::tempdir().unwrap();
        let _home = EnvVarGuard::set("PULP_HOME", td.path().to_str().unwrap());
        if !plant_trace_processor(td.path()) {
            return; // unsupported host — nothing pinned to install
        }
        let mut buf = Vec::new();
        let rc = install_trace_processor(None, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("already present"), "unexpected output: {s:?}");
    }

    #[test]
    fn install_trace_processor_ignores_a_version_override() {
        let td = tempfile::tempdir().unwrap();
        let _home = EnvVarGuard::set("PULP_HOME", td.path().to_str().unwrap());
        if !plant_trace_processor(td.path()) {
            return;
        }
        let mut buf = Vec::new();
        install_trace_processor(Some("v99.9"), &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(
            s.contains("ignoring --version v99.9")
                && s.contains(crate::cmd::trace_fetch::PINNED_VERSION),
            "expected a pin-override note: {s:?}"
        );
    }

    #[test]
    fn install_all_prefetches_trace_processor_via_the_fetcher() {
        // `--all` must fetch trace-processor through the verified fetcher (not
        // the delegated archive installer). With the binary pre-planted the
        // fetcher short-circuits to "already present"; its appearance proves the
        // pre-fetch ran before delegation. Fallthrough is disabled so the
        // delegated tail is an inert stub, never a real pulp-cpp / network call.
        let td = tempfile::tempdir().unwrap();
        // ENV_LOCK is non-reentrant — set both vars through one guard, not two.
        let home = td.path().to_path_buf();
        let _env = EnvVarGuard::set_many(&[
            ("PULP_HOME", Some(home.to_str().unwrap())),
            (crate::fallthrough::DISABLE_ENV, Some("1")),
        ]);
        if !plant_trace_processor(td.path()) {
            return;
        }
        let proj = plant_project(registry_with_trace_processor());
        let reg = load(
            &proj
                .path()
                .join("tools")
                .join("packages")
                .join("tool-registry.json"),
        )
        .unwrap();
        assert!(reg.tools.contains_key("trace-processor"));

        let mut buf = Vec::new();
        install(&reg, None, true, None, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("already present"), "no pre-fetch marker: {s:?}");
    }

    fn registry_with_trace_processor() -> &'static str {
        r#"{
            "schema_version": 1,
            "tools": {
                "trace-processor": {
                    "display_name": "Perfetto trace_processor",
                    "description": "query tool",
                    "install_method": "binary_download",
                    "pinned_version": "v57.2",
                    "managed_by_pulp": true,
                    "binary_sources": {
                        "macOS-arm64": {"url_template":"x","binary_name":"trace_processor_shell"},
                        "macOS-x64": {"url_template":"x","binary_name":"trace_processor_shell"},
                        "Linux-x64": {"url_template":"x","binary_name":"trace_processor_shell"},
                        "Linux-arm64": {"url_template":"x","binary_name":"trace_processor_shell"},
                        "Windows-x64": {"url_template":"x","binary_name":"trace_processor_shell.exe"}
                    }
                }
            }
        }"#
    }
}
