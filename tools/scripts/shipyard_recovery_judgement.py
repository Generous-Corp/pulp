#!/usr/bin/env python3
"""Decide whether a fenced repair may land autonomously, by WHAT it changed.

The existing fences count and bound: exact head, epoch, fingerprint,
force-with-lease, changed-file count, patch bytes, control-plane paths. On
2026-08-17 every one of them held while the repair lane, asked to satisfy
``static_assert(std::is_trivially_copyable_v<ParamValue>)``, removed all five
``std::atomic`` members from ``ParamValue`` and rewrote its documentation to
make thread-safety the caller's problem. ``ParamValue`` is the audio-thread to
UI-thread primitive of a real-time framework, so the repair introduced data
races on the audio thread — and it would have gone green, because the assertion
passes once the atomics are gone.

Nothing already in the fence reads meaning. This module is that check.

Two independent tests. Either one escalates to ``needs_human``:

1. **Surface.** An ALLOWLIST of paths a repair may touch autonomously, not a
   denylist of paths it may not. A denylist fails open on the surface nobody
   thought to name — and the surfaces that matter here are not only ``core/``:
   a repair that edited ``.github/workflows/build.yml`` could remove the gate
   that was failing, and a repair that edited ``tools/cmake`` could disable a
   check. An allowlist fails closed on all of them, including ones added later.

2. **Invariant removal.** Surface alone is not sufficient, and assuming it was
   would leave the same defect one directory over. A test-only diff that merely
   deletes ``REQUIRE`` lines satisfies any surface rule while being the same
   failure: the model can make a check pass either by making the claim true or
   by deleting the claim, and on 2026-08-17 it chose to delete. This test
   escalates a repair whose net effect is to remove invariant-bearing
   constructs.

Note what test 2 deliberately does NOT do: it does not try to tell a good
deletion from a bad one. The correct repair on 2026-08-17 was to delete the
false assertion, and this check escalates that too. That is the intended
behaviour. The lane's job is to never land a catastrophic change unattended,
not to land every correct change unattended; "a human deletes the assertion" is
a fine outcome and "a model silently deletes the invariant" is not.

This file sits under ``tools/scripts/shipyard_recovery_``, which is already in
``FORBIDDEN_PREFIXES`` in ``shipyard_recovery_repair.py``. A fenced repair model
therefore cannot edit the check that constrains it — the same reason
``shipyard_recovery_result_check.py`` lives here. ``test_shipyard_recovery_
judgement.py`` asserts that containment rather than trusting it.

Exit codes are distinct so a workflow can branch without parsing prose:
``0`` autonomous repair permitted, ``3`` escalate to ``needs_human``,
``1`` the inputs were unusable.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


#: The ONLY paths a repair may touch and still land without a human. Widening
#: this is a deliberate act with a test, which is the point of an allowlist.
#: Test code is here because a broken test is the common autonomous-repair case
#: and its blast radius is bounded by test 2 below.
AUTONOMOUS_PATH_PREFIXES: tuple[str, ...] = ("test/",)

#: Human-readable surface names, longest prefix first so the message names the
#: most specific surface rather than the first one that happens to match.
SURFACE_LABELS: tuple[tuple[str, str], ...] = (
    ("core/", "framework runtime (core)"),
    ("apple/", "Apple platform source"),
    ("android/", "Android platform source"),
    ("inspect/", "inspector / control SDK"),
    ("ship/", "signing and distribution"),
    (".github/workflows/", "CI definition"),
    ("tools/cmake/", "build system"),
    ("tools/ci/", "CI tooling"),
    ("tools/scripts/", "repo tooling"),
    ("tools/", "tooling"),
    ("examples/", "example project"),
    ("docs/", "documentation"),
    ("CMakeLists.txt", "root build definition"),
)

#: Constructs whose REMOVAL is the destructive way to satisfy a failing check.
#: Deliberately narrow: each names something whose absence changes runtime or
#: test meaning, not merely style. `const` and `noexcept` are excluded because
#: they churn for legitimate reasons and would drown the signal.
INVARIANT_MARKERS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("static assertion", re.compile(r"\b(?:static_assert|_Static_assert)\b")),
    ("atomic", re.compile(r"\bstd::atomic\b|\bstd::memory_order\w*")),
    ("lock", re.compile(r"\bstd::(?:mutex|recursive_mutex|shared_mutex|lock_guard"
                        r"|scoped_lock|unique_lock|shared_lock)\b")),
    ("test assertion", re.compile(r"\b(?:REQUIRE|CHECK|REQUIRE_FALSE|CHECK_FALSE"
                                  r"|REQUIRE_THROWS\w*|CHECK_THROWS\w*|REQUIRE_NOTHROW"
                                  r"|CHECK_NOTHROW|STATIC_REQUIRE)\s*\(")),
    ("runtime assertion", re.compile(r"(?<![\w.])assert\s*\(")),
)

#: GitHub caps a commit-status description at 140 characters. The reason has to
#: survive the trip to the exact head or the escalation is unexplained where a
#: reader will actually look for it.
STATUS_DESCRIPTION_LIMIT = 140


@dataclass
class Verdict:
    """Whether autonomous landing is permitted, and why not when it is not."""

    autonomous: bool
    reason: str
    detail: dict = field(default_factory=dict)

    @property
    def outcome(self) -> str:
        return "autonomous" if self.autonomous else "needs_human"

    def status_description(self) -> str:
        text = f"judgement: {self.outcome} — {self.reason}"
        if len(text) <= STATUS_DESCRIPTION_LIMIT:
            return text
        return text[: STATUS_DESCRIPTION_LIMIT - 1] + "…"

    def to_dict(self) -> dict:
        return {
            "outcome": self.outcome,
            "autonomous": self.autonomous,
            "reason": self.reason,
            "status_description": self.status_description(),
            **self.detail,
        }


def _surface_of(path: str) -> str:
    for prefix, label in SURFACE_LABELS:
        if path == prefix or path.startswith(prefix):
            return label
    return "unclassified surface"


def classify_paths(paths: list[str]) -> Verdict:
    """Escalate unless EVERY changed path is on the autonomous allowlist."""
    normalized = sorted({p.strip() for p in paths if p.strip()})
    if not normalized:
        return Verdict(False, "repair produced no changed files", {"paths": []})
    outside = [p for p in normalized
               if not any(p.startswith(prefix) for prefix in AUTONOMOUS_PATH_PREFIXES)]
    if outside:
        first = outside[0]
        return Verdict(
            False,
            f"{len(outside)} path(s) outside the autonomous surface, "
            f"first: {first} ({_surface_of(first)})",
            {"paths_outside_autonomous_surface": outside},
        )
    return Verdict(True, f"all {len(normalized)} path(s) within {AUTONOMOUS_PATH_PREFIXES}",
                   {"paths": normalized})


def count_markers(patch_text: str) -> dict[str, dict[str, int]]:
    """Count invariant markers on added and removed diff lines.

    Diff headers are skipped: `---`/`+++` are file markers, not content, and
    counting them would let a rename shift the balance.
    """
    counts = {name: {"removed": 0, "added": 0} for name, _ in INVARIANT_MARKERS}
    for line in patch_text.splitlines():
        if line.startswith("+++") or line.startswith("---"):
            continue
        if line.startswith("-"):
            side = "removed"
        elif line.startswith("+"):
            side = "added"
        else:
            continue
        body = line[1:]
        for name, pattern in INVARIANT_MARKERS:
            found = len(pattern.findall(body))
            if found:
                counts[name][side] += found
    return counts


def assess_invariants(patch_text: str) -> Verdict:
    """Escalate a repair whose net effect is to remove invariant constructs."""
    counts = count_markers(patch_text)
    losses = {
        name: data["removed"] - data["added"]
        for name, data in counts.items()
        if data["removed"] > data["added"]
    }
    if losses:
        worst = max(losses, key=lambda name: losses[name])
        total = sum(losses.values())
        return Verdict(
            False,
            f"removes {total} invariant construct(s) net; worst: {worst} "
            f"(-{losses[worst]})",
            {"invariant_losses": losses, "invariant_counts": counts},
        )
    return Verdict(True, "removes no invariant constructs on net",
                   {"invariant_counts": counts})


def judge(paths: list[str], patch_text: str) -> Verdict:
    """Both tests must pass. Surface is reported first because it is cheaper
    to act on: a reviewer who sees "touched core/" needs no diff to agree."""
    surface = classify_paths(paths)
    if not surface.autonomous:
        return surface
    invariants = assess_invariants(patch_text)
    if not invariants.autonomous:
        return invariants
    return Verdict(
        True,
        f"{surface.reason}; {invariants.reason}",
        {**surface.detail, **invariants.detail},
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--changed-paths", type=Path, required=True)
    parser.add_argument("--patch", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    try:
        paths = args.changed_paths.read_text(encoding="utf-8").splitlines()
        patch_text = args.patch.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        print(f"judgement: unusable inputs: {error}", file=sys.stderr)
        return 1

    verdict = judge(paths, patch_text)
    payload = json.dumps(verdict.to_dict(), indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(payload + "\n", encoding="utf-8")
    print(payload)
    if not verdict.autonomous:
        print(f"judgement: escalating to needs_human — {verdict.reason}", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
