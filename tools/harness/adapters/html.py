"""HTML surface adapter — week 1 deliverable (one of five).

Classifies each `html/*` entry in compat.json against three layers of
evidence:

1. The oracle (`tools/harness/oracles/html/html-supported.json`) — the
   reference table that says "this catalog row is an Element method called
   `setAttribute`" or "this is the `<button>` tag and should call
   `createToggleButton` in the bridge".
2. The catalog payload (`mapsTo`, `supportedValues`, `unsupportedValues`,
   `notes`) — what the catalog claims pulp does today.
3. The web-compat JS shims + bridge surface:

   * `core/view/js/web-compat-element.js` — Element.prototype.* methods and
     `Object.defineProperty(Element.prototype, ...)` properties.
   * `core/view/js/web-compat-document.js` — `document = { ... }` members.
   * `core/view/src/widget_bridge.cpp` — `engine_.register_function("...")`
     calls. We grep for these once at adapter init.

The verdict is PASS / DIVERGE / NO-OP / NOT-IMPL / OOS — see
`tools/harness/status.py` for the full taxonomy.

Like the yoga adapter, this is a static-evidence classifier. The Week-3+
upgrade path replaces the oracle table with a `jsdom`-driven trace
comparator (see `tools/harness/oracles/html/README.md`).
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
    "not routed",
    "not wired",
    "no binding",
    "no bridge",
    "not modeled",
    "not modelled",
    "no constructor surface",
    "no actual `new event(",
)

# Markers that imply "accepted but does nothing" — the binding entry exists.
NO_OP_MARKERS = (
    "no-op",
    "noop",
    "accepted silently",
    "accepted but ignored",
    "stored ... but not",
    "stored in _attributes but not",
    "stored but not",
    "ignored",
    "stub",
)


@register_adapter("html")
class HtmlAdapter(AdapterBase):
    surface = "html"

    def __init__(self, repo_root: Path):
        super().__init__(repo_root)
        self._oracle = self._load_oracle()
        # Element.prototype.* lives across multiple shim files. The dispatch
        # is split between web-compat-element.js (most accessors / methods),
        # web-compat-dom-ops.js (the four DOM-mutation methods —
        # appendChild/removeChild/insertBefore/replaceChild — patched as a
        # second pass after element.js loads), and the legacy
        # web-compat.js bundle (still hosts a few entries). Concatenate
        # them once so the prototype-evidence regex sees the full surface.
        # P5-7 follow-up (PR #2337) extracted Element.prototype's Event +
        # Pointer-capture methods (addEventListener / removeEventListener /
        # dispatchEvent / setPointerCapture / releasePointerCapture /
        # hasPointerCapture + the synthetic event payloads) into
        # web-compat-element-events.js. Without it in the union, the
        # `html/Element_setPointerCapture` oracle false-classifies as
        # NOT_IMPL (Codex P2 on PR #2337 — same class as #2253's
        # canvas2d adapter gap fixed by #2317).
        self._element_js = "\n".join(
            self._read(p)
            for p in (
                "core/view/js/web-compat-element.js",
                "core/view/js/web-compat-element-events.js",
                "core/view/js/web-compat-dom-ops.js",
                "core/view/js/web-compat.js",
            )
        )
        # `document.<member>` is defined either inside the
        # `var document = { ... }` literal in web-compat-document.js OR as a
        # post-init `document.<X> = ...` patch in web-compat.js. Concatenate
        # for the same reason.
        self._document_js = "\n".join(
            self._read(p)
            for p in (
                "core/view/js/web-compat-document.js",
                "core/view/js/web-compat.js",
            )
        )
        self._dom_ops_js = self._read("core/view/js/web-compat-dom-ops.js")
        self._bridge_cpp = self._read("core/view/src/widget_bridge.cpp")

    # ── Oracle + source loading ──────────────────────────────────────

    def _load_oracle(self) -> dict:
        path = (
            self.repo_root
            / "tools"
            / "harness"
            / "oracles"
            / "html"
            / "html-supported.json"
        )
        with open(path) as f:
            return json.load(f)

    def _read(self, rel: str) -> str:
        p = self.repo_root / rel
        if not p.exists():
            return ""
        return p.read_text(encoding="utf-8", errors="replace")

    # ── Evidence helpers ─────────────────────────────────────────────

    def _element_has_method(self, name: str) -> bool:
        """True if `Element.prototype.<name> = function` exists."""
        if not self._element_js or not name:
            return False
        pattern = rf"Element\.prototype\.{re.escape(name)}\s*=\s*function"
        return bool(re.search(pattern, self._element_js))

    def _element_has_property(self, name: str) -> bool:
        """True if `Object.defineProperty(Element.prototype, "<name>", ...)` exists."""
        if not self._element_js or not name:
            return False
        pattern = (
            rf"Object\.defineProperty\(\s*Element\.prototype\s*,\s*"
            rf"[\"']{re.escape(name)}[\"']"
        )
        return bool(re.search(pattern, self._element_js))

    def _document_has_member(self, name: str) -> bool:
        """True if `<name>:` appears inside web-compat-document.js.

        We accept either the literal `name:` (the property form used inside
        the `document = { ... }` literal) or `document.<name>` directly.
        """
        if not self._document_js or not name:
            return False
        # property form inside the document literal
        if re.search(rf"^\s*{re.escape(name)}\s*:", self._document_js, re.MULTILINE):
            return True
        # explicit `document.<name>` reference (e.g. `document.addEventListener`
        # is installed via `document.addEventListener = function`).
        if re.search(rf"\bdocument\.{re.escape(name)}\b", self._document_js):
            return True
        return False

    def _bridge_registers(self, fn_name: str) -> bool:
        """True if widget_bridge.cpp has `engine_.register_function("<fn_name>", ...)`."""
        if not self._bridge_cpp or not fn_name:
            return False
        pattern = (
            rf'register_function\(\s*"{re.escape(fn_name)}"'
        )
        return bool(re.search(pattern, self._bridge_cpp))

    def _ensure_native_handles_tag(self, tag: str) -> bool:
        """True if `_ensureNative` switch in element.js dispatches on this tag."""
        if not self._element_js or not tag:
            return False
        # Look for `tag === "<name>"` in the _ensureNative dispatch block.
        return bool(
            re.search(rf'tag\s*===\s*[\"\']{re.escape(tag.lower())}[\"\']', self._element_js)
        )

    # ── Catalog `mapsTo` heuristics ──────────────────────────────────

    @staticmethod
    def _maps_to_marks_unimpl(maps_to: str) -> bool:
        if not maps_to:
            return False
        m = maps_to.lower()
        return any(marker in m for marker in NOT_IMPL_MARKERS)

    @staticmethod
    def _maps_to_marks_noop(maps_to: str) -> bool:
        if not maps_to:
            return False
        m = maps_to.lower()
        return any(marker in m for marker in NO_OP_MARKERS)

    # ── Main classification ──────────────────────────────────────────

    def run(self, entry: CatalogEntry) -> Result:
        # 0. wontfix is OOS by definition.
        if entry.status == "wontfix":
            return Result(
                entry=entry,
                status=Status.OOS,
                detail="catalog status=wontfix (explicitly out of scope)",
            )

        # Look up the oracle entry by full key (e.g. "html/Element_classList").
        oracle_entry = self._oracle["entries"].get(entry.name)
        if oracle_entry is None:
            # We don't recognize this catalog row at all — treat as OOS so the
            # drift list calls it out. Better than silently passing.
            return Result(
                entry=entry,
                status=Status.OOS,
                detail=(
                    f"catalog key {entry.name!r} not present in HTML oracle "
                    "(out-of-scope or oracle out of date)"
                ),
            )

        category = oracle_entry.get("category", "")
        maps_to = entry.maps_to or ""

        # Catalog `missing` entries trump everything — record as NOT_IMPL with
        # the catalog's own diagnostic. (The harness still measures whether
        # any binding exists; below we check binding evidence too, so a
        # mis-claimed `missing` will appear in the drift list as PASS.)
        if entry.status == "missing" and self._maps_to_marks_unimpl(maps_to):
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=f"catalog says missing + mapsTo confirms: {maps_to[:120]!r}",
            )

        if self._maps_to_marks_noop(maps_to):
            return Result(
                entry=entry,
                status=Status.NO_OP,
                detail=f"mapsTo signals no-op acceptance: {maps_to[:120]!r}",
            )

        # ── Per-category evidence checks ─────────────────────────────

        if category == "element_method":
            member = oracle_entry.get("js_member", "")
            if not member:
                return self._oracle_misconfigured(entry, "missing js_member")
            if not self._element_has_method(member):
                return Result(
                    entry=entry,
                    status=Status.NOT_IMPL,
                    detail=(
                        f"Element.prototype.{member} not found in web-compat-element.js"
                    ),
                )
            return self._classify_with_unsupported_values(
                entry,
                detail=f"Element.prototype.{member} present in web-compat-element.js",
            )

        if category == "element_property":
            member = oracle_entry.get("js_member", "")
            if not member:
                return self._oracle_misconfigured(entry, "missing js_member")
            if not self._element_has_property(member):
                return Result(
                    entry=entry,
                    status=Status.NOT_IMPL,
                    detail=(
                        f"Object.defineProperty(Element.prototype, {member!r}) "
                        "not found in web-compat-element.js"
                    ),
                )
            return self._classify_with_unsupported_values(
                entry,
                detail=(
                    f"Element.prototype.{member} property accessor present in "
                    "web-compat-element.js"
                ),
            )

        if category == "document_member":
            member = oracle_entry.get("js_member", "")
            if not member:
                return self._oracle_misconfigured(entry, "missing js_member")
            if not self._document_has_member(member):
                return Result(
                    entry=entry,
                    status=Status.NOT_IMPL,
                    detail=(
                        f"document.{member} not found in web-compat-document.js"
                    ),
                )
            # For createElement specifically: also confirm a representative
            # set of bridge calls is registered. If even one of the expected
            # bridge fns is missing we DIVERGE.
            expected_bridge = oracle_entry.get("expected_bridge_calls", [])
            missing_bridge = [
                fn for fn in expected_bridge if not self._bridge_registers(fn)
            ]
            if missing_bridge:
                return Result(
                    entry=entry,
                    status=Status.DIVERGE,
                    detail=(
                        f"document.{member} present, but bridge missing "
                        f"create/factory functions: {missing_bridge}"
                    ),
                    unmatched_supported=missing_bridge,
                )
            return self._classify_with_unsupported_values(
                entry,
                detail=f"document.{member} present in web-compat-document.js",
            )

        if category == "html_tag":
            tag = oracle_entry.get("tag", "")
            if not tag:
                return self._oracle_misconfigured(entry, "missing tag")
            tag_handled = self._ensure_native_handles_tag(tag)
            expected_bridge = oracle_entry.get("expected_bridge_calls", [])
            missing_bridge = [
                fn for fn in expected_bridge if not self._bridge_registers(fn)
            ]
            if not tag_handled:
                return Result(
                    entry=entry,
                    status=Status.NOT_IMPL,
                    detail=(
                        f"_ensureNative does not dispatch on tag={tag!r} in "
                        "web-compat-element.js"
                    ),
                )
            if missing_bridge:
                return Result(
                    entry=entry,
                    status=Status.DIVERGE,
                    detail=(
                        f"<{tag}> dispatch present, but bridge missing: "
                        f"{missing_bridge}"
                    ),
                    unmatched_supported=missing_bridge,
                )
            return self._classify_with_unsupported_values(
                entry,
                detail=(
                    f"<{tag}> wired through _ensureNative -> bridge "
                    f"({', '.join(expected_bridge) if expected_bridge else 'no bridge calls listed'})"
                ),
            )

        if category == "feature":
            # Higher-level features (ARIA, StyleSheet_inline, DocumentFragment,
            # Event_constructor) — we trust the catalog claim plus the
            # not-impl / no-op markers we already checked. If the catalog
            # status is `supported` AND no marker fired, treat as PASS.
            status = (entry.status or "").lower()
            if status == "supported":
                return self._classify_with_unsupported_values(
                    entry,
                    detail=f"feature claimed supported; oracle: {oracle_entry.get('evidence', '')[:80]!r}",
                )
            if status == "partial":
                return Result(
                    entry=entry,
                    status=Status.DIVERGE,
                    detail=(
                        f"feature claimed partial; "
                        f"unsupported={entry.unsupported_values}"
                    ),
                    extra_unsupported=list(entry.unsupported_values or []),
                )
            if status == "missing":
                return Result(
                    entry=entry,
                    status=Status.NOT_IMPL,
                    detail=f"feature claimed missing: {oracle_entry.get('evidence', '')[:80]!r}",
                )
            return Result(
                entry=entry,
                status=Status.NOT_IMPL,
                detail=f"feature with unrecognized catalog status: {entry.status!r}",
            )

        # Unknown category — flag the oracle, don't blow up the run.
        return self._oracle_misconfigured(
            entry, f"unrecognized oracle category {category!r}"
        )

    # ── Helpers ──────────────────────────────────────────────────────

    @staticmethod
    def _oracle_misconfigured(entry: CatalogEntry, why: str) -> Result:
        return Result(
            entry=entry,
            status=Status.OOS,
            detail=f"oracle entry misconfigured ({why})",
        )

    def _classify_with_unsupported_values(
        self, entry: CatalogEntry, detail: str
    ) -> Result:
        """Once binding evidence is found, decide PASS vs. DIVERGE based on
        whether the catalog records gaps in `unsupportedValues`.
        """
        unsup = [
            v for v in (entry.unsupported_values or [])
            if v.strip() and v.strip().lower() != "none"
        ]
        if unsup:
            return Result(
                entry=entry,
                status=Status.DIVERGE,
                detail=(
                    f"{detail}; catalog lists {len(unsup)} unsupported "
                    f"value(s): {unsup[:6]}"
                ),
                extra_unsupported=unsup,
            )
        # PASS — binding present, no recorded gaps.
        return Result(
            entry=entry,
            status=Status.PASS,
            detail=detail,
            matched_supported=list(entry.supported_values or []),
        )
