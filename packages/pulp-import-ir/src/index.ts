// pulp #1486 — public surface for @pulp/import-ir.
//
// Phase 1 spike: types + content-hash anchors + Claude Design HTML
// adapter + tweaks layer + JSX-like tree map. CLI integration
// (`pulp import design path/x.html`) lives downstream; this package
// stays renderer-agnostic so adapters (Figma MCP, Pencil, Mitosis, …)
// can land independently against the same IR contract.

export * from './types.js';
export {
    hashCodeAsString,
    generateAnchorId,
    assignAnchors,
    DEFAULT_ANCHOR_STRATEGY,
} from './anchors.js';
export {
    applyTweaks,
    setByDottedPath,
    nodeFsTweaksIO,
    emptyTweaksFile,
    orphanedTweakDrifts,
    type TweaksIO,
} from './tweaks.js';
export { diff } from './diff.js';
export {
    toJSXLikeTree,
    flattenJSXLike,
    type JSXLikeNode,
} from './lower-via-prop-applier.js';
export {
    lowerClaudeDesignHtml,
    ADAPTER_NAME as CLAUDE_DESIGN_HTML_ADAPTER_NAME,
    ADAPTER_VERSION as CLAUDE_DESIGN_HTML_ADAPTER_VERSION,
} from './adapters/claude-design-html/lower.js';
