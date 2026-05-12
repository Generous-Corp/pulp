// pulp #109 — verify host-config's shouldSetTextContent recognizes
// HTML-intrinsic text-bearing tags. Without these aliases, React's
// host-config returned false from shouldSetTextContent('span', …),
// causing React to materialize a synthetic Label child for the string
// content. That synthetic child stacked on top of the outer span's
// own auto-derived text — the 2026-05-11 Spectr regression where every
// <span>SPECTR</span> rendered as two overlapping "SPECTR" labels.

import { describe, it, expect } from 'vitest';
import { PulpHostConfig } from '../src/host-config.js';

describe('host-config shouldSetTextContent (pulp #109 — TEXT_BEARING)', () => {
    const fn = PulpHostConfig.shouldSetTextContent as
        (type: string, props: Record<string, unknown>) => boolean;

    it('Label / Button / TextEditor (native pulp types) bear text', () => {
        expect(fn('Label', {})).toBe(true);
        expect(fn('Button', {})).toBe(true);
        expect(fn('TextEditor', {})).toBe(true);
    });

    it.each([
        'span', 'p', 'label', 'b', 'i', 'em', 'strong', 'code', 'pre',
        'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
        'li', 'td', 'th', 'button', 'text', 'tspan', 'title', 'desc',
    ])('HTML intrinsic <%s> is text-bearing', (tag) => {
        expect(fn(tag, {})).toBe(true);
    });

    it('non-text containers (div, View, section) still return false', () => {
        expect(fn('div', {})).toBe(false);
        expect(fn('View', {})).toBe(false);
        expect(fn('section', {})).toBe(false);
        expect(fn('Panel', {})).toBe(false);
    });

    it('uppercase variants do NOT match (HTML tags are lowercase by JSX convention)', () => {
        // React lowercases intrinsic tag names; uppercase indicates a
        // component reference, not an HTML element. Don't accidentally
        // mark a user component named "Span" as text-bearing.
        expect(fn('SPAN', {})).toBe(false);
        expect(fn('Span', {})).toBe(false);
    });

    // pulp #1836 P1 (Codex follow-up) — TEXT_BEARING marks a type as
    // CAPABLE of bearing text, but children must actually be string/number
    // for React to skip the child-node path. Nested markup must NOT
    // short-circuit or the inner element gets dropped.
    describe('children-aware short-circuit (P1 Codex follow-up)', () => {
        it('plain string child returns true', () => {
            expect(fn('span', { children: 'hello' })).toBe(true);
            expect(fn('p', { children: 'paragraph' })).toBe(true);
            expect(fn('h1', { children: 'heading' })).toBe(true);
        });

        it('numeric child returns true (React renders numbers as text)', () => {
            expect(fn('span', { children: 42 })).toBe(true);
            expect(fn('span', { children: 0 })).toBe(true);
        });

        it('null / undefined children returns true (empty container is text-able)', () => {
            expect(fn('span', { children: null })).toBe(true);
            expect(fn('span', { children: undefined })).toBe(true);
            expect(fn('span', {})).toBe(true);
        });

        it('array of only string/number scalars returns true', () => {
            expect(fn('span', { children: ['hello', ' ', 'world'] })).toBe(true);
            expect(fn('span', { children: ['count: ', 42] })).toBe(true);
            expect(fn('span', { children: [null, 'x', null] })).toBe(true);
        });

        it('nested element child returns false — React must descend into the child', () => {
            // The regression: <span><em>x</em></span>. children is an
            // object (React element). If we returned true, React would
            // skip mounting <em> and the inner text gets dropped.
            const reactElement = { type: 'em', props: { children: 'x' }, key: null, ref: null };
            expect(fn('span', { children: reactElement })).toBe(false);
            expect(fn('p', { children: reactElement })).toBe(false);
            expect(fn('button', { children: reactElement })).toBe(false);
        });

        it('mixed array with string + element returns false', () => {
            const el = { type: 'em', props: {}, key: null, ref: null };
            expect(fn('p', { children: ['prefix ', el, ' suffix'] })).toBe(false);
        });

        it('boolean children (React skips them at render time) returns false', () => {
            // Strictly speaking React treats true/false children as no-op
            // mount, but they aren't text scalars either. Returning false
            // makes React take the (cheap) child-mount path that no-ops.
            expect(fn('span', { children: true })).toBe(false);
            expect(fn('span', { children: false })).toBe(false);
        });
    });
});
