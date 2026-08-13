// host-config.ts — react-reconciler HostConfig targeting pulp::view::WidgetBridge.
//
// Design choices:
//   - Mutation mode (Ink/R3F pattern), NOT persistence (RNS pattern)
//   - isPrimaryRenderer: true (Pulp is standalone — no second renderer to coexist with)
//   - shouldSetTextContent selectively (Label/Button/TextEditor only) — NOT a separate text-node type
//   - Ink-style scheduler (supportsMicrotasks: true + queueMicrotask)
//   - DEFER concurrent mode for v0
//   - createInstance does NOT receive parent — attachment happens in appendChild/appendInitialChild
//   - insertBefore routes same-parent reorders through the bridge's indexed insert path
//   - Commit-time layout flush owned by the host config (resetAfterCommit), not the bridge

import type { HostConfig } from 'react-reconciler';
import { DefaultEventPriority } from 'react-reconciler/constants.js';

import type {
    PulpInstance,
    PulpContainer,
    IntrinsicElementName,
} from './types.js';
import { requestLayoutFlush } from './layout-flush.js';
import {
    applyAllProps,
    applyChangedProps,
    clearMaterializedEventCallbacks,
    normalizeHostProps,
} from './prop-applier.js';

type Type = IntrinsicElementName;
type Props = Record<string, unknown>;
type Container = PulpContainer;
type Instance = PulpInstance;
type TextInstance = never; // No text-node type — see shouldSetTextContent.
type SuspenseInstance = Instance;
type HydratableInstance = Instance;
type PublicInstance = Instance;
type HostContext = Record<string, never>;
type UpdatePayload = boolean; // Reconciler ignores; we always run commitUpdate
type ChildSet = never;
type TimeoutHandle = ReturnType<typeof setTimeout>;
type NoTimeout = -1;

type EventPriority = number;
const NoEventPriority = 0;
let currentUpdatePriority: EventPriority = NoEventPriority;

// All bridge globals are resolved through globalThis at call time so the
// mock-bridge install path (test/host-config.test.ts) and any later
// runtime swap (e.g. test isolation) are picked up. Bare global
// references would bind once at module load and miss the mocks.
type AnyFn = (...args: unknown[]) => unknown;
const g = globalThis as unknown as Record<string, AnyFn | undefined>;
type DomRegistry = Map<string, Record<string, unknown>>;
const domRegistry = (): DomRegistry => {
    const host = globalThis as unknown as Record<string, unknown>;
    const windowProxy = host.window as Record<string, unknown> | undefined;
    const hostRegistry = host.__pulpReactDomRegistry__ instanceof Map
        ? host.__pulpReactDomRegistry__ as DomRegistry : null;
    const proxyRegistry = windowProxy?.__pulpReactDomRegistry__ instanceof Map
        ? windowProxy.__pulpReactDomRegistry__ as DomRegistry : null;
    const registry = hostRegistry ?? proxyRegistry ?? new Map<string, Record<string, unknown>>();
    if (hostRegistry && proxyRegistry && hostRegistry !== proxyRegistry) {
        for (const [id, node] of proxyRegistry) if (!hostRegistry.has(id)) hostRegistry.set(id, node);
    }
    host.__pulpReactDomRegistry__ = registry;
    if (windowProxy) windowProxy.__pulpReactDomRegistry__ = registry;
    const values = () => Array.from(registry.values());
    host.__pulpReactDomRegistryValues__ = values;
    if (windowProxy) windowProxy.__pulpReactDomRegistryValues__ = values;
    return registry;
};
let _hc_count = 0;
function call(name: string, ...args: unknown[]): unknown {
    // A materialized browser document may legitimately define globals such as
    // `createCanvas` while installing its DOM compatibility layer.  Those
    // browser helpers must not shadow the renderer bridge functions captured
    // before the document booted. Ordinary native applications do not install
    // this table and retain the direct-global fast path.
    const nativeBridge = (g as Record<string, unknown>).__pulpNativeBridgeFunctions__ as
        | Record<string, AnyFn | undefined>
        | undefined;
    const fn = nativeBridge?.[name] ?? g[name];
    if (typeof fn !== 'function') {
        const lg = (g as Record<string, AnyFn | undefined>).__spectrLog;
        if (typeof lg === 'function') lg('[host-config] bridge fn missing: ' + name);
        throw new Error('@pulp/react: bridge function ' + name + ' is not installed');
    }
    _hc_count++;
    if (_hc_count <= 200) {
        const lg = (g as Record<string, AnyFn | undefined>).__spectrLog;
        if (typeof lg === 'function') {
            const a0 = args[0] !== undefined ? String(args[0]).slice(0, 30) : '';
            const a1 = args[1] !== undefined ? String(args[1]).slice(0, 30) : '';
            lg('[hc#' + _hc_count + '] ' + name + '(' + a0 + (args.length > 1 ? ',' + a1 : '') + ')');
        }
    }
    return fn(...args);
}

// ── element-name → bridge createX dispatch ──────────────────────────
function createWidget(type: Type, id: string, parentId: string, props: Props): void {
    switch (type) {
        case 'View':
        case 'Col':         call('createCol', id, parentId); return;
        case 'Row':         call('createRow', id, parentId); return;
        case 'Panel':       call('createPanel', id, parentId); return;
        case 'Label':       call('createLabel', id, asText(props.children) ?? (props.text as string ?? ''), parentId); return;
        case 'Button': {
            const text = asText(props.children) ?? (props.text as string ?? '');
            if (typeof g.createButton === 'function') {
                call('createButton', id, text, parentId);
            } else {
                call('createPanel', id, parentId);
                call('createLabel', id + '_l', text, id);
            }
            return;
        }
        case 'TextEditor':  call('createTextEditor', id, parentId); return;
        case 'ScrollView':  call('createScrollView', id, parentId); return;
        case 'Modal':       call('createModal', id, parentId); return;
        case 'Knob':        call('createKnob', id, parentId); return;
        case 'Fader':       call('createFader', id, (props.orientation as 'vertical' | 'horizontal') ?? 'vertical', parentId); return;
        case 'Spectrum':    call('createSpectrum', id, parentId); return;
        case 'Waveform':    call('createWaveform', id, parentId); return;
        case 'Meter':       call('createMeter', id, (props.orientation as 'vertical' | 'horizontal') ?? 'vertical', parentId); return;
        case 'Progress':    call('createProgress', id, parentId); return;
        case 'XYPad':       call('createXYPad', id, parentId); return;
        case 'Checkbox':    call('createCheckbox', id, parentId); return;
        case 'Toggle':      call('createToggle', id, parentId); return;
        case 'Combo':       call('createCombo', id, parentId); return;
        // Ink & Signal design-system widgets.
        case 'Badge':       call('createBadge', id, asText(props.children) ?? (props.text as string ?? ''), (props.tone as string) ?? 'neutral', parentId); return;
        case 'Stepper':     call('createStepper', id, parentId); return;
        case 'Pan':         call('createPan', id, parentId); return;
        case 'ListBox':     call('createListBox', id, parentId); return;
        case 'VirtualList': call('createVirtualList', id, parentId); return;
        case 'Canvas':      call('createCanvas', id, parentId); return;
        case 'Image':       call('createImage', id, parentId); return;
        case 'Icon':        call('createIcon', id, (props.name as string) ?? 'image_upload', parentId); return;
        case 'SvgPath':     call('createSvgPath', id, parentId); return;
        case 'SvgRect':     call('createSvgRect', id, parentId); return;
        case 'SvgLine':     call('createSvgLine', id, parentId); return;
        // Layout box for a platform-native child view (WebView / native text
        // field / video layer). Materializes empty; a C++ host binds the OS
        // handle by id. See docs/reference/js-bridge.md.
        case 'NativeView':  call('createNativeView', id, parentId); return;
        default: {
            // Lowercase HTML/SVG intrinsic aliases let imported raw JSX
            // (<div>, <svg>, <path>, ...) lower directly to bridge widgets,
            // matching the web-compat shim's tag-to-createX map.
            const lower = String(type).toLowerCase();
            switch (lower) {
                case 'div': case 'section': case 'article': case 'aside':
                case 'header': case 'footer': case 'nav': case 'main':
                case 'figure': case 'figcaption': case 'form': case 'ul':
                case 'ol': case 'li': case 'dl': case 'dt': case 'dd': {
                    // Pure text children become a single Label so imported
                    // `<div>+{value} ct</div>`-style content renders inline
                    // instead of as stacked synthetic Label siblings.
                    const txt = asText(props.children);
                    if (txt !== undefined && txt.length > 0) {
                        call('createLabel', id, txt, parentId);
                    } else {
                        call('createCol', id, parentId);
                    }
                    return;
                }
                case 'span': case 'p': case 'label':
                case 'h1': case 'h2': case 'h3': case 'h4': case 'h5': case 'h6':
                case 'b': case 'i': case 'em': case 'strong': case 'small': case 'code':
                case 'pre': case 'a': case 'td': case 'th': case 'title':
                case 'text': case 'tspan': case 'desc': {
                    // Text-bearing inline tags are Labels for pure text and
                    // containers for nested markup. That lets React append
                    // inner spans/emphasis instead of flattening or dropping
                    // the nested element content.
                    const txt = asText(props.children);
                    if (txt !== undefined) {
                        call('createLabel', id, txt, parentId);
                    } else {
                        // Mixed or element children — use a row container
                        // so child labels flow horizontally like inline
                        // text. Span/p/label semantics expect inline; div
                        // would default to column layout.
                        call('createRow', id, parentId);
                    }
                    return;
                }
                case 'button': {
                    const text = asText(props.children) ?? (props.text as string ?? '');
                    // A lowercase HTML <button> is a CSS-styled DOM box, not
                    // Pulp's opinionated stock TextButton. Lower it to a
                    // generic native View so Chromium-captured background,
                    // border, padding, typography and nested SVG/span content
                    // remain authoritative. Explicit <Button> continues to
                    // use createButton above for native application authors.
                    call('createCol', id, parentId);
                    const textId = id + '__text';
                    call('createLabel', textId, text, id);
                    // The browser's text line boxes are relative to the
                    // complete button box. Give the Label that same box so
                    // captured offsets/baselines remain directly usable.
                    // Always create it—even for nested markup—so a dynamic
                    // nested↔plain-text React update has a stable target.
                    call('setPosition', textId, 'absolute');
                    call('setTop', textId, 0);
                    call('setRight', textId, 0);
                    call('setBottom', textId, 0);
                    call('setLeft', textId, 0);
                    call('setPointerEvents', textId, 'none');
                    return;
                }
                case 'input': {
                    const inputType = String(props.type ?? 'text').toLowerCase();
                    if (inputType === 'range') {
                        const orient = (props['aria-orientation'] === 'vertical')
                            ? 'vertical' : 'horizontal';
                        call('createFader', id, orient, parentId);
                    } else if (inputType === 'checkbox') {
                        call('createCheckbox', id, parentId);
                    } else {
                        // text / search / email / url / tel / password / (default)
                        call('createTextEditor', id, parentId);
                    }
                    return;
                }
                case 'textarea': call('createTextEditor', id, parentId); return;
                case 'select':   call('createCombo', id, parentId); return;
                case 'progress': call('createProgress', id, parentId); return;
                case 'img':      call('createImage', id, parentId); return;
                case 'canvas':   call('createCanvas', id, parentId); return;
                // Lowercase widget aliases mirror the capitalized widget
                // cases so `<knob>` / `<fader>` / ... dispatch to the same
                // native createX calls instead of the container fallback.
                case 'knob':     call('createKnob', id, parentId); return;
                case 'fader':    call('createFader', id, (props.orientation as 'vertical' | 'horizontal') ?? 'vertical', parentId); return;
                case 'toggle':   call('createToggle', id, parentId); return;
                case 'combo':    call('createCombo', id, parentId); return;
                case 'checkbox': call('createCheckbox', id, parentId); return;
                case 'spectrum': call('createSpectrum', id, parentId); return;
                case 'waveform': call('createWaveform', id, parentId); return;
                case 'meter':    call('createMeter', id, (props.orientation as 'vertical' | 'horizontal') ?? 'vertical', parentId); return;
                case 'xypad':    call('createXYPad', id, parentId); return;
                case 'listbox':  call('createListBox', id, parentId); return;
                case 'virtuallist':
                case 'virtual-list': call('createVirtualList', id, parentId); return;
                case 'native-view': call('createNativeView', id, parentId); return;
                case 'badge':    call('createBadge', id, asText(props.children) ?? (props.text as string ?? ''), (props.tone as string) ?? 'neutral', parentId); return;
                case 'stepper':  call('createStepper', id, parentId); return;
                case 'pan':      call('createPan', id, parentId); return;
                case 'icon':     call('createIcon', id, (props.name as string) ?? 'image_upload', parentId); return;
                case 'svg':      call('createCol', id, parentId); return;  // SVG = container; children paint
                case 'path': {
                    call('createSvgPath', id, parentId);
                    // SVG primitives have no intrinsic Yoga size; the JSX
                    // width/height lives on the parent <svg>. Pin children
                    // to the parent bounds so zero-sized primitives still
                    // paint.
                    call('setPosition', id, 'absolute');
                    call('setTop', id, 0);
                    call('setLeft', id, 0);
                    call('setRight', id, 0);
                    call('setBottom', id, 0);
                    // Presentational SVG children should not intercept
                    // pointer events meant for the parent <svg> handler.
                    call('setPointerEvents', id, 'none');
                    return;
                }
                case 'circle': {
                    // SVG <circle cx="..." cy="..." r="..."> → SvgPath
                    // with synthesized `d` from a 2-arc construction.
                    // Mirrors web-compat-element.js __replaySvgCircleAttributes__.
                    // Render at construction time using whatever cx/cy/r
                    // the JSX commits in this pass; subsequent prop updates
                    // re-synth via the cx/cy/r handlers in prop-applier.
                    call('createSvgPath', id, parentId);
                    const cx = typeof props.cx === 'number' ? props.cx as number : Number(props.cx) || 0;
                    const cy = typeof props.cy === 'number' ? props.cy as number : Number(props.cy) || 0;
                    const r  = typeof props.r  === 'number' ? props.r  as number : Number(props.r)  || 0;
                    // Diagnostic counter — confirms circle reaches host-config
                    // before chasing downstream prop-applier issues.
                    const gg = g as Record<string, unknown>;
                    if (typeof gg.__pulpCircleStats__ !== 'object' || gg.__pulpCircleStats__ === null) {
                        gg.__pulpCircleStats__ = { total: 0, withR: 0, samples: [] };
                    }
                    const stats = gg.__pulpCircleStats__ as Record<string, unknown>;
                    stats.total = (stats.total as number) + 1;
                    if (r > 0) {
                        stats.withR = (stats.withR as number) + 1;
                        const d = `M ${cx - r} ${cy} a ${r} ${r} 0 1 0 ${2 * r} 0 a ${r} ${r} 0 1 0 ${-2 * r} 0 Z`;
                        call('setSvgPath', id, d);
                        const samples = stats.samples as unknown[];
                        if (samples.length < 5) {
                            samples.push({ id, cx, cy, r, d, fill: props.fill, stroke: props.stroke });
                        }
                    }
                    // Same fill-parent sizing as <path> (see comment above).
                    call('setPosition', id, 'absolute');
                    call('setTop', id, 0);
                    call('setLeft', id, 0);
                    call('setRight', id, 0);
                    call('setBottom', id, 0);
                    // Presentational SVG children should not intercept
                    // pointer events meant for the parent <svg> handler.
                    call('setPointerEvents', id, 'none');
                    return;
                }
                case 'rect': {
                    call('createSvgRect', id, parentId);
                    call('setPosition', id, 'absolute');
                    call('setTop', id, 0);
                    call('setLeft', id, 0);
                    call('setRight', id, 0);
                    call('setBottom', id, 0);
                    // Presentational SVG children should not intercept
                    // pointer events meant for the parent <svg> handler.
                    call('setPointerEvents', id, 'none');
                    return;
                }
                case 'line': {
                    call('createSvgLine', id, parentId);
                    call('setPosition', id, 'absolute');
                    call('setTop', id, 0);
                    call('setLeft', id, 0);
                    call('setRight', id, 0);
                    call('setBottom', id, 0);
                    // Presentational SVG children should not intercept
                    // pointer events meant for the parent <svg> handler.
                    call('setPointerEvents', id, 'none');
                    return;
                }
                case 'g':        call('createCol', id, parentId); return; // <svg><g> group
                default: {
                    // Last-resort fallback: treat any unknown intrinsic as a
                    // container so child mounts can still attach. Surfaces a
                    // diagnostic line in dev/test paths via __spectrLog.
                    const lg = (g as Record<string, AnyFn | undefined>).__spectrLog;
                    if (typeof lg === 'function') lg('[host-config] unknown intrinsic ' + lower + ' — falling back to createCol');
                    call('createCol', id, parentId);
                    return;
                }
            }
        }
    }
}

function asText(children: unknown): string | undefined {
    if (typeof children === 'string') return children;
    if (typeof children === 'number') return String(children);
    // React passes `<button>{count}{" bands"}</button>` as an array of text
    // fragments. Mirror shouldSetTextContent: skip React's empty sentinels,
    // flatten text scalars, and bail only for real elements.
    if (Array.isArray(children)) {
        const parts: string[] = [];
        for (const c of children) {
            if (c == null || typeof c === 'boolean') continue;
            const part = asText(c);
            if (part === undefined) return undefined;
            parts.push(part);
        }
        return parts.join('');
    }
    // ReactElement children are not flattenable text. Return undefined so
    // shouldSetTextContent returns false and the reconciler walks nested
    // markup as child instances.
    return undefined;
}

function isDomSemanticProp(key: string): boolean {
    return key === 'role' || key === 'title' || key === 'name'
        || key === 'className'
        || key.startsWith('aria-') || key.startsWith('data-');
}

function syncDomSemanticProps(
    shim: Record<string, unknown>,
    oldProps: Record<string, unknown>,
    newProps: Record<string, unknown>,
): void {
    const setAttribute = shim.setAttribute as
        | ((name: string, value: unknown) => void) | undefined;
    const removeAttribute = shim.removeAttribute as
        | ((name: string) => void) | undefined;
    if (typeof setAttribute !== 'function') return;
    const keys = new Set([...Object.keys(oldProps), ...Object.keys(newProps)]);
    for (const key of keys) {
        if (!isDomSemanticProp(key) || oldProps[key] === newProps[key]) continue;
        const attribute = key === 'className' ? 'class' : key;
        const value = newProps[key];
        if (value === undefined || value === null || value === false) {
            if (typeof removeAttribute === 'function') {
                removeAttribute.call(shim, attribute);
            }
        } else {
            setAttribute.call(shim, attribute, value === true ? '' : value);
        }
    }
}

/// Element types that lower their string children to setText / createLabel
/// rather than to a child node. shouldSetTextContent reads this set.
///
/// HTML-intrinsic JSX tags also bear their own text. These aliases prevent
/// React from materializing an extra synthetic Label child on top of the
/// Label created by createWidget for pure text content.
const TEXT_BEARING: Set<Type> = new Set([
    'Label', 'Button', 'TextEditor',
    'b', 'button', 'code', 'desc', 'em', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
    'i', 'label', 'li', 'p', 'pre', 'span', 'strong', 'td', 'text', 'th',
    'title', 'tspan',
    // Container tags can be text leaves only when their children are pure
    // string/number content. Element-bearing containers still route through
    // createCol so React can mount the nested children.
    'div', 'section', 'article', 'aside', 'header', 'footer', 'nav', 'main',
    'figure', 'figcaption', 'form', 'ul', 'ol', 'dl', 'dt', 'dd',
] as Type[]);

// ── HostConfig ──────────────────────────────────────────────────────
export const PulpHostConfig: HostConfig<
    Type, Props, Container, Instance, TextInstance, SuspenseInstance,
    HydratableInstance, PublicInstance, HostContext, UpdatePayload,
    ChildSet, TimeoutHandle, NoTimeout
> & {
    // react-reconciler 0.31+ also wants these — declared loosely.
    [key: string]: unknown;
} = {
    // ── Renderer identity ───────────────────────────────────────────
    supportsMutation: true,
    supportsPersistence: false,
    supportsHydration: false,
    isPrimaryRenderer: true,
    supportsMicrotasks: true,
    scheduleMicrotask: typeof queueMicrotask === 'function'
        ? queueMicrotask
        : (cb: () => void) => Promise.resolve().then(cb),

    // ── Timeouts (Ink pattern) ──────────────────────────────────────
    scheduleTimeout: setTimeout as unknown as (fn: () => void, delay?: number) => TimeoutHandle,
    cancelTimeout: clearTimeout as unknown as (handle: TimeoutHandle) => void,
    noTimeout: -1 as NoTimeout,

    // ── Event priority (DefaultEventPriority for v0) ────────────────
    setCurrentUpdatePriority(newPriority: EventPriority) { currentUpdatePriority = newPriority; },
    getCurrentUpdatePriority() { return currentUpdatePriority; },
    resolveUpdatePriority() {
        return currentUpdatePriority !== NoEventPriority
            ? currentUpdatePriority
            : DefaultEventPriority;
    },
    /// React 18 name for the same thing — react-reconciler@0.29 calls this
    /// from `requestUpdateLane`. Without it, every render throws
    /// "getCurrentEventPriority is not a function". Keep both names so the
    /// host config works on both 0.29 (React 18) and 0.31 (React 19).
    getCurrentEventPriority() { return DefaultEventPriority; },

    // ── Host context (no scoped state needed for v0) ────────────────
    getRootHostContext() { return {}; },
    getChildHostContext() { return {}; },

    // ── Instance lifecycle ──────────────────────────────────────────
    createInstance(
        type,
        props,
        rootContainer,
        _hostContext,
        _internalHandle,
    ): Instance {
        // CRITICAL: createInstance does NOT receive the parent. We construct
        // an unattached descriptor here and DEFER the bridge createX call
        // to appendInitialChild / appendChild, which DO receive the parent.
        // Flatten HTML/JSX-style `style` object + `className` string into
        // the flat-prop shape applyAllProps/applyChangedProps expect.
        // Native intrinsics already use flat props — normalizeHostProps
        // is a no-op fast path for them. Runtime-import bundles reach
        // prop-applier through here.
        const normalizedProps = normalizeHostProps(type, props as Record<string, unknown>);
        const id = (normalizedProps.id as string) ?? autoId(rootContainer);
        // Public instances expose the web-compat Element shim when it is
        // available. Refs need DOM-shaped methods such as getContext(),
        // getBoundingClientRect(), style setters, and addEventListener().
        // Mark the native widget as already created so shim methods route
        // to the widget id that host-config materializes later.
        let domShim: unknown = null;
        try {
            const ElementCtor = (globalThis as Record<string, unknown>).Element as
                | (new (tag: string, nativeId: string) => Record<string, unknown>)
                | undefined;
            if (typeof ElementCtor === 'function') {
                const shim = new ElementCtor(type, id);
                // Preserve semantic DOM attributes on the public Element
                // shim before marking it native. Imported application logic
                // uses selectors such as `[data-settings-open]` and
                // `[aria-label="Settings"]` to identify the same controls
                // Chromium did. These attributes are behavior identity, not
                // a second hand-authored control map.
                syncDomSemanticProps(shim, {}, props as Record<string, unknown>);
                const initialText = asText(normalizedProps.children)
                    ?? (normalizedProps.text as string | undefined);
                // React's host text fast-path does not append a Text node, so
                // the public Element shim cannot infer textContent from its
                // child list. Publish the same flattened text that native
                // createLabel/setText receives. Chromium materialization uses
                // this DOM-shaped value to attach captured line boxes to the
                // real native paint target after every commit.
                shim._textContent = initialText ?? '';
                shim._nativeCreated = true;
                shim.__pulpId = id;
                // The Element constructor seeds internal `_id`, but the
                // public `.id` getter returns an empty string until the
                // setter marks it user-visible. Calling the setter keeps
                // `ref.current.id` aligned with the native widget id.
                shim.id = id;
                domShim = shim;
                domRegistry().set(id, shim);
            }
        } catch { /* Element shim not available — pure-JS test path */ }
        return {
            id,
            type,
            // parentId stays undefined until attached
            props: { ...normalizedProps },
            childIds: [],
            onBridge: false,
            pendingChildren: [],
            _dom: domShim,
            textTargetId: String(type) === 'button'
                ? id + '__text' : undefined,
        };
    },

    createTextInstance(
        text,
        rootContainer,
        _hostContext,
        _internalHandle,
    ): TextInstance {
        // Loose text inside a non-text-bearing parent auto-wraps in a
        // synthetic Label so imported DOM-shaped markup can still render
        // instead of crashing on raw text nodes.
        if (text == null) return { id: 'text_empty', type: 'Label', props: {}, childIds: [], onBridge: false, pendingChildren: [] } as unknown as TextInstance;
        const id = autoId(rootContainer);
        // Intentionally returning a synthetic Label instance — the
        // reconciler treats it as an opaque host-text type, but our
        // appendChild path will materialize it via createLabel + setText
        // when its parent attaches.
        return {
            id,
            type: 'Label',
            props: { children: String(text), text: String(text) },
            childIds: [],
            onBridge: false,
            pendingChildren: [],
            anonymousTextTarget: true,
        } as unknown as TextInstance;
    },

    shouldSetTextContent(type, props) {
        // TEXT_BEARING marks a type as capable of bearing text directly,
        // but React should skip the child-node path only for plain text.
        // Nested markup like <span><em>x</em></span> must return false so
        // React mounts the inner element.
        if (!TEXT_BEARING.has(type)) return false;
        const children = props?.children;
        if (children == null) return true;  // empty container is text-able
        // Keep this decision identical to createWidget()/commitUpdate's
        // asText() contract. React commonly leaves null/boolean sentinels in
        // children arrays for conditional decorations. Treating those as
        // element content here while asText() flattened them caused both an
        // owning Label and a child TextInstance to paint the same caption.
        return asText(children) !== undefined;
    },

    // ── First-mount attachment ──────────────────────────────────────
    appendInitialChild(parentInstance, child) {
        attach(parentInstance, child);
    },

    finalizeInitialChildren(_instance, _type, _props, _rootContainer, _hostContext): boolean {
        // Return false — we don't need a commitMount callback. All prop
        // application happens in attach() during the append.
        return false;
    },

    // ── Mutation: append / insert / remove ──────────────────────────
    appendChild(parentInstance, child) {
        attach(parentInstance, child);
    },
    appendChildToContainer(container, child) {
        attachToRoot(container, child);
    },

    insertBefore(parentInstance, child, beforeChild) {
        const beforeIdx = parentInstance.childIds.indexOf(beforeChild.id);
        const sameParent = child.parentId === parentInstance.id && child.onBridge;
        if (sameParent) {
            // Same-parent reorder: React shuffled keyed siblings. Update
            // childIds and use the bridge's indexed insert path to keep
            // native order in sync.
            const oldIdx = parentInstance.childIds.indexOf(child.id);
            if (oldIdx >= 0) parentInstance.childIds.splice(oldIdx, 1);
            const insertIdx = beforeIdx >= 0 ? beforeIdx : parentInstance.childIds.length;
            parentInstance.childIds.splice(insertIdx, 0, child.id);
            if (typeof g.insertChild === 'function') {
                call('insertChild', parentInstance.id, child.id, insertIdx);
            } else if (typeof g.moveWidget === 'function') {
                // Older bridges: moveWidget within same parent works.
                call('moveWidget', child.id, parentInstance.id, insertIdx);
            }
            return;
        }
        attach(parentInstance, child, beforeIdx >= 0 ? beforeIdx : undefined);
    },
    insertInContainerBefore(container, child, beforeChild) {
        // Root container ordering is append-only on this path today:
        // attachToRoot materializes under the bridge root and does not
        // consult beforeChild. Non-root insertions preserve order via
        // insertBefore's insertChild / moveWidget path above.
        attachToRoot(container, child, /* index */ -1, beforeChild);
    },

    removeChild(parentInstance, child) {
        detach(parentInstance, child);
    },
    removeChildFromContainer(_container, child) {
        // Detach from root and let the bridge clean up the subtree.
        if (typeof g.removeWidget === 'function') call('removeWidget', child.id);
    },

    clearContainer(_container) {
        // No-op for v0 — React always issues per-child removals.
        // Could be optimized later by tracking a list of root children.
    },

    // ── Updates ────────────────────────────────────────────────────
    prepareUpdate(_instance, type, oldProps, newProps): UpdatePayload {
        // Flatten both sides before diffing so style/className changes
        // surface as real prop deltas (and vice versa: a flat width:100
        // → style:{width:100} swap collapses to a no-op).
        const oldN = normalizeHostProps(type, oldProps as Record<string, unknown>);
        const newN = normalizeHostProps(type, newProps as Record<string, unknown>);
        return shallowDiff(oldN, newN);
    },

    commitUpdate(instance, _updatePayload, type, oldProps, newProps, _internalHandle) {
        const oldN = normalizeHostProps(type, oldProps as Record<string, unknown>);
        const newN = normalizeHostProps(type, newProps as Record<string, unknown>);
        applyChangedProps(instance, oldN, newN);
        instance.props = { ...newN };
        if (instance._dom && typeof instance._dom === 'object') {
            syncDomSemanticProps(
                instance._dom as Record<string, unknown>,
                oldProps as Record<string, unknown>,
                newProps as Record<string, unknown>,
            );
            const dom = instance._dom as Record<string, unknown>;
            const committedText = asText(newN.children)
                ?? (newN.text as string | undefined);
            if (committedText !== undefined) dom._textContent = committedText;
            if (instance.textTargetId) dom.__pulpTextTargetId = instance.textTargetId;
            else delete dom.__pulpTextTargetId;
        }
        // Re-apply text children if changed (Label/Button special-case).
        if (TEXT_BEARING.has(instance.type)) {
            const oldText = asText(oldN.children) ?? (oldN.text as string | undefined);
            const newText = asText(newN.children) ?? (newN.text as string | undefined);
            if (oldText !== newText && newText !== undefined) {
                if (typeof g.setText === 'function') {
                    call('setText', instance.textTargetId ?? instance.id, newText);
                }
            }
        }
    },

    commitTextUpdate(_textInstance, _oldText, _newText) {
        // Unreachable — see createTextInstance.
    },

    // React's mutation reconciler calls resetTextContent when
    // shouldSetTextContent flips from true to false on an existing
    // TEXT_BEARING node, such as <span>hi</span> becoming
    // <span><em>hi</em></span>. Clear stale text before the new child
    // element mounts.
    resetTextContent(instance) {
        if (typeof g.setText === 'function') {
            call('setText', instance.textTargetId ?? instance.id, '');
        }
    },

    // ── Per-commit flush ───────────────────────────────────────────
    prepareForCommit(_container) { return null; },
    resetAfterCommit(_container) {
        // Materialized imports may install renderer-neutral Chromium evidence
        // (captured text line boxes today, with room for other stable metadata
        // later). Apply it only after React has committed the complete native
        // host tree, so structural paths see siblings in their final order.
        // The hook is optional: ordinary @pulp/react applications remain
        // independent of the import pipeline.
        const metadataHook = g.__pulpApplyMaterializedImportMetadata__;
        if (typeof metadataHook === 'function') metadataHook();
        // Own commit-time layout/repaint flush. The bridge's individual
        // setX calls don't all self-flush layout, so we trigger one
        // explicit pass per React commit. Mirrors Ink's resetAfterCommit.
        requestLayoutFlush(() => {
            if (typeof g.layout === 'function') call('layout');
        });
    },

    // ── Misc required no-ops / passthroughs ────────────────────────
    // Return the DOM-shim Element when available so `ref.current.X`
    // calls resolve to browser-shaped methods. Fall back to the Instance
    // descriptor in tests that run without the shim chain.
    getPublicInstance(instance) {
        const inst = instance as Instance & { _dom?: unknown };
        return (inst._dom ?? instance) as Instance;
    },
    preparePortalMount(_container) { /* no portals in v0 */ },
    detachDeletedInstance(_instance) { /* no extra cleanup needed */ },
    beforeActiveInstanceBlur() { /* no-op */ },
    afterActiveInstanceBlur() { /* no-op */ },
    prepareScopeUpdate() { /* no scopes in v0 */ },
    getInstanceFromScope() { return null; },
    getInstanceFromNode() { return null; },
};

// ── attach helper ──────────────────────────────────────────────────
//
// Lifecycle invariant: a child only lands on the bridge when its parent
// is already on the bridge. React calls appendInitialChild bottom-up,
// so when leaves are attached to mid-tree containers, the containers
// are not yet on the bridge — calling createX(child, parentId) at that
// point would route the child to the bridge's implicit root_ via
// resolve_parent's silent fallback. Instead, we record the attach as
// "pending" and drain it once the parent's own attach (or attachToRoot)
// flips its onBridge flag.
//
// Containers (root) ARE always on the bridge — the WidgetBridge's root_
// View exists at construction. So attachToRoot is the trigger that
// recursively flushes the pending tree.
function attach(parent: Instance, child: Instance, index?: number): void {
    const wasAttachedElsewhere = child.parentId !== undefined && child.parentId !== parent.id;

    // Bookkeeping (always runs, regardless of bridge state)
    child.parentId = parent.id;
    const insertIdx = index !== undefined && index >= 0 ? index : parent.childIds.length;
    if (insertIdx === parent.childIds.length) {
        parent.childIds.push(child.id);
    } else {
        parent.childIds.splice(insertIdx, 0, child.id);
    }
    // Keep the public DOM shims in the same hierarchy React committed without
    // calling Element.appendChild(): that method would create/reparent native
    // widgets a second time. Selector-backed imported behavior (menus,
    // dialogs, closest/contains/outside-click) still needs a truthful parent
    // chain even though host-config owns native attachment itself.
    const parentDom = parent._dom as Record<string, unknown> | null | undefined;
    const childDom = child._dom as Record<string, unknown> | null | undefined;
    if (parentDom && child.anonymousTextTarget) {
        if (!Array.isArray(parentDom.__pulpAnonymousTextTargets)) {
            parentDom.__pulpAnonymousTextTargets = [];
        }
        const targets = parentDom.__pulpAnonymousTextTargets as Array<{
            id: string; text: string;
        }>;
        const target = {
            id: child.id,
            text: String(child.props.text ?? child.props.children ?? ''),
        };
        const oldTarget = targets.findIndex(entry => entry.id === child.id);
        if (oldTarget >= 0) targets.splice(oldTarget, 1);
        // React's insertion index counts element and text children alike,
        // whereas this renderer-neutral list contains text targets only.
        // Preserve the text-node order by counting preceding text instances.
        let textIndex = 0;
        for (let childIndex = 0; childIndex < insertIdx; ++childIndex) {
            const siblingId = parent.childIds[childIndex];
            if (siblingId === child.id) continue;
            if (targets.some(entry => entry.id === siblingId)) ++textIndex;
        }
        targets.splice(Math.min(textIndex, targets.length), 0, target);
    }
    if (parentDom && childDom) {
        const oldParent = childDom._parentElement as Record<string, unknown> | null;
        if (oldParent && Array.isArray(oldParent._children)) {
            const oldIndex = oldParent._children.indexOf(childDom);
            if (oldIndex >= 0) oldParent._children.splice(oldIndex, 1);
        }
        childDom._parentElement = parentDom;
        if (!Array.isArray(parentDom._children)) parentDom._children = [];
        const children = parentDom._children as unknown[];
        const existing = children.indexOf(childDom);
        if (existing >= 0) children.splice(existing, 1);
        children.splice(Math.min(insertIdx, children.length), 0, childDom);
    }

    if (wasAttachedElsewhere && child.onBridge) {
        // Existing on-bridge widget being moved between parents.
        if (typeof g.moveWidget === 'function') {
            call('moveWidget', child.id, parent.id, insertIdx);
        } else {
            // Fallback: remove + recreate. Loses any subtree state.
            if (typeof g.removeWidget === 'function') call('removeWidget', child.id);
            child.onBridge = false;
            if (parent.onBridge) materialize(parent, child);
        }
        return;
    }

    if (parent.onBridge) {
        // Parent is live; child can land on the bridge immediately.
        materialize(parent, child);
    } else {
        // Defer until the parent itself reaches the bridge.
        parent.pendingChildren.push({ child, index: insertIdx });
    }
}

function attachToRoot(container: Container, child: Instance, _index = -1, _before?: Instance): void {
    child.parentId = container.rootId;
    // The bridge's root is always on the bridge — materialize immediately
    // and flush the entire deferred subtree.
    materializeUnder(container.rootId, child);
}

/// Emit createX + applyAllProps for a single child whose parent is on
/// the bridge, then recursively drain the child's pendingChildren.
function materialize(parent: Instance, child: Instance): void {
    materializeUnder(parent.id, child);
}

/// Forward React's dev-mode `__source` prop to the native `setSource`
/// bridge so the inspector can jump to the authoring JSX file:line.
/// Production bundles omit `__source`, making this a silent no-op.
function bindSourceLocation(child: Instance): void {
    const src = child.props.__source as
        | { fileName?: unknown; lineNumber?: unknown; columnNumber?: unknown }
        | undefined;
    if (!src || typeof src.fileName !== 'string' || src.fileName.length === 0) {
        return;
    }
    if (typeof g.setSource !== 'function') return;  // older runtime
    const line = typeof src.lineNumber === 'number' ? src.lineNumber : 0;
    const col = typeof src.columnNumber === 'number' ? src.columnNumber : 0;
    call('setSource', child.id, src.fileName, line, col);
}

function materializeUnder(parentId: string, child: Instance): void {
    if (child.onBridge) return;
    createWidget(child.type, child.id, parentId, child.props);
    if (child._dom && typeof child._dom === 'object' && child.textTargetId) {
        (child._dom as Record<string, unknown>).__pulpTextTargetId = child.textTargetId;
    }
    applyAllProps(child);
    bindSourceLocation(child);
    child.onBridge = true;
    // Drain any pending grand-children that were queued before this
    // descriptor reached the bridge.
    if (child.pendingChildren.length > 0) {
        const drained = child.pendingChildren;
        child.pendingChildren = [];
        for (const { child: gc } of drained) {
            materializeUnder(child.id, gc);
        }
    }
}

function detach(parent: Instance, child: Instance): void {
    const idx = parent.childIds.indexOf(child.id);
    if (idx >= 0) parent.childIds.splice(idx, 1);
    if (typeof g.removeWidget === 'function') call('removeWidget', child.id);
    const parentDom = parent._dom as Record<string, unknown> | null | undefined;
    const childDom = child._dom as Record<string, unknown> | null | undefined;
    if (parentDom && childDom && Array.isArray(parentDom._children)) {
        const domIndex = parentDom._children.indexOf(childDom);
        if (domIndex >= 0) parentDom._children.splice(domIndex, 1);
        childDom._parentElement = null;
    }
    domRegistry().delete(child.id);
    clearMaterializedEventCallbacks(child.id);
    child.parentId = undefined;
}

// ── Auto-ID generation ─────────────────────────────────────────────
function autoId(container: Container): string {
    const n = ++container.nextId;
    return `${container.idPrefix ?? 'pr_'}${n.toString(36)}`;
}

// ── Shallow diff (used by prepareUpdate) ───────────────────────────
function shallowDiff(a: Props, b: Props): boolean {
    const aKeys = Object.keys(a);
    const bKeys = Object.keys(b);
    if (aKeys.length !== bKeys.length) return true;
    for (const k of aKeys) if (a[k] !== b[k]) return true;
    return false;
}
