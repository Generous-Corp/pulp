#!/usr/bin/env python3
"""Timing provenance and bounded diagnostics for campaign evidence."""

from __future__ import annotations

import math
from typing import Any


PROVENANCE_FIELDS = (
    "availability", "clock_domain", "observer", "api_source", "relation",
    "start_clock_domain", "end_clock_domain", "start_observer", "end_observer",
    "callback_mode", "event_pump_strategy", "timestamp_scope", "correlation_method",
    "uncertainty_ns", "instrumentation_overhead_ns", "instrumentation_control",
)
OBSERVATION_FIELDS = ("value_ns",) + PROVENANCE_FIELDS


class Diagnostics(list[str]):
    """Keep a malformed million-block capture from allocating a million errors."""

    def append(self, message: str) -> None:
        if len(self) < 100:
            super().append(message)
        elif len(self) == 100:
            super().append("additional evidence errors suppressed after 100 diagnostics")

    def extend(self, messages: Any) -> None:
        for message in messages:
            self.append(message)


def finite_number(value: Any) -> bool:
    try:
        return (isinstance(value, (int, float)) and not isinstance(value, bool)
                and math.isfinite(value))
    except OverflowError:
        return False


def provenance(observation: dict[str, Any]) -> dict[str, Any]:
    return {name: observation.get(name) for name in PROVENANCE_FIELDS}


def verdict_observation(observation: Any) -> bool:
    return (isinstance(observation, dict)
            and observation.get("availability") == "available"
            and observation.get("relation") in ("direct", "correlated"))


def check_timing(name: str, timing: Any, where: str, errors: list[str]) -> None:
    label = f"{where}.{name}"
    if not isinstance(timing, dict):
        errors.append(f"{label} must be an observation object")
        return
    availability = timing.get("availability")
    if availability not in ("available", "unavailable"):
        errors.append(f"{label}.availability is invalid")
    relation = timing.get("relation")
    if relation not in ("direct", "correlated", "inferred", "unavailable"):
        errors.append(f"{label}.relation is invalid")
    for field in PROVENANCE_FIELDS:
        if field in ("availability", "relation", "uncertainty_ns", "instrumentation_overhead_ns"):
            continue
        value = timing.get(field)
        if not isinstance(value, str) or not value.strip():
            errors.append(f"{label}.{field} must be non-empty")
    value = timing.get("value_ns")
    if availability == "available":
        if not finite_number(value) or not 0 <= value <= (1 << 64) - 1:
            errors.append(f"{label}.value_ns must be finite and non-negative")
        if relation == "unavailable":
            errors.append(f"{label} is available but relation is unavailable")
        for field in ("uncertainty_ns", "instrumentation_overhead_ns"):
            value = timing.get(field)
            if not finite_number(value) or not 0 <= value <= (1 << 64) - 1:
                errors.append(f"{label}.{field} must be finite and non-negative")
        if relation == "direct":
            for field in ("clock_domain", "observer"):
                if timing.get(field) != timing.get(f"start_{field}") or timing.get(field) != timing.get(f"end_{field}"):
                    errors.append(f"{label}: direct timing requires matching endpoint {field}s")
            if timing.get("correlation_method") != "not_required":
                errors.append(f"{label}: direct timing correlation_method must be not_required")
        if relation == "correlated" and timing.get("correlation_method") in (
            None, "", "none", "unavailable", "not_required", "uncorrelated",
        ):
            errors.append(f"{label}: correlated timing requires a correlation method")
    elif availability == "unavailable":
        for field in ("value_ns", "uncertainty_ns", "instrumentation_overhead_ns"):
            if timing.get(field) is not None:
                errors.append(f"{label}.{field} must be null when unavailable")
        if relation != "unavailable":
            errors.append(f"{label}.relation must be unavailable")


class ProvenanceTracker:
    """One observation description per declared path and metric, independent of blocks."""

    def __init__(self) -> None:
        self.first: dict[tuple[str, str], dict[str, Any]] = {}
        self.mismatches: set[tuple[str, str]] = set()

    def check(self, path: str, name: str, observation: Any, where: str,
              errors: list[str]) -> None:
        if not isinstance(observation, dict):
            return
        key = (path, name)
        value = provenance(observation)
        if key not in self.first:
            self.first[key] = value
        elif self.first[key] != value and key not in self.mismatches:
            errors.append(f"{where}.{name}: timing provenance must be constant within each path")
            self.mismatches.add(key)
