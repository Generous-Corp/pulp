"""Semantic JSON differ for visual layout snapshots."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Difference:
    path: str
    expected: Any
    actual: Any
    message: str

    def format(self) -> str:
        return (
            f"{self.path}: {self.message}; "
            f"expected={self.expected!r} actual={self.actual!r}"
        )


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def tolerance_from_fixture(fixture: dict[str, Any] | None, default: float = 0.001) -> float:
    if not fixture:
        return default
    tolerance = fixture.get("tolerance")
    if not isinstance(tolerance, dict):
        return default
    value = tolerance.get("semantic", tolerance.get("rect", tolerance.get("number", default)))
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def compare(expected: Any, actual: Any, *, tolerance: float = 0.001) -> list[Difference]:
    differences: list[Difference] = []
    _compare_value(expected, actual, "$", tolerance, differences)
    return differences


def diff_files(expected_path: Path, actual_path: Path, *, tolerance: float = 0.001) -> list[Difference]:
    return compare(load_json(expected_path), load_json(actual_path), tolerance=tolerance)


def format_differences(differences: list[Difference], *, limit: int = 20) -> str:
    if not differences:
        return ""
    shown = differences[:limit]
    lines = [d.format() for d in shown]
    if len(differences) > limit:
        lines.append(f"... {len(differences) - limit} more difference(s)")
    return "\n".join(lines)


def _compare_value(
    expected: Any,
    actual: Any,
    path: str,
    tolerance: float,
    differences: list[Difference],
) -> None:
    if isinstance(expected, bool) or isinstance(actual, bool):
        if expected is not actual:
            differences.append(Difference(path, expected, actual, "boolean mismatch"))
        return

    if _is_number(expected) and _is_number(actual):
        delta = abs(float(expected) - float(actual))
        if delta > tolerance:
            differences.append(
                Difference(path, expected, actual, f"numeric delta {delta:.6g} > {tolerance:g}")
            )
        return

    if isinstance(expected, dict) and isinstance(actual, dict):
        expected_keys = set(expected)
        actual_keys = set(actual)
        for key in sorted(expected_keys - actual_keys):
            differences.append(Difference(f"{path}.{key}", expected[key], None, "missing key"))
        for key in sorted(actual_keys - expected_keys):
            differences.append(Difference(f"{path}.{key}", None, actual[key], "unexpected key"))
        for key in sorted(expected_keys & actual_keys):
            _compare_value(expected[key], actual[key], f"{path}.{key}", tolerance, differences)
        return

    if isinstance(expected, list) and isinstance(actual, list):
        if len(expected) != len(actual):
            differences.append(
                Difference(path, len(expected), len(actual), "array length mismatch")
            )
        for i, (exp, act) in enumerate(zip(expected, actual)):
            _compare_value(exp, act, f"{path}[{i}]", tolerance, differences)
        return

    if expected != actual:
        differences.append(Difference(path, expected, actual, "value mismatch"))


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)
