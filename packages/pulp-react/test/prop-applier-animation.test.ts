// pulp #1508 Codex audit (P1 #2) — animation* props must dispatch to
// the animation API, not to the transition equivalents. The original
// prop-applier wired `animationDuration` to `setTransitionDuration`,
// which mutated *transition* timing on the same View. The fix routes
// every animation* longhand through the legacy 2-arg `setAnimation`
// control-token form — which the C++ bridge stages on the View's
// pending-animation slot until the `name` token arrives and resolves
// against the keyframes registry.

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

function callsOf(b: MockBridge, fn: string) {
    return b.calls.filter((c) => c.fn === fn);
}

describe('animation* props dispatch to setAnimation (pulp #1508 P1)', () => {
    it('animationDuration goes to setAnimation, NOT setTransitionDuration', () => {
        applyChangedProps(makeInstance(), {}, { animationDuration: '250ms' });
        // Wrong dispatch — must NOT happen post-fix.
        expect(callsOf(bridge, 'setTransitionDuration')).toHaveLength(0);
        // Correct dispatch — legacy control-token form.
        const anim = callsOf(bridge, 'setAnimation');
        expect(anim).toHaveLength(1);
        expect(anim[0].args).toEqual(['k', 'duration', 0.25]);
    });

    it('animationDuration accepts a numeric value (seconds)', () => {
        applyChangedProps(makeInstance(), {}, { animationDuration: 0.4 });
        const anim = callsOf(bridge, 'setAnimation');
        expect(anim).toHaveLength(1);
        expect(anim[0].args).toEqual(['k', 'duration', 0.4]);
    });

    it('animationDelay routes to setAnimation/delay', () => {
        applyChangedProps(makeInstance(), {}, { animationDelay: '100ms' });
        expect(callsOf(bridge, 'setTransitionDelay')).toHaveLength(0);
        const anim = callsOf(bridge, 'setAnimation');
        expect(anim).toHaveLength(1);
        expect(anim[0].args).toEqual(['k', 'delay', 0.1]);
    });

    it('animationTimingFunction routes to setAnimation/easing', () => {
        applyChangedProps(makeInstance(), {}, { animationTimingFunction: 'ease-in-out' });
        const anim = callsOf(bridge, 'setAnimation');
        expect(anim).toHaveLength(1);
        expect(anim[0].args).toEqual(['k', 'easing', 'ease-in-out']);
    });

    it('animationIterationCount maps "infinite" to -1', () => {
        applyChangedProps(makeInstance(), {}, { animationIterationCount: 'infinite' });
        const anim = callsOf(bridge, 'setAnimation');
        expect(anim).toHaveLength(1);
        expect(anim[0].args).toEqual(['k', 'iterations', -1]);
    });

    it('animationIterationCount passes a finite count through', () => {
        applyChangedProps(makeInstance(), {}, { animationIterationCount: 3 });
        const anim = callsOf(bridge, 'setAnimation');
        expect(anim).toHaveLength(1);
        expect(anim[0].args).toEqual(['k', 'iterations', 3]);
    });

    it('animationDirection routes to setAnimation/direction', () => {
        applyChangedProps(makeInstance(), {}, { animationDirection: 'reverse' });
        const anim = callsOf(bridge, 'setAnimation');
        expect(anim).toHaveLength(1);
        expect(anim[0].args).toEqual(['k', 'direction', 'reverse']);
    });

    it('animationFillMode routes to setAnimation/fill', () => {
        applyChangedProps(makeInstance(), {}, { animationFillMode: 'forwards' });
        const anim = callsOf(bridge, 'setAnimation');
        expect(anim).toHaveLength(1);
        expect(anim[0].args).toEqual(['k', 'fill', 'forwards']);
    });

    it('animationName takes the positional setAnimation ABI (5-arg)', () => {
        applyChangedProps(makeInstance(), {}, { animationName: 'fade-in' });
        const anim = callsOf(bridge, 'setAnimation');
        expect(anim).toHaveLength(1);
        expect(anim[0].args).toEqual(['k', 'fade-in', 1.0, 1, 'normal']);
    });
});
