// A controlled value control (a `value` or `checked` prop) shows the committed
// prop after a discrete edit, as a controlled React DOM input does. The native
// widget has already moved when it reports the edit, so a handler that rejects
// it leaves nothing to commit; the bridge's after-dispatch hook then writes the
// committed prop back. An accepted or clamped edit is written by its commit
// and needs no second write.
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import React, { act, useState } from 'react';
import { createRoot, render, unmount } from '../src/index.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';

const h = React.createElement;

type Listener = (...args: unknown[]) => unknown;
type BatchHook = <A, R>(fn: (arg: A) => R, arg: A) => R;
const hooks = globalThis as unknown as {
    __pulpBatchUpdates__?: BatchHook;
    __pulpAfterDiscreteDispatch__?: (id: string, eventName: string) => void;
    isWidgetParamBound?: (id: string) => boolean;
};

describe('@pulp/react controlled value restore', () => {
    let bridge: MockBridge;
    let restoreEnabled = true;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
        restoreEnabled = true;
    });
    afterEach(() => {
        bridge.uninstall();
        delete hooks.isWidgetParamBound;
    });

    /// The native side of one widget event, as `__dispatch__` runs it: the
    /// listener inside one batch, then the after-dispatch hook.
    function dispatch(id: string, eventName: string, payload: number): void {
        const listener = [...bridge.calls].reverse().find(
            (c) => c.fn === 'on' && c.args[0] === id && c.args[1] === eventName);
        expect(listener).toBeDefined();
        act(() => {
            hooks.__pulpBatchUpdates__!(() => (listener!.args[2] as Listener)(payload), undefined);
        });
        if (restoreEnabled) hooks.__pulpAfterDiscreteDispatch__!(id, eventName);
    }

    const valueWrites = (id: string, from = 0) => bridge.calls.slice(from)
        .filter((c) => c.fn === 'setValue' && c.args[0] === id)
        .map((c) => c.args[1]);

    function mount(element: React.ReactElement) {
        const root = createRoot('controlled');
        act(() => { render(element, root); });
        return root;
    }

    it('installs the hook the bridge calls after an outermost dispatch', () => {
        expect(typeof hooks.__pulpAfterDiscreteDispatch__).toBe('function');
    });

    it('puts a rejected edit back to the committed prop', () => {
        const root = mount(h('Knob', { id: 'k', value: 0.5, onChange: () => {} }));
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.8);
        expect(valueWrites('k', mark)).toEqual([0.5]);
        act(() => unmount(root));
    });

    it('writes nothing when the edit reported the value the prop already holds', () => {
        const root = mount(h('Knob', { id: 'k', value: 0.5, onChange: () => {} }));
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.5);
        expect(valueWrites('k', mark)).toEqual([]);
        act(() => unmount(root));
    });

    it('restores after a setState to the value the control already had', () => {
        const App = () => {
            const [v, setV] = useState(0.5);
            return h('Knob', { id: 'k', value: v, onChange: () => setV(0.5) });
        };
        const root = mount(h(App));
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.8);
        expect(valueWrites('k', mark)).toEqual([0.5]);
        act(() => unmount(root));
    });

    it('writes a clamped edit once, from its commit', () => {
        const App = () => {
            const [v, setV] = useState(0.5);
            return h('Knob', {
                id: 'k', value: v,
                onChange: (e: { value: number }) => setV(Math.min(e.value, 0.6)),
            });
        };
        const root = mount(h(App));
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.8);
        expect(valueWrites('k', mark)).toEqual([0.6]);
        act(() => unmount(root));
    });

    it('writes an accepted edit once, from its commit', () => {
        const App = () => {
            const [v, setV] = useState(0.5);
            return h('Knob', { id: 'k', value: v, onChange: (e: { value: number }) => setV(e.value) });
        };
        const root = mount(h(App));
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.55);
        expect(valueWrites('k', mark)).toEqual([0.55]);
        act(() => unmount(root));
    });

    it.each([
        ['defaultValue', { defaultValue: 0.5 }],
        ['no value prop', {}],
    ])('leaves an uncontrolled control (%s) where the user put it', (_label, extra) => {
        const root = mount(h('Knob', { id: 'k', onChange: () => {}, ...extra }));
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.8);
        expect(valueWrites('k', mark)).toEqual([]);
        act(() => unmount(root));
    });

    it('leaves a parameter-bound control to its binding', () => {
        hooks.isWidgetParamBound = (id) => id === 'k';
        const root = mount(h('Knob', { id: 'k', value: 0.5, onChange: () => {} }));
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.8);
        expect(valueWrites('k', mark)).toEqual([]);
        act(() => unmount(root));
    });

    it('puts a rejected toggle back to the committed checked prop', () => {
        const root = mount(h('Toggle', { id: 't', checked: false, onToggle: () => {} }));
        const mark = bridge.calls.length;
        dispatch('t', 'toggle', 1);
        expect(valueWrites('t', mark)).toEqual([0]);
        act(() => unmount(root));
    });

    it('writes the checked prop to the widget', () => {
        const root = mount(h('Checkbox', { id: 'c', checked: true, onChange: () => {} }));
        expect(valueWrites('c')).toEqual([1]);
        act(() => unmount(root));
    });

    it('writes nothing to a control the handler unmounted', () => {
        const App = () => {
            const [shown, setShown] = useState(true);
            return shown
                ? h('Knob', { id: 'k', value: 0.5, onChange: () => setShown(false) })
                : h('View', { id: 'empty' });
        };
        const root = mount(h(App));
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.8);
        expect(bridge.calls.slice(mark)).toContainEqual({ fn: 'removeWidget', args: ['k'] });
        expect(valueWrites('k', mark)).toEqual([]);
        act(() => unmount(root));
    });

    it('restores without dispatching the change event again', () => {
        let changes = 0;
        const root = mount(h('Knob', { id: 'k', value: 0.5, onChange: () => { changes += 1; } }));
        dispatch('k', 'change', 0.8);
        expect(changes).toBe(1);
        // A second edit is judged on its own: the first left nothing pending.
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.7);
        expect(changes).toBe(2);
        expect(valueWrites('k', mark)).toEqual([0.5]);
        act(() => unmount(root));
    });

    it('reads the native value back when the restore is disabled', () => {
        // Negative control for the cases above: without the after-dispatch
        // hook a rejected edit and a rejected toggle write nothing at all.
        restoreEnabled = false;
        const root = mount(h('View', { id: 'panel' },
            h('Knob', { id: 'k', value: 0.5, onChange: () => {} }),
            h('Toggle', { id: 't', checked: false, onToggle: () => {} })));
        const mark = bridge.calls.length;
        dispatch('k', 'change', 0.8);
        dispatch('t', 'toggle', 1);
        expect(valueWrites('k', mark)).toEqual([]);
        expect(valueWrites('t', mark)).toEqual([]);
        act(() => unmount(root));
    });
});
