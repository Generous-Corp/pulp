"""Status taxonomy used by every adapter.

The harness reduces each catalog entry to one of six outcomes. The mapping
between catalog `status` (hand-edited, today) and harness `Status` (machine-
derived) is intentionally loose so the diff is informative — see the drift
list emitted alongside the coverage report.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Optional


class Status(str, Enum):
    """Harness verdict for a single catalog entry.

    Order is meaningful — better outcomes sort first. Used by the
    coverage table renderer.
    """

    PASS = "PASS"
    SUPPORTED_NO_EVIDENCE = "SUPPORTED-NO-EVIDENCE"
    DIVERGE = "DIVERGE"
    NO_OP = "NO-OP"
    NOT_IMPL = "NOT-IMPL"
    OOS = "OOS"

    @property
    def is_pass(self) -> bool:
        return self is Status.PASS

    @property
    def is_progress(self) -> bool:
        """Statuses that count as having an implementation surface."""
        return self in (Status.PASS, Status.SUPPORTED_NO_EVIDENCE, Status.DIVERGE)


# Canonical ordering for table rendering.
STATUS_ORDER = [
    Status.PASS,
    Status.SUPPORTED_NO_EVIDENCE,
    Status.DIVERGE,
    Status.NO_OP,
    Status.NOT_IMPL,
    Status.OOS,
]


@dataclass(frozen=True)
class StatusCounts:
    """Aggregated counts across one surface (or the whole run)."""

    pass_: int = 0
    supported_no_evidence: int = 0
    diverge: int = 0
    no_op: int = 0
    not_impl: int = 0
    oos: int = 0

    @property
    def total(self) -> int:
        return (self.pass_ + self.supported_no_evidence + self.diverge +
                self.no_op + self.not_impl + self.oos)

    @property
    def pass_pct(self) -> float:
        if self.total == 0:
            return 0.0
        return 100.0 * self.pass_ / self.total

    @property
    def progress_pct(self) -> float:
        """Statuses with an implementation surface / total."""
        if self.total == 0:
            return 0.0
        return 100.0 * (self.pass_ + self.supported_no_evidence + self.diverge) / self.total

    @classmethod
    def from_results(cls, statuses: list[Status]) -> "StatusCounts":
        return cls(
            pass_=sum(1 for s in statuses if s is Status.PASS),
            supported_no_evidence=sum(1 for s in statuses if s is Status.SUPPORTED_NO_EVIDENCE),
            diverge=sum(1 for s in statuses if s is Status.DIVERGE),
            no_op=sum(1 for s in statuses if s is Status.NO_OP),
            not_impl=sum(1 for s in statuses if s is Status.NOT_IMPL),
            oos=sum(1 for s in statuses if s is Status.OOS),
        )


def map_catalog_status_to_expected(catalog_status: Optional[str]) -> Status:
    """Map the hand-edited compat.json `status` field onto the harness taxonomy.

    The catalog uses `supported | partial | missing | wontfix | noop`. We map:

    * `supported` -> PASS  (claimed full implementation)
    * `partial`   -> DIVERGE  (claimed partial implementation)
    * `noop`      -> NO_OP  (intentional stub — bridge accepts the value
                              silently, e.g. `setAnimation` / `setTouchAction`
                              before their subsystems land. Distinct from
                              `missing` because the bridge entry exists.)
    * `missing`   -> NOT_IMPL  (claimed no implementation)
    * `wontfix`   -> OOS  (explicitly out of scope)
    * unknown / missing field -> NOT_IMPL

    The harness compares its verdict to this mapping. Disagreement is the
    drift signal we want to surface.

    The `noop` status distinguishes intentional bridge stubs from missing
    implementations: the bridge can intentionally NO-OP a property until
    a later subsystem lands, but no missing/not-implemented catalog status
    should map to NO_OP.
    """
    if catalog_status is None:
        return Status.NOT_IMPL
    s = catalog_status.strip().lower()
    if s == "supported":
        return Status.PASS
    if s == "partial":
        return Status.DIVERGE
    if s == "noop":
        return Status.NO_OP
    if s == "missing":
        return Status.NOT_IMPL
    if s == "wontfix":
        return Status.OOS
    return Status.NOT_IMPL
