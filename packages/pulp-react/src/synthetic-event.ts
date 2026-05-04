// synthetic-event.ts — pulp #1352
//
// JSX consumers expect React-DOM-style SyntheticEvent objects with
// `currentTarget`, `target`, `preventDefault`, etc. The bridge's
// `__dispatch__(id, eventName, ...rawArgs)` channel forwards positional
// args verbatim — for `mouseenter` it sends a literal `0`, for `click`
// also `0`, for `pointerdown` an object with clientX/Y, for `change`
// (text editor) the new string value. None of those match the JSX
// handler shape, so handlers like `e => e.currentTarget.style.background = ...`
// crash with `Cannot read property 'style' of undefined`.
//
// This module synthesizes a minimal React-DOM-compatible event object
// before the JSX handler runs. The `currentTarget` exposes a thin
// element wrapper (`makeElementWrapper(id)`) whose `.style` setters
// route directly to the bridge's setBackground / setBorder / etc. fns,
// matching what `e.currentTarget.style.background = '...'` does in
// React-DOM. This is the only place in @pulp/react that constructs an
// element-like surface — @pulp/react instances are not registered in
// the web-compat `__elements__` map (they bypass `document.createElement`),
// so we cannot reuse the web-compat Element class here.
//
// Coexistence with #1345's CSS `:hover` translation: that path already
// synthesizes proper events via web-compat-element's `_makeEvent` +
// `_dispatchEvent`. JSX-direct handlers bypass that pipeline entirely;
// this module is the matching synthesis for the prop-applier `on()` lane.

type AnyFn = (...args: unknown[]) => unknown;

const g = (): Record<string, AnyFn | undefined> =>
    globalThis as unknown as Record<string, AnyFn | undefined>;

/// Element-shim style API. Only the setters JSX consumers commonly
/// mutate are wired (`background`, `opacity`, `visible`, the border
/// shorthands and the per-axis flex setters). Reads are not supported
/// — JSX consumers that need to read back computed values should use
/// state, not the synthetic event.
function makeStyleProxy(id: string): Record<string, unknown> {
    // Map of style-property → bridge setter invocation. Each entry takes
    // the raw value JSX wrote and routes it through the same bridge fn
    // the prop-applier uses, so that a runtime mutation has the same
    // effect as a re-render with that prop.
    const setters: Record<string, (v: unknown) => void> = {
        background: (v) => callBridge('setBackground', id, String(v)),
        backgroundColor: (v) => callBridge('setBackground', id, String(v)),
        backgroundGradient: (v) => callBridge('setBackgroundGradient', id, String(v)),
        opacity: (v) => callBridge('setOpacity', id, Number(v)),
        // visibility: 'hidden' | 'visible' — matches CSS, not the inverse `hidden`.
        visibility: (v) => callBridge('setVisible', id, String(v) !== 'hidden'),
        // Border shorthands route to the per-attribute bridge setters
        // that preserve unset siblings (pulp #1027).
        borderColor: (v) => callBridge('setBorderColor', id, String(v)),
        borderWidth: (v) => callBridge('setBorderWidth', id, Number(v)),
        borderRadius: (v) => callBridge('setBorderRadius', id, Number(v)),
        // Text
        color: (v) => callBridge('setTextColor', id, String(v)),
        fontSize: (v) => callBridge('setFontSize', id, Number(v)),
        // Layout — minimal subset; matches what setFlex accepts.
        width: (v) => callBridge('setFlex', id, 'width', Number(v)),
        height: (v) => callBridge('setFlex', id, 'height', Number(v)),
    };
    const proxy: Record<string, unknown> = {};
    for (const key of Object.keys(setters)) {
        Object.defineProperty(proxy, key, {
            configurable: true,
            enumerable: true,
            get() { return undefined; }, // reads not supported — by design
            set(v: unknown) { setters[key]!(v); },
        });
    }
    return proxy;
}

function callBridge(name: string, ...args: unknown[]): void {
    const fn = g()[name];
    if (typeof fn === 'function') fn(...args);
}

/// Thin element-like wrapper for the synthetic event's currentTarget /
/// target slots. Carries the widget id so consumers can correlate events,
/// exposes a `.style` proxy that mirrors what the prop-applier emits,
/// and a `setAttribute`/`getAttribute` no-op pair so React-DOM-shaped
/// code that touches them doesn't crash.
export interface SyntheticElementWrapper {
    id: string;
    _id: string;
    style: Record<string, unknown>;
    setAttribute: (name: string, value: string) => void;
    getAttribute: (name: string) => string | null;
}

function makeElementWrapper(id: string): SyntheticElementWrapper {
    return {
        id,
        _id: id, // mirrors web-compat Element naming for cross-compat consumers
        style: makeStyleProxy(id),
        // pulp #1352: setAttribute/getAttribute are common in DOM-shaped
        // code; we accept the writes but don't persist them — there is
        // no HTML attribute layer in Pulp. JSX prop-applier is the
        // canonical write path.
        setAttribute(_name: string, _value: string): void { /* no-op */ },
        getAttribute(_name: string): string | null { return null; },
    };
}

/// Shape used by the bridge's `pointer*` and `gesture*` data payloads.
interface PointerLikeData {
    clientX?: number;
    clientY?: number;
    offsetX?: number;
    offsetY?: number;
    button?: number;
    pointerId?: number;
    pointerType?: string;
    isPrimary?: boolean;
    pressure?: number;
    ctrlKey?: boolean;
    shiftKey?: boolean;
    altKey?: boolean;
    metaKey?: boolean;
    scale?: number;
    rotation?: number;
}

/// React-DOM-compatible synthetic event surface (subset). Constructed
/// per dispatch so handlers can call preventDefault / stopPropagation
/// without leaking state across calls.
export interface SyntheticEvent {
    type: string;
    currentTarget: SyntheticElementWrapper;
    target: SyntheticElementWrapper;
    nativeEvent: { rawArgs: unknown[] };
    preventDefault: () => void;
    stopPropagation: () => void;
    defaultPrevented: boolean;
    // Position
    clientX: number;
    clientY: number;
    offsetX: number;
    offsetY: number;
    button: number;
    // Pointer
    pointerId: number;
    pointerType: string;
    isPrimary: boolean;
    pressure: number;
    // Modifier keys
    ctrlKey: boolean;
    shiftKey: boolean;
    altKey: boolean;
    metaKey: boolean;
    // Gesture (ignored unless gesture event)
    scale: number;
    rotation: number;
    // Form events (text input / change)
    key: string;
    keyCode: number;
}

function isPlainObject(v: unknown): v is Record<string, unknown> {
    return typeof v === 'object' && v !== null && !Array.isArray(v);
}

/// Build a synthetic event for a given (id, eventName, ...rawArgs)
/// dispatch. Routes raw args based on event type:
///
///   - mouseenter / mouseleave / click  → bridge sends literal 0; populate
///     currentTarget only.
///   - pointerdown / pointermove / etc. → bridge sends an object with
///     clientX/Y/etc.; lift the fields onto the synthetic event.
///   - change (text editor)             → bridge sends the new string
///     value; expose as `target.value` (and on the synthetic event for
///     code that reads `e.value` directly).
///   - return (text editor commit)      → bridge sends the committed
///     string; expose as `target.value`.
///   - keydown / keyup                  → bridge sends an object with
///     `key`/`code`/modifiers.
///   - focus / blur                     → bridge sends nothing; just the
///     synthetic shell.
///
/// `nativeEvent.rawArgs` always carries the unmodified bridge args for
/// debugging / escape hatch.
export function makeSyntheticEvent(
    id: string,
    eventName: string,
    rawArgs: unknown[],
): SyntheticEvent {
    const target = makeElementWrapper(id);
    const evt: SyntheticEvent = {
        type: eventName,
        currentTarget: target,
        target: target,
        nativeEvent: { rawArgs },
        defaultPrevented: false,
        preventDefault() { evt.defaultPrevented = true; },
        stopPropagation() { /* no-op — JSX handlers attach via on() with no bubble chain */ },
        clientX: 0, clientY: 0, offsetX: 0, offsetY: 0, button: 0,
        pointerId: 0, pointerType: 'mouse', isPrimary: true, pressure: 0.5,
        ctrlKey: false, shiftKey: false, altKey: false, metaKey: false,
        scale: 1, rotation: 0,
        key: '', keyCode: 0,
    };

    // Route raw args by event-name family.
    const a0 = rawArgs[0];

    // Pointer / gesture / mouse-with-data — bridge sends an object literal.
    if (isPlainObject(a0)) {
        const d = a0 as PointerLikeData & { key?: string; code?: string; keyCode?: number };
        if (typeof d.clientX === 'number') evt.clientX = d.clientX;
        if (typeof d.clientY === 'number') evt.clientY = d.clientY;
        if (typeof d.offsetX === 'number') evt.offsetX = d.offsetX;
        if (typeof d.offsetY === 'number') evt.offsetY = d.offsetY;
        if (typeof d.button === 'number') evt.button = d.button;
        if (typeof d.pointerId === 'number') evt.pointerId = d.pointerId;
        if (typeof d.pointerType === 'string') evt.pointerType = d.pointerType;
        if (typeof d.isPrimary === 'boolean') evt.isPrimary = d.isPrimary;
        if (typeof d.pressure === 'number') evt.pressure = d.pressure;
        if (typeof d.ctrlKey === 'boolean') evt.ctrlKey = d.ctrlKey;
        if (typeof d.shiftKey === 'boolean') evt.shiftKey = d.shiftKey;
        if (typeof d.altKey === 'boolean') evt.altKey = d.altKey;
        if (typeof d.metaKey === 'boolean') evt.metaKey = d.metaKey;
        if (typeof d.scale === 'number') evt.scale = d.scale;
        if (typeof d.rotation === 'number') evt.rotation = d.rotation;
        if (typeof d.key === 'string') evt.key = d.key;
        if (typeof d.keyCode === 'number') evt.keyCode = d.keyCode;
    }

    // Text-editor `change` / `return` — bridge sends a raw string. Expose
    // it as `target.value` so React-DOM-idiomatic code (`e.target.value`)
    // works. Also stash on the synthetic event itself via a dynamic
    // property since the typed surface above doesn't include `value`.
    if (eventName === 'change' || eventName === 'return' || eventName === 'input') {
        const val = typeof a0 === 'string' ? a0
            : typeof a0 === 'number' ? a0 // Knob/Fader/Toggle — numeric value
            : a0;
        // Mutate the wrapper so e.target.value works. JSX consumers
        // reach for this on text-editor change events (the canonical
        // React DOM idiom).
        (target as unknown as Record<string, unknown>).value = val;
        (evt as unknown as Record<string, unknown>).value = val;
    }

    // Toggle `toggle` / Checkbox `change` — bridge sends 0|1 (numeric).
    // Expose as `target.checked` so React-DOM-idiomatic code works.
    if (eventName === 'toggle' && typeof a0 === 'number') {
        (target as unknown as Record<string, unknown>).checked = a0 !== 0;
        (evt as unknown as Record<string, unknown>).checked = a0 !== 0;
    }

    return evt;
}
