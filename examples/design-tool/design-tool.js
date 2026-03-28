// Pulp Style Designer — JS-defined UI matching ai-style-designer layout
// Reference: ~/Code/ai-style-designer/Tools/theme-designer.html
// Hot-reloadable: edit and save to see changes instantly.

setTheme("dark");

// ═══════════════════════════════════════════════════════════════════
// Color/palette/app state
// ═══════════════════════════════════════════════════════════════════
var currentAccent = '#89B4FA';
var currentHarmony = 'monochromatic';
var msgCount = 0;

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

// ── LEFT PANEL (Token Browser) ───────────────────────────────────
createCol("left-panel", "main-area");
setFlex("left-panel", "width", 310);
setFlex("left-panel", "min_width", 260);
setFlex("left-panel", "flex_shrink", 0);
setFlex("left-panel", "padding", 0);
setBackground("left-panel", APP_SURFACE);
setBorder("left-panel", APP_BORDER, 1, 0);

// Color System section (height: title 14 + combo 26 + hue 24 + 5 ramps*38 + gaps + padding)
createCol("color-section", "left-panel");
setFlex("color-section", "height", 310);
setFlex("color-section", "flex_shrink", 0);
setFlex("color-section", "padding", 10);
setFlex("color-section", "gap", 6);

createLabel("cs-title", "COLOR SYSTEM", "color-section");
setFontSize("cs-title", 10);
setTextColor("cs-title", APP_TEXT_DIM);
setFlex("cs-title", "height", 14);

createCombo("harmony-selector", "color-section");
setItems("harmony-selector", ["Monochromatic", "Analogous", "Complementary", "Split Comp.", "None"]);
setFlex("harmony-selector", "height", 26);

createRow("hue-row", "color-section");
setFlex("hue-row", "gap", 8);
setFlex("hue-row", "align_items", "center");
setFlex("hue-row", "height", 24);

createLabel("hue-label", "Hue", "hue-row");
setFontSize("hue-label", 10);
setFlex("hue-label", "width", 30);

createFader("accent-hue", "horizontal", "hue-row");
setFlex("accent-hue", "flex_grow", 1);
setFlex("accent-hue", "height", 20);
setValue("accent-hue", 0.65);

// Token browser header
createRow("token-header", "left-panel");
setFlex("token-header", "height", 30);
setFlex("token-header", "padding", 10);
setFlex("token-header", "padding_bottom", 6);
setFlex("token-header", "align_items", "center");

createLabel("tokens-title", "TOKENS", "token-header");
setFontSize("tokens-title", 10);
setTextColor("tokens-title", APP_TEXT_DIM);

// Token list (scrollable)
createScrollView("token-list", "left-panel");
setFlex("token-list", "flex_grow", 1);
setScrollContentSize("token-list", 310, 800);

// Token groups
var tokenGroups = [
    { name: "Background", tokens: ["bg.primary", "bg.secondary", "bg.surface", "bg.elevated"] },
    { name: "Text", tokens: ["text.primary", "text.secondary", "text.disabled"] },
    { name: "Accent", tokens: ["accent.primary", "accent.secondary", "accent.success", "accent.warning", "accent.error"] },
    { name: "Controls", tokens: ["control.track", "control.fill", "control.thumb", "control.border"] }
];

for (var g = 0; g < tokenGroups.length; g++) {
    var group = tokenGroups[g];
    var gid = "tg-" + g;
    var groupHeight = 4 + 14 + 2 + group.tokens.length * 26; // padding_top + title + gap + tokens*(height+gap)
    createCol(gid, "token-list");
    setFlex(gid, "height", groupHeight);
    setFlex(gid, "padding_left", 10);
    setFlex(gid, "padding_right", 10);
    setFlex(gid, "padding_top", 4);
    setFlex(gid, "gap", 2);

    createLabel(gid + "-title", group.name, gid);
    setFontSize(gid + "-title", 10);
    setTextColor(gid + "-title", APP_TEXT_DIM);
    setFlex(gid + "-title", "height", 14);

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
    }
}

// ── Apply token colors to swatches ───────────────────────────────
function updateTokenSwatches() {
    var themeStr = getThemeJson();
    var theme = JSON.parse(themeStr);
    var colors = theme.colors || {};
    for (var g = 0; g < tokenGroups.length; g++) {
        var group = tokenGroups[g];
        for (var t = 0; t < group.tokens.length; t++) {
            var swatchId = "tok-" + g + "-" + t + "-sw";
            var tokenName = group.tokens[t];
            if (colors[tokenName]) {
                setBackground(swatchId, colors[tokenName]);
            }
        }
    }
}
updateTokenSwatches();

// ── Color System: OKLCH Shade Ramps ──────────────────────────────
var paletteNames = ["Accent", "Neutral", "Success", "Warning", "Error"];
var paletteKeys  = ["accent", "neutral", "success", "warning", "error"];

function buildShadeRamps() {
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    var steps = ShadeGenerator.STEPS;

    for (var p = 0; p < paletteNames.length; p++) {
        var rampId = "ramp-" + p;
        // Remove old ramp if it exists (on rebuild)
        removeWidget(rampId);

        createCol(rampId, "color-section");
        setFlex(rampId, "height", 36);
        setFlex(rampId, "gap", 2);

        createLabel(rampId + "-title", paletteNames[p], rampId);
        setFontSize(rampId + "-title", 9);
        setTextColor(rampId + "-title", APP_TEXT_DIM);
        setFlex(rampId + "-title", "height", 12);

        createRow(rampId + "-row", rampId);
        setFlex(rampId + "-row", "gap", 2);
        setFlex(rampId + "-row", "height", 20);

        var ramp = palette[paletteKeys[p]];
        for (var s = 0; s < steps.length; s++) {
            var shadeId = rampId + "-s" + s;
            createCol(shadeId, rampId + "-row");
            setFlex(shadeId, "flex_grow", 1);
            setFlex(shadeId, "height", 18);
            setBackground(shadeId, ramp[steps[s]].hex);
            setBorder(shadeId, APP_BORDER, 0, 2);
            // Click to show color info
            registerClick(shadeId);
            (function(hex, name, step) {
                on(shadeId, "click", function() {
                    setText("status-text", name + " " + step + ": " + hex);
                    layout();
                });
            })(ramp[steps[s]].hex, paletteNames[p], steps[s]);
        }
    }
}
buildShadeRamps();

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
on("harmony-selector", "select", function(idx) {
    var modes = ["monochromatic", "analogous", "complementary", "splitComplementary", "none"];
    currentHarmony = modes[idx];
    buildShadeRamps();

    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    var diff = PaletteSystem.toThemeDiff(palette);
    applyTokenDiff(diff);
    updateTokenSwatches();
    layout();
});

// ── CENTER PANEL (Preview) ───────────────────────────────────────
createCol("center-panel", "main-area");
setFlex("center-panel", "flex_grow", 1);
setFlex("center-panel", "padding", 20);
setFlex("center-panel", "gap", 12);
setBackground("center-panel", APP_BG);

// Preview content area (flush to top, no fake title bar)
createCol("preview-area", "center-panel");
setFlex("preview-area", "flex_grow", 1);
setFlex("preview-area", "padding", 12);
setFlex("preview-area", "gap", 10);
setBackground("preview-area", APP_PANEL);
setBorder("preview-area", APP_BORDER, 1, 4);

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

createCol("card-1", "card-grid-top");
setFlex("card-1", "flex_grow", 1);
setBackground("card-1", APP_PANEL);
setBorder("card-1", APP_BORDER, 1, 8);
setFlex("card-1", "padding", 8);
createLabel("card-1-label", "Panel A", "card-1");
setFontSize("card-1-label", 10);
setFlex("card-1-label", "height", 14);

createCol("card-2", "card-grid-top");
setFlex("card-2", "flex_grow", 1);
setBackground("card-2", APP_PANEL);
setBorder("card-2", APP_BORDER, 1, 8);
setFlex("card-2", "padding", 8);
createLabel("card-2-label", "Panel B", "card-2");
setFontSize("card-2-label", 10);
setFlex("card-2-label", "height", 14);

createRow("card-grid-bot", "preview-area");
setFlex("card-grid-bot", "gap", 8);
setFlex("card-grid-bot", "height", 56);

createCol("card-3", "card-grid-bot");
setFlex("card-3", "flex_grow", 1);
setBackground("card-3", APP_ACCENT);
setBorder("card-3", APP_ACCENT, 0, 8);
setFlex("card-3", "padding", 8);
createLabel("card-3-label", "Accent", "card-3");
setFontSize("card-3-label", 10);
setFlex("card-3-label", "height", 14);

createCol("card-4", "card-grid-bot");
setFlex("card-4", "flex_grow", 1);
setBackground("card-4", "#8b3a3a");
setBorder("card-4", "#8b3a3a", 0, 8);
setFlex("card-4", "padding", 8);
createLabel("card-4-label", "Error", "card-4");
setFontSize("card-4-label", 10);
setFlex("card-4-label", "height", 14);

// ── RIGHT PANEL (Inspector + Chat) ──────────────────────────────
createCol("right-panel", "main-area");
setFlex("right-panel", "width", 272);
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
setFlex("tab-inspector", "padding", 8);
setTextColor("tab-inspector", APP_TEXT_DIM);

// Full-width underline indicator (matches original HTML styling)
createCol("tab-inspector-line", "tab-inspector-col");
setFlex("tab-inspector-line", "height", 2);
setFlex("tab-inspector-line", "flex_grow", 1);

createCol("tab-chat-col", "right-tabs");
setFlex("tab-chat-col", "flex_grow", 1);
setFlex("tab-chat-col", "height", 36);
setFlex("tab-chat-col", "align_items", "center");
setFlex("tab-chat-col", "justify_content", "center");
setFlex("tab-chat-col", "gap", 0);

createLabel("tab-chat", "CHAT", "tab-chat-col");
setFontSize("tab-chat", 11);
setFlex("tab-chat", "height", 28);
setFlex("tab-chat", "padding", 8);
setTextColor("tab-chat", APP_ACCENT);

createCol("tab-chat-line", "tab-chat-col");
setFlex("tab-chat-line", "height", 2);
setFlex("tab-chat-line", "flex_grow", 1);
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
        setBackground("tab-inspector-line", "transparent");
    } else {
        setVisible("chat-area", false);
        setVisible("inspector-area", true);
        setTextColor("tab-inspector", APP_ACCENT);
        setBackground("tab-inspector-line", APP_ACCENT);
        setTextColor("tab-chat", APP_TEXT_DIM);
        setBackground("tab-chat-line", "transparent");
    }
    layout();
}

// Tab switching via registerClick + on() click events
registerClick("tab-inspector-col");
registerClick("tab-chat-col");
on("tab-inspector-col", "click", function() { switchTab("inspector"); });
on("tab-chat-col", "click", function() { switchTab("chat"); });

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

createLabel("context-label", "Editing: All Components", "model-row");
setFontSize("context-label", 9);
setTextColor("context-label", APP_TEXT_DIM);

createCombo("model-selector", "model-row");
setItems("model-selector", ["Sonnet 4.6", "Opus 4.6"]);
setFlex("model-selector", "width", 100);
setFlex("model-selector", "height", 22);

// Chat messages (scrollable)
createScrollView("chat-messages", "chat-area");
setFlex("chat-messages", "flex_grow", 1);
setScrollContentSize("chat-messages", 252, 400);

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

// Upload button (camera icon placeholder)
createCol("upload-btn", "chat-input-row");
setFlex("upload-btn", "width", 28);
setFlex("upload-btn", "height", 28);
setBackground("upload-btn", APP_PANEL);
setBorder("upload-btn", APP_BORDER, 1, 6);
setFlex("upload-btn", "justify_content", "center");
setFlex("upload-btn", "align_items", "center");
createLabel("upload-icon", "\u25A3", "upload-btn");
setFontSize("upload-icon", 14);
setTextColor("upload-icon", APP_TEXT_DIM);

createTextEditor("chat-input", "chat-input-row");
setPlaceholder("chat-input", "Describe a style...");
setFlex("chat-input", "flex_grow", 1);
setFlex("chat-input", "height", 28);

// Send button (arrow icon placeholder)
createCol("send-btn", "chat-input-row");
setFlex("send-btn", "width", 28);
setFlex("send-btn", "height", 28);
setBackground("send-btn", APP_ACCENT);
setBorder("send-btn", APP_ACCENT, 0, 6);
setFlex("send-btn", "justify_content", "center");
setFlex("send-btn", "align_items", "center");
createLabel("send-icon", "\u25B6", "send-btn");
setFontSize("send-icon", 12);
setFlex("send-icon", "height", 14);

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
setFontSize("status-schema", 10);
setTextColor("status-schema", APP_TEXT_DIM);

// ═══════════════════════════════════════════════════════════════════
// Inspector: Cmd+click detection
// ═══════════════════════════════════════════════════════════════════
enableInspectClick();
on("__inspect__", "click", function(widgetId) {
    // Populate inspector with widget info
    setText("insp-type-v", widgetId ? "View" : "—");
    setText("insp-id-v", widgetId || "—");
    setText("insp-bounds-v", "—");  // bounds not accessible from JS yet
    switchTab("inspector");
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

function addChatMessage(role, text) {
    var id = "msg-" + (msgCount++);
    // Capture theme snapshot for this message
    var snapshot = getThemeJson();

    // Estimate height: role(12) + gap(4) + text(~14/line) + restore btn(16) + padding(16)
    var lineCount = Math.ceil(text.length / 30);
    var hasRestore = (role === "assistant");
    var msgHeight = 12 + 4 + lineCount * 14 + (hasRestore ? 20 : 0) + 16;

    createCol(id, "chat-messages");
    setFlex(id, "height", msgHeight);
    setFlex(id, "padding", 8);
    setFlex(id, "gap", 4);
    setBorder(id, APP_BORDER, 1, 6);
    if (role === "user") {
        setBackground(id, "#2a2a3c");
    } else {
        setBackground(id, APP_PANEL);
    }

    // Role label row with optional restore button
    createRow(id + "-header", id);
    setFlex(id + "-header", "height", 12);
    setFlex(id + "-header", "align_items", "center");
    setFlex(id + "-header", "justify_content", "space-between");

    createLabel(id + "-role", role === "user" ? "You" : "Designer", id + "-header");
    setFontSize(id + "-role", 9);
    setTextColor(id + "-role", APP_TEXT_DIM);
    setFlex(id + "-role", "width", 60);

    if (hasRestore) {
        var restoreId = id + "-restore";
        createLabel(restoreId, "Restore", id + "-header");
        setFontSize(restoreId, 8);
        setTextColor(restoreId, APP_ACCENT);
        setFlex(restoreId, "width", 44);
        registerClick(restoreId);
        // Capture snapshot in closure
        (function(snap, rid) {
            on(rid, "click", function() {
                applyTokenDiff(snap);
                updateTokenSwatches();
                buildShadeRamps();
                setText("status-text", "Restored snapshot");
                layout();
            });
        })(snapshot, restoreId);
    }

    createLabel(id + "-text", text, id);
    setFontSize(id + "-text", 11);
    setFlex(id + "-text", "height", lineCount * 14);

    // Update scroll content size to fit all messages
    var totalHeight = (msgCount + 1) * (msgHeight + 8) + 60;
    setScrollContentSize("chat-messages", 252, totalHeight);
    layout();
}

on("chat-input", "return", function(text) {
    if (!text || text.length === 0) return;
    addChatMessage("user", text);
    setText("chat-input", "");
    setText("status-text", "Generating...");
    layout();

    var themeJson = getThemeJson();
    var model = "claude-sonnet-4-6";
    var prompt = "You are a design token expert for audio plugin UIs.\n";
    prompt += "Modify design tokens to achieve the requested style.\n\n";
    prompt += "## Current Theme\n" + themeJson + "\n\n";
    prompt += "## RULES\n1. Output ONLY JSON. No markdown.\n";
    prompt += "2. Include ONLY tokens that CHANGE (5-12 colors typically).\n";
    prompt += "3. Do NOT change dimensions unless asked.\n\n";
    prompt += '## Request\n"' + text + '"\n\n## Output\n';

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
    applyTokenDiff(jsonDiff);
    pushThemeSnapshot();

    var count = (jsonDiff.match(/#[0-9a-fA-F]{6}/g) || []).length;
    addChatMessage("assistant", "Applied " + count + " token changes");
    setText("status-text", count + " tokens modified");
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
    setTheme(p.theme);
    setText("theme-name-label", ["Default Dark","Light","Pro Audio","Violet","Amber","Ocean","Neon"][idx]);
    buildShadeRamps();
    updateTokenSwatches();
    pushThemeSnapshot();
    layout();
});

// ═══════════════════════════════════════════════════════════════════
// Export/Import buttons
// ═══════════════════════════════════════════════════════════════════
registerClick("export-btn-pill");
on("export-btn-pill", "click", function() {
    var json = getThemeJson();
    var path = "/tmp/pulp-theme-export.json";
    exec("cat > " + path + " << 'PULPEOF'\n" + json + "\nPULPEOF");
    setText("status-text", "Exported to " + path);
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
