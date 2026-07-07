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
import { call } from './prop-applier-internal.js';
import { applyLayoutProp } from './prop-applier-layout.js';
import { applyPaintProp } from './prop-applier-paint.js';
import { applyTypographyProp } from './prop-applier-typography.js';
import { applyTransformProp } from './prop-applier-transform.js';
import { applyEventProp } from './prop-applier-events.js';

// Per-domain handler modules own layout, paint, typography, transform,
// and declarative event-routing props. `applyOne` below is the thin
// dispatcher that calls each handler in sequence until one claims the
// key. The bridge `call` helper and shared `_resolveVar` resolver live
// in prop-applier-internal.ts so every domain shares one logging
// counter.
//
// The optional logging hook is looked up through globalThis at call time.
// Bridge setters dispatch through `call()` in prop-applier-internal.ts.
type AnyFn = (...args: unknown[]) => unknown;
const g = globalThis as unknown as Record<string, AnyFn | undefined>;

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

function virtualListPropRank(key: string): number {
    if (isEventHandler(key)) return -1;
    switch (key) {
        case 'rowCount': return 0;
        case 'rowHeight': return 1;
        case 'overscan': return 2;
        case 'selectionMode': return 3;
        case 'selected': return 4;
        case 'renderRow': return 5;
        case 'scrollToRow': return 6;
        default: return 100;
    }
}

function orderedPropKeys(type: string, props: Record<string, unknown>): string[] {
    const keys = Object.keys(props);
    if (type !== 'VirtualList' && type !== 'virtuallist' && type !== 'virtual-list') return keys;
    return keys
        .map((key, index) => ({ key, index }))
        .sort((a, b) => virtualListPropRank(a.key) - virtualListPropRank(b.key) || a.index - b.index)
        .map((item) => item.key);
}

function isVirtualListType(type: string): boolean {
    return type === 'VirtualList' || type === 'virtuallist' || type === 'virtual-list';
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
/// registerHover, the C++ side never fires the events even when the JS
/// listener is installed.
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
/// `on(id, 'pointerdown', fn)`. This mirrors hover gating: installing
/// the JS listener and arming the native dispatch path are separate
/// bridge operations.
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
/// `on_pointer_event` lambda. Wheel handlers need this separate gate
/// because pointer registration alone intentionally ignores wheel
/// events.
function isWheelEvent(eventName: string): boolean {
    return eventName === 'wheel';
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
        // Without this call the bridge keeps the JS listener in its
        // dispatch table but the View's on_pointer_event callback is
        // never armed. Idempotent on the bridge side.
        call('registerPointer', id);
    }
    // Also arm the pointer dispatch path for mouse events. Do not do
    // this for onClick: that is the W3C click-on-release semantic and
    // routes through on_click; onClick alone intentionally does not arm
    // pointer dispatch.
    // Imported JSX bundles install onMouseDown / onMouseMove / onMouseUp
    // handlers that need to fire on press, not release. Without pointer
    // registration, hit_test returns the widget with has_pointer=no and
    // the bridge's pointer event lambda never fires for mouse* event types.
    if (eventName === 'mousedown' || eventName === 'mouseup' ||
        eventName === 'mousemove') {
        call('registerPointer', id);
    }
    if (isWheelEvent(eventName)) {
        // Wheel dispatch is separately armed from pointer dispatch.
        call('registerWheel', id);
    }
    // Wrap the React handler in a synthetic-event factory so JSX
    // consumers receive a React-DOM-shaped event object (with
    // `currentTarget`, `target`, `preventDefault`, `nativeEvent.rawArgs`,
    // and event-type-specific fields) instead of the bridge's raw
    // positional args. Without this, idiomatic handlers like
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

/// Emit one setSvgRect call carrying the full geometry (x, y, width,
/// height). Driven by applyOne when any geometry prop changes so the
/// bridge never sees a partial update that would clobber unset axes
/// back to zero. Reads current values from `props` (the live React
/// props snapshot) and falls back to 0 for unset axes, matching SVG
/// `<rect>` defaults.
function emitSvgRectGeometry(id: string, props: Record<string, unknown>): void {
    const x = typeof props.x === 'number' ? props.x as number : 0;
    const y = typeof props.y === 'number' ? props.y as number : 0;
    const w = typeof props.width === 'number' ? props.width as number : 0;
    const h = typeof props.height === 'number' ? props.height as number : 0;
    call('setSvgRect', id, x, y, w, h);
}

/// Emit one setSvgLine call carrying the full geometry (x1, y1, x2,
/// y2). Same partial-update guard as emitSvgRectGeometry.
function emitSvgLineGeometry(id: string, props: Record<string, unknown>): void {
    const x1 = typeof props.x1 === 'number' ? props.x1 as number : 0;
    const y1 = typeof props.y1 === 'number' ? props.y1 as number : 0;
    const x2 = typeof props.x2 === 'number' ? props.x2 as number : 0;
    const y2 = typeof props.y2 === 'number' ? props.y2 as number : 0;
    call('setSvgLine', id, x1, y1, x2, y2);
}

/// Apply a single prop to its corresponding bridge setter.
///
/// Thin dispatcher over the per-domain handler modules. Each
/// `applyXProp` returns true if it claimed the key. Domain keys are
/// mutually exclusive, so the call order does not change which handler
/// runs. The type-dispatched widget/SVG props (`data` / `level` /
/// `value` / `d` / `viewBox` / `fill` / `stroke` / `strokeWidth`) stay
/// inline below because they route on `type`, not purely on `key`, and
/// do not fit the layout/paint/typography/transform/events taxonomy.
function applyOne(id: string, type: string, key: string, value: unknown, props?: Record<string, unknown>): void {
    if (value === undefined || value === null) {
        if (isVirtualListType(type)) {
            if (key === 'selectionMode') call('setVirtualListSelectionMode', id, 'single');
            if (key === 'selected') call('setVirtualListSelected', id, -1);
        }
        // No-op — Pulp has no "unset" for most setters; rely on React
        // unmount + recreate for full clears. Selective resets can be
        // added per-prop here if a regression appears.
        return;
    }

    // SvgRect / SvgLine geometry props collide with View flex props
    // (width/height) and event/positioning props (x/y), so dispatch on
    // `type` BEFORE the generic flex routing. The geometry setters are
    // atomic: one bridge call per rect/line carries the full quad of
    // coords to avoid partial updates clobbering unset axes back to zero.
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

    // Per-domain dispatch. The first handler that claims the key wins;
    // since domain keys are disjoint this is deterministic.
    if (applyLayoutProp(id, key, value, props)) return;
    if (applyPaintProp(id, key, value)) return;
    if (applyTypographyProp(id, key, value, props)) return;
    if (applyTransformProp(id, key, value)) return;
    if (applyEventProp(id, key, value)) return;

    // `<img src="…">` / `<Image src="…">` must forward to the ImageView
    // bridge via setImageSource, mirroring the non-React web-compat path
    // in core/view/js/web-compat-element.js. Without this, the
    // prop-applier creates the Image widget but never dispatches `src`,
    // so the emitted bundle has zero setImageSource calls and every
    // <img> renders as the empty "IMG" placeholder. Gate on the Image
    // element types: host-config maps both the lowercase `'img'`
    // intrinsic and the `'Image'` component to createImage, so accept
    // both; a stray `src` on any other widget cannot hit the
    // ImageView-only setter. The path is forwarded verbatim; C++
    // setImageSource -> ImageView::set_image_path resolves the rest,
    // exactly as the web-compat path does.
    if ((type === 'img' || type === 'Image') && key === 'src') {
        call('setImageSource', id, String(value));
        return;
    }

    if (isVirtualListType(type)) {
        switch (key) {
            case 'rowCount':      return call('setVirtualListRowCount', id, value as number);
            case 'rowHeight':     return call('setListRowHeight', id, value as number);
            case 'overscan':      return call('setVirtualListOverscan', id, value as number);
            case 'selectionMode': return call('setVirtualListSelectionMode', id, value as string);
            case 'selected':      return call('setVirtualListSelected', id, value as number);
            case 'scrollToRow':   return call('scrollVirtualListToRow', id, value as number);
            case 'renderRow':     return call('refreshVirtualListRows', id);
        }
    }

    // Type-dispatched widget / SVG props — these route on the widget
    // `type`, not purely on the prop key, so they stay in the
    // dispatcher rather than a key-keyed domain module.
    switch (key) {
        // Widget-specific data
        case 'data':
            // <Spectrum data={...}> + <Waveform data={...}> share the prop
            // name; we route based on widget type at the call site.
            if (type === 'Spectrum') return call('setSpectrumData', id, value as number[] | Float32Array);
            if (type === 'Waveform') return call('setWaveformData', id, value as number[] | Float32Array);
            return;
        case 'level':    return call('setMeterLevel', id, value as number);
        case 'value':
            // Type-aware routing: the bridge has separate setters per
            // widget type. setValue only handles knob/fader/toggle/
            // checkbox; Progress wants setProgress, and Spectrum /
            // Waveform use the 'data' prop handled above.
            if (type === 'Progress')   return call('setProgress', id, value as number);
            if (type === 'Meter')      return call('setMeterLevel', id, value as number);
            return call('setValue', id, value as number);

        // SvgPath wires the SvgPathWidget bridge surface through a
        // typed JSX intrinsic.
        case 'd':            return call('setSvgPath', id, value as string);
        case 'viewBox': {
            // Array form `[w, h]` — the original wiring.
            if (Array.isArray(value) && value.length >= 2) {
                return call('setSvgViewBox', id, value[0] as number, value[1] as number);
            }
            // SVG-spec string form `'min-x min-y w h'` (or `'w h'`).
            // The bridge consumes width + height only today
            // (the SvgPathWidget doesn't yet honor the min-x / min-y
            // origin offset — tracked as a separate paint-side gap),
            // so we extract the trailing two tokens as w/h. This makes
            // `<svg viewBox="0 0 24 24">` exports from Figma / Lucide
            // / Heroicons / etc. flow through without the consumer
            // having to re-shape the value into a tuple.
            if (typeof value === 'string') {
                const tokens = value.trim().split(/[\s,]+/).map(parseFloat).filter(Number.isFinite);
                if (tokens.length === 4) {
                    return call('setSvgViewBox', id, tokens[2], tokens[3]);
                }
                if (tokens.length === 2) {
                    return call('setSvgViewBox', id, tokens[0], tokens[1]);
                }
            }
            return;
        }
        case 'fill':         return call('setSvgFill', id, value as string);
        case 'fillRule':     return call('setSvgFillRule', id, value as string);
        case 'fillGradient': return call('setSvgFillGradient', id, value as string);
        case 'stroke':       return call('setSvgStroke', id, value as string);
        case 'strokeWidth':  return call('setSvgStrokeWidth', id, value as number);

        default:
            // Unknown prop — silently ignore. We could warn here in DEV
            // builds, but staying chatty in prod is annoying. The
            // typed JSX intrinsics in types.ts already gate this.
            return;
    }
}

// ── normalizeHostProps ───────────────────────────────────────────────
//
// Imported React apps (design-tool or runtime-import bundles) use
// HTML-style JSX:
//   <div style={{ width: 100, color: 'red' }} className="card">
// This flattens that to the shape applyAllProps/applyChangedProps
// expect (flat `width: 100, color: 'red'` props).
//
// Merge precedence: className rules < inline style < explicit flat
// props. Later overrides earlier.

type ClassRulesProvider = (className: string) => Record<string, unknown> | null;

let _classRulesProvider: ClassRulesProvider | null = null;

/// Install a class-rules resolver. runtime-import wires this from the
/// classnames.json that the source bundle ships. Pass null to clear.
export function setClassRulesProvider(fn: ClassRulesProvider | null): void {
    _classRulesProvider = fn;
}

/// Returns the active provider (test helper).
export function getClassRulesProvider(): ClassRulesProvider | null {
    return _classRulesProvider;
}

/// Flatten raw HTML/JSX props with `style` object + `className` string
/// into the flat-prop shape prop-applier consumes. Idempotent on
/// already-flat input.
export function normalizeHostProps(
    _type: string,
    rawProps: Record<string, unknown>,
): Record<string, unknown> {
    const hasStyle = rawProps.style !== undefined && rawProps.style !== null
        && typeof rawProps.style === 'object';
    const hasClassName = typeof rawProps.className === 'string'
        && (rawProps.className as string).length > 0;
    if (!hasStyle && !hasClassName) return rawProps;

    // Use a null-prototype prop map so malformed CSS rules or hostile
    // class-rules JSON carrying `__proto__` / `constructor` /
    // `prototype` keys cannot poison Object.prototype through merges.
    // The applier later iterates `Object.keys(out)`, which is safe on a
    // null-prototype object.
    const out: Record<string, unknown> = Object.create(null);

    // Filter dangerous keys when merging untrusted provider output. The
    // inline `style` and flat-prop paths trust the JSX author (React
    // already strips these at element-construction time), but the
    // class-rules provider returns JSON parsed from a bundle asset —
    // that's untrusted input.
    const isSafeKey = (k: string): boolean =>
        k !== '__proto__' && k !== 'constructor' && k !== 'prototype';

    if (hasClassName && _classRulesProvider) {
        const tokens = (rawProps.className as string)
            .split(/\s+/).filter(t => t.length > 0);
        for (const tok of tokens) {
            const rules = _classRulesProvider(tok);
            if (!rules) continue;
            for (const k of Object.keys(rules)) {
                if (isSafeKey(k)) out[k] = (rules as Record<string, unknown>)[k];
            }
        }
    }

    if (hasStyle) {
        const style = rawProps.style as Record<string, unknown>;
        for (const k of Object.keys(style)) out[k] = style[k];
    }

    for (const k of Object.keys(rawProps)) {
        if (k === 'style' || k === 'className') continue;
        out[k] = rawProps[k];
    }

    return out;
}

/// Apply ALL props for first-attach. Called from appendInitialChild /
/// appendChild after the instance lands on the bridge.
export function applyAllProps(instance: PulpInstance): void {
    const { id, type, props } = instance;
    logApply('applyAll', id, type, Object.keys(props).length);
    // For SvgRect / SvgLine, emit the geometry call once up-front from
    // the full props snapshot, then skip the per-prop dispatch for the
    // four geometry keys. Otherwise we'd issue four identical
    // setSvgRect / setSvgLine calls during the loop.
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
    for (const key of orderedPropKeys(type, props)) {
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
    const selectionModeChanged = isVirtualListType(type) &&
        oldProps.selectionMode !== newProps.selectionMode;

    // Coalesce SvgRect / SvgLine geometry changes into a single
    // setSvgRect / setSvgLine call sourced from the post-update props
    // snapshot. Without this, four separate prop diffs would each try
    // to emit a partial geometry update.
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
    for (const key of orderedPropKeys(type, newProps)) {
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
            // When overlay flips off or disappears from props, release
            // the global overlay slot so the platform host does not keep
            // routing clicks to a stale popover.
            if (key === 'overlay' && oldProps[key]) {
                call('releaseOverlay', id);
                mutated = true;
            }
            // Visual cluster keys must clear when they fall out of
            // newProps. The conditional-spread idiom
            //   style={{ ...base, ...(active ? activeStyle : {}) }}
            // contributes nothing to the spread on the inactive side, so
            // the active-only keys vanish from newProps but the bridge
            // keeps painting them. Reset to the canonical "no visual"
            // value here so the next paint reflects React's intent.
            // Any imported design using the same conditional-spread
            // pattern needs this reset path.
            if (key === 'background' || key === 'backgroundGradient') {
                call('setBackground', id, 'transparent');
                mutated = true;
            }
            if (key === 'border' || key === 'borderColor' || key === 'borderWidth') {
                // Width-zero collapses the painted edge regardless of the
                // color slot; cheaper than a 4-arg setBorder reset and
                // keeps borderRadius (a separate slot) untouched.
                call('setBorderWidth', id, 0);
                mutated = true;
            }
            if (key === 'borderTop' || key === 'borderRight' || key === 'borderBottom' || key === 'borderLeft') {
                const side = key.slice('border'.length).toLowerCase();
                call('setBorderSide', id, side, 0, 'transparent');
                mutated = true;
            }
            if (key === 'textColor') {
                // Empty string is the bridge-side "use default" sentinel.
                call('setTextColor', id, '');
                mutated = true;
            }
            // `<Image>` whose `src` is removed clears the ImageView back
            // to the empty placeholder, matching the web-compat
            // removeAttribute('src') reset semantics. Empty string is the
            // bridge-side "no source" sentinel.
            // `type` is typed as keyof IntrinsicElementMap (which lists 'Image'
            // but not the lowercase 'img' intrinsic — host-config routes 'img'
            // through its string-fallback switch). Cast for the 'img' compare so
            // the clear-on-removal seam matches the same element types as the
            // set seam above.
            if (key === 'src' && ((type as string) === 'img' || type === 'Image')) {
                call('setImageSource', id, '');
                mutated = true;
            }
            if (isVirtualListType(type) && key === 'selectionMode') {
                call('setVirtualListSelectionMode', id, 'single');
                mutated = true;
            }
            if (isVirtualListType(type) && key === 'selected') {
                call('setVirtualListSelected', id, -1);
                mutated = true;
            }
            // Other setters: no-op — let the next mount cycle handle it
        }
    }

    if (selectionModeChanged && Object.prototype.hasOwnProperty.call(newProps, 'selected')) {
        applyOne(id, type, 'selected', newProps.selected, newProps);
        mutated = true;
    }

    return mutated;
}
