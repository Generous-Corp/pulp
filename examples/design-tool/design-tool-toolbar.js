// ═══════════════════════════════════════════════════════════════════
// Root: vertical column (toolbar → main → status bar)
// ═══════════════════════════════════════════════════════════════════
function buildRootShell() {
    setFlex("", "direction", "col");
    setFlex("", "gap", 0);
    setBackground("", APP_BG);

    // ═══════════════════════════════════════════════════════════════════
    // TOOLBAR (44px, full width, space-between)
    // ═══════════════════════════════════════════════════════════════════
    createRow("toolbar");
    setFlex("toolbar", "height", 44);
    setFlex("toolbar", "flex_shrink", 0);
    setFlex("toolbar", "gap", 8);
    setFlex("toolbar", "padding_left", 12);
    setFlex("toolbar", "padding_right", 12);
    setFlex("toolbar", "align_items", "center");
    setBackground("toolbar", APP_SURFACE);
    setBorder("toolbar", APP_BORDER, 1, 0);
}
buildRootShell();

function createToolbarSeparator(id) {
    createCol(id, "toolbar");
    setFlex(id, "width", 1);
    setFlex(id, "height", 18);
    setFlex(id, "flex_shrink", 0);
    setBackground(id, APP_BORDER);
}

function buildToolbarTitleAndPreset() {
    createRow("toolbar-title-group", "toolbar");
    setFlex("toolbar-title-group", "height", 28);
    setFlex("toolbar-title-group", "align_items", "center");
    setFlex("toolbar-title-group", "flex_shrink", 0);

    createLabel("theme-name-label", "Default Dark", "toolbar-title-group");
    setFontSize("theme-name-label", 12);
    setFlex("theme-name-label", "width", 132);
    setFlex("theme-name-label", "flex_shrink", 0);
    setFlex("theme-name-label", "height", 26);
    setTextOverflow("theme-name-label", "ellipsis");

    createToolbarSeparator("toolbar-sep-1");

    createRow("toolbar-preset-group", "toolbar");
    setFlex("toolbar-preset-group", "height", 28);
    setFlex("toolbar-preset-group", "align_items", "center");
    setFlex("toolbar-preset-group", "gap", 6);
    setFlex("toolbar-preset-group", "flex_shrink", 0);

    createLabel("toolbar-preset-label", "Preset", "toolbar-preset-group");
    setFontSize("toolbar-preset-label", 10);
    setTextColor("toolbar-preset-label", APP_TEXT_DIM);
    setFlex("toolbar-preset-label", "width", 34);
    setFlex("toolbar-preset-label", "height", 14);
    setFlex("toolbar-preset-label", "flex_shrink", 0);

    createCombo("preset-selector", "toolbar-preset-group");
    setItems("preset-selector", ["Default Dark", "Light", "Pro Audio", "Violet", "Amber", "Ocean", "Neon"]);
    setFlex("preset-selector", "width", 118);
    setFlex("preset-selector", "height", 26);
    setFlex("preset-selector", "flex_shrink", 0);

    createToolbarSeparator("toolbar-sep-2");
}
buildToolbarTitleAndPreset();

// State scrubber pills
var stateNames = ["Default", "Hover", "Focus", "Disabled", "Error"];
var activeState = 0;
function buildToolbarStateGroup() {
    createRow("toolbar-state-group", "toolbar");
    setFlex("toolbar-state-group", "height", 28);
    setFlex("toolbar-state-group", "align_items", "center");
    setFlex("toolbar-state-group", "flex_shrink", 0);

    createRow("state-pills", "toolbar-state-group");
    setFlex("state-pills", "height", 26);
    setFlex("state-pills", "width", 316);
    setFlex("state-pills", "flex_shrink", 0);
    setFlex("state-pills", "gap", 2);
    setFlex("state-pills", "align_items", "center");
    setFlex("state-pills", "padding_left", 2);
    setFlex("state-pills", "padding_right", 2);
    setBackground("state-pills", APP_PANEL);
    setBorder("state-pills", APP_BORDER, 1, 6);
}
buildToolbarStateGroup();

var stateWidths = [64, 56, 56, 76, 54];
function buildToolbarStatePills() {
    for (var sp = 0; sp < stateNames.length; sp++) {
        var spId = "state-pill-" + sp;
        createCol(spId, "state-pills");
        setFlex(spId, "height", 20);
        setFlex(spId, "width", stateWidths[sp]);
        setFlex(spId, "flex_shrink", 0);
        setFlex(spId, "justify_content", "center");
        setFlex(spId, "align_items", "center");
        if (sp === 0) {
            setBackground(spId, '#2a2040');
        }
        setBorder(spId, "transparent", 0, 4);
        createLabel(spId + "-lbl", stateNames[sp], spId);
        setFontSize(spId + "-lbl", 10);
        setTextColor(spId + "-lbl", sp === 0 ? APP_ACCENT : APP_TEXT_DIM);
        setPointerEvents(spId + "-lbl", "none");
        registerClick(spId);
        (function(idx) {
            on("state-pill-" + idx, "click", function() {
                // Update pill visuals
                for (var si = 0; si < stateNames.length; si++) {
                    setBackground("state-pill-" + si, si === idx ? '#2a2040' : 'transparent');
                    setBorder("state-pill-" + si, "transparent", 0, 4);
                    setTextColor("state-pill-" + si + "-lbl", si === idx ? APP_ACCENT : APP_TEXT_DIM);
                }
                activeState = idx;
                setText("status-text", "State: " + stateNames[idx]);
                // #50: Apply state overrides to preview components
                applyStateToPreview(idx);
            });
        })(sp);
    }
}
buildToolbarStatePills();

// #50: Apply state overrides to preview components
function applyStateToPreview(stateIdx) {
    // 0=Default, 1=Hover, 2=Focus, 3=Disabled, 4=Error
    // Reset all to default first
    refreshPreviewThemeBase();
    setOpacity("btn-normal", 1); setOpacity("btn-hover", 1);
    setOpacity("btn-action", 1); setOpacity("btn-disabled", 0.5);
    setBackground("btn-normal", previewThemeColor("bg.surface", "#3a3a4c"));
    setBackground("btn-hover", previewThemeColor("bg.elevated", "#4a4a5c"));
    setBorder("btn-normal", previewThemeColor("control.border", APP_BORDER), 1, 6);
    setBorder("btn-hover", previewThemeColor("control.border", APP_BORDER), 1, 6);
    setBorder("btn-action", previewThemeColor("accent.primary", APP_ACCENT), 0, 6);
    setEnabled("btn-disabled", false);
    setBackground("btn-action", previewThemeColor("accent.primary", APP_ACCENT));

    setOpacity("sample-input", 1);
    setOpacity("sample-placeholder", 1);
    setOpacity("sample-combo", 1);
    setOpacity("tb1", 1);
    setOpacity("t1", 1);
    setOpacity("t2", 1);
    setOpacity("cb1", 1);
    setEnabled("sample-input", true);
    setEnabled("sample-placeholder", true);
    setEnabled("sample-combo", true);
    setEnabled("tb1", true);
    setEnabled("t1", true);
    setEnabled("t2", true);
    setEnabled("cb1", true);
    setBackground("sample-input", previewThemeColor("bg.surface", APP_PANEL));
    setBackground("sample-placeholder", previewThemeColor("bg.surface", APP_PANEL));
    setBackground("sample-combo", previewThemeColor("bg.surface", APP_PANEL));
    setBorder("sample-input", previewThemeColor("control.border", APP_BORDER), 1, 6);
    setBorder("sample-placeholder", previewThemeColor("control.border", APP_BORDER), 1, 6);
    setBorder("sample-combo", previewThemeColor("control.border", APP_BORDER), 1, 6);
    setTextColor("sample-combo-label", previewThemeColor("text.primary", APP_TEXT));
    setTextColor("sample-combo-caret", previewThemeColor("text.secondary", APP_TEXT_DIM));
    setBackground("tb1", previewThemeColor("bg.surface", APP_PANEL));
    setBorder("tb1", previewThemeColor("control.border", APP_BORDER), 1, 6);
    setTextColor("toggle-on-label", previewThemeColor("text.primary", APP_TEXT));
    setTextColor("toggle-off-label", previewThemeColor("text.primary", APP_TEXT));

    setBorder("panel-content", previewThemeColor("divider", APP_BORDER), 1, 6);
    setBackground("panel-content", previewThemeColor("bg.surface", APP_PANEL));
    refreshPreviewLayoutSection();
    setPreviewActiveTab(activePreviewTab, true);

    if (stateIdx === 1) { // Hover
        var hoverAccent = previewThemeColor("accent.primary", APP_ACCENT);
        setBackground("btn-normal", previewThemeColor("bg.elevated", "#4a4a5c"));
        setBackground("btn-hover", previewThemeColor("bg.elevated", "#5a5a6c"));
        setBorder("btn-normal", hoverAccent, 1, 6);
        setBorder("btn-action", hoverAccent, 1, 6);
        setBorder("sample-input", hoverAccent, 1, 6);
        setBorder("sample-combo", hoverAccent, 1, 6);
        setBackground("sample-combo", previewThemeColor("bg.elevated", "#4a4a5c"));
        setTextColor("sample-combo-caret", hoverAccent);
        setBackground("tb1", previewThemeColor("bg.elevated", "#4a4a5c"));
        setBorder("tb1", hoverAccent, 1, 6);
        setTextColor("ptab-1-l", previewThemeColor("text.primary", APP_TEXT));
        setBackground("card-2", previewThemeColor("card.loading", "#3e4245"));
    } else if (stateIdx === 2) { // Focus
        var focusAccent = previewThemeColor("focus.ring", APP_ACCENT);
        setBorder("btn-normal", focusAccent, 2, 6);
        setBorder("btn-hover", focusAccent, 2, 6);
        setBorder("btn-action", focusAccent, 2, 6);
        setBorder("sample-input", focusAccent, 2, 6);
        setBorder("sample-placeholder", focusAccent, 2, 6);
        setBorder("sample-combo", focusAccent, 2, 6);
        setBorder("tb1", focusAccent, 2, 6);
        setBorder("panel-content", focusAccent, 1, 6);
    } else if (stateIdx === 3) { // Disabled
        setOpacity("btn-normal", 0.4);
        setOpacity("btn-hover", 0.4);
        setOpacity("btn-action", 0.4);
        setOpacity("btn-disabled", 0.4);
        setOpacity("sample-input", 0.4);
        setOpacity("sample-placeholder", 0.4);
        setOpacity("sample-combo", 0.4);
        setOpacity("tb1", 0.4);
        setOpacity("t1", 0.4);
        setOpacity("t2", 0.4);
        setOpacity("cb1", 0.4);
        setEnabled("sample-input", false);
        setEnabled("sample-placeholder", false);
        setEnabled("sample-combo", false);
        setEnabled("tb1", false);
        setEnabled("t1", false);
        setEnabled("t2", false);
        setEnabled("cb1", false);
        setTextColor("toggle-on-label", previewThemeColor("text.disabled", APP_TEXT_DIM));
        setTextColor("toggle-off-label", previewThemeColor("text.disabled", APP_TEXT_DIM));
        setTextColor("sample-combo-label", previewThemeColor("text.disabled", APP_TEXT_DIM));
        setTextColor("sample-combo-caret", previewThemeColor("text.disabled", APP_TEXT_DIM));
    } else if (stateIdx === 4) { // Error
        var errorAccent = previewThemeColor("accent.error", "#e94560");
        setBorder("btn-normal", errorAccent, 1, 6);
        setBorder("btn-hover", errorAccent, 1, 6);
        setBackground("btn-action", errorAccent);
        setBorder("btn-action", errorAccent, 0, 6);
        setBorder("sample-input", errorAccent, 1, 6);
        setBorder("sample-placeholder", errorAccent, 1, 6);
        setBorder("sample-combo", errorAccent, 1, 6);
        setTextColor("sample-combo-caret", errorAccent);
        setBorder("tb1", errorAccent, 1, 6);
        setBorder("panel-content", errorAccent, 1, 6);
        setBackground("card-2", previewThemeColor("card.error", "#4a1f28"));
        setBackground("card-4", previewThemeColor("card.error", "#4a1f28"));
        setTextColor("card-4-label", errorAccent);
        setTextColor("toggle-off-label", errorAccent);
    }
    layout();
}

// Spacer
function buildToolbarSpacerAndGroups() {
    createCol("toolbar-spacer", "toolbar");
    setFlex("toolbar-spacer", "flex_grow", 1);

    createRow("toolbar-history-group", "toolbar");
    setFlex("toolbar-history-group", "height", 28);
    setFlex("toolbar-history-group", "align_items", "center");
    setFlex("toolbar-history-group", "gap", 8);
    setFlex("toolbar-history-group", "flex_shrink", 0);

    createToolbarSeparator("toolbar-sep-3");

    createRow("toolbar-file-group", "toolbar");
    setFlex("toolbar-file-group", "height", 28);
    setFlex("toolbar-file-group", "align_items", "center");
    setFlex("toolbar-file-group", "gap", 8);
    setFlex("toolbar-file-group", "flex_shrink", 0);
}
buildToolbarSpacerAndGroups();

// Toolbar action buttons with pill styling
var toolbarBtns = [
    { id: "undo-btn", label: "Undo", width: 74, group: "toolbar-history-group" },
    { id: "redo-btn", label: "Redo", width: 74, group: "toolbar-history-group" },
    { id: "import-btn", label: "Import", width: 88, group: "toolbar-file-group" },
    { id: "export-btn", label: "Export", width: 94, group: "toolbar-file-group", accent: true }
];
function buildToolbarActionsAndLeftPanelControls() {
    for (var tb = 0; tb < toolbarBtns.length; tb++) {
        var btn = toolbarBtns[tb];
        createCol(btn.id + "-pill", btn.group);
        setFlex(btn.id + "-pill", "width", btn.width);
        setFlex(btn.id + "-pill", "height", 26);
        setFlex(btn.id + "-pill", "flex_shrink", 0);
        setFlex(btn.id + "-pill", "padding_left", 8);
        setFlex(btn.id + "-pill", "padding_right", 8);
        setFlex(btn.id + "-pill", "justify_content", "center");
        setFlex(btn.id + "-pill", "align_items", "center");
        setBackground(btn.id + "-pill", btn.accent ? APP_ACCENT : APP_PANEL);
        setBorder(btn.id + "-pill", btn.accent ? APP_ACCENT : APP_BORDER, 1, 6);

        createLabel(btn.id, btn.label, btn.id + "-pill");
        setFontSize(btn.id, 10);
        setTextColor(btn.id, btn.accent ? "#ffffff" : APP_TEXT_DIM);
        setPointerEvents(btn.id, "none");
    }

    registerHover("undo-btn-pill");
    registerHover("redo-btn-pill");
    registerHover("import-btn-pill");
    registerHover("export-btn-pill");
    on("undo-btn-pill", "mouseenter", function() { setBorder("undo-btn-pill", APP_ACCENT, 1, 6); setBackground("undo-btn-pill", APP_PANEL_RAISED); setTextColor("undo-btn", APP_TEXT); });
    on("undo-btn-pill", "mouseleave", function() { setBorder("undo-btn-pill", APP_BORDER, 1, 6); setBackground("undo-btn-pill", APP_PANEL); setTextColor("undo-btn", APP_TEXT_DIM); });
    on("redo-btn-pill", "mouseenter", function() { setBorder("redo-btn-pill", APP_ACCENT, 1, 6); setBackground("redo-btn-pill", APP_PANEL_RAISED); setTextColor("redo-btn", APP_TEXT); });
    on("redo-btn-pill", "mouseleave", function() { setBorder("redo-btn-pill", APP_BORDER, 1, 6); setBackground("redo-btn-pill", APP_PANEL); setTextColor("redo-btn", APP_TEXT_DIM); });
    on("import-btn-pill", "mouseenter", function() { setBorder("import-btn-pill", APP_ACCENT, 1, 6); setBackground("import-btn-pill", APP_PANEL_RAISED); setTextColor("import-btn", APP_TEXT); });
    on("import-btn-pill", "mouseleave", function() { setBorder("import-btn-pill", APP_BORDER, 1, 6); setBackground("import-btn-pill", APP_PANEL); setTextColor("import-btn", APP_TEXT_DIM); });
    on("export-btn-pill", "mouseenter", function() { setBorder("export-btn-pill", APP_ACCENT_HOVER, 1, 6); setBackground("export-btn-pill", APP_ACCENT_HOVER); });
    on("export-btn-pill", "mouseleave", function() { setBorder("export-btn-pill", APP_ACCENT, 1, 6); setBackground("export-btn-pill", APP_ACCENT); });

    // ═══════════════════════════════════════════════════════════════════
    // MAIN AREA (3 columns: left 310px | center flex | right 272px)
    // ═══════════════════════════════════════════════════════════════════
    createRow("main-area");
    setFlex("main-area", "flex_grow", 1);

    // ── LEFT PANEL (Token Browser — scrollable) ─────────────────────
    createScrollView("left-panel", "main-area");
    setFlex("left-panel", "width", 310);
    setFlex("left-panel", "min_width", 260);
    setFlex("left-panel", "flex_shrink", 0);
    setFlex("left-panel", "padding_right", 12);
    setBackground("left-panel", APP_SURFACE);
    setBorder("left-panel", APP_BORDER, 1, 0);
    setScrollContentSize("left-panel", 310, 900);

    // Issue 9: Color System section matching HTML reference
    createCol("color-section", "left-panel");
    setFlex("color-section", "padding", 10);
    setFlex("color-section", "padding_right", 18);
    setFlex("color-section", "gap", 6);
    setFlex("color-section", "height", 386);
    setFlex("color-section", "flex_shrink", 0);

    createLabel("cs-title", "COLOR SYSTEM", "color-section");
    setFontSize("cs-title", 10);
    setTextColor("cs-title", APP_ACCENT);
    setFlex("cs-title", "height", 14);

    // #55: Template selector row
    createRow("template-row", "color-section");
    setFlex("template-row", "height", 22);
    setFlex("template-row", "gap", 6);
    setFlex("template-row", "align_items", "center");
    createLabel("template-lbl", "Template", "template-row");
    setFontSize("template-lbl", 9);
    setTextColor("template-lbl", APP_TEXT_DIM);
    setFlex("template-lbl", "width", 58);
    setFlex("template-lbl", "flex_shrink", 0);
    setTextOverflow("template-lbl", "ellipsis");
    createCombo("template-selector", "template-row");
    setItems("template-selector", ["Audio Studio", "Tailwind 4"]);
    setFlex("template-selector", "width", 200);
    setFlex("template-selector", "flex_grow", 1);
    setFlex("template-selector", "height", 22);
    setSelected("template-selector", 0);
    // #56: ? info button
    createLabel("template-help", "?", "template-row");
    setFontSize("template-help", 10);
    setTextColor("template-help", APP_TEXT_DIM);
    setTextAlign("template-help", "center");
    setFlex("template-help", "width", 18);
    setFlex("template-help", "height", 18);
    setFlex("template-help", "flex_shrink", 0);
    setBackground("template-help", "#ffffff08");
    setBorder("template-help", APP_BORDER, 1, 9);
    setCursor("template-help", "pointer");
    registerClick("template-help");
    on("template-help", "click", function() {
        showHelpModal("Template", "Choose the base palette preset the designer starts from. Audio Studio follows the native reference palette; Tailwind biases the ramps toward utility-style web tokens.");
    });

    on("template-selector", "select", function(idx) {
        var templates = [
            { title: "Audio Studio", theme: "dark", accent: "#89B4FA", harmony: "monochromatic", templateIndex: 0, snapshot: true },
            { title: "Tailwind 4", theme: "light", accent: "#2563EB", harmony: "complementary", templateIndex: 1, snapshot: true }
        ];
        var config = templates[idx] || templates[0];
        applyPaletteConfiguration(config);
        showToast(config.title + " template applied");
    });

    // Harmony selector row
    createRow("harmony-row", "color-section");
    setFlex("harmony-row", "height", 22);
    setFlex("harmony-row", "gap", 6);
    setFlex("harmony-row", "align_items", "center");
    createLabel("harmony-lbl", "Harmony", "harmony-row");
    setFontSize("harmony-lbl", 9);
    setTextColor("harmony-lbl", APP_TEXT_DIM);
    setFlex("harmony-lbl", "width", 58);
    setFlex("harmony-lbl", "flex_shrink", 0);
    setTextOverflow("harmony-lbl", "ellipsis");
    createCombo("harmony-selector", "harmony-row");
    setItems("harmony-selector", ["Monochromatic", "Analogous", "Complementary", "Split Comp.", "None"]);
    setFlex("harmony-selector", "width", 200);
    setFlex("harmony-selector", "flex_grow", 1);
    setFlex("harmony-selector", "height", 22);
    createLabel("harmony-help", "?", "harmony-row");
    setFontSize("harmony-help", 10);
    setTextColor("harmony-help", APP_TEXT_DIM);
    setTextAlign("harmony-help", "center");
    setFlex("harmony-help", "width", 18);
    setFlex("harmony-help", "height", 18);
    setFlex("harmony-help", "flex_shrink", 0);
    setBackground("harmony-help", "#ffffff08");
    setBorder("harmony-help", APP_BORDER, 1, 9);
    setCursor("harmony-help", "pointer");
    registerClick("harmony-help");
    on("harmony-help", "click", function() {
        showHelpModal("Harmony", "Controls how the non-accent semantic ramps relate to the accent hue. Use this to keep warning, success, and error colors either tightly coordinated or intentionally separated.");
    });

    // Mode selector row
    createRow("mode-row", "color-section");
    setFlex("mode-row", "height", 22);
    setFlex("mode-row", "gap", 6);
    setFlex("mode-row", "align_items", "center");
    createLabel("mode-lbl", "Mode", "mode-row");
    setFontSize("mode-lbl", 9);
    setTextColor("mode-lbl", APP_TEXT_DIM);
    setFlex("mode-lbl", "width", 58);
    setFlex("mode-lbl", "flex_shrink", 0);
    createCombo("mode-selector", "mode-row");
    setItems("mode-selector", ["Dark", "Light"]);
    setFlex("mode-selector", "width", 200);
    setFlex("mode-selector", "flex_grow", 1);
    setFlex("mode-selector", "height", 22);
    createLabel("mode-help", "?", "mode-row");
    setFontSize("mode-help", 10);
    setTextColor("mode-help", APP_TEXT_DIM);
    setTextAlign("mode-help", "center");
    setFlex("mode-help", "width", 18);
    setFlex("mode-help", "height", 18);
    setFlex("mode-help", "flex_shrink", 0);
    setBackground("mode-help", "#ffffff08");
    setBorder("mode-help", APP_BORDER, 1, 9);
    setCursor("mode-help", "pointer");
    registerClick("mode-help");
    on("mode-help", "click", function() {
        showHelpModal("Mode", "Dark mode maps lighter shades to text and surfaces over dark backgrounds. Light mode inverts the relationship so semantic ramps still read correctly.");
    });

    // Issue 9: 5 palette rows — each with base color dot + name + 11-shade mini ramp
    // (shade ramps are built dynamically by buildShadeRamps below)

    // Hue fader (compact)
    createRow("hue-row", "color-section");
    setFlex("hue-row", "gap", 6);
    setFlex("hue-row", "align_items", "center");
    setFlex("hue-row", "height", 22);

    createLabel("hue-label", "Hue", "hue-row");
    setFontSize("hue-label", 9);
    setTextColor("hue-label", APP_TEXT_DIM);
    setFlex("hue-label", "width", 26);

    createFader("accent-hue", "horizontal", "hue-row");
    setFlex("accent-hue", "flex_grow", 1);
    setFlex("accent-hue", "height", 18);
    setValue("accent-hue", 0.65);

    // Token search field
    createRow("token-search-row", "left-panel");
    setFlex("token-search-row", "height", 32);
    setFlex("token-search-row", "flex_shrink", 0);
    setFlex("token-search-row", "padding_left", 10);
    setFlex("token-search-row", "padding_right", 10);
    setFlex("token-search-row", "padding_right", 18);
    setFlex("token-search-row", "padding_top", 6);
    setFlex("token-search-row", "align_items", "center");
    setFlex("token-search-row", "gap", 0);

    createIcon("search-icon", "search", "token-search-row");
    setFlex("search-icon", "width", 24);
    setFlex("search-icon", "height", 24);

    createTextEditor("token-search", "token-search-row");
    setPlaceholder("token-search", "Search tokens...");
    setFlex("token-search", "flex_grow", 1);
    setFlex("token-search", "height", 24);
    setTextColor("token-search", APP_TEXT);

    // Token browser header
    createRow("token-header", "left-panel");
    setFlex("token-header", "height", 24);
    setFlex("token-header", "flex_shrink", 0);
    setFlex("token-header", "padding_left", 10);
    setFlex("token-header", "padding_right", 18);
    setFlex("token-header", "align_items", "center");

    createLabel("tokens-title", "TOKENS", "token-header");
    setFontSize("tokens-title", 10);
    setTextColor("tokens-title", APP_TEXT_DIM);

    // Token list (inside left-panel scroll, no nested scroll needed)
    createCol("token-list", "left-panel");
    setFlex("token-list", "height", 0);
    setFlex("token-list", "flex_shrink", 0);
}
buildToolbarActionsAndLeftPanelControls();

// Token groups
// D5: Expanded token registry (50+ semantic tokens matching HTML reference)
var tokenGroups = [
    { name: "Background", tokens: ["bg.primary", "bg.secondary", "bg.surface", "bg.elevated"] },
    { name: "Text", tokens: ["text.primary", "text.secondary", "text.disabled", "text.link"] },
    { name: "Accent", tokens: ["accent.primary", "accent.secondary", "accent.success", "accent.warning", "accent.error"] },
    { name: "Controls", tokens: ["control.track", "control.fill", "control.thumb", "control.border"] },
    { name: "Knob", tokens: ["knob.arc", "knob.arc.bg", "knob.thumb"] },
    { name: "Slider", tokens: ["slider.track", "slider.fill", "slider.thumb"] },
    { name: "Meter", tokens: ["meter.green", "meter.yellow", "meter.red"] },
    { name: "Waveform", tokens: ["waveform.line", "waveform.fill", "waveform.grid"] },
    { name: "Cards", tokens: ["card.empty", "card.loading", "card.ready", "card.error"] },
    { name: "Overlay", tokens: ["overlay.bg", "modal.bg", "modal.border", "tooltip.bg", "tooltip.text"] },
    { name: "Tabs", tokens: ["tab.active", "tab.inactive"] },
    { name: "Effects", tokens: ["divider", "focus.ring", "progress.track", "progress.fill", "spinner"] },
    { name: "Gradient", tokens: ["gradient.start", "gradient.end"] }
];

var TOKEN_GROUP_TITLE_HEIGHT = 18;
var TOKEN_ROW_HEIGHT = 24;
var TOKEN_GROUP_GAP = 2;
var TOKEN_GROUP_TOP_PADDING = 4;
var TOKEN_GROUP_BOTTOM_PADDING = 4;
var TOKEN_SECTION_STATIC_HEIGHT = 32 + 24 + 24;
var PALETTE_COLLAPSED_HEIGHT = 28;
var PALETTE_EXPANDED_HEIGHT = 320;
var PALETTE_EDITOR_HEIGHT = 286;
var COLOR_SECTION_FIXED_HEIGHT = 14 + 22 + 22 + 22 + 22 + 32 + 26;
var COLOR_SECTION_CHILD_GAP = 6;
var currentTokenListHeight = 0;
var helpModalOpen = false;
var contrastModalOpen = false;

function showHelpModal(title, msg) {
    var size = getRootSize();
    setFlex("help-modal", "width", size.width);
    setFlex("help-modal", "height", size.height);
    setTop("help-modal", 0);
    setLeft("help-modal", 0);
    setText("help-modal-title", title);
    setText("help-modal-body", msg);
    setPointerEvents("help-modal", "auto");
    setVisible("help-modal", true);
    setOpacity("help-modal", 1);
    helpModalOpen = true;
    layout();
}

function hideHelpModal() {
    setOpacity("help-modal", 0);
    setVisible("help-modal", false);
    setPointerEvents("help-modal", "none");
    helpModalOpen = false;
    layout();
}

function showContrastModal(title, hex) {
    var size = getRootSize();
    var normalized = (hex || "#000000").toUpperCase();
    var whiteRatio = OklchEngine.contrastRatio(normalized, "#FFFFFF");
    var blackRatio = OklchEngine.contrastRatio(normalized, "#111111");
    setFlex("contrast-modal", "width", size.width);
    setFlex("contrast-modal", "height", size.height);
    setTop("contrast-modal", 0);
    setLeft("contrast-modal", 0);
    setText("contrast-title", title + " Contrast");
    setText("contrast-hex", normalized);
    setTextColor("contrast-white-aa", normalized);
    setTextColor("contrast-black-aa", normalized);
    setText("contrast-white-ratio", whiteRatio.toFixed(2) + ":1 on white");
    setText("contrast-black-ratio", blackRatio.toFixed(2) + ":1 on dark");
    setText("contrast-note", "Preview the current palette color against light and dark surfaces before committing it to tokens.");
    setPointerEvents("contrast-modal", "auto");
    setVisible("contrast-modal", true);
    setOpacity("contrast-modal", 1);
    contrastModalOpen = true;
    layout();
}

function hideContrastModal() {
    setOpacity("contrast-modal", 0);
    setVisible("contrast-modal", false);
    setPointerEvents("contrast-modal", "none");
    contrastModalOpen = false;
    layout();
}

function updateColorSectionLayout() {
    if (typeof paletteNames === "undefined") return 386;
    var paletteHeight = paletteNames.length * PALETTE_COLLAPSED_HEIGHT;
    if (expandedPalette >= 0) {
        paletteHeight += (PALETTE_EXPANDED_HEIGHT - PALETTE_COLLAPSED_HEIGHT);
    }
    var childCount = 1 + 3 + 1 + paletteNames.length + 1 + 1;
    var total = 20 + COLOR_SECTION_FIXED_HEIGHT + paletteHeight + ((childCount - 1) * COLOR_SECTION_CHILD_GAP);
    setFlex("color-section", "height", total);
    return total;
}

function updateLeftPanelScrollMetrics() {
    var contentHeight = updateColorSectionLayout() + TOKEN_SECTION_STATIC_HEIGHT + currentTokenListHeight + 24;
    setScrollContentSize("left-panel", 310, Math.max(contentHeight, 900));
}

function applyPaletteExpandedLayout(idx, expanded) {
    var rampId = "ramp-" + idx;
    var editorId = rampId + "-editor";
    setVisible(editorId, expanded);
    setFlex(editorId, "height", expanded ? PALETTE_EDITOR_HEIGHT : 0);
    setFlex(editorId, "min_height", expanded ? PALETTE_EDITOR_HEIGHT : 0);
    setFlex(rampId, "height", expanded ? PALETTE_EXPANDED_HEIGHT : PALETTE_COLLAPSED_HEIGHT);
}

function updateTokenFilterLayout(query) {
    query = (query || "").toLowerCase();
    var totalHeight = 0;
    for (var g = 0; g < tokenGroups.length; g++) {
        var group = tokenGroups[g];
        var visibleRows = 0;
        for (var t = 0; t < group.tokens.length; t++) {
            var tid = "tok-" + g + "-" + t;
            var visible = query.length === 0 || group.tokens[t].toLowerCase().indexOf(query) >= 0;
            setVisible(tid, visible);
            if (visible) visibleRows++;
        }

        var groupVisible = query.length === 0 || visibleRows > 0;
        var groupId = "tg-" + g;
        setVisible(groupId, groupVisible);
        if (!groupVisible) {
            setFlex(groupId, "height", 0);
            continue;
        }

        var rowCount = query.length === 0 ? group.tokens.length : visibleRows;
        var groupHeight = TOKEN_GROUP_TOP_PADDING + TOKEN_GROUP_BOTTOM_PADDING + TOKEN_GROUP_TITLE_HEIGHT +
            (rowCount * TOKEN_ROW_HEIGHT) + (rowCount * TOKEN_GROUP_GAP);
        setFlex(groupId, "height", groupHeight);
        totalHeight += groupHeight;
    }

    currentTokenListHeight = totalHeight + 8;
    setFlex("token-list", "height", currentTokenListHeight);
    updateLeftPanelScrollMetrics();
}

function buildTokenList() {
    for (var g = 0; g < tokenGroups.length; g++) {
        var group = tokenGroups[g];
        var gid = "tg-" + g;
        // Yoga needs explicit height for each token group
        var groupHeight = 18 + (group.tokens.length * 28) + 8;  // title + rows + padding
        createCol(gid, "token-list");
        setFlex(gid, "height", groupHeight);
        setFlex(gid, "flex_shrink", 0);
        setFlex(gid, "padding_left", 10);
        setFlex(gid, "padding_right", 10);
        setFlex(gid, "padding_top", 4);
        setFlex(gid, "gap", 2);

        // D7: Styled group headers — uppercase, accent colored
        createLabel(gid + "-title", group.name, gid);
        setFontSize(gid + "-title", 9);
        setTextColor(gid + "-title", APP_ACCENT);
        setFlex(gid + "-title", "height", 18);

        for (var t = 0; t < group.tokens.length; t++) {
            var tid = "tok-" + g + "-" + t;
            createRow(tid, gid);
            setFlex(tid, "height", 24);
            setFlex(tid, "flex_shrink", 0);
            setFlex(tid, "gap", 6);
            setFlex(tid, "align_items", "center");

            // Color swatch (small colored box)
            var swatchId = tid + "-sw";
            createCol(swatchId, tid);
            setFlex(swatchId, "width", 18);
            setFlex(swatchId, "height", 18);
            setFlex(swatchId, "min_width", 18);
            setFlex(swatchId, "min_height", 18);
            setFlex(swatchId, "max_width", 18);
            setFlex(swatchId, "max_height", 18);
            setBorder(swatchId, APP_BORDER, 1, 4);

            // Token name
            createLabel(tid + "-name", group.tokens[t], tid);
            setFontSize(tid + "-name", 11);
            setFlex(tid + "-name", "flex_grow", 1);

            createLabel(tid + "-mod", "\u2022", tid);
            setFontSize(tid + "-mod", 11);
            setFlex(tid + "-mod", "width", 8);
            setFlex(tid + "-mod", "height", 12);
            setOpacity(tid + "-mod", 0.0);

            var resetId = tid + "-reset";
            createCol(resetId, tid);
            setFlex(resetId, "width", 34);
            setFlex(resetId, "min_width", 34);
            setFlex(resetId, "max_width", 34);
            setFlex(resetId, "height", 18);
            setFlex(resetId, "justify_content", "center");
            setFlex(resetId, "align_items", "center");
            setFlex(resetId, "flex_shrink", 0);
            setBackground(resetId, APP_SURFACE);
            setBorder(resetId, APP_BORDER, 1, 9);
            setVisible(resetId, false);
            registerClick(resetId);
            createLabel(resetId + "-lbl", "Reset", resetId);
            setFontSize(resetId + "-lbl", 8);
            setTextColor(resetId + "-lbl", APP_TEXT_DIM);
            setPointerEvents(resetId + "-lbl", "none");

            // D1: hex input field
            var hexId = tid + "-hex";
            createTextEditor(hexId, tid);
            setPlaceholder(hexId, "#000000");
            setFlex(hexId, "width", 72);
            setFlex(hexId, "min_width", 72);
            setFlex(hexId, "max_width", 72);
            setFlex(hexId, "flex_shrink", 0);
            setFlex(hexId, "height", 20);
            setFontSize(hexId, 10);

            // D7: hover highlight on token row
            registerHover(tid);
            (function(rowId) {
                on(rowId, 'mouseenter', function() { setBackground(rowId, '#ffffff08'); });
                on(rowId, 'mouseleave', function() { setBackground(rowId, 'transparent'); });
            })(tid);

            // D1: click swatch → open token popup
            registerClick(swatchId);
            (function(tokenName, sid, gi, ti) {
                on(sid, 'click', function() {
                    openTokenPopup(tokenName, sid, gi, ti);
                });
            })(group.tokens[t], swatchId, g, t);

            (function(tokenName, rid) {
                on(rid, 'click', function() {
                    resetTokenColor(tokenName);
                });
            })(group.tokens[t], resetId);

            // D1: hex input → apply on Enter
            (function(tokenName, hid) {
                on(hid, 'return', function(text) {
                    var hex = text.trim();
                    if (!/^#[0-9a-fA-F]{6}$/.test(hex)) return;
                    applyTokenColor(tokenName, hex);
                });
            })(group.tokens[t], hexId);
        }
    }

    // Token search filtering
    on("token-search", "change", function(query) {
        updateTokenFilterLayout(query);
        layout();
    });
}
buildTokenList();

// ── Apply token colors to swatches ───────────────────────────────
function updateTokenSwatches(skipPreviewRefresh) {
    var themeStr = getThemeJson();
    var theme = JSON.parse(themeStr);
    var colors = theme.colors || {};
    for (var g = 0; g < tokenGroups.length; g++) {
        var group = tokenGroups[g];
        for (var t = 0; t < group.tokens.length; t++) {
            var swatchId = "tok-" + g + "-" + t + "-sw";
            var hexId = "tok-" + g + "-" + t + "-hex";
            var tokenName = group.tokens[t];
            if (colors[tokenName]) {
                setBackground(swatchId, colors[tokenName]);
                setOpacity(swatchId, 1.0);
                setText(hexId, colors[tokenName]);
            }
        }
    }
    if (previewReady && !skipPreviewRefresh) applyStateToPreview(activeState);
}
updateTokenSwatches();
