#!/usr/bin/env python3
"""Resolve `runs-on` for every release-cli leg — one platform -> runs-on map.

Used by `.github/workflows/release-cli.yml`'s `resolve-macos-runner` job. Each
build/smoke leg then indexes the map by `matrix.platform`, so routing is DATA
rather than a chain of ternaries inside the `runs-on:` key.

Why this exists
---------------
Before this, only the macOS legs were variable-driven; every other leg fell through
to `|| matrix.os` — a literal GitHub-hosted label. That was the *only* reason Linux
and Windows releases could not run on the self-hosted VMs that were already booted
and idle on the local pool. The VMs were there; the wiring was not.

The fluidity invariant
----------------------
With EVERY variable unset, this must reproduce the previous hard-coded routing
exactly (see `HOSTED`). Opting a leg onto the local pool is `gh variable set`;
reverting it is `gh variable unset`. Neither requires a code change, which is the
whole point — if the local pool is down, or you simply want GitHub's runners back,
it must be one command and take effect on the next tag.

A malformed variable fails LOUD. A release routed to a runner that does not exist
would sit queued forever, and a job that never starts is the single failure mode
this pipeline is worst at noticing — see `release_reconcile.py`'s STUCK_QUEUE.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

# The GitHub-hosted label each leg used before per-leg routing existed. These
# defaults ARE the fluidity invariant.
HOSTED: dict[str, object] = {
    "darwin-arm64": ["macos-15"],
    "darwin-x64": ["macos-15"],
    "linux-x64": "ubuntu-24.04",
    "linux-arm64": "ubuntu-24.04-arm",
    "windows-x64": "windows-latest",
    "windows-arm64": "windows-11-arm",
}

# Per-platform env vars, in priority order. The first non-empty one wins.
# The legacy macOS variables are honoured so an existing config keeps working.
SOURCES: dict[str, tuple[str, ...]] = {
    "darwin-arm64": ("DARWIN_ARM64", "LOCAL_MACOS", "NAMESPACE_JSON"),
    "darwin-x64": ("DARWIN_X64",),
    "linux-x64": ("LINUX_X64",),
    "linux-arm64": ("LINUX_ARM64",),
    "windows-x64": ("WINDOWS_X64",),
    "windows-arm64": ("WINDOWS_ARM64",),
}

# Release class labels. tartci's event-class-v2 supervisors boot a slot only for
# jobs carrying their class label, so a release job without one can only ride
# a gate runner that happens to be idle. Appending the class makes the release
# a first-class demand on hosts that serve it. On hosts that do NOT serve it the
# job would queue forever, so the append is gated behind an explicit opt-in.
CLASS_TOKENS_VAR = "PULP_RELEASE_CLASS_TOKENS"
CLASS_TOKENS_ENABLED = ("1", "true")
RELEASE_TAGGED_CLASS = "pulp-release-tagged"
RELEASE_PR_GATE_CLASS = "pulp-release-pr-gate"
# The legacy shared gate label is dropped when a class is added, exactly as
# build.yml does: event-class-v2 registrations do not advertise it, and GitHub
# matches a job only when ALL of its labels are on the runner.
LEGACY_GATE_LABEL = "pulp-gate-fast"
CLASS_LABELLED_LEGS = ("darwin-arm64", "darwin-x64")


def class_tokens_enabled(env: dict[str, str]) -> bool:
    """True only for the exact opt-in values. Anything else is a no-op, and a
    non-empty unrecognised value emits a `::notice::` naming the fix."""
    raw = env.get(CLASS_TOKENS_VAR)
    if raw is None or raw == "":
        return False
    if raw in CLASS_TOKENS_ENABLED:
        return True
    print(
        f"::notice::{CLASS_TOKENS_VAR}={raw!r} is not an opt-in value and was "
        f"ignored; release class labels stay OFF. Set it to exactly '1' or "
        f"'true' to enable, or unset it.",
        file=sys.stderr,
    )
    return False


def with_class_label(selector: object, label: str) -> object:
    """Append `label` to a self-hosted list selector (dropping the legacy gate
    label), mirroring build.yml's event-class rewrite. Hosted selectors (a bare
    string, or a list without `self-hosted`) pass through unchanged, and a
    selector that already carries the label is not duplicated."""
    if not isinstance(selector, list) or "self-hosted" not in selector:
        return selector
    if label in selector:
        return selector
    return [x for x in selector if x != LEGACY_GATE_LABEL] + [label]


def apply_class_label_raw(raw: str, label: str, env: dict[str, str]) -> str:
    """Shell-workflow entry point: take the selector JSON the workflow chose and
    return the one to dispatch. Disabled -> `raw` byte-for-byte."""
    if not class_tokens_enabled(env):
        return raw
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        return raw
    updated = with_class_label(parsed, label)
    if updated is parsed:
        return raw
    return json.dumps(updated, separators=(",", ":"))


def resolve(env: dict[str, str]) -> dict[str, object]:
    """platform -> runs-on. Pure: takes the environment, returns the map."""
    out: dict[str, object] = {}
    for platform, keys in SOURCES.items():
        chosen = None
        for key in keys:
            raw = (env.get(key) or "").strip()
            if not raw:
                continue
            try:
                chosen = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise SystemExit(
                    f"{key} is not valid JSON: {raw!r} ({exc}). Refusing to guess — "
                    f"a release routed to a runner that does not exist queues forever."
                )
            if not chosen:
                raise SystemExit(f"{key} resolved to an empty runs-on: {raw!r}")
            break
        out[platform] = HOSTED[platform] if chosen is None else chosen
    if class_tokens_enabled(env):
        for platform in CLASS_LABELLED_LEGS:
            out[platform] = with_class_label(out[platform], RELEASE_TAGGED_CLASS)
    return out


def describe(env: dict[str, str], resolved: dict[str, object]) -> list[str]:
    """Human-readable routing summary — printed into the job log."""
    lines = []
    for platform, keys in SOURCES.items():
        src = next((k for k in keys if (env.get(k) or "").strip()), None)
        where = "LOCAL/override" if src else "GitHub-hosted (default)"
        lines.append(f"  {platform:14} {where:24} via {src or '—':16} -> {resolved[platform]}")
    return lines


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--github-output", action="store_true",
                    help="emit map=/runs_on_json= lines for $GITHUB_OUTPUT")
    ap.add_argument("--apply-class-label", metavar="LABEL",
                    choices=(RELEASE_TAGGED_CLASS, RELEASE_PR_GATE_CLASS),
                    help="print --selector with LABEL applied when "
                         f"{CLASS_TOKENS_VAR} opts in (verbatim otherwise)")
    ap.add_argument("--selector", help="runs-on JSON for --apply-class-label")
    args = ap.parse_args(argv)

    env = dict(os.environ)
    if args.apply_class_label:
        if args.selector is None:
            ap.error("--apply-class-label requires --selector")
        print(apply_class_label_raw(args.selector, args.apply_class_label, env))
        return 0
    resolved = resolve(env)

    for line in describe(env, resolved):
        print(line, file=sys.stderr)

    if args.github_output:
        print("map=" + json.dumps(resolved))
        # The Namespace-profile step keys off how the macOS leg resolved.
        print("runs_on_json=" + json.dumps(resolved["darwin-arm64"]))
    else:
        print(json.dumps(resolved, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
