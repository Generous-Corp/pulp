// pulp #1434 rn bridge-wires bundle (sub-agent #27 finding) — verify
// @pulp/react prop-applier forwards 7 RN-style props to bridge fns
// that already exist in C++ (widget_bridge.cpp). These were missing
// JSX dispatch despite the bridge surface being ready.

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

function callOf(b: MockBridge, fn: string) {
    return b.calls.find((c) => c.fn === fn);
}

describe('rn bridge-wires bundle (pulp #1434 sub-agent #27)', () => {
    it('backfaceVisibility forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { backfaceVisibility: 'hidden' });
        expect(callOf(bridge, 'setBackfaceVisibility')?.args).toEqual(['k', 'hidden']);
    });

    it('cursor forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { cursor: 'pointer' });
        expect(callOf(bridge, 'setCursor')?.args).toEqual(['k', 'pointer']);
    });

    it('cursor handles col-resize keyword (post-Triage #7 fan-out)', () => {
        applyChangedProps(makeInstance(), {}, { cursor: 'col-resize' });
        expect(callOf(bridge, 'setCursor')?.args).toEqual(['k', 'col-resize']);
    });

    it('filter forwards CSS string', () => {
        applyChangedProps(makeInstance(), {}, { filter: 'blur(4px)' });
        expect(callOf(bridge, 'setFilter')?.args).toEqual(['k', 'blur(4px)']);
    });

    it('pointerEvents forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { pointerEvents: 'none' });
        expect(callOf(bridge, 'setPointerEvents')?.args).toEqual(['k', 'none']);
    });

    it('textTransform forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { textTransform: 'uppercase' });
        expect(callOf(bridge, 'setTextTransform')?.args).toEqual(['k', 'uppercase']);
    });

    it('userSelect forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { userSelect: 'none' });
        expect(callOf(bridge, 'setUserSelect')?.args).toEqual(['k', 'none']);
    });

    it("transformOrigin parses 'center' to {0.5, 0.5}", () => {
        applyChangedProps(makeInstance(), {}, { transformOrigin: 'center' });
        expect(callOf(bridge, 'setTransformOrigin')?.args).toEqual(['k', 0.5, 0.5]);
    });

    it("transformOrigin parses '50% 50%' to {0.5, 0.5}", () => {
        applyChangedProps(makeInstance(), {}, { transformOrigin: '50% 50%' });
        expect(callOf(bridge, 'setTransformOrigin')?.args).toEqual(['k', 0.5, 0.5]);
    });

    it("transformOrigin parses '0% 100%' to {0, 1}", () => {
        applyChangedProps(makeInstance(), {}, { transformOrigin: '0% 100%' });
        expect(callOf(bridge, 'setTransformOrigin')?.args).toEqual(['k', 0, 1]);
    });

    it("transformOrigin parses 'left top' keywords", () => {
        applyChangedProps(makeInstance(), {}, { transformOrigin: 'left top' });
        expect(callOf(bridge, 'setTransformOrigin')?.args).toEqual(['k', 0, 0]);
    });

    it("transformOrigin parses 'right bottom' keywords", () => {
        applyChangedProps(makeInstance(), {}, { transformOrigin: 'right bottom' });
        expect(callOf(bridge, 'setTransformOrigin')?.args).toEqual(['k', 1, 1]);
    });

    it("transformOrigin one-arg defaults Y to same-axis token", () => {
        // CSS spec: single-token transform-origin means "{tok, center}" for x-axis tokens.
        // Our parser treats the missing 2nd token as a fallback to the 1st.
        applyChangedProps(makeInstance(), {}, { transformOrigin: '25%' });
        const args = callOf(bridge, 'setTransformOrigin')?.args;
        expect(args?.[1]).toBe(0.25);
    });

    it('all 7 wires coexist on the same instance', () => {
        applyChangedProps(makeInstance(), {}, {
            backfaceVisibility: 'hidden',
            cursor: 'grabbing',
            filter: 'blur(2px)',
            pointerEvents: 'box-only',
            textTransform: 'capitalize',
            transformOrigin: '25% 75%',
            userSelect: 'all',
        });
        expect(callOf(bridge, 'setBackfaceVisibility')?.args[1]).toBe('hidden');
        expect(callOf(bridge, 'setCursor')?.args[1]).toBe('grabbing');
        expect(callOf(bridge, 'setFilter')?.args[1]).toBe('blur(2px)');
        expect(callOf(bridge, 'setPointerEvents')?.args[1]).toBe('box-only');
        expect(callOf(bridge, 'setTextTransform')?.args[1]).toBe('capitalize');
        expect(callOf(bridge, 'setTransformOrigin')?.args).toEqual(['k', 0.25, 0.75]);
        expect(callOf(bridge, 'setUserSelect')?.args[1]).toBe('all');
    });
});
