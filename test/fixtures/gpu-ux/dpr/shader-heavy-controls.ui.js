// Deterministic shader-heavy control composition. The experiment runner must
// use the real GPU backend and reject a CPU-raster substitution.
const root = createRow("root");
setBackground("root", "#0b1017");
setFlex("root", "width", 640);
setFlex("root", "height", 360);
setFlex("root", "padding", 28);
setFlex("root", "gap", 28);

for (let i = 0; i < 8; ++i) {
    const column = createCol("column-" + i, "root");
    setFlex("column-" + i, "gap", 10);
    const knob = createKnob("knob-" + i, "column-" + i);
    setFlex("knob-" + i, "width", 52);
    setFlex("knob-" + i, "height", 52);
    setValue("knob-" + i, (i + 1) / 9);
    const meter = createMeter("meter-" + i, "vertical", "column-" + i);
    setFlex("meter-" + i, "width", 18);
    setFlex("meter-" + i, "height", 210);
    setMeterLevel("meter-" + i, 0.18 + i * 0.09, 0.12 + i * 0.075);
}

