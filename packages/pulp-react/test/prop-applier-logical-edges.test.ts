// pulp #1434 rn logical-edge bundle (sub-agent #27 finding) — verify
// the LTR-only fast path: Start → Left, End → Right, inset / insetBlock
// / insetInline shorthand fan-out.

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

describe('rn logical-edge bundle (pulp #1434 sub-agent #27)', () => {
    it('marginStart routes to margin_left (LTR)', () => {
        applyChangedProps(makeInstance(), {}, { marginStart: 8 });
        expect(setFlexCalls(bridge, 'margin_left')[0].args).toEqual(['k', 'margin_left', 8]);
    });

    it('marginEnd routes to margin_right (LTR)', () => {
        applyChangedProps(makeInstance(), {}, { marginEnd: 12 });
        expect(setFlexCalls(bridge, 'margin_right')[0].args).toEqual(['k', 'margin_right', 12]);
    });

    it('paddingStart routes to padding_left (LTR)', () => {
        applyChangedProps(makeInstance(), {}, { paddingStart: '5%' });
        expect(setFlexCalls(bridge, 'padding_left')[0].args).toEqual(['k', 'padding_left', '5%']);
    });

    it('paddingEnd routes to padding_right (LTR)', () => {
        applyChangedProps(makeInstance(), {}, { paddingEnd: '10%' });
        expect(setFlexCalls(bridge, 'padding_right')[0].args).toEqual(['k', 'padding_right', '10%']);
    });

    it('borderStartWidth routes to setBorderLeftWidth', () => {
        applyChangedProps(makeInstance(), {}, { borderStartWidth: 2 });
        const c = bridge.calls.find((x) => x.fn === 'setBorderLeftWidth');
        expect(c?.args).toEqual(['k', 2]);
    });

    it('borderEndWidth routes to setBorderRightWidth', () => {
        applyChangedProps(makeInstance(), {}, { borderEndWidth: 3 });
        const c = bridge.calls.find((x) => x.fn === 'setBorderRightWidth');
        expect(c?.args).toEqual(['k', 3]);
    });

    it('start routes to setLeft (LTR)', () => {
        applyChangedProps(makeInstance(), {}, { start: 10 });
        const c = bridge.calls.find((x) => x.fn === 'setLeft');
        expect(c?.args).toEqual(['k', 10]);
    });

    it('end routes to setRight (LTR)', () => {
        applyChangedProps(makeInstance(), {}, { end: 20 });
        const c = bridge.calls.find((x) => x.fn === 'setRight');
        expect(c?.args).toEqual(['k', 20]);
    });

    it('inset numeric fans out to all four edges', () => {
        applyChangedProps(makeInstance(), {}, { inset: 16 });
        expect(bridge.calls.find((x) => x.fn === 'setTop')?.args).toEqual(['k', 16]);
        expect(bridge.calls.find((x) => x.fn === 'setRight')?.args).toEqual(['k', 16]);
        expect(bridge.calls.find((x) => x.fn === 'setBottom')?.args).toEqual(['k', 16]);
        expect(bridge.calls.find((x) => x.fn === 'setLeft')?.args).toEqual(['k', 16]);
    });

    it('inset 2-token shorthand: top+bottom, right+left', () => {
        applyChangedProps(makeInstance(), {}, { inset: '10px 20px' });
        expect(bridge.calls.find((x) => x.fn === 'setTop')?.args).toEqual(['k', 10]);
        expect(bridge.calls.find((x) => x.fn === 'setRight')?.args).toEqual(['k', 20]);
        expect(bridge.calls.find((x) => x.fn === 'setBottom')?.args).toEqual(['k', 10]);
        expect(bridge.calls.find((x) => x.fn === 'setLeft')?.args).toEqual(['k', 20]);
    });

    it('inset 4-token shorthand', () => {
        applyChangedProps(makeInstance(), {}, { inset: '5 10 15 20' });
        expect(bridge.calls.find((x) => x.fn === 'setTop')?.args).toEqual(['k', 5]);
        expect(bridge.calls.find((x) => x.fn === 'setRight')?.args).toEqual(['k', 10]);
        expect(bridge.calls.find((x) => x.fn === 'setBottom')?.args).toEqual(['k', 15]);
        expect(bridge.calls.find((x) => x.fn === 'setLeft')?.args).toEqual(['k', 20]);
    });

    it('inset percent strings forward verbatim', () => {
        applyChangedProps(makeInstance(), {}, { inset: '50%' });
        expect(bridge.calls.find((x) => x.fn === 'setTop')?.args).toEqual(['k', '50%']);
        expect(bridge.calls.find((x) => x.fn === 'setLeft')?.args).toEqual(['k', '50%']);
    });

    it('insetBlock fans out to top + bottom', () => {
        applyChangedProps(makeInstance(), {}, { insetBlock: 8 });
        expect(bridge.calls.find((x) => x.fn === 'setTop')?.args).toEqual(['k', 8]);
        expect(bridge.calls.find((x) => x.fn === 'setBottom')?.args).toEqual(['k', 8]);
        expect(bridge.calls.find((x) => x.fn === 'setLeft')).toBeUndefined();
        expect(bridge.calls.find((x) => x.fn === 'setRight')).toBeUndefined();
    });

    it('insetInline fans out to left + right (LTR)', () => {
        applyChangedProps(makeInstance(), {}, { insetInline: 10 });
        expect(bridge.calls.find((x) => x.fn === 'setLeft')?.args).toEqual(['k', 10]);
        expect(bridge.calls.find((x) => x.fn === 'setRight')?.args).toEqual(['k', 10]);
        expect(bridge.calls.find((x) => x.fn === 'setTop')).toBeUndefined();
        expect(bridge.calls.find((x) => x.fn === 'setBottom')).toBeUndefined();
    });

    it('all 11 logical edges coexist on the same instance', () => {
        applyChangedProps(makeInstance(), {}, {
            marginStart: 1,
            marginEnd: 2,
            paddingStart: 3,
            paddingEnd: 4,
            borderStartWidth: 5,
            borderEndWidth: 6,
            start: 7,
            end: 8,
        });
        expect(setFlexCalls(bridge, 'margin_left')[0].args[2]).toBe(1);
        expect(setFlexCalls(bridge, 'margin_right')[0].args[2]).toBe(2);
        expect(setFlexCalls(bridge, 'padding_left')[0].args[2]).toBe(3);
        expect(setFlexCalls(bridge, 'padding_right')[0].args[2]).toBe(4);
        expect(bridge.calls.find((x) => x.fn === 'setBorderLeftWidth')?.args[1]).toBe(5);
        expect(bridge.calls.find((x) => x.fn === 'setBorderRightWidth')?.args[1]).toBe(6);
        expect(bridge.calls.find((x) => x.fn === 'setLeft')?.args[1]).toBe(7);
        expect(bridge.calls.find((x) => x.fn === 'setRight')?.args[1]).toBe(8);
    });
});
