//! `pulp-rs docs` — local documentation reader.
//!
//! # Phase 6d scope
//!
//! - `index` — Ported (walks `docs/status/docs-index.yaml`).
//! - `search <query>` — Ported (recursive grep + fuzzy fallback).
//! - `open <slug>` — Ported (resolves slug → path, prints file).
//! - `show support <thing>` — Ported (hierarchical YAML matcher).
//! - `show command <name>` — Ported (hand-rolled cli-commands.yaml walker).
//! - `show cmake <name>` — Ported.
//! - `show style` — Ported.
//! - `check` — Delegates to `tools/check-docs.sh` via the [`Spawner`].
//! - `build-site` — Delegates to `mkdocs build` via the [`Spawner`].
//! - `build-api` — Delegates to `tools/build-api-docs.sh`.
//!
//! # Parser choice
//!
//! The C++ reference deliberately avoids pulling a YAML library —
//! it uses the line-by-line `yaml_value(line, key)` helper. We do the
//! same for byte-parity on output (comments, whitespace, ordering
//! all preserved the way the C++ walker sees them). A real YAML
//! parser would make multi-line strings and anchors work, but those
//! aren't used in `docs/status/*.yaml`.

// The hand-rolled walkers in this module mirror the C++ YAML state
// machine line-by-line. Pedantic clippy doesn't like the style
// (loops with accumulated strings, long function bodies, local
// structs declared mid-function) but rewriting for elegance would
// drift the port from its oracle.
#![allow(
    clippy::assigning_clones,
    clippy::items_after_statements,
    clippy::needless_continue,
    clippy::too_many_lines,
    clippy::map_unwrap_or
)]

use std::fs::{self, File};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};

use crate::error::{CliError, Result};
use crate::proc::{Invocation, Spawner};

/// Parsed top-level docs subcommand.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Sub {
    /// `pulp docs` / `pulp docs help` — print banner.
    Help,
    /// `pulp docs index`.
    Index,
    /// `pulp docs search <query...>`.
    Search(String),
    /// `pulp docs open <slug>`.
    Open(String),
    /// `pulp docs show support <thing>`.
    ShowSupport(String),
    /// `pulp docs show command <name>`.
    ShowCommand(String),
    /// `pulp docs show cmake <name>`.
    ShowCmake(String),
    /// `pulp docs show style`.
    ShowStyle,
    /// `pulp docs check [extra...]`.
    Check(Vec<String>),
    /// `pulp docs build-site [extra...]`.
    BuildSite(Vec<String>),
    /// `pulp docs build-api [extra...]`.
    BuildApi(Vec<String>),
}

/// Parse the docs-command tail into a [`Sub`].
///
/// # Errors
///
/// Returns [`CliError::BadUsage`] for wrong usage shapes (e.g.
/// `open` with no slug), [`CliError::UnknownSubcommand`] for
/// unrecognised `show` topics.
pub fn parse_sub(args: &[String]) -> Result<Sub> {
    let Some(first) = args.first() else {
        return Ok(Sub::Help);
    };
    match first.as_str() {
        "help" | "--help" | "-h" => Ok(Sub::Help),
        "index" => Ok(Sub::Index),
        "search" => {
            let query = args[1..].join(" ");
            if query.is_empty() {
                return Err(CliError::BadUsage(
                    "Usage: pulp docs search <query>".to_owned(),
                ));
            }
            Ok(Sub::Search(query))
        }
        "open" => {
            let slug = args
                .get(1)
                .cloned()
                .ok_or_else(|| CliError::BadUsage("Usage: pulp docs open <slug>".to_owned()))?;
            Ok(Sub::Open(slug))
        }
        "show" => {
            let show_sub = args.get(1).map(String::as_str).unwrap_or("");
            match show_sub {
                "" => Err(CliError::BadUsage(
                    "Usage: pulp docs show <support|command|cmake|style> [name]".to_owned(),
                )),
                "support" => {
                    let thing = args.get(2).cloned().ok_or_else(|| {
                        CliError::BadUsage("Usage: pulp docs show support <thing>".to_owned())
                    })?;
                    Ok(Sub::ShowSupport(thing))
                }
                "command" => {
                    let name = args.get(2).cloned().ok_or_else(|| {
                        CliError::BadUsage("Usage: pulp docs show command <name>".to_owned())
                    })?;
                    Ok(Sub::ShowCommand(name))
                }
                "cmake" => {
                    let name = args.get(2).cloned().ok_or_else(|| {
                        CliError::BadUsage("Usage: pulp docs show cmake <name>".to_owned())
                    })?;
                    Ok(Sub::ShowCmake(name))
                }
                "style" => Ok(Sub::ShowStyle),
                _ => Err(CliError::UnknownSubcommand),
            }
        }
        "check" => Ok(Sub::Check(args[1..].to_vec())),
        "build-site" => Ok(Sub::BuildSite(args[1..].to_vec())),
        "build-api" => Ok(Sub::BuildApi(args[1..].to_vec())),
        _ => Err(CliError::UnknownSubcommand),
    }
}

/// Dispatch entry from `main`.
///
/// # Errors
///
/// Surfaces filesystem + spawn failures.
pub fn run<S: Spawner>(cwd: &Path, sub: &Sub, spawner: &S, out: &mut impl Write) -> Result<i32> {
    if matches!(sub, Sub::Help) {
        return print_help(out).map(|()| 0);
    }
    let Some(root) = find_root(cwd) else {
        return Err(CliError::Other(
            "Error: not in a Pulp project directory".to_owned(),
        ));
    };
    let docs_dir = root.join("docs");

    match sub {
        Sub::Help => unreachable!(),
        Sub::Index => docs_index(&docs_dir, out),
        Sub::Search(q) => docs_search(&docs_dir, q, out),
        Sub::Open(slug) => docs_open(&docs_dir, slug, out),
        Sub::ShowSupport(t) => docs_show_support(&docs_dir, t, out),
        Sub::ShowCommand(n) => docs_show_command(&docs_dir, n, out),
        Sub::ShowCmake(n) => docs_show_cmake(&docs_dir, n, out),
        Sub::ShowStyle => docs_show_style(&docs_dir, out),
        Sub::Check(extra) => {
            let script = root.join("tools").join("check-docs.sh");
            if !script.is_file() {
                return Err(CliError::Other(format!(
                    "Error: check script not found at {}",
                    script.display()
                )));
            }
            let inv = Invocation::new("bash")
                .arg(script.to_string_lossy().into_owned())
                .args(extra.iter().cloned());
            spawner.run(&inv)
        }
        Sub::BuildSite(extra) => {
            let yml = root.join("mkdocs.yml");
            let inv = Invocation::new("mkdocs")
                .arg("build")
                .arg("-f")
                .arg(yml.to_string_lossy().into_owned())
                .args(extra.iter().cloned());
            let rc = spawner.run(&inv)?;
            if rc != 0 {
                writeln!(
                    out,
                    "Hint: install docs deps with `pip install -r requirements-docs.txt`"
                )
                .map_err(io)?;
            }
            Ok(rc)
        }
        Sub::BuildApi(extra) => {
            let script = root.join("tools").join("build-api-docs.sh");
            if !script.is_file() {
                return Err(CliError::Other(format!(
                    "Error: build script not found at {}",
                    script.display()
                )));
            }
            let inv = Invocation::new("bash")
                .arg(script.to_string_lossy().into_owned())
                .args(extra.iter().cloned());
            spawner.run(&inv)
        }
    }
}

/// Banner printed when invoked with no subcommand.
///
/// # Errors
///
/// Write failure.
pub fn print_help(out: &mut impl Write) -> Result<()> {
    out.write_all(
        b"pulp docs - local documentation reader\n\n\
        Subcommands:\n\
        \x20 index                    List available docs\n\
        \x20 search <query>           Search docs for a string\n\
        \x20 open <slug>              Print a doc by slug\n\
        \x20 show support <thing>     Look up support status\n\
        \x20 show command <name>      Look up a CLI command\n\
        \x20 show cmake <name>        Look up a CMake function\n\
        \x20 show style               Show code style rules\n\
        \x20 check                    Validate docs consistency\n\
        \x20 build-site               Generate static docs site\n\
        \x20 build-api                Generate API reference (Doxygen)\n",
    )
    .map_err(io)
}

fn io(e: std::io::Error) -> CliError {
    CliError::io("<stdout>", e)
}

// ── Helpers ───────────────────────────────────────────────────────────

fn find_root(cwd: &Path) -> Option<PathBuf> {
    let mut cur = cwd.to_path_buf();
    loop {
        if cur.join("CMakeLists.txt").is_file() && cur.join("core").is_dir() {
            return Some(cur);
        }
        if cur.join("pulp.toml").is_file() {
            return Some(cur);
        }
        if !cur.pop() {
            return None;
        }
    }
}

fn yaml_value<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let pat = format!("{key}:");
    let idx = line.find(&pat)?;
    let val_start = idx + pat.len();
    if val_start >= line.len() {
        return None;
    }
    let raw = line[val_start..].trim();
    if raw.is_empty() {
        return None;
    }
    Some(raw)
}

fn trim_line(s: &str) -> &str {
    s.trim()
}

fn icontains(haystack: &str, needle: &str) -> bool {
    if needle.is_empty() {
        return true;
    }
    let h = haystack.to_lowercase();
    let n = needle.to_lowercase();
    h.contains(&n)
}

/// Ported from C++ `fuzzy_score`.
fn fuzzy_score(text: &str, query: &str) -> i32 {
    if query.is_empty() {
        return 1;
    }
    if text.is_empty() {
        return 0;
    }
    let lt = text.to_lowercase();
    let lq = query.to_lowercase();
    if lt.contains(&lq) {
        return 100 + i32::try_from(query.chars().count()).unwrap_or(0);
    }
    let text_chars: Vec<char> = lt.chars().collect();
    let query_chars: Vec<char> = lq.chars().collect();
    let mut score = 0;
    let mut qi = 0;
    let mut prev = false;
    for ch in text_chars {
        if qi < query_chars.len() && ch == query_chars[qi] {
            score += if prev { 3 } else { 1 };
            prev = true;
            qi += 1;
        } else {
            prev = false;
        }
    }
    if qi < query_chars.len() {
        return 0;
    }
    score
}

fn read_text(path: &Path) -> Option<String> {
    fs::read_to_string(path).ok()
}

// ── Subcommand implementations ────────────────────────────────────────

fn docs_index(docs_dir: &Path, out: &mut impl Write) -> Result<i32> {
    let idx_path = docs_dir.join("status").join("docs-index.yaml");
    let Some(content) = read_text(&idx_path) else {
        return Err(CliError::Other(format!(
            "Error: docs index not found at {}\nHint: the docs/ tree may not be set up yet.",
            idx_path.display()
        )));
    };

    writeln!(out, "Available Documentation").map_err(io)?;
    writeln!(out, "=======================\n").map_err(io)?;

    let mut slug = String::new();
    let mut path = String::new();
    let mut kind = String::new();

    let flush = |slug: &mut String,
                 path: &mut String,
                 kind: &mut String,
                 out: &mut dyn Write|
     -> std::io::Result<()> {
        if !slug.is_empty() {
            if kind.is_empty() {
                writeln!(out, "  {slug}")?;
            } else {
                writeln!(out, "  {slug} ({kind})")?;
            }
            if !path.is_empty() {
                writeln!(out, "    -> docs/{path}")?;
            }
        }
        slug.clear();
        path.clear();
        kind.clear();
        Ok(())
    };

    for line in content.lines() {
        if let Some(s) = yaml_value(line, "slug") {
            flush(&mut slug, &mut path, &mut kind, out).map_err(io)?;
            slug = s.to_owned();
        }
        if let Some(p) = yaml_value(line, "path") {
            path = p.to_owned();
        }
        if let Some(k) = yaml_value(line, "kind") {
            kind = k.to_owned();
        }
    }
    flush(&mut slug, &mut path, &mut kind, out).map_err(io)?;

    writeln!(out, "\nUse `pulp docs open <slug>` to read a doc.").map_err(io)?;
    Ok(0)
}

fn docs_search(docs_dir: &Path, query: &str, out: &mut impl Write) -> Result<i32> {
    if query.is_empty() {
        return Err(CliError::BadUsage(
            "Usage: pulp docs search <query>".to_owned(),
        ));
    }
    if !docs_dir.is_dir() {
        return Err(CliError::Other(
            "Error: docs/ directory not found.".to_owned(),
        ));
    }

    let mut match_count = 0usize;

    // Collect files deterministically (alphabetical).
    let mut md_files = Vec::new();
    collect_md(docs_dir, &mut md_files);
    md_files.sort();

    for path in &md_files {
        let Ok(f) = File::open(path) else { continue };
        let reader = BufReader::new(f);
        let mut file_printed = false;
        let mut matches_in_file = 0;
        for (idx, line) in reader.lines().enumerate() {
            let Ok(line) = line else { continue };
            let line_num = idx + 1;
            if icontains(&line, query) {
                if !file_printed {
                    let rel = path.strip_prefix(docs_dir).unwrap_or(path);
                    writeln!(out, "\ndocs/{}:", rel.display()).map_err(io)?;
                    file_printed = true;
                }
                if matches_in_file < 5 {
                    let display = if line.len() > 120 {
                        format!("{}...", &line[..117])
                    } else {
                        line.clone()
                    };
                    writeln!(out, "  {line_num}: {}", trim_line(&display)).map_err(io)?;
                }
                matches_in_file += 1;
                match_count += 1;
            }
        }
        if matches_in_file > 5 {
            writeln!(out, "  ... and {} more matches", matches_in_file - 5).map_err(io)?;
        }
    }

    if match_count == 0 {
        // Fuzzy fallback.
        let mut hits: Vec<(i32, String)> = Vec::new();
        for path in &md_files {
            let rel = path
                .strip_prefix(docs_dir)
                .unwrap_or(path)
                .display()
                .to_string();
            let name_score = fuzzy_score(&rel, query);
            if name_score > 0 {
                hits.push((name_score, format!("docs/{rel}")));
            }
            if let Ok(f) = File::open(path) {
                let reader = BufReader::new(f);
                for line in reader.lines().map_while(std::result::Result::ok).take(30) {
                    if line.len() > 2 && line.starts_with('#') {
                        let score = fuzzy_score(&line, query);
                        if score > name_score {
                            hits.push((score, format!("docs/{rel} - {}", trim_line(&line))));
                        }
                        break;
                    }
                }
            }
        }
        hits.sort_by(|a, b| b.0.cmp(&a.0));
        if hits.is_empty() {
            writeln!(out, "No matches for \"{query}\" in docs/").map_err(io)?;
        } else {
            writeln!(out, "No exact matches for \"{query}\". Did you mean:").map_err(io)?;
            for (_, d) in hits.iter().take(5) {
                writeln!(out, "  {d}").map_err(io)?;
            }
        }
    } else {
        writeln!(out, "\n{match_count} match(es) found.").map_err(io)?;
    }
    Ok(0)
}

fn collect_md(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for entry in rd.flatten() {
        let p = entry.path();
        if p.is_dir() {
            collect_md(&p, out);
        } else if p.extension().and_then(std::ffi::OsStr::to_str) == Some("md") {
            out.push(p);
        }
    }
}

fn docs_open(docs_dir: &Path, slug: &str, out: &mut impl Write) -> Result<i32> {
    let idx = docs_dir.join("status").join("docs-index.yaml");
    let Some(content) = read_text(&idx) else {
        return Err(CliError::Other(format!(
            "Error: docs index not found at {}",
            idx.display()
        )));
    };
    let mut cur_slug = String::new();
    let mut cur_path = String::new();
    let mut found_path: Option<String> = None;

    for line in content.lines() {
        if let Some(s) = yaml_value(line, "slug") {
            if cur_slug == slug && !cur_path.is_empty() {
                found_path = Some(cur_path.clone());
                break;
            }
            cur_slug = s.to_owned();
            cur_path.clear();
        }
        if let Some(p) = yaml_value(line, "path") {
            cur_path = p.to_owned();
        }
    }
    // Post-loop flush.
    if found_path.is_none() && cur_slug == slug && !cur_path.is_empty() {
        found_path = Some(cur_path);
    }

    let Some(rel) = found_path else {
        return Err(CliError::Other(format!(
            "Error: no doc found for slug \"{slug}\"\nRun `pulp docs index` to see available docs."
        )));
    };

    let file_path = docs_dir.join(&rel);
    let Some(body) = read_text(&file_path) else {
        return Err(CliError::Other(format!(
            "Error: file not found at {}",
            file_path.display()
        )));
    };
    out.write_all(body.as_bytes()).map_err(io)?;
    Ok(0)
}

fn docs_show_support(docs_dir: &Path, thing: &str, out: &mut impl Write) -> Result<i32> {
    let matrix = docs_dir.join("status").join("support-matrix.yaml");
    let Some(content) = read_text(&matrix) else {
        return Err(CliError::Other(format!(
            "Error: support matrix not found at {}",
            matrix.display()
        )));
    };

    let query_lower = thing.to_lowercase();
    let mut found = false;
    let mut section = String::new();

    // Two-pass (exact entry match, then fallback by key OR section).
    // Pass 1: entry match
    let mut lines = content.lines().peekable();
    while let Some(line) = lines.next() {
        let trimmed = trim_line(line);
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        let indent = line.chars().take_while(|c| *c == ' ').count();
        if indent == 0 && trimmed.ends_with(':') {
            section = trimmed[..trimmed.len() - 1].to_owned();
            continue;
        }
        if indent == 2 && trimmed.ends_with(':') {
            let entry_name = trimmed[..trimmed.len() - 1].to_owned();
            if entry_name.to_lowercase() == query_lower {
                if !found {
                    writeln!(out, "Support info for \"{thing}\":\n").map_err(io)?;
                    found = true;
                }
                writeln!(out, "  Section:  {section}").map_err(io)?;
                writeln!(out, "  Entry:    {entry_name}").map_err(io)?;
                // consume child lines until indent <= 2
                while let Some(&next) = lines.peek() {
                    let nind = next.chars().take_while(|c| *c == ' ').count();
                    let nt = trim_line(next);
                    if nt.is_empty() || nt.starts_with('#') {
                        lines.next();
                        continue;
                    }
                    if nind <= 2 {
                        break;
                    }
                    lines.next();
                    if let Some((k, v)) = nt.split_once(':') {
                        let key = k.trim();
                        let val = v.trim();
                        if key.is_empty() {
                            continue;
                        }
                        let mut key_cap = key.to_owned();
                        if let Some(first) = key_cap.get_mut(0..1) {
                            first.make_ascii_uppercase();
                        }
                        writeln!(out, "  {key_cap}: {val}").map_err(io)?;
                    }
                }
                writeln!(out).map_err(io)?;
            }
            continue;
        }
        if indent == 2 && !trimmed.is_empty() && !trimmed.ends_with(':') {
            if let Some((k, v)) = trimmed.split_once(':') {
                let key = k.trim();
                let val = v.trim();
                if key.to_lowercase() == query_lower {
                    if !found {
                        writeln!(out, "Support info for \"{thing}\":\n").map_err(io)?;
                        found = true;
                    }
                    writeln!(out, "  Section: {section}").map_err(io)?;
                    writeln!(out, "  {key}: {val}\n").map_err(io)?;
                }
            }
        }
    }

    // Pass 2: section match fallback.
    if !found {
        let mut in_section = false;
        for line in content.lines() {
            let trimmed = trim_line(line);
            if trimmed.is_empty() || trimmed.starts_with('#') {
                continue;
            }
            let indent = line.chars().take_while(|c| *c == ' ').count();
            if indent == 0 && trimmed.ends_with(':') {
                let name = trimmed[..trimmed.len() - 1].to_owned();
                in_section = name.to_lowercase() == query_lower;
                if in_section && !found {
                    writeln!(out, "Support info for \"{thing}\":\n").map_err(io)?;
                    writeln!(out, "[{name}]").map_err(io)?;
                    found = true;
                }
                continue;
            }
            if in_section && indent >= 2 {
                if let Some((k, v)) = trimmed.split_once(':') {
                    if !trimmed.starts_with('-') {
                        let key = k.trim();
                        let val = v.trim();
                        if val.is_empty() {
                            writeln!(out, "  {key}:").map_err(io)?;
                        } else {
                            writeln!(out, "  {key}: {val}").map_err(io)?;
                        }
                    }
                }
            }
        }
    }

    if !found {
        return Err(CliError::Other(format!(
            "No support info found for \"{thing}\"\n\
            Available sections: platforms, formats, audio_io, midi_io, rendering, subsystems\n\
            Available entries: macos, windows, linux, vst3, au_v2, clap, standalone, etc."
        )));
    }
    Ok(0)
}

fn docs_show_command(docs_dir: &Path, name: &str, out: &mut impl Write) -> Result<i32> {
    let path = docs_dir.join("status").join("cli-commands.yaml");
    let Some(content) = read_text(&path) else {
        return Err(CliError::Other(format!(
            "Error: CLI commands manifest not found at {}",
            path.display()
        )));
    };

    let mut found = false;
    let mut in_entry = false;
    let mut in_sub = false;
    let mut in_args = false;
    let mut in_sub_args = false;

    let mut cmd_status = String::new();
    let mut cmd_summary = String::new();

    struct Sub {
        name: String,
        summary: String,
    }
    struct Arg {
        name: String,
        description: String,
        kind: String,
    }
    let mut subs: Vec<Sub> = Vec::new();
    let mut top_args: Vec<Arg> = Vec::new();

    for line in content.lines() {
        let trimmed = trim_line(line);
        let indent = line.chars().take_while(|c| *c == ' ').count();
        let n = yaml_value(line, "name").map(str::to_owned);

        if let Some(ref nm) = n {
            if indent <= 4 {
                if in_entry {
                    break;
                }
                if nm == name {
                    found = true;
                    in_entry = true;
                }
                continue;
            }
        }
        if !in_entry {
            continue;
        }

        if trimmed.starts_with("subcommands:") {
            in_sub = true;
            in_args = false;
            in_sub_args = false;
            continue;
        }
        if trimmed.starts_with("args:") && !in_sub {
            in_args = true;
            in_sub = false;
            in_sub_args = false;
            continue;
        }

        if in_sub {
            if trimmed.starts_with("args:") {
                in_sub_args = true;
                continue;
            }
            if in_sub_args {
                if let Some(ref nm) = n {
                    if indent <= 6 {
                        in_sub_args = false;
                    } else {
                        let _ = nm;
                        continue;
                    }
                } else {
                    continue;
                }
            }
            if let Some(nm) = n.clone() {
                subs.push(Sub {
                    name: nm,
                    summary: String::new(),
                });
                continue;
            }
            if let Some(s) = yaml_value(line, "summary") {
                if let Some(last) = subs.last_mut() {
                    last.summary = s.to_owned();
                }
                continue;
            }
        }

        if in_args {
            if let Some(nm) = n.clone() {
                top_args.push(Arg {
                    name: nm,
                    description: String::new(),
                    kind: String::new(),
                });
                continue;
            }
            if let Some(d) = yaml_value(line, "description") {
                if let Some(last) = top_args.last_mut() {
                    last.description = d.to_owned();
                }
                continue;
            }
            if let Some(k) = yaml_value(line, "kind") {
                if let Some(last) = top_args.last_mut() {
                    last.kind = k.to_owned();
                }
                continue;
            }
        }

        if !in_sub && !in_args {
            if let Some(s) = yaml_value(line, "status") {
                cmd_status = s.to_owned();
                continue;
            }
            if let Some(s) = yaml_value(line, "summary") {
                cmd_summary = s.to_owned();
                continue;
            }
        }
    }

    if !found {
        return Err(CliError::Other(format!(
            "No command found for \"{name}\"\nCheck docs/status/cli-commands.yaml for available commands."
        )));
    }

    writeln!(out, "Command: {name}").map_err(io)?;
    if !cmd_status.is_empty() {
        writeln!(out, "  Status:  {cmd_status}").map_err(io)?;
    }
    if !cmd_summary.is_empty() {
        writeln!(out, "  Summary: {cmd_summary}").map_err(io)?;
    }
    if !top_args.is_empty() {
        writeln!(out, "\n  Arguments:").map_err(io)?;
        for a in &top_args {
            write!(out, "    {}", a.name).map_err(io)?;
            if !a.kind.is_empty() {
                write!(out, " ({})", a.kind).map_err(io)?;
            }
            if !a.description.is_empty() {
                write!(out, " - {}", a.description).map_err(io)?;
            }
            writeln!(out).map_err(io)?;
        }
    }
    if !subs.is_empty() {
        writeln!(out, "\n  Subcommands:").map_err(io)?;
        for s in &subs {
            write!(out, "    {}", s.name).map_err(io)?;
            if !s.summary.is_empty() {
                write!(out, " - {}", s.summary).map_err(io)?;
            }
            writeln!(out).map_err(io)?;
        }
    }
    writeln!(out, "\nSee also: docs/reference/cli.md").map_err(io)?;
    Ok(0)
}

fn docs_show_cmake(docs_dir: &Path, name: &str, out: &mut impl Write) -> Result<i32> {
    let path = docs_dir.join("status").join("cmake-functions.yaml");
    let Some(content) = read_text(&path) else {
        return Err(CliError::Other(format!(
            "Error: CMake functions manifest not found at {}",
            path.display()
        )));
    };

    let mut found = false;
    let mut in_entry = false;
    for line in content.lines() {
        if let Some(n) = yaml_value(line, "name") {
            if in_entry {
                break;
            }
            if n == name {
                found = true;
                in_entry = true;
                writeln!(out, "CMake function: {n}").map_err(io)?;
            }
            continue;
        }
        if in_entry {
            let t = trim_line(line);
            if t.is_empty() || t == "-" {
                continue;
            }
            writeln!(out, "  {t}").map_err(io)?;
        }
    }

    if !found {
        return Err(CliError::Other(format!(
            "No CMake function found for \"{name}\"\nCheck docs/status/cmake-functions.yaml for available functions."
        )));
    }
    writeln!(out, "\nSee also: docs/reference/cmake.md").map_err(io)?;
    Ok(0)
}

fn docs_show_style(docs_dir: &Path, out: &mut impl Write) -> Result<i32> {
    let path = docs_dir.join("status").join("style-rules.yaml");
    let Some(content) = read_text(&path) else {
        return Err(CliError::Other(format!(
            "Error: style rules not found at {}",
            path.display()
        )));
    };

    writeln!(out, "Style Rules").map_err(io)?;
    writeln!(out, "===========\n").map_err(io)?;

    for line in content.lines() {
        let trimmed = trim_line(line);
        if trimmed.is_empty() {
            continue;
        }
        let id = yaml_value(line, "id");
        let rule = yaml_value(line, "rule");
        let severity = yaml_value(line, "severity");
        if let Some(id) = id {
            writeln!(out, "\n[{id}]").map_err(io)?;
        } else if let Some(r) = rule {
            writeln!(out, "  Rule: {r}").map_err(io)?;
        } else if let Some(s) = severity {
            writeln!(out, "  Severity: {s}").map_err(io)?;
        } else if let Some((k, v)) = trimmed.split_once(':') {
            let key = k.trim();
            let val = v.trim();
            if !key.is_empty()
                && !val.is_empty()
                && !trimmed.starts_with('-')
                && !trimmed.starts_with('#')
            {
                writeln!(out, "  {key}: {val}").map_err(io)?;
            }
        }
    }
    writeln!(out, "\nFull details:").map_err(io)?;
    writeln!(out, "  docs/policies/code-style.md").map_err(io)?;
    writeln!(out, "  docs/policies/agent-contribution-rules.md").map_err(io)?;
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proc::testing::RecordingSpawner;

    fn plant_project() -> tempfile::TempDir {
        let td = tempfile::tempdir().unwrap();
        fs::create_dir_all(td.path().join("core")).unwrap();
        fs::write(td.path().join("CMakeLists.txt"), "project(Demo)\n").unwrap();
        fs::create_dir_all(td.path().join("docs").join("status")).unwrap();
        td
    }

    #[test]
    fn parse_help_default() {
        assert_eq!(parse_sub(&[]).unwrap(), Sub::Help);
        assert_eq!(parse_sub(&["help".into()]).unwrap(), Sub::Help);
    }

    #[test]
    fn parse_search_requires_query() {
        assert!(parse_sub(&["search".into()]).is_err());
        assert_eq!(
            parse_sub(&["search".into(), "foo".into(), "bar".into()]).unwrap(),
            Sub::Search("foo bar".to_owned())
        );
    }

    #[test]
    fn parse_show_variants() {
        assert_eq!(
            parse_sub(&["show".into(), "style".into()]).unwrap(),
            Sub::ShowStyle
        );
        assert_eq!(
            parse_sub(&["show".into(), "support".into(), "vst3".into()]).unwrap(),
            Sub::ShowSupport("vst3".to_owned())
        );
    }

    #[test]
    fn parse_show_unknown_topic_errors() {
        assert!(matches!(
            parse_sub(&["show".into(), "nope".into()]),
            Err(CliError::UnknownSubcommand)
        ));
    }

    #[test]
    fn index_renders_slugs_and_paths() {
        let td = plant_project();
        let idx = td.path().join("docs/status/docs-index.yaml");
        fs::write(
            &idx,
            "- slug: overview\n  path: reference/overview.md\n  kind: reference\n",
        )
        .unwrap();
        let mut buf = Vec::new();
        let rc = docs_index(&td.path().join("docs"), &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert_eq!(rc, 0);
        assert!(s.contains("Available Documentation"));
        assert!(s.contains("overview (reference)"));
        assert!(s.contains("-> docs/reference/overview.md"));
    }

    #[test]
    fn search_finds_literal_match() {
        let td = plant_project();
        let doc = td.path().join("docs").join("example.md");
        fs::write(&doc, "# Hello\nSome prose mentioning Pulp.\n").unwrap();
        let mut buf = Vec::new();
        docs_search(&td.path().join("docs"), "pulp", &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("docs/example.md"));
        assert!(s.contains("Some prose"));
        assert!(s.contains("1 match(es) found"));
    }

    #[test]
    fn search_fuzzy_fallback_suggests_filename() {
        let td = plant_project();
        let doc = td.path().join("docs").join("render.md");
        fs::write(&doc, "# Rendering details\n").unwrap();
        let mut buf = Vec::new();
        docs_search(&td.path().join("docs"), "rndr", &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Did you mean"));
        assert!(s.contains("render"));
    }

    #[test]
    fn open_resolves_slug_to_file() {
        let td = plant_project();
        fs::write(
            td.path().join("docs/status/docs-index.yaml"),
            "- slug: hello\n  path: hello.md\n",
        )
        .unwrap();
        fs::write(td.path().join("docs/hello.md"), "body\n").unwrap();
        let mut buf = Vec::new();
        docs_open(&td.path().join("docs"), "hello", &mut buf).unwrap();
        assert_eq!(buf, b"body\n");
    }

    #[test]
    fn open_reports_missing_slug() {
        let td = plant_project();
        fs::write(
            td.path().join("docs/status/docs-index.yaml"),
            "- slug: a\n  path: a.md\n",
        )
        .unwrap();
        let mut buf = Vec::new();
        let err = docs_open(&td.path().join("docs"), "b", &mut buf).unwrap_err();
        assert!(err.to_string().contains("no doc found for slug"));
    }

    #[test]
    fn show_style_prints_header() {
        let td = plant_project();
        fs::write(
            td.path().join("docs/status/style-rules.yaml"),
            "- id: rule1\n  rule: no tabs\n  severity: error\n",
        )
        .unwrap();
        let mut buf = Vec::new();
        docs_show_style(&td.path().join("docs"), &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Style Rules"));
        assert!(s.contains("[rule1]"));
        assert!(s.contains("Rule: no tabs"));
    }

    #[test]
    fn show_cmake_prints_function() {
        let td = plant_project();
        fs::write(
            td.path().join("docs/status/cmake-functions.yaml"),
            "- name: pulp_add_plugin\n  summary: Adds a plugin\n",
        )
        .unwrap();
        let mut buf = Vec::new();
        docs_show_cmake(&td.path().join("docs"), "pulp_add_plugin", &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("CMake function: pulp_add_plugin"));
        assert!(s.contains("summary: Adds a plugin"));
    }

    #[test]
    fn show_support_entry_match() {
        let td = plant_project();
        fs::write(
            td.path().join("docs/status/support-matrix.yaml"),
            "formats:\n  vst3:\n    status: stable\n    notes: tested\n",
        )
        .unwrap();
        let mut buf = Vec::new();
        docs_show_support(&td.path().join("docs"), "vst3", &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Section:  formats"));
        assert!(s.contains("Entry:    vst3"));
        assert!(s.contains("Status: stable"));
    }

    #[test]
    fn run_build_site_delegates_to_mkdocs() {
        let td = plant_project();
        fs::write(td.path().join("mkdocs.yml"), "site_name: Test\n").unwrap();
        let rec = RecordingSpawner::ok();
        let mut buf = Vec::new();
        let sub = Sub::BuildSite(vec!["--strict".to_owned()]);
        let rc = run(td.path(), &sub, &rec, &mut buf).unwrap();
        assert_eq!(rc, 0);
        let calls = rec.calls.borrow();
        assert_eq!(calls[0].program, "mkdocs");
        assert!(calls[0].args.contains(&"build".to_owned()));
        assert!(calls[0].args.contains(&"--strict".to_owned()));
    }

    #[test]
    fn run_check_requires_script_to_exist() {
        let td = plant_project();
        let rec = RecordingSpawner::ok();
        let mut buf = Vec::new();
        let err = run(td.path(), &Sub::Check(vec![]), &rec, &mut buf).unwrap_err();
        assert!(err.to_string().contains("check script not found"));
    }

    #[test]
    fn fuzzy_score_matches_cxx_semantics() {
        assert_eq!(fuzzy_score("", "x"), 0);
        assert_eq!(fuzzy_score("x", ""), 1);
        let s = fuzzy_score("render engine", "engine");
        assert!(s >= 100);
        // Non-substring with all chars in order should score > 0.
        assert!(fuzzy_score("render", "rndr") > 0);
        // Missing char → 0.
        assert_eq!(fuzzy_score("render", "zzzz"), 0);
    }
}
