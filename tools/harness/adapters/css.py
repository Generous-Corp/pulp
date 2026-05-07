"""CSS surface adapter — week 1 deliverable (pulp #1392, second of 5 surfaces).

Mirrors the three-layer classifier introduced by the yoga adapter:

1. The oracle (`tools/harness/oracles/css/css-supported.json`) — curated
   enum value sets per property, plus pointers to the supplementary
   sources (MDN spec, JS-side router, React prop applier).
2. The catalog payload (`mapsTo`, `supportedValues`, `unsupportedValues`,
   `notes`) — what `compat.json` claims pulp does today.
3. **The JS-side router** (`core/view/js/web-compat-style-decl.js`) —
   the `case "X":` set inside `_applyProperty` is parsed at adapter init
   and used as the allow-list of CSS properties that actually reach the
   bridge from `el.style.X = ...` assignments. This is the strongest
   evidence the harness has access to without spinning up a headless
   browser, so we lean on it heavily.

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
# Aligned with the yoga adapter's vocabulary plus a couple of CSS-specific
# phrases that show up in compat.json (e.g. "no branch", "no CSS pseudo-
# class system today").
NOT_IMPL_MARKERS = (
    "no implementation",
    "no implementation today",
    "no impl",
    "silently dropped",
    "silently drops",
    "no branch",
    "no field",
    "missing branch",
    "not wired",
    "no binding",
    "no bridge",
    "no css",
    "no setflex case",
    "n/a",
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
    "stored on element",
    "store on element",
)

# CSS shorthand / non-enum kinds that don't get an enum table. The adapter
# uses this to short-circuit the enum check for properties whose oracle
# entry is intentionally absent.
NON_ENUM_PROP_HINTS = (
    "color",
    "background",
    "border",
    "shadow",
    "filter",
    "transform",
    "transition",
    "animation",
    "size",
    "spacing",
    "image",
    "outline",
    "padding",
    "margin",
    "inset",
    "width",
    "height",
    "gap",
    "radius",
    "opacity",
    "clamp",
)


@register_adapter("css")
class CssAdapter(AdapterBase):
    surface = "css"

    def __init__(self, repo_root: Path):
        super().__init__(repo_root)
        self._oracle = self._load_oracle()
        # The wired set is parsed from web-compat-style-decl.js — adapter
        # init is the only time we read the file.
        self._js_text = self._read("core/view/js/web-compat-style-decl.js")
        self._wired = self._extract_wired_cases(self._js_text)
        # MDN-known kebab-case props (used for OOS gate, permissive).
        self._mdn = self._load_mdn_props()
        # @pulp/react prop-applier surface — secondary route for some CSS
        # entries. Parsed for cross-checking; not authoritative.
        self._prop_applier_text = self._read(
            "packages/pulp-react/src/prop-applier.ts"
        )

    # ── Oracle / source loading ──────────────────────────────────────

    def _load_oracle(self) -> dict:
        path = (
            self.repo_root
            / "tools"
            / "harness"
            / "oracles"
            / "css"
            / "css-supported.json"
        )
        with open(path) as f:
            return json.load(f)

    def _load_mdn_props(self) -> set[str]:
        path = self.repo_root / "tools" / "import-design" / "catalogs" / "mdn-css.tsv"
        if not path.exists():
            return set()
        out: set[str] = set()
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) >= 2 and parts[1] == "css-property":
                out.add(parts[0])
        return out

    def _read(self, rel: str) -> str:
        p = self.repo_root / rel
        if not p.exists():
            return ""
        return p.read_text(encoding="utf-8", errors="replace")

    @staticmethod
    def _extract_wired_cases(js_text: str) -> set[str]:
        """Return every `case "X":` key inside web-compat-style-decl.js.

        The file has exactly one switch — the `_applyProperty` body — so
        scanning the whole file's `case "X":` tokens is unambiguous.
        """
        if not js_text:
            return set()
        return set(re.findall(r'case\s+"([^"]+)"\s*:', js_text))

    # ── Catalog `mapsTo` heuristics ───────────────────────────────────

    @staticmethod
    def _maps_to_marks_unimpl(maps_to: str) -> bool:
        if not maps_to:
            return True
        m = maps_to.lower().strip()
        # n/a is a shorthand for wontfix in the catalog (table-layout etc.);
        # treat it as NOT_IMPL when the entry isn't already flagged wontfix.
        if m == "n/a":
            return True
        return any(marker in m for marker in NOT_IMPL_MARKERS)

    @staticmethod
    def _maps_to_marks_noop(maps_to: str) -> bool:
        if not maps_to:
            return False
        m = maps_to.lower()
        return any(marker in m for marker in NO_OP_MARKERS)

    # ── camelCase / kebab-case helpers ───────────────────────────────

    @staticmethod
    def _camel_to_kebab(name: str) -> str:
        return re.sub(r"(?<!^)(?=[A-Z])", "-", name).lower()

    # ── Synthetic / non-property entries (`css/__*`) ─────────────────

    @staticmethod
    def _is_synthetic(entry: CatalogEntry) -> bool:
        # css/__hover_pseudo, css/__pseudo_classes_note — used by the
        # catalog to track CSS *features* (pseudo-classes, cascade
        # semantics) that aren't bound to a single property name.
        return entry.short_name.startswith("__")

    # ── Main classification ──────────────────────────────────────────

    def run(self, entry: CatalogEntry) -> Result:
        prop = entry.short_name  # e.g. "flexDirection", "__hover_pseudo"

        # 0. wontfix is OOS by definition, regardless of any other signal.
        if entry.status == "wontfix":
            return Result(
                entry=entry,
                status=Status.OOS,
                detail="catalog status=wontfix (explicitly out of scope)",
            )

        # 1. Synthetic entries (`__*`) — the catalog tracks CSS features
        #    rather than properties. Trust the catalog status verbatim
        #    (no oracle lookup possible) but emit through the harness
        #    taxonomy. Drift cannot be detected for these.
        if self._is_synthetic(entry):
            return Result(
                entry=entry,
                status=entry.expected_status,
                detail=(
                    f"synthetic feature entry {prop!r} — no per-property "
                    f"oracle lookup; passthrough from catalog status="
                    f"{entry.status!r}"
                ),
            )

        # 2. mapsTo signals "no implementation" / "no branch".
        maps_to = entry.maps_to or ""
        wired = prop in self._wired

        if not wired and self._maps_to_marks_unimpl(maps_to):
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=(
                    f"no `case \"{prop}\":` in web-compat-style-decl.js and "
                    f"mapsTo signals no implementation: {maps_to[:100]!r}"
                ),
            )

        if self._maps_to_marks_noop(maps_to):
            return Result(
                entry=entry,
                status=Status.NO_OP,
                detail=f"mapsTo signals no-op acceptance: {maps_to[:100]!r}",
            )

        # 3. Wired check is the strongest available signal.
        if not wired:
            # Property is not in MDN AND not wired AND not flagged as a
            # no-op — best classification is OOS (made-up / non-spec
            # property). Catches `printMargin` (made-up) cleanly.
            kebab = self._camel_to_kebab(prop)
            if kebab not in self._mdn and prop not in (
                "wordWrap",
                "webkitLineClamp",
            ):
                return Result(
                    entry=entry,
                    status=Status.OOS,
                    detail=(
                        f"property {prop!r} (kebab={kebab!r}) is not in the "
                        f"MDN catalog and has no `case` in web-compat-style-"
                        f"decl.js; treating as out-of-scope"
                    ),
                )
            # Property is real per MDN but unwired → NOT_IMPL. If the
            # catalog claims `supported` or `partial`, the result still
            # gets flagged as drift via Result.drifts.
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=(
                    f"no `case \"{prop}\":` in web-compat-style-decl.js; "
                    f"mapsTo={maps_to[:100]!r}"
                ),
            )

        # 4. Property is wired. Compare against an enum oracle if one
        #    exists. Otherwise rely on the unsupportedValues signal.
        oracle_enum = (self._oracle.get("enums") or {}).get(prop)
        if oracle_enum is not None:
            sup = set(entry.supported_values or [])
            unsup = set(entry.unsupported_values or [])
            oracle_set = set(oracle_enum.get("values") or [])

            matched = sorted(oracle_set & sup)
            missing_from_supported = sorted(oracle_set - sup)

            if oracle_set <= sup and not (oracle_set & unsup):
                return Result(
                    entry=entry,
                    status=Status.PASS,
                    detail=(
                        f"all {len(oracle_set)} CSS-spec values for {prop!r} "
                        f"are claimed supported"
                    ),
                    matched_supported=matched,
                )

            if oracle_set & sup:
                return Result(
                    entry=entry,
                    status=Status.DIVERGE,
                    detail=(
                        f"{len(matched)}/{len(oracle_set)} CSS-spec values "
                        f"supported; missing: {missing_from_supported}"
                    ),
                    matched_supported=matched,
                    unmatched_supported=missing_from_supported,
                    extra_unsupported=sorted(oracle_set & unsup),
                )

            # Wired, but supportedValues doesn't intersect the oracle's
            # value set at all — the route exists but value translation
            # isn't claimed. Mark NOT_IMPL with the missing list.
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=(
                    f"`case \"{prop}\":` present but no CSS-spec values "
                    f"claimed in supportedValues; missing all of "
                    f"{sorted(oracle_set)}"
                ),
                unmatched_supported=sorted(oracle_set),
            )

        # 5. Wired, non-enum kind. If the catalog lists meaningful
        #    unsupported values, that's DIVERGE. Otherwise PASS.
        meaningful_unsup = [
            v for v in (entry.unsupported_values or [])
            if v and v.strip().lower() not in ("none", "n/a")
        ]
        if meaningful_unsup:
            return Result(
                entry=entry,
                status=Status.DIVERGE,
                detail=(
                    f"non-enum CSS property {prop!r} is wired but lists "
                    f"{len(meaningful_unsup)} unsupported values: "
                    f"{meaningful_unsup}"
                ),
                extra_unsupported=list(meaningful_unsup),
            )

        return Result(
            entry=entry,
            status=Status.PASS,
            detail=(
                f"non-enum CSS property {prop!r} is wired through "
                f"web-compat-style-decl.js; no unsupported values listed"
            ),
        )
