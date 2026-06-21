// Verify @pulp/react prop-applier forwards `flexDirection` (camelCase,
// the canonical RN/React JSX key) to the bridge. Without this routing,
// Yoga's column default collapses CSS-imported flex rows into vertical
// stacks.
// `col` and `col-reverse` aliases normalize to the bridge vocabulary
// `column` / `column-reverse`.

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

function makeInstance(id: string = 'k'): PulpInstance {
    return {
        id,
        type: 'View' as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

const callsOf = (b: MockBridge, fn: string) => b.calls.filter((c) => c.fn === fn);

describe('prop-applier flexDirection', () => {
    it('flexDirection=row dispatches setFlex(direction, row)', () => {
        applyChangedProps(makeInstance(), {}, { flexDirection: 'row' });
        const c = callsOf(bridge, 'setFlex');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', 'direction', 'row']);
    });

    it('flexDirection=column dispatches setFlex(direction, column)', () => {
        applyChangedProps(makeInstance(), {}, { flexDirection: 'column' });
        const c = callsOf(bridge, 'setFlex');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', 'direction', 'column']);
    });

    it('flexDirection=row-reverse passes through verbatim', () => {
        applyChangedProps(makeInstance(), {}, { flexDirection: 'row-reverse' });
        expect(callsOf(bridge, 'setFlex')[0].args).toEqual(['k', 'direction', 'row-reverse']);
    });

    it('flexDirection=col normalizes to column', () => {
        applyChangedProps(makeInstance(), {}, { flexDirection: 'col' });
        expect(callsOf(bridge, 'setFlex')[0].args).toEqual(['k', 'direction', 'column']);
    });

    it('flexDirection=col-reverse normalizes to column-reverse', () => {
        applyChangedProps(makeInstance(), {}, { flexDirection: 'col-reverse' });
        expect(callsOf(bridge, 'setFlex')[0].args).toEqual(['k', 'direction', 'column-reverse']);
    });

    it('flexDirection is case-insensitive (ROW / Row / row all map)', () => {
        applyChangedProps(makeInstance(), {}, { flexDirection: 'ROW' });
        expect(callsOf(bridge, 'setFlex')[0].args).toEqual(['k', 'direction', 'row']);
    });

    it('flexDirection does NOT route through setDirection (writing-direction)', () => {
        applyChangedProps(makeInstance(), {}, { flexDirection: 'row' });
        // setDirection is the writing-direction (rtl/ltr) bridge fn;
        // flexDirection must not collide with it.
        expect(callsOf(bridge, 'setDirection')).toHaveLength(0);
    });
});
