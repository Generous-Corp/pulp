"""Canvas2D surface adapter — week 1 deliverable.

Classifies each `canvas2d/*` entry in compat.json against four layers of
evidence:

1. The oracle (`tools/harness/oracles/canvas2d/canvas2d-supported.json`) —
   what the HTML5 Canvas2D spec defines for this method/attribute, which
   bridge function(s) it should route through, and any documented gotcha
   that caps the achievable status.
2. The catalog payload (`mapsTo`, `supportedValues`, `unsupportedValues`,
   `notes`) — what the catalog claims pulp does today.
3. The C++ bridge surface (`core/view/src/widget_bridge.cpp`'s
   `register_function("canvasX", ...)` calls) — the truth-of-record for
   what actually reaches the native side.
4. The JS shim (`core/view/js/web-compat-canvas.js`) — the
   `CanvasRenderingContext2D.prototype.X` and `CanvasGradient.prototype.X`
   declarations + tracked attribute fields.

The verdict is PASS / DIVERGE / NO-OP / NOT-IMPL / OOS — see
`tools/harness/status.py` for the full taxonomy.

This adapter mirrors the yoga adapter's structure intentionally. The two
share idioms (NOT_IMPL_MARKERS, NO_OP_MARKERS, mapsTo heuristic
classifiers) so the contract stays uniform across surfaces.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from ..status import Status
from .base import AdapterBase, CatalogEntry, Result, register_adapter


# Markers in the catalog `mapsTo` field that imply "no real binding".
NOT_IMPL_MARKERS = (
    "no implementation",
    "no implementation today",
    "no impl",
    "not implemented in shim or bridge",
    "not implemented",
    "silently dropped",
    "silently drops",
    "not pushed to bridge",
    "not pushed",
    "not yet plumbed",
    "no bridge",
    "no binding",
    "no canvas",
    "shim returns null",
    "shim returns an empty linear gradient",
    "tracked locally; not pushed",
    "tracked as a js field; not pushed",
)

# Markers that imply "accepted but does nothing" — distinct from NOT-IMPL
# because a JS-level shim entry exists, but no bridge call is made.
NO_OP_MARKERS = (
    "no-op",
    "noop",
    "accepted silently",
    "accepted but ignored",
    "ignored silently",
    "stub",
    "graceful fallback",
)

# Markers that imply "partial" — the entry routes somewhere but with a
# documented gap (degraded values, missing parameter handling, etc.).
PARTIAL_MARKERS = (
    "degrades",
    "degenerates",
    "ignored;",
    "ignored.",
    "fall back",
    "falls back",
    "fallback",
    "uniform-corner case",
    "no-op",
    "rotation ignored",
    "first-stop",
    "single-circle",
    "only exposes replace",
)


@register_adapter("canvas2d")
class Canvas2dAdapter(AdapterBase):
    surface = "canvas2d"

    def __init__(self, repo_root: Path):
        super().__init__(repo_root)
        self._oracle = self._load_oracle()
        self._bridge_text = self._read("core/view/src/widget_bridge.cpp")
        self._shim_text = self._read("core/view/js/web-compat-canvas.js")
        self._bridge_fns = self._extract_bridge_fns()
        self._shim_methods = self._extract_shim_methods()
        self._shim_attrs = self._extract_shim_attrs()

    # ── Oracle + source loading ──────────────────────────────────────

    def _load_oracle(self) -> dict:
        path = (
            self.repo_root
            / "tools"
            / "harness"
            / "oracles"
            / "canvas2d"
            / "canvas2d-supported.json"
        )
        with open(path) as f:
            return json.load(f)

    def _read(self, rel: str) -> str:
        p = self.repo_root / rel
        if not p.exists():
            return ""
        return p.read_text(encoding="utf-8", errors="replace")

    # ── Bridge / shim surface introspection ──────────────────────────

    def _extract_bridge_fns(self) -> set[str]:
        """Set of `canvas*` function names actually `register_function`'d."""
        names = set(re.findall(r'register_function\("(canvas[A-Za-z_]+)"', self._bridge_text))
        return names

    def _extract_shim_methods(self) -> set[str]:
        """CanvasRenderingContext2D.prototype.X + CanvasGradient.prototype.X identifiers."""
        names = set()
        for m in re.finditer(
            r"(?:CanvasRenderingContext2D|CanvasGradient)\.prototype\.([A-Za-z_][A-Za-z0-9_]*)\s*=",
            self._shim_text,
        ):
            names.add(m.group(1))
        return names

    def _extract_shim_attrs(self) -> set[str]:
        """`this.X = ...` initializers inside the shim — covers ctx attributes."""
        names = set()
        for m in re.finditer(
            r"\bthis\.([A-Za-z_][A-Za-z0-9_]*)\s*=", self._shim_text
        ):
            names.add(m.group(1))
        return names

    # ── Catalog `mapsTo` heuristics ──────────────────────────────────

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
    def _notes_marks_unimpl(notes: str) -> bool:
        if not notes:
            return False
        n = notes.lower()
        return any(marker in n for marker in NOT_IMPL_MARKERS)

    # ── Bridge-call extraction from mapsTo ───────────────────────────

    @staticmethod
    def _bridge_calls_in_maps_to(maps_to: str) -> list[str]:
        """Pull every `canvasFoo` identifier the catalog cites."""
        if not maps_to:
            return []
        return list(set(re.findall(r"\b(canvas[A-Z][A-Za-z_]*)\b", maps_to)))

    # ── Main classification ──────────────────────────────────────────

    def run(self, entry: CatalogEntry) -> Result:
        prop = entry.short_name  # e.g. "fillRect"

        # 0. wontfix is OOS by definition.
        if entry.status == "wontfix":
            return Result(
                entry=entry,
                status=Status.OOS,
                detail="catalog status=wontfix (explicitly out of scope)",
            )

        # 1. If the oracle doesn't define this entry, it's OOS — the
        #    canvas2d catalog includes Pulp-specific extensions like
        #    _native_canvasFillCircle and getContext_webgpu, which we
        #    EXPECT in the oracle. Anything missing from the oracle
        #    is genuinely out of scope for week-1 measurement.
        oracle_entry = self._oracle["entries"].get(prop)
        if oracle_entry is None:
            return Result(
                entry=entry,
                status=Status.OOS,
                detail=f"entry {prop!r} not present in canvas2d oracle (out-of-scope)",
            )

        kind = oracle_entry.get("kind", "method")
        oracle_bridge = list(oracle_entry.get("bridge") or [])
        expected_status = oracle_entry.get("expectedStatus")
        gotcha = oracle_entry.get("gotcha")
        oracle_values = list(oracle_entry.get("values") or [])

        maps_to = entry.maps_to or ""
        notes = entry.notes or ""

        # 2. Oracle-stipulated unimplemented (createConicGradient, createPattern,
        #    shadow*, filter, miterLimit, imageSmoothing*, direction).
        #    These have an empty bridge list AND are documented as missing.
        if expected_status == "missing":
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=f"oracle marks {prop!r} as NOT-IMPL (gotcha={gotcha!r}); {notes or 'no bridge route'}",
            )

        # 2b. Oracle-stipulated partial — short-circuit BEFORE the mapsTo
        #     no-op heuristic. The oracle's expectedStatus is the source of
        #     truth; the catalog's mapsTo language often mentions "no-op"
        #     as a sub-case of a known-partial implementation (e.g.
        #     transform: rotate/scale falls back to no-op).
        if expected_status == "partial":
            unsup = list(entry.unsupported_values or [])
            return Result(
                entry=entry,
                status=Status.DIVERGE,
                detail=(
                    f"oracle pins {prop!r} at partial (gotcha={gotcha!r}); "
                    f"{notes or 'see SKILL canvas2d gotchas'}"
                ),
                extra_unsupported=unsup,
            )

        # 3. Catalog `mapsTo` says no implementation. This catches catalog
        #    entries whose mapsTo states "Not implemented" or "shim returns null"
        #    even if the oracle didn't pre-mark them missing.
        if self._maps_to_marks_unimpl(maps_to):
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=f"mapsTo signals no implementation: {maps_to[:140]!r}",
            )

        # 3b. Catalog `mapsTo` says no-op — but only if the bridge route is
        #     genuinely absent. If the oracle's bridge fns are all registered
        #     AND the shim has the method, "no-op" in mapsTo is most likely
        #     describing a fallback path, not the primary behavior. In that
        #     case, fall through to the regular classification.
        bridge_present = (not oracle_bridge) or all(
            b in self._bridge_fns for b in oracle_bridge
        )
        if self._maps_to_marks_noop(maps_to) and not bridge_present:
            return Result(
                entry=entry,
                status=Status.NO_OP,
                detail=f"mapsTo signals no-op acceptance + bridge missing: {maps_to[:140]!r}",
            )

        # 4. Bridge presence check. If the oracle says "this entry should route
        #    through canvasX" we verify canvasX is actually `register_function`'d
        #    in widget_bridge.cpp.
        missing_bridge = [b for b in oracle_bridge if b not in self._bridge_fns]
        if oracle_bridge and missing_bridge:
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=(
                    f"oracle expects bridge fns {oracle_bridge!r} for {prop!r}; "
                    f"missing from widget_bridge.cpp: {missing_bridge}"
                ),
                unmatched_supported=missing_bridge,
            )

        # 5. Shim presence check (methods only — attributes use a different
        #    track because they're tracked-locally).
        if kind == "method" and prop not in self._shim_methods:
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=(
                    f"shim does not declare CanvasRenderingContext2D.prototype.{prop} "
                    f"(or CanvasGradient.prototype.{prop})"
                ),
            )
        if kind == "attribute" and prop not in self._shim_attrs:
            # Some attributes might be exposed via getters/setters not as
            # `this.X =`. Don't fail outright — fall through.
            pass

        # 6. Cross-check: the catalog's mapsTo cites bridge fns we know about.
        #    If mapsTo names a canvasFoo that's NOT `register_function`'d, that's
        #    a strong DIVERGE signal — the catalog is claiming a route that
        #    doesn't exist.
        cited = self._bridge_calls_in_maps_to(maps_to)
        cited_missing = [c for c in cited if c not in self._bridge_fns]
        if cited_missing:
            return Result(
                entry=entry,
                status=Status.DIVERGE,
                detail=(
                    f"catalog mapsTo cites bridge fns not registered: {cited_missing!r}"
                ),
                extra_unsupported=cited_missing,
            )

        # 7. (oracle expectedStatus=partial is handled in step 2b above.)

        # 8. Enum-attribute kind check (lineCap, lineJoin, textAlign,
        #    textBaseline). If the oracle defines the value set, compare it
        #    against supportedValues / unsupportedValues.
        if kind == "attribute" and oracle_values:
            sup = set(entry.supported_values or [])
            unsup = set(entry.unsupported_values or [])
            oracle_set = set(oracle_values)

            # No supportedValues claimed but bridge fn registered + oracle
            # values defined — still PASS (the catalog often leaves
            # supportedValues empty for these). Treat empty supportedValues
            # as "matches the oracle".
            if not sup:
                if not (oracle_set & unsup):
                    return Result(
                        entry=entry,
                        status=Status.PASS,
                        detail=(
                            f"attribute {prop!r} bound via {oracle_bridge}; "
                            f"oracle values {sorted(oracle_set)} (no per-value claim in catalog)"
                        ),
                    )

            matched = sorted(oracle_set & sup) if sup else sorted(oracle_set - unsup)
            missing_from_supported = sorted(oracle_set - sup) if sup else []

            if oracle_set <= sup and not (oracle_set & unsup):
                return Result(
                    entry=entry,
                    status=Status.PASS,
                    detail=f"all {len(oracle_set)} spec values for {prop!r} claimed supported",
                    matched_supported=matched,
                )

            if oracle_set & unsup:
                return Result(
                    entry=entry,
                    status=Status.DIVERGE,
                    detail=(
                        f"{prop!r} declares unsupportedValues that overlap spec set: "
                        f"{sorted(oracle_set & unsup)}"
                    ),
                    matched_supported=matched,
                    extra_unsupported=sorted(oracle_set & unsup),
                )

            if sup and oracle_set & sup:
                # supported values listed, but none in the oracle's set
                return Result(
                    entry=entry,
                    status=Status.DIVERGE,
                    detail=(
                        f"{prop!r} supportedValues {sorted(sup)} doesn't fully cover "
                        f"oracle spec set {sorted(oracle_set)}; missing {missing_from_supported}"
                    ),
                    matched_supported=matched,
                    unmatched_supported=missing_from_supported,
                )

        # 9. Generic case: if the catalog lists meaningful unsupportedValues,
        #    that's DIVERGE; otherwise PASS.
        if entry.unsupported_values and any(
            v.strip() and v.strip().lower() != "none" for v in entry.unsupported_values
        ):
            return Result(
                entry=entry,
                status=Status.DIVERGE,
                detail=(
                    f"{kind} {prop!r} has {len(entry.unsupported_values)} unsupported values listed: "
                    f"{entry.unsupported_values[:3]}{'...' if len(entry.unsupported_values) > 3 else ''}"
                ),
                extra_unsupported=list(entry.unsupported_values),
            )

        return Result(
            entry=entry,
            status=Status.PASS,
            detail=(
                f"{kind} {prop!r} bound via {oracle_bridge or '(none)'}; "
                f"no unsupported values listed"
            ),
        )
