// pulp #1434 (Triage #12) — verify the @pulp/react prop-applier
// dispatches `display: 'flex' | 'none'` to the right setVisible call.
//
// RN exports + Figma / v0.dev / Claude Design HTML routinely emit
// `style={{ display: 'flex' }}` (the implicit default in pulp, but
// the prop-applier shouldn't drop it as unknown) or `display: 'none'`
// to hide a subtree. The yoga / CSS-shim side was wired for the
// el.style proxy in #1422; this test guards the parallel RN-flavored
// JSX path so RN consumers don't have to round-trip through el.style
// (which costs a bridge call + DOM-lite proxy walk).

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps } from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    bridge.uninstall();
});

function makeInstance(id: string = 'k', type: string = 'View'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

function visibleCalls(b: MockBridge) {
    return b.calls.filter((c) => c.fn === 'setVisible');
}

describe('prop-applier display: flex / none (pulp #1434 Triage #12)', () => {
    it("display: 'none' calls setVisible(id, false)", () => {
        applyChangedProps(makeInstance(), {}, { display: 'none' });
        const calls = visibleCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', false]);
    });

    it("display: 'flex' calls setVisible(id, true)", () => {
        applyChangedProps(makeInstance(), {}, { display: 'flex' });
        const calls = visibleCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', true]);
    });

    it('unknown display values are silently dropped', () => {
        applyChangedProps(makeInstance(), {}, { display: 'block' });
        // Other JSX intrinsics (e.g. block / inline-block / grid) flow
        // through the CSS shim only — for RN consumers, the prop-applier
        // intentionally leaves the View at its current visibility.
        expect(visibleCalls(bridge)).toHaveLength(0);
    });

    it('display change from none to flex re-shows the View', () => {
        const inst = makeInstance();
        // First: hide.
        applyChangedProps(inst, {}, { display: 'none' });
        expect(visibleCalls(bridge)).toEqual([
            expect.objectContaining({ args: ['k', false] }),
        ]);

        // Then: re-show via display: 'flex'. applyChangedProps should
        // fire setVisible(true) because display moved from 'none' to
        // 'flex' and the value differs.
        applyChangedProps(inst, { display: 'none' }, { display: 'flex' });
        const calls = visibleCalls(bridge);
        // 1 from the first call + 1 from the change = 2 total.
        expect(calls).toHaveLength(2);
        expect(calls[1].args).toEqual(['k', true]);
    });
});
