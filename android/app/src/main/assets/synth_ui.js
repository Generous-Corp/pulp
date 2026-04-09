// ── PULP SYNTH — JS-Scripted UI ─────────────────────────────────────────

// Root layout
const root = createCol("root");
setFlex("root", "padding", 12);
setFlex("root", "gap", 4);
setBackground("root", "#1E1E2E");

// ── Title ────────────────────────────────────────────────────────────────
createLabel("title", "PULP SYNTH", "root");
setFontSize("title", 22);
setFontWeight("title", 700);
setTextColor("title", "#CDD6F4");
setFlex("title", "margin_bottom", 8);

// ── OSCILLATOR ───────────────────────────────────────────────────────────
createLabel("osc-label", "OSCILLATOR", "root");
setFontSize("osc-label", 11);
setTextColor("osc-label", "#A6ADC8");

const oscRow = createRow("osc-row", "root");
setFlex("osc-row", "justify_content", "space_evenly");
setFlex("osc-row", "align_items", "center");
setFlex("osc-row", "height", 56);
setFlex("osc-row", "margin_left", 8);
setFlex("osc-row", "margin_right", 8);

const oscNames = ["Pitch", "Detune", "Mix", "Level"];
const oscDefaults = [0.5, 0.3, 0.5, 0.6];
for (let i = 0; i < 4; i++) {
    const id = "osc-" + i;
    createKnob(id, "osc-row");
    setFlex(id, "width", 48);
    setFlex(id, "height", 48);
    setValue(id, oscDefaults[i]);
    setLabel(id, oscNames[i]);
}

// ── TOGGLES ──────────────────────────────────────────────────────────────
const toggleRow = createRow("toggle-row", "root");
setFlex("toggle-row", "justify_content", "space_evenly");
setFlex("toggle-row", "align_items", "center");
setFlex("toggle-row", "height", 36);
setFlex("toggle-row", "margin_top", 4);
setFlex("toggle-row", "margin_left", 8);
setFlex("toggle-row", "margin_right", 8);

const toggleDefaults = [true, false, true, false];
for (let i = 0; i < 4; i++) {
    const id = "toggle-" + i;
    createToggle(id, "toggle-row");
    setFlex(id, "width", 48);
    setFlex(id, "height", 28);
    setValue(id, toggleDefaults[i] ? 1.0 : 0.0);
}

// ── XY PAD ───────────────────────────────────────────────────────────────
createLabel("xy-label", "XY PAD", "root");
setFontSize("xy-label", 11);
setTextColor("xy-label", "#A6ADC8");
setFlex("xy-label", "margin_top", 4);

createXYPad("xy", "root");
setFlex("xy", "height", 120);
setFlex("xy", "margin_left", 8);
setFlex("xy", "margin_right", 8);

// ── FILTER ───────────────────────────────────────────────────────────────
createLabel("filter-label", "FILTER", "root");
setFontSize("filter-label", 11);
setTextColor("filter-label", "#A6ADC8");
setFlex("filter-label", "margin_top", 6);

const filterRow = createRow("filter-row", "root");
setFlex("filter-row", "justify_content", "space_evenly");
setFlex("filter-row", "align_items", "center");
setFlex("filter-row", "height", 52);
setFlex("filter-row", "margin_left", 8);
setFlex("filter-row", "margin_right", 8);

const filterNames = ["Cutoff", "Reso", "Env"];
const filterDefaults = [0.65, 0.35, 0.5];
for (let i = 0; i < 3; i++) {
    const id = "filter-" + i;
    createKnob(id, "filter-row");
    setFlex(id, "width", 44);
    setFlex(id, "height", 44);
    setValue(id, filterDefaults[i]);
    setLabel(id, filterNames[i]);
}

// ── ENVELOPE ─────────────────────────────────────────────────────────────
createLabel("env-label", "ENVELOPE", "root");
setFontSize("env-label", 11);
setTextColor("env-label", "#A6ADC8");
setFlex("env-label", "margin_top", 4);

const envRow = createRow("env-row", "root");
setFlex("env-row", "justify_content", "space_evenly");
setFlex("env-row", "align_items", "center");
setFlex("env-row", "height", 48);
setFlex("env-row", "margin_left", 8);
setFlex("env-row", "margin_right", 8);

const envNames = ["A", "D", "S", "R"];
const envDefaults = [0.05, 0.3, 0.7, 0.4];
for (let i = 0; i < 4; i++) {
    const id = "env-" + i;
    createKnob(id, "env-row");
    setFlex(id, "width", 40);
    setFlex(id, "height", 40);
    setValue(id, envDefaults[i]);
    setLabel(id, envNames[i]);
}

// ── MIXER ────────────────────────────────────────────────────────────────
createLabel("mixer-label", "MIXER", "root");
setFontSize("mixer-label", 11);
setTextColor("mixer-label", "#A6ADC8");
setFlex("mixer-label", "margin_top", 6);

const mixDefaults = [0.75, 0.6, 0.4, 0.2];
for (let i = 0; i < 4; i++) {
    const id = "mix-" + i;
    createFader(id, "horizontal", "root");
    setFlex(id, "height", 22);
    setFlex(id, "margin_left", 8);
    setFlex(id, "margin_right", 8);
    setFlex(id, "margin_top", i === 0 ? 0 : 4);
    setValue(id, mixDefaults[i]);
}

// ── MASTER ───────────────────────────────────────────────────────────────
createLabel("master-label", "MASTER", "root");
setFontSize("master-label", 11);
setTextColor("master-label", "#A6ADC8");
setFlex("master-label", "margin_top", 6);

createFader("master", "horizontal", "root");
setFlex("master", "height", 26);
setFlex("master", "margin_left", 8);
setFlex("master", "margin_right", 8);
setValue("master", 0.8);

// Oscilloscope + VU meter are drawn directly by the C++ render loop
// using GPU canvas drawing for crisp, animated waveform display.

