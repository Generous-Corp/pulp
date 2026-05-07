// prop-applier — turns React prop names into bridge setX calls.
//
// Used by both:
//   - host-config.appendChild / appendInitialChild (apply ALL props at first attach)
//   - host-config.commitUpdate (apply ONLY changed props)
//
// Each prop knows which bridge setter receives it. Centralizing the
// mapping here keeps the host config small and lets tests assert the
// emitted setX sequence with one diff helper.

import type { PulpInstance } from './types.js';
import { makeSyntheticEvent } from './synthetic-event.js';

// Bridge globals are looked up through globalThis at call time so the
// mock-bridge install path picks them up. See host-config.ts for the
// matching pattern.
type AnyFn = (...args: unknown[]) => unknown;
const g = globalThis as unknown as Record<string, AnyFn | undefined>;
let _pa_count = 0;
function call(name: string, ...args: unknown[]): void {
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

let _aap_count = 0;
function logApply(stage: string, id: string, type: string, propCount: number): void {
    _aap_count++;
    if (_aap_count <= 60) {
        const lg = (g as Record<string, AnyFn | undefined>).__spectrLog;
        if (typeof lg === 'function') {
            lg('[applyAll#' + _aap_count + '] ' + stage + ' ' + type + '/' + id + ' props=' + propCount);
        }
    }
}

/// Returns true if the prop is purely React-internal (not a bridge setter).
function isReactInternal(key: string): boolean {
    return (
        key === 'children' ||
        key === 'key' ||
        key === 'ref' ||
        key === 'id'
    );
}

/// Returns true if the prop is an event handler (onX) that we route
/// through the bridge's global `on(id, eventName, fn)` registrar.
function isEventHandler(key: string): boolean {
    return key.startsWith('on') && key.length > 2 && key[2] === key[2]?.toUpperCase();
}

/// Map a React-style on* prop to the bridge event name. The bridge
/// dispatches:
///   on_click → 'click'        (Button, Panel, etc.)
///   on_change → 'change'      (Knob, Fader, Checkbox, TextEditor)
///   on_toggle → 'toggle'      (Toggle, ToggleButton)
///   mouseenter / mouseleave   (Hover)
///   dismiss                   (Modal close)
function eventNameFor(propName: string): string {
    return propName.slice(2).toLowerCase();
}

/// Hover/pointer events the bridge gates behind registerHover(id).
/// onMouseEnter/onMouseLeave + their pointer-event aliases all map to
/// the bridge's mouseenter/mouseleave dispatch path; without
/// registerHover, the C++ side never fires the events even though the
/// JS listener is installed (pulp #1149).
function isHoverEvent(eventName: string): boolean {
    return (
        eventName === 'mouseenter' ||
        eventName === 'mouseleave' ||
        eventName === 'pointerenter' ||
        eventName === 'pointerleave'
    );
}

/// Pointer events the bridge gates behind registerPointer(id).
/// onPointerDown / onPointerUp / onPointerCancel / onPointerMove map
/// to the bridge's `on_pointer_event` callback path; without
/// `registerPointer`, the C++ side never wires the callback into the
/// View even though the JS listener is installed via
/// `on(id, 'pointerdown', fn)`. Spectr's FilterBank band drag was the
/// canonical repro — see import-design SKILL.md gotcha #8 (pulp #1381,
/// parallel to the existing isHoverEvent gating for #1149).
function isPointerEvent(eventName: string): boolean {
    return (
        eventName === 'pointerdown' ||
        eventName === 'pointerup' ||
        eventName === 'pointercancel' ||
        eventName === 'pointermove'
    );
}

/// Wheel events go through a separate `registerWheel(id)` bridge call
/// because the C++ dispatch lambda for wheel filters on `me.is_wheel`
/// (`registerPointer`'s lambda early-returns on `is_wheel`, and
/// `registerWheel`'s lambda early-returns on `!is_wheel`). Both can
/// coexist on the same widget since each chains to the previous
/// `on_pointer_event` lambda. Spectr's FilterBank zoom (onWheel
/// handler) needs this separate gate (pulp #1387 gap #4).
function isWheelEvent(eventName: string): boolean {
    return eventName === 'wheel';
}

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

// pulp #1434 (Triage #15) — parse a CSS-spec single-shadow `box-shadow`
// string. Mirrors the regex in `core/view/js/web-compat-style-decl.js`
// (the DOM-lite path) so the @pulp/react path produces identical
// dispatch shape. Multi-shadow comma-separated lists are deferred.
//
// Format: `[inset] <dx>px <dy>px <blur>px [<spread>px] <color>`
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

// pulp #1434 (Triage #9) — RN's `transform` is an array of single-property
// objects (`[{translateX:10},{rotate:'45deg'},{scale:1.5}]`). Figma /
// v0.dev / Claude Design exports emit this constantly. The bridge has
// setTranslate / setRotation / setScale (uniform), so the array-walker
// accumulates a per-render snapshot inside one pass and emits at most
// three consolidated calls. Within-array merging means
// `[{translateX:10},{translateY:20}]` produces ONE setTranslate(10,20)
// instead of two clobbering ones (the latter would zero the unrelated
// axis on each call). RN semantics also say each render's array is a
// complete description — absent fields reset to identity, so we don't
// carry state across renders.
//
// Bridge gaps (deferred — TODOs + follow-up issue):
//   • setScale is uniform-only — independent scaleX/scaleY axes can't
//     round-trip. We approximate: scale > scaleX > scaleY in priority,
//     last-write-wins within the array; if scaleX≠scaleY we emit the
//     last seen and document the limitation.
//   • rotateX/rotateY/perspective/matrix3d — 3D / matrix transforms not
//     modeled in pulp's 2D View (no perspective; rotation is Z-axis
//     only). Silently dropped with TODO.
//   • matrix(a b c d tx ty) — 2D affine; the CSS shim decomposes to
//     translate + uniform-scale + rotate components. The @pulp/react
//     RN array surface doesn't have a matrix entry today (RN spec:
//     only translateX/Y, scale, scaleX/Y, rotate/Z, skewX/Y), so the
//     walker just silently drops `matrix`/`matrix3d` for parity.
//
// Triage #9 fan-out (this PR) — `setSkew` is now a registered bridge
// function (View::set_skew has existed since the 2D slot was added).
// The walker dispatches skewX/skewY by accumulating both axes and
// emitting one consolidated setSkew(id, x_deg, y_deg) call.
interface _TransformSnapshot {
    tx: number;
    ty: number;
    rotateDeg: number;
    scale: number;
    skewX: number;
    skewY: number;
    haveTranslate: boolean;
    haveRotate: boolean;
    haveScale: boolean;
    haveSkew: boolean;
}

// Parse `'45deg'` / `'0.785rad'` / `45` (numeric) → degrees.
function _parseAngleDegrees(v: unknown): number {
    if (typeof v === 'number') return v;
    const s = String(v).trim();
    const m = s.match(/^(-?[\d.]+)\s*(deg|rad|turn|grad)?$/i);
    if (!m) return 0;
    const n = parseFloat(m[1]);
    const unit = (m[2] || 'deg').toLowerCase();
    if (unit === 'rad')  return n * (180 / Math.PI);
    if (unit === 'turn') return n * 360;
    if (unit === 'grad') return n * 0.9;
    return n;
}

// pulp #1434 rn bridge-wires bundle — parse a CSS transform-origin
// string into two fractional coordinates (0..1) the bridge expects.
// Accepts `'center'`, `'left top'`, `'NN%'` percentages, and `'NNpx'`
// pixel offsets (the latter assumed to be on a unit-bound View — so
// values just clamp). Falls back to {0.5, 0.5} on unrecognized input.
function _parseTransformOrigin(s: string): { x: number; y: number } {
    const work = s.trim().toLowerCase();
    if (work === 'center' || work === '') return { x: 0.5, y: 0.5 };
    const tokens = work.split(/\s+/);
    const tok2coord = (tok: string, axis: 'x' | 'y'): number => {
        if (tok === 'center') return 0.5;
        if (tok === 'left' || tok === 'top')   return 0.0;
        if (tok === 'right' || tok === 'bottom') return 1.0;
        if (tok.endsWith('%')) {
            const n = parseFloat(tok.slice(0, -1));
            return Number.isFinite(n) ? n / 100 : 0.5;
        }
        // Bare number / px — interpret as fractional 0..1 if <=1, else
        // clamp to 1.0 (better than negative/over-1 garbage on the
        // bridge side; full pixel resolution would need parent bounds).
        const n = parseFloat(tok);
        if (!Number.isFinite(n)) return 0.5;
        return Math.max(0, Math.min(1, n));
        void axis;
    };
    const x = tok2coord(tokens[0] ?? 'center', 'x');
    const y = tok2coord(tokens[1] ?? tokens[0] ?? 'center', 'y');
    return { x, y };
}

// Walk the RN-style transform array. Returns a snapshot with `have*`
// flags so the caller can dispatch only the operations the user
// actually specified (translate dispatch is gated on haveTranslate, etc.).
function _walkTransformArray(arr: ReadonlyArray<unknown>): _TransformSnapshot {
    const snap: _TransformSnapshot = {
        tx: 0, ty: 0,
        rotateDeg: 0,
        scale: 1,
        skewX: 0, skewY: 0,
        haveTranslate: false,
        haveRotate: false,
        haveScale: false,
        haveSkew: false,
    };
    for (const op of arr) {
        if (op == null || typeof op !== 'object') continue;
        const o = op as Record<string, unknown>;
        const keys = Object.keys(o);
        if (keys.length === 0) continue;
        const k = keys[0];
        const v = o[k];
        switch (k) {
            case 'translateX':
                snap.tx = typeof v === 'number' ? v : parseFloat(String(v));
                snap.haveTranslate = true;
                break;
            case 'translateY':
                snap.ty = typeof v === 'number' ? v : parseFloat(String(v));
                snap.haveTranslate = true;
                break;
            case 'rotate':
            case 'rotateZ':
                snap.rotateDeg = _parseAngleDegrees(v);
                snap.haveRotate = true;
                break;
            case 'scale':
                snap.scale = typeof v === 'number' ? v : parseFloat(String(v));
                snap.haveScale = true;
                break;
            case 'scaleX':
            case 'scaleY':
                // Bridge has uniform setScale only; last-write-wins.
                // pulp follow-up will add setScaleXY for independent axes.
                snap.scale = typeof v === 'number' ? v : parseFloat(String(v));
                snap.haveScale = true;
                break;
            // pulp #1434 Triage #9 fan-out — skewX / skewY now reach the
            // bridge via the freshly-registered setSkew(id, x_deg, y_deg).
            // Both axes accumulate independently; one consolidated call
            // emits at dispatch time.
            case 'skewX':
                snap.skewX = _parseAngleDegrees(v);
                snap.haveSkew = true;
                break;
            case 'skewY':
                snap.skewY = _parseAngleDegrees(v);
                snap.haveSkew = true;
                break;
            // 3D / matrix ops — not modeled in pulp's 2D View. Silently
            // drop. pulp follow-up tracks if/when 3D transforms are
            // introduced.
            case 'rotateX':
            case 'rotateY':
            case 'perspective':
            case 'matrix':
                break;
            default:
                // Unknown op — silently drop (matches CSS shim tolerance).
                break;
        }
    }
    return snap;
}

function applyEventHandler(id: string, key: string, value: unknown): void {
    if (typeof value !== 'function') return;
    const eventName = eventNameFor(key);
    if (isHoverEvent(eventName)) {
        // Arm the native hover dispatchers exactly once (idempotent on
        // the bridge — re-registers replace the lambdas, same shape).
        call('registerHover', id);
    }
    if (isPointerEvent(eventName)) {
        // pulp #1381 — without this call the bridge keeps the JS listener
        // in its dispatch table but the View's on_pointer_event callback
        // is never armed, and clicks never fire the React handler.
        // Idempotent on the bridge side (replaces the lambda).
        call('registerPointer', id);
    }
    if (isWheelEvent(eventName)) {
        // pulp #1387 gap #4 — Spectr's zoom-via-onWheel doesn't fire
        // unless we explicitly arm the wheel dispatch path.
        call('registerWheel', id);
    }
    // pulp #1352 — wrap the React handler in a synthetic-event factory so
    // JSX consumers receive a React-DOM-shaped event object (with
    // `currentTarget`, `target`, `preventDefault`, `nativeEvent.rawArgs`,
    // and event-type-specific fields) instead of the bridge's raw
    // positional args (e.g. literal `0` for mouseenter). Without this,
    // idiomatic handlers like
    //   onMouseEnter={e => e.currentTarget.style.background = 'rgba(...)'}
    // crash with `Cannot read property 'style' of undefined`. See the
    // synthetic-event module header for the full surface and the
    // event-type → field-extraction routing.
    const handler = value as (e: unknown) => void;
    call('on', id, eventName, (...rawArgs: unknown[]) => {
        const evt = makeSyntheticEvent(id, eventName, rawArgs);
        handler(evt);
    });
}

/// pulp #1416 — emit one setSvgRect call carrying the full geometry
/// (x, y, width, height). Driven by applyOne when ANY of the four
/// geometry props change so the bridge never sees a partial update
/// that would clobber unset axes back to zero. Reads current values
/// from `props` (the live React props snapshot) and falls back to 0
/// for un-set axes — matches SVG `<rect>` defaults.
function emitSvgRectGeometry(id: string, props: Record<string, unknown>): void {
    const x = typeof props.x === 'number' ? props.x as number : 0;
    const y = typeof props.y === 'number' ? props.y as number : 0;
    const w = typeof props.width === 'number' ? props.width as number : 0;
    const h = typeof props.height === 'number' ? props.height as number : 0;
    call('setSvgRect', id, x, y, w, h);
}

/// pulp #1416 — emit one setSvgLine call carrying the full geometry
/// (x1, y1, x2, y2). Same partial-update guard as emitSvgRectGeometry.
function emitSvgLineGeometry(id: string, props: Record<string, unknown>): void {
    const x1 = typeof props.x1 === 'number' ? props.x1 as number : 0;
    const y1 = typeof props.y1 === 'number' ? props.y1 as number : 0;
    const x2 = typeof props.x2 === 'number' ? props.x2 as number : 0;
    const y2 = typeof props.y2 === 'number' ? props.y2 as number : 0;
    call('setSvgLine', id, x1, y1, x2, y2);
}

/// Apply a single prop to its corresponding bridge setter.
function applyOne(id: string, type: string, key: string, value: unknown, props?: Record<string, unknown>): void {
    if (value === undefined || value === null) {
        // No-op — Pulp has no "unset" for most setters; rely on React
        // unmount + recreate for full clears. Selective resets can be
        // added per-prop here if a regression appears.
        return;
    }

    // pulp #1416 — SvgRect / SvgLine geometry props collide with View
    // flex props (width/height) and event/positioning props (x/y), so
    // dispatch on `type` BEFORE the generic flex routing. The geometry
    // setters are atomic — one bridge call per rect/line carries the
    // full quad of coords — to avoid partial updates clobbering the
    // unset axes back to zero.
    if (type === 'SvgRect') {
        if (key === 'x' || key === 'y' || key === 'width' || key === 'height') {
            if (props) emitSvgRectGeometry(id, props);
            return;
        }
    }
    if (type === 'SvgLine') {
        if (key === 'x1' || key === 'y1' || key === 'x2' || key === 'y2') {
            if (props) emitSvgLineGeometry(id, props);
            return;
        }
    }

    switch (key) {
        // Flex / layout — all forwarded through setFlex
        case 'direction':       return call('setFlex', id, 'direction', value as string);
        case 'gap':             return call('setFlex', id, 'gap', value as number);
        case 'rowGap':          return call('setFlex', id, 'row_gap', value as number);
        case 'columnGap':       return call('setFlex', id, 'column_gap', value as number);
        case 'padding':         return call('setFlex', id, 'padding', value as number);
        // pulp #1434 (cross-surface mega-batch) — per-edge padding accepts
        // either a number (px) or a percent string ('5%' → percent of
        // parent main-axis size). Yoga padding does NOT support 'auto'.
        case 'paddingTop':      return call('setFlex', id, 'padding_top',    value as number | string);
        case 'paddingRight':    return call('setFlex', id, 'padding_right',  value as number | string);
        case 'paddingBottom':   return call('setFlex', id, 'padding_bottom', value as number | string);
        case 'paddingLeft':     return call('setFlex', id, 'padding_left',   value as number | string);
        case 'margin':          return call('setFlex', id, 'margin', value as number);
        // pulp #1434 (cross-surface mega-batch) — per-edge margin accepts
        // a number (px), percent string ('5%'), or the keyword 'auto'
        // (Yoga's YGNodeStyleSetMarginAuto — used for centering with
        // marginLeft:'auto' + marginRight:'auto').
        case 'marginTop':       return call('setFlex', id, 'margin_top',    value as number | string);
        case 'marginRight':     return call('setFlex', id, 'margin_right',  value as number | string);
        case 'marginBottom':    return call('setFlex', id, 'margin_bottom', value as number | string);
        case 'marginLeft':      return call('setFlex', id, 'margin_left',   value as number | string);
        // pulp #1434 batch 4 — React Native shorthand aliases. RN code
        // commonly writes `style={{ marginHorizontal: 8 }}` and expects
        // it to fan out to marginLeft + marginRight on the underlying
        // layout. Same pattern for marginVertical / paddingHorizontal /
        // paddingVertical. We dispatch to the existing per-edge bridge
        // setters so the value reaches the same FlexStyle slot whether
        // it arrived through this alias or through the explicit edge
        // prop. No FlexStyle field change required.
        // pulp #1434 cross-surface mega-batch — RN aliases now forward
        // percent strings (and 'auto' for margin) through the per-edge
        // fan-out. The per-edge keys `margin_*` / `padding_*` accept
        // number | string at the bridge boundary.
        case 'marginHorizontal':
            call('setFlex', id, 'margin_left',  value as number | string);
            call('setFlex', id, 'margin_right', value as number | string);
            return;
        case 'marginVertical':
            call('setFlex', id, 'margin_top',    value as number | string);
            call('setFlex', id, 'margin_bottom', value as number | string);
            return;
        case 'paddingHorizontal':
            call('setFlex', id, 'padding_left',  value as number | string);
            call('setFlex', id, 'padding_right', value as number | string);
            return;
        case 'paddingVertical':
            call('setFlex', id, 'padding_top',    value as number | string);
            call('setFlex', id, 'padding_bottom', value as number | string);
            return;
        case 'flexGrow':        return call('setFlex', id, 'flex_grow', value as number);
        case 'flexShrink':      return call('setFlex', id, 'flex_shrink', value as number);
        // pulp #1434 (#1518) — RN-style `flex: <number>` shorthand.
        // RN spec: `flex: positive` → `{flexGrow: n, flexShrink: 1, flexBasis: 0}`;
        // `flex: 0` → no growth / no shrink at intrinsic basis;
        // `flex: -1` (or any negative) → no growth, can shrink at auto basis.
        // CSS spec is more nuanced (bare number is `flex: <n> 1 0`), but RN's
        // narrow contract is what consumers passing JSX `flex={1}` expect;
        // our adapter is RN-flavored so we honor RN semantics.
        case 'flex': {
            const n = value as number;
            if (typeof n !== 'number' || !Number.isFinite(n)) return;
            if (n > 0) {
                call('setFlex', id, 'flex_grow', n);
                call('setFlex', id, 'flex_shrink', 1);
                call('setFlex', id, 'flex_basis', 0);
            } else if (n === 0) {
                call('setFlex', id, 'flex_grow', 0);
                call('setFlex', id, 'flex_shrink', 0);
                call('setFlex', id, 'flex_basis', 'auto');
            } else {
                call('setFlex', id, 'flex_grow', 0);
                call('setFlex', id, 'flex_shrink', 1);
                call('setFlex', id, 'flex_basis', 'auto');
            }
            return;
        }
        // pulp #1434 (rn batch C) — dimension keys forward
        // `number | string` so the bridge sees `'50%'` / `'auto'`
        // verbatim. Numeric values still flow through unchanged.
        // The bridge's setFlex case for each key inspects the third
        // arg as a string and detects '%' / 'auto' suffix; otherwise
        // it falls back to the numeric path.
        case 'flexBasis':       return call('setFlex', id, 'flex_basis', value as number | string);
        // pulp #1434 Triage #14 — flexWrap accepts boolean (legacy
        // true/false) or the CSS keyword strings (`"wrap"` /
        // `"wrap-reverse"` / `"nowrap"`). Forward strings verbatim
        // so the bridge can route wrap-reverse through Yoga's
        // YGWrapWrapReverse.
        case 'flexWrap': {
            if (typeof value === 'string') {
                return call('setFlex', id, 'flex_wrap', value);
            }
            return call('setFlex', id, 'flex_wrap', value ? 1 : 0);
        }
        case 'order':           return call('setFlex', id, 'order', value as number);
        case 'width':           return call('setFlex', id, 'width', value as number | string);
        case 'height':          return call('setFlex', id, 'height', value as number | string);
        case 'minWidth':        return call('setFlex', id, 'min_width', value as number | string);
        case 'minHeight':       return call('setFlex', id, 'min_height', value as number | string);
        case 'maxWidth':        return call('setFlex', id, 'max_width', value as number | string);
        case 'maxHeight':       return call('setFlex', id, 'max_height', value as number | string);
        case 'alignItems':      return call('setFlex', id, 'align_items', value as string);
        case 'alignSelf':       return call('setFlex', id, 'align_self', value as string);
        // pulp #1434 (sub-agent #12 follow-up) — multi-line flex
        // cross-axis distribution. Yoga supports it natively via
        // YGNodeStyleSetAlignContent; the bridge accepts both bare
        // (`start`/`end`) and prefixed (`flex-start`/`flex-end`)
        // spellings plus the space-* values.
        case 'alignContent':    return call('setFlex', id, 'align_content', value as string);
        case 'justifyContent':  return call('setFlex', id, 'justify_content', value as string);
        // pulp #1434 — aspectRatio routes through setFlex like the other
        // flex props. Accepts a finite positive number (RN-style); strings
        // ("16/9", "auto") are NOT accepted at the JSX surface — those
        // belong to the CSS shim path (web-compat-style-decl.js). A value
        // of 0 / NaN / undefined clears the slot on the bridge side.
        case 'aspectRatio':     return call('setFlex', id, 'aspect_ratio', value as number);

        // Visual style
        case 'background':         return call('setBackground', id, value as string);
        case 'backgroundGradient': return call('setBackgroundGradient', id, value as string);
        // pulp #1517 — background sub-properties. The bridge stores the
        // keyword on the View slot. Paint impact today is partial:
        //   • `backgroundAttachment` — `scroll` is conformant; `fixed` /
        //     `local` are noop (pulp doesn't model scroll contexts).
        //   • `backgroundClip` — `text` is the interesting variant
        //     (paint-time SkBlendMode::kSrcIn against text glyphs);
        //     deferred to a future PR. Other values noop on solid bg.
        //   • `backgroundOrigin` — relevant only for repeating gradients;
        //     noop today.
        case 'backgroundAttachment': return call('setBackgroundAttachment', id, value as string);
        case 'backgroundClip':       return call('setBackgroundClip',       id, value as string);
        case 'backgroundOrigin':     return call('setBackgroundOrigin',     id, value as string);
        case 'border': {
            const b = value as { color: string; width?: number; radius?: number };
            return call('setBorder', id, b.color, b.width ?? 1, b.radius ?? 0);
        }
        // pulp #1027 (audit PR #1166 finding #4) — RN-style flat border props.
        // These MUST route through the per-attribute bridge setters so a
        // commitUpdate that touches only one of them preserves the others.
        // Lowering them onto the unified `setBorder(id, color, width, radius)`
        // would clobber the unset slots back to 0/empty.
        case 'borderColor':  return call('setBorderColor', id, value as string);
        case 'borderWidth':  return call('setBorderWidth', id, value as number);
        case 'borderRadius': return call('setBorderRadius', id, value as number);
        // pulp #1434 Triage #10 — borderStyle keyword passes verbatim
        // to setBorderStyle. Bridge maps to View::BorderStyle. Skia
        // installs the dash effect for `dashed` / `dotted`; other
        // named styles currently degrade to solid.
        case 'borderStyle':  return call('setBorderStyle', id, value as string);
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
            return;
        }
        case 'listStyleType':     return call('setListStyleType', id, value as string);
        case 'listStyleImage':    return call('setListStyleImage', id, value as string);
        case 'listStylePosition': return call('setListStylePosition', id, value as string);
        case 'borderTop':    { const b = value as { color: string; width: number }; return call('setBorderSide', id, 'top', b.width, b.color); }
        case 'borderRight':  { const b = value as { color: string; width: number }; return call('setBorderSide', id, 'right', b.width, b.color); }
        case 'borderBottom': { const b = value as { color: string; width: number }; return call('setBorderSide', id, 'bottom', b.width, b.color); }
        case 'borderLeft':   { const b = value as { color: string; width: number }; return call('setBorderSide', id, 'left', b.width, b.color); }
        // RN per-side flat props — route to the per-side bridge setters
        // that already preserve the unrelated attribute (see widget_bridge
        // applyBorderSide helper introduced in pulp #1026).
        case 'borderTopColor':       return call('setBorderTopColor', id, value as string);
        case 'borderRightColor':     return call('setBorderRightColor', id, value as string);
        case 'borderBottomColor':    return call('setBorderBottomColor', id, value as string);
        case 'borderLeftColor':      return call('setBorderLeftColor', id, value as string);
        case 'borderTopWidth':       return call('setBorderTopWidth', id, value as number);
        case 'borderRightWidth':     return call('setBorderRightWidth', id, value as number);
        case 'borderBottomWidth':    return call('setBorderBottomWidth', id, value as number);
        case 'borderLeftWidth':      return call('setBorderLeftWidth', id, value as number);
        case 'borderTopLeftRadius':     return call('setBorderTopLeftRadius', id, value as number);
        case 'borderTopRightRadius':    return call('setBorderTopRightRadius', id, value as number);
        case 'borderBottomLeftRadius':  return call('setBorderBottomLeftRadius', id, value as number);
        case 'borderBottomRightRadius': return call('setBorderBottomRightRadius', id, value as number);
        // pulp #1519 — RN outline cluster. Paint-time ring drawn OUTSIDE
        // the border-box (no Yoga layout impact). Each prop routes to its
        // own per-attribute bridge fn so a JSX prop diff that touches one
        // outline-* preserves the others. Style keyword set mirrors
        // borderStyle (CSS spec is identical).
        case 'outlineColor':  return call('setOutlineColor',  id, value as string);
        case 'outlineOffset': return call('setOutlineOffset', id, value as number);
        case 'outlineStyle':  return call('setOutlineStyle',  id, value as string);
        case 'outlineWidth':  return call('setOutlineWidth',  id, value as number);
        case 'opacity':      return call('setOpacity', id, value as number);
        case 'visible':      return call('setVisible', id, value as boolean);
        // pulp #1434 (rn batch — Triage #12) — `display: 'flex' | 'none'`.
        // RN exports + Figma / v0 / Claude Design HTML routinely emit
        // `style={{ display: 'flex' }}` (the implicit default in pulp,
        // but the prop-applier shouldn't drop it as unknown) or
        // `style={{ display: 'none' }}` to hide a subtree. The yoga
        // surface wired this for the CSS shim in #1422; this branch
        // makes the same path reachable from RN-flavored JSX without a
        // round-trip through the el.style proxy.
        //
        // 'none'  → setVisible(id, false). View::visible() is the
        //           canonical "skip render + don't lay out" signal.
        // 'flex'  → setVisible(id, true). Yoga's flex layout is pulp's
        //           default; explicit 'flex' just confirms it.
        // Anything else (block / inline-block / inline-flex / grid)
        // is silently ignored at this layer — the CSS shim handles
        // those for the el.style path; for RN consumers, the typical
        // emission is just 'flex' / 'none'.
        case 'display': {
            const sval = String(value);
            if (sval === 'none') return call('setVisible', id, false);
            if (sval === 'flex') return call('setVisible', id, true);
            return; // unknown display value — leave View at current visibility
        }
        // pulp #1434 (Triage #15) — `boxShadow` accepts:
        //  • `null` / `undefined` / `'none'` → clearBoxShadow
        //  • String form (`'2px 4px 8px rgba(0,0,0,0.3)'` with optional
        //    `inset`) — parsed inline below.
        //  • Object form `{ offsetX, offsetY, blur?, spread?, color,
        //    inset? }` — dispatched directly.
        // Multi-shadow comma-separated lists are deferred.
        case 'boxShadow': {
            if (value == null || value === 'none' || value === '') {
                return call('clearBoxShadow', id);
            }
            if (typeof value === 'object') {
                const s = value as { offsetX: number; offsetY: number; blur?: number; spread?: number; color: string; inset?: boolean };
                return call('setBoxShadow', id, s.offsetX, s.offsetY,
                            s.blur ?? 4, s.spread ?? 0,
                            s.color, !!s.inset);
            }
            if (typeof value === 'string') {
                const parsed = _parseBoxShadow(value);
                if (parsed) {
                    return call('setBoxShadow', id, parsed.offsetX, parsed.offsetY,
                                parsed.blur, parsed.spread, parsed.color, parsed.inset);
                }
                return; // unparseable — silently drop (matches CSS shim behavior)
            }
            return;
        }
        // pulp #1387 gap #1 — overflow was reachable via the DOM-lite
        // path (web-compat-style-decl.js routes 'overflow' to setOverflow)
        // but missing from the @pulp/react prop-applier, so JSX consumers
        // setting `style={{ overflow: 'hidden' }}` silently dropped it.
        // Spectr's dropdowns hit this — `width: 230 + overflow: hidden`
        // on the dropdown row was being discarded, so the row grew to
        // intrinsic content width and overflowed the container.
        // Accepts the CSS keyword strings ('hidden' / 'visible' /
        // 'scroll' / 'auto'); bridge maps to View::Overflow enum.
        case 'overflow':     return call('setOverflow', id, value as string);
        // pulp #1516 — CSS box-sizing. Yoga 3.x honors the spec via
        // YGNodeStyleSetBoxSizing; consumers passing JSX
        // `boxSizing: 'border-box'` get the standard
        // "padding+border are inside declared dimensions" behavior.
        // Web designs almost universally reset to `border-box`.
        case 'boxSizing':    return call('setBoxSizing', id, value as string);

        // pulp #1434 Phase A2-1 — CSS transitions. The bridge parses
        // the full shorthand into a list of TransitionSpecs that the
        // dispatcher (PR 2 of the ladder) consults when a property
        // changes. Longhand fields apply uniformly across the parsed
        // list (CSS spec semantics).
        case 'transition':                return call('setTransition', id, value as string);
        case 'transitionProperty':        return call('setTransitionProperty', id, value as string);
        case 'transitionDuration': {
            // Accept "200ms" / "0.3s" string OR plain number-of-seconds.
            if (typeof value === 'number') {
                return call('setTransitionDuration', id, value);
            }
            const s = String(value).trim();
            const ms = s.endsWith('ms');
            const n = parseFloat(s);
            return call('setTransitionDuration', id, ms ? n / 1000 : n);
        }
        case 'transitionDelay': {
            if (typeof value === 'number') {
                return call('setTransitionDelay', id, value);
            }
            const s = String(value).trim();
            const ms = s.endsWith('ms');
            const n = parseFloat(s);
            return call('setTransitionDelay', id, ms ? n / 1000 : n);
        }
        case 'transitionTimingFunction':  return call('setTransitionTimingFunction', id, value as string);

        // pulp #1434 Phase A2-1 — CSS animations. animation-name
        // resolves through the keyframes registry populated by
        // defineKeyframes; PR 4 wires the playback driver. The
        // shorthand path takes a single name + duration; longhand
        // props can be split out by the host as needed.
        case 'animationName':   return call('setAnimation', id, value as string, 1.0, 1, 'normal');
        // Codex audit on pulp #1508 (P1): animationDuration was
        // dispatched to setTransitionDuration here, which mutated
        // *transition* timing on the same View instead of *animation*
        // timing. Route through the legacy 2-arg setAnimation control-
        // token form (`setAnimation(id, 'duration', seconds)`) so the
        // bridge stages it on the View's pending-animation slot
        // alongside animationName / animationDelay / etc.
        case 'animationDuration': {
            if (typeof value === 'number') {
                return call('setAnimation', id, 'duration', value);
            }
            const s = String(value).trim();
            const ms = s.endsWith('ms');
            const n = parseFloat(s);
            return call('setAnimation', id, 'duration', ms ? n / 1000 : n);
        }
        case 'animationDelay': {
            if (typeof value === 'number') {
                return call('setAnimation', id, 'delay', value);
            }
            const s = String(value).trim();
            const ms = s.endsWith('ms');
            const n = parseFloat(s);
            return call('setAnimation', id, 'delay', ms ? n / 1000 : n);
        }
        case 'animationTimingFunction':
            return call('setAnimation', id, 'easing', value as string);
        case 'animationIterationCount':
            return call('setAnimation', id, 'iterations',
                value === 'infinite' ? -1 : (typeof value === 'number' ? value : parseFloat(String(value)) || 1));
        case 'animationDirection':
            return call('setAnimation', id, 'direction', value as string);
        case 'animationFillMode':
            return call('setAnimation', id, 'fill', value as string);

        // pulp #1434 rn logical-edge bundle (sub-agent #27 finding) —
        // RN's CSS-spec-equivalent logical-flow props. LTR-only fast
        // path: Start → Left, End → Right, inset shorthand expands to
        // top/right/bottom/left, insetBlock → top+bottom,
        // insetInline → left+right (LTR). True RTL bidi requires a
        // future direction system — tracked as a separate big project.
        // The 11 entries here close the missing-on-rn gap with the
        // honest LTR-only caveat documented in the catalog.
        case 'marginStart': {
            return call('setFlex', id, 'margin_left', value as number | string);
        }
        case 'marginEnd': {
            return call('setFlex', id, 'margin_right', value as number | string);
        }
        case 'paddingStart': {
            return call('setFlex', id, 'padding_left', value as number | string);
        }
        case 'paddingEnd': {
            return call('setFlex', id, 'padding_right', value as number | string);
        }
        case 'borderStartWidth': {
            // Routes to the per-side bridge setter that preserves the
            // unrelated attribute (color) — same pattern as borderLeftWidth.
            return call('setBorderLeftWidth', id, value as number);
        }
        case 'borderEndWidth': {
            return call('setBorderRightWidth', id, value as number);
        }
        case 'start': {
            return call('setLeft', id, value as number | string);
        }
        case 'end': {
            return call('setRight', id, value as number | string);
        }
        // CSS `inset` shorthand: 1 / 2 / 3 / 4 values fan out to
        // top / right / bottom / left (CSS spec — same expansion as
        // `margin` / `padding` shorthands). Numeric or percent strings.
        case 'inset': {
            const v = value as number | string;
            if (typeof v === 'number') {
                call('setTop',    id, v);
                call('setRight',  id, v);
                call('setBottom', id, v);
                call('setLeft',   id, v);
                return;
            }
            const tokens = String(v).trim().split(/\s+/);
            const t = tokens[0] ?? 0;
            const r = tokens[1] ?? t;
            const b = tokens[2] ?? t;
            const l = tokens[3] ?? r;
            // Each token may be a number or a percent string — forward
            // verbatim so the bridge can route through Yoga's
            // YGNodeStyleSetPositionPercent path for percent values.
            const coerce = (tok: string | number) => {
                if (typeof tok === 'number') return tok;
                if (tok.endsWith('%')) return tok;
                const n = parseFloat(tok);
                return Number.isFinite(n) ? n : tok;
            };
            call('setTop',    id, coerce(t));
            call('setRight',  id, coerce(r));
            call('setBottom', id, coerce(b));
            call('setLeft',   id, coerce(l));
            return;
        }
        case 'insetBlock': {
            // CSS insetBlock shorthand → top + bottom.
            const v = value as number | string;
            call('setTop',    id, v);
            call('setBottom', id, v);
            return;
        }
        case 'insetInline': {
            // CSS insetInline shorthand → left + right (LTR).
            const v = value as number | string;
            call('setLeft',  id, v);
            call('setRight', id, v);
            return;
        }

        // pulp #1434 Phase A2-2 — CSS Grid surface. Forwards each
        // property verbatim to setGrid; the C++ bridge handles
        // template-track parsing, named-area parsing, and the
        // grid-area shorthand (named token vs `row / col / row / col`
        // numeric form).
        case 'gridTemplateColumns': return call('setGrid', id, 'template_columns', value as string);
        case 'gridTemplateRows':    return call('setGrid', id, 'template_rows',    value as string);
        case 'gridTemplateAreas':   return call('setGrid', id, 'template_areas',   value as string);
        case 'gridAutoColumns':     return call('setGrid', id, 'auto_columns',     value as string);
        case 'gridAutoRows':        return call('setGrid', id, 'auto_rows',        value as string);
        case 'gridAutoFlow':        return call('setGrid', id, 'auto_flow',        value as string);
        case 'gridArea':            return call('setGrid', id, 'grid_area',        value as string);
        case 'gridColumn': {
            const parts = String(value).split('/').map((s) => parseInt(s.trim(), 10));
            if (parts[0]) call('setGrid', id, 'column_start', parts[0]);
            if (parts[1]) call('setGrid', id, 'column_end', parts[1]);
            return;
        }
        case 'gridRow': {
            const parts = String(value).split('/').map((s) => parseInt(s.trim(), 10));
            if (parts[0]) call('setGrid', id, 'row_start', parts[0]);
            if (parts[1]) call('setGrid', id, 'row_end', parts[1]);
            return;
        }
        case 'gridColumnStart': return call('setGrid', id, 'column_start', value as number);
        case 'gridColumnEnd':   return call('setGrid', id, 'column_end',   value as number);
        case 'gridRowStart':    return call('setGrid', id, 'row_start',    value as number);
        case 'gridRowEnd':      return call('setGrid', id, 'row_end',      value as number);
        case 'gridGap':         return call('setGrid', id, 'gap',          value as number);
        case 'gridColumnGap':   return call('setGrid', id, 'column_gap',   value as number);
        case 'gridRowGap':      return call('setGrid', id, 'row_gap',      value as number);

        // pulp #1434 rn bridge-wires bundle (sub-agent #27 finding) —
        // 7 RN-style props that already had C++ bridge fns registered
        // but no `@pulp/react` prop-applier dispatch. Each forwards
        // the keyword / string straight through to the matching setter.
        case 'backfaceVisibility': return call('setBackfaceVisibility', id, value as string);
        case 'cursor':             return call('setCursor', id, value as string);
        case 'filter':             return call('setFilter', id, value as string);
        case 'pointerEvents':      return call('setPointerEvents', id, value as string);
        case 'textTransform':      return call('setTextTransform', id, value as string);
        case 'userSelect':         return call('setUserSelect', id, value as string);
        // transformOrigin accepts CSS strings of the form `'NN% NN%'`,
        // `'NNpx NNpx'`, `'center'`, or two keyword tokens. The bridge
        // wants two numeric fractions (0..1). Defaults to 0.5/0.5
        // (center) when a token is unrecognized — matches CSS default.
        case 'transformOrigin': {
            const parsed = _parseTransformOrigin(String(value ?? 'center'));
            return call('setTransformOrigin', id, parsed.x, parsed.y);
        }

        // pulp #1434 Triage #9 — RN array transform.
        // RN's transform is an array of single-property objects:
        //   transform: [
        //     { translateX: 10 }, { translateY: 20 },
        //     { rotate: '45deg' }, { scale: 1.5 },
        //   ]
        // Walk-once accumulates the snapshot in one pass (so
        // {translateX:10} and {translateY:20} as separate entries
        // produce ONE setTranslate(10,20), not two clobbering calls),
        // then emits only the operations the user specified.
        // Within-array semantics: each render's array is a complete
        // description; absent fields reset to identity. No cross-
        // render state is maintained — passing `transform: undefined`
        // (or removing the prop) goes through the standard prop-
        // removal path and resets translate/rotate/scale on the next
        // re-render that includes the prop.
        case 'transform': {
            if (value == null) return;
            if (!Array.isArray(value)) return;  // CSS string form deferred
            const snap = _walkTransformArray(value as ReadonlyArray<unknown>);
            if (snap.haveTranslate) call('setTranslate', id, snap.tx, snap.ty);
            if (snap.haveRotate)    call('setRotation', id, snap.rotateDeg);
            if (snap.haveScale)     call('setScale', id, snap.scale);
            // pulp #1434 Triage #9 fan-out — setSkew is now a registered
            // bridge fn; emit one consolidated call that captures both
            // axes accumulated in the walker.
            if (snap.haveSkew)      call('setSkew', id, snap.skewX, snap.skewY);
            return;
        }

        // CSS-style positioning (pulp #779 follow-up; matches setPosition
        // + setTop/setLeft/setRight/setBottom on the bridge).
        // pulp #1434 batch 6 — top/right/bottom/left accept either a number
        // ('50' → px) or a percent string ('50%' → percent of parent).
        // Mirrors PR #1426 (width/height percent) for the four View
        // positional fields. Figma absolute-positioned overlays, v0.dev
        // hero anchors, and Claude Design sticky elements all emit
        // `top:'50%'` etc. routinely; without percent forwarding the
        // layout collapses to numeric 0 silently. The bridge inspects
        // arg index 1 as a string, detects the '%' suffix, and routes to
        // Yoga's YGNodeStyleSetPositionPercent path via View::top_unit_.
        case 'position':     return call('setPosition', id, value as string);
        case 'top':          return call('setTop', id, value as number | string);
        case 'left':         return call('setLeft', id, value as number | string);
        case 'right':        return call('setRight', id, value as number | string);
        case 'bottom':       return call('setBottom', id, value as number | string);
        case 'zIndex':       return call('setZIndex', id, value as number);

        // Text
        case 'text':            return call('setText', id, String(value));
        case 'textColor':       return call('setTextColor', id, value as string);
        // pulp #1434 — widen to include `'auto'` and `'justify'` (CSS /
        // RN canonical). `'auto'` is writing-direction-relative
        // (LTR-only today, degrades to `'left'`); `'justify'` flows to
        // canvas TextAlign::justify (SkParagraph kJustify wiring is a
        // follow-up — backends approximate as left for now).
        case 'textAlign':       return call('setTextAlign', id, value as string);
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
        case 'fontFamily':      return call('setFontFamily', id, value as string);
        case 'fontSize':        return call('setFontSize', id, value as number);
        case 'fontWeight':      return call('setFontWeight', id, _normalizeFontWeight(value));
        case 'fontStyle':       return call('setFontStyle', id, value as string);
        case 'letterSpacing':   return call('setLetterSpacing', id, value as number);
        case 'lineHeight':      return call('setLineHeight', id, value as number);
        // pulp #1552 — line-clamp + webkit-line-clamp + background-repeat
        // wiring. Both line-clamp keys funnel through the same setter
        // (shared CSS shim case + RN-style alias). 0 / non-finite clears
        // the slot. backgroundRepeat is storage-only at the View layer
        // (paint-time honoring is a follow-up for url() / repeating
        // gradient backgrounds).
        case 'lineClamp':
        case 'webkitLineClamp': {
            const n = typeof value === 'number' ? value : parseInt(String(value), 10);
            return call('setLineClamp', id, Number.isFinite(n) ? n : 0);
        }
        case 'backgroundRepeat': return call('setBackgroundRepeat', id, value as string);

        // Widget-specific data
        case 'data':
            // <Spectrum data={...}> + <Waveform data={...}> share the prop
            // name; we route based on widget type at the call site.
            if (type === 'Spectrum') return call('setSpectrumData', id, value as number[] | Float32Array);
            if (type === 'Waveform') return call('setWaveformData', id, value as number[] | Float32Array);
            return;
        case 'level':    return call('setMeterLevel', id, value as number);
        case 'value':
            // Type-aware routing — bridge has separate setters per
            // widget type. Codex P2 review on PR #779: setValue only
            // handles knob/fader/toggle/checkbox; Progress wants
            // setProgress, Spectrum/Waveform want setSpectrumData /
            // setWaveformData (handled via 'data' prop already).
            if (type === 'Progress')   return call('setProgress', id, value as number);
            if (type === 'Meter')      return call('setMeterLevel', id, value as number);
            return call('setValue', id, value as number);

        // pulp #1148 — generalized overlay-click routing. `overlay={true}`
        // claims the view as the active click-eligible overlay so React
        // popovers built on `<View position="absolute">` receive clicks
        // even though hit_test would otherwise resolve to a sibling. The
        // matching releaseOverlay is emitted by applyChangedProps when
        // the prop flips off, and by detach() at unmount.
        case 'overlay':
            if (value) return call('claimOverlay', id);
            return call('releaseOverlay', id);

        // SvgPath (pulp #994) — wires the SvgPathWidget bridge surface
        // (createSvgPath / setSvgPath / setSvgViewBox / setSvgFill /
        // setSvgStroke / setSvgStrokeWidth) through a typed JSX intrinsic.
        case 'd':            return call('setSvgPath', id, value as string);
        case 'viewBox': {
            const vb = value as [number, number];
            if (Array.isArray(vb) && vb.length >= 2) {
                return call('setSvgViewBox', id, vb[0], vb[1]);
            }
            return;
        }
        case 'fill':         return call('setSvgFill', id, value as string);
        case 'stroke':       return call('setSvgStroke', id, value as string);
        case 'strokeWidth':  return call('setSvgStrokeWidth', id, value as number);

        default:
            // Unknown prop — silently ignore. We could warn here in DEV
            // builds, but staying chatty in prod is annoying. The
            // typed JSX intrinsics in types.ts already gate this.
            return;
    }
}

/// Apply ALL props for first-attach. Called from appendInitialChild /
/// appendChild after the instance lands on the bridge.
export function applyAllProps(instance: PulpInstance): void {
    const { id, type, props } = instance;
    logApply('applyAll', id, type, Object.keys(props).length);
    // pulp #1416 — for SvgRect / SvgLine, emit the geometry call once
    // up-front from the full props snapshot, then skip the per-prop
    // dispatch for the four geometry keys (otherwise we'd issue four
    // identical setSvgRect / setSvgLine calls during the loop).
    let svgGeometryEmitted = false;
    if (type === 'SvgRect') {
        if (('x' in props) || ('y' in props) || ('width' in props) || ('height' in props)) {
            emitSvgRectGeometry(id, props);
            svgGeometryEmitted = true;
        }
    } else if (type === 'SvgLine') {
        if (('x1' in props) || ('y1' in props) || ('x2' in props) || ('y2' in props)) {
            emitSvgLineGeometry(id, props);
            svgGeometryEmitted = true;
        }
    }
    for (const key of Object.keys(props)) {
        if (isReactInternal(key)) continue;
        if (key === 'children') continue;  // text children handled by caller
        if (isEventHandler(key)) {
            applyEventHandler(id, key, props[key]);
            continue;
        }
        if (svgGeometryEmitted) {
            if (type === 'SvgRect' && (key === 'x' || key === 'y' || key === 'width' || key === 'height')) continue;
            if (type === 'SvgLine' && (key === 'x1' || key === 'y1' || key === 'x2' || key === 'y2')) continue;
        }
        applyOne(id, type, key, props[key], props);
    }
}

/// Apply ONLY changed props for commitUpdate.
/// Returns true if any setter was called (caller can use for repaint hints).
export function applyChangedProps(
    instance: PulpInstance,
    oldProps: Record<string, unknown>,
    newProps: Record<string, unknown>,
): boolean {
    const { id, type } = instance;
    let mutated = false;

    // pulp #1416 — coalesce SvgRect / SvgLine geometry changes into a
    // single setSvgRect / setSvgLine call sourced from the post-update
    // props snapshot. Without this, four separate prop diffs would each
    // try to emit a partial geometry update.
    const svgRectGeoChanged = type === 'SvgRect' && (
        oldProps.x !== newProps.x ||
        oldProps.y !== newProps.y ||
        oldProps.width !== newProps.width ||
        oldProps.height !== newProps.height
    );
    const svgLineGeoChanged = type === 'SvgLine' && (
        oldProps.x1 !== newProps.x1 ||
        oldProps.y1 !== newProps.y1 ||
        oldProps.x2 !== newProps.x2 ||
        oldProps.y2 !== newProps.y2
    );
    if (svgRectGeoChanged) {
        emitSvgRectGeometry(id, newProps);
        mutated = true;
    }
    if (svgLineGeoChanged) {
        emitSvgLineGeometry(id, newProps);
        mutated = true;
    }

    // Walk new props — set anything that changed value
    for (const key of Object.keys(newProps)) {
        if (isReactInternal(key)) continue;
        if (key === 'children') continue;
        // SvgRect / SvgLine geometry already emitted above; skip the
        // per-prop dispatch so we don't double-fire bridge calls.
        if (svgRectGeoChanged && type === 'SvgRect' && (key === 'x' || key === 'y' || key === 'width' || key === 'height')) continue;
        if (svgLineGeoChanged && type === 'SvgLine' && (key === 'x1' || key === 'y1' || key === 'x2' || key === 'y2')) continue;
        if (oldProps[key] !== newProps[key]) {
            if (isEventHandler(key)) {
                applyEventHandler(id, key, newProps[key]);
            } else {
                applyOne(id, type, key, newProps[key], newProps);
            }
            mutated = true;
        }
    }

    // Walk old props — clear anything that disappeared (best-effort —
    // most setters have no inverse; visible/opacity are obvious cases)
    for (const key of Object.keys(oldProps)) {
        if (isReactInternal(key)) continue;
        if (key === 'children') continue;
        if (!(key in newProps)) {
            // Specific resets we can do meaningfully
            if (key === 'visible')  { call('setVisible', id, true); mutated = true; }
            if (key === 'opacity')  { call('setOpacity', id, 1.0); mutated = true; }
            // pulp #1148 — overlay flipped off (or simply removed from
            // props) must release the global overlay slot, otherwise the
            // platform host keeps routing clicks to a stale popover.
            if (key === 'overlay' && oldProps[key]) {
                call('releaseOverlay', id);
                mutated = true;
            }
            // Other setters: no-op — let the next mount cycle handle it
        }
    }

    return mutated;
}
