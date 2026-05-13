// pulp #1925 — disappearing-style-key resets in applyChangedProps.
//
// Pre-#1925, applyChangedProps only reset visible/opacity/overlay when a
// key fell out of newProps; the rest of the visual cluster (background,
// border, textColor, per-side borders) silently kept their last value.
//
// This bites the conditional-spread idiom that Spectr's Settings Manager
// Preset chips, PatternRow rows, and most imported designs (Stitch / v0
// / Figma) use:
//
//   style={{ ...base, ...(active ? activeStyle : {}) }}
//
// When `active` flips true → false, the spread contributes nothing, the
// active-only keys vanish from newProps, but the bridge keeps painting
// them. The contract pinned here is:
//
//   - background       removed → setBackground(id, "transparent")
//   - backgroundGradient removed → setBackground(id, "transparent")
//   - border           removed → setBorderWidth(id, 0)
//   - borderColor      removed → setBorderWidth(id, 0)
//   - borderWidth      removed → setBorderWidth(id, 0)
//   - borderTop/Right/Bottom/Left removed → setBorderSide(id, side, 0, "transparent")
//   - textColor        removed → setTextColor(id, "")

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

function makeInstance(id: string = 'chip', type: string = 'View'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

describe('@pulp/react prop-applier — disappearing style keys (pulp #1925)', () => {
    it('clears background when the key falls out of newProps', () => {
        applyChangedProps(
            makeInstance('c1'),
            { background: 'red' },
            {},
        );
        const calls = bridge.calls.filter((c) => c.fn === 'setBackground');
        expect(calls.length).toBe(1);
        expect(calls[0].args).toEqual(['c1', 'transparent']);
    });

    it('clears backgroundGradient via setBackground transparent', () => {
        applyChangedProps(
            makeInstance('c2'),
            { backgroundGradient: 'linear-gradient(red, blue)' },
            {},
        );
        const calls = bridge.calls.filter((c) => c.fn === 'setBackground');
        expect(calls.length).toBe(1);
        expect(calls[0].args).toEqual(['c2', 'transparent']);
    });

    it('clears border via setBorderWidth(0)', () => {
        applyChangedProps(
            makeInstance('c3'),
            { border: { color: 'red', width: 2 } },
            {},
        );
        const calls = bridge.calls.filter((c) => c.fn === 'setBorderWidth');
        expect(calls.length).toBe(1);
        expect(calls[0].args).toEqual(['c3', 0]);
    });

    it('clears borderColor via setBorderWidth(0)', () => {
        applyChangedProps(
            makeInstance('c4'),
            { borderColor: 'red' },
            {},
        );
        const calls = bridge.calls.filter((c) => c.fn === 'setBorderWidth');
        expect(calls.length).toBe(1);
        expect(calls[0].args).toEqual(['c4', 0]);
    });

    it('clears borderWidth via setBorderWidth(0)', () => {
        applyChangedProps(
            makeInstance('c5'),
            { borderWidth: 2 },
            {},
        );
        const calls = bridge.calls.filter((c) => c.fn === 'setBorderWidth');
        expect(calls.length).toBe(1);
        expect(calls[0].args).toEqual(['c5', 0]);
    });

    it('clears each per-side border via setBorderSide(side, 0, transparent)', () => {
        for (const side of ['Top', 'Right', 'Bottom', 'Left'] as const) {
            bridge.calls.length = 0;
            const key = `border${side}`;
            applyChangedProps(
                makeInstance('c6'),
                { [key]: { color: 'red', width: 2 } },
                {},
            );
            const calls = bridge.calls.filter((c) => c.fn === 'setBorderSide');
            expect(calls.length, `setBorderSide called once for ${key}`).toBe(1);
            expect(calls[0].args).toEqual(['c6', side.toLowerCase(), 0, 'transparent']);
        }
    });

    it('clears textColor via setTextColor("")', () => {
        applyChangedProps(
            makeInstance('c7', 'Text'),
            { textColor: '#fff' },
            {},
        );
        const calls = bridge.calls.filter((c) => c.fn === 'setTextColor');
        expect(calls.length).toBe(1);
        expect(calls[0].args).toEqual(['c7', '']);
    });

    it('conditional-spread true → false: active-only background + border clear', () => {
        // Mirrors Spectr Settings Manager Preset chip:
        //   style={{ ...base, ...(active ? activeStyle : {}) }}
        // Active state set background + borderColor; inactive drops them.
        const baseProps = { padding: 8, borderRadius: 4 };
        const active = { ...baseProps, background: '#3a7', borderColor: '#fff' };
        const inactive = { ...baseProps };
        applyChangedProps(makeInstance('chip-3'), active, inactive);
        const bg = bridge.calls.filter((c) => c.fn === 'setBackground');
        const bw = bridge.calls.filter((c) => c.fn === 'setBorderWidth');
        expect(bg.length).toBe(1);
        expect(bg[0].args).toEqual(['chip-3', 'transparent']);
        expect(bw.length).toBe(1);
        expect(bw[0].args).toEqual(['chip-3', 0]);
        // borderRadius is unchanged across old/new → no setBorderRadius call.
        expect(bridge.calls.some((c) => c.fn === 'setBorderRadius')).toBe(false);
    });

    it('does not clear when the key stays in newProps with a new value', () => {
        // Sanity: the changed-value path should run, NOT the disappearing-
        // key path. setBackground gets the new value, not "transparent".
        applyChangedProps(
            makeInstance('c8'),
            { background: 'red' },
            { background: 'blue' },
        );
        const calls = bridge.calls.filter((c) => c.fn === 'setBackground');
        expect(calls.length).toBe(1);
        expect(calls[0].args).toEqual(['c8', 'blue']);
    });
});
