#!/usr/bin/env python3
"""Reclaim build directories, artifact directories and finished lane clones.

Agent lanes live in plain clones under a root such as
`<worktrees>/lanes/<lane>/<clone>`. `git worktree` based reapers
(`clean_worktree_builds.sh`, `clean_build_cov.sh`) never see them, so each
finished lane leaves tens of gigabytes behind. This tool covers that case.

It is a DRY RUN unless `--yes` is passed. It lists, per lane:

  * build directories at the top level of a checkout (`build`, `build-*`,
    `build_*`, `_build`);
  * large untracked artifact directories or files at the top level of a
    checkout (package outputs, extracted SDK copies; see ARTIFACT_PATTERNS);
  * whole lane directories that are finished.

A build or artifact entry is reclaimable only when ALL of these hold:

  1. the checkout's HEAD is an ancestor of the remote default branch, or a
     pull request containing HEAD is merged or closed and none is open
     (`ghread` for Generous-Corp, `ghapp` for danielraffel; cached);
  2. nothing in the checkout was modified in the last `--idle-hours` (48);
  3. no live process has a cwd or open file under the checkout;
  4. the tracked tree is clean;
  5. the entry holds no tracked file and carries no `.reaper-keep` marker.

A whole lane is reclaimable only when every checkout in it is merged (rule 1),
clean (rule 4, with no stray untracked files), has no unpushed commits and no
stash, the whole lane has been idle for `--lane-idle-days` (7), nothing is
open under it, no unassessed git repository hides inside it, and no linked
worktree is involved.

Any measurement that cannot be made (no `lsof`, a failed API call, an
unparseable remote) blocks reclamation instead of passing it. Removal renames
the verified path to a `.reaping-*` sibling first and then deletes only that
renamed path, after re-verifying every mutable gate.

Usage:
    clean_lane_builds.py --root /path/to/lanes             # dry run
    clean_lane_builds.py --root /path/to/lanes --yes       # act
"""

from __future__ import annotations

import argparse
import bisect
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_IDLE_HOURS = 48.0
DEFAULT_LANE_IDLE_DAYS = 7.0
DEFAULT_MIN_ARTIFACT_MB = 100.0
DEFAULT_CACHE_TTL_HOURS = 6.0
KEEP_MARKER = ".reaper-keep"
REAPING_PREFIX = ".reaping-"
EXIT_INSTRUMENT_UNAVAILABLE = 3

BUILD_PATTERNS = ("build", "build-*", "build_*", "_build")
ARTIFACT_PATTERNS = (
    "*.pkg", "*.dmg", "*.xcarchive", "*.zip", "*.tar", "*.tar.gz", "*.tgz",
    "*.tar.xz", "*sdk*", "pkgroot*", "*-pkgroot", "dist", "out",
    "nodal-native-*", "artifacts", "*-rebuild-*",
)

PROVIDERS = {"generous-corp": "PULP_REAPER_GHREAD", "danielraffel": "PULP_REAPER_GHAPP"}
PROVIDER_DEFAULTS = {"PULP_REAPER_GHREAD": "ghread", "PULP_REAPER_GHAPP": "ghapp"}


class InstrumentUnavailable(RuntimeError):
    """A required measurement (lsof) cannot be trusted; nothing may be reaped."""


def git_env() -> dict[str, str]:
    env = dict(os.environ)
    # Reading status must not refresh the index: that would stamp every
    # checkout as recently modified and defeat the idle gate.
    env["GIT_OPTIONAL_LOCKS"] = "0"
    return env


def run(cmd: list[str], cwd: str | Path | None = None, timeout: float = 600) -> tuple[int, str]:
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           timeout=timeout, env=git_env())
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 127, str(exc)
    return p.returncode, p.stdout


def git(path: Path, *args: str) -> tuple[int, str]:
    return run(["git", "-C", str(path), *args])


# ---------------------------------------------------------------------------
# Live process detection
# ---------------------------------------------------------------------------

class LiveFiles:
    """One system-wide `lsof` snapshot, queried by path prefix."""

    def __init__(self) -> None:
        exe = os.environ.get("PULP_REAPER_LSOF", "lsof")
        if shutil.which(exe) is None and not os.path.isfile(exe):
            raise InstrumentUnavailable(f"{exe} not found")
        try:
            p = subprocess.run([exe, "-nP", "-w", "-F", "pn"], capture_output=True,
                               text=True, timeout=600)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise InstrumentUnavailable(f"lsof failed: {exc}") from exc
        pids: set[int] = set()
        paths: list[str] = []
        for line in p.stdout.splitlines():
            if line.startswith("p") and line[1:].isdigit():
                pids.add(int(line[1:]))
            elif line.startswith("n/"):
                paths.append(line[1:])
        # Control: lsof must list this very process. A listing that omits it
        # is blind (permissions, a stub, a truncated run), and an empty answer
        # from a blind instrument must not read as "nothing is running".
        if os.getpid() not in pids:
            raise InstrumentUnavailable("lsof output does not include this process")
        self.paths = sorted(set(paths))

    def busy(self, target: Path) -> bool:
        for t in {str(target), os.path.realpath(target)}:
            i = bisect.bisect_left(self.paths, t)
            while i < len(self.paths) and self.paths[i].startswith(t):
                s = self.paths[i]
                if s == t or s[len(t)] == "/":
                    return True
                i += 1
        return False


# ---------------------------------------------------------------------------
# Pull request state
# ---------------------------------------------------------------------------

class PrStates:
    """Pull request state for a commit, cached on disk and in memory."""

    def __init__(self, cache_file: Path | None, ttl_hours: float) -> None:
        self.cache_file = cache_file
        self.ttl = ttl_hours * 3600
        self.mem: dict[str, str] = {}
        self.disk: dict[str, dict] = {}
        self.calls = 0
        if cache_file and cache_file.is_file():
            try:
                self.disk = json.loads(cache_file.read_text())
            except (OSError, ValueError):
                self.disk = {}

    def lookup(self, owner: str, repo: str, sha: str) -> tuple[str, str]:
        """Return (state, detail); state is merged|closed|open|none|error."""
        key = f"{owner}/{repo}@{sha}".lower()
        if key in self.mem:
            return self.mem[key], "cached"
        hit = self.disk.get(key)
        if hit and time.time() - hit.get("at", 0) < self.ttl:
            self.mem[key] = hit["state"]
            return hit["state"], "cached"
        env_name = PROVIDERS.get(owner.lower())
        if env_name is None:
            return "error", f"no PR provider for owner {owner}"
        exe = os.environ.get(env_name, PROVIDER_DEFAULTS[env_name])
        if shutil.which(exe) is None and not os.path.isfile(exe):
            return "error", f"{exe} not found"
        self.calls += 1
        rc, out = run([exe, "api", f"repos/{owner}/{repo}/commits/{sha}/pulls"], timeout=120)
        try:
            prs = json.loads(out)
        except ValueError:
            prs = None
        if rc != 0 or not isinstance(prs, list):
            return "error", f"{exe} api failed (rc={rc})"
        state = "none"
        for pr in prs:
            if not isinstance(pr, dict):
                return "error", "unexpected API payload"
            if pr.get("state") == "open":
                state = "open"
                break
            if pr.get("merged_at"):
                state = "merged"
            elif pr.get("state") == "closed" and state != "merged":
                state = "closed"
        self.mem[key] = state
        if state in ("merged", "closed"):
            self.disk[key] = {"state": state, "at": time.time()}
        return state, "api"

    def save(self) -> None:
        if not self.cache_file:
            return
        try:
            self.cache_file.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.cache_file.with_suffix(".tmp")
            tmp.write_text(json.dumps(self.disk))
            os.replace(tmp, self.cache_file)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Filesystem measurements
# ---------------------------------------------------------------------------

def modified_since(path: Path, cutoff: float) -> bool | None:
    """True when anything under path is newer than cutoff; None if unknown."""
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(cutoff))
    try:
        p = subprocess.run(["find", os.path.realpath(path), "-newermt", stamp,
                            "-print", "-quit"], capture_output=True, text=True,
                           timeout=1800)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if p.stdout.strip():
        return True
    return False if p.returncode == 0 else None


def disk_bytes(path: Path) -> int:
    rc, out = run(["du", "-sk", str(path)], timeout=1800)
    try:
        return int(out.split()[0]) * 1024 if out.strip() else 0
    except (ValueError, IndexError):
        return 0


def is_checkout(path: Path) -> bool:
    return (path / ".git").exists() and not path.is_symlink()


def find_checkouts(root: Path, max_depth: int = 3) -> list[Path]:
    """Directories holding a `.git`, not descending into a checkout."""
    found: list[Path] = []

    def walk(d: Path, depth: int) -> None:
        try:
            entries = sorted(os.scandir(d), key=lambda e: e.name)
        except OSError:
            return
        for e in entries:
            if e.name.startswith(REAPING_PREFIX) or e.is_symlink():
                continue
            if not e.is_dir(follow_symlinks=False):
                continue
            p = Path(e.path)
            if is_checkout(p):
                found.append(p)
            elif depth < max_depth:
                walk(p, depth + 1)

    walk(root, 1)
    return found


def stray_git_dirs(lane: Path, known: list[Path], max_depth: int = 8) -> list[Path]:
    """Git repositories inside a lane that the checkout scan did not assess."""
    knownset = {str(k) for k in known}
    stray: list[Path] = []

    def walk(d: Path, depth: int) -> None:
        if depth > max_depth:
            return
        try:
            entries = list(os.scandir(d))
        except OSError:
            return
        for e in entries:
            if e.is_symlink() or not e.is_dir(follow_symlinks=False):
                continue
            p = Path(e.path)
            if str(p) in knownset:
                continue
            if is_checkout(p):
                stray.append(p)
                continue
            walk(p, depth + 1)

    if is_checkout(lane) and str(lane) not in knownset:
        stray.append(lane)
    walk(lane, 1)
    return stray


def matches(name: str, patterns: tuple[str, ...]) -> bool:
    return any(fnmatch.fnmatch(name.lower(), pat) for pat in patterns)


def parse_remote(url: str) -> tuple[str, str] | None:
    m = re.match(r"^(?:git@github\.com:|ssh://git@github\.com/|https://github\.com/)"
                 r"([^/\s]+)/([^/\s]+?)(?:\.git)?/?$", url.strip())
    return (m.group(1), m.group(2)) if m else None


# ---------------------------------------------------------------------------
# Assessment
# ---------------------------------------------------------------------------

@dataclass
class Item:
    kind: str            # lane | build | artifact | leftover
    path: Path
    lane: str
    bytes: int = 0
    reclaimable: bool = False
    reasons: list[str] = field(default_factory=list)


@dataclass
class Context:
    root: Path
    live: LiveFiles
    prs: PrStates
    idle_cutoff: float
    lane_cutoff: float
    min_artifact_bytes: int


def keep_marked(path: Path) -> bool:
    return (path / KEEP_MARKER).exists()


def tracked_clean(co: Path) -> bool:
    rc, out = git(co, "status", "--porcelain", "--untracked-files=no")
    return rc == 0 and not out.strip()


def is_tracked(co: Path, name: str) -> bool:
    rc, out = git(co, "ls-files", "--", name)
    return rc != 0 or bool(out.strip())


def merged_state(co: Path, ctx: Context) -> tuple[bool, str]:
    rc, head = git(co, "rev-parse", "HEAD")
    head = head.strip()
    if rc != 0 or not head:
        return False, "HEAD unreadable"
    rc, ref = git(co, "symbolic-ref", "--short", "refs/remotes/origin/HEAD")
    candidates = [ref.strip()] if rc == 0 and ref.strip() else []
    candidates += ["origin/main", "origin/master"]
    for c in candidates:
        if git(co, "rev-parse", "--verify", "--quiet", f"refs/remotes/{c}")[0] != 0:
            continue
        if git(co, "merge-base", "--is-ancestor", head, f"refs/remotes/{c}")[0] == 0:
            return True, f"HEAD is in {c}"
        break
    rc, url = git(co, "remote", "get-url", "origin")
    parsed = parse_remote(url) if rc == 0 else None
    if parsed is None:
        return False, "origin is not a recognised GitHub remote"
    state, detail = ctx.prs.lookup(parsed[0], parsed[1], head)
    if state in ("merged", "closed"):
        return True, f"PR {state}"
    if state == "none":
        return False, "HEAD is not in the default branch and has no PR"
    return False, f"PR state {state}: {detail}"


def unpushed(co: Path) -> bool:
    rc, out = git(co, "rev-list", "--count", "HEAD", "--branches", "--not", "--remotes")
    if rc != 0 or not out.strip().isdigit() or int(out) > 0:
        return True
    rc, stash = git(co, "stash", "list")
    return rc != 0 or bool(stash.strip())


def checkout_gate(co: Path, ctx: Context) -> str | None:
    """Return the reason the checkout blocks reclamation, or None."""
    if keep_marked(co):
        return f"{KEEP_MARKER} present"
    recent = modified_since(co, ctx.idle_cutoff)
    if recent is None:
        return "idle check could not run"
    if recent:
        return "modified within the idle window"
    if ctx.live.busy(co):
        return "live process under the checkout"
    if not tracked_clean(co):
        return "tracked tree is dirty"
    ok, why = merged_state(co, ctx)
    if not ok:
        return why
    return None


def candidates(co: Path, ctx: Context) -> list[tuple[str, Path]]:
    out: list[tuple[str, Path]] = []
    try:
        entries = sorted(os.scandir(co), key=lambda e: e.name)
    except OSError:
        return out
    for e in entries:
        if e.name == ".git" or e.is_symlink():
            continue
        p = Path(e.path)
        if e.name.startswith(REAPING_PREFIX):
            out.append(("leftover", p))
        elif e.is_dir(follow_symlinks=False) and matches(e.name, BUILD_PATTERNS):
            out.append(("build", p))
        elif matches(e.name, ARTIFACT_PATTERNS):
            out.append(("artifact", p))
    return out


def assess_checkout(co: Path, lane: str, ctx: Context) -> list[Item]:
    items: list[Item] = []
    cands = candidates(co, ctx)
    if not cands:
        return items
    sized = [(k, p, disk_bytes(p)) for k, p in cands]
    block = None  # evaluated lazily so a checkout with no large entry costs nothing
    for kind, p, size in sized:
        if kind == "artifact" and size < ctx.min_artifact_bytes:
            continue
        item = Item(kind, p, lane, size)
        if kind != "leftover" and is_tracked(co, p.name):
            item.reasons.append("entry holds tracked files")
        elif keep_marked(p) if p.is_dir() else False:
            item.reasons.append(f"{KEEP_MARKER} present")
        else:
            if block is None:
                block = checkout_gate(co, ctx) or ""
            if block:
                item.reasons.append(block)
        item.reclaimable = not item.reasons
        items.append(item)
    return items


def untracked_strays(co: Path, ctx: Context) -> list[str]:
    rc, out = git(co, "status", "--porcelain", "--untracked-files=normal")
    if rc != 0:
        return ["status failed"]
    known = {p.name for _, p in candidates(co, ctx)}
    strays = []
    for line in out.splitlines():
        if line.startswith("?? "):
            top = line[3:].strip('"').split("/")[0]
            if top not in known:
                strays.append(top)
    return strays


def assess_lane(lane_dir: Path, checkouts: list[Path], ctx: Context) -> Item | None:
    """Item for the whole lane when every guard holds, else None plus reasons."""
    item = Item("lane", lane_dir, lane_dir.name)
    if not checkouts:
        item.reasons.append("no git checkout in lane")
        return item
    if keep_marked(lane_dir):
        item.reasons.append(f"{KEEP_MARKER} present")
        return item
    for co in checkouts:
        if (co / ".git").is_file():
            item.reasons.append(f"{co.name} is a linked worktree or submodule checkout")
            return item
    stray = stray_git_dirs(lane_dir, checkouts)
    if stray:
        item.reasons.append(f"unassessed git repository at {stray[0].name}")
        return item
    recent = modified_since(lane_dir, ctx.lane_cutoff)
    if recent is None:
        item.reasons.append("idle check could not run")
        return item
    if recent:
        item.reasons.append("modified within the lane idle window")
        return item
    if ctx.live.busy(lane_dir):
        item.reasons.append("live process under the lane")
        return item
    for co in checkouts:
        if keep_marked(co):
            item.reasons.append(f"{KEEP_MARKER} present in {co.name}")
            return item
        if not tracked_clean(co):
            item.reasons.append(f"{co.name}: tracked tree is dirty")
            return item
        strays = untracked_strays(co, ctx)
        if strays:
            item.reasons.append(f"{co.name}: untracked files outside artifact dirs ({strays[0]})")
            return item
        if unpushed(co):
            item.reasons.append(f"{co.name}: unpushed commits or a stash")
            return item
        ok, why = merged_state(co, ctx)
        if not ok:
            item.reasons.append(f"{co.name}: {why}")
            return item
    item.reclaimable = True
    return item


def lane_dirs(root: Path) -> list[Path]:
    try:
        return sorted(Path(e.path) for e in os.scandir(root)
                      if e.is_dir(follow_symlinks=False) and not e.name.startswith(".")
                      and not e.name.startswith("_"))
    except OSError:
        return []


def assess_lane_tree(lane_dir: Path, ctx: Context) -> list[Item]:
    lane = lane_dir.name
    checkouts = [lane_dir] if is_checkout(lane_dir) else find_checkouts(lane_dir)
    whole = assess_lane(lane_dir, checkouts, ctx)
    if whole.reclaimable:
        whole.bytes = disk_bytes(lane_dir)
        return [whole]
    items: list[Item] = []
    for co in checkouts:
        items.extend(assess_checkout(co, lane, ctx))
    for stale in (e for e in safe_scandir(lane_dir) if e.name.startswith(REAPING_PREFIX)):
        p = Path(stale.path)
        it = Item("leftover", p, lane, disk_bytes(p))
        if modified_since(p, ctx.idle_cutoff) is not False:
            it.reasons.append("modified within the idle window")
        elif ctx.live.busy(p):
            it.reasons.append("live process")
        it.reclaimable = not it.reasons
        items.append(it)
    items.append(whole)  # always last; non-reclaimable lanes carry the reason
    return items


def safe_scandir(d: Path):
    try:
        return list(os.scandir(d))
    except OSError:
        return []


# ---------------------------------------------------------------------------
# Removal
# ---------------------------------------------------------------------------

def under(root: Path, path: Path) -> bool:
    r, p = os.path.realpath(root), os.path.realpath(path.parent)
    return (p == r or p.startswith(r + os.sep)) and os.path.realpath(path) != r


def remove(item: Item, root: Path) -> str:
    """Rename then delete. Only ever deletes the renamed `.reaping-*` path."""
    p = item.path
    if p.is_symlink() or not p.exists():
        return "skipped: path is a symlink or missing"
    if not under(root, p):
        return "refused: path is outside the root"
    if item.kind == "lane" and p.parent.resolve() != root.resolve():
        return "refused: lane is not directly under the root"
    if p.name in ("", ".", "..", ".git"):
        return "refused: unsafe name"
    if item.kind != "lane" and not is_checkout(p.parent) and p.parent.parent != root:
        return "refused: entry is not directly inside a checkout"
    target = p.parent / f"{REAPING_PREFIX}{p.name}-{os.getpid()}-{int(time.time())}"
    try:
        os.rename(p, target)
    except OSError as exc:
        return f"rename failed: {exc}"
    if not target.name.startswith(REAPING_PREFIX) or target.is_symlink():
        return "refused: renamed path failed verification"
    try:
        if target.is_dir():
            shutil.rmtree(target)
        else:
            target.unlink()
    except OSError as exc:
        return f"renamed to {target.name}; delete incomplete: {exc}"
    return "removed"


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def human(n: int) -> str:
    v = float(n)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if v < 1024 or unit == "TiB":
            return f"{v:.1f} {unit}" if unit != "B" else f"{int(v)} B"
        v /= 1024
    return str(n)


def build_context(args: argparse.Namespace, live: LiveFiles, prs: PrStates) -> Context:
    now = time.time()
    return Context(Path(args.root), live, prs, now - args.idle_hours * 3600,
                   now - args.lane_idle_days * 86400,
                   int(args.min_artifact_mb * 1024 * 1024))


def collect(args: argparse.Namespace, ctx: Context) -> list[Item]:
    items: list[Item] = []
    for lane_dir in lane_dirs(ctx.root):
        if args.lane and lane_dir.name not in args.lane:
            continue
        items.extend(assess_lane_tree(lane_dir, ctx))
    return items


def summarize(items: list[Item]) -> dict[str, dict]:
    lanes: dict[str, dict] = {}
    for it in items:
        row = lanes.setdefault(it.lane, {"reclaimable": 0, "held": 0, "entries": []})
        if it.reclaimable:
            row["reclaimable"] += it.bytes
        elif it.kind != "lane":
            row["held"] += it.bytes
        if it.kind != "lane" or it.reclaimable or it.reasons:
            row["entries"].append({"kind": it.kind, "path": str(it.path), "bytes": it.bytes,
                                   "reclaimable": it.reclaimable, "reasons": it.reasons})
    return lanes


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", required=True, help="directory holding one subdirectory per lane")
    ap.add_argument("--yes", action="store_true", help="delete (default is a dry run)")
    ap.add_argument("--idle-hours", type=float, default=DEFAULT_IDLE_HOURS)
    ap.add_argument("--lane-idle-days", type=float, default=DEFAULT_LANE_IDLE_DAYS)
    ap.add_argument("--min-artifact-mb", type=float, default=DEFAULT_MIN_ARTIFACT_MB)
    ap.add_argument("--cache-file", default=os.path.expanduser("~/.cache/pulp/lane-reaper/pr-states.json"))
    ap.add_argument("--cache-ttl-hours", type=float, default=DEFAULT_CACHE_TTL_HOURS)
    ap.add_argument("--lane", action="append", help="limit to this lane (repeatable)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--verbose", action="store_true", help="explain every held entry")
    args = ap.parse_args(argv)

    root = Path(args.root)
    if not root.is_dir():
        print(f"clean_lane_builds: root {root} is not a directory", file=sys.stderr)
        return 2
    try:
        live = LiveFiles()
    except InstrumentUnavailable as exc:
        print(f"clean_lane_builds: refusing to run, process listing unavailable: {exc}",
              file=sys.stderr)
        return EXIT_INSTRUMENT_UNAVAILABLE
    prs = PrStates(Path(args.cache_file) if args.cache_file else None, args.cache_ttl_hours)
    ctx = build_context(args, live, prs)
    items = collect(args, ctx)
    prs.save()

    results: dict[str, str] = {}
    if args.yes:
        # Re-measure with a fresh process listing right before acting, and act
        # only on entries that are still reclaimable.
        fresh_live = LiveFiles()
        fresh = Context(ctx.root, fresh_live, prs, time.time() - args.idle_hours * 3600,
                        time.time() - args.lane_idle_days * 86400, ctx.min_artifact_bytes)
        again = {(i.kind, str(i.path)) for i in collect(args, fresh) if i.reclaimable}
        for it in items:
            if it.reclaimable and (it.kind, str(it.path)) in again:
                results[str(it.path)] = remove(it, ctx.root)
            elif it.reclaimable:
                results[str(it.path)] = "skipped: no longer reclaimable on re-check"
        prs.save()

    lanes = summarize(items)
    total = sum(r["reclaimable"] for r in lanes.values())
    held = sum(r["held"] for r in lanes.values())
    if args.json:
        print(json.dumps({"dry_run": not args.yes, "root": str(root), "lanes": lanes,
                          "reclaimable_bytes": total, "held_bytes": held,
                          "api_calls": prs.calls, "results": results}, indent=2))
        return 0
    mode = "ACTING" if args.yes else "DRY RUN (nothing is deleted)"
    print(f"clean_lane_builds: {mode}; root {root}")
    print(f"{'lane':44} {'reclaimable':>12} {'held':>12}")
    for lane, row in sorted(lanes.items(), key=lambda kv: -kv[1]["reclaimable"]):
        if row["reclaimable"] == 0 and row["held"] == 0 and not args.verbose:
            continue
        print(f"{lane:44} {human(row['reclaimable']):>12} {human(row['held']):>12}")
        if args.verbose:
            for e in row["entries"]:
                tag = "RECLAIM" if e["reclaimable"] else "held   "
                why = "; ".join(e["reasons"])
                print(f"    {tag} {e['kind']:8} {human(e['bytes']):>10} {e['path']} {why}")
    print(f"TOTAL reclaimable {human(total)}; held {human(held)}; "
          f"PR API calls {prs.calls}")
    for path, res in results.items():
        print(f"  {res}: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
