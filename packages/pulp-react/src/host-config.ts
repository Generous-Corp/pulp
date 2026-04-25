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

// ── element-name → bridge createX dispatch ──────────────────────────
function createWidget(type: Type, id: string, parentId: string, props: Props): void {
    switch (type) {
        case 'View':
        case 'Col':         return createCol(id, parentId);
        case 'Row':         return createRow(id, parentId);
        case 'Panel':       return createPanel(id, parentId);
        case 'Label':       return createLabel(id, asText(props.children) ?? (props.text as string ?? ''), parentId);
        case 'Button':      return createButton
                                ? createButton(id, asText(props.children) ?? (props.text as string ?? ''), parentId)
                                : (createPanel(id, parentId), createLabel(id + '_l', asText(props.children) ?? '', id));
        case 'TextEditor':  return createTextEditor(id, parentId);
        case 'ScrollView':  return createScrollView(id, parentId);
        case 'Modal':       return createModal(id, parentId);
        case 'Knob':        return createKnob(id, parentId);
        case 'Fader':       return createFader(id, (props.orientation as 'vertical' | 'horizontal') ?? 'vertical', parentId);
        case 'Spectrum':    return createSpectrum(id, parentId);
        case 'Waveform':    return createWaveform(id, parentId);
        case 'Meter':       return createMeter(id, parentId);
        case 'Progress':    return createProgress(id, parentId);
        case 'XYPad':       return createXYPad(id, parentId);
        case 'Checkbox':    return createCheckbox(id, parentId);
        case 'Toggle':      return createToggle(id, parentId);
        case 'Combo':       return createCombo(id, parentId);
        case 'ListBox':     return createListBox(id, parentId);
        case 'Canvas':      return createCanvas(id, parentId);
        case 'Image':       return createImage(id, parentId);
        case 'Icon':        return createIcon(id, parentId);
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
const TEXT_BEARING: Set<Type> = new Set(['Label', 'Button', 'TextEditor']);

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
        };
    },

    createTextInstance(
        _text,
        _rootContainer,
        _hostContext,
        _internalHandle,
    ): TextInstance {
        // We never get here because shouldSetTextContent returns true
        // for the only types that accept string children (Label/Button/
        // TextEditor). If a string lands somewhere unexpected, throwing
        // is louder than silently dropping.
        throw new Error('@pulp/react: text outside a text-bearing parent (Label/Button/TextEditor) is not supported. Wrap text in <Label>...</Label>.');
    },

    shouldSetTextContent(type, _props) {
        return TEXT_BEARING.has(type);
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
        if (typeof removeWidget === 'function') removeWidget(child.id);
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
                if (typeof setText === 'function') setText(instance.id, newText);
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
        if (typeof layout === 'function') layout();
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
function attach(parent: Instance, child: Instance, index?: number): void {
    const wasAttachedElsewhere = child.parentId !== undefined && child.parentId !== parent.id;
    if (wasAttachedElsewhere) {
        // Existing widget being moved between parents — use moveWidget if
        // the bridge exposes it, else fall back to remove + recreate.
        if (typeof moveWidget === 'function') {
            moveWidget(child.id, parent.id, index ?? parent.childIds.length);
        } else {
            if (typeof removeWidget === 'function') removeWidget(child.id);
            createWidget(child.type, child.id, parent.id, child.props);
            applyAllProps(child);
        }
    } else if (child.parentId === undefined) {
        // First-time attach.
        createWidget(child.type, child.id, parent.id, child.props);
        applyAllProps(child);
        // Text-bearing widgets that took text-as-child need a setText if
        // we already set it via createX — Label/Button do this correctly
        // because createWidget passed the text into createLabel/createButton
        // directly. So this is a no-op for first-mount text.
    } else {
        // Same parent, just reordering — needs insertChild bridge fn.
        // For v0 this is treated as a no-op if the bridge doesn't expose
        // it; React will still see correct order in our childIds bookkeeping.
        // TODO: wire the (yet-to-be-added) insertChild(parent_id, child_id, index) bridge call.
    }

    child.parentId = parent.id;
    if (index !== undefined && index >= 0) {
        parent.childIds.splice(index, 0, child.id);
    } else {
        parent.childIds.push(child.id);
    }
}

function attachToRoot(container: Container, child: Instance, index = -1, _before?: Instance): void {
    if (child.parentId === undefined) {
        createWidget(child.type, child.id, container.rootId, child.props);
        applyAllProps(child);
    }
    child.parentId = container.rootId;
    // Container child order tracking is implicit via React's commit order
    // for v0; reorder support comes in with insertChild + container.childIds.
    void index;
}

function detach(parent: Instance, child: Instance): void {
    const idx = parent.childIds.indexOf(child.id);
    if (idx >= 0) parent.childIds.splice(idx, 1);
    if (typeof removeWidget === 'function') removeWidget(child.id);
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
