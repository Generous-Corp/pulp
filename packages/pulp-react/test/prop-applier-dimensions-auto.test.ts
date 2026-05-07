// pulp #1434 (sub-agent #12 follow-up) — verify the @pulp/react
// prop-applier forwards `'auto'` for width/height verbatim to the
// bridge so Yoga's YGNodeStyleSetWidthAuto / SetHeightAuto path is
// reached.
//
// Figma auto-layout "hug contents" frames, v0.dev intrinsic-sizing
// cards, and Claude Design responsive containers all emit `width:
// 'auto'` / `height: 'auto'`. Before this batch the bridge had no
// 'auto' branch on setFlex(width|height); strings reached `(float)val`
// = NaN/0 and the node was effectively pinned to 0×0. The TS surface
// already typed width/height as `number | string` from the percent
// follow-up (#1434 batch C), so this test only exercises the
// 'auto' string path through the existing code shape.

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

describe("prop-applier width/height 'auto' (pulp #1434 sub-agent #12 follow-up)", () => {
    it("forwards width: 'auto' verbatim to setFlex(width)", () => {
        applyChangedProps(makeInstance(), {}, { width: 'auto' });
        const calls = flexCalls(bridge, 'width');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 'width', 'auto']);
    });

    it("forwards height: 'auto' verbatim to setFlex(height)", () => {
        applyChangedProps(makeInstance(), {}, { height: 'auto' });
        const calls = flexCalls(bridge, 'height');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 'height', 'auto']);
    });

    it("forwards width + height: 'auto' together (Figma hug-contents)", () => {
        applyChangedProps(makeInstance(), {}, { width: 'auto', height: 'auto' });
        expect(flexCalls(bridge, 'width')).toHaveLength(1);
        expect(flexCalls(bridge, 'height')).toHaveLength(1);
    });

    it('still forwards numeric values verbatim (regression check)', () => {
        applyChangedProps(makeInstance(), {}, { width: 200, height: 100 });
        expect(flexCalls(bridge, 'width')[0].args).toEqual(['k', 'width', 200]);
        expect(flexCalls(bridge, 'height')[0].args).toEqual(['k', 'height', 100]);
    });

    it('still forwards percent strings verbatim (regression check)', () => {
        applyChangedProps(makeInstance(), {}, { width: '50%', height: '25%' });
        expect(flexCalls(bridge, 'width')[0].args).toEqual(['k', 'width', '50%']);
        expect(flexCalls(bridge, 'height')[0].args).toEqual(['k', 'height', '25%']);
    });
});
