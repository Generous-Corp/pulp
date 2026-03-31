// Pulp Style Designer — JS-defined UI matching ai-style-designer layout
// Reference: ~/Code/ai-style-designer/Tools/theme-designer.html
// Hot-reloadable: edit and save to see changes instantly.

setTheme("dark");

// Wrap __requestFrame__ to use the callback registry pattern
// (C++ can't receive JS functions directly — use ID-based dispatch)
var __origRequestFrame__ = __requestFrame__;
__requestFrame__ = function(fn) {
    var id = __frameNextId__++;
    __frameCallbacks__[id] = fn;
    return __origRequestFrame__(id);
};

// ═══════════════════════════════════════════════════════════════════
// Color/palette/app state
// ═══════════════════════════════════════════════════════════════════
var currentAccent = '#89B4FA';
var currentHarmony = 'monochromatic';
var msgCount = 0;

// ═══════════════════════════════════════════════════════════════════
// D1: Per-token edit state
// ═══════════════════════════════════════════════════════════════════
var tokenEditState = {
    history: {},       // { "bg.primary": { original, stack: [hex...], cursor: 0 } }
    modified: {},      // { "bg.primary": true }
    activeToken: null,
    activeSwatchId: null
};

function tokenHistory(name) {
    if (!tokenEditState.history[name]) {
        var themeColors = JSON.parse(getThemeJson()).colors || {};
        tokenEditState.history[name] = {
            original: themeColors[name] || '#000000',
            stack: [themeColors[name] || '#000000'],
            cursor: 0
        };
    }
    return tokenEditState.history[name];
}

function pushTokenEdit(name, hex) {
    var h = tokenHistory(name);
    h.stack = h.stack.slice(0, h.cursor + 1);
    h.stack.push(hex);
    h.cursor = h.stack.length - 1;
    tokenEditState.modified[name] = (hex !== h.original);
    if (!tokenEditState.modified[name]) delete tokenEditState.modified[name];
}

function applyTokenColor(name, hex) {
    var swatchId = null;
    for (var g = 0; g < tokenGroups.length; g++) {
        for (var t = 0; t < tokenGroups[g].tokens.length; t++) {
            if (tokenGroups[g].tokens[t] === name) {
                swatchId = "tok-" + g + "-" + t + "-sw";
                break;
            }
        }
        if (swatchId) break;
    }
    pushTokenEdit(name, hex);
    var obj = { colors: {} };
    obj.colors[name] = hex;
    applyTokenDiff(JSON.stringify(obj));
    pushThemeSnapshot();
    // Flash: briefly show accent color on swatch
    if (swatchId) setBackground(swatchId, APP_ACCENT);
    layout();
    updateTokenSwatches();
    updateModifiedCount();
    updateAllTokenNameDisplays();
    if (tokenEditState.activeToken) updatePopupState(tokenEditState.activeToken);
    layout();
}

function updateModifiedCount() {
    var n = 0;
    for (var k in tokenEditState.modified) n++;
    setText("status-text", n > 0 ? n + " token" + (n === 1 ? "" : "s") + " modified" : "0 tokens modified");
}

function updateAllTokenNameDisplays() {
    for (var g = 0; g < tokenGroups.length; g++) {
        for (var t = 0; t < tokenGroups[g].tokens.length; t++) {
            var name = tokenGroups[g].tokens[t];
            var labelId = "tok-" + g + "-" + t + "-name";
            if (tokenEditState.modified[name]) {
                setText(labelId, name + " *");
                setTextColor(labelId, APP_ACCENT);
            } else {
                setText(labelId, name);
                setTextColor(labelId, APP_TEXT);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// App colors (matching original --app-* CSS variables)
// ═══════════════════════════════════════════════════════════════════
var APP_BG      = '#18181f';
var APP_SURFACE = '#1e1e26';
var APP_PANEL   = '#242429';
var APP_PANEL_RAISED = '#2a2a31';
var APP_BORDER  = '#2e2e36';
var APP_TEXT    = '#d4d4dc';
var APP_TEXT_DIM = '#808090';
var APP_ACCENT  = '#aa88ff';
var APP_ACCENT_HOVER = '#bf9fff';
var previewReady = false;
var activePreviewTab = 0;

function previewThemeColor(token, fallback) {
    var colors = JSON.parse(getThemeJson()).colors || {};
    return colors[token] || fallback;
}

var previewTabContent = [
    { title: "Panel content area", subtitle: "With divider and secondary text" },
    { title: "Audio routing", subtitle: "Meters, waveforms, and controls align here" },
    { title: "MIDI mapping", subtitle: "Assignments and ranges preview here" },
    { title: "About this preset", subtitle: "Metadata, export details, and notes" }
];

function setPreviewActiveTab(idx, skipLayout) {
    if (!previewReady) return;
    activePreviewTab = idx;
    var activeUnderline = previewThemeColor("tab.active", APP_ACCENT);
    var inactiveText = previewThemeColor("tab.inactive", APP_TEXT_DIM);
    for (var ti = 0; ti < 4; ti++) {
        setTextColor("ptab-" + ti + "-l", ti === idx ? APP_TEXT : inactiveText);
        setBackground("ptab-" + ti + "-line", ti === idx ? activeUnderline : "transparent");
    }
    setText("panel-title", previewTabContent[idx].title);
    setText("panel-sub", previewTabContent[idx].subtitle);
    if (!skipLayout) layout();
}

function refreshPreviewLayoutSection() {
    if (!previewReady) return;
    var cardEmpty = previewThemeColor("card.empty", APP_PANEL);
    var cardLoading = previewThemeColor("card.loading", "#3e4245");
    var cardReady = previewThemeColor("card.ready", APP_PANEL);
    var cardError = previewThemeColor("card.error", "#4a1f28");
    var success = previewThemeColor("success", "#4CAF50");
    var error = previewThemeColor("error", "#F44336");
    var spinner = previewThemeColor("spinner", APP_ACCENT);
    var divider = previewThemeColor("divider", APP_BORDER);

    setBackground("card-1", cardEmpty);
    setBackground("card-2", cardLoading);
    setBackground("card-3", cardReady);
    setBackground("card-4", cardError);
    setBorder("card-1", divider, 1, 8);
    setBorder("card-2", divider, 1, 8);
    setBorder("card-3", divider, 1, 8);
    setBorder("card-4", divider, 1, 8);
    setTextColor("card-1-label", APP_TEXT_DIM);
    setTextColor("card-3-label", APP_TEXT_DIM);
    setTextColor("card-4-label", error);
    setBackground("card-3-badge", success);
    setBackground("card-4-badge", error);
    setTextColor("card-3-badge-lbl", "#ffffff");
    setTextColor("card-4-badge-lbl", "#ffffff");
    setTextColor("card-2-spinner", spinner);
    setBackground("panel-divider", divider);
    setTextColor("spinner-icon", spinner);
    setTextColor("spinner-text", previewThemeColor("text.secondary", APP_TEXT_DIM));
    setPreviewActiveTab(activePreviewTab, true);
}

// ═══════════════════════════════════════════════════════════════════
// Root: vertical column (toolbar → main → status bar)
// ═══════════════════════════════════════════════════════════════════
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

function createToolbarSeparator(id) {
    createCol(id, "toolbar");
    setFlex(id, "width", 1);
    setFlex(id, "height", 18);
    setFlex(id, "flex_shrink", 0);
    setBackground(id, APP_BORDER);
}

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

// State scrubber pills
var stateNames = ["Default", "Hover", "Focus", "Disabled", "Error"];
var activeState = 0;
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

var stateWidths = [64, 56, 56, 76, 54];
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

// #50: Apply state overrides to preview components
function applyStateToPreview(stateIdx) {
    // 0=Default, 1=Hover, 2=Focus, 3=Disabled, 4=Error
    // Reset all to default first
    setOpacity("btn-normal", 1); setOpacity("btn-hover", 1);
    setOpacity("btn-action", 1); setOpacity("btn-disabled", 0.5);
    setBackground("btn-normal", "#3a3a4c");
    setBackground("btn-hover", "#4a4a5c");
    setBorder("btn-normal", APP_BORDER, 1, 6);
    setBorder("btn-hover", APP_BORDER, 1, 6);
    setBorder("btn-action", APP_ACCENT, 0, 6);
    setEnabled("btn-disabled", false);
    setBackground("btn-action", APP_ACCENT);

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
    setBackground("sample-input", APP_PANEL);
    setBackground("sample-placeholder", APP_PANEL);
    setBackground("sample-combo", APP_PANEL);
    setBorder("sample-input", APP_BORDER, 1, 6);
    setBorder("sample-placeholder", APP_BORDER, 1, 6);
    setBorder("sample-combo", APP_BORDER, 1, 6);
    setBackground("tb1", APP_PANEL);
    setBorder("tb1", APP_BORDER, 1, 6);
    setTextColor("toggle-on-label", APP_TEXT);
    setTextColor("toggle-off-label", APP_TEXT);

    setBorder("panel-content", previewThemeColor("divider", APP_BORDER), 1, 6);
    setBackground("panel-content", APP_PANEL);
    refreshPreviewLayoutSection();
    setPreviewActiveTab(0, true);

    if (stateIdx === 1) { // Hover
        setBackground("btn-normal", "#4a4a5c");
        setBackground("btn-hover", "#5a5a6c");
        setBorder("btn-normal", APP_ACCENT, 1, 6);
        setBorder("btn-action", APP_ACCENT, 1, 6);
        setBorder("sample-input", APP_ACCENT, 1, 6);
        setBorder("sample-combo", APP_ACCENT, 1, 6);
        setBackground("tb1", "#4a4a5c");
        setBorder("tb1", APP_ACCENT, 1, 6);
        setTextColor("ptab-1-l", APP_TEXT);
        setBackground("card-2", previewThemeColor("card.loading", "#3e4245"));
    } else if (stateIdx === 2) { // Focus
        setBorder("btn-normal", APP_ACCENT, 2, 6);
        setBorder("btn-hover", APP_ACCENT, 2, 6);
        setBorder("btn-action", APP_ACCENT, 2, 6);
        setBorder("sample-input", APP_ACCENT, 2, 6);
        setBorder("sample-placeholder", APP_ACCENT, 2, 6);
        setBorder("sample-combo", APP_ACCENT, 2, 6);
        setBorder("tb1", APP_ACCENT, 2, 6);
        setBorder("panel-content", APP_ACCENT, 1, 6);
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
        setTextColor("toggle-on-label", APP_TEXT_DIM);
        setTextColor("toggle-off-label", APP_TEXT_DIM);
    } else if (stateIdx === 4) { // Error
        setBorder("btn-normal", "#e94560", 1, 6);
        setBorder("btn-hover", "#e94560", 1, 6);
        setBackground("btn-action", "#e94560");
        setBorder("sample-input", "#e94560", 1, 6);
        setBorder("sample-placeholder", "#e94560", 1, 6);
        setBorder("sample-combo", "#e94560", 1, 6);
        setBorder("tb1", "#e94560", 1, 6);
        setBorder("panel-content", "#e94560", 1, 6);
        setBackground("card-2", previewThemeColor("card.error", "#4a1f28"));
        setBackground("card-4", previewThemeColor("card.error", "#4a1f28"));
        setTextColor("card-4-label", previewThemeColor("error", "#F44336"));
        setTextColor("toggle-off-label", "#f38ba8");
    }
    layout();
}

// Spacer
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

// Toolbar action buttons with pill styling
var toolbarBtns = [
    { id: "undo-btn", label: "Undo", width: 74, group: "toolbar-history-group" },
    { id: "redo-btn", label: "Redo", width: 74, group: "toolbar-history-group" },
    { id: "import-btn", label: "Import", width: 88, group: "toolbar-file-group" },
    { id: "export-btn", label: "Export", width: 94, group: "toolbar-file-group", accent: true }
];
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
    layout();
}

function hideHelpModal() {
    setOpacity("help-modal", 0);
    setVisible("help-modal", false);
    setPointerEvents("help-modal", "none");
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
        setFlex(swatchId, "width", 16);
        setFlex(swatchId, "height", 16);
        setBorder(swatchId, APP_BORDER, 1, 3);

        // Token name
        createLabel(tid + "-name", group.tokens[t], tid);
        setFontSize(tid + "-name", 11);
        setFlex(tid + "-name", "flex_grow", 1);

        // D1: hex input field
        var hexId = tid + "-hex";
        createTextEditor(hexId, tid);
        setPlaceholder(hexId, "#000000");
        setFlex(hexId, "width", 58);
        setFlex(hexId, "height", 18);
        setFontSize(hexId, 9);

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

// ── Apply token colors to swatches ───────────────────────────────
function updateTokenSwatches() {
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
                setText(hexId, colors[tokenName]);
            }
        }
    }
    if (previewReady) {
        refreshPreviewLayoutSection();
    }
}
updateTokenSwatches();

// ── Color System: OKLCH Shade Ramps ──────────────────────────────
var paletteNames = ["Accent", "Neutral", "Success", "Warning", "Error"];
var paletteKeys  = ["accent", "neutral", "success", "warning", "error"];
var paletteValueFormats = ["OKLCH", "OKLCH", "OKLCH", "OKLCH", "OKLCH"];

function hexToRgbParts(hex) {
    if (!hex || hex.length !== 7 || hex.charAt(0) !== "#") return { r: 0, g: 0, b: 0 };
    return {
        r: parseInt(hex.slice(1, 3), 16) || 0,
        g: parseInt(hex.slice(3, 5), 16) || 0,
        b: parseInt(hex.slice(5, 7), 16) || 0
    };
}

function getPaletteValueFields(format, oklch, hex) {
    var rgb = hexToRgbParts(hex);
    if (format === "RGB") {
        return {
            labels: ["R", "G", "B"],
            values: [String(rgb.r), String(rgb.g), String(rgb.b)]
        };
    }
    if (format === "HEX") {
        return {
            labels: ["R", "G", "B"],
            values: [
                hex.slice(1, 3).toUpperCase(),
                hex.slice(3, 5).toUpperCase(),
                hex.slice(5, 7).toUpperCase()
            ]
        };
    }
    return {
        labels: ["L", "C", "H"],
        values: [
            (oklch.L * 100).toFixed(1),
            oklch.C.toFixed(3),
            oklch.H.toFixed(1)
        ]
    };
}

function updatePaletteValueDisplay(paletteIdx, oklch, hex) {
    var display = getPaletteValueFields(paletteValueFormats[paletteIdx] || "OKLCH", oklch, hex);
    for (var i = 0; i < 3; i++) {
        setText("ramp-" + paletteIdx + "-value-" + i + "-key", display.labels[i]);
        setText("ramp-" + paletteIdx + "-value-" + i + "-text", display.values[i]);
    }
}

// Color system: palette rows with expandable gamut editor
// Clicking a palette row toggles the expanded editor (gamut triangle + sliders + shades)
var expandedPalette = -1;  // -1 = all collapsed, click a row to expand

function buildShadeRamps() {
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    var steps = ShadeGenerator.STEPS;

    for (var p = 0; p < paletteNames.length; p++) {
        var rampId = "ramp-" + p;
        removeWidget(rampId);

        // Container for palette row + editor
        createCol(rampId, "color-section");
        setFlex(rampId, "gap", 4);
        setFlex(rampId, "height", PALETTE_COLLAPSED_HEIGHT);
        setFlex(rampId, "flex_shrink", 0);

        // Palette row: dot + name + shade ramp (clickable to expand)
        var rowId = rampId + "-header";
        createRow(rowId, rampId);
        setFlex(rowId, "height", 24);
        setFlex(rowId, "flex_shrink", 0);
        setFlex(rowId, "gap", 6);
        setFlex(rowId, "align_items", "center");
        registerClick(rowId);

        var ramp = palette[paletteKeys[p]];

        // Base color dot (clickable to expand)
        var dotId = rampId + "-dot";
        createCol(dotId, rowId);
        setFlex(dotId, "width", 14);
        setFlex(dotId, "height", 14);
        setFlex(dotId, "min_width", 14);
        setFlex(dotId, "min_height", 14);
        setFlex(dotId, "max_width", 14);
        setFlex(dotId, "max_height", 14);
        setFlex(dotId, "flex_grow", 0);
        setFlex(dotId, "flex_shrink", 0);
        setBackground(dotId, ramp[500].hex);
        setBorder(dotId, ramp[500].hex, 0, 7);
        registerClick(dotId);

        // Name (clickable to expand)
        createLabel(rampId + "-name", paletteNames[p], rowId);
        setFontSize(rampId + "-name", 10);
        setTextColor(rampId + "-name", APP_TEXT_DIM);
        setFlex(rampId + "-name", "min_width", 64);
        setFlex(rampId + "-name", "flex_grow", 1);
        setTextOverflow(rampId + "-name", "ellipsis");
        registerClick(rampId + "-name");

        // Mini ramp
        createRow(rampId + "-row", rowId);
        setFlex(rampId + "-row", "width", 98);
        setFlex(rampId + "-row", "flex_shrink", 0);
        setFlex(rampId + "-row", "gap", 1);
        setFlex(rampId + "-row", "height", 12);

        for (var s = 0; s < steps.length; s++) {
            var shadeId = rampId + "-s" + s;
            createCol(shadeId, rampId + "-row");
            setFlex(shadeId, "width", 8);
            setFlex(shadeId, "height", 12);
            setBackground(shadeId, ramp[steps[s]].hex);
            setBorder(shadeId, APP_BORDER, 0, 1);
            // Click shade → expand this palette with that shade's color
            registerClick(shadeId);
        }

        // Expanded editor section (hidden unless this palette is expanded)
        var editorId = rampId + "-editor";
        createCol(editorId, rampId);
        setFlex(editorId, "gap", 4);
        setFlex(editorId, "padding_left", 4);
        setFlex(editorId, "padding_top", 2);
        setFlex(editorId, "padding_bottom", 2);
        setFlex(editorId, "flex_shrink", 0);
        applyPaletteExpandedLayout(p, p === expandedPalette);

        // Gamut triangle canvas (draggable for dot positioning)
        var gamutWrapId = rampId + "-gamut-wrap";
        createCol(gamutWrapId, editorId);
        setPosition(gamutWrapId, "relative");
        setFlex(gamutWrapId, "width", 270);
        setFlex(gamutWrapId, "height", 130);
        setFlex(gamutWrapId, "flex_shrink", 0);

        var gamutId = rampId + "-gamut";
        createCanvas(gamutId, gamutWrapId);
        setFlex(gamutId, "width", 270);
        setFlex(gamutId, "height", 130);
        setBackground(gamutId, APP_SURFACE);
        setPointerEvents(gamutId, "none");

        var gamutOverlayId = rampId + "-gamut-overlay";
        createCanvas(gamutOverlayId, gamutWrapId);
        setPosition(gamutOverlayId, "absolute");
        setTop(gamutOverlayId, 0);
        setLeft(gamutOverlayId, 0);
        setFlex(gamutOverlayId, "width", 270);
        setFlex(gamutOverlayId, "height", 130);
        registerPointer(gamutOverlayId);
        (function(idx, pKey) {
            // Handle both pointerdown and pointermove (drag) on gamut
            var dragCount = 0;
            var dragging = false;
            function onGamutPointer(evt) {
                if (evt.type === "pointermove" && !dragging) return;
                var gw = 270, gh = 130;
                var x = evt.offsetX, y = evt.offsetY;
                var L = Math.max(0, Math.min(1, x / gw));
                var C = Math.max(0, Math.min(0.4, (1 - y / gh) * 0.4));
                var h = getValue("ramp-" + idx + "-h-fdr") * 360;
                var mapped = OklchEngine.gamutMap(L, C, h);
                setValue("ramp-" + idx + "-c-fdr", Math.min(mapped.C / 0.4, 1));
                renderPaletteGamut(idx, h, mapped.L, mapped.C, evt.type === 'pointerdown');
                updatePaletteValueDisplay(idx, mapped, OklchEngine.oklchToHex(mapped.L, mapped.C, h));
                // Throttle palette rebuild during drag (every 3rd event)
                dragCount++;
                if (dragCount % 3 === 0 || evt.type === 'pointerdown') {
                    if (pKey === "accent") currentAccent = OklchEngine.oklchToHex(mapped.L, mapped.C, h);
                    var palette = PaletteSystem.create(currentAccent, currentHarmony);
                    if (pKey !== "accent") palette[pKey] = ShadeGenerator.generateRamp(mapped.L, mapped.C, h);
                    applyTokenDiff(PaletteSystem.toThemeDiff(palette));
                    updateTokenSwatches();
                    var rampData = palette[pKey]; var stepsArr = ShadeGenerator.STEPS;
                    for (var ss = 0; ss < stepsArr.length; ss++) setBackground("ramp-" + idx + "-s" + ss, rampData[stepsArr[ss]].hex);
                    setBackground("ramp-" + idx + "-dot", rampData[500].hex);
                    var si2 = [0,2,4,5,7,9];
                    for (var ls = 0; ls < 6; ls++) setBackground("ramp-" + idx + "-lg-" + ls, rampData[stepsArr[si2[ls]]].hex);
                }
            }
            on(gamutOverlayId, "pointerdown", function(e) {
                dragging = true;
                dragCount = 0;
                nativeSetPointerCapture(gamutOverlayId, e && e.pointerId ? e.pointerId : 0);
                onGamutPointer(e);
            });
            on(gamutOverlayId, "pointermove", onGamutPointer);
            on(gamutOverlayId, "pointerup", function(e) {
                dragging = false;
                nativeReleasePointerCapture(gamutOverlayId, e && e.pointerId ? e.pointerId : 0);
            });
            on(gamutOverlayId, "pointercancel", function(e) {
                dragging = false;
                nativeReleasePointerCapture(gamutOverlayId, e && e.pointerId ? e.pointerId : 0);
            });
        })(p, paletteKeys[p]);

        // H — rainbow gradient bar (visual) above fader with label
        var hGradId = rampId + "-h-grad";
        createCanvas(hGradId, editorId);
        setFlex(hGradId, "height", 14);
        setBorder(hGradId, APP_BORDER, 0, 7);
        var hGradW = 260;
        for (var hg = 0; hg < 60; hg++) {
            canvasRect(hGradId, (hg / 60) * hGradW, 0, (hGradW / 60) + 1, 14, OklchEngine.oklchToHex(0.65, 0.25, (hg / 60) * 360));
        }
        var hRowId = rampId + "-h-row";
        createRow(hRowId, editorId);
        setFlex(hRowId, "height", 18);
        setFlex(hRowId, "gap", 4);
        setFlex(hRowId, "align_items", "center");
        createLabel(rampId + "-h-lbl", "H", hRowId);
        setFontSize(rampId + "-h-lbl", 9);
        setFlex(rampId + "-h-lbl", "width", 12);
        createFader(rampId + "-h-fdr", "horizontal", hRowId);
        setFlex(rampId + "-h-fdr", "flex_grow", 1);
        setFlex(rampId + "-h-fdr", "height", 14);

        // C — chroma gradient bar (visual) above fader with label
        var cGradId = rampId + "-c-grad";
        createCanvas(cGradId, editorId);
        setFlex(cGradId, "height", 14);
        setBorder(cGradId, APP_BORDER, 0, 7);
        var cRowId = rampId + "-c-row";
        createRow(cRowId, editorId);
        setFlex(cRowId, "height", 18);
        setFlex(cRowId, "gap", 4);
        setFlex(cRowId, "align_items", "center");
        createLabel(rampId + "-c-lbl", "C", cRowId);
        setFontSize(rampId + "-c-lbl", 9);
        setFlex(rampId + "-c-lbl", "width", 12);
        createFader(rampId + "-c-fdr", "horizontal", cRowId);
        setFlex(rampId + "-c-fdr", "flex_grow", 1);
        setFlex(rampId + "-c-fdr", "height", 14);

        // Color value format selector + value fields
        var valuesRowId = rampId + "-values";
        createRow(valuesRowId, editorId);
        setFlex(valuesRowId, "height", 34);
        setFlex(valuesRowId, "gap", 8);
        setFlex(valuesRowId, "align_items", "flex_end");

        var formatId = rampId + "-format";
        createCombo(formatId, valuesRowId);
        setItems(formatId, ["OKLCH", "HEX", "RGB"]);
        setSelected(formatId, 0);
        setFlex(formatId, "width", 72);
        setFlex(formatId, "height", 26);
        setFlex(formatId, "flex_shrink", 0);

        for (var vf = 0; vf < 3; vf++) {
            var fieldId = rampId + "-value-" + vf;
            createCol(fieldId, valuesRowId);
            setFlex(fieldId, "width", 56);
            setFlex(fieldId, "height", 32);
            setFlex(fieldId, "gap", 2);
            setFlex(fieldId, "flex_shrink", 0);

            createLabel(fieldId + "-key", vf === 0 ? "L" : (vf === 1 ? "C" : "H"), fieldId);
            setFontSize(fieldId + "-key", 8);
            setTextColor(fieldId + "-key", APP_TEXT_DIM);
            setFlex(fieldId + "-key", "height", 10);

            createCol(fieldId + "-box", fieldId);
            setFlex(fieldId + "-box", "height", 20);
            setFlex(fieldId + "-box", "justify_content", "center");
            setFlex(fieldId + "-box", "align_items", "center");
            setBackground(fieldId + "-box", APP_PANEL_RAISED);
            setBorder(fieldId + "-box", APP_BORDER, 1, 6);
            createLabel(fieldId + "-text", vf === 0 ? "50.0" : (vf === 1 ? "0.100" : "180.0"), fieldId + "-box");
            setFontSize(fieldId + "-text", 10);
        }

        (function(idx) {
            on("ramp-" + idx + "-format", "select", function(selIdx) {
                paletteValueFormats[idx] = selIdx === 1 ? "HEX" : (selIdx === 2 ? "RGB" : "OKLCH");
                var palette = PaletteSystem.create(currentAccent, currentHarmony);
                var base = palette[paletteKeys[idx]][500];
                updatePaletteValueDisplay(idx, OklchEngine.hexToOklch(base.hex), base.hex);
            });
        })(p);

        // Compact shade swatches with WCAG badges
        createRow(rampId + "-shades", editorId);
        setFlex(rampId + "-shades", "gap", 4);
        setFlex(rampId + "-shades", "height", 34);
        setFlex(rampId + "-shades", "flex_shrink", 0);
        for (var ls = 0; ls < 6; ls++) {
            var lsId = rampId + "-lg-" + ls;
            var stepIdx = [0, 2, 4, 5, 7, 9][ls];
            var shadeHex = ramp[steps[stepIdx]].hex;
            createCol(lsId, rampId + "-shades");
            setFlex(lsId, "width", 40);
            setFlex(lsId, "min_width", 40);
            setFlex(lsId, "max_width", 40);
            setFlex(lsId, "height", 30);
            setFlex(lsId, "flex_grow", 0);
            setFlex(lsId, "flex_shrink", 0);
            setBackground(lsId, shadeHex);
            setBorder(lsId, APP_BORDER, 0, 10);
            setFlex(lsId, "justify_content", "flex_end");
            setFlex(lsId, "align_items", "center");
            setFlex(lsId, "padding_bottom", 4);
            var ratio = OklchEngine.contrastRatio(shadeHex, "#ffffff");
            var level = OklchEngine.contrastLevel(ratio);
            var badgeText = "";
            if (ratio >= 7.0) badgeText = "AAA";
            else if (ratio >= 4.5) badgeText = "AA";
            else if (ratio >= 3.0) badgeText = ratio.toFixed(1) + ":1";
            if (badgeText.length > 0) {
                createCol(lsId + "-pill", lsId);
                setFlex(lsId + "-pill", "height", 14);
                setFlex(lsId + "-pill", "padding_left", 6);
                setFlex(lsId + "-pill", "padding_right", 6);
                setFlex(lsId + "-pill", "justify_content", "center");
                setFlex(lsId + "-pill", "align_items", "center");
                setBackground(lsId + "-pill", ratio > 4.5 ? "#00000038" : "#ffffff88");
                setBorder(lsId + "-pill", ratio > 4.5 ? "#ffffff18" : "#00000014", 1, 7);
                createLabel(lsId + "-badge", badgeText, lsId + "-pill");
                setFontSize(lsId + "-badge", badgeText.length > 3 ? 7 : 8);
                setTextColor(lsId + "-badge", ratio > 4.5 ? "#ffffff" : "#111111");
            }
        }

        // Draw gamut + set faders if this palette is expanded
        if (p === expandedPalette) {
            var base = ramp[500];
            var oklch = OklchEngine.hexToOklch(base.hex);
            renderPaletteGamut(p, oklch.H, oklch.L, oklch.C);
            renderChromaGradient(p, oklch.H);
            setValue(rampId + "-h-fdr", oklch.H / 360);
            setValue(rampId + "-c-fdr", Math.min(oklch.C / 0.4, 1));
            updatePaletteValueDisplay(p, oklch, base.hex);
        }

        // Click dot or name → toggle expand (just visibility, no rebuild)
        (function(idx) {
            var toggleExpand = function() {
                if (expandedPalette === idx) {
                    // Collapse — shrink container back
                    applyPaletteExpandedLayout(idx, false);
                    expandedPalette = -1;
                } else {
                    // Collapse previous
                    if (expandedPalette >= 0) {
                        applyPaletteExpandedLayout(expandedPalette, false);
                    }
                    // Expand — grow container for editor content
                    expandedPalette = idx;
                    applyPaletteExpandedLayout(idx, true);
                    // Render gamut triangle for this palette
                    var pal = PaletteSystem.create(currentAccent, currentHarmony);
                    var pKey = paletteKeys[idx];
                    var base = pal[pKey][500];
                    var oklch = OklchEngine.hexToOklch(base.hex);
                    renderPaletteGamut(idx, oklch.H, oklch.L, oklch.C);
                    renderChromaGradient(idx, oklch.H);
                    setValue("ramp-" + idx + "-h-fdr", oklch.H / 360);
                    setValue("ramp-" + idx + "-c-fdr", Math.min(oklch.C / 0.4, 1));
                    updatePaletteValueDisplay(idx, oklch, base.hex);
                }
                updateLeftPanelScrollMetrics();
                layout();
            };
            on(rowId, "click", toggleExpand);
            on("ramp-" + idx + "-dot", "click", toggleExpand);
            on("ramp-" + idx + "-name", "click", toggleExpand);
            // Shade clicks also expand + position dot at that shade's color
            for (var si = 0; si < ShadeGenerator.STEPS.length; si++) {
                (function(paletteIdx, shadeStep) {
                    on("ramp-" + paletteIdx + "-s" + shadeStep, "click", function() {
                        var pal = PaletteSystem.create(currentAccent, currentHarmony);
                        var shade = pal[paletteKeys[paletteIdx]][ShadeGenerator.STEPS[shadeStep]];
                        var oklch = OklchEngine.hexToOklch(shade.hex);
                        // Expand if not already
                        if (expandedPalette >= 0 && expandedPalette !== paletteIdx) {
                            applyPaletteExpandedLayout(expandedPalette, false);
                        }
                        expandedPalette = paletteIdx;
                        applyPaletteExpandedLayout(paletteIdx, true);
                        renderPaletteGamut(paletteIdx, oklch.H, oklch.L, oklch.C);
                        renderChromaGradient(paletteIdx, oklch.H);
                        setValue("ramp-" + paletteIdx + "-h-fdr", oklch.H / 360);
                        setValue("ramp-" + paletteIdx + "-c-fdr", Math.min(oklch.C / 0.4, 1));
                        updatePaletteValueDisplay(paletteIdx, oklch, shade.hex);
                        updateLeftPanelScrollMetrics();
                        layout();
                    });
                })(idx, si);
            }
        })(p);

        // H/C slider change handlers — update gamut, dot, accent, and preview
        (function(idx, pKey) {
            function onPaletteSliderChange() {
                var h = getValue("ramp-" + idx + "-h-fdr") * 360;
                var c = getValue("ramp-" + idx + "-c-fdr") * 0.4;
                var mapped = OklchEngine.gamutMap(0.55, c, h);
                // Redraw gamut with dot + update C gradient for new hue
                renderPaletteGamut(idx, h, mapped.L, mapped.C);
                renderChromaGradient(idx, h);
                updatePaletteValueDisplay(idx, mapped, OklchEngine.oklchToHex(mapped.L, mapped.C, h));
                // Update the palette base color
                if (pKey === "accent") {
                    currentAccent = OklchEngine.oklchToHex(mapped.L, mapped.C, h);
                }
                // Rebuild full palette and apply all tokens to preview
                var palette = PaletteSystem.create(currentAccent, currentHarmony);
                // For non-accent palettes, regenerate that specific ramp with the slider values
                if (pKey !== "accent") {
                    palette[pKey] = ShadeGenerator.generateRamp(mapped.L, mapped.C, h);
                }
                var diff = PaletteSystem.toThemeDiff(palette);
                applyTokenDiff(diff);
                updateTokenSwatches();
                // Update the mini ramp swatches and dot color
                var ramp = palette[pKey];
                var steps = ShadeGenerator.STEPS;
                for (var s = 0; s < steps.length; s++) {
                    setBackground("ramp-" + idx + "-s" + s, ramp[steps[s]].hex);
                }
                setBackground("ramp-" + idx + "-dot", ramp[500].hex);
                // Update large shade swatches
                var stepIdxs = [0, 2, 4, 5, 7, 9];
                for (var ls = 0; ls < 6; ls++) {
                    setBackground("ramp-" + idx + "-lg-" + ls, ramp[steps[stepIdxs[ls]]].hex);
                }
            }
            on("ramp-" + idx + "-h-fdr", "change", onPaletteSliderChange);
            on("ramp-" + idx + "-c-fdr", "change", onPaletteSliderChange);
        })(p, paletteKeys[p]);
    }
    updateColorSectionLayout();
    updateLeftPanelScrollMetrics();
    if (tokenEditState.activeToken) rebuildPopupPalette();
}

// Render gamut triangle for a specific palette editor
// Render chroma gradient for a palette editor (gray → saturated at current hue)
function renderChromaGradient(paletteIdx, hue) {
    var cGradId = "ramp-" + paletteIdx + "-c-grad";
    canvasClear(cGradId);
    var w = 260;
    var steps = 32;
    for (var cs = 0; cs < steps; cs++) {
        var c = (cs / steps) * 0.4;
        var hex = OklchEngine.oklchToHex(0.6, c, hue);
        canvasRect(cGradId, (cs / steps) * w, 0, (w / steps) + 1, 12, hex);
    }
}

// fullRedraw=true renders the color grid; false only redraws dot overlay
var gamutCache = {};  // cache hue → avoid full redraw during drag
var gamutBoundaryCache = {};

function computeGamutBoundary(hue, steps) {
    var boundary = [];
    for (var bx = 0; bx <= steps; bx++) {
        var L = bx / steps;
        var lo = 0, hi = 0.4;
        for (var bi = 0; bi < 16; bi++) {
            var mid = (lo + hi) / 2;
            if (OklchEngine.isInGamut(L, mid, hue)) lo = mid; else hi = mid;
        }
        boundary.push(lo);
    }
    return boundary;
}

function renderPaletteGamutOverlay(paletteIdx, hue, dotL, dotC) {
    var overlayId = "ramp-" + paletteIdx + "-gamut-overlay";
    var w = 270, h = 130;
    canvasClear(overlayId);
    if (dotL === undefined || dotC === undefined) return;
    var dx = dotL * w;
    var dy = (1 - dotC / 0.4) * h;
    canvasStrokeLine(overlayId, dx, 0, dx, h, '#ffffff15', 0.5);
    canvasStrokeLine(overlayId, 0, dy, w, dy, '#ffffff15', 0.5);
    canvasFillCircle(overlayId, dx, dy, 9, '#00000040');
    canvasFillCircle(overlayId, dx, dy, 7, '#ffffffdd');
    canvasFillCircle(overlayId, dx, dy, 4, OklchEngine.oklchToHex(dotL, dotC, hue));
}

function renderPaletteGamut(paletteIdx, hue, dotL, dotC, fullRedraw) {
    var gamutId = "ramp-" + paletteIdx + "-gamut";
    var w = 270, h = 130;

    // Full redraw: render the entire color grid (expensive)
    if (fullRedraw !== false || gamutCache[paletteIdx] !== hue) {
        gamutCache[paletteIdx] = hue;
        canvasClear(gamutId);
        canvasRect(gamutId, 0, 0, w, h, APP_SURFACE);

        var bSteps = 720;
        var boundary = computeGamutBoundary(hue, bSteps);
        gamutBoundaryCache[paletteIdx] = boundary;

        // Render the gamut with per-column gradients and the exact boundary
        // height. This removes the stepped/pixelated top edge from the old
        // cell-grid renderer and tracks the HTML reference more closely.
        var cols = 360;
        var colW = w / cols;
        for (var gx = 0; gx < cols; gx++) {
            var L = gx / (cols - 1);
            var boundaryIdx = Math.min(bSteps, Math.max(0, Math.round((gx / (cols - 1)) * bSteps)));
            var maxC = boundary[boundaryIdx];
            var topY = (1 - maxC / 0.4) * h;
            if (topY > 0.35) {
                canvasRect(gamutId, gx * colW, 0, colW + 0.55, topY + 0.35, APP_SURFACE);
            }
            if (maxC > 0.001) {
                var topHex = OklchEngine.oklchToHex(L, Math.max(0.015, maxC * 0.96), hue);
                var bottomHex = OklchEngine.oklchToHex(L, 0.0001, hue);
                canvasSetLinearGradient(gamutId, gx * colW, topY, gx * colW, h, topHex, bottomHex);
                canvasBeginPath(gamutId);
                canvasMoveTo(gamutId, gx * colW, topY);
                canvasLineTo(gamutId, gx * colW + colW + 0.75, topY);
                canvasLineTo(gamutId, gx * colW + colW + 0.75, h);
                canvasLineTo(gamutId, gx * colW, h);
                canvasClosePath(gamutId);
                canvasFillPath(gamutId);
                canvasClearGradient(gamutId);
            }
        }

        canvasSetLineJoin(gamutId, "round");
        canvasSetLineCap(gamutId, "round");
        canvasSetStrokeColor(gamutId, '#ffffff12');
        canvasSetLineWidth(gamutId, 2.4);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, (1 - boundary[0] / 0.4) * h);
        for (var bx2 = 1; bx2 <= bSteps; bx2++) {
            canvasLineTo(gamutId, (bx2 / bSteps) * w, (1 - boundary[bx2] / 0.4) * h);
        }
        canvasStrokePath(gamutId);

        canvasSetStrokeColor(gamutId, '#ffffff33');
        canvasSetLineWidth(gamutId, 1.05);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, (1 - boundary[0] / 0.4) * h);
        for (var bx2a = 1; bx2a <= bSteps; bx2a++) {
            canvasLineTo(gamutId, (bx2a / bSteps) * w, (1 - boundary[bx2a] / 0.4) * h);
        }
        canvasStrokePath(gamutId);

        canvasSetStrokeColor(gamutId, '#ffffff10');
        canvasSetLineWidth(gamutId, 0.8);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, h);
        canvasLineTo(gamutId, 0, (1 - boundary[0] / 0.4) * h);
        for (var bx3 = 1; bx3 <= bSteps; bx3++) {
            canvasLineTo(gamutId, (bx3 / bSteps) * w, (1 - boundary[bx3] / 0.4) * h);
        }
        canvasLineTo(gamutId, w, h);
        canvasStrokePath(gamutId);
    }

    renderPaletteGamutOverlay(paletteIdx, hue, dotL, dotC);
}

buildShadeRamps();

// "Generate Opposite Mode" button below palette rows
createCol("gen-opposite-btn", "color-section");
setFlex("gen-opposite-btn", "height", 32);
setFlex("gen-opposite-btn", "justify_content", "center");
setFlex("gen-opposite-btn", "align_items", "center");
setBackground("gen-opposite-btn", APP_ACCENT);
setBorder("gen-opposite-btn", APP_ACCENT, 0, 6);
createLabel("gen-opposite-lbl", "Generate Opposite Mode", "gen-opposite-btn");
setFontSize("gen-opposite-lbl", 11);
setTextColor("gen-opposite-lbl", "#ffffff");
setPointerEvents("gen-opposite-lbl", "none");
registerClick("gen-opposite-btn");
on("gen-opposite-btn", "click", function() {
    // Toggle dark/light mode
    var currentMode = 0;
    try { currentMode = getValue("mode-selector"); } catch(e) {}
    var newIdx = currentMode > 0.5 ? 0 : 1;
    setSelected("mode-selector", newIdx);
    var mode = newIdx === 0 ? "dark" : "light";
    setTheme(mode);
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    applyTokenDiff(PaletteSystem.toThemeDiff(palette));
    buildShadeRamps();
    updateTokenSwatches();
    showToast("Switched to " + (newIdx === 0 ? "Dark" : "Light") + " mode");
});

// #60: Save/Load palette buttons
createRow("palette-io-row", "color-section");
setFlex("palette-io-row", "height", 26);
setFlex("palette-io-row", "gap", 6);

createCol("palette-save-btn", "palette-io-row");
setFlex("palette-save-btn", "flex_grow", 1);
setFlex("palette-save-btn", "height", 26);
setFlex("palette-save-btn", "justify_content", "center");
setFlex("palette-save-btn", "align_items", "center");
setBorder("palette-save-btn", APP_BORDER, 1, 4);
createLabel("palette-save-lbl", "Save", "palette-save-btn");
setFontSize("palette-save-lbl", 10);
setPointerEvents("palette-save-lbl", "none");
registerClick("palette-save-btn");
on("palette-save-btn", "click", function() {
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    var data = JSON.stringify({ accent: currentAccent, harmony: currentHarmony, palette: palette });
    exec("cat > /tmp/pulp-palette.json << 'PULPEOF'\n" + data + "\nPULPEOF");
    showToast("Palette saved");
});

createCol("palette-load-btn", "palette-io-row");
setFlex("palette-load-btn", "flex_grow", 1);
setFlex("palette-load-btn", "height", 26);
setFlex("palette-load-btn", "justify_content", "center");
setFlex("palette-load-btn", "align_items", "center");
setBorder("palette-load-btn", APP_BORDER, 1, 4);
createLabel("palette-load-lbl", "Load", "palette-load-btn");
setFontSize("palette-load-lbl", 10);
setPointerEvents("palette-load-lbl", "none");
registerClick("palette-load-btn");
on("palette-load-btn", "click", function() {
    var json = exec("cat /tmp/pulp-palette.json 2>/dev/null");
    if (json && json.length > 10) {
        var data = JSON.parse(json);
        if (data.accent) currentAccent = data.accent;
        if (data.harmony) currentHarmony = data.harmony;
        var palette = PaletteSystem.create(currentAccent, currentHarmony);
        applyTokenDiff(PaletteSystem.toThemeDiff(palette));
        buildShadeRamps();
        updateTokenSwatches();
        showToast("Palette loaded");
        layout();
    } else {
        showToast("No saved palette found");
    }
});

updateTokenFilterLayout("");
updateLeftPanelScrollMetrics();

// ── Color Picker (legacy — hidden, replaced by inline palette editor)
var pickerVisible = false;
var pickerColor = { L: 0, C: 0, H: 0, hex: '#000000' };

createCol("color-picker", "");
setFlex("color-picker", "width", 280);
setFlex("color-picker", "height", 200);
setFlex("color-picker", "padding", 12);
setFlex("color-picker", "gap", 8);
setBackground("color-picker", APP_PANEL);
setBorder("color-picker", APP_BORDER, 1, 8);
setBoxShadow("color-picker", 0, 4, 12, 0, "#00000060");
setVisible("color-picker", false);

// Color preview swatch
createCol("picker-preview", "color-picker");
setFlex("picker-preview", "height", 40);
setBorder("picker-preview", APP_BORDER, 1, 6);

// OKLCH values row
createRow("picker-values", "color-picker");
setFlex("picker-values", "height", 16);
setFlex("picker-values", "gap", 12);
setFlex("picker-values", "align_items", "center");

createLabel("picker-l-label", "L: 0.00", "picker-values");
setFontSize("picker-l-label", 10);
setFlex("picker-l-label", "width", 60);

createLabel("picker-c-label", "C: 0.000", "picker-values");
setFontSize("picker-c-label", 10);
setFlex("picker-c-label", "width", 70);

createLabel("picker-h-label", "H: 0.0", "picker-values");
setFontSize("picker-h-label", 10);
setFlex("picker-h-label", "width", 60);

// Hex value
createRow("picker-hex-row", "color-picker");
setFlex("picker-hex-row", "height", 20);
setFlex("picker-hex-row", "align_items", "center");

createLabel("picker-hex", "#000000", "picker-hex-row");
setFontSize("picker-hex", 12);

// H slider
createRow("picker-h-row", "color-picker");
setFlex("picker-h-row", "height", 20);
setFlex("picker-h-row", "gap", 6);
setFlex("picker-h-row", "align_items", "center");

createLabel("picker-h-lbl", "H", "picker-h-row");
setFontSize("picker-h-lbl", 10);
setFlex("picker-h-lbl", "width", 14);

createFader("picker-h-fader", "horizontal", "picker-h-row");
setFlex("picker-h-fader", "flex_grow", 1);
setFlex("picker-h-fader", "height", 16);

// C slider
createRow("picker-c-row", "color-picker");
setFlex("picker-c-row", "height", 20);
setFlex("picker-c-row", "gap", 6);
setFlex("picker-c-row", "align_items", "center");

createLabel("picker-c-lbl", "C", "picker-c-row");
setFontSize("picker-c-lbl", 10);
setFlex("picker-c-lbl", "width", 14);

createFader("picker-c-fader", "horizontal", "picker-c-row");
setFlex("picker-c-fader", "flex_grow", 1);
setFlex("picker-c-fader", "height", 16);

// L slider
createRow("picker-l-row", "color-picker");
setFlex("picker-l-row", "height", 20);
setFlex("picker-l-row", "gap", 6);
setFlex("picker-l-row", "align_items", "center");

createLabel("picker-l-lbl", "L", "picker-l-row");
setFontSize("picker-l-lbl", 10);
setFlex("picker-l-lbl", "width", 14);

createFader("picker-l-fader", "horizontal", "picker-l-row");
setFlex("picker-l-fader", "flex_grow", 1);
setFlex("picker-l-fader", "height", 16);

function showColorPicker(hex) {
    var oklch = OklchEngine.hexToOklch(hex);
    pickerColor = { L: oklch.L, C: oklch.C, H: oklch.H, hex: hex };

    setBackground("picker-preview", hex);
    setText("picker-l-label", "L: " + oklch.L.toFixed(2));
    setText("picker-c-label", "C: " + oklch.C.toFixed(3));
    setText("picker-h-label", "H: " + oklch.H.toFixed(1));
    setText("picker-hex", hex);
    setValue("picker-h-fader", oklch.H / 360);
    setValue("picker-c-fader", Math.min(oklch.C / 0.4, 1));
    setValue("picker-l-fader", oklch.L);

    setVisible("color-picker", true);
    pickerVisible = true;
    layout();
}

function hideColorPicker() {
    setVisible("color-picker", false);
    pickerVisible = false;
    layout();
}

function updatePickerFromSliders() {
    var h = getValue("picker-h-fader") * 360;
    var c = getValue("picker-c-fader") * 0.4;
    var l = getValue("picker-l-fader");
    var mapped = OklchEngine.gamutMap(l, c, h);
    var hex = OklchEngine.oklchToHex(mapped.L, mapped.C, mapped.H);

    pickerColor = { L: mapped.L, C: mapped.C, H: mapped.H, hex: hex };
    setBackground("picker-preview", hex);
    setText("picker-l-label", "L: " + mapped.L.toFixed(2));
    setText("picker-c-label", "C: " + mapped.C.toFixed(3));
    setText("picker-h-label", "H: " + mapped.H.toFixed(1));
    setText("picker-hex", hex);
}

on("picker-h-fader", "change", function() { updatePickerFromSliders(); });
on("picker-c-fader", "change", function() { updatePickerFromSliders(); });
on("picker-l-fader", "change", function() { updatePickerFromSliders(); });

// ═══════════════════════════════════════════════════════════════════
// D1: Token Palette Picker Popup
// ═══════════════════════════════════════════════════════════════════
var TOKEN_POPUP_W = 280;

// Click-outside backdrop
createCol("tp-backdrop", "");
setPosition("tp-backdrop", "absolute");
setTop("tp-backdrop", 0);
setLeft("tp-backdrop", 0);
setFlex("tp-backdrop", "width", 9999);
setFlex("tp-backdrop", "height", 9999);
setZIndex("tp-backdrop", 99);
setVisible("tp-backdrop", false);
registerClick("tp-backdrop");
on("tp-backdrop", "click", function() { closeTokenPopup(); });

// Popup container
createCol("token-popup", "");
setPosition("token-popup", "absolute");
setFlex("token-popup", "width", TOKEN_POPUP_W);
setFlex("token-popup", "padding", 10);
setFlex("token-popup", "gap", 6);
setBackground("token-popup", APP_PANEL);
setBorder("token-popup", APP_BORDER, 1, 8);
setBoxShadow("token-popup", 0, 8, 24, 0, "#000000a0");
setZIndex("token-popup", 100);
setVisible("token-popup", false);

// Header row: token name + close
createRow("tp-header", "token-popup");
setFlex("tp-header", "height", 22);
setFlex("tp-header", "align_items", "center");
createLabel("tp-token-name", "—", "tp-header");
setFontSize("tp-token-name", 11);
setTextColor("tp-token-name", APP_ACCENT);
setFlex("tp-token-name", "flex_grow", 1);

createCol("tp-close", "tp-header");
setFlex("tp-close", "width", 20);
setFlex("tp-close", "height", 20);
setFlex("tp-close", "justify_content", "center");
setFlex("tp-close", "align_items", "center");
createLabel("tp-close-lbl", "x", "tp-close");
setFontSize("tp-close-lbl", 11);
registerClick("tp-close");
on("tp-close", "click", function() { closeTokenPopup(); });

// Undo / Redo / Reset row
createRow("tp-undo-row", "token-popup");
setFlex("tp-undo-row", "height", 22);
setFlex("tp-undo-row", "gap", 4);

var tpUndoBtns = ["Undo", "Redo", "Reset"];
for (var ub = 0; ub < tpUndoBtns.length; ub++) {
    var ubId = "tp-btn-" + ub;
    createCol(ubId, "tp-undo-row");
    setFlex(ubId, "flex_grow", 1);
    setFlex(ubId, "height", 22);
    setFlex(ubId, "justify_content", "center");
    setFlex(ubId, "align_items", "center");
    setBorder(ubId, APP_BORDER, 1, 4);
    createLabel(ubId + "-lbl", tpUndoBtns[ub], ubId);
    setFontSize(ubId + "-lbl", 9);
    registerClick(ubId);
}

on("tp-btn-0", "click", function() { // Undo
    if (!tokenEditState.activeToken) return;
    var h = tokenHistory(tokenEditState.activeToken);
    if (h.cursor > 0) {
        h.cursor--;
        var hex = h.stack[h.cursor];
        var obj = { colors: {} };
        obj.colors[tokenEditState.activeToken] = hex;
        applyTokenDiff(JSON.stringify(obj));
        pushThemeSnapshot();
        tokenEditState.modified[tokenEditState.activeToken] = (hex !== h.original);
        if (!tokenEditState.modified[tokenEditState.activeToken])
            delete tokenEditState.modified[tokenEditState.activeToken];
        updateTokenSwatches();
        updateModifiedCount();
        updateAllTokenNameDisplays();
        updatePopupState(tokenEditState.activeToken);
        layout();
    }
});

on("tp-btn-1", "click", function() { // Redo
    if (!tokenEditState.activeToken) return;
    var h = tokenHistory(tokenEditState.activeToken);
    if (h.cursor < h.stack.length - 1) {
        h.cursor++;
        var hex = h.stack[h.cursor];
        var obj = { colors: {} };
        obj.colors[tokenEditState.activeToken] = hex;
        applyTokenDiff(JSON.stringify(obj));
        pushThemeSnapshot();
        tokenEditState.modified[tokenEditState.activeToken] = (hex !== h.original);
        if (!tokenEditState.modified[tokenEditState.activeToken])
            delete tokenEditState.modified[tokenEditState.activeToken];
        updateTokenSwatches();
        updateModifiedCount();
        updateAllTokenNameDisplays();
        updatePopupState(tokenEditState.activeToken);
        layout();
    }
});

on("tp-btn-2", "click", function() { // Reset
    if (!tokenEditState.activeToken) return;
    var h = tokenHistory(tokenEditState.activeToken);
    var orig = h.original;
    h.stack = [orig];
    h.cursor = 0;
    delete tokenEditState.modified[tokenEditState.activeToken];
    var obj = { colors: {} };
    obj.colors[tokenEditState.activeToken] = orig;
    applyTokenDiff(JSON.stringify(obj));
    pushThemeSnapshot();
    updateTokenSwatches();
    updateModifiedCount();
    updateAllTokenNameDisplays();
    updatePopupState(tokenEditState.activeToken);
    layout();
});

// Hex input in popup
createRow("tp-hex-row", "token-popup");
setFlex("tp-hex-row", "height", 24);
setFlex("tp-hex-row", "gap", 6);
setFlex("tp-hex-row", "align_items", "center");
createLabel("tp-hex-lbl", "Hex", "tp-hex-row");
setFontSize("tp-hex-lbl", 10);
setTextColor("tp-hex-lbl", APP_TEXT_DIM);
createTextEditor("tp-hex-input", "tp-hex-row");
setFlex("tp-hex-input", "flex_grow", 1);
setFlex("tp-hex-input", "height", 22);
setFontSize("tp-hex-input", 11);
on("tp-hex-input", "return", function(text) {
    var hex = text.trim();
    if (!tokenEditState.activeToken) return;
    if (!/^#[0-9a-fA-F]{6}$/.test(hex)) return;
    applyTokenColor(tokenEditState.activeToken, hex);
});

// #62: Token alpha (opacity) fader
createRow("tp-alpha-row", "token-popup");
setFlex("tp-alpha-row", "height", 20);
setFlex("tp-alpha-row", "gap", 6);
setFlex("tp-alpha-row", "align_items", "center");
createLabel("tp-alpha-lbl", "Alpha", "tp-alpha-row");
setFontSize("tp-alpha-lbl", 9);
setTextColor("tp-alpha-lbl", APP_TEXT_DIM);
setFlex("tp-alpha-lbl", "width", 30);
createFader("tp-alpha-fdr", "horizontal", "tp-alpha-row");
setFlex("tp-alpha-fdr", "flex_grow", 1);
setFlex("tp-alpha-fdr", "height", 14);
setValue("tp-alpha-fdr", 1.0);
createLabel("tp-alpha-val", "1.0", "tp-alpha-row");
setFontSize("tp-alpha-val", 9);
setFlex("tp-alpha-val", "width", 24);
on("tp-alpha-fdr", "change", function() {
    var alpha = getValue("tp-alpha-fdr");
    setText("tp-alpha-val", alpha.toFixed(1));
    if (tokenEditState.activeToken) {
        // Apply opacity to the token swatch
        var swId = tokenEditState.activeSwatchId;
        if (swId) setOpacity(swId, alpha);
    }
});

// 5 palette shade grids
createCol("tp-palettes", "token-popup");
setFlex("tp-palettes", "gap", 4);

for (var pp = 0; pp < paletteNames.length; pp++) {
    var ppId = "tp-pal-" + pp;
    createCol(ppId, "tp-palettes");
    setFlex(ppId, "gap", 1);

    createLabel(ppId + "-title", paletteNames[pp].toUpperCase(), ppId);
    setFontSize(ppId + "-title", 8);
    setTextColor(ppId + "-title", APP_TEXT_DIM);
    setFlex(ppId + "-title", "height", 12);

    createRow(ppId + "-row", ppId);
    setFlex(ppId + "-row", "gap", 2);
    setFlex(ppId + "-row", "height", 18);

    for (var ps = 0; ps < ShadeGenerator.STEPS.length; ps++) {
        var psId = ppId + "-s" + ps;
        createCol(psId, ppId + "-row");
        setFlex(psId, "flex_grow", 1);
        setFlex(psId, "height", 18);
        setBorder(psId, APP_BORDER, 0, 2);
        registerClick(psId);
        (function(palIdx, shadeIdx) {
            on("tp-pal-" + palIdx + "-s" + shadeIdx, "click", function() {
                if (!tokenEditState.activeToken) return;
                var palette = PaletteSystem.create(currentAccent, currentHarmony);
                var pKey = paletteKeys[palIdx];
                var hex = palette[pKey][ShadeGenerator.STEPS[shadeIdx]].hex;
                applyTokenColor(tokenEditState.activeToken, hex);
            });
        })(pp, ps);
    }
}

// Custom HCL section (expandable)
var tpCustomOpen = false;
createRow("tp-custom-toggle", "token-popup");
setFlex("tp-custom-toggle", "height", 22);
setFlex("tp-custom-toggle", "align_items", "center");
registerClick("tp-custom-toggle");
createLabel("tp-custom-lbl", "Custom color  >", "tp-custom-toggle");
setFontSize("tp-custom-lbl", 10);
setTextColor("tp-custom-lbl", APP_TEXT_DIM);

createCol("tp-custom", "token-popup");
setFlex("tp-custom", "gap", 4);
setVisible("tp-custom", false);

// D2: Gamut triangle canvas — rendered as grid of colored rectangles
var GAMUT_W = 50;  // columns (lightness steps)
var GAMUT_H = 30;  // rows (chroma steps)
var GAMUT_MAX_C = 0.4;
var gamutHue = 0;

createCanvas("tp-gamut", "tp-custom");
setFlex("tp-gamut", "width", 250);
setFlex("tp-gamut", "height", 120);

function renderGamutTriangle(hue) {
    gamutHue = hue;
    canvasClear("tp-gamut");
    var w = 250, h = 120;
    // Use gradient strips: one vertical gradient per lightness column
    var cols = 120;
    var colW = w / cols;
    var boundary = [];
    for (var gx = 0; gx < cols; gx++) {
        var L = gx / (cols - 1);
        // Find max in-gamut chroma at this lightness via binary search
        var maxC = 0;
        var lo = 0, hi = GAMUT_MAX_C;
        for (var bi = 0; bi < 16; bi++) {
            var mid = (lo + hi) / 2;
            if (OklchEngine.isInGamut(L, mid, hue)) lo = mid;
            else hi = mid;
        }
        maxC = lo;
        boundary.push(maxC);
        // Draw gradient strip from maxC (top, saturated) to 0 (bottom, gray)
        var topHex = OklchEngine.oklchToHex(L, maxC, hue);
        var botHex = OklchEngine.oklchToHex(L, 0, hue);
        var gamutH = (maxC / GAMUT_MAX_C) * h;
        // Gradient fill for in-gamut area
        if (gamutH > 1) {
            canvasSetLinearGradient("tp-gamut", gx * colW, h - gamutH, gx * colW, h, topHex, botHex);
            canvasBeginPath("tp-gamut");
            canvasMoveTo("tp-gamut", gx * colW, h - gamutH);
            canvasLineTo("tp-gamut", gx * colW + colW + 0.5, h - gamutH);
            canvasLineTo("tp-gamut", gx * colW + colW + 0.5, h);
            canvasLineTo("tp-gamut", gx * colW, h);
            canvasClosePath("tp-gamut");
            canvasFillPath("tp-gamut");
            canvasClearGradient("tp-gamut");
        }
        // Out-of-gamut area (dark background above the gamut boundary)
        if (h - gamutH > 0) {
            canvasRect("tp-gamut", gx * colW, 0, colW + 0.5, h - gamutH, APP_SURFACE);
        }
    }
    canvasSetLineJoin("tp-gamut", "round");
    canvasSetLineCap("tp-gamut", "round");
    canvasSetStrokeColor("tp-gamut", '#ffffff14');
    canvasSetLineWidth("tp-gamut", 2.0);
    canvasBeginPath("tp-gamut");
    canvasMoveTo("tp-gamut", 0, h - ((boundary[0] || 0) / GAMUT_MAX_C) * h);
    for (var bx = 1; bx < boundary.length; bx++) {
        canvasLineTo("tp-gamut", (bx / (cols - 1)) * w, h - (boundary[bx] / GAMUT_MAX_C) * h);
    }
    canvasStrokePath("tp-gamut");
    canvasSetStrokeColor("tp-gamut", '#ffffff30');
    canvasSetLineWidth("tp-gamut", 0.95);
    canvasBeginPath("tp-gamut");
    canvasMoveTo("tp-gamut", 0, h - ((boundary[0] || 0) / GAMUT_MAX_C) * h);
    for (var bx2 = 1; bx2 < boundary.length; bx2++) {
        canvasLineTo("tp-gamut", (bx2 / (cols - 1)) * w, h - (boundary[bx2] / GAMUT_MAX_C) * h);
    }
    canvasStrokePath("tp-gamut");
}

// Gamut click handler — map click position to L,C and apply
registerClick("tp-gamut");
on("tp-gamut", "click", function() {
    // Use the current fader values as approximation since we can't get click position
    // The real interaction needs drag-to-JS (future C++ bridge addition)
    // For now, the H fader + L/C faders below provide the interaction
});

// H (hue) fader
createRow("tp-hue-row", "tp-custom");
setFlex("tp-hue-row", "height", 20);
setFlex("tp-hue-row", "gap", 6);
setFlex("tp-hue-row", "align_items", "center");
createLabel("tp-h-fader-lbl", "H", "tp-hue-row");
setFontSize("tp-h-fader-lbl", 10);
setFlex("tp-h-fader-lbl", "width", 14);
createFader("tp-h-fader", "horizontal", "tp-hue-row");
setFlex("tp-h-fader", "flex_grow", 1);
setFlex("tp-h-fader", "height", 16);

// C (chroma) fader
createRow("tp-chroma-row", "tp-custom");
setFlex("tp-chroma-row", "height", 20);
setFlex("tp-chroma-row", "gap", 6);
setFlex("tp-chroma-row", "align_items", "center");
createLabel("tp-c-fader-lbl", "C", "tp-chroma-row");
setFontSize("tp-c-fader-lbl", 10);
setFlex("tp-c-fader-lbl", "width", 14);
createFader("tp-c-fader", "horizontal", "tp-chroma-row");
setFlex("tp-c-fader", "flex_grow", 1);
setFlex("tp-c-fader", "height", 16);

// L (lightness) fader
createRow("tp-light-row", "tp-custom");
setFlex("tp-light-row", "height", 20);
setFlex("tp-light-row", "gap", 6);
setFlex("tp-light-row", "align_items", "center");
createLabel("tp-l-fader-lbl", "L", "tp-light-row");
setFontSize("tp-l-fader-lbl", 10);
setFlex("tp-l-fader-lbl", "width", 14);
createFader("tp-l-fader", "horizontal", "tp-light-row");
setFlex("tp-l-fader", "flex_grow", 1);
setFlex("tp-l-fader", "height", 16);

// OKLCH value display
createRow("tp-oklch-vals", "tp-custom");
setFlex("tp-oklch-vals", "height", 16);
setFlex("tp-oklch-vals", "gap", 8);
createLabel("tp-oklch-display", "L: 0.50  C: 0.100  H: 180", "tp-oklch-vals");
setFontSize("tp-oklch-display", 9);
setTextColor("tp-oklch-display", APP_TEXT_DIM);

function onTpHclChange() {
    if (!tokenEditState.activeToken) return;
    var h = getValue("tp-h-fader") * 360;
    var c = getValue("tp-c-fader") * 0.4;
    var l = getValue("tp-l-fader");
    var mapped = OklchEngine.gamutMap(l, c, h);
    var hex = OklchEngine.oklchToHex(mapped.L, mapped.C, mapped.H);
    setText("tp-oklch-display", "L: " + mapped.L.toFixed(2) + "  C: " + mapped.C.toFixed(3) + "  H: " + mapped.H.toFixed(1));
    applyTokenColor(tokenEditState.activeToken, hex);
    // Redraw gamut triangle if hue changed
    if (Math.abs(h - gamutHue) > 1) renderGamutTriangle(h);
}
on("tp-h-fader", "change", function() { onTpHclChange(); });
on("tp-c-fader", "change", function() { onTpHclChange(); });
on("tp-l-fader", "change", function() { onTpHclChange(); });

on("tp-custom-toggle", "click", function() {
    tpCustomOpen = !tpCustomOpen;
    setVisible("tp-custom", tpCustomOpen);
    setText("tp-custom-lbl", tpCustomOpen ? "Custom color  v" : "Custom color  >");
    if (tpCustomOpen && tokenEditState.activeToken) {
        var hex = (JSON.parse(getThemeJson()).colors || {})[tokenEditState.activeToken] || '#808080';
        var oklch = OklchEngine.hexToOklch(hex);
        renderGamutTriangle(oklch.H);
    }
    layout();
});

// ── Popup open/close/update functions ───────────────────────────
function rebuildPopupPalette() {
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    for (var pp = 0; pp < paletteKeys.length; pp++) {
        var ramp = palette[paletteKeys[pp]];
        for (var ps = 0; ps < ShadeGenerator.STEPS.length; ps++) {
            setBackground("tp-pal-" + pp + "-s" + ps, ramp[ShadeGenerator.STEPS[ps]].hex);
        }
    }
}

function updatePopupState(tokenName) {
    var themeColors = JSON.parse(getThemeJson()).colors || {};
    var hex = themeColors[tokenName] || '#000000';
    setText("tp-hex-input", hex);
    // D2: Sync HCL faders and gamut triangle
    if (tpCustomOpen) {
        var oklch = OklchEngine.hexToOklch(hex);
        setValue("tp-h-fader", oklch.H / 360);
        setValue("tp-c-fader", Math.min(oklch.C / 0.4, 1));
        setValue("tp-l-fader", oklch.L);
        setText("tp-oklch-display", "L: " + oklch.L.toFixed(2) + "  C: " + oklch.C.toFixed(3) + "  H: " + oklch.H.toFixed(1));
        if (Math.abs(oklch.H - gamutHue) > 1) renderGamutTriangle(oklch.H);
    }
    // Undo/redo button opacity
    var h = tokenHistory(tokenName);
    setOpacity("tp-btn-0", h.cursor > 0 ? 1.0 : 0.3);
    setOpacity("tp-btn-1", h.cursor < h.stack.length - 1 ? 1.0 : 0.3);
    setOpacity("tp-btn-2", tokenEditState.modified[tokenName] ? 1.0 : 0.3);
}

function openTokenPopup(tokenName, swatchId, gIdx, tIdx) {
    tokenHistory(tokenName);
    tokenEditState.activeToken = tokenName;
    tokenEditState.activeSwatchId = swatchId;
    setText("tp-token-name", tokenName);
    rebuildPopupPalette();
    updatePopupState(tokenName);
    layout();
    var rect = getLayoutRect(swatchId);
    var popX = rect.right + 4;
    var popY = rect.y;
    // Clamp to window
    if (popX + TOKEN_POPUP_W > 1100) popX = rect.x - TOKEN_POPUP_W - 4;
    if (popY + 380 > 700) popY = 700 - 380 - 8;
    if (popX < 4) popX = 4;
    if (popY < 4) popY = 4;
    setTop("token-popup", popY);
    setLeft("token-popup", popX);
    setVisible("tp-backdrop", true);
    setVisible("token-popup", true);
    layout();
}

function closeTokenPopup() {
    tokenEditState.activeToken = null;
    tokenEditState.activeSwatchId = null;
    setVisible("token-popup", false);
    setVisible("tp-backdrop", false);
    layout();
}

// ── Accent hue slider handler ────────────────────────────────────
on("accent-hue", "change", function(val) {
    var hue = val * 360;
    var oklch = OklchEngine.hexToOklch(currentAccent);
    currentAccent = OklchEngine.oklchToHex(oklch.L, oklch.C, hue);

    // Rebuild shade ramps with new hue
    buildShadeRamps();

    // Apply new palette as theme
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    var diff = PaletteSystem.toThemeDiff(palette);
    applyTokenDiff(diff);
    updateTokenSwatches();
    layout();
});

// Harmony selector handler
// Harmony change: update palette colors in-place (no widget tree mutation)
on("harmony-selector", "select", function(idx) {
    var modes = ["monochromatic", "analogous", "complementary", "splitComplementary", "none"];
    if (idx >= 0 && idx < modes.length) {
        currentHarmony = modes[idx];
        setSelected("harmony-selector", idx);
        // Just update colors — don't rebuild widget tree during mouse event
        var palette = PaletteSystem.create(currentAccent, currentHarmony);
        applyTokenDiff(PaletteSystem.toThemeDiff(palette));
        updateTokenSwatches();
        // Update shade ramp colors in-place
        var steps = ShadeGenerator.STEPS;
        for (var p = 0; p < paletteKeys.length; p++) {
            var ramp = palette[paletteKeys[p]];
            for (var s = 0; s < steps.length; s++) {
                setBackground("ramp-" + p + "-s" + s, ramp[steps[s]].hex);
            }
            setBackground("ramp-" + p + "-dot", ramp[500].hex);
        }
        // Close any expanded editor since palette changed
        if (expandedPalette >= 0) {
            applyPaletteExpandedLayout(expandedPalette, false);
            expandedPalette = -1;
            updateLeftPanelScrollMetrics();
        }
        layout();
    }
});

// Dark/Light mode handler
on("mode-selector", "select", function(idx) {
    var mode = idx === 0 ? "dark" : "light";
    setSelected("mode-selector", idx);
    setTheme(mode);
    // Update shade ramp colors in-place
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    applyTokenDiff(PaletteSystem.toThemeDiff(palette));
    updateTokenSwatches();
    var steps = ShadeGenerator.STEPS;
    for (var p = 0; p < paletteKeys.length; p++) {
        var ramp = palette[paletteKeys[p]];
        for (var s = 0; s < steps.length; s++) {
            setBackground("ramp-" + p + "-s" + s, ramp[steps[s]].hex);
        }
        setBackground("ramp-" + p + "-dot", ramp[500].hex);
    }
    if (expandedPalette >= 0) {
        applyPaletteExpandedLayout(expandedPalette, false);
        expandedPalette = -1;
        updateLeftPanelScrollMetrics();
    }
    pushThemeSnapshot();
    layout();
});

// ── CENTER PANEL (Preview) ───────────────────────────────────────
createCol("center-panel", "main-area");
setFlex("center-panel", "flex_grow", 1);
setFlex("center-panel", "flex_shrink", 1);
setFlex("center-panel", "min_width", 420);  // Issue 5: prevent scrollbar behind content
setFlex("center-panel", "padding", 16);
setFlex("center-panel", "gap", 8);
setBackground("center-panel", APP_BG);

// Preview content area (scrollable, flush to top — no chrome title bar)
createScrollView("preview-scroll", "center-panel");
setFlex("preview-scroll", "flex_grow", 1);
setBackground("preview-scroll", APP_PANEL);
setBorder("preview-scroll", APP_BORDER, 0, 0);
setScrollContentSize("preview-scroll", 500, 1900);

createCol("preview-area", "preview-scroll");
setFlex("preview-area", "height", 1900);
setFlex("preview-area", "flex_shrink", 0);
setFlex("preview-area", "padding", 12);
setFlex("preview-area", "padding_right", 24);  // extra space for scrollbar
setFlex("preview-area", "gap", 10);

// Foundations section: bg swatches + text hierarchy
createLabel("foundations-header", "Foundations", "preview-area");
setFontSize("foundations-header", 11);
setTextColor("foundations-header", APP_TEXT_DIM);
setFlex("foundations-header", "height", 16);

// Background swatches row
createRow("bg-swatches", "preview-area");
setFlex("bg-swatches", "gap", 6);
setFlex("bg-swatches", "height", 32);
setFlex("bg-swatches", "align_items", "center");

var bgTokens = ["bg.primary", "bg.secondary", "bg.surface", "bg.elevated"];
var bgLabels = ["Primary", "Secondary", "Surface", "Elevated"];
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
createLabel("controls-header", "Controls", "preview-area");
setFontSize("controls-header", 11);
setTextColor("controls-header", APP_TEXT_DIM);
setFlex("controls-header", "height", 16);

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

createTextEditor("sample-placeholder", "input-row");
setPlaceholder("sample-placeholder", "Placeholder...");
setFlex("sample-placeholder", "width", 120);
setFlex("sample-placeholder", "height", 26);

createCombo("sample-combo", "input-row");
setItems("sample-combo", ["Select preset...", "Option A", "Option B"]);
setFlex("sample-combo", "width", 130);
setFlex("sample-combo", "height", 26);

// Data display: Waveform
createLabel("data-header", "Data Display", "preview-area");
setFontSize("data-header", 11);
setTextColor("data-header", APP_TEXT_DIM);
setFlex("data-header", "height", 16);

createWaveform("waveform", "preview-area");
setFlex("waveform", "height", 60);

var waveData = [];
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

// Layout section: 2x2 card grid
createLabel("layout-header", "LAYOUT", "preview-area");
setFontSize("layout-header", 10);
setTextColor("layout-header", APP_TEXT_DIM);
setFlex("layout-header", "height", 16);

// D3: Card grid matching HTML reference — Empty, Loading, Ready (OK badge), Error (! badge)
var cardDefs = [
    { id: "card-1", label: "Empty", bg: APP_PANEL, border: APP_BORDER, badge: null },
    { id: "card-2", label: "Loading", bg: APP_PANEL, border: APP_BORDER, badge: null, loading: true },
    { id: "card-3", label: "Ready", bg: APP_PANEL, border: '#4CAF50', badge: "OK", badgeColor: '#4CAF50' },
    { id: "card-4", label: "Error", bg: '#3a2020', border: '#e94560', badge: "!", badgeColor: '#e94560' }
];
var cardRows = [["card-grid-top", [0, 1]], ["card-grid-bot", [2, 3]]];
for (var cr = 0; cr < cardRows.length; cr++) {
    var crId = cardRows[cr][0];
    createRow(crId, "preview-area");
    setFlex(crId, "gap", 6);
    setFlex(crId, "height", 56);
    var indices = cardRows[cr][1];
    for (var ci = 0; ci < indices.length; ci++) {
        var cd = cardDefs[indices[ci]];
        createCol(cd.id, crId);
        setPosition(cd.id, "relative");
        setFlex(cd.id, "flex_grow", 1);
        setBackground(cd.id, cd.bg);
        setBorder(cd.id, cd.border, 1, 8);
        setFlex(cd.id, "padding", 6);
        setFlex(cd.id, "justify_content", "center");
        setFlex(cd.id, "align_items", "center");
        if (cd.loading) {
            createLabel(cd.id + "-spinner", "\u25DC", cd.id);
            setFontSize(cd.id + "-spinner", 12);
            setTextColor(cd.id + "-spinner", APP_ACCENT);
        } else {
            createLabel(cd.id + "-label", cd.label, cd.id);
            setFontSize(cd.id + "-label", 9);
            setTextColor(cd.id + "-label", cd.id === "card-4" ? "#F44336" : APP_TEXT_DIM);
        }
        if (cd.badge) {
            createCol(cd.id + "-badge", cd.id);
            setPosition(cd.id + "-badge", "absolute");
            setTop(cd.id + "-badge", 4);
            setRight(cd.id + "-badge", 4);
            setFlex(cd.id + "-badge", "height", 16);
            setFlex(cd.id + "-badge", "padding_left", 4);
            setFlex(cd.id + "-badge", "padding_right", 4);
            setFlex(cd.id + "-badge", "justify_content", "center");
            setFlex(cd.id + "-badge", "align_items", "center");
            setBackground(cd.id + "-badge", cd.badgeColor);
            setBorder(cd.id + "-badge", cd.badgeColor, 0, 3);
            createLabel(cd.id + "-badge-lbl", cd.badge, cd.id + "-badge");
            setFontSize(cd.id + "-badge-lbl", 7);
            setTextColor(cd.id + "-badge-lbl", "#ffffff");
        }
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

// Animate spinner character rotation
var spinnerFrames = ["\u25DC", "\u25DD", "\u25DE", "\u25DF"];
var spinnerIdx = 0;
function tickSpinner() {
    spinnerIdx = (spinnerIdx + 1) % spinnerFrames.length;
    setText("spinner-icon", spinnerFrames[spinnerIdx]);
    setText("card-2-spinner", spinnerFrames[spinnerIdx]);
    __requestFrame__(tickSpinner);
}
__requestFrame__(tickSpinner);

// ── Tab Bar (General / Audio / MIDI / About) ─────────────────────
createRow("tab-bar-preview", "preview-area");
setFlex("tab-bar-preview", "height", 28);
setFlex("tab-bar-preview", "gap", 0);
setFlex("tab-bar-preview", "align_items", "flex_end");
setBorder("tab-bar-preview", APP_BORDER, 0, 0);
createCol("tab-bar-preview-line", "tab-bar-preview");
setPosition("tab-bar-preview-line", "absolute");
setLeft("tab-bar-preview-line", 0);
setRight("tab-bar-preview-line", 0);
setBottom("tab-bar-preview-line", 0);
setFlex("tab-bar-preview-line", "height", 2);
setBackground("tab-bar-preview-line", previewThemeColor("divider", APP_BORDER));

var tabNames = ["General", "Audio", "MIDI", "About"];
for (var ti = 0; ti < tabNames.length; ti++) {
    var tabId = "ptab-" + ti;
    createCol(tabId, "tab-bar-preview");
    setPosition(tabId, "relative");
    setFlex(tabId, "height", 28);
    setFlex(tabId, "padding_left", 14);
    setFlex(tabId, "padding_right", 14);
    setFlex(tabId, "justify_content", "flex_end");
    setFlex(tabId, "align_items", "center");
    setFlex(tabId, "gap", 6);
    registerClick(tabId);

    createLabel(tabId + "-l", tabNames[ti], tabId);
    setFontSize(tabId + "-l", 11);
    setTextColor(tabId + "-l", ti === 0 ? APP_TEXT : APP_TEXT_DIM);
    setPointerEvents(tabId + "-l", "none");

    createCol(tabId + "-line", tabId);
    setFlex(tabId + "-line", "height", 2);
    setFlex(tabId + "-line", "width", 999);
    setBackground(tabId + "-line", ti === 0 ? APP_ACCENT : "transparent");

    (function(idx) {
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
refreshPreviewLayoutSection();
setPreviewActiveTab(0, true);

// ── Overlays (Static Preview) ────────────────────────────────────
createLabel("overlays-header", "Overlays", "preview-area");
setFontSize("overlays-header", 11);
setTextColor("overlays-header", APP_TEXT_DIM);

createRow("overlay-row", "preview-area");
setFlex("overlay-row", "gap", 8);
setFlex("overlay-row", "height", 96);

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

var ctxItems = ["Copy", "Paste", "---", "Rename", "Delete"];
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
        if (ctxItems[ci] === "Paste") {
            setBackground(cid, APP_ACCENT + "22");
            setTextColor(cid + "-l", APP_ACCENT);
        }
        if (ctxItems[ci] === "Delete") setTextColor(cid + "-l", "#f38ba8");
    }
}

// ── States ───────────────────────────────────────────────────────
createLabel("states-header", "States", "preview-area");
setFontSize("states-header", 11);
setTextColor("states-header", APP_TEXT_DIM);

createRow("states-row", "preview-area");
setFlex("states-row", "gap", 6);
setFlex("states-row", "height", 26);
setFlex("states-row", "align_items", "center");

var stateNames = ["Normal", "Hover", "Active", "Focus", "Disabled"];
var stateBgs   = ["#3a3a4c", "#4a4a5c", APP_ACCENT, "#3a3a4c", "#2a2a36"];
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
createLabel("effects-header", "Effects", "preview-area");
setFontSize("effects-header", 11);
setTextColor("effects-header", APP_TEXT_DIM);

createRow("effects-row", "preview-area");
setFlex("effects-row", "gap", 8);
setFlex("effects-row", "height", 40);
setFlex("effects-row", "align_items", "center");

var effectNames = ["Shadow", "Glow", "Blur", "Gradient"];
for (var ei = 0; ei < effectNames.length; ei++) {
    var eid = "effect-" + ei;
    createCol(eid, "effects-row");
    setFlex(eid, "flex_grow", 1);
    setFlex(eid, "height", 36);
    setBackground(eid, APP_PANEL);
    setBorder(eid, APP_BORDER, 1, 6);
    setFlex(eid, "justify_content", "center");
    setFlex(eid, "align_items", "center");
    createLabel(eid + "-l", effectNames[ei], eid);
    setFontSize(eid + "-l", 10);
}

// ── GPU Showcase ─────────────────────────────────────────────────
createLabel("showcase-header", "GPU Showcase", "preview-area");
setFontSize("showcase-header", 11);
setTextColor("showcase-header", APP_TEXT_DIM);

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

// Generate some spectrum data
var specData = [];
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
createLabel("waveform2-header", "Audio Waveform", "preview-area");
setFontSize("waveform2-header", 11);
setTextColor("waveform2-header", APP_TEXT_DIM);

createWaveform("waveform2", "preview-area");
setFlex("waveform2", "height", 60);

var wave2 = [];
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

// Chat messages (scrollable)
createScrollView("chat-messages", "chat-area");
setFlex("chat-messages", "flex_grow", 1);
setFlex("chat-messages", "padding_right", 10);
setScrollContentSize("chat-messages", 236, 400);  // #61: narrower to clear scrollbar

createLabel("welcome-msg", "Describe a visual style and the preview will update live.", "chat-messages");
setFontSize("welcome-msg", 11);
setFlex("welcome-msg", "height", 30);

createLabel("hint-msg", 'Try: "warm vintage" or "neon cyberpunk"', "chat-messages");
setFontSize("hint-msg", 10);
setTextColor("hint-msg", APP_TEXT_DIM);
setFlex("hint-msg", "height", 16);

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
// Issue 2: image upload via file dialog
var uploadedImagePath = "";
var uploadedImageName = "";

function clearUploadedImage() {
    uploadedImagePath = "";
    uploadedImageName = "";
    setText("chat-attachment-label", "");
    setVisible("chat-attachment-row", false);
    setFlex("chat-attachment-row", "height", 0);
    layout();
}

function shellQuote(s) {
    return "'" + String(s || "").split("'").join("'\\''") + "'";
}

function buildAiCliCommand(promptFile, model, provider, reasoningEffort) {
    var aiCli = "";
    try { aiCli = getAICli(); } catch (e) {}
    var bridgeDefaultCli = "claude --print --model {model}";
    if (!provider || provider.length === 0) provider = "claude";
    if (!aiCli || aiCli.length === 0 || (provider === "codex" && aiCli === bridgeDefaultCli)) {
        if (provider === "codex") {
            aiCli = "codex exec - --model {model} --skip-git-repo-check --sandbox read-only --color never --ephemeral";
            if (reasoningEffort && reasoningEffort.length > 0) {
                aiCli += " -c model_reasoning_effort={reasoning_effort}";
            }
            aiCli += " -o {output_file} >/dev/null";
        } else {
            aiCli = bridgeDefaultCli;
        }
    }

    var usesPromptFile = aiCli.indexOf("{prompt_file}") >= 0;
    var usesModel = aiCli.indexOf("{model}") >= 0;
    var usesProvider = aiCli.indexOf("{provider}") >= 0;
    var usesReasoningEffort = aiCli.indexOf("{reasoning_effort}") >= 0;
    var usesOutputFile = aiCli.indexOf("{output_file}") >= 0;
    var outputFile = promptFile + ".out.txt";
    var cmd = aiCli;
    if (usesPromptFile) cmd = cmd.split("{prompt_file}").join(shellQuote(promptFile));
    if (usesModel) cmd = cmd.split("{model}").join(shellQuote(model));
    if (usesProvider) cmd = cmd.split("{provider}").join(shellQuote(provider || ""));
    if (usesReasoningEffort) cmd = cmd.split("{reasoning_effort}").join(shellQuote(reasoningEffort || ""));
    if (usesOutputFile) cmd = cmd.split("{output_file}").join(shellQuote(outputFile));
    if (!usesPromptFile) {
        cmd = "cat " + shellQuote(promptFile) + " | " + cmd;
    }
    if (usesOutputFile) {
        return cmd + "; __pulp_status=$?; if [ -f " + shellQuote(outputFile) + " ]; then cat " + shellQuote(outputFile) + "; fi; rm -f " + shellQuote(promptFile) + " " + shellQuote(outputFile) + "; exit $__pulp_status";
    }
    return cmd + "; rm -f " + shellQuote(promptFile);
}

function setUploadedImage(path) {
    uploadedImagePath = path || "";
    var parts = uploadedImagePath.split("/");
    uploadedImageName = parts.length > 0 ? parts[parts.length - 1] : uploadedImagePath;
    if (!uploadedImagePath) {
        clearUploadedImage();
        return;
    }
    setText("chat-attachment-label", uploadedImageName);
    setVisible("chat-attachment-row", true);
    setFlex("chat-attachment-row", "height", 22);
    layout();
}

on("chat-attachment-clear", "click", function() {
    clearUploadedImage();
});

function updateChatInputSizing(text) {
    var value = text || "";
    var explicitLines = value.split("\n");
    var wrappedLines = 0;
    for (var li = 0; li < explicitLines.length; li++) {
        wrappedLines += Math.max(1, Math.ceil(explicitLines[li].length / 34));
    }
    var visibleLines = Math.max(1, Math.min(5, wrappedLines));
    var editorHeight = Math.max(32, 14 + visibleLines * 16);
    setFlex("chat-input", "height", editorHeight);
    setFlex("chat-input-row", "height", Math.max(36, editorHeight));
    layout();
}

registerClick("upload-btn");
on("upload-btn", "click", function() {
    var path = showOpenDialog("Select Reference Image", "Images", "png;jpg;jpeg;gif;webp");
    if (path && path.length > 0) {
        setUploadedImage(path);
        showToast("Attached " + uploadedImageName);
    }
});

createTextEditor("chat-input", "chat-input-row");
setPlaceholder("chat-input", "Describe a style...");
setMultiLine("chat-input", 1);
setFlex("chat-input", "flex_grow", 1);
setFlex("chat-input", "height", 32);
on("chat-input", "change", function(text) {
    updateChatInputSizing(text);
});

// #49: Send button with proper icon sizing
createCol("send-btn", "chat-input-row");
setFlex("send-btn", "width", 32);
setFlex("send-btn", "height", 32);
setBackground("send-btn", APP_ACCENT);
setBorder("send-btn", APP_ACCENT, 1, 6);
setFlex("send-btn", "justify_content", "center");
setFlex("send-btn", "align_items", "center");
createIcon("send-icon", "send", "send-btn");
setFlex("send-icon", "width", 16);
setFlex("send-icon", "height", 16);
setPointerEvents("send-icon", "none");
// Issue 3: hover state for send button
registerHover("send-btn");
on("send-btn", "mouseenter", function() { setBackground("send-btn", APP_ACCENT_HOVER); setBorder("send-btn", APP_ACCENT_HOVER, 1, 6); });
on("send-btn", "mouseleave", function() { setBackground("send-btn", APP_ACCENT); setBorder("send-btn", APP_ACCENT, 1, 6); });

// ═══════════════════════════════════════════════════════════════════
// STATUS BAR (28px, full width)
// ═══════════════════════════════════════════════════════════════════
createRow("status-bar");
setFlex("status-bar", "height", 28);
setFlex("status-bar", "flex_shrink", 0);
setFlex("status-bar", "padding_left", 12);
setFlex("status-bar", "padding_right", 12);
setFlex("status-bar", "align_items", "center");
setFlex("status-bar", "justify_content", "space-between");
setBackground("status-bar", APP_SURFACE);
setBorder("status-bar", APP_BORDER, 1, 0);

createLabel("status-text", "0 tokens modified", "status-bar");
setFontSize("status-text", 10);
setTextColor("status-text", APP_TEXT_DIM);

createLabel("status-schema", "pulp-theme/v1", "status-bar");

// ═══════════════════════════════════════════════════════════════════
// D7: Toast notification system
// ═══════════════════════════════════════════════════════════════════
createCol("toast-overlay", "");
setPosition("toast-overlay", "absolute");
setFlex("toast-overlay", "width", 200);
setFlex("toast-overlay", "height", 30);
setFlex("toast-overlay", "justify_content", "center");
setFlex("toast-overlay", "align_items", "center");
setBackground("toast-overlay", APP_PANEL);
setBorder("toast-overlay", APP_BORDER, 1, 6);
setBoxShadow("toast-overlay", 0, 4, 16, 0, "#00000060");
setZIndex("toast-overlay", 200);
setOpacity("toast-overlay", 0);
setVisible("toast-overlay", false);

createLabel("toast-text", "", "toast-overlay");
setFontSize("toast-text", 10);

createModal("help-modal", "");
setPosition("help-modal", "absolute");
setFlex("help-modal", "width", 1100);
setFlex("help-modal", "height", 700);
setFlex("help-modal", "justify_content", "center");
setFlex("help-modal", "align_items", "center");
setBackground("help-modal", "#00000088");
setZIndex("help-modal", 210);
setOpacity("help-modal", 0);
setVisible("help-modal", false);
setPointerEvents("help-modal", "none");
registerClick("help-modal");

createCol("help-card", "help-modal");
setFlex("help-card", "width", 340);
setFlex("help-card", "min_height", 144);
setFlex("help-card", "padding", 16);
setFlex("help-card", "gap", 10);
setBackground("help-card", APP_PANEL);
setBorder("help-card", APP_BORDER, 1, 12);
setBoxShadow("help-card", 0, 12, 32, 0, "#00000088");

createRow("help-card-header", "help-card");
setFlex("help-card-header", "height", 26);
setFlex("help-card-header", "align_items", "center");
setFlex("help-card-header", "justify_content", "space-between");
setFlex("help-card-header", "gap", 8);

createLabel("help-modal-title", "Help", "help-card-header");
setFontSize("help-modal-title", 14);
setTextColor("help-modal-title", APP_TEXT);
setFlex("help-modal-title", "flex_grow", 1);
setTextOverflow("help-modal-title", "ellipsis");

createCol("help-modal-close-btn", "help-card-header");
setFlex("help-modal-close-btn", "width", 60);
setFlex("help-modal-close-btn", "height", 26);
setFlex("help-modal-close-btn", "padding_left", 6);
setFlex("help-modal-close-btn", "padding_right", 6);
setFlex("help-modal-close-btn", "justify_content", "center");
setFlex("help-modal-close-btn", "align_items", "center");
setBackground("help-modal-close-btn", APP_SURFACE);
setBorder("help-modal-close-btn", APP_BORDER, 1, 8);
registerClick("help-modal-close-btn");

createLabel("help-modal-close-label", "Close", "help-modal-close-btn");
setFontSize("help-modal-close-label", 10);
setTextColor("help-modal-close-label", APP_TEXT_DIM);
setPointerEvents("help-modal-close-label", "none");

createLabel("help-modal-body", "", "help-card");
setFontSize("help-modal-body", 11);
setTextColor("help-modal-body", APP_TEXT_DIM);
setMultiLine("help-modal-body", 1);
setFlex("help-modal-body", "flex_grow", 1);

on("help-modal", "click", function() { hideHelpModal(); });
on("help-modal", "dismiss", function() { hideHelpModal(); });
on("help-modal-close-btn", "click", function() { hideHelpModal(); });

var toastTimer = 0;
function showToast(msg) {
    setText("toast-text", msg);
    setTop("toast-overlay", 650);
    setLeft("toast-overlay", 450);
    setVisible("toast-overlay", true);
    setOpacity("toast-overlay", 1);
    toastTimer = 60; // ~60 frames at 60fps = 1 second
    function fadeToast() {
        toastTimer--;
        if (toastTimer <= 0) {
            setOpacity("toast-overlay", 0);
            setVisible("toast-overlay", false);
            return;
        }
        if (toastTimer < 15) {
            setOpacity("toast-overlay", toastTimer / 15);
        }
        __requestFrame__(fadeToast);
    }
    __requestFrame__(fadeToast);
}
setFontSize("status-schema", 10);
setTextColor("status-schema", APP_TEXT_DIM);

// ═══════════════════════════════════════════════════════════════════
// Inspector: Cmd+click detection
// ═══════════════════════════════════════════════════════════════════
// Issue 6: Cmd+click inspector with chat context scoping
var inspectedComponent = null;

function clearInspectedComponent() {
    inspectedComponent = null;
    setText("context-label", "Editing: All");
    setVisible("context-clear", false);
    setText("insp-bounds-v", "—");
    lastDesignDebugState.target = "all";
    lastDesignDebugState.targetBounds = null;
}

function syncDesignDebugTargetBounds() {
    if (!inspectedComponent) {
        setText("insp-bounds-v", "—");
        lastDesignDebugState.targetBounds = null;
        return;
    }
    var rect = getLayoutRect(inspectedComponent);
    if (!rect || rect.width === undefined) {
        setText("insp-bounds-v", "—");
        lastDesignDebugState.targetBounds = null;
        return;
    }
    var boundsText = Math.round(rect.x) + ", " + Math.round(rect.y) + " · " +
        Math.round(rect.width) + "×" + Math.round(rect.height);
    setText("insp-bounds-v", boundsText);
    lastDesignDebugState.targetBounds = {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height
    };
}

function setDesignDebugTarget(widgetId) {
    setText("insp-type-v", widgetId ? "View" : "—");
    setText("insp-id-v", widgetId || "—");
    setText("insp-bounds-v", "—");
    inspectedComponent = widgetId || null;
    if (inspectedComponent) {
        setText("context-label", "Editing: " + inspectedComponent);
        setVisible("context-clear", true);
        lastDesignDebugState.target = inspectedComponent;
        syncDesignDebugTargetBounds();
    } else {
        clearInspectedComponent();
    }
}

function getDesignDebugStateJson() {
    syncDesignDebugTargetBounds();
    return JSON.stringify(lastDesignDebugState);
}

enableInspectClick();
on("__inspect__", "click", function(widgetId) {
    setDesignDebugTarget(widgetId || null);
    if (activeTab === "inspector") {
        switchTab("inspector");
    }
});

// Clear context — click x or the badge itself
on("context-clear", "click", function() {
    clearInspectedComponent();
});
on("context-badge", "click", function() {
    if (inspectedComponent) {
        clearInspectedComponent();
    }
});

// ═══════════════════════════════════════════════════════════════════
// Global keyboard shortcuts (Cmd+Z undo, Cmd+Shift+Z redo)
// ═══════════════════════════════════════════════════════════════════
on("__global__", "keydown", function(evt) {
    if (!evt) return;
    var cmd = (evt.mods & 0x18) !== 0;  // kModMeta | kModCmd
    var shift = (evt.mods & 0x01) !== 0;
    // 'z' key = 122 ASCII or platform key code
    if (cmd && evt.key === 122) {
        if (shift) {
            // Redo
            if (historyIndex < themeHistory.length - 1) {
                historyIndex++;
                applyTokenDiff(themeHistory[historyIndex]);
                updateTokenSwatches();
                setText("status-text", "Redo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
                layout();
            }
        } else {
            // Undo
            if (historyIndex > 0) {
                historyIndex--;
                applyTokenDiff(themeHistory[historyIndex]);
                updateTokenSwatches();
                setText("status-text", "Undo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
                layout();
            }
        }
    }
});

// ═══════════════════════════════════════════════════════════════════
// Chat logic
// ═══════════════════════════════════════════════════════════════════

// Issue 8: Track cumulative chat height for proper scroll sizing
var chatTotalHeight = 60; // start after welcome/hint messages

function addChatMessage(role, text) {
    var id = "msg-" + (msgCount++);
    var snapshot = getThemeJson();
    var hasRestore = (role === "assistant");

    // Issue 8: Better height estimation — wider chars-per-line for 230px width
    var charsPerLine = 25;
    var lineCount = Math.max(1, Math.ceil(text.length / charsPerLine));
    var msgHeight = 16 + lineCount * 16 + (hasRestore ? 24 : 0) + 20;

    createCol(id, "chat-messages");
    setFlex(id, "height", msgHeight);
    setFlex(id, "flex_shrink", 0);  // Issue 8: prevent squishing
    setFlex(id, "padding", 10);
    setFlex(id, "padding_right", 16);  // Issue 8: clear scrollbar
    setFlex(id, "gap", 4);
    setBorder(id, APP_BORDER, 1, 8);
    if (role === "user") {
        setBackground(id, "#2a2a3c");
    } else {
        setBackground(id, APP_PANEL);
    }

    // Role label row
    createRow(id + "-header", id);
    setFlex(id + "-header", "height", 16);
    setFlex(id + "-header", "flex_shrink", 0);
    setFlex(id + "-header", "align_items", "center");
    setFlex(id + "-header", "justify_content", "space-between");

    createLabel(id + "-role", role === "user" ? "You" : "Designer", id + "-header");
    setFontSize(id + "-role", 9);
    setTextColor(id + "-role", APP_TEXT_DIM);

    if (hasRestore) {
        var restoreId = id + "-restore";
        createLabel(restoreId, "Restore", id + "-header");
        setFontSize(restoreId, 9);
        setTextColor(restoreId, APP_ACCENT);
        registerClick(restoreId);
        (function(snap, rid) {
            on(rid, "click", function() {
                applyTokenDiff(snap);
                updateTokenSwatches();
                buildShadeRamps();
                showToast("Restored snapshot");
                layout();
            });
        })(snapshot, restoreId);
    }

    // Issue 1: multi-line label for wrapping
    createLabel(id + "-text", text, id);
    setFontSize(id + "-text", 12);
    setFlex(id + "-text", "flex_grow", 1);
    setMultiLine(id + "-text", 1);

    // Update scroll to fit all messages
    chatTotalHeight += msgHeight + 8;
    setScrollContentSize("chat-messages", 230, chatTotalHeight);
    layout();
}

var chatRequestPending = false;
var chatRequestCounter = 0;
var widgetLookState = {};
var lastChatRequestText = "";
var lastDesignDebugState = {
    target: "all",
    targetBounds: null,
    provider: "claude",
    model: "claude-sonnet-4-6",
    reasoningEffort: "",
    requestText: "",
    responseLength: 0,
    changedColors: [],
    changedDimensions: [],
    widgetLookIds: [],
    widgetLookCount: 0,
    summary: "",
    status: "idle",
    error: "",
    promptLength: 0
};
var widgetKindById = {
    k1: "knob",
    k2: "knob",
    k3: "knob",
    k4: "knob",
    slider1: "fader",
    t1: "toggle",
    t2: "toggle"
};
var aiProviderOptions = [
    { id: "claude", label: "Claude" },
    { id: "codex", label: "Codex" }
];
var aiModelOptions = {
    claude: [
        { id: "claude-sonnet-4-6", label: "Sonnet 4.6" },
        { id: "claude-opus-4-6", label: "Opus 4.6" }
    ],
    codex: [
        { id: "gpt-5.4", label: "GPT-5.4" },
        { id: "gpt-5.4-pro", label: "GPT-5.4 Pro" },
        { id: "gpt-5.3-codex", label: "GPT-5.3 Codex" },
        { id: "gpt-5.2-codex", label: "GPT-5.2 Codex" }
    ]
};
var aiProviderIndex = 0;
var aiModelIndexByProvider = { claude: 0, codex: 0 };
var aiReasoningEffortIndex = 0;
var aiReasoningEffortValues = ["", "low", "medium", "high", "xhigh"];
function clamp01(value) {
    return Math.max(0, Math.min(1, Number(value)));
}

function getSelectedAIProvider() {
    var idx = Math.max(0, Math.min(aiProviderOptions.length - 1, Math.round(aiProviderIndex)));
    return aiProviderOptions[idx].id;
}

function getSelectedAIModel() {
    var provider = getSelectedAIProvider();
    var options = aiModelOptions[provider] || aiModelOptions.claude;
    var idx = Math.max(0, Math.min(options.length - 1, Math.round(aiModelIndexByProvider[provider] || 0)));
    return options[idx].id;
}

function getSelectedAIReasoningEffort() {
    var idx = Math.max(0, Math.min(aiReasoningEffortValues.length - 1, Math.round(aiReasoningEffortIndex)));
    return aiReasoningEffortValues[idx];
}

function refreshAISelectors() {
    var provider = aiProviderOptions[aiProviderIndex].id;
    setValue("provider-selector", aiProviderIndex);
    var modelOptions = aiModelOptions[provider] || aiModelOptions.claude;
    var modelLabels = [];
    for (var i = 0; i < modelOptions.length; i++) modelLabels.push(modelOptions[i].label);
    setItems("model-selector", modelLabels);
    setValue("model-selector", aiModelIndexByProvider[provider] || 0);
    var showEffort = provider === "codex";
    setVisible("effort-selector", showEffort);
    setValue("effort-selector", aiReasoningEffortIndex);
    lastDesignDebugState.provider = provider;
    lastDesignDebugState.model = modelOptions[Math.max(0, aiModelIndexByProvider[provider] || 0)].id;
    lastDesignDebugState.reasoningEffort = showEffort ? aiReasoningEffortValues[aiReasoningEffortIndex] : "";
}

function setDesignDebugAIConfig(providerId, modelId, reasoningEffort) {
    for (var i = 0; i < aiProviderOptions.length; i++) {
        if (aiProviderOptions[i].id === providerId) {
            aiProviderIndex = i;
            break;
        }
    }
    var provider = aiProviderOptions[aiProviderIndex].id;
    var modelOptions = aiModelOptions[provider] || aiModelOptions.claude;
    for (var mi = 0; mi < modelOptions.length; mi++) {
        if (modelOptions[mi].id === modelId) {
            aiModelIndexByProvider[provider] = mi;
            break;
        }
    }
    for (var ei = 0; ei < aiReasoningEffortValues.length; ei++) {
        if (aiReasoningEffortValues[ei] === (reasoningEffort || "")) {
            aiReasoningEffortIndex = ei;
            break;
        }
    }
    refreshAISelectors();
}

on("provider-selector", "select", function(idx) {
    aiProviderIndex = Math.max(0, Math.min(aiProviderOptions.length - 1, Math.round(idx)));
    refreshAISelectors();
    layout();
});

on("model-selector", "select", function(idx) {
    var provider = getSelectedAIProvider();
    aiModelIndexByProvider[provider] = Math.max(0, Math.round(idx));
    lastDesignDebugState.model = getSelectedAIModel();
});

on("effort-selector", "select", function(idx) {
    aiReasoningEffortIndex = Math.max(0, Math.min(aiReasoningEffortValues.length - 1, Math.round(idx)));
    lastDesignDebugState.reasoningEffort = getSelectedAIReasoningEffort();
});

refreshAISelectors();

function numericParam(params, key, fallback, minValue, maxValue) {
    var raw = params && params[key] !== undefined ? Number(params[key]) : fallback;
    if (!isFinite(raw)) raw = fallback;
    if (minValue !== undefined) raw = Math.max(minValue, raw);
    if (maxValue !== undefined) raw = Math.min(maxValue, raw);
    return raw;
}

function shaderFloat(value) {
    var rounded = Math.round(Number(value) * 10000) / 10000;
    if (!isFinite(rounded)) rounded = 0;
    return rounded.toFixed(4);
}

function mergeParams(base, overrides) {
    var merged = {};
    var key;
    if (base) {
        for (key in base) merged[key] = base[key];
    }
    if (overrides) {
        for (key in overrides) merged[key] = overrides[key];
    }
    return merged;
}

function mergeWidgetMaterialSpec(spec) {
    if (!spec || !spec.material) return spec;
    var material = spec.material;
    var merged = {};
    for (var key in spec) merged[key] = spec[key];
    delete merged.material;
    merged.params = merged.params || {};

    if (typeof material === "string") {
        merged.preset = merged.preset || material;
        return merged;
    }

    if (material.preset && !merged.preset) merged.preset = material.preset;
    if (material.family && !merged.family) merged.family = material.family;
    if (material.styleFamily && !merged.family) merged.family = material.styleFamily;
    if (material.params) {
        for (var paramKey in material.params) {
            merged.params[paramKey] = material.params[paramKey];
        }
    }
    if (material.body && material.body.bevel && material.body.bevel.width !== undefined) {
        merged.params.bevel = material.body.bevel.width;
    }
    if (material.body && material.body.rim && material.body.rim.opacity !== undefined) {
        merged.params.rim = material.body.rim.opacity;
    }
    if (material.effects && material.effects.highlight && material.effects.highlight.intensity !== undefined) {
        merged.params.gloss = material.effects.highlight.intensity;
    }
    if (material.effects && material.effects.noise && material.effects.noise.intensity !== undefined) {
        merged.params.noise = material.effects.noise.intensity;
    }
    return merged;
}

function buildMacos7KnobShader(params) {
    var gloss = shaderFloat(numericParam(params, "gloss", 0.82, 0.05, 1.0));
    var metalness = shaderFloat(numericParam(params, "metalness", 0.72, 0.0, 1.0));
    var rim = shaderFloat(numericParam(params, "rim", 0.22, 0.0, 1.0));
    var noise = shaderFloat(numericParam(params, "noise", 0.022, 0.0, 0.08));
    var bevel = shaderFloat(numericParam(params, "bevel", 0.90, 0.0, 1.5));
    var bodyRadius = shaderFloat(numericParam(params, "bodyRadius", 0.34, 0.28, 0.40));
    var trackWidth = shaderFloat(numericParam(params, "trackWidth", 0.028, 0.012, 0.060));
    var trackInner = shaderFloat(Number(bodyRadius) + 0.040);
    var trackOuter = shaderFloat(Number(trackInner) + Number(trackWidth));
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "uniform float time;",
        "layout(color) uniform float4 accentColor;",
        "layout(color) uniform float4 bgColor;",
        "layout(color) uniform float4 trackColor;",
        "layout(color) uniform float4 fillColor;",
        "layout(color) uniform float4 thumbColor;",
        "float hash21(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453123); }",
        "float ringMask(float2 p, float innerR, float outerR, float aa) {",
        "  float d = length(p);",
        "  float outer = 1.0 - smoothstep(outerR - aa, outerR + aa, d);",
        "  float inner = 1.0 - smoothstep(innerR - aa, innerR + aa, d);",
        "  return clamp(outer - inner, 0.0, 1.0);",
        "}",
        "float3 toLinear(float3 c) {",
        "  return pow(clamp(c, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)), float3(2.2, 2.2, 2.2));",
        "}",
        "float3 toSrgb(float3 c) {",
        "  return pow(max(c, float3(0.0, 0.0, 0.0)), float3(0.454545, 0.454545, 0.454545));",
        "}",
        "half4 main(float2 coord) {",
        "  float size = max(min(resolution.x, resolution.y), 1.0);",
        "  float2 p = (coord - resolution * 0.5) / size;",
        "  float r = length(p);",
        "  float aa = max(1.5 / size, 0.0025);",
        "  float bodyRadius = " + bodyRadius + ";",
        "  float body = 1.0 - smoothstep(bodyRadius - aa, bodyRadius + aa, r);",
        "  float ring = ringMask(p, " + trackInner + ", " + trackOuter + ", aa * 1.2);",
        "  float shadow = (1.0 - smoothstep(bodyRadius + 0.02, bodyRadius + 0.10, length(p - float2(0.0, 0.025)))) * 0.30;",
        "  float normR = clamp(r / max(bodyRadius, 0.0001), 0.0, 1.0);",
        "  float nz = sqrt(max(1.0 - normR * normR, 0.0));",
        "  float2 nxy = p / max(bodyRadius, 0.0001);",
        "  float3 N = normalize(float3(nxy.x, nxy.y, nz));",
        "  float3 L = normalize(float3(-0.42, -0.70, 0.58));",
        "  float3 V = float3(0.0, 0.0, 1.0);",
        "  float3 H = normalize(L + V);",
        "  float gloss = " + gloss + ";",
        "  float diff = max(dot(N, L), 0.0);",
        "  float spec = pow(max(dot(N, H), 0.0), mix(18.0, 140.0, gloss));",
        "  float fres = pow(1.0 - max(dot(N, V), 0.0), 3.0) * " + rim + ";",
        "  float bevelMask = smoothstep(bodyRadius * 0.68, bodyRadius, r) * " + bevel + ";",
        "  float bevelLight = clamp(0.65 - 0.55 * nxy.y - 0.32 * nxy.x, 0.0, 1.0);",
        "  float centerLift = 1.0 - smoothstep(0.0, bodyRadius * 0.96, r);",
        "  float brushed = sin((atan(p.y, p.x) + 3.14159265) * 42.0 + time * 0.05);",
        "  float grain = (hash21(coord + float2(time * 7.0, time * 13.0)) - 0.5) * " + noise + ";",
        "  float metalness = " + metalness + ";",
        "  float3 bgLin = toLinear(bgColor.rgb);",
        "  float3 trackLin = toLinear(trackColor.rgb);",
        "  float3 fillLin = toLinear(fillColor.rgb);",
        "  float3 accentLin = toLinear(accentColor.rgb);",
        "  float3 thumbLin = toLinear(thumbColor.rgb);",
        "  float3 faceBase = mix(bgLin * 0.74 + fillLin * 0.04, float3(0.88, 0.90, 0.95), metalness * 0.78);",
        "  float3 face = faceBase * (0.34 + diff * 0.52 + centerLift * 0.16);",
        "  face += spec * mix(float3(0.45, 0.47, 0.52), float3(1.08, 1.08, 1.04), metalness);",
        "  face += fres * float3(0.22, 0.24, 0.30);",
        "  face = mix(face, face + (bevelLight - 0.5) * 0.36, clamp(bevelMask, 0.0, 1.0));",
        "  face *= 1.0 + grain + brushed * " + noise + " * metalness * 0.65;",
        "  float angle = atan(-p.y, p.x);",
        "  float start = 2.35619449;",
        "  float sweep = 4.71238898;",
        "  if (angle < start) angle += 6.283185307;",
        "  float t = clamp((angle - start) / sweep, 0.0, 1.0);",
        "  float active = step(t, clamp(value, 0.0, 1.0));",
        "  float grooveShade = clamp(0.52 + 0.32 * (-nxy.y * 0.7 - nxy.x * 0.25), 0.0, 1.0);",
        "  float3 grooveColor = mix(trackLin * 0.42, bgLin * 0.28, 0.35) * grooveShade;",
        "  float3 activeColor = mix(fillLin * 0.84, accentLin * 1.06, 0.35) * (0.82 + spec * 0.30);",
        "  float indicatorAngle = start + sweep * clamp(value, 0.0, 1.0);",
        "  float2 dir = float2(cos(indicatorAngle), -sin(indicatorAngle));",
        "  float2 perp = float2(-dir.y, dir.x);",
        "  float2 rel = p - dir * (bodyRadius * 0.54);",
        "  float notchLen = bodyRadius * 0.18;",
        "  float notchHalfWidth = max(aa * 2.0, 0.010);",
        "  float notchDist = max(abs(dot(rel, dir)) - notchLen * 0.5, abs(dot(rel, perp)) - notchHalfWidth);",
        "  float notch = body * (1.0 - smoothstep(0.0, aa * 2.2, notchDist));",
        "  float3 notchColor = mix(float3(0.96, 0.97, 1.00), thumbLin * 1.04, 0.55);",
        "  float alpha = max(shadow, max(body, ring));",
        "  float3 colorLin = bgLin * shadow * 0.35;",
        "  colorLin = mix(colorLin, face, body);",
        "  colorLin = mix(colorLin, grooveColor, ring * 0.94);",
        "  colorLin = mix(colorLin, activeColor, ring * active);",
        "  colorLin = mix(colorLin, notchColor, notch);",
        "  float3 outColor = toSrgb(colorLin);",
        "  return half4(half3(outColor) * half(alpha), half(alpha));",
        "}"
    ].join("\n");
}

function buildGlassFaderShader(params) {
    var gloss = shaderFloat(numericParam(params, "gloss", 0.68, 0.0, 1.0));
    var noise = shaderFloat(numericParam(params, "noise", 0.012, 0.0, 0.05));
    var depth = shaderFloat(numericParam(params, "depth", 0.32, 0.0, 1.0));
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "uniform float time;",
        "layout(color) uniform float4 trackColor;",
        "layout(color) uniform float4 fillColor;",
        "layout(color) uniform float4 thumbColor;",
        "float hash21(float2 p) { return fract(sin(dot(p, float2(91.7, 173.3))) * 47453.5453); }",
        "float3 toLinear(float3 c) { return pow(clamp(c, float3(0.0,0.0,0.0), float3(1.0,1.0,1.0)), float3(2.2,2.2,2.2)); }",
        "float3 toSrgb(float3 c) { return pow(max(c, float3(0.0,0.0,0.0)), float3(0.454545,0.454545,0.454545)); }",
        "half4 main(float2 coord) {",
        "  float2 uv = coord / resolution;",
        "  float aa = max(1.25 / max(resolution.y, 1.0), 0.003);",
        "  float capsule = 1.0 - smoothstep(0.23 - aa, 0.23 + aa, abs(uv.y - 0.5));",
        "  float fill = step(uv.x, clamp(value, 0.0, 1.0)) * capsule;",
        "  float thumb = 1.0 - smoothstep(0.055, 0.085, length(uv - float2(clamp(value, 0.0, 1.0), 0.5)));",
        "  float highlight = pow(max(1.0 - abs(uv.y - 0.33) * 4.2, 0.0), mix(1.3, 3.2, " + gloss + "));",
        "  float grain = (hash21(coord + float2(time * 5.0, 0.0)) - 0.5) * " + noise + ";",
        "  float3 trackLin = toLinear(trackColor.rgb);",
        "  float3 fillLin = toLinear(fillColor.rgb);",
        "  float3 thumbLin = toLinear(thumbColor.rgb);",
        "  float3 base = trackLin * (0.58 + " + depth + " * 0.22);",
        "  base += highlight * mix(trackLin * 0.15, float3(0.22, 0.24, 0.28), " + gloss + ");",
        "  base *= 1.0 + grain;",
        "  float3 active = mix(fillLin * 0.75, fillLin * 1.08, highlight * 0.75);",
        "  float3 thumbColorLin = mix(thumbLin * 0.82, float3(0.96, 0.97, 1.0), highlight * 0.45);",
        "  float3 colorLin = base;",
        "  colorLin = mix(colorLin, active, fill);",
        "  colorLin = mix(colorLin, thumbColorLin, thumb);",
        "  float alpha = max(capsule, thumb);",
        "  return half4(half3(toSrgb(colorLin)) * half(alpha), half(alpha));",
        "}"
    ].join("\n");
}

function buildCapsuleToggleShader(params) {
    var gloss = shaderFloat(numericParam(params, "gloss", 0.56, 0.0, 1.0));
    var rim = shaderFloat(numericParam(params, "rim", 0.16, 0.0, 1.0));
    var noise = shaderFloat(numericParam(params, "noise", 0.010, 0.0, 0.04));
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "uniform float time;",
        "layout(color) uniform float4 accentColor;",
        "layout(color) uniform float4 trackColor;",
        "layout(color) uniform float4 thumbColor;",
        "float hash21(float2 p) { return fract(sin(dot(p, float2(53.1, 127.9))) * 15153.5453); }",
        "float3 toLinear(float3 c) { return pow(clamp(c, float3(0.0,0.0,0.0), float3(1.0,1.0,1.0)), float3(2.2,2.2,2.2)); }",
        "float3 toSrgb(float3 c) { return pow(max(c, float3(0.0,0.0,0.0)), float3(0.454545,0.454545,0.454545)); }",
        "half4 main(float2 coord) {",
        "  float2 uv = coord / resolution;",
        "  float2 p = uv - float2(0.5, 0.5);",
        "  float2 q = abs(p) - float2(0.22, 0.18);",
        "  float dist = length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0);",
        "  float aa = max(1.25 / max(min(resolution.x, resolution.y), 1.0), 0.003);",
        "  float capsule = 1.0 - smoothstep(0.0, aa * 2.4, dist);",
        "  float t = clamp(value, 0.0, 1.0);",
        "  float thumbX = mix(0.28, 0.72, t);",
        "  float thumb = 1.0 - smoothstep(0.10, 0.145, length(uv - float2(thumbX, 0.5)));",
        "  float highlight = pow(max(1.0 - abs(uv.y - 0.34) * 4.4, 0.0), mix(1.2, 3.0, " + gloss + "));",
        "  float edge = smoothstep(0.08, 0.24, abs(p.x)) * " + rim + ";",
        "  float grain = (hash21(coord + float2(time * 3.0, time * 9.0)) - 0.5) * " + noise + ";",
        "  float3 trackLin = toLinear(trackColor.rgb);",
        "  float3 accentLin = toLinear(accentColor.rgb);",
        "  float3 thumbLin = toLinear(thumbColor.rgb);",
        "  float3 base = mix(trackLin * 0.72, accentLin * 0.92, t);",
        "  base += highlight * mix(trackLin * 0.12, float3(0.14, 0.15, 0.18), " + gloss + ");",
        "  base += edge * float3(0.10, 0.10, 0.12);",
        "  base *= 1.0 + grain;",
        "  float3 thumbColorLin = mix(thumbLin * 0.82, float3(0.96, 0.97, 0.99), highlight * 0.5);",
        "  float alpha = max(capsule, thumb);",
        "  float3 colorLin = mix(base, thumbColorLin, thumb);",
        "  return half4(half3(toSrgb(colorLin)) * half(alpha), half(alpha));",
        "}"
    ].join("\n");
}

function buildBakeliteKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.28,
        metalness: 0.10,
        rim: 0.10,
        noise: 0.035,
        bevel: 0.62,
        bodyRadius: 0.345,
        trackWidth: 0.024
    }, params || {}));
}

function buildLedRingKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.96,
        metalness: 0.35,
        rim: 0.42,
        noise: 0.010,
        bevel: 0.48,
        bodyRadius: 0.325,
        trackWidth: 0.040
    }, params || {}));
}

function buildAnalogSliderShader(params) {
    return buildGlassFaderShader(mergeParams({
        gloss: 0.24,
        noise: 0.024,
        depth: 0.18
    }, params || {}));
}

function buildIlluminatedToggleShader(params) {
    return buildCapsuleToggleShader(mergeParams({
        gloss: 0.82,
        rim: 0.34,
        noise: 0.008
    }, params || {}));
}

function buildPrecisionKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.58,
        metalness: 0.46,
        rim: 0.07,
        noise: 0.006,
        bevel: 0.44,
        bodyRadius: 0.332,
        trackWidth: 0.022
    }, params || {}));
}

function buildHeritageKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.78,
        metalness: 0.68,
        rim: 0.18,
        noise: 0.020,
        bevel: 1.02,
        bodyRadius: 0.346,
        trackWidth: 0.028
    }, params || {}));
}

function buildRetroKnobShader(params) {
    return buildBakeliteKnobShader(mergeParams({
        gloss: 0.34,
        metalness: 0.08,
        rim: 0.08,
        noise: 0.040,
        bevel: 0.54,
        bodyRadius: 0.350,
        trackWidth: 0.022
    }, params || {}));
}

function buildMasteringKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.46,
        metalness: 0.30,
        rim: 0.05,
        noise: 0.004,
        bevel: 0.30,
        bodyRadius: 0.324,
        trackWidth: 0.020
    }, params || {}));
}

function buildPrecisionFaderShader(params) {
    return buildGlassFaderShader(mergeParams({
        gloss: 0.28,
        noise: 0.004,
        depth: 0.10
    }, params || {}));
}

function buildModularFaderShader(params) {
    return buildGlassFaderShader(mergeParams({
        gloss: 0.88,
        noise: 0.010,
        depth: 0.40
    }, params || {}));
}

function buildConsoleSliderShader(params) {
    return buildAnalogSliderShader(mergeParams({
        gloss: 0.30,
        noise: 0.020,
        depth: 0.24
    }, params || {}));
}

function buildPrecisionToggleShader(params) {
    return buildCapsuleToggleShader(mergeParams({
        gloss: 0.30,
        rim: 0.06,
        noise: 0.004
    }, params || {}));
}

function buildHeritageToggleShader(params) {
    return buildCapsuleToggleShader(mergeParams({
        gloss: 0.56,
        rim: 0.14,
        noise: 0.015
    }, params || {}));
}

var shaderPresetLibrary = {
    macos7_knob: function(widgetId, params) { return buildMacos7KnobShader(params || {}); },
    bakelite_knob: function(widgetId, params) { return buildBakeliteKnobShader(params || {}); },
    led_ring_knob: function(widgetId, params) { return buildLedRingKnobShader(params || {}); },
    precision_knob: function(widgetId, params) { return buildPrecisionKnobShader(params || {}); },
    heritage_knob: function(widgetId, params) { return buildHeritageKnobShader(params || {}); },
    retro_knob: function(widgetId, params) { return buildRetroKnobShader(params || {}); },
    mastering_knob: function(widgetId, params) { return buildMasteringKnobShader(params || {}); },
    glass_fader: function(widgetId, params) { return buildGlassFaderShader(params || {}); },
    analog_slider: function(widgetId, params) { return buildAnalogSliderShader(params || {}); },
    precision_fader: function(widgetId, params) { return buildPrecisionFaderShader(params || {}); },
    modular_fader: function(widgetId, params) { return buildModularFaderShader(params || {}); },
    console_slider: function(widgetId, params) { return buildConsoleSliderShader(params || {}); },
    capsule_toggle: function(widgetId, params) { return buildCapsuleToggleShader(params || {}); },
    illuminated_toggle: function(widgetId, params) { return buildIlluminatedToggleShader(params || {}); },
    precision_toggle: function(widgetId, params) { return buildPrecisionToggleShader(params || {}); },
    heritage_toggle: function(widgetId, params) { return buildHeritageToggleShader(params || {}); }
};

var presetAliasMap = {
    "mac_os_7": "macos7_knob",
    "macos7": "macos7_knob",
    "system7_knob": "macos7_knob",
    "classic_mac_knob": "macos7_knob",
    "vintage_bakelite_knob": "bakelite_knob",
    "bakelite": "bakelite_knob",
    "cyberpunk_knob": "led_ring_knob",
    "neon_knob": "led_ring_knob",
    "led_knob": "led_ring_knob",
    "fabfilter_knob": "precision_knob",
    "precision_eq_knob": "precision_knob",
    "hardware_knob": "heritage_knob",
    "mastering_knob": "mastering_knob",
    "eventide_knob": "retro_knob",
    "soundtoys_knob": "retro_knob",
    "analog_fader": "analog_slider",
    "vintage_slider": "analog_slider",
    "precision_fader": "precision_fader",
    "neon_fader": "modular_fader",
    "console_fader": "console_slider",
    "neon_toggle": "illuminated_toggle",
    "led_toggle": "illuminated_toggle",
    "glow_toggle": "illuminated_toggle",
    "precision_toggle": "precision_toggle",
    "hardware_toggle": "heritage_toggle"
};

var audioPluginStyleFamilies = {
    precision_analyzer: {
        description: "FabFilter-style precision, disciplined chrome, analyzer-centric clarity",
        keywords: ["fabfilter", "pro-q", "precision", "analyzer", "surgical", "clean", "transparent", "metering"],
        presets: { knob: "precision_knob", fader: "precision_fader", toggle: "precision_toggle" }
    },
    heritage_hardware: {
        description: "UA / Softube / Brainworx / Moog hardware with metal, bakelite, and premium studio heft",
        keywords: ["universal audio", "uad", "softube", "brainworx", "moog", "hardware", "studio hardware", "metal", "platinum"],
        presets: { knob: "heritage_knob", fader: "analog_slider", toggle: "heritage_toggle" }
    },
    retro_character: {
        description: "Soundtoys / Eventide character, chunky labels, playful retro personality",
        keywords: ["soundtoys", "eventide", "retro", "character", "playful", "chunky", "vintage fx", "warm vintage"],
        presets: { knob: "retro_knob", fader: "analog_slider", toggle: "capsule_toggle" }
    },
    modular_neon: {
        description: "Arturia Pigments / Massive X neon modular with vivid accents and futuristic gloss",
        keywords: ["arturia", "pigments", "massive x", "native instruments", "cyberpunk", "neon", "modular", "sci-fi", "glow", "led"],
        presets: { knob: "led_ring_knob", fader: "modular_fader", toggle: "illuminated_toggle" }
    },
    mastering_lab: {
        description: "Voxengo / mastering utility aesthetic with restrained surfaces and technical focus",
        keywords: ["voxengo", "mastering", "lab", "utility", "technical", "dense", "analysis", "measurement"],
        presets: { knob: "mastering_knob", fader: "precision_fader", toggle: "precision_toggle" }
    },
    console_strip: {
        description: "Waves / Slate / console-strip workflow with vertical signal-flow pragmatism",
        keywords: ["waves", "slate", "console", "channel strip", "strip", "mixer", "desk", "rack"],
        presets: { knob: "heritage_knob", fader: "console_slider", toggle: "heritage_toggle" }
    }
};

var styleFamilyAliasMap = {
    "precision": "precision_analyzer",
    "fabfilter": "precision_analyzer",
    "clean_precision": "precision_analyzer",
    "heritage": "heritage_hardware",
    "hardware": "heritage_hardware",
    "analog_hardware": "heritage_hardware",
    "retro": "retro_character",
    "character": "retro_character",
    "modular": "modular_neon",
    "cyberpunk": "modular_neon",
    "neon": "modular_neon",
    "mastering": "mastering_lab",
    "lab": "mastering_lab",
    "console": "console_strip",
    "channel_strip": "console_strip"
};

function normalizeStyleFamily(familyId) {
    var raw = String(familyId || "").trim();
    if (!raw) return "";
    if (audioPluginStyleFamilies[raw]) return raw;
    var normalized = raw.toLowerCase().replace(/[\s\-]+/g, "_");
    return styleFamilyAliasMap[normalized] || normalized;
}

function scoreStyleFamily(text, family) {
    var score = 0;
    var keywords = family.keywords || [];
    for (var i = 0; i < keywords.length; i++) {
        if (text.indexOf(keywords[i]) >= 0) score += keywords[i].length > 8 ? 2 : 1;
    }
    return score;
}

function detectStyleFamily(requestText) {
    var text = String(requestText || "").toLowerCase();
    var bestFamily = "";
    var bestScore = 0;
    for (var familyId in audioPluginStyleFamilies) {
        var score = scoreStyleFamily(text, audioPluginStyleFamilies[familyId]);
        if (score > bestScore) {
            bestScore = score;
            bestFamily = familyId;
        }
    }
    return bestFamily;
}

function presetForFamily(familyId, widgetId) {
    var normalizedFamily = normalizeStyleFamily(familyId);
    if (!normalizedFamily || !audioPluginStyleFamilies[normalizedFamily]) return "";
    var kind = widgetKindForId(widgetId);
    var presets = audioPluginStyleFamilies[normalizedFamily].presets || {};
    return presets[kind] || "";
}

function normalizePresetId(presetId) {
    var raw = String(presetId || "").trim();
    if (!raw) return "";
    if (shaderPresetLibrary[raw]) return raw;
    var normalized = raw.toLowerCase().replace(/[\s\-]+/g, "_");
    return presetAliasMap[normalized] || normalized;
}

function buildShaderPreset(presetId, widgetId, params) {
    var normalizedPresetId = normalizePresetId(presetId);
    var preset = shaderPresetLibrary[normalizedPresetId];
    if (!preset) return "";
    if (typeof preset === "function") return preset(widgetId, params || {});
    return preset;
}
var schemaPresetLibrary = {
    notched_knob: JSON.stringify({
        type: "knob",
        elements: [
            { type: "arc", radius: "42%", width: 4, startAngle: -135, sweepAngle: 270, color: "control.track" },
            { type: "arc", radius: "42%", width: 4, startAngle: -135, sweepAngle: { bind: "value", range: [0, 270] }, color: "accent.primary" },
            { type: "circle", radius: "30%", color: "bg.surface" },
            { type: "line", innerRadius: "14%", outerRadius: "34%", width: 3, angle: { bind: "value", range: [-135, 135] }, color: "control.thumb" }
        ]
    }),
    minimal_toggle: JSON.stringify({
        type: "toggle",
        elements: [
            { type: "rect", cornerRadius: "18", color: "control.track" },
            { type: "circle", radius: "18%", color: "control.thumb" }
        ]
    })
};

function widgetKindForId(widgetId) {
    return widgetKindById[widgetId] || "generic";
}

function compactShaderError(error) {
    if (!error) return "Unknown shader error";
    var singleLine = String(error).replace(/\s+/g, " ").trim();
    if (singleLine.length > 220) singleLine = singleLine.substring(0, 217) + "...";
    return singleLine;
}

function inferFallbackPreset(widgetId, requestText) {
    var kind = widgetKindForId(widgetId);
    var text = (requestText || "").toLowerCase();
    var familyPreset = presetForFamily(detectStyleFamily(text), widgetId);
    if (familyPreset) return familyPreset;

    if (kind === "knob") {
        if (text.indexOf("mac") >= 0 || text.indexOf("os 7") >= 0 || text.indexOf("classic") >= 0) return "macos7_knob";
        if (text.indexOf("bakelite") >= 0 || text.indexOf("analog") >= 0 || text.indexOf("warm") >= 0) return "bakelite_knob";
        if (text.indexOf("vintage") >= 0) return "retro_knob";
        if (text.indexOf("gloss") >= 0 || text.indexOf("glass") >= 0) return "heritage_knob";
        return "macos7_knob";
    }
    if (kind === "fader") {
        if (text.indexOf("glass") >= 0 || text.indexOf("modern") >= 0) return "glass_fader";
        if (text.indexOf("analog") >= 0 || text.indexOf("vintage") >= 0 || text.indexOf("warm") >= 0) return "analog_slider";
        return "precision_fader";
    }
    if (kind === "toggle") {
        if (text.indexOf("illuminated") >= 0 || text.indexOf("glow") >= 0 || text.indexOf("led") >= 0) return "illuminated_toggle";
        if (text.indexOf("hardware") >= 0 || text.indexOf("console") >= 0) return "heritage_toggle";
        return "capsule_toggle";
    }
    return familyPreset;
}

function buildWidgetShaderFromBody(widgetId, shaderBody) {
    var body = String(shaderBody || "").trim();
    if (!body) return "";
    var kind = widgetKindForId(widgetId);
    if (kind === "knob") {
        return [
            "uniform float2 resolution;",
            "uniform float value;",
            "uniform float time;",
            "layout(color) uniform float4 accentColor;",
            "layout(color) uniform float4 bgColor;",
            "layout(color) uniform float4 trackColor;",
            "layout(color) uniform float4 fillColor;",
            "layout(color) uniform float4 thumbColor;",
            "float ringMask(float2 p, float r0, float r1) {",
            "  float d = length(p);",
            "  float outer = 1.0 - smoothstep(r1 - 0.012, r1 + 0.012, d);",
            "  float inner = 1.0 - smoothstep(r0 - 0.012, r0 + 0.012, d);",
            "  return clamp(outer - inner, 0.0, 1.0);",
            "}",
            "float sdDiamond(float2 p, float s) {",
            "  float2 q = abs(p);",
            "  return (q.x + q.y - s) * 0.7071;",
            "}",
            "half4 main(float2 coord) {",
            "  float2 uv = coord / resolution;",
            "  float2 p = uv - float2(0.5);",
            "  p.y *= resolution.y / max(resolution.x, 1.0);",
            "  float r = length(p);",
            body,
            "}"
        ].join("\n");
    }
    if (kind === "fader") {
        return [
            "uniform float2 resolution;",
            "uniform float value;",
            "uniform float time;",
            "layout(color) uniform float4 accentColor;",
            "layout(color) uniform float4 bgColor;",
            "layout(color) uniform float4 trackColor;",
            "layout(color) uniform float4 fillColor;",
            "layout(color) uniform float4 thumbColor;",
            "half4 main(float2 coord) {",
            "  float2 uv = coord / resolution;",
            "  float2 p = uv - float2(0.5);",
            body,
            "}"
        ].join("\n");
    }
    if (kind === "toggle") {
        return [
            "uniform float2 resolution;",
            "uniform float value;",
            "uniform float time;",
            "layout(color) uniform float4 accentColor;",
            "layout(color) uniform float4 bgColor;",
            "layout(color) uniform float4 trackColor;",
            "layout(color) uniform float4 fillColor;",
            "layout(color) uniform float4 thumbColor;",
            "float sdDiamond(float2 p, float s) {",
            "  float2 q = abs(p);",
            "  return (q.x + q.y - s) * 0.7071;",
            "}",
            "half4 main(float2 coord) {",
            "  float2 uv = coord / resolution;",
            "  float2 p = uv - float2(0.5);",
            body,
            "}"
        ].join("\n");
    }
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "uniform float time;",
        "layout(color) uniform float4 accentColor;",
        "layout(color) uniform float4 bgColor;",
        "layout(color) uniform float4 trackColor;",
        "layout(color) uniform float4 fillColor;",
        "layout(color) uniform float4 thumbColor;",
        "half4 main(float2 coord) {",
        "  float2 uv = coord / resolution;",
        body,
        "}"
    ].join("\n");
}

function extractShaderBody(shaderText) {
    var shader = String(shaderText || "");
    var match = shader.match(/main\s*\(\s*float2\s+\w+\s*\)\s*\{([\s\S]*)\}\s*$/);
    return match ? match[1].trim() : "";
}

function tryApplyCompiledShader(widgetId, shaderSource, statePreset) {
    if (!shaderSource || shaderSource.length === 0) return false;
    var compiled = compileShader(shaderSource);
    if (compiled && compiled.success) {
        clearWidgetSchema(widgetId);
        setWidgetShader(widgetId, shaderSource);
        widgetLookState[widgetId] = { kind: "shader", preset: statePreset || "custom" };
        return true;
    }
    return compiled || { success: false, error: "Shader compilation failed" };
}

function applyPresetFallback(widgetId, fallbackPreset, compileError) {
    var normalizedFallback = normalizePresetId(fallbackPreset);
    if (!normalizedFallback || !shaderPresetLibrary[normalizedFallback]) return false;
    var presetShader = buildShaderPreset(normalizedFallback, widgetId, {});
    var compiled = compileShader(presetShader);
    if (!compiled || !compiled.success) return false;
    clearWidgetSchema(widgetId);
    setWidgetShader(widgetId, presetShader);
    widgetLookState[widgetId] = { kind: "shader", preset: normalizedFallback };
    var note = "Custom shader failed for " + widgetId + "; applied " + normalizedFallback + " fallback.";
    if (compileError) note += " " + compactShaderError(compileError);
    addChatMessage("assistant", note);
    return true;
}

function clearWidgetLook(widgetId) {
    clearWidgetShader(widgetId);
    clearWidgetSchema(widgetId);
    delete widgetLookState[widgetId];
}

function applyWidgetLook(widgetId, spec) {
    if (!widgetId || !spec) return false;
    if (spec === "default") {
        clearWidgetLook(widgetId);
        return true;
    }

    if (typeof spec === "string") {
        spec = { preset: spec };
    }

    spec = mergeWidgetMaterialSpec(spec);
    var normalizedPreset = normalizePresetId(spec.preset);
    var normalizedFamily = normalizeStyleFamily(spec.family || spec.styleFamily);
    var familyPreset = presetForFamily(normalizedFamily, widgetId);

    if (spec.reset) {
        clearWidgetLook(widgetId);
        return true;
    }

    if (spec.schemaPreset && schemaPresetLibrary[spec.schemaPreset]) {
        clearWidgetShader(widgetId);
        setWidgetSchema(widgetId, schemaPresetLibrary[spec.schemaPreset]);
        widgetLookState[widgetId] = { kind: "schema", preset: spec.schemaPreset };
        return true;
    }

    var fallbackPreset = spec.fallbackPreset || inferFallbackPreset(widgetId, lastChatRequestText);
    if (!normalizedPreset && familyPreset) normalizedPreset = familyPreset;

    if (normalizedPreset && shaderPresetLibrary[normalizedPreset]) {
        var presetShader = buildShaderPreset(normalizedPreset, widgetId, spec.params || {});
        var compiledPreset = compileShader(presetShader);
        if (compiledPreset && compiledPreset.success) {
            clearWidgetSchema(widgetId);
            setWidgetShader(widgetId, presetShader);
            widgetLookState[widgetId] = {
                kind: "shader",
                preset: normalizedPreset,
                family: normalizedFamily,
                params: spec.params || {}
            };
            return true;
        }
        return false;
    }

    if (spec.shaderBody && spec.shaderBody.length > 0) {
        var wrappedShader = buildWidgetShaderFromBody(widgetId, spec.shaderBody);
        var wrappedResult = tryApplyCompiledShader(widgetId, wrappedShader, "custom-body");
        if (wrappedResult === true) return true;
        if (applyPresetFallback(widgetId, fallbackPreset, wrappedResult.error)) return true;
        addChatMessage("assistant", "Shader compile failed for " + widgetId + ": " + compactShaderError(wrappedResult.error));
        return false;
    }

    if (spec.shader && spec.shader.length > 0) {
        var compiled = tryApplyCompiledShader(widgetId, spec.shader, "custom");
        if (compiled === true) return true;

        var extractedBody = extractShaderBody(spec.shader);
        if (extractedBody && extractedBody.length > 0) {
            var recoveredShader = buildWidgetShaderFromBody(widgetId, extractedBody);
            var recoveredResult = tryApplyCompiledShader(widgetId, recoveredShader, "custom-body");
            if (recoveredResult === true) return true;
            compiled = recoveredResult;
        }

        if (applyPresetFallback(widgetId, fallbackPreset, compiled.error)) return true;
        if (compiled && compiled.error) addChatMessage("assistant", "Shader compile failed for " + widgetId + ": " + compactShaderError(compiled.error));
        return false;
    }

    if (spec.schema) {
        clearWidgetShader(widgetId);
        setWidgetSchema(widgetId, JSON.stringify(spec.schema));
        widgetLookState[widgetId] = { kind: "schema", preset: "custom" };
        return true;
    }

    return false;
}

function applyDesignChatResponse(response) {
    chatRequestPending = false;
    lastDesignDebugState.requestText = lastChatRequestText || "";
    lastDesignDebugState.target = inspectedComponent || "all";
    lastDesignDebugState.responseLength = response ? response.length : 0;
    lastDesignDebugState.changedColors = [];
    lastDesignDebugState.changedDimensions = [];
    lastDesignDebugState.widgetLookIds = [];
    lastDesignDebugState.widgetLookCount = 0;
    lastDesignDebugState.summary = "";
    lastDesignDebugState.status = "error";
    lastDesignDebugState.error = "";

    if (!response || response.length === 0) {
        addChatMessage("assistant", "No response from Claude");
        setText("status-text", "Error");
        layout();
        lastDesignDebugState.error = "No response from Claude";
        return "No response from Claude";
    }

    var jsonStart = response.indexOf("{");
    var jsonEnd = response.lastIndexOf("}");
    if (jsonStart < 0 || jsonEnd < 0) {
        addChatMessage("assistant", "No JSON in response");
        setText("status-text", "Error");
        layout();
        lastDesignDebugState.error = "No JSON in response";
        return "No JSON in response";
    }

    var jsonDiff = response.substring(jsonStart, jsonEnd + 1);
    var diffObj = {};
    try { diffObj = JSON.parse(jsonDiff); } catch(e) {
        addChatMessage("assistant", "Invalid JSON in response");
        setText("status-text", "Error");
        layout();
        lastDesignDebugState.error = "Invalid JSON in response";
        return "Invalid JSON in response";
    }

    if (diffObj.colors) {
        applyTokenDiff(JSON.stringify({ colors: diffObj.colors }));
    }
    pushThemeSnapshot();

    var dimChanges = diffObj.dimensions || {};
    var dimCount = 0;
    var dimNames = [];
    for (var dk in dimChanges) {
        dimCount++;
        dimNames.push(dk);
        var dv = dimChanges[dk];
        if (dk === "cornerRadius") {
            setBorder("btn-normal", APP_BORDER, 1, dv);
            setBorder("btn-hover", APP_BORDER, 1, dv);
            setBorder("btn-action", APP_ACCENT, 0, dv);
            setBorder("btn-disabled", APP_BORDER, 1, dv);
        } else if (dk === "headingSize") {
            setFontSize("heading-text", dv);
        } else if (dk === "bodySize") {
            setFontSize("body-text", dv);
        } else if (dk === "labelSize") {
            setFontSize("caption-text", dv);
        }
    }

    var widgetLookCount = 0;
    var widgetLookIds = [];
    if (diffObj.widgetLooks) {
        for (var widgetId in diffObj.widgetLooks) {
            if (applyWidgetLook(widgetId, diffObj.widgetLooks[widgetId])) {
                widgetLookCount++;
                widgetLookIds.push(widgetId);
            }
        }
    }

    if (diffObj.widgetShader && inspectedComponent) {
        if (applyWidgetLook(inspectedComponent, { shader: diffObj.widgetShader })) {
            widgetLookCount++;
            widgetLookIds.push(inspectedComponent);
        }
    }

    if (diffObj.widgetSchema && inspectedComponent) {
        if (applyWidgetLook(inspectedComponent, { schema: diffObj.widgetSchema })) {
            widgetLookCount++;
            widgetLookIds.push(inspectedComponent);
        }
    }

    var diffColors = diffObj.colors || {};
    var changedNames = [];
    for (var ck in diffColors) changedNames.push(ck);
    var summary = "Applied " + changedNames.length + " colors";
    if (dimCount > 0) summary += " + " + dimCount + " styles";
    if (widgetLookCount > 0) summary += " + " + widgetLookCount + " widget looks";
    if (changedNames.length > 0 && changedNames.length <= 6) {
        summary += ": " + changedNames.join(", ");
    }
    addChatMessage("assistant", summary);
    updateTokenSwatches();
    updateModifiedCount();
    setText("status-text", (changedNames.length + dimCount + widgetLookCount) + " changes by AI");
    lastDesignDebugState.changedColors = changedNames;
    lastDesignDebugState.changedDimensions = dimNames;
    lastDesignDebugState.widgetLookIds = widgetLookIds;
    lastDesignDebugState.widgetLookCount = widgetLookCount;
    lastDesignDebugState.summary = summary;
    lastDesignDebugState.status = "ok";
    lastDesignDebugState.error = "";
    layout();
    return summary;
}

on("__design-chat__", "result", function(response) {
    try {
        applyDesignChatResponse(response);
    } catch (e) {
        chatRequestPending = false;
        addChatMessage("assistant", "Chat apply failed: " + String(e));
        setText("status-text", "Chat error");
        layout();
    }
});

function buildDesignChatPrompt(text) {
    var themeJson = getThemeJson();
    var scope = inspectedComponent ? "\nScope: ONLY modify tokens related to '" + inspectedComponent + "'" : "";
    var provider = getSelectedAIProvider();
    var model = getSelectedAIModel();
    var reasoningEffort = getSelectedAIReasoningEffort();
    var prompt = "You are a design system expert for audio plugin UIs.\n";
    prompt += "Modify the theme to achieve the requested look. Be creative and bold.\n";
    prompt += "You can change colors, style properties, and widget looks using built-in rendering presets plus material parameters.\n\n";
    prompt += "## Current Theme\n" + themeJson + "\n\n";
    prompt += "## Available Color Tokens (hex values)\n";
    for (var gi = 0; gi < tokenGroups.length; gi++) {
        prompt += tokenGroups[gi].name + ": " + tokenGroups[gi].tokens.join(", ") + "\n";
    }
    prompt += "\n## Available Style Properties (numeric values)\n";
    prompt += "dimensions.cornerRadius (0-24): border radius for panels/buttons\n";
    prompt += "dimensions.borderWidth (0-4): border thickness\n";
    prompt += "dimensions.shadowBlur (0-20): shadow blur radius\n";
    prompt += "dimensions.shadowAlpha (0-1): shadow opacity\n";
    prompt += "dimensions.knobArcWidth (2-8): knob arc stroke width\n";
    prompt += "dimensions.knobSize (32-80): knob diameter\n";
    prompt += "dimensions.headingSize (12-36): heading font size\n";
    prompt += "dimensions.bodySize (10-16): body font size\n";
    prompt += "dimensions.labelSize (8-12): label font size\n";
    prompt += "\n## Widget Targets For Material Restyling\n";
    prompt += "k1, k2, k3, k4 = knobs\n";
    prompt += "slider1 = fader\n";
    prompt += "t1, t2 = toggles\n";
    if (inspectedComponent) {
        prompt += "Current target = " + inspectedComponent + "\n";
    }
    prompt += "\n## Audio Plugin Style Families\n";
    prompt += "precision_analyzer = FabFilter-style precision, restrained chrome, analyzer-forward clarity\n";
    prompt += "heritage_hardware = UA / Softube / Brainworx / Moog metal-and-bakelite studio hardware\n";
    prompt += "retro_character = Soundtoys / Eventide chunky retro character and playful personality\n";
    prompt += "modular_neon = Arturia Pigments / Massive X vivid modular neon and futuristic gloss\n";
    prompt += "mastering_lab = Voxengo-style technical mastering utility with restrained surfaces\n";
    prompt += "console_strip = Waves / Slate console-strip pragmatism and channel-strip grouping\n";
    prompt += "\n## Built-in Material Look Presets\n";
    prompt += "precision_knob, heritage_knob, retro_knob, mastering_knob, macos7_knob, bakelite_knob, led_ring_knob\n";
    prompt += "precision_fader, glass_fader, analog_slider, modular_fader, console_slider\n";
    prompt += "precision_toggle, heritage_toggle, capsule_toggle, illuminated_toggle\n";
    prompt += "notched_knob = declarative schema knob with bolder indicator\n";
    prompt += "minimal_toggle = declarative schema toggle simplification\n";
    prompt += "default = clear any custom shader/schema for a widget\n";
    prompt += "Prefer family + preset for brand/style requests: FabFilter -> precision_analyzer, Arturia/NI cyberpunk -> modular_neon, Soundtoys/Eventide -> retro_character, UA/Softube/Moog/Brainworx -> heritage_hardware, Waves/Slate -> console_strip, Voxengo/mastering -> mastering_lab.\n";
    prompt += "\n## Widget Look Contract\n";
    prompt += "Use widgetLooks when the request asks for a material or physical look change, not just color token changes.\n";
    prompt += "Prefer preset + params. The renderer is deterministic and higher quality than free-form shader code.\n";
    prompt += "You may use family to express the high-level audio-plugin aesthetic, and the runtime will map that deterministically per widget kind.\n";
    prompt += "Available params for preset/material overrides: gloss (0-1), metalness (0-1), rim (0-1), noise (0-0.08), bevel (0-1.5), trackWidth (0.012-0.06), bodyRadius (0.28-0.40), depth (0-1).\n";
    prompt += "Use schemaPreset only when a simple declarative look is enough.\n";
    prompt += "Make the change materially different: body treatment, gloss, bevel, depth, rim, notch, track, or thumb treatment.\n";
    prompt += "Do NOT output raw SkSL shader code unless the user explicitly asks for shader source.\n";
    prompt += "\nUse widgetLooks ONLY when the request asks for a material change in shape, gloss, body treatment, track treatment, or other non-color styling.\n";
    prompt += "\n## RULES\n";
    prompt += "1. Output ONLY valid JSON. No markdown, no explanation.\n";
    prompt += '2. Format: {"colors": {"token.name": "#hex", ...}, "dimensions": {"prop": number, ...}, "widgetLooks": {"widgetId": {"family": "precision_analyzer"|"heritage_hardware"|"retro_character"|"modular_neon"|"mastering_lab"|"console_strip", "preset": "precision_knob"|"heritage_knob"|"retro_knob"|"mastering_knob"|"macos7_knob"|"bakelite_knob"|"led_ring_knob"|"precision_fader"|"glass_fader"|"analog_slider"|"modular_fader"|"console_slider"|"precision_toggle"|"heritage_toggle"|"capsule_toggle"|"illuminated_toggle"|"default", "params": {"gloss": 0.8, "metalness": 0.7, "rim": 0.2, "noise": 0.02, "bevel": 0.9}, "material": {"family": "heritage_hardware", "preset": "heritage_knob", "params": {...}}, "schemaPreset": "notched_knob"|"minimal_toggle"}}}\n';
    prompt += "3. Change 5-30 tokens. Include BOTH colors and dimensions for dramatic effect.\n";
    prompt += "4. Use the exact token/property names listed above.\n";
    prompt += "5. Be bold — if asked for 'cyberpunk', make it look like cyberpunk. If 'warm analog', make it warm.\n";
    prompt += "6. When a specific control is targeted and the request is about its physical look, include widgetLooks for that control.\n";
    prompt += "7. Prefer family + preset first, then material params, then schemaPreset.\n";
    prompt += "8. Never return empty widgetLooks for a targeted control restyle request.\n";
    prompt += "9. Never emit shaderBody or shader unless explicitly asked for shader source by the user.\n";
    prompt += scope + "\n\n";
    if (uploadedImagePath && uploadedImagePath.length > 0) {
        prompt += "## Reference Image\nThe user uploaded a reference image at: " + uploadedImagePath + "\n";
        prompt += "Extract the visual mood, colors, and style from this image and apply them.\n\n";
    }
    prompt += '## Request\n"' + text + '"\n\n## JSON Output\n';
    lastChatRequestText = text || "";
    lastDesignDebugState.provider = provider;
    lastDesignDebugState.model = model;
    lastDesignDebugState.reasoningEffort = reasoningEffort;
    lastDesignDebugState.requestText = text || "";
    lastDesignDebugState.target = inspectedComponent || "all";
    lastDesignDebugState.promptLength = prompt.length;
    return prompt;
}

function submitChat(text) {
    if (chatRequestPending) return;
    if (!text || text.length === 0) {
        if (uploadedImagePath && uploadedImagePath.length > 0) {
            showToast("Add a message before sending the image");
        }
        return;
    }

    var userText = uploadedImageName ? (text + " [image attached: " + uploadedImageName + "]") : text;
    lastChatRequestText = text;
    addChatMessage("user", userText);
    setText("chat-input", "");
    setText("status-text", "Generating...");
    chatRequestPending = true;
    // #58: Show typing indicator
    addChatMessage("assistant", "...");
    layout();

    var provider = getSelectedAIProvider();
    var model = getSelectedAIModel();
    var reasoningEffort = getSelectedAIReasoningEffort();
    var prompt = buildDesignChatPrompt(text);

    var tmpFile = "/tmp/pulp-design-prompt-" + (chatRequestCounter++) + ".txt";
    exec("cat > " + tmpFile + " << 'PULPEOF'\n" + prompt + "\nPULPEOF");
    execAsync(buildAiCliCommand(tmpFile, model, provider, reasoningEffort), "__design-chat__");
    clearUploadedImage();
    updateChatInputSizing("");
}

on("chat-input", "return", function(text) {
    submitChat(text);
});

// Send button triggers same as return key
registerClick("send-btn");
on("send-btn", "click", function() {
    var text = getText("chat-input");
    submitChat(text);
});

// Preset handler
on("preset-selector", "select", function(idx) {
    var presets = [
        { theme: "dark", accent: "#89B4FA" },
        { theme: "light", accent: "#2563EB" },
        { theme: "pro_audio", accent: "#89B4FA" },
        { theme: "dark", accent: "#AA88FF" },
        { theme: "dark", accent: "#D4A017" },
        { theme: "dark", accent: "#0EA5E9" },
        { theme: "dark", accent: "#FF00FF" }
    ];
    var p = presets[idx];
    currentAccent = p.accent;
    setSelected("preset-selector", idx);
    setTheme(p.theme);
    setText("theme-name-label", ["Default Dark","Light","Pro Audio","Violet","Amber","Ocean","Neon"][idx]);
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    applyTokenDiff(PaletteSystem.toThemeDiff(palette));
    updateTokenSwatches();
    var steps = ShadeGenerator.STEPS;
    for (var pp = 0; pp < paletteKeys.length; pp++) {
        var ramp = palette[paletteKeys[pp]];
        for (var s = 0; s < steps.length; s++) {
            setBackground("ramp-" + pp + "-s" + s, ramp[steps[s]].hex);
        }
        setBackground("ramp-" + pp + "-dot", ramp[500].hex);
    }
    if (expandedPalette >= 0) {
        applyPaletteExpandedLayout(expandedPalette, false);
        expandedPalette = -1;
        updateLeftPanelScrollMetrics();
    }
    pushThemeSnapshot();
    layout();
});

// ═══════════════════════════════════════════════════════════════════
// Export/Import buttons
// ═══════════════════════════════════════════════════════════════════
// D4: Multi-format export
// #57: 5 export formats including C++ Header and Palette
var exportFormats = ["JSON", "CSS Vars", "OKLCH", "C++ Header", "C++ Palette"];
var activeExportFormat = 0;

function generateExport(formatIdx) {
    var json = getThemeJson();
    var theme = JSON.parse(json);
    var colors = theme.colors || {};
    if (formatIdx === 0) return json;
    if (formatIdx === 1) {
        var css = ":root {\n";
        for (var k in colors) css += "  --pulp-" + k.replace(/\./g, "-") + ": " + colors[k] + ";\n";
        css += "}\n";
        return css;
    }
    if (formatIdx === 2) {
        var oklch = "/* OKLCH Color Tokens */\n:root {\n";
        for (var k in colors) {
            var o = OklchEngine.hexToOklch(colors[k]);
            oklch += "  --pulp-" + k.replace(/\./g, "-") + ": oklch(" + (o.L*100).toFixed(1) + "% " + o.C.toFixed(3) + " " + o.H.toFixed(1) + ");\n";
        }
        oklch += "}\n";
        return oklch;
    }
    if (formatIdx === 3) {
        // C++ Header — PULP_THEME_COLOR macros
        var cpp = "#pragma once\n// Generated by Pulp Style Designer\n\n";
        cpp += "namespace pulp_theme {\n\n";
        for (var k in colors) {
            var hex = colors[k].replace("#", "");
            var name = k.replace(/\./g, "_");
            cpp += "PULP_THEME_COLOR(" + name + ", 0xFF" + hex.toUpperCase() + ")\n";
        }
        cpp += "\n} // namespace pulp_theme\n";
        return cpp;
    }
    if (formatIdx === 4) {
        // C++ Palette — palette.setColor() calls
        var pal = "// Generated by Pulp Style Designer\n";
        pal += "inline void initThemePalette(pulp::Palette& palette) {\n";
        for (var k in colors) {
            var hex = colors[k].replace("#", "");
            var name = k.replace(/\./g, "_");
            pal += "    palette.setColor(pulp::" + name + ", pulp::Color(0xFF" + hex.toUpperCase() + "));\n";
        }
        pal += "}\n";
        return pal;
    }
    return json;
}

// Export popup overlay
createCol("export-popup", "");
setPosition("export-popup", "absolute");
setFlex("export-popup", "width", 480);
setFlex("export-popup", "height", 400);
setFlex("export-popup", "padding", 16);
setFlex("export-popup", "gap", 10);
setBackground("export-popup", APP_PANEL);
setBorder("export-popup", APP_BORDER, 1, 10);
setBoxShadow("export-popup", 0, 16, 48, 0, "#000000c0");
setZIndex("export-popup", 150);
setVisible("export-popup", false);

// Export header
createRow("exp-header", "export-popup");
setFlex("exp-header", "height", 24);
setFlex("exp-header", "align_items", "center");
createLabel("exp-title", "Export Theme", "exp-header");
setFontSize("exp-title", 14);
setFlex("exp-title", "flex_grow", 1);

createCol("exp-close", "exp-header");
setFlex("exp-close", "width", 22);
setFlex("exp-close", "height", 22);
setFlex("exp-close", "justify_content", "center");
setFlex("exp-close", "align_items", "center");
createLabel("exp-close-lbl", "x", "exp-close");
setFontSize("exp-close-lbl", 12);
registerClick("exp-close");
on("exp-close", "click", function() { setVisible("export-popup", false); layout(); });

// Format tabs
createRow("exp-tabs", "export-popup");
setFlex("exp-tabs", "height", 26);
setFlex("exp-tabs", "gap", 2);
for (var ef = 0; ef < exportFormats.length; ef++) {
    var efId = "exp-tab-" + ef;
    createCol(efId, "exp-tabs");
    setFlex(efId, "flex_grow", 1);
    setFlex(efId, "height", 26);
    setFlex(efId, "justify_content", "center");
    setFlex(efId, "align_items", "center");
    setBorder(efId, ef === 0 ? APP_ACCENT : APP_BORDER, 1, 4);
    createLabel(efId + "-lbl", exportFormats[ef], efId);
    setFontSize(efId + "-lbl", 10);
    setTextColor(efId + "-lbl", ef === 0 ? APP_ACCENT : APP_TEXT_DIM);
    registerClick(efId);
    (function(idx) {
        on("exp-tab-" + idx, "click", function() {
            activeExportFormat = idx;
            for (var i = 0; i < exportFormats.length; i++) {
                setBorder("exp-tab-" + i, i === idx ? APP_ACCENT : APP_BORDER, 1, 4);
                setTextColor("exp-tab-" + i + "-lbl", i === idx ? APP_ACCENT : APP_TEXT_DIM);
            }
            setText("exp-code", generateExport(idx));
        });
    })(ef);
}

// Code preview (scrollable text)
createScrollView("exp-code-scroll", "export-popup");
setFlex("exp-code-scroll", "flex_grow", 1);
setBackground("exp-code-scroll", APP_BG);
setBorder("exp-code-scroll", APP_BORDER, 1, 4);
setScrollContentSize("exp-code-scroll", 440, 1200);

createLabel("exp-code", "", "exp-code-scroll");
setFontSize("exp-code", 10);
setFlex("exp-code", "padding", 8);
setFlex("exp-code", "width", 440);

// Action buttons
createRow("exp-actions", "export-popup");
setFlex("exp-actions", "height", 28);
setFlex("exp-actions", "gap", 8);
setFlex("exp-actions", "justify_content", "flex-end");

createCol("exp-copy-btn", "exp-actions");
setFlex("exp-copy-btn", "width", 80);
setFlex("exp-copy-btn", "height", 28);
setFlex("exp-copy-btn", "justify_content", "center");
setFlex("exp-copy-btn", "align_items", "center");
setBorder("exp-copy-btn", APP_BORDER, 1, 4);
createLabel("exp-copy-lbl", "Copy", "exp-copy-btn");
setFontSize("exp-copy-lbl", 10);
registerClick("exp-copy-btn");
on("exp-copy-btn", "click", function() {
    var code = generateExport(activeExportFormat);
    exec("echo " + JSON.stringify(code) + " | pbcopy");
    showToast("Copied to clipboard");
});

createCol("exp-save-btn", "exp-actions");
setFlex("exp-save-btn", "width", 80);
setFlex("exp-save-btn", "height", 28);
setFlex("exp-save-btn", "justify_content", "center");
setFlex("exp-save-btn", "align_items", "center");
setBackground("exp-save-btn", APP_ACCENT);
setBorder("exp-save-btn", APP_ACCENT, 0, 4);
createLabel("exp-save-lbl", "Save", "exp-save-btn");
setFontSize("exp-save-lbl", 10);
registerClick("exp-save-btn");
on("exp-save-btn", "click", function() {
    var code = generateExport(activeExportFormat);
    var ext = [".json", ".css", ".css"][activeExportFormat];
    var path = "/tmp/pulp-theme" + ext;
    exec("cat > " + path + " << 'PULPEOF'\n" + code + "\nPULPEOF");
    showToast("Saved to " + path);
});

registerClick("export-btn-pill");
on("export-btn-pill", "click", function() {
    setText("exp-code", generateExport(activeExportFormat));
    setTop("export-popup", 60);
    setLeft("export-popup", 200);
    setVisible("export-popup", true);
    layout();
});

registerClick("import-btn-pill");
on("import-btn-pill", "click", function() {
    var path = "/tmp/pulp-theme-export.json";
    var json = exec("cat " + path + " 2>/dev/null");
    if (json && json.length > 10) {
        applyTokenDiff(json);
        updateTokenSwatches();
        setText("status-text", "Imported from " + path);
    } else {
        setText("status-text", "No theme file found");
    }
    layout();
});

// Undo/Redo (snapshot-based)
var themeHistory = [];
var historyIndex = -1;

function pushThemeSnapshot() {
    var snap = getThemeJson();
    if (historyIndex < themeHistory.length - 1) {
        themeHistory = themeHistory.slice(0, historyIndex + 1);
    }
    themeHistory.push(snap);
    historyIndex = themeHistory.length - 1;
}
pushThemeSnapshot();

registerClick("undo-btn-pill");
on("undo-btn-pill", "click", function() {
    if (historyIndex > 0) {
        historyIndex--;
        applyTokenDiff(themeHistory[historyIndex]);
        updateTokenSwatches();
        setText("status-text", "Undo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
        layout();
    }
});

registerClick("redo-btn-pill");
on("redo-btn-pill", "click", function() {
    if (historyIndex < themeHistory.length - 1) {
        historyIndex++;
        applyTokenDiff(themeHistory[historyIndex]);
        updateTokenSwatches();
        setText("status-text", "Redo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
        layout();
    }
});

layout();
