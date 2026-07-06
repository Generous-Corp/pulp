"""Golden-render regression net — attach a cited, multi-axis before/after verdict to a DSP change.

The daily-driver loop: keep a golden render per plugin/preset, render the candidate (`pulp audio
render --plugin …`), and `compare` it against the golden across every wired axis. This is the
reusable reference implementation; a plugin suite (classic effects, a time-stretcher, a GPU amp
model) wires its own renders into `run_net` and commits a portable manifest.

Two orthogonal signals, deliberately kept apart:

- **Regression** — an axis reports `regression_suspected`. This is the net's *fail* condition
  (`net_failed`), and the ONLY thing that keys off axis verdicts.
- **Error** — a pair could not be measured (`invalid`), or the run executed zero checks. This is a
  broken pipeline, not a judgment about the audio; it is reported separately (`net_errored`) with
  its own exit code so a missing or corrupt render is never greenlit as clean.

The corroboration column is purely informational and never affects either signal: a time-variant
effect (chorus, phaser, flanger, tremolo, ring-mod, a fixed-ratio stretch) legitimately reads
`not_corroborated` forever because its phase-sensitive sample-domain residual disagrees with a tonal
axis — the `uncaptured_material_difference` headline flag carries
`expected_for: ["time_variant_processing"]` so a caller filters it as expected. Gating proper stays
`pulp audio validate compare`; this net is advisory reporting attached to a change, not a gate.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import Any

from . import compare

# Default to the broadly-applicable axes so a comparison doesn't emit a spurious not_applicable row
# for a capability-specific axis (stereo-width needs 2 channels; transient-integrity needs percussive
# material) on the wrong material. A suite adds those to its manifest's `profiles` explicitly. A
# plugin picks whatever subset fits its change class.
DEFAULT_PROFILES: tuple[str, ...] = compare.NET_DEFAULT_PROFILES

VALID_REFERENCE_ROLES = ("peer", "golden")


@dataclass
class NetRow:
    """One golden-vs-candidate result for one axis."""
    name: str
    profile: str
    verdict: str
    corroboration: str | None            # informational — never part of the fail or error decision
    flags: list[str] = field(default_factory=list)
    summary: str = ""

    @property
    def is_regression(self) -> bool:
        return self.verdict == compare.VERDICT_REGRESSION

    @property
    def is_invalid(self) -> bool:
        return self.verdict == compare.VERDICT_INVALID

    def to_dict(self) -> dict[str, Any]:
        return {"name": self.name, "profile": self.profile, "verdict": self.verdict,
                "corroboration": self.corroboration, "flags": self.flags, "summary": self.summary}


def run_net(
    pairs: list[tuple[str, str, str]],
    profiles: tuple[str, ...] = DEFAULT_PROFILES,
    reference_role: str = "golden",
) -> list[NetRow]:
    """Run every (name, golden_wav, candidate_wav) pair through every profile. `reference_role`
    defaults to `golden` (the golden render is the known-good baseline, so a bad-direction change is
    a `regression_suspected`). Returns a flat list of NetRow — one per (pair, profile)."""
    rows: list[NetRow] = []
    for name, golden, candidate in pairs:
        for profile in profiles:
            report = compare.compare_files(golden, candidate, profile=profile,
                                           reference_role=reference_role)
            corr = (report.get("advisory") or {}).get("corroboration")
            rows.append(NetRow(
                name=name, profile=profile, verdict=report["verdict"],
                corroboration=corr["status"] if corr else None,
                flags=[f["flag"] for f in report.get("headline_flags", [])],
                summary=report["summary"],
            ))
    return rows


def net_failed(rows: list[NetRow]) -> bool:
    """The net fails ONLY when an axis reports `regression_suspected`. Corroboration disagreement —
    endemic to time-variant effects — is deliberately NOT a fail condition."""
    return any(r.is_regression for r in rows)


def net_errored(rows: list[NetRow]) -> bool:
    """A pair could not be measured (an `invalid` verdict — a missing/corrupt render or a
    sample-rate mismatch), or the run executed zero checks. Reported separately from `net_failed`
    so a broken render is never silently greenlit as clean; the axis-verdict-only fail contract of
    `net_failed` stays intact."""
    return (not rows) or any(r.is_invalid for r in rows)


def format_table(rows: list[NetRow]) -> str:
    """A compact fixed-width table: name | profile | verdict | corroboration | flags. The
    corroboration column is present precisely so a reader SEES the time-variant disagreement and
    knows (from the flags and this net's policy) that it is informational, not a failure."""
    if not rows:
        return "(no pairs)"
    headers = ("plugin/preset", "profile", "verdict", "corroboration", "flags")
    table = [(r.name, r.profile, r.verdict, r.corroboration or "-", ",".join(r.flags) or "-") for r in rows]
    widths = [max(len(headers[i]), *(len(row[i]) for row in table)) for i in range(len(headers))]
    def fmt(cols: tuple[str, ...]) -> str:
        return "  ".join(c.ljust(widths[i]) for i, c in enumerate(cols))
    lines = [fmt(headers), fmt(tuple("-" * w for w in widths))]
    lines += [fmt(row) for row in table]
    return "\n".join(lines)


def status(rows: list[NetRow]) -> str:
    """A single word for the run: REGRESSION (an axis regressed) > ERROR (a pair couldn't be
    measured, or zero checks ran) > INCONCLUSIVE (some axis couldn't decide) > CLEAN (every axis
    measured, none regressed). Regression and error are distinct because one is a judgment about the
    audio and the other is a broken pipeline."""
    if net_failed(rows):
        return "REGRESSION"
    if net_errored(rows):
        return "ERROR"
    if any(r.verdict == compare.VERDICT_INCONCLUSIVE for r in rows):
        return "INCONCLUSIVE"
    return "CLEAN"


def _pairs_from_manifest(manifest: dict[str, Any], base_dir: str = "") -> list[tuple[str, str, str]]:
    out: list[tuple[str, str, str]] = []
    pairs = manifest.get("pairs", [])
    if not isinstance(pairs, list):
        raise ValueError("manifest 'pairs' must be a list")
    for i, p in enumerate(pairs):
        try:
            name, golden, candidate = p["name"], p["golden"], p["candidate"]
        except (KeyError, TypeError) as exc:
            raise ValueError(f"manifest pair #{i} is missing a required key "
                             f"(name/golden/candidate): {exc}")
        golden = os.path.join(base_dir, golden) if base_dir else golden
        candidate = os.path.join(base_dir, candidate) if base_dir else candidate
        out.append((str(name), golden, candidate))
    return out


def run_manifest(manifest_path: str) -> tuple[list[NetRow], bool]:
    """Load a JSON manifest — {"reference_role"?, "profiles"?, "pairs":[{name,golden,candidate}]} —
    resolve golden/candidate paths relative to the manifest's directory, run the net, and return
    (rows, failed). Raises ValueError on a malformed manifest (no pairs, empty profiles, an unknown
    reference_role, a pair missing a key) so a caller can report a clean error instead of a
    traceback."""
    with open(manifest_path) as fh:
        manifest = json.load(fh)
    base_dir = os.path.dirname(os.path.abspath(manifest_path))
    pairs = _pairs_from_manifest(manifest, base_dir)
    if not pairs:
        raise ValueError("manifest has no pairs to check")
    profiles = tuple(manifest.get("profiles", DEFAULT_PROFILES))
    if not profiles:
        raise ValueError("manifest has an empty 'profiles' list")
    reference_role = manifest.get("reference_role", "golden")
    if reference_role not in VALID_REFERENCE_ROLES:
        raise ValueError(f"manifest reference_role must be one of {VALID_REFERENCE_ROLES}, "
                         f"got {reference_role!r}")
    rows = run_net(pairs, profiles=profiles, reference_role=reference_role)
    return rows, net_failed(rows)
