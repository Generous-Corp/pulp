#!/usr/bin/env python3
"""Regenerate the GPU Vellum handoff identity ledger from one exact commit.

``docs/status/gpu-vellum-handoff.yaml`` pins every referenced Pulp path to a
revision, object id, and object type. ``gpu_recipe_catalog.py`` validates those
identities fail-closed but cannot produce a correction, so this module owns the
only supported regeneration path.

The canonical source list is the ordered inventory of
``entries[*].pulp_paths[*]`` rows already declared in the handoff document. The
document stays the authority for which paths matter; this tool derives only the
three identity fields, and it derives all of them from a single authenticated
commit so a regenerated ledger can never mix trees from different revisions.

Identity contract enforced by the validator, and therefore reproduced here for
each path P at source commit C:

    revision    = git log -1 --format=%H C -- P
    object_id   = git rev-parse revision:P
    object_type = git cat-file -t object_id

The validator additionally requires ``git rev-parse HEAD:P == object_id`` and
``git log -1 --format=%H HEAD -- P == revision``, which is why C defaults to
HEAD and must be an ancestor of it. It is also why a regenerated ledger lands as
its own commit: amending a commit that touches a pinned path changes that path's
owning revision and re-stales the row that was just repaired.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
from typing import Any, Iterator, NamedTuple

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import connected_git_history  # noqa: E402


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_HANDOFF = ROOT / "docs/status/gpu-vellum-handoff.yaml"
RECEIPT_RELATIVE = pathlib.Path("docs/validation/gpu-handoff-provenance/receipt.json")
DEFAULT_RECEIPT = ROOT / RECEIPT_RELATIVE

HANDOFF_REPO = "Generous-Corp/pulp"
HANDOFF_SELF_PATH = "docs/status/gpu-vellum-handoff.yaml"
IDENTITY_FIELDS = ("revision", "object_id", "object_type")
OBJECT_TYPES = frozenset({"blob", "tree"})
RECEIPT_SCHEMA = "pulp.gpu-handoff-provenance-receipt.v1"
GIT_TIMEOUT_SECONDS = 30

# Identity is immutable for a given (checkout, source commit, HEAD, path), and
# the validator module keeps Git caches of its own that only pay off when it is
# imported once. Both caches exist so a suite that validates several times does
# not re-shell the same hundreds of Git queries.
_IDENTITY_CACHE: dict[tuple[str, str, str, str], "Identity"] = {}
_CATALOG_MODULE: Any = None
# Reading the graft-boundary list is one Git query plus one file read, and the
# answer cannot change mid-run. Caching it per checkout keeps the guard below off
# the per-path hot loop, which resolves a hundred rows.
_BOUNDARY_CACHE: dict[str, set[str]] = {}


class ProvenanceError(RuntimeError):
    """The handoff ledger cannot be regenerated from an authenticated source."""


class PathRow(NamedTuple):
    """One canonical inventory row addressed by its position in the document."""

    entry_index: int
    row_index: int
    path: str

    @property
    def label(self) -> str:
        return f"entries[{self.entry_index}].pulp_paths[{self.row_index}]"


class Identity(NamedTuple):
    """The three derived fields the validator compares against Git."""

    revision: str
    object_id: str
    object_type: str

    def as_dict(self) -> dict[str, str]:
        return {
            "revision": self.revision,
            "object_id": self.object_id,
            "object_type": self.object_type,
        }


def git_output(root: pathlib.Path, arguments: list[str]) -> str:
    """Run one Git query and reject any nonzero status fail-closed."""

    completed = subprocess.run(
        ["git", *arguments],
        cwd=root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=GIT_TIMEOUT_SECONDS,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or f"exit status {completed.returncode}"
        raise ProvenanceError(f"git {' '.join(arguments)} failed: {detail}")
    return completed.stdout.strip()


def git_status_code(root: pathlib.Path, arguments: list[str]) -> int:
    """Run one Git predicate whose exit status is the answer."""

    return subprocess.run(
        ["git", *arguments],
        cwd=root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=GIT_TIMEOUT_SECONDS,
        check=False,
    ).returncode


def load_handoff(path: pathlib.Path) -> dict[str, Any]:
    """Read the handoff ledger, which is JSON carried under a .yaml name."""

    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise ProvenanceError(f"cannot read handoff document {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise ProvenanceError(f"handoff document {path} is not valid JSON: {error}") from error
    if not isinstance(document, dict):
        raise ProvenanceError(f"handoff document {path} is not an object")
    return document


def serialize_handoff(document: dict[str, Any]) -> str:
    """Emit the ledger deterministically in the checked-in encoding."""

    return (
        json.dumps(document, indent=2, ensure_ascii=True, separators=(",", ": "))
        + "\n"
    )


def canonical_inventory(document: dict[str, Any]) -> list[PathRow]:
    """Return every Pulp path row in document order, refusing malformed input.

    Document order is the canonical order. It is stable because the generator
    never adds, removes, or reorders rows; a path list change is a human edit.
    """

    entries = document.get("entries")
    if not isinstance(entries, list) or not entries:
        raise ProvenanceError("handoff entries must be a nonempty array")

    inventory: list[PathRow] = []
    for entry_index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ProvenanceError(f"handoff entries[{entry_index}] is not an object")
        rows = entry.get("pulp_paths")
        if not isinstance(rows, list):
            raise ProvenanceError(
                f"handoff entries[{entry_index}].pulp_paths must be an array"
            )
        for row_index, row in enumerate(rows):
            location = f"entries[{entry_index}].pulp_paths[{row_index}]"
            if not isinstance(row, dict):
                raise ProvenanceError(f"handoff {location} is not an object")
            path = row.get("path")
            if not isinstance(path, str) or not path:
                raise ProvenanceError(f"handoff {location} has no path")
            repo = row.get("repo")
            if repo != HANDOFF_REPO:
                raise ProvenanceError(
                    f"handoff {location} names repository {repo!r}, not {HANDOFF_REPO!r}"
                )
            if path == HANDOFF_SELF_PATH:
                raise ProvenanceError(
                    f"handoff {location} creates a circular self-reference"
                )
            inventory.append(PathRow(entry_index, row_index, path))
    if not inventory:
        raise ProvenanceError("handoff declares no Pulp paths to pin")
    return inventory


def canonical_paths(document: dict[str, Any]) -> list[str]:
    """Return the sorted unique path set the inventory covers."""

    return sorted({row.path for row in canonical_inventory(document)})


def newest_pinned_ancestor(root: pathlib.Path, paths: list[str]) -> str | None:
    """Name the newest commit reachable from HEAD that touched a pinned path.

    Identities derived there are identical to HEAD's for every pinned path,
    because by construction nothing later touched one, so it satisfies the
    validator exactly as HEAD does. Unlike HEAD it does not move when an
    unrelated commit lands, which is what makes it the commit to record once an
    amend or a rebase has orphaned the one a receipt named.

    Returns ``None`` rather than raising: this only ever enriches an error that
    is already being reported, and a checkout too shallow to answer must not
    turn that error into a different one.
    """

    if not paths:
        return None
    try:
        commit = git_output(root, ["log", "-1", "--format=%H", "HEAD", "--", *paths])
    except ProvenanceError:
        return None
    return commit or None


def resolve_source_commit(
    root: pathlib.Path, requested: str, canonical: list[str] | None = None
) -> str:
    """Authenticate one exact commit and prove it is reachable from HEAD.

    Every emitted revision is an ancestor of the source commit, so requiring the
    source commit to be an ancestor of HEAD is what makes the validator's own
    ancestry check pass for all of them at once.

    ``canonical`` is the pinned path inventory. It is optional because the
    ancestry rule does not depend on it; when supplied, a failure names a commit
    that would satisfy the rule instead of leaving the reader to derive one.
    """

    try:
        commit = git_output(root, ["rev-parse", "--verify", f"{requested}^{{commit}}"])
    except ProvenanceError as error:
        raise ProvenanceError(
            f"source commit {requested!r} does not resolve to a commit: {error}"
        ) from error
    if git_status_code(root, ["merge-base", "--is-ancestor", commit, "HEAD"]) != 0:
        # Rewriting the commit a receipt pins is the way this is reached in
        # practice, and the obvious repair regresses: regenerating creates a new
        # commit, so recording that one is never satisfiable. Say which
        # operations orphan the pin, and name an existing commit that does not.
        detail = (
            f"source commit {commit} is not an ancestor of HEAD; identities "
            "generated from it cannot satisfy the handoff validator. A rebase, "
            "amend, or squash of the pinned commit is the usual cause; merging "
            "origin/main preserves the pin where rebasing onto it does not. "
            "Record a commit that already exists rather than the one "
            "regeneration is about to create"
        )
        suggestion = newest_pinned_ancestor(root, canonical or [])
        if suggestion is not None:
            detail += f", such as {suggestion}"
        raise ProvenanceError(detail)
    return commit


def shallow_boundaries_for(root: pathlib.Path) -> set[str]:
    """Return this checkout's graft boundaries, resolved once per run."""

    key = str(root)
    cached = _BOUNDARY_CACHE.get(key)
    if cached is None:
        cached = connected_git_history.shallow_boundaries(root)
        _BOUNDARY_CACHE[key] = cached
    return cached


def resolve_identity(
    root: pathlib.Path,
    commit: str,
    path: str,
    require_current: bool = False,
) -> Identity:
    """Derive one row's identity fields from the single source commit.

    What this commit did to this path is a fact about history, so by default
    the answer is read out of history alone and a later commit cannot change
    it. That is what a reader asking "did this source commit produce these
    bytes?" needs, and it stays true however far HEAD moves on.

    ``require_current`` additionally demands that the derived blob still match
    HEAD. A generator wants that, because an identity that disagrees with HEAD
    cannot satisfy a currency-checking consumer no matter how faithfully it
    describes its source commit; raising here names the problem instead of
    emitting a ledger that fails later.
    """

    head_revision = git_output(root, ["rev-parse", "HEAD"])
    cache_key = (str(root), commit, head_revision, path, require_current)
    cached = _IDENTITY_CACHE.get(cache_key)
    if cached is not None:
        return cached

    revision = git_output(root, ["log", "-1", "--format=%H", commit, "--", path])
    boundaries = shallow_boundaries_for(root)
    # Same predicate as connected_git_history.resolves_to_boundary, applied to
    # the answer already in hand rather than re-running the identical query for
    # each of a hundred rows.
    if boundaries and (not revision or revision in boundaries):
        # Emitting this revision would pin the row to the oldest commit the
        # checkout happens to hold rather than to the commit that last changed
        # the path -- a well-formed SHA that is simply wrong, written over a
        # correct ledger with exit status 0. Refuse instead.
        raise ProvenanceError(
            f"cannot resolve the owning revision of {path!r}: this checkout's Git "
            "history is truncated, so per-path last-owner revisions resolve to the "
            "shallow graft boundary instead of the real owning commit; "
            f"{connected_git_history.REMEDY}"
        )
    if not revision:
        raise ProvenanceError(
            f"path {path!r} has no commit history at source commit {commit}"
        )
    object_id = git_output(root, ["rev-parse", f"{revision}:{path}"])
    if require_current:
        head_object = git_output(root, ["rev-parse", f"HEAD:{path}"])
        if object_id != head_object:
            raise ProvenanceError(
                f"path {path!r} is {object_id} at its owning revision {revision} "
                f"but {head_object} at HEAD, so no identity derived from source "
                f"commit {commit} can satisfy a currency-checking consumer; "
                "regenerate with --source-commit HEAD"
            )
    object_type = git_output(root, ["cat-file", "-t", object_id])
    if object_type not in OBJECT_TYPES:
        raise ProvenanceError(
            f"path {path!r} resolves to unsupported object type {object_type!r}"
        )
    identity = Identity(revision, object_id, object_type)
    _IDENTITY_CACHE[cache_key] = identity
    return identity


def resolve_inventory_identities(
    root: pathlib.Path,
    commit: str,
    inventory: list[PathRow],
    require_current: bool = False,
) -> dict[str, Identity]:
    """Resolve each distinct path once and reuse it across duplicate rows.

    ``require_current`` is forwarded unchanged; see ``resolve_identity``.
    """

    identities: dict[str, Identity] = {}
    for row in inventory:
        if row.path not in identities:
            identities[row.path] = resolve_identity(
                root, commit, row.path, require_current=require_current
            )
    return identities


def dirty_canonical_paths(root: pathlib.Path, paths: list[str]) -> list[str]:
    """Return the canonical paths whose checkout state is not committed."""

    if not paths:
        return []
    completed = subprocess.run(
        [
            "git",
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
            "--",
            *paths,
        ],
        cwd=root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=False,
        timeout=GIT_TIMEOUT_SECONDS,
        check=False,
    )
    if completed.returncode != 0:
        raise ProvenanceError("repository checkout status cannot be resolved")
    dirty: set[str] = set()
    records = completed.stdout.split(b"\0")
    index = 0
    while index < len(records) and records[index]:
        record = records[index]
        code = record[:2]
        dirty.add(record[3:].decode("utf-8", errors="surrogateescape"))
        if b"R" in code or b"C" in code:
            index += 1
            if index < len(records) and records[index]:
                dirty.add(records[index].decode("utf-8", errors="surrogateescape"))
        index += 1
    return sorted(
        path
        for path in paths
        if any(entry == path or entry.startswith(path.rstrip("/") + "/") for entry in dirty)
    )


class Drift(NamedTuple):
    """One row whose checked-in identity differs from the derived identity."""

    row: PathRow
    field: str
    checked_in: Any
    derived: str


def compare_inventory(
    document: dict[str, Any],
    inventory: list[PathRow],
    identities: dict[str, Identity],
) -> list[Drift]:
    """Report every identity field that disagrees with the source commit."""

    drifts: list[Drift] = []
    for row in inventory:
        current = document["entries"][row.entry_index]["pulp_paths"][row.row_index]
        derived = identities[row.path].as_dict()
        for field in IDENTITY_FIELDS:
            if current.get(field) != derived[field]:
                drifts.append(Drift(row, field, current.get(field), derived[field]))
    return drifts


def apply_identities(
    document: dict[str, Any],
    inventory: list[PathRow],
    identities: dict[str, Identity],
) -> dict[str, Any]:
    """Return a copy of the ledger with only the identity fields replaced.

    Key insertion order is preserved so an already-current ledger regenerates to
    the identical bytes.
    """

    updated = json.loads(json.dumps(document))
    for row in inventory:
        target = updated["entries"][row.entry_index]["pulp_paths"][row.row_index]
        derived = identities[row.path].as_dict()
        for field in IDENTITY_FIELDS:
            target[field] = derived[field]
    return updated


def validate_with_catalog(
    document: dict[str, Any],
    root: pathlib.Path,
    require_current: bool = False,
) -> list[str]:
    """Run the existing fail-closed validator against a candidate ledger.

    The validator remains the authority on acceptance. This tool never relaxes
    it; it only refuses to emit output the validator would reject.

    ``require_current`` additionally demands that every pinned path still match
    HEAD. This tool is a consumer rather than a merge gate, so it asks for that
    stronger claim: reading or regenerating the ledger is exactly when a pin
    that has fallen behind is worth reporting.
    """

    global _CATALOG_MODULE
    catalog = _CATALOG_MODULE
    if catalog is None:
        helper = pathlib.Path(__file__).resolve().parent / "gpu_recipe_catalog.py"
        spec = importlib.util.spec_from_file_location(
            "gpu_handoff_provenance_catalog", helper
        )
        if spec is None or spec.loader is None:
            raise ProvenanceError(f"cannot load the handoff validator from {helper}")
        catalog = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(catalog)
        _CATALOG_MODULE = catalog
    problems = list(catalog.validate_handoff(document))
    problems.extend(
        catalog.validate_handoff_routing(document, root, require_current=require_current)
    )
    return problems


def repair_command(handoff: pathlib.Path, commit: str) -> str:
    """Return the exact invocation that repairs the reported drift.

    ``--handoff`` is a top-level option, so it has to precede the subcommand or
    argparse rejects the very command this prints. ``--receipt`` is included
    because the published receipt is asserted against the ledger's bytes, so a
    regeneration without it leaves a different gate red.
    """

    try:
        relative = handoff.resolve().relative_to(ROOT)
        location = "" if relative == DEFAULT_HANDOFF.relative_to(ROOT) else f" --handoff {relative}"
    except ValueError:
        location = f" --handoff {handoff}"
    return (
        f"python3 tools/scripts/gpu_handoff_provenance.py{location} write "
        f"--source-commit {commit} --receipt"
    )


def build_receipt(
    document: dict[str, Any], commit: str, rendered: str, handoff: pathlib.Path
) -> dict[str, Any]:
    """Describe the source commit and the exact output it produced.

    The receipt carries no timestamp so that regenerating it from the same
    commit is reproducible.
    """

    paths = canonical_paths(document)
    inventory_digest = hashlib.sha256(
        "\n".join(paths).encode("utf-8") + b"\n"
    ).hexdigest()
    try:
        handoff_name = str(handoff.resolve().relative_to(ROOT))
    except ValueError:
        handoff_name = str(handoff)
    return {
        "schema": RECEIPT_SCHEMA,
        "generator": "tools/scripts/gpu_handoff_provenance.py",
        "handoff_path": handoff_name,
        "source_commit": commit,
        "source_repository": HANDOFF_REPO,
        "canonical_path_count": len(paths),
        "canonical_paths": paths,
        "canonical_path_sha256": inventory_digest,
        "handoff_sha256": hashlib.sha256(rendered.encode("utf-8")).hexdigest(),
    }


def receipt_binding_problems(
    handoff: pathlib.Path, receipt_path: pathlib.Path, document: dict[str, Any]
) -> list[str]:
    """Say how the receipt fails to describe the ledger on disk, if it does.

    A receipt names one ledger by content hash and canonical-path set. This is
    bytes and JSON only: no git, and no currency claim, so it holds on every
    commit and costs nothing. An empty list means the receipt binds this
    ledger; anything else means the ledger was regenerated (or merged) without
    its receipt, which `check` must report rather than exit 0 past.
    """

    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return [f"receipt {receipt_path} is unreadable: {error}"]
    if not isinstance(receipt, dict):
        return [f"receipt {receipt_path} is not a JSON object"]
    problems: list[str] = []
    schema = receipt.get("schema")
    if schema != RECEIPT_SCHEMA:
        problems.append(f"receipt schema is {schema!r}, expected {RECEIPT_SCHEMA!r}")
    # Hash the text the same way the receipt writer did, so a receipt written
    # by `receipt`/`write --receipt` on this platform compares byte-for-byte.
    actual = hashlib.sha256(
        handoff.read_text(encoding="utf-8").encode("utf-8")
    ).hexdigest()
    named = receipt.get("handoff_sha256")
    if named != actual:
        problems.append(
            f"receipt names ledger sha256 {str(named)[:12]}... but the ledger on "
            f"disk is {actual[:12]}..."
        )
    if receipt.get("canonical_paths") != canonical_paths(document):
        problems.append("receipt canonical_paths differ from the ledger's")
    return problems


def _iter_drift_lines(drifts: list[Drift]) -> Iterator[str]:
    for drift in drifts:
        yield (
            f"{drift.row.label} {drift.row.path}: {drift.field} "
            f"{drift.checked_in!r} -> {drift.derived!r}"
        )


def command_paths(args: argparse.Namespace) -> int:
    document = load_handoff(args.handoff)
    paths = canonical_paths(document)
    if args.json:
        print(json.dumps({"count": len(paths), "paths": paths}, indent=2))
    else:
        for path in paths:
            print(path)
    return 0


def command_check(args: argparse.Namespace) -> int:
    document = load_handoff(args.handoff)
    inventory = canonical_inventory(document)
    commit = resolve_source_commit(
        args.root, args.source_commit, canonical_paths(document)
    )
    identities = resolve_inventory_identities(
        args.root, commit, inventory, require_current=True
    )
    drifts = compare_inventory(document, inventory, identities)
    problems = validate_with_catalog(document, args.root, require_current=True)
    command = repair_command(args.handoff, commit)
    # The receipt is a third, independent claim: that THIS ledger's bytes are
    # the ones its published receipt describes. Every identity can match while
    # the receipt names another ledger entirely (a regeneration or a conflicted
    # merge that shipped without `--receipt`). Until this was checked here,
    # `check` exited 0 on exactly that state while the selftest that would
    # have caught it was opt-in.
    receipt_path = (
        args.receipt if args.receipt is not None else args.root / RECEIPT_RELATIVE
    )
    receipt_problems: list[str] = []
    receipt_state = "absent"
    if receipt_path.is_file():
        receipt_problems = receipt_binding_problems(args.handoff, receipt_path, document)
        receipt_state = "stale" if receipt_problems else "bound"

    if args.json:
        print(
            json.dumps(
                {
                    "source_commit": commit,
                    "row_count": len(inventory),
                    "drift_count": len(drifts),
                    "receipt": {
                        "path": str(receipt_path),
                        "state": receipt_state,
                        "problems": receipt_problems,
                    },
                    "drifts": [
                        {
                            "location": drift.row.label,
                            "path": drift.row.path,
                            "field": drift.field,
                            "checked_in": drift.checked_in,
                            "derived": drift.derived,
                        }
                        for drift in drifts
                    ],
                    "validator_problems": problems,
                    "repair_command": command,
                },
                indent=2,
            )
        )
    else:
        print(f"gpu-handoff-provenance: source commit {commit}")
        print(f"gpu-handoff-provenance: {len(inventory)} pinned rows")
        for line in _iter_drift_lines(drifts):
            print(f"gpu-handoff-provenance: STALE {line}")
        for problem in problems:
            print(f"gpu-handoff-provenance: VALIDATOR {problem}")
        for problem in receipt_problems:
            print(f"gpu-handoff-provenance: RECEIPT {problem}")
        if receipt_state == "absent":
            # Loud, not silent: an absent receipt is a state the reader must
            # see, never a quiet pass on a claim that was not examined.
            print(
                f"gpu-handoff-provenance: receipt: none at {receipt_path}; "
                "binding not checked"
            )
        if drifts or receipt_problems:
            print(f"gpu-handoff-provenance: repair with: {command}")
        elif problems:
            # Contract violations are human edits, not drift. Printing the
            # regeneration command for them sends the reader to a tool that
            # cannot fix what the validator is objecting to.
            print(
                "gpu-handoff-provenance: every pinned identity matches; the "
                "problems above are contract violations regeneration cannot repair"
            )
        elif receipt_state == "bound":
            print(
                "gpu-handoff-provenance: OK: every pinned identity matches and "
                "the receipt binds this ledger"
            )
        else:
            print("gpu-handoff-provenance: OK: every pinned identity matches")
    return 1 if drifts or problems or receipt_problems else 0


def command_write(args: argparse.Namespace) -> int:
    document = load_handoff(args.handoff)
    inventory = canonical_inventory(document)
    commit = resolve_source_commit(
        args.root, args.source_commit, canonical_paths(document)
    )

    dirty = dirty_canonical_paths(args.root, canonical_paths(document))
    if dirty:
        print(
            "gpu-handoff-provenance: refusing to generate from an unclean checkout; "
            "commit or restore these canonical paths first:",
            file=sys.stderr,
        )
        for path in dirty:
            print(f"  {path}", file=sys.stderr)
        return 2

    identities = resolve_inventory_identities(
        args.root, commit, inventory, require_current=True
    )
    updated = apply_identities(document, inventory, identities)
    rendered = serialize_handoff(updated)

    problems = validate_with_catalog(updated, args.root, require_current=True)
    if problems:
        print(
            "gpu-handoff-provenance: generated ledger still fails the handoff "
            "validator; nothing was written:",
            file=sys.stderr,
        )
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1

    previous = args.handoff.read_text(encoding="utf-8")
    if previous == rendered:
        print(
            f"gpu-handoff-provenance: OK: already generated from {commit}; "
            f"{len(inventory)} rows unchanged"
        )
    else:
        args.handoff.write_text(rendered, encoding="utf-8")
        drifts = compare_inventory(document, inventory, identities)
        print(f"gpu-handoff-provenance: regenerated {args.handoff} from {commit}")
        print(f"gpu-handoff-provenance: repaired {len(drifts)} identity fields")

    if args.receipt is not None:
        receipt = build_receipt(updated, commit, rendered, args.handoff)
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(f"gpu-handoff-provenance: wrote receipt {args.receipt}")
    return 0


def command_receipt(args: argparse.Namespace) -> int:
    document = load_handoff(args.handoff)
    commit = resolve_source_commit(
        args.root, args.source_commit, canonical_paths(document)
    )
    # Hash what the file actually holds. Re-serializing would describe bytes
    # that may differ from the ledger this receipt names.
    rendered = args.handoff.read_text(encoding="utf-8")
    receipt = build_receipt(document, commit, rendered, args.handoff)
    payload = json.dumps(receipt, indent=2) + "\n"
    if args.output is None:
        sys.stdout.write(payload)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
        print(f"gpu-handoff-provenance: wrote receipt {args.output}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--handoff", type=pathlib.Path, default=DEFAULT_HANDOFF)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    subparsers = parser.add_subparsers(dest="command", required=True)

    paths = subparsers.add_parser(
        "paths", help="print the canonical path inventory the ledger pins"
    )
    paths.add_argument("--json", action="store_true")
    paths.set_defaults(handler=command_paths)

    check = subparsers.add_parser(
        "check", help="report every stale identity and the exact repair command"
    )
    check.add_argument("--source-commit", default="HEAD")
    check.add_argument(
        "--receipt",
        type=pathlib.Path,
        default=None,
        help="receipt to bind against (default: <root>/"
        f"{RECEIPT_RELATIVE.as_posix()}; reported loudly when absent)",
    )
    check.add_argument("--json", action="store_true")
    check.set_defaults(handler=command_check)

    write = subparsers.add_parser(
        "write", help="regenerate the ledger identities from one exact commit"
    )
    write.add_argument("--source-commit", default="HEAD")
    write.add_argument(
        "--receipt",
        type=pathlib.Path,
        nargs="?",
        const=DEFAULT_RECEIPT,
        default=None,
        help="also write the provenance receipt (default path when bare)",
    )
    write.set_defaults(handler=command_write)

    receipt = subparsers.add_parser(
        "receipt", help="emit the provenance receipt for the checked-in ledger"
    )
    receipt.add_argument("--source-commit", default="HEAD")
    receipt.add_argument("--output", type=pathlib.Path, default=None)
    receipt.set_defaults(handler=command_receipt)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    args.handoff = args.handoff.resolve()
    args.root = args.root.resolve()
    try:
        return args.handler(args)
    except ProvenanceError as error:
        print(f"gpu-handoff-provenance: {error}", file=sys.stderr)
        return 2
    except subprocess.TimeoutExpired as error:
        print(f"gpu-handoff-provenance: git timed out: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"gpu-handoff-provenance: cannot write output: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
