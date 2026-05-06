// pulp #1486 — tweaks layer unit tests.

import { describe, it, expect } from 'vitest';
import {
    applyTweaks,
    setByDottedPath,
    emptyTweaksFile,
    orphanedTweakDrifts,
} from '../src/tweaks.js';
import type { IRNode } from '../src/types.js';

function makeNode(anchor: string, partial: Partial<IRNode> = {}): IRNode {
    return {
        tag: 'View',
        stable_anchor_id: anchor,
        children: [],
        provenance: { adapter: 'test', version: '0.0.0', ts: '2026-05-05T17:00:00Z' },
        raw_source: { kind: 'unknown', payload: null },
        confidence: 'PASS',
        ...partial,
    };
}

describe('setByDottedPath', () => {
    it('sets a typed-section field', () => {
        const node = makeNode('a');
        const next = setByDottedPath(node, 'paint.backgroundColor', '#ff0000');
        expect(next.paint?.backgroundColor).toBe('#ff0000');
        expect(node.paint).toBeUndefined();
    });

    it('clears a field when value is null', () => {
        const node = makeNode('a', { paint: { backgroundColor: '#000' } });
        const next = setByDottedPath(node, 'paint.backgroundColor', null);
        expect(next.paint).toEqual({});
        expect(node.paint?.backgroundColor).toBe('#000');
    });

    it('preserves other fields in the section', () => {
        const node = makeNode('a', { paint: { backgroundColor: '#000', opacity: 0.5 } });
        const next = setByDottedPath(node, 'paint.backgroundColor', '#ff0000');
        expect(next.paint?.backgroundColor).toBe('#ff0000');
        expect(next.paint?.opacity).toBe(0.5);
    });
});

describe('applyTweaks', () => {
    it('applies a tweak to the matching anchor', () => {
        const root: IRNode = makeNode('root', {
            children: [makeNode('a'), makeNode('b')],
        });
        const tweaks = emptyTweaksFile('0.78.1', 's-1');
        tweaks.tweaks['a'] = { 'paint.backgroundColor': '#ff00aa' };

        const next = applyTweaks(root, tweaks);
        const childA = next.children[0];
        expect(childA.paint?.backgroundColor).toBe('#ff00aa');
        expect(next.meta?.orphaned_tweaks).toBeUndefined();
    });

    it('attaches orphaned tweaks to the root meta when anchor missing', () => {
        const root = makeNode('root', { children: [makeNode('a')] });
        const tweaks = emptyTweaksFile('0.78.1', 's-1');
        tweaks.tweaks['ghost'] = { 'paint.backgroundColor': '#ff00aa' };

        const next = applyTweaks(root, tweaks);
        expect(next.meta?.orphaned_tweaks?.['ghost']).toEqual({
            'paint.backgroundColor': '#ff00aa',
        });
    });

    it('orphanedTweakDrifts reports each orphan', () => {
        const node = makeNode('root', {
            meta: {
                orphaned_tweaks: {
                    ghost1: { 'paint.opacity': 0.5 },
                    ghost2: { 'text.fontSize': 14 },
                },
            },
        });
        const drifts = orphanedTweakDrifts(node);
        expect(drifts).toHaveLength(2);
        expect(drifts.every((d) => d.kind === 'orphaned-tweak')).toBe(true);
    });

    it('does not mutate the input tree', () => {
        const root = makeNode('root', { children: [makeNode('a')] });
        const tweaks = emptyTweaksFile('0.78.1', 's-1');
        tweaks.tweaks['a'] = { 'paint.opacity': 0.5 };

        const next = applyTweaks(root, tweaks);
        expect(root.children[0].paint).toBeUndefined();
        expect(next.children[0].paint?.opacity).toBe(0.5);
    });
});
