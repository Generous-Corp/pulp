// pulp #1486 — anchor strategy unit tests.

import { describe, it, expect } from 'vitest';
import { hashCodeAsString, generateAnchorId, assignAnchors } from '../src/anchors.js';

describe('hashCodeAsString', () => {
    it('is deterministic across runs', () => {
        const a = hashCodeAsString({ tag: 'Button', role: 'cta', text: 'subscribe', depth: 2 });
        const b = hashCodeAsString({ tag: 'Button', role: 'cta', text: 'subscribe', depth: 2 });
        expect(a).toBe(b);
    });

    it('is order-stable (sorted keys)', () => {
        const a = hashCodeAsString({ a: 1, b: 2, c: 3 });
        const b = hashCodeAsString({ c: 3, a: 1, b: 2 });
        expect(a).toBe(b);
    });

    it('produces different hashes for different content', () => {
        const a = hashCodeAsString({ tag: 'Button', text: 'subscribe' });
        const b = hashCodeAsString({ tag: 'Button', text: 'cancel' });
        expect(a).not.toBe(b);
    });
});

describe('generateAnchorId', () => {
    it('content-hash strategy depends on tag + role + text + depth', () => {
        const node = {
            tag: 'Button',
            text: { text: 'Click' },
            meta: { role: 'cta' },
            children: [],
        };
        const id = generateAnchorId(node, null, 0, 1, 'content-hash');
        expect(id).toMatch(/^[0-9a-z]+$/);
    });

    it('content-hash collapses whitespace in text', () => {
        const a = generateAnchorId(
            { tag: 'Label', text: { text: 'hello\n   world' }, children: [] },
            null,
            0,
            1,
            'content-hash',
        );
        const b = generateAnchorId(
            { tag: 'Label', text: { text: 'hello world' }, children: [] },
            null,
            0,
            1,
            'content-hash',
        );
        expect(a).toBe(b);
    });

    it('content-hash differs by depth', () => {
        const node = { tag: 'Button', text: { text: 'X' }, children: [] };
        const a = generateAnchorId(node, null, 0, 1, 'content-hash');
        const b = generateAnchorId(node, null, 0, 2, 'content-hash');
        expect(a).not.toBe(b);
    });

    it("path strategy emits 'Tag[idx]' segments", () => {
        const id = generateAnchorId(
            { tag: 'Button', children: [] },
            { tag: 'Frame', children: [] },
            0,
            1,
            'path',
            'Frame[0]',
        );
        expect(id).toBe('Frame[0]/Button[0]');
    });

    it('adapter strategy uses _adapter:source_node_id', () => {
        const id = generateAnchorId(
            { tag: 'Button', children: [], _adapter: 'figma-mcp', source_node_id: '42:15' },
            null,
            0,
            0,
            'adapter',
        );
        expect(id).toBe('figma-mcp:42:15');
    });

    it('adapter strategy throws when source_node_id missing', () => {
        expect(() =>
            generateAnchorId(
                { tag: 'Button', children: [] },
                null,
                0,
                0,
                'adapter',
            ),
        ).toThrow(/anchor strategy='adapter' requires/);
    });

    it('meta.anchor_id_override forces a specific anchor', () => {
        const id = generateAnchorId(
            { tag: 'Button', children: [], meta: { anchor_id_override: 'forced-1' } },
            null,
            0,
            0,
            'content-hash',
        );
        expect(id).toBe('forced-1');
    });
});

describe('assignAnchors', () => {
    it('produces an anchor for every node in the tree', () => {
        const root = {
            tag: 'View',
            children: [
                { tag: 'View', children: [{ tag: 'Button', text: { text: 'A' }, children: [] }] },
                { tag: 'View', children: [{ tag: 'Button', text: { text: 'B' }, children: [] }] },
            ],
        };
        const anchors = assignAnchors(root, 'content-hash');
        // Root + 2 mid-Views + 2 Buttons = 5 nodes.
        expect(anchors.size).toBe(5);
        // All anchors are non-empty strings.
        for (const id of anchors.values()) expect(id).toMatch(/^[0-9a-z]+$/);
    });

    it('path strategy: sibling tags get [0] / [1] / [2] ...', () => {
        const root = {
            tag: 'View',
            children: [
                { tag: 'Button', children: [] },
                { tag: 'Button', children: [] },
                { tag: 'Button', children: [] },
            ],
        };
        const anchors = assignAnchors(root, 'path');
        const childAnchors = root.children.map((c) => anchors.get(c));
        expect(childAnchors[0]).toBe('View[0]/Button[0]');
        expect(childAnchors[1]).toBe('View[0]/Button[1]');
        expect(childAnchors[2]).toBe('View[0]/Button[2]');
    });

    // pulp #1499 follow-up — Codex P1: duplicate-sibling content-hash
    // collision. Two siblings with identical {tag, role, text} (and
    // therefore identical depth) used to produce the same anchor,
    // which silently broke tweak routing and made `diff()` lossy.
    // The discriminator is the Nth occurrence of the same signature
    // among earlier siblings — stable across re-imports of the same
    // source HTML, distinct for each duplicate.
    it('content-hash disambiguates duplicate siblings (#1499 follow-up)', () => {
        const root = {
            tag: 'View',
            children: [
                {
                    tag: 'Button',
                    text: { text: 'Save' },
                    meta: { role: 'cta' },
                    children: [],
                },
                {
                    tag: 'Button',
                    text: { text: 'Save' },
                    meta: { role: 'cta' },
                    children: [],
                },
                {
                    tag: 'Button',
                    text: { text: 'Save' },
                    meta: { role: 'cta' },
                    children: [],
                },
            ],
        };
        const anchors = assignAnchors(root, 'content-hash');
        const a0 = anchors.get(root.children[0]);
        const a1 = anchors.get(root.children[1]);
        const a2 = anchors.get(root.children[2]);
        expect(a0).toBeDefined();
        expect(a1).toBeDefined();
        expect(a2).toBeDefined();
        expect(a0).not.toBe(a1);
        expect(a1).not.toBe(a2);
        expect(a0).not.toBe(a2);
    });

    it('content-hash duplicate-sibling discriminator is stable across re-imports', () => {
        const make = () => ({
            tag: 'View',
            children: [
                { tag: 'Label', text: { text: 'Hello' }, children: [] },
                { tag: 'Label', text: { text: 'Hello' }, children: [] },
            ],
        });
        const a = assignAnchors(make(), 'content-hash');
        const b = assignAnchors(make(), 'content-hash');
        const aRoot = [...a.entries()];
        const bRoot = [...b.entries()];
        // Same N-th-duplicate gets same anchor across re-imports.
        expect(aRoot[1][1]).toBe(bRoot[1][1]); // first Label
        expect(aRoot[2][1]).toBe(bRoot[2][1]); // second Label
        // The two duplicates remain distinct from each other.
        expect(aRoot[1][1]).not.toBe(aRoot[2][1]);
    });

    it('content-hash preserves single-child anchors when ancestor siblings reorder', () => {
        // Root has [header, section] both lowering to View. The single
        // unique-signature button under section should stay anchored
        // even though header and section are duplicate-signature
        // siblings — its hash inputs (depth + signature + sigIndex
        // among same-signature siblings) are identical in both
        // orderings.
        const make = (sectionFirst: boolean) => ({
            tag: 'View',
            children: sectionFirst
                ? [
                      {
                          tag: 'View',
                          children: [
                              { tag: 'Button', text: { text: 'Subscribe' }, children: [] },
                          ],
                      },
                      { tag: 'View', children: [] },
                  ]
                : [
                      { tag: 'View', children: [] },
                      {
                          tag: 'View',
                          children: [
                              { tag: 'Button', text: { text: 'Subscribe' }, children: [] },
                          ],
                      },
                  ],
        });
        const v1 = make(false);
        const v5 = make(true);
        const a1 = assignAnchors(v1, 'content-hash');
        const a5 = assignAnchors(v5, 'content-hash');
        // Find each Button by walking and matching text.
        const findButton = (
            node: { tag: string; text?: { text?: string }; children: unknown[] },
        ): unknown => {
            if (node.tag === 'Button') return node;
            for (const c of node.children) {
                const f = findButton(c as never);
                if (f) return f;
            }
            return null;
        };
        const b1 = findButton(v1)!;
        const b5 = findButton(v5)!;
        expect(a1.get(b1 as never)).toBe(a5.get(b5 as never));
    });
});
