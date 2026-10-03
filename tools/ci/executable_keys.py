#!/usr/bin/env python3
"""Per-executable source keys: may a test executable's base result stand
for the head tree?

An executable's key is computed twice, over the base and the head trees,
from one input set: the input set the BASE's own gate build recorded (the
objects and archive members its link pulled, and the headers each of those
objects included). Equal keys mean every one of those files has the same
content in both trees, the target's content-keyed codemodel digest (flags,
defines, link fragments, registrations, generated headers) is the same, and
so is the runner image. Head's preprocessing then follows the same paths
over the same bytes, so its objects and links equal base's. The input set
has to come from a build of one of the two compared trees: a warm build
directory's dependency log comes from whatever commit it last built, and
equal content over that set proves nothing.

What a key cannot see is made an always_run reason, never a guess:

    base_unrecorded    no usable reuse record for the base commit, or its
                       commit is not an ancestor of head
    base_other_image   the record was produced on another runner image
    dependency_pin     a dependency pin moved (every build input may move)
    codemodel_unknown  no content-keyed codemodel digest for the target on
                       one side
    commit_bound       the executable embeds the commit
    environment        a registration drives a shared host resource, or is
                       one of the always-run names (drift, lint, probes, ...)
    unrecorded         the base link, an object's dependency list, or a
                       pulled member's objects are not recorded
    include_shadow     a header with the name of one of its inputs was added
                       or deleted, so an #include may resolve elsewhere
    data_*             the data scan cannot vouch for what it reads
    spawns_*           the spawn scan cannot vouch for what it runs

Which record is the base is the planner's choice, made where credentials
exist: a run on the SAME runner image as the one that will use the keys,
whose tree is an ancestor of head. For the local validation lane that is
the lane's own most recent record on its image (its warm build directory
holds that build); for the merge-group gate it is the gate's record at the
base commit. A record from another image keys nothing: a verdict carried
across hosts is exactly what nobody has measured.

The computation reads only local data: the source checkout's git objects,
the base reuse-record files the host planner fetched, the head codemodel
digest and the head ctest inventory. It never touches the network, so it
runs where the validation lane runs. It is policy code: the planner runs the
BASE's copy of KEY_CODE_PATHS over the head tree as data, and the manifest
names the base commit, those files' digest and the record's digest, so the
runner can re-derive the selection and refuse a difference.

    executable_keys.py --source-root S --base-sha B --head-sha H \\
        --base-record DIR --base-record-run-id N --head-codemodel F \\
        --ctest-json J --build-dir D --image-id I --out M

The replay (tools/scripts/reuse_replay_collect.py) imports the scan and
input-matching helpers from here, so the lane and the replay share one
implementation.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Iterable

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import link_members  # noqa: E402
import object_deps  # noqa: E402
from spawn_closure import SpawnIndex  # noqa: E402
from test_receipts_shadow import ALWAYS_RUN_NAME_RE  # noqa: E402

SCHEMA = "pulp-executable-keys/v1"
# The files whose base copy computes the keys; touching any of them makes a
# plan select everything.
KEY_CODE_PATHS = ("tools/ci/executable_keys.py", "tools/ci/link_members.py", "tools/ci/object_deps.py",
                  "tools/ci/spawn_closure.py", "tools/ci/test_receipts_shadow.py")
SCRIPT_INPUTS_PATH = "test/ctest_script_inputs.json"
CONTENT_KEYED_SCHEMA = "pulp-codemodel-digest/v2"
DEPENDENCY_PIN_PATHS = frozenset({"tools/deps/manifest.json", "tools/cmake/PulpDependencies.cmake",
                                  "tools/cmake/PulpFetchContent.cmake"})
# Registration labels that mean the test drives a shared host resource.
ENVIRONMENT_LABELS = frozenset({"gpu", "browser-capture"})
COMPILE_SUFFIXES = (".cpp", ".cc", ".cxx", ".c", ".mm", ".m")
CODE_SUFFIXES = COMPILE_SUFFIXES + (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc", ".def")
HEADER_SUFFIXES = tuple(s for s in CODE_SUFFIXES if s not in COMPILE_SUFFIXES)
# A data scan that finds no reads in an executable is trusted only when it
# demonstrably detects the readers it already knows: this share of the
# executables with declared reads, and at least this many of them, must
# carry a source the scan itself matched (`detected_sources`), or every
# executable falls to the broad rule. The floor keeps a list with a handful
# of declared readers from passing on the share alone.
DATA_SCAN_MIN_DETECTED = 0.85
DATA_SCAN_MIN_DETECTED_READERS = 20
SRC, BUILD = "<src>/", "<build>/"


# -- scans and declared inputs (shared with the replay) ----------------------

def declared_hit(inputs: Iterable[str], changed: Iterable[str]) -> bool:
    """Whether a changed path is a declared input or lies beneath one.
    `!path` (pulp_test_data ABSENT) is a probed path expected missing; a
    change to it is its creation."""
    inputs = [(i[1:] if i.startswith("!") else i).rstrip("/") for i in inputs]
    return any(f == i or f.startswith(i + "/") for f in changed for i in inputs)


def environment_bound(mapped: dict) -> bool:
    return bool(mapped.get("resource_locks")) or bool(ENVIRONMENT_LABELS & set(mapped.get("labels") or []))


def object_source(obj: str) -> str | None:
    """The repo-relative source a CMake object path compiles, or None.

    `<dir>/CMakeFiles/<target>.dir/<path>.o` compiles `<dir>/<path>`, with
    `__/` standing for `../`."""
    head, sep, rest = obj.partition("CMakeFiles/")
    if not sep or "/" not in rest or not rest.endswith(".o"):
        return None
    path = os.path.normpath(os.path.join(head, rest.split("/", 1)[1][:-2].replace("__/", "../")))
    return None if path.startswith("..") or os.path.isabs(path) else path


def data_scan_saw_readers(entries: dict[str, dict]) -> bool:
    """Whether a data scan detected its known readers; a list that does not
    record what the scan matched cannot show it."""
    declared = [e for e in entries.values() if e.get("data") == "declared"]
    if not declared or any("detected_sources" not in e for e in declared):
        return False
    seen = sum(1 for e in declared if e["detected_sources"])
    return seen >= DATA_SCAN_MIN_DETECTED_READERS and seen / len(declared) >= DATA_SCAN_MIN_DETECTED


def spawn_scan_of(doc: dict | None, kind: str = "spawns") -> dict | None:
    """The `kind` scan (spawns or data) a checked-in script-input list
    carries: per-executable entries plus the set of executables the scan
    covered, or None when that list did not scan for it (an older tree, or
    an unreadable list)."""
    if not doc or kind not in (doc.get("executables_scanned_for") or []) \
            or not isinstance(doc.get("executables_scanned"), list):
        return None
    entries = dict(doc.get("executables") or {})
    if kind == "data" and not data_scan_saw_readers(entries):
        return None
    return {"entries": entries, "scanned": frozenset(doc["executables_scanned"])}


def scan_entry(executable: str, scan: dict) -> tuple[dict | None, bool]:
    name = os.path.basename(executable)
    name = name[:-4] if name.endswith(".exe") else name
    return scan["entries"].get(name), name in scan["scanned"]


def spawn_status(executable: str, scan: dict | None) -> str:
    """`clean`, `undeclared` or `unknown` for one test executable (relative
    to the build dir) under a spawn scan.

    An entry whose `spawns` is absent, `declared` (its edges are in the
    codemodel) or `none` (reviewed: it runs nothing this repo builds) is
    clean; any other value is undeclared. A missing entry is clean only when
    the scan covered that executable; otherwise nothing is known about it."""
    if scan is None:
        return "unknown"
    entry, scanned = scan_entry(executable, scan)
    if entry is None:
        return "clean" if scanned else "unknown"
    return "clean" if entry.get("spawns") in (None, "declared", "none") else "undeclared"


def data_status(executable: str, scan: dict | None) -> tuple[str, list[str]]:
    """(`none` | `declared` | `whole_checkout` | `undeclared` | `unknown`,
    declared inputs) for one executable under a data scan. A missing entry
    reads nothing only when the scan covered that executable; a state this
    reader does not know is unknown."""
    if scan is None:
        return "unknown", []
    entry, scanned = scan_entry(executable, scan)
    if entry is None:
        return ("none" if scanned else "unknown"), []
    state = entry.get("data")
    if state in ("none", "declared", "whole_checkout", "undeclared"):
        return state, list(entry.get("inputs") or [])
    return "unknown", []


# -- trees and records -------------------------------------------------------

def tree_blobs(root: Path, rev: str) -> dict[str, str]:
    """path -> blob id for every file of `rev`."""
    out = subprocess.run(["git", "-C", str(root), "ls-tree", "-r", "-z", "--full-tree", rev],
                         check=True, capture_output=True).stdout.decode("utf-8", "surrogateescape")
    blobs = {}
    for row in out.split("\0"):
        if row:
            meta, path = row.split("\t", 1)
            blobs[path] = meta.split()[2]
    return blobs


def show(root: Path, rev: str, path: str) -> str | None:
    res = subprocess.run(["git", "-C", str(root), "show", f"{rev}:{path}"], capture_output=True)
    return None if res.returncode else res.stdout.decode("utf-8", "replace")


def _one(directory: Path, prefix: str) -> Path | None:
    found = sorted(directory.glob(f"{prefix}*.json"))
    return found[0] if len(found) == 1 else None


def load_record(directory: Path | None) -> tuple[dict | None, str | None]:
    """The parts of a reuse record a key needs, and the sha256 over every
    file in it (so the runner can check what it was handed), or (None, None)
    when there is no record."""
    if directory is None or not directory.is_dir():
        return None, None
    digest = hashlib.sha256()
    for f in sorted(p for p in directory.rglob("*") if p.is_file()):
        digest.update(f.relative_to(directory).as_posix().encode() + b"\0")
        digest.update(hashlib.sha256(f.read_bytes()).hexdigest().encode() + b"\n")

    def read(path: Path | None) -> dict | None:
        try:
            return json.loads(path.read_text(encoding="utf-8")) if path and path.is_file() else None
        except json.JSONDecodeError:
            return None
    job = read(directory / "job.json") or {}
    image = job.get("runner_image")
    return {"codemodel": read(_one(directory, "codemodel-")), "links": read(_one(directory, "link-members-")),
            "deps": read(_one(directory, "object-deps-")),
            "image": image.get("digest") if isinstance(image, dict) else image}, digest.hexdigest()


def content_keyed(codemodel: dict | None) -> bool:
    return bool(codemodel) and codemodel.get("schema") == CONTENT_KEYED_SCHEMA \
        and codemodel.get("generated_headers") == "ninja-deps"


# -- the input set -----------------------------------------------------------

def _repo_path(token: str) -> str | None:
    """A recorded path inside the source tree, repo-relative; None for a
    build-tree file (the content-keyed digest covers generated files) or one
    outside both trees (pinned)."""
    return token[len(SRC):] if token.startswith(SRC) else None


class InputSets:
    """Base input sets from one record's link members and object deps."""

    def __init__(self, links: dict, deps: dict) -> None:
        self.links = link_members.expand(links)
        self.headers = object_deps.expand(deps)
        self.stale = set(deps.get("stale") or [])
        self.members = deps.get("members") or {}

    def _object(self, obj: str, out: set[str]) -> bool:
        if obj in self.stale or obj not in self.headers:
            return False
        source = object_source(obj.removeprefix(BUILD))
        if source is not None:
            out.add(source)
        out.update(p for h in self.headers[obj] if (p := _repo_path(h)))
        return True

    def of(self, artifact: str) -> set[str] | None:
        """Repo paths an executable's base build read, or None when any part
        of it is not recorded."""
        rec = self.links.get(BUILD + artifact)
        if rec is None:
            return None
        out: set[str] = set()
        for obj in rec["objects"]:
            if obj.startswith(BUILD) and not self._object(obj, out):
                return None
        for archive, info in rec["archives"].items():
            if not archive.startswith(BUILD) or archive.startswith(BUILD + "_deps/"):
                continue  # toolchain, prebuilt or FetchContent: pinned
            for name in info["members"]:
                objs = (self.members.get(archive) or {}).get(name)
                if not objs or not all(self._object(o, out) for o in objs):
                    return None
        return out


def data_paths(inputs: list[str], *trees: dict[str, str]) -> set[str]:
    """Every path of either tree a declared data input names: the path, what
    lies beneath it, or a glob's matches (and what lies beneath those)."""
    found: set[str] = set()
    paths = set().union(*trees)
    for raw in inputs:
        i = (raw[1:] if raw.startswith("!") else raw).rstrip("/")
        found.add(i)
        if any(c in i for c in "*?["):
            for p in paths:
                parts = p.split("/")
                if any(fnmatch.fnmatchcase("/".join(parts[:n]), i) for n in range(1, len(parts) + 1)):
                    found.add(p)
        else:
            found.update(p for p in paths if p.startswith(i + "/"))
    return found


def key_of(digest: str, image: str | None, paths: Iterable[str], blobs: dict[str, str]) -> str:
    body = {"codemodel": digest, "image": image, "inputs": [[p, blobs.get(p, "absent")] for p in sorted(paths)]}
    return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


# -- the manifest ------------------------------------------------------------

def registrations(ctest: dict | None, build_dir: Path | None) -> dict[str, list[dict]]:
    """artifact (relative to the build dir) -> the ctest registrations that
    run it."""
    out: dict[str, list[dict]] = {}
    if not ctest or build_dir is None:
        return out
    root = os.path.realpath(build_dir)
    for test in ctest.get("tests") or []:
        cmd = test.get("command") or []
        if not cmd:
            continue
        real = os.path.realpath(cmd[0])
        if not real.startswith(root + os.sep):
            continue
        props = {p.get("name"): p.get("value") for p in test.get("properties") or []}
        labels = props.get("LABELS") or []
        locks = props.get("RESOURCE_LOCK") or []
        out.setdefault(os.path.relpath(real, root), []).append(
            {"name": test.get("name"), "labels": list(labels) if isinstance(labels, list) else [labels],
             "resource_locks": list(locks) if isinstance(locks, list) else [locks]})
    return out


def compute(source_root: Path, base_sha: str, head_sha: str, record: dict | None, head_codemodel: dict | None,
            ctest: dict | None, build_dir: Path | None, image_id: str | None) -> dict:
    """The key manifest's `executables` and a count per always_run reason.
    Pure over its inputs and the two git trees."""
    ancestor = subprocess.run(["git", "-C", str(source_root), "merge-base", "--is-ancestor", base_sha, head_sha],
                              capture_output=True).returncode == 0
    base_tree, head_tree = tree_blobs(source_root, base_sha), tree_blobs(source_root, head_sha)
    changed_paths = {p for p in set(base_tree) | set(head_tree) if base_tree.get(p) != head_tree.get(p)}
    shadowing = {os.path.basename(p) for p in set(base_tree) ^ set(head_tree) if p.endswith(HEADER_SUFFIXES)}
    pins = bool(DEPENDENCY_PIN_PATHS & changed_paths)
    head_targets = (head_codemodel or {}).get("targets") or {}
    base_cm = (record or {}).get("codemodel")
    base_targets = (base_cm or {}).get("targets") or {}
    keyed = content_keyed(base_cm) and content_keyed(head_codemodel)
    sets = None
    if record and link_members.unusable(record.get("links")) is None \
            and object_deps.unusable(record.get("deps")) is None:
        sets = InputSets(record["links"], record["deps"])
    script_list = show(source_root, head_sha, SCRIPT_INPUTS_PATH)
    script_doc = json.loads(script_list) if script_list else None
    data_scan, spawn_scan = spawn_scan_of(script_doc, "data"), spawn_scan_of(script_doc)
    spawns = SpawnIndex(head_targets)
    regs = registrations(ctest, build_dir)
    by_artifact = {a.removeprefix(BUILD): n for n, t in head_targets.items()
                   if t.get("type") in ("EXECUTABLE", "MODULE_LIBRARY") for a in t.get("artifacts") or []}
    out: dict[str, dict] = {}
    for artifact, name in sorted(by_artifact.items()):
        target, base_target = head_targets[name], base_targets.get(name) or {}
        kind = "module" if target.get("type") == "MODULE_LIBRARY" else "executable"
        tests = regs.get(artifact, [])
        paths = sets.of(artifact) if sets else None
        dstate, dinputs = data_status(artifact, data_scan)
        sstate = spawn_status(artifact, spawn_scan)
        reason = (
            "base_unrecorded" if record is None or sets is None or not ancestor else
            "base_other_image" if record.get("image") != image_id else
            "dependency_pin" if pins else
            "codemodel_unknown" if not keyed or not base_target.get("digest") or not target.get("digest") else
            "commit_bound" if target.get("commit_bound") or base_target.get("commit_bound") else
            "environment" if any(environment_bound(t) or ALWAYS_RUN_NAME_RE.search(t["name"] or "")
                                 for t in tests) else
            "unrecorded" if paths is None else
            "include_shadow" if any(os.path.basename(p) in shadowing for p in paths
                                    if p.endswith(HEADER_SUFFIXES)) else
            None)
        if reason is None and kind == "executable":
            reason = ({"whole_checkout": "data_whole_checkout", "undeclared": "data_undeclared",
                       "unknown": "data_unknown"}.get(dstate)
                      or {"undeclared": "spawns_undeclared", "unknown": "spawns_unknown"}.get(sstate))
        entry = {"kind": kind, "registrations": [t["name"] for t in tests], "always_run": reason,
                 "spawns": sorted(spawns.closure(artifact)), "head_key": None, "base_key": None}
        if paths is not None and reason not in ("base_unrecorded", "base_other_image", "codemodel_unknown"):
            keyed_paths = paths | (data_paths(dinputs, base_tree, head_tree) if dstate == "declared" else set())
            entry["base_key"] = key_of(base_target["digest"], (record or {}).get("image"), keyed_paths, base_tree)
            entry["head_key"] = key_of(target["digest"], image_id, keyed_paths, head_tree)
        out[artifact] = entry
    counts: dict[str, int] = {}
    for e in out.values():
        counts[e["always_run"] or "keyed"] = counts.get(e["always_run"] or "keyed", 0) + 1
    return {"executables": out, "reasons": dict(sorted(counts.items()))}


def code_digest(source_root: Path, base_sha: str) -> str:
    """sha256 over the base's copy of the key code."""
    digest = hashlib.sha256()
    for path in KEY_CODE_PATHS:
        body = show(source_root, base_sha, path)
        digest.update(path.encode() + b"\0" + (body or "absent").encode() + b"\n")
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--source-root", required=True, type=Path)
    ap.add_argument("--base-sha", required=True)
    ap.add_argument("--head-sha", default="HEAD")
    ap.add_argument("--base-record", type=Path, help="the base reuse-record files, fetched by the planner")
    ap.add_argument("--base-record-run-id", default=None)
    ap.add_argument("--head-codemodel", type=Path, help="codemodel_digest.py output for the head configure")
    ap.add_argument("--ctest-json", type=Path, help="`ctest --show-only=json-v1` for the head configure")
    ap.add_argument("--build-dir", type=Path)
    ap.add_argument("--image-id", default=None)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args(argv[1:])

    def read(path: Path | None) -> dict | None:
        return json.loads(path.read_text(encoding="utf-8")) if path and path.is_file() else None
    record, record_digest = load_record(a.base_record)
    rev = lambda r: subprocess.run(["git", "-C", str(a.source_root), "rev-parse", r],  # noqa: E731
                                   check=True, capture_output=True, text=True).stdout.strip()
    base_sha, head_sha = rev(a.base_sha), rev(a.head_sha)
    body = compute(a.source_root, base_sha, head_sha, record, read(a.head_codemodel), read(a.ctest_json),
                   a.build_dir, a.image_id)
    manifest = {"schema": SCHEMA,
                "producer": {"base_sha": base_sha, "head_sha": head_sha,
                             "code_paths": list(KEY_CODE_PATHS), "code_sha256": code_digest(a.source_root, base_sha),
                             "base_record_run_id": a.base_record_run_id, "base_record_sha256": record_digest,
                             "image_id": a.image_id},
                **body}
    a.out.write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(f"executable-keys: {len(body['executables'])} entries; {body['reasons']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
