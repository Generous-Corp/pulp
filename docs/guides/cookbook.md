# Cookbook

10 practical recipes for common Pulp UI patterns. Each recipe is self-contained — copy the JS into your `ui/main.js` and adapt.

## 1. Form Layout

A labeled parameter section with knobs in a row.

```js
const form = createCol("form", "root");
setFlex("form", "gap", 16);
setFlex("form", "padding_top", 20);
setFlex("form", "padding_left", 20);
setFlex("form", "padding_right", 20);

// Section header
createLabel("hdr", "EQ Controls", "form");
setFontSize("hdr", 16);
setFontWeight("hdr", 700);
setTextColor("hdr", "#e0e0e0");

// Row of knobs
const row = createRow("knobs", "form");
setFlex("knobs", "gap", 24);
setFlex("knobs", "justify_content", "center");

["Low", "Mid", "High"].forEach((name) => {
    const id = name.toLowerCase();
    const col = createCol(id + "-col", "knobs");
    setFlex(id + "-col", "align_items", "center");
    setFlex(id + "-col", "gap", 4);

    createKnob(id, id + "-col");
    setFlex(id, "width", 60);
    setFlex(id, "height", 60);
    setValue(id, getParam(name));
    on(id, "change", (v) => setParam(name, v));

    createLabel(id + "-lbl", name, id + "-col");
    setFontSize(id + "-lbl", 12);
    setTextColor(id + "-lbl", "#888888");
    setTextAlign(id + "-lbl", "center");
});
```

## 2. Modal Dialog

A centered overlay with OK/Cancel buttons.

```js
// Backdrop
const backdrop = createPanel("backdrop", "root");
setFlex("backdrop", "width", 600);
setFlex("backdrop", "height", 400);
setBackground("backdrop", "rgba(0,0,0,0.6)");
setFlex("backdrop", "justify_content", "center");
setFlex("backdrop", "align_items", "center");

// Dialog box
const dialog = createCol("dialog", "backdrop");
setFlex("dialog", "width", 300);
setFlex("dialog", "padding_top", 24);
setFlex("dialog", "padding_left", 24);
setFlex("dialog", "padding_right", 24);
setFlex("dialog", "padding_bottom", 24);
setFlex("dialog", "gap", 16);
setBackground("dialog", "#1e1e2e");
setBorder("dialog", "#333333", 1, 12);
setBoxShadow("dialog", 0, 8, 24, 0, "rgba(0,0,0,0.5)");

createLabel("dialog-title", "Save Preset?", "dialog");
setFontSize("dialog-title", 18);
setFontWeight("dialog-title", 700);
setTextColor("dialog-title", "#e0e0e0");

createLabel("dialog-msg", "This will overwrite the existing preset.", "dialog");
setFontSize("dialog-msg", 14);
setTextColor("dialog-msg", "#999999");

// Button row
const btns = createRow("btns", "dialog");
setFlex("btns", "gap", 12);
setFlex("btns", "justify_content", "flex_end");

const cancel = createToggleButton("cancel", "btns");
setLabel("cancel", "Cancel");
registerClick("cancel");
on("cancel", "click", () => setVisible("backdrop", false));

const ok = createToggleButton("ok", "btns");
setLabel("ok", "Save");
registerClick("ok");
on("ok", "click", () => {
    // Save logic here
    setVisible("backdrop", false);
});
```

## 3. Scrolling List

A scrollable preset list with click selection.

```js
const scroll = createScrollView("list", "root");
setFlex("list", "width", 200);
setFlex("list", "height", 300);
setBorder("list", "#333333", 1, 4);

const content = createCol("list-content", "list");
setFlex("list-content", "gap", 1);

const presets = ["Init", "Warm Pad", "Bright Lead", "Deep Bass",
    "Pluck", "Strings", "Brass", "Choir", "Sweep", "Noise",
    "Arp", "Bell", "Organ", "Keys", "Sub"];

let selectedId = null;

presets.forEach((name, i) => {
    const id = "preset-" + i;
    createLabel(id, name, "list-content");
    setFlex(id, "padding_top", 8);
    setFlex(id, "padding_left", 12);
    setFlex(id, "padding_right", 12);
    setFlex(id, "padding_bottom", 8);
    setFontSize(id, 13);
    setTextColor(id, "#cccccc");
    setBackground(id, i % 2 === 0 ? "#1a1a2e" : "#1e1e32");

    registerClick(id);
    registerHover(id);

    on(id, "mouseenter", () => setBackground(id, "#2a2a4a"));
    on(id, "mouseleave", () => {
        setBackground(id, id === selectedId ? "#3a3a5a" : (i % 2 === 0 ? "#1a1a2e" : "#1e1e32"));
    });
    on(id, "click", () => {
        if (selectedId) setBackground(selectedId, "#1a1a2e");
        selectedId = id;
        setBackground(id, "#3a3a5a");
    });
});

setScrollContentSize("list", 200, presets.length * 35);
```

## 4. Tab Panel

Switchable content panels with tab buttons.

```js
const tabs = createCol("tabs", "root");
setFlex("tabs", "width", 400);
setFlex("tabs", "height", 300);

// Tab bar
const bar = createRow("tab-bar", "tabs");
setFlex("tab-bar", "gap", 0);
setBackground("tab-bar", "#111122");

const pages = ["Oscillator", "Filter", "Envelope"];
let activeTab = 0;

pages.forEach((name, i) => {
    const id = "tab-" + i;
    createLabel(id, name, "tab-bar");
    setFlex(id, "flex_grow", 1);
    setFlex(id, "padding_top", 10);
    setFlex(id, "padding_bottom", 10);
    setTextAlign(id, "center");
    setFontSize(id, 13);
    setTextColor(id, i === 0 ? "#ffffff" : "#666666");
    setBackground(id, i === 0 ? "#1a1a2e" : "transparent");
    setCursor(id, "pointer");

    registerClick(id);
    on(id, "click", () => {
        // Deactivate old tab
        setTextColor("tab-" + activeTab, "#666666");
        setBackground("tab-" + activeTab, "transparent");
        setVisible("page-" + activeTab, false);
        // Activate new tab
        activeTab = i;
        setTextColor(id, "#ffffff");
        setBackground(id, "#1a1a2e");
        setVisible("page-" + i, true);
    });
});

// Content pages
pages.forEach((name, i) => {
    const id = "page-" + i;
    const page = createCol(id, "tabs");
    setFlex(id, "flex_grow", 1);
    setFlex(id, "padding_top", 16);
    setFlex(id, "padding_left", 16);
    setBackground(id, "#1a1a2e");
    setVisible(id, i === 0);

    createLabel(id + "-title", name + " Settings", id);
    setFontSize(id + "-title", 14);
    setTextColor(id + "-title", "#cccccc");
});
```

## 5. Custom Meter

A stereo level meter drawn on a CanvasWidget.

```js
const meter = createCanvas("meter", "root");
setFlex("meter", "width", 40);
setFlex("meter", "height", 200);

function drawMeter(peakL, peakR) {
    canvasClear("meter");
    const w = 40, h = 200;
    const barW = 16, gap = 8;

    // Background
    canvasRect("meter", 0, 0, w, h, "#111111");

    // Left channel
    const lH = peakL * h;
    const lColor = peakL > 0.9 ? "#ff4444" : peakL > 0.7 ? "#ffaa00" : "#44ff44";
    canvasRect("meter", 0, h - lH, barW, lH, lColor);

    // Right channel
    const rH = peakR * h;
    const rColor = peakR > 0.9 ? "#ff4444" : peakR > 0.7 ? "#ffaa00" : "#44ff44";
    canvasRect("meter", barW + gap, h - rH, barW, rH, rColor);

    // Grid lines at -6, -12, -24 dB
    [0.5, 0.25, 0.063].forEach((level) => {
        const y = h - level * h;
        canvasStrokeLine("meter", 0, y, w, y, "#333333", 1);
    });
}

// Poll audio bridge meter data (called by the view system)
// In practice, use Meter widget for automatic polling — this
// shows how to build a custom visualization.
drawMeter(0.6, 0.4);
```

## 6. Theme Switching

Toggle between built-in themes with animated transitions.

```js
const root = createCol("root");
setFlex("root", "padding_top", 20);
setFlex("root", "padding_left", 20);
setFlex("root", "gap", 12);

createLabel("theme-label", "Theme", "root");
setFontSize("theme-label", 14);
setTextColor("theme-label", "#cccccc");

const combo = createCombo("theme-picker", "root");
setItems("theme-picker", ["Dark", "Light", "Pro Audio"]);

on("theme-picker", "change", (index) => {
    const themes = ["dark", "light", "pro_audio"];
    const oldTheme = getThemeJson();
    setTheme(themes[index]);
    const newTheme = getThemeJson();
    applyTokenDiff(newTheme); // Smooth transition
});
```

## 7. Hover Effects

Glow and scale on mouse hover with animation.

```js
function makeHoverButton(id, label, parent) {
    const btn = createPanel(id, parent);
    setFlex(id, "padding_top", 12);
    setFlex(id, "padding_left", 24);
    setFlex(id, "padding_right", 24);
    setFlex(id, "padding_bottom", 12);
    setBackground(id, "#2a2a4a");
    setBorder(id, "#444466", 1, 8);
    setTransitionDuration(id, 0.15);

    const lbl = createLabel(id + "-lbl", label, id);
    setFontSize(id + "-lbl", 14);
    setTextColor(id + "-lbl", "#cccccc");
    setTextAlign(id + "-lbl", "center");

    registerHover(id);
    on(id, "mouseenter", () => {
        setBackground(id, "#3a3a6a");
        setBorder(id, "#6666aa", 1, 8);
        setBoxShadow(id, 0, 0, 12, 2, "rgba(100,100,200,0.3)");
        animate(id, "scale", 1.05, 150, "ease_out_cubic");
    });
    on(id, "mouseleave", () => {
        setBackground(id, "#2a2a4a");
        setBorder(id, "#444466", 1, 8);
        setBoxShadow(id, 0, 0, 0, 0, "transparent");
        animate(id, "scale", 1.0, 150, "ease_out_cubic");
    });
}

const row = createRow("btns", "root");
setFlex("btns", "gap", 12);
makeHoverButton("btn-a", "Preset A", "btns");
makeHoverButton("btn-b", "Preset B", "btns");
makeHoverButton("btn-c", "Preset C", "btns");
```

## 8. Keyboard Shortcuts

Listen for key events on the root to implement shortcuts.

```js
// The root widget receives key events when the plugin window has focus.
on("root", "keydown", (event) => {
    const key = event.key;
    const cmd = event.metaKey || event.ctrlKey;

    if (cmd && key === "s") {
        // Save preset
        showSaveDialog();
    } else if (cmd && key === "z") {
        // Undo — handled by DAW, but you can respond to it
    } else if (key === "Escape") {
        // Close any open modal
        setVisible("backdrop", false);
    } else if (key === "Tab") {
        // Cycle focus between knobs
        cycleFocus();
    } else if (key === " ") {
        // Toggle bypass
        const bypass = getParam("Bypass");
        setParam("Bypass", bypass >= 0.5 ? 0.0 : 1.0);
    }
});

function cycleFocus() {
    // Move focus to next interactive widget
    // The view system handles Tab focus traversal automatically,
    // but you can override for custom behavior.
}

function showSaveDialog() {
    setVisible("backdrop", true);
}
```

## 9. Drag Interaction

An XY pad with custom drag behavior and value readout.

```js
const container = createCol("xy-container", "root");
setFlex("xy-container", "align_items", "center");
setFlex("xy-container", "gap", 8);

// XY Pad
const pad = createXYPad("xy", "xy-container");
setFlex("xy", "width", 200);
setFlex("xy", "height", 200);
setXY("xy", getParam("Cutoff"), getParam("Resonance"));

// Readout labels
const readout = createRow("xy-readout", "xy-container");
setFlex("xy-readout", "gap", 16);

createLabel("xy-x-lbl", "Cutoff: 50%", "xy-readout");
setFontSize("xy-x-lbl", 12);
setTextColor("xy-x-lbl", "#888888");

createLabel("xy-y-lbl", "Resonance: 50%", "xy-readout");
setFontSize("xy-y-lbl", 12);
setTextColor("xy-y-lbl", "#888888");

on("xy", "change", (x, y) => {
    setParam("Cutoff", x);
    setParam("Resonance", y);
    setText("xy-x-lbl", "Cutoff: " + Math.round(x * 100) + "%");
    setText("xy-y-lbl", "Resonance: " + Math.round(y * 100) + "%");
});
```

## 10. Preset Browser

A searchable, categorized preset browser with text filtering.

```js
const browser = createCol("browser", "root");
setFlex("browser", "width", 250);
setFlex("browser", "height", 350);
setBackground("browser", "#141422");
setBorder("browser", "#333333", 1, 8);

// Search bar
const search = createTextEditor("search", "browser");
setFlex("search", "height", 32);
setFlex("search", "margin_top", 8);
setFlex("search", "margin_left", 8);
setFlex("search", "margin_right", 8);
setPlaceholder("search", "Search presets...");

// Category filter
const cats = createRow("cats", "browser");
setFlex("cats", "gap", 4);
setFlex("cats", "padding_left", 8);
setFlex("cats", "padding_top", 8);

const categories = ["All", "Bass", "Lead", "Pad", "FX"];
categories.forEach((cat, i) => {
    const id = "cat-" + i;
    createLabel(id, cat, "cats");
    setFontSize(id, 11);
    setTextColor(id, i === 0 ? "#ffffff" : "#666666");
    setFlex(id, "padding_top", 4);
    setFlex(id, "padding_left", 8);
    setFlex(id, "padding_right", 8);
    setFlex(id, "padding_bottom", 4);
    setBackground(id, i === 0 ? "#333355" : "transparent");
    setBorder(id, "transparent", 0, 4);
    setCursor(id, "pointer");
    registerClick(id);
    on(id, "click", () => filterByCategory(cat));
});

// Preset list
const list = createScrollView("preset-list", "browser");
setFlex("preset-list", "flex_grow", 1);
setFlex("preset-list", "margin_top", 8);

const presets = [
    { name: "Deep Sub", cat: "Bass" },
    { name: "Warm Bass", cat: "Bass" },
    { name: "Screamer", cat: "Lead" },
    { name: "Bright Lead", cat: "Lead" },
    { name: "Lush Pad", cat: "Pad" },
    { name: "Dark Pad", cat: "Pad" },
    { name: "Riser", cat: "FX" },
    { name: "Wobble", cat: "FX" },
];

function renderPresets(filter, category) {
    // Remove old items
    presets.forEach((_, i) => removeWidget("p-" + i));

    presets
        .filter((p) => !category || category === "All" || p.cat === category)
        .filter((p) => !filter || p.name.toLowerCase().includes(filter.toLowerCase()))
        .forEach((p, i) => {
            const id = "p-" + i;
            createLabel(id, p.name, "preset-list");
            setFlex(id, "padding_top", 6);
            setFlex(id, "padding_left", 12);
            setFlex(id, "padding_bottom", 6);
            setFontSize(id, 13);
            setTextColor(id, "#cccccc");
            setCursor(id, "pointer");
            registerClick(id);
            registerHover(id);
            on(id, "mouseenter", () => setBackground(id, "#2a2a4a"));
            on(id, "mouseleave", () => setBackground(id, "transparent"));
            on(id, "click", () => loadPreset(p.name));
        });
}

let currentCategory = "All";

function filterByCategory(cat) {
    currentCategory = cat;
    renderPresets(getText("search"), cat);
}

on("search", "change", () => {
    renderPresets(getText("search"), currentCategory);
});

function loadPreset(name) {
    // Load preset logic — set parameters, update UI
}

renderPresets("", "All");
```
