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
setFlex("toolbar", "padding_left", 12);
setFlex("toolbar", "padding_right", 12);
setFlex("toolbar", "align_items", "center");
setFlex("toolbar", "justify_content", "space-between");
setBackground("toolbar", APP_SURFACE);
setBorder("toolbar", APP_BORDER, 1, 0);

// Left toolbar group: theme name + preset buttons
createRow("toolbar-left", "toolbar");
setFlex("toolbar-left", "gap", 8);
setFlex("toolbar-left", "align_items", "center");

createLabel("theme-name-label", "Default Dark", "toolbar-left");
setFontSize("theme-name-label", 13);

createCombo("preset-selector", "toolbar-left");
setItems("preset-selector", ["Default Dark", "Light", "Pro Audio", "Violet", "Amber", "Ocean", "Neon"]);
setFlex("preset-selector", "width", 120);
setFlex("preset-selector", "height", 26);

// Right toolbar group: undo/redo + import/export
createRow("toolbar-right", "toolbar");
setFlex("toolbar-right", "gap", 6);
setFlex("toolbar-right", "align_items", "center");

createLabel("undo-btn", "Undo", "toolbar-right");
setFontSize("undo-btn", 11);

createLabel("redo-btn", "Redo", "toolbar-right");
setFontSize("redo-btn", 11);

createLabel("import-btn", "Import", "toolbar-right");
setFontSize("import-btn", 11);

createLabel("export-btn", "Export", "toolbar-right");
setFontSize("export-btn", 11);
setTextColor("export-btn", APP_ACCENT);

// ═══════════════════════════════════════════════════════════════════
// MAIN AREA (3 columns: left 310px | center flex | right 272px)
// ═══════════════════════════════════════════════════════════════════
createRow("main-area");
setFlex("main-area", "flex_grow", 1);

// ── LEFT PANEL (Token Browser) ───────────────────────────────────
createCol("left-panel", "main-area");
setFlex("left-panel", "width", 310);
setFlex("left-panel", "padding", 0);
setBackground("left-panel", APP_SURFACE);
setBorder("left-panel", APP_BORDER, 1, 0);

// Color System section
createCol("color-section", "left-panel");
setFlex("color-section", "padding", 10);
setFlex("color-section", "gap", 6);

createLabel("cs-title", "COLOR SYSTEM", "color-section");
setFontSize("cs-title", 10);
setTextColor("cs-title", APP_TEXT_DIM);

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
setFlex("token-header", "padding", 10);
setFlex("token-header", "padding_bottom", 6);

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
    createCol(gid, "token-list");
    setFlex(gid, "padding_left", 10);
    setFlex(gid, "padding_right", 10);
    setFlex(gid, "padding_top", 4);
    setFlex(gid, "gap", 2);

    createLabel(gid + "-title", group.name, gid);
    setFontSize(gid + "-title", 10);
    setTextColor(gid + "-title", APP_TEXT_DIM);

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
        // TODO: set swatch background to actual token color

        // Token name
        createLabel(tid + "-name", group.tokens[t], tid);
        setFontSize(tid + "-name", 11);
        setFlex(tid + "-name", "flex_grow", 1);
    }
}

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

// Buttons row
createRow("btn-row", "preview-area");
setFlex("btn-row", "gap", 8);
setFlex("btn-row", "height", 32);
setFlex("btn-row", "align_items", "center");

createCol("btn-normal", "btn-row");
setFlex("btn-normal", "width", 80);
setFlex("btn-normal", "height", 28);
setBackground("btn-normal", "#3a3a4c");
setBorder("btn-normal", APP_BORDER, 1, 6);

createLabel("btn-normal-label", "Normal", "btn-normal");
setFontSize("btn-normal-label", 11);
setFlex("btn-normal-label", "padding", 6);

createCol("btn-action", "btn-row");
setFlex("btn-action", "width", 80);
setFlex("btn-action", "height", 28);
setBackground("btn-action", APP_ACCENT);
setBorder("btn-action", APP_ACCENT, 0, 6);

createLabel("btn-action-label", "Action", "btn-action");
setFontSize("btn-action-label", 11);
setFlex("btn-action-label", "padding", 6);

// Toggles row
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

// Data display: Waveform
createLabel("data-header", "Data Display", "preview-area");
setFontSize("data-header", 11);
setTextColor("data-header", APP_TEXT_DIM);

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

// ── RIGHT PANEL (Inspector + Chat) ──────────────────────────────
createCol("right-panel", "main-area");
setFlex("right-panel", "width", 272);
setBackground("right-panel", APP_SURFACE);
setBorder("right-panel", APP_BORDER, 1, 0);

// Tab bar
createRow("right-tabs", "right-panel");
setFlex("right-tabs", "height", 36);
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

// Chat content area
createCol("chat-area", "right-panel");
setFlex("chat-area", "flex_grow", 1);
setFlex("chat-area", "padding", 10);
setFlex("chat-area", "gap", 8);

// Model selector
createRow("model-row", "chat-area");
setFlex("model-row", "height", 24);
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

createLabel("hint-msg", 'Try: "warm vintage" or "neon cyberpunk"', "chat-messages");
setFontSize("hint-msg", 10);
setTextColor("hint-msg", APP_TEXT_DIM);

// Chat input area
createRow("chat-input-row", "chat-area");
setFlex("chat-input-row", "height", 32);
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
    createCol(id, "chat-messages");
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

    createLabel(id + "-text", text, id);
    setFontSize(id + "-text", 11);

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

layout();
