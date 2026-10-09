#!/usr/bin/env python3
"""Per-executable source keys: may a test executable's base result stand
for the head tree?

An executable's key is computed twice, over the base and the head trees,
from one input set: the input set the BASE's own gate build recorded (the
objects and archive members its link pulled, and the headers each of those
objects included). Equal keys mean every one of those files has the same
content in both trees, the target's content-keyed codemodel digest (flags,
defines, link fragments, registrations, generated headers) is the same, and
so is the toolchain. Head's preprocessing then follows the same paths
over the same bytes, so its objects and links equal base's. The input set
has to come from a build of one of the two compared trees: a warm build
directory's dependency log comes from whatever commit it last built, and
equal content over that set proves nothing.

Each object's flags, definitions and generated build-tree inputs belong to
the target that compiled it, so the key carries the codemodel digest of the
executable's own target and of every library target whose archive or
objects its link pulled.

What a key cannot see is made an always_run reason, never a guess:

    inventory_unmatched
                       the ctest inventory has tests but none run anything
                       under the build dir (the two are spelled differently)
    base_unrecorded    no usable reuse record for the base commit, or its
                       commit is not an ancestor of head
    base_other_toolchain
                       the record was built by another toolchain
    toolchain_unknown  either side's toolchain identity is incomplete
    dependency_pin     a dependency it builds against moved its pin, or a pin
                       file changed in a way no dependency can be named for
                       (dependency_pins.py)
    codemodel_unknown  no content-keyed codemodel digest for the target on
                       one side
    commit_bound       the executable embeds the commit
    key_blind          its bytes have changed while its key did not (the
                       replay's key-blind list)
    audit_uncovered    the last clean read audit of main did not observe it
                       (a macOS-only test, or no clean audit was handed in), so
                       nothing has shown its declared data is all it reads
    shared_link        its link names a shared library the build produced,
                       whose content reaches it without changing its inputs
                       (link_members.shared_scope); not modelled, so it runs
    environment        a registration drives a shared host resource, or is
                       one of the always-run names (drift, lint, probes, ...)
    unrecorded         the base link, an object's dependency list, or a
                       pulled member's objects are not recorded
    include_shadow     a header with the name of one of its inputs was added
                       or deleted, so an #include may resolve elsewhere
    data_*             the data scan cannot vouch for what it reads
    spawns_*           the spawn scan cannot vouch for what it runs

The toolchain is what decides object bytes for equal inputs and flags: the
OS family and architecture, plus the identity the reuse record computes
(the compiler CMake chose, the SDK, the deployment target and the
allow-listed environment), computed on this host by the same function. The
OS version is not part of it (it is recorded for diagnosis), and neither is
the target triple the compiler prints, whose OS component is the host's;
the triple a build targets comes from its deployment target. A test whose
verdict depends on the host itself is environment-bound, or is what the
sampled re-run of would-be skips exists to catch.

Which record is the base is the planner's choice, made where credentials
exist: a run built by the SAME toolchain as the one that will use the keys,
whose tree is an ancestor of head. Prefer the lane's own most recent record
(its warm build directory holds that build), else the gate's record at the
base commit. A record from another toolchain keys nothing.

The computation reads only local data: the source checkout's git objects,
the base reuse-record files the host planner fetched, the head codemodel
digest and the head ctest inventory. It never touches the network, so it
runs where the validation lane runs. It is policy code: the planner runs the
BASE's copy of KEY_CODE_PATHS over the head tree as data, and the manifest
names the base commit, those files' digest and the record's digest, so the
runner can re-derive the selection and refuse a difference.

    executable_keys.py --source-root S --base-sha B --head-sha H \\
        --base-record DIR --base-record-run-id N --head-codemodel F \\
        --ctest-json J --build-dir D [--toolchain-json T] --out M
    executable_keys.py --print-toolchain --build-dir D

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
import dependency_pins  # noqa: E402
import link_members  # noqa: E402
import object_deps  # noqa: E402
from spawn_closure import SpawnIndex  # noqa: E402
from always_run_names import ALWAYS_RUN_NAME_RE  # noqa: E402

SCHEMA = "pulp-executable-keys/v1"
KEY_BLIND_SCHEMA = "pulp-key-blind/v1"
# The files whose base copy computes the keys and the selection over them;
# touching any of them makes a plan select everything.
KEY_CODE_PATHS = ("tools/ci/executable_keys.py", "tools/ci/link_members.py", "tools/ci/object_deps.py",
                  "tools/ci/reuse_record.py", "tools/ci/spawn_closure.py", "tools/ci/always_run_names.py",
                  "tools/ci/executable_selection.py", "tools/ci/key_blind_executables.json",
                  "tools/ci/dependency_pins.py", "tools/ci/dependency_pin_map.json")
# Executables whose recorded bytes changed while their content-keyed source
# key did not, as the reuse replay measured them (reuse_policy_replay.py
# key-blind). The list only grows: an entry always runs until the mechanism
# behind it is keyed and the entry is removed by hand.
KEY_BLIND_LIST = HERE / "key_blind_executables.json"
SCRIPT_INPUTS_PATH = "test/ctest_script_inputs.json"
CONTENT_KEYED_SCHEMA = "pulp-codemodel-digest/v2"
# Files that pin third-party dependencies. A bump can change a dependency's
# content without changing any path the codemodel digests or any archive a
# recorded link names (FetchContent archives are treated as pinned), so an
# executable that builds against a moved dependency is not keyed. The root
# CMakeLists.txt counts only through its FetchContent blocks and setup.sh only
# through its SDK refs (dependency_pins.PIN_PATHS); this set is the files that
# are pins as a whole, which the reuse replay's blunter rule reads.
DEPENDENCY_PIN_PATHS = frozenset(dependency_pins.PIN_PATHS) - {dependency_pins.ROOT_CMAKE,
                                                               dependency_pins.SETUP_SCRIPT}
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


def record_digest_bytes(directory: Path) -> bytes:
    """What `base_record_sha256` hashes, the contract a planner reproduces:
    for every regular file under `directory` (recursively, symlinks
    followed), in ascending order of the UTF-8 bytes of its POSIX path
    relative to `directory`, that path, a NUL, the lowercase hex sha256 of
    the file's bytes, and a newline."""
    rows = sorted((f.relative_to(directory).as_posix().encode("utf-8"), f)
                  for f in directory.rglob("*") if f.is_file())
    return b"".join(rel + b"\0" + hashlib.sha256(f.read_bytes()).hexdigest().encode() + b"\n" for rel, f in rows)


def _one(directory: Path, prefix: str) -> Path | None:
    found = sorted(directory.glob(f"{prefix}*.json"))
    return found[0] if len(found) == 1 else None


def load_record(directory: Path | None) -> tuple[dict | None, str | None]:
    """The parts of a reuse record a key needs, and the sha256 over every
    file in it (so the runner can check what it was handed), or (None, None)
    when there is no record."""
    if directory is None or not directory.is_dir():
        return None, None
    digest = hashlib.sha256(record_digest_bytes(directory)).hexdigest()

    def read(path: Path | None) -> dict | None:
        try:
            return json.loads(path.read_text(encoding="utf-8")) if path and path.is_file() else None
        except json.JSONDecodeError:
            return None
    job = read(directory / "job.json") or {}
    image = job.get("runner_image") if isinstance(job.get("runner_image"), dict) else {}
    # A record from another platform (the gate's no-suite alias job uploads
    # one), or with no complete toolchain identity, keys nothing.
    toolchain = toolchain_key(job)
    return {"codemodel": read(_one(directory, "codemodel-")), "links": read(_one(directory, "link-members-")),
            "deps": read(_one(directory, "object-deps-")),
            "image": image.get("digest"), "toolchain": toolchain}, digest


# The toolchain identity the reuse record computes (reuse_record.
# toolchain_identity: the compiler CMake chose, the SDK, the deployment
# target, the allow-listed environment) is what decides object bytes for
# equal inputs and flags. The key takes all of it except the compiler's
# printed target triple, whose OS component is the host's; the triple a
# build targets comes from its deployment target, which stays in the key.
# The OS family and architecture come from the runner fingerprint.
TOOLCHAIN_NOT_KEYED = ("target",)


def toolchain_key(job: dict) -> dict | None:
    """The key's toolchain identity from a record's job.json (or the same
    shape computed on this host), or None when the record is from another
    platform or its identity is missing or incomplete."""
    block = job.get("toolchain")
    fields = ((job.get("runner_image") or {}).get("fields")) or {}
    platform = job.get("platform")
    if platform is not None and not str(platform).startswith("darwin-"):
        return None
    if not isinstance(block, dict) or block.get("complete") is not True or not fields.get("os") \
            or not fields.get("arch"):
        return None
    key = {k: v for k, v in (block.get("fields") or {}).items() if k not in TOOLCHAIN_NOT_KEYED}
    return {"os": fields["os"], "arch": fields["arch"], **key}


def host_identity(build_dir: Path | None) -> dict:
    """This host's identity in the shape a reuse record's job.json carries it
    (`platform`, `runner_image`, `toolchain`), computed by the same functions
    that write the record, so a planner compares a probe with a stored record
    field for field."""
    import reuse_record
    env = dict(os.environ)
    image = reuse_record.runner_image(env)
    return {"platform": reuse_record.platform_id(), "runner_image": image,
            "toolchain": reuse_record.toolchain_identity(build_dir, env, image["fields"])}


def probe_toolchain(build_dir: Path | None) -> dict | None:
    """This host's toolchain key for a configured build."""
    return toolchain_key(host_identity(build_dir))


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

    def linked_targets(self, artifact: str, targets: dict[str, dict]) -> set[str] | None:
        """The targets whose objects an executable's link pulled: the ones
        owning its object directories (`CMakeFiles/<t>.dir/`) and in-build
        archives. Their codemodel digests key the flags and generated files
        those objects were compiled with. None when an in-build archive has
        no owning target."""
        rec = self.links.get(BUILD + artifact)
        if rec is None:
            return None
        owner = {a: n for n, t in targets.items() for a in t.get("artifacts") or []}
        out: set[str] = set()
        for obj in rec["objects"]:
            _, sep, rest = obj.partition("CMakeFiles/")
            if sep and rest.split("/", 1)[0].endswith(".dir"):
                out.add(rest.split("/", 1)[0][:-len(".dir")])
        for archive in rec["archives"]:
            if not archive.startswith(BUILD) or archive.startswith(BUILD + "_deps/"):
                continue
            if archive not in owner:
                return None
            out.add(owner[archive])
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


def key_of(digest: str | dict, toolchain: dict | None, paths: Iterable[str], blobs: dict[str, str]) -> str:
    """`digest`: the executable's codemodel digest, or {target: digest} for
    it and every target its link pulled objects from."""
    body = {"codemodel": digest, "toolchain": toolchain,
            "inputs": [[p, blobs.get(p, "absent")] for p in sorted(paths)]}
    return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


# -- the manifest ------------------------------------------------------------

def load_key_blind(path: Path) -> frozenset[str]:
    """The key-blind executables (relative to the build dir). A list that
    cannot be read is an error, not an empty list: the code runs from the
    base's copy, which always carries it."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    if doc.get("schema") != KEY_BLIND_SCHEMA:
        raise ValueError(f"{path}: schema is not {KEY_BLIND_SCHEMA}")
    return frozenset(doc.get("executables") or {})


def registrations(ctest: dict | None, build_dir: Path | None) -> dict[str, list[dict]]:
    """artifact (relative to the build dir) -> the ctest registrations that
    run it. `build_dir` is the path the configure used, matched as a string
    against the inventory's commands: the same inputs give the same answer
    on the runner and on a host that only holds copies of them."""
    out: dict[str, list[dict]] = {}
    if not ctest or build_dir is None:
        return out
    root = os.path.abspath(str(build_dir))
    for test in ctest.get("tests") or []:
        cmd = test.get("command") or []
        if not cmd:
            continue
        real = os.path.normpath(cmd[0])
        if not real.startswith(root + os.sep):
            continue
        props = {p.get("name"): p.get("value") for p in test.get("properties") or []}
        labels = props.get("LABELS") or []
        locks = props.get("RESOURCE_LOCK") or []
        out.setdefault(os.path.relpath(real, root), []).append(
            {"name": test.get("name"), "labels": list(labels) if isinstance(labels, list) else [labels],
             "resource_locks": list(locks) if isinstance(locks, list) else [locks]})
    return out


READ_AUDIT_SCHEMA = "pulp-read-audit/v1"


def audit_covered(report: dict | None) -> frozenset[str] | None:
    """Executable names the read audit observed reading only what they
    declare, from its report (tools/ci/read_audit.py's read-audit.json), or
    None when the report cannot vouch for any: absent, another schema, a
    verdict other than clean, or no published covered set."""
    if not isinstance(report, dict) or report.get("schema") != READ_AUDIT_SCHEMA:
        return None
    s0 = report.get("stage0") or {}
    if s0.get("verdict") != "clean" or not isinstance(s0.get("covered"), list):
        return None
    return frozenset(str(n) for n in s0["covered"])


PIN_MAP = dependency_pins.load_map()


def toolchain_platform(toolchain: dict | None) -> str:
    """The platform the pin rules evaluate CMake for: the toolchain's OS."""
    return str((toolchain or {}).get("os") or "").lower()


def moved_pins(source_root: Path, base_sha: str, head_sha: str, changed: Iterable[str],
               toolchain: dict | None) -> dependency_pins.Pins:
    """The dependencies the changed pin files moved, on this toolchain's
    platform."""
    changed = sorted(changed)
    if not changed:
        return dependency_pins.NONE
    base = {p: show(source_root, base_sha, p) for p in changed}
    head = {p: show(source_root, head_sha, p) for p in changed}
    return dependency_pins.attribute(base, head, toolchain_platform(toolchain), PIN_MAP)


def compute(source_root: Path, base_sha: str, head_sha: str, record: dict | None, head_codemodel: dict | None,
            ctest: dict | None, build_dir: Path | None, toolchain: dict | None,
            key_blind_path: Path | None = None, *, audited: frozenset[str] | None) -> dict:
    """The key manifest's `executables` and a count per always_run reason.
    Pure over its inputs and the two git trees."""
    ancestor = subprocess.run(["git", "-C", str(source_root), "merge-base", "--is-ancestor", base_sha, head_sha],
                              capture_output=True).returncode == 0
    base_tree, head_tree = tree_blobs(source_root, base_sha), tree_blobs(source_root, head_sha)
    changed_paths = {p for p in set(base_tree) | set(head_tree) if base_tree.get(p) != head_tree.get(p)}
    shadowing = {os.path.basename(p) for p in set(base_tree) ^ set(head_tree) if p.endswith(HEADER_SUFFIXES)}
    pins = moved_pins(source_root, base_sha, head_sha, set(dependency_pins.PIN_PATHS) & changed_paths, toolchain)
    head_targets = (head_codemodel or {}).get("targets") or {}
    base_cm = (record or {}).get("codemodel")
    base_targets = (base_cm or {}).get("targets") or {}
    keyed = content_keyed(base_cm) and content_keyed(head_codemodel)
    sets = None
    shared_loaders: frozenset[str] = frozenset()
    if record and link_members.unusable(record.get("links")) is None \
            and object_deps.unusable(record.get("deps")) is None:
        sets = InputSets(record["links"], record["deps"])
        shared_loaders = link_members.shared_scope(record["links"])
    script_list = show(source_root, head_sha, SCRIPT_INPUTS_PATH)
    script_doc = json.loads(script_list) if script_list else None
    data_scan, spawn_scan = spawn_scan_of(script_doc, "data"), spawn_scan_of(script_doc)
    spawns = SpawnIndex(head_targets)
    regs = registrations(ctest, build_dir)
    # Commands that all miss the build dir mean the two were spelled
    # differently (a relative path, /var vs /private/var), not that no test
    # runs anything: keying then would skip nothing and say nothing.
    unmatched = bool((ctest or {}).get("tests")) and not regs
    key_blind = load_key_blind(KEY_BLIND_LIST if key_blind_path is None else key_blind_path)
    pin_index = dependency_pins.DependencyIndex(
        PIN_MAP, toolchain_platform(toolchain), head_targets, base_targets,
        ((record or {}).get("links") or {}).get("executables"))
    pins = dependency_pins.resolve(pins, pin_index)
    by_artifact = {a.removeprefix(BUILD): n for n, t in head_targets.items()
                   if t.get("type") in ("EXECUTABLE", "MODULE_LIBRARY") for a in t.get("artifacts") or []}
    out: dict[str, dict] = {}
    for artifact, name in sorted(by_artifact.items()):
        target, base_target = head_targets[name], base_targets.get(name) or {}
        kind = "module" if target.get("type") == "MODULE_LIBRARY" else "executable"
        tests = regs.get(artifact, [])
        paths = sets.of(artifact) if sets else None
        linked = (sets.linked_targets(artifact, base_targets) if sets else None)
        if linked is not None:
            linked = linked | {name}
        if paths is not None and linked is None:
            paths = None  # an archive no target owns: its flags are unknown
        dstate, dinputs = data_status(artifact, data_scan)
        sstate = spawn_status(artifact, spawn_scan)
        reason = (
            "inventory_unmatched" if unmatched else
            "base_unrecorded" if record is None or sets is None or not ancestor else
            "toolchain_unknown" if not record.get("toolchain") or not toolchain else
            "base_other_toolchain" if record["toolchain"] != toolchain else
            "dependency_pin" if pins.scope == "all" or (pins.names and pin_index.reaches(artifact, name, pins.names))
            else
            "codemodel_unknown" if not keyed or not base_target.get("digest") or not target.get("digest")
            or any(not (base_targets.get(n) or {}).get("digest") for n in linked or ()) else
            "commit_bound" if target.get("commit_bound") or base_target.get("commit_bound") else
            "key_blind" if artifact in key_blind else
            "shared_link" if BUILD + artifact in shared_loaders else
            # Only an executable runs its own reads; a module's reads are its
            # loader's, which this same rule covers.
            "audit_uncovered" if kind == "executable" and (audited is None
                                                           or os.path.basename(artifact) not in audited) else
            "environment" if any(environment_bound(t) or ALWAYS_RUN_NAME_RE.search(t["name"] or "")
                                 for t in tests) else
            "unrecorded" if paths is None else
            "include_shadow" if any(os.path.basename(p) in shadowing for p in paths
                                    if p.endswith(HEADER_SUFFIXES)) else
            None)
        # A module the tests load reads the checkout and runs programs the
        # same way an executable does, so the same scans have to vouch for it.
        if reason is None:
            reason = ({"whole_checkout": "data_whole_checkout", "undeclared": "data_undeclared",
                       "unknown": "data_unknown"}.get(dstate)
                      or {"undeclared": "spawns_undeclared", "unknown": "spawns_unknown"}.get(sstate))
        entry = {"kind": kind, "registrations": [t["name"] for t in tests], "always_run": reason,
                 "spawns": sorted(spawns.closure(artifact)), "head_key": None, "base_key": None}
        if paths is not None and reason not in ("inventory_unmatched", "base_unrecorded", "toolchain_unknown",
                                                "base_other_toolchain", "codemodel_unknown"):
            keyed_paths = paths | (data_paths(dinputs, base_tree, head_tree) if dstate == "declared" else set())
            digests = lambda side: {n: (side.get(n) or {}).get("digest", "absent") for n in sorted(linked)}  # noqa: E731
            entry["base_key"] = key_of(digests(base_targets), record["toolchain"], keyed_paths, base_tree)
            entry["head_key"] = key_of(digests(head_targets), toolchain, keyed_paths, head_tree)
        out[artifact] = entry
    counts: dict[str, int] = {}
    for e in out.values():
        counts[e["always_run"] or "keyed"] = counts.get(e["always_run"] or "keyed", 0) + 1
    return {"executables": out, "reasons": dict(sorted(counts.items())), "dependency_pins": pins.as_json()}


def code_digest(source_root: Path, base_sha: str) -> str:
    """sha256 over the base's copy of the key code."""
    digest = hashlib.sha256()
    for path in KEY_CODE_PATHS:
        body = show(source_root, base_sha, path)
        digest.update(path.encode() + b"\0" + (body or "absent").encode() + b"\n")
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--print-toolchain", action="store_true",
                    help="print this host's identity for --build-dir in job.json shape and exit")
    ap.add_argument("--source-root", type=Path)
    ap.add_argument("--base-sha")
    ap.add_argument("--head-sha", default="HEAD")
    ap.add_argument("--base-record", type=Path, help="the base reuse-record files, fetched by the planner")
    ap.add_argument("--base-record-run-id", default=None)
    ap.add_argument("--head-codemodel", type=Path, help="codemodel_digest.py output for the head configure")
    ap.add_argument("--ctest-json", type=Path, help="`ctest --show-only=json-v1` for the head configure")
    ap.add_argument("--build-dir", type=Path)
    ap.add_argument("--toolchain-json", type=Path,
                    help="the head toolchain key ({os, arch} plus reuse_record.toolchain_identity's fields "
                         "without `target`); computed from --build-dir when absent")
    ap.add_argument("--audit-report", type=Path,
                    help="read-audit.json of the last clean read audit of main, fetched by the planner; "
                         "absent or not clean, every executable is audit_uncovered")
    ap.add_argument("--out", type=Path)
    a = ap.parse_args(argv[1:])
    if a.print_toolchain:
        print(json.dumps(host_identity(a.build_dir), indent=1, sort_keys=True))
        return 0
    missing = [f"--{n.replace('_', '-')}" for n in ("source_root", "base_sha", "out") if getattr(a, n) is None]
    if missing:
        ap.error(f"the following arguments are required: {', '.join(missing)}")

    def read(path: Path | None) -> dict | None:
        return json.loads(path.read_text(encoding="utf-8")) if path and path.is_file() else None
    record, record_digest = load_record(a.base_record)
    rev = lambda r: subprocess.run(["git", "-C", str(a.source_root), "rev-parse", r],  # noqa: E731
                                   check=True, capture_output=True, text=True).stdout.strip()
    base_sha, head_sha = rev(a.base_sha), rev(a.head_sha)
    toolchain = read(a.toolchain_json) if a.toolchain_json else probe_toolchain(a.build_dir)
    audit = read(a.audit_report)
    body = compute(a.source_root, base_sha, head_sha, record, read(a.head_codemodel), read(a.ctest_json),
                   a.build_dir, toolchain, audited=audit_covered(audit))
    manifest = {"schema": SCHEMA,
                "producer": {"base_sha": base_sha, "head_sha": head_sha,
                             "code_paths": list(KEY_CODE_PATHS), "code_sha256": code_digest(a.source_root, base_sha),
                             "base_record_run_id": a.base_record_run_id, "base_record_sha256": record_digest,
                             "toolchain": toolchain,
                             "base_record_image": (record or {}).get("image"),
                             "audit_report_sha256": (hashlib.sha256(a.audit_report.read_bytes()).hexdigest()
                                                     if a.audit_report and a.audit_report.is_file() else None),
                             "audit_commit": (audit or {}).get("commit"),
                             # Says in words why nothing was keyed on data
                             # when a report was missing or not clean.
                             "dependency_pins": body.pop("dependency_pins"),
                             "audit_status": ("absent" if audit is None else
                                              "clean" if audit_covered(audit) is not None else "not_clean")},
                **body}
    a.out.write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(f"executable-keys: {len(body['executables'])} entries; {body['reasons']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
