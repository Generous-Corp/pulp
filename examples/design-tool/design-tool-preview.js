
var previewChromeDots = ["#ff5f57", "#febc2e", "#28c840"];
function buildPreviewChrome() {
    for (var pcd = 0; pcd < previewChromeDots.length; pcd++) {
        var dotId = "preview-chrome-dot-" + pcd;
        createCol(dotId, "preview-chrome");
        setFlex(dotId, "width", 8);
        setFlex(dotId, "height", 8);
        setBackground(dotId, previewChromeDots[pcd]);
        setBorder(dotId, previewChromeDots[pcd], 0, 4);
    }

    createLabel("preview-chrome-title", "Plugin Preview", "preview-chrome");
    setFontSize("preview-chrome-title", 10);
    setTextColor("preview-chrome-title", APP_TEXT_DIM);
    setFlex("preview-chrome-title", "padding_left", 4);

    createCol("preview-chrome-divider", "preview-shell");
    setFlex("preview-chrome-divider", "height", 1);
    setBackground("preview-chrome-divider", APP_BORDER);

    // Preview content area (scrollable, inside a plugin-style shell)
    createScrollView("preview-scroll", "preview-shell");
    setFlex("preview-scroll", "flex_grow", 1);
    setBackground("preview-scroll", APP_BG);
    setBorder("preview-scroll", APP_BORDER, 0, 0);
    setScrollContentSize("preview-scroll", 500, 1900);

    createCol("preview-area", "preview-scroll");
    setFlex("preview-area", "height", 1900);
    setFlex("preview-area", "flex_shrink", 0);
    setFlex("preview-area", "padding", 14);
    setFlex("preview-area", "padding_right", 28);  // extra space for scrollbar
    setFlex("preview-area", "gap", 10);
}
buildPreviewChrome();

function stylePreviewSectionHeader(id) {
    setFontSize(id, 10);
    setLetterSpacing(id, 1.2);
    setTextColor(id, APP_TEXT_DIM);
    setFlex(id, "height", 14);
}

// Foundations section: bg swatches + text hierarchy
function buildPreviewFoundationsHeader() {
    createLabel("foundations-header", "FOUNDATIONS", "preview-area");
    stylePreviewSectionHeader("foundations-header");

    // Background swatches row
    createRow("bg-swatches", "preview-area");
    setFlex("bg-swatches", "gap", 6);
    setFlex("bg-swatches", "height", 32);
    setFlex("bg-swatches", "align_items", "center");
}
buildPreviewFoundationsHeader();

var bgTokens = ["bg.primary", "bg.secondary", "bg.surface", "bg.elevated"];
var bgLabels = ["Primary", "Secondary", "Surface", "Elevated"];
function buildPreviewControls() {
    for (var bi = 0; bi < bgTokens.length; bi++) {
        var bsId = "bg-sw-" + bi;
        createCol(bsId, "bg-swatches");
        setFlex(bsId, "flex_grow", 1);
        setFlex(bsId, "height", 28);
        setBorder(bsId, APP_BORDER, 1, 4);
        setFlex(bsId, "padding", 4);
        setFlex(bsId, "justify_content", "center");
        // Color from theme
        var themeColors = JSON.parse(getThemeJson()).colors || {};
        if (themeColors[bgTokens[bi]]) setBackground(bsId, themeColors[bgTokens[bi]]);
        createLabel(bsId + "-l", bgLabels[bi], bsId);
        setFontSize(bsId + "-l", 8);
        setFlex(bsId + "-l", "height", 10);
    }

    // Text hierarchy row
    createRow("text-hierarchy", "preview-area");
    setFlex("text-hierarchy", "gap", 16);
    setFlex("text-hierarchy", "height", 24);
    setFlex("text-hierarchy", "align_items", "center");

    createLabel("th-heading", "Heading", "text-hierarchy");
    setFontSize("th-heading", 16);
    setFlex("th-heading", "width", 80);

    createLabel("th-body", "Body text", "text-hierarchy");
    setFontSize("th-body", 12);
    setFlex("th-body", "width", 80);

    createLabel("th-caption", "Caption", "text-hierarchy");
    setFontSize("th-caption", 9);
    setTextColor("th-caption", APP_TEXT_DIM);
    setFlex("th-caption", "width", 60);

    // Controls section: knobs
    createLabel("controls-header", "CONTROLS", "preview-area");
    stylePreviewSectionHeader("controls-header");

    createRow("knob-row", "preview-area");
    setFlex("knob-row", "gap", 16);
    setFlex("knob-row", "height", 64);
    setFlex("knob-row", "align_items", "center");

    createKnob("k1", "knob-row");
    setLabel("k1", "Gain");
    setFlex("k1", "width", 56);
    setFlex("k1", "height", 64);
    setValue("k1", 0.4);

    createKnob("k2", "knob-row");
    setLabel("k2", "Freq");
    setFlex("k2", "width", 56);
    setFlex("k2", "height", 64);
    setValue("k2", 0.6);

    createKnob("k3", "knob-row");
    setLabel("k3", "Res");
    setFlex("k3", "width", 56);
    setFlex("k3", "height", 64);
    setValue("k3", 0.8);

    createKnob("k4", "knob-row");
    setLabel("k4", "Mix");
    setFlex("k4", "width", 56);
    setFlex("k4", "height", 64);
    setValue("k4", 1.0);

    // Fader
    createFader("slider1", "horizontal", "preview-area");
    setFlex("slider1", "height", 24);
    setValue("slider1", 0.65);

    // Buttons row (Normal, Hover, Action, Disabled)
    createRow("btn-row", "preview-area");
    setFlex("btn-row", "gap", 8);
    setFlex("btn-row", "height", 32);
    setFlex("btn-row", "align_items", "center");

    createCol("btn-normal", "btn-row");
    setFlex("btn-normal", "flex_grow", 1);
    setFlex("btn-normal", "height", 28);
    setBackground("btn-normal", "#3a3a4c");
    setBorder("btn-normal", APP_BORDER, 1, 6);
    setFlex("btn-normal", "justify_content", "center");
    setFlex("btn-normal", "align_items", "center");
    createLabel("btn-normal-label", "Normal", "btn-normal");
    setFontSize("btn-normal-label", 11);
    setFlex("btn-normal-label", "height", 16);

    createCol("btn-hover", "btn-row");
    setFlex("btn-hover", "flex_grow", 1);
    setFlex("btn-hover", "height", 28);
    setBackground("btn-hover", "#4a4a5c");
    setBorder("btn-hover", APP_BORDER, 1, 6);
    setFlex("btn-hover", "justify_content", "center");
    setFlex("btn-hover", "align_items", "center");
    createLabel("btn-hover-label", "Hover", "btn-hover");
    setFontSize("btn-hover-label", 11);
    setFlex("btn-hover-label", "height", 16);

    createCol("btn-action", "btn-row");
    setFlex("btn-action", "flex_grow", 1);
    setFlex("btn-action", "height", 28);
    setBackground("btn-action", APP_ACCENT);
    setBorder("btn-action", APP_ACCENT, 0, 6);
    setFlex("btn-action", "justify_content", "center");
    setFlex("btn-action", "align_items", "center");
    createLabel("btn-action-label", "Action", "btn-action");
    setFontSize("btn-action-label", 11);
    setFlex("btn-action-label", "height", 16);

    createCol("btn-disabled", "btn-row");
    setFlex("btn-disabled", "flex_grow", 1);
    setFlex("btn-disabled", "height", 28);
    setBackground("btn-disabled", "#2a2a36");
    setBorder("btn-disabled", APP_BORDER, 1, 6);
    setOpacity("btn-disabled", 0.5);
    setFlex("btn-disabled", "justify_content", "center");
    setFlex("btn-disabled", "align_items", "center");
    createLabel("btn-disabled-label", "Disabled", "btn-disabled");
    setFontSize("btn-disabled-label", 11);
    setFlex("btn-disabled-label", "height", 16);
    setTextColor("btn-disabled-label", APP_TEXT_DIM);

    // Toggles + checkbox row
    createRow("toggle-row", "preview-area");
    setFlex("toggle-row", "gap", 10);
    setFlex("toggle-row", "height", 28);
    setFlex("toggle-row", "align_items", "center");

    createToggle("t1", "toggle-row");
    setFlex("t1", "width", 36);
    setFlex("t1", "height", 20);
    setValue("t1", 1);

    createLabel("toggle-on-label", "Enabled", "toggle-row");
    setFontSize("toggle-on-label", 11);
    setFlex("toggle-on-label", "width", 50);

    createToggle("t2", "toggle-row");
    setFlex("t2", "width", 36);
    setFlex("t2", "height", 20);

    createLabel("toggle-off-label", "Disabled", "toggle-row");
    setFontSize("toggle-off-label", 11);
    setFlex("toggle-off-label", "width", 52);

    // Checkbox
    createCheckbox("cb1", "toggle-row");
    setFlex("cb1", "width", 22);
    setFlex("cb1", "height", 22);
    setValue("cb1", 1);

    // Toggle button row
    createRow("toggle-btn-row", "preview-area");
    setFlex("toggle-btn-row", "height", 32);
    setFlex("toggle-btn-row", "gap", 8);

    createToggleButton("tb1", "toggle-btn-row");
    setLabel("tb1", "Toggle");
    setFlex("tb1", "flex_grow", 1);
    setFlex("tb1", "height", 28);

    // Text input + Placeholder + Combo preview
    createRow("input-row", "preview-area");
    setFlex("input-row", "gap", 8);
    setFlex("input-row", "height", 30);
    setFlex("input-row", "align_items", "center");

    createTextEditor("sample-input", "input-row");
    setPlaceholder("sample-input", "Some text");
    setText("sample-input", "Some text");
    setFlex("sample-input", "width", 120);
    setFlex("sample-input", "height", 26);
    setTextColor("sample-input", APP_TEXT);

    createTextEditor("sample-placeholder", "input-row");
    setPlaceholder("sample-placeholder", "Placeholder...");
    setFlex("sample-placeholder", "width", 120);
    setFlex("sample-placeholder", "height", 26);
    setTextColor("sample-placeholder", APP_TEXT);

    createRow("sample-combo", "input-row");
    setFlex("sample-combo", "width", 148);
    setFlex("sample-combo", "height", 26);
    setFlex("sample-combo", "padding_left", 8);
    setFlex("sample-combo", "padding_right", 8);
    setFlex("sample-combo", "align_items", "center");
    setFlex("sample-combo", "gap", 8);
    setBackground("sample-combo", APP_PANEL);
    setBorder("sample-combo", APP_BORDER, 1, 6);
    createLabel("sample-combo-label", "Select preset...", "sample-combo");
    setFontSize("sample-combo-label", 10);
    setFlex("sample-combo-label", "flex_grow", 1);
    setTextColor("sample-combo-label", APP_TEXT);
    setPointerEvents("sample-combo-label", "none");
    createLabel("sample-combo-caret", "\u25be", "sample-combo");
    setFontSize("sample-combo-caret", 10);
    setTextColor("sample-combo-caret", APP_TEXT_DIM);
    setPointerEvents("sample-combo-caret", "none");

    // Data display: Waveform
    createLabel("data-header", "DATA DISPLAY", "preview-area");
    stylePreviewSectionHeader("data-header");

    createWaveform("waveform", "preview-area");
    setFlex("waveform", "height", 60);
}
buildPreviewControls();

var waveData = [];
function buildPreviewWaveformAndMeters() {
    for (var i = 0; i < 512; i++) {
        var phase = i / 512;
        var envelope = 0.74 + Math.sin(phase * Math.PI * 6) * 0.08;
        waveData.push((Math.sin(2 * Math.PI * 3 * phase) * 0.48 +
                       Math.sin(2 * Math.PI * 9 * phase) * 0.20 +
                       Math.sin(2 * Math.PI * 17 * phase) * 0.08) * envelope);
    }
    setWaveformData("waveform", waveData);

    // Meters
    createRow("meter-row", "preview-area");
    setFlex("meter-row", "gap", 4);
    setFlex("meter-row", "height", 52);

    createMeter("m1", "vertical", "meter-row");
    setFlex("m1", "width", 14);
    setWidgetStyle("m1", "minimal");
    setMeterLevel("m1", 0.75, 0.88);

    createMeter("m2", "vertical", "meter-row");
    setFlex("m2", "width", 14);
    setWidgetStyle("m2", "minimal");
    setMeterLevel("m2", 0.55, 0.72);

    createMeter("m3", "vertical", "meter-row");
    setFlex("m3", "width", 14);
    setWidgetStyle("m3", "minimal");
    setMeterLevel("m3", 0.3, 0.45);

    createMeter("m4", "vertical", "meter-row");
    setFlex("m4", "width", 14);
    setWidgetStyle("m4", "minimal");
    setMeterLevel("m4", 0.85, 0.95);

    // Layout section: single-row status cards matching the HTML reference
    createRow("layout-header-row", "preview-area");
    setFlex("layout-header-row", "height", 16);
    setFlex("layout-header-row", "gap", 8);
    setFlex("layout-header-row", "align_items", "center");

    createLabel("layout-header", "LAYOUT", "layout-header-row");
    setFontSize("layout-header", 10);
    setLetterSpacing("layout-header", 1.2);
    setTextColor("layout-header", APP_TEXT_DIM);
    setFlex("layout-header", "height", 14);
    setFlex("layout-header", "width", 52);
    setFlex("layout-header", "flex_shrink", 0);

    createCol("layout-header-line", "layout-header-row");
    setFlex("layout-header-line", "height", 1);
    setFlex("layout-header-line", "flex_grow", 1);
    setBackground("layout-header-line", previewThemeColor("divider", APP_BORDER));
}
buildPreviewWaveformAndMeters();

// D3: Card grid matching HTML reference — Empty, Loading, Ready (OK badge), Error (! badge)
var cardDefs = [
    { id: "card-1", label: "Empty", bg: APP_PANEL, border: APP_BORDER, badge: null },
    { id: "card-2", label: "Loading", bg: APP_PANEL, border: APP_BORDER, badge: null, loading: true },
    { id: "card-3", label: "Ready", bg: APP_PANEL, border: '#4CAF50', badge: "OK", badgeColor: '#4CAF50' },
    { id: "card-4", label: "Error", bg: '#3a2020', border: '#e94560', badge: "!", badgeColor: '#e94560' }
];
function buildPreviewLayoutCards() {
    createRow("card-grid-row", "preview-area");
    setFlex("card-grid-row", "gap", 6);
    setFlex("card-grid-row", "height", 56);
    setFlex("card-grid-row", "align_items", "stretch");
    setFlex("card-grid-row", "flex_wrap", 0);

    for (var ci = 0; ci < cardDefs.length; ci++) {
        var cd = cardDefs[ci];
        createCol(cd.id, "card-grid-row");
        setPosition(cd.id, "relative");
        setFlex(cd.id, "flex_grow", 1);
        setFlex(cd.id, "flex_basis", 0);
        setFlex(cd.id, "min_width", 0);
        setBackground(cd.id, cd.bg);
        setBorder(cd.id, cd.border, 1, 8);
        setFlex(cd.id, "padding", 6);
        setFlex(cd.id, "justify_content", "center");
        setFlex(cd.id, "align_items", "center");

        if (cd.loading) {
            createLabel(cd.id + "-label", cd.label, cd.id);
            setPosition(cd.id + "-label", "absolute");
            setTop(cd.id + "-label", 8);
            setLeft(cd.id + "-label", 10);
            setFontSize(cd.id + "-label", 9);
            setTextColor(cd.id + "-label", APP_TEXT_DIM);

            createLabel(cd.id + "-spinner", "\u25DC", cd.id);
            setFontSize(cd.id + "-spinner", 15);
            setTextColor(cd.id + "-spinner", APP_ACCENT);
        } else {
            createLabel(cd.id + "-label", cd.label, cd.id);
            setFontSize(cd.id + "-label", 11);
            setTextColor(cd.id + "-label", cd.id === "card-4" ? "#F44336" : APP_TEXT_DIM);
        }

        if (cd.badge) {
            createCol(cd.id + "-badge", cd.id);
            setPosition(cd.id + "-badge", "absolute");
            setTop(cd.id + "-badge", 6);
            setRight(cd.id + "-badge", 6);
            setFlex(cd.id + "-badge", "height", 14);
            setFlex(cd.id + "-badge", "min_width", cd.badge === "OK" ? 18 : 14);
            setFlex(cd.id + "-badge", "max_width", cd.badge === "OK" ? 18 : 14);
            setFlex(cd.id + "-badge", "justify_content", "center");
            setFlex(cd.id + "-badge", "align_items", "center");
            setBackground(cd.id + "-badge", cd.badgeColor);
            setBorder(cd.id + "-badge", cd.badgeColor, 0, 3);
            createLabel(cd.id + "-badge-lbl", cd.badge, cd.id + "-badge");
            setFontSize(cd.id + "-badge-lbl", 7);
            setTextColor(cd.id + "-badge-lbl", "#ffffff");
        }
    }

    // ── Progress + Spinner ───────────────────────────────────────────
    createRow("progress-row", "preview-area");
    setFlex("progress-row", "gap", 12);
    setFlex("progress-row", "height", 24);
    setFlex("progress-row", "align_items", "center");

    createProgress("prog1", "progress-row");
    setFlex("prog1", "flex_grow", 1);
    setFlex("prog1", "height", 6);
    setProgress("prog1", 0.65);

    createRow("spinner-row", "progress-row");
    setFlex("spinner-row", "gap", 8);
    setFlex("spinner-row", "align_items", "center");
    setFlex("spinner-row", "height", 16);

    createLabel("spinner-icon", "\u25E0", "spinner-row");
    setFontSize("spinner-icon", 11);
    setTextColor("spinner-icon", APP_ACCENT);
    setFlex("spinner-icon", "width", 12);

    createLabel("spinner-text", "Loading...", "spinner-row");
    setFontSize("spinner-text", 10);
    setTextColor("spinner-text", APP_TEXT_DIM);
    setFlex("spinner-text", "width", 54);
}
buildPreviewLayoutCards();

// Animate spinner character rotation
var spinnerFrames = ["\u25DC", "\u25DD", "\u25DE", "\u25DF"];
var spinnerIdx = 0;
function tickSpinner() {
    spinnerIdx = (spinnerIdx + 1) % spinnerFrames.length;
    setText("spinner-icon", spinnerFrames[spinnerIdx]);
    setText("card-2-spinner", spinnerFrames[spinnerIdx]);
    __requestFrame__(tickSpinner);
}
function buildPreviewSpinnerAndTabsHeader() {
    __requestFrame__(tickSpinner);

    // ── Tab Bar (General / Audio / MIDI / About) ─────────────────────
    createLabel("tabs-header", "TABS", "preview-area");
    stylePreviewSectionHeader("tabs-header");

    createRow("tab-bar-preview", "preview-area");
    setFlex("tab-bar-preview", "height", 30);
    setFlex("tab-bar-preview", "gap", 0);
    setFlex("tab-bar-preview", "align_items", "stretch");
    setBorder("tab-bar-preview", APP_BORDER, 0, 0);
    createCol("tab-bar-preview-line", "tab-bar-preview");
    setPosition("tab-bar-preview-line", "absolute");
    setLeft("tab-bar-preview-line", 0);
    setRight("tab-bar-preview-line", 0);
    setBottom("tab-bar-preview-line", 0);
    setFlex("tab-bar-preview-line", "height", 1);
    setBackground("tab-bar-preview-line", "transparent");
}
buildPreviewSpinnerAndTabsHeader();

var tabNames = ["General", "Audio", "MIDI", "About"];
function buildPreviewTabBarAndPanel() {
    for (var ti = 0; ti < tabNames.length; ti++) {
        var tabId = "ptab-" + ti;
        createCol(tabId, "tab-bar-preview");
        setPosition(tabId, "relative");
        setFlex(tabId, "height", 30);
        setFlex(tabId, "padding_left", 14);
        setFlex(tabId, "padding_right", 14);
        setFlex(tabId, "justify_content", "center");
        setFlex(tabId, "align_items", "center");
        registerClick(tabId);
        registerHover(tabId);

        createLabel(tabId + "-l", tabNames[ti], tabId);
        setFontSize(tabId + "-l", 12);
        setTextColor(tabId + "-l", ti === 0 ? APP_TEXT : APP_TEXT_DIM);
        setPointerEvents(tabId + "-l", "none");

        createCol(tabId + "-line", tabId);
        setPosition(tabId + "-line", "absolute");
        setLeft(tabId + "-line", 10);
        setRight(tabId + "-line", 10);
        setBottom(tabId + "-line", 0);
        setFlex(tabId + "-line", "height", 2);
        setBackground(tabId + "-line", ti === 0 ? APP_ACCENT : "transparent");

        (function(idx) {
            on("ptab-" + idx, "mouseenter", function() {
                previewTabHoverIndex = idx;
                refreshPreviewTabs(false);
            });
            on("ptab-" + idx, "mouseleave", function() {
                if (previewTabHoverIndex === idx) previewTabHoverIndex = -1;
                refreshPreviewTabs(false);
            });
            on("ptab-" + idx, "click", function() { setPreviewActiveTab(idx); });
        })(ti);
    }

    // Panel content area with divider
    createCol("panel-content", "preview-area");
    setFlex("panel-content", "height", 56);
    setFlex("panel-content", "padding", 8);
    setFlex("panel-content", "gap", 4);
    setBackground("panel-content", APP_PANEL);
    setBorder("panel-content", APP_BORDER, 1, 6);

    createLabel("panel-title", "Panel content area", "panel-content");
    setFontSize("panel-title", 11);

    createCol("panel-divider", "panel-content");
    setFlex("panel-divider", "height", 1);
    setBackground("panel-divider", APP_BORDER);

    createLabel("panel-sub", "With divider and secondary text", "panel-content");
    setFontSize("panel-sub", 10);
    setTextColor("panel-sub", APP_TEXT_DIM);

    previewReady = true;
    applyStateToPreview(activeState);

    // ── Overlays (Static Preview) ────────────────────────────────────
    createLabel("overlays-header", "OVERLAYS (STATIC PREVIEW)", "preview-area");
    stylePreviewSectionHeader("overlays-header");

    createRow("overlay-row", "preview-area");
    setFlex("overlay-row", "gap", 8);
    setFlex("overlay-row", "height", 96);
    setBackground("overlay-row", applyHexAlpha(APP_PANEL_RAISED, 0.82));
    setBorder("overlay-row", APP_BORDER, 1, 8);

    // Confirm Action dialog card
    createCol("dialog-card", "overlay-row");
    setFlex("dialog-card", "flex_grow", 1);
    setFlex("dialog-card", "padding", 8);
    setFlex("dialog-card", "gap", 4);
    setBackground("dialog-card", APP_PANEL);
    setBorder("dialog-card", APP_BORDER, 1, 8);

    createLabel("dialog-title", "Confirm Action", "dialog-card");
    setFontSize("dialog-title", 11);

    createLabel("dialog-msg", "Are you sure you want to delete this preset? This cannot be undone.", "dialog-card");
    setFontSize("dialog-msg", 9);
    setTextColor("dialog-msg", APP_TEXT_DIM);

    createRow("dialog-btns", "dialog-card");
    setFlex("dialog-btns", "gap", 6);
    setFlex("dialog-btns", "height", 22);
    setFlex("dialog-btns", "align_items", "center");
    setFlex("dialog-btns", "justify_content", "flex-end");

    createCol("dialog-cancel", "dialog-btns");
    setFlex("dialog-cancel", "width", 50);
    setFlex("dialog-cancel", "height", 20);
    setBorder("dialog-cancel", APP_BORDER, 1, 4);
    setFlex("dialog-cancel", "justify_content", "center");
    setFlex("dialog-cancel", "align_items", "center");
    createLabel("dialog-cancel-l", "Cancel", "dialog-cancel");
    setFontSize("dialog-cancel-l", 9);

    createCol("dialog-accept", "dialog-btns");
    setFlex("dialog-accept", "width", 50);
    setFlex("dialog-accept", "height", 20);
    setBackground("dialog-accept", APP_ACCENT);
    setBorder("dialog-accept", APP_ACCENT, 0, 4);
    setFlex("dialog-accept", "justify_content", "center");
    setFlex("dialog-accept", "align_items", "center");
    createLabel("dialog-accept-l", "Accept", "dialog-accept");
    setFontSize("dialog-accept-l", 9);

    // Context menu card
    createCol("ctx-menu", "overlay-row");
    setFlex("ctx-menu", "width", 120);
    setFlex("ctx-menu", "padding", 4);
    setFlex("ctx-menu", "gap", 0);
    setBackground("ctx-menu", APP_PANEL);
    setBorder("ctx-menu", APP_BORDER, 1, 6);
}
buildPreviewTabBarAndPanel();

var ctxItems = ["Copy", "Paste", "---", "Rename", "Delete"];
function buildPreviewContextMenu() {
    for (var ci = 0; ci < ctxItems.length; ci++) {
        if (ctxItems[ci] === "---") {
            createCol("ctx-sep-" + ci, "ctx-menu");
            setFlex("ctx-sep-" + ci, "height", 1);
            setBackground("ctx-sep-" + ci, APP_BORDER);
        } else {
            var cid = "ctx-" + ci;
            createRow(cid, "ctx-menu");
            setFlex(cid, "height", 22);
            setFlex(cid, "padding_left", 8);
            setFlex(cid, "align_items", "center");
            createLabel(cid + "-l", ctxItems[ci], cid);
            setFontSize(cid + "-l", 11);
            registerHover(cid);
            if (ctxItems[ci] === "Paste") {
                setBackground(cid, APP_ACCENT + "22");
                setTextColor(cid + "-l", APP_ACCENT);
                (function(id, labelId) {
                    on(id, "mouseenter", function() {
                        setBackground(id, APP_ACCENT + "32");
                        setTextColor(labelId, APP_ACCENT_HOVER);
                    });
                    on(id, "mouseleave", function() {
                        setBackground(id, APP_ACCENT + "22");
                        setTextColor(labelId, APP_ACCENT);
                    });
                })(cid, cid + "-l");
            } else {
                (function(id, labelId, isDelete) {
                    on(id, "mouseenter", function() {
                        setBackground(id, "#ffffff10");
                        if (!isDelete) setTextColor(labelId, APP_TEXT);
                    });
                    on(id, "mouseleave", function() {
                        setBackground(id, "transparent");
                        if (!isDelete) setTextColor(labelId, APP_TEXT_DIM);
                    });
                })(cid, cid + "-l", ctxItems[ci] === "Delete");
            }
            if (ctxItems[ci] === "Delete") setTextColor(cid + "-l", "#f38ba8");
        }
    }

    // ── States ───────────────────────────────────────────────────────
    createLabel("states-header", "STATES", "preview-area");
    stylePreviewSectionHeader("states-header");

    createRow("states-row", "preview-area");
    setFlex("states-row", "gap", 6);
    setFlex("states-row", "height", 26);
    setFlex("states-row", "align_items", "center");
}
buildPreviewContextMenu();

var stateNames = ["Normal", "Hover", "Active", "Focus", "Disabled"];
var stateBgs   = ["#3a3a4c", "#4a4a5c", APP_ACCENT, "#3a3a4c", "#2a2a36"];
function buildPreviewStates() {
    for (var si = 0; si < stateNames.length; si++) {
        var sid = "state-" + si;
        createCol(sid, "states-row");
        setFlex(sid, "flex_grow", 1);
        setFlex(sid, "height", 24);
        setBackground(sid, stateBgs[si]);
        setBorder(sid, si === 3 ? APP_ACCENT : APP_BORDER, 1, 4);
        if (si === 4) setOpacity(sid, 0.5);
        setFlex(sid, "justify_content", "center");
        setFlex(sid, "align_items", "center");
        createLabel(sid + "-l", stateNames[si], sid);
        setFontSize(sid + "-l", 9);
    }

    // ── Effects ──────────────────────────────────────────────────────
    createLabel("effects-header", "EFFECTS", "preview-area");
    stylePreviewSectionHeader("effects-header");

    createRow("effects-row", "preview-area");
    setFlex("effects-row", "gap", 8);
    setFlex("effects-row", "height", 44);
    setFlex("effects-row", "align_items", "center");
}
buildPreviewStates();

var effectNames = ["Shadow", "Glow", "Blur", "Gradient"];
function buildPreviewEffects() {
    for (var ei = 0; ei < effectNames.length; ei++) {
        var eid = "effect-" + ei;
        createCol(eid, "effects-row");
        setFlex(eid, "flex_grow", 1);
        setFlex(eid, "height", 40);
        setFlex(eid, "padding_top", 5);
        setFlex(eid, "padding_bottom", 5);
        setFlex(eid, "gap", 4);
        setBackground(eid, APP_PANEL);
        setBorder(eid, APP_BORDER, 1, 6);
        setFlex(eid, "align_items", "center");
        setFlex(eid, "justify_content", "center");

        createRow(eid + "-viz", eid);
        setFlex(eid + "-viz", "height", 10);
        setFlex(eid + "-viz", "gap", 3);
        setFlex(eid + "-viz", "align_items", "center");

        if (ei === 0) {
            createCol(eid + "-chip", eid + "-viz");
            setFlex(eid + "-chip", "width", 22);
            setFlex(eid + "-chip", "height", 8);
            setBackground(eid + "-chip", APP_PANEL_RAISED);
            setBorder(eid + "-chip", APP_BORDER, 1, 4);
        } else if (ei === 1) {
            createCol(eid + "-chip", eid + "-viz");
            setFlex(eid + "-chip", "width", 18);
            setFlex(eid + "-chip", "height", 8);
            setBackground(eid + "-chip", APP_ACCENT);
            setBorder(eid + "-chip", APP_ACCENT, 0, 4);
        } else if (ei === 2) {
            for (var blurStep = 0; blurStep < 3; blurStep++) {
                createCol(eid + "-chip-" + blurStep, eid + "-viz");
                setFlex(eid + "-chip-" + blurStep, "width", 8 + blurStep * 4);
                setFlex(eid + "-chip-" + blurStep, "height", 8);
                setBackground(eid + "-chip-" + blurStep, APP_ACCENT);
                setBorder(eid + "-chip-" + blurStep, "transparent", 0, 4);
                setOpacity(eid + "-chip-" + blurStep, 0.35 + blurStep * 0.2);
            }
        } else {
            for (var gradStep = 0; gradStep < 3; gradStep++) {
                createCol(eid + "-chip-" + gradStep, eid + "-viz");
                setFlex(eid + "-chip-" + gradStep, "width", 8);
                setFlex(eid + "-chip-" + gradStep, "height", 8);
                setBorder(eid + "-chip-" + gradStep, "transparent", 0, 4);
            }
        }

        createLabel(eid + "-l", effectNames[ei], eid);
        setFontSize(eid + "-l", 9);
    }

    // ── GPU Showcase ─────────────────────────────────────────────────
    createLabel("showcase-header", "GPU SHOWCASE", "preview-area");
    stylePreviewSectionHeader("showcase-header");

    // XY Pad for touch/mouse interaction demo
    createRow("showcase-row", "preview-area");
    setFlex("showcase-row", "gap", 8);
    setFlex("showcase-row", "height", 84);

    createXYPad("xy-demo", "showcase-row");
    setFlex("xy-demo", "width", 80);
    setFlex("xy-demo", "height", 84);
    setXY("xy-demo", 0.38, 0.67);

    // Spectrum analyzer
    createSpectrum("spectrum-demo", "showcase-row");
    setFlex("spectrum-demo", "flex_grow", 1);
    setFlex("spectrum-demo", "height", 84);
}
buildPreviewEffects();

// Generate some spectrum data
var specData = [];
function buildPreviewSpectrum() {
    for (var si = 0; si < 72; si++) {
        var freq = si / 71;
        var lowPeak = 26 * Math.exp(-Math.pow((freq - 0.14) / 0.08, 2));
        var midPeak = 18 * Math.exp(-Math.pow((freq - 0.46) / 0.10, 2));
        var airPeak = 22 * Math.exp(-Math.pow((freq - 0.78) / 0.06, 2));
        var ripple = Math.sin(freq * 26.0) * 2.2 + Math.cos(freq * 13.0) * 1.1;
        var floor = -74 + Math.sin(freq * 5.0) * 1.5;
        specData.push(Math.max(-78, Math.min(-8, floor + lowPeak + midPeak + airPeak + ripple)));
    }
    setSpectrumData("spectrum-demo", specData);

    // Second waveform with different data
    createLabel("waveform2-header", "AUDIO WAVEFORM", "preview-area");
    stylePreviewSectionHeader("waveform2-header");

    createWaveform("waveform2", "preview-area");
    setFlex("waveform2", "height", 60);
}
buildPreviewSpectrum();

var wave2 = [];
function buildPreviewWaveform2AndRightPanel() {
    for (var w2 = 0; w2 < 512; w2++) {
        var phase2 = w2 / 512;
        var envelope2 = 0.58 + 0.18 * Math.sin(phase2 * Math.PI * 4.0);
        wave2.push((Math.sin(2 * Math.PI * 5 * phase2) * 0.42 +
                    Math.sin(2 * Math.PI * 13 * phase2) * 0.18 +
                    Math.sin(2 * Math.PI * 27 * phase2) * 0.09) * envelope2);
    }
    setWaveformData("waveform2", wave2);

    // ── RIGHT PANEL (Inspector + Chat) ──────────────────────────────
    createCol("right-panel", "main-area");
    setFlex("right-panel", "width", 272);
    setFlex("right-panel", "min_width", 272);
    setFlex("right-panel", "max_width", 272);
    setFlex("right-panel", "flex_grow", 0);
    setFlex("right-panel", "flex_shrink", 0);
    setBackground("right-panel", APP_SURFACE);
    setBorder("right-panel", APP_BORDER, 1, 0);

    // Tab bar
    createRow("right-tabs", "right-panel");
    setFlex("right-tabs", "height", 36);
    setFlex("right-tabs", "flex_shrink", 0);
    setFlex("right-tabs", "align_items", "center");
    setFlex("right-tabs", "justify_content", "center");
    setFlex("right-tabs", "gap", 0);
    setFlex("right-tabs", "padding_left", 10);
    setFlex("right-tabs", "padding_right", 10);
    setBackground("right-tabs", APP_PANEL);
    setBorder("right-tabs", APP_BORDER, 1, 0);

    // Tab columns (label + underline indicator)
    createCol("tab-inspector-col", "right-tabs");
    setFlex("tab-inspector-col", "flex_grow", 1);
    setFlex("tab-inspector-col", "height", 36);
    setFlex("tab-inspector-col", "align_items", "center");
    setFlex("tab-inspector-col", "justify_content", "center");
    setFlex("tab-inspector-col", "gap", 0);

    createLabel("tab-inspector", "INSPECTOR", "tab-inspector-col");
    setFontSize("tab-inspector", 11);
    setFlex("tab-inspector", "height", 28);
    setTextAlign("tab-inspector", "center");
    setTextColor("tab-inspector", APP_TEXT_DIM);

    // Underline indicator — use width not flex_grow to avoid vertical expansion
    createCol("tab-inspector-line", "tab-inspector-col");
    setFlex("tab-inspector-line", "height", 2);
    setFlex("tab-inspector-line", "width", 120);
    setFlex("tab-inspector-line", "flex_shrink", 0);

    createCol("tab-chat-col", "right-tabs");
    setFlex("tab-chat-col", "flex_grow", 1);
    setFlex("tab-chat-col", "height", 36);
    setFlex("tab-chat-col", "align_items", "center");
    setFlex("tab-chat-col", "justify_content", "center");
    setFlex("tab-chat-col", "gap", 0);

    createLabel("tab-chat", "CHAT", "tab-chat-col");
    setFontSize("tab-chat", 11);
    setFlex("tab-chat", "height", 28);
    setTextAlign("tab-chat", "center");
    setTextColor("tab-chat", APP_ACCENT);

    createCol("tab-chat-line", "tab-chat-col");
    setFlex("tab-chat-line", "height", 2);
    setFlex("tab-chat-line", "width", 120);
    setFlex("tab-chat-line", "flex_shrink", 0);
    setBackground("tab-chat-line", APP_ACCENT);

    // Inspector content area (hidden by default)
    createCol("inspector-area", "right-panel");
    setFlex("inspector-area", "flex_grow", 1);
    setFlex("inspector-area", "padding", 10);
    setFlex("inspector-area", "gap", 6);
    setVisible("inspector-area", false);

    createLabel("insp-title", "ELEMENT INSPECTOR", "inspector-area");
    setFontSize("insp-title", 10);
    setTextColor("insp-title", APP_TEXT_DIM);
    setFlex("insp-title", "height", 14);

    createLabel("insp-hint", "Cmd+click any element in the preview to inspect its properties.", "inspector-area");
    setFontSize("insp-hint", 10);
    setTextColor("insp-hint", APP_TEXT_DIM);
    setFlex("insp-hint", "height", 28);

    // Inspector property rows (placeholder)
    createRow("insp-row-type", "inspector-area");
    setFlex("insp-row-type", "height", 20);
    setFlex("insp-row-type", "align_items", "center");
    createLabel("insp-type-k", "Type:", "insp-row-type");
    setFontSize("insp-type-k", 10);
    setTextColor("insp-type-k", APP_TEXT_DIM);
    setFlex("insp-type-k", "width", 60);
    createLabel("insp-type-v", "—", "insp-row-type");
    setFontSize("insp-type-v", 10);
    setFlex("insp-type-v", "flex_grow", 1);

    createRow("insp-row-id", "inspector-area");
    setFlex("insp-row-id", "height", 20);
    setFlex("insp-row-id", "align_items", "center");
    createLabel("insp-id-k", "ID:", "insp-row-id");
    setFontSize("insp-id-k", 10);
    setTextColor("insp-id-k", APP_TEXT_DIM);
    setFlex("insp-id-k", "width", 60);
    createLabel("insp-id-v", "—", "insp-row-id");
    setFontSize("insp-id-v", 10);
    setFlex("insp-id-v", "flex_grow", 1);

    createRow("insp-row-bounds", "inspector-area");
    setFlex("insp-row-bounds", "height", 20);
    setFlex("insp-row-bounds", "align_items", "center");
    createLabel("insp-bounds-k", "Bounds:", "insp-row-bounds");
    setFontSize("insp-bounds-k", 10);
    setTextColor("insp-bounds-k", APP_TEXT_DIM);
    setFlex("insp-bounds-k", "width", 60);
    createLabel("insp-bounds-v", "—", "insp-row-bounds");
    setFontSize("insp-bounds-v", 10);
    setFlex("insp-bounds-v", "flex_grow", 1);
}
buildPreviewWaveform2AndRightPanel();

// Tab switching logic
var activeTab = "chat";
function switchTab(tab) {
    activeTab = tab;
    if (tab === "chat") {
        setVisible("chat-area", true);
        setVisible("inspector-area", false);
        setTextColor("tab-chat", APP_ACCENT);
        setBackground("tab-chat-line", APP_ACCENT);
        setTextColor("tab-inspector", APP_TEXT_DIM);
        setBackground("tab-inspector-line", APP_PANEL);  // hide by matching background
    } else {
        setVisible("chat-area", false);
        setVisible("inspector-area", true);
        setTextColor("tab-inspector", APP_ACCENT);
        setBackground("tab-inspector-line", APP_ACCENT);
        setTextColor("tab-chat", APP_TEXT_DIM);
        setBackground("tab-chat-line", APP_PANEL);  // hide by matching background
    }
    layout();
}

// Tab switching via registerClick + on() click events
function buildRightPanelChat() {
    registerClick("tab-inspector-col");
    registerClick("tab-chat-col");
    registerClick("tab-inspector");
    registerClick("tab-chat");
    on("tab-inspector-col", "click", function() { switchTab("inspector"); });
    on("tab-chat-col", "click", function() { switchTab("chat"); });
    on("tab-inspector", "click", function() { switchTab("inspector"); });
    on("tab-chat", "click", function() { switchTab("chat"); });

    // Chat content area
    createCol("chat-area", "right-panel");
    setFlex("chat-area", "flex_grow", 1);
    setFlex("chat-area", "padding", 10);
    setFlex("chat-area", "gap", 8);

    // AI provider / model selector
    createCol("model-row", "chat-area");
    setFlex("model-row", "height", 48);
    setFlex("model-row", "flex_shrink", 0);
    setFlex("model-row", "gap", 4);

    createRow("model-top-row", "model-row");
    setFlex("model-top-row", "height", 22);
    setFlex("model-top-row", "align_items", "center");
    setFlex("model-top-row", "justify_content", "space-between");

    // #51: Context badge with accent styling
    // Chat context badge
    createRow("context-badge", "model-top-row");
    setFlex("context-badge", "height", 22);
    setFlex("context-badge", "width", 136);
    setFlex("context-badge", "flex_shrink", 0);
    setFlex("context-badge", "padding_left", 8);
    setFlex("context-badge", "padding_right", 6);
    setFlex("context-badge", "align_items", "center");
    setFlex("context-badge", "gap", 6);
    setBackground("context-badge", '#2a2040');
    setBorder("context-badge", '#9f7aea', 1, 11);
    createLabel("context-label", "Editing: All", "context-badge");
    setFontSize("context-label", 9);
    setTextColor("context-label", APP_ACCENT);
    setFlex("context-label", "flex_grow", 1);
    setFlex("context-label", "height", 14);
    setTextOverflow("context-label", "ellipsis");
    createCol("context-clear", "context-badge");
    setFlex("context-clear", "width", 14);
    setFlex("context-clear", "height", 14);
    setFlex("context-clear", "justify_content", "center");
    setFlex("context-clear", "align_items", "center");
    setBackground("context-clear", "#ffffff0c");
    setBorder("context-clear", "#ffffff12", 1, 7);
    createIcon("context-clear-icon", "close", "context-clear");
    setFlex("context-clear-icon", "width", 9);
    setFlex("context-clear-icon", "height", 9);
    setPointerEvents("context-clear-icon", "none");
    setVisible("context-clear", false);
    registerClick("context-clear");
    registerClick("context-badge");

    createCombo("provider-selector", "model-top-row");
    setItems("provider-selector", ["Claude", "Codex"]);
    setFlex("provider-selector", "width", 84);
    setFlex("provider-selector", "height", 22);

    createRow("model-bottom-row", "model-row");
    setFlex("model-bottom-row", "height", 22);
    setFlex("model-bottom-row", "align_items", "center");
    setFlex("model-bottom-row", "gap", 6);

    createCombo("model-selector", "model-bottom-row");
    setItems("model-selector", ["Sonnet 4.6", "Opus 4.6"]);
    setFlex("model-selector", "flex_grow", 1);
    setFlex("model-selector", "height", 22);

    createCombo("effort-selector", "model-bottom-row");
    setItems("effort-selector", ["Default", "Low", "Medium", "High", "xHigh"]);
    setFlex("effort-selector", "width", 74);
    setFlex("effort-selector", "height", 22);
    setVisible("effort-selector", false);

    createCol("chat-export-btn", "model-bottom-row");
    setFlex("chat-export-btn", "width", 52);
    setFlex("chat-export-btn", "height", 22);
    setFlex("chat-export-btn", "justify_content", "center");
    setFlex("chat-export-btn", "align_items", "center");
    setBorder("chat-export-btn", APP_BORDER, 1, 6);
    createLabel("chat-export-label", "Export", "chat-export-btn");
    setFontSize("chat-export-label", 9);
    setTextColor("chat-export-label", APP_TEXT_DIM);
    setPointerEvents("chat-export-label", "none");
    registerClick("chat-export-btn");

    // Chat messages (scrollable)
    createScrollView("chat-messages", "chat-area");
    setFlex("chat-messages", "flex_grow", 1);
    setFlex("chat-messages", "padding_right", 10);
    setScrollContentSize("chat-messages", 224, 400);  // keep content clear of scrollbar

    createCol("chat-thread", "chat-messages");
    setFlex("chat-thread", "gap", 8);
    setFlex("chat-thread", "padding_right", 14);
    setFlex("chat-thread", "flex_shrink", 0);

    createLabel("welcome-msg", "Describe a visual style and the preview will update live.", "chat-thread");
    setFontSize("welcome-msg", 11);
    setFlex("welcome-msg", "height", 30);

    createLabel("hint-msg", 'Try: "warm vintage" or "neon cyberpunk"', "chat-thread");
    setFontSize("hint-msg", 10);
    setTextColor("hint-msg", APP_TEXT_DIM);
    setFlex("hint-msg", "height", 16);

    createRow("chat-typing-row", "chat-area");
    setFlex("chat-typing-row", "height", 0);
    setFlex("chat-typing-row", "align_items", "center");
    setFlex("chat-typing-row", "gap", 6);
    setFlex("chat-typing-row", "padding_left", 6);
    setFlex("chat-typing-row", "padding_right", 6);
    setFlex("chat-typing-row", "flex_shrink", 0);
    setBackground("chat-typing-row", APP_PANEL);
    setBorder("chat-typing-row", APP_BORDER, 1, 6);
    setVisible("chat-typing-row", false);

    createLabel("chat-typing-label", "", "chat-typing-row");
    setFontSize("chat-typing-label", 9);
    setTextColor("chat-typing-label", APP_TEXT_DIM);

    // Chat input area
    createRow("chat-attachment-row", "chat-area");
    setFlex("chat-attachment-row", "height", 0);
    setFlex("chat-attachment-row", "gap", 6);
    setFlex("chat-attachment-row", "align_items", "center");
    setFlex("chat-attachment-row", "padding_left", 6);
    setFlex("chat-attachment-row", "padding_right", 6);
    setFlex("chat-attachment-row", "flex_shrink", 0);
    setBackground("chat-attachment-row", APP_PANEL);
    setBorder("chat-attachment-row", APP_BORDER, 1, 6);
    setVisible("chat-attachment-row", false);

    createLabel("chat-attachment-label", "", "chat-attachment-row");
    setFontSize("chat-attachment-label", 9);
    setTextColor("chat-attachment-label", APP_TEXT_DIM);
    setFlex("chat-attachment-label", "flex_grow", 1);
    setTextOverflow("chat-attachment-label", "ellipsis");

    createCol("chat-attachment-clear", "chat-attachment-row");
    setFlex("chat-attachment-clear", "width", 16);
    setFlex("chat-attachment-clear", "height", 16);
    setFlex("chat-attachment-clear", "justify_content", "center");
    setFlex("chat-attachment-clear", "align_items", "center");
    setBorder("chat-attachment-clear", APP_BORDER, 1, 8);
    createLabel("chat-attachment-clear-label", "x", "chat-attachment-clear");
    setFontSize("chat-attachment-clear-label", 9);
    setTextColor("chat-attachment-clear-label", APP_TEXT_DIM);
    setPointerEvents("chat-attachment-clear-label", "none");
    registerClick("chat-attachment-clear");

    createRow("chat-input-row", "chat-area");
    setFlex("chat-input-row", "height", 36);
    setFlex("chat-input-row", "align_items", "center");
    setFlex("chat-input-row", "flex_shrink", 0);
    setFlex("chat-input-row", "gap", 6);

    // Upload button with hover state (Issue 3)
    // #49: Upload button with proper icon sizing
    createCol("upload-btn", "chat-input-row");
    setFlex("upload-btn", "width", 32);
    setFlex("upload-btn", "height", 32);
    setBackground("upload-btn", APP_PANEL);
    setBorder("upload-btn", APP_BORDER, 1, 6);
    setFlex("upload-btn", "justify_content", "center");
    setFlex("upload-btn", "align_items", "center");
    createIcon("upload-icon", "image_upload", "upload-btn");
    setFlex("upload-icon", "width", 16);
    setFlex("upload-icon", "height", 16);
    setPointerEvents("upload-icon", "none");
    registerHover("upload-btn");
    on("upload-btn", "mouseenter", function() { setBorder("upload-btn", APP_ACCENT, 1, 6); setBackground("upload-btn", APP_PANEL_RAISED); });
    on("upload-btn", "mouseleave", function() { setBorder("upload-btn", APP_BORDER, 1, 6); setBackground("upload-btn", APP_PANEL); });
}
buildRightPanelChat();
