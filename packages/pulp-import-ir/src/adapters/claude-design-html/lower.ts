// pulp #1486 — Claude Design HTML adapter (Phase 1 spike target).
//
// Lowers a Claude-Design-style HTML payload (a single document or a
// fragment) into an IRNode tree. Uses content-hash anchors per §4.4.
//
// First-spike scope: structural translation + the most common style
// properties. Anything not yet typed is preserved verbatim under
// `raw_source.outerHtml` so a future Pulp version can re-walk the
// source without a re-import.

import type {
    IRNode,
    LowerOptions,
    SourceFormat,
    TypedLayout,
    TypedPaint,
    TypedText,
    Confidence,
} from '../../types.js';
import { assignAnchors, DEFAULT_ANCHOR_STRATEGY, type PreAnchorIRNode } from '../../anchors.js';

export const ADAPTER_NAME = 'claude-design-html';
export const ADAPTER_VERSION = '0.1.0';

// Pre-anchor IRNode used during the build phase. The final `lower()`
// call resolves anchors via assignAnchors() and produces the public
// IRNode shape.
interface BuildNode extends PreAnchorIRNode {
    layout?: TypedLayout;
    paint?: TypedPaint;
    text?: TypedText;
    children: BuildNode[];
    rawHtml: string;
    inlineStyle?: Record<string, string>;
    confidence: Confidence;
}

/**
 * Lower a Claude Design HTML string (or fragment) into an IRNode tree.
 *
 * The payload is parsed via a small DOM-like walker that the Phase 1
 * spike implements without depending on a full HTML parser — Claude
 * Design output is well-formed enough that a regex-driven walker
 * suffices for the first spike. A real implementation would lean on
 * `parse5` or `htmlparser2`; that lands when we move to a real
 * parser-driven adapter (Phase 2).
 */
export async function lowerClaudeDesignHtml(
    rawHtml: string,
    opts: LowerOptions = {},
): Promise<IRNode> {
    const strategy = opts.anchorStrategy ?? DEFAULT_ANCHOR_STRATEGY['claude-design-html'];
    const ts = new Date().toISOString();

    // 1. Parse HTML → BuildNode tree.
    const root = parseHtmlToBuildNode(rawHtml.trim(), opts.preserveRawSource ?? true);

    // 2. Compute stable_anchor_ids over the tree.
    const anchors = assignAnchors(root, strategy);

    // 3. Materialize public IRNode tree with provenance + raw_source.
    const irRoot = materialize(root, anchors, ts, /*isRoot=*/ true);
    return irRoot;
}

function materialize(
    node: BuildNode,
    anchors: Map<BuildNode, string>,
    ts: string,
    isRoot: boolean,
): IRNode {
    const stableAnchor = anchors.get(node);
    if (!stableAnchor) {
        throw new Error('assignAnchors did not produce an anchor for this BuildNode');
    }
    const rawSource: SourceFormat = {
        kind: 'claude-design-html',
        outerHtml: node.rawHtml,
        ...(node.inlineStyle ? { computedStyle: node.inlineStyle } : {}),
    };
    const provenance = {
        adapter: ADAPTER_NAME,
        version: ADAPTER_VERSION,
        ts,
        ...(isRoot ? { imported_at: ts, last_seen_at: ts } : {}),
    };
    return {
        tag: node.tag,
        stable_anchor_id: stableAnchor,
        ...(node.layout ? { layout: node.layout } : {}),
        ...(node.paint ? { paint: node.paint } : {}),
        ...(node.text ? { text: node.text } : {}),
        children: node.children.map((c) => materialize(c, anchors, ts, false)),
        ...(node.meta ? { meta: node.meta } : {}),
        provenance,
        raw_source: rawSource,
        confidence: node.confidence,
    };
}

// ── HTML parser ───────────────────────────────────────────────────────
//
// Phase 1: a regex-driven walker over a small subset of HTML that
// covers what Claude Design generates. We accept:
//   <tag attr="..." style="...">…</tag>
//   <tag />            — self-closing
//   text runs between tags
//
// We do NOT yet handle: DOCTYPE, comments, CDATA, scripts, attributes
// without quotes, malformed nesting (assumes well-formed). Phase 2
// swaps to parse5.

interface ParsedTag {
    tagName: string;
    attrs: Record<string, string>;
    selfClosing: boolean;
    /** Position in the input string where the inner content starts. */
    contentStart: number;
}

const TAG_OPEN = /<([a-zA-Z][a-zA-Z0-9-]*)((?:\s+[a-zA-Z-]+="[^"]*")*)\s*(\/?)>/g;
const ATTR = /([a-zA-Z-]+)="([^"]*)"/g;

function parseHtmlToBuildNode(html: string, preserveRawSource: boolean): BuildNode {
    // If the input has multiple roots, wrap them in an implicit View
    // (the spec's "fragment becomes a synthetic root" pattern).
    // We try to find a single outer tag first; if that consumes the
    // whole input, use it as the root, otherwise wrap.
    const trimmed = html.trim();
    const singleRoot = matchSingleOuterTag(trimmed);
    if (singleRoot) {
        const node = buildFromTag(trimmed, 0, trimmed.length, preserveRawSource);
        if (node) return node;
    }
    // Multi-root or text-only — wrap in a synthetic View.
    const children = parseFragment(trimmed, preserveRawSource);
    return {
        tag: 'View',
        children,
        rawHtml: trimmed,
        confidence: 'PASS',
    };
}

function matchSingleOuterTag(html: string): boolean {
    if (!html.startsWith('<')) return false;
    // Quick check: does the first tag's matching close-tag end the string?
    const firstOpenMatch = html.match(/^<([a-zA-Z][a-zA-Z0-9-]*)/);
    if (!firstOpenMatch) return false;
    const tag = firstOpenMatch[1];
    const closeTag = `</${tag}>`;
    return html.endsWith(closeTag);
}

function buildFromTag(
    src: string,
    start: number,
    end: number,
    preserveRawSource: boolean,
): BuildNode | null {
    // Parse the opening tag.
    const openMatch = src.slice(start).match(/^<([a-zA-Z][a-zA-Z0-9-]*)((?:\s+[a-zA-Z-]+="[^"]*")*)\s*(\/?)>/);
    if (!openMatch) return null;
    const [openText, tagName, attrText, selfClose] = openMatch;
    const attrs = parseAttrs(attrText);
    const contentStart = start + openText.length;

    if (selfClose === '/') {
        return makeNode(
            tagName,
            attrs,
            [],
            src.slice(start, contentStart),
            undefined,
            preserveRawSource,
        );
    }

    // Find the matching close-tag. Phase 1 simplification: assumes
    // well-formed, no same-tag nesting that would confuse us. (Phase 2
    // uses parse5 and this concern goes away.)
    const closeTag = `</${tagName}>`;
    const closeIdx = findMatchingCloseTag(src, contentStart, tagName);
    if (closeIdx < 0) {
        // Malformed — treat as self-closing on the whole remainder.
        return makeNode(tagName, attrs, [], src.slice(start, end), undefined, preserveRawSource);
    }

    const innerSrc = src.slice(contentStart, closeIdx);
    const children = parseFragment(innerSrc, preserveRawSource);
    const ownText = extractDirectTextContent(innerSrc);

    const outerEnd = closeIdx + closeTag.length;
    return makeNode(
        tagName,
        attrs,
        children,
        src.slice(start, outerEnd),
        ownText,
        preserveRawSource,
    );
}

function findMatchingCloseTag(src: string, fromIdx: number, tagName: string): number {
    const open = `<${tagName}`;
    const close = `</${tagName}>`;
    let depth = 1;
    let i = fromIdx;
    while (i < src.length) {
        const nextOpen = src.indexOf(open, i);
        const nextClose = src.indexOf(close, i);
        if (nextClose < 0) return -1;
        if (nextOpen >= 0 && nextOpen < nextClose) {
            // Could be a same-tag opening — verify it's actually a tag start.
            const after = src.charAt(nextOpen + open.length);
            if (after === ' ' || after === '>' || after === '\n' || after === '\t' || after === '/') {
                depth++;
                i = nextOpen + open.length;
                continue;
            }
            i = nextOpen + 1;
            continue;
        }
        depth--;
        if (depth === 0) return nextClose;
        i = nextClose + close.length;
    }
    return -1;
}

function parseAttrs(attrText: string): Record<string, string> {
    const out: Record<string, string> = {};
    if (!attrText) return out;
    let m: RegExpExecArray | null;
    const re = new RegExp(ATTR.source, 'g');
    while ((m = re.exec(attrText)) !== null) {
        out[m[1]] = m[2];
    }
    return out;
}

function parseFragment(src: string, preserveRawSource: boolean): BuildNode[] {
    const children: BuildNode[] = [];
    let i = 0;
    while (i < src.length) {
        // Skip leading whitespace between tags but preserve text-runs.
        if (src[i] === '<' && src[i + 1] !== '/') {
            // Compute the consumed length from the parser's view of the
            // tag's outer slice — independent of whether `node.rawHtml`
            // is retained on the BuildNode. When `preserveRawSource`
            // is false, `makeNode` deliberately stores `rawHtml: ''`,
            // so reading `node.rawHtml.length` would advance by 0 and
            // loop forever on any tag-shaped input. Compute consumed
            // length from the same outer-slice the builder uses.
            const consumed = consumeTagOuterLength(src, i);
            if (consumed <= 0) {
                i++;
                continue;
            }
            const node = buildFromTag(src, i, src.length, preserveRawSource);
            if (!node) {
                // Malformed but had a tag-shaped opener — skip past the
                // opener so we don't loop forever on the same byte.
                i += consumed;
                continue;
            }
            i += consumed;
            children.push(node);
            continue;
        }
        // Text run — collect up to next tag.
        const nextLt = src.indexOf('<', i);
        const textEnd = nextLt < 0 ? src.length : nextLt;
        const textRun = src.slice(i, textEnd).trim();
        if (textRun) {
            children.push({
                tag: 'Label',
                children: [],
                rawHtml: src.slice(i, textEnd),
                text: { text: textRun },
                confidence: 'PASS',
            });
        }
        // Guarantee forward progress even if textEnd === i (e.g. when
        // `<` appears immediately at the cursor and the open-tag branch
        // above bailed without consuming).
        i = textEnd === i ? i + 1 : textEnd;
    }
    return children;
}

/**
 * Compute the length of the outer-tag slice starting at `start` in
 * `src`, mirroring the structural traversal `buildFromTag` performs.
 * Returns 0 if `src[start]` doesn't begin a tag we can parse.
 *
 * This duplicates a bit of `buildFromTag`'s parsing because we need
 * the consumed length BEFORE constructing a BuildNode whose `rawHtml`
 * may have been intentionally cleared by `makeNode` when
 * `preserveRawSource` is false.
 */
function consumeTagOuterLength(src: string, start: number): number {
    const slice = src.slice(start);
    const openMatch = slice.match(/^<([a-zA-Z][a-zA-Z0-9-]*)((?:\s+[a-zA-Z-]+="[^"]*")*)\s*(\/?)>/);
    if (!openMatch) return 0;
    const [openText, tagName, , selfClose] = openMatch;
    if (selfClose === '/') {
        return openText.length;
    }
    const contentStart = start + openText.length;
    const closeIdx = findMatchingCloseTag(src, contentStart, tagName);
    if (closeIdx < 0) {
        // Malformed — `buildFromTag` treats this as self-closing on
        // the whole remainder; mirror that so we don't loop.
        return src.length - start;
    }
    const closeTag = `</${tagName}>`;
    return closeIdx + closeTag.length - start;
}

function extractDirectTextContent(innerSrc: string): string | undefined {
    // If the inner content has NO child tags, it's a leaf text-run.
    if (innerSrc.indexOf('<') < 0) {
        const trimmed = innerSrc.trim();
        return trimmed || undefined;
    }
    return undefined;
}

function makeNode(
    tagName: string,
    attrs: Record<string, string>,
    children: BuildNode[],
    rawHtml: string,
    leafText: string | undefined,
    preserveRawSource: boolean,
): BuildNode {
    const tag = mapHtmlTagToIRTag(tagName);
    const inlineStyle = parseInlineStyle(attrs.style);

    const layout = extractLayout(inlineStyle, attrs);
    const paint = extractPaint(inlineStyle, attrs);
    const text = extractText(inlineStyle, attrs, leafText);

    const node: BuildNode = {
        tag,
        children,
        rawHtml: preserveRawSource ? rawHtml : '',
        confidence: 'PASS',
    };
    if (layout) node.layout = layout;
    if (paint) node.paint = paint;
    if (text) node.text = text;
    if (Object.keys(inlineStyle).length > 0) {
        node.inlineStyle = inlineStyle;
    }
    if (attrs.role || attrs['data-pulp-role']) {
        node.meta = { role: attrs.role ?? attrs['data-pulp-role'] };
    }
    if (attrs.id) {
        node.source_node_id = attrs.id;
        node._adapter = ADAPTER_NAME;
    }
    return node;
}

function mapHtmlTagToIRTag(htmlTag: string): string {
    const lower = htmlTag.toLowerCase();
    switch (lower) {
        case 'p':
        case 'span':
        case 'h1':
        case 'h2':
        case 'h3':
        case 'h4':
        case 'h5':
        case 'h6':
        case 'label':
            return 'Label';
        case 'button':
            return 'Button';
        case 'img':
            return 'Image';
        case 'svg':
        case 'path':
            return 'Icon';
        case 'input':
            return 'TextEditor';
        case 'div':
        case 'section':
        case 'article':
        case 'header':
        case 'footer':
        case 'nav':
        case 'main':
        case 'aside':
        default:
            return 'View';
    }
}

function parseInlineStyle(styleAttr: string | undefined): Record<string, string> {
    if (!styleAttr) return {};
    const out: Record<string, string> = {};
    for (const decl of styleAttr.split(';')) {
        const idx = decl.indexOf(':');
        if (idx < 0) continue;
        const k = decl.slice(0, idx).trim();
        const v = decl.slice(idx + 1).trim();
        if (k && v) out[k] = v;
    }
    return out;
}

function extractLayout(
    style: Record<string, string>,
    attrs: Record<string, string>,
): TypedLayout | undefined {
    const out: TypedLayout = {};
    let any = false;
    const setLen = (k: keyof TypedLayout, v: string | undefined) => {
        if (v === undefined) return;
        const parsed = parseLength(v);
        if (parsed !== undefined) {
            (out as Record<string, unknown>)[k] = parsed;
            any = true;
        }
    };
    setLen('width', style['width']);
    setLen('height', style['height']);
    setLen('minWidth', style['min-width']);
    setLen('maxWidth', style['max-width']);
    setLen('minHeight', style['min-height']);
    setLen('maxHeight', style['max-height']);
    setLen('padding', style['padding']);
    setLen('paddingTop', style['padding-top']);
    setLen('paddingRight', style['padding-right']);
    setLen('paddingBottom', style['padding-bottom']);
    setLen('paddingLeft', style['padding-left']);
    setLen('margin', style['margin']);
    setLen('marginTop', style['margin-top']);
    setLen('marginRight', style['margin-right']);
    setLen('marginBottom', style['margin-bottom']);
    setLen('marginLeft', style['margin-left']);
    setLen('top', style['top']);
    setLen('right', style['right']);
    setLen('bottom', style['bottom']);
    setLen('left', style['left']);
    setLen('gap', style['gap']);
    setLen('rowGap', style['row-gap']);
    setLen('columnGap', style['column-gap']);
    setLen('flexBasis', style['flex-basis']);

    const display = style['display'];
    if (display) {
        out.display = display as TypedLayout['display'];
        any = true;
    }
    const flexDirection = style['flex-direction'];
    if (flexDirection) {
        out.flexDirection = flexDirection as TypedLayout['flexDirection'];
        any = true;
    }
    const flexWrap = style['flex-wrap'];
    if (flexWrap) {
        out.flexWrap = flexWrap as TypedLayout['flexWrap'];
        any = true;
    }
    const alignItems = style['align-items'];
    if (alignItems) {
        out.alignItems = alignItems as TypedLayout['alignItems'];
        any = true;
    }
    const alignSelf = style['align-self'];
    if (alignSelf) {
        out.alignSelf = alignSelf as TypedLayout['alignSelf'];
        any = true;
    }
    const justifyContent = style['justify-content'];
    if (justifyContent) {
        out.justifyContent = justifyContent as TypedLayout['justifyContent'];
        any = true;
    }
    const position = style['position'];
    if (position) {
        out.position = position as TypedLayout['position'];
        any = true;
    }
    const zIndex = style['z-index'];
    if (zIndex && /^-?\d+$/.test(zIndex)) {
        out.zIndex = parseInt(zIndex, 10);
        any = true;
    }
    const flexGrow = style['flex-grow'];
    if (flexGrow && !isNaN(parseFloat(flexGrow))) {
        out.flexGrow = parseFloat(flexGrow);
        any = true;
    }
    const flexShrink = style['flex-shrink'];
    if (flexShrink && !isNaN(parseFloat(flexShrink))) {
        out.flexShrink = parseFloat(flexShrink);
        any = true;
    }
    const overflow = style['overflow'];
    if (overflow) {
        out.overflow = overflow as TypedLayout['overflow'];
        any = true;
    }
    void attrs; // reserved for future data-* layout extensions
    return any ? out : undefined;
}

function extractPaint(
    style: Record<string, string>,
    _attrs: Record<string, string>,
): TypedPaint | undefined {
    const out: TypedPaint = {};
    let any = false;
    if (style['background-color']) {
        out.backgroundColor = style['background-color'];
        any = true;
    }
    if (style['color']) {
        out.color = style['color'];
        any = true;
    }
    if (style['border-color']) {
        out.borderColor = style['border-color'];
        any = true;
    }
    if (style['border-style']) {
        out.borderStyle = style['border-style'] as TypedPaint['borderStyle'];
        any = true;
    }
    const setLen = (k: keyof TypedPaint, v: string | undefined) => {
        if (v === undefined) return;
        const parsed = parseLength(v);
        if (typeof parsed === 'number') {
            (out as Record<string, unknown>)[k] = parsed;
            any = true;
        }
    };
    setLen('borderWidth', style['border-width']);
    setLen('borderTopWidth', style['border-top-width']);
    setLen('borderRightWidth', style['border-right-width']);
    setLen('borderBottomWidth', style['border-bottom-width']);
    setLen('borderLeftWidth', style['border-left-width']);
    setLen('borderRadius', style['border-radius']);
    setLen('borderTopLeftRadius', style['border-top-left-radius']);
    setLen('borderTopRightRadius', style['border-top-right-radius']);
    setLen('borderBottomLeftRadius', style['border-bottom-left-radius']);
    setLen('borderBottomRightRadius', style['border-bottom-right-radius']);
    if (style['opacity']) {
        const o = parseFloat(style['opacity']);
        if (!isNaN(o)) {
            out.opacity = o;
            any = true;
        }
    }
    return any ? out : undefined;
}

function extractText(
    style: Record<string, string>,
    _attrs: Record<string, string>,
    leafText: string | undefined,
): TypedText | undefined {
    const out: TypedText = {};
    let any = false;
    if (leafText) {
        out.text = leafText;
        any = true;
    }
    if (style['font-family']) {
        out.fontFamily = style['font-family'];
        any = true;
    }
    if (style['font-size']) {
        const fs = parseLength(style['font-size']);
        if (typeof fs === 'number') {
            out.fontSize = fs;
            any = true;
        }
    }
    if (style['font-weight']) {
        const w = style['font-weight'];
        out.fontWeight = /^\d+$/.test(w) ? (parseInt(w, 10) as number) : (w as TypedText['fontWeight']);
        any = true;
    }
    if (style['font-style']) {
        out.fontStyle = style['font-style'] as TypedText['fontStyle'];
        any = true;
    }
    if (style['line-height']) {
        const lh = parseLineHeight(style['line-height']);
        if (lh !== undefined) {
            out.lineHeight = lh;
            any = true;
        }
    }
    if (style['letter-spacing']) {
        const ls = parseLength(style['letter-spacing']);
        if (typeof ls === 'number') {
            out.letterSpacing = ls;
            any = true;
        }
    }
    if (style['text-align']) {
        out.textAlign = style['text-align'] as TypedText['textAlign'];
        any = true;
    }
    if (style['text-transform']) {
        out.textTransform = style['text-transform'] as TypedText['textTransform'];
        any = true;
    }
    return any ? out : undefined;
}

// ── Length parsing ────────────────────────────────────────────────────

function parseLength(raw: string | undefined): number | string | undefined {
    if (!raw) return undefined;
    const s = raw.trim();
    if (s === 'auto') return 'auto';
    // Token-ref: '{spacing.sm}' — DTCG resolution happens post-lowering.
    if (s.startsWith('{') && s.endsWith('}')) return s as `{${string}}`;
    if (s.endsWith('px')) {
        const n = parseFloat(s);
        if (!isNaN(n)) return n;
    }
    if (s.endsWith('%') || s.endsWith('vw') || s.endsWith('vh')
        || s.endsWith('vmin') || s.endsWith('vmax')) {
        return s; // forwarded verbatim to Yoga via Dimension struct
    }
    // Bare number → px (CSS quirk: only valid in some properties, but
    // we accept it leniently at the IR layer).
    const n = parseFloat(s);
    if (!isNaN(n) && /^[\d.]+$/.test(s)) return n;
    return undefined;
}

function parseLineHeight(raw: string): number | undefined {
    const s = raw.trim();
    if (s.endsWith('px')) {
        const n = parseFloat(s);
        return isNaN(n) ? undefined : n;
    }
    const n = parseFloat(s);
    return isNaN(n) ? undefined : n;
}
