// Typography lengths reach their bridge setters as pixel numbers. The setters
// read a string as its leading number with the unit dropped, so every unit is
// resolved here, by the property table and the active dialect, never by the
// value's magnitude.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyAllProps } from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { resolveTypographyLength, setLengthDialect } from '../src/prop-applier-typography.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    bridge.uninstall();
    setLengthDialect('auto');
});

function applied(props: Record<string, unknown>, verb: string): unknown[] | undefined {
    const instance: PulpInstance = {
        id: 'k', type: 'Label' as PulpInstance['type'], props, childIds: [],
        onBridge: true, pendingChildren: [],
    };
    applyAllProps(instance);
    return bridge.calls.find((c) => c.fn === verb)?.args;
}

describe('typography length resolution', () => {
    it.each([
        // [property, value, own props, expected px]
        ['letterSpacing', '0.08em', { fontSize: 20 }, 1.6],
        ['letterSpacing', '0.1em', { fontSize: 20 }, 2],
        ['letterSpacing', '2px', {}, 2],
        ['letterSpacing', 1.5, {}, 1.5],
        ['letterSpacing', 'normal', {}, 0],
        ['letterSpacing', '-0.02em', { fontSize: '1.5rem' }, -0.48],
        ['fontSize', '16px', {}, 16],
        ['fontSize', '1.5rem', {}, 24],
        ['fontSize', '2em', {}, 28],
        ['fontSize', '150%', {}, 21],
        ['fontSize', 13, {}, 13],
        ['lineHeight', '6px', { fontSize: 20 }, 6],
        ['lineHeight', '120%', { fontSize: 10 }, 12],
        ['lineHeight', '1.5em', { fontSize: '16px' }, 24],
        ['lineHeight', '2rem', {}, 32],
        ['lineHeight', 'normal', {}, 0],
        ['lineHeight', 1.5, { fontSize: 16 }, 24],
        ['lineHeight', '1.25', { fontSize: 20 }, 25],
        ['lineHeight', 24, {}, 24],
    ])('%s %j with %j resolves to %d px', (property, value, props, expected) => {
        const px = resolveTypographyLength(property, value, props);
        expect(px).not.toBeNull();
        expect(px as number).toBeCloseTo(expected, 6);
    });

    it('refuses values it cannot resolve instead of guessing', () => {
        expect(resolveTypographyLength('fontSize', 'calc(10px + 2px)', {})).toBeNull();
        expect(resolveTypographyLength('fontSize', 'normal', {})).toBeNull();
        expect(resolveTypographyLength('letterSpacing', '1vw', {})).toBeNull();
        expect(resolveTypographyLength('letterSpacing', {}, {})).toBeNull();
    });

    it('rejects a long digit run that fails at its end in linear time', () => {
        // A pattern that can split one run of digits two ways backtracks
        // quadratically on '000...0x'; this length takes seconds there.
        const hostile = '0'.repeat(40000) + 'x';
        const started = performance.now();
        expect(resolveTypographyLength('letterSpacing', hostile, {})).toBeNull();
        expect(performance.now() - started).toBeLessThan(250);
        // CONTROL: the same digits with a valid ending still resolve.
        expect(resolveTypographyLength('letterSpacing', '0'.repeat(40000) + '1px', {})).toBe(1);
    });

    it('the dialect decides a bare multiplier-property number, not its size', () => {
        setLengthDialect('dom');
        expect(resolveTypographyLength('lineHeight', 24, { fontSize: 10 })).toBe(240);
        expect(resolveTypographyLength('lineHeight', 1.5, { fontSize: 10 })).toBe(15);
        setLengthDialect('native');
        expect(resolveTypographyLength('lineHeight', 1.5, { fontSize: 10 })).toBe(1.5);
        expect(resolveTypographyLength('lineHeight', 24, { fontSize: 10 })).toBe(24);
        // A unit-bearing string never consults the dialect.
        expect(resolveTypographyLength('lineHeight', '6px', { fontSize: 10 })).toBe(6);
        setLengthDialect('dom');
        expect(resolveTypographyLength('lineHeight', '6px', { fontSize: 10 })).toBe(6);
        // A pixel property ignores the dialect entirely.
        expect(resolveTypographyLength('letterSpacing', 3, {})).toBe(3);
    });
});

describe('typography setters receive resolved pixels', () => {
    it('forwards em letter spacing as pixels of the own font size', () => {
        expect(applied({ fontSize: 20, letterSpacing: '0.08em' }, 'setLetterSpacing'))
            .toEqual(['k', expect.closeTo(1.6, 6)]);
    });

    it('forwards a unit string font size as a number', () => {
        expect(applied({ fontSize: '1.5rem' }, 'setFontSize')).toEqual(['k', 24]);
    });

    it('keeps a px line height absolute even when small', () => {
        expect(applied({ fontSize: 20, lineHeight: '6px' }, 'setLineHeight')).toEqual(['k', 6]);
    });

    it('does not call a setter for a value it cannot resolve', () => {
        expect(applied({ fontSize: 'calc(1em + 2px)' }, 'setFontSize')).toBeUndefined();
    });
});
