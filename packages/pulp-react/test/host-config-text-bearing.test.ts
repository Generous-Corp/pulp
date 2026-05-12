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
});
