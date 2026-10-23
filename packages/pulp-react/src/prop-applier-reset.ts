// prop-applier-reset — what a prop does to the native widget when it
// disappears from a React commit.
//
// The conditional spread `style={{ ...base, ...(active ? activeStyle : {}) }}`
// drops the active-only keys from the next commit instead of setting them to
// a neutral value. Every key the appliers handle therefore needs an answer to
// "what does removing it mean": a reset that hands its slots back to the
// widget's starting value, or a stated reason why nothing has to happen.
// `STYLE_KEY_RULES` and `NO_RESET_NEEDED` together cover every handled key;
// prop-applier-disappearing-style.test.ts enforces that, so a new key cannot
// land without a decision.
//
// Layout slots reset natively through `resetStyleProperty`, which restores
// the value the widget had before its first layout write (a `createRow` stays
// a row). Paint, typography and transform slots reset through their own
// setters with the argument that means "unset" or the widget's constructed
// value. Where no setter argument can express the starting value, the rule
// says so in `gap`.
//
// A reset is coarse: it may clear a slot another surviving key still wants
// (removing `padding` clears the edges a surviving `paddingTop` set). Each
// rule names the slots it writes, and after the resets every surviving key
// and every host-config creation default that shares a reset slot is applied
// again, so the survivors win exactly as they did before.

import { call } from './prop-applier-internal.js';
import { writeVisibility } from './prop-applier-visibility.js';

/// Returns false when it decided there was nothing to undo, so the slots it
/// names are not treated as reset.
type ResetFn = (id: string, type: string, oldValue: unknown) => void | boolean;

export interface StyleKeyRule {
    /// Native style slots this key writes. Keys that share a slot are
    /// re-applied after any of them is reset.
    readonly slots: readonly string[];
    /// Calls that hand the slots back to the widget's starting value.
    readonly reset?: ResetFn;
    /// Present when the reset cannot restore the starting value exactly:
    /// what is missing, in one sentence.
    readonly gap?: string;
}

const layout = (...slots: string[]): ResetFn => (id) => {
    for (const slot of slots) call('resetStyleProperty', id, slot);
};
const flexRule = (...slots: string[]): StyleKeyRule => ({
    slots: slots.map((s) => `flex.${s}`),
    reset: layout(...slots),
});
const gridRule = (...slots: string[]): StyleKeyRule => ({
    slots: slots.map((s) => `grid.${s}`),
    reset: layout(...slots.map((s) => `grid.${s}`)),
});
const setter = (slots: readonly string[], fn: string, ...args: unknown[]): StyleKeyRule => ({
    slots,
    reset: (id) => call(fn, id, ...args),
});

const OFFSET_GAP = 'View has no way to clear an inset edge once set, and 0 '
    + 'is only right for static or relative boxes, so the stale offset stays: '
    + 'a hover lift written {position: relative, top: -2} stays lifted.';
const COLOUR_GAP = 'the colour slot has no unset argument, so the edge is '
    + 'hidden instead; a surviving width re-applies with the last colour.';
const TYPOGRAPHY_GAP = 'the typography bridge has no clear for an own value, '
    + 'so the Label gets its constructed value as its own and stops inheriting '
    + 'this property from an ancestor.';

const BORDER_SLOTS = ['border.width', 'border.color', 'border.style', 'radius'];
const OUTLINE_SLOTS = ['outline'];
const SHADOW_SLOTS = ['shadow'];
const TEXT_SHADOW_SLOTS = ['textShadow'];
const TRANSFORM_SLOTS = ['transform'];
const TRANSITION_SLOTS = ['transition'];
const ANIMATION_SLOTS = ['animation'];
const BACKGROUND_SLOTS = ['background'];
const OVERLAY_CLAIM_SLOTS = ['overlayClaim'];

const clearBackground: StyleKeyRule = {
    slots: BACKGROUND_SLOTS,
    // An empty value clears each slot rather than painting a colour, so a
    // widget with its own background skin gets that skin back.
    reset: (id) => {
        call('setBackground', id, '');
        call('setBackgroundGradient', id, '');
    },
};
const clearOutline: StyleKeyRule = {
    slots: OUTLINE_SLOTS,
    reset: (id) => {
        call('setOutlineWidth', id, 0);
        call('setOutlineStyle', id, 'none');
        call('setOutlineOffset', id, 0);
    },
    gap: 'the outline colour has no unset argument; it is hidden with the outline.',
};
const clearShadow = setter(SHADOW_SLOTS, 'clearBoxShadow');
const clearTextShadow: StyleKeyRule = {
    slots: TEXT_SHADOW_SLOTS,
    reset: (id) => {
        call('setTextShadowColor', id, '');
        call('setTextShadowOffset', id, 0, 0);
        call('setTextShadowRadius', id, 0);
    },
};
const clearTransform: StyleKeyRule = {
    slots: TRANSFORM_SLOTS,
    reset: (id) => {
        call('setTranslate', id, 0, 0);
        call('setRotation', id, 0);
        call('setScale', id, 1);
        call('setSkew', id, 0, 0);
    },
};
const clearTransition = setter(TRANSITION_SLOTS, 'setTransition', 'none');
const stopAnimation = { slots: ANIMATION_SLOTS, reset: layout('animation') };
const borderEdge = (side: 'Top' | 'Right' | 'Bottom' | 'Left'): StyleKeyRule =>
    setter([`border.${side.toLowerCase()}`], `setBorder${side}Width`, 0);
const borderEdgeColour = (side: 'Top' | 'Right' | 'Bottom' | 'Left'): StyleKeyRule => ({
    ...borderEdge(side),
    gap: `per-edge ${COLOUR_GAP}`,
});
const cornerRadius = (corner: string): StyleKeyRule =>
    setter(['radius'], `setBorder${corner}Radius`, 0);
// Release the overlay slot only when the removed value had claimed it.
const releaseClaim = (claimed: (oldValue: unknown) => boolean): StyleKeyRule => ({
    slots: OVERLAY_CLAIM_SLOTS,
    reset: (id, _type, oldValue) => {
        if (!claimed(oldValue)) return false;
        call('releaseOverlay', id);
        return true;
    },
});
const whiteSpaceReset: StyleKeyRule = {
    slots: ['whiteSpace', 'multiLine'],
    // A constructed Label is single-line; `normal` alone would leave it
    // wrapping. Host-config text defaults re-apply after this.
    reset: (id) => {
        call('setWhiteSpace', id, 'normal');
        call('setMultiLine', id, false);
    },
};
const lineClampReset: StyleKeyRule = {
    slots: ['lineClamp', 'multiLine'],
    reset: (id) => {
        call('setLineClamp', id, 0);
        call('setMultiLine', id, false);
    },
};

function overlayRole(role: unknown): boolean {
    if (typeof role !== 'string') return false;
    const r = role.toLowerCase();
    return r === 'dialog' || r === 'alertdialog' || r === 'menu' || r === 'listbox';
}

/// Every handled key whose removal changes native state.
export const STYLE_KEY_RULES: Readonly<Record<string, StyleKeyRule>> = {
    // ── layout ───────────────────────────────────────────────────────
    direction: {
        slots: ['flex.direction', 'writingDirection'],
        reset: (id) => {
            call('resetStyleProperty', id, 'direction');
            call('setDirection', id, 'auto');
        },
    },
    flexDirection: flexRule('direction'),
    display: {
        // `none` hid the widget; `flex` turned a column into a row.
        slots: ['visible', 'flex.direction'],
        reset: (id) => {
            writeVisibility(id, true);
            call('resetStyleProperty', id, 'direction');
        },
    },
    gap: flexRule('gap'),
    rowGap: flexRule('row_gap'),
    columnGap: flexRule('column_gap'),
    padding: flexRule('padding', 'padding_top', 'padding_right', 'padding_bottom', 'padding_left'),
    paddingTop: flexRule('padding_top'),
    paddingRight: flexRule('padding_right'),
    paddingBottom: flexRule('padding_bottom'),
    paddingLeft: flexRule('padding_left'),
    paddingHorizontal: flexRule('padding_left', 'padding_right'),
    paddingVertical: flexRule('padding_top', 'padding_bottom'),
    paddingStart: flexRule('padding_left'),
    paddingEnd: flexRule('padding_right'),
    margin: flexRule('margin', 'margin_top', 'margin_right', 'margin_bottom', 'margin_left'),
    marginTop: flexRule('margin_top'),
    marginRight: flexRule('margin_right'),
    marginBottom: flexRule('margin_bottom'),
    marginLeft: flexRule('margin_left'),
    marginHorizontal: flexRule('margin_left', 'margin_right'),
    marginVertical: flexRule('margin_top', 'margin_bottom'),
    marginStart: flexRule('margin_left'),
    marginEnd: flexRule('margin_right'),
    flexGrow: flexRule('flex_grow'),
    flexShrink: flexRule('flex_shrink'),
    flex: flexRule('flex_grow', 'flex_shrink', 'flex_basis'),
    flexBasis: flexRule('flex_basis'),
    flexWrap: flexRule('flex_wrap'),
    order: flexRule('order'),
    width: flexRule('width'),
    height: flexRule('height'),
    minWidth: flexRule('min_width'),
    minHeight: flexRule('min_height'),
    maxWidth: flexRule('max_width'),
    maxHeight: flexRule('max_height'),
    alignItems: flexRule('align_items'),
    alignSelf: flexRule('align_self'),
    alignContent: flexRule('align_content'),
    justifyContent: flexRule('justify_content'),
    aspectRatio: flexRule('aspect_ratio'),
    boxSizing: flexRule('box_sizing'),
    overflow: setter(['overflow'], 'setOverflow', 'visible'),
    overflowX: setter(['overflow'], 'setOverflow', 'visible'),
    overflowY: setter(['overflow'], 'setOverflow', 'visible'),
    position: setter(['position'], 'setPosition', 'static'),
    zIndex: setter(['zIndex'], 'setZIndex', 0),
    top: { slots: ['inset.top'], gap: OFFSET_GAP },
    right: { slots: ['inset.right'], gap: OFFSET_GAP },
    bottom: { slots: ['inset.bottom'], gap: OFFSET_GAP },
    left: { slots: ['inset.left'], gap: OFFSET_GAP },
    start: { slots: ['inset.left'], gap: OFFSET_GAP },
    end: { slots: ['inset.right'], gap: OFFSET_GAP },
    inset: { slots: ['inset.top', 'inset.right', 'inset.bottom', 'inset.left'], gap: OFFSET_GAP },
    insetBlock: { slots: ['inset.top', 'inset.bottom'], gap: OFFSET_GAP },
    insetInline: { slots: ['inset.left', 'inset.right'], gap: OFFSET_GAP },
    gridTemplateColumns: gridRule('template_columns'),
    gridTemplateRows: gridRule('template_rows'),
    gridTemplateAreas: gridRule('template_areas'),
    gridAutoColumns: gridRule('auto_columns'),
    gridAutoRows: gridRule('auto_rows'),
    gridAutoFlow: gridRule('auto_flow'),
    gridArea: {
        slots: ['grid.grid_area', 'grid.column_start', 'grid.column_end',
                'grid.row_start', 'grid.row_end'],
        reset: layout('grid.grid_area'),
    },
    gridColumn: gridRule('column_start', 'column_end'),
    gridRow: gridRule('row_start', 'row_end'),
    gridColumnStart: gridRule('column_start'),
    gridColumnEnd: gridRule('column_end'),
    gridRowStart: gridRule('row_start'),
    gridRowEnd: gridRule('row_end'),
    gridGap: { slots: ['grid.column_gap', 'grid.row_gap'], reset: layout('grid.gap') },
    gridColumnGap: gridRule('column_gap'),
    gridRowGap: gridRule('row_gap'),

    // ── paint ────────────────────────────────────────────────────────
    background: clearBackground,
    backgroundGradient: clearBackground,
    backgroundImage: clearBackground,
    experimental_backgroundImage: clearBackground,
    backgroundAttachment: setter(['backgroundAttachment'], 'setBackgroundAttachment', ''),
    backgroundClip: setter(['backgroundClip'], 'setBackgroundClip', ''),
    backgroundOrigin: setter(['backgroundOrigin'], 'setBackgroundOrigin', ''),
    backgroundRepeat: setter(['backgroundRepeat'], 'setBackgroundRepeat', ''),
    border: {
        // The object form also writes the corner radius.
        slots: BORDER_SLOTS,
        reset: (id) => {
            call('setBorderWidth', id, 0);
            call('setBorderStyle', id, 'solid');
            call('setBorderRadius', id, 0);
        },
        gap: COLOUR_GAP,
    },
    // Width zero collapses the painted edge whatever the colour slot holds.
    borderColor: { ...setter(['border.color', 'border.width'], 'setBorderWidth', 0), gap: COLOUR_GAP },
    borderWidth: setter(['border.width'], 'setBorderWidth', 0),
    borderStyle: setter(['border.style'], 'setBorderStyle', 'solid'),
    borderRadius: setter(['radius'], 'setBorderRadius', 0),
    borderTopLeftRadius: cornerRadius('TopLeft'),
    borderTopRightRadius: cornerRadius('TopRight'),
    borderBottomLeftRadius: cornerRadius('BottomLeft'),
    borderBottomRightRadius: cornerRadius('BottomRight'),
    borderTop: { slots: ['border.top'], reset: (id) => call('setBorderSide', id, 'top', 0, 'transparent') },
    borderRight: { slots: ['border.right'], reset: (id) => call('setBorderSide', id, 'right', 0, 'transparent') },
    borderBottom: { slots: ['border.bottom'], reset: (id) => call('setBorderSide', id, 'bottom', 0, 'transparent') },
    borderLeft: { slots: ['border.left'], reset: (id) => call('setBorderSide', id, 'left', 0, 'transparent') },
    borderTopWidth: borderEdge('Top'),
    borderRightWidth: borderEdge('Right'),
    borderBottomWidth: borderEdge('Bottom'),
    borderLeftWidth: borderEdge('Left'),
    borderStartWidth: borderEdge('Left'),
    borderEndWidth: borderEdge('Right'),
    borderTopColor: borderEdgeColour('Top'),
    borderRightColor: borderEdgeColour('Right'),
    borderBottomColor: borderEdgeColour('Bottom'),
    borderLeftColor: borderEdgeColour('Left'),
    borderCurve: setter(['borderCurve'], 'setBorderCurve', 'circular'),
    listStyle: {
        slots: ['listStyle.type', 'listStyle.position', 'listStyle.image'],
        reset: (id) => {
            call('setListStyleType', id, 'disc');
            call('setListStylePosition', id, 'outside');
            call('setListStyleImage', id, 'none');
        },
    },
    listStyleType: setter(['listStyle.type'], 'setListStyleType', 'disc'),
    listStylePosition: setter(['listStyle.position'], 'setListStylePosition', 'outside'),
    listStyleImage: setter(['listStyle.image'], 'setListStyleImage', 'none'),
    outline: clearOutline,
    outlineColor: clearOutline,
    outlineOffset: clearOutline,
    outlineStyle: clearOutline,
    outlineWidth: clearOutline,
    opacity: setter(['opacity'], 'setOpacity', 1),
    visible: { slots: ['visible'], reset: (id) => writeVisibility(id, true) },
    boxShadow: clearShadow,
    shadowColor: clearShadow,
    shadowOffset: clearShadow,
    shadowOpacity: clearShadow,
    shadowRadius: clearShadow,
    elevation: clearShadow,
    backdropFilter: setter(['backdropFilter'], 'setBackdropFilter', 0),
    backfaceVisibility: setter(['backfaceVisibility'], 'setBackfaceVisibility', 'visible'),
    cursor: setter(['cursor'], 'setCursor', 'default'),
    filter: setter(['filter'], 'setFilter', 'none'),
    clipPath: setter(['clipPath'], 'setClipPath', 'none'),
    mask: setter(['mask'], 'setMask', ''),
    maskImage: setter(['maskImage'], 'setMaskImage', ''),
    maskSize: setter(['maskSize'], 'setMaskSize', ''),
    appearance: setter(['appearance'], 'setAppearance', ''),
    objectFit: setter(['objectFit'], 'setObjectFit', ''),
    objectPosition: setter(['objectPosition'], 'setObjectPosition', ''),
    mixBlendMode: setter(['mixBlendMode'], 'setMixBlendMode', 'normal'),
    pointerEvents: setter(['pointerEvents'], 'setPointerEvents', 'auto'),
    hitSlop: setter(['hitSlop'], 'setHitSlop', 0, 0, 0, 0),
    userSelect: setter(['userSelect'], 'setUserSelect', 'auto'),
    isolation: setter(['isolation'], 'setIsolation', ''),

    // ── typography ───────────────────────────────────────────────────
    whiteSpace: whiteSpaceReset,
    textOverflow: setter(['textOverflow'], 'setTextOverflow', 'clip'),
    writingDirection: setter(['writingDirection'], 'setDirection', 'auto'),
    verticalAlign: setter(['verticalAlign'], 'setVerticalAlign', 'top'),
    textAlignVertical: setter(['verticalAlign'], 'setVerticalAlign', 'top'),
    textDecorationColor: {
        slots: ['textDecorationColor'],
        gap: 'the decoration colour has no unset argument; the last colour stays '
            + 'until the decoration line is removed.',
    },
    textDecorationStyle: setter(['textDecorationStyle'], 'setTextDecorationStyle', 'solid'),
    textDecorationLine: setter(['textDecorationLine'], 'setTextDecoration', 'none'),
    text: {
        slots: ['text'],
        gap: 'text content is resolved from children and `text` together by the '
            + 'host config on every commit; clearing it here would erase text the '
            + 'children still supply, so a widget left with neither keeps its last text.',
    },
    color: {
        ...setter(['textColor'], 'setTextColor', ''),
        gap: 'setTextColor ignores the empty "use default" argument today, so the '
            + 'last colour stays painted until the typography bridge clears it.',
    },
    textColor: {
        ...setter(['textColor'], 'setTextColor', ''),
        gap: 'setTextColor ignores the empty "use default" argument today, so the '
            + 'last colour stays painted until the typography bridge clears it.',
    },
    textAlign: { ...setter(['textAlign'], 'setTextAlign', 'left'), gap: TYPOGRAPHY_GAP },
    fontFamily: setter(['fontFamily'], 'setFontFamily', ''),
    fontSize: {
        ...setter(['fontSize'], 'setFontSize', 14),
        gap: `${TYPOGRAPHY_GAP} The imported-text scale also applies to the 14 px.`,
    },
    fontWeight: { ...setter(['fontWeight'], 'setFontWeight', 400), gap: TYPOGRAPHY_GAP },
    fontStyle: setter(['fontStyle'], 'setFontStyle', 'normal'),
    letterSpacing: { ...setter(['letterSpacing'], 'setLetterSpacing', 0), gap: TYPOGRAPHY_GAP },
    lineHeight: setter(['lineHeight'], 'setLineHeight', 0),
    lineClamp: lineClampReset,
    webkitLineClamp: lineClampReset,
    textShadowColor: clearTextShadow,
    textShadowOffset: clearTextShadow,
    textShadowRadius: clearTextShadow,
    includeFontPadding: setter(['includeFontPadding'], 'setIncludeFontPadding', true),
    textTransform: setter(['textTransform'], 'setTextTransform', 'none'),
    fontVariant: setter(['fontVariant'], 'setFontVariant', ''),

    // ── transform, transition, animation ─────────────────────────────
    transform: clearTransform,
    transformOrigin: setter(['transformOrigin'], 'setTransformOrigin', 0.5, 0.5),
    transition: clearTransition,
    transitionProperty: clearTransition,
    transitionDuration: clearTransition,
    transitionDelay: clearTransition,
    transitionTimingFunction: clearTransition,
    animationName: stopAnimation,
    animationDuration: stopAnimation,
    animationDelay: stopAnimation,
    animationTimingFunction: stopAnimation,
    animationIterationCount: stopAnimation,
    animationDirection: stopAnimation,
    animationFillMode: stopAnimation,
    animationPlayState: stopAnimation,

    // ── event routing ────────────────────────────────────────────────
    overlay: releaseClaim((v) => Boolean(v)),
    role: releaseClaim(overlayRole),
    'aria-modal': releaseClaim((v) => v === true || v === 'true' || v === ''),
    'aria-haspopup': setter(['overlayTrigger'], 'setOverlayTrigger', false),
    overlayTrigger: setter(['overlayTrigger'], 'setOverlayTrigger', false),

    // ── widget-typed keys ────────────────────────────────────────────
    shader: setter(['shader'], 'clearWidgetShader'),
    disabled: setter(['enabled'], 'setEnabled', true),
    accentColor: setter(['accentColor'], 'setAccentColor', ''),
    placeholder: setter(['placeholder'], 'setPlaceholder', ''),
    d: setter(['svgPath'], 'setSvgPath', ''),
    fillRule: setter(['fillRule'], 'setSvgFillRule', 'nonzero'),
    fillGradient: setter(['fill'], 'setSvgFillGradient', ''),
    strokeWidth: setter(['strokeWidth'], 'setSvgStrokeWidth', 1),
    stroke: {
        slots: ['stroke'],
        gap: 'SvgPath re-emits stroke and strokeGradient as one compound state; '
            + 'SvgRect and SvgLine keep the last stroke, since an empty stroke means '
            + '"none" rather than the constructed one.',
    },
    strokeGradient: {
        slots: ['stroke'],
        gap: 'SvgPath re-emits stroke and strokeGradient as one compound state; '
            + 'other SVG widgets have no gradient stroke to clear.',
    },
    fill: {
        slots: ['fill'],
        gap: 'SVG widgets do not expose their constructed fill, and an empty fill '
            + 'means "none" rather than the SVG default black.',
    },
    viewBox: {
        slots: ['viewBox'],
        gap: 'a native SVG widget has no unset viewBox; a primitive that loses its '
            + 'own keeps the last one rather than re-inheriting the parent <svg>.',
    },
    orientation: {
        slots: ['orientation'],
        gap: 'orientation is chosen when the control is built and the widget does '
            + 'not expose its constructed value.',
    },
};

const SVG_GEOMETRY_REASON = 'SvgRect and SvgLine geometry is re-emitted whole '
    + 'whenever a coordinate changes, with SVG\'s default 0 for a missing one.';

/// Handled keys whose removal needs no native write, and why.
export const NO_RESET_NEEDED: Readonly<Record<string, string>> = {
    overlayParent: 'qualifies an overlay claim; reclaimOverlayForMovedParent '
        + 'compares the old and new declaration on every commit.',
    src: 'the applier clears the image source itself when the prop disappears.',
    selectionMode: 'the applier restores single selection itself when the prop disappears.',
    selected: 'the applier clears the selection itself when the prop disappears.',
    value: 'control data, not style: as with an uncontrolled DOM input the widget '
        + 'keeps the value the user or the last commit gave it.',
    level: 'meter data, not style: the next sample overwrites it.',
    data: 'spectrum and waveform data, not style: the next frame overwrites it.',
    min: 'range bounds are control data; a control that loses its bounds keeps '
        + 'the last range so its value does not jump.',
    max: 'range bounds are control data; a control that loses its bounds keeps '
        + 'the last range so its value does not jump.',
    step: 'control data; the last quantisation step stays in force.',
    scrollTop: 'scroll position is user state; as in the DOM, dropping the prop '
        + 'leaves the view where it is.',
    scrollLeft: 'scroll position is user state; as in the DOM, dropping the prop '
        + 'leaves the view where it is.',
    scrollToRow: 'a one-shot scroll command, not a persistent state.',
    rowCount: 'virtual-list data that a list cannot render without; dropping it '
        + 'keeps the rows on screen until the list unmounts.',
    rowHeight: 'virtual-list geometry; the rows keep their measured height.',
    overscan: 'a virtual-list tuning value with no visible effect on removal.',
    renderRow: 'the row renderer is read on each refresh; removing it only stops refreshes.',
    x: SVG_GEOMETRY_REASON,
    y: SVG_GEOMETRY_REASON,
    x1: SVG_GEOMETRY_REASON,
    y1: SVG_GEOMETRY_REASON,
    x2: SVG_GEOMETRY_REASON,
    y2: SVG_GEOMETRY_REASON,
};

/// Apply the reset for each removed key, then re-apply every creation
/// default and surviving key that shares a reset slot. Returns true if any
/// bridge call was made.
export function resetRemovedStyleKeys(
    id: string,
    type: string,
    removed: ReadonlyArray<{ key: string; oldValue: unknown }>,
    survivorKeys: readonly string[],
    creationStyle: Readonly<Record<string, unknown>> | undefined,
    reapply: (key: string, value: unknown, fromCreation: boolean) => void,
): boolean {
    const touched = new Set<string>();
    let mutated = false;
    for (const { key, oldValue } of removed) {
        const rule = STYLE_KEY_RULES[key];
        if (!rule?.reset) continue;
        if (rule.reset(id, type, oldValue) === false) continue;
        mutated = true;
        for (const slot of rule.slots) touched.add(slot);
    }
    if (touched.size === 0) return mutated;
    const shares = (key: string) =>
        STYLE_KEY_RULES[key]?.slots.some((slot) => touched.has(slot)) ?? false;
    const survivors = new Set(survivorKeys);
    if (creationStyle) {
        for (const key of Object.keys(creationStyle)) {
            if (survivors.has(key) || !shares(key)) continue;
            reapply(key, creationStyle[key], true);
            mutated = true;
        }
    }
    for (const key of survivorKeys) {
        if (!shares(key)) continue;
        reapply(key, undefined, false);
        mutated = true;
    }
    return mutated;
}
