// pulp #1434 batch 6 — verify the @pulp/react prop-applier forwards
// percent strings ('50%') verbatim to the bridge for the four View
// positional fields (top/right/bottom/left). The bridge inspects
// arg index 1 as a string, detects the '%' suffix, and routes through
// Yoga's YGNodeStyleSetPositionPercent path.
//
// This is the JSX-side counterpart to PR #1426 (which routed width/height
// percent through to Yoga). Figma absolute-positioned overlays, v0.dev
// hero anchors, and Claude Design sticky elements all emit `top:'50%'`
// etc. routinely; without this forwarding the layout collapses to numeric
// 0 silently because `value as number` would coerce '50%' → NaN at the
// bridge boundary.
//
// Each test asserts that ONE positional prop emits ONE matching bridge
// call, with the value forwarded as either a number (px path) or a
// string ending in '%' (percent path).

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

function callsFor(b: MockBridge, fn: string) {
    return b.calls.filter((c) => c.fn === fn);
}

describe('prop-applier top/right/bottom/left percent (pulp #1434 batch 6)', () => {
    it("top: '50%' lands as a percent string on setTop", () => {
        applyChangedProps(makeInstance(), {}, { top: '50%' });
        const c = callsFor(bridge, 'setTop');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', '50%']);
    });

    it("right: '25%' lands as a percent string on setRight", () => {
        applyChangedProps(makeInstance(), {}, { right: '25%' });
        const c = callsFor(bridge, 'setRight');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', '25%']);
    });

    it("bottom: '100%' lands as a percent string on setBottom", () => {
        applyChangedProps(makeInstance(), {}, { bottom: '100%' });
        const c = callsFor(bridge, 'setBottom');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', '100%']);
    });

    it("left: '0%' lands as a percent string on setLeft", () => {
        applyChangedProps(makeInstance(), {}, { left: '0%' });
        const c = callsFor(bridge, 'setLeft');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', '0%']);
    });

    it('numeric values still flow through unchanged (regression guard)', () => {
        applyChangedProps(makeInstance(), {}, {
            top: 10,
            right: 20,
            bottom: 30,
            left: 40,
        });
        expect(callsFor(bridge, 'setTop')[0].args).toEqual(['k', 10]);
        expect(callsFor(bridge, 'setRight')[0].args).toEqual(['k', 20]);
        expect(callsFor(bridge, 'setBottom')[0].args).toEqual(['k', 30]);
        expect(callsFor(bridge, 'setLeft')[0].args).toEqual(['k', 40]);
    });

    it('absolute-positioned centered overlay pattern emits all four bridge calls', () => {
        // The canonical centering idiom for an absolute overlay:
        //   <View style={{ position:'absolute', top:'50%', left:'50%',
        //                  transform:[{translateX:-50%}, {translateY:-50%}] }} />
        // We assert the four positional bridge calls fire — the transform
        // legs are out of scope for this test (translateX/Y percent is
        // a separate batch).
        applyChangedProps(makeInstance(), {}, {
            position: 'absolute',
            top: '50%',
            left: '50%',
            right: '50%',
            bottom: '50%',
        });
        expect(callsFor(bridge, 'setPosition')[0].args).toEqual(['k', 'absolute']);
        expect(callsFor(bridge, 'setTop')[0].args).toEqual(['k', '50%']);
        expect(callsFor(bridge, 'setLeft')[0].args).toEqual(['k', '50%']);
        expect(callsFor(bridge, 'setRight')[0].args).toEqual(['k', '50%']);
        expect(callsFor(bridge, 'setBottom')[0].args).toEqual(['k', '50%']);
    });

    it('zero is a real value (not a no-op) for both numeric and percent', () => {
        // Spectr / Figma exports legitimately use top:0 to anchor an
        // overlay at the top edge. Truthiness gating would drop it.
        applyChangedProps(makeInstance('a'), {}, { top: 0 });
        expect(callsFor(bridge, 'setTop')[0].args).toEqual(['a', 0]);

        bridge.reset();
        applyChangedProps(makeInstance('b'), {}, { top: '0%' });
        expect(callsFor(bridge, 'setTop')[0].args).toEqual(['b', '0%']);
    });

    it('mixed percent + numeric across the four edges fans out independently', () => {
        // Mirroring a Figma-style frame with one percent edge and three
        // pixel edges — common in fixed-size sidebars anchored to a
        // percent-positioned content area.
        applyChangedProps(makeInstance(), {}, {
            top: '50%',
            left: 24,
            right: 24,
            bottom: 0,
        });
        expect(callsFor(bridge, 'setTop')[0].args).toEqual(['k', '50%']);
        expect(callsFor(bridge, 'setLeft')[0].args).toEqual(['k', 24]);
        expect(callsFor(bridge, 'setRight')[0].args).toEqual(['k', 24]);
        expect(callsFor(bridge, 'setBottom')[0].args).toEqual(['k', 0]);
    });
});
