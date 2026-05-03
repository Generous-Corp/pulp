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
            | 'justify_content',
        value: number | string,
    ): void;

    // ── Visual style ────────────────────────────────────────────────
    function setBackground(id: string, hexColor: string): void;
    function setBackgroundGradient(id: string, css: string): void;
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
    function setOpacity(id: string, alpha: number): void;
    function setVisible(id: string, visible: boolean): void;
    function setPosition(id: string, top: number, left: number, right?: number, bottom?: number): void;

    // ── Text ────────────────────────────────────────────────────────
    function setText(id: string, text: string): void;
    function setTextColor(id: string, hexColor: string): void;
    function setTextAlign(id: string, align: 'left' | 'center' | 'right'): void;

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

    // ── Overlay click routing (pulp #1148) ──────────────────────────
    /// Claim the view as the active click-eligible overlay so the
    /// platform window host short-circuits hit-testing for clicks that
    /// land inside the view's bounds. Optional at runtime so older
    /// bridges still link.
    const claimOverlay: ((id: string) => void) | undefined;
    /// Release the named view if (and only if) it currently holds the
    /// active overlay slot. Idempotent. Optional at runtime.
    const releaseOverlay: ((id: string) => void) | undefined;
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
        // pulp #1027 — per-attribute border setters needed for the audit
        // PR #1166 finding-#4 fix (preserve unset siblings).
        'setBorderColor', 'setBorderWidth', 'setBorderRadius',
        'setBorderTopColor', 'setBorderRightColor',
        'setBorderBottomColor', 'setBorderLeftColor',
        'setBorderTopWidth', 'setBorderRightWidth',
        'setBorderBottomWidth', 'setBorderLeftWidth',
        'setBorderTopLeftRadius', 'setBorderTopRightRadius',
        'setBorderBottomLeftRadius', 'setBorderBottomRightRadius',
        'setBorderSide', 'setOpacity', 'setVisible', 'setPosition',
        'setText', 'setTextColor', 'setTextAlign',
        'setSpectrumData', 'setWaveformData', 'setMeterLevel', 'setProgress',
        'setValue', 'setTheme', 'layout', 'on', 'registerHover',
        // pulp #994 — SvgPath intrinsic surface
        'createSvgPath', 'setSvgPath', 'setSvgViewBox',
        'setSvgFill', 'setSvgStroke', 'setSvgStrokeWidth',
        // pulp #1148 — generalized overlay-click routing
        'claimOverlay', 'releaseOverlay',
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
