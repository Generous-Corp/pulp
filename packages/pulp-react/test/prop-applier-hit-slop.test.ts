import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps } from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;
beforeEach(() => { bridge = createMockBridge(); bridge.install(); });
afterEach(() => { bridge.uninstall(); });

function makeInstance(id: string = 'k', type: string = 'View'): PulpInstance {
    return { id, type: type as PulpInstance['type'], props: {},
             childIds: [], onBridge: true, pendingChildren: [] };
}

// Edges as the bridge received them: [top, right, bottom, left].
function slopEdges(b: MockBridge): number[] {
    const calls = b.calls.filter((c) => c.fn === 'setHitSlop');
    expect(calls).toHaveLength(1);
    expect(calls[0].args[0]).toBe('k');
    return calls[0].args.slice(1) as number[];
}

describe('prop-applier hitSlop', () => {
    it('applies a number uniformly to all four edges', () => {
        applyChangedProps(makeInstance(), {}, { hitSlop: 12 });
        expect(slopEdges(bridge)).toEqual([12, 12, 12, 12]);
    });

    it('accepts a single-value string with a unit', () => {
        applyChangedProps(makeInstance(), {}, { hitSlop: '10px' });
        expect(slopEdges(bridge)).toEqual([10, 10, 10, 10]);
    });

    // The half-fix this guards against: a string used to fall through the
    // number/object branches and silently apply a slop of zero, so a control
    // authored `hitSlop: '12px 2px'` kept its original small hit target with
    // no error anywhere.
    it('fills a two-value string shorthand the way `margin` does', () => {
        applyChangedProps(makeInstance(), {}, { hitSlop: '12px 2px' });
        expect(slopEdges(bridge)).toEqual([12, 2, 12, 2]);
    });

    it('fills three- and four-value string shorthands', () => {
        applyChangedProps(makeInstance(), {}, { hitSlop: '1 2 3' });
        expect(slopEdges(bridge)).toEqual([1, 2, 3, 2]);
        bridge.calls.length = 0;
        applyChangedProps(makeInstance(), {}, { hitSlop: '1 2 3 4' });
        expect(slopEdges(bridge)).toEqual([1, 2, 3, 4]);
    });

    it('applies each object edge independently', () => {
        applyChangedProps(makeInstance(), {}, { hitSlop: { top: 1, right: 2, bottom: 3, left: 4 } });
        expect(slopEdges(bridge)).toEqual([1, 2, 3, 4]);
    });

    // React Native treats a missing object edge as zero -- it does NOT inherit
    // the CSS-shorthand way. Inheriting would silently grow edges the author
    // never asked for, and would disagree with `el.style.hitSlop`.
    it('treats a missing object edge as zero, not as an inherited value', () => {
        applyChangedProps(makeInstance(), {}, { hitSlop: { top: 8 } });
        expect(slopEdges(bridge)).toEqual([8, 0, 0, 0]);
    });

    it('coerces unparseable edges to zero rather than NaN', () => {
        applyChangedProps(makeInstance(), {}, { hitSlop: { top: 'auto' as unknown as number, left: 5 } });
        expect(slopEdges(bridge)).toEqual([0, 0, 0, 5]);
    });
});
