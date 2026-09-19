#!/usr/bin/env python3
"""Self-tests for the fleet remote probe.

The load-bearing test is `test_minimal_path_refuses_to_report_absence`, which
replays the exact 2026-09-15 incident: a non-login ssh shell with a minimal
PATH reports a tool "not found" when the tool is installed in ~/.local/bin.
The probe must classify that as INSTRUMENT FAILURE, never as absence.
"""

import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import probe_remote as P  # noqa: E402


# The PATH a non-interactive `ssh m3 '...'` actually gets. Measured 2026-09-15.
MINIMAL_PATH = "/opt/homebrew/bin:/opt/homebrew/sbin:/usr/bin:/bin:/usr/sbin:/sbin"
# The PATH a login shell gets on the same host. Measured 2026-09-15.
LOGIN_PATH = (
    "/Users/danielraffel/.local/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
    ":/opt/homebrew/bin:/opt/homebrew/sbin"
)


HOME_BIN = "/Users/danielraffel/.local/bin"


def payload(path, tools, home_bin=HOME_BIN, home_bin_exists=True):
    """Build a fenced probe payload. `tools` maps name -> (file, kind, version)."""
    lines = [P.BEGIN, f"PATH\t{path}", "SHELL_KIND\tzsh",
             f"HOMEBIN\t{home_bin}\t{1 if home_bin_exists else 0}"]
    for name, (file_path, kind, version) in tools.items():
        lines.append(f"TOOL\t{name}\t{file_path}\t{kind}")
        if file_path:
            lines.append(f"VERSION\t{name}\t{version}")
    lines.append(P.END)
    return "\n".join(lines) + "\n"


def fake_runner(stdout="", stderr="", returncode=0, exc=None):
    def run(argv, **kwargs):
        if exc is not None:
            raise exc
        return subprocess.CompletedProcess(argv, returncode, stdout, stderr)
    return run


class ParsePayload(unittest.TestCase):
    def test_missing_begin_marker_raises(self):
        # A login shell that never ran the script (or printed only a banner).
        with self.assertRaises(ValueError) as cm:
            P.parse_payload("Welcome to macOS\nLast login: today\n")
        self.assertIn("BEGIN", str(cm.exception))

    def test_truncated_output_raises(self):
        # Output cut off mid-probe must not be read as "the rest was absent".
        truncated = P.BEGIN + f"\nPATH\t{LOGIN_PATH}\nTOOL\tshipyard\t\tnone\n"
        with self.assertRaises(ValueError) as cm:
            P.parse_payload(truncated)
        self.assertIn("END", str(cm.exception))

    def test_banner_noise_before_fence_is_ignored(self):
        noisy = "MOTD line\n" + payload(LOGIN_PATH, {"x": ("/usr/bin/x", "command", "x 1.0")})
        parsed = P.parse_payload(noisy)
        self.assertEqual(parsed["tools"]["x"]["path"], "/usr/bin/x")
        self.assertEqual(parsed["searched_path"], LOGIN_PATH)


class Classify(unittest.TestCase):
    def test_file_wins(self):
        self.assertEqual(
            P.classify({"path": "/Users/d/.local/bin/shipyard", "shell_kind": "function"}),
            "FILE",
        )

    def test_function_without_binary_is_not_present(self):
        # The ghapp shape: `command -v` would call this "present". It is not a binary.
        self.assertEqual(P.classify({"path": "", "shell_kind": "function"}), "FUNCTION_ONLY")

    def test_alias_without_binary_is_not_present(self):
        self.assertEqual(P.classify({"path": "", "shell_kind": "alias"}), "FUNCTION_ONLY")

    def test_nothing_is_absent(self):
        self.assertEqual(P.classify({"path": "", "shell_kind": "none"}), "ABSENT")


class Controls(unittest.TestCase):
    def test_all_controls_pass_on_a_healthy_login_shell(self):
        parsed = P.parse_payload(payload(LOGIN_PATH, {
            "shipyard": ("/Users/danielraffel/.local/bin/shipyard", "function", "shipyard 0.204.0"),
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        }))
        controls = P.evaluate_controls(parsed)
        self.assertTrue(all(c["ok"] for c in controls), controls)

    def test_path_breadth_control_fails_on_the_minimal_ssh_path(self):
        bare = "/usr/bin:/bin:/usr/sbin:/sbin"
        parsed = P.parse_payload(payload(bare, {
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        }))
        breadth = [c for c in P.evaluate_controls(parsed) if c["name"] == "path_breadth"][0]
        self.assertFalse(breadth["ok"])

    def test_path_breadth_fails_on_the_EXACT_incident_path(self):
        # The regression that matters. MINIMAL_PATH is the real non-login ssh
        # PATH from m3, and it DOES contain /opt/homebrew/bin. The first
        # version of this control only asked "any user-bin marker present?",
        # so it PASSED here -- on the exact PATH that produced the wrong
        # answer -- while advertising sensitivity to it. ~/.local/bin exists on
        # the host and was not searched, which is the question that matters.
        parsed = P.parse_payload(payload(MINIMAL_PATH, {
            "shipyard": ("", "none", ""),
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        }))
        breadth = [c for c in P.evaluate_controls(parsed) if c["name"] == "path_breadth"][0]
        self.assertFalse(breadth["ok"], "the control must fail on the incident's own PATH")
        self.assertIn(HOME_BIN, breadth["detail"])

    def test_path_breadth_passes_when_home_bin_does_not_exist(self):
        # A host with no ~/.local/bin cannot be blind to it. Do not manufacture
        # a failure where the directory is simply not a thing on this host.
        parsed = P.parse_payload(payload(MINIMAL_PATH, {
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        }, home_bin_exists=False))
        breadth = [c for c in P.evaluate_controls(parsed) if c["name"] == "path_breadth"][0]
        self.assertTrue(breadth["ok"])

    def test_scanner_control_fails_when_ls_does_not_resolve(self):
        parsed = P.parse_payload(payload(LOGIN_PATH, {
            P.SCANNER_CONTROL: ("", "none", ""),
            P.SENTINEL: ("", "none", ""),
        }))
        scanner = [c for c in P.evaluate_controls(parsed) if c["name"] == "scanner"][0]
        self.assertFalse(scanner["ok"])

    def test_sentinel_control_fails_when_impossible_name_is_found(self):
        parsed = P.parse_payload(payload(LOGIN_PATH, {
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("/usr/bin/anything", "command", ""),
        }))
        sentinel = [c for c in P.evaluate_controls(parsed) if c["name"] == "sentinel"][0]
        self.assertFalse(sentinel["ok"])

    def test_every_control_states_its_blind_spot(self):
        # The whole point of the tool: a control whose sensitivity nobody
        # stated is how the incident happened.
        parsed = P.parse_payload(payload(LOGIN_PATH, {
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        }))
        for c in P.evaluate_controls(parsed):
            self.assertTrue(c["sensitive_to"].strip(), c)
            self.assertTrue(c["not_sensitive_to"].strip(), c)


class IncidentRegression(unittest.TestCase):
    """The 2026-09-15 incident, replayed."""

    def test_minimal_path_refuses_to_report_absence(self):
        # Non-login ssh shell: shipyard is installed at ~/.local/bin/shipyard,
        # but ~/.local/bin is not on PATH, so the scan cannot see it.
        out = payload("/usr/bin:/bin:/usr/sbin:/sbin", {
            "shipyard": ("", "none", ""),
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        })
        r = P.probe_host("m3", ["shipyard"], timeout=5, connect_timeout=5,
                         shell="zsh", runner=fake_runner(stdout=out))
        self.assertFalse(r["instrument_ok"])
        _, code = P.render([r], ["shipyard"])
        self.assertEqual(code, 3, "a blind probe must exit 3, never claim absence")

    def test_ghapp_style_control_cannot_pass_as_present(self):
        # `command -v ghapp` printed a bare name and read as "present".
        # Here a function with no binary must read as FUNCTION_ONLY.
        out = payload(LOGIN_PATH, {
            "ghapp": ("", "function", ""),
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        })
        r = P.probe_host("m3", ["ghapp"], timeout=5, connect_timeout=5,
                         shell="zsh", runner=fake_runner(stdout=out))
        self.assertTrue(r["instrument_ok"])
        self.assertEqual(r["tools"]["ghapp"]["status"], "FUNCTION_ONLY")

    def test_login_shell_finds_the_tool_the_old_probe_missed(self):
        out = payload(LOGIN_PATH, {
            "shipyard": ("/Users/danielraffel/.local/bin/shipyard", "function", "shipyard 0.204.0"),
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        })
        r = P.probe_host("m3", ["shipyard"], timeout=5, connect_timeout=5,
                         shell="zsh", runner=fake_runner(stdout=out))
        self.assertTrue(r["instrument_ok"])
        self.assertEqual(r["tools"]["shipyard"]["status"], "FILE")
        self.assertEqual(r["tools"]["shipyard"]["path"], "/Users/danielraffel/.local/bin/shipyard")
        self.assertIn("0.204.0", r["tools"]["shipyard"]["version"])
        _, code = P.render([r], ["shipyard"])
        self.assertEqual(code, 0)


class Unreachable(unittest.TestCase):
    def test_ssh_failure_is_instrument_failure_not_absence(self):
        r = P.probe_host("nosuchhost", ["shipyard"], timeout=5, connect_timeout=1,
                         shell="zsh",
                         runner=fake_runner(stdout="", stderr="ssh: Could not resolve hostname",
                                            returncode=255))
        self.assertFalse(r["instrument_ok"])
        self.assertEqual(r["tools"], {})
        text, code = P.render([r], ["shipyard"])
        self.assertEqual(code, 3)
        self.assertNotIn("ABSENT", text)

    def test_subprocess_exception_is_instrument_failure(self):
        r = P.probe_host("m3", ["shipyard"], timeout=5, connect_timeout=1, shell="zsh",
                         runner=fake_runner(exc=subprocess.TimeoutExpired("ssh", 5)))
        self.assertFalse(r["instrument_ok"])
        _, code = P.render([r], ["shipyard"])
        self.assertEqual(code, 3)

    def test_falls_back_to_next_shell_when_first_is_missing(self):
        calls = []

        def run(argv, **kwargs):
            calls.append(argv)
            if "zsh" in argv:
                return subprocess.CompletedProcess(argv, 127, "", "zsh: not found")
            return subprocess.CompletedProcess(argv, 0, payload(LOGIN_PATH, {
                "shipyard": ("/usr/local/bin/shipyard", "command", "shipyard 0.204.0"),
                P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
                P.SENTINEL: ("", "none", ""),
            }), "")

        r = P.probe_host("m3", ["shipyard"], timeout=5, connect_timeout=5,
                         shell=None, runner=run)
        self.assertTrue(r["instrument_ok"])
        self.assertEqual(r["shell"], "bash")
        self.assertGreaterEqual(len(calls), 2)


class RemoteScript(unittest.TestCase):
    def test_uses_a_login_shell(self):
        captured = {}

        def run(argv, **kwargs):
            captured["argv"] = argv
            return subprocess.CompletedProcess(argv, 0, payload(LOGIN_PATH, {
                "shipyard": ("/x/shipyard", "command", "v"),
                P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
                P.SENTINEL: ("", "none", ""),
            }), "")

        P.probe_host("m3", ["shipyard"], timeout=5, connect_timeout=5, shell="zsh", runner=run)
        self.assertIn("-lc", captured["argv"], "probe must run under a LOGIN shell")
        self.assertIn("BatchMode=yes", captured["argv"])

    def test_script_does_not_resolve_via_command_v(self):
        # Resolution must be a PATH scan. `command -v`/`which`/`type` answer
        # with the shell's opinion, which is what broke the original probe.
        script = P.build_remote_script(["shipyard"])
        # Comment-aware: the prose explains why these are banned, so strip
        # comments before checking, or the explanation trips its own check.
        code = "\n".join(
            line.split("#", 1)[0] for line in script.splitlines()
        )
        self.assertNotIn("command -v", code)
        self.assertNotIn("which ", code)

    def test_path_is_not_split_by_word_splitting(self):
        # zsh does NOT word-split unquoted parameter expansions, so
        # `IFS=:; for _dir in $PATH` iterates once over the whole string and
        # finds nothing. The first live run hit this; the scanner control
        # caught it and the probe refused to report absence.
        script = P.build_remote_script(["shipyard"])
        self.assertNotIn("for _dir in $PATH", script)
        self.assertIn("${_rest%%:*}", script)

    def test_script_reports_whether_home_bin_exists(self):
        script = P.build_remote_script(["shipyard"])
        self.assertIn("HOMEBIN", script)
        self.assertIn('-d "$HOME/.local/bin"', script)

    def test_version_is_read_from_the_absolute_path(self):
        script = P.build_remote_script(["shipyard"])
        self.assertIn('"$_p" --version', script)

    def test_names_are_shell_quoted(self):
        import shlex
        name = "weird; rm -rf /"
        script = P.build_remote_script([name])
        loop = script.split("for _n in")[1].split("\n")[0]
        # The name must appear only in its shell-quoted form, so the remote
        # shell cannot re-parse it into extra commands.
        self.assertIn(shlex.quote(name), loop)
        self.assertEqual(loop.strip(), f"{shlex.quote(name)}; do")

    def test_controls_are_always_probed(self):
        script = P.build_remote_script(["shipyard"])
        # probe_host adds them; build_remote_script gets them from the caller.
        names = ["shipyard", P.SCANNER_CONTROL, P.SENTINEL]
        script2 = P.build_remote_script(names)
        for n in names:
            self.assertIn(n, script2)


class Render(unittest.TestCase):
    def test_absent_only_reported_when_controls_passed(self):
        out = payload(LOGIN_PATH, {
            "nope": ("", "none", ""),
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        })
        r = P.probe_host("m3", ["nope"], timeout=5, connect_timeout=5,
                         shell="zsh", runner=fake_runner(stdout=out))
        text, code = P.render([r], ["nope"])
        self.assertEqual(code, 1)
        self.assertIn("ABSENT", text)
        self.assertIn("trustworthy", text)

    def test_searched_path_is_always_shown(self):
        out = payload(LOGIN_PATH, {
            "shipyard": ("/x/shipyard", "command", "v"),
            P.SCANNER_CONTROL: ("/bin/ls", "command", ""),
            P.SENTINEL: ("", "none", ""),
        })
        r = P.probe_host("m3", ["shipyard"], timeout=5, connect_timeout=5,
                         shell="zsh", runner=fake_runner(stdout=out))
        text, _ = P.render([r], ["shipyard"])
        self.assertIn("searched PATH", text)


if __name__ == "__main__":
    unittest.main()
