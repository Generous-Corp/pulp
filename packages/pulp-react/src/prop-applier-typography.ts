// prop-applier-typography — text / font / text-decoration / writing
// props.
//
// `applyTypographyProp(id, key, value, props)` returns true if it
// handled the key, false otherwise. Each handled prop emits the bridge
// calls expected by the corresponding text/style contract.

import { call, _resolveVar } from './prop-applier-internal.js';

// Translate CSS / React-Native fontWeight keyword values to numeric weights
// before reaching the bridge. Mirrors the same logic in the JS CSS shim
// (`web-compat-style-decl.js`). Numeric values (`400`, `'500'`) flow through
// unchanged. Do not collapse this to `Number(value)`: `Number('bold')` is NaN
// and would silently fall back to 400, rendering bold text as normal.
// The fallback keeps CSS keyword semantics explicit:
//   normal  -> 400
//   bold    -> 700
//   lighter -> 300 (no font cascade in pulp; safe lower default)
//   bolder  -> 700 (no font cascade in pulp; "one step bolder than
//                   normal" lands on bold)
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

/// How a bare number reads on a length property whose CSS unitless form is
/// a multiplier (today: `lineHeight`).
///
/// - `'dom'`: React DOM / CSS. `lineHeight: 1.5` and `lineHeight: 24` are
///   both multipliers of the font size.
/// - `'native'`: React Native. Every number is a pixel length.
/// - `'auto'` (default): mixed sources. A number `<= 8` is a multiplier and a
///   larger one is pixels, because RN ports and design-tool exports emit
///   `lineHeight: 24` (px) and `lineHeight: 1.5` (ratio) side by side.
///
/// Strings carry their own unit and never consult the dialect, except a bare
/// numeric string on a multiplier property, which follows it like a number.
export type LengthDialect = 'dom' | 'native' | 'auto';

let lengthDialect: LengthDialect = 'auto';

/// Select how bare numbers on multiplier properties are read. Returns the
/// previous dialect so a caller can restore it.
export function setLengthDialect(dialect: LengthDialect): LengthDialect {
    const previous = lengthDialect;
    if (dialect === 'dom' || dialect === 'native' || dialect === 'auto') lengthDialect = dialect;
    return previous;
}

/// The font size a Label paints at when none is set, and the CSS root font
/// size `rem` resolves against.
const DEFAULT_FONT_SIZE = 14;
const ROOT_FONT_SIZE = 16;

/// What each typography length means when it has no unit, a `%`, or the
/// `normal` keyword. `em` and `%` resolve against the element's own font size
/// for every property except `fontSize`, where they resolve against the
/// inherited size (the applier does not see the parent, so Pulp's default
/// Label size stands in). `normal: null` means the keyword is not valid there.
const TYPOGRAPHY_LENGTHS: Record<string, { unitless: 'px' | 'multiplier'; normal: number | null }> = {
    fontSize:      { unitless: 'px',         normal: null },
    letterSpacing: { unitless: 'px',         normal: 0 },
    // 0 is the bridge's "no fixed line height": the Label uses its font's.
    lineHeight:    { unitless: 'multiplier', normal: 0 },
};

function _multiplierFromUnitless(n: number): boolean {
    if (lengthDialect === 'dom') return true;
    if (lengthDialect === 'native') return false;
    return n > 0 && n <= 8;
}

const LENGTH_PATTERN = /^([+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?)(px|em|rem|%)?$/i;

/// Resolve the element's own font size in pixels from its props, for `em`
/// and `%` on the other typography lengths.
function _ownFontSize(props: Record<string, unknown> | undefined): number {
    const size = props ? resolveTypographyLength('fontSize', props.fontSize, undefined) : null;
    return size !== null && size > 0 ? size : DEFAULT_FONT_SIZE;
}

/// Resolve a typography length (`fontSize`, `letterSpacing`, `lineHeight`) to
/// the pixel number its bridge setter takes. Units come from the value and the
/// property table, never from the value's magnitude, except for a bare number
/// on a multiplier property under the `'auto'` dialect. Returns null when the
/// value is not a length this resolver understands (for example `calc()`), so
/// the caller leaves the native value alone rather than writing a guess.
export function resolveTypographyLength(
    property: string,
    value: unknown,
    props: Record<string, unknown> | undefined,
): number | null {
    const rule = TYPOGRAPHY_LENGTHS[property];
    if (!rule) return null;
    const resolved = _resolveVar(value);
    const base = () => property === 'fontSize' ? DEFAULT_FONT_SIZE : _ownFontSize(props);
    if (typeof resolved === 'number') {
        if (!Number.isFinite(resolved)) return null;
        if (rule.unitless === 'multiplier' && _multiplierFromUnitless(resolved))
            return resolved * base();
        return resolved;
    }
    if (typeof resolved !== 'string') return null;
    const text = resolved.trim();
    if (text.toLowerCase() === 'normal') return rule.normal;
    const match = LENGTH_PATTERN.exec(text);
    if (!match) return null;
    const n = Number(match[1]);
    if (!Number.isFinite(n)) return null;
    switch ((match[2] || '').toLowerCase()) {
        case 'px':  return n;
        case 'em':  return n * base();
        case 'rem': return n * ROOT_FONT_SIZE;
        case '%':   return n / 100 * base();
        default:
            if (rule.unitless === 'multiplier' && _multiplierFromUnitless(n))
                return n * base();
            return n;
    }
}

function _callLength(verb: string, id: string, key: string, value: unknown,
                     props: Record<string, unknown> | undefined): void {
    const px = resolveTypographyLength(key, value, props);
    if (px !== null) call(verb, id, px);
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

        // RN ViewStyle uses `writingDirection` for writing direction. CSS
        // uses `direction`, but the pulp prop name `direction` already
        // routes to FlexProps via setFlex above. The CSS-string-form
        // `style.direction = 'rtl'` path goes through the el.style adapter's
        // `direction` case, which calls setDirection directly.
        case 'writingDirection': call('setDirection', id, value as string); return true;

        // RN text layout aliases that map to existing bridge setters:
        // verticalAlign is RN's cross-platform vertical-align prop, while
        // textAlignVertical is the RN-Android equivalent and uses the same
        // setVerticalAlign target. textDecorationColor / textDecorationStyle
        // route to the matching text-decoration longhand setters.
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
        // Include `'auto'` and `'justify'` (CSS / RN canonical). `'auto'` is
        // writing-direction-relative (LTR-only today, degrades to `'left'`);
        // `'justify'` flows to canvas TextAlign::justify, with backends
        // approximating as left when full justify layout is unavailable.
        case 'textAlign':       call('setTextAlign', id, value as string); return true;
        // Typography. Label widgets honor these via setX bridge functions.
        // The fontFamily bridge picks the first non-empty family from a
        // comma-separated CSS list and stores it on the Label or on the
        // owning View's `inheritable_font_family_` slot for container cascade.
        // Families not registered with Skia fall through to the platform
        // default. Resolve `var(--mono)` before forwarding because the bridge
        // expects a real family name, not a CSS variable expression.
        case 'fontFamily':      call('setFontFamily', id, _resolveVar(value) as string); return true;
        // Length strings ('0.08em', '1.5rem', '120%', 'normal') resolve to
        // pixels here: the bridge setters take numbers and read a string as
        // its leading number with the unit dropped.
        case 'fontSize':        _callLength('setFontSize', id, key, value, props); return true;
        case 'fontWeight':      call('setFontWeight', id, _normalizeFontWeight(value)); return true;
        case 'fontStyle':       call('setFontStyle', id, value as string); return true;
        case 'letterSpacing':   _callLength('setLetterSpacing', id, key, value, props); return true;
        // A bare `lineHeight` number is a font-size multiplier or pixels by
        // the active LengthDialect; a unit-bearing string is never re-read.
        case 'lineHeight':      _callLength('setLineHeight', id, key, value, props); return true;
        // line-clamp + webkit-line-clamp wiring. Both line-clamp keys funnel
        // through the same setter. 0 / non-finite clears the slot.
        case 'lineClamp':
        case 'webkitLineClamp': {
            const n = typeof value === 'number' ? value : parseInt(String(value), 10);
            call('setLineClamp', id, Number.isFinite(n) ? n : 0);
            return true;
        }

        // RN's `textDecorationLine` is the spec-aligned name; pulp's bridge
        // uses `setTextDecoration` (single-keyword form). RN allows
        // `'underline line-through'` as a compound; pass through verbatim.
        // The bridge's keyword table currently honors single-keyword `'none'
        // / 'underline' / 'line-through' / 'overline'`.
        case 'textDecorationLine': call('setTextDecoration', id, value as string); return true;
        // RN textShadow cluster. The CSS shim (`web-compat-style-decl.js`)
        // handles aggregate CSS shorthand. Each per-attribute setter here
        // writes one slot in isolation so a diff that touches one prop
        // doesn't clobber the others.
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

        // RN's Android-only `includeFontPadding`. Pulp's text-shaping
        // doesn't add Android-vestigial vertical glyph padding regardless of
        // this value, so the bridge accepts the keyword, stores it on a View
        // slot for round-trip state, and the paint pipeline ignores it.
        // Authors setting `false` get tight-baseline behavior by default;
        // authors setting `true` get the same result because Pulp cannot add
        // padding it does not model.
        case 'includeFontPadding': call('setIncludeFontPadding', id, Boolean(value)); return true;

        // CSS `text-transform`.
        case 'textTransform':      call('setTextTransform', id, value as string); return true;

        // RN's `fontVariant` is a string-array of OpenType feature tokens
        // (`['small-caps', 'tabular-nums', ...]`). Full implementation
        // requires HarfBuzz hb_feature_t settings on the shape pass. Forward
        // as a comma-joined string to a reserved bridge function; until the
        // bridge registers `setFontVariant`, the dispatch is a no-op because
        // the `call` helper silently skips unregistered names.
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
