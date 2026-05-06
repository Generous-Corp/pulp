// pulp #1434 (#1518) — RN-style `flex: <number>` shorthand. RN spec
// flattens a bare numeric `flex` to `{flexGrow: n, flexShrink: 1,
// flexBasis: 0}` for positive `n`, distinct shapes for `0` and
// negatives. CSS bare-number shorthand (`flex: 1` ≡ `flex: 1 1 0`)
// is the same as RN's positive case, which is what consumers passing
// JSX `flex={1}` overwhelmingly expect; our adapter is RN-flavored
// so we honor RN semantics directly here. The `0` and negative cases
// match RN's contract: `0` collapses with no growth/shrink at
// intrinsic basis; negatives keep `auto` basis but allow shrinking.

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

function flexCalls(b: MockBridge, slot: string) {
    return b.calls.filter((c) => c.fn === 'setFlex' && c.args[1] === slot);
}

describe('prop-applier flex shorthand (pulp #1518)', () => {
    it('flex={1} fans out to flex_grow=1 / flex_shrink=1 / flex_basis=0', () => {
        applyChangedProps(makeInstance(), {}, { flex: 1 } as never);
        expect(flexCalls(bridge, 'flex_grow')[0].args[2]).toBe(1);
        expect(flexCalls(bridge, 'flex_shrink')[0].args[2]).toBe(1);
        expect(flexCalls(bridge, 'flex_basis')[0].args[2]).toBe(0);
    });

    it('flex={2} fans out the numeric grow factor', () => {
        applyChangedProps(makeInstance(), {}, { flex: 2 } as never);
        expect(flexCalls(bridge, 'flex_grow')[0].args[2]).toBe(2);
        expect(flexCalls(bridge, 'flex_shrink')[0].args[2]).toBe(1);
        expect(flexCalls(bridge, 'flex_basis')[0].args[2]).toBe(0);
    });

    it('flex={0} disables both growth and shrink at auto basis', () => {
        applyChangedProps(makeInstance(), {}, { flex: 0 } as never);
        expect(flexCalls(bridge, 'flex_grow')[0].args[2]).toBe(0);
        expect(flexCalls(bridge, 'flex_shrink')[0].args[2]).toBe(0);
        expect(flexCalls(bridge, 'flex_basis')[0].args[2]).toBe('auto');
    });

    it('flex={-1} keeps auto basis and lets the item shrink', () => {
        applyChangedProps(makeInstance(), {}, { flex: -1 } as never);
        expect(flexCalls(bridge, 'flex_grow')[0].args[2]).toBe(0);
        expect(flexCalls(bridge, 'flex_shrink')[0].args[2]).toBe(1);
        expect(flexCalls(bridge, 'flex_basis')[0].args[2]).toBe('auto');
    });

    it('non-finite flex value is ignored (no bridge calls)', () => {
        applyChangedProps(makeInstance(), {}, { flex: NaN } as never);
        expect(flexCalls(bridge, 'flex_grow')).toHaveLength(0);
        expect(flexCalls(bridge, 'flex_shrink')).toHaveLength(0);
        expect(flexCalls(bridge, 'flex_basis')).toHaveLength(0);
    });

    it('explicit flexGrow / flexShrink / flexBasis still flow through verbatim', () => {
        applyChangedProps(makeInstance(), {}, { flexGrow: 3, flexShrink: 2, flexBasis: 50 } as never);
        expect(flexCalls(bridge, 'flex_grow')[0].args[2]).toBe(3);
        expect(flexCalls(bridge, 'flex_shrink')[0].args[2]).toBe(2);
        expect(flexCalls(bridge, 'flex_basis')[0].args[2]).toBe(50);
    });
});
