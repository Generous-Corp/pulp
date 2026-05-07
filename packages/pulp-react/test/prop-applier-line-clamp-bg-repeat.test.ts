// pulp #1552 — line-clamp / -webkit-line-clamp / background-repeat
// JSX dispatch. The C++ bridge fns (setLineClamp, setBackgroundRepeat)
// landed in the same PR; these tests pin the prop-applier dispatch
// shape so a future refactor can't silently drop the keys.

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

function makeInstance(id: string = 'k', type: string = 'Label'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

function callOf(b: MockBridge, fn: string) {
    return b.calls.find((c) => c.fn === fn);
}

describe('line-clamp + background-repeat dispatch (pulp #1552)', () => {
    it('lineClamp forwards numeric count to setLineClamp', () => {
        applyChangedProps(makeInstance(), {}, { lineClamp: 3 });
        expect(callOf(bridge, 'setLineClamp')?.args).toEqual(['k', 3]);
    });

    it('webkitLineClamp uses the same setLineClamp dispatch', () => {
        applyChangedProps(makeInstance(), {}, { webkitLineClamp: 5 });
        expect(callOf(bridge, 'setLineClamp')?.args).toEqual(['k', 5]);
    });

    it('lineClamp coerces a numeric string to int', () => {
        // CSS shim path may pass through the resolved string, RN path
        // sends a number. Both should reach the bridge as a number.
        applyChangedProps(makeInstance(), {}, { lineClamp: '4' as unknown as number });
        expect(callOf(bridge, 'setLineClamp')?.args).toEqual(['k', 4]);
    });

    it('lineClamp coerces non-finite to 0', () => {
        applyChangedProps(makeInstance(), {}, { lineClamp: 'wat' as unknown as number });
        expect(callOf(bridge, 'setLineClamp')?.args).toEqual(['k', 0]);
    });

    it('backgroundRepeat forwards keyword verbatim', () => {
        applyChangedProps(makeInstance(), {}, { backgroundRepeat: 'no-repeat' });
        expect(callOf(bridge, 'setBackgroundRepeat')?.args).toEqual(['k', 'no-repeat']);
    });

    it('backgroundRepeat accepts the full CSS keyword set', () => {
        for (const kw of ['repeat', 'repeat-x', 'repeat-y', 'no-repeat', 'space', 'round'] as const) {
            bridge.reset();
            applyChangedProps(makeInstance(), {}, { backgroundRepeat: kw });
            expect(callOf(bridge, 'setBackgroundRepeat')?.args).toEqual(['k', kw]);
        }
    });

    it('all three keys can be dispatched in one prop diff without clobber', () => {
        applyChangedProps(
            makeInstance(),
            {},
            { lineClamp: 2, webkitLineClamp: 2, backgroundRepeat: 'no-repeat' },
        );
        // Both line-clamp keys fire setLineClamp — the second one wins
        // (idempotent because both pass the same value), and the bridge
        // captures both calls in the log.
        const clamps = bridge.calls.filter((c) => c.fn === 'setLineClamp');
        expect(clamps.length).toBe(2);
        expect(clamps[0].args).toEqual(['k', 2]);
        expect(clamps[1].args).toEqual(['k', 2]);
        expect(callOf(bridge, 'setBackgroundRepeat')?.args).toEqual(['k', 'no-repeat']);
    });
});
