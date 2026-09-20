#!/usr/bin/env python3
"""Reject a generated ledger the merge driver resolved but nobody regenerated.

``docs/status/gpu-vellum-handoff.yaml`` and its receipt are regenerated from one
exact commit whenever main moves, so every branch that outlives a main move
collides on them. ``tools/scripts/gpu_ledger_merge_driver.py`` resolves that
collision to the identity ``regenerate-me`` instead of to a merged value,
because every value that *looks* like an identity is accepted somewhere: a
stale-but-ancestral pin satisfies the always-on provenance tier, so a driver
that computed the merge -- or ``merge=ours`` -- would hand Git a wrong answer it
would commit without a word.

An invalid value only helps if something invalid-aware is looking, and the
guards that already exist do not look here:

* ``gpu_handoff_pin_freshness.py`` rejects an identity-only ledger change, and
  a sentinel resolution is one. It fires here and its repair is the right one --
  restoring the merge base clears the sentinel and is what a clean merge of an
  untouched file would have produced -- but it cannot say *why* the file is that
  way, and it goes quiet the moment the branch also carries a real content
  change, which is the case that genuinely needs regenerating.
* ``conflict_marker_check.py`` looks for ``<<<<<<<``. The driver's entire
  purpose is that there are none.

Which leaves the required CI gate, twenty minutes downstream -- the roundtrip
the driver exists to remove. So this is the check that reads the merged bytes
and says no. It is a literal substring scan over two files: no imports beyond
the stdlib, no Git, no parsing, fast enough for the pre-push path.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# Must equal the value tools/scripts/gpu_ledger_merge_driver.py substitutes.
# The driver cannot import this module -- Git runs it as a bare shell command in
# the middle of a merge -- so the two literals are kept in step by a test rather
# than by construction.
SENTINEL = "regenerate-me"

LEDGERS = (
    Path("docs/status/gpu-vellum-handoff.yaml"),
    Path("docs/validation/gpu-handoff-provenance/receipt.json"),
)

# Both files are regenerated as a unit and the receipt binds itself to the
# ledger's exact bytes, so dropping --receipt repairs one and leaves the other
# describing bytes that no longer exist -- green here, red in CI.
REPAIR = "python3 tools/scripts/gpu_handoff_provenance.py write --source-commit HEAD --receipt"
VERIFY = "python3 tools/scripts/gpu_handoff_provenance.py check"
RESOLVE = "python3 tools/scripts/gpu_handoff_provenance.py resolve"

# .gitattributes routes both ledgers here by name. Git resolves that name
# against local config only, so the attribute is present in every clone while
# the driver behind it is present in none until a bootstrap registers it.
DRIVER_ATTRIBUTE = "pulp-gpu-ledger"
INSTALLER = "tools/scripts/install-githooks.sh"


def read_revision(root: Path, rev: str, relative: Path) -> str | None:
    """The file as of a revision, or None when that revision does not carry it."""

    completed = subprocess.run(
        ["git", "-C", str(root), "show", f"{rev}:{relative.as_posix()}"],
        capture_output=True, text=True, check=False,
    )
    return completed.stdout if completed.returncode == 0 else None


def sentinel_files(root: Path, rev: str | None = None) -> list[Path]:
    """Every ledger still carrying an unregenerated merge resolution.

    With ``rev``, read the committed object rather than the working tree. The
    push ships the commit, and a developer who regenerated but has not committed
    the result has a clean tree over a tip that still says ``regenerate-me`` --
    which the working-tree scan calls clean and CI rejects twenty minutes later,
    the exact roundtrip this guard exists to remove.
    """

    found = []
    for relative in LEDGERS:
        candidate = root / relative
        try:
            if rev is not None:
                text = read_revision(root, rev, relative)
                if text is None:
                    continue
            else:
                text = candidate.read_text(encoding="utf-8", errors="replace")
        except OSError:
            # A missing ledger is not this guard's business; the provenance
            # suite owns that. Saying nothing is correct, but only because
            # absence cannot hide a sentinel.
            continue
        if SENTINEL in text:
            found.append(relative)
    return found


def declared_driver_paths(root: Path) -> list[str]:
    """Paths .gitattributes routes to the driver, read from the file itself.

    Read rather than assumed: a check that hardcodes the routed paths keeps
    passing after somebody removes the attribute, which is the state it exists
    to notice.
    """

    try:
        text = (root / ".gitattributes").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    marker = f"merge={DRIVER_ATTRIBUTE}"
    return [
        line.split()[0]
        for line in text.splitlines()
        if marker in line and not line.lstrip().startswith("#") and line.split()
    ]


def driver_is_registered(root: Path) -> bool:
    """Whether Git in this checkout can resolve the name .gitattributes uses."""

    completed = subprocess.run(
        ["git", "-C", str(root), "config", "--get", f"merge.{DRIVER_ATTRIBUTE}.driver"],
        capture_output=True, text=True, check=False,
    )
    return completed.returncode == 0 and bool(completed.stdout.strip())


def report_unregistered_driver(root: Path, routed: list[str]) -> None:
    """Say that the repo's own merge automation is not installed here."""

    print("", file=sys.stderr)
    print(
        "gpu-ledger-sentinel: .gitattributes routes these files to the "
        f"{DRIVER_ATTRIBUTE!r} merge driver, but this checkout has no driver "
        "registered under that name:",
        file=sys.stderr,
    )
    for path in routed:
        print(f"    {path}", file=sys.stderr)
    print(
        "  Git does not error on a name it cannot resolve. It falls back to the",
        file=sys.stderr,
    )
    print(
        "  ordinary text merge, in silence — so the generated-ledger collision",
        file=sys.stderr,
    )
    print(
        "  comes back conflicted on every sweep, and the automation that was",
        file=sys.stderr,
    )
    print(
        "  supposed to end it looks installed because the attribute is there.",
        file=sys.stderr,
    )
    print(
        "  A checkout bootstrapped before the driver landed is the usual cause.",
        file=sys.stderr,
    )
    print(f"  Repair:  {INSTALLER}", file=sys.stderr)
    print(f"  Then:    {RESOLVE}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument(
        "--rev",
        help="read the ledgers from this revision instead of the working tree",
    )
    parser.add_argument("--mode", choices=("report", "hint"), default="report")
    parser.add_argument(
        "--skip-registration",
        action="store_true",
        help="scan only for the sentinel; do not check the local driver registration",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()

    # Two independent claims about the same mechanism, and each is invisible to
    # the other's evidence. A sentinel says the driver ran and nobody
    # regenerated; an unregistered driver says it never ran at all, and that one
    # produces no sentinel to find.
    failed = False
    if not args.skip_registration:
        routed = declared_driver_paths(root)
        if routed and not driver_is_registered(root):
            report_unregistered_driver(root, routed)
            failed = args.mode != "hint"

    hits = sentinel_files(root, args.rev)
    if not hits:
        return 1 if failed else 0

    where = "in the commit being pushed" if args.rev else "in the working tree"
    print("", file=sys.stderr)
    print(
        f"gpu-ledger-sentinel: a ledger merge was resolved but never regenerated ({where}):",
        file=sys.stderr,
    )
    for hit in hits:
        print(f"    {hit}", file=sys.stderr)
    print(
        f"  Every pinned identity there reads {SENTINEL!r}. The merge driver writes",
        file=sys.stderr,
    )
    print(
        "  that on purpose: a plausible-looking merged pin would be committed in",
        file=sys.stderr,
    )
    print("  silence, and this cannot be.", file=sys.stderr)
    print("  If this resolution is the only thing the branch did to these files, do", file=sys.stderr)
    print("  not regenerate: restore the merge base instead. That clears the sentinel", file=sys.stderr)
    print("  and leaves main's own identities in place, and regenerating here would", file=sys.stderr)
    print("  only produce the identity churn gpu_handoff_pin_freshness.py rejects.", file=sys.stderr)
    print("    git restore --source=$(git merge-base origin/main HEAD) -- \\", file=sys.stderr)
    for index, relative in enumerate(LEDGERS):
        trailer = " \\" if index + 1 < len(LEDGERS) else ""
        print(f"        {relative}{trailer}", file=sys.stderr)
    print("  If the branch also edits the ledger's content, regenerate — and land the", file=sys.stderr)
    print("  result as its own commit, because amending a commit that touches a", file=sys.stderr)
    print("  pinned path re-stales the row it just repaired.", file=sys.stderr)
    print(f"  Repair:  {REPAIR}", file=sys.stderr)
    print(f"  Verify:  {VERIFY}", file=sys.stderr)
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    raise SystemExit(main())
