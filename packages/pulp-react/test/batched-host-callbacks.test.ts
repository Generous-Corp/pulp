// Host-driven callbacks (requestAnimationFrame, timers) run through
// globalThis.__pulpBatchUpdates__, which WidgetBridge's frame/timer pump calls
// around each callback. Under LegacyRoot every setState outside a batch commits
// synchronously, so a callback that sets N pieces of state would commit N
// times; inside the hook it commits once.
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import React, { act, useState } from 'react';
import { createRoot, render, unmount } from '../src/index.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

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

// A native widget event reaches React through WidgetBridge's JS prelude
// (__dispatch__ / __dispatchCallbackOnly__), which runs the handlers inside the
// same batching hook. These cases evaluate the shipped prelude source, so they
// exercise the real dispatch path rather than a re-implementation of it.
const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '../../..');

function rawString(source: string, name: string): string {
    const open = `${name} = R"(`;
    const start = source.indexOf(open);
    if (start < 0) throw new Error(`${name} not found`);
    const end = source.indexOf('\n)";', start);
    return source.slice(start + open.length, end);
}

// __pulpRunHostCallback__ is a sequence of adjacent C string literals in the
// runtime API registrar; join them back into the JS function.
function hostCallbackSource(source: string): string {
    const lines = source.split('\n');
    const first = lines.findIndex((l) => l.includes('"function __pulpRunHostCallback__'));
    if (first < 0) throw new Error('__pulpRunHostCallback__ not found');
    const parts: string[] = [];
    for (let i = first; i < lines.length; ++i) {
        const literal = JSON.parse(lines[i].trim()) as string;
        parts.push(literal);
        if (literal === '}') break;
    }
    return parts.join('\n');
}

interface Prelude {
    __dispatch__: (id: string, eventName: string, ...args: unknown[]) => unknown;
    __dispatchCallbackOnly__: (id: string, eventName: string, ...args: unknown[]) => number;
    on: (id: string, eventName: string, fn: (...a: unknown[]) => unknown) => void;
    depth: () => number;
}

function loadPrelude(): Prelude {
    const read = (rel: string) => readFileSync(resolve(repoRoot, rel), 'utf8');
    const dispatch = read('core/view/src/widget_bridge/bridge_dispatch.cpp');
    const bridge = read('core/view/src/widget_bridge.cpp');
    const runtime = read('core/view/src/widget_bridge/runtime_api.cpp');
    const body = rawString(dispatch, 'kDispatchPrelude')
        + rawString(dispatch, 'kDispatchBatchPrelude')
        + rawString(bridge, 'kJSPreamblePart2')
        + hostCallbackSource(runtime)
        + '\nreturn { __dispatch__, __dispatchCallbackOnly__, on,'
        + ' depth: function() { return __pulpDispatchDepth__; } };';
    return new Function(body)() as Prelude;
}

describe('@pulp/react batched native widget events', () => {
    let bridge: MockBridge;
    let prelude: Prelude;
    let commits = 0;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
        prelude = loadPrelude();
        const g = globalThis as Record<string, unknown>;
        // Listeners go into the prelude's own callback table, as on device.
        g.on = prelude.on;
        // resetAfterCommit reads the root size exactly once per commit.
        commits = 0;
        g.getRootSize = () => { commits += 1; return undefined; };
    });
    afterEach(() => {
        delete (globalThis as Record<string, unknown>).__pulpReactEventCallbacks__;
        bridge.uninstall();
        delete (globalThis as Record<string, unknown>).getRootSize;
    });

    function textOf(id: string): unknown {
        const writes = bridge.calls.filter((c) => c.fn === 'setText' && c.args[0] === id);
        return writes.length ? writes[writes.length - 1].args[1] : undefined;
    }

    function createdId(fn: string, index = 0): string {
        const made = bridge.calls.filter((c) => c.fn === fn);
        return made[index].args[0] as string;
    }

    function mountThreeCounters(onClick: (set: (n: number) => void) => void) {
        const App = () => {
            const [a, setA] = useState(0);
            const [b, setB] = useState(0);
            const [c, setC] = useState(0);
            const set = (n: number) => { setA(n); setB(n + 1); setC(n + 2); };
            return h('View', null,
                h('Button', { onClick: () => onClick(set) }, 'go'),
                h('Label', null, String(a)),
                h('Label', null, String(b)),
                h('Label', null, String(c)));
        };
        const root = createRoot('event-batch-root');
        act(() => { render(h(App), root); });
        return {
            root,
            button: createdId('createButton'),
            labels: [createdId('createLabel', 0), createdId('createLabel', 1),
                createdId('createLabel', 2)],
        };
    }

    it('a click that sets three states commits once, before dispatch returns', () => {
        const { root, button, labels } = mountThreeCounters((set) => set(1));
        const before = commits;
        prelude.__dispatch__(button, 'click', {});
        expect(commits - before).toBe(1);
        // No microtask: the native text already reflects every update.
        expect(labels.map(textOf)).toEqual(['1', '2', '3']);
        act(() => unmount(root));
    });

    it('a callback-only dispatch is one commit boundary too', () => {
        const { root, button, labels } = mountThreeCounters((set) => set(4));
        const before = commits;
        prelude.__dispatchCallbackOnly__(button, 'click', {});
        expect(commits - before).toBe(1);
        expect(labels.map(textOf)).toEqual(['4', '5', '6']);
        act(() => unmount(root));
    });

    it('a dispatch from inside a handler joins the outer batch', () => {
        const depths: number[] = [];
        const target = { id: '' };
        const { root, button, labels } = mountThreeCounters((set) => {
            depths.push(prelude.depth());
            set(depths.length * 10);
            if (depths.length === 1) prelude.__dispatch__(target.id, 'click', {});
        });
        target.id = button;
        const before = commits;
        prelude.__dispatch__(button, 'click', {});
        expect(depths).toEqual([1, 2]);
        expect(commits - before).toBe(1);
        expect(labels.map(textOf)).toEqual(['20', '21', '22']);
        act(() => unmount(root));
    });
});
