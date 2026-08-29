#!/usr/bin/env python3
"""Export source-neutral aggregate learnings from the private patch corpus.

    corpus_export.py <new-destination-dir>

The local corpus is private research material. This command is the public
boundary, not a corpus backup: it writes one fixed-schema JSON report containing
only aggregate counts and independently worded conclusions. It never writes
patch bodies, per-patch rows, provenance, identifiers, source prose, or
replayable module/cable topology. Licence metadata does not relax that rule.

Private backup and migration belong in private-repository tooling. The analysis
tools in this directory continue to read the local corpus directly.
"""

from __future__ import annotations

import json
import ctypes
import errno
import os
import re
import shutil
import sys
import tempfile
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import corpus_sweep as cs                                   # noqa: E402
import idiom_check as ic                                    # noqa: E402


SCHEMA = "pulp.public_corpus_learnings.v1"
COVERAGE_BUCKETS = (
    "all_modules_known",
    "most_modules_known",
    "some_modules_known",
    "mostly_unknown",
    "empty",
)
GATE_OUTCOMES = ("accepted", "rejected", "unclassified", "unverifiable")
SIGNAL_KINDS = ("audio", "clock", "gate", "pitch")
SUPPORT_BUCKETS = ("3", "4_to_7", "8_or_more")
FUNCTIONAL_ROLES = (
    "control_source",
    "level_control",
    "signal_output",
    "signal_source",
    "spectral_shaping",
    "timing_source",
)
APPROVED_CONCLUSIONS = (
    "Classify behavior only when independently defined functional-role "
    "criteria are satisfied.",
    "Treat incomplete functional-role coverage as unverified rather than as "
    "negative evidence.",
    "Require corroboration from at least three independent contributors "
    "before admitting a routing prior.",
)
POLICY = (
    "Source-neutral aggregates only; private source material cannot be "
    "reconstructed from this report."
)

_SOURCE_SHAPED_KEYS = {
    "author", "author_id", "body", "bodies", "cable", "cables", "excerpt",
    "file", "filename", "hash", "id", "license", "license_slug", "module",
    "modules", "patch", "provenance", "quote", "sha256", "source", "text",
    "title", "uploader", "url", "uri",
}
_URL = re.compile(r"(?:https?://|www\.)", re.IGNORECASE)
_HEX_DIGEST = re.compile(r"^[0-9a-f]{32,}$", re.IGNORECASE)


class PublicBoundaryError(ValueError):
    """The proposed report is outside the public aggregate schema."""


def _is_count(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _is_positive_count(value: object) -> bool:
    return _is_count(value) and value > 0


def _exact_keys(value: object, expected: set[str], where: str) -> dict:
    if not isinstance(value, dict) or set(value) != expected:
        got = sorted(value) if isinstance(value, dict) else type(value).__name__
        raise PublicBoundaryError(
            f"{where} must contain exactly {sorted(expected)}; got {got}")
    return value


def _reject_source_shape(value: object, where: str = "report") -> None:
    """Defense in depth for future schema edits.

    The exact-schema checks below are authoritative. This recursive scan makes
    a later, overly permissive field fail closed when it resembles provenance,
    a replayable body, a source locator, or a content fingerprint.
    """
    if isinstance(value, dict):
        for key, item in value.items():
            if str(key).lower() in _SOURCE_SHAPED_KEYS:
                raise PublicBoundaryError(f"{where}.{key} is source-shaped")
            _reject_source_shape(item, f"{where}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            _reject_source_shape(item, f"{where}[{index}]")
    elif isinstance(value, str):
        if (_URL.search(value) or _HEX_DIGEST.fullmatch(value)
                or ".vcv" in value.lower()):
            raise PublicBoundaryError(
                f"{where} contains a source locator or fingerprint")


def _validate_count_rows(rows: object, field: str, allowed: tuple[str, ...],
                         where: str) -> None:
    if not isinstance(rows, list) or len(rows) != len(allowed):
        raise PublicBoundaryError(f"{where} must contain every fixed bucket")
    seen = set()
    for index, row in enumerate(rows):
        row = _exact_keys(row, {field, "count"}, f"{where}[{index}]")
        if (row[field] not in allowed or row[field] in seen
                or not _is_count(row["count"])):
            raise PublicBoundaryError(
                f"{where}[{index}] is not a unique valid count")
        seen.add(row[field])
    if tuple(row[field] for row in rows) != allowed:
        raise PublicBoundaryError(f"{where} must use canonical bucket order")


def validate_public_report(report: dict) -> None:
    """Accept only the non-reconstructive public schema."""
    top = _exact_keys(report, {
        "schema", "generated_at", "policy", "functional_roles",
        "aggregate_counts", "corroborated_priors", "conclusions",
    }, "report")
    if top["schema"] != SCHEMA or top["policy"] != POLICY:
        raise PublicBoundaryError(
            "report schema or policy is not the public contract")
    if not isinstance(top["generated_at"], str) or not re.fullmatch(
            r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", top["generated_at"]):
        raise PublicBoundaryError(
            "generated_at must be a UTC second timestamp")
    if top["functional_roles"] != list(FUNCTIONAL_ROLES):
        raise PublicBoundaryError(
            "functional_roles must use the fixed abstract vocabulary")

    counts = _exact_keys(top["aggregate_counts"], {
        "patches_analyzed", "coverage_buckets", "gate_outcomes",
    }, "aggregate_counts")
    if not _is_count(counts["patches_analyzed"]):
        raise PublicBoundaryError(
            "patches_analyzed must be a non-negative count")
    _validate_count_rows(counts["coverage_buckets"], "coverage",
                         COVERAGE_BUCKETS, "coverage_buckets")
    _validate_count_rows(counts["gate_outcomes"], "outcome",
                         GATE_OUTCOMES, "gate_outcomes")
    for key in ("coverage_buckets", "gate_outcomes"):
        if sum(row["count"] for row in counts[key]) != counts["patches_analyzed"]:
            raise PublicBoundaryError(
                f"{key} counts must total patches_analyzed")

    if not isinstance(top["corroborated_priors"], list):
        raise PublicBoundaryError("corroborated_priors must be a list")
    seen_priors = set()
    for index, row in enumerate(top["corroborated_priors"]):
        row = _exact_keys(row, {"signal", "support_bucket", "prior_count"},
                          f"corroborated_priors[{index}]")
        key = (row["signal"], row["support_bucket"])
        if (row["signal"] not in SIGNAL_KINDS
                or row["support_bucket"] not in SUPPORT_BUCKETS):
            raise PublicBoundaryError(
                f"corroborated_priors[{index}] uses a non-generic enum")
        if key in seen_priors or not _is_positive_count(row["prior_count"]):
            raise PublicBoundaryError(
                f"corroborated_priors[{index}] is duplicate or invalid")
        seen_priors.add(key)

    if top["conclusions"] != list(APPROVED_CONCLUSIONS):
        raise PublicBoundaryError(
            "conclusions must be independently authored approved text")
    _reject_source_shape(top)


def _coverage_bucket(known: int, total: int) -> str:
    if not total:
        return "empty"
    fraction = known / total
    if fraction == 1.0:
        return "all_modules_known"
    if fraction >= 0.8:
        return "most_modules_known"
    if fraction >= 0.5:
        return "some_modules_known"
    return "mostly_unknown"


def _gate_outcome(patch: dict, meta: dict, inventory: dict, roles: dict,
                  idioms: dict) -> str:
    if not idioms:
        return "unclassified"
    prompt = " ".join([str(meta.get("title") or "")] +
                      [str(tag) for tag in (meta.get("tags") or [])])
    slug = ic.resolve_exact(prompt, idioms)
    if not slug or slug not in idioms:
        return "unclassified"
    unchecked: list = []
    rejected = ic.check(patch, inventory, idioms[slug], roles, unchecked)
    if unchecked:
        return "unverifiable"
    return "rejected" if rejected else "accepted"


def _support_bucket(support: int) -> str:
    return ("3" if support == 3 else
            "4_to_7" if support <= 7 else "8_or_more")


def summarize_priors(private_report: dict | None) -> list[dict]:
    """Reduce a private per-port report to anonymous generic count buckets."""
    counts = Counter()
    for row in (private_report or {}).get("admitted", []):
        signal = row.get("signal")
        support = row.get("support")
        if signal not in SIGNAL_KINDS or not _is_count(support) or support < 3:
            continue
        counts[(signal, _support_bucket(support))] += 1
    return [
        {"signal": signal, "support_bucket": support, "prior_count": count}
        for (signal, support), count in sorted(counts.items())
    ]


def build_public_report(corpus: list[tuple[dict, dict]], inventory: dict,
                        roles: dict, idioms: dict,
                        private_prior_report: dict | None = None,
                        generated_at: str | None = None) -> dict:
    coverage = Counter()
    outcomes = Counter()
    for meta, patch in corpus:
        known, total = cs.coverage_of(patch, inventory)
        coverage[_coverage_bucket(known, total)] += 1
        outcomes[_gate_outcome(patch, meta, inventory, roles, idioms)] += 1
    report = {
        "schema": SCHEMA,
        "generated_at": generated_at or time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "policy": POLICY,
        "functional_roles": list(FUNCTIONAL_ROLES),
        "aggregate_counts": {
            "patches_analyzed": len(corpus),
            "coverage_buckets": [
                {"coverage": name, "count": coverage[name]}
                for name in COVERAGE_BUCKETS
            ],
            "gate_outcomes": [
                {"outcome": name, "count": outcomes[name]}
                for name in GATE_OUTCOMES
            ],
        },
        "corroborated_priors": summarize_priors(private_prior_report),
        "conclusions": list(APPROVED_CONCLUSIONS),
    }
    validate_public_report(report)
    return report


def private_usage_prior_report(roles: dict) -> dict:
    """Build the private evidence input that is reduced at this boundary."""
    import corpus_audit as audit                            # noqa: PLC0415

    rows = list(audit.walk(audit.portmap(), roles))
    return audit.usage_prior_report(rows, min_support=3)


def _fsync_directory(path: str) -> None:
    """Persist a directory entry where the platform supports directory fsync."""
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    try:
        fd = os.open(path, flags)
    except OSError:
        return
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _rename_noreplace(source: str, destination: str) -> None:
    """Atomically publish a directory without replacing an existing path."""
    if sys.platform == "darwin":
        libc = ctypes.CDLL(None, use_errno=True)
        renamex_np = libc.renamex_np
        renamex_np.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                               ctypes.c_uint]
        renamex_np.restype = ctypes.c_int
        result = renamex_np(os.fsencode(source), os.fsencode(destination), 0x4)
    elif sys.platform.startswith("linux"):
        libc = ctypes.CDLL(None, use_errno=True)
        renameat2 = libc.renameat2
        renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p,
                              ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        renameat2.restype = ctypes.c_int
        result = renameat2(-100, os.fsencode(source), -100,
                           os.fsencode(destination), 0x1)
    elif os.name == "nt":
        os.rename(source, destination)
        return
    else:
        raise PublicBoundaryError(
            "atomic no-replace publication is unsupported on this platform")
    if result == 0:
        return
    error = ctypes.get_errno()
    if error in (errno.EEXIST, errno.ENOTEMPTY):
        raise FileExistsError(error, os.strerror(error), destination)
    raise OSError(error, os.strerror(error), destination)


def write_public_report(dest: str, report: dict) -> str:
    """Publish a validated report atomically, never over old exports."""
    validate_public_report(report)
    if os.path.exists(dest):
        raise PublicBoundaryError(
            "destination already exists; use a new directory so private or "
            "stale files cannot survive beside the public report")
    parent = os.path.dirname(dest) or os.curdir
    if not os.path.isdir(parent):
        raise PublicBoundaryError(
            "destination parent must already exist")
    prefix = f".{os.path.basename(dest)}.tmp-"
    staging = tempfile.mkdtemp(prefix=prefix, dir=parent)
    try:
        staged_path = os.path.join(staging, "public-corpus-learnings.json")
        with open(staged_path, "x", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        _fsync_directory(staging)
        try:
            _rename_noreplace(staging, dest)
        except FileExistsError as error:
            raise PublicBoundaryError(
                "destination appeared during export; refusing to replace it") \
                from error
        staging = ""
        _fsync_directory(parent)
        return os.path.join(dest, "public-corpus-learnings.json")
    finally:
        if staging:
            shutil.rmtree(staging, ignore_errors=True)


def export(dest: str) -> int:
    import patch as patch_mod                               # noqa: PLC0415

    roles = ic.load_roles()
    report = build_public_report(
        cs.held_patches(), patch_mod.inventory(), roles, ic.load_idioms(),
        private_usage_prior_report(roles))
    path = write_public_report(dest, report)
    print(f"exported source-neutral aggregate learnings to {path}")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().split("\n\n")[0])
        return 2
    try:
        return export(os.path.abspath(argv[1]))
    except (OSError, PublicBoundaryError) as error:
        print(f"refused public corpus export: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
