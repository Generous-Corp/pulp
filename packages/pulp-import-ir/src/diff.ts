// pulp #1486 — Drift diff (§7 of the spec).
//
// Compares two IR trees produced by the SAME adapter and emits a flat
// list of Drift entries keyed by stable_anchor_id. Phase 1 supports:
//   - added / removed / changed / reordered
//   - orphaned-tweak (carried over from applyTweaks's meta.orphaned_tweaks)
//
// Field-level diff walks the typed top-level fields (layout/paint/text)
// and emits 'changed' per differing dotted path. Same path-encoding as
// the tweaks file, so a Drift.changed can be replayed against
// applyTweaks() without reformatting.

import type { IRNode, Drift } from './types.js';

/**
 * Two-tree diff. Pure-sync. Result is sorted by anchor for
 * deterministic test fixtures.
 */
export function diff(prev: IRNode, next: IRNode): Drift[] {
    const prevByAnchor = indexByAnchor(prev);
    const nextByAnchor = indexByAnchor(next);
    const result: Drift[] = [];

    // added / changed / reordered
    for (const [anchor, nextEntry] of nextByAnchor) {
        const prevEntry = prevByAnchor.get(anchor);
        if (!prevEntry) {
            result.push({ kind: 'added', anchor, node: nextEntry.node });
            continue;
        }
        // Field-level diff
        const fieldDrifts = diffTypedFields(prevEntry.node, nextEntry.node);
        for (const fd of fieldDrifts) result.push(fd);

        // Reorder
        if (
            prevEntry.parentAnchor !== nextEntry.parentAnchor ||
            prevEntry.childIndex !== nextEntry.childIndex
        ) {
            result.push({
                kind: 'reordered',
                anchor,
                oldIndex: prevEntry.childIndex,
                newIndex: nextEntry.childIndex,
                parent: nextEntry.parentAnchor ?? '',
            });
        }
    }

    // removed
    for (const [anchor, prevEntry] of prevByAnchor) {
        if (!nextByAnchor.has(anchor)) {
            result.push({ kind: 'removed', anchor, node: prevEntry.node });
        }
    }

    // orphaned-tweak: passed through if next.meta.orphaned_tweaks is set.
    const orphaned = next.meta?.orphaned_tweaks;
    if (orphaned) {
        for (const [anchor, tweak] of Object.entries(orphaned)) {
            result.push({
                kind: 'orphaned-tweak',
                anchor,
                tweak: tweak as { [path: string]: unknown },
                reason: 'node-deleted',
            });
        }
    }

    // Deterministic order
    result.sort((a, b) => {
        if (a.anchor !== b.anchor) return a.anchor < b.anchor ? -1 : 1;
        return a.kind < b.kind ? -1 : a.kind > b.kind ? 1 : 0;
    });
    return result;
}

interface IndexEntry {
    node: IRNode;
    parentAnchor: string | null;
    childIndex: number;
}

function indexByAnchor(root: IRNode): Map<string, IndexEntry> {
    const out = new Map<string, IndexEntry>();
    walk(root, null, 0, out);
    return out;
}

function walk(
    node: IRNode,
    parentAnchor: string | null,
    childIndex: number,
    out: Map<string, IndexEntry>,
): void {
    out.set(node.stable_anchor_id, { node, parentAnchor, childIndex });
    for (let i = 0; i < node.children.length; i++) {
        walk(node.children[i], node.stable_anchor_id, i, out);
    }
}

function diffTypedFields(prev: IRNode, next: IRNode): Drift[] {
    const out: Drift[] = [];
    const sections = ['layout', 'paint', 'text'] as const;
    for (const sec of sections) {
        const a = (prev as unknown as Record<string, unknown>)[sec] as
            | Record<string, unknown>
            | undefined;
        const b = (next as unknown as Record<string, unknown>)[sec] as
            | Record<string, unknown>
            | undefined;
        const keys = new Set<string>([
            ...(a ? Object.keys(a) : []),
            ...(b ? Object.keys(b) : []),
        ]);
        for (const k of keys) {
            const ov = a?.[k];
            const nv = b?.[k];
            if (!shallowEqual(ov, nv)) {
                out.push({
                    kind: 'changed',
                    anchor: next.stable_anchor_id,
                    field: `${sec}.${k}`,
                    oldValue: ov,
                    newValue: nv,
                });
            }
        }
    }
    return out;
}

function shallowEqual(a: unknown, b: unknown): boolean {
    if (a === b) return true;
    if (a == null || b == null) return false;
    if (typeof a !== typeof b) return false;
    // For arrays / objects we go deep via JSON — values in the IR are
    // either primitives or POJOs; this is a fine spike-quality check.
    return JSON.stringify(a) === JSON.stringify(b);
}
