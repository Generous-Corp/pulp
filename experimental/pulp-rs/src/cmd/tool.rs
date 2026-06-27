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
    self, current_platform_key, load, locate_tool, uninstall_tool, ToolRegistry,
};

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
        Sub::Install { id, all, force: _ } => install(id.as_deref(), *all, out),
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
        \x20 info <tool> [--json]    Show install/package metadata for one tool\n\
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
        let body = serde_json::json!({
            "id": tool.id,
            "display_name": tool.display_name,
            "category": tool.category,
            "description": tool.description,
            "install_method": tool.install_method,
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

fn install(_id: Option<&str>, _all: bool, out: &mut impl Write) -> Result<i32> {
    // Archive download + tar/zip/xz extraction + xattr cleanup is
    // ~500 LOC of new deps (tar + flate2 + zip). Delegate to pulp-cpp
    // when present; print the stub when it's not on PATH so
    // CI/sandboxed callers see a clear error.
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

// Helper used only in tests to resolve fixtures.
#[cfg(test)]
#[allow(dead_code)]
pub(crate) fn registry_at(p: &std::path::Path) -> Result<ToolRegistry> {
    load(p)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::EnvVarGuard;
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
