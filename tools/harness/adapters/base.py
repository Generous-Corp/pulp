"""Adapter interface shared by every surface."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Optional

from ..status import Status, map_catalog_status_to_expected


@dataclass
class CatalogEntry:
    """One row of compat.json — surface-namespaced key + the per-entry payload."""

    surface: str  # e.g. "yoga"
    name: str  # e.g. "yoga/flexDirection"
    status: Optional[str] = None  # supported | partial | missing | wontfix
    maps_to: Optional[str] = None
    supported_values: list[str] = field(default_factory=list)
    unsupported_values: list[str] = field(default_factory=list)
    tests: list[str] = field(default_factory=list)
    notes: Optional[str] = None
    issue: Optional[str] = None

    @classmethod
    def from_compat_json(cls, surface: str, name: str, payload: dict[str, Any]) -> "CatalogEntry":
        return cls(
            surface=surface,
            name=name,
            status=payload.get("status"),
            maps_to=payload.get("mapsTo"),
            supported_values=list(payload.get("supportedValues") or []),
            unsupported_values=list(payload.get("unsupportedValues") or []),
            tests=list(payload.get("tests") or []),
            notes=payload.get("notes"),
            issue=payload.get("issue"),
        )

    @property
    def short_name(self) -> str:
        """`yoga/flexDirection` -> `flexDirection`."""
        if "/" in self.name:
            return self.name.split("/", 1)[1]
        return self.name

    @property
    def expected_status(self) -> Status:
        """Harness-taxonomy translation of the catalog status field."""
        return map_catalog_status_to_expected(self.status)


@dataclass
class Result:
    """The harness's verdict for one entry."""

    entry: CatalogEntry
    status: Status
    detail: str = ""
    matched_supported: list[str] = field(default_factory=list)
    unmatched_supported: list[str] = field(default_factory=list)
    extra_unsupported: list[str] = field(default_factory=list)

    @property
    def drifts(self) -> bool:
        """True when the harness verdict disagrees with the catalog status."""
        return self.status is not self.entry.expected_status

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.entry.name,
            "surface": self.entry.surface,
            "catalog_status": self.entry.status,
            "harness_status": self.status.value,
            "drift": self.drifts,
            "detail": self.detail,
            "matched_supported": self.matched_supported,
            "unmatched_supported": self.unmatched_supported,
            "extra_unsupported": self.extra_unsupported,
            "issue": self.entry.issue,
        }


class AdapterBase:
    """Each surface implements `run` to classify a single catalog entry."""

    surface: str = ""

    def __init__(self, repo_root):
        self.repo_root = repo_root

    def run(self, entry: CatalogEntry) -> Result:  # pragma: no cover - interface
        raise NotImplementedError
