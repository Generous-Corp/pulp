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
var APP_BORDER  = '#2e2e36';
var APP_TEXT    = '#d4d4dc';
var APP_TEXT_DIM = '#808090';
var APP_ACCENT  = '#aa88ff';

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
setFlex("toolbar", "padding_left", 12);
setFlex("toolbar", "padding_right", 12);
setFlex("toolbar", "align_items", "center");
setFlex("toolbar", "justify_content", "space-between");
setBackground("toolbar", APP_SURFACE);
setBorder("toolbar", APP_BORDER, 1, 0);

// Toolbar items (flat layout)
createLabel("theme-name-label", "Default Dark", "toolbar");
setFontSize("theme-name-label", 13);
setFlex("theme-name-label", "width", 90);

createCombo("preset-selector", "toolbar");
setItems("preset-selector", ["Default Dark", "Light", "Pro Audio", "Violet", "Amber", "Ocean", "Neon"]);
setFlex("preset-selector", "width", 120);
setFlex("preset-selector", "height", 26);

// D3: State scrubber pills
var stateNames = ["Default", "Hover", "Focus", "Disabled", "Error"];
var activeState = 0;
createRow("state-pills", "toolbar");
setFlex("state-pills", "height", 26);
setFlex("state-pills", "gap", 2);
setFlex("state-pills", "align_items", "center");
setFlex("state-pills", "padding_left", 8);
setFlex("state-pills", "padding_right", 8);
setBorder("state-pills", APP_BORDER, 1, 4);

for (var sp = 0; sp < stateNames.length; sp++) {
    var spId = "state-pill-" + sp;
    createCol(spId, "state-pills");
    setFlex(spId, "height", 24);
    setFlex(spId, "padding_left", 12);
    setFlex(spId, "padding_right", 12);
    setFlex(spId, "justify_content", "center");
    setFlex(spId, "align_items", "center");
    if (sp === 0) {
        setBackground(spId, '#2a2040');
        setBorder(spId, APP_ACCENT, 1, 4);
    } else {
        setBorder(spId, "transparent", 0, 4);
    }
    createLabel(spId + "-lbl", stateNames[sp], spId);
    setFontSize(spId + "-lbl", 11);
    setTextColor(spId + "-lbl", sp === 0 ? APP_ACCENT : APP_TEXT_DIM);
    registerClick(spId);
    (function(idx) {
        on("state-pill-" + idx, "click", function() {
            // Update pill visuals
            for (var si = 0; si < stateNames.length; si++) {
                setBackground("state-pill-" + si, si === idx ? '#2a2040' : 'transparent');
                setBorder("state-pill-" + si, si === idx ? APP_ACCENT : "transparent", si === idx ? 1 : 0, 4);
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

    if (stateIdx === 1) { // Hover
        setBackground("btn-normal", "#4a4a5c");
        setBackground("btn-hover", "#5a5a6c");
        setBorder("btn-normal", APP_ACCENT, 1, 6);
        setBorder("btn-action", APP_ACCENT, 1, 6);
    } else if (stateIdx === 2) { // Focus
        setBorder("btn-normal", APP_ACCENT, 2, 6);
        setBorder("btn-hover", APP_ACCENT, 2, 6);
        setBorder("btn-action", APP_ACCENT, 2, 6);
    } else if (stateIdx === 3) { // Disabled
        setOpacity("btn-normal", 0.4);
        setOpacity("btn-hover", 0.4);
        setOpacity("btn-action", 0.4);
        setOpacity("btn-disabled", 0.4);
    } else if (stateIdx === 4) { // Error
        setBorder("btn-normal", "#e94560", 1, 6);
        setBorder("btn-hover", "#e94560", 1, 6);
        setBackground("btn-action", "#e94560");
    }
    layout();
}

// Spacer
createCol("toolbar-spacer", "toolbar");
setFlex("toolbar-spacer", "flex_grow", 1);

// Toolbar action buttons with pill styling
var toolbarBtns = [
    { id: "undo-btn", label: "Undo", width: 44 },
    { id: "redo-btn", label: "Redo", width: 44 },
    { id: "import-btn", label: "Import", width: 52 },
    { id: "export-btn", label: "Export", width: 52, accent: true }
];
for (var tb = 0; tb < toolbarBtns.length; tb++) {
    var btn = toolbarBtns[tb];
    createCol(btn.id + "-pill", "toolbar");
    setFlex(btn.id + "-pill", "width", btn.width);
    setFlex(btn.id + "-pill", "height", 26);
    setFlex(btn.id + "-pill", "justify_content", "center");
    setFlex(btn.id + "-pill", "align_items", "center");
    setBorder(btn.id + "-pill", APP_BORDER, 1, 4);

    createLabel(btn.id, btn.label, btn.id + "-pill");
    setFontSize(btn.id, 11);
    if (btn.accent) setTextColor(btn.id, APP_ACCENT);
}

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
setBackground("left-panel", APP_SURFACE);
setBorder("left-panel", APP_BORDER, 1, 0);
setScrollContentSize("left-panel", 310, 1600);

// Issue 9: Color System section matching HTML reference
createCol("color-section", "left-panel");
setFlex("color-section", "padding", 10);
setFlex("color-section", "gap", 6);

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
setFlex("template-lbl", "width", 52);
createCombo("template-selector", "template-row");
setItems("template-selector", ["Audio Studio", "Tailwind 4"]);
setFlex("template-selector", "flex_grow", 1);
setFlex("template-selector", "height", 22);
// #56: ? info button
createLabel("template-help", "?", "template-row");
setFontSize("template-help", 9);
setTextColor("template-help", APP_TEXT_DIM);
registerClick("template-help");
on("template-help", "click", function() {
    showToast("Template: base color palette preset (Audio Studio or Tailwind)");
});

// Harmony selector row
createRow("harmony-row", "color-section");
setFlex("harmony-row", "height", 22);
setFlex("harmony-row", "gap", 6);
setFlex("harmony-row", "align_items", "center");
createLabel("harmony-lbl", "Harmony", "harmony-row");
setFontSize("harmony-lbl", 9);
setTextColor("harmony-lbl", APP_TEXT_DIM);
setFlex("harmony-lbl", "width", 52);
createCombo("harmony-selector", "harmony-row");
setItems("harmony-selector", ["Monochromatic", "Analogous", "Complementary", "Split Comp.", "None"]);
setFlex("harmony-selector", "flex_grow", 1);
setFlex("harmony-selector", "height", 22);
createLabel("harmony-help", "?", "harmony-row");
setFontSize("harmony-help", 9);
setTextColor("harmony-help", APP_TEXT_DIM);
registerClick("harmony-help");
on("harmony-help", "click", function() {
    showToast("Harmony: how semantic colors relate to the accent hue");
});

// Mode selector row
createRow("mode-row", "color-section");
setFlex("mode-row", "height", 22);
setFlex("mode-row", "gap", 6);
setFlex("mode-row", "align_items", "center");
createLabel("mode-lbl", "Mode", "mode-row");
setFontSize("mode-lbl", 9);
setTextColor("mode-lbl", APP_TEXT_DIM);
setFlex("mode-lbl", "width", 52);
createCombo("mode-selector", "mode-row");
setItems("mode-selector", ["Dark", "Light"]);
setFlex("mode-selector", "flex_grow", 1);
setFlex("mode-selector", "height", 22);
createLabel("mode-help", "?", "mode-row");
setFontSize("mode-help", 9);
setTextColor("mode-help", APP_TEXT_DIM);
registerClick("mode-help");
on("mode-help", "click", function() {
    showToast("Mode: dark assigns light shades to text, light inverts");
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
setFlex("token-search-row", "padding_left", 10);
setFlex("token-search-row", "padding_right", 10);
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
setFlex("token-header", "align_items", "center");

createLabel("tokens-title", "TOKENS", "token-header");
setFontSize("tokens-title", 10);
setTextColor("tokens-title", APP_TEXT_DIM);

// Token list (inside left-panel scroll, no nested scroll needed)
createCol("token-list", "left-panel");
setFlex("token-list", "height", 800);

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

for (var g = 0; g < tokenGroups.length; g++) {
    var group = tokenGroups[g];
    var gid = "tg-" + g;
    // No fixed height — let flex layout compute from visible children (CSS auto height)
    createCol(gid, "token-list");
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
    query = (query || "").toLowerCase();
    for (var g = 0; g < tokenGroups.length; g++) {
        var group = tokenGroups[g];
        var anyVisible = false;
        for (var t = 0; t < group.tokens.length; t++) {
            var tid = "tok-" + g + "-" + t;
            var visible = query.length === 0 || group.tokens[t].toLowerCase().indexOf(query) >= 0;
            setVisible(tid, visible);
            if (visible) anyVisible = true;
        }
        setVisible("tg-" + g, anyVisible || query.length === 0);
    }
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
}
updateTokenSwatches();

// ── Color System: OKLCH Shade Ramps ──────────────────────────────
var paletteNames = ["Accent", "Neutral", "Success", "Warning", "Error"];
var paletteKeys  = ["accent", "neutral", "success", "warning", "error"];

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

        // Palette row: dot + name + shade ramp (clickable to expand)
        var rowId = rampId + "-header";
        createRow(rowId, rampId);
        setFlex(rowId, "height", 24);
        setFlex(rowId, "gap", 6);
        setFlex(rowId, "align_items", "center");
        registerClick(rowId);

        var ramp = palette[paletteKeys[p]];

        // Base color dot (clickable to expand)
        var dotId = rampId + "-dot";
        createCol(dotId, rowId);
        setFlex(dotId, "width", 14);
        setFlex(dotId, "height", 14);
        setBackground(dotId, ramp[500].hex);
        setBorder(dotId, ramp[500].hex, 0, 7);
        registerClick(dotId);

        // Name (clickable to expand)
        createLabel(rampId + "-name", paletteNames[p], rowId);
        setFontSize(rampId + "-name", 10);
        setFlex(rampId + "-name", "width", 52);
        registerClick(rampId + "-name");

        // Mini ramp
        createRow(rampId + "-row", rowId);
        setFlex(rampId + "-row", "flex_grow", 1);
        setFlex(rampId + "-row", "gap", 1);
        setFlex(rampId + "-row", "height", 16);

        for (var s = 0; s < steps.length; s++) {
            var shadeId = rampId + "-s" + s;
            createCol(shadeId, rampId + "-row");
            setFlex(shadeId, "flex_grow", 1);
            setFlex(shadeId, "height", 14);
            setBackground(shadeId, ramp[steps[s]].hex);
            setBorder(shadeId, APP_BORDER, 0, 2);
            // Click shade → expand this palette with that shade's color
            registerClick(shadeId);
        }

        // Expanded editor section (hidden unless this palette is expanded)
        var editorId = rampId + "-editor";
        createCol(editorId, rampId);
        setFlex(editorId, "gap", 6);
        setFlex(editorId, "padding_left", 4);
        setVisible(editorId, p === expandedPalette);

        // Gamut triangle canvas (draggable for dot positioning)
        var gamutId = rampId + "-gamut";
        createCanvas(gamutId, editorId);
        setFlex(gamutId, "width", 270);
        setFlex(gamutId, "height", 130);
        setBackground(gamutId, APP_SURFACE);  // opaque bg prevents content bleed-through
        // Enable pointer events for drag interaction on the gamut
        registerPointer(gamutId);
        (function(idx, pKey) {
            // Handle both pointerdown and pointermove (drag) on gamut
            var dragCount = 0;
            function onGamutPointer(evt) {
                var gw = 270, gh = 130;
                var x = evt.offsetX, y = evt.offsetY;
                var L = Math.max(0, Math.min(1, x / gw));
                var C = Math.max(0, Math.min(0.4, (1 - y / gh) * 0.4));
                var h = getValue("ramp-" + idx + "-h-fdr") * 360;
                var mapped = OklchEngine.gamutMap(L, C, h);
                setValue("ramp-" + idx + "-c-fdr", Math.min(mapped.C / 0.4, 1));
                // Full gamut redraw (cached — skips grid if hue unchanged)
                renderPaletteGamut(idx, h, mapped.L, mapped.C);
                setText("ramp-" + idx + "-oklch", "L: " + mapped.L.toFixed(2) + "  C: " + mapped.C.toFixed(3) + "  H: " + h.toFixed(1));
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
            on(gamutId, "pointerdown", function(e) { dragCount = 0; onGamutPointer(e); });
            on(gamutId, "pointermove", onGamutPointer);
        })(p, paletteKeys[p]);

        // H slider — rainbow hue gradient bar + fader below
        var hGradId = rampId + "-h-grad";
        createCanvas(hGradId, editorId);
        setFlex(hGradId, "height", 12);
        setBorder(hGradId, APP_BORDER, 0, 6);
        // Draw rainbow hue gradient (red → orange → yellow → green → cyan → blue → violet → pink)
        var hGradW = 260;
        for (var hg = 0; hg < 48; hg++) {
            var hueVal = (hg / 48) * 360;
            canvasRect(hGradId, (hg / 48) * hGradW, 0, (hGradW / 48) + 1, 12, OklchEngine.oklchToHex(0.65, 0.25, hueVal));
        }
        // H fader (interactive)
        var hRowId = rampId + "-h-row";
        createRow(hRowId, editorId);
        setFlex(hRowId, "height", 16);
        setFlex(hRowId, "gap", 6);
        setFlex(hRowId, "align_items", "center");
        createLabel(rampId + "-h-lbl", "H", hRowId);
        setFontSize(rampId + "-h-lbl", 9);
        setFlex(rampId + "-h-lbl", "width", 12);
        createFader(rampId + "-h-fdr", "horizontal", hRowId);
        setFlex(rampId + "-h-fdr", "flex_grow", 1);
        setFlex(rampId + "-h-fdr", "height", 14);

        // C slider — chroma gradient bar (gray to saturated) + fader below
        var cGradId = rampId + "-c-grad";
        createCanvas(cGradId, editorId);
        setFlex(cGradId, "height", 12);
        setBorder(cGradId, APP_BORDER, 0, 6);
        // C fader (interactive)
        var cRowId = rampId + "-c-row";
        createRow(cRowId, editorId);
        setFlex(cRowId, "height", 16);
        setFlex(cRowId, "gap", 6);
        setFlex(cRowId, "align_items", "center");
        createLabel(rampId + "-c-lbl", "C", cRowId);
        setFontSize(rampId + "-c-lbl", 9);
        setFlex(rampId + "-c-lbl", "width", 12);
        createFader(rampId + "-c-fdr", "horizontal", cRowId);
        setFlex(rampId + "-c-fdr", "flex_grow", 1);
        setFlex(rampId + "-c-fdr", "height", 14);

        // OKLCH display
        createLabel(rampId + "-oklch", "L: 0.50  C: 0.100  H: 180", editorId);
        setFontSize(rampId + "-oklch", 9);
        setTextColor(rampId + "-oklch", APP_TEXT_DIM);
        setFlex(rampId + "-oklch", "height", 14);

        // #54: Large shade swatches with WCAG contrast badges
        createRow(rampId + "-shades", editorId);
        setFlex(rampId + "-shades", "gap", 3);
        setFlex(rampId + "-shades", "height", 32);
        for (var ls = 0; ls < 6; ls++) {
            var lsId = rampId + "-lg-" + ls;
            var stepIdx = [0, 2, 4, 5, 7, 9][ls];
            var shadeHex = ramp[steps[stepIdx]].hex;
            createCol(lsId, rampId + "-shades");
            setFlex(lsId, "flex_grow", 1);
            setFlex(lsId, "height", 32);
            setBackground(lsId, shadeHex);
            setBorder(lsId, APP_BORDER, 0, 6);
            setFlex(lsId, "justify_content", "center");
            setFlex(lsId, "align_items", "center");
            // WCAG contrast badge
            var ratio = OklchEngine.contrastRatio(shadeHex, "#ffffff");
            var level = OklchEngine.contrastLevel(ratio);
            if (level !== "fail") {
                createLabel(lsId + "-badge", level, lsId);
                setFontSize(lsId + "-badge", 7);
                setTextColor(lsId + "-badge", ratio > 4.5 ? "#ffffff" : "#000000");
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
            setText(rampId + "-oklch", "L: " + oklch.L.toFixed(2) + "  C: " + oklch.C.toFixed(3) + "  H: " + oklch.H.toFixed(1));
        }

        // Click dot or name → toggle expand (just visibility, no rebuild)
        (function(idx) {
            var toggleExpand = function() {
                if (expandedPalette === idx) {
                    // Collapse this palette
                    setVisible("ramp-" + idx + "-editor", false);
                    expandedPalette = -1;
                } else {
                    // Collapse previous
                    if (expandedPalette >= 0) {
                        setVisible("ramp-" + expandedPalette + "-editor", false);
                    }
                    // Expand this one
                    expandedPalette = idx;
                    setVisible("ramp-" + idx + "-editor", true);
                    // Render gamut triangle for this palette
                    var pal = PaletteSystem.create(currentAccent, currentHarmony);
                    var pKey = paletteKeys[idx];
                    var base = pal[pKey][500];
                    var oklch = OklchEngine.hexToOklch(base.hex);
                    renderPaletteGamut(idx, oklch.H, oklch.L, oklch.C);
                    renderChromaGradient(idx, oklch.H);
                    setValue("ramp-" + idx + "-h-fdr", oklch.H / 360);
                    setValue("ramp-" + idx + "-c-fdr", Math.min(oklch.C / 0.4, 1));
                    setText("ramp-" + idx + "-oklch", "L: " + oklch.L.toFixed(2) + "  C: " + oklch.C.toFixed(3) + "  H: " + oklch.H.toFixed(1));
                }
                layout();
            };
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
                            setVisible("ramp-" + expandedPalette + "-editor", false);
                        }
                        expandedPalette = paletteIdx;
                        setVisible("ramp-" + paletteIdx + "-editor", true);
                        renderPaletteGamut(paletteIdx, oklch.H, oklch.L, oklch.C);
                        renderChromaGradient(paletteIdx, oklch.H);
                        setValue("ramp-" + paletteIdx + "-h-fdr", oklch.H / 360);
                        setValue("ramp-" + paletteIdx + "-c-fdr", Math.min(oklch.C / 0.4, 1));
                        setText("ramp-" + paletteIdx + "-oklch", "L: " + oklch.L.toFixed(2) + "  C: " + oklch.C.toFixed(3) + "  H: " + oklch.H.toFixed(1));
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
                setText("ramp-" + idx + "-oklch", "L: " + mapped.L.toFixed(2) + "  C: " + mapped.C.toFixed(3) + "  H: " + h.toFixed(1));
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

function renderPaletteGamut(paletteIdx, hue, dotL, dotC, fullRedraw) {
    var gamutId = "ramp-" + paletteIdx + "-gamut";
    var w = 270, h = 130;

    // Full redraw: render the entire color grid (expensive)
    if (fullRedraw !== false || gamutCache[paletteIdx] !== hue) {
        gamutCache[paletteIdx] = hue;
        canvasClear(gamutId);
        // Background
        canvasRect(gamutId, 0, 0, w, h, APP_SURFACE);

        // Precompute boundary
        var boundary = [];
        var bSteps = 200;
        for (var bx = 0; bx <= bSteps; bx++) {
            var bL = bx / bSteps;
            var blo = 0, bhi = 0.4;
            for (var bbi = 0; bbi < 16; bbi++) {
                var bmid = (blo + bhi) / 2;
                if (OklchEngine.isInGamut(bL, bmid, hue)) blo = bmid; else bhi = bmid;
            }
            boundary.push(blo);
        }

        // Draw color grid — high resolution for smooth edges
        var cols = 135, rows = 65;
        var cellW = w / cols, cellH = h / rows;
        for (var gx = 0; gx < cols; gx++) {
            var L = gx / (cols - 1);
            var bIdx = Math.round(L * bSteps);
            var maxC = boundary[bIdx];
            for (var gy = 0; gy < rows; gy++) {
                var C = (1 - gy / (rows - 1)) * 0.4;
                if (C <= maxC) {
                    canvasRect(gamutId, gx * cellW, gy * cellH, cellW + 1, cellH + 1, OklchEngine.oklchToHex(L, C, hue));
                }
            }
        }

        // Mask out-of-gamut area — paint the dark background over the jagged edges
        // using a path that covers everything ABOVE the smooth gamut boundary
        canvasSetFillColor(gamutId, '#1e1e26');
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, 0);  // top-left
        canvasLineTo(gamutId, w, 0);  // top-right
        canvasLineTo(gamutId, w, (1 - boundary[bSteps] / 0.4) * h);  // right boundary point
        for (var mx = bSteps; mx >= 0; mx--) {
            canvasLineTo(gamutId, (mx / bSteps) * w, (1 - boundary[mx] / 0.4) * h);
        }
        canvasClosePath(gamutId);
        canvasFillPath(gamutId);

        // Smooth boundary stroke
        canvasSetStrokeColor(gamutId, '#ffffff20');
        canvasSetLineWidth(gamutId, 1);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, (1 - boundary[0] / 0.4) * h);
        for (var bx2 = 1; bx2 <= bSteps; bx2++) {
            canvasLineTo(gamutId, (bx2 / bSteps) * w, (1 - boundary[bx2] / 0.4) * h);
        }
        canvasStrokePath(gamutId);
    }

    // Draw dot + crosshair (always, even during drag-only updates)
    if (dotL !== undefined && dotC !== undefined) {
        var dx = dotL * w;
        var dy = (1 - dotC / 0.4) * h;
        // Crosshair lines
        canvasStrokeLine(gamutId, dx, 0, dx, h, '#ffffff15', 0.5);
        canvasStrokeLine(gamutId, 0, dy, w, dy, '#ffffff15', 0.5);
        // Dot: shadow → white ring → color center
        canvasFillCircle(gamutId, dx, dy, 9, '#00000040');
        canvasFillCircle(gamutId, dx, dy, 7, '#ffffffdd');
        canvasFillCircle(gamutId, dx, dy, 4, OklchEngine.oklchToHex(dotL, dotC, hue));
    }
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
    var cols = 60;
    var colW = w / cols;
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
            setVisible("ramp-" + expandedPalette + "-editor", false);
            expandedPalette = -1;
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
        setVisible("ramp-" + expandedPalette + "-editor", false);
        expandedPalette = -1;
    }
    pushThemeSnapshot();
    layout();
});

// ── CENTER PANEL (Preview) ───────────────────────────────────────
createCol("center-panel", "main-area");
setFlex("center-panel", "flex_grow", 1);
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
    waveData.push(Math.sin(2 * Math.PI * 3 * i / 512) * 0.7 +
                  Math.sin(2 * Math.PI * 7 * i / 512) * 0.3);
}
setWaveformData("waveform", waveData);

// Meters
createRow("meter-row", "preview-area");
setFlex("meter-row", "gap", 4);
setFlex("meter-row", "height", 48);

createMeter("m1", "vertical", "meter-row");
setFlex("m1", "width", 12);
setMeterLevel("m1", 0.75, 0.88);

createMeter("m2", "vertical", "meter-row");
setFlex("m2", "width", 12);
setMeterLevel("m2", 0.55, 0.72);

createMeter("m3", "vertical", "meter-row");
setFlex("m3", "width", 12);
setMeterLevel("m3", 0.3, 0.45);

createMeter("m4", "vertical", "meter-row");
setFlex("m4", "width", 12);
setMeterLevel("m4", 0.85, 0.95);

// Layout section: 2x2 card grid
createLabel("layout-header", "Layout", "preview-area");
setFontSize("layout-header", 11);
setTextColor("layout-header", APP_TEXT_DIM);
setFlex("layout-header", "height", 16);

createRow("card-grid-top", "preview-area");
setFlex("card-grid-top", "gap", 8);
setFlex("card-grid-top", "height", 56);

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
    setFlex(crId, "gap", 8);
    setFlex(crId, "height", 56);
    var indices = cardRows[cr][1];
    for (var ci = 0; ci < indices.length; ci++) {
        var cd = cardDefs[indices[ci]];
        createCol(cd.id, crId);
        setFlex(cd.id, "flex_grow", 1);
        setBackground(cd.id, cd.bg);
        setBorder(cd.id, cd.border, 1, 8);
        setFlex(cd.id, "padding", 8);
        setFlex(cd.id, "justify_content", "center");
        setFlex(cd.id, "align_items", "center");
        createLabel(cd.id + "-label", cd.label, cd.id);
        setFontSize(cd.id + "-label", 10);
        setTextColor(cd.id + "-label", cd.loading ? APP_TEXT_DIM : APP_TEXT);
        if (cd.badge) {
            // Badge in top-right corner
            createCol(cd.id + "-badge", cd.id);
            setFlex(cd.id + "-badge", "width", 22);
            setFlex(cd.id + "-badge", "height", 16);
            setFlex(cd.id + "-badge", "justify_content", "center");
            setFlex(cd.id + "-badge", "align_items", "center");
            setBackground(cd.id + "-badge", cd.badgeColor);
            setBorder(cd.id + "-badge", cd.badgeColor, 0, 4);
            createLabel(cd.id + "-badge-lbl", cd.badge, cd.id + "-badge");
            setFontSize(cd.id + "-badge-lbl", 8);
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

createLabel("spinner-label", "\u25E0 Loading...", "progress-row");
setFontSize("spinner-label", 11);
setTextColor("spinner-label", APP_ACCENT);
setFlex("spinner-label", "width", 90);

// Animate spinner character rotation
var spinnerFrames = ["\u25DC", "\u25DD", "\u25DE", "\u25DF"];
var spinnerIdx = 0;
function tickSpinner() {
    spinnerIdx = (spinnerIdx + 1) % spinnerFrames.length;
    setText("spinner-label", spinnerFrames[spinnerIdx] + " Loading...");
    __requestFrame__(tickSpinner);
}
__requestFrame__(tickSpinner);

// ── Tab Bar (General / Audio / MIDI / About) ─────────────────────
createLabel("tabs-header", "Tabs", "preview-area");
setFontSize("tabs-header", 11);
setTextColor("tabs-header", APP_TEXT_DIM);

createRow("tab-bar-preview", "preview-area");
setFlex("tab-bar-preview", "height", 30);
setFlex("tab-bar-preview", "gap", 0);
setFlex("tab-bar-preview", "align_items", "center");
setBorder("tab-bar-preview", APP_BORDER, 1, 0);

var tabNames = ["General", "Audio", "MIDI", "About"];
for (var ti = 0; ti < tabNames.length; ti++) {
    var tabId = "ptab-" + ti;
    createCol(tabId, "tab-bar-preview");
    setFlex(tabId, "flex_grow", 1);
    setFlex(tabId, "height", 30);
    setFlex(tabId, "justify_content", "center");
    setFlex(tabId, "align_items", "center");

    createLabel(tabId + "-l", tabNames[ti], tabId);
    setFontSize(tabId + "-l", 11);
    if (ti === 0) setTextColor(tabId + "-l", APP_ACCENT);
    else setTextColor(tabId + "-l", APP_TEXT_DIM);

    if (ti === 0) {
        createCol(tabId + "-line", tabId);
        setFlex(tabId + "-line", "height", 2);
        setFlex(tabId + "-line", "flex_grow", 1);
        setBackground(tabId + "-line", APP_ACCENT);
    }
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
setFlex("showcase-row", "height", 80);

createXYPad("xy-demo", "showcase-row");
setFlex("xy-demo", "width", 80);
setFlex("xy-demo", "height", 80);
setLabel("xy-demo", "XY Pad");

// Spectrum analyzer
createSpectrum("spectrum-demo", "showcase-row");
setFlex("spectrum-demo", "flex_grow", 1);
setFlex("spectrum-demo", "height", 80);

// Generate some spectrum data
var specData = [];
for (var si = 0; si < 64; si++) {
    var freq = si / 64;
    specData.push(Math.exp(-freq * 3) * 0.8 + Math.random() * 0.1);
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
    wave2.push(Math.sin(2 * Math.PI * 5 * w2 / 512) * 0.5 *
               Math.exp(-w2 / 256) +
               Math.sin(2 * Math.PI * 13 * w2 / 512) * 0.2);
}
setWaveformData("waveform2", wave2);

// ── RIGHT PANEL (Inspector + Chat) ──────────────────────────────
createCol("right-panel", "main-area");
setFlex("right-panel", "width", 272);
setFlex("right-panel", "min_width", 250);  // Issue 8: prevent scrollbar overlap
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

// Model selector
createRow("model-row", "chat-area");
setFlex("model-row", "height", 24);
setFlex("model-row", "flex_shrink", 0);
setFlex("model-row", "align_items", "center");
setFlex("model-row", "justify_content", "space-between");

// #51: Context badge with accent styling
createCol("context-badge", "model-row");
setFlex("context-badge", "height", 20);
setFlex("context-badge", "padding_left", 8);
setFlex("context-badge", "padding_right", 8);
setFlex("context-badge", "justify_content", "center");
setFlex("context-badge", "align_items", "center");
setFlex("context-badge", "direction", "row");
setFlex("context-badge", "gap", 4);
setBackground("context-badge", '#2a2040');
setBorder("context-badge", APP_ACCENT, 1, 10);
createLabel("context-label", "Editing: All Components", "context-badge");
setFontSize("context-label", 8);
setTextColor("context-label", APP_ACCENT);
// Clear button (x) — visible when component selected
createLabel("context-clear", "x", "context-badge");
setFontSize("context-clear", 9);
setTextColor("context-clear", APP_TEXT_DIM);
setVisible("context-clear", false);
registerClick("context-clear");
registerClick("context-badge");

createCombo("model-selector", "model-row");
setItems("model-selector", ["Sonnet 4.6", "Opus 4.6"]);
setFlex("model-selector", "width", 100);
setFlex("model-selector", "height", 22);

// Chat messages (scrollable)
createScrollView("chat-messages", "chat-area");
setFlex("chat-messages", "flex_grow", 1);
setScrollContentSize("chat-messages", 236, 400);  // #61: narrower to clear scrollbar

createLabel("welcome-msg", "Describe a visual style and the preview will update live.", "chat-messages");
setFontSize("welcome-msg", 11);
setFlex("welcome-msg", "height", 30);

createLabel("hint-msg", 'Try: "warm vintage" or "neon cyberpunk"', "chat-messages");
setFontSize("hint-msg", 10);
setTextColor("hint-msg", APP_TEXT_DIM);
setFlex("hint-msg", "height", 16);

// Chat input area
createRow("chat-input-row", "chat-area");
setFlex("chat-input-row", "height", 32);
setFlex("chat-input-row", "flex_shrink", 0);
setFlex("chat-input-row", "gap", 6);

// Upload button with hover state (Issue 3)
// #49: Upload button with proper icon sizing
createCol("upload-btn", "chat-input-row");
setFlex("upload-btn", "width", 32);
setFlex("upload-btn", "height", 32);
setBackground("upload-btn", APP_PANEL);
setBorder("upload-btn", APP_BORDER, 1, 8);
setFlex("upload-btn", "justify_content", "center");
setFlex("upload-btn", "align_items", "center");
createIcon("upload-icon", "image_upload", "upload-btn");
setFlex("upload-icon", "width", 16);
setFlex("upload-icon", "height", 16);
registerHover("upload-btn");
on("upload-btn", "mouseenter", function() { setBorder("upload-btn", APP_ACCENT, 1, 6); });
on("upload-btn", "mouseleave", function() { setBorder("upload-btn", APP_BORDER, 1, 6); });
// Issue 2: image upload via file dialog
var uploadedImagePath = "";
registerClick("upload-btn");
on("upload-btn", "click", function() {
    var path = showOpenDialog("Select Reference Image", "Images", "png,jpg,jpeg,gif,webp");
    if (path && path.length > 0) {
        uploadedImagePath = path;
        showToast("Image: " + path.split("/").pop());
    }
});

createTextEditor("chat-input", "chat-input-row");
setPlaceholder("chat-input", "Describe a style...");
setFlex("chat-input", "flex_grow", 1);
setFlex("chat-input", "height", 28);

// #49: Send button with proper icon sizing
createCol("send-btn", "chat-input-row");
setFlex("send-btn", "width", 32);
setFlex("send-btn", "height", 32);
setBackground("send-btn", APP_ACCENT);
setBorder("send-btn", APP_ACCENT, 0, 8);
setFlex("send-btn", "justify_content", "center");
setFlex("send-btn", "align_items", "center");
createIcon("send-icon", "send", "send-btn");
setFlex("send-icon", "width", 16);
setFlex("send-icon", "height", 16);
// Issue 3: hover state for send button
registerHover("send-btn");
on("send-btn", "mouseenter", function() { setOpacity("send-btn", 0.8); });
on("send-btn", "mouseleave", function() { setOpacity("send-btn", 1.0); });

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

enableInspectClick();
on("__inspect__", "click", function(widgetId) {
    setText("insp-type-v", widgetId ? "View" : "—");
    setText("insp-id-v", widgetId || "—");
    setText("insp-bounds-v", "—");
    inspectedComponent = widgetId || null;
    // Update chat context label
    if (inspectedComponent) {
        setText("context-label", "Editing: " + inspectedComponent);
        setVisible("context-clear", true);
    }
    switchTab("inspector");
});

// Clear context — click x or the badge itself
on("context-clear", "click", function() {
    inspectedComponent = null;
    setText("context-label", "Editing: All Components");
    setVisible("context-clear", false);
});
on("context-badge", "click", function() {
    if (inspectedComponent) {
        inspectedComponent = null;
        setText("context-label", "Editing: All Components");
        setVisible("context-clear", false);
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

on("chat-input", "return", function(text) {
    if (!text || text.length === 0) return;
    addChatMessage("user", text);
    setText("chat-input", "");
    setText("status-text", "Generating...");
    // #58: Show typing indicator
    addChatMessage("assistant", "...");
    layout();

    var themeJson = getThemeJson();
    // D6: Use model selector value
    var modelIdx = 0;
    try { modelIdx = getValue("model-selector"); } catch(e) {}
    var model = modelIdx > 0.5 ? "claude-opus-4-6" : "claude-sonnet-4-6";
    // #59: Prompt supports BOTH colors AND styles
    var scope = inspectedComponent ? "\nScope: ONLY modify tokens related to '" + inspectedComponent + "'" : "";
    var prompt = "You are a design system expert for audio plugin UIs.\n";
    prompt += "Modify the theme to achieve the requested look. Be creative and bold.\n";
    prompt += "You can change colors AND style properties to create dramatic transformations.\n\n";
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
    prompt += "\n## RULES\n";
    prompt += "1. Output ONLY valid JSON. No markdown, no explanation.\n";
    prompt += '2. Format: {"colors": {"token.name": "#hex", ...}, "dimensions": {"prop": number, ...}}\n';
    prompt += "3. Change 5-30 tokens. Include BOTH colors and dimensions for dramatic effect.\n";
    prompt += "4. Use the exact token/property names listed above.\n";
    prompt += "5. Be bold — if asked for 'cyberpunk', make it look like cyberpunk. If 'warm analog', make it warm.\n";
    prompt += scope + "\n\n";
    if (uploadedImagePath && uploadedImagePath.length > 0) {
        prompt += "## Reference Image\nThe user uploaded a reference image at: " + uploadedImagePath + "\n";
        prompt += "Extract the visual mood, colors, and style from this image and apply them.\n\n";
    }
    prompt += '## Request\n"' + text + '"\n\n## JSON Output\n';

    var tmpFile = "/tmp/pulp-design-prompt.txt";
    exec("cat > " + tmpFile + " << 'PULPEOF'\n" + prompt + "\nPULPEOF");
    var response = exec("cat " + tmpFile + " | claude --print --model " + model + " 2>/dev/null");

    if (!response || response.length === 0) {
        addChatMessage("assistant", "No response from Claude");
        setText("status-text", "Error");
        layout();
        return;
    }

    var jsonStart = response.indexOf("{");
    var jsonEnd = response.lastIndexOf("}");
    if (jsonStart < 0 || jsonEnd < 0) {
        addChatMessage("assistant", "No JSON in response");
        setText("status-text", "Error");
        layout();
        return;
    }

    var jsonDiff = response.substring(jsonStart, jsonEnd + 1);
    var diffObj = {};
    try { diffObj = JSON.parse(jsonDiff); } catch(e) {}

    // Apply color changes via theme diff
    if (diffObj.colors) {
        applyTokenDiff(JSON.stringify({ colors: diffObj.colors }));
    }
    pushThemeSnapshot();

    // #59: Apply dimension/style changes directly to preview widgets
    var dimChanges = diffObj.dimensions || {};
    var dimCount = 0;
    for (var dk in dimChanges) {
        dimCount++;
        var dv = dimChanges[dk];
        if (dk === "cornerRadius") {
            setBorderRadius("btn-normal", dv); setBorderRadius("btn-hover", dv);
            setBorderRadius("btn-action", dv); setBorderRadius("btn-disabled", dv);
        } else if (dk === "headingSize") {
            setFontSize("heading-text", dv);
        } else if (dk === "bodySize") {
            setFontSize("body-text", dv);
        } else if (dk === "labelSize") {
            setFontSize("caption-text", dv);
        }
        // Store dimension in status for user visibility
    }

    // Change summary
    var diffColors = diffObj.colors || {};
    var changedNames = [];
    for (var ck in diffColors) changedNames.push(ck);
    var summary = "Applied " + changedNames.length + " colors";
    if (dimCount > 0) summary += " + " + dimCount + " styles";
    if (changedNames.length > 0 && changedNames.length <= 6) {
        summary += ": " + changedNames.join(", ");
    }
    addChatMessage("assistant", summary);
    updateTokenSwatches();
    updateModifiedCount();
    setText("status-text", (changedNames.length + dimCount) + " changes by AI");
    layout();
});

// Send button triggers same as return key
registerClick("send-btn");
on("send-btn", "click", function() {
    var text = getText("chat-input");
    if (text && text.length > 0) {
        // Trigger the return handler by dispatching manually
        __dispatch__("chat-input", "return", text);
    }
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
        setVisible("ramp-" + expandedPalette + "-editor", false);
        expandedPalette = -1;
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
