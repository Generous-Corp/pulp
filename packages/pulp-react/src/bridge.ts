// Type-safe shim over the C++ WidgetBridge API surface.
// All bridge functions are registered as globals on the JS engine
// (QuickJS / JSC / V8) by core/view/src/widget_bridge.cpp. We declare
// them as ambient globals here so the rest of @pulp/react can call
// them with type-checking, without the host config having to wrap
// every call in a runtime existence check.

declare global {
    // ── Widget creation ─────────────────────────────────────────────
    function createCol(id: string, parentId: string): void;
    function createRow(id: string, parentId: string): void;
    function createPanel(id: string, parentId: string): void;
    function createLabel(id: string, text: string, parentId: string): void;
    function createButton(id: string, text: string, parentId: string): void;
    function createKnob(id: string, parentId: string): void;
    function createFader(id: string, orientation: 'vertical' | 'horizontal', parentId: string): void;
    function createSpectrum(id: string, parentId: string): void;
    function createWaveform(id: string, parentId: string): void;
    function createCanvas(id: string, parentId: string): void;
    function createCheckbox(id: string, parentId: string): void;
    function createToggle(id: string, parentId: string): void;
    function createToggleButton(id: string, parentId: string): void;
    function createCombo(id: string, parentId: string): void;
    function createListBox(id: string, parentId: string): void;
    function createModal(id: string, parentId: string): void;
    function createTextEditor(id: string, parentId: string): void;
    function createScrollView(id: string, parentId: string): void;
    function createImage(id: string, parentId: string): void;
    function createIcon(id: string, parentId: string): void;
    function createProgress(id: string, parentId: string): void;
    function createMeter(id: string, parentId: string): void;
    function createXYPad(id: string, parentId: string): void;
    function createGrid(id: string, parentId: string): void;

    // ── Widget mutation ─────────────────────────────────────────────
    function removeWidget(id: string): void;
    /// Move an existing widget to a new parent at the given index.
    /// pulp #772 adds this as a non-DOM-coupled alternative to the
    /// __domAppend reparent path in core/view/src/widget_bridge.cpp:1550.
    /// If absent at runtime, fall back to remove + create at parent.
    /// Declared as a const so consumers can branch on `typeof moveWidget`.
    const moveWidget: ((id: string, newParentId: string, index: number) => void) | undefined;
    /// insertBefore on an existing sibling under the same parent.
    /// Symmetric with moveWidget; absent on older runtimes.
    const insertChild: ((parentId: string, childId: string, index: number) => void) | undefined;

    // ── Flex / Yoga layout ──────────────────────────────────────────
    function setFlex(
        id: string,
        key:
            | 'direction'
            | 'gap'
            | 'row_gap'
            | 'column_gap'
            | 'padding'
            | 'padding_top'
            | 'padding_right'
            | 'padding_bottom'
            | 'padding_left'
            | 'margin'
            | 'margin_top'
            | 'margin_right'
            | 'margin_bottom'
            | 'margin_left'
            | 'flex_grow'
            | 'flex_shrink'
            | 'flex_basis'
            | 'flex_wrap'
            | 'order'
            | 'width'
            | 'height'
            | 'min_width'
            | 'min_height'
            | 'max_width'
            | 'max_height'
            | 'align_items'
            | 'align_self'
            | 'justify_content'
            // pulp #1434 — width/height ratio. Value is a finite positive
            // number; 0 / non-finite clears the slot on the bridge side.
            | 'aspect_ratio',
        value: number | string,
    ): void;

    // ── Visual style ────────────────────────────────────────────────
    function setBackground(id: string, hexColor: string): void;
    function setBackgroundGradient(id: string, css: string): void;
    // pulp #1517 — background sub-properties (storage-only, partial paint).
    const setBackgroundAttachment: ((id: string, kw: string) => void) | undefined;
    const setBackgroundClip:       ((id: string, kw: string) => void) | undefined;
    const setBackgroundOrigin:     ((id: string, kw: string) => void) | undefined;
    function setBorder(id: string, hexColor: string, width: number, radius: number): void;
    function setBorderSide(
        id: string,
        side: 'top' | 'right' | 'bottom' | 'left',
        width: number,
        hexColor: string,
    ): void;
    // pulp #1027 — per-attribute border setters (preserve unset siblings).
    // The unified `setBorder` clobbers all three slots; these mutate one
    // field at a time on the View. Optional at runtime so older bridges
    // still link, hence the `const | undefined` shape.
    const setBorderColor: ((id: string, hexColor: string) => void) | undefined;
    const setBorderWidth: ((id: string, width: number) => void) | undefined;
    const setBorderRadius: ((id: string, radius: number) => void) | undefined;
    // pulp #1434 Triage #10 — border-style keyword. Bridge maps to
    // View::BorderStyle; Skia installs SkDashPathEffect for dashed/
    // dotted at stroke time. Other named styles degrade to solid.
    const setBorderStyle: ((id: string, style: string) => void) | undefined;
    /// pulp #1434 Phase A2-3 — writing direction. Maps to
    /// View::WritingDirection; Yoga + Skia honor at layout / text shape.
    const setDirection: ((id: string, dir: 'ltr' | 'rtl' | 'inherit' | string) => void) | undefined;
    // pulp #1514 — list-style cluster. Pulp doesn't model
    // <li>/<ul>/<ol> semantics; the bridge stores the value
    // verbatim on the View. Marker glyph rendering is deferred —
    // catalog status is `partial` (stored, not painted).
    const setListStyleType: ((id: string, type: string) => void) | undefined;
    const setListStyleImage: ((id: string, url: string) => void) | undefined;
    const setListStylePosition: ((id: string, pos: string) => void) | undefined;
    /// pulp #1434 Phase A2-2 — CSS Grid bridge fn. The C++ side parses
    /// template-track strings, named-area strings, and the grid-area
    /// shorthand (named token vs `row / col / row / col` numeric form).
    const setGrid: ((id: string, key: string, value: string | number) => void) | undefined;
    const setBorderTopColor: ((id: string, hexColor: string) => void) | undefined;
    const setBorderRightColor: ((id: string, hexColor: string) => void) | undefined;
    const setBorderBottomColor: ((id: string, hexColor: string) => void) | undefined;
    const setBorderLeftColor: ((id: string, hexColor: string) => void) | undefined;
    const setBorderTopWidth: ((id: string, width: number) => void) | undefined;
    const setBorderRightWidth: ((id: string, width: number) => void) | undefined;
    const setBorderBottomWidth: ((id: string, width: number) => void) | undefined;
    const setBorderLeftWidth: ((id: string, width: number) => void) | undefined;
    const setBorderTopLeftRadius: ((id: string, radius: number) => void) | undefined;
    const setBorderTopRightRadius: ((id: string, radius: number) => void) | undefined;
    const setBorderBottomLeftRadius: ((id: string, radius: number) => void) | undefined;
    const setBorderBottomRightRadius: ((id: string, radius: number) => void) | undefined;
    // pulp #1519 — outline cluster. Paint-time ring drawn OUTSIDE the
    // border-box; does NOT take Yoga layout space. Style keyword set
    // mirrors setBorderStyle.
    const setOutlineColor: ((id: string, hexColor: string) => void) | undefined;
    const setOutlineOffset: ((id: string, offsetPx: number) => void) | undefined;
    const setOutlineStyle: ((id: string, style: string) => void) | undefined;
    const setOutlineWidth: ((id: string, widthPx: number) => void) | undefined;
    function setOpacity(id: string, alpha: number): void;
    function setVisible(id: string, visible: boolean): void;
    /// pulp #1434 Phase A2-1 — CSS transitions + animations.
    /// `setTransition` parses the full shorthand; the longhand setters
    /// apply uniformly across the parsed list.
    const setTransition: ((id: string, css: string) => void) | undefined;
    const setTransitionProperty: ((id: string, props: string) => void) | undefined;
    const setTransitionDuration: ((id: string, seconds: number) => void) | undefined;
    const setTransitionDelay: ((id: string, seconds: number) => void) | undefined;
    const setTransitionTimingFunction: ((id: string, easing: string) => void) | undefined;
    /// `defineKeyframes` populates the application-wide registry; PR 4
    /// wires playback. Phase A2-1 PR 1 ships parser + storage.
    const defineKeyframes: ((name: string, stops_json: string) => void) | undefined;
    const setAnimation: ((id: string, name: string, duration: number, iterations: number, direction: string) => void) | undefined;
    function setPosition(id: string, top: number, left: number, right?: number, bottom?: number): void;
    // pulp #1434 (Triage #15) — surface the existing C++ setBoxShadow /
    // clearBoxShadow bridge fns at the @pulp/react TS layer so RN-style
    // JSX (`style={{ boxShadow: '0 2px 4px rgba(0,0,0,.1)' }}` or the
    // object form) reaches View::set_box_shadow without going through
    // the el.style proxy. Multi-shadow comma-separated lists are
    // deferred (single-shadow path lands first).
    function setBoxShadow(
        id: string,
        offsetX: number,
        offsetY: number,
        blur: number,
        spread: number,
        color: string,
        inset?: boolean
    ): void;
    function clearBoxShadow(id: string): void;

    // pulp #1434 (Triage #9) — RN-style `transform: [{translateX: 10},
    // {rotate: '45deg'}, {scale: 1.5}]` is dispatched by the prop-
    // applier as a consolidated trio of bridge calls. setTranslate
    // takes both axes at once (no axis-clobber); setRotation is
    // degrees; setScale is uniform-only (independent scaleX/scaleY
    // remains a deferred bridge-side gap). All three already exist
    // on the C++ side via View::set_translate / set_rotation /
    // set_scale and are registered in widget_bridge.cpp.
    function setTranslate(id: string, x: number, y: number): void;
    function setRotation(id: string, degrees: number): void;
    function setScale(id: string, scale: number): void;
    // pulp #1434 Triage #9 fan-out — setSkew dispatches both axes at
    // once. View::set_skew has existed since the 2D slot was added;
    // the bridge fn registration landed alongside this prop-applier
    // walker extension so skewX/skewY now reaches the View.
    function setSkew(id: string, x_deg: number, y_deg: number): void;

    // ── Text ────────────────────────────────────────────────────────
    function setText(id: string, text: string): void;
    function setTextColor(id: string, hexColor: string): void;
    function setTextAlign(id: string, align: 'left' | 'center' | 'right'): void;

    // pulp #1552 — line-clamp + background-repeat. setLineClamp clamps
    // a multi-line Label to N visible lines (0 disables; >=1 enables
    // wrap implicitly on the bridge side). setBackgroundRepeat is
    // storage-only on the View; paint-time honoring lands with the
    // background-image / repeating-gradient work. Optional at runtime
    // so older bridges still link.
    const setLineClamp: ((id: string, n: number) => void) | undefined;
    const setBackgroundRepeat: ((id: string, kw: string) => void) | undefined;

    // ── Widget-specific data ────────────────────────────────────────
    function setSpectrumData(id: string, samples: number[] | Float32Array): void;
    function setWaveformData(id: string, samples: number[] | Float32Array): void;
    function setMeterLevel(id: string, level: number): void;
    function setProgress(id: string, fraction: number): void;
    function setValue(id: string, value: number): void;

    // ── Theme ───────────────────────────────────────────────────────
    function setTheme(name: 'dark' | 'light' | 'pro_audio' | string): void;

    // ── Layout flush + frame service ────────────────────────────────
    /// Force a layout pass on the root container. Used in
    /// `resetAfterCommit` so the host config owns commit-time flush.
    /// Declared as a const so consumers can branch on `typeof layout`.
    const layout: (() => void) | undefined;

    // ── pulp #1515 — CSS clip-path / mask cluster ──────────────────
    /// CSS `clip-path: path("...")`. Bridge accepts the SVG-path-d
    /// string; Skia parses via `SkPath::FromSVGString` and installs
    /// it as the canvas clip before children paint. URL refs and
    /// named shape forms are deferred. Optional at runtime so older
    /// bridges still link.
    const setClipPath: ((id: string, svgPathD: string) => void) | undefined;
    /// CSS `mask-image`. Storage-only today; shader composite paint
    /// slice is the follow-up. Optional at runtime.
    const setMaskImage: ((id: string, value: string) => void) | undefined;
    /// CSS `mask` shorthand. Stored verbatim alongside the
    /// `maskImage` longhand the JS shim extracts. Optional at runtime.
    const setMask: ((id: string, shorthand: string) => void) | undefined;

    // ── Overlay click routing (pulp #1148) ──────────────────────────
    /// Claim the view as the active click-eligible overlay so the
    /// platform window host short-circuits hit-testing for clicks that
    /// land inside the view's bounds. Optional at runtime so older
    /// bridges still link.
    const claimOverlay: ((id: string) => void) | undefined;
    /// Release the named view if (and only if) it currently holds the
    /// active overlay slot. Idempotent. Optional at runtime.
    const releaseOverlay: ((id: string) => void) | undefined;

    // ── Keyboard shortcuts (pulp #135 Phase B) ──────────────────────
    /// Register a top-level keyboard shortcut. The platform host
    /// (window_host_mac.mm `performKeyEquivalent:` and friends)
    /// invokes `callbackName` as a global function when the chord
    /// fires. There is no unregister C++-side; the `useShortcut`
    /// hook works around this via a per-chord dispatcher pattern.
    /// Optional at runtime so older bridges still link.
    const registerShortcut: ((keyCode: number, modMask: number, callbackName: string) => void) | undefined;
}

/// Test-only mock-bridge for unit tests. Replaces all the global
/// bridge functions with a recorder that captures calls to a log,
/// so we can assert that the host config emits the right setX
/// sequences without spinning up the full Pulp runtime.
export interface MockBridgeCall {
    fn: string;
    args: unknown[];
}

export interface MockBridge {
    calls: MockBridgeCall[];
    install(): void;
    uninstall(): void;
    reset(): void;
}

export function createMockBridge(): MockBridge {
    const calls: MockBridgeCall[] = [];
    const fns = [
        'createCol', 'createRow', 'createPanel', 'createLabel', 'createButton',
        'createKnob', 'createFader', 'createSpectrum', 'createWaveform', 'createCanvas',
        'createCheckbox', 'createToggle', 'createToggleButton', 'createCombo',
        'createListBox', 'createModal', 'createTextEditor', 'createScrollView',
        'createImage', 'createIcon', 'createProgress', 'createMeter', 'createXYPad',
        'createGrid', 'removeWidget', 'moveWidget', 'insertChild',
        'setFlex', 'setBackground', 'setBackgroundGradient', 'setBorder',
        // pulp #1517 — background sub-properties (mostly noop today).
        'setBackgroundAttachment', 'setBackgroundClip', 'setBackgroundOrigin',
        // pulp #1027 — per-attribute border setters needed for the audit
        // PR #1166 finding-#4 fix (preserve unset siblings).
        'setBorderColor', 'setBorderWidth', 'setBorderRadius', 'setBorderStyle',
        // pulp #1514 — list-style cluster mock-bridge fns. The bridge
        // stores the value on the View; paint-time marker rendering
        // is deferred (catalog: `partial`).
        'setListStyleType', 'setListStyleImage', 'setListStylePosition',
        'setBorderTopColor', 'setBorderRightColor',
        'setBorderBottomColor', 'setBorderLeftColor',
        'setBorderTopWidth', 'setBorderRightWidth',
        'setBorderBottomWidth', 'setBorderLeftWidth',
        'setBorderTopLeftRadius', 'setBorderTopRightRadius',
        'setBorderBottomLeftRadius', 'setBorderBottomRightRadius',
        // pulp #1519 — RN outline cluster (paint-time ring outside the
        // border-box; does not affect Yoga layout).
        'setOutlineColor', 'setOutlineOffset', 'setOutlineStyle', 'setOutlineWidth',
        'setBorderSide', 'setOpacity', 'setVisible', 'setPosition',
        // pulp #1434 Triage #15 — boxShadow surfaced at the @pulp/react
        // layer; mock-bridge captures both setBoxShadow (with the full
        // 7-arg signature) and clearBoxShadow.
        'setBoxShadow', 'clearBoxShadow',
        // pulp #1434 Triage #9 — transform array dispatches a
        // consolidated trio of bridge calls; mock-bridge captures
        // all three so vitest cases can assert on the args + arity.
        'setTranslate', 'setRotation', 'setScale', 'setSkew',
        // pulp #1434 batch 6 — CSS positional setters (top/right/bottom/left)
        // need to be capturable so the percent-string forwarding test
        // can assert on the bridge call shape.
        'setTop', 'setRight', 'setBottom', 'setLeft', 'setZIndex',
        'setText', 'setTextColor', 'setTextAlign',
        // pulp #1552 — line-clamp + webkit-line-clamp + background-repeat.
        // CSS shim and prop-applier both route through these two setters;
        // mock-bridge captures the round-trip so vitest can assert dispatch
        // shape (numeric line count + keyword string).
        'setLineClamp', 'setBackgroundRepeat',
        // pulp #1434 batch 3 — typography keyword translation needs the
        // mock bridge to capture setFontWeight calls so the prop-applier
        // fontWeight test can assert on the numeric value handed off
        // post-translation. Same surface for fontSize / fontStyle /
        // letterSpacing / lineHeight / fontFamily / textTransform /
        // textDecoration which the prop-applier already dispatches.
        'setFontSize', 'setFontWeight', 'setFontStyle', 'setFontFamily',
        'setLetterSpacing', 'setLineHeight',
        'setTextTransform', 'setTextDecoration',
        // pulp #1434 batch 3 — text-decoration longhands.
        'setTextDecorationColor', 'setTextDecorationStyle',
        // pulp #1434 (rn NOT-IMPL bundle 1) — RN textShadow cluster.
        // Each per-attribute setter writes one slot in isolation so
        // a JSX prop diff that touches one preserves the others.
        // Bridge-side registration is staged on the #1548 feature
        // branch; mock-bridge captures the calls so the JSX surface
        // can be tested ahead of the bridge merge.
        'setTextShadowColor', 'setTextShadowOffset', 'setTextShadowRadius',
        // pulp #1434 (rn NOT-IMPL bundle 1) — RN fontVariant. The
        // OpenType feature CSV is forwarded to a planned setFontVariant
        // bridge fn (HarfBuzz hb_feature_t shape-pass wiring deferred).
        // Mock-bridge captures the dispatch shape today.
        'setFontVariant',
        // pulp #1366 / #1434 — backdrop-filter (numeric blur arg).
        'setBackdropFilter',
        // pulp #1434 rn bridge-wires bundle (sub-agent #27 finding) —
        // 7 setX bridge fns now reachable from @pulp/react JSX. The
        // C++ side has had these registered for a while; the gap was
        // purely on the prop-applier dispatch.
        'setBackfaceVisibility', 'setCursor', 'setFilter',
        'setPointerEvents', 'setTransformOrigin', 'setUserSelect',
        // pulp #1549 — RN `mixBlendMode` (New Architecture). Bridge fn
        // wires the View::mix_blend_mode_ slot; paint-time saveLayer
        // composites back with the requested mode.
        'setMixBlendMode',
        'setSpectrumData', 'setWaveformData', 'setMeterLevel', 'setProgress',
        'setValue', 'setTheme', 'layout', 'on', 'registerHover',
        // pulp #1381 — registerPointer arms the bridge's on_pointer_event
        // callback for pointer-down/up/move/cancel events. Wheel goes
        // through a separate registerWheel because the bridge's wheel
        // lambda filters on me.is_wheel (registerPointer's lambda
        // early-returns on is_wheel; registerWheel's on !is_wheel).
        'registerPointer', 'registerWheel',
        // pulp #1387 gap #1 — `overflow` prop missing from prop-applier.
        // DOM-lite path (web-compat-style-decl.js) already routed
        // overflow:hidden to setOverflow, but JSX consumers setting
        // `style={{ overflow: 'hidden' }}` silently dropped it.
        'setOverflow',
        // Spectr import-coverage batch 2 — bridge fns already registered
        // C++-side (widget_bridge.cpp setWhiteSpace at 2588, setTextOverflow
        // at 4902); just needed to surface here so the mock-bridge route
        // captures the applier's calls for unit tests.
        'setWhiteSpace', 'setTextOverflow',
        // pulp #1434 Phase A2-3 — writing direction.
        'setDirection',
        // pulp #1434 Phase A2-1 — transitions + animations.
        'setTransition', 'setTransitionProperty', 'setTransitionDuration',
        'setTransitionDelay', 'setTransitionTimingFunction',
        'defineKeyframes', 'setAnimation',
        // pulp #1516 — CSS box-sizing keyword (content-box / border-box).
        'setBoxSizing',
        // pulp #1434 Phase A2-2 — CSS Grid bridge surface.
        'setGrid',
        // pulp #1515 — CSS clip-path / mask cluster.
        'setClipPath', 'setMaskImage', 'setMask',
        // pulp #994 — SvgPath intrinsic surface
        'createSvgPath', 'setSvgPath', 'setSvgViewBox',
        'setSvgFill', 'setSvgStroke', 'setSvgStrokeWidth',
        // pulp #1416 — SvgRect + SvgLine intrinsic surface
        'createSvgRect', 'setSvgRect',
        'createSvgLine', 'setSvgLine',
        // pulp #1148 — generalized overlay-click routing
        'claimOverlay', 'releaseOverlay',
        // pulp #135 Phase B — runtime keyboard shortcut injection.
        // C++ surface (widget_bridge.cpp `registerShortcut`):
        //   registerShortcut(keyCode: int, modMask: int, callbackName: string)
        // useShortcut hook in shortcuts.ts wraps this with a dispatcher
        // pattern so unregistration works without a bridge change.
        'registerShortcut',
        // pulp #1899 (gap #3) — string-token bridge fns. setStringToken
        // writes theme.strings[name]; getStringToken reads it back. The
        // prop-applier _resolveVar helper consults getStringToken to
        // resolve var(--mono) etc. in fontFamily / color / borderColor
        // before forwarding to the bridge.
        'setStringToken', 'getStringToken',
    ];
    const saved: Record<string, unknown> = {};
    return {
        calls,
        install() {
            for (const fn of fns) {
                saved[fn] = (globalThis as Record<string, unknown>)[fn];
                (globalThis as Record<string, unknown>)[fn] =
                    (...args: unknown[]) => { calls.push({ fn, args }); };
            }
        },
        uninstall() {
            for (const fn of fns) {
                (globalThis as Record<string, unknown>)[fn] = saved[fn];
            }
        },
        reset() { calls.length = 0; },
    };
}
