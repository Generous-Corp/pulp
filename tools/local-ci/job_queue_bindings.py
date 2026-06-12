"""Bindings from the local_ci facade to queue persistence helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def normalize_job(bindings: Mapping[str, Any], job: dict) -> dict:
    return _binding(bindings, "_job_queue").normalize_job(job)


def load_queue_unlocked(bindings: Mapping[str, Any]) -> list[dict]:
    return _binding(bindings, "_job_queue").load_queue_unlocked()


def save_queue_unlocked(bindings: Mapping[str, Any], queue: list[dict]) -> None:
    return _binding(bindings, "_job_queue").save_queue_unlocked(queue)
