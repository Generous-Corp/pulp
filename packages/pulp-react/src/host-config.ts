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
import { applyAllProps, applyChangedProps } from './prop-applier.js';

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
            // Should be unreachable thanks to the typed intrinsics in types.ts.
            const _exhaustive: never = type;
            throw new Error('@pulp/react: unknown intrinsic type: ' + String(_exhaustive));
        }
    }
}

function asText(children: unknown): string | undefined {
    if (typeof children === 'string') return children;
    if (typeof children === 'number') return String(children);
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
        const id = (props.id as string) ?? autoId(rootContainer);
        return {
            id,
            type,
            // parentId stays undefined until attached
            props: { ...props },
            childIds: [],
            onBridge: false,
            pendingChildren: [],
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
    prepareUpdate(_instance, _type, oldProps, newProps): UpdatePayload {
        // Cheap shallow inequality check. Returning truthy schedules
        // commitUpdate; falsy skips it. We do the actual diff inside
        // commitUpdate via applyChangedProps for symmetry with mount.
        return shallowDiff(oldProps, newProps);
    },

    commitUpdate(instance, _updatePayload, _type, oldProps, newProps, _internalHandle) {
        applyChangedProps(instance, oldProps, newProps);
        instance.props = { ...newProps };
        // Re-apply text children if changed (Label/Button special-case).
        if (TEXT_BEARING.has(instance.type)) {
            const oldText = asText(oldProps.children) ?? (oldProps.text as string | undefined);
            const newText = asText(newProps.children) ?? (newProps.text as string | undefined);
            if (oldText !== newText && newText !== undefined) {
                if (typeof g.setText === 'function') call('setText', instance.id, newText);
            }
        }
    },

    commitTextUpdate(_textInstance, _oldText, _newText) {
        // Unreachable — see createTextInstance.
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
    getPublicInstance(instance) { return instance; },
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
