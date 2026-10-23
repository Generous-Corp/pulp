// A suspended <Suspense> boundary keeps its primary children mounted and asks
// the renderer to hide them (hideInstance / hideTextInstance), then reveals
// them on resolve (unhideInstance / unhideTextInstance). Under LegacyRoot this
// happens on the FIRST suspension too: the partial primary tree is committed
// hidden beside the fallback. A host config without those methods throws at
// commit and mounts nothing at all.
//
// The hidden state belongs to the boundary, not the author: a widget the
// author hid stays hidden after the reveal, and an authored write that lands
// while the boundary is suspended (a prop change or a removed style key) must
// not reveal it early.
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import React, { act, Suspense, useState } from 'react';
import { createRoot, render, unmount } from '../src/index.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { PulpHostConfig } from '../src/host-config.js';
import { applyChangedProps } from '../src/prop-applier.js';
import { makeSyntheticEvent } from '../src/synthetic-event.js';

const h = React.createElement;

/// A child that suspends on a promise the test resolves by hand.
function makeGate(initiallyPending = true) {
    let resolve!: () => void;
    let pending = initiallyPending;
    let promise = new Promise<void>((r) => { resolve = r; });
    const Gate = (): React.ReactElement => {
        if (pending) throw promise;
        return h('Label', { id: 'ready', text: 'ready' });
    };
    return {
        Gate,
        async open() {
            pending = false;
            await act(async () => { resolve(); await promise; });
        },
        close() {
            pending = true;
            promise = new Promise<void>((r) => { resolve = r; });
        },
    };
}

describe('@pulp/react Suspense host visibility', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => bridge.uninstall());

    const created = (id: string) => bridge.calls.some(
        (c) => c.fn.startsWith('create') && c.args[0] === id);
    const removed = (id: string) => bridge.calls.some(
        (c) => c.fn === 'removeWidget' && c.args[0] === id);
    const visibilityWrites = (id: string) => bridge.calls
        .filter((c) => c.fn === 'setVisible' && c.args[0] === id)
        .map((c) => c.args[1]);
    /// The native widget's visibility after replaying the writes made since
    /// it was last created: a widget starts visible, a write to an id with
    /// no widget yet does nothing, and the last setVisible wins.
    const nativeVisible = (id: string) => {
        let visible = true;
        for (const c of bridge.calls) {
            if (c.args[0] !== id) continue;
            if (c.fn.startsWith('create')) visible = true;
            else if (c.fn === 'setVisible') visible = c.args[1] === true;
        }
        return visible;
    };

    it('commits a first suspension with the primary hidden beside the fallback', async () => {
        const gate = makeGate();
        const root = createRoot('suspense-first');
        // The boundary sits inside a fresh outer View, so the primary widget
        // reaches the bridge only when the outer View is placed, after the
        // reconciler has already asked for it to be hidden.
        let thrown: unknown = null;
        try {
            act(() => {
                render(h('View', { id: 'outer' },
                    h(Suspense, { fallback: h('Label', { id: 'fallback', text: 'loading' }) },
                        h('View', { id: 'primary' }, h(gate.Gate)))), root);
            });
        } catch (error) {
            thrown = error;
        }
        expect(thrown).toBeNull();
        expect(created('primary')).toBe(true);
        expect(created('fallback')).toBe(true);
        expect(nativeVisible('primary')).toBe(false);
        expect(nativeVisible('fallback')).toBe(true);

        await gate.open();
        expect(nativeVisible('primary')).toBe(true);
        expect(created('ready')).toBe(true);
        expect(removed('fallback')).toBe(true);
        act(() => unmount(root));
    });

    it('hides resolved content on re-suspension and reveals the same widgets', async () => {
        const gate = makeGate(false);
        const root = createRoot('suspense-again');
        let showGate!: (v: boolean) => void;
        const App = () => {
            const [withGate, setWithGate] = useState(false);
            showGate = setWithGate;
            return h(Suspense, { fallback: h('Label', { id: 'fallback', text: 'loading' }) },
                h('View', { id: 'primary' }, withGate ? h(gate.Gate) : h('Label', { id: 'idle', text: 'idle' })));
        };
        act(() => { render(h(App), root); });
        expect(nativeVisible('primary')).toBe(true);
        expect(created('fallback')).toBe(false);

        gate.close();
        act(() => showGate(true));
        expect(nativeVisible('primary')).toBe(false);
        expect(created('fallback')).toBe(true);
        expect(removed('primary')).toBe(false);
        expect(bridge.calls.filter((c) => c.fn.startsWith('create') && c.args[0] === 'primary'))
            .toHaveLength(1);

        await gate.open();
        expect(nativeVisible('primary')).toBe(true);
        expect(removed('fallback')).toBe(true);
        expect(removed('primary')).toBe(false);
        act(() => unmount(root));
    });

    it.each([
        ['visible={false}', { visible: false }],
        ["style display: 'none'", { style: { display: 'none' } }],
    ])('keeps an authored hide (%s) through hide and reveal', async (_label, authored) => {
        const gate = makeGate();
        const root = createRoot('suspense-authored');
        act(() => {
            render(h(Suspense, { fallback: h('Label', { id: 'fallback', text: 'loading' }) },
                h('View', { id: 'primary', ...authored }, h(gate.Gate))), root);
        });
        expect(nativeVisible('primary')).toBe(false);

        await gate.open();
        // The reveal ran: the boundary's own content arrived.
        expect(created('ready')).toBe(true);
        expect(removed('fallback')).toBe(true);
        expect(nativeVisible('primary')).toBe(false);
        act(() => unmount(root));
    });

    it.each([
        ['a visible prop flipped to true', { visible: false }, { visible: true }],
        ["a removed display: 'none' style key", { style: { display: 'none' } }, { style: {} }],
        ['a removed visible prop', { visible: false }, {}],
    ])('holds the hide while suspended through %s', async (_label, before, after) => {
        const gate = makeGate();
        const root = createRoot('suspense-write-under-hide');
        let setShown!: (v: boolean) => void;
        const App = () => {
            const [shown, setShownState] = useState(false);
            setShown = setShownState;
            // opacity changes with every flip, so the update reaching the
            // hidden widget is observable on its own.
            const props = shown
                ? { id: 'primary', opacity: 0.5, ...after }
                : { id: 'primary', opacity: 0.25, ...before };
            return h(Suspense, { fallback: h('Label', { id: 'fallback', text: 'loading' }) },
                h('View', props, h(gate.Gate)));
        };
        act(() => { render(h(App), root); });
        expect(nativeVisible('primary')).toBe(false);

        act(() => setShown(true));
        // Control: the commit did reach the hidden widget's props.
        expect(bridge.calls.some((c) => c.fn === 'setOpacity'
            && c.args[0] === 'primary' && c.args[1] === 0.5)).toBe(true);
        expect(nativeVisible('primary')).toBe(false);

        await gate.open();
        expect(removed('fallback')).toBe(true);
        expect(nativeVisible('primary')).toBe(true);
        act(() => unmount(root));
    });

    // Under LegacyRoot any commit that reaches a suspended boundary's primary
    // tree retries it, and the retry hides the top-most widgets again, so the
    // React-driven cases above cannot tell a gated applier from an ungated
    // one mid-suspension: the bridge batch keeps only the boundary's final
    // write. These drive the appliers directly on a held widget, as a commit
    // that did not re-run the boundary would.
    it.each([
        ['the visible arm', { visible: false }, { visible: true }],
        ['the display arm', { display: 'none' }, { display: 'flex' }],
        ['the visible reset', { visible: false }, {}],
        ['the display reset', { display: 'none' }, {}],
    ])('lets no authored write through %s reveal a held widget', (_label, before, after) => {
        const instance = PulpHostConfig.createInstance(
            'View' as never, { id: 'held', ...before } as never,
            { rootId: 'root', nextId: 0 } as never, {} as never, null as never);
        PulpHostConfig.appendChildToContainer!({ rootId: 'root', nextId: 0 } as never, instance);
        PulpHostConfig.hideInstance!(instance);
        bridge.reset();

        applyChangedProps(instance, { ...before, opacity: 0.25 }, { ...after, opacity: 0.5 });
        // Control: the change was applied, and only the visibility was held.
        expect(bridge.calls).toContainEqual({ fn: 'setOpacity', args: ['held', 0.5] });
        expect(visibilityWrites('held')).toEqual([]);

        PulpHostConfig.unhideInstance!(instance, after as never);
        expect(visibilityWrites('held')).toEqual([true]);
    });

    it('holds a handler style.visibility write on a held widget', () => {
        const instance = PulpHostConfig.createInstance(
            'View' as never, { id: 'held' } as never,
            { rootId: 'root', nextId: 0 } as never, {} as never, null as never);
        PulpHostConfig.appendChildToContainer!({ rootId: 'root', nextId: 0 } as never, instance);
        PulpHostConfig.hideInstance!(instance);
        bridge.reset();

        const style = makeSyntheticEvent('held', 'click', []).currentTarget.style as Record<string, unknown>;
        style.visibility = 'hidden';
        style.opacity = 0.5;
        expect(bridge.calls).toContainEqual({ fn: 'setOpacity', args: ['held', 0.5] });
        expect(visibilityWrites('held')).toEqual([]);

        PulpHostConfig.unhideInstance!(instance, {} as never);
        expect(visibilityWrites('held')).toEqual([false]);
    });

    it('applies a reused id\'s own visibility after its boundary unmounted while suspended', () => {
        const gate = makeGate();
        const root = createRoot('suspense-remount');
        let setMounted!: (v: boolean) => void;
        const App = () => {
            const [mounted, setMountedState] = useState(true);
            setMounted = setMountedState;
            return mounted
                ? h('View', { key: 'boundary', id: 'outer' },
                    h(Suspense, { fallback: h('Label', { id: 'fallback', text: 'loading' }) },
                        h('View', { id: 'primary' }, h(gate.Gate))))
                : h('View', { key: 'plain', id: 'primary', visible: false });
        };
        act(() => { render(h(App), root); });
        expect(nativeVisible('primary')).toBe(false);

        act(() => setMounted(false));
        expect(removed('outer')).toBe(true);
        const creates = bridge.calls
            .map((c, at) => ({ ...c, at }))
            .filter((c) => c.fn.startsWith('create') && c.args[0] === 'primary');
        expect(creates).toHaveLength(2);
        // The new widget's own hide reaches the bridge: no stale hold from
        // the widget that left swallows it.
        const afterRemount = bridge.calls.slice(creates[1].at)
            .filter((c) => c.fn === 'setVisible' && c.args[0] === 'primary')
            .map((c) => c.args[1]);
        expect(afterRemount).toEqual([false]);
        act(() => unmount(root));
    });

    it('hides a text instance under the boundary on its label id', async () => {
        const gate = makeGate();
        const root = createRoot('suspense-text');
        act(() => {
            render(h('View', { id: 'outer' },
                h(Suspense, { fallback: h('Label', { id: 'fallback', text: 'loading' }) },
                    'loose text', h(gate.Gate))), root);
        });
        const textLabel = bridge.calls.find((c) => c.fn === 'createLabel'
            && bridge.calls.some((t) => t.fn === 'setText'
                && t.args[0] === c.args[0] && t.args[1] === 'loose text'));
        expect(textLabel).toBeDefined();
        const textId = textLabel!.args[0] as string;
        expect(nativeVisible(textId)).toBe(false);

        await gate.open();
        expect(nativeVisible(textId)).toBe(true);
        act(() => unmount(root));
    });
});
