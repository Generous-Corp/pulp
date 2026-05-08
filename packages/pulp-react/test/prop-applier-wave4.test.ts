// pulp Wave 4 rn extensive DIVERGE sweep — verifies the wiring landed
// for the harness rn surface drift bundle (16 DIVERGE → 0 DIVERGE):
//
//   • RN Fabric `BoxShadowValue[]` array form on `boxShadow`
//   • textShadow* per-attribute setters now register on the bridge
//
// Other entries in the sweep flipped via catalog reclassification only
// (gotcha-ifying paint-side gaps that were over-reported as
// `unsupportedValues`); those don't need new prop-applier behavior tests.

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

function makeInstance(id: string = 'w', type: string = 'View'): PulpInstance {
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

describe('Wave 4 rn — boxShadow BoxShadowValue[] array form', () => {
    it('dispatches one setBoxShadow per element + clearBoxShadow first', () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: [
                { offsetX: 2, offsetY: 4, blurRadius: 8, color: 'rgba(0,0,0,0.3)' },
                { offsetX: 0, offsetY: 1, blurRadius: 2, spreadDistance: 1, color: '#000', inset: true },
            ],
        });
        const clearCalls = callsOf(bridge, 'clearBoxShadow');
        const setCalls   = callsOf(bridge, 'setBoxShadow');
        expect(clearCalls).toHaveLength(1);
        expect(setCalls).toHaveLength(2);
        // First: offsetX=2, offsetY=4, blur=8, spread=0, color=rgba(0,0,0,0.3), inset=false
        expect(setCalls[0].args).toEqual(['w', 2, 4, 8, 0, 'rgba(0,0,0,0.3)', false]);
        // Second: offsetX=0, offsetY=1, blur=2, spread=1, color=#000, inset=true
        expect(setCalls[1].args).toEqual(['w', 0, 1, 2, 1, '#000', true]);
    });

    it('honors RN Fabric field names blurRadius / spreadDistance', () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: [
                { offsetX: 1, offsetY: 2, blurRadius: 6, spreadDistance: 3, color: 'red' },
            ],
        });
        const setCalls = callsOf(bridge, 'setBoxShadow');
        expect(setCalls).toHaveLength(1);
        expect(setCalls[0].args).toEqual(['w', 1, 2, 6, 3, 'red', false]);
    });

    it('falls back to defaults when blur/spread omitted', () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: [{ offsetX: 0, offsetY: 0, color: '#fff' }],
        });
        const setCalls = callsOf(bridge, 'setBoxShadow');
        expect(setCalls).toHaveLength(1);
        // Default blur=4, spread=0, inset=false (matches scalar object branch).
        expect(setCalls[0].args).toEqual(['w', 0, 0, 4, 0, '#fff', false]);
    });

    it('also accepts CSS field names blur / spread inside the array elements', () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: [{ offsetX: 5, offsetY: 5, blur: 10, spread: 2, color: '#abc' }],
        });
        const setCalls = callsOf(bridge, 'setBoxShadow');
        expect(setCalls).toHaveLength(1);
        expect(setCalls[0].args).toEqual(['w', 5, 5, 10, 2, '#abc', false]);
    });

    it('empty array still clears (no setBoxShadow dispatches)', () => {
        applyChangedProps(makeInstance(), {}, { boxShadow: [] });
        expect(callsOf(bridge, 'clearBoxShadow')).toHaveLength(1);
        expect(callsOf(bridge, 'setBoxShadow')).toHaveLength(0);
    });

    it('skips falsy elements but keeps the rest', () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: [
                null,
                { offsetX: 1, offsetY: 1, color: '#000' },
                undefined,
            ] as any,
        });
        expect(callsOf(bridge, 'clearBoxShadow')).toHaveLength(1);
        expect(callsOf(bridge, 'setBoxShadow')).toHaveLength(1);
    });
});

describe('Wave 4 rn — textShadow* per-attribute dispatch (#1548)', () => {
    it('dispatches setTextShadowColor with the color string', () => {
        applyChangedProps(makeInstance(), {}, { textShadowColor: '#ff00aa' } as any);
        const calls = callsOf(bridge, 'setTextShadowColor');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['w', '#ff00aa']);
    });

    it('dispatches setTextShadowOffset with dx, dy from { width, height }', () => {
        applyChangedProps(makeInstance(), {}, {
            textShadowOffset: { width: 2, height: 4 },
        } as any);
        const calls = callsOf(bridge, 'setTextShadowOffset');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['w', 2, 4]);
    });

    it('defaults missing axis to 0 on textShadowOffset', () => {
        applyChangedProps(makeInstance(), {}, {
            textShadowOffset: { width: 3 },
        } as any);
        const calls = callsOf(bridge, 'setTextShadowOffset');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['w', 3, 0]);
    });

    it('dispatches setTextShadowRadius with the px number', () => {
        applyChangedProps(makeInstance(), {}, { textShadowRadius: 6 } as any);
        const calls = callsOf(bridge, 'setTextShadowRadius');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['w', 6]);
    });

    it('per-attribute setters compose without clobbering each other', () => {
        applyChangedProps(makeInstance(), {}, {
            textShadowColor:  '#000',
            textShadowOffset: { width: 1, height: 2 },
            textShadowRadius: 4,
        } as any);
        expect(callsOf(bridge, 'setTextShadowColor')).toHaveLength(1);
        expect(callsOf(bridge, 'setTextShadowOffset')).toHaveLength(1);
        expect(callsOf(bridge, 'setTextShadowRadius')).toHaveLength(1);
    });
});
