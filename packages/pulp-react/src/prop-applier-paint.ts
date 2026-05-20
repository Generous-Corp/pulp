// prop-applier-paint — visual-style props: background, border,
// outline, shadow, list-style, opacity/visibility, filters, masks
// (P5-NEW-A split of the former monolithic applyOne switch).
//
// `applyPaintProp(id, key, value)` returns true if it handled the key,
// false otherwise. Behavior is byte-identical to the matching cases in
// the pre-split prop-applier switch — same bridge calls in the same
// order.

import { call, _resolveVar } from './prop-applier-internal.js';

// pulp #1434 (Triage #15) — parse a CSS-spec single-shadow `box-shadow`
// string. Mirrors the regex in `core/view/js/web-compat-style-decl.js`
// (the DOM-lite path) so the @pulp/react path produces identical
// dispatch shape.
//
// Wave 2 rn — added comma-separated multi-shadow support:
// `_splitMultiShadow` walks the string respecting paren depth (so an
// `rgba(0,0,0,0.3)` color literal inside one shadow doesn't get cut by
// its internal commas). Each substring then flows through the existing
// single-shadow parser; the prop-applier dispatches one setBoxShadow
// per parsed shadow. This matches the CSS spec layering order where
// the FIRST shadow paints on TOP (closest to the viewer); the bridge
// keeps last-write-wins on the View slot today, so order matters at
// paint time. Mock-bridge captures one call per shadow.
//
// Format (per shadow): `[inset] <dx>px <dy>px <blur>px [<spread>px] <color>`
// Returns the parsed shape or null if the string doesn't match.
interface _ParsedBoxShadow {
    offsetX: number;
    offsetY: number;
    blur: number;
    spread: number;
    color: string;
    inset: boolean;
}
function _parseBoxShadow(s: string): _ParsedBoxShadow | null {
    let work = s.trim();
    let inset = false;
    if (/^inset\s+/i.test(work)) {
        inset = true;
        work = work.replace(/^inset\s+/i, '');
    } else if (/\s+inset\s*$/i.test(work)) {
        inset = true;
        work = work.replace(/\s+inset\s*$/i, '');
    }
    // <dx>px <dy>px <blur>px [<spread>px] <color>
    const m = work.match(/(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px(?:\s+(-?[\d.]+)px)?\s+(.*)/);
    if (!m) return null;
    return {
        offsetX: parseFloat(m[1]),
        offsetY: parseFloat(m[2]),
        blur: parseFloat(m[3]),
        spread: parseFloat(m[4] ?? '0'),
        color: m[5].trim(),
        inset,
    };
}

// Wave 2 rn — split a comma-separated multi-shadow string into individual
// single-shadow substrings. Respects paren depth so commas inside a
// `rgba(...)` color literal don't split a single shadow. Returns the
// list of trimmed substrings (or `[s.trim()]` for a single shadow with
// no top-level comma).
function _splitMultiShadow(s: string): string[] {
    const out: string[] = [];
    let depth = 0;
    let start = 0;
    for (let i = 0; i < s.length; i++) {
        const c = s[i];
        if (c === '(') depth++;
        else if (c === ')') { if (depth > 0) depth--; }
        else if (c === ',' && depth === 0) {
            out.push(s.slice(start, i).trim());
            start = i + 1;
        }
    }
    out.push(s.slice(start).trim());
    return out.filter((x) => x.length > 0);
}

// Wave 2 rn — coerce a borderRadius value (uniform number OR RN
// Fabric elliptical `{ x, y }` object) to the single number the
// Skia paint backend currently honors. The RN Fabric form differs x
// from y to draw an ellipse-shaped corner; the bridge / Skia path
// today only takes a uniform radius, so we average x and y as the
// closest visual approximation. True elliptical rendering needs the
// paint side to call SkRRect::setRectXY; tracked as a deferred gap.
function _coerceRadius(value: unknown): number | string {
    if (typeof value === 'number') return value;
    if (value != null && typeof value === 'object') {
        const o = value as { x?: number; y?: number };
        const x = typeof o.x === 'number' ? o.x : 0;
        const y = typeof o.y === 'number' ? o.y : 0;
        return (x + y) / 2;
    }
    if (typeof value === 'string') {
        // pulp #1663 — preserve "%" suffix so the bridge can route to
        // percent-aware paint-time resolution. Other unit suffixes (px,
        // em, etc.) collapse to a plain number as before.
        const trimmed = value.trim();
        if (trimmed.endsWith('%')) {
            const n = parseFloat(trimmed);
            return Number.isFinite(n) ? `${n}%` : 0;
        }
        const n = parseFloat(trimmed);
        return Number.isFinite(n) ? n : 0;
    }
    return 0;
}

/// Apply a visual-style/paint prop. Returns true if handled.
export function applyPaintProp(
    id: string,
    key: string,
    value: unknown,
): boolean {
    switch (key) {
        // Visual style
        // CSS `background` shorthand can carry color, gradient, or url. The
        // C++ `setBackground` bridge fn (widget_bridge.cpp:3350) only parses
        // a color token via `set_background_color(parseHexColor(...))`, so
        // gradient strings collapse to a bogus color (often white). Caught
        // by Spectr's 2026-05-11 live render — chip strip's
        // `background: linear-gradient(to bottom, ...)` painted white, making
        // the white SPECTR / ZOOMABLE FILTER BANK / axis labels invisible.
        // Mirrors the same gradient-detection fix applied to backgroundImage
        // below (Codex P1 on #1831).
        case 'background': {
            const sval = String(value);
            if (/(linear|radial|conic)-gradient\(/i.test(sval)) {
                call('setBackgroundGradient', id, sval);
                return true;
            }
            if (/^\s*url\s*\(/i.test(sval)) return true;
            // pulp #1899 (gap #3) — resolve var() for solid-color backgrounds
            // (`background: var(--panel)`). Gradient strings keep their
            // raw value because the gradient parser handles var() itself
            // via the C++ shim.
            call('setBackground', id, _resolveVar(sval) as string);
            return true;
        }
        case 'backgroundGradient': call('setBackgroundGradient', id, value as string); return true;
        // CSS `background-image` longhand. The C++ `setBackground` bridge fn
        // only parses a color token (widget_bridge.cpp:3350 →
        // `set_background_color(parseHexColor(...))`), so we must detect
        // gradient strings here and route to the gradient bridge instead;
        // otherwise inputs like `linear-gradient(...)` collapse to a bogus
        // color parse. `url(...)` has no image bridge today — drop quietly
        // rather than corrupting the color slot. (Codex P1 on #1831.)
        case 'backgroundImage': {
            const sval = String(value);
            if (/(linear|radial|conic)-gradient\(/i.test(sval)) {
                call('setBackgroundGradient', id, sval);
                return true;
            }
            if (/^\s*url\s*\(/i.test(sval)) {
                return true;
            }
            // pulp #1899 (gap #3) — mirror `background` above.
            call('setBackground', id, _resolveVar(sval) as string);
            return true;
        }
        // pulp #1517 — background sub-properties. The bridge stores the
        // keyword on the View slot. Paint impact today is partial:
        //   • `backgroundAttachment` — `scroll` is conformant; `fixed` /
        //     `local` are noop (pulp doesn't model scroll contexts).
        //   • `backgroundClip` — `text` is the interesting variant
        //     (paint-time SkBlendMode::kSrcIn against text glyphs);
        //     deferred to a future PR. Other values noop on solid bg.
        //   • `backgroundOrigin` — relevant only for repeating gradients;
        //     noop today.
        case 'backgroundAttachment': call('setBackgroundAttachment', id, value as string); return true;
        case 'backgroundClip':       call('setBackgroundClip',       id, value as string); return true;
        case 'backgroundOrigin':     call('setBackgroundOrigin',     id, value as string); return true;
        case 'border': {
            const b = value as { color: string; width?: number; radius?: number };
            call('setBorder', id, b.color, b.width ?? 1, b.radius ?? 0);
            return true;
        }
        // pulp #1027 (audit PR #1166 finding #4) — RN-style flat border props.
        // These MUST route through the per-attribute bridge setters so a
        // commitUpdate that touches only one of them preserves the others.
        // Lowering them onto the unified `setBorder(id, color, width, radius)`
        // would clobber the unset slots back to 0/empty.
        case 'borderColor':  call('setBorderColor', id, _resolveVar(value) as string); return true;
        case 'borderWidth':  call('setBorderWidth', id, value as number); return true;
        // Wave 2 rn — `borderRadius` accepts the RN Fabric elliptical
        // form `{ x, y }`. The Skia paint backend currently takes a
        // single uniform radius per corner (no rrect ellipse axes), so
        // we degrade the elliptical input by averaging x and y — the
        // closest visual fidelity for the common Fabric usage where x
        // and y differ only modestly. True elliptical rendering needs
        // a paint-side rrect (Skia SkRRect::setRectXY) and remains a
        // deferred gap. Numeric values flow through unchanged.
        case 'borderRadius': call('setBorderRadius', id, _coerceRadius(value)); return true;
        // pulp #1434 Triage #10 — borderStyle keyword passes verbatim
        // to setBorderStyle. Bridge maps to View::BorderStyle. Skia
        // installs the dash effect for `dashed` / `dotted`; other
        // named styles currently degrade to solid.
        case 'borderStyle':  call('setBorderStyle', id, value as string); return true;
        // pulp #1514 — list-style cluster. Pulp doesn't model
        // <li>/<ul>/<ol> semantics, so the bridge stores the value
        // verbatim on the View and a future paint pass renders the
        // marker. Today the catalog is `partial` (stored, not
        // painted). The shorthand `listStyle` parses on the JS side
        // into the 3 longhands; consumers MAY emit any combo of
        // type / position / image keywords (CSS spec: any order).
        case 'listStyle': {
            const sval = String(value).trim();
            const tokens = sval.split(/\s+/);
            const typeSet: Record<string, true> = {
                none: true, disc: true, circle: true, square: true, decimal: true,
            };
            const posSet: Record<string, true> = { inside: true, outside: true };
            let sawType = false, sawImage = false;
            for (const tok of tokens) {
                if (tok.indexOf('url(') === 0) {
                    call('setListStyleImage', id, tok);
                    sawImage = true;
                } else if (posSet[tok]) {
                    call('setListStylePosition', id, tok);
                } else if (typeSet[tok]) {
                    if (tok === 'none' && sawType && !sawImage) {
                        call('setListStyleImage', id, 'none');
                        sawImage = true;
                    } else {
                        call('setListStyleType', id, tok);
                        sawType = true;
                    }
                }
            }
            return true;
        }
        case 'listStyleType':     call('setListStyleType', id, value as string); return true;
        case 'listStyleImage':    call('setListStyleImage', id, value as string); return true;
        case 'listStylePosition': call('setListStylePosition', id, value as string); return true;
        case 'borderTop':    { const b = value as { color: string; width: number }; call('setBorderSide', id, 'top', b.width, b.color); return true; }
        case 'borderRight':  { const b = value as { color: string; width: number }; call('setBorderSide', id, 'right', b.width, b.color); return true; }
        case 'borderBottom': { const b = value as { color: string; width: number }; call('setBorderSide', id, 'bottom', b.width, b.color); return true; }
        case 'borderLeft':   { const b = value as { color: string; width: number }; call('setBorderSide', id, 'left', b.width, b.color); return true; }
        // RN per-side flat props — route to the per-side bridge setters
        // that already preserve the unrelated attribute (see widget_bridge
        // applyBorderSide helper introduced in pulp #1026).
        case 'borderTopColor':       call('setBorderTopColor', id, _resolveVar(value) as string); return true;
        case 'borderRightColor':     call('setBorderRightColor', id, _resolveVar(value) as string); return true;
        case 'borderBottomColor':    call('setBorderBottomColor', id, _resolveVar(value) as string); return true;
        case 'borderLeftColor':      call('setBorderLeftColor', id, _resolveVar(value) as string); return true;
        case 'borderTopWidth':       call('setBorderTopWidth', id, value as number); return true;
        case 'borderRightWidth':     call('setBorderRightWidth', id, value as number); return true;
        case 'borderBottomWidth':    call('setBorderBottomWidth', id, value as number); return true;
        case 'borderLeftWidth':      call('setBorderLeftWidth', id, value as number); return true;
        // Wave 2 rn — per-corner radii accept the RN Fabric elliptical
        // `{ x, y }` form too (degraded to averaged uniform radius;
        // see `borderRadius` above for the rrect rationale).
        case 'borderTopLeftRadius':     call('setBorderTopLeftRadius', id, _coerceRadius(value)); return true;
        case 'borderTopRightRadius':    call('setBorderTopRightRadius', id, _coerceRadius(value)); return true;
        case 'borderBottomLeftRadius':  call('setBorderBottomLeftRadius', id, _coerceRadius(value)); return true;
        case 'borderBottomRightRadius': call('setBorderBottomRightRadius', id, _coerceRadius(value)); return true;
        // pulp #1519 — RN outline cluster. Paint-time ring drawn OUTSIDE
        // the border-box (no Yoga layout impact). Each prop routes to its
        // own per-attribute bridge fn so a JSX prop diff that touches one
        // outline-* preserves the others. Style keyword set mirrors
        // borderStyle (CSS spec is identical).
        case 'outlineColor':  call('setOutlineColor',  id, _resolveVar(value) as string); return true;
        case 'outlineOffset': call('setOutlineOffset', id, value as number); return true;
        case 'outlineStyle':  call('setOutlineStyle',  id, value as string); return true;
        case 'outlineWidth':  call('setOutlineWidth',  id, value as number); return true;
        // CSS shorthand `outline: <width> <style> <color>` — mirror the
        // parser already wired in web-compat-style-decl.js so JSX
        // `style={{ outline: '1px solid red' }}` fans out to the three
        // per-attribute setters identically to el.style.outline = '...'.
        case 'outline': {
            const m = String(value).match(/([\d.]+)px\s+(\w+)\s+(.+)/);
            if (!m) return true;
            call('setOutlineWidth', id, parseFloat(m[1]));
            call('setOutlineStyle', id, m[2]);
            call('setOutlineColor', id, m[3].trim());
            return true;
        }
        case 'opacity':      call('setOpacity', id, value as number); return true;
        case 'visible':      call('setVisible', id, value as boolean); return true;
        // pulp #1434 (Triage #15) — `boxShadow` accepts:
        //  • `null` / `undefined` / `'none'` → clearBoxShadow
        //  • String form (`'2px 4px 8px rgba(0,0,0,0.3)'` with optional
        //    `inset`) — parsed inline below.
        //  • Object form `{ offsetX, offsetY, blur?, spread?, color,
        //    inset? }` — dispatched directly.
        //
        // Wave 2 rn — multi-shadow comma-separated string lists are now
        // wired: we split on commas (respecting paren depth so a color
        // literal like `rgba(0,0,0,0.3)` doesn't get cut), then dispatch
        // one setBoxShadow per parsed shadow. CSS spec layers the first
        // shadow on TOP (closest to viewer); the bridge applies them in
        // dispatch order so paint order matches the input string.
        case 'boxShadow': {
            if (value == null || value === 'none' || value === '') {
                call('clearBoxShadow', id);
                return true;
            }
            // Wave 4 rn — RN Fabric `BoxShadowValue[]` array form. Each
            // element is `{ offsetX, offsetY, color, blurRadius?,
            // spreadDistance?, inset? }` (RN spec field names; CSS
            // box-shadow uses `blur` / `spread`). The bridge takes one
            // dispatch per shadow; we clear first to avoid append-only
            // accumulation across re-renders, then dispatch in order
            // (CSS layers the first shadow on top — paint order matches
            // input order, same as the multi-shadow string path above).
            if (Array.isArray(value)) {
                call('clearBoxShadow', id);
                for (const s of value as ReadonlyArray<{
                    offsetX: number; offsetY: number;
                    blurRadius?: number; spreadDistance?: number;
                    blur?: number; spread?: number;
                    color: string; inset?: boolean;
                }>) {
                    if (!s) continue;
                    const blur   = typeof s.blurRadius     === 'number' ? s.blurRadius
                                 : typeof s.blur           === 'number' ? s.blur   : 4;
                    const spread = typeof s.spreadDistance === 'number' ? s.spreadDistance
                                 : typeof s.spread         === 'number' ? s.spread : 0;
                    call('setBoxShadow', id, s.offsetX, s.offsetY, blur, spread,
                         s.color, !!s.inset);
                }
                return true;
            }
            if (typeof value === 'object') {
                const s = value as { offsetX: number; offsetY: number; blur?: number; spread?: number; blurRadius?: number; spreadDistance?: number; color: string; inset?: boolean };
                const blur   = typeof s.blurRadius     === 'number' ? s.blurRadius
                             : typeof s.blur           === 'number' ? s.blur   : 4;
                const spread = typeof s.spreadDistance === 'number' ? s.spreadDistance
                             : typeof s.spread         === 'number' ? s.spread : 0;
                call('setBoxShadow', id, s.offsetX, s.offsetY,
                            blur, spread,
                            s.color, !!s.inset);
                return true;
            }
            if (typeof value === 'string') {
                const parts = _splitMultiShadow(value);
                let emitted = 0;
                for (const p of parts) {
                    const parsed = _parseBoxShadow(p);
                    if (parsed) {
                        call('setBoxShadow', id, parsed.offsetX, parsed.offsetY,
                             parsed.blur, parsed.spread, parsed.color, parsed.inset);
                        emitted++;
                    }
                }
                return true; // unparseable shadows silently dropped; non-empty parts that fail just skip (matches CSS shim behavior)
                void emitted;
            }
            return true;
        }
        // CSS `backdrop-filter: blur(Npx)`. The bridge setter takes a numeric
        // blur radius in px; mirror the parser in web-compat-style-decl.js
        // (other filter functions are intentionally ignored — matches the
        // `unsupportedValues: ["other filter functions"]` compat entry).
        case 'backdropFilter': {
            const sval = String(value).trim().toLowerCase();
            if (sval === '' || sval === 'none') {
                call('setBackdropFilter', id, 0);
                return true;
            }
            const bdm = sval.match(/blur\(\s*([\d.]+)\s*(px)?\s*\)/);
            if (bdm) {
                call('setBackdropFilter', id, parseFloat(bdm[1]) || 0);
                return true;
            }
            return true;
        }

        // pulp #1434 rn logical-edge bundle (sub-agent #27 finding) —
        // RN's logical border-width edges. Route to the per-side bridge
        // setter that preserves the unrelated attribute (color) — same
        // pattern as borderLeftWidth / borderRightWidth.
        case 'borderStartWidth': {
            call('setBorderLeftWidth', id, value as number);
            return true;
        }
        case 'borderEndWidth': {
            call('setBorderRightWidth', id, value as number);
            return true;
        }

        // pulp #1434 rn bridge-wires bundle (sub-agent #27 finding) —
        // RN-style props that already had C++ bridge fns registered
        // but no `@pulp/react` prop-applier dispatch. Each forwards
        // the keyword / string straight through to the matching setter.
        case 'backfaceVisibility': call('setBackfaceVisibility', id, value as string); return true;
        case 'cursor':             call('setCursor', id, value as string); return true;
        case 'filter':             call('setFilter', id, value as string); return true;
        // pulp #1515 — CSS clip-path / mask cluster. The bridge fns
        // store the value on the View; Skia paint side honors
        // `clip-path: path("...")` via SkPath::FromSVGString. Other
        // clip-path forms (URL refs, named shapes) and mask painting
        // (saveLayer + SkBlendMode::kDstIn) are deferred. The
        // prop-applier dispatches verbatim — keyword normalization
        // and shorthand expansion live in the bridge / JS shim.
        case 'clipPath':           call('setClipPath', id, value as string); return true;
        case 'mask':               call('setMask', id, value as string); return true;
        case 'maskImage':          call('setMaskImage', id, value as string); return true;
        case 'maskSize':           call('setMaskSize', id, value as string); return true;
        // CSS `appearance` — Pulp paints all widgets custom, so this
        // is observably storage-only. Slot exists for round-trip.
        case 'appearance':         call('setAppearance', id, value as string); return true;
        // CSS `object-fit` / `object-position` — image fitting.
        // Storage-only today; ImageView paint follow-up.
        case 'objectFit':          call('setObjectFit', id, value as string); return true;
        case 'objectPosition':     call('setObjectPosition', id, value as string); return true;
        // pulp #1549 — RN `mixBlendMode` (RN 0.76 New Architecture).
        // Forwards the W3C blend-mode keyword string to
        // `setMixBlendMode`; the bridge keyword→enum table lives at
        // widget_bridge.cpp::setMixBlendMode.
        case 'mixBlendMode':       call('setMixBlendMode', id, value as string); return true;
        case 'pointerEvents':      call('setPointerEvents', id, value as string); return true;
        case 'userSelect':         call('setUserSelect', id, value as string); return true;

        // pulp #1552 — backgroundRepeat is storage-only at the View layer
        // (paint-time honoring is a follow-up for url() / repeating
        // gradient backgrounds).
        case 'backgroundRepeat': call('setBackgroundRepeat', id, value as string); return true;

        // pulp #1737 RN-OOS-fixup (audit 2026-05-11) — RN iOS-legacy
        // box-shadow longhand. Modern RN code uses `boxShadow` (CSS
        // shorthand) which Pulp fully supports, but upstream RN still
        // accepts shadowColor / shadowOffset / shadowOpacity /
        // shadowRadius as cross-platform-ish style props (originally
        // iOS-only, but Pulp implements them cross-platform via the
        // unified BoxShadow struct on View). Each per-attribute setter
        // writes ONE slot of View::shadow_ in isolation so a JSX diff
        // that touches one prop doesn't clobber the others.
        case 'shadowColor':   call('setShadowColor',   id, _resolveVar(value) as string); return true;
        case 'shadowOffset': {
            // RN spec: `{ width, height }` (number / number).
            const o = value as { width?: number; height?: number };
            const dx = typeof o?.width  === 'number' ? o.width  : 0;
            const dy = typeof o?.height === 'number' ? o.height : 0;
            call('setShadowOffset', id, dx, dy);
            return true;
        }
        case 'shadowOpacity': call('setShadowOpacity', id, value as number); return true;
        case 'shadowRadius':  call('setShadowRadius',  id, value as number); return true;

        // pulp #1737 RN-OOS-fixup final sweep — RN's Android-only
        // `elevation` (Material Design 0..24dp). The bridge shim
        // translates the elevation value to a Material-approximated
        // single-shadow BoxShadow so consumers shipping unchanged
        // RN-Android styles render a visible shadow on every Pulp
        // platform. Upstream RN ignores `elevation` on iOS entirely;
        // Pulp's cross-platform translation is a strict improvement.
        case 'elevation':     call('setElevation', id, value as number); return true;

        // pulp #1737 RN-OOS-fixup (#1812) — RN's `borderCurve` corner
        // shape. `circular` (default) keeps Pulp's standard rounded
        // corner; `continuous` switches the View paint to the iOS-
        // style squircle approximation (super-ellipse path). Authors
        // shipping iOS-aesthetic designs with `borderCurve: 'continuous'`
        // now get the visible squircle on every Pulp platform.
        case 'borderCurve':       call('setBorderCurve', id, value as string); return true;

        // pulp #1737 RN-OOS-fixup (final round) — RN's `isolation`.
        // Pulp's per-View paint model is structurally isolated by
        // default (per-View save_layer_with_blend composition + paint-
        // order-scoped z-index), matching the dominant author intent
        // of `isolation: isolate`. Honest CSS-subset claim: both
        // keywords round-trip; behavior matches `isolate` regardless.
        case 'isolation':         call('setIsolation', id, value as string); return true;

        // pulp #1434 (rn NOT-IMPL bundle 1) — RN's `experimental_backgroundImage`
        // (New Architecture only) accepts a CSS gradient string. Route
        // through the existing setBackgroundGradient bridge fn — same
        // shape, same parser. RN also allows an array-of-objects gradient
        // form on Fabric; that shape is NOT supported here (gradient
        // strings only). Catalog flips missing → partial.
        case 'experimental_backgroundImage':
            call('setBackgroundGradient', id, value as string);
            return true;

        default:
            return false;
    }
}
