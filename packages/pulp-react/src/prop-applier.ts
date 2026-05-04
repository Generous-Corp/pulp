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

function applyEventHandler(id: string, key: string, value: unknown): void {
    if (typeof value !== 'function') return;
    const eventName = eventNameFor(key);
    if (isHoverEvent(eventName)) {
        // Arm the native hover dispatchers exactly once (idempotent on
        // the bridge — re-registers replace the lambdas, same shape).
        call('registerHover', id);
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

/// Apply a single prop to its corresponding bridge setter.
function applyOne(id: string, type: string, key: string, value: unknown): void {
    if (value === undefined || value === null) {
        // No-op — Pulp has no "unset" for most setters; rely on React
        // unmount + recreate for full clears. Selective resets can be
        // added per-prop here if a regression appears.
        return;
    }

    switch (key) {
        // Flex / layout — all forwarded through setFlex
        case 'direction':       return call('setFlex', id, 'direction', value as string);
        case 'gap':             return call('setFlex', id, 'gap', value as number);
        case 'rowGap':          return call('setFlex', id, 'row_gap', value as number);
        case 'columnGap':       return call('setFlex', id, 'column_gap', value as number);
        case 'padding':         return call('setFlex', id, 'padding', value as number);
        case 'paddingTop':      return call('setFlex', id, 'padding_top', value as number);
        case 'paddingRight':    return call('setFlex', id, 'padding_right', value as number);
        case 'paddingBottom':   return call('setFlex', id, 'padding_bottom', value as number);
        case 'paddingLeft':     return call('setFlex', id, 'padding_left', value as number);
        case 'margin':          return call('setFlex', id, 'margin', value as number);
        case 'marginTop':       return call('setFlex', id, 'margin_top', value as number);
        case 'marginRight':     return call('setFlex', id, 'margin_right', value as number);
        case 'marginBottom':    return call('setFlex', id, 'margin_bottom', value as number);
        case 'marginLeft':      return call('setFlex', id, 'margin_left', value as number);
        case 'flexGrow':        return call('setFlex', id, 'flex_grow', value as number);
        case 'flexShrink':      return call('setFlex', id, 'flex_shrink', value as number);
        case 'flexBasis':       return call('setFlex', id, 'flex_basis', value as number);
        case 'flexWrap':        return call('setFlex', id, 'flex_wrap', value ? 1 : 0);
        case 'order':           return call('setFlex', id, 'order', value as number);
        case 'width':           return call('setFlex', id, 'width', value as number);
        case 'height':          return call('setFlex', id, 'height', value as number);
        case 'minWidth':        return call('setFlex', id, 'min_width', value as number);
        case 'minHeight':       return call('setFlex', id, 'min_height', value as number);
        case 'maxWidth':        return call('setFlex', id, 'max_width', value as number);
        case 'maxHeight':       return call('setFlex', id, 'max_height', value as number);
        case 'alignItems':      return call('setFlex', id, 'align_items', value as string);
        case 'alignSelf':       return call('setFlex', id, 'align_self', value as string);
        case 'justifyContent':  return call('setFlex', id, 'justify_content', value as string);

        // Visual style
        case 'background':         return call('setBackground', id, value as string);
        case 'backgroundGradient': return call('setBackgroundGradient', id, value as string);
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
        case 'opacity':      return call('setOpacity', id, value as number);
        case 'visible':      return call('setVisible', id, value as boolean);

        // CSS-style positioning (pulp #779 follow-up; matches setPosition
        // + setTop/setLeft/setRight/setBottom on the bridge).
        case 'position':     return call('setPosition', id, value as string);
        case 'top':          return call('setTop', id, value as number);
        case 'left':         return call('setLeft', id, value as number);
        case 'right':        return call('setRight', id, value as number);
        case 'bottom':       return call('setBottom', id, value as number);
        case 'zIndex':       return call('setZIndex', id, value as number);

        // Text
        case 'text':            return call('setText', id, String(value));
        case 'textColor':       return call('setTextColor', id, value as string);
        case 'textAlign':       return call('setTextAlign', id, value as 'left' | 'center' | 'right');
        // Typography — Label widgets honor these via setX bridge fns.
        // Note: fontFamily NOT dispatched today — SkFontMgr font registration
        // (pulp#932) blocks proper resolution and would force Skia to return
        // a null SkTypeface when JetBrains Mono can't be looked up. The
        // remaining typography dispatchers are needed though — the
        // bundle's chrome layout (especially Label widths under #935 auto-grow)
        // depends on letter-spacing and font-size being set correctly.
        case 'fontSize':        return call('setFontSize', id, value as number);
        case 'fontWeight':      return call('setFontWeight', id, typeof value === 'number' ? value : Number(value));
        case 'fontStyle':       return call('setFontStyle', id, value as string);
        case 'letterSpacing':   return call('setLetterSpacing', id, value as number);
        case 'lineHeight':      return call('setLineHeight', id, value as number);

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
    for (const key of Object.keys(props)) {
        if (isReactInternal(key)) continue;
        if (key === 'children') continue;  // text children handled by caller
        if (isEventHandler(key)) {
            applyEventHandler(id, key, props[key]);
            continue;
        }
        applyOne(id, type, key, props[key]);
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

    // Walk new props — set anything that changed value
    for (const key of Object.keys(newProps)) {
        if (isReactInternal(key)) continue;
        if (key === 'children') continue;
        if (oldProps[key] !== newProps[key]) {
            if (isEventHandler(key)) {
                applyEventHandler(id, key, newProps[key]);
            } else {
                applyOne(id, type, key, newProps[key]);
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
