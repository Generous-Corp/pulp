// prop-applier-typography — text / font / text-decoration / writing
// props (P5-NEW-A split of the former monolithic applyOne switch).
//
// `applyTypographyProp(id, key, value, props)` returns true if it
// handled the key, false otherwise. Behavior is byte-identical to the
// matching cases in the pre-split prop-applier switch — same bridge
// calls in the same order.

import { call, _resolveVar } from './prop-applier-internal.js';

// pulp #1434 (batch 3) — translate CSS / React-Native fontWeight keyword
// values to numeric weights before reaching the bridge. Mirrors the same
// logic in the JS CSS shim (`web-compat-style-decl.js`). Numeric values
// (`400`, `'500'`) flow through unchanged. The previous `Number(value)`
// fallback returned NaN for keywords like `'bold'`, which the bridge
// then coerced to 400 — silently mapping bold to normal. CSS spec:
//   normal  → 400
//   bold    → 700
//   lighter → 300 (no font cascade in pulp; safe lower default)
//   bolder  → 700 (no font cascade in pulp; "one step bolder than
//                  normal" lands on bold)
function _normalizeFontWeight(value: unknown): number {
    if (typeof value === 'number') return value;
    const s = String(value).trim().toLowerCase();
    if (s === 'normal') return 400;
    if (s === 'bold') return 700;
    if (s === 'lighter') return 300;
    if (s === 'bolder') return 700;
    const n = Number(s);
    return Number.isFinite(n) ? n : 400;
}

// Wave 2 rn — resolve a `lineHeight` value with optional unitless
// multiplier semantics. CSS spec: a unitless number on `line-height`
// means "multiply by the element's font-size" (e.g.
// `line-height: 1.5` with `font-size: 16` → 24px). We detect unitless
// multipliers by treating values <= 8 as ratios (RN ports + design-tool
// exports virtually always emit either `lineHeight: 24` (px) or
// `lineHeight: 1.5` (multiplier); the threshold of 8 leaves room for
// extremely large ratios while keeping common px values like 16 / 24 /
// 32 unambiguous). Scaled by the live `fontSize` from props (defaults
// to 14 — same Label default the bridge uses when no fontSize is
// present). Numeric strings ("1.5") are treated like numbers; px-suffix
// strings ("24px") strip the suffix and treat as absolute.
function _resolveLineHeight(value: unknown, props: Record<string, unknown> | undefined): number {
    let n: number;
    if (typeof value === 'number') {
        n = value;
    } else {
        const s = String(value).trim();
        const sp = s.endsWith('px') ? s.slice(0, -2) : s;
        n = parseFloat(sp);
    }
    if (!Number.isFinite(n)) return 0;
    if (n > 0 && n <= 8) {
        // Unitless multiplier — scale by current font-size.
        const fs = (props && typeof props.fontSize === 'number')
            ? (props.fontSize as number)
            : 14;
        return n * fs;
    }
    return n;
}

/// Apply a text/typography prop. Returns true if handled.
export function applyTypographyProp(
    id: string,
    key: string,
    value: unknown,
    props?: Record<string, unknown>,
): boolean {
    switch (key) {
        // CSS `white-space` — text wrapping behavior (normal | nowrap | pre |
        // pre-wrap). Bridge stores the slot on View; consumed by TextShaper
        // when computing line breaks. Mirror el.style path so JSX
        // `style={{ whiteSpace: 'nowrap' }}` doesn't silently drop.
        case 'whiteSpace':   call('setWhiteSpace', id, value as string); return true;
        // CSS `text-overflow` — clip | ellipsis. Bridge stores the slot for
        // Label paint to consume.
        case 'textOverflow': call('setTextOverflow', id, value as string); return true;

        // pulp #1434 Phase A2-3 — writing direction (RN ViewStyle uses
        // `writingDirection` for this — CSS uses `direction`, but the
        // pulp prop name `direction` already routes to FlexProps via
        // setFlex above. The CSS-string-form `style.direction = 'rtl'`
        // path goes through the el.style adapter's `direction` case
        // which calls setDirection directly).
        case 'writingDirection': call('setDirection', id, value as string); return true;

        // pulp #1737 RN-OOS-fixup (catalog audit 2026-05-11) — these 4
        // RN style props were classified `wontfix` in compat.json despite
        // having fully-wired bridge fns AND css-side proof on the
        // matching css/* surfaces (css/verticalAlign, css/textDecoration*
        // all `supported`). One-line route closes the rn-side gap; the
        // catalog flips from wontfix → supported on the same JSON edit.
        //
        // verticalAlign — RN's cross-platform vertical-align prop.
        // textAlignVertical — RN-Android equivalent of verticalAlign;
        //                     same setVerticalAlign target works for
        //                     both since Pulp's Label::vertical_align_
        //                     models what both CSS+RN-Android need.
        // textDecorationColor / textDecorationStyle — text-decoration
        //                     longhand setters; bridge already routes
        //                     to Label::set_text_decoration_color /
        //                     ::set_text_decoration_style.
        case 'verticalAlign':         call('setVerticalAlign', id, value as string); return true;
        case 'textAlignVertical':     call('setVerticalAlign', id, value as string); return true;
        case 'textDecorationColor':   call('setTextDecorationColor', id, _resolveVar(value) as string); return true;
        case 'textDecorationStyle':   call('setTextDecorationStyle', id, value as string); return true;

        // Text
        case 'text':            call('setText', id, String(value)); return true;
        // CSS-canonical `color` aliases RN-canonical `textColor`. Imported
        // React designs (JSX `style={{ color: '...' }}`) silently dropped
        // before this — bridge has `setTextColor`, the dispatch case was
        // missing the alias.
        case 'color':
        case 'textColor':       call('setTextColor', id, _resolveVar(value) as string); return true;
        // pulp #1434 — widen to include `'auto'` and `'justify'` (CSS /
        // RN canonical). `'auto'` is writing-direction-relative
        // (LTR-only today, degrades to `'left'`); `'justify'` flows to
        // canvas TextAlign::justify (SkParagraph kJustify wiring is a
        // follow-up — backends approximate as left for now).
        case 'textAlign':       call('setTextAlign', id, value as string); return true;
        // Typography — Label widgets honor these via setX bridge fns.
        // pulp #1434 Phase A2-5 — fontFamily IS now dispatched. The
        // bridge picks the first non-empty family from a comma-
        // separated CSS list and stores it on the Label or on the
        // owning View's `inheritable_font_family_` slot for container
        // cascade. The whole-list fallback chain (full font-stack
        // resolution) still depends on SkFontMgr registration in
        // pulp #932 — until that lands, families that aren't already
        // registered with Skia fall through to the platform default.
        // Wiring is independent: when #932 lands, no consumer change
        // is needed — the registry just resolves the same name.
        // pulp #1899 (gap #3) — resolve `var(--mono)` before forwarding.
        // The bridge expects a real family name; the literal "var(--mono)"
        // gives Skia's font matcher nothing to match against and silently
        // falls back to a proportional sans (e.g. Spectr top-bar labels
        // rendered in the wrong typeface AND inside an opacity layer that
        // degraded the LCD AA — both fixed in this change).
        case 'fontFamily':      call('setFontFamily', id, _resolveVar(value) as string); return true;
        case 'fontSize':        call('setFontSize', id, value as number); return true;
        case 'fontWeight':      call('setFontWeight', id, _normalizeFontWeight(value)); return true;
        case 'fontStyle':       call('setFontStyle', id, value as string); return true;
        case 'letterSpacing':   call('setLetterSpacing', id, value as number); return true;
        // Wave 2 rn — `lineHeight` accepts CSS unitless-multiplier
        // semantics. A value `<= 8` is treated as a multiplier of the
        // current `fontSize` from props (defaults to 14 when absent —
        // matches the Label default). Larger values flow through as
        // absolute pixels (the existing path). Px-suffix strings strip
        // the suffix and pass through as absolute too. The bridge
        // setter signature is unchanged — it always sees a number.
        case 'lineHeight':      call('setLineHeight', id, _resolveLineHeight(value, props)); return true;
        // pulp #1552 — line-clamp + webkit-line-clamp wiring. Both
        // line-clamp keys funnel through the same setter (shared CSS
        // shim case + RN-style alias). 0 / non-finite clears the slot.
        case 'lineClamp':
        case 'webkitLineClamp': {
            const n = typeof value === 'number' ? value : parseInt(String(value), 10);
            call('setLineClamp', id, Number.isFinite(n) ? n : 0);
            return true;
        }

        // pulp #1434 (rn NOT-IMPL bundle 1) — RN's `textDecorationLine`
        // is the spec-aligned name; pulp's bridge uses `setTextDecoration`
        // (single-keyword form). RN allows `'underline line-through'` as
        // a compound — pass through verbatim; the bridge's keyword table
        // currently honors single-keyword `'none' / 'underline' /
        // 'line-through' / 'overline'`. Compound rendering is the same
        // partial gap as css/textDecoration's "single-keyword only" note.
        case 'textDecorationLine': call('setTextDecoration', id, value as string); return true;
        // pulp #1434 (rn NOT-IMPL bundle 1) — RN textShadow cluster.
        // The CSS shim (`web-compat-style-decl.js`) calls `setTextShadow`
        // defensively (`if (typeof setTextShadow === 'function')`); the
        // bridge does NOT yet register that fn, so paint is a no-op
        // today. Wire the @pulp/react surface here so when the bridge
        // gains the registration (planned slot, see #1548 feature
        // branch) every consumer flips on without a JSX-side change.
        // Each per-attribute setter writes ONE slot in isolation so a
        // diff that touches one prop doesn't clobber the others.
        case 'textShadowColor':  call('setTextShadowColor',  id, _resolveVar(value) as string); return true;
        case 'textShadowOffset': {
            // RN spec: `{ width, height }` (number / number).
            const o = value as { width?: number; height?: number };
            const dx = typeof o?.width  === 'number' ? o.width  : 0;
            const dy = typeof o?.height === 'number' ? o.height : 0;
            call('setTextShadowOffset', id, dx, dy);
            return true;
        }
        case 'textShadowRadius': call('setTextShadowRadius', id, value as number); return true;

        // pulp #1737 RN-OOS-fixup final sweep — RN's Android-only
        // `includeFontPadding`. Pulp's text-shaping doesn't add
        // Android-vestigial vertical glyph padding regardless of this
        // value, so the bridge fn accepts the keyword + stores it on
        // a View slot (round-trip), and the paint pipeline ignores it.
        // Authors setting `false` (the common case — remove Android
        // padding) get what they want by default; authors setting
        // `true` get the same tight-baseline behavior (Pulp can't add
        // padding it doesn't model). Catalog status is `supported` as
        // an honest CSS-subset claim (same pattern as overscroll-behavior).
        case 'includeFontPadding': call('setIncludeFontPadding', id, Boolean(value)); return true;

        // CSS `text-transform`.
        case 'textTransform':      call('setTextTransform', id, value as string); return true;

        // pulp #1434 (rn NOT-IMPL bundle 1) — RN's `fontVariant` is a
        // string-array of OpenType feature tokens (`['small-caps',
        // 'tabular-nums', ...]`). True implementation requires HarfBuzz
        // hb_feature_t setting on the shape pass — outside the scope of
        // this prop-applier wiring. Forward as a comma-joined string to
        // a bridge fn name reserved for the future paint-time impl;
        // until the bridge registers `setFontVariant`, the dispatch is
        // a no-op (the `call` helper silently skips unregistered names).
        // Catalog flips missing → partial with the gotcha documented.
        case 'fontVariant': {
            const joined = Array.isArray(value)
                ? (value as string[]).join(',')
                : String(value);
            call('setFontVariant', id, joined);
            return true;
        }

        default:
            return false;
    }
}
