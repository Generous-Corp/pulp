// pulp #1434 Triage #9 — verify the @pulp/react prop-applier walks the
// RN-style `transform` array and dispatches the consolidated
// setTranslate / setRotation / setScale bridge calls.
//
// The walker accumulates a {tx, ty, rotateDeg, scale} snapshot in one
// pass, then emits ONE call per axis-of-transform that the user
// specified. Within-array merging means [{translateX:10},{translateY:20}]
// produces ONE setTranslate(10, 20) — not two clobbering calls.
//
// Bridge gaps (silently no-op): skewX/skewY (no setSkew bridge fn),
// rotateX/rotateY/perspective/matrix (no 3D in pulp's 2D View). Tests
// guard the silent-drop behavior so a future bridge wiring flagging
// these as failures is the intended signal to update.

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

function callsOf(b: MockBridge, fn: 'setTranslate' | 'setRotation' | 'setScale') {
    return b.calls.filter((c) => c.fn === fn);
}

describe('prop-applier transform array (pulp #1434 Triage #9)', () => {
    it('translateX-only dispatches one setTranslate(x, 0)', () => {
        applyChangedProps(makeInstance(), {}, { transform: [{ translateX: 10 }] });
        const calls = callsOf(bridge, 'setTranslate');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 10, 0]);
        expect(callsOf(bridge, 'setRotation')).toHaveLength(0);
        expect(callsOf(bridge, 'setScale')).toHaveLength(0);
    });

    it('translateY-only dispatches one setTranslate(0, y)', () => {
        applyChangedProps(makeInstance(), {}, { transform: [{ translateY: 20 }] });
        const calls = callsOf(bridge, 'setTranslate');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 0, 20]);
    });

    it('translateX + translateY merge into ONE setTranslate(x, y) — no axis clobber', () => {
        applyChangedProps(makeInstance(), {}, {
            transform: [{ translateX: 10 }, { translateY: 20 }],
        });
        const calls = callsOf(bridge, 'setTranslate');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 10, 20]);
    });

    it('rotate "45deg" dispatches setRotation with degrees', () => {
        applyChangedProps(makeInstance(), {}, { transform: [{ rotate: '45deg' }] });
        const calls = callsOf(bridge, 'setRotation');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 45]);
    });

    it('rotate numeric (no unit) is treated as degrees', () => {
        applyChangedProps(makeInstance(), {}, { transform: [{ rotate: 90 }] });
        const calls = callsOf(bridge, 'setRotation');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 90]);
    });

    it('rotate "1rad" converts to degrees', () => {
        applyChangedProps(makeInstance(), {}, { transform: [{ rotate: '1rad' }] });
        const calls = callsOf(bridge, 'setRotation');
        expect(calls).toHaveLength(1);
        const deg = calls[0].args[1] as number;
        expect(deg).toBeCloseTo(180 / Math.PI, 4); // 1 rad ≈ 57.2958°
    });

    it('rotate "0.5turn" converts to degrees', () => {
        applyChangedProps(makeInstance(), {}, { transform: [{ rotate: '0.5turn' }] });
        const calls = callsOf(bridge, 'setRotation');
        expect(calls[0].args[1]).toBe(180);
    });

    it('rotate "100grad" converts to degrees', () => {
        applyChangedProps(makeInstance(), {}, { transform: [{ rotate: '100grad' }] });
        const calls = callsOf(bridge, 'setRotation');
        expect(calls[0].args[1]).toBe(90); // 100 grad = 90°
    });

    it('rotateZ is treated as the same axis as rotate (RN parity)', () => {
        applyChangedProps(makeInstance(), {}, { transform: [{ rotateZ: '30deg' }] });
        const calls = callsOf(bridge, 'setRotation');
        expect(calls[0].args).toEqual(['k', 30]);
    });

    it('scale uniform dispatches setScale(s)', () => {
        applyChangedProps(makeInstance(), {}, { transform: [{ scale: 1.5 }] });
        const calls = callsOf(bridge, 'setScale');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 1.5]);
    });

    it('multi-op array: translate + rotate + scale dispatches one of each', () => {
        applyChangedProps(makeInstance(), {}, {
            transform: [
                { translateX: 5 },
                { translateY: 15 },
                { rotate: '45deg' },
                { scale: 2 },
            ],
        });
        const t = callsOf(bridge, 'setTranslate');
        const r = callsOf(bridge, 'setRotation');
        const s = callsOf(bridge, 'setScale');
        expect(t).toHaveLength(1);
        expect(t[0].args).toEqual(['k', 5, 15]);
        expect(r).toHaveLength(1);
        expect(r[0].args).toEqual(['k', 45]);
        expect(s).toHaveLength(1);
        expect(s[0].args).toEqual(['k', 2]);
    });

    it('order: translate ops accumulate; rotate / scale last-write-wins on the same axis', () => {
        // The walker is one pass, so within an array later entries
        // overwrite earlier ones for the SAME axis. translateX/Y are
        // independent axes so they merge. Two `rotate` entries: the
        // second wins. Same for `scale`.
        applyChangedProps(makeInstance(), {}, {
            transform: [
                { rotate: '10deg' },
                { rotate: '20deg' },
                { scale: 0.5 },
                { scale: 1.5 },
                { translateX: 1 },
                { translateX: 9 }, // overrides the previous translateX
            ],
        });
        expect(callsOf(bridge, 'setRotation')[0].args).toEqual(['k', 20]);
        expect(callsOf(bridge, 'setScale')[0].args).toEqual(['k', 1.5]);
        expect(callsOf(bridge, 'setTranslate')[0].args).toEqual(['k', 9, 0]);
    });

    it('non-array value (CSS string form) is silently dropped — deferred', () => {
        applyChangedProps(makeInstance(), {}, {
            transform: 'translateX(10px) rotate(45deg)' as unknown as never,
        });
        expect(callsOf(bridge, 'setTranslate')).toHaveLength(0);
        expect(callsOf(bridge, 'setRotation')).toHaveLength(0);
        expect(callsOf(bridge, 'setScale')).toHaveLength(0);
    });

    it('empty array is a no-op', () => {
        applyChangedProps(makeInstance(), {}, { transform: [] });
        expect(callsOf(bridge, 'setTranslate')).toHaveLength(0);
        expect(callsOf(bridge, 'setRotation')).toHaveLength(0);
        expect(callsOf(bridge, 'setScale')).toHaveLength(0);
    });

    it('skewX + skewY accumulate into ONE setSkew (Triage #9 fan-out)', () => {
        // pulp #1434 Triage #9 fan-out — setSkew is now a registered
        // bridge fn; the walker accumulates both axes and emits one
        // consolidated call. Previously this entry asserted silent
        // drop because the bridge fn didn't exist.
        const bcalls = (b: MockBridge, fn: string) => b.calls.filter((c) => c.fn === fn);
        applyChangedProps(makeInstance(), {}, {
            transform: [{ skewX: '10deg' }, { skewY: '5deg' }],
        });
        const skewCalls = bcalls(bridge, 'setSkew');
        expect(skewCalls).toHaveLength(1);
        expect(skewCalls[0].args).toEqual(['k', 10, 5]);
        // Other axes are not touched.
        expect(callsOf(bridge, 'setTranslate')).toHaveLength(0);
        expect(callsOf(bridge, 'setRotation')).toHaveLength(0);
        expect(callsOf(bridge, 'setScale')).toHaveLength(0);
    });

    it('skewX with rad unit converts to degrees', () => {
        const bcalls = (b: MockBridge, fn: string) => b.calls.filter((c) => c.fn === fn);
        applyChangedProps(makeInstance(), {}, {
            transform: [{ skewX: '1rad' }],
        });
        const skewCalls = bcalls(bridge, 'setSkew');
        expect(skewCalls).toHaveLength(1);
        // 1 rad ≈ 57.2958°; second arg (skewY) defaults to 0.
        expect((skewCalls[0].args[1] as number)).toBeCloseTo(180 / Math.PI, 4);
        expect(skewCalls[0].args[2]).toBe(0);
    });

    it('skew + translate + rotate + scale all dispatch as separate consolidated calls', () => {
        const bcalls = (b: MockBridge, fn: string) => b.calls.filter((c) => c.fn === fn);
        applyChangedProps(makeInstance(), {}, {
            transform: [
                { translateX: 5 }, { translateY: 15 },
                { rotate: '30deg' },
                { scale: 1.2 },
                { skewX: '8deg' }, { skewY: '4deg' },
            ],
        });
        expect(callsOf(bridge, 'setTranslate')).toHaveLength(1);
        expect(callsOf(bridge, 'setRotation')).toHaveLength(1);
        expect(callsOf(bridge, 'setScale')).toHaveLength(1);
        expect(bcalls(bridge, 'setSkew')).toHaveLength(1);
        expect(bcalls(bridge, 'setSkew')[0].args).toEqual(['k', 8, 4]);
    });

    it('rotateX / rotateY / perspective / matrix silently no-op (no 3D in 2D View)', () => {
        applyChangedProps(makeInstance(), {}, {
            transform: [
                { rotateX: '45deg' },
                { rotateY: '30deg' },
                { perspective: 1000 },
                { matrix: [1, 0, 0, 1, 0, 0] },
            ],
        });
        expect(callsOf(bridge, 'setTranslate')).toHaveLength(0);
        expect(callsOf(bridge, 'setRotation')).toHaveLength(0);
        expect(callsOf(bridge, 'setScale')).toHaveLength(0);
    });

    it('null / undefined transform is a no-op', () => {
        applyChangedProps(makeInstance(), {}, { transform: null as unknown as never });
        expect(bridge.calls).toHaveLength(0);
    });

    it('scaleX / scaleY map to uniform setScale (last-write-wins)', () => {
        // The bridge has only uniform setScale. Independent axes are a
        // deferred bridge gap — last-seen value wins. This test guards
        // that behavior; a future bridge with setScaleXY would update it.
        applyChangedProps(makeInstance(), {}, {
            transform: [{ scaleX: 2 }, { scaleY: 0.5 }],
        });
        expect(callsOf(bridge, 'setScale')[0].args).toEqual(['k', 0.5]);
    });

    it('negative translate values are preserved', () => {
        applyChangedProps(makeInstance(), {}, {
            transform: [{ translateX: -10 }, { translateY: -5 }],
        });
        expect(callsOf(bridge, 'setTranslate')[0].args).toEqual(['k', -10, -5]);
    });
});
