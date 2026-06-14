//! `pulp-rs tool` — list / info / install / uninstall / path / run / doctor.
//!
//! # Phase 6d scope
//!
//! Everything except archive/python `install` is ported Rust-native:
//!
//! | Subcommand  | Status      | Notes                                        |
//! |-------------|-------------|----------------------------------------------|
//! | `list`      | Ported      | Two-column table; color-aware.               |
//! | `info`      | Ported      | Install/distribution metadata; JSON mode.    |
//! | `uninstall` | Ported      | `rm -rf` of `$PULP_HOME/tools/<id>`.          |
//! | `path`      | Ported      | Prints absolute path via `locate_tool`.      |
//! | `run`       | Ported      | Exec via `Spawner`.                          |
//! | `doctor`    | Ported      | All-tool and focused health/smoke checks.    |
//! | `install`   | Partial     | `npm_package` tools install natively;        |
//! |             |             | archive/python installs fall through.        |
//!
//! Archive/python installs still dispatch (so `pulp-rs tool install <id>`
//! returns the C++ behavior when `pulp-cpp` is present, or a clean "not yet
//! ported" exit code otherwise). See `tool_registry.cpp` for the reference.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use crate::color;
use crate::error::{CliError, Result};
use crate::proc::{Invocation, Spawner};
use crate::tool_registry::{
    self, current_platform_key, load, locate_tool, uninstall_tool, ToolDescriptor, ToolRegistry,
};

/// Parsed subcommand token.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Sub {
    /// `pulp tool` (no sub) — print help.
    Help,
    /// `list`.
    List,
    /// `info <id> [--json]`.
    Info {
        /// Tool id.
        id: String,
        /// Emit machine-readable JSON.
        json: bool,
    },
    /// `install <id>` / `install --all` / `install <id> --force`.
    Install {
        /// Tool id to install, `None` when `--all` is set.
        id: Option<String>,
        /// `--all`.
        all: bool,
        /// `--force`.
        force: bool,
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
        /// Optional focused tool id.
        id: Option<String>,
        /// Execute the tool's smoke command when focused.
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
                match a.as_str() {
                    "--json" => json = true,
                    _ if id.is_none() => id = Some(a.clone()),
                    _ => {
                        return Err(CliError::BadUsage(
                            "Usage: pulp tool info <tool-id> [--json]".to_owned(),
                        ));
                    }
                }
            }
            let id = id.ok_or_else(|| {
                CliError::BadUsage("Usage: pulp tool info <tool-id> [--json]".to_owned())
            })?;
            Ok(Sub::Info { id, json })
        }
        "install" => {
            let mut id = None;
            let mut all = false;
            let mut force = false;
            for a in &args[1..] {
                match a.as_str() {
                    "--all" => all = true,
                    "--force" => force = true,
                    _ => id = Some(a.clone()),
                }
            }
            if !all && id.is_none() {
                return Err(CliError::BadUsage(
                    "Usage: pulp tool install <tool-id> [--force]".to_owned(),
                ));
            }
            Ok(Sub::Install { id, all, force })
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
                match a.as_str() {
                    "--run" => run = true,
                    _ if id.is_none() => id = Some(a.clone()),
                    _ => {
                        return Err(CliError::BadUsage(
                            "Usage: pulp tool doctor [tool-id] [--run]".to_owned(),
                        ));
                    }
                }
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
    let Some(reg_path) = tool_registry::find_tool_registry_path(&cwd) else {
        return Err(CliError::Other(
            "Tool registry not found at tools/packages/tool-registry.json".to_owned(),
        ));
    };
    let reg = load(&reg_path)?;

    match sub {
        Sub::Help => unreachable!(), // handled above
        Sub::List => list(&reg, out),
        Sub::Info { id, json } => info(&reg, id, *json, out),
        Sub::Install { id, all, force } => {
            install(&reg, &reg_path, id.as_deref(), *all, *force, spawner, out)
        }
        Sub::Uninstall(id) => uninstall(id, out),
        Sub::Path(id) => path(&reg, id, out),
        Sub::Run { id, args } => run_tool(&reg, id, args, spawner, out),
        Sub::Doctor { id, run } => doctor(&reg, id.as_deref(), *run, spawner, out),
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
        \x20 info <tool> [--json]    Show install metadata for a tool\n\
        \x20 install <tool>          Download and install a tool\n\
        \x20 install --all           Install all tools for current platform\n\
        \x20 uninstall <tool>        Remove a pulp-managed tool\n\
        \x20 path <tool>             Print path to a tool's binary\n\
        \x20 run <tool> [args]       Run a tool with arguments\n\
        \x20 doctor [tool] [--run]   Check tool health\n",
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
    if json {
        writeln!(
            out,
            "{{\"id\":\"{}\",\"display_name\":\"{}\",\"category\":\"{}\",\"description\":\"{}\",\"install_method\":\"{}\",\"install_scope\":\"{}\",\"distribution_lane\":\"{}\",\"package_format\":\"{}\",\"artifact_status\":\"{}\",\"artifact_policy\":\"{}\",\"artifact_pack_command\":\"{}\",\"artifact_pack_npm_script\":\"{}\",\"artifact_manifest_schema\":\"{}\",\"pinned_version\":\"{}\",\"bundleable\":{},\"managed_by_pulp\":{},\"platform\":\"{}\",\"available_on_platform\":{},\"installed\":{},\"location_source\":\"{}\",\"path\":\"{}\"}}",
            json_escape(&tool.id),
            json_escape(&tool.display_name),
            json_escape(&tool.category),
            json_escape(&tool.description),
            json_escape(&tool.install_method),
            json_escape(&tool.install_scope),
            json_escape(&tool.distribution_lane),
            json_escape(&tool.package_format),
            json_escape(&tool.artifact_status),
            json_escape(&tool.artifact_policy),
            json_escape(&tool.artifact_pack_command),
            json_escape(&tool.artifact_pack_npm_script),
            json_escape(&tool.artifact_manifest_schema),
            json_escape(&tool.pinned_version),
            tool.bundleable,
            tool.managed_by_pulp,
            json_escape(platform),
            tool_available_on_platform(tool, platform),
            loc.found,
            json_escape(&loc.source),
            json_escape(&loc.path.to_string_lossy()),
        )
        .map_err(io)?;
        return Ok(0);
    }

    writeln!(
        out,
        "{} {dim}({}){reset}\n",
        display_name(tool),
        tool.id,
        dim = color::dim(),
        reset = color::reset()
    )
    .map_err(io)?;
    if !tool.description.is_empty() {
        writeln!(out, "{}\n", tool.description).map_err(io)?;
    }
    writeln!(out, "Install method: {}", tool.install_method).map_err(io)?;
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
        writeln!(out, "Artifact pack npm script: {}", tool.artifact_pack_npm_script).map_err(io)?;
    }
    if !tool.artifact_manifest_schema.is_empty() {
        writeln!(out, "Artifact manifest schema: {}", tool.artifact_manifest_schema).map_err(io)?;
    }
    if !tool.pinned_version.is_empty() {
        writeln!(out, "Pinned version: {}", tool.pinned_version).map_err(io)?;
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

fn status_label(
    tool: &ToolDescriptor,
    loc: &tool_registry::LocateResult,
    platform: &str,
) -> String {
    if loc.found && loc.source == "pulp-managed" {
        format!("{}installed{}", color::green(), color::reset())
    } else if loc.found {
        format!(
            "{}system ({}){}",
            color::yellow(),
            loc.path.display(),
            color::reset()
        )
    } else if tool_available_on_platform(tool, platform) {
        format!("{}available{}", color::dim(), color::reset())
    } else {
        format!(
            "{}not available for {platform}{}",
            color::red(),
            color::reset()
        )
    }
}

fn install<S: Spawner>(
    reg: &ToolRegistry,
    reg_path: &Path,
    id: Option<&str>,
    all: bool,
    force: bool,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    if all {
        let mut rc = 0;
        let mut delegated = false;
        for (tool_id, tool) in &reg.tools {
            if tool.install_method == "npm_package" {
                rc |= install_npm_tool(tool, reg_path, force, spawner, out)?;
            } else {
                delegated = true;
                writeln!(
                    out,
                    "pulp-rs tool install: {tool_id} uses {}; delegating non-npm installers.",
                    tool.install_method
                )
                .map_err(io)?;
            }
        }
        if delegated {
            rc |= delegate_install(out)?;
        }
        return Ok(rc);
    }

    let Some(id) = id else {
        writeln!(out, "Usage: pulp tool install <tool-id> [--force]").map_err(io)?;
        return Ok(1);
    };
    let Some(tool) = reg.tools.get(id) else {
        writeln!(
            out,
            "{red}✗{reset} Tool '{id}' not found in registry",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    };

    if tool.install_method == "npm_package" {
        return install_npm_tool(tool, reg_path, force, spawner, out);
    }

    delegate_install(out)
}

fn delegate_install(out: &mut impl Write) -> Result<i32> {
    // Phase 7: archive download + tar/zip/xz extraction + xattr cleanup is
    // still delegated to pulp-cpp. The native path above covers repo-local
    // npm tools such as `video-proof` without adding archive dependencies.
    let argv = crate::fallthrough::current_argv_tail();
    match crate::fallthrough::delegate(&argv)? {
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

fn install_npm_tool<S: Spawner>(
    tool: &ToolDescriptor,
    reg_path: &Path,
    force: bool,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let wrapper = npm_tool_wrapper_path(tool);
    if !force && wrapper.is_file() {
        writeln!(
            out,
            "{green}✓{reset} Installed {} {}",
            display_name(tool),
            tool.pinned_version,
            green = color::green(),
            reset = color::reset()
        )
        .map_err(io)?;
        writeln!(
            out,
            "  {dim}{}{reset}",
            wrapper.display(),
            dim = color::dim(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(0);
    }

    let Some(npm) = crate::proc::which("npm") else {
        writeln!(
            out,
            "{red}✗{reset} npm not found on PATH",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    };

    if tool.npm_package_root.is_empty() {
        writeln!(
            out,
            "{red}✗{reset} npm_package_root is required for npm_package tools",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }

    let package_root = package_root_from_registry(reg_path, &tool.npm_package_root)?;
    if !package_root.join("package.json").is_file() {
        writeln!(
            out,
            "{red}✗{reset} package.json not found at {}",
            package_root.display(),
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }

    writeln!(
        out,
        "  Installing npm dependencies for {}...",
        display_name(tool)
    )
    .map_err(io)?;
    let rc = spawner.run(
        &Invocation::new(npm.to_string_lossy().into_owned())
            .arg("--prefix")
            .arg(package_root.to_string_lossy().into_owned())
            .arg("install"),
    )?;
    if rc != 0 {
        writeln!(
            out,
            "{red}✗{reset} npm install failed with exit code {rc}",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }

    let install_dir = npm_tool_dir(tool);
    fs::create_dir_all(&install_dir).map_err(|e| CliError::io(install_dir.clone(), e))?;
    fs::write(&wrapper, npm_wrapper_body(tool, &package_root))
        .map_err(|e| CliError::io(wrapper.clone(), e))?;
    make_executable(&wrapper)?;

    let manifest = format!(
        "{{\n  \"tool_id\": \"{}\",\n  \"version\": \"{}\",\n  \"method\": \"npm_package\",\n  \"package_root\": \"{}\",\n  \"wrapper_path\": \"{}\"\n}}\n",
        json_escape(&tool.id),
        json_escape(&tool.pinned_version),
        json_escape(package_root.to_string_lossy().as_ref()),
        json_escape(wrapper.to_string_lossy().as_ref()),
    );
    fs::write(install_dir.join("manifest.json"), manifest)
        .map_err(|e| CliError::io(install_dir.join("manifest.json"), e))?;

    writeln!(
        out,
        "{green}✓{reset} Installed {} {}",
        display_name(tool),
        tool.pinned_version,
        green = color::green(),
        reset = color::reset()
    )
    .map_err(io)?;
    writeln!(
        out,
        "  {dim}{}{reset}",
        wrapper.display(),
        dim = color::dim(),
        reset = color::reset()
    )
    .map_err(io)?;
    Ok(0)
}

fn display_name(tool: &ToolDescriptor) -> &str {
    if tool.display_name.is_empty() {
        &tool.id
    } else {
        &tool.display_name
    }
}

fn package_root_from_registry(reg_path: &Path, raw_root: &str) -> Result<PathBuf> {
    let mut root = PathBuf::from(raw_root);
    if root.is_relative() {
        let repo_root = reg_path
            .parent()
            .and_then(Path::parent)
            .and_then(Path::parent)
            .ok_or_else(|| {
                CliError::Other(format!(
                    "invalid tool registry path: {}",
                    reg_path.display()
                ))
            })?;
        root = repo_root.join(root);
    }
    Ok(root.canonicalize().unwrap_or(root))
}

fn npm_tool_dir(tool: &ToolDescriptor) -> PathBuf {
    tool_registry::tools_dir()
        .join("npm-packages")
        .join(&tool.id)
}

fn npm_tool_wrapper_path(tool: &ToolDescriptor) -> PathBuf {
    if cfg!(windows) {
        npm_tool_dir(tool).join("run.bat")
    } else {
        npm_tool_dir(tool).join("run.sh")
    }
}

fn npm_wrapper_body(tool: &ToolDescriptor, package_root: &Path) -> String {
    let default_script = if tool.npm_default_script.is_empty() {
        "smoke-video-proof"
    } else {
        &tool.npm_default_script
    };
    let root = package_root.to_string_lossy();
    if cfg!(windows) {
        format!(
            "@echo off\r\nset PULP_VIDEO_PROOF_PACKAGE_ROOT={root}\r\nif \"%1\"==\"\" (\r\n  npm --prefix \"{root}\" run {default_script}\r\n) else (\r\n  npm --prefix \"{root}\" run %*\r\n)\r\n"
        )
    } else {
        format!(
            "#!/bin/sh\nset -e\nPACKAGE_ROOT='{}'\nexport PULP_VIDEO_PROOF_PACKAGE_ROOT=\"$PACKAGE_ROOT\"\nif [ \"$#\" -eq 0 ]; then\n  exec npm --prefix \"$PACKAGE_ROOT\" run {default_script}\nfi\nexec npm --prefix \"$PACKAGE_ROOT\" run \"$@\"\n",
            shell_single_quote(&root)
        )
    }
}

fn shell_single_quote(value: &str) -> String {
    value.replace('\'', "'\"'\"'")
}

fn json_escape(value: &str) -> String {
    value
        .replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('\n', "\\n")
}

fn make_executable(path: &Path) -> Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mut perms = fs::metadata(path)
            .map_err(|e| CliError::io(path.to_path_buf(), e))?
            .permissions();
        perms.set_mode(perms.mode() | 0o110);
        fs::set_permissions(path, perms).map_err(|e| CliError::io(path.to_path_buf(), e))?;
    }
    Ok(())
}

fn uninstall(id: &str, out: &mut impl Write) -> Result<i32> {
    if uninstall_tool(id)? {
        writeln!(
            out,
            "{green}✓{reset} Uninstalled {id}",
            green = color::green(),
            reset = color::reset()
        )
        .map_err(io)?;
        Ok(0)
    } else {
        writeln!(
            out,
            "{red}✗{reset} {id} is not installed (pulp-managed)",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        Ok(1)
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

fn doctor<S: Spawner>(
    reg: &ToolRegistry,
    focused_id: Option<&str>,
    run: bool,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let platform = current_platform_key();
    writeln!(
        out,
        "Tool Health {dim}({platform}){reset}:\n",
        dim = color::dim(),
        reset = color::reset()
    )
    .map_err(io)?;
    if let Some(id) = focused_id {
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
        if !tool_available_on_platform(tool, platform) {
            writeln!(
                out,
                "  {red}✗{reset} {} — not available for {platform}",
                display_name(tool),
                red = color::red(),
                reset = color::reset()
            )
            .map_err(io)?;
            return Ok(1);
        }
        let loc = locate_tool(tool);
        if !loc.found {
            writeln!(
                out,
                "  {red}✗{reset} {} — not installed {dim}(pulp tool install {id}){reset}",
                display_name(tool),
                red = color::red(),
                reset = color::reset(),
                dim = color::dim()
            )
            .map_err(io)?;
            return Ok(1);
        }
        writeln!(
            out,
            "  {green}✓{reset} {} — {} ({})",
            display_name(tool),
            loc.source,
            loc.path.display(),
            green = color::green(),
            reset = color::reset()
        )
        .map_err(io)?;
        if !run {
            if tool.install_method == "npm_package" {
                writeln!(
                    out,
                    "  {dim}Run `pulp tool doctor {id} --run` to execute its smoke check.{reset}",
                    dim = color::dim(),
                    reset = color::reset()
                )
                .map_err(io)?;
            }
            return Ok(0);
        }
        let rc = spawner.run(&Invocation::new(loc.path.to_string_lossy().into_owned()))?;
        if rc == 0 {
            writeln!(
                out,
                "  {green}✓{reset} {} smoke check passed",
                display_name(tool),
                green = color::green(),
                reset = color::reset()
            )
            .map_err(io)?;
            return Ok(0);
        }
        writeln!(
            out,
            "{red}✗{reset} {} smoke check failed with exit code {rc}",
            display_name(tool),
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(rc);
    }

    let mut issues = 0;
    for (id, tool) in &reg.tools {
        let loc = locate_tool(tool);
        if loc.found {
            writeln!(
                out,
                "  {green}✓{reset} {} — {} ({})",
                display_name(tool),
                loc.source,
                loc.path.display(),
                green = color::green(),
                reset = color::reset()
            )
            .map_err(io)?;
        } else {
            let available = tool_available_on_platform(tool, platform);
            if available {
                writeln!(
                    out,
                    "  {yel}-{reset} {} — not installed {dim}(pulp tool install {id}){reset}",
                    display_name(tool),
                    yel = color::yellow(),
                    reset = color::reset(),
                    dim = color::dim()
                )
                .map_err(io)?;
            } else {
                writeln!(
                    out,
                    "  {red}✗{reset} {} — not available for {platform}",
                    display_name(tool),
                    red = color::red(),
                    reset = color::reset()
                )
                .map_err(io)?;
                issues += 1;
            }
        }
    }
    Ok(i32::from(issues > 0))
}

fn tool_available_on_platform(tool: &ToolDescriptor, platform: &str) -> bool {
    tool.binary_sources.contains_key(platform)
        || tool.install_method == "python_pip"
        || tool.install_method == "npm_package"
}

// Helper used only in tests to resolve fixtures.
#[cfg(test)]
#[allow(dead_code)]
pub(crate) fn registry_at(p: &std::path::Path) -> Result<ToolRegistry> {
    load(p)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

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
                },
                "video-proof": {
                    "display_name": "Video Proof",
                    "category": "developer_tool",
                    "description": "Validation video proof tooling",
                    "install_method": "npm_package",
                    "npm_package_root": "tools/local-ci",
                    "npm_default_script": "smoke-video-proof",
                    "pinned_version": "0.0.0",
                    "managed_by_pulp": true,
                    "bundleable": false,
                    "install_scope": "machine",
                    "distribution_lane": "tool_addon",
                    "package_format": "not_pulp_add",
                    "artifact_status": "source_tree_iteration",
                    "artifact_policy": "Keep Remotion outside shipped artifacts.",
                    "artifact_pack_command": "python3 tools/local-ci/pack_video_proof_tool.py --json",
                    "artifact_pack_npm_script": "npm --prefix tools/local-ci run pack-video-proof-tool -- --json",
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
    fn parse_info_accepts_json_flag() {
        let s = parse_sub(&["info".into(), "video-proof".into(), "--json".into()]).unwrap();
        assert_eq!(
            s,
            Sub::Info {
                id: "video-proof".to_owned(),
                json: true
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
    fn install_prints_stub_notice_for_binary_tool() {
        let _l = crate::test_support::ENV_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        std::env::set_var(crate::fallthrough::DISABLE_ENV, "1");
        let td = plant_project(registry_body());
        let reg_path = td.path().join("tools/packages/tool-registry.json");
        let reg = load(&reg_path).unwrap();
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = install(
            &reg,
            &reg_path,
            Some("uv"),
            false,
            false,
            &spawner,
            &mut buf,
        )
        .unwrap();
        assert_eq!(rc, 1);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("not ported"));
        std::env::remove_var(crate::fallthrough::DISABLE_ENV);
    }

    #[test]
    fn install_npm_tool_writes_wrapper_and_manifest() {
        let _l = crate::test_support::ENV_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let td = plant_project(registry_body());
        fs::create_dir_all(td.path().join("tools/local-ci")).unwrap();
        fs::write(
            td.path().join("tools/local-ci/package.json"),
            r#"{"scripts":{"smoke-video-proof":"node smoke.mjs"}}"#,
        )
        .unwrap();
        let fake_bin = tempfile::tempdir().unwrap();
        let npm_path = if cfg!(windows) {
            fake_bin.path().join("npm.exe")
        } else {
            fake_bin.path().join("npm")
        };
        fs::write(&npm_path, "fake npm\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = fs::metadata(&npm_path).unwrap().permissions();
            perms.set_mode(0o755);
            fs::set_permissions(&npm_path, perms).unwrap();
        }
        let old_path = std::env::var_os("PATH");
        let mut path_entries = vec![fake_bin.path().to_path_buf()];
        if let Some(existing) = old_path.as_ref() {
            path_entries.extend(std::env::split_paths(existing));
        }
        let joined_path = std::env::join_paths(path_entries).unwrap();
        let home = tempfile::tempdir().unwrap();
        std::env::set_var("PATH", joined_path);
        std::env::set_var("PULP_HOME", home.path());

        let reg_path = td.path().join("tools/packages/tool-registry.json");
        let reg = load(&reg_path).unwrap();
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = install(
            &reg,
            &reg_path,
            Some("video-proof"),
            false,
            false,
            &spawner,
            &mut buf,
        )
        .unwrap();

        assert_eq!(rc, 0);
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].program, npm_path.to_string_lossy().as_ref());
        assert_eq!(calls[0].args[0], "--prefix");
        assert!(calls[0].args[1].ends_with("tools/local-ci"));
        assert_eq!(calls[0].args[2], "install");

        let wrapper = home
            .path()
            .join("tools/npm-packages/video-proof")
            .join(if cfg!(windows) { "run.bat" } else { "run.sh" });
        let manifest = home
            .path()
            .join("tools/npm-packages/video-proof/manifest.json");
        assert!(wrapper.is_file());
        assert!(manifest.is_file());
        let wrapper_text = fs::read_to_string(wrapper).unwrap();
        assert!(wrapper_text.contains("smoke-video-proof"));
        let manifest_text = fs::read_to_string(manifest).unwrap();
        assert!(manifest_text.contains("\"method\": \"npm_package\""));
        assert!(String::from_utf8(buf)
            .unwrap()
            .contains("Installed Video Proof"));

        match old_path {
            Some(value) => std::env::set_var("PATH", value),
            None => std::env::remove_var("PATH"),
        }
        std::env::remove_var("PULP_HOME");
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

    #[test]
    fn info_json_reports_install_policy_metadata() {
        let td = plant_project(registry_body());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let mut buf = Vec::new();
        let rc = info(&reg, "video-proof", true, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("\"install_scope\":\"machine\""));
        assert!(s.contains("\"distribution_lane\":\"tool_addon\""));
        assert!(s.contains("\"package_format\":\"not_pulp_add\""));
        assert!(s.contains("\"artifact_status\":\"source_tree_iteration\""));
        assert!(s.contains("\"artifact_pack_command\":\"python3 tools/local-ci/pack_video_proof_tool.py --json\""));
        assert!(s.contains("\"artifact_pack_npm_script\":\"npm --prefix tools/local-ci run pack-video-proof-tool -- --json\""));
        assert!(s.contains("\"artifact_manifest_schema\":\"pulp.video-proof-tool-package.v1\""));
    }

    #[test]
    fn focused_doctor_missing_tool_returns_nonzero() {
        let _l = crate::test_support::ENV_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let td = plant_project(registry_body());
        let home = tempfile::tempdir().unwrap();
        let original_home = std::env::var_os("PULP_HOME");
        std::env::set_var("PULP_HOME", home.path());
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = doctor(&reg, Some("video-proof"), false, &spawner, &mut buf).unwrap();
        match original_home {
            Some(value) => std::env::set_var("PULP_HOME", value),
            None => std::env::remove_var("PULP_HOME"),
        }
        assert_eq!(rc, 1);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("pulp tool install video-proof"));
    }

    #[test]
    fn focused_doctor_run_executes_installed_wrapper() {
        let _l = crate::test_support::ENV_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let td = plant_project(registry_body());
        let home = tempfile::tempdir().unwrap();
        std::env::set_var("PULP_HOME", home.path());
        let wrapper = home.path().join("tools/npm-packages/video-proof/run.sh");
        fs::create_dir_all(wrapper.parent().unwrap()).unwrap();
        fs::write(&wrapper, "#!/bin/sh\nexit 0\n").unwrap();
        let reg = load(&td.path().join("tools/packages/tool-registry.json")).unwrap();
        let spawner = crate::proc::testing::RecordingSpawner::ok();
        let mut buf = Vec::new();
        let rc = doctor(&reg, Some("video-proof"), true, &spawner, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].program, wrapper.to_string_lossy().as_ref());
        assert!(String::from_utf8(buf)
            .unwrap()
            .contains("smoke check passed"));
        std::env::remove_var("PULP_HOME");
    }

    // ── #45 coverage uplift slice 8 — tool.rs parse_sub edges ─────

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
    fn parse_sub_doctor_accepts_focused_run() {
        let s = parse_sub(&[
            "doctor".to_owned(),
            "video-proof".to_owned(),
            "--run".to_owned(),
        ])
        .unwrap();
        assert_eq!(
            s,
            Sub::Doctor {
                id: Some("video-proof".to_owned()),
                run: true
            }
        );
    }

    #[test]
    fn parse_sub_install_with_id_only() {
        let s = parse_sub(&["install".to_owned(), "uv".to_owned()]).unwrap();
        match s {
            Sub::Install { id, all, force } => {
                assert_eq!(id.as_deref(), Some("uv"));
                assert!(!all);
                assert!(!force);
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
            Sub::Install { id, all, force } => {
                assert!(id.is_none());
                assert!(all);
                assert!(force);
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
}
