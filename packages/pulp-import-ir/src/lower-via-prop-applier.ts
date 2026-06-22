// IR → JSX-equivalent intrinsics → @pulp/react prop-applier dispatch.
//
// This file isolates the IR → React-tree translation so renderers and tests
// can consume a stable JSX-like tree without depending on the raw IR shape.
//
// The translation is a pure tree map: each IRNode becomes a `{ tag,
// props, children }` shape that a renderer can feed into
// `React.createElement` (or any equivalent tree-builder). Returning a
// concrete JSX-equivalent shape rather than React elements lets the
// IR package stay decoupled from a specific React version.

import type { IRNode } from './types.js';

/**
 * JSX-equivalent shape — a concrete tree any tree-builder can consume.
 * Caller chooses how to render: React.createElement(tag, props,
 * ...children.map(toReactElement)) is the obvious bridge.
 */
export interface JSXLikeNode {
    tag: string;
    props: Record<string, unknown>;
    children: JSXLikeNode[];
}

/**
 * Convert an IRNode tree to a JSX-equivalent tree shape. Pure.
 *
 * The output's `props` map mirrors @pulp/react's StyleProps + FlexProps
 * shape — which the prop-applier already knows how to dispatch through
 * to the bridge. The flattening rule is:
 *   - layout / paint / text typed fields flatten into one top-level
 *     `style`-flavored object whose keys match the prop-applier's
 *     `case 'X':` dispatchers.
 *   - text leaf content becomes the `text` prop on Label / Button.
 *   - children recurse.
 *   - meta.role becomes a data-pulp-role marker on the props map for
 *     diagnostics; non-mappable IR fields ride through unchanged for
 *     renderer-specific handling.
 */
export function toJSXLikeTree(node: IRNode): JSXLikeNode {
    const props: Record<string, unknown> = {};

    if (node.layout) {
        for (const [k, v] of Object.entries(node.layout)) {
            // The prop-applier's case keys map 1:1 to TypedLayout names.
            props[k] = v;
        }
    }
    if (node.paint) {
        for (const [k, v] of Object.entries(node.paint)) {
            props[k] = v;
        }
    }
    if (node.text) {
        for (const [k, v] of Object.entries(node.text)) {
            // text leaf content goes via the `text` prop the bridge
            // already understands; everything else (fontSize, etc.)
            // flattens with its own name.
            props[k] = v;
        }
    }

    // Stable anchor ID rides through as React-key + data-* marker so
    // the renderer can correlate back to the IR + tweaks layer when
    // building dev-tools surfaces.
    props['key'] = node.stable_anchor_id;
    props['data-pulp-anchor'] = node.stable_anchor_id;
    if (node.meta?.role) {
        props['data-pulp-role'] = node.meta.role;
    }

    return {
        tag: node.tag,
        props,
        children: node.children.map(toJSXLikeTree),
    };
}

/**
 * Walk a JSXLikeTree and produce a flat list of `(tag, props)` pairs
 * for tests + dev-tools. The IR's tree-shape is preserved via parent
 * indexing on every entry.
 */
export function flattenJSXLike(
    root: JSXLikeNode,
): { tag: string; props: Record<string, unknown>; depth: number }[] {
    const out: { tag: string; props: Record<string, unknown>; depth: number }[] = [];
    walk(root, 0, out);
    return out;
}

function walk(
    n: JSXLikeNode,
    depth: number,
    out: { tag: string; props: Record<string, unknown>; depth: number }[],
): void {
    out.push({ tag: n.tag, props: n.props, depth });
    for (const c of n.children) walk(c, depth + 1, out);
}
