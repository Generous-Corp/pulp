#!/usr/bin/env python3
"""Select one explicitly enrolled Shipyard recovery worker.

The GitHub-hosted controller is the only assignment authority. A Mac becomes
eligible only when one online, idle repository runner advertises its dedicated
recovery label. Merely being a Pulp CI runner never enrolls it.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


WORKER_ORDER = ("m3", "m5", "m1")
BASE_LABELS = {"self-hosted", "macOS", "ARM64", "pulp-build-vm"}


def _load(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _labels(runner: dict[str, Any]) -> set[str]:
    result: set[str] = set()
    for label in runner.get("labels") or []:
        if isinstance(label, dict) and isinstance(label.get("name"), str):
            result.add(label["name"])
        elif isinstance(label, str):
            result.add(label)
    return result


def select_worker(census: Any) -> dict[str, Any]:
    if not isinstance(census, dict) or not isinstance(census.get("runners"), list):
        raise ValueError("runner census must contain a runners array")
    eligible: dict[str, list[dict[str, Any]]] = {worker: [] for worker in WORKER_ORDER}
    errors: list[str] = []
    for runner in census["runners"]:
        if not isinstance(runner, dict):
            continue
        if str(runner.get("status") or "").lower() != "online" or runner.get("busy") is not False:
            continue
        labels = _labels(runner)
        if not BASE_LABELS.issubset(labels):
            continue
        advertised = [
            worker for worker in WORKER_ORDER
            if f"shipyard-recovery-{worker}" in labels
        ]
        if len(advertised) > 1:
            errors.append(
                f"runner {runner.get('name') or runner.get('id')} advertises multiple "
                "recovery workers"
            )
            continue
        if advertised:
            eligible[advertised[0]].append(runner)
    errors.extend(
        f"multiple eligible runners advertise shipyard-recovery-{worker}"
        for worker, runners in eligible.items()
        if len(runners) > 1
    )
    if errors:
        return {
            "schema_version": 1,
            "selected": None,
            "eligible_workers": [],
            "errors": errors,
        }
    eligible_workers = [worker for worker in WORKER_ORDER if eligible[worker]]
    selected = None
    if eligible_workers:
        worker = eligible_workers[0]
        runner = eligible[worker][0]
        selected = {
            "worker": worker,
            "runner_id": int(runner["id"]),
            "runner_name": str(runner.get("name") or ""),
            "label": f"shipyard-recovery-{worker}",
        }
    return {
        "schema_version": 1,
        "selected": selected,
        "eligible_workers": eligible_workers,
        "errors": [],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runners", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = select_worker(_load(args.runners))
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if not result["errors"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
