//! `pulp-rs version [--json] | bump <component> [--plugin] | check
//! [--with-bump-check]` orchestrator.
//!
//! # Scope
//!
//! Phase 5's warmup port delivered the `show` lane only. Phase 6e adds
//! the remaining two subcommands so the Rust port matches the C++
//! surface at `tools/cli/cmd_version.cpp`:
//!
//! | Subcommand | Port status |
//! |------------|-------------|
//! | `show` (default) | Ported — human + `--json` identical to Phase 5. |
//! | `bump <major|minor|patch> [--plugin]` | Ported — CMakeLists.txt rewrite + CHANGELOG heading insert + footer hint. |
//! | `check [--with-bump-check]` | Ported — SDK / AU plist / CHANGELOG / plugin.json / marketplace.json consistency; `--with-bump-check` delegates to `tools/scripts/version_bump_check.py --mode=report`. |
//!
//! # `check` semantics caveat
//!
//! The Rust binary now receives the SDK version through the CMake build
//! bridge, so the reported "SDK version consistent / mismatch" line
//! compares projects against the released CLI version. Direct Cargo
//! prototype builds fall back to the crate version.

use std::io::Write;
use std::path::Path;
use std::sync::OnceLock;

use regex::Regex;
use serde_json::Value;

use crate::error::{CliError, Result};
use crate::parse::cmake;
use crate::proc::{Invocation, Spawner};
use crate::project::{self, ActiveProject};
use crate::version_info;

/// Parsed `pulp-rs version ...` invocation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VersionCmd {
    /// Print the installed CLI + plugin versions. Optional `--json`.
    Show {
        /// Emit machine-readable JSON instead of the human block.
        json: bool,
    },
    /// Rewrite the project's `CMakeLists.txt` version (optionally
    /// targeting the `pulp_add_plugin(... VERSION ...)` cell instead of
    /// `project(... VERSION ...)`), inserting a CHANGELOG heading
    /// stub when the SDK-side form is used.
    Bump {
        /// Which semver slot to bump.
        component: BumpKind,
        /// When `true`, target the `pulp_add_plugin(...)` cell only —
        /// leaves the SDK `project(VERSION ...)` untouched.
        plugin: bool,
    },
    /// Report drift across SDK, CHANGELOG, and plugin.json /
    /// marketplace.json. `--with-bump-check` chains
    /// `version_bump_check.py --mode=report`.
    Check {
        /// Shell out to `tools/scripts/version_bump_check.py
        /// --mode=report` after the Rust-native checks finish.
        with_bump_check: bool,
    },
}

/// Semver component to bump.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BumpKind {
    /// `major` — bumps `X` and zeroes minor + patch.
    Major,
    /// `minor` — bumps `Y` and zeroes patch.
    Minor,
    /// `patch` — bumps `Z`.
    Patch,
}

impl BumpKind {
    fn parse(s: &str) -> Option<Self> {
        match s {
            "major" => Some(Self::Major),
            "minor" => Some(Self::Minor),
            "patch" => Some(Self::Patch),
            _ => None,
        }
    }
}

/// Parse `pulp-rs version <tail>`.
///
/// # Errors
///
/// [`CliError::BadUsage`] when a subcommand is malformed (missing
/// component, wrong keyword, etc.).
pub fn parse(tail: &[String]) -> Result<VersionCmd> {
    if tail.is_empty() {
        return Ok(VersionCmd::Show { json: false });
    }
    // `--json` anywhere in the tail promotes a no-subcommand call to
    // `Show { json: true }`. C++ behaviour: `pulp version --json`.
    let mut positional: Vec<&str> = Vec::new();
    let mut json = false;
    let mut plugin_flag = false;
    let mut with_bump_check = false;
    for a in tail {
        match a.as_str() {
            "--json" => json = true,
            "--plugin" => plugin_flag = true,
            "--with-bump-check" => with_bump_check = true,
            s if s.starts_with("--") => {
                return Err(CliError::BadUsage(format!(
                    "pulp-rs version: unknown flag {s}"
                )));
            }
            s => positional.push(s),
        }
    }

    match positional.first().copied() {
        None => Ok(VersionCmd::Show { json }),
        Some("bump") => {
            let Some(component) = positional.get(1).copied().and_then(BumpKind::parse) else {
                return Err(CliError::BadUsage(
                    "Usage: pulp version bump <major|minor|patch> [--plugin]".to_owned(),
                ));
            };
            Ok(VersionCmd::Bump {
                component,
                plugin: plugin_flag,
            })
        }
        Some("check") => Ok(VersionCmd::Check { with_bump_check }),
        Some(other) => Err(CliError::BadUsage(format!(
            "pulp-rs version: unknown subcommand {other}"
        ))),
    }
}

/// Run the `version` subcommand with a live system spawner. Convenience
/// for `main.rs`.
///
/// # Errors
///
/// Propagates [`CliError`] from [`dispatch`].
pub fn run_system(cmd: &VersionCmd, out: &mut impl Write) -> Result<i32> {
    let cwd = std::env::current_dir().map_err(|e| CliError::io(".", e))?;
    let spawner = crate::proc::SystemSpawner;
    dispatch(&cwd, cmd, &spawner, out)
}

/// Dispatch a parsed [`VersionCmd`].
///
/// # Errors
///
/// Propagates [`CliError`] from the subcommand handlers.
pub fn dispatch<S: Spawner>(
    cwd: &Path,
    cmd: &VersionCmd,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    match *cmd {
        VersionCmd::Show { json } => show(json, out).map(|()| 0),
        VersionCmd::Bump { component, plugin } => bump(cwd, component, plugin, out),
        VersionCmd::Check { with_bump_check } => check(cwd, with_bump_check, spawner, out),
    }
}

// ── show ─────────────────────────────────────────────────────────────

/// Pre-Phase-6e entry point; kept for back-compat with callers that
/// hardcode the flag-only surface.
///
/// # Errors
///
/// Propagates [`CliError`] from the snapshot collector.
pub fn run(json: bool, out: &mut impl Write) -> Result<()> {
    show(json, out)
}

fn show(json: bool, out: &mut impl Write) -> Result<()> {
    let cwd = std::env::current_dir().map_err(|e| CliError::io(".", e))?;
    let snap = version_info::collect(&cwd);

    if json {
        writeln!(out, "{}", version_info::emit_json(&snap)).map_err(io_err)?;
        return Ok(());
    }

    // Phase 8 binary swap (#767 / #686): user-facing label is now
    // `pulp` (was `pulp-rs (prototype)`). The C++ delegate is `pulp-cpp`.
    writeln!(out, "pulp v{}", snap.cli.raw).map_err(io_err)?;
    if !snap.plugin.raw.is_empty() {
        writeln!(out, "Claude plugin: v{}", snap.plugin.raw).map_err(io_err)?;
    }
    Ok(())
}

// ── bump ─────────────────────────────────────────────────────────────

fn bump(cwd: &Path, component: BumpKind, plugin: bool, out: &mut impl Write) -> Result<i32> {
    let Some(proj) = project::resolve(cwd) else {
        return Err(CliError::Other(
            "not in a Pulp project directory".to_owned(),
        ));
    };
    let cmake_path = proj.root.join("CMakeLists.txt");
    let content = std::fs::read_to_string(&cmake_path).map_err(|e| CliError::io(&cmake_path, e))?;

    let current = if plugin {
        read_plugin_version(&content).ok_or_else(|| {
            CliError::Other(r#"no pulp_add_plugin(... VERSION "x.y.z" ...) found"#.to_owned())
        })?
    } else {
        cmake::read(&proj.root)
            .ok_or_else(|| CliError::Other("no project(... VERSION x.y.z ...) found".to_owned()))?
    };

    let parsed = SemVer::parse(&current).ok_or_else(|| {
        CliError::Other(format!("version string is not a clean triple: {current}"))
    })?;
    let bumped = parsed.bumped(component);
    let new_str = bumped.to_string();

    let new_content = if plugin {
        rewrite_plugin_version(&content, &current, &new_str)
    } else {
        rewrite_first_occurrence(&content, &current, &new_str)
    };
    let Some(new_content) = new_content else {
        return Err(CliError::Other(format!(
            "failed to update version in {}",
            cmake_path.display()
        )));
    };
    std::fs::write(&cmake_path, new_content).map_err(|e| CliError::io(&cmake_path, e))?;
    writeln!(out, "✓ Version bumped: {current} -> {new_str}").map_err(io_err)?;

    if !plugin {
        let changelog = proj.root.join("CHANGELOG.md");
        if changelog.exists() {
            let cl =
                std::fs::read_to_string(&changelog).map_err(|e| CliError::io(&changelog, e))?;
            let new_entry = format!("## [{new_str}]\n\n");
            let merged = cl.find("## [").map_or_else(
                || format!("{new_entry}{cl}"),
                |idx| {
                    let (head, tail) = cl.split_at(idx);
                    format!("{head}{new_entry}{tail}")
                },
            );
            std::fs::write(&changelog, merged).map_err(|e| CliError::io(&changelog, e))?;
            writeln!(out, "✓ Added CHANGELOG.md entry for {new_str}").map_err(io_err)?;
        }
        writeln!(
            out,
            "  Note: PULP_SDK_VERSION is derived from CMake project(VERSION)"
        )
        .map_err(io_err)?;
        writeln!(
            out,
            "        via configure_file — rebuild to pick up the change."
        )
        .map_err(io_err)?;
        writeln!(out, "  Tag with: git tag v{new_str}").map_err(io_err)?;
    }
    Ok(0)
}

// ── check ────────────────────────────────────────────────────────────

fn check<S: Spawner>(
    cwd: &Path,
    with_bump_check: bool,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let Some(proj) = project::resolve(cwd) else {
        return Err(CliError::Other(
            "not in a Pulp project directory".to_owned(),
        ));
    };
    let sdk_ver = crate::version_info::collect(&proj.root).cli.raw;
    let cmake_ver = cmake::read(&proj.root).unwrap_or_default();

    let mut all_ok = true;
    all_ok &= check_sdk_consistency(&cmake_ver, &sdk_ver, out)?;
    all_ok &= check_au_plist(&proj, out)?;
    check_changelog_heading(&proj, &cmake_ver, out)?;
    all_ok &= check_plugin_and_marketplace(&proj, out)?;
    if with_bump_check {
        all_ok &= run_bump_check(&proj, spawner, out)?;
    }
    Ok(i32::from(!all_ok))
}

fn check_sdk_consistency(cmake_ver: &str, sdk_ver: &str, out: &mut impl Write) -> Result<bool> {
    if !cmake_ver.is_empty() && !sdk_ver.is_empty() && cmake_ver != sdk_ver {
        writeln!(
            out,
            "✗ SDK version mismatch: CMakeLists.txt={cmake_ver} cli={sdk_ver}"
        )
        .map_err(io_err)?;
        return Ok(false);
    }
    if !cmake_ver.is_empty() {
        writeln!(out, "✓ SDK version consistent: {cmake_ver}").map_err(io_err)?;
    }
    Ok(true)
}

fn check_au_plist(proj: &ActiveProject, out: &mut impl Write) -> Result<bool> {
    let au_plist = proj.root.join("tools/cmake/PulpInfoPlist.au.in");
    if !au_plist.exists() {
        return Ok(true);
    }
    let content = std::fs::read_to_string(&au_plist).map_err(|e| CliError::io(&au_plist, e))?;
    if content.contains("<integer>65536</integer>") {
        writeln!(out, "✗ AU Info.plist has hardcoded version integer (65536)").map_err(io_err)?;
        Ok(false)
    } else {
        writeln!(out, "✓ AU Info.plist uses computed version integer").map_err(io_err)?;
        Ok(true)
    }
}

fn check_changelog_heading(
    proj: &ActiveProject,
    cmake_ver: &str,
    out: &mut impl Write,
) -> Result<()> {
    let changelog = proj.root.join("CHANGELOG.md");
    if !changelog.exists() {
        return Ok(());
    }
    let cl = std::fs::read_to_string(&changelog).map_err(|e| CliError::io(&changelog, e))?;
    let Some(latest) = changelog_latest_version(&cl) else {
        return Ok(());
    };
    if latest == cmake_ver {
        writeln!(out, "✓ CHANGELOG latest version matches ({latest})").map_err(io_err)?;
    } else {
        writeln!(
            out,
            "⚠ CHANGELOG latest ({latest}) differs from CMakeLists.txt ({cmake_ver})"
        )
        .map_err(io_err)?;
    }
    Ok(())
}

fn check_plugin_and_marketplace(proj: &ActiveProject, out: &mut impl Write) -> Result<bool> {
    let plugin_path = proj.root.join(".claude-plugin/plugin.json");
    let market_path = proj.root.join(".claude-plugin/marketplace.json");
    let plugin_ver = read_json_version_field(&plugin_path);
    let market_ver = read_json_version_field(&market_path);
    let entry_ver = read_marketplace_plugin_entry_version(&market_path);

    let mut all_ok = true;

    if plugin_ver.is_empty() {
        if plugin_path.exists() {
            writeln!(
                out,
                r#"⚠ .claude-plugin/plugin.json has no "version" field"#
            )
            .map_err(io_err)?;
        }
    } else if is_semver_triple(&plugin_ver) {
        writeln!(out, "✓ Claude plugin version: {plugin_ver}").map_err(io_err)?;
    } else {
        writeln!(out, "✗ plugin.json version is not semver: {plugin_ver}").map_err(io_err)?;
        all_ok = false;
    }

    if !market_ver.is_empty() && !plugin_ver.is_empty() {
        if market_ver == plugin_ver {
            writeln!(out, "✓ marketplace.json version matches plugin.json").map_err(io_err)?;
        } else {
            writeln!(
                out,
                "✗ marketplace.json version ({market_ver}) differs from plugin.json ({plugin_ver})"
            )
            .map_err(io_err)?;
            all_ok = false;
        }
    }

    if !entry_ver.is_empty() && !plugin_ver.is_empty() {
        if entry_ver == plugin_ver {
            writeln!(
                out,
                "✓ marketplace.json plugins[0].version matches plugin.json"
            )
            .map_err(io_err)?;
        } else {
            writeln!(
                out,
                "✗ marketplace.json plugins[0].version ({entry_ver}) differs from plugin.json ({plugin_ver})"
            )
            .map_err(io_err)?;
            all_ok = false;
        }
    }

    Ok(all_ok)
}

fn run_bump_check<S: Spawner>(
    proj: &ActiveProject,
    spawner: &S,
    out: &mut impl Write,
) -> Result<bool> {
    let vbc = proj.root.join("tools/scripts/version_bump_check.py");
    if !vbc.exists() {
        writeln!(
            out,
            "⚠ version_bump_check.py not present — skipping --with-bump-check"
        )
        .map_err(io_err)?;
        return Ok(true);
    }
    writeln!(out, "\n--- version_bump_check (mode=report) ---").map_err(io_err)?;
    let inv = Invocation::new("python3")
        .arg(vbc.to_string_lossy().into_owned())
        .arg("--base")
        .arg("origin/main")
        .arg("--mode=report");
    let rc = spawner.run(&inv)?;
    if rc != 0 {
        writeln!(out, "✗ version-bump gate reported missing bump(s)").map_err(io_err)?;
        return Ok(false);
    }
    Ok(true)
}

fn dispatch_usage_msg() -> &'static str {
    "Usage: pulp version [bump <major|minor|patch>] [check [--with-bump-check]]"
}

/// Public so `main.rs` can surface a usage hint after a parse error.
#[must_use]
pub fn usage_hint() -> &'static str {
    dispatch_usage_msg()
}

// ── helpers ──────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy)]
struct SemVer {
    major: u32,
    minor: u32,
    patch: u32,
}

impl SemVer {
    fn parse(s: &str) -> Option<Self> {
        let mut parts = s.split('.');
        let major = parts.next()?.parse().ok()?;
        let minor = parts.next()?.parse().ok()?;
        let patch = parts.next()?.parse().ok()?;
        if parts.next().is_some() {
            return None;
        }
        Some(Self {
            major,
            minor,
            patch,
        })
    }

    fn bumped(self, kind: BumpKind) -> Self {
        match kind {
            BumpKind::Major => Self {
                major: self.major + 1,
                minor: 0,
                patch: 0,
            },
            BumpKind::Minor => Self {
                major: self.major,
                minor: self.minor + 1,
                patch: 0,
            },
            BumpKind::Patch => Self {
                major: self.major,
                minor: self.minor,
                patch: self.patch + 1,
            },
        }
    }
}

impl std::fmt::Display for SemVer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}.{}.{}", self.major, self.minor, self.patch)
    }
}

fn plugin_re() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| {
        Regex::new(r#"(?s)pulp_add_plugin\s*\([^)]*VERSION\s+"(\d+\.\d+\.\d+)""#)
            .expect("valid regex")
    })
}

fn read_plugin_version(content: &str) -> Option<String> {
    plugin_re()
        .captures(content)
        .and_then(|c| c.get(1))
        .map(|m| m.as_str().to_owned())
}

fn rewrite_first_occurrence(content: &str, old: &str, new: &str) -> Option<String> {
    let idx = content.find(old)?;
    let (head, tail) = content.split_at(idx);
    let after = &tail[old.len()..];
    Some(format!("{head}{new}{after}"))
}

fn rewrite_plugin_version(content: &str, old: &str, new: &str) -> Option<String> {
    // The C++ regex replaces `pulp_add_plugin\s*\([^)]*VERSION\s+"<old>"`
    // with `<prefix>"<new>"`. Using `captures_iter().next()` with the
    // same regex gives us the start+end of the literal old-version
    // capture so we can splice without disturbing surrounding whitespace.
    let caps = plugin_re().captures(content)?;
    let m = caps.get(1)?;
    if m.as_str() != old {
        return None;
    }
    let mut out = String::with_capacity(content.len() + new.len() - old.len());
    out.push_str(&content[..m.start()]);
    out.push_str(new);
    out.push_str(&content[m.end()..]);
    Some(out)
}

fn changelog_latest_version(body: &str) -> Option<String> {
    static RE: OnceLock<Regex> = OnceLock::new();
    let re = RE.get_or_init(|| Regex::new(r"##\s*\[(\d+\.\d+\.\d+)\]").expect("valid regex"));
    re.captures(body)
        .and_then(|c| c.get(1))
        .map(|m| m.as_str().to_owned())
}

fn is_semver_triple(s: &str) -> bool {
    SemVer::parse(s).is_some()
}

fn read_json_version_field(path: &Path) -> String {
    json_at(path, |root| {
        root.get("version")
            .and_then(Value::as_str)
            .map(str::to_owned)
    })
    .unwrap_or_default()
}

fn read_marketplace_plugin_entry_version(path: &Path) -> String {
    json_at(path, |root| {
        root.get("plugins")
            .and_then(Value::as_array)
            .and_then(|arr| arr.first())
            .and_then(|v| v.get("version"))
            .and_then(Value::as_str)
            .map(str::to_owned)
    })
    .unwrap_or_default()
}

fn json_at<F>(path: &Path, extract: F) -> Option<String>
where
    F: FnOnce(&Value) -> Option<String>,
{
    if !path.exists() {
        return None;
    }
    let body = std::fs::read_to_string(path).ok()?;
    let root: Value = serde_json::from_str(&body).ok()?;
    extract(&root)
}

#[inline]
fn io_err(e: std::io::Error) -> CliError {
    CliError::io("<stdout>", e)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proc::testing::RecordingSpawner;
    use crate::test_support::ENV_LOCK;

    const PINNED_VERSION: &str = "9.9.9";

    fn write_cmake(root: &Path, version: &str) {
        let body = format!(
            "cmake_minimum_required(VERSION 3.25)\nproject(Pulp VERSION {version} LANGUAGES CXX)\n"
        );
        std::fs::write(root.join("CMakeLists.txt"), body).unwrap();
        std::fs::create_dir_all(root.join("core")).unwrap();
    }

    /// Pin the reported CLI version to a known value for a single test.
    /// Returns an RAII guard that restores the previous env on drop so
    /// parallel tests can't see a leaked override.
    struct PinCliVersion {
        prev: Option<std::ffi::OsString>,
        _env: std::sync::MutexGuard<'static, ()>,
    }

    impl PinCliVersion {
        fn new(version: &str) -> Self {
            let env = ENV_LOCK
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            let prev = std::env::var_os("PULP_RS_CLI_VERSION");
            std::env::set_var("PULP_RS_CLI_VERSION", version);
            Self { prev, _env: env }
        }
    }

    impl Drop for PinCliVersion {
        fn drop(&mut self) {
            match self.prev.take() {
                Some(v) => std::env::set_var("PULP_RS_CLI_VERSION", v),
                None => std::env::remove_var("PULP_RS_CLI_VERSION"),
            }
        }
    }

    #[test]
    fn parse_show_default() {
        let c = parse(&[]).unwrap();
        assert_eq!(c, VersionCmd::Show { json: false });
    }

    #[test]
    fn parse_show_json_flag() {
        let c = parse(&["--json".to_owned()]).unwrap();
        assert_eq!(c, VersionCmd::Show { json: true });
    }

    #[test]
    fn parse_bump_patch() {
        let c = parse(&["bump".to_owned(), "patch".to_owned()]).unwrap();
        assert_eq!(
            c,
            VersionCmd::Bump {
                component: BumpKind::Patch,
                plugin: false
            }
        );
    }

    #[test]
    fn parse_bump_plugin_flag() {
        let c = parse(&["bump".to_owned(), "minor".to_owned(), "--plugin".to_owned()]).unwrap();
        assert!(matches!(
            c,
            VersionCmd::Bump {
                component: BumpKind::Minor,
                plugin: true
            }
        ));
    }

    #[test]
    fn parse_bump_rejects_bad_component() {
        let err = parse(&["bump".to_owned(), "wibble".to_owned()]).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn parse_check_with_bump_flag() {
        let c = parse(&["check".to_owned(), "--with-bump-check".to_owned()]).unwrap();
        assert_eq!(
            c,
            VersionCmd::Check {
                with_bump_check: true
            }
        );
    }

    #[test]
    fn parse_rejects_unknown_flag() {
        let err = parse(&["--bogus".to_owned()]).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn semver_parse_and_bump() {
        let v = SemVer::parse("1.2.3").unwrap();
        assert_eq!(v.bumped(BumpKind::Major).to_string(), "2.0.0");
        assert_eq!(v.bumped(BumpKind::Minor).to_string(), "1.3.0");
        assert_eq!(v.bumped(BumpKind::Patch).to_string(), "1.2.4");
        assert!(SemVer::parse("1.2").is_none());
        assert!(SemVer::parse("1.2.3.4").is_none());
    }

    #[test]
    fn bump_rewrites_cmake_project_version() {
        let td = tempfile::tempdir().unwrap();
        write_cmake(td.path(), "0.40.0");
        let mut out = Vec::new();
        let rc = bump(td.path(), BumpKind::Minor, false, &mut out).unwrap();
        assert_eq!(rc, 0);
        let updated = std::fs::read_to_string(td.path().join("CMakeLists.txt")).unwrap();
        assert!(updated.contains("VERSION 0.41.0"));
        assert!(!updated.contains("VERSION 0.40.0"));
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("0.40.0 -> 0.41.0"));
    }

    #[test]
    fn bump_plugin_rewrites_only_plugin_entry() {
        let td = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(td.path().join("core")).unwrap();
        std::fs::write(
            td.path().join("CMakeLists.txt"),
            "project(Pulp VERSION 0.40.0)\npulp_add_plugin(MyPlug VERSION \"0.40.0\")\n",
        )
        .unwrap();
        let mut out = Vec::new();
        bump(td.path(), BumpKind::Patch, true, &mut out).unwrap();
        let updated = std::fs::read_to_string(td.path().join("CMakeLists.txt")).unwrap();
        // Project version unchanged.
        assert!(updated.contains("project(Pulp VERSION 0.40.0)"));
        // Plugin version bumped.
        assert!(updated.contains("pulp_add_plugin(MyPlug VERSION \"0.40.1\")"));
    }

    #[test]
    fn bump_inserts_changelog_heading() {
        let td = tempfile::tempdir().unwrap();
        write_cmake(td.path(), "0.40.0");
        std::fs::write(
            td.path().join("CHANGELOG.md"),
            "# Changelog\n\n## [0.40.0]\n- previous\n",
        )
        .unwrap();
        let mut out = Vec::new();
        bump(td.path(), BumpKind::Minor, false, &mut out).unwrap();
        let updated = std::fs::read_to_string(td.path().join("CHANGELOG.md")).unwrap();
        assert!(updated.contains("## [0.41.0]"));
        // New entry sits above the old one.
        assert!(updated.find("## [0.41.0]").unwrap() < updated.find("## [0.40.0]").unwrap());
    }

    #[test]
    fn check_ok_when_versions_match() {
        let _pin = PinCliVersion::new(PINNED_VERSION);
        let td = tempfile::tempdir().unwrap();
        write_cmake(td.path(), PINNED_VERSION);
        let spawner = RecordingSpawner::ok();
        let mut out = Vec::new();
        let rc = check(td.path(), false, &spawner, &mut out).unwrap();
        assert_eq!(rc, 0);
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("SDK version consistent"));
    }

    #[test]
    fn check_flags_mismatch_between_plugin_and_marketplace() {
        let _pin = PinCliVersion::new(PINNED_VERSION);
        let td = tempfile::tempdir().unwrap();
        write_cmake(td.path(), PINNED_VERSION);
        std::fs::create_dir_all(td.path().join(".claude-plugin")).unwrap();
        std::fs::write(
            td.path().join(".claude-plugin/plugin.json"),
            r#"{"version":"0.12.0"}"#,
        )
        .unwrap();
        std::fs::write(
            td.path().join(".claude-plugin/marketplace.json"),
            r#"{"version":"1.0.0","plugins":[{"version":"0.11.0"}]}"#,
        )
        .unwrap();
        let spawner = RecordingSpawner::ok();
        let mut out = Vec::new();
        let rc = check(td.path(), false, &spawner, &mut out).unwrap();
        assert_eq!(rc, 1);
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("marketplace.json plugins[0].version"));
    }

    #[test]
    fn check_with_bump_check_shells_out() {
        let _pin = PinCliVersion::new(PINNED_VERSION);
        let td = tempfile::tempdir().unwrap();
        write_cmake(td.path(), PINNED_VERSION);
        std::fs::create_dir_all(td.path().join("tools/scripts")).unwrap();
        std::fs::write(
            td.path().join("tools/scripts/version_bump_check.py"),
            "# stub\n",
        )
        .unwrap();
        let spawner = RecordingSpawner::with_codes(vec![0]);
        let mut out = Vec::new();
        let rc = check(td.path(), true, &spawner, &mut out).unwrap();
        assert_eq!(rc, 0);
        let calls = spawner.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].program, "python3");
        assert!(calls[0].args.iter().any(|a| a == "--mode=report"));
    }

    #[test]
    fn check_with_bump_check_reports_non_zero_exit() {
        let _pin = PinCliVersion::new(PINNED_VERSION);
        let td = tempfile::tempdir().unwrap();
        write_cmake(td.path(), PINNED_VERSION);
        std::fs::create_dir_all(td.path().join("tools/scripts")).unwrap();
        std::fs::write(
            td.path().join("tools/scripts/version_bump_check.py"),
            "# stub\n",
        )
        .unwrap();
        let spawner = RecordingSpawner::with_codes(vec![1]);
        let mut out = Vec::new();
        let rc = check(td.path(), true, &spawner, &mut out).unwrap();
        assert_eq!(rc, 1);
    }

    // ── #45 coverage uplift slice 5 — version.rs helpers ──────────
    //
    // The pure helpers below the dispatch surface (read_plugin_version,
    // rewrite_plugin_version, rewrite_first_occurrence,
    // changelog_latest_version, is_semver_triple, read_json_version_field,
    // read_marketplace_plugin_entry_version) drove most of version.rs's
    // 123 missing lines but had no direct unit coverage — only the
    // bump end-to-end tests indirectly hit some of them. Add small
    // hermetic tests so the regex + JSON-extract paths can't regress
    // silently.

    #[test]
    fn read_plugin_version_extracts_pulp_add_plugin() {
        let src = r#"pulp_add_plugin(MyPlugin
            VERSION "1.2.3"
            COMPANY "Acme")
"#;
        assert_eq!(read_plugin_version(src), Some("1.2.3".to_owned()));
    }

    #[test]
    fn read_plugin_version_returns_none_when_no_call() {
        assert_eq!(read_plugin_version("project(Foo VERSION 1.2.3)\n"), None);
        assert_eq!(read_plugin_version(""), None);
    }

    #[test]
    fn rewrite_first_occurrence_replaces_only_first() {
        let src = "abc xyz abc";
        let out = rewrite_first_occurrence(src, "abc", "ZZZ").unwrap();
        assert_eq!(out, "ZZZ xyz abc");
    }

    #[test]
    fn rewrite_first_occurrence_returns_none_when_not_found() {
        assert!(rewrite_first_occurrence("hello world", "missing", "x").is_none());
    }

    #[test]
    fn rewrite_plugin_version_swaps_only_inside_call() {
        let src = r#"# header
pulp_add_plugin(MyPlugin
    VERSION "1.2.3"
    COMPANY "Acme")
project(Other VERSION 1.2.3)
"#;
        let out = rewrite_plugin_version(src, "1.2.3", "1.2.4").unwrap();
        // The `pulp_add_plugin` literal swaps but the bare
        // `project(...VERSION 1.2.3)` stays untouched — that's the
        // whole point of the regex.
        assert!(out.contains(r#"VERSION "1.2.4""#));
        assert!(out.contains("project(Other VERSION 1.2.3)"));
    }

    #[test]
    fn rewrite_plugin_version_returns_none_on_old_mismatch() {
        let src = r#"pulp_add_plugin(P VERSION "1.2.3")"#;
        assert!(rewrite_plugin_version(src, "9.9.9", "1.2.4").is_none());
    }

    #[test]
    fn changelog_latest_version_picks_first_h2_semver() {
        let body = "# Changelog\n\n## [0.47.0] — today\n\n## [0.46.0] — yesterday\n";
        assert_eq!(changelog_latest_version(body), Some("0.47.0".to_owned()));
    }

    #[test]
    fn changelog_latest_version_returns_none_on_no_h2() {
        assert_eq!(changelog_latest_version("just text"), None);
    }

    #[test]
    fn is_semver_triple_accepts_and_rejects() {
        assert!(is_semver_triple("0.1.2"));
        assert!(is_semver_triple("99.0.0"));
        assert!(!is_semver_triple("v1.2.3"));     // SemVer::parse rejects v-prefix
        assert!(!is_semver_triple("1.2"));
        assert!(!is_semver_triple("not-semver"));
    }

    #[test]
    fn read_json_version_field_returns_value() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("plugin.json");
        std::fs::write(&path, r#"{"name":"x","version":"1.2.3"}"#).unwrap();
        assert_eq!(read_json_version_field(&path), "1.2.3");
    }

    #[test]
    fn read_json_version_field_returns_empty_on_missing_or_malformed() {
        let td = tempfile::tempdir().unwrap();
        let missing = td.path().join("nope.json");
        assert_eq!(read_json_version_field(&missing), "");
        let malformed = td.path().join("bad.json");
        std::fs::write(&malformed, "not-json").unwrap();
        assert_eq!(read_json_version_field(&malformed), "");
        let no_version = td.path().join("nover.json");
        std::fs::write(&no_version, r#"{"name":"x"}"#).unwrap();
        assert_eq!(read_json_version_field(&no_version), "");
    }

    #[test]
    fn read_marketplace_plugin_entry_version_picks_first_plugins_entry() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("marketplace.json");
        std::fs::write(
            &path,
            r#"{"plugins":[{"id":"a","version":"3.4.5"},{"id":"b","version":"9.9.9"}]}"#,
        )
        .unwrap();
        assert_eq!(read_marketplace_plugin_entry_version(&path), "3.4.5");
    }

    #[test]
    fn read_marketplace_plugin_entry_version_returns_empty_on_no_array() {
        let td = tempfile::tempdir().unwrap();
        let path = td.path().join("nope.json");
        std::fs::write(&path, r#"{"plugins":"not-an-array"}"#).unwrap();
        assert_eq!(read_marketplace_plugin_entry_version(&path), "");
    }
}
