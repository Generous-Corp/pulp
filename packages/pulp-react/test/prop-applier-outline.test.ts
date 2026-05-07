// pulp #1519 — verify the @pulp/react prop-applier forwards the RN
// outline cluster (outlineColor / outlineOffset / outlineStyle /
// outlineWidth) verbatim to the matching bridge setOutlineX fns.
//
// Outline is a paint-time ring drawn OUTSIDE the border-box and does
// NOT take up Yoga layout space (no parent reservation). Each prop
// must route through its own per-attribute bridge fn so a JSX prop
// diff that touches one outline-* preserves the others — same shape
// as the borderColor / borderWidth / borderStyle cluster (#1027 +
// #1434 Triage #10).

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

describe('prop-applier outline cluster (pulp #1519)', () => {
    it('forwards outlineColor verbatim', () => {
        applyChangedProps(makeInstance(), {}, { outlineColor: '#ff8800' });
        const calls = callsFor(bridge, 'setOutlineColor');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', '#ff8800']);
    });

    it('forwards outlineOffset verbatim (number)', () => {
        applyChangedProps(makeInstance(), {}, { outlineOffset: 4 });
        const calls = callsFor(bridge, 'setOutlineOffset');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 4]);
    });

    it('forwards outlineStyle keyword set verbatim', () => {
        const styles = [
            'solid', 'dashed', 'dotted', 'double',
            'groove', 'ridge', 'inset', 'outset',
            'none', 'hidden',
        ] as const;
        for (const s of styles) {
            const inst = makeInstance(s, 'View');
            applyChangedProps(inst, {}, { outlineStyle: s });
        }
        const dispatched = callsFor(bridge, 'setOutlineStyle').map((c) => c.args[1]);
        expect(dispatched).toEqual([...styles]);
    });

    it('forwards outlineWidth verbatim (number)', () => {
        applyChangedProps(makeInstance(), {}, { outlineWidth: 2.5 });
        const calls = callsFor(bridge, 'setOutlineWidth');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 2.5]);
    });

    it('all four outline-* props coexist on the same instance', () => {
        applyChangedProps(makeInstance(), {}, {
            outlineColor: '#0080ff',
            outlineOffset: 3,
            outlineStyle: 'dashed',
            outlineWidth: 2,
        });
        expect(callsFor(bridge, 'setOutlineColor')[0].args).toEqual(['k', '#0080ff']);
        expect(callsFor(bridge, 'setOutlineOffset')[0].args).toEqual(['k', 3]);
        expect(callsFor(bridge, 'setOutlineStyle')[0].args).toEqual(['k', 'dashed']);
        expect(callsFor(bridge, 'setOutlineWidth')[0].args).toEqual(['k', 2]);
    });

    it('outline cluster does NOT clobber sibling border props', () => {
        applyChangedProps(makeInstance(), {}, {
            borderColor: '#ff0000',
            borderWidth: 1,
            outlineColor: '#00ff00',
            outlineWidth: 4,
        });
        // Each cluster goes to its own bridge fn — no cross-talk.
        expect(callsFor(bridge, 'setBorderColor')).toHaveLength(1);
        expect(callsFor(bridge, 'setBorderWidth')).toHaveLength(1);
        expect(callsFor(bridge, 'setOutlineColor')).toHaveLength(1);
        expect(callsFor(bridge, 'setOutlineWidth')).toHaveLength(1);
        // ... and zero on the OTHER cluster's setters.
        expect(callsFor(bridge, 'setOutlineStyle')).toHaveLength(0);
        expect(callsFor(bridge, 'setOutlineOffset')).toHaveLength(0);
    });
});
