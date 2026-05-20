// prop-applier-internal — shared plumbing for the per-domain prop
// handlers (P5-NEW-A split of the former monolithic applyOne switch).
//
// This module owns the bridge `call` helper and the file-local value
// coercion helpers that more than one domain handler needs. Domain
// modules (prop-applier-layout / -paint / -typography / -transform /
// -events) import from here so the single `_pa_count` logging counter
// stays shared — exactly as it was when `call` lived inside
// prop-applier.ts.

// Bridge globals are looked up through globalThis at call time so the
// mock-bridge install path picks them up. See host-config.ts for the
// matching pattern.
type AnyFn = (...args: unknown[]) => unknown;
const g = globalThis as unknown as Record<string, AnyFn | undefined>;
let _pa_count = 0;
export function call(name: string, ...args: unknown[]): void {
    const fn = g[name];
    if (typeof fn !== 'function') return; // optional bridge fns are fine to skip
    _pa_count++;
    if (_pa_count <= 100) {
        const lg = (g as Record<string, AnyFn | undefined>).__spectrLog;
        if (typeof lg === 'function') {
            const a0 = args[0] !== undefined ? String(args[0]).slice(0, 25) : '';
            const a1 = args[1] !== undefined ? String(args[1]).slice(0, 25) : '';
            const a2 = args[2] !== undefined ? String(args[2]).slice(0, 25) : '';
            lg('[pa#' + _pa_count + '] ' + name + '(' + a0 + ',' + a1 + (args.length > 2 ? ',' + a2 : '') + ')');
        }
    }
    fn(...args);
}

// pulp #1899 (gap #3) — resolve `var(--name [, fallback])` references in
// string-valued style props before forwarding to the bridge.
//
// The HTML-shim path (`core/view/js/css-parser.js`) already resolves
// var() via the same lookup tiers, but the React-prop-applier lane
// bypasses that — it forwards the raw `style.fontFamily` value straight
// into `setFontFamily`. The string "var(--mono)" then reached Skia's
// font matcher as a literal family name, returned no match, and fell
// through to a proportional sans (visible as the Spectr top-bar "faint
// label" symptom — labels rendered in the wrong typeface AND in a layer
// that degraded the LCD AA, compounding the faintness).
//
// Resolution tiers (first hit wins):
//   1. `globalThis.__pulpCssVars[name]` — developer-set runtime
//       registry. Apps populate this at mount when they have direct
//       knowledge of which CSS variables map to which strings.
//   2. `getStringToken(name)` — bridge call into `theme.strings` (the
//       same map design-import.cpp writes when loading Stitch / W3C
//       tokens, and the same map `setStringToken` from the CSS shim
//       writes for string-valued custom properties).
//   3. `getMotionToken(name)` — bridge call into `theme.dimensions`
//       (only useful when the var is numeric — fontFamily callers
//       won't hit this branch, but length-typed callers might).
//   4. The literal fallback supplied as `var(--name, FALLBACK)`.
//   5. The original string, returned unchanged so the caller can decide
//       how to handle the miss.
//
// The regex match is conservative: only top-level `^var(...)` is
// resolved. Embedded `calc(var(...) + 10px)` is out of scope for the
// React lane — the bridge consumers for `fontFamily` / `color` /
// `borderColor` etc. don't accept calc-expressions anyway.
export function _resolveVar(value: unknown): unknown {
    if (typeof value !== 'string') return value;
    const s = value.trim();
    if (!s.startsWith('var(') || !s.endsWith(')')) return value;

    // Find the FIRST top-level comma inside var(...) so a nested
    // `var(--a, var(--b, default))` is split at the right point.
    const inner = s.slice(4, s.length - 1);
    let commaPos = -1;
    let depth = 0;
    for (let i = 0; i < inner.length; i++) {
        const c = inner.charAt(i);
        if (c === '(') depth++;
        else if (c === ')') depth--;
        else if (c === ',' && depth === 0) { commaPos = i; break; }
    }

    let name: string;
    let fallback: string | undefined;
    if (commaPos >= 0) {
        name = inner.slice(0, commaPos).trim();
        fallback = inner.slice(commaPos + 1).trim();
    } else {
        name = inner.trim();
    }
    if (name.startsWith('--')) name = name.slice(2);
    if (!name) return value;

    // Tier 1: runtime developer-set registry.
    const reg = (globalThis as Record<string, unknown>).__pulpCssVars as
        | Record<string, string> | undefined;
    if (reg && typeof reg[name] === 'string' && reg[name]) {
        return reg[name];
    }

    // Tier 2: theme.strings via bridge.
    const getStr = (globalThis as Record<string, unknown>).getStringToken as
        | ((n: string) => string) | undefined;
    if (typeof getStr === 'function') {
        const sv = getStr(name);
        if (typeof sv === 'string' && sv) return sv;
    }

    // Tier 3: theme.dimensions via bridge (numeric tokens).
    const getNum = (globalThis as Record<string, unknown>).getMotionToken as
        | ((n: string) => number) | undefined;
    if (typeof getNum === 'function') {
        const nv = getNum(name);
        if (typeof nv === 'number' && nv !== 0 && Number.isFinite(nv)) {
            return String(nv);
        }
    }

    // Tier 4: explicit fallback. Recurse so `var(--a, var(--b, 0))`
    // walks the chain. Cap recursion via the input shrinking strictly
    // on each step (we strip one var() wrapper per call).
    if (fallback !== undefined && fallback.length > 0) {
        return _resolveVar(fallback);
    }

    // Tier 5: surface the original unresolved string. Better than ""
    // because downstream Skia / View code can at least log the unknown
    // token name.
    return value;
}
