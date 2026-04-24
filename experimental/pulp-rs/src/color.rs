//! Terminal color helpers with the same disable policy as the C++ CLI.
//!
//! # Contract
//!
//! A returned escape is either the real ANSI sequence or the empty
//! string. Callers can always `write!(…, "{}msg{}", yellow(), reset())`
//! without worrying about whether the writer is a TTY.
//!
//! # Disable rules (match `cli_common.cpp::init_color()`)
//!
//! 1. `NO_COLOR` env var set (any value, [no-color.org]) → disabled.
//! 2. `stdout` is not a TTY → disabled.
//! 3. Otherwise → enabled.
//!
//! [no-color.org]: https://no-color.org
//!
//! # Why not a dependency
//!
//! `termcolor` / `owo-colors` are great, but for four distinct
//! attributes the dependency is overkill for a prototype whose goal
//! is to argue for replacing a C++ module.

use std::io::{self, IsTerminal};
use std::sync::OnceLock;

/// Cached "should we emit color?" decision.
///
/// Computed once on first access — subsequent calls are a plain atomic
/// load, so the hot-path cost of a colour escape is negligible.
fn enabled() -> bool {
    static CACHE: OnceLock<bool> = OnceLock::new();
    *CACHE.get_or_init(|| {
        if std::env::var_os("NO_COLOR").is_some() {
            return false;
        }
        io::stdout().is_terminal()
    })
}

/// `\x1b[0m` or empty string.
#[must_use]
pub fn reset() -> &'static str {
    if enabled() {
        "\x1b[0m"
    } else {
        ""
    }
}

/// `\x1b[2m` (dim) or empty string.
#[must_use]
pub fn dim() -> &'static str {
    if enabled() {
        "\x1b[2m"
    } else {
        ""
    }
}

/// `\x1b[33m` (yellow) or empty string.
#[must_use]
pub fn yellow() -> &'static str {
    if enabled() {
        "\x1b[33m"
    } else {
        ""
    }
}

/// `\x1b[32m` (green) or empty string.
#[must_use]
pub fn green() -> &'static str {
    if enabled() {
        "\x1b[32m"
    } else {
        ""
    }
}

/// `\x1b[31m` (red) or empty string.
///
/// Added in Phase 6b so the `project bump` / `project undo` failure
/// paths can colour "failed" the same red as the C++ CLI.
#[must_use]
pub fn red() -> &'static str {
    if enabled() {
        "\x1b[31m"
    } else {
        ""
    }
}

/// `\x1b[36m` (cyan) or empty string.
///
/// Added in Phase 6b for `project bump --dry-run` "would bump"
/// markers — matches the C++ report colour.
#[must_use]
pub fn cyan() -> &'static str {
    if enabled() {
        "\x1b[36m"
    } else {
        ""
    }
}

/// `\x1b[1m` (bold) or empty string.
///
/// Added in Phase 6b for the `pulp project bump` report header.
#[must_use]
pub fn bold() -> &'static str {
    if enabled() {
        "\x1b[1m"
    } else {
        ""
    }
}
