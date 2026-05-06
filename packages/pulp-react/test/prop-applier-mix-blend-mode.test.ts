// pulp #1549 — RN `mixBlendMode` (RN 0.76 New Architecture). Verify the
// `@pulp/react` prop-applier forwards the 16 W3C blend-mode keywords to
// the matching bridge fn `setMixBlendMode`. The bridge keyword→
// canvas::Canvas::BlendMode mapping itself is exercised by C++ tests; this
// test guarantees the JSX dispatch is wired and routes the string through
// verbatim.

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

function makeInstance(id: string = 'm', type: string = 'View'): PulpInstance {
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

const W3C_BLEND_KEYWORDS = [
    'normal', 'multiply', 'screen', 'overlay',
    'darken', 'lighten', 'color-dodge', 'color-burn',
    'hard-light', 'soft-light', 'difference', 'exclusion',
    'hue', 'saturation', 'color', 'luminosity',
] as const;

describe('rn mixBlendMode (pulp #1549)', () => {
    it('multiply forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { mixBlendMode: 'multiply' });
        expect(callOf(bridge, 'setMixBlendMode')?.args).toEqual(['m', 'multiply']);
    });

    it('screen forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { mixBlendMode: 'screen' });
        expect(callOf(bridge, 'setMixBlendMode')?.args).toEqual(['m', 'screen']);
    });

    it('all 16 W3C blend keywords forward verbatim', () => {
        for (const kw of W3C_BLEND_KEYWORDS) {
            const b = createMockBridge();
            b.install();
            try {
                applyChangedProps(makeInstance(), {}, { mixBlendMode: kw });
                expect(callOf(b, 'setMixBlendMode')?.args).toEqual(['m', kw]);
            } finally {
                b.uninstall();
            }
        }
    });

    it('no dispatch when prop unchanged', () => {
        applyChangedProps(
            makeInstance(),
            { mixBlendMode: 'multiply' },
            { mixBlendMode: 'multiply' },
        );
        expect(callOf(bridge, 'setMixBlendMode')).toBeUndefined();
    });

    it('coexists with other paint-time props', () => {
        applyChangedProps(makeInstance(), {}, {
            opacity: 0.5,
            filter: 'blur(2px)',
            mixBlendMode: 'overlay',
        });
        expect(callOf(bridge, 'setOpacity')?.args[1]).toBe(0.5);
        expect(callOf(bridge, 'setFilter')?.args[1]).toBe('blur(2px)');
        expect(callOf(bridge, 'setMixBlendMode')?.args[1]).toBe('overlay');
    });
});
