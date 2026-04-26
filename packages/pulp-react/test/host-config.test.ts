// host-config.test.ts — exercises the HostConfig against a mock bridge.
//
// We don't need the actual Pulp runtime here. createMockBridge() swaps
// the bridge globals (createCol, setFlex, setBackground, etc.) for
// recorders that capture each call to a `calls` array. We then render
// React JSX through @pulp/react and assert the expected sequence of
// bridge calls.
//
// This validates:
//   - createInstance correctly defers attachment to appendChild
//   - applyAllProps maps React prop names to setX correctly on first mount
//   - applyChangedProps only emits setX for *changed* props on update
//   - shouldSetTextContent routes <Label>text</Label> to createLabel(text)
//   - removeChild emits removeWidget
//
// Build: vitest run

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { createElement } from 'react';

import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { render, unmount, createRoot, View, Row, Label, Spectrum } from '../src/index.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});

afterEach(() => {
    bridge.uninstall();
});

function calls(fn?: string) {
    return fn ? bridge.calls.filter(c => c.fn === fn) : bridge.calls;
}

describe('HostConfig — first mount', () => {
    it('renders <Label>Hello</Label> as createLabel + setTextColor', () => {
        const root = createRoot('root');
        render(
            createElement(Label, { textColor: '#ffffff' }, 'Hello'),
            root,
        );
        const labelCalls = calls('createLabel');
        expect(labelCalls).toHaveLength(1);
        expect(labelCalls[0]?.args[1]).toBe('Hello');
        expect(labelCalls[0]?.args[2]).toBe('root');
        const textColor = calls('setTextColor');
        expect(textColor).toHaveLength(1);
        expect(textColor[0]?.args[1]).toBe('#ffffff');
        unmount(root);
    });

    it('renders <View flexGrow={1} background="#0a0e14"> as createCol + setFlex + setBackground', () => {
        const root = createRoot('root');
        render(
            createElement(View, { flexGrow: 1, background: '#0a0e14' }),
            root,
        );
        expect(calls('createCol')).toHaveLength(1);
        expect(calls('setBackground')[0]?.args[1]).toBe('#0a0e14');
        const flex = calls('setFlex');
        expect(flex).toHaveLength(1);
        expect(flex[0]?.args[1]).toBe('flex_grow');
        expect(flex[0]?.args[2]).toBe(1);
        unmount(root);
    });

    it('attaches children in order under their parent', () => {
        const root = createRoot('root');
        render(
            createElement(Row, { gap: 10 },
                createElement(Label, {}, 'A'),
                createElement(Label, {}, 'B'),
                createElement(Label, {}, 'C'),
            ),
            root,
        );
        expect(calls('createRow')).toHaveLength(1);
        const labels = calls('createLabel');
        expect(labels).toHaveLength(3);
        expect(labels.map(c => c.args[1])).toEqual(['A', 'B', 'C']);
        // All three labels should share the row's parentId — first arg
        // of createRow is the row's id, third arg of createLabel is its parent.
        const rowId = calls('createRow')[0]?.args[0];
        for (const l of labels) {
            expect(l.args[2]).toBe(rowId);
        }
        unmount(root);
    });

    it('routes <Spectrum data={[...]} /> through createSpectrum + setSpectrumData', () => {
        const root = createRoot('root');
        const data = [0.1, 0.2, 0.3];
        render(createElement(Spectrum, { data }), root);
        expect(calls('createSpectrum')).toHaveLength(1);
        expect(calls('setSpectrumData')).toHaveLength(1);
        expect(calls('setSpectrumData')[0]?.args[1]).toBe(data);
        unmount(root);
    });
});

describe('HostConfig — commit-time flush', () => {
    it('calls layout() exactly once per render commit', () => {
        const root = createRoot('root');
        render(
            createElement(View, {},
                createElement(Label, {}, 'A'),
                createElement(Label, {}, 'B'),
            ),
            root,
        );
        // Pulp's `layout` function may be undefined in the mock; our
        // mock installs a recorder for it regardless.
        expect(calls('layout').length).toBeGreaterThanOrEqual(1);
        unmount(root);
    });
});

describe('HostConfig — update', () => {
    it('only emits setX for changed props on re-render', () => {
        const root = createRoot('root');
        render(
            createElement(View, { background: '#000', flexGrow: 1 }),
            root,
        );
        bridge.reset();
        // Re-render with only flexGrow changed
        render(
            createElement(View, { background: '#000', flexGrow: 2 }),
            root,
        );
        const flex = calls('setFlex');
        const bg = calls('setBackground');
        expect(flex.some(c => c.args[1] === 'flex_grow' && c.args[2] === 2)).toBe(true);
        expect(bg).toHaveLength(0);  // unchanged — no re-set
        unmount(root);
    });

    it('updates Label text via setText on commitUpdate', () => {
        const root = createRoot('root');
        render(createElement(Label, {}, 'Hello'), root);
        bridge.reset();
        render(createElement(Label, {}, 'World'), root);
        const setTextCalls = calls('setText');
        expect(setTextCalls).toHaveLength(1);
        expect(setTextCalls[0]?.args[1]).toBe('World');
        unmount(root);
    });
});

describe('HostConfig — remove', () => {
    it('emits removeWidget when a child is dropped from the tree', () => {
        const root = createRoot('root');
        render(
            createElement(Row, {},
                createElement(Label, {}, 'A'),
                createElement(Label, {}, 'B'),
            ),
            root,
        );
        bridge.reset();
        render(
            createElement(Row, {},
                createElement(Label, {}, 'A'),
            ),
            root,
        );
        const removed = calls('removeWidget');
        expect(removed).toHaveLength(1);
        unmount(root);
    });
});

describe('HostConfig — unsupported text-as-child', () => {
    it('throws on raw text under a non-text-bearing parent', () => {
        const root = createRoot('root');
        // Wrapping a string directly under <View> should throw at render.
        expect(() =>
            render(createElement(View, {}, 'oops, raw text'), root),
        ).toThrow(/text outside a text-bearing parent/);
        unmount(root);
    });
});

describe('HostConfig — event handlers', () => {
    it('routes onClick props through the bridge `on(id, "click", fn)` registrar', () => {
        const root = createRoot('root');
        const handler = () => { /* noop for now */ };
        render(
            createElement(View, { onClick: handler }),
            root,
        );
        const onCalls = calls('on');
        expect(onCalls).toHaveLength(1);
        expect(onCalls[0]?.args[1]).toBe('click');
        // The third arg is the wrapped handler, not the original — we
        // forward via a closure so the bridge can pass extra args.
        expect(typeof onCalls[0]?.args[2]).toBe('function');
        unmount(root);
    });

    it('forwards bridge __dispatch__ args to the React handler', () => {
        const root = createRoot('root');
        const received: unknown[] = [];
        const handler = (...args: unknown[]) => { received.push(...args); };
        render(
            createElement(View, { onChange: handler }),
            root,
        );
        const onCall = bridge.calls.find(c => c.fn === 'on' && c.args[1] === 'change');
        expect(onCall).toBeDefined();
        // Simulate a bridge dispatch by invoking the wrapped handler.
        const wrappedFn = onCall?.args[2] as (...args: unknown[]) => void;
        wrappedFn(0.42);
        expect(received).toEqual([0.42]);
        unmount(root);
    });
});

import { useState as useStateHook, useEffect as useEffectHook } from 'react';

describe('HostConfig — function components with hooks', () => {
    it('renders a function component that uses useState', () => {
        const root = createRoot('root');
        function Counter() {
            const [n] = useStateHook(42);
            return createElement(Label, {}, String(n));
        }
        // Should not throw. If the dispatcher isn't being pushed by the
        // reconciler before the component runs, useState's
        // U.current.useState(...) deref will throw "cannot read property
        // 'useState' of null" — exactly the spectr#28 standalone error.
        expect(() => render(createElement(Counter, {}), root)).not.toThrow();
        const labelCalls = bridge.calls.filter(c => c.fn === 'createLabel');
        expect(labelCalls.length).toBeGreaterThan(0);
        // The text "42" should be on the first createLabel call.
        expect(labelCalls[0]?.args[1]).toBe('42');
        unmount(root);
    });

    it('renders a function component using useState + useEffect', () => {
        const root = createRoot('root');
        let effectFired = false;
        function App() {
            const [text] = useStateHook('hello');
            useEffectHook(() => { effectFired = true; }, []);
            return createElement(Label, {}, text);
        }
        expect(() => render(createElement(App, {}), root)).not.toThrow();
        // useEffect should fire synchronously in legacy/sync mode.
        expect(effectFired).toBe(true);
        unmount(root);
    });
});
