#!/usr/bin/env python3
"""Update managed Pulp SDK consumers to a new SDK version, and print the
per-consumer republish runbook.

Companion to the min-OS sweep (sdk_consumer_sweep.py): the sweep proves a new SDK
still BUILDS every downstream consumer; this bumps each consumer's SDK pin and,
when asked, opens the update PRs — then prints the exact steps to rebuild and
republish each packaged demo.

Two operations over the same registry (planning/sdk-consumers/consumers.yaml):

  update           Rewrite each buildable consumer's SDK pin to <version>.
                   DRY-RUN by default — prints the per-repo plan and changes
                   nothing. Pass --open-prs to actually clone each repo, apply
                   the edit on a branch, commit, push, and open a PR.

  publish-runbook  Print the rebuild + package + publish command sequence for
                   every consumer that ships a package. Prints only: it never
                   runs a build, signs anything, or touches a release.

Why publishing is a runbook, not a button: each packaged demo has its own
signing identity, packaging script, and release process, and publishing mutates
public releases. Auto-running that across repos can't be generalized safely, so
this emits the reviewable hand-off instead of firing it. The steps are gated
behind a human.

Typical use:

    # See what a bump to 0.640.0 would change across every consumer (no writes):
    sdk_consumer_update.py update --to 0.640.0

    # Actually open the pin-bump PRs (clones, branches, pushes, gh pr create):
    sdk_consumer_update.py update --to 0.640.0 --open-prs

    # Print the republish runbook for the packaged demos:
    sdk_consumer_update.py publish-runbook --to 0.640.0
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CONSUMERS = REPO / "planning" / "sdk-consumers" / "consumers.yaml"

# How consumers pin the Pulp SDK. Each rule is (label, filename, regex) where the
# regex's first group is everything up to and including the version prefix and its
# second group is the semver itself. Only concrete X.Y.Z pins are rewritten — a
# floating `sdk_version = "latest"` is left alone (the semver group won't match).
#
# Every rule MUST be scoped to the Pulp SDK specifically. The FetchContent rule is
# the trap: a bare `GIT_TAG vX.Y.Z` also matches every OTHER FetchContent_Declare
# in the file (Catch2, fmt, json, …), so it is anchored to a declare block that
# names `pulp` — `FetchContent_Declare( … pulp … GIT_TAG v<ver>)`. `[^)]*?` keeps
# the match inside one declare's parentheses (a git URL contains no `)`), so an
# unrelated dependency's tag is never touched.
SDK_PIN_RULES = [
    ("pulp.toml sdk_version", "pulp.toml",
     re.compile(r'(?m)^(\s*sdk_version\s*=\s*")(\d+\.\d+\.\d+)(")')),
    ("find_package(Pulp)", "CMakeLists.txt",
     re.compile(r'(find_package\s*\(\s*Pulp\s+)(\d+\.\d+\.\d+)')),
    ("FetchContent GIT_TAG", "CMakeLists.txt",
     re.compile(r'(?is)(FetchContent_Declare\s*\([^)]*?\bpulp\b[^)]*?'
                r'GIT_TAG\s+v?)(\d+\.\d+\.\d+)')),
]

SEMVER_RE = re.compile(r'^\d+\.\d+\.\d+$')


def short_name(repo: str) -> str:
    """'danielraffel/pulp-gpu-nam' -> 'pulp-gpu-nam'."""
    return repo.split("/", 1)[-1]


def load_yaml(path: Path) -> dict:
    try:
        import yaml
    except ModuleNotFoundError:
        raise SystemExit(
            "PyYAML is required to parse the consumer registry.\n"
            "Install it with:  python3 -m pip install pyyaml")
    return yaml.safe_load(path.read_text()) or {}


def detect_pins(text: str, filename: str) -> list[tuple[str, str]]:
    """Return [(rule_label, current_version), ...] for every SDK pin found in
    `text` (a file named `filename`). A file may carry more than one pin."""
    found: list[tuple[str, str]] = []
    for label, fname, rx in SDK_PIN_RULES:
        if fname != filename:
            continue
        for m in rx.finditer(text):
            found.append((label, m.group(2)))
    return found


def rewrite_pins(text: str, filename: str,
                 new_version: str) -> tuple[str, list[tuple[str, str, str]]]:
    """Rewrite every SDK pin in `text` to `new_version`. Returns
    (new_text, [(rule_label, old_version, new_version), ...] for pins that
    actually changed). A pin already at new_version is not counted as a change."""
    if not SEMVER_RE.match(new_version):
        raise ValueError(f"not a semver: {new_version!r}")
    changes: list[tuple[str, str, str]] = []
    out = text
    for label, fname, rx in SDK_PIN_RULES:
        if fname != filename:
            continue

        def _sub(m: re.Match, _label=label) -> str:
            old = m.group(2)
            if old != new_version:
                changes.append((_label, old, new_version))
            return m.group(1) + new_version + (m.group(3) if m.re.groups >= 3 else "")

        out = rx.sub(_sub, out)
    return out, changes


# Files a consumer may pin the SDK in (checked in this order for reporting).
PIN_FILES = ["pulp.toml", "CMakeLists.txt"]


def plan_repo_update(checkout: Path,
                     new_version: str) -> dict:
    """Compute the per-file pin changes for one already-cloned consumer, without
    writing anything. Returns {file: {current: [...], changes: [(label,old,new)]}}."""
    plan: dict = {}
    for fname in PIN_FILES:
        f = checkout / fname
        if not f.exists():
            continue
        text = f.read_text()
        current = detect_pins(text, fname)
        _, changes = rewrite_pins(text, fname, new_version)
        if current or changes:
            plan[fname] = {"current": current, "changes": changes}
    return plan


def apply_repo_update(checkout: Path, new_version: str) -> list[str]:
    """Write the pin rewrites into the checkout. Returns the list of files that
    actually changed on disk."""
    changed: list[str] = []
    for fname in PIN_FILES:
        f = checkout / fname
        if not f.exists():
            continue
        text = f.read_text()
        new_text, changes = rewrite_pins(text, fname, new_version)
        if changes and new_text != text:
            f.write_text(new_text)
            changed.append(fname)
    return changed


def buildable_consumers(consumers: dict, only: set[str] | None) -> list[dict]:
    """Consumers with buildable source: everything except registry entries marked
    status.state == 'not-applicable' (README/PKG-only release mirrors)."""
    out = []
    for entry in consumers.get("repos", []) or []:
        name = short_name(entry.get("repo", ""))
        if only is not None and name not in only:
            continue
        if (entry.get("status", {}) or {}).get("state") == "not-applicable":
            continue
        out.append(entry)
    return out


def _run(cmd: list[str], cwd: Path | None = None) -> tuple[int, str]:
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr)


def open_update_pr(entry: dict, checkout: Path, new_version: str,
                   changed_files: list[str]) -> tuple[bool, str]:
    """Branch + commit + push + open a PR for an applied pin update. Uses SSH for
    the clone/push (agent git is SSH-routed) and gh for the PR."""
    repo = entry.get("repo", "")
    branch = f"chore/sdk-{new_version}"
    # `-B` (reset if it exists) and `--force-with-lease` keep a re-run idempotent:
    # a fresh clone re-creates the same branch and re-pushes it rather than dying on
    # a non-fast-forward left behind by an earlier run that pushed but failed to open
    # the PR. `--force-with-lease` still refuses to clobber an unexpected remote tip.
    steps = [
        (["git", "checkout", "-B", branch], "branch"),
        (["git", "add", *changed_files], "stage"),
        (["git", "commit", "-m", f"chore: bump Pulp SDK to {new_version}"], "commit"),
        (["git", "push", "--force-with-lease", "-u", "origin", branch], "push"),
        (["gh", "pr", "create", "--repo", repo, "--head", branch,
          "--title", f"chore: bump Pulp SDK to {new_version}",
          "--body", f"Update the pinned Pulp SDK to {new_version}.\n\n"
                    f"Opened by the managed SDK consumer update "
                    f"(tools/scripts/sdk_consumer_update.py)."], "pr"),
    ]
    for cmd, what in steps:
        rc, out = _run(cmd, cwd=checkout)
        if rc != 0:
            return False, f"{what} failed: {out.strip().splitlines()[-1] if out.strip() else '?'}"
    return True, out.strip().splitlines()[-1] if out.strip() else "opened"


def clone(repo: str, dest: Path) -> tuple[bool, str]:
    if dest.exists():
        # A pre-existing checkout in --workdir. Verify it is actually THIS repo
        # and refresh it to the remote tip before we rewrite pins / open a PR from
        # it — never bump a stale or foreign tree and push it as an SDK update.
        rc, out = _run(["git", "remote", "get-url", "origin"], cwd=dest)
        url = out.strip().splitlines()[-1].strip() if rc == 0 and out.strip() else ""
        slug = repo.split("/", 1)[-1].removesuffix(".git")
        if rc != 0 or slug not in url:
            return False, f"existing checkout is not {repo} (origin={url or 'unknown'})"
        rc, _ = _run(["git", "fetch", "--depth", "1", "origin", "HEAD"], cwd=dest)
        if rc != 0:
            return False, "reused checkout: fetch failed (stale; refusing to update it)"
        _run(["git", "reset", "--hard", "FETCH_HEAD"], cwd=dest)
        return True, "reused + refreshed existing checkout"
    rc, out = _run(["git", "clone", "--depth", "1",
                    f"git@github.com:{repo}.git", str(dest)])
    return rc == 0, (out.strip().splitlines()[-1] if out.strip() else "clone failed")


def publish_runbook(consumers: dict, new_version: str | None,
                    only: set[str] | None) -> str:
    """Print the rebuild + package + publish steps for every consumer that ships
    a package (has latest_release.package_assets). Human runbook only."""
    ver = new_version or "<new-version>"
    lines = [
        f"Republish runbook — rebuild the packaged demos against SDK {ver} and "
        "cut new releases.",
        "",
        "This lists the steps; it runs none of them. Each repo signs with its own "
        "identity and cuts its own release — those stay behind a human.",
        "",
    ]
    any_packaged = False
    for entry in consumers.get("repos", []) or []:
        name = short_name(entry.get("repo", ""))
        if only is not None and name not in only:
            continue
        rel = entry.get("latest_release") or {}
        assets = rel.get("package_assets") or []
        if not assets:
            continue
        any_packaged = True
        state = (entry.get("status", {}) or {}).get("state")
        mirror = state == "not-applicable"
        lines.append(f"### {name}  (latest {rel.get('tag', '?')} → {', '.join(assets)})")
        if mirror:
            lines.append("  ⚠ README/PKG-only release mirror — no public build source.")
            lines.append("     Rebuild from the private package source, then:")
        else:
            lines.append(f"  1. Land the SDK-{ver} pin PR "
                         f"(sdk_consumer_update.py update --to {ver} --open-prs).")
            lines.append(f"  2. git clone git@github.com:{entry.get('repo')}.git && cd {name}")
            lines.append("  3. Build Release against the new SDK "
                         "(pulp build, or the repo's package.sh).")
        lines.append(f"  {'  ' if mirror else '  4.'} Package + sign + notarize with the "
                     "repo's identity (pulp ship package / package.sh).")
        lines.append(f"  {'  ' if mirror else '  5.'} Publish: "
                     f"gh release create v{ver} <asset>.pkg --repo {entry.get('repo')}.")
        lines.append("")
    if not any_packaged:
        lines.append("(No packaged consumers matched.)")
    return "\n".join(lines)


def cmd_update(args, consumers: dict) -> int:
    only = set(s.split("/", 1)[-1] for s in args.only.split(",")) if args.only else None
    targets = buildable_consumers(consumers, only)
    if not targets:
        print("No buildable consumers matched.", file=sys.stderr)
        return 2

    # A dry run still has to clone each consumer to read its real pins, but it must
    # not litter /tmp with those clones the way a `--keep`-less run would. Clean up
    # only the temp dir we minted ourselves; an explicit --workdir is the user's.
    created_temp = args.workdir is None
    workdir = args.workdir or Path(
        __import__("tempfile").mkdtemp(prefix="pulp-sdk-update-"))
    workdir.mkdir(parents=True, exist_ok=True)

    print(f"{'DRY-RUN — ' if not args.open_prs else ''}update {len(targets)} "
          f"consumer(s) to SDK {args.to}\n")
    any_change = False
    try:
        for entry in targets:
            name = short_name(entry.get("repo", ""))
            checkout = workdir / name
            ok, msg = clone(entry.get("repo", ""), checkout)
            if not ok:
                print(f"  {name:32} clone✗  {msg}")
                continue
            plan = plan_repo_update(checkout, args.to)
            changed_here = [(f, d) for f, d in plan.items() if d["changes"]]
            if not plan:
                print(f"  {name:32} —       no SDK pin found "
                      "(installed-SDK / floating / no pin)")
                continue
            if not changed_here:
                print(f"  {name:32} ok      already at {args.to}")
                continue
            any_change = True
            for fname, d in changed_here:
                for label, old, new in d["changes"]:
                    print(f"  {name:32} {fname:15} {label}: {old} → {new}")
            if args.open_prs:
                files = apply_repo_update(checkout, args.to)
                pr_ok, pr_msg = open_update_pr(entry, checkout, args.to, files)
                print(f"  {name:32} {'PR ✓' if pr_ok else 'PR ✗'}  {pr_msg}")
    finally:
        if created_temp:
            shutil.rmtree(workdir, ignore_errors=True)

    if not args.open_prs and any_change:
        print(f"\nDry-run only. Re-run with --open-prs to open the update PRs.")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    up = sub.add_parser("update", help="rewrite each consumer's SDK pin (dry-run default)")
    up.add_argument("--to", required=True, metavar="VERSION",
                    help="target SDK version, e.g. 0.640.0")
    up.add_argument("--only", help="comma-separated repo names (short or owner/name)")
    up.add_argument("--open-prs", action="store_true",
                    help="actually clone/edit/branch/commit/push and open a PR per repo "
                         "(omit for a dry-run plan)")
    up.add_argument("--workdir", type=Path, help="clone location (default: a temp dir)")
    up.add_argument("--consumers", type=Path, default=DEFAULT_CONSUMERS)

    pr = sub.add_parser("publish-runbook",
                        help="print the rebuild+package+publish steps (prints only)")
    pr.add_argument("--to", metavar="VERSION", help="target SDK version (for the printed steps)")
    pr.add_argument("--only", help="comma-separated repo names")
    pr.add_argument("--consumers", type=Path, default=DEFAULT_CONSUMERS)

    args = ap.parse_args(argv)

    if not args.consumers.exists():
        print(f"consumers registry not found: {args.consumers}\n"
              "(initialize the planning submodule: git submodule update --init planning)",
              file=sys.stderr)
        return 2
    consumers = load_yaml(args.consumers)

    if args.command == "update":
        if not SEMVER_RE.match(args.to):
            print(f"--to must be a semver like 0.640.0, got {args.to!r}", file=sys.stderr)
            return 2
        return cmd_update(args, consumers)

    if args.command == "publish-runbook":
        only = set(s.split("/", 1)[-1] for s in args.only.split(",")) if args.only else None
        print(publish_runbook(consumers, args.to, only))
        return 0

    return 2


if __name__ == "__main__":
    sys.exit(main())
