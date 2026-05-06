// pulp #1434 Triage #10 — verify the @pulp/react prop-applier forwards
// `borderStyle` keyword strings verbatim to the bridge's setBorderStyle.
// Bridge maps to View::BorderStyle and Skia honors dashed / dotted via
// SkDashPathEffect at stroke time. Other named styles currently degrade
// to solid (paint-side gap, tracked for follow-up).

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

function styleCalls(b: MockBridge) {
    return b.calls.filter((c) => c.fn === 'setBorderStyle');
}

describe('prop-applier borderStyle (pulp #1434 Triage #10)', () => {
    it('forwards "solid" verbatim', () => {
        applyChangedProps(makeInstance(), {}, { borderStyle: 'solid' });
        expect(styleCalls(bridge)).toHaveLength(1);
        expect(styleCalls(bridge)[0].args).toEqual(['k', 'solid']);
    });

    it('forwards "dashed" verbatim', () => {
        applyChangedProps(makeInstance(), {}, { borderStyle: 'dashed' });
        expect(styleCalls(bridge)[0].args).toEqual(['k', 'dashed']);
    });

    it('forwards "dotted" verbatim', () => {
        applyChangedProps(makeInstance(), {}, { borderStyle: 'dotted' });
        expect(styleCalls(bridge)[0].args).toEqual(['k', 'dotted']);
    });

    it('forwards "double" / "groove" / "ridge" / "inset" / "outset"', () => {
        for (const s of ['double', 'groove', 'ridge', 'inset', 'outset'] as const) {
            const inst = makeInstance(s, 'View');
            applyChangedProps(inst, {}, { borderStyle: s });
        }
        const dispatched = styleCalls(bridge).map((c) => c.args[1]);
        expect(dispatched).toEqual(['double', 'groove', 'ridge', 'inset', 'outset']);
    });

    it('forwards "none" / "hidden"', () => {
        const a = makeInstance('a', 'View');
        const b = makeInstance('b', 'View');
        applyChangedProps(a, {}, { borderStyle: 'none' });
        applyChangedProps(b, {}, { borderStyle: 'hidden' });
        const args = styleCalls(bridge).map((c) => c.args[1]);
        expect(args).toEqual(['none', 'hidden']);
    });

    it('coexists with borderColor / borderWidth on the same instance', () => {
        applyChangedProps(makeInstance(), {}, {
            borderColor: '#ff0000',
            borderWidth: 2,
            borderStyle: 'dashed',
        });
        const colorCall = bridge.calls.find((c) => c.fn === 'setBorderColor');
        const widthCall = bridge.calls.find((c) => c.fn === 'setBorderWidth');
        const styleCall = bridge.calls.find((c) => c.fn === 'setBorderStyle');
        expect(colorCall?.args).toEqual(['k', '#ff0000']);
        expect(widthCall?.args).toEqual(['k', 2]);
        expect(styleCall?.args).toEqual(['k', 'dashed']);
    });
});
