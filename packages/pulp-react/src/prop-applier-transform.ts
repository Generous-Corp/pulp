// prop-applier-transform — transform / transform-origin plus CSS
// transition + animation timing props.
//
// `applyTransformProp(id, key, value)` returns true if it handled the
// key, false otherwise. Transitions and animations are grouped here
// because they describe time-based mutation of mostly transform and
// paint state.

import { call } from './prop-applier-internal.js';

// RN's `transform` is an array of single-property objects
// (`[{translateX:10},{rotate:'45deg'},{scale:1.5}]`). The bridge has
// setTranslate / setRotation / setScale (uniform), so the array-walker
// accumulates a per-render snapshot inside one pass and emits consolidated
// calls. Within-array merging means
// `[{translateX:10},{translateY:20}]` produces ONE setTranslate(10,20)
// instead of two clobbering ones (the latter would zero the unrelated
// axis on each call). RN semantics also say each render's array is a
// complete description — absent fields reset to identity, so we don't
// carry state across renders.
//
// Current 2D bridge limits:
//   • setScale is uniform-only; independent scaleX/scaleY axes can't
//     round-trip. We approximate: scale > scaleX > scaleY in priority,
//     last-write-wins within the array; if scaleX≠scaleY we emit the
//     last seen and document the limitation.
//   • rotateX/rotateY/perspective/matrix3d: 3D / matrix transforms not
//     modeled in pulp's 2D View (no perspective; rotation is Z-axis
//     only). Silently dropped.
//   • matrix(a b c d tx ty): 2D affine; the CSS shim decomposes to
//     translate + uniform-scale + rotate components. The @pulp/react
//     RN array surface doesn't have a matrix entry today (RN spec:
//     only translateX/Y, scale, scaleX/Y, rotate/Z, skewX/Y), so the
//     walker just silently drops `matrix`/`matrix3d` for parity.
//
// `setSkew` is a registered bridge function. The walker dispatches
// skewX/skewY by accumulating both axes and emitting one consolidated
// setSkew(id, x_deg, y_deg) call.
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

// Parse `'45deg'` / `'0.785rad'` / `45` (numeric) to degrees.
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

// Parse a CSS transform-origin string into two fractional coordinates
// (0..1) the bridge expects.
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
                // Bridge has uniform setScale only; last-write-wins keeps
                // independent-axis input deterministic until the bridge has
                // separate scale axes.
                snap.scale = typeof v === 'number' ? v : parseFloat(String(v));
                snap.haveScale = true;
                break;
            // skewX / skewY reach the bridge through setSkew(id, x_deg, y_deg).
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
            // 3D / matrix ops are not modeled in pulp's 2D View, so this
            // surface silently drops them.
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

/// Apply a transform / transition / animation prop. Returns true if handled.
export function applyTransformProp(
    id: string,
    key: string,
    value: unknown,
): boolean {
    switch (key) {
        // transformOrigin accepts CSS strings of the form `'NN% NN%'`,
        // `'NNpx NNpx'`, `'center'`, or two keyword tokens. The bridge
        // wants two numeric fractions (0..1). Defaults to 0.5/0.5
        // (center) when a token is unrecognized — matches CSS default.
        case 'transformOrigin': {
            const parsed = _parseTransformOrigin(String(value ?? 'center'));
            call('setTransformOrigin', id, parsed.x, parsed.y);
            return true;
        }

        // RN array transform. RN's transform is an array of single-property
        // objects:
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
            if (value == null) return true;
            if (!Array.isArray(value)) return true;  // CSS string form deferred
            const snap = _walkTransformArray(value as ReadonlyArray<unknown>);
            if (snap.haveTranslate) call('setTranslate', id, snap.tx, snap.ty);
            if (snap.haveRotate)    call('setRotation', id, snap.rotateDeg);
            if (snap.haveScale)     call('setScale', id, snap.scale);
            // Emit one consolidated bridge call that captures both skew axes
            // accumulated in the walker.
            if (snap.haveSkew)      call('setSkew', id, snap.skewX, snap.skewY);
            return true;
        }

        // CSS transitions. The bridge parses the full shorthand into a list
        // of TransitionSpecs that the dispatcher consults when a property
        // changes. Longhand fields apply uniformly across the parsed list
        // (CSS spec semantics).
        case 'transition':                call('setTransition', id, value as string); return true;
        case 'transitionProperty':        call('setTransitionProperty', id, value as string); return true;
        case 'transitionDuration': {
            // Accept "200ms" / "0.3s" string OR plain number-of-seconds.
            if (typeof value === 'number') {
                call('setTransitionDuration', id, value);
                return true;
            }
            const s = String(value).trim();
            const ms = s.endsWith('ms');
            const n = parseFloat(s);
            call('setTransitionDuration', id, ms ? n / 1000 : n);
            return true;
        }
        case 'transitionDelay': {
            if (typeof value === 'number') {
                call('setTransitionDelay', id, value);
                return true;
            }
            const s = String(value).trim();
            const ms = s.endsWith('ms');
            const n = parseFloat(s);
            call('setTransitionDelay', id, ms ? n / 1000 : n);
            return true;
        }
        case 'transitionTimingFunction':  call('setTransitionTimingFunction', id, value as string); return true;

        // CSS animations. animation-name resolves through the keyframes
        // registry populated by defineKeyframes. The shorthand path takes a
        // single name + duration; longhand props can be split out by the host
        // as needed.
        case 'animationName':   call('setAnimation', id, value as string, 1.0, 1, 'normal'); return true;
        // animationDuration must route through animation timing, not
        // transition timing. The legacy 2-arg setAnimation control-token
        // form stages it on the View's pending-animation slot alongside
        // animationName / animationDelay / etc.
        case 'animationDuration': {
            if (typeof value === 'number') {
                call('setAnimation', id, 'duration', value);
                return true;
            }
            const s = String(value).trim();
            const ms = s.endsWith('ms');
            const n = parseFloat(s);
            call('setAnimation', id, 'duration', ms ? n / 1000 : n);
            return true;
        }
        case 'animationDelay': {
            if (typeof value === 'number') {
                call('setAnimation', id, 'delay', value);
                return true;
            }
            const s = String(value).trim();
            const ms = s.endsWith('ms');
            const n = parseFloat(s);
            call('setAnimation', id, 'delay', ms ? n / 1000 : n);
            return true;
        }
        case 'animationTimingFunction':
            call('setAnimation', id, 'easing', value as string);
            return true;
        case 'animationIterationCount':
            call('setAnimation', id, 'iterations',
                value === 'infinite' ? -1 : (typeof value === 'number' ? value : parseFloat(String(value)) || 1));
            return true;
        case 'animationDirection':
            call('setAnimation', id, 'direction', value as string);
            return true;
        case 'animationFillMode':
            call('setAnimation', id, 'fill', value as string);
            return true;
        // animation-play-state routes through the legacy 2-arg setAnimation
        // control-token form so the bridge stores the keyword on
        // View::animation_play_state_; View::tick_animations honors `paused`
        // by skipping the timeline advance (web spec semantic).
        case 'animationPlayState':
            call('setAnimation', id, 'play_state', value as string);
            return true;

        default:
            return false;
    }
}
