// Deterministic DPR fidelity fixture: small text and sub-two-pixel strokes.
const root = createCol("root");
setBackground("root", "#10151c");
setFlex("root", "width", 640);
setFlex("root", "height", 360);
setFlex("root", "padding", 24);
setFlex("root", "gap", 10);

[9, 10, 11, 12, 14].forEach((size, index) => {
    const id = "text-" + size;
    createLabel(id, "Hamburgefonts 0123456789 @ " + size + "pt", "root");
    setFontSize(id, size);
    setTextColor(id, index % 2 ? "#d5dde8" : "#f2f5f8");
});
const strokes = createCanvas("thin-strokes", "root");
setFlex("thin-strokes", "width", 592);
setFlex("thin-strokes", "height", 140);
canvasSetFillColor("thin-strokes", "#17202b");
canvasFillRect("thin-strokes", 0, 0, 592, 140);
[0.5, 1.0, 1.5, 2.0].forEach((width, index) => {
    const y = 22 + index * 28;
    canvasSetStrokeColor("thin-strokes", index % 2 ? "#5ce1cc" : "#f6b847");
    canvasSetLineWidth("thin-strokes", width);
    canvasMoveTo("thin-strokes", 18, y);
    canvasLineTo("thin-strokes", 574, y);
    canvasStrokePath("thin-strokes");
});
