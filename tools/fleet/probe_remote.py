#!/usr/bin/env python3
"""Probe fleet hosts for installed binaries — without lying about absence.

Why this exists
---------------
The obvious probe is wrong:

    ssh m3 'command -v shipyard >/dev/null 2>&1 && shipyard --version || echo MISSING'

It reported ``MISSING`` on a host where shipyard 0.204.0 was installed at
``~/.local/bin/shipyard``. Two independent defects, either of which is enough:

1. A non-interactive ssh shell gets a minimal PATH
   (``/opt/homebrew/bin:/opt/homebrew/sbin:/usr/bin:/bin:/usr/sbin:/sbin``) that
   does not include ``~/.local/bin``. The tool was never searched for.
2. ``command -v`` answers with whatever the *shell* thinks the name means. On
   this fleet ``~/.config/whence/hook.sh`` defines shell functions that shadow
   real binaries, so ``command -v`` returns a bare name for a function that has
   no binary behind it — and, in the same shell, resolves regardless of PATH.

Defect 2 is why the session's control was worthless: the control probe
``command -v ghapp`` printed ``present``, which read as proof the instrument
worked. ``ghapp`` is a shell function. A function resolves in any shell no
matter what PATH is, so the control was **insensitive to the exact failure mode
it was meant to guard against** — and it passed while the instrument was blind.

The durable rule, which this tool implements rather than documents:

    A control must be sensitive to the SPECIFIC failure mode you are guarding
    against, not merely non-zero. A control that would still pass when the
    instrument is broken in the way you fear is decoration.

What this tool guarantees
-------------------------
* **Resolution is by PATH scan, not by shell opinion.** For each name it walks
  the PATH directories looking for an executable file. Functions, aliases and
  builtins cannot fake a hit, and cannot hide one either.
* **It reports WHERE, not whether.** Output is an absolute path plus the
  version read by executing *that path*, never the bare name (a function would
  otherwise answer the version question on the binary's behalf).
* **It names shell functions/aliases as such**, so a ``FUNCTION_ONLY`` result is
  visibly not an installed binary.
* **It cannot silently report absence.** Absence is only ever reported when all
  of its self-checks passed. Connection failure, shell failure, truncated
  output, or a failed self-check exits ``3`` (INSTRUMENT FAILURE) and reports
  no absence at all.
* **It prints the PATH it searched** on every run, so a blind spot is visible
  rather than implied.

Control sensitivity — stated honestly, per control
--------------------------------------------------
Each self-check says what it is, and is NOT, sensitive to. That accounting is
the point of the tool; a control whose sensitivity nobody stated is how the
original incident happened.

``scanner``   positive control. A binary that must exist in a system directory
              (``ls``) must resolve to a FILE via the same PATH scan used for
              real answers.
              Sensitive to: a broken scan/parse, a dead shell, truncated output.
              NOT sensitive to: a short PATH missing user dirs — ``/bin`` is on
              even the minimal PATH. This is the control that the original
              incident's ``ghapp`` check was pretending to be, and it would
              NOT have caught that incident on its own. ``path_breadth`` is the
              one that does.
``sentinel``  negative control. A name that cannot exist must report ABSENT.
              Sensitive to: a parser that reports everything as found.
              NOT sensitive to: PATH breadth.
``path_breadth``  the control that actually catches the incident. The PATH the
              probe searched must contain the user-local bin directories where
              fleet tools are installed (``~/.local/bin`` and friends). A
              non-login ssh shell fails this.
              Sensitive to: exactly the "tool was never searched for" failure.
              NOT sensitive to: a tool genuinely absent from a complete PATH.

Usage
-----
    python3 tools/fleet/probe_remote.py --hosts m1,m3 shipyard ghapp
    python3 tools/fleet/probe_remote.py --hosts m3 --json shipyard
    python3 tools/fleet/probe_remote.py --local shipyard      # this machine

Exit codes
----------
    0  every requested tool resolved to an executable FILE on every host
    1  a trustworthy negative: some tool is ABSENT or FUNCTION_ONLY, and every
       self-check passed, so the absence is real
    2  usage error
    3  INSTRUMENT FAILURE on at least one host — unreachable, shell failure,
       truncated output, or a failed self-check. NO absence is claimed.
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys

# Marker lines fence the payload so a login shell's MOTD/banner/profile chatter
# cannot be mistaken for probe output. A login shell is required for a correct
# PATH, and a login shell is exactly the kind that prints banners, so the
# fencing is not optional.
BEGIN = "---PULP-FLEET-PROBE-BEGIN---"
END = "---PULP-FLEET-PROBE-END---"

# Must not exist anywhere. Negative control.
SENTINEL = "pulp_fleet_probe_sentinel_must_not_exist"
# Must exist as a real file on any POSIX host. Positive control for the scanner.
SCANNER_CONTROL = "ls"

# Directories that hold user-installed fleet tooling. If the probe's PATH
# contains none of these, the probe could not have seen a user-installed tool,
# and any "absent" it produced would be meaningless.
USER_BIN_MARKERS = (".local/bin", "/usr/local/bin", "/opt/homebrew/bin")

DEFAULT_LOGIN_SHELLS = ("zsh", "bash", "sh")

# POSIX sh. Runs under a LOGIN shell so PATH is the user's real PATH.
#
# Resolution deliberately does NOT use `command -v` / `which` / `type`: those
# answer with the shell's opinion of a name, which on this fleet is often a
# function from ~/.config/whence/hook.sh. We want the binary or nothing.
REMOTE_SCRIPT = r"""
printf '%s\n' '@@BEGIN@@'
printf 'PATH\t%s\n' "$PATH"
printf 'SHELL_KIND\t%s\n' "${ZSH_VERSION:+zsh}${BASH_VERSION:+bash}"

# Find an executable regular file for $1 by scanning PATH entries in order.
# Immune to functions, aliases and builtins in both directions.
# NOTE: splits PATH with POSIX parameter expansion, NOT with `for d in $PATH`
# under IFS=:. zsh does not word-split unquoted parameter expansions, so the
# IFS form silently iterates ONCE over the whole PATH string and finds nothing.
# That bug was caught by this tool's own scanner control on its first live run.
# Empty PATH entries (which POSIX reads as the cwd) are skipped deliberately: a
# fleet probe must not resolve a binary out of whatever directory ssh landed in.
probe_file() {
    _name=$1
    _rest=$PATH
    while [ -n "$_rest" ]; do
        case "$_rest" in
            *:*) _dir=${_rest%%:*}; _rest=${_rest#*:} ;;
            *)   _dir=$_rest; _rest= ;;
        esac
        [ -n "$_dir" ] || continue
        if [ -f "$_dir/$_name" ] && [ -x "$_dir/$_name" ]; then
            printf '%s' "$_dir/$_name"
            return 0
        fi
    done
    return 1
}

# What does the SHELL think this name is? Reported alongside, never instead of,
# the PATH scan -- so a function that shadows a missing binary is visible.
probe_kind() {
    _name=$1
    if [ -n "${ZSH_VERSION:-}" ]; then
        # zsh: `whence -w` prints "name: function|builtin|command|alias|none"
        whence -w "$_name" 2>/dev/null | sed 's/^[^:]*: *//' || printf 'unknown'
    elif [ -n "${BASH_VERSION:-}" ]; then
        # bash: `type -t` prints function|builtin|file|alias|keyword, or nothing
        _t=$(type -t "$_name" 2>/dev/null || true)
        [ -n "$_t" ] && printf '%s' "$_t" || printf 'none'
    else
        printf 'unknown'
    fi
}

for _n in @@NAMES@@; do
    _p=$(probe_file "$_n" || true)
    _k=$(probe_kind "$_n")
    printf 'TOOL\t%s\t%s\t%s\n' "$_n" "$_p" "$_k"
    if [ -n "$_p" ]; then
        # Version comes from executing the RESOLVED ABSOLUTE PATH. Running the
        # bare name here would let a shell function answer for the binary.
        _v=$("$_p" --version 2>/dev/null | head -1 || true)
        printf 'VERSION\t%s\t%s\n' "$_n" "$_v"
    fi
done
printf '%s\n' '@@END@@'
"""


def build_remote_script(names: list[str]) -> str:
    """Interpolate the probe names and markers into the remote script."""
    quoted = " ".join(shlex.quote(n) for n in names)
    return (
        REMOTE_SCRIPT.replace("@@NAMES@@", quoted)
        .replace("@@BEGIN@@", BEGIN)
        .replace("@@END@@", END)
    )


def parse_payload(stdout: str) -> dict:
    """Parse the fenced payload.

    Raises ValueError if the fence is missing or incomplete -- which is an
    INSTRUMENT FAILURE, never an 'absent' result.
    """
    if BEGIN not in stdout:
        raise ValueError("probe output has no BEGIN marker (shell never ran the script?)")
    if END not in stdout:
        raise ValueError("probe output has no END marker (output truncated mid-probe)")
    body = stdout.split(BEGIN, 1)[1].split(END, 1)[0]

    searched_path = ""
    shell_kind = ""
    tools: dict[str, dict] = {}
    for line in body.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        tag = parts[0]
        if tag == "PATH" and len(parts) >= 2:
            searched_path = parts[1]
        elif tag == "SHELL_KIND" and len(parts) >= 2:
            shell_kind = parts[1]
        elif tag == "TOOL" and len(parts) >= 4:
            tools[parts[1]] = {"path": parts[2], "shell_kind": parts[3], "version": ""}
        elif tag == "VERSION" and len(parts) >= 3:
            if parts[1] in tools:
                tools[parts[1]]["version"] = parts[2]
    return {"searched_path": searched_path, "shell_kind": shell_kind, "tools": tools}


def classify(entry: dict) -> str:
    """FILE | FUNCTION_ONLY | ABSENT, from a parsed tool entry."""
    if entry.get("path"):
        return "FILE"
    kind = (entry.get("shell_kind") or "").strip()
    if kind in ("function", "alias"):
        # The name resolves in a shell but there is no binary. This is exactly
        # what makes `command -v` unsafe as a presence check.
        return "FUNCTION_ONLY"
    return "ABSENT"


def evaluate_controls(parsed: dict) -> list[dict]:
    """Run the self-checks. Each states what it is sensitive to."""
    tools = parsed["tools"]
    path = parsed["searched_path"]
    controls = []

    scanner = tools.get(SCANNER_CONTROL, {})
    controls.append({
        "name": "scanner",
        "ok": bool(scanner.get("path")),
        "detail": f"{SCANNER_CONTROL} -> {scanner.get('path') or 'NOT FOUND'}",
        "sensitive_to": "broken scan/parse, dead shell, truncated output",
        "not_sensitive_to": "a short PATH missing user dirs (/bin is always present)",
    })

    sentinel = tools.get(SENTINEL, {})
    controls.append({
        "name": "sentinel",
        "ok": classify(sentinel) == "ABSENT" if sentinel else False,
        "detail": f"{SENTINEL} -> {classify(sentinel) if sentinel else 'NOT PROBED'}",
        "sensitive_to": "a parser that reports everything as found",
        "not_sensitive_to": "PATH breadth",
    })

    entries = [d for d in path.split(":") if d]
    hit = [m for m in USER_BIN_MARKERS if any(m in d for d in entries)]
    controls.append({
        "name": "path_breadth",
        "ok": bool(hit),
        "detail": (
            f"searched {len(entries)} dirs; user-bin markers present: {hit or 'NONE'}"
        ),
        "sensitive_to": "the non-login-shell minimal PATH — a tool never searched for",
        "not_sensitive_to": "a tool genuinely absent from a complete PATH",
    })
    return controls


def probe_host(host: str | None, names: list[str], *, timeout: int, connect_timeout: int,
               shell: str | None, runner=subprocess.run) -> dict:
    """Probe one host (or locally when host is None). Never raises for a
    reachability problem — returns an ``instrument_ok: False`` record instead."""
    probe_names = list(dict.fromkeys(list(names) + [SCANNER_CONTROL, SENTINEL]))
    script = build_remote_script(probe_names)
    shells = [shell] if shell else list(DEFAULT_LOGIN_SHELLS)

    last_err = ""
    for sh in shells:
        if host is None:
            argv = [sh, "-lc", script]
        else:
            # shlex.quote: ssh concatenates argv with spaces and the remote
            # shell re-parses, so the script must survive one round of parsing.
            argv = [
                "ssh", "-o", "BatchMode=yes", "-o", f"ConnectTimeout={connect_timeout}",
                host, sh, "-lc", shlex.quote(script),
            ]
        try:
            res = runner(argv, capture_output=True, text=True, timeout=timeout)
        except (OSError, subprocess.SubprocessError) as exc:
            last_err = f"{type(exc).__name__}: {exc}"
            continue

        stdout = res.stdout or ""
        if BEGIN not in stdout:
            # Could be "zsh: not found" on this host -> try the next shell.
            last_err = (res.stderr or stdout or f"exit {res.returncode}").strip()[:400]
            continue
        try:
            parsed = parse_payload(stdout)
        except ValueError as exc:
            return {
                "host": host or "local", "instrument_ok": False,
                "error": str(exc), "shell": sh, "controls": [], "tools": {},
                "searched_path": "",
            }
        controls = evaluate_controls(parsed)
        return {
            "host": host or "local",
            "instrument_ok": all(c["ok"] for c in controls),
            "error": "",
            "shell": sh,
            "searched_path": parsed["searched_path"],
            "controls": controls,
            "tools": {
                n: {**parsed["tools"].get(n, {}), "status": classify(parsed["tools"].get(n, {}))}
                for n in names
            },
        }

    return {
        "host": host or "local", "instrument_ok": False,
        "error": last_err or "no login shell produced probe output",
        "shell": "", "controls": [], "tools": {}, "searched_path": "",
    }


def render(results: list[dict], names: list[str]) -> tuple[str, int]:
    """Human-readable report + exit code."""
    lines: list[str] = []
    any_instrument_failure = False
    any_negative = False

    for r in results:
        lines.append(f"host {r['host']}")
        if not r["instrument_ok"]:
            any_instrument_failure = True
            if r["error"]:
                lines.append(f"  INSTRUMENT FAILURE: {r['error']}")
            for c in r.get("controls", []):
                if not c["ok"]:
                    lines.append(f"  control {c['name']}: FAILED — {c['detail']}")
                    lines.append(f"    sensitive to: {c['sensitive_to']}")
            lines.append("  -> no absence is reported from this host; the probe could not see.")
            lines.append("")
            continue

        lines.append(f"  searched PATH ({len([d for d in r['searched_path'].split(':') if d])} dirs) via {r['shell']} -lc")
        for n in names:
            e = r["tools"].get(n, {})
            status = e.get("status", "ABSENT")
            if status == "FILE":
                extra = f"  [{e['shell_kind']}]" if e.get("shell_kind") in ("function", "alias") else ""
                ver = e.get("version") or "(no --version output)"
                lines.append(f"  {n:<16} FILE           {e['path']}  {ver}{extra}")
            elif status == "FUNCTION_ONLY":
                any_negative = True
                lines.append(f"  {n:<16} FUNCTION_ONLY  shell {e['shell_kind']}, NO binary on PATH")
            else:
                any_negative = True
                lines.append(f"  {n:<16} ABSENT         (trustworthy: all controls passed)")
        lines.append("")

    if any_instrument_failure:
        return "\n".join(lines), 3
    if any_negative:
        return "\n".join(lines), 1
    return "\n".join(lines), 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Probe fleet hosts for installed binaries without lying about absence.",
    )
    ap.add_argument("names", nargs="+", help="binary names to probe")
    ap.add_argument("--hosts", default="", help="comma-separated ssh hosts (e.g. m1,m3)")
    ap.add_argument("--local", action="store_true", help="probe this machine too")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--connect-timeout", type=int, default=10)
    ap.add_argument("--shell", default="", help="login shell to use (default: try zsh, bash, sh)")
    args = ap.parse_args(argv)

    hosts: list[str | None] = [h.strip() for h in args.hosts.split(",") if h.strip()]
    if args.local or not hosts:
        hosts.append(None)
    if not hosts:
        ap.error("nothing to probe: pass --hosts and/or --local")

    results = [
        probe_host(h, args.names, timeout=args.timeout,
                   connect_timeout=args.connect_timeout,
                   shell=args.shell or None)
        for h in hosts
    ]

    text, code = render(results, args.names)
    if args.json:
        print(json.dumps({"results": results, "exit_code": code}, indent=2))
    else:
        print(text)
    return code


if __name__ == "__main__":
    sys.exit(main())
