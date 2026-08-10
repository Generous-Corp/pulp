#!/usr/bin/env python3
"""Open one exact .vcv in Rack and fail unless Rack proves it loaded.

Rack 2.6.6 on macOS consumes a patch passed at cold launch as a document-open
event, but can ignore the same event while it is already running. A successful
`open` process is therefore not evidence that the requested patch is visible.
This helper focuses Rack immediately when its latest logged document and a
previous verified content identity prove it is already the requested patch.
Otherwise it tries the nondestructive warm handoff, then cooperatively restarts
Rack when necessary and verifies ordered evidence from Rack's log.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


DEFAULT_LOG = os.path.expanduser("~/Library/Application Support/Rack2/log.txt")
DEFAULT_IDENTITY = os.path.expanduser(
    "~/Library/Application Support/Forge Modular/rack-open-identity.json")


def patch_module_count(patch: str) -> int:
    modules, _ = _patch_identity(patch)
    return modules


def _patch_identity(patch: str) -> tuple[int, str]:
    """Derive declared module count and digest from one immutable byte read."""
    payload = Path(patch).read_bytes()
    document = json.loads(payload)
    modules = document.get("modules")
    if not isinstance(modules, list) or not modules:
        raise ValueError("the Rack patch contains no modules")
    return len(modules), hashlib.sha256(payload).hexdigest()


def _logged_patch(line: str) -> str | None:
    match = re.search(r"Loading patch\s+(.+?)\s*$", line)
    return match.group(1).strip().strip('"') if match else None


def _module_creations(lines: list[str]) -> list[str]:
    return [line for line in lines
            if "Creating module " in line and
            "Creating module widget" not in line]


def open_evidence(log_text: str, expected_patch: str,
                  expected_modules: int) -> list[str]:
    """Return exact-load plus subsequent module-creation evidence."""
    expected = os.path.realpath(expected_patch)
    lines = log_text.splitlines()
    for index, line in enumerate(lines):
        logged = _logged_patch(line)
        if logged is None:
            continue
        if os.path.realpath(logged) != expected:
            continue
        end = next(
            (later for later in range(index + 1, len(lines))
             if _logged_patch(lines[later]) is not None),
            len(lines))
        created = _module_creations(lines[index + 1:end])
        if len(created) >= expected_modules:
            return [line, *created]
    return []


def latest_open_evidence(log_text: str, expected_patch: str,
                         expected_modules: int) -> list[str]:
    """Return evidence only when Rack's latest loaded patch is the expected one.

    This reads existing evidence from a running Rack, so an older matching load
    is not enough: Rack may have loaded another document since. The latest load
    must name the exact canonical path and have all of the patch's declared
    module creations after it.
    """
    expected = os.path.realpath(expected_patch)
    lines = log_text.splitlines()
    loaded = [
        (index, logged)
        for index, line in enumerate(lines)
        if (logged := _logged_patch(line)) is not None
    ]
    if not loaded:
        return []
    index, logged = loaded[-1]
    if os.path.realpath(logged) != expected:
        return []
    created = _module_creations(lines[index + 1:])
    if len(created) < expected_modules:
        return []
    return [lines[index], *created]


def _read(path: str) -> str:
    try:
        return Path(path).read_text(errors="replace")
    except OSError:
        return ""


def _file_sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _identity_path(log_path: str) -> Path:
    if os.path.realpath(log_path) == os.path.realpath(DEFAULT_LOG):
        return Path(DEFAULT_IDENTITY)
    # Alternate logs are used by tests and isolated proofs. Keep their state
    # beside them so those runs never touch the user's persistent identity.
    return Path(log_path + ".forge-open-identity.json")


def _load_count(log_text: str) -> int:
    return sum(_logged_patch(line) is not None for line in log_text.splitlines())


def _text_sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _open_identity_snapshot(app: str, patch: str, modules: int,
                            log_path: str,
                            selected_sha256: str,
                            process_timeout: float = 2.0,
                            fail_on_patch_change: bool = False
                            ) -> tuple[dict, list[str]] | None:
    """Sample the load-bearing state, excluding unrelated trailing log lines."""
    try:
        patch_sha256 = _file_sha256(patch)
    except OSError as error:
        if fail_on_patch_change:
            raise RuntimeError(
                "the selected Rack patch became unreadable while it was being "
                "opened; retry with the current project") from error
        return None
    if patch_sha256 != selected_sha256:
        if fail_on_patch_change:
            raise RuntimeError(
                "the selected Rack patch changed while it was being opened; "
                "retry with the current project")
        return None
    try:
        log_text = _read(log_path)
        log_inode = os.stat(log_path).st_ino
    except OSError:
        return None
    evidence = latest_open_evidence(log_text, patch, modules)
    if not evidence:
        return None
    process_identity = _rack_process_identity(app, timeout=process_timeout)
    if process_identity is None:
        return None
    identity = {
        "version": 3,
        "app": os.path.realpath(app),
        "patch": os.path.realpath(patch),
        "patch_sha256": selected_sha256,
        "modules": modules,
        "log_inode": log_inode,
        "load_count": _load_count(log_text),
        "evidence_sha256": _text_sha256("\n".join(evidence) + "\n"),
        "process_identity": process_identity,
    }
    return identity, evidence


def _write_identity(log_path: str, identity: dict) -> None:
    """Atomically write the optional future-focus cache."""
    destination = _identity_path(log_path)
    temporary: str | None = None
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(
            prefix=destination.name + ".", suffix=".tmp",
            dir=destination.parent)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(identity, sort_keys=True) + "\n")
        os.replace(temporary, destination)
    except OSError:
        # The exact load already passed. Cache persistence only enables a later
        # same-document focus shortcut and cannot invalidate that success.
        return
    finally:
        if temporary is not None:
            try:
                os.unlink(temporary)
            except OSError:
                pass


def _record_verified_open(app: str, patch: str, modules: int, log_path: str,
                          selected_sha256: str, settle_timeout: float = 1.0,
                          poll_interval: float = 0.05,
                          proven_evidence: list[str] | None = None, *,
                          _clock=None, _sleep=None) -> None:
    """Persist two stable samples of this invocation's proof, with a hard bound."""
    clock = _clock or time.monotonic
    sleeper = _sleep or time.sleep
    deadline = clock() + max(0.0, settle_timeout)
    expected_evidence_sha256 = (
        _text_sha256("\n".join(proven_evidence) + "\n")
        if proven_evidence else None)

    remaining = deadline - clock()
    if remaining <= 0.0:
        return
    first = _open_identity_snapshot(
        app, patch, modules, log_path, selected_sha256,
        process_timeout=max(0.001, remaining / 2.0),
        fail_on_patch_change=True)
    if (first is None or
            (expected_evidence_sha256 is not None and
             first[0]["evidence_sha256"] != expected_evidence_sha256)):
        return

    remaining = deadline - clock()
    if remaining <= 0.0:
        return
    sleeper(min(max(0.0, poll_interval), remaining))
    remaining = deadline - clock()
    if remaining <= 0.0:
        return
    second = _open_identity_snapshot(
        app, patch, modules, log_path, selected_sha256,
        process_timeout=max(0.001, remaining / 2.0),
        fail_on_patch_change=True)
    if clock() >= deadline:
        return
    if second is None or second[0] != first[0]:
        return
    _write_identity(log_path, second[0])


def _matches_recorded_identity(app: str, patch: str, modules: int,
                               log_path: str,
                               selected_sha256: str) -> bool:
    """Whether two current semantic snapshots match the verified record."""
    try:
        identity = json.loads(_identity_path(log_path).read_text())
    except (OSError, ValueError, TypeError):
        return False
    if (not isinstance(identity, dict) or identity.get("version") != 3 or
            not isinstance(identity.get("process_identity"), str) or
            not identity["process_identity"]):
        return False
    first = _open_identity_snapshot(
        app, patch, modules, log_path, selected_sha256)
    second = _open_identity_snapshot(
        app, patch, modules, log_path, selected_sha256)
    return (first is not None and second is not None and
            identity == first[0] == second[0])


def _verified_existing_evidence(app: str, patch: str, modules: int,
                                log_path: str,
                                selected_sha256: str) -> list[str]:
    if not _matches_recorded_identity(
            app, patch, modules, log_path, selected_sha256):
        return []
    # Return evidence from the current segment, not the precheck read. Harmless
    # trailing lines may grow forever; the semantic segment remains identical.
    current = _open_identity_snapshot(
        app, patch, modules, log_path, selected_sha256)
    try:
        recorded = json.loads(_identity_path(log_path).read_text())
    except (OSError, ValueError, TypeError):
        return []
    return (current[1] if current is not None and current[0] == recorded
            else [])


def _fresh(before: str, now: str) -> str:
    return now[len(before):] if now.startswith(before) else now


def _app_binary(app: str) -> str:
    return os.path.join(app, "Contents", "MacOS", "Rack")


def _rack_process_identity(app: str, timeout: float = 2.0) -> str | None:
    """Return PID plus start time for one running instance of this exact app."""
    pattern = "^" + re.escape(_app_binary(app)) + "($| )"
    try:
        found = subprocess.run(
            ["pgrep", "-f", pattern], capture_output=True, text=True,
            timeout=max(0.001, timeout / 2.0))
    except subprocess.TimeoutExpired:
        return None
    if found.returncode != 0 or not isinstance(found.stdout, str):
        return None
    pids = [line.strip() for line in found.stdout.splitlines()
            if line.strip().isdigit()]
    if len(pids) != 1:
        return None
    try:
        started = subprocess.run(
            ["ps", "-o", "lstart=", "-p", pids[0]],
            capture_output=True, text=True,
            timeout=max(0.001, timeout / 2.0))
    except subprocess.TimeoutExpired:
        return None
    if started.returncode != 0 or not isinstance(started.stdout, str):
        return None
    start_time = started.stdout.strip()
    return f"{pids[0]}:{start_time}" if start_time else None


def rack_running(app: str) -> bool:
    binary = _app_binary(app)
    # macOS /usr/bin/pgrep uses POSIX ERE, not PCRE. A non-capturing group
    # `(?: …)` is a compile error there, which made every warm Rack look cold
    # and skipped the cooperative restart path entirely.
    result = subprocess.run(
        ["pgrep", "-f", "^" + re.escape(binary) + "($| )"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if result.returncode == 0:
        return True
    if result.returncode == 1:
        return False
    raise RuntimeError(
        f"could not determine whether Rack is running (pgrep exit "
        f"{result.returncode})")


def _wait_for_evidence(log_path: str, before: str, patch: str, modules: int,
                       timeout: float) -> list[str]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        evidence = latest_open_evidence(
            _fresh(before, _read(log_path)), patch, modules)
        if evidence:
            return evidence
        time.sleep(0.1)
    return []


def _document_open(app: str, patch: str) -> None:
    subprocess.run(["/usr/bin/open", "-a", app, patch], check=True)


def _focus(app: str) -> None:
    subprocess.run(["/usr/bin/open", "-a", app], check=True)


def _quit(app: str) -> None:
    name = Path(app).name.removesuffix(".app").replace('"', '\\"')
    # The document handoff and a user's own close can race this request. A
    # non-zero AppleScript status is harmless when Rack has already stopped;
    # _wait_stopped() below remains the authority and still fails closed if it
    # is actually alive (for example behind an unsaved-work dialog).
    subprocess.run(
        ["osascript", "-e", f'tell application "{name}" to quit'],
        check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _wait_stopped(app: str, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not rack_running(app):
            return True
        time.sleep(0.1)
    return not rack_running(app)


def open_patch(app: str, patch: str, log_path: str = DEFAULT_LOG,
               warm_timeout: float = 2.0, cold_timeout: float = 45.0) -> list[str]:
    """Focus or open and verify, restarting only after a warm handoff fails."""
    patch = os.path.realpath(patch)
    if not os.path.isabs(patch) or not os.path.isfile(patch):
        raise RuntimeError(f"Rack patch is not a readable absolute file: {patch}")
    if not os.path.isdir(app):
        raise RuntimeError(f"Rack application is not installed at: {app}")
    modules, selected_sha256 = _patch_identity(patch)

    was_running = rack_running(app)
    if was_running:
        existing = _verified_existing_evidence(
            app, patch, modules, log_path, selected_sha256)
        if existing:
            _focus(app)
            # Do not report stale evidence if Rack changed documents while the
            # focus request was being delivered. In that race, fall through to
            # the normal exact-document handoff.
            existing = _verified_existing_evidence(
                app, patch, modules, log_path, selected_sha256)
            if existing:
                return existing
    # Establish the fresh-evidence boundary immediately before the handoff.
    # Precheck activity must never be mistaken for the document-open result.
    before = _read(log_path)
    _document_open(app, patch)
    evidence = _wait_for_evidence(
        log_path, before, patch, modules,
        warm_timeout if was_running else cold_timeout)
    if evidence or not was_running:
        if evidence:
            _record_verified_open(
                app, patch, modules, log_path, selected_sha256,
                proven_evidence=evidence)
            return evidence
        raise RuntimeError("Rack started but never logged the exact patch and "
                           f"all {modules} module creations")

    # Rack accepted the LaunchServices request but ignored the document while
    # warm. Quit cooperatively: an unsaved-work dialog may keep it alive, in
    # which case fail and ask the user rather than killing their session.
    _quit(app)
    if not _wait_stopped(app, 15.0):
        raise RuntimeError("Rack ignored the patch while running and did not "
                           "quit cleanly; close/save Rack and try again")

    before = _read(log_path)
    _document_open(app, patch)
    evidence = _wait_for_evidence(log_path, before, patch, modules, cold_timeout)
    if not evidence:
        raise RuntimeError("Rack restarted but never logged the exact patch and "
                           f"all {modules} module creations")
    _record_verified_open(
        app, patch, modules, log_path, selected_sha256,
        proven_evidence=evidence)
    return evidence


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True)
    parser.add_argument("--patch", required=True)
    parser.add_argument("--log", default=DEFAULT_LOG)
    args = parser.parse_args(argv[1:])
    try:
        evidence = open_patch(args.app, args.patch, args.log)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    for line in evidence:
        print(line)
    print("PASS: Rack loaded the exact patch and created its declared modules")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
