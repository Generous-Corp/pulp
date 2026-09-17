#!/usr/bin/env python3
"""Bounded disk-backed storage for the GPU-audio P4 evidence analyzer."""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import sqlite3
import tempfile
from collections.abc import Iterator, Sequence
from typing import Any


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class RecordStore(Sequence[dict[str, Any]]):
    """Disk-backed records keep large captures out of the Python heap."""

    def __init__(self) -> None:
        self._root = tempfile.TemporaryDirectory(prefix="pulp-gpu-audio-p4-")
        self._connection = sqlite3.connect(Path(self._root.name) / "records.sqlite3")
        self._connection.execute(
            "CREATE TABLE records (ordinal INTEGER PRIMARY KEY, payload TEXT NOT NULL)"
        )
        self._count = 0

    def append(self, value: dict[str, Any]) -> None:
        self._connection.execute(
            "INSERT INTO records(ordinal, payload) VALUES (?, ?)",
            (self._count, json.dumps(value, separators=(",", ":"))),
        )
        self._count += 1

    def finish(self) -> None:
        self._connection.commit()

    def __len__(self) -> int:
        return self._count

    def __getitem__(self, index: int) -> dict[str, Any]:
        if not isinstance(index, int):
            raise TypeError("record indexes must be integers")
        if index < 0:
            index += self._count
        row = self._connection.execute(
            "SELECT payload FROM records WHERE ordinal = ?", (index,)
        ).fetchone()
        if row is None:
            raise IndexError(index)
        return json.loads(row[0])

    def __iter__(self) -> Iterator[dict[str, Any]]:
        cursor = self._connection.execute("SELECT payload FROM records ORDER BY ordinal")
        for (payload,) in cursor:
            yield json.loads(payload)

    def close(self) -> None:
        if getattr(self, "_connection", None) is not None:
            self._connection.close()
            self._connection = None
            self._root.cleanup()

    def __del__(self) -> None:
        self.close()


class IdentityStore:
    """Disk-backed uniqueness set for uint64 block identities."""

    def __init__(self) -> None:
        self._root = tempfile.TemporaryDirectory(prefix="pulp-gpu-audio-p4-identities-")
        self._connection = sqlite3.connect(Path(self._root.name) / "identities.sqlite3")
        self._connection.execute(
            "CREATE TABLE identities (engine TEXT, generation TEXT, sequence TEXT, "
            "PRIMARY KEY(engine, generation, sequence)) WITHOUT ROWID"
        )

    def add(self, identity: tuple[int, int, int]) -> bool:
        try:
            self._connection.execute(
                "INSERT INTO identities VALUES (?, ?, ?)", tuple(map(str, identity))
            )
        except sqlite3.IntegrityError:
            return False
        return True

    def close(self) -> None:
        self._connection.close()
        self._root.cleanup()


class MetricStore:
    """Exact disk-backed percentile input for bounded-memory campaign analysis."""

    def __init__(self) -> None:
        self._root = tempfile.TemporaryDirectory(prefix="pulp-gpu-audio-p4-metrics-")
        self._connection = sqlite3.connect(Path(self._root.name) / "metrics.sqlite3")
        self._connection.execute(
            "CREATE TABLE metrics (path TEXT NOT NULL, trial_id INTEGER NOT NULL, "
            "name TEXT NOT NULL, value REAL NOT NULL)"
        )

    def add(self, path: str, trial_id: int, name: str, value: float) -> None:
        self._connection.execute(
            "INSERT INTO metrics(path, trial_id, name, value) VALUES (?, ?, ?, ?)",
            (path, trial_id, name, value),
        )

    def finish(self) -> None:
        self._connection.commit()
        self._connection.execute(
            "CREATE INDEX metrics_lookup ON metrics(path, name, trial_id, value)"
        )

    def count(self, path: str, name: str, trial_id: int | None = None) -> int:
        query = "SELECT COUNT(*) FROM metrics WHERE path = ? AND name = ?"
        arguments: list[Any] = [path, name]
        if trial_id is not None:
            query += " AND trial_id = ?"
            arguments.append(trial_id)
        return int(self._connection.execute(query, arguments).fetchone()[0])

    def mean(self, path: str, name: str, trial_id: int | None = None) -> float | None:
        query = "SELECT AVG(value) FROM metrics WHERE path = ? AND name = ?"
        arguments: list[Any] = [path, name]
        if trial_id is not None:
            query += " AND trial_id = ?"
            arguments.append(trial_id)
        value = self._connection.execute(query, arguments).fetchone()[0]
        return float(value) if value is not None else None

    def percentile(self, path: str, name: str, pct: float,
                   trial_id: int | None = None) -> float | None:
        count = self.count(path, name, trial_id)
        if count == 0:
            return None
        position = (count - 1) * pct / 100.0
        low = math.floor(position)
        high = math.ceil(position)
        query = "SELECT value FROM metrics WHERE path = ? AND name = ?"
        arguments: list[Any] = [path, name]
        if trial_id is not None:
            query += " AND trial_id = ?"
            arguments.append(trial_id)
        query += " ORDER BY value LIMIT 1 OFFSET ?"
        low_value = float(self._connection.execute(query, arguments + [low]).fetchone()[0])
        if low == high:
            return low_value
        high_value = float(self._connection.execute(query, arguments + [high]).fetchone()[0])
        return low_value * (1.0 - (position - low)) + high_value * (position - low)

    def maximum(self, path: str, name: str) -> float | None:
        value = self._connection.execute(
            "SELECT MAX(value) FROM metrics WHERE path = ? AND name = ?", (path, name)
        ).fetchone()[0]
        return float(value) if value is not None else None

    def close(self) -> None:
        self._connection.close()
        self._root.cleanup()
