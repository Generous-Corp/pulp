// Anchor strategy unit tests.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { hashCodeAsString, generateAnchorId, assignAnchors } from '../src/anchors.js';
import type { PreAnchorIRNode } from '../src/anchors.js';

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

    // Duplicate siblings with identical {tag, role, text} and depth need
    // distinct content-hash anchors so tweak routing and diff() remain lossless.
    // The discriminator is the Nth occurrence of the same signature
    // among earlier siblings — stable across re-imports of the same
    // source HTML, distinct for each duplicate.
    it('content-hash disambiguates duplicate siblings', () => {
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

// ── Cross-language conformance ──────────────────────────────────────────
//
// anchors.ts and core/view/src/anchor_strategy.cpp each claim in comments to
// mirror the other exactly. Nothing enforced that: this suite only ever
// checked itself against itself, and the C++ suite pinned no literal hash
// values at all, so the two could drift apart without any build going red.
//
// The shared table in test/fixtures/anchor_vectors.json records BOTH columns
// per vector. The C++ suite pins the cpp_anchor column; this suite pins the
// ts_anchor column. Vectors where the columns differ carry `known_divergence`
// — the drift is recorded as data rather than asserted away, because fixing it
// re-keys every pulp-tweaks.json containing non-ASCII text and is a migration
// decision, not a test change. See the fixture's `$comment`.

interface AnchorVector {
    name: string;
    strategy: 'content-hash' | 'path' | 'adapter';
    note: string;
    input: {
        tag: string;
        role: string;
        text: string;
        depth: number;
        sig_index: number;
        parent_anchor?: string;
        sibling_tag_index?: number;
        adapter?: string;
        source_node_id?: string;
    };
    cpp_anchor: string;
    ts_anchor: string;
    known_divergence: string | null;
    divergence_axes?: string[];
}

const vectorsPath = resolve(__dirname, '..', '..', '..', 'test', 'fixtures', 'anchor_vectors.json');
const vectorDoc = JSON.parse(readFileSync(vectorsPath, 'utf8')) as { vectors: AnchorVector[] };

// Rebuild the node + call arguments a vector describes, then run the real
// generateAnchorId() over them.
function anchorForVector(v: AnchorVector): string {
    const { input } = v;
    const node: PreAnchorIRNode = { tag: input.tag, text: { text: input.text }, children: [] };
    if (input.role) node.meta = { role: input.role };
    if (input.adapter) node._adapter = input.adapter;
    if (input.source_node_id) node.source_node_id = input.source_node_id;

    // The path strategy reads the count of earlier same-tag siblings out of
    // this counter, so seed it rather than materializing throwaway siblings.
    const siblingTagCounter = new Map<string, number>();
    if (v.strategy === 'path') siblingTagCounter.set(input.tag, input.sibling_tag_index ?? 0);

    return generateAnchorId(
        node,
        null,
        0,
        input.depth,
        v.strategy,
        input.parent_anchor ?? '',
        siblingTagCounter,
        input.sig_index,
    );
}

describe('cross-language anchor conformance', () => {
    it('has vectors to check', () => {
        expect(vectorDoc.vectors.length).toBeGreaterThan(0);
    });

    // Pins the TS column. Fails if anchors.ts changes its output — including a
    // well-meaning switch to UTF-8 folding, which would re-key existing tweaks
    // files and so must be deliberate rather than quiet.
    it.each(vectorDoc.vectors.map((v) => [v.name, v] as const))(
        'TS output matches the recorded ts_anchor for %s',
        (_name, v) => {
            expect(anchorForVector(v)).toBe(v.ts_anchor);
        },
    );

    // The headline contract. For every vector NOT marked as a known
    // divergence, the two implementations must agree — this is what "mirrors
    // the TS contract exactly" is supposed to mean, and it is now enforced
    // rather than asserted in a comment.
    it.each(vectorDoc.vectors.filter((v) => !v.known_divergence).map((v) => [v.name, v] as const))(
        'TS and C++ agree for %s',
        (_name, v) => {
            expect(anchorForVector(v)).toBe(v.cpp_anchor);
        },
    );

    // The divergent vectors, asserted as still-divergent. This deliberately
    // keeps the suite green while the drift is unfixed, but the moment someone
    // unifies the encoding these fail loudly and force the table (and the
    // pulp-tweaks.json migration) to be dealt with explicitly.
    it.each(vectorDoc.vectors.filter((v) => v.known_divergence).map((v) => [v.name, v] as const))(
        'C++ still disagrees with TS for %s (known_divergence)',
        (_name, v) => {
            const ts = anchorForVector(v);
            expect(ts).toBe(v.ts_anchor);
            expect(ts).not.toBe(v.cpp_anchor);
            expect(v.known_divergence).toBe('utf8-vs-utf16');
            // Only the hashing strategy can diverge; path/adapter are pure
            // string concatenation on both sides.
            expect(v.strategy).toBe('content-hash');
        },
    );

    it('records the divergence axes proven by the vector pairs', () => {
        const byName = new Map(vectorDoc.vectors.map((v) => [v.name, v]));
        const anchorOf = (n: string) => anchorForVector(byName.get(n)!);

        // Encoding axis, isolated: already-lowercase, no whitespace, so no
        // case mapping or whitespace class can be involved. TS folds UTF-16
        // code units, C++ folds UTF-8 bytes.
        expect(anchorOf('nonascii-umlaut-lowercase')).not.toBe(
            byName.get('nonascii-umlaut-lowercase')!.cpp_anchor,
        );

        // Case-folding axis: TS toLowerCase() is full-Unicode, so the capital
        // and lowercase umlaut collapse to ONE anchor here. The C++ column
        // records two different values because it only folds A-Z.
        expect(anchorOf('nonascii-umlaut-uppercase')).toBe(anchorOf('nonascii-umlaut-lowercase'));
        expect(byName.get('nonascii-umlaut-uppercase')!.cpp_anchor).not.toBe(
            byName.get('nonascii-umlaut-lowercase')!.cpp_anchor,
        );

        // Whitespace-class axis: the TS /\s+/ class includes NBSP, so the NBSP
        // vector lands on the plain-space anchor. The C++ column does not,
        // because its normalizer only knows space/tab/CR/LF.
        expect(anchorOf('nonascii-nbsp-whitespace')).toBe(anchorOf('ascii-plain-space-control'));
        expect(byName.get('nonascii-nbsp-whitespace')!.cpp_anchor).not.toBe(
            byName.get('ascii-plain-space-control')!.cpp_anchor,
        );
    });

    // Controls. The path/adapter strategies never hash, so they must agree
    // even on non-ASCII input — proving the harness reports a specific defect
    // in the hash path rather than "everything cross-language is broken".
    it('agrees on non-ASCII input for the non-hashing strategies', () => {
        const controls = vectorDoc.vectors.filter((v) => v.strategy !== 'content-hash');
        expect(controls.length).toBeGreaterThan(0);
        for (const v of controls) {
            expect(v.known_divergence).toBeNull();
            expect(anchorForVector(v)).toBe(v.cpp_anchor);
        }
    });
});
