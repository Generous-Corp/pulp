// pulp #1486 — Tweaks layer (§7, §8 of the spec).
//
// Reads `pulp-tweaks.json` from disk, applies dev overrides on top of
// a freshly-lowered IRNode tree. Pure functions — no side effects.
//
// Tweaks are keyed by stable_anchor_id; values are dotted-path overrides
// into the IRNode's typed fields:
//   { "anchor-X": { "paint.backgroundColor": "#ff00aa", "text.fontSize": 18 } }

import type { IRNode, TweaksFile, TweakValue, Drift } from './types.js';

/**
 * Apply tweaks on top of an IR tree. Pure — does not mutate `node` or
 * `tweaks`. Tweaks whose anchor IS in the tree are deep-merged onto
 * the typed fields; tweaks whose anchor is NOT found are attached to
 * the root's `meta.orphaned_tweaks` and surfaced via diff() as
 * Drift.orphaned-tweak.
 */
export function applyTweaks(node: IRNode, tweaks: TweaksFile): IRNode {
    const seen = new Set<string>();
    const result = applyToNode(node, tweaks, seen);

    // Compute orphaned tweaks (anchors in the file that didn't appear
    // in the tree). Attach them to the result root's meta so diff()
    // can surface them in subsequent calls.
    const orphaned: Record<string, TweakValue> = {};
    for (const [anchor, tweak] of Object.entries(tweaks.tweaks)) {
        if (!seen.has(anchor)) {
            orphaned[anchor] = tweak;
        }
    }
    if (Object.keys(orphaned).length > 0) {
        return {
            ...result,
            meta: {
                ...(result.meta ?? {}),
                orphaned_tweaks: orphaned,
            },
        };
    }
    return result;
}

function applyToNode(
    node: IRNode,
    tweaks: TweaksFile,
    seen: Set<string>,
): IRNode {
    const tweak = tweaks.tweaks[node.stable_anchor_id];
    let nextNode: IRNode = {
        ...node,
        children: node.children.map((c) => applyToNode(c, tweaks, seen)),
    };
    if (tweak) {
        seen.add(node.stable_anchor_id);
        nextNode = applyTweakValue(nextNode, tweak);
    }
    return nextNode;
}

function applyTweakValue(node: IRNode, tweak: TweakValue): IRNode {
    let next = node;
    for (const [path, value] of Object.entries(tweak)) {
        next = setByDottedPath(next, path, value);
    }
    return next;
}

/**
 * Set a dotted-path field on an IRNode without mutating the original.
 * Phase 1 supports flat paths into typed top-level fields:
 *   'paint.backgroundColor'
 *   'text.fontSize'
 *   'layout.padding'
 *   'meta.role'
 * `null` value clears the field (deletion semantics per §8.3).
 */
export function setByDottedPath(node: IRNode, path: string, value: unknown): IRNode {
    const segments = path.split('.');
    if (segments.length < 2) return node;

    const [section, ...rest] = segments;
    const key = rest.join('.');

    // Walk into the typed section, clone it, set the leaf, return a
    // new node with the cloned section spliced in.
    const sectionVal = (node as unknown as Record<string, unknown>)[section];
    const sectionObj = (sectionVal && typeof sectionVal === 'object'
        ? { ...(sectionVal as Record<string, unknown>) }
        : {}) as Record<string, unknown>;

    if (value === null) {
        delete sectionObj[key];
    } else {
        sectionObj[key] = value;
    }
    return {
        ...node,
        [section]: sectionObj,
    } as IRNode;
}

// ── File I/O — narrow API surface for the spike ──────────────────────
//
// Phase 1 keeps fs access optional so the IR core stays browser-loadable.
// CLI integration (pulp import design ...) will inject the fs hook.

export interface TweaksIO {
    readTweaks(filePath: string): Promise<TweaksFile | null>;
    writeTweaks(filePath: string, tweaks: TweaksFile): Promise<void>;
}

/**
 * Default Node-fs implementation. Lazily imports `node:fs/promises` so
 * browser bundlers don't try to resolve it.
 */
export function nodeFsTweaksIO(): TweaksIO {
    return {
        async readTweaks(filePath: string): Promise<TweaksFile | null> {
            const fs = await import('node:fs/promises');
            try {
                const raw = await fs.readFile(filePath, 'utf8');
                return JSON.parse(raw) as TweaksFile;
            } catch (err: unknown) {
                if ((err as NodeJS.ErrnoException)?.code === 'ENOENT') return null;
                throw err;
            }
        },
        async writeTweaks(filePath: string, tweaks: TweaksFile): Promise<void> {
            const fs = await import('node:fs/promises');
            await fs.writeFile(filePath, JSON.stringify(tweaks, null, 2) + '\n', 'utf8');
        },
    };
}

// ── Empty tweaks file helper (used at first import) ──────────────────

export function emptyTweaksFile(pulpVersion: string, importSession: string): TweaksFile {
    return {
        $schema: 'pulp-tweaks://v1',
        meta: { pulpVersion, importSession },
        tweaks: {},
    };
}

// ── Diff — produces orphaned-tweak Drift entries ─────────────────────
//
// Surfaced when applyTweaks() saw tweaks whose anchors weren't found
// in the freshly-lowered tree. The CLI uses this to print a
// "tweaks lost" report.
export function orphanedTweakDrifts(node: IRNode): Drift[] {
    const orphaned = node.meta?.orphaned_tweaks;
    if (!orphaned) return [];
    return Object.entries(orphaned).map(([anchor, tweak]) => ({
        kind: 'orphaned-tweak' as const,
        anchor,
        tweak: tweak as TweakValue,
        reason: 'node-deleted' as const,
    }));
}
