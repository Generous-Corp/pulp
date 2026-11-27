#!/usr/bin/env python3
"""Check every prerequisite gates.sh needs before it runs a single gate.

A fresh or reused checkout used to surface these one failed gates run at a time,
two to fifteen minutes apart: a shallow clone, a missing merge driver, no
VST3/AU SDKs, no jsx-runtime packages, a build dir without the CMake file-API
reply, a Vellum-enabled build dir picked as the gate build. This prints the
whole list once, each with the exact command that fixes it.

    gates_preflight.py                  report; exit 1 when anything is missing
    gates_preflight.py --bootstrap      also do the safe fixes, then re-check

Every check is conditional on what makes it relevant, so a checkout that has no
use for a prerequisite is never told to install it:

    shallow clone       always (the silent-revert and cut-manifest gates read history)
    merge driver        .gitattributes routes a file to merge=pulp-gpu-ledger
    SDKs                the committed census records a profile for this platform with the SDK on
    jsx-runtime         tools/import-design/jsx-runtime/package-lock.json exists
    file-API reply      a reusable build dir exists (a missing one is configured with the query)
    Vellum build dir    PULP_GATES_BUILD_DIR names one (an unset choice just skips it)

Bootstrap performs only the automatic ones, and says what each touches: a fetch
(unshallow), this worktree's git config (never the shared one), gitignored
symlinks and packages under the checkout (setup.sh, npm ci, both at nice 10), and
a configure-only rerun of an existing build dir for the file-API reply. It never
moves or deletes a build dir: a Vellum-enabled tree is left alone and the gates
choose another.

With --base, the SDK, jsx-runtime and file-API checks apply only when the diff can
reach a configure (the generated-manifest and census lanes); otherwise they are
advisory and neither fail the run nor are fixed by --bootstrap. The shallow clone
and merge driver checks always apply: their gates fail whatever the diff.

Exit codes: 0 ready, 1 a prerequisite is missing (or a bootstrap step failed), 2 internal error.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "ci"))
import configure_only  # noqa: E402
import source_selftests as sst  # noqa: E402

CENSUS_FILE = Path("docs") / "status" / "consumption-profiles.json"
LEDGER_DRIVER = "pulp-gpu-ledger"
CODEMODEL_QUERY = Path(".cmake") / "api" / "v1" / "query" / "codemodel-v2"
CODEMODEL_REPLY = Path(".cmake") / "api" / "v1" / "reply"
# (census feature, directory marker setup.sh --deps-only materializes, platform it applies to)
SDKS = (
    ("PULP_HAS_VST3", "VST3 SDK", Path("external/vst3sdk/pluginterfaces"), None),
    ("PULP_HAS_AUSDK", "AudioUnit SDK", Path("external/AudioUnitSDK/include/AudioUnitSDK/AUBase.h"), "darwin"),
)


@dataclass
class Finding:
    key: str
    what: str
    fix: list[str]
    # Performs the fix; returns "" on success, else why it could not. None: not automatable.
    action: Callable[[], str] | None = None
    why_not_automatic: str = ""
    advisory: bool = False


@dataclass
class Report:
    findings: list[Finding] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)


def say(msg: str = "") -> None:
    print(msg, file=sys.stderr)


def git(root: Path, *args: str) -> tuple[int, str]:
    proc = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          encoding="utf-8", check=False)
    return proc.returncode, proc.stdout.strip()


def run(cmd: list[str], root: Path, env: dict[str, str] | None = None) -> str:
    """Run a fix command; "" on success, else its tail."""
    say(f"    $ {' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=root, env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=False)
    if proc.returncode == 0:
        return ""
    tail = (proc.stdout + proc.stderr).strip().splitlines()[-6:]
    return f"exit {proc.returncode}: " + " | ".join(tail)


def platform_key() -> tuple[str, str]:
    return platform.system().lower(), platform.machine().lower()


def census_profiles_for_host(root: Path) -> list[dict]:
    """The committed census profiles recorded for this platform."""
    system, machine = platform_key()
    try:
        document = json.loads((root / CENSUS_FILE).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    prefix = f"{system}-{machine}-"
    return [p for k, p in document.get("profiles", {}).items() if k.startswith(prefix)]


def check_shallow(root: Path) -> Finding | None:
    rc, out = git(root, "rev-parse", "--is-shallow-repository")
    if rc != 0 or out != "true":
        return None
    return Finding(
        "shallow", "this is a shallow clone; the silent-revert, cut-manifest and sequencer-exposure "
        "gates read history the clone does not have",
        ["git fetch --unshallow"],
        lambda: run(["git", "fetch", "--unshallow"], root))


HOOKS_DIR = Path(".githooks")
DRIVER_CMD = "tools/scripts/gpu_ledger_merge_driver.py %O %A %B"
DRIVER_NAME = "GPU handoff ledger: three-way merge with regenerable fields sentinelled"


def worktree_config_commands() -> list[list[str]]:
    """The git config for this worktree only. The shared .git/config would switch the
    enforcing pre-push hook on in every sibling worktree, so only the repository
    format flag that makes per-worktree config readable is set there."""
    return [["git", "config", "extensions.worktreeConfig", "true"],
            ["git", "config", "--worktree", "core.hooksPath", ".githooks"],
            ["git", "config", "--worktree", f"merge.{LEDGER_DRIVER}.name", DRIVER_NAME],
            ["git", "config", "--worktree", f"merge.{LEDGER_DRIVER}.driver", DRIVER_CMD]]


def install_worktree_hooks(root: Path) -> str:
    for cmd in worktree_config_commands():
        why = run(cmd, root)
        if why:
            return why
    for hook in (root / HOOKS_DIR).glob("*"):
        try:
            hook.chmod(hook.stat().st_mode | 0o111)
        except OSError:
            pass
    return ""


def check_merge_driver(root: Path) -> Finding | None:
    try:
        attributes = (root / ".gitattributes").read_text(encoding="utf-8")
    except OSError:
        return None
    if f"merge={LEDGER_DRIVER}" not in attributes or not (root / HOOKS_DIR).is_dir():
        return None
    _, driver = git(root, "config", "--get", f"merge.{LEDGER_DRIVER}.driver")
    _, hooks = git(root, "config", "--get", "core.hooksPath")
    missing = [n for n, ok in ((f"the {LEDGER_DRIVER} merge driver", bool(driver)),
                               ("core.hooksPath=.githooks", hooks == ".githooks")) if not ok]
    if not missing:
        return None
    return Finding(
        "githooks", f"{' and '.join(missing)} not set for this worktree (a merge of main then leaves the GPU "
        "ledger unregenerated and the sentinel gate fails). The fix sets them for this worktree only; "
        "tools/scripts/install-githooks.sh sets them for every worktree of the clone",
        [" && ".join(" ".join(shlex.quote(a) for a in c) for c in worktree_config_commands())],
        lambda: install_worktree_hooks(root))


def check_sdks(root: Path, env: dict[str, str]) -> Finding | None:
    setup = env.get("PULP_GATES_SETUP", str(root / "setup.sh"))
    if not setup or not Path(setup).is_file():
        return None
    system, _ = platform_key()
    profiles = census_profiles_for_host(root)
    missing = []
    for feature, label, marker, only_on in SDKS:
        if only_on and only_on != system:
            continue
        if any(p.get("features", {}).get(feature) == "ON" for p in profiles) \
                and not (root / marker).exists():
            missing.append(label)
    if not missing:
        return None
    return Finding(
        "sdks", f"{' and '.join(missing)} missing under external/; without them the configure matches a "
        "different census profile and the gate reports drift that is not yours",
        ["./setup.sh --deps-only --non-interactive"],
        lambda: run(["nice", "-n", "10", "bash", setup, "--deps-only", "--non-interactive"], root))


def npm_cache_dir(root: Path, env: dict[str, str]) -> Path:
    declared = env.get("npm_config_cache", "").strip()
    if declared:
        return Path(declared).expanduser()
    _, path = git(root, "rev-parse", "--path-format=absolute", "--git-path", "pulp-npm-cache")
    return Path(path) if path else root / ".git" / "pulp-npm-cache"


def check_jsx_runtime(root: Path, env: dict[str, str]) -> Finding | None:
    jsx = root / configure_only.JSX_RUNTIME
    if not (jsx / "package-lock.json").is_file():
        return None
    if all((jsx / "node_modules" / m).is_file() for m in configure_only.JSX_RUNTIME_MODULES):
        return None
    cache = npm_cache_dir(root, env)
    shown = configure_only.JSX_RUNTIME.as_posix()
    if shutil.which("npm") is None:
        return Finding("jsx-runtime", f"{shown}/node_modules is missing and npm is not on PATH "
                       "(the changed-surface families check needs the packages)",
                       [f"install Node, then: (cd {shown} && npm ci)"])
    return Finding(
        "jsx-runtime", f"{shown}/node_modules is missing (the changed-surface families check needs it)",
        [f"(cd {shown} && npm ci --cache {cache})"],
        lambda: run(["nice", "-n", "10", "npm", "ci", "--prefix", str(jsx), "--cache", str(cache),
                     "--prefer-offline", "--no-audit", "--no-fund"], root))


def reconfigure_for_reply(root: Path, build: Path) -> str:
    """Add the file-API query and configure again so CMake writes the reply (no compile)."""
    say(f"    reconfiguring your build dir {build.name} (configure only, no compile; adds the file-API query)")
    (build / CODEMODEL_QUERY).parent.mkdir(parents=True, exist_ok=True)
    (build / CODEMODEL_QUERY).touch()
    env = {**os.environ, **configure_only.GOVERNED_ENV}
    cmd = configure_only.governor_prefix(root) + [os.environ.get("CMAKE", "cmake"), "-S", str(root),
                                                  "-B", str(build)]
    why = run(cmd, root, env)
    if why:
        return why
    return "" if has_codemodel_reply(build) else f"{build.name}: configured, but CMake wrote no codemodel reply"


def has_codemodel_reply(build: Path) -> bool:
    return (build / CODEMODEL_QUERY).exists() and any((build / CODEMODEL_REPLY).glob("codemodel-v2-*.json"))


def vellum_dirs(root: Path) -> list[tuple[Path, list[str]]]:
    found = []
    for candidate in sorted(root.glob("build*")):
        if candidate.name.startswith(sst.CONFIGURE_ONLY_PREFIX) or not (candidate / "CMakeCache.txt").is_file():
            continue
        options = sst.vellum_options_on(candidate)
        if options:
            found.append((candidate, options))
    return found


def check_build_dir(root: Path, env: dict[str, str], report: Report) -> None:
    declared = env.get(sst.GATES_BUILD_DIR_ENV, "").strip()
    if declared:
        path = Path(declared).expanduser()
        path = path if path.is_absolute() else root / path
        options = sst.vellum_options_on(path)
        if options:
            report.findings.append(Finding(
                "vellum-build", f"{sst.GATES_BUILD_DIR_ENV}={declared} is a Vellum-enabled tree "
                f"({', '.join(options)}); the census would read as drift against it",
                [f"unset {sst.GATES_BUILD_DIR_ENV}   # the gates then use build/ or configure build-gate"]))
            return
    for candidate, options in vellum_dirs(root):
        if not declared:
            report.notes.append(
                f"{candidate.name} has Vellum options on ({', '.join(options)}); the gates ignore it and use "
                "a default-profile tree. It stays where it is.")
    build, why = sst.choose_build_dir(root, env)
    if build is None:
        report.notes.append(f"no usable build dir ({why}); the gates configure build-gate when a diff needs one "
                            "(configure only, no compile)")
        return
    report.notes.append(f"gate build dir: {build.name} ({why})")
    if has_codemodel_reply(build):
        return
    shown = build.name
    report.findings.append(Finding(
        "file-api", f"{shown} has no CMake file-API codemodel reply (the changed-surface families check "
        "cannot read the target graph from it); --bootstrap reruns cmake -S . -B on it, configure only",
        [f"mkdir -p {shown}/.cmake/api/v1/query && touch {shown}/.cmake/api/v1/query/codemodel-v2",
         f"cmake -S . -B {shown}"],
        lambda: reconfigure_for_reply(root, build)))


CONFIGURE_KEYS = {"sdks", "jsx-runtime", "file-api"}


def diff_needs_configure(root: Path, base: str) -> bool:
    """Whether the diff can reach a lane that configures a build (the generated test
    manifests, the changed-surface families, the census). Fails closed: a diff that
    cannot be read is treated as needing one."""
    try:
        import gates_script_inputs as gsi  # noqa: PLC0415  (heavy; only --base callers pay for it)
        changed = gsi.sti.changed_files(root, base)
        return bool(gsi.sti.touch_reasons(root, base) or gsi.generated_manifest_reasons(changed)
                    or gsi.census_reasons(root, base, changed))
    except Exception:  # noqa: BLE001 - any failure to read the diff means "assume it does"
        return True


def collect(root: Path, env: dict[str, str], needs_configure: bool = True) -> Report:
    report = Report()
    for finding in (check_shallow(root), check_merge_driver(root), check_sdks(root, env),
                    check_jsx_runtime(root, env)):
        if finding:
            report.findings.append(finding)
    check_build_dir(root, env, report)
    for finding in report.findings:
        finding.advisory = finding.key in CONFIGURE_KEYS and not needs_configure
    return report


def blocking(report: Report) -> list[Finding]:
    return [f for f in report.findings if not f.advisory]


def print_report(report: Report, bootstrap_hint: bool) -> None:
    for note in report.notes:
        say(f"  note: {note}")
    for finding in report.findings:
        if finding.advisory:
            say(f"  note: [{finding.key}] not needed for this diff (it reaches no lane that configures a build): "
                f"{finding.what.split(';')[0]}")
    todo = blocking(report)
    if not todo:
        return
    say(f"gates: preflight found {len(todo)} missing prerequisite(s). Each one otherwise "
        "fails a separate gates run, minutes apart:")
    for index, finding in enumerate(todo, 1):
        say(f"  {index}. [{finding.key}] {finding.what}")
        for command in finding.fix:
            say(f"       fix: {command}")
        if finding.action is None:
            say("       (bootstrap does not do this one)")
    if bootstrap_hint:
        say("  Do the automatic ones and continue:  tools/scripts/gates.sh --bootstrap [base]")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=str(HERE.parents[1]))
    ap.add_argument("--bootstrap", action="store_true", help="perform the safe fixes, then re-check")
    ap.add_argument("--base", default=None, help="scope the configure-time checks to what this diff needs")
    args = ap.parse_args(argv[1:])
    root = Path(args.repo_root).resolve()
    env = dict(os.environ)
    needs = diff_needs_configure(root, args.base) if args.base else True

    report = collect(root, env, needs)
    if args.bootstrap and blocking(report):
        say(f"gates: bootstrap: fixing {len(blocking(report))} prerequisite(s)")
        for finding in blocking(report):
            if finding.action is None:
                continue
            say(f"  [{finding.key}] {finding.what.split(';')[0]}")
            why = finding.action()
            say(f"    {'done' if not why else 'FAILED: ' + why}")
        report = collect(root, env, needs)
    print_report(report, bootstrap_hint=not args.bootstrap)
    if blocking(report):
        return 1
    say("gates: preflight ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
