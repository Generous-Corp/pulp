//! Usage banner + known-command inventory — shared source of truth.
//!
//! # Why a dedicated module
//!
//! Three call sites need the same command list:
//!
//! - [`crate::cmd::help::run`] — the explicit `pulp-rs help` subcommand.
//! - `main::real_main` — the bare-invocation case (`pulp-rs` with no
//!   args), which in the C++ CLI prints the usage banner and exits 0.
//! - `main::suggest_closest` — the fuzzy-match "Did you mean…?" helper
//!   that runs when `clap` rejects an unknown subcommand.
//!
//! Duplicating the command table between those sites is exactly how
//! drift enters: someone adds a Rust-only subcommand, the suggester
//! forgets about it, and the help banner forgets about it. One
//! inventory, one writer — no drift.
//!
//! # Layout
//!
//! [`Entry`] mirrors the C++ dispatch tables in `pulp_cli.cpp`:
//!
//! - `commands[]` — native Rust-ported (or to-be-ported) subcommands.
//! - `script_commands[]` — Python-script delegates (Rust does not
//!   re-implement them; the banner just lists them so users can see
//!   they're available on the C++ binary).
//! - `binary_commands[]` — built-binary delegates (same treatment).
//! - Inline/trailing pseudo-commands (`audit`, package/tool commands,
//!   `help`) — parked in a second block to match the C++ banner's
//!   shape.
//!
//! Entries are ordered to match the C++ banner, with Rust-native
//! extensions inserted beside the closest related command. The
//! `help_parity_test` integration test pins that installed surface.
//!
//! # Parity posture
//!
//! The Rust port's banner intentionally mirrors the C++ banner for
//! delegated commands and adds Rust-native commands that have no C++
//! table entry. The user still invokes `pulp` expecting one installed
//! surface; Rust-native vs delegated distinctions should not leak into
//! user-visible output.

use std::io::Write;

use crate::error::{CliError, Result};

/// One entry in the usage banner.
#[derive(Debug, Clone, Copy)]
pub struct Entry {
    /// Subcommand name as typed by the user.
    pub name: &'static str,
    /// One-line description shown in the installed CLI usage banner.
    pub summary: &'static str,
}

/// Primary dispatch table — mirrors `commands[]` in `pulp_cli.cpp`.
pub const COMMANDS: &[Entry] = &[
    Entry {
        name: "build",
        summary: "Configure and build the project",
    },
    Entry {
        name: "run",
        summary: "Launch a standalone Pulp application",
    },
    Entry {
        name: "test",
        summary: "Run the test suite",
    },
    Entry {
        name: "status",
        summary: "Show project status and info",
    },
    Entry {
        name: "create",
        summary: "Scaffold a new plugin project",
    },
    Entry {
        name: "validate",
        summary: "Run plugin format validators",
    },
    Entry {
        name: "doctor",
        summary: "Diagnose environment issues",
    },
    Entry {
        name: "ship",
        summary: "Sign, package, and distribute",
    },
    Entry {
        name: "design",
        summary: "Launch the AI design tool",
    },
    Entry {
        name: "docs",
        summary: "Browse local documentation",
    },
    Entry {
        name: "clean",
        summary: "Remove build directory",
    },
    Entry {
        name: "fmt",
        summary: "Run clang-format on the source tree",
    },
    Entry {
        name: "cache",
        summary: "Manage SDK and asset cache",
    },
    Entry {
        name: "audio",
        summary: "Repo-level audio model and bundle tooling",
    },
    Entry {
        name: "sdk",
        summary: "Manage the Pulp SDK installation",
    },
    Entry {
        name: "upgrade",
        summary: "Update the CLI to the latest version",
    },
    Entry {
        name: "version",
        summary: "Show, bump, or check version info",
    },
    Entry {
        name: "dev",
        summary: "Unified dev loop: watch, build, test, run",
    },
    Entry {
        name: "loop",
        summary: "Leveraged-prototype focus marker + watch loop",
    },
    Entry {
        name: "inspect",
        summary: "Connect to a running plugin inspector",
    },
    Entry {
        name: "motion",
        summary: "Agent-facing wrappers around inspector Motion protocol",
    },
    Entry {
        name: "trace",
        summary: "Agent-facing wrappers around inspector Trace (Perfetto) protocol",
    },
    Entry {
        name: "scan",
        summary: "List VST3 / AU / AUv3 / CLAP / LV2 plug-ins",
    },
    Entry {
        name: "host",
        summary: "Load a plug-in and run a synthetic audio block through it",
    },
    Entry {
        name: "import",
        summary: "Detect a framework project and emit a Pulp migration scaffold",
    },
    Entry {
        name: "identity",
        summary: "Manage the .pulp/identity.lock contract",
    },
    Entry {
        name: "kit",
        summary: "Inspect and apply local Pulp package manifests",
    },
    Entry {
        name: "content",
        summary: "Validate and install data-only content packs",
    },
    Entry {
        name: "pr",
        summary: "One-shot push-a-PR: gates + bump + ship",
    },
    Entry {
        name: "projects",
        summary: "Manage the ~/.pulp/projects.json registry",
    },
    Entry {
        name: "project",
        summary: "Per-project SDK pin: pin, unpin, undo",
    },
    Entry {
        name: "config",
        summary: "Read or write ~/.pulp/config.toml settings",
    },
    Entry {
        name: "coverage",
        summary: "Local coverage tooling (diff-cover gate mirror)",
    },
    Entry {
        name: "macos",
        summary: "Per-PR macOS-runner retargeting (local/namespace/github-hosted)",
    },
    Entry {
        name: "ci-host",
        summary: "Onboard a Mac as a Tart-VM CI host (optional; wraps setup-ci-host.sh)",
    },
    Entry {
        name: "overflow",
        summary: "Configure macOS-runner overflow routing (status/enable/disable/threshold)",
    },
    Entry {
        name: "tweaks",
        summary: "Inspect the pulp-tweaks.json sidecar (diff against a design)",
    },
];

/// Script-delegate table — mirrors `script_commands[]`.
pub const SCRIPT_COMMANDS: &[Entry] = &[
    Entry {
        name: "ci-local",
        summary: "Local-first CI across configured hosts",
    },
    Entry {
        name: "harness",
        summary: "Catalog-driven coverage harness (compat.json verifier)",
    },
];

/// Binary-delegate table — mirrors `binary_commands[]`.
pub const BINARY_COMMANDS: &[Entry] = &[
    Entry {
        name: "design-debug",
        summary: "Headless design debug runner",
    },
    Entry {
        name: "import-design",
        summary: "Import designs from Figma/Stitch/v0/Pencil",
    },
    Entry {
        name: "export-tokens",
        summary: "Export theme as W3C Design Tokens",
    },
];

/// Package-manager + legacy pseudo-entries — mirrors the trailing
/// inline entries in the C++ banner.
pub const PACKAGE_EXTRAS: &[Entry] = &[
    Entry {
        name: "audit",
        summary: "License and clean-room audit",
    },
    Entry {
        name: "add",
        summary: "Add a component to the project",
    },
    Entry {
        name: "remove",
        summary: "Remove a previously added package",
    },
    Entry {
        name: "list",
        summary: "Show installed packages",
    },
    Entry {
        name: "search",
        summary: "Search the package registry",
    },
    Entry {
        name: "update",
        summary: "Check for and apply package updates",
    },
    Entry {
        name: "suggest",
        summary: "Context-aware package recommendations",
    },
    Entry {
        name: "target",
        summary: "Manage project platform targets",
    },
    Entry {
        name: "tool",
        summary: "Manage third-party developer tools",
    },
    Entry {
        name: "help",
        summary: "Show this help",
    },
];

/// Trailing example block — matches the C++ "Examples:" trailer.
pub const EXAMPLES: &[&str] = &[
    "  pulp create MyPlugin              # Create a new effect plugin",
    "  pulp create MySynth --type instrument  # Create an instrument",
    "  pulp doctor             # Check environment for issues",
    "  pulp build              # Build all targets",
    "  pulp test               # Run all tests",
    "  pulp validate           # Validate built plugins",
    "  pulp docs index         # List available docs",
    "  pulp status             # Show project info",
];

/// Every subcommand name the CLI recognizes, in display order. Used
/// by the fuzzy suggester so it walks the full surface, not just
/// native-Rust ports.
#[must_use]
pub fn known_commands() -> Vec<&'static str> {
    let mut v = Vec::with_capacity(
        COMMANDS.len() + SCRIPT_COMMANDS.len() + BINARY_COMMANDS.len() + PACKAGE_EXTRAS.len(),
    );
    for e in COMMANDS {
        v.push(e.name);
    }
    for e in SCRIPT_COMMANDS {
        v.push(e.name);
    }
    for e in BINARY_COMMANDS {
        v.push(e.name);
    }
    // Package-manager commands handled inline in the C++ if-ladder.
    for e in PACKAGE_EXTRAS {
        v.push(e.name);
    }
    v
}

/// Write the full usage banner. Mirrors the C++ `print_usage()` byte
/// layout, including the 14-char command column and the blank line
/// between native commands and delegate commands.
///
/// # Errors
///
/// Returns [`CliError::Io`] if `out` fails a write.
pub fn write_usage(out: &mut impl Write) -> Result<()> {
    write_usage_with_banner(out, "pulp")
}

/// Like [`write_usage`] but lets callers override the banner name.
/// Tests can reuse the same table with a different header when they
/// need to compare against legacy captures.
///
/// # Errors
///
/// Returns [`CliError::Io`] if `out` fails a write.
pub fn write_usage_with_banner(out: &mut impl Write, binary: &str) -> Result<()> {
    let io = |e: std::io::Error| CliError::io("<stdout>", e);

    writeln!(out, "{binary} — Pulp audio plugin framework CLI").map_err(io)?;
    writeln!(out).map_err(io)?;
    writeln!(out, "Usage: {binary} <command> [options]").map_err(io)?;
    writeln!(out).map_err(io)?;
    writeln!(out, "Commands:").map_err(io)?;

    for e in COMMANDS {
        writeln!(out, "  {:<14} {}", e.name, e.summary).map_err(io)?;
    }
    writeln!(out).map_err(io)?;
    for e in SCRIPT_COMMANDS {
        writeln!(out, "  {:<14} {}", e.name, e.summary).map_err(io)?;
    }
    for e in BINARY_COMMANDS {
        writeln!(out, "  {:<14} {}", e.name, e.summary).map_err(io)?;
    }
    for e in PACKAGE_EXTRAS {
        writeln!(out, "  {:<14} {}", e.name, e.summary).map_err(io)?;
    }

    writeln!(out).map_err(io)?;
    writeln!(out, "Examples:").map_err(io)?;
    for line in EXAMPLES {
        writeln!(out, "{line}").map_err(io)?;
    }
    Ok(())
}

/// Levenshtein edit distance. Used by the unknown-subcommand
/// "Did you mean…?" suggester.
///
/// Matches the C++ inline implementation in `pulp_cli.cpp`. Works on
/// bytes, not codepoints — the command surface is ASCII-only so the
/// difference doesn't matter, and byte comparison keeps the loop body
/// dense.
#[must_use]
pub fn levenshtein(a: &str, b: &str) -> usize {
    let a = a.as_bytes();
    let b = b.as_bytes();
    let (m, n) = (a.len(), b.len());

    // Two-row DP — O(n) space, O(m*n) time. For a command surface of
    // ~25 entries and a typo of <10 characters that's negligible.
    let mut prev: Vec<usize> = (0..=n).collect();
    let mut curr: Vec<usize> = vec![0; n + 1];

    for i in 1..=m {
        curr[0] = i;
        for j in 1..=n {
            let cost = usize::from(a[i - 1] != b[j - 1]);
            curr[j] = (curr[j - 1] + 1).min(prev[j] + 1).min(prev[j - 1] + cost);
        }
        std::mem::swap(&mut prev, &mut curr);
    }
    prev[n]
}

/// Find the command whose name is closest in edit distance to
/// `typed`. Returns `(name, distance)` or `None` when the known list
/// is empty (never happens in production — guarded for tests).
///
/// The caller decides the "close enough" threshold. The C++ CLI uses
/// `distance <= 3`; the Rust port inherits that value in
/// [`suggest_hint`] so bare-typo invocations print the same hint.
#[must_use]
pub fn closest<'a>(typed: &str, candidates: &[&'a str]) -> Option<(&'a str, usize)> {
    candidates
        .iter()
        .map(|name| (*name, levenshtein(typed, name)))
        .min_by_key(|(_, d)| *d)
}

/// Produce the stderr hint the C++ CLI prints on unknown-subcommand.
///
/// Returns a ready-to-print multi-line string (including trailing
/// newline). Shape:
///
/// ```text
/// Unknown command: xzv
/// Did you mean: pulp build?
/// ```
///
/// When no candidate is within the distance threshold, the hint
/// collapses to:
///
/// ```text
/// Unknown command: xzv
/// Run `pulp help` for usage
/// ```
///
/// `threshold` is the inclusive distance ceiling; the C++ CLI uses
/// `3`, which this port reuses (see `pulp_cli.cpp`
/// `print_usage_banner()` / main-trampoline).
#[must_use]
pub fn suggest_hint(typed: &str, binary: &str, threshold: usize) -> String {
    use std::fmt::Write as _;
    let names = known_commands();
    let mut out = format!("Unknown command: {typed}\n");
    if let Some((name, dist)) = closest(typed, &names) {
        if dist <= threshold {
            writeln!(out, "Did you mean: {binary} {name}?").expect("writing to String");
            return out;
        }
    }
    writeln!(out, "Run `{binary} help` for usage").expect("writing to String");
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    fn assert_banner_rows(output: &str, entries: &[Entry]) {
        for e in entries {
            let row = format!("  {:<14} {}", e.name, e.summary);
            assert!(
                output.lines().any(|line| line == row),
                "banner missing row `{row}`:\n{output}"
            );
        }
    }

    #[test]
    fn usage_banner_lists_every_native_command() {
        let mut buf = Vec::new();
        write_usage(&mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert_banner_rows(&s, COMMANDS);
    }

    #[test]
    fn usage_banner_lists_every_package_extra() {
        let mut buf = Vec::new();
        write_usage(&mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert_banner_rows(&s, PACKAGE_EXTRAS);
    }

    #[test]
    fn command_tables_do_not_duplicate_names() {
        let mut seen = HashSet::new();
        for entries in [COMMANDS, SCRIPT_COMMANDS, BINARY_COMMANDS, PACKAGE_EXTRAS] {
            for e in entries {
                assert!(seen.insert(e.name), "duplicate command `{}`", e.name);
            }
        }
        assert_eq!(known_commands().len(), seen.len());
    }

    #[test]
    fn usage_banner_starts_with_pulp_header() {
        let mut buf = Vec::new();
        write_usage(&mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.starts_with("pulp — Pulp audio plugin framework CLI"));
        assert!(s.contains("Usage: pulp <command> [options]"));
    }

    #[test]
    fn usage_banner_ends_with_example_lines() {
        let mut buf = Vec::new();
        write_usage(&mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("Examples:"));
        assert!(s.contains("pulp create MyPlugin"));
        assert!(s.contains("pulp status"));
    }

    #[test]
    fn levenshtein_handles_edge_cases() {
        assert_eq!(levenshtein("", ""), 0);
        assert_eq!(levenshtein("", "abc"), 3);
        assert_eq!(levenshtein("abc", ""), 3);
        assert_eq!(levenshtein("abc", "abc"), 0);
        assert_eq!(levenshtein("kitten", "sitting"), 3);
        assert_eq!(levenshtein("buld", "build"), 1);
    }

    #[test]
    fn closest_picks_known_command() {
        let cands = known_commands();
        let (name, dist) = closest("buld", &cands).unwrap();
        assert_eq!(name, "build");
        assert_eq!(dist, 1);
    }

    #[test]
    fn suggest_hint_prints_did_you_mean_within_threshold() {
        let h = suggest_hint("buld", "pulp", 3);
        assert!(h.contains("Unknown command: buld"));
        assert!(h.contains("Did you mean: pulp build?"));
    }

    #[test]
    fn suggest_hint_falls_back_when_far_off() {
        // "xyzxyzxyz" is at distance >3 from every known command.
        let h = suggest_hint("xyzxyzxyz", "pulp", 3);
        assert!(h.contains("Unknown command: xyzxyzxyz"));
        assert!(h.contains("Run `pulp help` for usage"));
    }

    #[test]
    fn known_commands_contains_help_and_package_surface() {
        let c = known_commands();
        for name in [
            "help", "add", "remove", "list", "search", "update", "suggest", "target", "audit",
            "tool",
        ] {
            assert!(c.contains(&name), "known_commands missing `{name}`");
        }
    }
}
