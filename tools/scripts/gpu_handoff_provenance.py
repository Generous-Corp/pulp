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


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_HANDOFF = ROOT / "docs/status/gpu-vellum-handoff.yaml"
DEFAULT_RECEIPT = ROOT / "docs/validation/gpu-handoff-provenance/receipt.json"

HANDOFF_REPO = "Generous-Corp/pulp"
HANDOFF_SELF_PATH = "docs/status/gpu-vellum-handoff.yaml"
IDENTITY_FIELDS = ("revision", "object_id", "object_type")
OBJECT_TYPES = frozenset({"blob", "tree"})
RECEIPT_SCHEMA = "pulp.gpu-handoff-provenance-receipt.v1"
GIT_TIMEOUT_SECONDS = 30


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

    return json.dumps(document, indent=2) + "\n"


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


def resolve_source_commit(root: pathlib.Path, requested: str) -> str:
    """Authenticate one exact commit and prove it is reachable from HEAD.

    Every emitted revision is an ancestor of the source commit, so requiring the
    source commit to be an ancestor of HEAD is what makes the validator's own
    ancestry check pass for all of them at once.
    """

    try:
        commit = git_output(root, ["rev-parse", "--verify", f"{requested}^{{commit}}"])
    except ProvenanceError as error:
        raise ProvenanceError(
            f"source commit {requested!r} does not resolve to a commit: {error}"
        ) from error
    if git_status_code(root, ["merge-base", "--is-ancestor", commit, "HEAD"]) != 0:
        raise ProvenanceError(
            f"source commit {commit} is not an ancestor of HEAD; identities "
            "generated from it cannot satisfy the handoff validator"
        )
    return commit


def resolve_identity(root: pathlib.Path, commit: str, path: str) -> Identity:
    """Derive one row's identity fields from the single source commit."""

    revision = git_output(root, ["log", "-1", "--format=%H", commit, "--", path])
    if not revision:
        raise ProvenanceError(
            f"path {path!r} has no commit history at source commit {commit}"
        )
    object_id = git_output(root, ["rev-parse", f"{revision}:{path}"])
    tree_object_id = git_output(root, ["rev-parse", f"{commit}:{path}"])
    if object_id != tree_object_id:
        raise ProvenanceError(
            f"path {path!r} resolves to {object_id} at its owning revision "
            f"{revision} but {tree_object_id} at source commit {commit}"
        )
    object_type = git_output(root, ["cat-file", "-t", object_id])
    if object_type not in OBJECT_TYPES:
        raise ProvenanceError(
            f"path {path!r} resolves to unsupported object type {object_type!r}"
        )
    return Identity(revision, object_id, object_type)


def resolve_inventory_identities(
    root: pathlib.Path, commit: str, inventory: list[PathRow]
) -> dict[str, Identity]:
    """Resolve each distinct path once and reuse it across duplicate rows."""

    identities: dict[str, Identity] = {}
    for row in inventory:
        if row.path not in identities:
            identities[row.path] = resolve_identity(root, commit, row.path)
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


def validate_with_catalog(document: dict[str, Any], root: pathlib.Path) -> list[str]:
    """Run the existing fail-closed validator against a candidate ledger.

    The validator remains the authority on acceptance. This tool never relaxes
    it; it only refuses to emit output the validator would reject.
    """

    helper = pathlib.Path(__file__).resolve().parent / "gpu_recipe_catalog.py"
    spec = importlib.util.spec_from_file_location("gpu_handoff_provenance_catalog", helper)
    if spec is None or spec.loader is None:
        raise ProvenanceError(f"cannot load the handoff validator from {helper}")
    catalog = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(catalog)
    problems = list(catalog.validate_handoff(document))
    problems.extend(catalog.validate_handoff_routing(document, root))
    return problems


def repair_command(handoff: pathlib.Path, commit: str) -> str:
    """Return the exact invocation that repairs the reported drift."""

    try:
        relative = handoff.resolve().relative_to(ROOT)
        location = f" --handoff {relative}"
    except ValueError:
        location = f" --handoff {handoff}"
    if location == f" --handoff {DEFAULT_HANDOFF.relative_to(ROOT)}":
        location = ""
    return f"python3 tools/scripts/gpu_handoff_provenance.py write{location} --source-commit {commit}"


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
    commit = resolve_source_commit(args.root, args.source_commit)
    identities = resolve_inventory_identities(args.root, commit, inventory)
    drifts = compare_inventory(document, inventory, identities)
    problems = validate_with_catalog(document, args.root)
    command = repair_command(args.handoff, commit)

    if args.json:
        print(
            json.dumps(
                {
                    "source_commit": commit,
                    "row_count": len(inventory),
                    "drift_count": len(drifts),
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
        if drifts or problems:
            print(f"gpu-handoff-provenance: repair with: {command}")
        else:
            print("gpu-handoff-provenance: OK: every pinned identity matches")
    return 1 if drifts or problems else 0


def command_write(args: argparse.Namespace) -> int:
    document = load_handoff(args.handoff)
    inventory = canonical_inventory(document)
    commit = resolve_source_commit(args.root, args.source_commit)

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

    identities = resolve_inventory_identities(args.root, commit, inventory)
    updated = apply_identities(document, inventory, identities)
    rendered = serialize_handoff(updated)

    problems = validate_with_catalog(updated, args.root)
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
    commit = resolve_source_commit(args.root, args.source_commit)
    rendered = serialize_handoff(document)
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


if __name__ == "__main__":
    raise SystemExit(main())
