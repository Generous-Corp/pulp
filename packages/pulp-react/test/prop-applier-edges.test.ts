// pulp #1434 (cross-surface mega-batch) — verify @pulp/react prop-applier
// forwards per-edge margin/padding values verbatim (number, percent string,
// or 'auto' for margin only). Bridge + Yoga path is covered by the
// Catch2 round-trip tests; this file covers the JS-side type-narrowing
// + bridge-call shape.

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

function setFlexCalls(b: MockBridge, key: string) {
    return b.calls.filter((c) => c.fn === 'setFlex' && c.args[1] === key);
}

describe('prop-applier per-edge margin / padding (pulp #1434 cross-surface mega-batch)', () => {
    it('paddingTop accepts numeric (px) and forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { paddingTop: 12 });
        expect(setFlexCalls(bridge, 'padding_top')[0].args).toEqual(['k', 'padding_top', 12]);
    });

    it('paddingTop accepts percent string and forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { paddingTop: '25%' });
        expect(setFlexCalls(bridge, 'padding_top')[0].args).toEqual(['k', 'padding_top', '25%']);
    });

    it('all four padding edges accept percent strings', () => {
        applyChangedProps(makeInstance(), {}, {
            paddingTop:    '5%',
            paddingRight:  '10%',
            paddingBottom: '15%',
            paddingLeft:   '20%',
        });
        expect(setFlexCalls(bridge, 'padding_top')[0].args[2]).toBe('5%');
        expect(setFlexCalls(bridge, 'padding_right')[0].args[2]).toBe('10%');
        expect(setFlexCalls(bridge, 'padding_bottom')[0].args[2]).toBe('15%');
        expect(setFlexCalls(bridge, 'padding_left')[0].args[2]).toBe('20%');
    });

    it('marginTop accepts numeric (px) and forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { marginTop: 8 });
        expect(setFlexCalls(bridge, 'margin_top')[0].args).toEqual(['k', 'margin_top', 8]);
    });

    it('marginTop accepts percent string and forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { marginTop: '50%' });
        expect(setFlexCalls(bridge, 'margin_top')[0].args).toEqual(['k', 'margin_top', '50%']);
    });

    it("marginLeft + marginRight 'auto' enables Yoga centering", () => {
        applyChangedProps(makeInstance(), {}, {
            marginLeft:  'auto',
            marginRight: 'auto',
        });
        expect(setFlexCalls(bridge, 'margin_left')[0].args[2]).toBe('auto');
        expect(setFlexCalls(bridge, 'margin_right')[0].args[2]).toBe('auto');
    });

    it('all four margin edges accept percent strings', () => {
        applyChangedProps(makeInstance(), {}, {
            marginTop:    '5%',
            marginRight:  '10%',
            marginBottom: '15%',
            marginLeft:   '20%',
        });
        expect(setFlexCalls(bridge, 'margin_top')[0].args[2]).toBe('5%');
        expect(setFlexCalls(bridge, 'margin_right')[0].args[2]).toBe('10%');
        expect(setFlexCalls(bridge, 'margin_bottom')[0].args[2]).toBe('15%');
        expect(setFlexCalls(bridge, 'margin_left')[0].args[2]).toBe('20%');
    });

    it('numeric and string-percent on different edges coexist on the same instance', () => {
        applyChangedProps(makeInstance(), {}, {
            paddingTop:    8,
            paddingRight:  '20%',
            marginLeft:    'auto',
            marginRight:   'auto',
            marginBottom:  '15%',
            marginTop:     12,
        });
        expect(setFlexCalls(bridge, 'padding_top')[0].args[2]).toBe(8);
        expect(setFlexCalls(bridge, 'padding_right')[0].args[2]).toBe('20%');
        expect(setFlexCalls(bridge, 'margin_left')[0].args[2]).toBe('auto');
        expect(setFlexCalls(bridge, 'margin_right')[0].args[2]).toBe('auto');
        expect(setFlexCalls(bridge, 'margin_bottom')[0].args[2]).toBe('15%');
        expect(setFlexCalls(bridge, 'margin_top')[0].args[2]).toBe(12);
    });
});
