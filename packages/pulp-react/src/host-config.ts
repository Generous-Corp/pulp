// host-config.ts — react-reconciler HostConfig targeting pulp::view::WidgetBridge.
//
// Design choices (validated by Codex consult + RepoPrompt review on 2026-04-25):
//   - Mutation mode (Ink/R3F pattern), NOT persistence (RNS pattern)
//   - isPrimaryRenderer: true (Pulp is standalone — no second renderer to coexist with)
//   - shouldSetTextContent selectively (Label/Button/TextEditor only) — NOT a separate text-node type
//   - Ink-style scheduler (supportsMicrotasks: true + queueMicrotask)
//   - DEFER concurrent mode for v0
//   - createInstance does NOT receive parent — attachment happens in appendChild/appendInitialChild
//   - insertBefore requires View::insert_child(index) on the C++ side (pulp #772 bridge addition)
//   - Commit-time layout flush owned by the host config (resetAfterCommit), not the bridge

import type { HostConfig } from 'react-reconciler';
import { DefaultEventPriority } from 'react-reconciler/constants.js';

import type {
    PulpInstance,
    PulpContainer,
    IntrinsicElementName,
} from './types.js';
import { applyAllProps, applyChangedProps, normalizeHostProps } from './prop-applier.js';

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
let _hc_count = 0;
function call(name: string, ...args: unknown[]): unknown {
    const fn = g[name];
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
        case 'Meter':       call('createMeter', id, parentId); return;
        case 'Progress':    call('createProgress', id, parentId); return;
        case 'XYPad':       call('createXYPad', id, parentId); return;
        case 'Checkbox':    call('createCheckbox', id, parentId); return;
        case 'Toggle':      call('createToggle', id, parentId); return;
        case 'Combo':       call('createCombo', id, parentId); return;
        case 'ListBox':     call('createListBox', id, parentId); return;
        case 'Canvas':      call('createCanvas', id, parentId); return;
        case 'Image':       call('createImage', id, parentId); return;
        case 'Icon':        call('createIcon', id, parentId); return;
        case 'SvgPath':     call('createSvgPath', id, parentId); return;
        case 'SvgRect':     call('createSvgRect', id, parentId); return;
        case 'SvgLine':     call('createSvgLine', id, parentId); return;
        default: {
            // pulp jsx-instrument-import 2026-05-17 — lowercase HTML/SVG
            // intrinsic aliases. Lets the reconciler handle Chainer-style
            // raw JSX (<div>, <svg>, <path>, …) directly through the
            // existing bridge widgets, mirroring the web-compat shim's
            // _ensureNative tag → createX map. Per ChatGPT/Codex consult:
            // the right architecture is react-konva/r3f-style direct
            // reconciler wiring, not ReactDOM event delegation.
            const lower = String(type).toLowerCase();
            switch (lower) {
                case 'div': case 'section': case 'article': case 'aside':
                case 'header': case 'footer': case 'nav': case 'main':
                case 'figure': case 'figcaption': case 'form': case 'ul':
                case 'ol': case 'li': case 'dl': case 'dt': case 'dd': {
                    // pulp jsx-instrument-import 2026-05-17 — when a div
                    // contains ONLY string/number children (no nested
                    // ReactElements), treat it as a text-bearing leaf
                    // (createLabel with concatenated text). Matches browser
                    // inline-text behavior: `<div>+{val} ct</div>` renders
                    // text on one line, not as three stacked column siblings.
                    // Chainer's detune display `{detuneC > 0 ? "+" : ""}{detuneC} ct`
                    // depends on this — three text expressions were creating
                    // three Labels in a column that wrapped vertically.
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
                    // pulp jsx-instrument-import 2026-05-17 — text-bearing
                    // tags need to be containers when they have ReactElement
                    // children (e.g. Chainer's `<span>CHAINER /
                    // <span>polywave ms-split</span></span>`). Pure-text
                    // children → createLabel (leaf). Mixed/element children
                    // → createCol so the reconciler can appendChild the
                    // inner spans as siblings. The string portions get
                    // their own synthetic Label instances via
                    // createTextInstance.
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
                    if (typeof g.createButton === 'function') {
                        call('createButton', id, text, parentId);
                    } else {
                        call('createPanel', id, parentId);
                        call('createLabel', id + '_l', text, id);
                    }
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
                case 'svg':      call('createCol', id, parentId); return;  // SVG = container; children paint
                case 'path': {
                    call('createSvgPath', id, parentId);
                    // pulp jsx-instrument-import 2026-05-17 — lowercase
                    // <path>/<circle>/<line> children of <svg> have no
                    // intrinsic flex sizing (JSX puts width/height on the
                    // parent <svg>, children inherit via viewBox in a
                    // real browser). SvgPathWidget::paint early-returns
                    // when local_bounds is 0×0. Pin children to fill
                    // their parent svg via position:absolute + inset:0.
                    call('setPosition', id, 'absolute');
                    call('setTop', id, 0);
                    call('setLeft', id, 0);
                    call('setRight', id, 0);
                    call('setBottom', id, 0);
                    // pulp jsx-instrument-import 2026-05-17 — fill-parent
                    // makes the SVG primitive the topmost hit-test target,
                    // shadowing the parent <svg>'s onMouseDown handler.
                    // Set pointer-events: none so clicks fall through to
                    // the parent <svg> which holds the JSX handler.
                    // Matches browser SVG behavior where presentational
                    // children of <svg> don't intercept events by default
                    // (they need explicit pointer-events="visible" or
                    // similar).
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
                    // pulp jsx-instrument-import 2026-05-17 — fill-parent
                    // makes the SVG primitive the topmost hit-test target,
                    // shadowing the parent <svg>'s onMouseDown handler.
                    // Set pointer-events: none so clicks fall through to
                    // the parent <svg> which holds the JSX handler.
                    // Matches browser SVG behavior where presentational
                    // children of <svg> don't intercept events by default
                    // (they need explicit pointer-events="visible" or
                    // similar).
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
                    // pulp jsx-instrument-import 2026-05-17 — fill-parent
                    // makes the SVG primitive the topmost hit-test target,
                    // shadowing the parent <svg>'s onMouseDown handler.
                    // Set pointer-events: none so clicks fall through to
                    // the parent <svg> which holds the JSX handler.
                    // Matches browser SVG behavior where presentational
                    // children of <svg> don't intercept events by default
                    // (they need explicit pointer-events="visible" or
                    // similar).
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
                    // pulp jsx-instrument-import 2026-05-17 — fill-parent
                    // makes the SVG primitive the topmost hit-test target,
                    // shadowing the parent <svg>'s onMouseDown handler.
                    // Set pointer-events: none so clicks fall through to
                    // the parent <svg> which holds the JSX handler.
                    // Matches browser SVG behavior where presentational
                    // children of <svg> don't intercept events by default
                    // (they need explicit pointer-events="visible" or
                    // similar).
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
    // pulp #71 — React passes `<button>{count}{" bands"}</button>` as the
    // array `[count, " bands"]`. shouldSetTextContent already accepts mixed
    // string/number arrays (it lowers them to text), but asText used to bail
    // and return undefined, so commitUpdate's setText branch never fired and
    // the button label froze at its first-render value (Spectr "32 bands"
    // never advancing when the user picked a new count). Mirror
    // shouldSetTextContent: skip null/undefined/boolean entries (React's
    // standard "skip" sentinels), recurse on the rest, and bail only when an
    // entry is a real element we can't flatten to a string.
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
    // pulp jsx-instrument-import 2026-05-17 — ReactElement children
    // (`<span>CHAINER / <span>polywave ms-split</span></span>` from
    // Chainer's titlebar): return undefined HERE so shouldSetTextContent
    // returns false and the reconciler instead walks the children as
    // child node instances. The outer `<span>` then renders as a
    // container (NOT a label that swallows the children), and the
    // inner `<span>polywave ms-split</span>` gets its own createLabel.
    // Pre-fix the outer span swallowed children as text → empty string,
    // dropping the inner span's content entirely.
    return undefined;
}

/// Element types that lower their string children to setText / createLabel
/// rather than to a child node. shouldSetTextContent reads this set.
///
/// pulp #109 — HTML-intrinsic JSX tags ALSO bear their own text. Without
/// these aliases, React's host-config returned false from
/// shouldSetTextContent('span', …), causing React to materialize a
/// synthetic Label child for the string content. That synthetic child
/// stacked on top of the outer span's own auto-derived text (createWidget
/// pulls asText(props.children) into the Label dispatch), producing the
/// 2026-05-11 Spectr regression where every <span>SPECTR</span> rendered
/// as two overlapping "SPECTR" labels. Fix surfaces for EVERY imported
/// design with raw HTML text tags, not just Spectr.
const TEXT_BEARING: Set<Type> = new Set([
    'Label', 'Button', 'TextEditor',
    'b', 'button', 'code', 'desc', 'em', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
    'i', 'label', 'li', 'p', 'pre', 'span', 'strong', 'td', 'text', 'th',
    'title', 'tspan',
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
        // (Codex + RepoPrompt review both flagged the original plan that
        // tried createX(id, parent) here as the wrong lifecycle point.)
        // Flatten HTML/JSX-style `style` object + `className` string into
        // the flat-prop shape applyAllProps/applyChangedProps expect.
        // Native intrinsics already use flat props — normalizeHostProps
        // is a no-op fast path for them. Runtime-import bundles
        // (Claude/Stitch/Figma/v0/Pencil) reach prop-applier through here.
        const normalizedProps = normalizeHostProps(type, props as Record<string, unknown>);
        const id = (normalizedProps.id as string) ?? autoId(rootContainer);
        // Phase 7 codex round 5 — public-instance is an Element shim.
        // The bundle's refs (canvasRef.current, wrapRef.current) need
        // DOM-element shape: getContext('2d'), getBoundingClientRect(),
        // style setters, etc. Plain Instance descriptors fail the
        // bundle's resize useEffect with "not a function" → infinite
        // re-render loop. We instantiate the existing web-compat
        // Element class (installed by the C++ runtime-import shims) and
        // mark its native widget as already created (host-config will
        // call createWidget later via materializeUnder). _ensureNative
        // is a no-op once _nativeCreated=true, so DOM-shim methods
        // route to the existing native widget by id.
        let domShim: unknown = null;
        try {
            const ElementCtor = (globalThis as Record<string, unknown>).Element as
                | (new (tag: string, nativeId: string) => Record<string, unknown>)
                | undefined;
            if (typeof ElementCtor === 'function') {
                const shim = new ElementCtor(type, id);
                shim._nativeCreated = true;
                shim.__pulpId = id;  // non-enumerable backref (codex round 5)
                // Codex P2 follow-up on #1859: Element constructor seeds
                // internal `_id` but the public `.id` getter
                // (web-compat-element.js:259) returns `""` until the SETTER
                // runs (gated on `_userIdSet`). Calling the setter ensures
                // `ref.current.id` matches the native widget id rather than
                // appearing as an empty string — preserves prior observable
                // behavior for any consumer that reads .id off the ref.
                shim.id = id;
                domShim = shim;
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
        };
    },

    createTextInstance(
        text,
        rootContainer,
        _hostContext,
        _internalHandle,
    ): TextInstance {
        // Loose text inside a non-text-bearing parent — auto-wrap in a
        // synthetic Label so we don't crash the render. Real apps using
        // typed JSX intrinsics never hit this; the spectr#28 WebView
        // parity port hits it because the extracted editor.html has raw
        // text inside <span>/<div>/SVG nodes that lower to View parents
        // through dom-adapter. Silent auto-wrap unblocks the render and
        // lets the visible widgets land. Diagnostic warning still fires
        // in dev builds.
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
        } as unknown as TextInstance;
    },

    shouldSetTextContent(type, props) {
        // pulp #1836 P1 (Codex follow-up) — TEXT_BEARING marks a type as
        // CAPABLE of bearing text directly, but the children must
        // actually be string/number for React to skip the child-node
        // path. For nested markup like <span><em>x</em></span>, the
        // children prop is an element (or array of mixed nodes), so
        // returning true here would cause React to drop the inner <em>.
        // Mirror React DOM's behavior: only short-circuit when children
        // are plain text scalars (string / number) or arrays of only
        // string/number scalars.
        if (!TEXT_BEARING.has(type)) return false;
        const children = props?.children;
        if (children == null) return true;  // empty container is text-able
        if (typeof children === 'string' || typeof children === 'number') return true;
        if (Array.isArray(children)) {
            for (const c of children) {
                if (c == null) continue;
                if (typeof c !== 'string' && typeof c !== 'number') return false;
            }
            return true;
        }
        return false;
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
            // Same-parent reorder — React shuffled keyed siblings.
            // Call the bridge's insertChild(parent_id, child_id, index)
            // (added in pulp PR #779) to update native order. Update
            // our childIds bookkeeping too. Codex P1 review on PR #779.
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
        // Container has no per-instance childIds tracking by default —
        // bridge handles ordering via insertChild on the root parent.
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
        // Re-apply text children if changed (Label/Button special-case).
        if (TEXT_BEARING.has(instance.type)) {
            const oldText = asText(oldN.children) ?? (oldN.text as string | undefined);
            const newText = asText(newN.children) ?? (newN.text as string | undefined);
            if (oldText !== newText && newText !== undefined) {
                if (typeof g.setText === 'function') call('setText', instance.id, newText);
            }
        }
    },

    commitTextUpdate(_textInstance, _oldText, _newText) {
        // Unreachable — see createTextInstance.
    },

    // pulp #1840 P1 (Codex follow-up) — React's mutation reconciler
    // calls resetTextContent(instance) when shouldSetTextContent flips
    // from true → false on an existing TEXT_BEARING node. Concretely,
    // a transition like <span>hi</span> → <span><em>hi</em></span>:
    // the old commit treated <span> as text-bearing and pushed "hi" via
    // setText; the new commit needs the inner <em> child mounted, so
    // React first asks the host to clear the stale text. Without this
    // hook the reconciler can throw (or leave stale text un-cleared on
    // hosts that tolerate the missing callback). Clear by calling
    // setText(id, '') — which the bridge already handles as a no-op for
    // non-text-capable types, so this is safe for the whole alias set.
    resetTextContent(instance) {
        if (typeof g.setText === 'function') call('setText', instance.id, '');
    },

    // ── Per-commit flush ───────────────────────────────────────────
    prepareForCommit(_container) { return null; },
    resetAfterCommit(_container) {
        // Own commit-time layout/repaint flush. The bridge's individual
        // setX calls don't all self-flush layout, so we trigger one
        // explicit pass per React commit. Mirrors Ink's resetAfterCommit.
        if (typeof g.layout === 'function') call('layout');
    },

    // ── Misc required no-ops / passthroughs ────────────────────────
    // Phase 7 codex round 5 — return the DOM-shim element when
    // available so the bundle's `ref.current.X` calls resolve to
    // browser-DOM-shape methods (getContext, getBoundingClientRect,
    // style setters, addEventListener, etc.). Falls back to the
    // Instance descriptor for tests that run without the C++ shim
    // chain. The PulpInstance return type is technically wrong (we
    // return an Element or PulpInstance), but react-reconciler's
    // types are inflexible here and we cast at the call site.
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

function materializeUnder(parentId: string, child: Instance): void {
    if (child.onBridge) return;
    createWidget(child.type, child.id, parentId, child.props);
    applyAllProps(child);
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
    child.parentId = undefined;
}

// ── Auto-ID generation ─────────────────────────────────────────────
function autoId(container: Container): string {
    const n = ++container.nextId;
    return `pr_${n.toString(36)}`;
}

// ── Shallow diff (used by prepareUpdate) ───────────────────────────
function shallowDiff(a: Props, b: Props): boolean {
    const aKeys = Object.keys(a);
    const bKeys = Object.keys(b);
    if (aKeys.length !== bKeys.length) return true;
    for (const k of aKeys) if (a[k] !== b[k]) return true;
    return false;
}
