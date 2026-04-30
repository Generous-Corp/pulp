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

function applyEventHandler(id: string, key: string, value: unknown): void {
    if (typeof value !== 'function') return;
    const eventName = eventNameFor(key);
    // Wrap the React handler so the bridge's __dispatch__ can call it.
    // The bridge passes positional args after id+eventName; just forward.
    call('on', id, eventName, (...a: unknown[]) => (value as (...x: unknown[]) => void)(...a));
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
        case 'borderTop':    { const b = value as { color: string; width: number }; return call('setBorderSide', id, 'top', b.width, b.color); }
        case 'borderRight':  { const b = value as { color: string; width: number }; return call('setBorderSide', id, 'right', b.width, b.color); }
        case 'borderBottom': { const b = value as { color: string; width: number }; return call('setBorderSide', id, 'bottom', b.width, b.color); }
        case 'borderLeft':   { const b = value as { color: string; width: number }; return call('setBorderSide', id, 'left', b.width, b.color); }
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
            // Other setters: no-op — let the next mount cycle handle it
        }
    }

    return mutated;
}
