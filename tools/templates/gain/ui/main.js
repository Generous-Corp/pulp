// {{PLUGIN_NAME}} — UI Script
// Edit this file and save — hot-reload updates the UI instantly.
//
// Live data stays cheap by construction here: the output meter is bound
// natively (no JS runs per audio tick), and the readout is throttled to ~10 Hz
// with a pinned width. Keep it that way as the UI grows — see the view-bridge
// skill's "Realtime scripted editors: the performance checklist".

// Root layout
const root = createCol("root");
setFlex("root", "padding_top", 24);
setFlex("root", "padding_left", 24);
setFlex("root", "padding_right", 24);
setFlex("root", "padding_bottom", 24);
setFlex("root", "gap", 20);
setFlex("root", "align_items", "center");
setBackground("root", "#1a1a2e");

// Title
createLabel("title", "{{PLUGIN_NAME}}", "root");
setFontSize("title", 20);
setFontWeight("title", 700);
setTextColor("title", "#e0e0e0");
setTextAlign("title", "center");

// Gain knob section
const knobCol = createCol("knob-col", "root");
setFlex("knob-col", "align_items", "center");
setFlex("knob-col", "gap", 8);

createKnob("gain", "knob-col");
setFlex("gain", "width", 80);
setFlex("gain", "height", 80);
setValue("gain", getParam("Gain"));
setLabel("gain", "Gain");

// Value readout. The width is pinned to fit the widest string ("-60.0 dB"),
// so a digit change repaints the label without re-running layout.
createLabel("readout", "0.0 dB", "knob-col");
setFlex("readout", "width", 72);
setFontSize("readout", 14);
setTextColor("readout", "#888888");
setTextAlign("readout", "center");

// Write at most every 100 ms while dragging, then once more with the final
// value. Faster digits are unreadable and each write shapes text.
function throttled(write, intervalMs) {
    let last = 0, pending = null, timer = 0;
    return (value) => {
        pending = value;
        const wait = last + intervalMs - Date.now();
        if (wait <= 0) {
            last = Date.now();
            write(pending);
        } else if (!timer) {
            timer = setTimeout(() => {
                timer = 0;
                last = Date.now();
                write(pending);
            }, wait);
        }
    };
}
const showGain = throttled((v) => {
    const db = -60 + v * 84; // map 0..1 to -60..+24 dB
    setText("readout", db.toFixed(1) + " dB");
}, 100);

on("gain", "change", (v) => {
    setParam("Gain", v);
    showGain(v);
});

// Output meter: bound to the processor's "output" value channel. The framework
// reads the latest level each frame and repaints; no script runs per tick.
createMeter("out-meter", "horizontal", "knob-col");
setFlex("out-meter", "width", 120);
setFlex("out-meter", "height", 8);
bindMeter("out-meter", "value:output");

// Bypass toggle
const bypassRow = createRow("bypass-row", "root");
setFlex("bypass-row", "align_items", "center");
setFlex("bypass-row", "gap", 8);

createToggle("bypass", "bypass-row");
setValue("bypass", getParam("Bypass"));

createLabel("bypass-lbl", "Bypass", "bypass-row");
setFontSize("bypass-lbl", 13);
setTextColor("bypass-lbl", "#999999");

on("bypass", "toggle", (v) => {
    setParam("Bypass", v ? 1.0 : 0.0);
    setOpacity("gain", v ? 0.4 : 1.0);
});
