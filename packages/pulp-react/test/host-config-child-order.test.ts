import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { PulpHostConfig } from '../src/host-config.js';

// Replays recorded bridge calls with the native factory's real semantics:
// every createX appends to its parent, `insertChild` repositions an existing
// child, and `removeWidget` detaches one. Asserting against this model rather
// than against the call list is the point — a renderer can emit a perfectly
// reasonable-looking sequence of calls and still leave the widgets in the
// wrong order, which is the defect this file covers.
function nativeChildrenOf(calls: readonly { fn: string; args: unknown[] }[], parentId: string): string[] {
    const children = new Map<string, string[]>();
    const parentOf = new Map<string, string>();
    const detach = (id: string): void => {
        const owner = parentOf.get(id);
        if (owner === undefined) return;
        const siblings = children.get(owner) ?? [];
        const at = siblings.indexOf(id);
        if (at >= 0) siblings.splice(at, 1);
        parentOf.delete(id);
    };
    for (const { fn, args } of calls) {
        if (fn === 'createCol') {
            const [id, pid] = args as [string, string];
            detach(id);
            if (!children.has(pid)) children.set(pid, []);
            children.get(pid)!.push(id);
            parentOf.set(id, pid);
        } else if (fn === 'insertChild') {
            const [pid, id, index] = args as [string, string, number];
            if (parentOf.get(id) !== pid) continue;  // native fails closed
            const siblings = children.get(pid)!;
            siblings.splice(siblings.indexOf(id), 1);
            siblings.splice(Math.min(index, siblings.length), 0, id);
        } else if (fn === 'removeWidget') {
            detach((args as [string])[0]);
        }
    }
    return children.get(parentId) ?? [];
}

const container = () => ({ rootId: 'root', nextId: 0 }) as never;
const view = (id: string) => PulpHostConfig.createInstance(
    'View' as never, { id } as never, container(), {} as never, null as never,
);

describe('materialized widget-bridge child ordering', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => bridge.uninstall());

    it('emits the widget factory calls the order model reads', () => {
        // Positive control for nativeChildrenOf: a run that recorded no
        // createCol would report an empty order for every assertion below,
        // and an empty order matches nothing rather than failing loudly.
        const root = view('panel');
        PulpHostConfig.appendInitialChild!(root, view('only'));
        PulpHostConfig.appendChildToContainer!(container(), root);
        expect(bridge.calls.filter(c => c.fn === 'createCol').length).toBe(2);
        expect(nativeChildrenOf(bridge.calls, 'panel')).toEqual(['only']);
    });

    it('places a child that mounts late into a live parent', () => {
        const panel = view('panel');
        const caption = view('caption');
        const footer = view('footer');
        PulpHostConfig.appendInitialChild!(panel, caption);
        PulpHostConfig.appendInitialChild!(panel, footer);
        PulpHostConfig.appendChildToContainer!(container(), panel);

        // The parent is live and already full. A newly created child inserted
        // between two existing siblings is the case the factory cannot express
        // on its own: createX can only append.
        PulpHostConfig.insertBefore!(panel, view('description'), footer);

        expect(nativeChildrenOf(bridge.calls, 'panel'))
            .toEqual(['caption', 'description', 'footer']);
    });

    it('restores authored order when a subtree unmounts and remounts', () => {
        const panel = view('panel');
        const caption = view('caption');
        const body = view('body');
        const footer = view('footer');
        PulpHostConfig.appendInitialChild!(panel, caption);
        PulpHostConfig.appendInitialChild!(panel, body);
        PulpHostConfig.appendInitialChild!(panel, footer);
        PulpHostConfig.appendChildToContainer!(container(), panel);
        expect(nativeChildrenOf(bridge.calls, 'panel'))
            .toEqual(['caption', 'body', 'footer']);

        // Closing and reopening a dropdown unmounts its content and mounts a
        // fresh instance back between the siblings that stayed.
        PulpHostConfig.removeChild!(panel, body);
        PulpHostConfig.insertBefore!(panel, view('body2'), footer);

        expect(nativeChildrenOf(bridge.calls, 'panel'))
            .toEqual(['caption', 'body2', 'footer']);
    });

    it('reorders keyed siblings that are already on the bridge', () => {
        const panel = view('panel');
        const first = view('first');
        const second = view('second');
        PulpHostConfig.appendInitialChild!(panel, first);
        PulpHostConfig.appendInitialChild!(panel, second);
        PulpHostConfig.appendChildToContainer!(container(), panel);

        PulpHostConfig.insertBefore!(panel, second, first);

        expect(nativeChildrenOf(bridge.calls, 'panel')).toEqual(['second', 'first']);
    });

    it('drains a deferred subtree in authored order, not arrival order', () => {
        // A parent that has not reached the bridge queues its children. The
        // queue records arrival, so anything inserted into it before the parent
        // materializes must still create in the order the author wrote.
        const panel = view('panel');
        const caption = view('caption');
        const footer = view('footer');
        PulpHostConfig.appendInitialChild!(panel, caption);
        PulpHostConfig.appendInitialChild!(panel, footer);
        PulpHostConfig.insertBefore!(panel, view('description'), footer);
        PulpHostConfig.appendChildToContainer!(container(), panel);

        expect(nativeChildrenOf(bridge.calls, 'panel'))
            .toEqual(['caption', 'description', 'footer']);
    });

    it('still mounts against a host with no indexed insert', () => {
        // One renderer bundle runs on hosts of both vintages. Without the
        // native function, ordering degrades to append instead of throwing.
        const saved = (globalThis as Record<string, unknown>).insertChild;
        (globalThis as Record<string, unknown>).insertChild = undefined;
        try {
            const panel = view('panel');
            const caption = view('caption');
            const footer = view('footer');
            PulpHostConfig.appendInitialChild!(panel, caption);
            PulpHostConfig.appendInitialChild!(panel, footer);
            PulpHostConfig.appendChildToContainer!(container(), panel);
            expect(() => PulpHostConfig.insertBefore!(panel, view('description'), footer))
                .not.toThrow();
            expect(nativeChildrenOf(bridge.calls, 'panel')).toContain('description');
        } finally {
            (globalThis as Record<string, unknown>).insertChild = saved;
        }
    });
});
