"""RN ViewStyle surface adapter — week 1 deliverable (third of 5).

Classifies each `rn/*` entry in compat.json against three layers of evidence:

1. The oracle (`tools/harness/oracles/rn/rn-viewstyle.json`) — what RN
   itself supports for this property, including platform flags (iOS / Android),
   third-party extension markers (react-native-svg), and Pulp extensions.
2. The catalog payload (`mapsTo`, `supportedValues`, `unsupportedValues`,
   `notes`) — what the catalog claims pulp does today.
3. The bridge surface — `packages/pulp-react/src/prop-applier*.ts` switch
   cases (the JSX-prop-to-bridge-setter router) plus the actual bridge
   function registration list embedded in the oracle. We grep the split
   prop-applier modules once at adapter init for cheap presence checks.

The verdict is PASS / DIVERGE / NO-OP / NOT-IMPL / OOS — see
`tools/harness/status.py` for the full taxonomy.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from ..status import Status
from .base import AdapterBase, CatalogEntry, Result, register_adapter


# Catalog `mapsTo` markers that imply "no binding exists in @pulp/react TS".
NOT_IMPL_MARKERS = (
    "no branch",
    "no prop-applier case",
    "does not expose",
    "no @pulp/react prop",
    "no shorthand prop in @pulp/react",
    "not surfaced in @pulp/react",
    "not surfaced as a flat prop",
    "not surfaced.",
    "not yet exposed at @pulp/react",
    "not yet exposed",
    "intentionally not dispatched",
    "no bridge support",
    "no bridge (",
    "no bridge --",
    "no field",
    "no implementation",
    "no impl",
    "silently dropped",
    "silently drops",
    "no prop ",
    "not wired",
    "no binding",
    "no setbridge",
)

NO_OP_MARKERS = (
    "no-op",
    "noop",
    "accepted silently",
    "accepted but ignored",
    "ignored",
    "stub",
)


@register_adapter("rn")
class RnAdapter(AdapterBase):
    surface = "rn"

    def __init__(self, repo_root: Path):
        super().__init__(repo_root)
        self._oracle = self._load_oracle()
        # P5-NEW-A split the former monolithic prop-applier switch into a
        # thin dispatcher plus per-domain modules. Concatenate all of them
        # before extracting `case 'X':` arms so the RN harness sees the live
        # JSX bridge surface instead of only the handful of type-dispatched
        # cases that remain in prop-applier.ts.
        self._prop_applier_text = "\n".join(
            self._read(rel)
            for rel in (
                "packages/pulp-react/src/prop-applier.ts",
                "packages/pulp-react/src/prop-applier-layout.ts",
                "packages/pulp-react/src/prop-applier-paint.ts",
                "packages/pulp-react/src/prop-applier-typography.ts",
                "packages/pulp-react/src/prop-applier-transform.ts",
                "packages/pulp-react/src/prop-applier-events.ts",
            )
        )
        self._prop_applier_cases = self._extract_prop_applier_cases(
            self._prop_applier_text
        )
        self._registered_bridge_fns = set(
            self._oracle.get("bridgeFunctions", {}).get("registered", [])
        )

    # ── Oracle + source loading ──────────────────────────────────────

    def _load_oracle(self) -> dict:
        path = (
            self.repo_root
            / "tools"
            / "harness"
            / "oracles"
            / "rn"
            / "rn-viewstyle.json"
        )
        with open(path) as f:
            return json.load(f)

    def _read(self, rel: str) -> str:
        p = self.repo_root / rel
        if not p.exists():
            return ""
        return p.read_text(encoding="utf-8", errors="replace")

    @staticmethod
    def _extract_prop_applier_cases(src: str) -> set[str]:
        """Set of `case 'X':` prop names that prop-applier handles."""
        if not src:
            return set()
        return set(re.findall(r"case\s+'([A-Za-z_][A-Za-z0-9_]*)'\s*:", src))

    # ── Catalog `mapsTo` heuristics ───────────────────────────────────

    @staticmethod
    def _maps_to_marks_unimpl(maps_to: str) -> bool:
        if not maps_to:
            return True
        m = maps_to.lower()
        return any(marker in m for marker in NOT_IMPL_MARKERS)

    @staticmethod
    def _maps_to_marks_noop(maps_to: str) -> bool:
        if not maps_to:
            return False
        m = maps_to.lower()
        return any(marker in m for marker in NO_OP_MARKERS)

    @staticmethod
    def _bridge_fns_in_maps_to(maps_to: str) -> list[str]:
        """Pull every `setX` identifier referenced in mapsTo."""
        if not maps_to:
            return []
        return re.findall(r"\b(set[A-Z][A-Za-z0-9_]*)\b", maps_to)

    # ── Main classification ──────────────────────────────────────────

    def run(self, entry: CatalogEntry) -> Result:
        prop = entry.short_name  # e.g. "alignItems"
        maps_to = entry.maps_to or ""

        # 0. wontfix is OOS by definition.
        if entry.status == "wontfix":
            return Result(
                entry=entry,
                status=Status.OOS,
                detail="catalog status=wontfix (explicitly out of scope)",
            )

        oracle_entry = self._oracle["properties"].get(prop)

        # 1. Property not in the RN oracle at all → OOS (unknown surface).
        if oracle_entry is None:
            return Result(
                entry=entry,
                status=Status.OOS,
                detail=(
                    f"property {prop!r} not present in RN ViewStyle oracle "
                    f"(unknown surface)"
                ),
            )

        # 2. Platform-only props (iOS-only or Android-only) are out of scope
        #    for the cross-platform pulp surface unless we've explicitly
        #    decided to ship them. The catalog typically marks these
        #    `wontfix` already; if it doesn't, we still treat them as OOS
        #    because the cross-platform "missing" is not a bug.
        platform_only = oracle_entry.get("platformOnly")
        if platform_only:
            return Result(
                entry=entry,
                status=Status.OOS,
                detail=(
                    f"RN {platform_only}-only prop; out of scope for the "
                    f"cross-platform pulp surface"
                ),
            )

        kind = oracle_entry.get("kind", "string")
        oracle_values = list(oracle_entry.get("values") or [])

        # 3. Look at the catalog mapsTo string and the prop-applier surface.
        case_present = prop in self._prop_applier_cases
        # Some catalog entries route through a *different* prop-applier case
        # (e.g. rn/backgroundColor → @pulp/react's `background` prop, rn/color
        # → @pulp/react's `textColor` prop, rn/flexDirection → @pulp/react's
        # `direction` prop). The mapsTo field cites the alternate prop-name;
        # pull it out so we don't miscount these as missing.
        # Patterns we handle (captured group is the alternate prop name):
        #   "background prop -> setBackground"            (backgroundColor)
        #   "textColor prop -> setTextColor"              (color)
        #   "prop-applier 'direction' -> setFlex(...)"    (flexDirection alias)
        #   "prop-applier.ts: 'd' -> setSvgPath(...)"     (d, fill, stroke, ...)
        alt_props_in_maps_to: list[str] = []
        alt_props_in_maps_to += re.findall(
            r"\b([a-z][a-zA-Z]*)\s+prop\b", maps_to
        )
        alt_props_in_maps_to += re.findall(
            r"prop-applier(?:\.ts)?[:\s]+'([A-Za-z_][A-Za-z0-9_]*)'",
            maps_to,
        )
        alt_case_present = any(
            p in self._prop_applier_cases for p in alt_props_in_maps_to
        )
        has_binding = case_present or alt_case_present

        # 4. Cross-check any `setX` referenced in mapsTo against the
        #    actually-registered bridge surface. A claim like
        #    "Bridge has setBackfaceVisibility -- not surfaced" is honest
        #    about the @pulp/react gap, but a claim like
        #    "Bridge has setFooBar" with setFooBar absent is a DIVERGE we
        #    should surface to the user.
        bridge_fns_cited = self._bridge_fns_in_maps_to(maps_to)
        unregistered_cited = [
            fn for fn in bridge_fns_cited if fn not in self._registered_bridge_fns
        ]

        # 5. NO_OP markers (rare for rn).
        if self._maps_to_marks_noop(maps_to):
            return Result(
                entry=entry,
                status=Status.NO_OP,
                detail=f"mapsTo signals no-op acceptance: {maps_to!r}",
            )

        # 6. NOT-IMPL markers (most common reason for `missing`).
        if self._maps_to_marks_unimpl(maps_to):
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=f"mapsTo signals no implementation: {maps_to!r}",
            )

        # 7. If the catalog cites a bridge function that doesn't actually
        #    exist, that's a documentation drift — DIVERGE.
        if unregistered_cited:
            return Result(
                entry=entry,
                status=Status.DIVERGE,
                detail=(
                    f"mapsTo cites bridge fn(s) not in widget_bridge.cpp: "
                    f"{unregistered_cited}"
                ),
            )

        # 8. No prop-applier binding and no helpful marker — fall back to
        #    NOT_IMPL.
        if not has_binding:
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=(
                    f"no prop-applier case for {prop!r} (or alias); "
                    f"mapsTo={maps_to!r}"
                ),
            )

        # 9. Compare supportedValues vs the oracle's enum value set.
        if kind in ("enum", "enum-or-number") and oracle_values:
            sup = set(entry.supported_values or [])
            unsup = set(entry.unsupported_values or [])
            oracle_set = set(oracle_values)

            matched = sorted(oracle_set & sup)
            missing_from_supported = sorted(oracle_set - sup)

            # PASS iff every oracle value is in supportedValues AND
            # nothing the oracle defines is listed as unsupported.
            if oracle_set <= sup and not (oracle_set & unsup):
                return Result(
                    entry=entry,
                    status=Status.PASS,
                    detail=(
                        f"all {len(oracle_set)} RN values for {prop!r} are "
                        f"claimed supported"
                    ),
                    matched_supported=matched,
                )

            # If at least one oracle value is in supported, it's DIVERGE.
            if oracle_set & sup:
                return Result(
                    entry=entry,
                    status=Status.DIVERGE,
                    detail=(
                        f"{len(matched)}/{len(oracle_set)} RN values supported; "
                        f"missing: {missing_from_supported}"
                    ),
                    matched_supported=matched,
                    unmatched_supported=missing_from_supported,
                    extra_unsupported=sorted(oracle_set & unsup),
                )

            # Binding exists but no oracle value claimed — treat as DIVERGE
            # (the surface is wired but the value translation isn't).
            return Result(
                entry=entry,
                status=Status.DIVERGE,
                detail=(
                    f"prop-applier case for {prop!r} present but no RN "
                    f"values claimed in supportedValues; missing all of "
                    f"{sorted(oracle_set)}"
                ),
                unmatched_supported=sorted(oracle_set),
            )

        # 10. Non-enum kinds (number, length, color, etc).
        # If supportedValues has nothing and unsupportedValues has nothing
        # meaningful, call PASS. If unsupportedValues lists real values,
        # DIVERGE.
        meaningful_unsup = [
            v
            for v in (entry.unsupported_values or [])
            if v.strip() and v.strip().lower() not in ("none", "all")
        ]
        if meaningful_unsup:
            return Result(
                entry=entry,
                status=Status.DIVERGE,
                detail=(
                    f"non-enum {kind!r} property has "
                    f"{len(meaningful_unsup)} unsupported values listed: "
                    f"{meaningful_unsup}"
                ),
                extra_unsupported=meaningful_unsup,
            )

        # If supportedValues has [] but unsupported is ['all'], that's
        # really a NOT_IMPL claim dressed in non-enum garb — treat it as
        # such for honesty.
        unsup_lower = [v.strip().lower() for v in (entry.unsupported_values or [])]
        if unsup_lower == ["all"]:
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=(
                    f"unsupportedValues=['all'] indicates the prop-applier "
                    f"case is present but rejects every value"
                ),
            )

        return Result(
            entry=entry,
            status=Status.PASS,
            detail=f"non-enum {kind!r} property bound; no unsupported values listed",
        )
