#!/usr/bin/env python3
"""The agent-capability catalog's committed records, one file per entry.

Three records used to be single files that nearly every capability change
rewrote, so two independent pull requests conflicted on them even when they
touched different headers:

  docs/status/agent-capability-surface/          the public-surface ledger
      meta.json                                  schema, roots, inventory_version
      headers/<include>.json                     one record per public header
      tombstones/<include>.json                  one record per removed header
  tools/agent-capabilities/history/<digest>.json the contract history, one file
                                                 per recorded state
  tools/agent-capabilities/fingerprints/<include>.json
                                                 the reviewed SHA-256 of a header
                                                 a capability binding names

A file name is a pure function of its own entry (a header's include path, or
an entry's digest), never of a neighbour, so adding one entry cannot rename or
rewrite another file. Two changes on different headers therefore touch
different files, and git merges them without a conflict. Two changes on the
same header still meet in one file, which is the review signal worth keeping.

The history is a chain, not an array: each entry names its `parent` digest,
and the order is read from the chain. An entry stores the content-addressed
objects (see agent_capability_history.py) that no earlier entry already
stores, so the directory holds the same bytes the single file did, spread over
one file per state. Appending a state adds one file and modifies none.

Every consumer still reads the combined documents, assembled on read by the
loaders here: the surface ledger as the `pulp.agent-capability-surface.v1`
document, the history as the `pulp.agent-capability-history.v2` document.
`render_surface` and `render_history` give the exact text the single files
held, which is how the split is proven lossless. A revision older than this
layout (or a fixture repository) still holds the single files, and the loaders
read those when the directory is absent, so history replays and protected-base
comparisons across the migration keep working. Re-adding a single file is
refused by `hotspot_edit_guard.py` and by `agent_capability_manifest.py
--check`.

Standard library only.
"""
from __future__ import annotations

import json
import pathlib
import re
import subprocess
from dataclasses import replace
from typing import Any

# Each directory and legacy file is spelled as one literal so
# script_test_inputs.py records it as an input of every test whose scripts
# reach this module.
SURFACE_DIR = pathlib.Path("docs/status/agent-capability-surface")
SURFACE_LEGACY = pathlib.Path("docs/status/agent-capability-surface.json")
HISTORY_DIR = pathlib.Path("tools/agent-capabilities/history")
HISTORY_LEGACY = pathlib.Path("tools/agent-capabilities/contract-history.json")
FINGERPRINT_DIR = pathlib.Path("tools/agent-capabilities/fingerprints")

SURFACE_LAYOUT = "pulp.agent-capability-surface-fragments.v1"
HISTORY_ENTRY_SCHEMA = "pulp.agent-capability-history-entry.v1"
FINGERPRINT_SCHEMA = "pulp.agent-capability-fingerprint.v1"
# The combined history document every reader expands. Kept here (not imported
# from agent_capability_history) so this module has no project imports.
HISTORY_SCHEMA = "pulp.agent-capability-history.v2"

META_FILE = "meta.json"
HEADERS_DIR = "headers"
TOMBSTONES_DIR = "tombstones"
_LIST_KEYS = ("headers", "tombstones")
_INCLUDE = re.compile(r"pulp/(?:[A-Za-z0-9_][A-Za-z0-9_.+-]*/)*[A-Za-z0-9_][A-Za-z0-9_.+-]*")
_DIGEST = re.compile(r"sha256:[0-9a-f]{64}")
_REF, _CONCAT, _MERGE = "$ref", "$concat", "$merge"


class StoreError(ValueError):
    """A fragment directory that does not describe one consistent document."""


# ── Shared helpers ──────────────────────────────────────────────────────────

def _dumps(value: Any) -> str:
    """The text of a surface or fingerprint fragment: the single file's style."""
    return json.dumps(value, indent=2, ensure_ascii=False) + "\n"


def include_stem(include: Any) -> str:
    """The relative file stem for a header include, refusing anything unsafe."""
    if not isinstance(include, str) or not _INCLUDE.fullmatch(include) or ".." in include:
        raise StoreError(f"not a public header include: {include!r}")
    return include


# Files a desktop or an editor leaves beside fragments in a working tree. Git
# never tracks them here, so they are not part of a record.
_IGNORED_SUFFIXES = ("~", ".swp", ".swo", ".swx", ".bak", ".orig", ".tmp")


def _ignored(rel: str) -> bool:
    return any(part.startswith(".") for part in rel.split("/")) or rel.endswith(_IGNORED_SUFFIXES)


def read_dir(directory: pathlib.Path) -> dict[str, str]:
    """{relative path: text} for every fragment under `directory`."""
    files: dict[str, str] = {}
    if not directory.is_dir():
        return files
    for path in sorted(directory.rglob("*")):
        rel = path.relative_to(directory).as_posix()
        if path.is_file() and not _ignored(rel):
            files[rel] = path.read_text(encoding="utf-8")
    return files


# ── Surface ledger ──────────────────────────────────────────────────────────

def split_surface(document: dict[str, Any]) -> dict[str, str]:
    """The fragment files (relative to SURFACE_DIR) for a surface document."""
    if not isinstance(document, dict) or not all(
            isinstance(document.get(key), list) for key in _LIST_KEYS):
        raise StoreError("surface document has no headers and tombstones arrays")
    keys = list(document)
    if keys[-2:] != list(_LIST_KEYS):
        # The single file ends with these two arrays; storing them apart is only
        # lossless when nothing follows them.
        raise StoreError("surface document must end with headers and tombstones")
    meta = {key: document[key] for key in keys[:-2]}
    files = {META_FILE: _dumps({"layout": SURFACE_LAYOUT, "document": meta})}
    for kind, items in ((HEADERS_DIR, document["headers"]),
                        (TOMBSTONES_DIR, document["tombstones"])):
        previous = None
        for item in items:
            include = item.get("include") if isinstance(item, dict) else None
            rel = f"{kind}/{include_stem(include)}.json"
            if rel in files:
                raise StoreError(f"two surface {kind} records share {rel}")
            if previous is not None and include <= previous:
                raise StoreError(f"surface {kind} are not sorted by include at {include}")
            previous = include
            files[rel] = _dumps(item)
    return files


def join_surface(files: dict[str, str]) -> dict[str, Any]:
    """The surface document from its fragment texts."""
    if META_FILE not in files:
        raise StoreError(f"{SURFACE_DIR.as_posix()}/{META_FILE} is missing")
    try:
        meta = json.loads(files[META_FILE])
    except json.JSONDecodeError as error:
        raise StoreError(f"{SURFACE_DIR.as_posix()}/{META_FILE}: {error}") from error
    if not isinstance(meta, dict) or set(meta) != {"layout", "document"} \
            or meta.get("layout") != SURFACE_LAYOUT or not isinstance(meta["document"], dict):
        raise StoreError(f"{SURFACE_DIR.as_posix()}/{META_FILE} is not a {SURFACE_LAYOUT} meta file")
    document = dict(meta["document"])
    if any(key in document for key in _LIST_KEYS):
        raise StoreError(f"{SURFACE_DIR.as_posix()}/{META_FILE} carries a record array")
    lists: dict[str, list[tuple[str, Any]]] = {HEADERS_DIR: [], TOMBSTONES_DIR: []}
    for rel in sorted(files):
        if rel == META_FILE:
            continue
        kind, _, stem = rel.partition("/")
        if kind not in lists or not stem.endswith(".json"):
            raise StoreError(f"unexpected file in {SURFACE_DIR.as_posix()}: {rel}")
        try:
            item = json.loads(files[rel])
        except json.JSONDecodeError as error:
            raise StoreError(f"{SURFACE_DIR.as_posix()}/{rel}: {error}") from error
        include = item.get("include") if isinstance(item, dict) else None
        if stem != f"{include_stem(include)}.json":
            raise StoreError(
                f"{SURFACE_DIR.as_posix()}/{rel} holds {include!r}; "
                f"its file is {kind}/{include}.json")
        lists[kind].append((include, item))
    document["headers"] = [item for _, item in sorted(lists[HEADERS_DIR], key=lambda p: p[0])]
    document["tombstones"] = [item for _, item in sorted(lists[TOMBSTONES_DIR], key=lambda p: p[0])]
    return document


def render_surface(document: dict[str, Any]) -> str:
    """The text the single ledger file held for this document."""
    return _dumps(document)


# ── Contract history ────────────────────────────────────────────────────────

def _node_keys(node: Any) -> list[str]:
    """Object keys a stored node refers to, in a stable order."""
    keys: list[str] = []
    stack = [node]
    while stack:
        item = stack.pop()
        if isinstance(item, list):
            stack.extend(reversed(item))
        elif isinstance(item, dict):
            if len(item) == 1 and next(iter(item)) in (_REF, _CONCAT, _MERGE):
                marker, value = next(iter(item.items()))
                keys.extend([value] if marker == _REF else list(value or []))
            else:
                stack.extend(reversed(list(item.values())))
    return keys


def history_entry_text(digest: str, parent: str | None, objects: dict[str, Any]) -> str:
    """One entry file: its digest, parent, and the objects it introduces, one per line."""
    lines = [
        "{",
        f'  "schema": {json.dumps(HISTORY_ENTRY_SCHEMA)},',
        f'  "entry_digest": {json.dumps(digest)},',
        f'  "parent": {json.dumps(parent)},',
        '  "objects": {',
    ]
    keys = sorted(objects)
    for index, key in enumerate(keys):
        body = json.dumps(objects[key], ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        lines.append(f"    {json.dumps(key)}: {body}{',' if index + 1 < len(keys) else ''}")
    lines += ["  }", "}"]
    return "\n".join(lines) + "\n"


def history_entry_name(digest: str) -> str:
    if not isinstance(digest, str) or not _DIGEST.fullmatch(digest):
        raise StoreError(f"not a history entry digest: {digest!r}")
    return digest.removeprefix("sha256:") + ".json"


def split_history(history: dict[str, Any]) -> dict[str, str]:
    """The entry files (relative to HISTORY_DIR) for a schema-v2 history document.

    Each object is stored by the first entry, in chain order, that reaches it,
    so the split is a pure function of the document."""
    if not isinstance(history, dict) or history.get("schema") != HISTORY_SCHEMA \
            or set(history) != {"schema", "entries", "objects"}:
        raise StoreError(f"history to split is not a {HISTORY_SCHEMA} document")
    digests, objects = history["entries"], history["objects"]
    if not isinstance(digests, list) or not isinstance(objects, dict):
        raise StoreError("history entries or objects are invalid")
    if len(set(digests)) != len(digests):
        raise StoreError("history records one state twice; a chain cannot hold it")
    owner: dict[str, str] = {}
    files: dict[str, str] = {}
    parent: str | None = None
    for digest in digests:
        mine: dict[str, Any] = {}
        stack = [digest]
        while stack:
            key = stack.pop()
            if key in owner or key in mine:
                continue
            if key not in objects:
                raise StoreError(f"history references a missing object {key!r}")
            mine[key] = objects[key]
            stack.extend(reversed(_node_keys(objects[key])))
        for key in mine:
            owner[key] = digest
        files[history_entry_name(digest)] = history_entry_text(digest, parent, mine)
        parent = digest
    unowned = set(objects) - set(owner)
    if unowned:
        raise StoreError(f"history stores {len(unowned)} object(s) no entry reaches")
    return files


def join_history(files: dict[str, str]) -> tuple[dict[str, Any], list[str]]:
    """The schema-v2 history document from its entry files, plus chain problems.

    A fork (two entries naming the same parent, which two branches that each
    record a state against one base produce) is not fatal here: the entries are
    ordered deterministically, the fork is reported, and `--write` re-chains it.
    Anything that cannot be ordered at all raises StoreError."""
    entries: dict[str, dict[str, Any]] = {}
    for rel in sorted(files):
        try:
            entry = json.loads(files[rel])
        except json.JSONDecodeError as error:
            raise StoreError(f"{HISTORY_DIR.as_posix()}/{rel}: {error}") from error
        if not isinstance(entry, dict) or set(entry) != {"schema", "entry_digest", "parent", "objects"} \
                or entry.get("schema") != HISTORY_ENTRY_SCHEMA:
            raise StoreError(f"{HISTORY_DIR.as_posix()}/{rel} is not a {HISTORY_ENTRY_SCHEMA} entry")
        digest = entry["entry_digest"]
        if rel != history_entry_name(digest):
            raise StoreError(f"{HISTORY_DIR.as_posix()}/{rel} holds entry {digest}")
        if entry["parent"] is not None:
            history_entry_name(entry["parent"])
        if not isinstance(entry["objects"], dict):
            raise StoreError(f"{HISTORY_DIR.as_posix()}/{rel} objects are not a map")
        entries[digest] = entry
    if not entries:
        return {"schema": HISTORY_SCHEMA, "entries": [], "objects": {}}, []
    children: dict[str | None, list[str]] = {}
    for digest, entry in entries.items():
        parent = entry["parent"]
        if parent is not None and parent not in entries:
            raise StoreError(f"history entry {digest} names a missing parent {parent}")
        children.setdefault(parent, []).append(digest)
    roots = children.get(None, [])
    if len(roots) != 1:
        raise StoreError(f"history must have exactly one first entry (parent null); found {len(roots)}")
    order: list[str] = []
    stack = [roots[0]]
    while stack:
        digest = stack.pop()
        order.append(digest)
        stack.extend(sorted(children.get(digest, []), reverse=True))
    if len(order) != len(entries):
        raise StoreError("history entries do not form one chain from its first entry")
    problems = [
        f"capability history forks after {parent}: {', '.join(sorted(kids))} share that "
        "parent; run tools/scripts/agent_capability_manifest.py --write to re-chain it"
        for parent, kids in sorted(children.items(), key=lambda item: str(item[0]))
        if len(kids) > 1
    ]
    objects: dict[str, Any] = {}
    for digest in order:
        for key, value in entries[digest]["objects"].items():
            if key in objects and objects[key] != value:
                raise StoreError(f"history object {key} is stored twice with different content")
            objects[key] = value
    return {"schema": HISTORY_SCHEMA, "entries": order, "objects": objects}, problems


def render_history(history: dict[str, Any]) -> str:
    """The text the single history file held: one digest and one object per line."""
    lines = ["{", f'  "schema": {json.dumps(history["schema"])},', '  "entries": [']
    digests = history["entries"]
    lines += [f"    {json.dumps(d)}{',' if i + 1 < len(digests) else ''}"
              for i, d in enumerate(digests)]
    lines += ["  ],", '  "objects": {']
    keys = sorted(history["objects"])
    for index, key in enumerate(keys):
        body = json.dumps(history["objects"][key], ensure_ascii=False,
                          separators=(",", ":"), sort_keys=True)
        lines.append(f"    {json.dumps(key)}: {body}{',' if index + 1 < len(keys) else ''}")
    lines += ["  }", "}"]
    return "\n".join(lines) + "\n"


# ── Binding header fingerprints ─────────────────────────────────────────────

def fingerprint_text(include: str, fingerprint: str) -> str:
    return _dumps({"schema": FINGERPRINT_SCHEMA, "include": include, "fingerprint": fingerprint})


def fingerprint_path(include: str) -> pathlib.Path:
    """Repository-relative path of one header's reviewed fingerprint."""
    return FINGERPRINT_DIR / f"{include_stem(include)}.json"


def split_fingerprints(fingerprints: dict[str, str]) -> dict[str, str]:
    return {f"{include_stem(include)}.json": fingerprint_text(include, value)
            for include, value in sorted(fingerprints.items())}


def join_fingerprints(files: dict[str, str]) -> dict[str, str]:
    """{include: reviewed fingerprint} from the fragment texts."""
    result: dict[str, str] = {}
    for rel in sorted(files):
        try:
            item = json.loads(files[rel])
        except json.JSONDecodeError as error:
            raise StoreError(f"{FINGERPRINT_DIR.as_posix()}/{rel}: {error}") from error
        if not isinstance(item, dict) or set(item) != {"schema", "include", "fingerprint"} \
                or item.get("schema") != FINGERPRINT_SCHEMA:
            raise StoreError(f"{FINGERPRINT_DIR.as_posix()}/{rel} is not a {FINGERPRINT_SCHEMA} record")
        include, value = item["include"], item["fingerprint"]
        if rel != f"{include_stem(include)}.json":
            raise StoreError(f"{FINGERPRINT_DIR.as_posix()}/{rel} holds {include!r}")
        if not isinstance(value, str) or not _DIGEST.fullmatch(value):
            raise StoreError(f"{FINGERPRINT_DIR.as_posix()}/{rel} fingerprint is not a sha256 digest")
        result[include] = value
    return result


def load_fingerprints(root: pathlib.Path) -> dict[str, str]:
    return join_fingerprints(read_dir(pathlib.Path(root) / FINGERPRINT_DIR))


# ── Working tree ────────────────────────────────────────────────────────────

def _invalid(error: BaseException) -> dict[str, str]:
    # The shape agent_capability_manifest's loaders already use for an
    # unreadable record, so every downstream check reports it.
    return {"_invalid": str(error) or type(error).__name__}


def _load_legacy(path: pathlib.Path) -> Any:
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, RecursionError) as error:
        return _invalid(error)


def load_surface(root: pathlib.Path) -> Any:
    """The working tree's surface ledger, None when absent, `_invalid` when unreadable."""
    directory = pathlib.Path(root) / SURFACE_DIR
    if directory.is_dir():
        try:
            return join_surface(read_dir(directory))
        except StoreError as error:
            return _invalid(error)
    return _load_legacy(pathlib.Path(root) / SURFACE_LEGACY)


def load_history(root: pathlib.Path) -> Any:
    """The working tree's history document, None when absent, `_invalid` when unreadable."""
    directory = pathlib.Path(root) / HISTORY_DIR
    if directory.is_dir():
        try:
            return join_history(read_dir(directory))[0]
        except StoreError as error:
            return _invalid(error)
    return _load_legacy(pathlib.Path(root) / HISTORY_LEGACY)


def layout_problems(
    root: pathlib.Path,
    surface_document: dict[str, Any] | None,
    history: Any,
    fingerprints: dict[str, str] | None,
) -> list[str]:
    """Why the committed fragments are not exactly the split of these documents.

    Parsing alone cannot see a reformatted fragment, a misplaced object, a
    stray file, or a re-added single file; only comparing every file's text
    with the canonical split does."""
    root = pathlib.Path(root)
    problems: list[str] = []
    for legacy, directory in ((SURFACE_LEGACY, SURFACE_DIR), (HISTORY_LEGACY, HISTORY_DIR)):
        if (root / legacy).exists():
            problems.append(
                f"{legacy} is the retired single-file layout; the record lives in "
                f"{directory}/. Delete it (`git rm {legacy}`), merge origin/main, and "
                "rerun tools/scripts/agent_capability_manifest.py --write")
    expected: list[tuple[pathlib.Path, dict[str, str] | None]] = []
    if surface_document is not None:
        try:
            expected.append((SURFACE_DIR, split_surface(surface_document)))
        except StoreError as error:
            problems.append(f"{SURFACE_DIR}: {error}")
    if isinstance(history, dict) and "_invalid" not in history:
        try:
            expected.append((HISTORY_DIR, split_history(history)))
        except StoreError as error:
            problems.append(f"{HISTORY_DIR}: {error}")
        try:
            problems.extend(join_history(read_dir(root / HISTORY_DIR))[1])
        except StoreError:
            pass  # reported by the history loader
    if fingerprints is not None:
        expected.append((FINGERPRINT_DIR, split_fingerprints(fingerprints)))
    for directory, files in expected:
        actual = read_dir(root / directory)
        for rel in sorted(set(files) | set(actual)):
            path = f"{directory.as_posix()}/{rel}"
            if rel not in actual:
                problems.append(f"{path} is missing; rerun --write")
            elif rel not in files:
                problems.append(f"{path} is not part of the generated record; rerun --write")
            elif actual[rel] != files[rel]:
                problems.append(f"{path} is not in its rendered form; rerun --write")
    return problems


def outputs(
    root: pathlib.Path,
    surface_document: dict[str, Any],
    history: dict[str, Any],
    fingerprints: dict[str, str],
) -> tuple[dict[pathlib.Path, str], list[pathlib.Path]]:
    """(files to write, files to remove) to make the tree hold these documents.

    Only fragments whose text changed are written, a fragment no longer in a
    document is removed, and a retired single file is removed, so one
    regeneration touches exactly the entries that moved."""
    root = pathlib.Path(root)
    writes: dict[pathlib.Path, str] = {}
    removals: list[pathlib.Path] = []
    for directory, files in ((SURFACE_DIR, split_surface(surface_document)),
                             (HISTORY_DIR, split_history(history)),
                             (FINGERPRINT_DIR, split_fingerprints(fingerprints))):
        existing = read_dir(root / directory)
        for rel, text in sorted(files.items()):
            if existing.get(rel) != text:
                writes[root / directory / rel] = text
        removals.extend(root / directory / rel for rel in sorted(set(existing) - set(files)))
    for legacy in (SURFACE_LEGACY, HISTORY_LEGACY):
        if (root / legacy).exists():
            removals.append(root / legacy)
    return writes, removals


def replace_directory(root: pathlib.Path, directory: pathlib.Path, files: dict[str, str]) -> None:
    """Make `directory` hold exactly `files` (relative path -> text)."""
    base = pathlib.Path(root) / directory
    existing = read_dir(base)
    for rel, text in files.items():
        if existing.get(rel) != text:
            target = base / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
    for rel in set(existing) - set(files):
        (base / rel).unlink()


def generated_roots(root: pathlib.Path) -> tuple[list[pathlib.Path], list[pathlib.Path]]:
    """(single files, directories) a capability write may replace or remove."""
    root = pathlib.Path(root)
    return ([root / SURFACE_LEGACY, root / HISTORY_LEGACY],
            [root / SURFACE_DIR, root / HISTORY_DIR, root / FINGERPRINT_DIR])


# ── A revision ──────────────────────────────────────────────────────────────

def _git(root: pathlib.Path, *args: str, stdin: bytes | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(root), *args], input=stdin, capture_output=True)


def ref_files(root: pathlib.Path, rev: str, directory: pathlib.Path) -> dict[str, str] | None:
    """The fragment texts under `directory` at `rev`; None when it has none.

    Raises StoreError when git cannot list or read them."""
    prefix = directory.as_posix() + "/"
    listing = _git(root, "ls-tree", "-r", "-z", "--full-tree", rev, "--", prefix)
    if listing.returncode != 0:
        raise StoreError(f"git ls-tree {rev} {prefix} failed: "
                         f"{listing.stderr.decode('utf-8', 'replace').strip()}")
    blobs: dict[str, str] = {}
    for record in listing.stdout.decode("utf-8", "replace").split("\0"):
        meta, _, path = record.partition("\t")
        parts = meta.split()
        if len(parts) == 3 and parts[1] == "blob" and path.startswith(prefix):
            blobs[path[len(prefix):]] = parts[2]
    if not blobs:
        return None
    unique = sorted(set(blobs.values()))
    out = _git(root, "cat-file", "--batch", stdin=("\n".join(unique) + "\n").encode())
    if out.returncode != 0:
        raise StoreError(f"git cat-file failed reading {prefix} at {rev}")
    texts: dict[str, str] = {}
    data, pos = out.stdout, 0
    while pos < len(data):
        end = data.index(b"\n", pos)
        header = data[pos:end].split()
        pos = end + 1
        if len(header) < 3 or header[1] == b"missing":
            raise StoreError(f"git is missing an object under {prefix} at {rev}")
        size = int(header[2])
        texts[header[0].decode()] = data[pos:pos + size].decode("utf-8", "replace")
        pos += size + 1
    return {rel: texts[sha] for rel, sha in blobs.items() if not _ignored(rel)}


_DIRECTORY_FOR = {
    SURFACE_LEGACY.as_posix(): (SURFACE_DIR, lambda files: render_surface(join_surface(files))),
    HISTORY_LEGACY.as_posix(): (HISTORY_DIR, lambda files: render_history(join_history(files)[0])),
}


def text_at(root: pathlib.Path, rev: str, legacy: pathlib.Path) -> str | None:
    """The single-file text of a record at `rev`, from either layout; None when absent.

    Raises StoreError when the fragments at `rev` cannot be read or joined."""
    directory, render = _DIRECTORY_FOR[pathlib.Path(legacy).as_posix()]
    files = ref_files(root, rev, directory)
    if files is not None:
        return render(files)
    shown = _git(root, "show", f"{rev}:{pathlib.Path(legacy).as_posix()}")
    if shown.returncode != 0:
        return None
    return shown.stdout.decode("utf-8", "replace")


def read_at(root: pathlib.Path, comparison: Any, legacy: pathlib.Path) -> Any:
    """`gate_common.read_git_path` for a record that may be a fragment directory.

    The result names the legacy path and carries the single-file text the
    fragments assemble to, so a caller reads either layout the same way."""
    from gate_common import read_git_path

    legacy = pathlib.Path(legacy)
    directory, render = _DIRECTORY_FOR[legacy.as_posix()]
    if comparison.status != "available" or comparison.comparison_anchor is None:
        return read_git_path(root, comparison, legacy)
    try:
        files = ref_files(pathlib.Path(root), comparison.comparison_anchor, directory)
        if files is None:
            return read_git_path(root, comparison, legacy)
        text = render(files)
    except StoreError as error:
        return replace(comparison, path=directory.as_posix(), content=None,
                       status="command_failed", stderr=str(error)[:512])
    return replace(comparison, path=legacy.as_posix(), content=text, status="available", stderr="")


def reset_to(root: pathlib.Path, rev: str) -> list[str]:
    """Make the surface and history directories hold exactly what `rev` holds.

    Reads either layout at `rev` and writes fragments (an older revision's
    single files are split), removing any fragment `rev` lacks and any retired
    single file. This is the reset to a base that `git checkout <rev> -- <dir>`
    cannot do, since a checkout leaves files the revision does not have.
    A history `rev` holds in the retired inline schema is not converted here;
    run `agent_capability_manifest.py --write` for that. Raises StoreError."""
    root = pathlib.Path(root)
    texts = {legacy: text_at(root, rev, legacy) for legacy in (SURFACE_LEGACY, HISTORY_LEGACY)}
    missing = [str(legacy) for legacy, text in texts.items() if text is None]
    if missing:
        raise StoreError(f"{rev} holds no {', '.join(missing)} in either layout")
    try:
        surface_files = split_surface(json.loads(texts[SURFACE_LEGACY]))
        history_files = split_history(json.loads(texts[HISTORY_LEGACY]))
    except json.JSONDecodeError as error:
        raise StoreError(f"{rev}: {error}") from error
    replace_directory(root, SURFACE_DIR, surface_files)
    replace_directory(root, HISTORY_DIR, history_files)
    for legacy in (SURFACE_LEGACY, HISTORY_LEGACY):
        (root / legacy).unlink(missing_ok=True)
    return [f"{SURFACE_DIR}/ ({len(surface_files)} files)", f"{HISTORY_DIR}/ ({len(history_files)} files)"]


def migrate(root: pathlib.Path) -> list[str]:
    """Split the single surface and history files in place; return what moved.

    Refuses (StoreError) unless each split re-renders to the exact bytes of
    the file it replaces, so a lossy split can never delete its source."""
    root = pathlib.Path(root)
    moved: list[str] = []
    for legacy, directory, split, join, render in (
        (SURFACE_LEGACY, SURFACE_DIR, split_surface, join_surface, render_surface),
        (HISTORY_LEGACY, HISTORY_DIR, split_history, lambda f: join_history(f)[0], render_history),
    ):
        path = root / legacy
        if not path.is_file():
            continue
        if (root / directory).exists():
            raise StoreError(f"both {legacy} and {directory}/ exist")
        original = path.read_text(encoding="utf-8")
        files = split(json.loads(original))
        if render(join(files)) != original:
            raise StoreError(f"{legacy} does not round-trip through {directory}/; left in place")
        for rel, text in files.items():
            target = root / directory / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
        if render(join(read_dir(root / directory))) != original:
            raise StoreError(f"{directory}/ does not re-render {legacy} byte for byte")
        path.unlink()
        moved.append(f"{legacy} -> {directory}/ ({len(files)} files)")
    return moved


def main(argv: list[str]) -> int:
    """`agent_capability_store.py [--repo-root R] {show-surface|show-history} [REV] | reset REV | migrate`"""
    import argparse
    import sys

    parser = argparse.ArgumentParser(allow_abbrev=False, description=main.__doc__)
    parser.add_argument("--repo-root", default=str(pathlib.Path(__file__).resolve().parents[2]))
    sub = parser.add_subparsers(dest="command", required=True)
    for name, legacy in (("show-surface", SURFACE_LEGACY), ("show-history", HISTORY_LEGACY)):
        show = sub.add_parser(name, allow_abbrev=False,
                              help=f"print the single-file text of {legacy}")
        show.add_argument("rev", nargs="?", help="a revision (default: the working tree)")
        show.set_defaults(legacy=legacy)
    sub.add_parser("migrate", allow_abbrev=False,
                   help="split the single surface and history files into fragments")
    reset = sub.add_parser("reset", allow_abbrev=False,
                           help="make the surface and history directories hold exactly REV's records")
    reset.add_argument("rev")
    args = parser.parse_args(argv[1:])
    root = pathlib.Path(args.repo_root)
    try:
        if args.command == "migrate":
            for line in migrate(root) or ["nothing to migrate"]:
                print(f"agent-capability-store: {line}")
            return 0
        if args.command == "reset":
            for line in reset_to(root, args.rev):
                print(f"agent-capability-store: reset {line} to {args.rev}")
            return 0
        if args.rev:
            text = text_at(root, args.rev, args.legacy)
        else:
            document = (load_surface(root) if args.legacy == SURFACE_LEGACY else load_history(root))
            if document is None or "_invalid" in document:
                text = None
            else:
                text = (render_surface(document) if args.legacy == SURFACE_LEGACY
                        else render_history(document))
    except StoreError as error:
        print(f"agent-capability-store: ERROR: {error}", file=sys.stderr)
        return 2
    if text is None:
        print(f"agent-capability-store: no readable {args.legacy}", file=sys.stderr)
        return 2
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv))
