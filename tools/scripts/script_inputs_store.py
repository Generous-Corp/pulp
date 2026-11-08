#!/usr/bin/env python3
"""The script-test input list, stored as one fragment file per entry.

`script_test_inputs.py` records what every script-driven and data-reading
ctest reads. The record used to be one generated JSON document, and every
pull request that added or re-pointed a test rewrote neighbouring lines of
it, so two independent pull requests conflicted on it more often than on any
other file. It is now a directory, one file per entry, so two changes that
touch different tests touch different files and git merges them without a
conflict:

  test/ctest_script_inputs/meta.json               schema and section flags
  test/ctest_script_inputs/tests/<slug>.json        one script-driven test
  test/ctest_script_inputs/executables/<slug>.json  one scanned test executable

The combined document (the shape every consumer reads) is assembled on read
by `load()` / `load_ref()`; it is never committed. A revision older than the
fragment layout, or a fixture repository, still holds the single
`test/ctest_script_inputs.json`, and the loaders read that when the directory
is absent, so history replays keep working. Re-adding the single file next to
the directory is rejected by `hotspot_edit_guard.py` and by the drift check.

A slug is a pure function of the entry's own name, never of its neighbours,
so adding one entry can never rename another file. A name that is already a
safe lowercase file name is its own slug; any other name is sanitized and
suffixed with a hash of the exact name, so names that differ only in case or
punctuation cannot collide on a case-insensitive file system.

Standard library only: the gate's Python and the source-selftest lane import
it.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path

# Spelled as one literal so script_test_inputs.py records the directory as an
# input of every test whose scripts reach this module.
FRAGMENT_DIR = Path("test/ctest_script_inputs")
LEGACY_LIST = Path("test/ctest_script_inputs.json")
META_FILE = "meta.json"
TESTS_DIR = "tests"
EXECUTABLES_DIR = "executables"
LAYOUT = "pulp-ctest-script-inputs-fragments/v1"
# Keys of the combined document that live in the per-executable fragments.
COMPILED_KEYS = ("executable_targets", "executables", "executables_scanned")
_SAFE_SLUG = re.compile(r"[a-z0-9][a-z0-9._-]{0,119}")
_UNSAFE = re.compile(r"[^A-Za-z0-9._-]+")


class StoreError(ValueError):
    """A fragment directory that does not describe one consistent document."""


def slug(name: str) -> str:
    """The file stem for an entry named `name` (see the module docstring)."""
    if _SAFE_SLUG.fullmatch(name) and "--" not in name:
        return name
    safe = _UNSAFE.sub("_", name).strip("._-").lower()[:80] or "entry"
    return f"{safe}--{hashlib.sha1(name.encode('utf-8')).hexdigest()[:10]}"


def dumps(obj) -> str:
    """Canonical text for a fragment or for the combined document."""
    return json.dumps(obj, indent=1, sort_keys=True) + "\n"


def is_store_path(rel: str) -> bool:
    """Whether a repository-relative path is part of the list (either layout)."""
    rel = rel.replace("\\", "/")
    return rel == LEGACY_LIST.as_posix() or rel == FRAGMENT_DIR.as_posix() \
        or rel.startswith(FRAGMENT_DIR.as_posix() + "/")


def store_paths(paths) -> set[str]:
    """The members of `paths` that belong to the list."""
    return {p for p in paths if is_store_path(p)}


def fragment_path(kind: str, name: str) -> str:
    """Repository-relative path of one entry's fragment (`kind`: tests or executables)."""
    if kind not in (TESTS_DIR, EXECUTABLES_DIR):
        raise ValueError(f"unknown fragment kind {kind!r}")
    return f"{FRAGMENT_DIR.as_posix()}/{kind}/{slug(name)}.json"


def split(doc: dict) -> dict[str, str]:
    """The fragment files (relative to the fragment directory) for `doc`."""
    files: dict[str, str] = {}
    meta = {k: v for k, v in doc.items() if k not in COMPILED_KEYS and k != "tests"}
    compiled = any(k in doc for k in COMPILED_KEYS)
    files[META_FILE] = dumps({"layout": LAYOUT, "compiled": compiled, "document": meta})
    for name, rec in (doc.get("tests") or {}).items():
        rel = f"{TESTS_DIR}/{slug(name)}.json"
        if rel in files:
            raise StoreError(f"two tests share the fragment {rel}: {name!r}")
        files[rel] = dumps({"name": name, "record": rec})
    if compiled:
        targets = doc.get("executable_targets") or {}
        records = doc.get("executables") or {}
        scanned = set(doc.get("executables_scanned") or [])
        for name in sorted(scanned | set(records) | set(targets)):
            rel = f"{EXECUTABLES_DIR}/{slug(name)}.json"
            if rel in files:
                raise StoreError(f"two executables share the fragment {rel}: {name!r}")
            frag: dict = {"name": name, "scanned": name in scanned}
            if name in targets:
                frag["target"] = targets[name]
            if name in records:
                frag["record"] = records[name]
            files[rel] = dumps(frag)
    return files


def join(files: dict[str, str]) -> dict:
    """The combined document from fragment texts keyed by their relative path."""
    if META_FILE not in files:
        raise StoreError(f"{FRAGMENT_DIR.as_posix()}/{META_FILE} is missing")
    try:
        meta = json.loads(files[META_FILE])
    except json.JSONDecodeError as exc:
        raise StoreError(f"{META_FILE}: {exc}") from exc
    if meta.get("layout") != LAYOUT:
        raise StoreError(f"{META_FILE}: layout {meta.get('layout')!r}, expected {LAYOUT!r}")
    doc = dict(meta.get("document") or {})
    tests: dict = {}
    targets: dict = {}
    records: dict = {}
    scanned: list[str] = []
    for rel in sorted(files):
        if rel == META_FILE:
            continue
        kind, _, base = rel.partition("/")
        if kind not in (TESTS_DIR, EXECUTABLES_DIR) or "/" in base or not base.endswith(".json"):
            raise StoreError(f"unexpected file in {FRAGMENT_DIR.as_posix()}: {rel}")
        try:
            frag = json.loads(files[rel])
        except json.JSONDecodeError as exc:
            raise StoreError(f"{rel}: {exc}") from exc
        name = frag.get("name") if isinstance(frag, dict) else None
        if not isinstance(name, str):
            raise StoreError(f"{rel}: no entry name")
        if base != slug(name) + ".json":
            raise StoreError(f"{rel}: holds {name!r}, whose fragment is {kind}/{slug(name)}.json")
        if kind == TESTS_DIR:
            tests[name] = frag.get("record")
            continue
        if not meta.get("compiled"):
            raise StoreError(f"{rel}: executable fragment, but {META_FILE} records no compiled entries")
        if frag.get("scanned"):
            scanned.append(name)
        if "target" in frag:
            targets[name] = frag["target"]
        if "record" in frag:
            records[name] = frag["record"]
    doc["tests"] = tests
    if meta.get("compiled"):
        doc["executable_targets"] = targets
        doc["executables"] = records
        doc["executables_scanned"] = sorted(scanned)
    return doc


# Files a desktop or an editor leaves beside the fragments in a working tree
# (Finder's .DS_Store, vim swap files, backups). Git never tracks them here, so
# they are not part of the list; `join` still refuses any other stray file.
_IGNORED_SUFFIXES = ("~", ".swp", ".swo", ".swx", ".bak", ".orig", ".tmp")


def _ignored(rel: str) -> bool:
    return any(part.startswith(".") for part in rel.split("/")) or rel.endswith(_IGNORED_SUFFIXES)


def _read_dir(directory: Path) -> dict[str, str]:
    files = {}
    for p in sorted(directory.rglob("*")):
        rel = p.relative_to(directory).as_posix()
        if p.is_file() and not _ignored(rel):
            files[rel] = p.read_text(encoding="utf-8")
    return files


def load_path(path: Path) -> dict | None:
    """A list at `path`: a fragment directory, or a single legacy JSON file.

    Returns None when nothing is there or the legacy file does not parse;
    raises StoreError for a fragment directory that is inconsistent, so a
    hand-edited or half-merged fragment fails closed instead of reading empty."""
    if path.is_dir():
        return join(_read_dir(path))
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def load(root: Path) -> dict | None:
    """The working tree's list: the fragment directory, else the legacy file."""
    root = Path(root)
    if (root / FRAGMENT_DIR).is_dir():
        return load_path(root / FRAGMENT_DIR)
    return load_path(root / LEGACY_LIST)


def location(root: Path) -> Path:
    """Where the working tree's list lives (the directory unless only the legacy file exists)."""
    root = Path(root)
    if not (root / FRAGMENT_DIR).is_dir() and (root / LEGACY_LIST).is_file():
        return root / LEGACY_LIST
    return root / FRAGMENT_DIR


def _git(root: Path, *args: str, stdin: bytes | None = None,
         env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(root), *args], input=stdin, capture_output=True, env=env)


def ref_files(root: Path, rev: str, env: dict[str, str] | None = None) -> dict[str, str] | None:
    """The fragment texts at `rev`, or None when `rev` has no fragment directory."""
    listing = _git(root, "ls-tree", "-r", "-z", "--full-tree", rev, "--", FRAGMENT_DIR.as_posix() + "/",
                   env=env)
    if listing.returncode != 0:
        return None
    blobs: dict[str, str] = {}
    prefix = FRAGMENT_DIR.as_posix() + "/"
    for record in listing.stdout.decode("utf-8", "replace").split("\0"):
        meta, _, path = record.partition("\t")
        parts = meta.split()
        if len(parts) == 3 and parts[1] == "blob" and path.startswith(prefix):
            blobs[path[len(prefix):]] = parts[2]
    if not blobs:
        return None
    out = _git(root, "cat-file", "--batch", stdin=("\n".join(blobs.values()) + "\n").encode(), env=env)
    if out.returncode != 0:
        return None
    texts: dict[str, str] = {}
    data, pos = out.stdout, 0
    while pos < len(data):
        end = data.index(b"\n", pos)
        header = data[pos:end].split()
        pos = end + 1
        if len(header) < 3 or header[1] == b"missing":
            return None
        size = int(header[2])
        texts[header[0].decode()] = data[pos:pos + size].decode("utf-8", "replace")
        pos += size + 1
    return {rel: texts[sha] for rel, sha in blobs.items()}


def load_ref(root: Path, rev: str, env: dict[str, str] | None = None) -> dict | None:
    """The list as `rev` has it (fragments, else the legacy file), or None."""
    files = ref_files(root, rev, env)
    if files is not None:
        return join(files)
    shown = _git(root, "show", f"{rev}:{LEGACY_LIST.as_posix()}", env=env)
    if shown.returncode != 0:
        return None
    try:
        return json.loads(shown.stdout.decode("utf-8", "replace"))
    except json.JSONDecodeError:
        return None


def ref_identity(root: Path, rev: str = "HEAD") -> str | None:
    """A git object id that changes exactly when the list at `rev` changes."""
    for rel in (FRAGMENT_DIR.as_posix(), LEGACY_LIST.as_posix()):
        res = _git(root, "rev-parse", "--verify", "--quiet", f"{rev}:{rel}")
        if res.returncode == 0 and res.stdout.strip():
            return res.stdout.decode().strip()
    return None


def write_path(path: Path, doc: dict) -> list[str]:
    """Write `doc` at `path` (a fragment directory, or a `.json` legacy file).

    Only fragments whose text changed are rewritten and a fragment no longer
    in `doc` is removed, so a regeneration touches exactly the entries that
    moved. Returns the relative paths written or removed."""
    if path.suffix == ".json":
        text = dumps(doc)
        old = path.read_text(encoding="utf-8") if path.is_file() else None
        if old == text:
            return []
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return [path.name]
    files = split(doc)
    existing = _read_dir(path) if path.is_dir() else {}
    touched = []
    for rel, text in sorted(files.items()):
        if existing.get(rel) != text:
            target = path / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
            touched.append(rel)
    for rel in sorted(set(existing) - set(files)):
        (path / rel).unlink()
        touched.append(rel)
    return touched


def main(argv: list[str]) -> int:
    """`script_inputs_store.py [--repo-root R] {show [REV] | split}`: print the
    combined document, or convert a legacy file in place into fragments."""
    import argparse
    ap = argparse.ArgumentParser(allow_abbrev=False, description=main.__doc__)
    ap.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[2]))
    sub = ap.add_subparsers(dest="cmd", required=True)
    show = sub.add_parser("show", allow_abbrev=False, help="print the combined document")
    show.add_argument("rev", nargs="?", help="a revision (default: the working tree)")
    sub.add_parser("split", allow_abbrev=False, help="move test/ctest_script_inputs.json into fragments")
    a = ap.parse_args(argv[1:])
    root = Path(a.repo_root)
    if a.cmd == "show":
        doc = load_ref(root, a.rev) if a.rev else load(root)
        if doc is None:
            print("script-inputs-store: no list found", flush=True)
            return 2
        print(dumps(doc), end="")
        return 0
    doc = load_path(root / LEGACY_LIST)
    if doc is None:
        print(f"script-inputs-store: {LEGACY_LIST} is missing or unreadable")
        return 2
    write_path(root / FRAGMENT_DIR, doc)
    if join(_read_dir(root / FRAGMENT_DIR)) != doc:
        print("script-inputs-store: fragments do not round-trip; left the legacy file in place")
        return 2
    (root / LEGACY_LIST).unlink()
    print(f"script-inputs-store: split {LEGACY_LIST} into {FRAGMENT_DIR}/")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv))
