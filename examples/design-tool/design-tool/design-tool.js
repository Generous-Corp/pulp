// Pulp Style Designer — JS-defined UI
// Hot-reloadable: edit and save to see changes instantly.
// Requires oklch.js loaded first (color math library).

setTheme("dark");

// ═══════════════════════════════════════════════════════════════════
// State
// ═══════════════════════════════════════════════════════════════════

var currentAccent = '#89B4FA';  // Default Pulp accent
var currentHarmony = 'monochromatic';
var currentPalette = null;
var msgCount = 0;
var generating = false;

// Undo/redo history
var themeHistory = [];
var historyIndex = -1;
var maxHistory = 50;

// Per-message snapshots: [{role, text, themeJson}]
var chatSnapshots = [];

function pushThemeState() {
    var json = getThemeJson();
    // Trim future states if we undid something
    if (historyIndex < themeHistory.length - 1)
        themeHistory = themeHistory.slice(0, historyIndex + 1);
    themeHistory.push(json);
    if (themeHistory.length > maxHistory)
        themeHistory.shift();
    historyIndex = themeHistory.length - 1;
    updateUndoRedoLabels();
}

function undo() {
    if (historyIndex <= 0) return;
    historyIndex--;
    applyTokenDiff(themeHistory[historyIndex]);
    updateUndoRedoLabels();
    layout();
}

function redo() {
    if (historyIndex >= themeHistory.length - 1) return;
    historyIndex++;
    applyTokenDiff(themeHistory[historyIndex]);
    updateUndoRedoLabels();
    layout();
}

function updateUndoRedoLabels() {
    setText("undo-btn", historyIndex > 0 ? "Undo (" + historyIndex + ")" : "Undo");
    setText("redo-btn", historyIndex < themeHistory.length - 1 ?
        "Redo (" + (themeHistory.length - 1 - historyIndex) + ")" : "Redo");
}

// Export theme in different formats
function exportAsJson() {
    return getThemeJson();
}

function exportAsCss() {
    var json = getThemeJson();
    var theme = JSON.parse(json);
    var css = ":root {\n";
    if (theme.colors) {
        var keys = Object.keys(theme.colors);
        for (var i = 0; i < keys.length; i++) {
            var varName = keys[i].replace(/\./g, '-');
            css += "  --pulp-" + varName + ": " + theme.colors[keys[i]] + ";\n";
        }
    }
    if (theme.dimensions) {
        var dkeys = Object.keys(theme.dimensions);
        for (var j = 0; j < dkeys.length; j++) {
            var dvar = dkeys[j].replace(/\./g, '-');
            css += "  --pulp-" + dvar + ": " + theme.dimensions[dkeys[j]] + "px;\n";
        }
    }
    css += "}\n";
    return css;
}

function exportAsCppHeader() {
    var json = getThemeJson();
    var theme = JSON.parse(json);
    var h = "// Auto-generated Pulp theme\n";
    h += "#pragma once\n\n";
    h += "#include <pulp/view/theme.hpp>\n\n";
    h += "namespace pulp::themes {\n\n";
    h += "inline Theme custom_theme() {\n";
    h += "    Theme t = Theme::dark();\n";
    if (theme.colors) {
        var keys = Object.keys(theme.colors);
        for (var i = 0; i < keys.length; i++) {
            h += '    t.set_color("' + keys[i] + '", "' + theme.colors[keys[i]] + '");\n';
        }
    }
    h += "    return t;\n";
    h += "}\n\n";
    h += "} // namespace pulp::themes\n";
    return h;
}

function saveToFile(content, filename) {
    // Escape for shell
    var escaped = content.replace(/'/g, "'\\''");
    exec("cat > " + filename + " << 'PULPEXPORTEOF'\n" + content + "\nPULPEXPORTEOF");
    return filename;
}

function importThemeFromFile() {
    var path = exec("osascript -e 'POSIX path of (choose file of type {\"json\"} with prompt \"Import Pulp Theme\")' 2>/dev/null").trim();
    if (!path || path.length === 0) return false;
    var content = exec("cat '" + path + "' 2>/dev/null").trim();
    if (!content || content.length === 0) return false;
    var jsonStart = content.indexOf("{");
    var jsonEnd = content.lastIndexOf("}");
    if (jsonStart < 0) return false;
    var jsonStr = content.substring(jsonStart, jsonEnd + 1);
    pushThemeState();
    applyTokenDiff(jsonStr);
    pushThemeState();
    return true;
}

// Generate initial palette
currentPalette = PaletteSystem.create(currentAccent, currentHarmony);

// ═══════════════════════════════════════════════════════════════════
// Root layout: three columns
// ═══════════════════════════════════════════════════════════════════

setFlex("", "direction", "row");
setFlex("", "gap", 0);

// ═══════════════════════════════════════════════════════════════════
// LEFT PANEL — Color System + Widgets
// ═══════════════════════════════════════════════════════════════════

createCol("left-panel");
setFlex("left-panel", "width", 220);
setFlex("left-panel", "padding", 10);
setFlex("left-panel", "gap", 6);

// ── Color System Header ──────────────────────────────────────────
createLabel("color-title", "Color System", "left-panel");
setFontSize("color-title", 13);

// Harmony mode selector
createCombo("harmony-selector", "left-panel");
setItems("harmony-selector", ["Monochromatic", "Analogous", "Complementary", "Split Comp.", "None"]);
setFlex("harmony-selector", "height", 24);

on("harmony-selector", "select", function(idx) {
    var modes = ["monochromatic", "analogous", "complementary", "splitComplementary", "none"];
    currentHarmony = modes[idx];
    currentPalette = PaletteSystem.create(currentAccent, currentHarmony);
    var diff = PaletteSystem.toThemeDiff(currentPalette);
    pushThemeState();
    applyTokenDiff(diff);
    pushThemeState();
    layout();
});

// ── Accent Color ─────────────────────────────────────────────────
createLabel("accent-label", "Accent: " + currentAccent, "left-panel");
setFontSize("accent-label", 10);

// Accent hue fader (0-360 mapped to 0-1)
createFader("accent-hue", "horizontal", "left-panel");
setFlex("accent-hue", "height", 20);
setLabel("accent-hue", "Hue");
var accentOklch = OklchEngine.hexToOklch(currentAccent);
setValue("accent-hue", accentOklch.H / 360);

on("accent-hue", "change", function(v) {
    var hue = v * 360;
    var oklch = OklchEngine.hexToOklch(currentAccent);
    var mapped = OklchEngine.gamutMap(oklch.L, oklch.C, hue);
    currentAccent = OklchEngine.oklchToHex(mapped.L, mapped.C, mapped.H);
    setText("accent-label", "Accent: " + currentAccent);
    currentPalette = PaletteSystem.create(currentAccent, currentHarmony);
    var diff = PaletteSystem.toThemeDiff(currentPalette);
    applyTokenDiff(diff);
    layout();
});

// Apply palette button
createLabel("apply-label", "Drag hue slider to change accent", "left-panel");
setFontSize("apply-label", 9);

// ── Theme Presets ────────────────────────────────────────────────
createLabel("preset-label", "Presets", "left-panel");
setFontSize("preset-label", 10);

createCombo("preset-selector", "left-panel");
setItems("preset-selector", ["Dark (default)", "Light", "Pro Audio", "Violet", "Amber", "Ocean", "Neon"]);
setFlex("preset-selector", "height", 24);

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
    pushThemeState();
    setTheme(p.theme);
    currentAccent = p.accent;
    currentPalette = PaletteSystem.create(currentAccent, currentHarmony);
    var diff = PaletteSystem.toThemeDiff(currentPalette);
    applyTokenDiff(diff);
    pushThemeState();
    var oklch = OklchEngine.hexToOklch(currentAccent);
    setValue("accent-hue", oklch.H / 360);
    setText("accent-label", "Accent: " + currentAccent);
    layout();
});

// ── Widget Showcase ──────────────────────────────────────────────
createLabel("widget-label", "Widgets", "left-panel");
setFontSize("widget-label", 13);

// Knobs
createRow("knob-row", "left-panel");
setFlex("knob-row", "gap", 6);
setFlex("knob-row", "height", 68);
setFlex("knob-row", "align_items", "center");

createKnob("k-arc", "knob-row");
setLabel("k-arc", "Arc");
setStyle("k-arc", "arc");
setFlex("k-arc", "width", 54);
setFlex("k-arc", "height", 68);
setValue("k-arc", 0.5);

createKnob("k-filled", "knob-row");
setLabel("k-filled", "Filled");
setStyle("k-filled", "filled");
setFlex("k-filled", "width", 54);
setFlex("k-filled", "height", 68);
setValue("k-filled", 0.7);

createKnob("k-notched", "knob-row");
setLabel("k-notched", "Notch");
setStyle("k-notched", "notched");
setFlex("k-notched", "width", 54);
setFlex("k-notched", "height", 68);
setValue("k-notched", 0.85);

// Toggles
createRow("toggle-row", "left-panel");
setFlex("toggle-row", "gap", 6);
setFlex("toggle-row", "height", 32);
setFlex("toggle-row", "align_items", "center");

createToggle("t-pill", "toggle-row");
setLabel("t-pill", "");
setStyle("t-pill", "pill");
setFlex("t-pill", "width", 44);
setFlex("t-pill", "height", 24);
setValue("t-pill", 1);

createToggle("t-check", "toggle-row");
setLabel("t-check", "");
setStyle("t-check", "checkbox");
setFlex("t-check", "width", 24);
setFlex("t-check", "height", 24);
setValue("t-check", 1);

createToggle("t-rocker", "toggle-row");
setLabel("t-rocker", "");
setStyle("t-rocker", "rocker");
setFlex("t-rocker", "width", 44);
setFlex("t-rocker", "height", 24);

// Combo + progress
createCombo("filter-combo", "left-panel");
setItems("filter-combo", ["Lowpass", "Highpass", "Bandpass", "Notch"]);
setFlex("filter-combo", "height", 24);

createProgress("load-progress", "left-panel");
setFlex("load-progress", "height", 12);
setProgress("load-progress", 0.65);

// ── Undo / Redo ─────────────────────────────────────────────────
createLabel("history-label", "History", "left-panel");
setFontSize("history-label", 10);

createRow("undo-redo-row", "left-panel");
setFlex("undo-redo-row", "gap", 4);
setFlex("undo-redo-row", "height", 22);

createLabel("undo-btn", "Undo", "undo-redo-row");
setFontSize("undo-btn", 10);
setFlex("undo-btn", "flex_grow", 1);

createLabel("redo-btn", "Redo", "undo-redo-row");
setFontSize("redo-btn", 10);
setFlex("redo-btn", "flex_grow", 1);

// Wire undo/redo click handlers
on("undo-btn", "click", function() { undo(); });
on("redo-btn", "click", function() { redo(); });

// ── Import / Export ─────────────────────────────────────────────
createLabel("io-label", "Import / Export", "left-panel");
setFontSize("io-label", 10);

createRow("io-row", "left-panel");
setFlex("io-row", "gap", 4);
setFlex("io-row", "height", 22);

createLabel("import-btn", "Import JSON...", "io-row");
setFontSize("import-btn", 10);
setFlex("import-btn", "flex_grow", 1);

createCombo("export-format", "io-row");
setItems("export-format", ["Export JSON", "Export CSS", "Export C++"]);
setFlex("export-format", "flex_grow", 1);
setFlex("export-format", "height", 22);

on("import-btn", "click", function() {
    var ok = importThemeFromFile();
    if (ok) {
        addChatMessage("system", "Theme imported successfully");
    }
});

on("export-format", "select", function(idx) {
    var content = "";
    var ext = "";
    if (idx === 0) { content = exportAsJson(); ext = "json"; }
    else if (idx === 1) { content = exportAsCss(); ext = "css"; }
    else { content = exportAsCppHeader(); ext = "hpp"; }

    var path = "/tmp/pulp-theme-export." + ext;
    saveToFile(content, path);
    exec("open " + path);
    addChatMessage("system", "Exported to " + path);
});

// ═══════════════════════════════════════════════════════════════════
// CENTER PANEL — Preview Canvas
// ═══════════════════════════════════════════════════════════════════

createCol("center-panel");
setFlex("center-panel", "flex_grow", 1);
setFlex("center-panel", "padding", 16);
setFlex("center-panel", "gap", 10);

createLabel("preview-title", "Preview", "center-panel");
setFontSize("preview-title", 13);

// Large interactive knobs
createRow("preview-knobs", "center-panel");
setFlex("preview-knobs", "gap", 20);
setFlex("preview-knobs", "height", 96);
setFlex("preview-knobs", "align_items", "center");

createKnob("gain-knob", "preview-knobs");
setLabel("gain-knob", "Gain");
setFlex("gain-knob", "width", 76);
setFlex("gain-knob", "height", 96);
setValue("gain-knob", 0.6);

createKnob("freq-knob", "preview-knobs");
setLabel("freq-knob", "Frequency");
setStyle("freq-knob", "filled");
setFlex("freq-knob", "width", 76);
setFlex("freq-knob", "height", 96);
setValue("freq-knob", 0.45);

createKnob("res-knob", "preview-knobs");
setLabel("res-knob", "Resonance");
setStyle("res-knob", "notched");
setFlex("res-knob", "width", 76);
setFlex("res-knob", "height", 96);
setValue("res-knob", 0.3);

// Horizontal fader
createFader("mix-fader", "horizontal", "center-panel");
setFlex("mix-fader", "height", 24);
setValue("mix-fader", 0.5);

// Waveform display
createWaveform("waveform", "center-panel");
setFlex("waveform", "height", 90);

var waveData = [];
for (var i = 0; i < 512; i++) {
    waveData.push(Math.sin(2 * Math.PI * 3 * i / 512));
}
setWaveformData("waveform", waveData);

// Spectrum display
createSpectrum("spectrum", "center-panel");
setFlex("spectrum", "height", 70);
setStyle("spectrum", "filled");

var specData = [];
for (var i = 0; i < 64; i++) {
    specData.push(-10 - i * 1.0);
}
setSpectrumData("spectrum", specData);

// Bottom row: XYPad + Meter
createRow("bottom-row", "center-panel");
setFlex("bottom-row", "gap", 12);
setFlex("bottom-row", "flex_grow", 1);
setFlex("bottom-row", "align_items", "center");

createXYPad("xypad", "bottom-row");
setFlex("xypad", "flex_grow", 1);
setFlex("xypad", "min_height", 80);
setXY("xypad", 0.3, 0.6);

createMeter("meter1", "vertical", "bottom-row");
setFlex("meter1", "width", 16);
setFlex("meter1", "min_height", 80);
setMeterLevel("meter1", 0.7, 0.85);

createMeter("meter2", "vertical", "bottom-row");
setFlex("meter2", "width", 16);
setFlex("meter2", "min_height", 80);
setMeterLevel("meter2", 0.55, 0.72);

// ═══════════════════════════════════════════════════════════════════
// RIGHT PANEL — Chat
// ═══════════════════════════════════════════════════════════════════

createCol("right-panel");
setFlex("right-panel", "width", 280);
setFlex("right-panel", "padding", 10);
setFlex("right-panel", "gap", 6);

// Header: title + model
createRow("chat-header", "right-panel");
setFlex("chat-header", "height", 22);
setFlex("chat-header", "gap", 8);
setFlex("chat-header", "align_items", "center");

createLabel("chat-title", "Chat", "chat-header");
setFontSize("chat-title", 13);
setFlex("chat-title", "flex_grow", 1);

createCombo("model-selector", "chat-header");
setItems("model-selector", ["Sonnet 4.6", "Opus 4.6"]);
setFlex("model-selector", "width", 100);
setFlex("model-selector", "height", 20);

// Context
createLabel("chat-context", "Editing: All Components", "right-panel");
setFontSize("chat-context", 9);

// Messages
createCol("chat-messages", "right-panel");
setFlex("chat-messages", "flex_grow", 1);
setFlex("chat-messages", "gap", 4);
setFlex("chat-messages", "padding", 2);

createLabel("welcome-msg", "Describe a style to restyle the showcase live.", "chat-messages");
setFontSize("welcome-msg", 11);

createLabel("hint-msg", 'Try: "warm vintage" or "neon cyberpunk"', "chat-messages");
setFontSize("hint-msg", 10);

// Status
createLabel("status-label", "Ready", "right-panel");
setFontSize("status-label", 9);

// Input
createTextEditor("chat-input", "right-panel");
setPlaceholder("chat-input", "Describe a style...");
setFlex("chat-input", "height", 28);

// ═══════════════════════════════════════════════════════════════════
// Chat + AI Logic
// ═══════════════════════════════════════════════════════════════════

function addChatMessage(role, text) {
    var id = "msg-" + (msgCount++);
    var prefix = role === "user" ? "You: " : (role === "system" ? "» " : "AI: ");
    createLabel(id, prefix + text, "chat-messages");
    setFontSize(id, 11);
    // Save snapshot for this message
    chatSnapshots.push({ role: role, text: text, themeJson: getThemeJson() });
    layout();
}

on("chat-input", "return", function(text) {
    if (!text || text.length === 0 || generating) return;

    addChatMessage("user", text);
    setText("chat-input", "");
    setText("status-label", "Generating...");
    generating = true;
    layout();

    var themeJson = getThemeJson();
    var model = "claude-sonnet-4-6";

    var prompt = "You are a design token expert for audio plugin UIs.\n";
    prompt += "You modify design tokens to achieve a requested visual style.\n\n";
    prompt += "## Current Theme\n" + themeJson + "\n\n";
    prompt += "## CRITICAL RULES\n";
    prompt += "1. Output ONLY a JSON object. No markdown, no explanation.\n";
    prompt += "2. Include ONLY tokens that CHANGE. Typically 5-12 color tokens.\n";
    prompt += "3. Do NOT change dimensions unless specifically asked.\n";
    prompt += "4. Colors are hex #rrggbb.\n\n";
    prompt += '## Request\n"' + text + '"\n\n## Output\n';

    var tmpFile = "/tmp/pulp-design-prompt.txt";
    exec("cat > " + tmpFile + " << 'PULPEOF'\n" + prompt + "\nPULPEOF");
    var response = exec("cat " + tmpFile + " | claude --print --model " + model + " 2>/dev/null");

    generating = false;

    if (!response || response.length === 0) {
        addChatMessage("assistant", "No response from Claude");
        setText("status-label", "Error");
        layout();
        return;
    }

    var jsonStart = response.indexOf("{");
    var jsonEnd = response.lastIndexOf("}");
    if (jsonStart < 0 || jsonEnd < 0) {
        addChatMessage("assistant", "No JSON in response");
        setText("status-label", "Error");
        layout();
        return;
    }

    var jsonDiff = response.substring(jsonStart, jsonEnd + 1);
    pushThemeState();
    applyTokenDiff(jsonDiff);
    pushThemeState();

    var changeCount = (jsonDiff.match(/#[0-9a-fA-F]{6}/g) || []).length;
    addChatMessage("assistant", "Applied " + changeCount + " token changes");
    setText("status-label", changeCount + " tokens changed");
    layout();
});

// Apply initial palette and capture initial undo state
var initialDiff = PaletteSystem.toThemeDiff(currentPalette);
applyTokenDiff(initialDiff);
pushThemeState();

layout();
