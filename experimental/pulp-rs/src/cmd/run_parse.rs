//! Pure flag parser for `pulp-rs run`. Mirrors
//! `tools/cli/cmd_run_parse.cpp` from the C++ side.
//!
//! Kept dependency-free so unit tests don't need to drag in the
//! orchestrator or its filesystem helpers — same split the C++ side
//! uses (parser is in its own translation unit so test_cli_run_options
//! doesn't link cli_common).
//!
//! # Supported surface
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
//! - `--audio-inspector` — forwarded as `--audio-inspector` and
//!   `PULP_AUDIO_INSPECTOR=1`.
//! - `--audio-probe-json <path>` — implies `--headless`, forwarded as
//!   argv plus `PULP_AUDIO_PROBE_JSON=<path>`.
//! - `--audio-scope-json <path>` plus `--audio-scope-window`,
//!   `--audio-scope-trigger`, and `--audio-scope-channel` — forwarded
//!   as argv plus matching `PULP_AUDIO_SCOPE_*` env vars.
//! - `--audio-capture-wav <path>` plus `--audio-capture-frames <n>` —
//!   implies `--headless`, forwarded as argv plus matching
//!   `PULP_AUDIO_CAPTURE_WAV*` env vars.
//! - `--` — everything after this is `user_pass_through` (verbatim
//!   forwarding to the launched binary).
//!
//! Anything else that begins with `-` and isn't recognized becomes
//! `user_pass_through` (legacy permissive behavior matching the C++
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

    // Headless-render flags.
    /// `--headless` (or implied by `--screenshot`).
    pub headless: bool,
    /// `--screenshot <path>` (path is empty when not requested).
    pub screenshot_path: String,
    /// `--frames <n>` — defaults to 1.
    pub frames: i32,
    /// `--watch` — re-launch on source change.
    pub watch: bool,
    /// `--audio-inspector` — open the live Audio Inspector window.
    pub audio_inspector: bool,
    /// `--audio-probe-json <path>` — live probe one-shot JSON.
    pub audio_probe_json_path: String,
    /// `--audio-scope-json <path>` — live Audio Scope one-shot JSON.
    pub audio_scope_json_path: String,
    /// `--audio-scope-window <samples>`.
    pub audio_scope_window: i32,
    /// `--audio-scope-trigger <mode>`.
    pub audio_scope_trigger: String,
    /// `--audio-scope-channel <index>`.
    pub audio_scope_channel: i32,
    /// `--audio-capture-wav <path>` — live output ring WAV dump.
    pub audio_capture_wav_path: String,
    /// `--audio-capture-frames <samples>`.
    pub audio_capture_frames: i32,

    /// Args explicitly forwarded by the user with `-- ...`, plus any
    /// unknown flags (legacy permissive behavior).
    pub user_pass_through: Vec<String>,
}

impl RunOptions {
    /// Construct with `frames = 1` to match the C++ default.
    #[must_use]
    pub fn new() -> Self {
        Self {
            frames: 1,
            audio_scope_window: 2048,
            audio_scope_trigger: "rising-zero".to_owned(),
            ..Self::default()
        }
    }
}

fn parse_positive_i32(value: &str) -> Option<i32> {
    if value.is_empty() || value.starts_with('+') {
        return None;
    }
    value.parse::<i32>().ok().filter(|n| *n > 0)
}

fn parses_i32_without_plus(value: &str) -> bool {
    !value.is_empty() && !value.starts_with('+') && value.parse::<i32>().is_ok()
}

fn parse_nonnegative_i32(value: &str) -> Option<i32> {
    if value.is_empty() || value.starts_with('+') {
        return None;
    }
    value.parse::<i32>().ok().filter(|n| *n >= 0)
}

fn valid_scope_trigger(value: &str) -> bool {
    let normalized = value.replace('_', "-");
    matches!(normalized.as_str(), "none" | "off" | "raw" | "rising-zero")
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
    let mut audio_scope_acquisition_option_seen = false;
    let mut audio_capture_frames_option_seen = false;
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
                if let Some(n) = parse_positive_i32(&args[i + 1]) {
                    r.frames = n;
                    i += 2;
                    continue;
                }
                r.error = if parses_i32_without_plus(&args[i + 1]) {
                    "--frames must be > 0".to_owned()
                } else {
                    "--frames requires an integer argument".to_owned()
                };
                return r;
            }
            r.error = "--frames requires an integer argument".to_owned();
            return r;
        }
        if let Some(rest) = a.strip_prefix("--frames=") {
            if let Some(n) = parse_positive_i32(rest) {
                r.frames = n;
                i += 1;
                continue;
            }
            r.error = if parses_i32_without_plus(rest) {
                "--frames must be > 0".to_owned()
            } else {
                "--frames= requires an integer".to_owned()
            };
            return r;
        }
        if a == "--watch" {
            r.watch = true;
            i += 1;
            continue;
        }
        if a == "--audio-inspector" {
            r.audio_inspector = true;
            i += 1;
            continue;
        }
        if a == "--audio-probe-json" {
            r.headless = true;
            if i + 1 < args.len() && !args[i + 1].is_empty() && !args[i + 1].starts_with('-') {
                r.audio_probe_json_path = args[i + 1].clone();
                i += 2;
                continue;
            }
            r.error = "--audio-probe-json requires a path argument".to_owned();
            return r;
        }
        if let Some(rest) = a.strip_prefix("--audio-probe-json=") {
            r.headless = true;
            if rest.is_empty() {
                r.error = "--audio-probe-json= requires a non-empty path".to_owned();
                return r;
            }
            r.audio_probe_json_path = rest.to_owned();
            i += 1;
            continue;
        }
        if a == "--audio-scope-json" {
            r.headless = true;
            if i + 1 < args.len() && !args[i + 1].is_empty() && !args[i + 1].starts_with('-') {
                r.audio_scope_json_path = args[i + 1].clone();
                i += 2;
                continue;
            }
            r.error = "--audio-scope-json requires a path argument".to_owned();
            return r;
        }
        if let Some(rest) = a.strip_prefix("--audio-scope-json=") {
            r.headless = true;
            if rest.is_empty() {
                r.error = "--audio-scope-json= requires a non-empty path".to_owned();
                return r;
            }
            r.audio_scope_json_path = rest.to_owned();
            i += 1;
            continue;
        }
        if a == "--audio-scope-window" {
            audio_scope_acquisition_option_seen = true;
            if i + 1 < args.len() {
                if let Some(n) = parse_positive_i32(&args[i + 1]) {
                    r.audio_scope_window = n;
                    i += 2;
                    continue;
                }
                r.error = if parses_i32_without_plus(&args[i + 1]) {
                    "--audio-scope-window must be > 0".to_owned()
                } else {
                    "--audio-scope-window requires an integer argument".to_owned()
                };
                return r;
            }
            r.error = "--audio-scope-window requires an integer argument".to_owned();
            return r;
        }
        if let Some(rest) = a.strip_prefix("--audio-scope-window=") {
            audio_scope_acquisition_option_seen = true;
            if let Some(n) = parse_positive_i32(rest) {
                r.audio_scope_window = n;
                i += 1;
                continue;
            }
            r.error = if parses_i32_without_plus(rest) {
                "--audio-scope-window must be > 0".to_owned()
            } else {
                "--audio-scope-window= requires an integer".to_owned()
            };
            return r;
        }
        if a == "--audio-scope-trigger" {
            audio_scope_acquisition_option_seen = true;
            if i + 1 >= args.len() || args[i + 1].is_empty() || args[i + 1].starts_with('-') {
                r.error = "--audio-scope-trigger requires a value".to_owned();
                return r;
            }
            r.audio_scope_trigger = args[i + 1].clone();
            if !valid_scope_trigger(&r.audio_scope_trigger) {
                r.error =
                    "--audio-scope-trigger must be one of none, raw, off, rising-zero".to_owned();
                return r;
            }
            i += 2;
            continue;
        }
        if let Some(rest) = a.strip_prefix("--audio-scope-trigger=") {
            audio_scope_acquisition_option_seen = true;
            if rest.is_empty() {
                r.error = "--audio-scope-trigger= requires a non-empty value".to_owned();
                return r;
            }
            r.audio_scope_trigger = rest.to_owned();
            if !valid_scope_trigger(&r.audio_scope_trigger) {
                r.error =
                    "--audio-scope-trigger must be one of none, raw, off, rising-zero".to_owned();
                return r;
            }
            i += 1;
            continue;
        }
        if a == "--audio-scope-channel" {
            audio_scope_acquisition_option_seen = true;
            if i + 1 < args.len() {
                if let Some(n) = parse_nonnegative_i32(&args[i + 1]) {
                    r.audio_scope_channel = n;
                    i += 2;
                    continue;
                }
            }
            r.error = "--audio-scope-channel requires a non-negative integer".to_owned();
            return r;
        }
        if let Some(rest) = a.strip_prefix("--audio-scope-channel=") {
            audio_scope_acquisition_option_seen = true;
            if let Some(n) = parse_nonnegative_i32(rest) {
                r.audio_scope_channel = n;
                i += 1;
                continue;
            }
            r.error = "--audio-scope-channel requires a non-negative integer".to_owned();
            return r;
        }

        if a == "--audio-capture-wav" {
            if i + 1 < args.len() && !args[i + 1].is_empty() {
                r.audio_capture_wav_path = args[i + 1].clone();
                r.headless = true;
                i += 2;
                continue;
            }
            r.error = "--audio-capture-wav requires a path argument".to_owned();
            return r;
        }
        if let Some(rest) = a.strip_prefix("--audio-capture-wav=") {
            if rest.is_empty() {
                r.error = "--audio-capture-wav= requires a non-empty path".to_owned();
                return r;
            }
            r.audio_capture_wav_path = rest.to_owned();
            r.headless = true;
            i += 1;
            continue;
        }
        if a == "--audio-capture-frames" {
            audio_capture_frames_option_seen = true;
            if i + 1 < args.len() {
                if let Some(n) = parse_positive_i32(&args[i + 1]) {
                    r.audio_capture_frames = n;
                    i += 2;
                    continue;
                }
            }
            r.error = "--audio-capture-frames requires a positive integer".to_owned();
            return r;
        }
        if let Some(rest) = a.strip_prefix("--audio-capture-frames=") {
            audio_capture_frames_option_seen = true;
            if let Some(n) = parse_positive_i32(rest) {
                r.audio_capture_frames = n;
                i += 1;
                continue;
            }
            r.error = "--audio-capture-frames requires a positive integer".to_owned();
            return r;
        }

        if r.target_name.is_empty() && !a.is_empty() && !a.starts_with('-') {
            r.target_name = a.clone();
            i += 1;
            continue;
        }

        r.user_pass_through.push(a.clone());
        i += 1;
    }

    if !r.audio_scope_json_path.is_empty() && r.audio_inspector {
        r.error = "--audio-scope-json cannot be combined with --audio-inspector; both consume the live capture FIFO".to_owned();
    }
    if audio_scope_acquisition_option_seen && r.audio_scope_json_path.is_empty() {
        r.error = "--audio-scope-window, --audio-scope-trigger, and --audio-scope-channel require --audio-scope-json".to_owned();
    }
    if !r.audio_capture_wav_path.is_empty()
        && (r.audio_inspector || !r.audio_scope_json_path.is_empty())
    {
        r.error = "--audio-capture-wav cannot be combined with --audio-inspector or --audio-scope-json; they share the one live capture FIFO".to_owned();
    }
    if audio_capture_frames_option_seen && r.audio_capture_wav_path.is_empty() {
        r.error = "--audio-capture-frames requires --audio-capture-wav".to_owned();
    }

    r
}

/// Build the argv that gets passed to the launched standalone binary.
/// Order mirrors the C++ parser: render flags, live-audio flags, then
/// `user_pass_through` verbatim.
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
    if opts.audio_inspector {
        out.push("--audio-inspector".to_owned());
    }
    if !opts.audio_probe_json_path.is_empty() {
        out.push("--audio-probe-json".to_owned());
        out.push(opts.audio_probe_json_path.clone());
    }
    if !opts.audio_scope_json_path.is_empty() {
        out.push("--audio-scope-json".to_owned());
        out.push(opts.audio_scope_json_path.clone());
        out.push("--audio-scope-window".to_owned());
        out.push(opts.audio_scope_window.to_string());
        out.push("--audio-scope-trigger".to_owned());
        out.push(opts.audio_scope_trigger.clone());
        out.push("--audio-scope-channel".to_owned());
        out.push(opts.audio_scope_channel.to_string());
    }
    if !opts.audio_capture_wav_path.is_empty() {
        out.push("--audio-capture-wav".to_owned());
        out.push(opts.audio_capture_wav_path.clone());
        if opts.audio_capture_frames > 0 {
            out.push("--audio-capture-frames".to_owned());
            out.push(opts.audio_capture_frames.to_string());
        }
    }
    for a in &opts.user_pass_through {
        out.push(a.clone());
    }
    out
}

/// Build the environment variables exported to the launched standalone binary.
///
/// Kept next to [`assemble_launch_args`] so future launcher flags update argv
/// and env forwarding together.
#[must_use]
pub fn assemble_launch_env(opts: &RunOptions) -> Vec<(String, String)> {
    let mut out = Vec::new();
    if opts.headless {
        out.push(("PULP_HEADLESS".to_owned(), "1".to_owned()));
    }
    if !opts.screenshot_path.is_empty() {
        out.push(("PULP_SCREENSHOT".to_owned(), opts.screenshot_path.clone()));
    }
    if opts.frames != 1 {
        out.push(("PULP_FRAMES".to_owned(), opts.frames.to_string()));
    }
    if opts.audio_inspector {
        out.push(("PULP_AUDIO_INSPECTOR".to_owned(), "1".to_owned()));
    }
    if !opts.audio_probe_json_path.is_empty() {
        out.push((
            "PULP_AUDIO_PROBE_JSON".to_owned(),
            opts.audio_probe_json_path.clone(),
        ));
    }
    if !opts.audio_scope_json_path.is_empty() {
        out.push((
            "PULP_AUDIO_SCOPE_JSON".to_owned(),
            opts.audio_scope_json_path.clone(),
        ));
        out.push((
            "PULP_AUDIO_SCOPE_WINDOW".to_owned(),
            opts.audio_scope_window.to_string(),
        ));
        out.push((
            "PULP_AUDIO_SCOPE_TRIGGER".to_owned(),
            opts.audio_scope_trigger.clone(),
        ));
        out.push((
            "PULP_AUDIO_SCOPE_CHANNEL".to_owned(),
            opts.audio_scope_channel.to_string(),
        ));
    }
    if !opts.audio_capture_wav_path.is_empty() {
        out.push((
            "PULP_AUDIO_CAPTURE_WAV".to_owned(),
            opts.audio_capture_wav_path.clone(),
        ));
        if opts.audio_capture_frames > 0 {
            out.push((
                "PULP_AUDIO_CAPTURE_WAV_FRAMES".to_owned(),
                opts.audio_capture_frames.to_string(),
            ));
        }
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
        assert!(r.audio_capture_wav_path.is_empty());
        assert_eq!(r.audio_capture_frames, 0);
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
    fn parse_frames_rejects_plus_prefixed_values() {
        assert!(parse_run_options(&argv(&["--frames", "+5"]))
            .error
            .contains("requires an integer"));
        assert!(parse_run_options(&argv(&["--frames=+5"]))
            .error
            .contains("--frames= requires an integer"));
    }

    #[test]
    fn parse_watch_sets_flag_and_is_not_in_pass_through() {
        let r = parse_run_options(&argv(&["--watch"]));
        assert!(r.watch);
        assert!(r.user_pass_through.is_empty());
    }

    #[test]
    fn parse_audio_inspector_sets_flag() {
        let r = parse_run_options(&argv(&["--audio-inspector"]));
        assert!(r.error.is_empty());
        assert!(r.audio_inspector);
        assert!(!r.headless);
        assert!(r.user_pass_through.is_empty());
    }

    #[test]
    fn parse_audio_probe_json_implies_headless_and_captures_path() {
        let r = parse_run_options(&argv(&["--audio-probe-json", "probe.json"]));
        assert!(r.error.is_empty());
        assert!(r.headless);
        assert_eq!(r.audio_probe_json_path, "probe.json");
        assert!(r.target_name.is_empty());

        let eq = parse_run_options(&argv(&["--audio-probe-json=probe.json"]));
        assert!(eq.headless);
        assert_eq!(eq.audio_probe_json_path, "probe.json");
    }

    #[test]
    fn parse_audio_probe_json_requires_path() {
        assert!(parse_run_options(&argv(&["--audio-probe-json"]))
            .error
            .contains("--audio-probe-json requires a path"));
        assert!(parse_run_options(&argv(&["--audio-probe-json="]))
            .error
            .contains("--audio-probe-json= requires"));
    }

    #[test]
    fn parse_audio_scope_json_accepts_acquisition_options() {
        let r = parse_run_options(&argv(&[
            "--audio-scope-json",
            "scope.json",
            "--audio-scope-window",
            "4096",
            "--audio-scope-trigger=rising_zero",
            "--audio-scope-channel",
            "2",
        ]));
        assert!(r.error.is_empty());
        assert!(r.headless);
        assert_eq!(r.audio_scope_json_path, "scope.json");
        assert_eq!(r.audio_scope_window, 4096);
        assert_eq!(r.audio_scope_trigger, "rising_zero");
        assert_eq!(r.audio_scope_channel, 2);
        assert!(r.target_name.is_empty());
    }

    #[test]
    fn parse_audio_scope_json_rejects_invalid_combinations() {
        let missing_scope = parse_run_options(&argv(&["--audio-scope-window", "128"]));
        assert!(missing_scope.error.contains("require --audio-scope-json"));

        let conflict = parse_run_options(&argv(&[
            "--audio-inspector",
            "--audio-scope-json",
            "scope.json",
        ]));
        assert!(conflict.error.contains("cannot be combined"));

        let bad_trigger = parse_run_options(&argv(&[
            "--audio-scope-json",
            "scope.json",
            "--audio-scope-trigger",
            "bogus",
        ]));
        assert!(bad_trigger.error.contains("--audio-scope-trigger"));

        let bad_channel = parse_run_options(&argv(&[
            "--audio-scope-json=scope.json",
            "--audio-scope-channel=-1",
        ]));
        assert!(bad_channel.error.contains("--audio-scope-channel"));

        let plus_window = parse_run_options(&argv(&[
            "--audio-scope-json=scope.json",
            "--audio-scope-window=+128",
        ]));
        assert!(plus_window.error.contains("requires an integer"));
    }

    #[test]
    fn parse_audio_capture_wav_implies_headless_and_captures_path() {
        let r = parse_run_options(&argv(&[
            "--audio-capture-wav",
            "capture.wav",
            "--audio-capture-frames",
            "1024",
        ]));
        assert!(r.error.is_empty());
        assert!(r.headless);
        assert_eq!(r.audio_capture_wav_path, "capture.wav");
        assert_eq!(r.audio_capture_frames, 1024);
        assert!(r.target_name.is_empty());

        let eq = parse_run_options(&argv(&["--audio-capture-wav=capture2.wav"]));
        assert!(eq.error.is_empty());
        assert!(eq.headless);
        assert_eq!(eq.audio_capture_wav_path, "capture2.wav");
        assert_eq!(eq.audio_capture_frames, 0);
    }

    #[test]
    fn parse_audio_capture_wav_rejects_invalid_combinations() {
        assert!(parse_run_options(&argv(&["--audio-capture-wav"]))
            .error
            .contains("--audio-capture-wav requires a path"));
        assert!(parse_run_options(&argv(&["--audio-capture-wav="]))
            .error
            .contains("--audio-capture-wav= requires"));
        assert!(parse_run_options(&argv(&["--audio-capture-frames", "512"]))
            .error
            .contains("requires --audio-capture-wav"));
        assert!(parse_run_options(&argv(&[
            "--audio-capture-wav",
            "capture.wav",
            "--audio-capture-frames",
            "0",
        ]))
        .error
        .contains("requires a positive integer"));
        assert!(parse_run_options(&argv(&[
            "--audio-capture-wav",
            "capture.wav",
            "--audio-capture-frames=+128",
        ]))
        .error
        .contains("requires a positive integer"));
        assert!(parse_run_options(&argv(&[
            "--audio-inspector",
            "--audio-capture-wav",
            "capture.wav",
        ]))
        .error
        .contains("cannot be combined"));
        assert!(parse_run_options(&argv(&[
            "--audio-scope-json",
            "scope.json",
            "--audio-capture-wav",
            "capture.wav",
        ]))
        .error
        .contains("cannot be combined"));
    }

    #[test]
    fn parse_target_first_non_flag_wins() {
        let r = parse_run_options(&argv(&["my-app", "another"]));
        assert_eq!(r.target_name, "my-app");
        assert_eq!(r.user_pass_through, vec!["another".to_owned()]);
    }

    #[test]
    fn parse_separator_forwards_everything_after_verbatim() {
        let r = parse_run_options(&argv(&["--headless", "--", "--frames", "999", "--watch"]));
        assert!(r.headless);
        assert_eq!(r.frames, 1, "--frames after `--` is NOT consumed");
        assert!(!r.watch, "--watch after `--` is NOT consumed");
        assert_eq!(
            r.user_pass_through,
            vec![
                "--frames".to_owned(),
                "999".to_owned(),
                "--watch".to_owned()
            ]
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
    fn assemble_env_matches_forwarded_render_flags() {
        let mut opts = RunOptions::new();
        opts.headless = true;
        opts.screenshot_path = "ui.png".to_owned();
        opts.frames = 30;

        assert_eq!(
            assemble_launch_env(&opts),
            vec![
                ("PULP_HEADLESS".to_owned(), "1".to_owned()),
                ("PULP_SCREENSHOT".to_owned(), "ui.png".to_owned()),
                ("PULP_FRAMES".to_owned(), "30".to_owned()),
            ]
        );
    }

    #[test]
    fn assemble_full_combo_order_matches_cpp() {
        let mut opts = RunOptions::new();
        opts.headless = true;
        opts.screenshot_path = "ui.png".to_owned();
        opts.frames = 30;
        opts.audio_inspector = true;
        opts.audio_probe_json_path = "probe.json".to_owned();
        opts.audio_capture_wav_path = "capture.wav".to_owned();
        opts.audio_capture_frames = 1024;
        opts.user_pass_through = vec!["--child".to_owned()];
        assert_eq!(
            assemble_launch_args(&opts),
            vec![
                "--headless".to_owned(),
                "--screenshot".to_owned(),
                "ui.png".to_owned(),
                "--frames".to_owned(),
                "30".to_owned(),
                "--audio-inspector".to_owned(),
                "--audio-probe-json".to_owned(),
                "probe.json".to_owned(),
                "--audio-capture-wav".to_owned(),
                "capture.wav".to_owned(),
                "--audio-capture-frames".to_owned(),
                "1024".to_owned(),
                "--child".to_owned()
            ]
        );
    }

    #[test]
    fn assemble_env_matches_forwarded_live_audio_flags() {
        let mut opts = RunOptions::new();
        opts.headless = true;
        opts.audio_inspector = true;
        opts.audio_probe_json_path = "probe.json".to_owned();
        opts.audio_scope_json_path = "scope.json".to_owned();
        opts.audio_scope_window = 4096;
        opts.audio_scope_trigger = "raw".to_owned();
        opts.audio_scope_channel = 1;
        opts.audio_capture_wav_path = "capture.wav".to_owned();
        opts.audio_capture_frames = 512;

        assert_eq!(
            assemble_launch_env(&opts),
            vec![
                ("PULP_HEADLESS".to_owned(), "1".to_owned()),
                ("PULP_AUDIO_INSPECTOR".to_owned(), "1".to_owned()),
                ("PULP_AUDIO_PROBE_JSON".to_owned(), "probe.json".to_owned()),
                ("PULP_AUDIO_SCOPE_JSON".to_owned(), "scope.json".to_owned()),
                ("PULP_AUDIO_SCOPE_WINDOW".to_owned(), "4096".to_owned()),
                ("PULP_AUDIO_SCOPE_TRIGGER".to_owned(), "raw".to_owned()),
                ("PULP_AUDIO_SCOPE_CHANNEL".to_owned(), "1".to_owned()),
                (
                    "PULP_AUDIO_CAPTURE_WAV".to_owned(),
                    "capture.wav".to_owned()
                ),
                ("PULP_AUDIO_CAPTURE_WAV_FRAMES".to_owned(), "512".to_owned()),
            ]
        );
    }

    #[test]
    fn assemble_audio_scope_order_matches_cpp() {
        let mut opts = RunOptions::new();
        opts.headless = true;
        opts.audio_scope_json_path = "scope.json".to_owned();
        opts.audio_scope_window = 4096;
        opts.audio_scope_trigger = "raw".to_owned();
        opts.audio_scope_channel = 1;

        assert_eq!(
            assemble_launch_args(&opts),
            vec![
                "--headless".to_owned(),
                "--audio-scope-json".to_owned(),
                "scope.json".to_owned(),
                "--audio-scope-window".to_owned(),
                "4096".to_owned(),
                "--audio-scope-trigger".to_owned(),
                "raw".to_owned(),
                "--audio-scope-channel".to_owned(),
                "1".to_owned(),
            ]
        );
    }

    #[test]
    fn assemble_audio_capture_wav_order_matches_cpp() {
        let mut opts = RunOptions::new();
        opts.headless = true;
        opts.audio_capture_wav_path = "capture.wav".to_owned();
        opts.audio_capture_frames = 1024;

        assert_eq!(
            assemble_launch_args(&opts),
            vec![
                "--headless".to_owned(),
                "--audio-capture-wav".to_owned(),
                "capture.wav".to_owned(),
                "--audio-capture-frames".to_owned(),
                "1024".to_owned(),
            ]
        );

        opts.audio_capture_frames = 0;
        assert_eq!(
            assemble_launch_args(&opts),
            vec![
                "--headless".to_owned(),
                "--audio-capture-wav".to_owned(),
                "capture.wav".to_owned(),
            ]
        );
    }
}
