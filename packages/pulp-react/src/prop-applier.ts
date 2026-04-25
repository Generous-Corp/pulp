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

/// Returns true if the prop is purely React-internal (not a bridge setter).
function isReactInternal(key: string): boolean {
    return (
        key === 'children' ||
        key === 'key' ||
        key === 'ref' ||
        key === 'id' ||
        key.startsWith('on')   // onClick, onChange, etc. — handled separately
    );
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
        case 'direction':       return setFlex(id, 'direction', value as string);
        case 'gap':             return setFlex(id, 'gap', value as number);
        case 'rowGap':          return setFlex(id, 'row_gap', value as number);
        case 'columnGap':       return setFlex(id, 'column_gap', value as number);
        case 'padding':         return setFlex(id, 'padding', value as number);
        case 'paddingTop':      return setFlex(id, 'padding_top', value as number);
        case 'paddingRight':    return setFlex(id, 'padding_right', value as number);
        case 'paddingBottom':   return setFlex(id, 'padding_bottom', value as number);
        case 'paddingLeft':     return setFlex(id, 'padding_left', value as number);
        case 'margin':          return setFlex(id, 'margin', value as number);
        case 'marginTop':       return setFlex(id, 'margin_top', value as number);
        case 'marginRight':     return setFlex(id, 'margin_right', value as number);
        case 'marginBottom':    return setFlex(id, 'margin_bottom', value as number);
        case 'marginLeft':      return setFlex(id, 'margin_left', value as number);
        case 'flexGrow':        return setFlex(id, 'flex_grow', value as number);
        case 'flexShrink':      return setFlex(id, 'flex_shrink', value as number);
        case 'flexBasis':       return setFlex(id, 'flex_basis', value as number);
        case 'flexWrap':        return setFlex(id, 'flex_wrap', value ? 1 : 0);
        case 'order':           return setFlex(id, 'order', value as number);
        case 'width':           return setFlex(id, 'width', value as number);
        case 'height':          return setFlex(id, 'height', value as number);
        case 'minWidth':        return setFlex(id, 'min_width', value as number);
        case 'minHeight':       return setFlex(id, 'min_height', value as number);
        case 'maxWidth':        return setFlex(id, 'max_width', value as number);
        case 'maxHeight':       return setFlex(id, 'max_height', value as number);
        case 'alignItems':      return setFlex(id, 'align_items', value as string);
        case 'alignSelf':       return setFlex(id, 'align_self', value as string);
        case 'justifyContent':  return setFlex(id, 'justify_content', value as string);

        // Visual style
        case 'background':         return setBackground(id, value as string);
        case 'backgroundGradient': return setBackgroundGradient(id, value as string);
        case 'border': {
            const b = value as { color: string; width?: number; radius?: number };
            return setBorder(id, b.color, b.width ?? 1, b.radius ?? 0);
        }
        case 'borderTop':    { const b = value as { color: string; width: number }; return setBorderSide(id, 'top', b.width, b.color); }
        case 'borderRight':  { const b = value as { color: string; width: number }; return setBorderSide(id, 'right', b.width, b.color); }
        case 'borderBottom': { const b = value as { color: string; width: number }; return setBorderSide(id, 'bottom', b.width, b.color); }
        case 'borderLeft':   { const b = value as { color: string; width: number }; return setBorderSide(id, 'left', b.width, b.color); }
        case 'opacity':      return setOpacity(id, value as number);
        case 'visible':      return setVisible(id, value as boolean);

        // Text
        case 'text':       return setText(id, String(value));
        case 'textColor':  return setTextColor(id, value as string);
        case 'textAlign':  return setTextAlign(id, value as 'left' | 'center' | 'right');

        // Widget-specific data
        case 'data':
            // <Spectrum data={...}> + <Waveform data={...}> share the prop
            // name; we route based on widget type at the call site.
            if (type === 'Spectrum') return setSpectrumData(id, value as number[] | Float32Array);
            if (type === 'Waveform') return setWaveformData(id, value as number[] | Float32Array);
            return;
        case 'level':    return setMeterLevel(id, value as number);
        case 'value':    return setValue(id, value as number);

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
    for (const key of Object.keys(props)) {
        if (isReactInternal(key)) continue;
        if (key === 'children') continue;  // text children handled by caller
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
            applyOne(id, type, key, newProps[key]);
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
            if (key === 'visible')  { setVisible(id, true); mutated = true; }
            if (key === 'opacity')  { setOpacity(id, 1.0); mutated = true; }
            // Other setters: no-op — let the next mount cycle handle it
        }
    }

    return mutated;
}
