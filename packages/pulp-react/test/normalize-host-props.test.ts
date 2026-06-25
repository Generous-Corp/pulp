// Unit tests for the prop-applier's normalizeHostProps and
// classRulesProvider surface.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import {
    normalizeHostProps,
    setClassRulesProvider,
    getClassRulesProvider,
} from '../src/prop-applier.js';

afterEach(() => {
    setClassRulesProvider(null);
});

describe('normalizeHostProps', () => {
    it('returns input unchanged when no style + no className (fast path)', () => {
        const flat = { width: 100, color: 'red' };
        expect(normalizeHostProps('div', flat)).toBe(flat);
    });

    it('flattens style object into top-level props', () => {
        const out = normalizeHostProps('div', { style: { width: 100, color: 'red' } });
        expect(out.width).toBe(100);
        expect(out.color).toBe('red');
        expect(out.style).toBeUndefined();
    });

    it('flat props override style props (flat > style precedence)', () => {
        const out = normalizeHostProps('div', {
            style: { width: 100 },
            width: 200,
        });
        expect(out.width).toBe(200);
    });

    it('class-rules provider merges className tokens into style slot', () => {
        setClassRulesProvider((cls) => {
            if (cls === 'card') return { padding: 8, background: '#fff' };
            if (cls === 'shadow') return { boxShadow: '0 2px 4px rgba(0,0,0,0.1)' };
            return null;
        });
        const out = normalizeHostProps('div', { className: 'card shadow' });
        expect(out.padding).toBe(8);
        expect(out.background).toBe('#fff');
        expect(out.boxShadow).toBe('0 2px 4px rgba(0,0,0,0.1)');
    });

    it('style overrides className rules (className < style precedence)', () => {
        setClassRulesProvider((cls) => cls === 'card' ? { padding: 8 } : null);
        const out = normalizeHostProps('div', {
            className: 'card',
            style: { padding: 16 },
        });
        expect(out.padding).toBe(16);
    });

    it('flat props override className rules (className < flat precedence)', () => {
        setClassRulesProvider((cls) => cls === 'card' ? { padding: 8 } : null);
        const out = normalizeHostProps('div', {
            className: 'card',
            padding: 24,
        });
        expect(out.padding).toBe(24);
    });

    it('no class-rules provider installed → className alone returns input (no flatten)', () => {
        // With no provider, hasClassName is true but no rules merge.
        // The function still strips className from output and copies
        // flat props. Style absent + provider absent = empty out.
        setClassRulesProvider(null);
        const out = normalizeHostProps('div', { className: 'card', width: 100 });
        expect(out.width).toBe(100);
        expect(out.className).toBeUndefined();
    });

    // Class-rule output must not be able to mutate Object.prototype.
    it('class-rules provider returning __proto__ key does not poison Object.prototype', () => {
        setClassRulesProvider(() => ({ __proto__: { polluted: 'yes' } } as Record<string, unknown>));
        normalizeHostProps('div', { className: 'attacker' });
        // Object.prototype must not have a new `polluted` field.
        expect(({} as Record<string, unknown>).polluted).toBeUndefined();
    });

    it('class-rules provider returning constructor / prototype keys are filtered', () => {
        setClassRulesProvider(() => ({
            constructor: 'evil',
            prototype: 'evil',
            __proto__: 'evil',
            width: 100,
        } as Record<string, unknown>));
        const out = normalizeHostProps('div', { className: 'attacker' });
        expect(out.width).toBe(100);
        expect(out.constructor).not.toBe('evil');
        expect(out.prototype).toBeUndefined();
    });
});

describe('classRulesProvider', () => {
    it('setClassRulesProvider null clears the provider', () => {
        setClassRulesProvider((cls) => ({ width: 100 }));
        expect(getClassRulesProvider()).not.toBeNull();
        setClassRulesProvider(null);
        expect(getClassRulesProvider()).toBeNull();
    });

    it('getClassRulesProvider returns the installed fn', () => {
        const fn = (cls: string) => ({ width: 100 });
        setClassRulesProvider(fn);
        expect(getClassRulesProvider()).toBe(fn);
    });
});
