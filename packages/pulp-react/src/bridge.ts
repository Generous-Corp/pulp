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
    // Ink & Signal design-system widgets.
    function createBadge(id: string, text: string, tone: string, parentId: string): void;
    function createStepper(id: string, parentId: string): void;
    function createPan(id: string, parentId: string): void;

    // ── Widget mutation ─────────────────────────────────────────────
    function removeWidget(id: string): void;
    /// Move an existing widget to a new parent at the given index.
    /// This is the non-DOM-coupled alternative to the __domAppend
    /// reparent path in core/view/src/widget_bridge.cpp.
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
            // Width/height ratio. Value is a finite positive number;
            // 0 / non-finite clears the slot on the bridge side.
            | 'aspect_ratio',
        value: number | string,
    ): void;

    // ── Visual style ────────────────────────────────────────────────
    function setBackground(id: string, hexColor: string): void;
    function setBackgroundGradient(id: string, css: string): void;
    // Background sub-properties are stored on the View; paint support is partial.
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
    // Per-attribute border setters preserve unset siblings. The unified
    // `setBorder` clobbers all three slots; these mutate one field at a
    // time on the View. Optional at runtime so older bridges still link,
    // hence the `const | undefined` shape.
    const setBorderColor: ((id: string, hexColor: string) => void) | undefined;
    const setBorderWidth: ((id: string, width: number) => void) | undefined;
    const setBorderRadius: ((id: string, radius: number) => void) | undefined;
    // Border-style keywords map to View::BorderStyle; Skia installs
    // SkDashPathEffect for dashed/dotted at stroke time. Other named
    // styles degrade to solid.
    const setBorderStyle: ((id: string, style: string) => void) | undefined;
    /// Writing direction maps to View::WritingDirection; Yoga + Skia
    /// honor it at layout / text shape time.
    const setDirection: ((id: string, dir: 'ltr' | 'rtl' | 'inherit' | string) => void) | undefined;
    // List-style values are stored verbatim on the View. Pulp does not
    // model <li>/<ul>/<ol> semantics yet, so marker glyph rendering is
    // partial: stored, not painted.
    const setListStyleType: ((id: string, type: string) => void) | undefined;
    const setListStyleImage: ((id: string, url: string) => void) | undefined;
    const setListStylePosition: ((id: string, pos: string) => void) | undefined;
    /// CSS Grid bridge surface. The C++ side parses template-track
    /// strings, named-area strings, and the grid-area shorthand
    /// (named token vs `row / col / row / col` numeric form).
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
    // Outline paints outside the border box and does not take Yoga layout
    // space. Style keywords mirror setBorderStyle.
    const setOutlineColor: ((id: string, hexColor: string) => void) | undefined;
    const setOutlineOffset: ((id: string, offsetPx: number) => void) | undefined;
    const setOutlineStyle: ((id: string, style: string) => void) | undefined;
    const setOutlineWidth: ((id: string, widthPx: number) => void) | undefined;
    function setOpacity(id: string, alpha: number): void;
    function setVisible(id: string, visible: boolean): void;
    /// CSS transitions and animations. `setTransition` parses the full
    /// shorthand; the longhand setters apply uniformly across the parsed list.
    const setTransition: ((id: string, css: string) => void) | undefined;
    const setTransitionProperty: ((id: string, props: string) => void) | undefined;
    const setTransitionDuration: ((id: string, seconds: number) => void) | undefined;
    const setTransitionDelay: ((id: string, seconds: number) => void) | undefined;
    const setTransitionTimingFunction: ((id: string, easing: string) => void) | undefined;
    /// `defineKeyframes` populates the application-wide keyframes
    /// registry consumed by the animation bridge.
    const defineKeyframes: ((name: string, stops_json: string) => void) | undefined;
    const setAnimation: ((id: string, name: string, duration: number, iterations: number, direction: string) => void) | undefined;
    function setPosition(id: string, top: number, left: number, right?: number, bottom?: number): void;
    // setBoxShadow / clearBoxShadow surface View::set_box_shadow at the
    // TS layer. The prop-applier dispatches one setBoxShadow per shadow
    // for comma-separated CSS strings and RN BoxShadowValue[] arrays;
    // array form clears first to avoid cross-render accumulation.
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

    // RN-style transform arrays dispatch as consolidated bridge calls.
    // setTranslate takes both axes at once (no axis clobber), setRotation
    // uses degrees, and setScale is uniform-only; independent scaleX/scaleY
    // remains a bridge-side gap.
    function setTranslate(id: string, x: number, y: number): void;
    function setRotation(id: string, degrees: number): void;
    function setScale(id: string, scale: number): void;
    // setSkew dispatches both axes at once so skewX/skewY reach the View
    // through one consolidated bridge call.
    function setSkew(id: string, x_deg: number, y_deg: number): void;

    // ── Text ────────────────────────────────────────────────────────
    function setText(id: string, text: string): void;
    function setTextColor(id: string, hexColor: string): void;
    function setTextAlign(id: string, align: 'left' | 'center' | 'right'): void;

    // setLineClamp clamps a multi-line Label to N visible lines (0
    // disables; >=1 enables wrap implicitly on the bridge side).
    // setBackgroundRepeat is stored on the View; paint support is tied
    // to background-image / repeating-gradient handling. Optional at
    // runtime so older bridges still link.
    const setLineClamp: ((id: string, n: number) => void) | undefined;
    const setBackgroundRepeat: ((id: string, kw: string) => void) | undefined;

    // ── Widget-specific data ────────────────────────────────────────
    function setSpectrumData(id: string, samples: number[] | Float32Array): void;
    function setWaveformData(id: string, samples: number[] | Float32Array): void;
    function setMeterLevel(id: string, level: number): void;
    function setProgress(id: string, fraction: number): void;
    function setValue(id: string, value: number): void;

    // ── Declarative param/meter bindings (no per-frame JS) ───────────
    /// Remaps a bound param before it reaches the widget. Applied in order:
    /// dB→linear map → scale → offset → clamp. Absent → identity on the
    /// store's normalized [0,1] value.
    interface BindingTransform {
        db?: boolean; dbMin?: number; dbMax?: number;
        scale?: number; offset?: number;
        min?: number; max?: number; clamp?: boolean;
    }
    /// Bind a value widget (knob/fader/slider/toggle/progress) to a param.
    /// Registered once; C++ pushes the store value each frame with zero
    /// per-frame JS crossing. Returns true when the param exists.
    const bindWidgetToParam:
        ((widgetId: string, paramName: string, transform?: BindingTransform) => boolean) | undefined;
    /// Bind a Meter's fill to a param (drives rms + peak). Same contract.
    const bindMeter:
        ((widgetId: string, paramName: string, transform?: BindingTransform) => boolean) | undefined;
    /// Remove the binding(s) for a widget. Returns the number removed.
    const unbindWidget: ((widgetId: string) => number) | undefined;
    /// `<Image src>` forwards to ImageView::set_image_path via
    /// WidgetBridge::register_widget_assets_api
    /// (core/view/src/widget_bridge/widget_assets_api.cpp). The path is
    /// forwarded verbatim; absolute paths come from importers and C++
    /// resolves the rest.
    function setImageSource(id: string, path: string): void;

    // ── Theme ───────────────────────────────────────────────────────
    function setTheme(name: 'dark' | 'light' | 'pro_audio' | string): void;

    // ── Layout flush + frame service ────────────────────────────────
    /// Force a layout pass on the root container. Used in
    /// `resetAfterCommit` so the host config owns commit-time flush.
    /// Declared as a const so consumers can branch on `typeof layout`.
    const layout: (() => void) | undefined;

    // ── CSS clip-path / mask cluster ────────────────────────────────
    /// CSS `clip-path: path("...")`. Bridge accepts the SVG-path-d
    /// string; Skia parses via `SkPath::FromSVGString` and installs
    /// it as the canvas clip before children paint. URL refs and
    /// named shape forms are deferred. Optional at runtime so older
    /// bridges still link.
    const setClipPath: ((id: string, svgPathD: string) => void) | undefined;
    /// CSS `mask-image`. Stored on the View; shader composite paint is
    /// handled separately. Optional at runtime.
    const setMaskImage: ((id: string, value: string) => void) | undefined;
    /// CSS `mask` shorthand. Stored verbatim alongside any separately
    /// dispatched `maskImage` longhand. Optional at runtime.
    const setMask: ((id: string, shorthand: string) => void) | undefined;

    // ── Overlay click routing ────────────────────────────────────────
    /// Claim the view as the active click-eligible overlay so the
    /// platform window host short-circuits hit-testing for clicks that
    /// land inside the view's bounds. Optional at runtime so older
    /// bridges still link.
    const claimOverlay: ((id: string) => void) | undefined;
    /// Release the named view if (and only if) it currently holds the
    /// active overlay slot. Idempotent. Optional at runtime.
    const releaseOverlay: ((id: string) => void) | undefined;

    // ── Keyboard shortcuts ───────────────────────────────────────────
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
        'createGrid',
        // Ink & Signal design-system widgets.
        'createBadge', 'createStepper', 'createPan',
        'removeWidget', 'moveWidget', 'insertChild',
        'setFlex', 'setBackground', 'setBackgroundGradient', 'setBorder',
        // Background sub-properties are stored on the View.
        'setBackgroundAttachment', 'setBackgroundClip', 'setBackgroundOrigin',
        // Per-attribute border setters preserve unset siblings.
        'setBorderColor', 'setBorderWidth', 'setBorderRadius', 'setBorderStyle',
        // List-style bridge calls store values on the View; marker
        // rendering is still partial.
        'setListStyleType', 'setListStyleImage', 'setListStylePosition',
        'setBorderTopColor', 'setBorderRightColor',
        'setBorderBottomColor', 'setBorderLeftColor',
        'setBorderTopWidth', 'setBorderRightWidth',
        'setBorderBottomWidth', 'setBorderLeftWidth',
        'setBorderTopLeftRadius', 'setBorderTopRightRadius',
        'setBorderBottomLeftRadius', 'setBorderBottomRightRadius',
        // Outline paints outside the border box and does not affect Yoga layout.
        'setOutlineColor', 'setOutlineOffset', 'setOutlineStyle', 'setOutlineWidth',
        'setBorderSide', 'setOpacity', 'setVisible', 'setPosition',
        // Mock-bridge captures both setBoxShadow (with the full 7-arg
        // signature) and clearBoxShadow.
        'setBoxShadow', 'clearBoxShadow',
        // Transform array dispatches consolidated bridge calls; the
        // recorder captures each axis group so tests can assert args and arity.
        'setTranslate', 'setRotation', 'setScale', 'setSkew',
        // CSS positional setters need to be capturable so percent-string
        // forwarding tests can assert the bridge call shape.
        'setTop', 'setRight', 'setBottom', 'setLeft', 'setZIndex',
        'setText', 'setTextColor', 'setTextAlign',
        // CSS shim and prop-applier both route line-clamp and
        // background-repeat through these setters; tests assert numeric
        // line count and keyword string dispatch.
        'setLineClamp', 'setBackgroundRepeat',
        // Typography tests need the mock bridge to capture translated
        // fontWeight values plus the remaining text bridge calls.
        'setFontSize', 'setFontWeight', 'setFontStyle', 'setFontFamily',
        'setLetterSpacing', 'setLineHeight',
        'setTextTransform', 'setTextDecoration',
        // Text-decoration longhands.
        'setTextDecorationColor', 'setTextDecorationStyle',
        // RN textShadow longhands write one slot in isolation so a JSX
        // prop diff that touches one preserves the others.
        'setTextShadowColor', 'setTextShadowOffset', 'setTextShadowRadius',
        // RN fontVariant forwards the OpenType feature CSV to setFontVariant.
        'setFontVariant',
        // Backdrop-filter uses a numeric blur argument.
        'setBackdropFilter',
        // RN bridge wires reachable from @pulp/react JSX.
        'setBackfaceVisibility', 'setCursor', 'setFilter',
        'setPointerEvents', 'setTransformOrigin', 'setUserSelect',
        // RN mixBlendMode wires the View::mix_blend_mode_ slot; paint-time
        // saveLayer composites back with the requested mode.
        'setMixBlendMode',
        'setSpectrumData', 'setWaveformData', 'setMeterLevel', 'setProgress',
        'setValue',
        // `<Image src>` forwards to ImageView via setImageSource; the
        // recorder captures the call for prop-applier tests.
        'setImageSource',
        'setTheme', 'layout', 'on', 'registerHover',
        // registerPointer arms pointer-down/up/move/cancel events. Wheel
        // goes through registerWheel because each bridge callback filters
        // on the wheel flag in the opposite direction.
        'registerPointer', 'registerWheel',
        // Overflow is routed through setOverflow for JSX style props.
        'setOverflow',
        // These bridge fns are registered C++-side; the mock bridge
        // captures the applier's calls for unit tests.
        'setWhiteSpace', 'setTextOverflow',
        // Writing direction.
        'setDirection',
        // Transitions and animations.
        'setTransition', 'setTransitionProperty', 'setTransitionDuration',
        'setTransitionDelay', 'setTransitionTimingFunction',
        'defineKeyframes', 'setAnimation',
        // CSS box-sizing keyword (content-box / border-box).
        'setBoxSizing',
        // CSS Grid bridge surface.
        'setGrid',
        // CSS clip-path / mask cluster.
        'setClipPath', 'setMaskImage', 'setMask',
        // SvgPath intrinsic surface.
        'createSvgPath', 'setSvgPath', 'setSvgViewBox',
        'setSvgFill', 'setSvgFillRule', 'setSvgFillGradient',
        'setSvgStroke', 'setSvgStrokeWidth',
        // SvgRect + SvgLine intrinsic surface.
        'createSvgRect', 'setSvgRect',
        'createSvgLine', 'setSvgLine',
        // Generalized overlay-click routing.
        'claimOverlay', 'releaseOverlay',
        // Runtime keyboard shortcut injection.
        // C++ surface (widget_bridge.cpp `registerShortcut`):
        //   registerShortcut(keyCode: int, modMask: int, callbackName: string)
        // useShortcut hook in shortcuts.ts wraps this with a dispatcher
        // pattern so unregistration works without a bridge change.
        'registerShortcut',
        // setStringToken writes theme.strings[name]; getStringToken reads
        // it back so prop-applier token resolution can run before forwarding
        // fontFamily / color / borderColor values to the bridge.
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
