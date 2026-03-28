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

createLabel("undo-btn", "Undo", "toolbar");
setFontSize("undo-btn", 11);
setFlex("undo-btn", "width", 36);

createLabel("redo-btn", "Redo", "toolbar");
setFontSize("redo-btn", 11);
setFlex("redo-btn", "width", 36);

createLabel("import-btn", "Import", "toolbar");
setFontSize("import-btn", 11);
setFlex("import-btn", "width", 48);

createLabel("export-btn", "Export", "toolbar");
setFontSize("export-btn", 11);
setFlex("export-btn", "width", 48);
setTextColor("export-btn", APP_ACCENT);

// ═══════════════════════════════════════════════════════════════════
// MAIN AREA (3 columns: left 310px | center flex | right 272px)
// ═══════════════════════════════════════════════════════════════════
createRow("main-area");
setFlex("main-area", "flex_grow", 1);

// ── LEFT PANEL (Token Browser) ───────────────────────────────────
createCol("left-panel", "main-area");
setFlex("left-panel", "width", 310);
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

// Plugin chrome (rounded card with traffic lights)
createCol("plugin-chrome", "center-panel");
setFlex("plugin-chrome", "flex_grow", 1);
setBorder("plugin-chrome", APP_BORDER, 1, 12);
setBackground("plugin-chrome", APP_PANEL);
setFlex("plugin-chrome", "padding", 0);

// Chrome title bar
createRow("chrome-titlebar", "plugin-chrome");
setFlex("chrome-titlebar", "height", 32);
setFlex("chrome-titlebar", "padding_left", 12);
setFlex("chrome-titlebar", "padding_right", 12);
setFlex("chrome-titlebar", "align_items", "center");
setFlex("chrome-titlebar", "gap", 8);
setBackground("chrome-titlebar", "#1a1a22");
setBorder("chrome-titlebar", APP_BORDER, 0, 12);

// Traffic lights
createCol("tl-close", "chrome-titlebar");
setFlex("tl-close", "width", 12);
setFlex("tl-close", "height", 12);
setBackground("tl-close", "#ff5f57");
setBorder("tl-close", "#e04040", 0, 6);

createCol("tl-min", "chrome-titlebar");
setFlex("tl-min", "width", 12);
setFlex("tl-min", "height", 12);
setBackground("tl-min", "#ffbd2e");
setBorder("tl-min", "#d4a020", 0, 6);

createCol("tl-max", "chrome-titlebar");
setFlex("tl-max", "width", 12);
setFlex("tl-max", "height", 12);
setBackground("tl-max", "#28c840");
setBorder("tl-max", "#20a835", 0, 6);

createLabel("chrome-title", "Plugin Preview", "chrome-titlebar");
setFontSize("chrome-title", 11);
setTextColor("chrome-title", APP_TEXT_DIM);

// Preview content area
createCol("preview-area", "plugin-chrome");
setFlex("preview-area", "flex_grow", 1);
setFlex("preview-area", "padding", 16);
setFlex("preview-area", "gap", 16);

// Controls section: knobs
createLabel("controls-header", "Controls", "preview-area");
setFontSize("controls-header", 11);
setTextColor("controls-header", APP_TEXT_DIM);
setFlex("controls-header", "height", 16);

createRow("knob-row", "preview-area");
setFlex("knob-row", "gap", 20);
setFlex("knob-row", "height", 80);
setFlex("knob-row", "align_items", "center");

createKnob("k1", "knob-row");
setLabel("k1", "Gain");
setFlex("k1", "width", 64);
setFlex("k1", "height", 80);
setValue("k1", 0.4);

createKnob("k2", "knob-row");
setLabel("k2", "Freq");
setFlex("k2", "width", 64);
setFlex("k2", "height", 80);
setValue("k2", 0.6);

createKnob("k3", "knob-row");
setLabel("k3", "Res");
setFlex("k3", "width", 64);
setFlex("k3", "height", 80);
setValue("k3", 0.8);

createKnob("k4", "knob-row");
setLabel("k4", "Mix");
setFlex("k4", "width", 64);
setFlex("k4", "height", 80);
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
setFlex("btn-normal", "width", 72);
setFlex("btn-normal", "height", 28);
setBackground("btn-normal", "#3a3a4c");
setBorder("btn-normal", APP_BORDER, 1, 6);
createLabel("btn-normal-label", "Normal", "btn-normal");
setFontSize("btn-normal-label", 11);
setFlex("btn-normal-label", "padding", 6);

createCol("btn-hover", "btn-row");
setFlex("btn-hover", "width", 72);
setFlex("btn-hover", "height", 28);
setBackground("btn-hover", "#4a4a5c");
setBorder("btn-hover", APP_BORDER, 1, 6);
createLabel("btn-hover-label", "Hover", "btn-hover");
setFontSize("btn-hover-label", 11);
setFlex("btn-hover-label", "padding", 6);

createCol("btn-action", "btn-row");
setFlex("btn-action", "width", 72);
setFlex("btn-action", "height", 28);
setBackground("btn-action", APP_ACCENT);
setBorder("btn-action", APP_ACCENT, 0, 6);
createLabel("btn-action-label", "Action", "btn-action");
setFontSize("btn-action-label", 11);
setFlex("btn-action-label", "padding", 6);

createCol("btn-disabled", "btn-row");
setFlex("btn-disabled", "width", 72);
setFlex("btn-disabled", "height", 28);
setBackground("btn-disabled", "#2a2a36");
setBorder("btn-disabled", APP_BORDER, 1, 6);
setOpacity("btn-disabled", 0.5);
createLabel("btn-disabled-label", "Disabled", "btn-disabled");
setFontSize("btn-disabled-label", 11);
setFlex("btn-disabled-label", "padding", 6);
setTextColor("btn-disabled-label", APP_TEXT_DIM);

// Toggles + checkbox row
createRow("toggle-row", "preview-area");
setFlex("toggle-row", "gap", 12);
setFlex("toggle-row", "height", 28);
setFlex("toggle-row", "align_items", "center");

createToggle("t1", "toggle-row");
setFlex("t1", "width", 44);
setFlex("t1", "height", 24);
setValue("t1", 1);

createToggle("t2", "toggle-row");
setFlex("t2", "width", 44);
setFlex("t2", "height", 24);

createLabel("toggle-label", "Toggle", "toggle-row");
setFontSize("toggle-label", 11);
setFlex("toggle-label", "width", 50);

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
setFlex("waveform", "height", 80);

var waveData = [];
for (var i = 0; i < 512; i++) {
    waveData.push(Math.sin(2 * Math.PI * 3 * i / 512) * 0.7 +
                  Math.sin(2 * Math.PI * 7 * i / 512) * 0.3);
}
setWaveformData("waveform", waveData);

// Meters
createRow("meter-row", "preview-area");
setFlex("meter-row", "gap", 4);
setFlex("meter-row", "height", 60);

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

createLabel("tab-inspector", "Inspector", "right-tabs");
setFontSize("tab-inspector", 12);
setFlex("tab-inspector", "flex_grow", 1);
setFlex("tab-inspector", "padding", 10);

createLabel("tab-chat", "Chat", "right-tabs");
setFontSize("tab-chat", 12);
setFlex("tab-chat", "flex_grow", 1);
setFlex("tab-chat", "padding", 10);
setTextColor("tab-chat", APP_ACCENT);

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
        setTextColor("tab-inspector", APP_TEXT);
    } else {
        setVisible("chat-area", false);
        setVisible("inspector-area", true);
        setTextColor("tab-inspector", APP_ACCENT);
        setTextColor("tab-chat", APP_TEXT);
    }
    layout();
}

// Tab switching via registerClick + on() click events
registerClick("tab-inspector");
registerClick("tab-chat");
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

createTextEditor("chat-input", "chat-input-row");
setPlaceholder("chat-input", "Describe a style...");
setFlex("chat-input", "flex_grow", 1);
setFlex("chat-input", "height", 28);

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
// Chat logic
// ═══════════════════════════════════════════════════════════════════

function addChatMessage(role, text) {
    var id = "msg-" + (msgCount++);
    // Estimate height: role(12) + gap(4) + text lines(~14 per line) + padding(16)
    var lineCount = Math.ceil(text.length / 30);
    var msgHeight = 12 + 4 + lineCount * 14 + 16;

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

    createLabel(id + "-role", role === "user" ? "You" : "Designer", id);
    setFontSize(id + "-role", 9);
    setTextColor(id + "-role", APP_TEXT_DIM);
    setFlex(id + "-role", "height", 12);

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
    setTheme(p.theme);
    setText("theme-name-label", ["Default Dark","Light","Pro Audio","Violet","Amber","Ocean","Neon"][idx]);
    layout();
});

// ═══════════════════════════════════════════════════════════════════
// Export/Import buttons
// ═══════════════════════════════════════════════════════════════════
registerClick("export-btn");
on("export-btn", "click", function() {
    var json = getThemeJson();
    var path = "/tmp/pulp-theme-export.json";
    exec("cat > " + path + " << 'PULPEOF'\n" + json + "\nPULPEOF");
    setText("status-text", "Exported to " + path);
    layout();
});

registerClick("import-btn");
on("import-btn", "click", function() {
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

registerClick("undo-btn");
on("undo-btn", "click", function() {
    if (historyIndex > 0) {
        historyIndex--;
        applyTokenDiff(themeHistory[historyIndex]);
        updateTokenSwatches();
        setText("status-text", "Undo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
        layout();
    }
});

registerClick("redo-btn");
on("redo-btn", "click", function() {
    if (historyIndex < themeHistory.length - 1) {
        historyIndex++;
        applyTokenDiff(themeHistory[historyIndex]);
        updateTokenSwatches();
        setText("status-text", "Redo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
        layout();
    }
});

layout();
