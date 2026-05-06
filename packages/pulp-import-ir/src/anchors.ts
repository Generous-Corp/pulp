// pulp #1486 — stable_anchor_id strategies (§4 of the spec).
//
// Three strategies:
//   • content-hash — used by Claude Design HTML / Stitch / generic HTML.
//                     Hash over (tag, role, normalized text, depth).
//   • path        — used by RN file exports / hand-edited code.
//                    `Frame[2]/Section[0]/Button#0` w/ tiebreak.
//   • adapter     — used by Figma / Pencil / Mitosis (sources with native IDs).
//
// Phase 1 spike: content-hash + path (adapter strategy is a thin
// passthrough that the adapter populates during lower()).

import type { IRNode, AnchorStrategy } from './types.js';

// ── Mitosis-shape hash (lifted from MIT-licensed
// mitosis/packages/core/src/symbols/symbol-processor.ts:187,
// also referenced by Builder content-hash IDs). 32-bit FNV-style fold;
// deterministic over sorted-key object input.
//
// We re-implement here rather than vendor — the algorithm is ~30 lines
// and the upstream surface is liable to change in ways unrelated to
// Pulp's needs (per CLAUDE.md "pattern lift, no vendoring").
export function hashCodeAsString(input: unknown): string {
    const str = stableStringify(input);
    // FNV-1a 32-bit. Fast, stable, low collision rate at the scale we
    // care about (a single design tree's worth of nodes — typically
    // <10k). Output is base-36 for compact anchors.
    let hash = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) {
        hash ^= str.charCodeAt(i);
        // Math.imul for 32-bit multiplication w/o JS-number overflow.
        hash = Math.imul(hash, 0x01000193);
    }
    // Force unsigned 32-bit.
    return (hash >>> 0).toString(36);
}

// JSON.stringify is non-deterministic for objects (key order is
// implementation-defined). Sort keys recursively for a stable hash.
function stableStringify(v: unknown): string {
    if (v === null || typeof v !== 'object') return JSON.stringify(v);
    if (Array.isArray(v)) {
        return '[' + v.map(stableStringify).join(',') + ']';
    }
    const obj = v as Record<string, unknown>;
    const keys = Object.keys(obj).sort();
    return (
        '{' +
        keys.map((k) => JSON.stringify(k) + ':' + stableStringify(obj[k])).join(',') +
        '}'
    );
}

// Pre-anchor input shape — what the adapter passes BEFORE the anchor is
// computed. (anchor depends on the typed fields, but not on the anchor
// itself, so we accept this lighter shape.)
export interface PreAnchorIRNode {
    tag: string;
    text?: { text?: string };
    meta?: { role?: string; anchor_id_override?: string };
    children: PreAnchorIRNode[];
    /** Adapter-side native ID (only used for the 'adapter' strategy). */
    source_node_id?: string;
    /** Adapter name — used for the 'adapter' strategy prefix. */
    _adapter?: string;
}

/**
 * Generate a stable_anchor_id for `node` according to `strategy`.
 *
 * - `content-hash`: hash of (tag, role, normalized text, depth,
 *   indexAmongSameSignatureSiblings). The duplicate-sibling discriminator is
 *   the Nth-occurrence of this exact `{tag, role, text}` triple within the
 *   parent's child list — that index is stable across re-imports of the
 *   SAME source HTML (the Nth duplicate is still the Nth duplicate even if
 *   non-matching siblings are reordered) and disambiguates repeated labels
 *   / buttons that previously collided on a single anchor. Reordering of
 *   *same-signature* siblings still rotates the Nth-of-each, which is the
 *   inherent limit of content-hash without parent context — designers who
 *   need stricter stability across reorder of duplicates should opt into
 *   the `path` strategy instead.
 *   Survives reorder of unrelated siblings; brittle to text edits.
 * - `path`: `Tag[idx]/Tag[idx]/...` from root.
 *   Survives text edits; brittle to reorder + sibling insert.
 * - `adapter`: `<adapter-name>:<source_node_id>`.
 *   Best-of-all for sources with native IDs.
 */
export function generateAnchorId(
    node: PreAnchorIRNode,
    parent: PreAnchorIRNode | null,
    parentChildIndex: number,
    depth: number,
    strategy: AnchorStrategy,
    parentAnchorId: string = '',
    siblingTagCounter: Map<string, number> = new Map(),
    indexAmongSameSignatureSiblings: number = 0,
): string {
    if (node.meta?.anchor_id_override) {
        return node.meta.anchor_id_override;
    }
    if (strategy === 'adapter') {
        if (!node._adapter || !node.source_node_id) {
            throw new Error(
                `anchor strategy='adapter' requires node._adapter + source_node_id; ` +
                    `got adapter=${node._adapter}, source_node_id=${node.source_node_id}`,
            );
        }
        return `${node._adapter}:${node.source_node_id}`;
    }
    if (strategy === 'path') {
        const tag = node.tag;
        const idxAmongSameTag = siblingTagCounter.get(tag) ?? 0;
        siblingTagCounter.set(tag, idxAmongSameTag + 1);
        const seg = `${tag}[${idxAmongSameTag}]`;
        return parentAnchorId ? `${parentAnchorId}/${seg}` : seg;
    }
    // content-hash
    const tag = node.tag;
    const role = node.meta?.role ?? '';
    const text = (node.text?.text ?? '').replace(/\s+/g, ' ').trim().toLowerCase();
    return hashCodeAsString({
        tag,
        role,
        text,
        depth,
        sigIndex: indexAmongSameSignatureSiblings,
    });
}

/**
 * Compute the normalized content-hash signature key for a node — exactly
 * the {tag, role, text} triple the hash uses, so siblings with the same
 * triple can be counted before walking. Kept private to the module since
 * it must mirror the field set used inside `generateAnchorId`.
 */
function contentHashSignatureKey(node: PreAnchorIRNode): string {
    const tag = node.tag;
    const role = node.meta?.role ?? '';
    const text = (node.text?.text ?? '').replace(/\s+/g, ' ').trim().toLowerCase();
    // JSON-encode each field so collisions like {tag:'ab', role:''} vs.
    // {tag:'a', role:'b'} cannot collapse to the same key.
    return JSON.stringify([tag, role, text]);
}

/**
 * Walk a pre-anchor tree and return a parallel tree where every node
 * has a `stable_anchor_id`. Used by adapter `lower()` after the typed
 * fields are populated.
 *
 * Purely traversal-shaped — no IR mutation. The adapter calls this in
 * the final stage of `lower()`.
 */
export function assignAnchors<T extends PreAnchorIRNode>(
    root: T,
    strategy: AnchorStrategy,
): Map<T, string> {
    const out = new Map<T, string>();
    walk(root, null, 0, 0, '', out, strategy);
    return out;
}

function walk<T extends PreAnchorIRNode>(
    node: T,
    parent: T | null,
    parentChildIndex: number,
    depth: number,
    parentAnchor: string,
    out: Map<T, string>,
    strategy: AnchorStrategy,
    sigIndex: number = 0,
): void {
    const id = generateAnchorId(
        node,
        parent,
        parentChildIndex,
        depth,
        strategy,
        parentAnchor,
        // path strategy needs a sibling counter across the parent's
        // children — we re-build it per child-index so the count
        // matches "previous siblings of the same tag".
        countSiblingTags(parent, parentChildIndex),
        sigIndex,
    );
    out.set(node, id);

    // For the 'path' strategy, child-anchors are scoped to this
    // node's anchor. For 'content-hash' / 'adapter', children
    // generate independently.
    const childParentAnchor = strategy === 'path' ? id : '';

    // For content-hash, count how many earlier siblings share the same
    // {tag, role, text} signature so each duplicate gets a distinct
    // discriminator. The Nth duplicate stays the Nth duplicate across
    // re-imports of the same source HTML, which is exactly the
    // stability property we need.
    const sigCounts = new Map<string, number>();
    for (let i = 0; i < node.children.length; i++) {
        const c = node.children[i] as T;
        const childSigIndex =
            strategy === 'content-hash'
                ? (() => {
                      const key = contentHashSignatureKey(c);
                      const n = sigCounts.get(key) ?? 0;
                      sigCounts.set(key, n + 1);
                      return n;
                  })()
                : 0;
        walk(c, node, i, depth + 1, childParentAnchor, out, strategy, childSigIndex);
    }
}

function countSiblingTags(
    parent: PreAnchorIRNode | null,
    upToIndex: number,
): Map<string, number> {
    const counter = new Map<string, number>();
    if (!parent) return counter;
    for (let i = 0; i < upToIndex; i++) {
        const c = parent.children[i];
        if (!c) continue;
        counter.set(c.tag, (counter.get(c.tag) ?? 0) + 1);
    }
    return counter;
}

// Default per-source strategy, per §4.4 of the spec.
export const DEFAULT_ANCHOR_STRATEGY: Record<string, AnchorStrategy> = {
    'figma-mcp': 'adapter',
    'figma-zip': 'adapter',
    'pencil-mcp': 'adapter',
    mitosis: 'adapter',
    'rn-file': 'path',
    'v0-tsx': 'content-hash',
    'stitch-html': 'content-hash',
    'claude-design-html': 'content-hash',
    'generic-html': 'content-hash',
};
