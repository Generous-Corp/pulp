// pulp #1486 — diff() unit tests.

import { describe, it, expect } from 'vitest';
import { diff } from '../src/diff.js';
import type { IRNode } from '../src/types.js';

function n(anchor: string, paint?: Record<string, unknown>, children: IRNode[] = []): IRNode {
    return {
        tag: 'View',
        stable_anchor_id: anchor,
        ...(paint ? { paint: paint as IRNode['paint'] } : {}),
        children,
        provenance: { adapter: 'test', version: '0.0.0', ts: '2026-05-05T17:00:00Z' },
        raw_source: { kind: 'unknown', payload: null },
        confidence: 'PASS',
    };
}

describe('diff', () => {
    it('reports added nodes when next has anchors prev lacks', () => {
        const prev = n('root', undefined, [n('a')]);
        const next = n('root', undefined, [n('a'), n('b')]);
        const drifts = diff(prev, next);
        const added = drifts.filter((d) => d.kind === 'added');
        expect(added).toHaveLength(1);
        expect(added[0].anchor).toBe('b');
    });

    it('reports removed nodes when prev has anchors next lacks', () => {
        const prev = n('root', undefined, [n('a'), n('b')]);
        const next = n('root', undefined, [n('a')]);
        const drifts = diff(prev, next);
        const removed = drifts.filter((d) => d.kind === 'removed');
        expect(removed).toHaveLength(1);
        expect(removed[0].anchor).toBe('b');
    });

    it('reports field changes via dotted-path entries', () => {
        const prev = n('root', undefined, [n('a', { backgroundColor: '#000' })]);
        const next = n('root', undefined, [n('a', { backgroundColor: '#fff' })]);
        const drifts = diff(prev, next);
        const changed = drifts.filter((d) => d.kind === 'changed');
        expect(changed).toHaveLength(1);
        expect(changed[0].field).toBe('paint.backgroundColor');
        if (changed[0].kind === 'changed') {
            expect(changed[0].oldValue).toBe('#000');
            expect(changed[0].newValue).toBe('#fff');
        }
    });

    it('reports reorders when same anchor moves child-index', () => {
        const prev = n('root', undefined, [n('a'), n('b')]);
        const next = n('root', undefined, [n('b'), n('a')]);
        const drifts = diff(prev, next);
        const reorders = drifts.filter((d) => d.kind === 'reordered');
        expect(reorders).toHaveLength(2);
    });

    it('result is sorted by anchor for deterministic fixtures', () => {
        const prev = n('root', undefined, [n('z'), n('a')]);
        const next = n('root', undefined, [n('a'), n('m'), n('z')]);
        const drifts = diff(prev, next);
        const anchors = drifts.map((d) => d.anchor);
        const sorted = [...anchors].sort();
        expect(anchors).toEqual(sorted);
    });
});
