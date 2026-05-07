// pulp #1434 Triage #15 — verify the @pulp/react prop-applier forwards
// `boxShadow` to the existing `setBoxShadow` bridge (or `clearBoxShadow`
// on `null` / `undefined` / `'none'`). Three input shapes:
//
//   1. CSS-spec single-shadow string: `'2px 4px 8px rgba(0,0,0,0.3)'`
//   2. Object form (RN-style): `{ offsetX, offsetY, blur, color, ... }`
//   3. Sentinel clear: `null` / `'none'` → `clearBoxShadow`
//
// Multi-shadow comma-separated lists are deferred — single-shadow path
// lands first.

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

function shadowCalls(b: MockBridge, fn: 'setBoxShadow' | 'clearBoxShadow') {
    return b.calls.filter((c) => c.fn === fn);
}

describe('prop-applier boxShadow (pulp #1434 Triage #15)', () => {
    it('parses CSS string: dx dy blur color', () => {
        applyChangedProps(makeInstance(), {}, { boxShadow: '2px 4px 8px rgba(0,0,0,0.3)' });
        const calls = shadowCalls(bridge, 'setBoxShadow');
        expect(calls).toHaveLength(1);
        // setBoxShadow(id, offsetX, offsetY, blur, spread, color, inset)
        expect(calls[0].args[0]).toBe('k');
        expect(calls[0].args[1]).toBe(2);
        expect(calls[0].args[2]).toBe(4);
        expect(calls[0].args[3]).toBe(8);
        expect(calls[0].args[4]).toBe(0);
        expect(calls[0].args[5]).toBe('rgba(0,0,0,0.3)');
        expect(calls[0].args[6]).toBe(false);
    });

    it('parses CSS string with explicit spread', () => {
        applyChangedProps(makeInstance(), {}, { boxShadow: '0px 6px 12px 2px #00000080' });
        const calls = shadowCalls(bridge, 'setBoxShadow');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 0, 6, 12, 2, '#00000080', false]);
    });

    it('parses CSS string with leading "inset" keyword', () => {
        applyChangedProps(makeInstance(), {}, { boxShadow: 'inset 2px 4px 8px rgba(0,0,0,0.3)' });
        const calls = shadowCalls(bridge, 'setBoxShadow');
        expect(calls).toHaveLength(1);
        expect(calls[0].args[6]).toBe(true);
    });

    it('parses CSS string with trailing "inset" keyword', () => {
        applyChangedProps(makeInstance(), {}, { boxShadow: '2px 4px 8px rgba(0,0,0,0.3) inset' });
        const calls = shadowCalls(bridge, 'setBoxShadow');
        expect(calls).toHaveLength(1);
        expect(calls[0].args[6]).toBe(true);
    });

    it('parses negative offsets', () => {
        applyChangedProps(makeInstance(), {}, { boxShadow: '-2px -4px 6px #ff0000' });
        const calls = shadowCalls(bridge, 'setBoxShadow');
        expect(calls).toHaveLength(1);
        expect(calls[0].args[1]).toBe(-2);
        expect(calls[0].args[2]).toBe(-4);
    });

    it('forwards object-form boxShadow with all fields', () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: {
                offsetX: 2,
                offsetY: 4,
                blur: 8,
                spread: 1,
                color: '#0000ff',
                inset: true,
            },
        });
        const calls = shadowCalls(bridge, 'setBoxShadow');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 2, 4, 8, 1, '#0000ff', true]);
    });

    it('forwards object-form with defaults for blur / spread / inset', () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: { offsetX: 1, offsetY: 2, color: '#000' },
        });
        const calls = shadowCalls(bridge, 'setBoxShadow');
        expect(calls).toHaveLength(1);
        // blur default = 4, spread default = 0, inset default = false
        expect(calls[0].args).toEqual(['k', 1, 2, 4, 0, '#000', false]);
    });

    it("'none' clears the shadow", () => {
        applyChangedProps(makeInstance(), {}, { boxShadow: 'none' });
        expect(shadowCalls(bridge, 'clearBoxShadow')).toHaveLength(1);
        expect(shadowCalls(bridge, 'setBoxShadow')).toHaveLength(0);
    });

    it('removing the shadow (prev set, next undefined) clears it via empty-string', () => {
        // applyChangedProps treats `undefined` / `null` as "key was
        // removed" and doesn't dispatch — same shape as other style
        // props. Consumers wanting an explicit clear pass `'none'` or
        // an empty string.
        applyChangedProps(makeInstance(), {}, { boxShadow: '' });
        expect(shadowCalls(bridge, 'clearBoxShadow')).toHaveLength(1);
    });

    it('unparseable string is silently dropped', () => {
        applyChangedProps(makeInstance(), {}, { boxShadow: 'banana' });
        expect(shadowCalls(bridge, 'setBoxShadow')).toHaveLength(0);
        expect(shadowCalls(bridge, 'clearBoxShadow')).toHaveLength(0);
    });
});
