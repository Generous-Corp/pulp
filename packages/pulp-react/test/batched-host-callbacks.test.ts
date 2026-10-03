// Host-driven callbacks (requestAnimationFrame, timers) run through
// globalThis.__pulpBatchUpdates__, which WidgetBridge's frame/timer pump calls
// around each callback. Under LegacyRoot every setState outside a batch commits
// synchronously, so a callback that sets N pieces of state would commit N
// times; inside the hook it commits once.
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import React, { act, useState } from 'react';
import { createRoot, render, unmount } from '../src/index.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';

const h = React.createElement;

type BatchHook = <A, R>(fn: (arg: A) => R, arg: A) => R;
const batchHook = () =>
    (globalThis as { __pulpBatchUpdates__?: BatchHook }).__pulpBatchUpdates__;

describe('@pulp/react batched host callbacks', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => bridge.uninstall());

    function mountCounter() {
        const setters: { a?: (v: number) => void; b?: (v: number) => void } = {};
        let renders = 0;
        const App = () => {
            const [a, setA] = useState(0);
            const [b, setB] = useState(0);
            setters.a = setA;
            setters.b = setB;
            renders += 1;
            return h('div', { id: 'counter' }, `${a}:${b}`);
        };
        const root = createRoot('batch-root');
        act(() => { render(h(App), root); });
        return { root, setters, renders: () => renders };
    }

    it('installs the hook the bridge pump looks for', () => {
        expect(typeof batchHook()).toBe('function');
        expect(batchHook()!((x: number) => x + 1, 41)).toBe(42);
    });

    it('commits once for several state updates made inside the hook', () => {
        const { root, setters, renders } = mountCounter();
        const before = renders();
        batchHook()!(() => { setters.a!(1); setters.b!(1); }, undefined);
        expect(renders() - before).toBe(1);
        act(() => unmount(root));
    });

    it('commits per update outside the hook (the cost the hook removes)', () => {
        const { root, setters, renders } = mountCounter();
        const before = renders();
        setters.a!(2);
        setters.b!(2);
        expect(renders() - before).toBe(2);
        act(() => unmount(root));
    });
});
