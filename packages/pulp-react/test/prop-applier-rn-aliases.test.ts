// pulp #1434 batch 4 — verify the @pulp/react prop-applier fans out
// React Native shorthand aliases (marginHorizontal, marginVertical,
// paddingHorizontal, paddingVertical) to the per-edge bridge setters.
//
// Pulp's value is broad import-readiness from {Figma, Pencil.dev, v0,
// Claude Design HTML, RN, generic HTML/CSS/React}. RN code commonly
// writes `style={{ marginHorizontal: 8 }}` and expects the framework to
// decompose the shorthand to marginLeft + marginRight on the underlying
// layout. Without a fan-out path the alias was silently dropped at the
// JSX entry point, so RN snippets ported verbatim lost their padding /
// margin information. The fan-out lands at two layers:
//
//   - core/view/js/web-compat-style-decl.js — DOM-lite el.style adapter
//     (covered by harness coverage on the css/* surface).
//   - packages/pulp-react/src/prop-applier.ts — @pulp/react JSX
//     intrinsic (covered by *this* test file plus the harness rn/*
//     surface adapter).
//
// Each test asserts that ONE alias triggers exactly TWO setFlex calls
// targeting the matching per-edge slots with the supplied value.

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

describe('prop-applier RN shorthand aliases (pulp #1434 batch 4)', () => {
    it('marginHorizontal fans out to margin_left + margin_right', () => {
        applyChangedProps(makeInstance(), {}, { marginHorizontal: 8 });

        const left = flexCalls(bridge, 'margin_left');
        const right = flexCalls(bridge, 'margin_right');
        expect(left).toHaveLength(1);
        expect(right).toHaveLength(1);
        expect(left[0].args).toEqual(['k', 'margin_left', 8]);
        expect(right[0].args).toEqual(['k', 'margin_right', 8]);

        // Sanity — no stray top/bottom emission from this alias.
        expect(flexCalls(bridge, 'margin_top')).toHaveLength(0);
        expect(flexCalls(bridge, 'margin_bottom')).toHaveLength(0);
    });

    it('marginVertical fans out to margin_top + margin_bottom', () => {
        applyChangedProps(makeInstance(), {}, { marginVertical: 12 });

        const top = flexCalls(bridge, 'margin_top');
        const bot = flexCalls(bridge, 'margin_bottom');
        expect(top).toHaveLength(1);
        expect(bot).toHaveLength(1);
        expect(top[0].args).toEqual(['k', 'margin_top', 12]);
        expect(bot[0].args).toEqual(['k', 'margin_bottom', 12]);

        expect(flexCalls(bridge, 'margin_left')).toHaveLength(0);
        expect(flexCalls(bridge, 'margin_right')).toHaveLength(0);
    });

    it('paddingHorizontal fans out to padding_left + padding_right', () => {
        applyChangedProps(makeInstance(), {}, { paddingHorizontal: 16 });

        const left = flexCalls(bridge, 'padding_left');
        const right = flexCalls(bridge, 'padding_right');
        expect(left).toHaveLength(1);
        expect(right).toHaveLength(1);
        expect(left[0].args).toEqual(['k', 'padding_left', 16]);
        expect(right[0].args).toEqual(['k', 'padding_right', 16]);

        expect(flexCalls(bridge, 'padding_top')).toHaveLength(0);
        expect(flexCalls(bridge, 'padding_bottom')).toHaveLength(0);
    });

    it('paddingVertical fans out to padding_top + padding_bottom', () => {
        applyChangedProps(makeInstance(), {}, { paddingVertical: 4 });

        const top = flexCalls(bridge, 'padding_top');
        const bot = flexCalls(bridge, 'padding_bottom');
        expect(top).toHaveLength(1);
        expect(bot).toHaveLength(1);
        expect(top[0].args).toEqual(['k', 'padding_top', 4]);
        expect(bot[0].args).toEqual(['k', 'padding_bottom', 4]);

        expect(flexCalls(bridge, 'padding_left')).toHaveLength(0);
        expect(flexCalls(bridge, 'padding_right')).toHaveLength(0);
    });

    it('zero is a real value (not a no-op) and fans out unchanged', () => {
        // RN zero is meaningful — explicit "no margin/padding" override
        // when a parent style was setting it. The fan-out must not gate
        // on truthiness.
        applyChangedProps(makeInstance(), {}, { marginHorizontal: 0 });
        const left = flexCalls(bridge, 'margin_left');
        const right = flexCalls(bridge, 'margin_right');
        expect(left[0]?.args[2]).toBe(0);
        expect(right[0]?.args[2]).toBe(0);
    });

    it('combining horizontal + vertical aliases hits all four edges', () => {
        applyChangedProps(makeInstance(), {}, {
            marginHorizontal: 8,
            marginVertical: 12,
            paddingHorizontal: 16,
            paddingVertical: 4,
        });

        // 4 aliases × 2 edges = 8 setFlex calls.
        const allFlex = bridge.calls.filter((c) => c.fn === 'setFlex');
        expect(allFlex).toHaveLength(8);

        expect(flexCalls(bridge, 'margin_left')[0]?.args[2]).toBe(8);
        expect(flexCalls(bridge, 'margin_right')[0]?.args[2]).toBe(8);
        expect(flexCalls(bridge, 'margin_top')[0]?.args[2]).toBe(12);
        expect(flexCalls(bridge, 'margin_bottom')[0]?.args[2]).toBe(12);
        expect(flexCalls(bridge, 'padding_left')[0]?.args[2]).toBe(16);
        expect(flexCalls(bridge, 'padding_right')[0]?.args[2]).toBe(16);
        expect(flexCalls(bridge, 'padding_top')[0]?.args[2]).toBe(4);
        expect(flexCalls(bridge, 'padding_bottom')[0]?.args[2]).toBe(4);
    });

    it('explicit per-edge prop set alongside an alias reaches both edges', () => {
        // The prop-applier processes prop-keys in iteration order.
        // marginHorizontal sets BOTH edges to its value; a subsequent
        // marginRight in the same props bag overrides only the right
        // edge. This mirrors RN behavior where per-edge wins.
        applyChangedProps(makeInstance(), {}, {
            marginHorizontal: 8,
            marginRight: 24,
        });
        // margin_left set once (from alias) at 8; margin_right set
        // twice — once at 8 from the alias, once at 24 from the
        // explicit prop. The bridge uses the latest value, and the
        // prop-applier emits both calls.
        const right = flexCalls(bridge, 'margin_right');
        expect(right.length).toBeGreaterThanOrEqual(1);
        // The last setFlex call for margin_right wins on the bridge
        // side, so assert the most recent value reflects the explicit
        // override.
        expect(right[right.length - 1].args[2]).toBe(24);
        const left = flexCalls(bridge, 'margin_left');
        expect(left[0].args[2]).toBe(8);
    });
});
