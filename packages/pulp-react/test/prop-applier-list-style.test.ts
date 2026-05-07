// pulp #1514 — verify the @pulp/react prop-applier dispatches the
// list-style cluster (listStyle / listStyleType / listStyleImage /
// listStylePosition) to the correct bridge fns. Pulp doesn't model
// HTML <li>/<ul>/<ol> semantics; the bridge stores the values
// verbatim on the View so a future paint pass (or future
// semantic-list surface) can honor them. The catalog status is
// `partial` (stored, not painted).

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

describe('prop-applier list-style cluster (pulp #1514)', () => {
    it('listStyleType forwards each keyword verbatim', () => {
        for (const t of ['none', 'disc', 'circle', 'square', 'decimal'] as const) {
            const inst = makeInstance(t, 'View');
            applyChangedProps(inst, {}, { listStyleType: t });
        }
        const dispatched = callsFor(bridge, 'setListStyleType').map((c) => c.args[1]);
        expect(dispatched).toEqual(['none', 'disc', 'circle', 'square', 'decimal']);
    });

    it('listStyleImage forwards url() string verbatim', () => {
        applyChangedProps(makeInstance(), {}, { listStyleImage: 'url(bullet.png)' });
        const calls = callsFor(bridge, 'setListStyleImage');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 'url(bullet.png)']);
    });

    it('listStyleImage forwards "none" verbatim', () => {
        applyChangedProps(makeInstance(), {}, { listStyleImage: 'none' });
        const calls = callsFor(bridge, 'setListStyleImage');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 'none']);
    });

    it('listStylePosition forwards inside / outside', () => {
        applyChangedProps(makeInstance('a'), {}, { listStylePosition: 'inside' });
        applyChangedProps(makeInstance('b'), {}, { listStylePosition: 'outside' });
        const dispatched = callsFor(bridge, 'setListStylePosition').map((c) => c.args[1]);
        expect(dispatched).toEqual(['inside', 'outside']);
    });

    it('listStyle shorthand parses type + position + image (any order)', () => {
        applyChangedProps(makeInstance('a'), {}, {
            listStyle: 'square inside url(bullet.png)',
        });
        const typeCalls = callsFor(bridge, 'setListStyleType');
        const posCalls = callsFor(bridge, 'setListStylePosition');
        const imgCalls = callsFor(bridge, 'setListStyleImage');
        expect(typeCalls.map((c) => c.args[1])).toEqual(['square']);
        expect(posCalls.map((c) => c.args[1])).toEqual(['inside']);
        expect(imgCalls.map((c) => c.args[1])).toEqual(['url(bullet.png)']);
    });

    it('listStyle shorthand handles image-first / type-last order', () => {
        applyChangedProps(makeInstance('b'), {}, {
            listStyle: 'url(dot.png) circle outside',
        });
        const typeCalls = callsFor(bridge, 'setListStyleType');
        const posCalls = callsFor(bridge, 'setListStylePosition');
        const imgCalls = callsFor(bridge, 'setListStyleImage');
        expect(typeCalls.map((c) => c.args[1])).toEqual(['circle']);
        expect(posCalls.map((c) => c.args[1])).toEqual(['outside']);
        expect(imgCalls.map((c) => c.args[1])).toEqual(['url(dot.png)']);
    });

    it('listStyle: "none" routes to type (the common "list-style: none" reset)', () => {
        applyChangedProps(makeInstance('c'), {}, { listStyle: 'none' });
        const typeCalls = callsFor(bridge, 'setListStyleType');
        expect(typeCalls.map((c) => c.args[1])).toEqual(['none']);
        // No image dispatch — `none` alone is a type-reset.
        expect(callsFor(bridge, 'setListStyleImage')).toHaveLength(0);
    });

    it('listStyle coexists with sibling style props on the same instance', () => {
        applyChangedProps(makeInstance(), {}, {
            background: '#222',
            listStyle: 'disc inside',
            opacity: 0.5,
        });
        expect(callsFor(bridge, 'setBackground')).toHaveLength(1);
        expect(callsFor(bridge, 'setOpacity')).toHaveLength(1);
        expect(callsFor(bridge, 'setListStyleType').map((c) => c.args[1])).toEqual(['disc']);
        expect(callsFor(bridge, 'setListStylePosition').map((c) => c.args[1])).toEqual(['inside']);
    });
});
