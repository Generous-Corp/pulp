"""Yoga surface adapter — week 1 deliverable.

Classifies each `yoga/*` entry in compat.json against three layers of evidence:

1. The oracle (`tools/harness/oracles/yoga/yoga-supported.json`) — what Yoga
   itself supports for this property + value set.
2. The catalog payload (`mapsTo`, `supportedValues`, `unsupportedValues`,
   `notes`) — what the catalog claims pulp does today.
3. The C++ binding surface (`core/view/include/pulp/view/geometry.hpp` —
   `FlexStyle` struct, plus `core/view/src/yoga_layout.cpp` — the
   `YGNodeStyleSet*` calls that wire FlexStyle through to Yoga). We grep
   these once at adapter init for cheap presence checks.

The verdict is PASS / DIVERGE / NO-OP / NOT-IMPL / OOS — see
`tools/harness/status.py` for the full taxonomy.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from ..status import Status
from .base import AdapterBase, CatalogEntry, Result, register_adapter


# Markers in the catalog `mapsTo` field that imply "no real binding".
# These are the language the existing audit already uses; we lean on them
# rather than re-deriving from C++.
NOT_IMPL_MARKERS = (
    "no implementation",
    "no implementation today",
    "no impl",
    "silently dropped",
    "silently drops",
    "no 'align_content' branch",
    "no `align_content` branch",
    "no branch",
    "no field",
    "missing branch",
    "not wired",
    "no binding",
    "no bridge",
    "rtl not implemented",
    "rtl unsupported",
)

# Markers that imply "accepted but does nothing" — distinct from NOT-IMPL
# because the bridge entry exists. NO-OP is the right verdict.
NO_OP_MARKERS = (
    "no-op",
    "noop",
    "accepted silently",
    "accepted but ignored",
    "ignored",
    "stub",
)


@register_adapter("yoga")
class YogaAdapter(AdapterBase):
    surface = "yoga"

    def __init__(self, repo_root: Path):
        super().__init__(repo_root)
        self._oracle = self._load_oracle()
        self._flex_style_text = self._read(
            "core/view/include/pulp/view/geometry.hpp"
        )
        self._yoga_layout_text = self._read("core/view/src/yoga_layout.cpp")
        # Some yoga properties (top/right/bottom/left/position) are bound through
        # View accessors rather than FlexStyle fields. Read view.hpp once so the
        # adapter can corroborate.
        self._view_hpp_text = self._read("core/view/include/pulp/view/view.hpp")

    # ── Oracle + source loading ──────────────────────────────────────

    def _load_oracle(self) -> dict:
        path = self.repo_root / "tools" / "harness" / "oracles" / "yoga" / "yoga-supported.json"
        with open(path) as f:
            return json.load(f)

    def _read(self, rel: str) -> str:
        p = self.repo_root / rel
        if not p.exists():
            return ""
        return p.read_text(encoding="utf-8", errors="replace")

    # ── Field-presence introspection on FlexStyle / yoga_layout.cpp ──

    def _flex_style_has_field(self, candidates: list[str]) -> bool:
        """True if any candidate identifier appears as a field/identifier in geometry.hpp."""
        if not self._flex_style_text:
            return False
        for c in candidates:
            # match as a whole word — `flex_grow`, `align_items`, etc.
            if re.search(r"\b" + re.escape(c) + r"\b", self._flex_style_text):
                return True
        return False

    def _view_has_accessor(self, candidates: list[str]) -> bool:
        """True if View::set_<name> / View::<name>() exists in view.hpp.

        Handles the catalog cases where a yoga property is bound through a
        View accessor (e.g. position/top/right/bottom/left) rather than a
        FlexStyle field.
        """
        if not self._view_hpp_text:
            return False
        for c in candidates:
            for pat in (
                rf"\bset_{re.escape(c)}\b",
                rf"\b{re.escape(c)}\s*\(",
            ):
                if re.search(pat, self._view_hpp_text):
                    return True
        return False

    def _yoga_layout_has_call(self, ygnode_setter: str) -> bool:
        """True if yoga_layout.cpp invokes the given YGNodeStyleSet* helper."""
        if not self._yoga_layout_text:
            return False
        return ygnode_setter in self._yoga_layout_text

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

    # ── Snake-case helper (camelCase -> snake_case) for FlexStyle lookup ─

    @staticmethod
    def _camel_to_snake(name: str) -> str:
        return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()

    # ── Enum-value normalization ─────────────────────────────────────
    #
    # Catalog `supportedValues` entries are hand-edited and frequently carry
    # human-readable annotations in trailing parentheses to explain the
    # binding nuance, e.g.::
    #
    #     "flex (implicit)"
    #     "none (via setVisible(false))"
    #     "row-reverse (RTL)"
    #
    # The Yoga oracle stores bare CSS-style tokens (`flex`, `none`,
    # `row-reverse`). Naive string equality between the two surfaces
    # mis-classifies annotated catalog entries as NOT-IMPL even when the
    # adapter found a binding, inflating the drift count (Codex P1 on
    # PR #1395, tracked as #1413).
    #
    # Normalize by stripping a single trailing parenthetical group plus any
    # surrounding whitespace. Apply on BOTH sides so the contract is
    # symmetric — if the oracle ever grows annotations, comparison stays
    # well-defined. We deliberately do NOT strip mid-string parentheses
    # because no Yoga value contains them today; if that changes the rule
    # widens at that point, not now.

    @staticmethod
    def _normalize_enum_value(v: str) -> str:
        """Strip a trailing ``" (...)"`` annotation from a catalog/oracle value.

        ``"flex (implicit)"`` -> ``"flex"``;
        ``"none (via setVisible(false))"`` -> ``"none"`` (handles nested ``()``);
        ``"row-reverse"`` -> ``"row-reverse"`` (unchanged);
        ``"mid (paren) inside"`` -> ``"mid (paren) inside"`` (only trailing);
        ``"unbalanced ("`` -> ``"unbalanced ("`` (no balanced group at end).
        Empty / non-string inputs round-trip to ``""``.
        """
        if not v:
            return ""
        s = str(v).strip()
        # Strip exactly ONE trailing balanced parenthetical group. We can't
        # use a flat regex because annotations may themselves contain ``()``
        # (e.g. ``"none (via setVisible(false))"``). Walk backward from the
        # end maintaining a paren-depth counter; when depth returns to 0,
        # we've found the matching opening ``(`` and can slice it off.
        if s.endswith(")"):
            depth = 0
            for i in range(len(s) - 1, -1, -1):
                c = s[i]
                if c == ")":
                    depth += 1
                elif c == "(":
                    depth -= 1
                    if depth == 0:
                        return s[:i].rstrip()
            # Unbalanced — no opening paren found. Leave as-is rather than
            # silently truncating; the catalog reviewer will spot it.
        return s

    @classmethod
    def _normalize_enum_values(cls, values) -> set[str]:
        """Return a set of normalized non-empty values, preserving uniqueness."""
        out: set[str] = set()
        for v in values or []:
            n = cls._normalize_enum_value(v)
            if n:
                out.add(n)
        return out

    # ── Main classification ──────────────────────────────────────────

    def run(self, entry: CatalogEntry) -> Result:
        prop = entry.short_name  # e.g. "flexDirection"

        # 0. wontfix is OOS by definition.
        if entry.status == "wontfix":
            return Result(
                entry=entry,
                status=Status.OOS,
                detail="catalog status=wontfix (explicitly out of scope)",
            )

        # 1. If Yoga itself doesn't define this property, it's OOS.
        oracle_entry = self._oracle["properties"].get(prop)
        if oracle_entry is None:
            return Result(
                entry=entry,
                status=Status.OOS,
                detail=f"property {prop!r} not present in Yoga oracle (out-of-scope)",
            )

        kind = oracle_entry.get("kind", "string")
        oracle_values = list(oracle_entry.get("values") or [])

        # 2. Check whether mapsTo signals "no implementation today".
        maps_to = entry.maps_to or ""
        if self._maps_to_marks_unimpl(maps_to):
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=f"mapsTo signals no implementation: {maps_to!r}",
            )

        if self._maps_to_marks_noop(maps_to):
            return Result(
                entry=entry,
                status=Status.NO_OP,
                detail=f"mapsTo signals no-op acceptance: {maps_to!r}",
            )

        # 3. Look for binding evidence in either FlexStyle (geometry.hpp)
        #    or a direct View accessor (view.hpp). We accept either the
        #    snake_case field name or any explicit identifier the catalog
        #    cited in mapsTo.
        snake = self._camel_to_snake(prop)
        candidates = [snake, prop]
        # extract any backticked / FlexStyle.X / View::set_X identifiers from mapsTo
        candidates += re.findall(r"FlexStyle\.([a-zA-Z_]+)", maps_to)
        candidates += re.findall(r"View::set_([a-zA-Z_]+)", maps_to)
        candidates += re.findall(r"`([a-zA-Z_][a-zA-Z0-9_]*)`", maps_to)
        has_flex_field = self._flex_style_has_field(candidates)
        has_view_accessor = self._view_has_accessor(candidates)

        if not (has_flex_field or has_view_accessor):
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=(
                    f"no FlexStyle field or View accessor found for {prop!r} "
                    f"(tried {candidates}); mapsTo={maps_to!r}"
                ),
            )

        # 4. Compare supportedValues vs the oracle's enum value set.
        #    Normalize annotated values like "flex (implicit)" -> "flex" on
        #    BOTH sides so the comparison is robust to either surface
        #    growing parenthetical context. (Codex P1 on PR #1395 / #1413.)
        if kind == "enum" and oracle_values:
            sup = self._normalize_enum_values(entry.supported_values)
            unsup = self._normalize_enum_values(entry.unsupported_values)
            oracle_set = self._normalize_enum_values(oracle_values)

            matched = sorted(oracle_set & sup)
            missing_from_supported = sorted(oracle_set - sup)

            # PASS iff every oracle value is in supportedValues AND
            # nothing the oracle defines is listed as unsupported.
            if oracle_set <= sup and not (oracle_set & unsup):
                return Result(
                    entry=entry,
                    status=Status.PASS,
                    detail=(
                        f"all {len(oracle_set)} Yoga values for {prop!r} are claimed supported"
                    ),
                    matched_supported=matched,
                )

            # If at least one oracle value is in supported, it's DIVERGE.
            if oracle_set & sup:
                return Result(
                    entry=entry,
                    status=Status.DIVERGE,
                    detail=(
                        f"{len(matched)}/{len(oracle_set)} Yoga values supported; "
                        f"missing: {missing_from_supported}"
                    ),
                    matched_supported=matched,
                    unmatched_supported=missing_from_supported,
                    extra_unsupported=sorted(oracle_set & unsup),
                )

            # Field exists but no oracle value is claimed — treat as NOT_IMPL
            # (the binding is present but the value translation isn't wired).
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=(
                    f"FlexStyle field {snake!r} present but no Yoga values claimed "
                    f"in supportedValues; missing all of {sorted(oracle_set)}"
                ),
                unmatched_supported=sorted(oracle_set),
            )

        # 5. Non-enum kinds (number, length, length-or-percentage*, edge-set, string).
        # If supportedValues is empty/"all" and unsupportedValues has nothing meaningful,
        # call PASS. Otherwise DIVERGE.
        if entry.unsupported_values and any(
            v.strip() and v.strip().lower() != "none" for v in entry.unsupported_values
        ):
            return Result(
                entry=entry,
                status=Status.DIVERGE,
                detail=(
                    f"non-enum {kind!r} property has {len(entry.unsupported_values)} "
                    f"unsupported values listed: {entry.unsupported_values}"
                ),
                extra_unsupported=list(entry.unsupported_values),
            )

        return Result(
            entry=entry,
            status=Status.PASS,
            detail=f"non-enum {kind!r} property bound; no unsupported values listed",
        )
