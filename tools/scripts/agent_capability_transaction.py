"""Crash-recoverable replacement of the capability generator's coupled outputs."""
from __future__ import annotations

import hashlib
import json
import os
import pathlib
import tempfile
import uuid
from collections.abc import Iterable, Mapping


TRANSACTION_SCHEMA = "pulp.agent-capability-write-transaction.v1"


class SimulatedInterruption(RuntimeError):
    """Test-only interruption after a requested number of replacements."""


def _digest(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _fsync_directory(path: pathlib.Path) -> None:
    if os.name == "nt":
        return
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_staged(path: pathlib.Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        _fsync_directory(path.parent)
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def _write_journal(path: pathlib.Path, document: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary_path = pathlib.Path(temporary)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(document, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_path, path)
        _fsync_directory(path.parent)
    finally:
        temporary_path.unlink(missing_ok=True)


def _load_journal(path: pathlib.Path) -> list[dict[str, str]]:
    try:
        document = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid capability write transaction {path}: {error}") from error
    if (
        not isinstance(document, dict)
        or set(document) != {"schema", "entries"}
        or document.get("schema") != TRANSACTION_SCHEMA
        or not isinstance(document.get("entries"), list)
        or not document["entries"]
    ):
        raise RuntimeError(f"invalid capability write transaction fields: {path}")
    entries: list[dict[str, str]] = []
    for entry in document["entries"]:
        if (
            not isinstance(entry, dict)
            or set(entry) != {"target", "staged", "sha256"}
            or not all(isinstance(entry.get(key), str) for key in entry)
        ):
            raise RuntimeError(f"invalid capability write transaction entry: {path}")
        entries.append(entry)
    return entries


def recover_transaction(
    journal: pathlib.Path,
    expected_targets: Iterable[pathlib.Path],
    *,
    interrupt_after: int | None = None,
) -> bool:
    """Finish a journaled replacement, returning whether recovery was needed."""
    if not journal.exists():
        return False
    entries = _load_journal(journal)
    expected_target_set = {path.resolve() for path in expected_targets}
    actual = [pathlib.Path(entry["target"]) for entry in entries]
    if len(actual) != len(expected_target_set) or set(actual) != expected_target_set:
        raise RuntimeError(
            "capability write transaction targets do not match generated outputs"
        )
    transaction_ids: set[str] = set()
    for entry, target in zip(entries, actual, strict=True):
        staged = pathlib.Path(entry["staged"])
        prefix = f".{target.name}."
        suffix = ".staged"
        if staged.parent != target.parent or not (
            staged.name.startswith(prefix) and staged.name.endswith(suffix)
        ):
            raise RuntimeError(f"invalid staged capability output path: {staged}")
        transaction_id = staged.name[len(prefix) : -len(suffix)]
        if len(transaction_id) != 32 or any(
            character not in "0123456789abcdef" for character in transaction_id
        ):
            raise RuntimeError(f"invalid staged capability transaction id: {staged}")
        transaction_ids.add(transaction_id)
    if len(transaction_ids) != 1:
        raise RuntimeError("capability write transaction mixes transaction ids")
    replaced = 0
    for entry in entries:
        target = pathlib.Path(entry["target"])
        staged = pathlib.Path(entry["staged"])
        expected = entry["sha256"]
        if staged.is_file():
            if _digest(staged.read_bytes()) != expected:
                raise RuntimeError(f"staged capability output digest mismatch: {staged}")
            target.parent.mkdir(parents=True, exist_ok=True)
            os.replace(staged, target)
            _fsync_directory(target.parent)
            replaced += 1
            if interrupt_after is not None and replaced >= interrupt_after:
                raise SimulatedInterruption("simulated capability write interruption")
        elif not target.is_file() or _digest(target.read_bytes()) != expected:
            raise RuntimeError(
                f"capability write transaction lost both staged and committed output: {target}"
            )
    journal.unlink()
    _fsync_directory(journal.parent)
    return True


def write_transaction(
    outputs: Mapping[pathlib.Path, str],
    journal: pathlib.Path,
    *,
    interrupt_after: int | None = None,
) -> None:
    """Stage every output durably, journal the batch, then commit and recover it."""
    if journal.exists():
        recover_transaction(journal, outputs)
    transaction_id = uuid.uuid4().hex
    entries: list[dict[str, str]] = []
    staged_paths: list[pathlib.Path] = []
    try:
        for target, text in outputs.items():
            target = target.resolve()
            payload = text.encode("utf-8")
            staged = target.with_name(f".{target.name}.{transaction_id}.staged")
            _write_staged(staged, payload)
            staged_paths.append(staged)
            entries.append(
                {"target": str(target), "staged": str(staged), "sha256": _digest(payload)}
            )
        _write_journal(journal, {"schema": TRANSACTION_SCHEMA, "entries": entries})
        recover_transaction(journal, outputs, interrupt_after=interrupt_after)
    except SimulatedInterruption:
        raise
    except BaseException:
        if not journal.exists():
            for staged in staged_paths:
                staged.unlink(missing_ok=True)
        raise
