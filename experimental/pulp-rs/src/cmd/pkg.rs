//! `pulp-rs add / remove / list / search / update / suggest / target`
//! — the package-manager subcommand surface.
//!
//! # Why one module for seven commands
//!
//! They share a lot of plumbing (project-root discovery, registry
//! load, lock-file I/O, `CMake` regeneration). Splitting each into its
//! own file would require either re-duplicating those helpers or
//! exposing them behind a public crate surface no external caller
//! needs. The C++ reference also ships all seven commands in
//! `package_commands.cpp` — keeping the same shape makes sync-audit
//! diffs easier to follow.
//!
//! # Parity bar
//!
//! - **Human output** — column layout, bracket headers, "No packages
//!   installed." strings match byte-for-byte with `NO_COLOR` set.
//! - **JSON output** — on `list` and `search --format json` / `suggest
//!   --format json`, shape matches the C++ writer. Pretty-printed for
//!   readability; tests parse the JSON back and assert on fields.
//! - **Exit codes** — 0 success, 1 runtime failure, 2 bad usage.

#![allow(clippy::too_many_lines)] // parity with the C++ surface keeps these functions linear

use std::io::Write;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use crate::color;
use crate::error::{CliError, Result};
use crate::pkg::{
    cmake,
    license::{self, LicenseVerdict},
    metadata,
    registry::{self, LockedPackage, PackageDescriptor, Registry},
    targets::{self, PlatformTarget},
};

// ── Shared helpers ─────────────────────────────────────────────────

fn io(e: std::io::Error) -> CliError {
    CliError::io("<stdout>", e)
}

fn resolve_root() -> Result<PathBuf> {
    let cwd = std::env::current_dir().map_err(|e| CliError::io("<cwd>", e))?;
    registry::find_project_root(&cwd).ok_or_else(|| {
        CliError::Other("Not in a Pulp project (no CMakeLists.txt or pulp.toml found)".to_owned())
    })
}

fn load_or_err(path: &Path) -> Result<Registry> {
    registry::load(path)
}

fn ensure_cmake_include(root: &Path) -> Result<()> {
    let cml = root.join("CMakeLists.txt");
    let content = std::fs::read_to_string(&cml).unwrap_or_default();
    if content.contains("cmake/pulp-packages.cmake") {
        return Ok(());
    }
    let mut body = content;
    if !body.ends_with('\n') && !body.is_empty() {
        body.push('\n');
    }
    body.push_str("\n# Pulp package manager\n");
    body.push_str("include(cmake/pulp-packages.cmake OPTIONAL)\n");
    std::fs::write(&cml, body).map_err(|e| CliError::io(cml, e))
}

fn write_atomic(path: &Path, body: &[u8]) -> Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent).map_err(|e| CliError::io(parent.to_path_buf(), e))?;
        }
    }
    let tmp: PathBuf = {
        let mut p = path.to_path_buf();
        p.as_mut_os_string().push(".tmp");
        p
    };
    std::fs::write(&tmp, body).map_err(|e| CliError::io(tmp.clone(), e))?;
    match std::fs::rename(&tmp, path) {
        Ok(()) => Ok(()),
        Err(e) => {
            let _ = std::fs::remove_file(&tmp);
            Err(CliError::io(path.to_path_buf(), e))
        }
    }
}

fn dots(s: &str, width: usize) -> String {
    let n = width.saturating_sub(s.chars().count()).max(1);
    ".".repeat(n)
}

// ── list ───────────────────────────────────────────────────────────

/// Run `pulp-rs list [--json]`.
///
/// # Errors
///
/// Returns [`CliError::Io`] / [`CliError::Json`] on registry or lock
/// read failures. Returns [`CliError::Other`] when invoked outside a
/// Pulp project.
pub fn run_list(json_output: bool, out: &mut impl Write) -> Result<()> {
    let root = resolve_root()?;
    let lock_path = root.join("packages.lock.json");
    if !lock_path.is_file() {
        writeln!(out, "No packages installed.").map_err(io)?;
        writeln!(
            out,
            "{}Use 'pulp add <package>' to add a package, or 'pulp search <query>' to find packages.{}",
            color::dim(),
            color::reset()
        )
        .map_err(io)?;
        return Ok(());
    }
    let lock = registry::load_lock(&lock_path);
    if lock.packages.is_empty() {
        writeln!(out, "No packages installed.").map_err(io)?;
        return Ok(());
    }
    // Enrich where possible.
    let reg = registry::find_registry_path(&root)
        .and_then(|p| registry::load(&p).ok())
        .unwrap_or_default();

    if json_output {
        let entries: Vec<Value> = lock
            .packages
            .iter()
            .map(|(id, lp)| {
                json!({
                    "id": id,
                    "version": lp.version,
                })
            })
            .collect();
        let rendered = serde_json::to_string_pretty(&entries).unwrap_or_else(|_| "[]".to_owned());
        writeln!(out, "{rendered}").map_err(io)?;
        return Ok(());
    }

    writeln!(out, "Installed packages ({}):\n", lock.packages.len()).map_err(io)?;
    for (id, lp) in &lock.packages {
        let (name, license_str, category) = reg.packages.get(id).map_or_else(
            || (id.clone(), "?".to_owned(), "?".to_owned()),
            |p| (p.name.clone(), p.license.clone(), p.category.clone()),
        );
        writeln!(
            out,
            "  {name} {dots} {dim}v{ver}{reset} {dim}[{lic}]{reset} {dim}{cat}{reset}",
            dots = dots(&name, 35),
            dim = color::dim(),
            reset = color::reset(),
            ver = lp.version,
            lic = license_str,
            cat = category,
        )
        .map_err(io)?;
    }
    Ok(())
}

// ── search ─────────────────────────────────────────────────────────

/// Parsed flags for `pulp-rs search`.
#[derive(Debug, Default, Clone)]
pub struct SearchArgs {
    /// Space-joined query terms.
    pub query: String,
    /// Emit JSON shape instead of ranked table.
    pub json_output: bool,
    /// Fetch the remote registry before searching. Stubbed in Rust
    /// port — the C++ path shells out to `curl`; here we match the
    /// empty-registry code path (print "No packages found") so the
    /// parity bar stays predictable offline.
    pub refresh: bool,
}

/// Parse a flag slice into a [`SearchArgs`].
///
/// # Errors
///
/// Returns [`CliError::BadUsage`] for unknown `--format` values.
pub fn parse_search_args(args: &[String]) -> Result<SearchArgs> {
    let mut out = SearchArgs::default();
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        if a == "--refresh" {
            out.refresh = true;
            i += 1;
            continue;
        }
        if a == "--format" && i + 1 < args.len() {
            let v = &args[i + 1];
            if v == "json" {
                out.json_output = true;
            } else {
                return Err(CliError::BadUsage(format!(
                    "pulp search: unknown --format '{v}' (expected json)"
                )));
            }
            i += 2;
            continue;
        }
        if !out.query.is_empty() {
            out.query.push(' ');
        }
        out.query.push_str(a);
        i += 1;
    }
    Ok(out)
}

/// Run `pulp-rs search <query> [--refresh] [--format json]`.
///
/// # Errors
///
/// Surfaces registry read errors.
pub fn run_search(args: &SearchArgs, out: &mut impl Write) -> Result<()> {
    if args.query.is_empty() {
        print_search_help(out);
        return Ok(());
    }
    let cwd = std::env::current_dir().map_err(|e| CliError::io("<cwd>", e))?;
    let root = registry::find_project_root(&cwd);
    let reg_path = root.as_deref().and_then(registry::find_registry_path);
    let reg = reg_path.map(|p| registry::load(&p)).transpose()?;
    let reg = reg.unwrap_or_default();

    if reg.packages.is_empty() {
        if args.json_output {
            writeln!(out, "[]").map_err(io)?;
        } else {
            writeln!(out, "No packages found matching: {}", args.query).map_err(io)?;
        }
        return Ok(());
    }

    let results = registry::search(&reg, &args.query);
    if results.is_empty() {
        if args.json_output {
            writeln!(out, "[]").map_err(io)?;
        } else {
            writeln!(out, "No packages found matching: {}", args.query).map_err(io)?;
        }
        return Ok(());
    }

    if args.json_output {
        let entries: Vec<Value> = results
            .iter()
            .map(|p| {
                json!({
                    "id": p.id,
                    "name": p.name,
                    "version": p.version,
                    "license": p.license,
                    "description": p.description,
                })
            })
            .collect();
        let rendered = serde_json::to_string_pretty(&entries).unwrap_or_else(|_| "[]".to_owned());
        writeln!(out, "{rendered}").map_err(io)?;
        return Ok(());
    }

    writeln!(
        out,
        "Found {} package(s) matching \"{}\":\n",
        results.len(),
        args.query
    )
    .map_err(io)?;
    for p in &results {
        let verdict = license::check(&p.license);
        let note = match verdict {
            LicenseVerdict::Rejected => format!(
                " {red}(license incompatible){reset}",
                red = color::red(),
                reset = color::reset()
            ),
            LicenseVerdict::ReviewRequired => format!(
                " {yel}(license review required){reset}",
                yel = color::yellow(),
                reset = color::reset()
            ),
            LicenseVerdict::Allowed => String::new(),
        };
        writeln!(
            out,
            "  {green}{id}{reset} {dim}v{ver}{reset} {dim}[{lic}]{reset}{note}",
            green = color::green(),
            reset = color::reset(),
            dim = color::dim(),
            id = p.id,
            ver = p.version,
            lic = p.license,
            note = note,
        )
        .map_err(io)?;
        writeln!(out, "    {}\n", p.description).map_err(io)?;
    }
    Ok(())
}

fn print_search_help(out: &mut impl Write) {
    let _ = writeln!(
        out,
        "Usage: pulp search <query> [options]\n\n\
Search the package registry by name, description, tags, or category.\n\n\
Options:\n  \
--refresh      Force refresh the remote registry cache\n  \
--format json  Output as JSON"
    );
}

// ── add / remove ───────────────────────────────────────────────────

/// Parsed `pulp-rs add` args.
#[derive(Debug, Default, Clone)]
pub struct AddArgs {
    /// Package id (required when not printing help).
    pub package_id: Option<String>,
    /// Explicit SPDX id accepted via `--accept-license`.
    pub accepted_license: Option<String>,
    /// `--license-override commercial` was passed.
    pub license_override: bool,
    /// `--platform-guard` was passed.
    pub platform_guard: bool,
    /// `--no-cmake` was passed.
    pub no_cmake: bool,
}

/// Parse the `pulp-rs add` tail.
#[must_use]
pub fn parse_add_args(args: &[String]) -> AddArgs {
    let mut out = AddArgs::default();
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        if a == "--accept-license" && i + 1 < args.len() {
            out.accepted_license = Some(args[i + 1].clone());
            out.license_override = true;
            i += 2;
            continue;
        }
        if a == "--license-override" && i + 1 < args.len() && args[i + 1] == "commercial" {
            out.license_override = true;
            i += 2;
            continue;
        }
        if a == "--platform-guard" {
            out.platform_guard = true;
            i += 1;
            continue;
        }
        if a == "--no-cmake" {
            out.no_cmake = true;
            i += 1;
            continue;
        }
        if out.package_id.is_none() && !a.starts_with('-') {
            out.package_id = Some(a.to_owned());
        }
        i += 1;
    }
    out
}

/// Run `pulp-rs add <package>`.
///
/// # Errors
///
/// Registry / lock I/O; rejected licenses without override;
/// unsupported platforms without `--platform-guard`.
pub fn run_add(args: &AddArgs, out: &mut impl Write) -> Result<i32> {
    let Some(package_id) = args.package_id.as_deref() else {
        print_add_help(out);
        return Ok(0);
    };
    let root = resolve_root()?;
    let reg_path = registry::find_registry_path(&root).ok_or_else(|| {
        CliError::Other("Package registry not found at tools/packages/registry.json".to_owned())
    })?;
    let reg = load_or_err(&reg_path)?;
    let Some(pkg) = reg.packages.get(package_id).cloned() else {
        writeln!(
            out,
            "{red}✗{reset} Package '{package_id}' not found in registry",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        let results = registry::search(&reg, package_id);
        if !results.is_empty() {
            writeln!(out, "\nDid you mean:").map_err(io)?;
            for p in results.iter().take(3) {
                writeln!(out, "  {} — {}", p.id, p.description).map_err(io)?;
            }
        }
        return Ok(1);
    };

    // License policy
    let verdict = license::check(&pkg.license);
    let tier = license::tier(&pkg.license);
    if verdict == LicenseVerdict::Rejected && !args.license_override {
        if tier == "restricted" {
            writeln!(
                out,
                "{red}✗{reset} {} is {} licensed (copyleft).",
                pkg.name,
                pkg.license,
                red = color::red(),
                reset = color::reset()
            )
            .map_err(io)?;
            writeln!(out, "\n  {}\n", license::explanation(&pkg.license)).map_err(io)?;
            writeln!(out, "  To proceed:").map_err(io)?;
            writeln!(
                out,
                "    pulp add {package_id} --accept-license {}",
                pkg.license
            )
            .map_err(io)?;
            return Ok(1);
        }
        writeln!(
            out,
            "{red}✗{reset} {} is {} licensed — cannot be used.",
            pkg.name,
            pkg.license,
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }
    if verdict == LicenseVerdict::Rejected && args.license_override {
        if let Some(accepted) = args.accepted_license.as_deref() {
            if accepted != pkg.license {
                writeln!(
                    out,
                    "{red}✗{reset} --accept-license {accepted} does not match package license {}",
                    pkg.license,
                    red = color::red(),
                    reset = color::reset()
                )
                .map_err(io)?;
                return Ok(1);
            }
        }
        writeln!(
            out,
            "{yel}⚠{reset} Installing {} package — your distributed binary must comply with {}",
            pkg.license,
            pkg.license,
            yel = color::yellow(),
            reset = color::reset()
        )
        .map_err(io)?;
    }
    if verdict == LicenseVerdict::ReviewRequired {
        writeln!(
            out,
            "{yel}⚠{reset} {} license ({}) requires manual review",
            pkg.name,
            pkg.license,
            yel = color::yellow(),
            reset = color::reset()
        )
        .map_err(io)?;
    }

    let lock_path = root.join("packages.lock.json");
    let mut lock = registry::load_lock(&lock_path);
    if let Some(lp) = lock.packages.get(package_id) {
        if lp.version == pkg.version {
            writeln!(out, "{} v{} is already installed.", pkg.name, pkg.version).map_err(io)?;
            return Ok(0);
        }
        writeln!(
            out,
            "{yel}⚠{reset} {} is installed at v{}, registry has v{}. Use 'pulp update' to upgrade.",
            pkg.name,
            lp.version,
            pkg.version,
            yel = color::yellow(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(0);
    }

    // Platform guard
    let project_targets = targets::read(&root);
    let unsup = registry::unsupported_targets(&pkg, &project_targets);
    if !unsup.is_empty() {
        writeln!(
            out,
            "{yel}⚠{reset} {} does not support all your project targets:",
            pkg.name,
            yel = color::yellow(),
            reset = color::reset()
        )
        .map_err(io)?;
        for t in &unsup {
            writeln!(
                out,
                "  {red}✗{reset} {}",
                t.display(),
                red = color::red(),
                reset = color::reset()
            )
            .map_err(io)?;
        }
        if !args.platform_guard {
            writeln!(
                out,
                "\nOptions:\n  1. Add with platform guard (compile only on supported platforms)\n  2. Cancel\nUse --platform-guard to skip this prompt."
            )
            .map_err(io)?;
            return Ok(1);
        }
    }

    // Overlap notice
    if !pkg.overlaps_with_builtin.is_empty() {
        writeln!(
            out,
            "{yel}⚠{reset} {} overlaps with Pulp built-ins:",
            pkg.name,
            yel = color::yellow(),
            reset = color::reset()
        )
        .map_err(io)?;
        for (header, desc) in &pkg.overlaps_with_builtin {
            writeln!(
                out,
                "  {dim}{header}{reset} — {desc}",
                dim = color::dim(),
                reset = color::reset()
            )
            .map_err(io)?;
        }
        if !pkg.unique_value.is_empty() {
            writeln!(
                out,
                "  {green}Unique value:{reset} {}",
                pkg.unique_value,
                green = color::green(),
                reset = color::reset()
            )
            .map_err(io)?;
        }
    }

    if !args.no_cmake {
        lock.packages.insert(
            package_id.to_owned(),
            LockedPackage {
                version: pkg.version.clone(),
                resolved: pkg.fetch.git_repository.clone(),
                integrity: String::new(),
                commit: pkg.fetch.git_tag.clone(),
            },
        );
        let cmake_path = root.join("cmake").join("pulp-packages.cmake");
        let cmake_content = cmake::generate_packages_cmake(&lock, &reg);
        write_atomic(&cmake_path, cmake_content.as_bytes())?;
        ensure_cmake_include(&root)?;
    }

    lock.packages.insert(
        package_id.to_owned(),
        LockedPackage {
            version: pkg.version.clone(),
            resolved: pkg.fetch.git_repository.clone(),
            integrity: String::new(),
            commit: pkg.fetch.git_tag.clone(),
        },
    );
    registry::save_lock(&lock_path, &lock)?;

    metadata::update_dependencies_md(&root, &pkg, true)?;
    metadata::update_notice_md(&root, &pkg, true)?;

    writeln!(
        out,
        "{green}✓{reset} Added {} v{}",
        pkg.name,
        pkg.version,
        green = color::green(),
        reset = color::reset()
    )
    .map_err(io)?;
    if !args.no_cmake {
        if let Some(target) = pkg.cmake.targets.first() {
            writeln!(out, "\n  Add to your CMakeLists.txt target:").map_err(io)?;
            writeln!(
                out,
                "    target_link_libraries(YourTarget PRIVATE {target})"
            )
            .map_err(io)?;
        }
    }
    Ok(0)
}

fn print_add_help(out: &mut impl Write) {
    let _ = writeln!(
        out,
        "Usage: pulp add <package> [options]\n\nOptions:\n  --accept-license <SPDX>         Accept a copyleft license (e.g., GPL-3.0, AGPL-3.0)\n  --license-override commercial   Accept with a commercial license\n  --platform-guard                Add with platform guard (skip prompt)\n  --no-cmake                      Skip CMake wiring"
    );
}

/// Run `pulp-rs remove <package>`.
///
/// # Errors
///
/// Surfaces I/O failures; returns exit code 1 when the package isn't
/// in the lock file.
pub fn run_remove(args: &[String], out: &mut impl Write) -> Result<i32> {
    let Some(package_id) = args.first() else {
        writeln!(out, "Usage: pulp remove <package>").map_err(io)?;
        return Ok(0);
    };
    let root = resolve_root()?;
    let lock_path = root.join("packages.lock.json");
    let mut lock = registry::load_lock(&lock_path);
    if !lock.packages.contains_key(package_id) {
        writeln!(
            out,
            "{red}✗{reset} Package '{package_id}' is not installed",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }

    // Pull descriptor for metadata cleanup (best-effort).
    let reg = registry::find_registry_path(&root)
        .and_then(|p| registry::load(&p).ok())
        .unwrap_or_default();
    let pkg = reg
        .packages
        .get(package_id)
        .cloned()
        .unwrap_or_else(|| PackageDescriptor {
            id: package_id.clone(),
            ..PackageDescriptor::default()
        });

    lock.packages.remove(package_id);
    registry::save_lock(&lock_path, &lock)?;

    let cmake_path = root.join("cmake").join("pulp-packages.cmake");
    if lock.packages.is_empty() {
        let _ = std::fs::remove_file(&cmake_path);
    } else {
        let body = cmake::generate_packages_cmake(&lock, &reg);
        write_atomic(&cmake_path, body.as_bytes())?;
    }

    if !pkg.name.is_empty() {
        metadata::update_dependencies_md(&root, &pkg, false)?;
        metadata::update_notice_md(&root, &pkg, false)?;
    }

    writeln!(
        out,
        "{green}✓{reset} Removed {}",
        if pkg.name.is_empty() {
            package_id.as_str()
        } else {
            pkg.name.as_str()
        },
        green = color::green(),
        reset = color::reset()
    )
    .map_err(io)?;
    writeln!(
        out,
        "{dim}  Remember to remove target_link_libraries and #include directives from your code.{reset}",
        dim = color::dim(),
        reset = color::reset()
    )
    .map_err(io)?;
    Ok(0)
}

// ── update ─────────────────────────────────────────────────────────

/// Run `pulp-rs update [--apply]`.
///
/// # Errors
///
/// Registry / lock I/O.
pub fn run_update(args: &[String], out: &mut impl Write) -> Result<i32> {
    let root = resolve_root()?;
    let lock_path = root.join("packages.lock.json");
    if !lock_path.is_file() {
        writeln!(out, "No packages installed.").map_err(io)?;
        return Ok(0);
    }
    let reg_path = registry::find_registry_path(&root)
        .ok_or_else(|| CliError::Other("Package registry not found".to_owned()))?;
    let reg = load_or_err(&reg_path)?;
    let mut lock = registry::load_lock(&lock_path);
    let apply = args.iter().any(|a| a == "--apply");
    let mut has_updates = false;

    for (id, lp) in &mut lock.packages {
        let Some(pkg) = reg.packages.get(id) else {
            continue;
        };
        if lp.version != pkg.version {
            has_updates = true;
            writeln!(
                out,
                "  {name}: {dim}{old}{reset} → {green}{new}{reset}",
                name = pkg.name,
                dim = color::dim(),
                reset = color::reset(),
                old = lp.version,
                green = color::green(),
                new = pkg.version,
            )
            .map_err(io)?;
            if apply {
                lp.version.clone_from(&pkg.version);
                lp.commit.clone_from(&pkg.fetch.git_tag);
            }
        }
    }

    if !has_updates {
        writeln!(
            out,
            "{green}✓{reset} All packages are up to date",
            green = color::green(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(0);
    }

    if apply {
        registry::save_lock(&lock_path, &lock)?;
        let body = cmake::generate_packages_cmake(&lock, &reg);
        write_atomic(
            &root.join("cmake").join("pulp-packages.cmake"),
            body.as_bytes(),
        )?;
        writeln!(
            out,
            "{green}✓{reset} Updated packages and regenerated cmake/pulp-packages.cmake",
            green = color::green(),
            reset = color::reset()
        )
        .map_err(io)?;
    } else {
        writeln!(out, "\nRun 'pulp update --apply' to apply these updates.").map_err(io)?;
    }
    Ok(0)
}

// ── suggest ────────────────────────────────────────────────────────

/// Parsed `pulp-rs suggest` args.
#[derive(Debug, Default, Clone)]
pub struct SuggestArgs {
    /// `--description <text>`
    pub description: Option<String>,
    /// `--analyze <file>`
    pub analyze: Option<PathBuf>,
    /// `--alternative <package>`
    pub alternative: Option<String>,
    /// `--format json`
    pub json_output: bool,
}

/// Parse a `suggest` tail.
///
/// # Errors
///
/// Returns [`CliError::BadUsage`] for an unknown `--format`.
pub fn parse_suggest_args(args: &[String]) -> Result<SuggestArgs> {
    let mut out = SuggestArgs::default();
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        match a {
            "--description" if i + 1 < args.len() => {
                out.description = Some(args[i + 1].clone());
                i += 2;
            }
            "--analyze" if i + 1 < args.len() => {
                out.analyze = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            "--alternative" if i + 1 < args.len() => {
                out.alternative = Some(args[i + 1].clone());
                i += 2;
            }
            "--format" if i + 1 < args.len() => {
                if args[i + 1] == "json" {
                    out.json_output = true;
                } else {
                    return Err(CliError::BadUsage(format!(
                        "pulp suggest: unknown --format '{}'",
                        args[i + 1]
                    )));
                }
                i += 2;
            }
            _ => i += 1,
        }
    }
    Ok(out)
}

/// Run `pulp-rs suggest`.
///
/// # Errors
///
/// Registry load + file read for `--analyze`.
pub fn run_suggest(args: &SuggestArgs, out: &mut impl Write) -> Result<i32> {
    if args.description.is_none() && args.analyze.is_none() && args.alternative.is_none() {
        print_suggest_help(out);
        return Ok(0);
    }
    let cwd = std::env::current_dir().map_err(|e| CliError::io("<cwd>", e))?;
    let root = registry::find_project_root(&cwd);
    let reg_path = root
        .as_deref()
        .and_then(registry::find_registry_path)
        .ok_or_else(|| CliError::Other("Package registry not found".to_owned()))?;
    let reg = load_or_err(&reg_path)?;

    if let Some(desc) = &args.description {
        let results = registry::search(&reg, desc);
        if results.is_empty() {
            writeln!(
                out,
                "{}",
                if args.json_output {
                    "[]"
                } else {
                    "No packages match that description."
                }
            )
            .map_err(io)?;
            return Ok(0);
        }
        if args.json_output {
            let entries: Vec<Value> = results
                .iter()
                .take(5)
                .map(|p| {
                    json!({
                        "id": p.id,
                        "version": p.version,
                        "license": p.license,
                        "description": p.description,
                    })
                })
                .collect();
            let rendered =
                serde_json::to_string_pretty(&entries).unwrap_or_else(|_| "[]".to_owned());
            writeln!(out, "{rendered}").map_err(io)?;
            return Ok(0);
        }
        writeln!(out, "Suggested packages for \"{desc}\":\n").map_err(io)?;
        for p in results.iter().take(5) {
            writeln!(
                out,
                "  {green}{id}{reset} {dim}v{ver}{reset} {dim}[{lic}]{reset}",
                green = color::green(),
                reset = color::reset(),
                dim = color::dim(),
                id = p.id,
                ver = p.version,
                lic = p.license,
            )
            .map_err(io)?;
            writeln!(out, "    {}", p.description).map_err(io)?;
            if !p.overlaps_with_builtin.is_empty() {
                writeln!(
                    out,
                    "    {yel}Note:{reset} overlaps with Pulp built-ins",
                    yel = color::yellow(),
                    reset = color::reset()
                )
                .map_err(io)?;
            }
            writeln!(out).map_err(io)?;
        }
        return Ok(0);
    }

    if let Some(file) = &args.analyze {
        let content = std::fs::read_to_string(file).map_err(|e| CliError::io(file.clone(), e))?;
        let includes = extract_includes(&content);
        let mut found_any = false;
        for pkg in reg.packages.values() {
            let hit = pkg
                .tags
                .iter()
                .any(|tag| includes.iter().any(|inc| inc.contains(tag)));
            if hit {
                if !found_any {
                    writeln!(out, "Based on includes in {}:\n", file.display()).map_err(io)?;
                    found_any = true;
                }
                writeln!(
                    out,
                    "  {green}{}{reset} — {}",
                    pkg.id,
                    pkg.description,
                    green = color::green(),
                    reset = color::reset()
                )
                .map_err(io)?;
            }
        }
        if !found_any {
            writeln!(out, "No package suggestions based on {}", file.display()).map_err(io)?;
        }
        return Ok(0);
    }

    if let Some(id) = &args.alternative {
        let Some(pkg) = reg.packages.get(id) else {
            writeln!(
                out,
                "{red}✗{reset} Package '{id}' not found",
                red = color::red(),
                reset = color::reset()
            )
            .map_err(io)?;
            return Ok(1);
        };
        writeln!(out, "Alternatives to {}:\n", pkg.name).map_err(io)?;
        for provide in &pkg.provides {
            for (other_id, other) in &reg.packages {
                if other_id == id {
                    continue;
                }
                if other.provides.contains(provide) {
                    writeln!(
                        out,
                        "  {green}{oid}{reset} {dim}[{lic}]{reset} — {desc}",
                        oid = other.id,
                        lic = other.license,
                        desc = other.description,
                        green = color::green(),
                        reset = color::reset(),
                        dim = color::dim(),
                    )
                    .map_err(io)?;
                }
            }
        }
        if !pkg.alternatives.is_empty() {
            writeln!(out, "\nAlso noted (may require commercial license):").map_err(io)?;
            for alt in &pkg.alternatives {
                writeln!(
                    out,
                    "  {dim}{alt}{reset}",
                    dim = color::dim(),
                    reset = color::reset()
                )
                .map_err(io)?;
            }
        }
        return Ok(0);
    }

    Ok(0)
}

fn extract_includes(content: &str) -> Vec<String> {
    let re = regex::Regex::new(r#"#include\s*[<"]([^>"]+)[>"]"#).expect("static regex");
    re.captures_iter(content)
        .filter_map(|c| c.get(1).map(|m| m.as_str().to_owned()))
        .collect()
}

fn print_suggest_help(out: &mut impl Write) {
    let _ = writeln!(
        out,
        "Usage: pulp suggest [options]\n\nOptions:\n  --description \"<text>\"   Find packages matching a description\n  --analyze <file>          Scan a source file for package suggestions\n  --alternative <package>   Find alternatives to a package\n  --format json             Output as JSON"
    );
}

// ── target ─────────────────────────────────────────────────────────

/// Subcommands under `pulp-rs target`.
#[derive(Debug, Clone)]
pub enum TargetSub {
    /// `pulp-rs target list`.
    List,
    /// `pulp-rs target add <Platform-arch>`.
    Add(String),
    /// `pulp-rs target remove <Platform-arch>`.
    Remove(String),
    /// No subcommand — print help and exit 0.
    Help,
}

/// Parse the positional tail after `target`.
#[must_use]
pub fn parse_target_sub(args: &[String]) -> TargetSub {
    match args.first().map(String::as_str) {
        Some("list") => TargetSub::List,
        Some("add") => TargetSub::Add(args.get(1).cloned().unwrap_or_default()),
        Some("remove") => TargetSub::Remove(args.get(1).cloned().unwrap_or_default()),
        // None + unknown both fall through to help (matches C++
        // "unknown target subcommand").
        None | Some(_) => TargetSub::Help,
    }
}

/// Run `pulp-rs target …`.
///
/// # Errors
///
/// pulp.toml write failures.
pub fn run_target(sub: &TargetSub, out: &mut impl Write) -> Result<i32> {
    if matches!(sub, TargetSub::Help) {
        print_target_help(out);
        return Ok(0);
    }
    let root = resolve_root()?;
    match sub {
        TargetSub::List => do_target_list(&root, out),
        TargetSub::Add(s) => do_target_add(&root, s, out),
        TargetSub::Remove(s) => do_target_remove(&root, s, out),
        TargetSub::Help => Ok(0),
    }
}

fn do_target_list(root: &Path, out: &mut impl Write) -> Result<i32> {
    let list = targets::read(root);
    let has_toml = root.join("pulp.toml").is_file();
    if has_toml {
        writeln!(out, "Project targets:").map_err(io)?;
    } else {
        writeln!(
            out,
            "Project targets {dim}(defaults — no pulp.toml){reset}:",
            dim = color::dim(),
            reset = color::reset()
        )
        .map_err(io)?;
    }
    for t in &list {
        writeln!(out, "  {}", t.display()).map_err(io)?;
    }
    Ok(0)
}

fn do_target_add(root: &Path, s: &str, out: &mut impl Write) -> Result<i32> {
    if s.is_empty() {
        writeln!(
            out,
            "{red}✗{reset} Usage: pulp target add <Platform-arch>",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }
    let Some(parsed) = PlatformTarget::parse(s) else {
        writeln!(
            out,
            "{red}✗{reset} Invalid target: {s}",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        writeln!(out, "Valid platforms: macOS, Windows, Linux, iOS, WASM").map_err(io)?;
        writeln!(out, "Valid architectures: arm64, x64, wasm32").map_err(io)?;
        return Ok(1);
    };
    let mut list = targets::read(root);
    if list.iter().any(|t| t == &parsed) {
        writeln!(
            out,
            "{yel}⚠{reset} Target {} is already configured",
            parsed.display(),
            yel = color::yellow(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(0);
    }
    list.push(parsed.clone());
    targets::write(root, &list)?;
    writeln!(
        out,
        "{green}✓{reset} Added target: {}",
        parsed.display(),
        green = color::green(),
        reset = color::reset()
    )
    .map_err(io)?;

    // Installed-package compatibility warning.
    let lock_path = root.join("packages.lock.json");
    if lock_path.is_file() {
        if let Some(reg_path) = registry::find_registry_path(root) {
            if let Ok(reg) = registry::load(&reg_path) {
                let lock = registry::load_lock(&lock_path);
                for id in lock.packages.keys() {
                    let Some(pkg) = reg.packages.get(id) else {
                        continue;
                    };
                    let unsup = registry::unsupported_targets(pkg, std::slice::from_ref(&parsed));
                    if !unsup.is_empty() {
                        writeln!(
                            out,
                            "{yel}⚠{reset} {} does not support {}",
                            pkg.name,
                            parsed.display(),
                            yel = color::yellow(),
                            reset = color::reset()
                        )
                        .map_err(io)?;
                    }
                }
            }
        }
    }
    Ok(0)
}

fn do_target_remove(root: &Path, s: &str, out: &mut impl Write) -> Result<i32> {
    if s.is_empty() {
        writeln!(
            out,
            "{red}✗{reset} Usage: pulp target remove <Platform-arch>",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }
    let Some(parsed) = PlatformTarget::parse(s) else {
        writeln!(
            out,
            "{red}✗{reset} Invalid target: {s}",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    };
    let mut list = targets::read(root);
    let pos = list.iter().position(|t| t == &parsed);
    let Some(pos) = pos else {
        writeln!(
            out,
            "{red}✗{reset} Target {} is not configured",
            parsed.display(),
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    };
    if list.len() == 1 {
        writeln!(
            out,
            "{red}✗{reset} Cannot remove the last target",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }
    list.remove(pos);
    targets::write(root, &list)?;
    writeln!(
        out,
        "{green}✓{reset} Removed target: {}",
        parsed.display(),
        green = color::green(),
        reset = color::reset()
    )
    .map_err(io)?;
    Ok(0)
}

fn print_target_help(out: &mut impl Write) {
    let _ = writeln!(
        out,
        "Usage: pulp target <list|add|remove> [target]\n\nManage platform targets for your project.\nTarget format: Platform-arch (e.g., macOS-arm64, Windows-x64)\n\nCommands:\n  list              Show current project targets\n  add <target>      Add a platform target\n  remove <target>   Remove a platform target"
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_add_captures_flags() {
        let a = vec![
            "alac".to_owned(),
            "--platform-guard".to_owned(),
            "--no-cmake".to_owned(),
        ];
        let p = parse_add_args(&a);
        assert_eq!(p.package_id.as_deref(), Some("alac"));
        assert!(p.platform_guard);
        assert!(p.no_cmake);
    }

    #[test]
    fn parse_add_accept_license_sets_override() {
        let a = vec![
            "aubio".to_owned(),
            "--accept-license".to_owned(),
            "GPL-3.0".to_owned(),
        ];
        let p = parse_add_args(&a);
        assert_eq!(p.accepted_license.as_deref(), Some("GPL-3.0"));
        assert!(p.license_override);
    }

    #[test]
    fn parse_search_collects_query_terms() {
        let a = vec!["pitch".to_owned(), "tracker".to_owned()];
        let p = parse_search_args(&a).unwrap();
        assert_eq!(p.query, "pitch tracker");
        assert!(!p.json_output);
    }

    #[test]
    fn parse_search_rejects_unknown_format() {
        let a = vec!["q".to_owned(), "--format".to_owned(), "yaml".to_owned()];
        let err = parse_search_args(&a).unwrap_err();
        assert!(matches!(err, CliError::BadUsage(_)));
    }

    #[test]
    fn parse_suggest_sets_description() {
        let a = vec![
            "--description".to_owned(),
            "pitch detection".to_owned(),
            "--format".to_owned(),
            "json".to_owned(),
        ];
        let p = parse_suggest_args(&a).unwrap();
        assert_eq!(p.description.as_deref(), Some("pitch detection"));
        assert!(p.json_output);
    }

    #[test]
    fn parse_target_sub_list() {
        let sub = parse_target_sub(&["list".to_owned()]);
        assert!(matches!(sub, TargetSub::List));
    }

    #[test]
    fn parse_target_sub_empty_is_help() {
        let sub = parse_target_sub(&[]);
        assert!(matches!(sub, TargetSub::Help));
    }

    #[test]
    fn extract_includes_finds_angle_and_quote_forms() {
        let src = r#"#include <foo.h>
#include "bar.h"
int main(){return 0;}"#;
        let v = extract_includes(src);
        assert_eq!(v, vec!["foo.h", "bar.h"]);
    }
}
