// pulp #1548 — verify the @pulp/react prop-applier forwards the RN
// textShadow cluster (textShadowColor / textShadowOffset / textShadowRadius)
// to the matching bridge fns. RN's textShadowOffset arrives as
// `{ width, height }` and the prop-applier must split it into two scalars
// before bridge dispatch (the C++ setTextShadowOffset takes (id, dx, dy)).
//
// textShadow is paint-time only — Label::paint plumbs the trio into the
// canvas shadow state around fill_text. Each prop routes through its own
// per-attribute bridge fn so a JSX prop diff that touches one slot does
// not clobber the others.

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

describe('prop-applier textShadow cluster (pulp #1548)', () => {
    it('forwards textShadowColor verbatim', () => {
        applyChangedProps(makeInstance(), {}, { textShadowColor: '#3366ff' });
        const calls = callsFor(bridge, 'setTextShadowColor');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', '#3366ff']);
    });

    it('forwards textShadowRadius verbatim (number)', () => {
        applyChangedProps(makeInstance(), {}, { textShadowRadius: 4 });
        const calls = callsFor(bridge, 'setTextShadowRadius');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 4]);
    });

    it('splits RN textShadowOffset { width, height } into two scalars', () => {
        applyChangedProps(makeInstance(), {}, {
            textShadowOffset: { width: 2, height: -3 },
        });
        const calls = callsFor(bridge, 'setTextShadowOffset');
        expect(calls).toHaveLength(1);
        // Bridge signature: (id, dx, dy) — width → dx, height → dy.
        expect(calls[0].args).toEqual(['k', 2, -3]);
    });

    it('treats partial textShadowOffset (missing width or height) as 0', () => {
        // Defensive: RN consumers occasionally omit one half — match the
        // applier's coercion to 0 so a missing axis doesn't propagate NaN
        // into the bridge.
        applyChangedProps(makeInstance('a'), {}, {
            textShadowOffset: { width: 5 } as unknown as { width: number; height: number },
        });
        const a = callsFor(bridge, 'setTextShadowOffset');
        expect(a).toHaveLength(1);
        expect(a[0].args).toEqual(['a', 5, 0]);
    });

    it('all three textShadow-* props coexist on the same instance', () => {
        applyChangedProps(makeInstance(), {}, {
            textShadowColor: '#000080',
            textShadowOffset: { width: 1, height: 2 },
            textShadowRadius: 3,
        });
        expect(callsFor(bridge, 'setTextShadowColor')[0].args).toEqual(['k', '#000080']);
        expect(callsFor(bridge, 'setTextShadowOffset')[0].args).toEqual(['k', 1, 2]);
        expect(callsFor(bridge, 'setTextShadowRadius')[0].args).toEqual(['k', 3]);
    });

    it('textShadow cluster does NOT clobber sibling outline / boxShadow props', () => {
        applyChangedProps(makeInstance(), {}, {
            outlineColor: '#ff0000',
            outlineWidth: 1,
            textShadowColor: '#00ff00',
            textShadowRadius: 4,
        });
        // Each cluster goes to its own bridge fn — no cross-talk.
        expect(callsFor(bridge, 'setOutlineColor')).toHaveLength(1);
        expect(callsFor(bridge, 'setOutlineWidth')).toHaveLength(1);
        expect(callsFor(bridge, 'setTextShadowColor')).toHaveLength(1);
        expect(callsFor(bridge, 'setTextShadowRadius')).toHaveLength(1);
        // ... and zero on the OTHER cluster's setters.
        expect(callsFor(bridge, 'setTextShadowOffset')).toHaveLength(0);
        expect(callsFor(bridge, 'setOutlineStyle')).toHaveLength(0);
    });

    // pulp #1548 (Codex P2 audit follow-up) — when a textShadow* prop is
    // removed from JSX on a later render (common with conditional style
    // objects), applyChangedProps must dispatch an explicit reset so the
    // Label stops rendering the shadow. Without this, the previously-set
    // color/offset/radius would persist as stale visual state because no
    // setter overwrites them.
    describe('removed textShadow props are explicitly reset', () => {
        it('removed textShadowColor → setTextShadowColor(id, "transparent")', () => {
            applyChangedProps(makeInstance(), { textShadowColor: '#000000' }, {});
            const calls = callsFor(bridge, 'setTextShadowColor');
            expect(calls).toHaveLength(1);
            expect(calls[0].args).toEqual(['k', 'transparent']);
        });

        it('removed textShadowOffset → setTextShadowOffset(id, 0, 0)', () => {
            applyChangedProps(makeInstance(), {
                textShadowOffset: { width: 2, height: 3 },
            }, {});
            const calls = callsFor(bridge, 'setTextShadowOffset');
            expect(calls).toHaveLength(1);
            expect(calls[0].args).toEqual(['k', 0, 0]);
        });

        it('removed textShadowRadius → setTextShadowRadius(id, 0)', () => {
            applyChangedProps(makeInstance(), { textShadowRadius: 4 }, {});
            const calls = callsFor(bridge, 'setTextShadowRadius');
            expect(calls).toHaveLength(1);
            expect(calls[0].args).toEqual(['k', 0]);
        });

        it('removing the entire cluster dispatches all three resets', () => {
            applyChangedProps(makeInstance(), {
                textShadowColor: '#000080',
                textShadowOffset: { width: 1, height: 2 },
                textShadowRadius: 3,
            }, {});
            expect(callsFor(bridge, 'setTextShadowColor')).toHaveLength(1);
            expect(callsFor(bridge, 'setTextShadowColor')[0].args).toEqual(['k', 'transparent']);
            expect(callsFor(bridge, 'setTextShadowOffset')).toHaveLength(1);
            expect(callsFor(bridge, 'setTextShadowOffset')[0].args).toEqual(['k', 0, 0]);
            expect(callsFor(bridge, 'setTextShadowRadius')).toHaveLength(1);
            expect(callsFor(bridge, 'setTextShadowRadius')[0].args).toEqual(['k', 0]);
        });

        it('keeping a textShadow prop does NOT trigger the reset path', () => {
            // Sanity check — only the OLD prop being absent from NEW
            // triggers the reset. If the prop is still in newProps (even
            // unchanged), the reset must not fire.
            applyChangedProps(makeInstance(), { textShadowColor: '#000000' }, { textShadowColor: '#000000' });
            // Value didn't change → no setTextShadowColor call at all.
            expect(callsFor(bridge, 'setTextShadowColor')).toHaveLength(0);
        });
    });
});
