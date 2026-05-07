// pulp #1434 (rn batch C) — verify the @pulp/react prop-applier
// forwards dimension props as `number | string` so percent strings
// (`'50%'`) and the `'auto'` keyword (flexBasis) reach the bridge
// verbatim. The bridge's setFlex case for each dimension key
// dispatches percent values to Yoga's
// `YGNodeStyleSet{Width,Height,Min*,Max*,FlexBasis}Percent` API and
// `'auto'` to `YGNodeStyleSetFlexBasisAuto`.
//
// Before this batch, the prop-applier cast `value as number` for these
// keys, which silently coerced strings to NaN at the bridge boundary
// and dropped the percent / auto signal entirely.

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

describe('prop-applier dimension percent strings (pulp #1434 batch C)', () => {
    it.each([
        ['width',     'width'],
        ['height',    'height'],
        ['minWidth',  'min_width'],
        ['minHeight', 'min_height'],
        ['maxWidth',  'max_width'],
        ['maxHeight', 'max_height'],
    ])('%s forwards percent string verbatim to setFlex(%s)', (jsxKey, slot) => {
        applyChangedProps(makeInstance(), {}, { [jsxKey]: '50%' });
        const calls = flexCalls(bridge, slot);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', slot, '50%']);
    });

    it.each([
        ['width',     'width',      120],
        ['height',    'height',     80],
        ['minWidth',  'min_width',  40],
        ['minHeight', 'min_height', 30],
        ['maxWidth',  'max_width',  200],
        ['maxHeight', 'max_height', 150],
    ])('%s forwards numeric value verbatim to setFlex(%s)', (jsxKey, slot, value) => {
        applyChangedProps(makeInstance(), {}, { [jsxKey]: value });
        const calls = flexCalls(bridge, slot);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', slot, value]);
    });

    it('flexBasis forwards percent string verbatim', () => {
        applyChangedProps(makeInstance(), {}, { flexBasis: '40%' });
        const calls = flexCalls(bridge, 'flex_basis');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 'flex_basis', '40%']);
    });

    it('flexBasis forwards "auto" keyword verbatim', () => {
        applyChangedProps(makeInstance(), {}, { flexBasis: 'auto' });
        const calls = flexCalls(bridge, 'flex_basis');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 'flex_basis', 'auto']);
    });

    it('flexBasis forwards numeric value as number', () => {
        applyChangedProps(makeInstance(), {}, { flexBasis: 80 });
        const calls = flexCalls(bridge, 'flex_basis');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 'flex_basis', 80]);
    });
});
