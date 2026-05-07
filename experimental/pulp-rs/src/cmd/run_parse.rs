//! Pure flag parser for `pulp-rs run`. Direct port of
//! `tools/cli/cmd_run_parse.cpp` from the C++ side (Pulp #914 / PR #917).
//!
//! Kept dependency-free so unit tests don't need to drag in the
//! orchestrator or its filesystem helpers — same split the C++ side
//! uses (parser is in its own translation unit so test_cli_run_options
//! doesn't link cli_common).
//!
//! # Surface ported from #914
//!
//! - `--headless` — render offscreen. Forwarded as `--headless` arg
//!   AND `PULP_HEADLESS=1` env var so binaries that only read one
//!   source pick it up.
//! - `--screenshot <path>` (also `--screenshot=path`) — implies
//!   `--headless`. Forwarded as arg + `PULP_SCREENSHOT=<path>`.
//! - `--frames <n>` (also `--frames=n`) — must be > 0. Forwarded as
//!   arg + `PULP_FRAMES=<n>` when not the default of 1.
//! - `--watch` — re-launch the binary on source changes. Consumed by
//!   the CLI; NOT forwarded.
//! - `--` — everything after this is `user_pass_through` (verbatim
//!   forwarding to the launched binary).
//!
//! Anything else that begins with `-` and isn't recognised becomes
//! `user_pass_through` (legacy permissive behaviour matching the C++
//! parser).

/// Parsed result of a `pulp-rs run` invocation. Mirrors C++
/// `pulp_cli::ParseRunResult` field-for-field.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct RunOptions {
    /// `--help` / `-h` was requested.
    pub help: bool,
    /// Non-empty on parse failure (fails fast before project
    /// resolution, matches C++ exit-2 contract).
    pub error: String,
    /// Optional positional target name.
    pub target_name: String,

    // #914 flags
    /// `--headless` (or implied by `--screenshot`).
    pub headless: bool,
    /// `--screenshot <path>` (path is empty when not requested).
    pub screenshot_path: String,
    /// `--frames <n>` — defaults to 1.
    pub frames: i32,
    /// `--watch` — re-launch on source change.
    pub watch: bool,

    /// Args explicitly forwarded by the user with `-- ...`, plus any
    /// unknown flags (legacy permissive behaviour).
    pub user_pass_through: Vec<String>,
}

impl RunOptions {
    /// Construct with `frames = 1` to match the C++ default.
    #[must_use]
    pub fn new() -> Self {
        Self {
            frames: 1,
            ..Self::default()
        }
    }
}

/// Parse `pulp-rs run` arguments. Pure — no side effects, no I/O.
///
/// Direct port of `pulp_cli::parse_run_options` in
/// `tools/cli/cmd_run_parse.cpp`. The order of flag handling matches
/// the C++ side so any future change there can be ported as a
/// line-for-line patch.
#[must_use]
pub fn parse_run_options(args: &[String]) -> RunOptions {
    let mut r = RunOptions::new();
    let mut after_separator = false;
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];

        if a == "--help" || a == "-h" {
            r.help = true;
            return r;
        }

        if a == "--" {
            after_separator = true;
            i += 1;
            continue;
        }

        if after_separator {
            r.user_pass_through.push(a.clone());
            i += 1;
            continue;
        }

        if a == "--headless" {
            r.headless = true;
            i += 1;
            continue;
        }
        if a == "--screenshot" {
            r.headless = true; // --screenshot implies --headless
            if i + 1 < args.len() && !args[i + 1].is_empty() && !args[i + 1].starts_with('-') {
                r.screenshot_path = args[i + 1].clone();
                i += 2;
                continue;
            }
            r.error = "--screenshot requires a path argument".to_owned();
            return r;
        }
        if let Some(rest) = a.strip_prefix("--screenshot=") {
            r.headless = true;
            if rest.is_empty() {
                r.error = "--screenshot= requires a non-empty path".to_owned();
                return r;
            }
            r.screenshot_path = rest.to_owned();
            i += 1;
            continue;
        }
        if a == "--frames" {
            if i + 1 < args.len() {
                match args[i + 1].parse::<i32>() {
                    Ok(n) if n > 0 => {
                        r.frames = n;
                        i += 2;
                        continue;
                    }
                    Ok(_) => {
                        r.error = "--frames must be > 0".to_owned();
                        return r;
                    }
                    Err(_) => {
                        r.error = "--frames requires an integer argument".to_owned();
                        return r;
                    }
                }
            }
            r.error = "--frames requires an integer argument".to_owned();
            return r;
        }
        if let Some(rest) = a.strip_prefix("--frames=") {
            match rest.parse::<i32>() {
                Ok(n) if n > 0 => {
                    r.frames = n;
                    i += 1;
                    continue;
                }
                Ok(_) => {
                    r.error = "--frames must be > 0".to_owned();
                    return r;
                }
                Err(_) => {
                    r.error = "--frames= requires an integer".to_owned();
                    return r;
                }
            }
        }
        if a == "--watch" {
            r.watch = true;
            i += 1;
            continue;
        }

        if r.target_name.is_empty() && !a.is_empty() && !a.starts_with('-') {
            r.target_name = a.clone();
            i += 1;
            continue;
        }

        r.user_pass_through.push(a.clone());
        i += 1;
    }

    r
}

/// Build the argv that gets passed to the launched standalone binary.
/// Order: `--headless` (if set), `--screenshot <path>` (if set),
/// `--frames <n>` (if not default), then `user_pass_through` verbatim.
///
/// Direct port of `pulp_cli::assemble_launch_args`.
#[must_use]
pub fn assemble_launch_args(opts: &RunOptions) -> Vec<String> {
    let mut out = Vec::new();
    if opts.headless {
        out.push("--headless".to_owned());
    }
    if !opts.screenshot_path.is_empty() {
        out.push("--screenshot".to_owned());
        out.push(opts.screenshot_path.clone());
    }
    if opts.frames != 1 {
        out.push("--frames".to_owned());
        out.push(opts.frames.to_string());
    }
    for a in &opts.user_pass_through {
        out.push(a.clone());
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn argv(parts: &[&str]) -> Vec<String> {
        parts.iter().map(|s| (*s).to_owned()).collect()
    }

    #[test]
    fn parse_default_is_empty_with_frames_1() {
        let r = parse_run_options(&[]);
        assert!(!r.help);
        assert!(r.error.is_empty());
        assert!(!r.headless);
        assert!(r.screenshot_path.is_empty());
        assert_eq!(r.frames, 1);
        assert!(!r.watch);
        assert!(r.target_name.is_empty());
        assert!(r.user_pass_through.is_empty());
    }

    #[test]
    fn parse_help_short_and_long_set_help() {
        for flag in &["--help", "-h"] {
            let r = parse_run_options(&argv(&[flag]));
            assert!(r.help, "{flag} should set help");
        }
    }

    #[test]
    fn parse_headless_alone_sets_flag() {
        let r = parse_run_options(&argv(&["--headless"]));
        assert!(r.headless);
        assert!(r.screenshot_path.is_empty());
    }

    #[test]
    fn parse_screenshot_implies_headless_and_captures_path() {
        let r = parse_run_options(&argv(&["--screenshot", "out.png"]));
        assert!(r.headless, "--screenshot must imply --headless");
        assert_eq!(r.screenshot_path, "out.png");
    }

    #[test]
    fn parse_screenshot_eq_form_works() {
        let r = parse_run_options(&argv(&["--screenshot=out.png"]));
        assert!(r.headless);
        assert_eq!(r.screenshot_path, "out.png");
    }

    #[test]
    fn parse_screenshot_without_path_errors() {
        let r = parse_run_options(&argv(&["--screenshot"]));
        assert!(r.error.contains("--screenshot requires a path"));
    }

    #[test]
    fn parse_screenshot_followed_by_flag_errors() {
        // The next arg starts with `-`, so the parser treats the path as missing.
        let r = parse_run_options(&argv(&["--screenshot", "--frames"]));
        assert!(r.error.contains("--screenshot requires a path"));
    }

    #[test]
    fn parse_screenshot_eq_empty_errors() {
        let r = parse_run_options(&argv(&["--screenshot="]));
        assert!(r.error.contains("--screenshot= requires"));
    }

    #[test]
    fn parse_frames_captures_int() {
        let r = parse_run_options(&argv(&["--frames", "30"]));
        assert_eq!(r.frames, 30);
        assert!(r.error.is_empty());
    }

    #[test]
    fn parse_frames_eq_form_works() {
        let r = parse_run_options(&argv(&["--frames=30"]));
        assert_eq!(r.frames, 30);
    }

    #[test]
    fn parse_frames_zero_or_negative_errors() {
        let r0 = parse_run_options(&argv(&["--frames", "0"]));
        assert!(r0.error.contains("--frames must be > 0"));
        let rneg = parse_run_options(&argv(&["--frames", "-5"]));
        assert!(rneg.error.contains("--frames must be > 0"));
    }

    #[test]
    fn parse_frames_non_integer_errors() {
        let r = parse_run_options(&argv(&["--frames", "abc"]));
        assert!(r.error.contains("--frames requires an integer"));
    }

    #[test]
    fn parse_frames_eq_non_integer_errors() {
        let r = parse_run_options(&argv(&["--frames=abc"]));
        assert!(r.error.contains("--frames= requires an integer"));
    }

    #[test]
    fn parse_watch_sets_flag_and_is_not_in_pass_through() {
        let r = parse_run_options(&argv(&["--watch"]));
        assert!(r.watch);
        assert!(r.user_pass_through.is_empty());
    }

    #[test]
    fn parse_target_first_non_flag_wins() {
        let r = parse_run_options(&argv(&["my-app", "another"]));
        assert_eq!(r.target_name, "my-app");
        assert_eq!(r.user_pass_through, vec!["another".to_owned()]);
    }

    #[test]
    fn parse_separator_forwards_everything_after_verbatim() {
        let r = parse_run_options(&argv(&[
            "--headless", "--", "--frames", "999", "--watch",
        ]));
        assert!(r.headless);
        assert_eq!(r.frames, 1, "--frames after `--` is NOT consumed");
        assert!(!r.watch, "--watch after `--` is NOT consumed");
        assert_eq!(
            r.user_pass_through,
            vec!["--frames".to_owned(), "999".to_owned(), "--watch".to_owned()]
        );
    }

    #[test]
    fn parse_unknown_flag_lands_in_pass_through() {
        let r = parse_run_options(&argv(&["--unknown-flag"]));
        assert!(r.error.is_empty());
        assert_eq!(r.user_pass_through, vec!["--unknown-flag".to_owned()]);
    }

    #[test]
    fn parse_full_combo() {
        let r = parse_run_options(&argv(&[
            "MyTarget",
            "--headless",
            "--screenshot",
            "out.png",
            "--frames",
            "60",
            "--watch",
            "--",
            "--child-flag",
        ]));
        assert_eq!(r.target_name, "MyTarget");
        assert!(r.headless);
        assert_eq!(r.screenshot_path, "out.png");
        assert_eq!(r.frames, 60);
        assert!(r.watch);
        assert_eq!(r.user_pass_through, vec!["--child-flag".to_owned()]);
        assert!(r.error.is_empty());
    }

    #[test]
    fn assemble_default_is_empty() {
        let opts = RunOptions::new();
        assert!(assemble_launch_args(&opts).is_empty());
    }

    #[test]
    fn assemble_headless_only_emits_headless() {
        let mut opts = RunOptions::new();
        opts.headless = true;
        assert_eq!(assemble_launch_args(&opts), vec!["--headless".to_owned()]);
    }

    #[test]
    fn assemble_screenshot_emits_pair() {
        let mut opts = RunOptions::new();
        opts.headless = true;
        opts.screenshot_path = "out.png".to_owned();
        assert_eq!(
            assemble_launch_args(&opts),
            vec![
                "--headless".to_owned(),
                "--screenshot".to_owned(),
                "out.png".to_owned()
            ]
        );
    }

    #[test]
    fn assemble_frames_only_when_not_default() {
        let mut opts = RunOptions::new();
        // frames=1 (default) — NOT emitted
        assert!(assemble_launch_args(&opts).is_empty());
        opts.frames = 60;
        assert_eq!(
            assemble_launch_args(&opts),
            vec!["--frames".to_owned(), "60".to_owned()]
        );
    }

    #[test]
    fn assemble_pass_through_appended_verbatim() {
        let mut opts = RunOptions::new();
        opts.user_pass_through = vec!["--child-flag".to_owned(), "val".to_owned()];
        assert_eq!(
            assemble_launch_args(&opts),
            vec!["--child-flag".to_owned(), "val".to_owned()]
        );
    }

    #[test]
    fn assemble_full_combo_order_matches_cpp() {
        let mut opts = RunOptions::new();
        opts.headless = true;
        opts.screenshot_path = "ui.png".to_owned();
        opts.frames = 30;
        opts.user_pass_through = vec!["--child".to_owned()];
        assert_eq!(
            assemble_launch_args(&opts),
            vec![
                "--headless".to_owned(),
                "--screenshot".to_owned(),
                "ui.png".to_owned(),
                "--frames".to_owned(),
                "30".to_owned(),
                "--child".to_owned()
            ]
        );
    }
}
