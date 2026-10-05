#!/usr/bin/env python3
"""Validate a CDP browser-source fidelity receipt.

The Spectr browser harness is the producer of this receipt.  Pulp keeps the
consumer deliberately small and format-only: the native importer must not
need Chromium, but an import roundtrip can fail before any native comparison
when the browser oracle was incomplete.  This checker turns the producer's
positive/repeat/negative evidence into a reusable, fail-closed contract.

A valid receipt proves that both browser launches evaluated the exact source
bytes named by `sourceSha256`, reached the same visible DOM marker, produced a
non-empty root, reported no console or network failures, and wrote
byte-identical screenshots.  It also records that a planted broken mount was
rejected.  The checker never treats a missing field as an empty success value.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_REQUIRED_TOP_LEVEL = {
    "source",
    "sourceSha256",
    "positive",
    "repeat",
    "deterministicDomMarker",
    "negativeControl",
}
_REQUIRED_RUN_FIELDS = {"marker", "dom", "consoleErrors", "networkFailures", "screenshotSha256"}
_REQUIRED_DOM_FIELDS = {"title", "ready", "rootChildren"}
_REQUIRED_MARKER_FIELDS = {"count", "text"}


class ReceiptError(ValueError):
    """A receipt does not satisfy the browser fidelity contract."""


def _require_mapping(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ReceiptError(f"{name} must be an object")
    return value


def _require_string(value: Any, name: str, *, nonempty: bool = True) -> str:
    if not isinstance(value, str) or (nonempty and not value):
        raise ReceiptError(f"{name} must be a non-empty string")
    return value


def _check_keys(mapping: dict[str, Any], required: set[str], name: str) -> None:
    missing = sorted(required - mapping.keys())
    if missing:
        raise ReceiptError(f"{name} missing required field(s): {', '.join(missing)}")


def _check_sha(value: Any, name: str) -> str:
    text = _require_string(value, name)
    if not _SHA256.fullmatch(text):
        raise ReceiptError(f"{name} must be a lowercase SHA-256 digest")
    return text


def _check_source(
    source: Any,
    source_sha256: Any,
    *,
    source_override: Path | None,
) -> Path:
    recorded = Path(_require_string(source, "source")).expanduser()
    expected_sha256 = _check_sha(source_sha256, "sourceSha256")
    candidate = source_override.expanduser() if source_override is not None else recorded
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as exc:
        raise ReceiptError(f"source does not exist: {candidate}") from exc
    if not resolved.is_file():
        raise ReceiptError(f"source is not a regular file: {resolved}")
    source_bytes = resolved.read_bytes()
    if not source_bytes:
        raise ReceiptError(f"source is empty: {resolved}")
    actual_sha256 = hashlib.sha256(source_bytes).hexdigest()
    if actual_sha256 != expected_sha256:
        raise ReceiptError(
            "sourceSha256 does not match source bytes "
            f"(got {actual_sha256})"
        )
    return resolved


def _check_run(run: Any, name: str) -> dict[str, Any]:
    value = _require_mapping(run, name)
    _check_keys(value, _REQUIRED_RUN_FIELDS, name)

    marker = _require_mapping(value["marker"], f"{name}.marker")
    _check_keys(marker, _REQUIRED_MARKER_FIELDS, f"{name}.marker")
    if not isinstance(marker["count"], int) or isinstance(marker["count"], bool):
        raise ReceiptError(f"{name}.marker.count must be an integer")
    if marker["count"] != 1:
        raise ReceiptError(f"{name}.marker.count must equal 1")
    _require_string(marker["text"], f"{name}.marker.text", nonempty=False)

    dom = _require_mapping(value["dom"], f"{name}.dom")
    _check_keys(dom, _REQUIRED_DOM_FIELDS, f"{name}.dom")
    _require_string(dom["title"], f"{name}.dom.title")
    if not isinstance(dom["ready"], int) or isinstance(dom["ready"], bool):
        raise ReceiptError(f"{name}.dom.ready must be an integer")
    if dom["ready"] != 1:
        raise ReceiptError(f"{name}.dom.ready must equal 1")
    if not isinstance(dom["rootChildren"], int) or isinstance(dom["rootChildren"], bool):
        raise ReceiptError(f"{name}.dom.rootChildren must be an integer")
    if dom["rootChildren"] < 1:
        raise ReceiptError(f"{name}.dom.rootChildren must be at least 1")

    for field in ("consoleErrors", "networkFailures"):
        entries = value[field]
        if not isinstance(entries, list):
            raise ReceiptError(f"{name}.{field} must be an array")
        if entries:
            raise ReceiptError(f"{name}.{field} must be empty (found {len(entries)})")
        if any(not isinstance(item, str) for item in entries):
            raise ReceiptError(f"{name}.{field} entries must be strings")

    _check_sha(value["screenshotSha256"], f"{name}.screenshotSha256")
    return value


def validate_receipt(
    payload: Any,
    *,
    source_override: Path | None = None,
    expected_screenshot_sha256: str | None = None,
    expected_title: str | None = None,
) -> dict[str, Any]:
    """Validate *payload* and return a small, deterministic summary.

    ``source_override`` is useful when a receipt is copied between checkouts:
    the receipt's ``source`` remains provenance, while the override identifies
    the source that the current import is about to consume.  The override is
    the path checked and must contain the recorded source digest.
    """
    root = _require_mapping(payload, "receipt")
    _check_keys(root, _REQUIRED_TOP_LEVEL, "receipt")
    source = _check_source(
        root["source"], root["sourceSha256"], source_override=source_override
    )

    if root["deterministicDomMarker"] is not True:
        raise ReceiptError("deterministicDomMarker must be true")
    if root["negativeControl"] != "rejected":
        raise ReceiptError("negativeControl must equal 'rejected'")

    positive = _check_run(root["positive"], "positive")
    repeat = _check_run(root["repeat"], "repeat")
    if positive["marker"] != repeat["marker"]:
        raise ReceiptError("positive and repeat DOM markers differ")
    if positive["dom"] != repeat["dom"]:
        raise ReceiptError("positive and repeat DOM summaries differ")
    if positive["screenshotSha256"] != repeat["screenshotSha256"]:
        raise ReceiptError("positive and repeat screenshot SHA-256 values differ")

    if expected_screenshot_sha256 is not None:
        expected = _check_sha(expected_screenshot_sha256, "expected_screenshot_sha256")
        if positive["screenshotSha256"] != expected:
            raise ReceiptError(
                "screenshot SHA-256 does not match expected value "
                f"(got {positive['screenshotSha256']})"
            )
    if expected_title is not None and positive["dom"]["title"] != expected_title:
        raise ReceiptError(
            f"positive.dom.title does not match expected value {expected_title!r}"
        )

    return {
        "source": str(source),
        "title": positive["dom"]["title"],
        "marker_count": positive["marker"]["count"],
        "root_children": positive["dom"]["rootChildren"],
        "screenshot_sha256": positive["screenshotSha256"],
        "negative_control": root["negativeControl"],
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("receipt", type=Path, help="CDP browser fidelity receipt JSON")
    parser.add_argument(
        "--source",
        type=Path,
        help="source HTML to validate for a copied receipt (otherwise receipt.source)",
    )
    parser.add_argument("--expected-screenshot-sha256", help="pin the expected screenshot digest")
    parser.add_argument("--expected-title", help="pin the expected document title")
    parser.add_argument("--json", action="store_true", help="emit the validated summary as JSON")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        with args.receipt.open(encoding="utf-8") as fh:
            payload = json.load(fh)
        summary = validate_receipt(
            payload,
            source_override=args.source,
            expected_screenshot_sha256=args.expected_screenshot_sha256,
            expected_title=args.expected_title,
        )
    except (OSError, json.JSONDecodeError, ReceiptError) as exc:
        print(f"FAIL: browser fidelity receipt: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(summary, sort_keys=True))
    else:
        print(
            "OK: browser fidelity receipt "
            f"marker={summary['marker_count']} root_children={summary['root_children']} "
            f"screenshot_sha256={summary['screenshot_sha256']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
