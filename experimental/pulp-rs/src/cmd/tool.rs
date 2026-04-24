//! `pulp-rs tool` — list / install / uninstall / path / run / doctor.
//!
//! # Phase 6d scope
//!
//! Everything **except** `install` is ported Rust-native:
//!
//! | Subcommand  | Status      | Notes                                        |
//! |-------------|-------------|----------------------------------------------|
//! | `list`      | Ported      | Two-column table; color-aware.               |
//! | `uninstall` | Ported      | `rm -rf` of `$PULP_HOME/tools/<id>`.          |
//! | `path`      | Ported      | Prints absolute path via `locate_tool`.      |
//! | `run`       | Ported      | Exec via `Spawner`.                          |
//! | `doctor`    | Ported      | Health report per tool.                      |
//! | `install`   | **Stubbed** | Prints notice; archive download + extraction |
//! |             |             | needs `ureq` + `tar` + `zip` crates.         |
//!
//! The install stub still dispatches (so `pulp-rs tool install <id>`
//! returns a clean "not yet ported" exit code, not an "unknown
//! subcommand" error). See `tool_registry.cpp` for the reference.

use std::io::Write;

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
    /// `doctor`.
    Doctor,
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
        "doctor" => Ok(Sub::Doctor),
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
        Sub::Install { id, all, force: _ } => install(id.as_deref(), *all, out),
        Sub::Uninstall(id) => uninstall(id, out),
        Sub::Path(id) => path(&reg, id, out),
        Sub::Run { id, args } => run_tool(&reg, id, args, spawner, out),
        Sub::Doctor => doctor(&reg, out),
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
        \x20 install <tool>          Download and install a tool\n\
        \x20 install --all           Install all tools for current platform\n\
        \x20 uninstall <tool>        Remove a pulp-managed tool\n\
        \x20 path <tool>             Print path to a tool's binary\n\
        \x20 run <tool> [args]       Run a tool with arguments\n\
        \x20 doctor                  Check tool health\n",
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
    } else if tool.binary_sources.contains_key(platform) || tool.install_method == "python_pip" {
        format!("{}available{}", color::dim(), color::reset())
    } else {
        format!(
            "{}not available for {platform}{}",
            color::red(),
            color::reset()
        )
    }
}

fn install(_id: Option<&str>, _all: bool, out: &mut impl Write) -> Result<i32> {
    // Phase 7: archive download + tar/zip/xz extraction + xattr
    // cleanup is ~500 LOC of new deps (tar + flate2 + zip). Delegate
    // to pulp-cpp when present; print the pre-Phase-7 stub when it's
    // not on PATH so CI/sandboxed callers see a clear error.
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

fn doctor(reg: &ToolRegistry, out: &mut impl Write) -> Result<i32> {
    let platform = current_platform_key();
    writeln!(
        out,
        "Tool Health {dim}({platform}){reset}:\n",
        dim = color::dim(),
        reset = color::reset()
    )
    .map_err(io)?;
    let mut issues = 0;
    for (id, tool) in &reg.tools {
        let loc = locate_tool(tool);
        if loc.found {
            writeln!(
                out,
                "  {green}✓{reset} {} — {} ({})",
                tool.display_name,
                loc.source,
                loc.path.display(),
                green = color::green(),
                reset = color::reset()
            )
            .map_err(io)?;
        } else {
            let available =
                tool.binary_sources.contains_key(platform) || tool.install_method == "python_pip";
            if available {
                writeln!(
                    out,
                    "  {yel}-{reset} {} — not installed {dim}(pulp tool install {id}){reset}",
                    tool.display_name,
                    yel = color::yellow(),
                    reset = color::reset(),
                    dim = color::dim()
                )
                .map_err(io)?;
            } else {
                writeln!(
                    out,
                    "  {red}✗{reset} {} — not available for {platform}",
                    tool.display_name,
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
                }
            }
        }"#
    }

    #[test]
    fn parse_list_simple() {
        assert_eq!(parse_sub(&["list".into()]).unwrap(), Sub::List);
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
        let mut buf = Vec::new();
        let rc = doctor(&reg, &mut buf).unwrap();
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
    fn install_prints_stub_notice() {
        let mut buf = Vec::new();
        let rc = install(Some("uv"), false, &mut buf).unwrap();
        assert_eq!(rc, 1);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("not ported"));
    }

    #[test]
    fn print_help_mentions_all_subcommands() {
        let mut buf = Vec::new();
        print_help(&mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        for cmd in ["list", "install", "uninstall", "path", "run", "doctor"] {
            assert!(s.contains(cmd), "missing {cmd}");
        }
    }
}
