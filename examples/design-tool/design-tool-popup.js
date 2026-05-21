
// ═══════════════════════════════════════════════════════════════════
// D1: Token Palette Picker Popup
// ═══════════════════════════════════════════════════════════════════
var TOKEN_POPUP_W = 280;

// Click-outside backdrop
function buildTokenPopupShell() {
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
    setFlex("token-popup", "padding", 12);
    setFlex("token-popup", "gap", 8);
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
    createLabel("tp-token-modified", "*", "tp-header");
    setFontSize("tp-token-modified", 12);
    setTextColor("tp-token-modified", APP_ACCENT_HOVER);
    setFlex("tp-token-modified", "width", 10);
    setFlex("tp-token-modified", "height", 14);
    setOpacity("tp-token-modified", 0.0);

    createCol("tp-close", "tp-header");
    setFlex("tp-close", "width", 26);
    setFlex("tp-close", "height", 26);
    setFlex("tp-close", "justify_content", "center");
    setFlex("tp-close", "align_items", "center");
    setBackground("tp-close", APP_SURFACE);
    setBorder("tp-close", APP_BORDER, 1, 13);
    setCursor("tp-close", "pointer");
    createLabel("tp-close-lbl", "\u00d7", "tp-close");
    setFontSize("tp-close-lbl", 12);
    setPointerEvents("tp-close-lbl", "none");
    registerClick("tp-close");
    registerPointer("tp-close");
    on("tp-close", "pointerdown", function() { closeTokenPopup(); });
    on("tp-close", "click", function() { closeTokenPopup(); });

    // Undo / Redo / Reset row
    createRow("tp-undo-row", "token-popup");
    setFlex("tp-undo-row", "height", 22);
    setFlex("tp-undo-row", "gap", 4);
    setFlex("tp-undo-row", "align_items", "center");
}
buildTokenPopupShell();

var tpUndoBtns = ["Undo", "Redo", "Reset"];
function buildTokenPopupControls() {
    for (var ub = 0; ub < tpUndoBtns.length; ub++) {
        var ubId = "tp-btn-" + ub;
        createCol(ubId, "tp-undo-row");
        setFlex(ubId, "width", ub === 2 ? 44 : 48);
        setFlex(ubId, "min_width", ub === 2 ? 44 : 48);
        setFlex(ubId, "max_width", ub === 2 ? 44 : 48);
        setFlex(ubId, "flex_shrink", 0);
        setFlex(ubId, "height", 22);
        setFlex(ubId, "justify_content", "center");
        setFlex(ubId, "align_items", "center");
        setBackground(ubId, APP_SURFACE);
        setBorder(ubId, APP_BORDER, 1, 4);
        createLabel(ubId + "-lbl", tpUndoBtns[ub], ubId);
        setFontSize(ubId + "-lbl", 9);
        setPointerEvents(ubId + "-lbl", "none");
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
        resetTokenColor(tokenEditState.activeToken);
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
        if (!/^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/.test(hex)) return;
        if (hex.length === 7) hex = applyHexAlpha(hex, getValue("tp-alpha-fdr"));
        applyTokenColor(tokenEditState.activeToken, hex);
    });
    on("tp-hex-input", "escape", function() {
        closeTokenPopup();
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
            var themeColors = JSON.parse(getThemeJson()).colors || {};
            var currentHex = themeColors[tokenEditState.activeToken] || '#000000';
            applyTokenColor(tokenEditState.activeToken, applyHexAlpha(currentHex, alpha), { flash: false });
        }
    });

    // 5 palette shade grids
    createCol("tp-palettes", "token-popup");
    setFlex("tp-palettes", "gap", 6);

    for (var pp = 0; pp < paletteNames.length; pp++) {
        var ppId = "tp-pal-" + pp;
        createCol(ppId, "tp-palettes");
        setFlex(ppId, "gap", 2);

        createLabel(ppId + "-title", paletteNames[pp].toUpperCase(), ppId);
        setFontSize(ppId + "-title", 9);
        setTextColor(ppId + "-title", APP_TEXT_DIM);
        setFlex(ppId + "-title", "height", 14);

        createRow(ppId + "-row", ppId);
        setFlex(ppId + "-row", "gap", 2);
        setFlex(ppId + "-row", "height", 20);

        for (var ps = 0; ps < ShadeGenerator.STEPS.length; ps++) {
            var psId = ppId + "-s" + ps;
            createCol(psId, ppId + "-row");
            setFlex(psId, "flex_grow", 1);
            setFlex(psId, "height", 20);
            setBorder(psId, APP_BORDER, 0, 2);
            registerClick(psId);
            (function(palIdx, shadeIdx) {
                on("tp-pal-" + palIdx + "-s" + shadeIdx, "click", function() {
                    if (!tokenEditState.activeToken) return;
                    var palette = PaletteSystem.create(currentAccent, currentHarmony);
                    var pKey = paletteKeys[palIdx];
                    var hex = palette[pKey][ShadeGenerator.STEPS[shadeIdx]].hex;
                    hex = applyHexAlpha(hex, getValue("tp-alpha-fdr"));
                    applyTokenColor(tokenEditState.activeToken, hex);
                });
            })(pp, ps);
        }
    }
}
buildTokenPopupControls();

// Custom HCL section (expandable)
var tpCustomOpen = false;
function buildTokenPopupCustomToggle() {
    createRow("tp-custom-toggle", "token-popup");
    setFlex("tp-custom-toggle", "height", 22);
    setFlex("tp-custom-toggle", "align_items", "center");
    registerClick("tp-custom-toggle");
    createLabel("tp-custom-lbl", "\u25b6 Custom color picker", "tp-custom-toggle");
    setFontSize("tp-custom-lbl", 10);
    setTextColor("tp-custom-lbl", APP_TEXT_DIM);
    setPointerEvents("tp-custom-lbl", "none");

    createCol("tp-custom", "token-popup");
    setFlex("tp-custom", "gap", 4);
    setVisible("tp-custom", false);
}
buildTokenPopupCustomToggle();

// D2: Gamut triangle canvas — rendered as grid of colored rectangles
var GAMUT_W = 50;  // columns (lightness steps)
var GAMUT_H = 30;  // rows (chroma steps)
var GAMUT_MAX_C = 0.4;
var GAMUT_TRIANGLE_COLS = 220;
var gamutHue = 0;

var tpGamutRenderHue = -1;
var tpPopupHclSyncActive = false;
var tpPopupGamutDragActive = false;
var tpPopupSliderDragKind = "";
var tpPopupShaderHueBucket = -1;
var tpPopupSliderApplyFrame = 0;
var tpPopupSliderPending = null;

function buildTokenPopupGamut() {
    createCol("tp-gamut-wrap", "tp-custom");
    setPosition("tp-gamut-wrap", "relative");
    setFlex("tp-gamut-wrap", "width", 250);
    setFlex("tp-gamut-wrap", "height", 120);
    setFlex("tp-gamut-wrap", "flex_shrink", 0);

    createCanvas("tp-gamut", "tp-gamut-wrap");
    setFlex("tp-gamut", "width", 250);
    setFlex("tp-gamut", "height", 120);
    setBackground("tp-gamut", APP_SURFACE);
    setPointerEvents("tp-gamut", "none");

    createCanvas("tp-gamut-overlay", "tp-gamut-wrap");
    setPosition("tp-gamut-overlay", "absolute");
    setTop("tp-gamut-overlay", 0);
    setLeft("tp-gamut-overlay", 0);
    setFlex("tp-gamut-overlay", "width", 250);
    setFlex("tp-gamut-overlay", "height", 120);
    registerPointer("tp-gamut-overlay");
}
buildTokenPopupGamut();

function renderTokenPopupGamutOverlay(renderHue, dotL, dotC, dotHex) {
    var overlayId = "tp-gamut-overlay";
    var w = 250, h = 120;
    canvasClear(overlayId);
    if (dotL === undefined || dotC === undefined) return;
    var dx = Math.max(0, Math.min(w, dotL * w));
    var dy = Math.max(0, Math.min(h, (1 - dotC / GAMUT_MAX_C) * h));
    canvasStrokeLine(overlayId, dx, 0, dx, h, '#ffffff16', 0.5);
    canvasStrokeLine(overlayId, 0, dy, w, dy, '#ffffff16', 0.5);
    canvasFillCircle(overlayId, dx, dy, 10, '#00000052');
    canvasFillCircle(overlayId, dx, dy, 8, '#f5f7ffef');
    canvasFillCircle(overlayId, dx, dy, 5.5, '#10131bcc');
    canvasFillCircle(overlayId, dx, dy, 3.3, dotHex || OklchEngine.oklchToHex(dotL, dotC, renderHue));
}

function renderGamutTriangle(renderHue, dotL, dotC, fullRedraw, dotHex) {
    gamutHue = renderHue;
    var gamutId = "tp-gamut";
    var w = 250, h = 120;
    if (fullRedraw !== false || tpGamutRenderHue !== renderHue) {
        tpGamutRenderHue = renderHue;
        canvasClear(gamutId);
        canvasRect(gamutId, 0, 0, w, h, APP_SURFACE);

        var bSteps = 1024;
        var boundary = computeGamutBoundary(renderHue, bSteps);
        var cols = 520;
        var colW = w / cols;
        for (var gx = 0; gx < cols; gx++) {
            var L = gx / (cols - 1);
            var boundaryIdx = Math.min(bSteps, Math.max(0, Math.round((gx / (cols - 1)) * bSteps)));
            var maxC = boundary[boundaryIdx];
            var topY = (1 - maxC / GAMUT_MAX_C) * h;
            if (topY > 0.35) {
                canvasRect(gamutId, gx * colW, 0, colW + 0.55, topY + 0.35, APP_SURFACE);
            }
            if (maxC > 0.001) {
                var topHex = OklchEngine.oklchToHex(L, Math.max(0.015, maxC * 0.96), renderHue);
                var bottomHex = OklchEngine.oklchToHex(L, 0.0001, renderHue);
                canvasSetLinearGradient(gamutId, gx * colW, topY, gx * colW, h, topHex, bottomHex);
                canvasBeginPath(gamutId);
                canvasMoveTo(gamutId, gx * colW, topY);
                canvasLineTo(gamutId, gx * colW + colW + 0.75, topY);
                canvasLineTo(gamutId, gx * colW + colW + 0.75, h);
                canvasLineTo(gamutId, gx * colW, h);
                canvasClosePath(gamutId);
                canvasFillPath(gamutId);
                canvasClearGradient(gamutId);
            }
        }

        canvasSetLineJoin(gamutId, "round");
        canvasSetLineCap(gamutId, "round");
        canvasSetStrokeColor(gamutId, '#ffffff12');
        canvasSetLineWidth(gamutId, 2.25);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, (1 - boundary[0] / GAMUT_MAX_C) * h);
        for (var bx = 1; bx <= bSteps; bx++) {
            canvasLineTo(gamutId, (bx / bSteps) * w, (1 - boundary[bx] / GAMUT_MAX_C) * h);
        }
        canvasStrokePath(gamutId);

        canvasSetStrokeColor(gamutId, '#ffffff34');
        canvasSetLineWidth(gamutId, 1.05);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, (1 - boundary[0] / GAMUT_MAX_C) * h);
        for (var bx2 = 1; bx2 <= bSteps; bx2++) {
            canvasLineTo(gamutId, (bx2 / bSteps) * w, (1 - boundary[bx2] / GAMUT_MAX_C) * h);
        }
        canvasStrokePath(gamutId);

        canvasSetStrokeColor(gamutId, '#ffffff10');
        canvasSetLineWidth(gamutId, 0.75);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, h);
        canvasLineTo(gamutId, 0, (1 - boundary[0] / GAMUT_MAX_C) * h);
        for (var bx3 = 1; bx3 <= bSteps; bx3++) {
            canvasLineTo(gamutId, (bx3 / bSteps) * w, (1 - boundary[bx3] / GAMUT_MAX_C) * h);
        }
        canvasLineTo(gamutId, w, h);
        canvasStrokePath(gamutId);
    }

    renderTokenPopupGamutOverlay(renderHue, dotL, dotC, dotHex);
}

function syncTokenPopupHclFaders(oklch) {
    tpPopupHclSyncActive = true;
    setValue("tp-h-fader", oklch.H / 360);
    setValue("tp-c-fader", Math.min(oklch.C / GAMUT_MAX_C, 1));
    setValue("tp-l-fader", oklch.L);
    tpPopupHclSyncActive = false;
}

function applyTokenPopupSliderShaders(hue) {
    var shaderHue = quantizePaletteShaderHue(hue);
    if (tpPopupShaderHueBucket === shaderHue) return;
    tpPopupShaderHueBucket = shaderHue;
    setWidgetShader("tp-h-fader", buildPaletteHueSliderShader());
    setWidgetShader("tp-c-fader", buildPaletteChromaSliderShader(shaderHue));
}

function updateTokenPopupCustomUi(oklch, renderHue, fullRedraw, syncFaders) {
    var alpha = getValue("tp-alpha-fdr");
    var hex = applyHexAlpha(OklchEngine.oklchToHex(oklch.L, oklch.C, oklch.H), alpha).toUpperCase();
    if (syncFaders) syncTokenPopupHclFaders(oklch);
    setText("tp-hex-input", hex);
    setText("tp-oklch-display", "L: " + oklch.L.toFixed(2) + "  C: " + oklch.C.toFixed(3) + "  H: " + oklch.H.toFixed(1));
    setOpacity("tp-token-modified", isTokenModified(tokenEditState.activeToken) ? 1.0 : 0.0);
    applyTokenPopupSliderShaders(renderHue === undefined ? oklch.H : renderHue);
    renderGamutTriangle(renderHue === undefined ? oklch.H : renderHue, oklch.L, oklch.C, fullRedraw, OklchEngine.oklchToHex(oklch.L, oklch.C, oklch.H));
}

function applyTokenPopupOklch(oklch, options) {
    options = options || {};
    if (!tokenEditState.activeToken) return;
    var alpha = getValue("tp-alpha-fdr");
    var hex = applyHexAlpha(OklchEngine.oklchToHex(oklch.L, oklch.C, oklch.H), alpha);
    var commit = !!options.commit;
    updateTokenPopupCustomUi(oklch, options.renderHue, options.fullRedraw, !!options.syncFaders);
    applyTokenColor(tokenEditState.activeToken, hex, {
        flash: false,
        refreshPopup: false,
        recordHistory: commit,
        snapshot: commit,
        relayout: commit
    });
    setOpacity("tp-token-modified", isTokenModified(tokenEditState.activeToken) ? 1.0 : 0.0);
}

function applyTokenPopupGamutPoint(x, y, commit) {
    if (!tokenEditState.activeToken) return;
    var gw = 250, gh = 120;
    var h = getValue("tp-h-fader") * 360;
    var L = Math.max(0, Math.min(1, x / gw));
    var C = Math.max(0, Math.min(GAMUT_MAX_C, (1 - y / gh) * GAMUT_MAX_C));
    var mapped = OklchEngine.gamutMap(L, C, h);
    syncTokenPopupHclFaders(mapped);
    applyTokenPopupOklch(mapped, { commit: !!commit, renderHue: h, fullRedraw: !!commit, syncFaders: false });
}

function handleTokenPopupGamutPointer(evt, commit) {
    if (!evt) return;
    applyTokenPopupGamutPoint(evt.offsetX, evt.offsetY, commit);
}

function wireTokenPopupGamut() {
    on("tp-gamut-overlay", "pointerdown", function(evt) {
        tpPopupGamutDragActive = true;
        nativeSetPointerCapture("tp-gamut-overlay", evt && evt.pointerId ? evt.pointerId : 0);
        handleTokenPopupGamutPointer(evt, false);
    });
    on("tp-gamut-overlay", "pointermove", function(evt) {
        if (!tpPopupGamutDragActive) return;
        handleTokenPopupGamutPointer(evt, false);
    });
    on("tp-gamut-overlay", "pointerup", function(evt) {
        if (!tpPopupGamutDragActive) return;
        tpPopupGamutDragActive = false;
        handleTokenPopupGamutPointer(evt, true);
        nativeReleasePointerCapture("tp-gamut-overlay", evt && evt.pointerId ? evt.pointerId : 0);
        updatePopupState(tokenEditState.activeToken);
    });
    on("tp-gamut-overlay", "pointercancel", function(evt) {
        tpPopupGamutDragActive = false;
        nativeReleasePointerCapture("tp-gamut-overlay", evt && evt.pointerId ? evt.pointerId : 0);
        if (tokenEditState.activeToken) updatePopupState(tokenEditState.activeToken);
    });

    // H (hue) fader
    createRow("tp-hue-row", "tp-custom");
    setFlex("tp-hue-row", "height", 24);
    setFlex("tp-hue-row", "gap", 6);
    setFlex("tp-hue-row", "align_items", "center");
    createLabel("tp-h-fader-lbl", "H", "tp-hue-row");
    setFontSize("tp-h-fader-lbl", 10);
    setFlex("tp-h-fader-lbl", "width", 14);
    createFader("tp-h-fader", "horizontal", "tp-hue-row");
    setFlex("tp-h-fader", "flex_grow", 1);
    setFlex("tp-h-fader", "height", 20);

    // C (chroma) fader
    createRow("tp-chroma-row", "tp-custom");
    setFlex("tp-chroma-row", "height", 24);
    setFlex("tp-chroma-row", "gap", 6);
    setFlex("tp-chroma-row", "align_items", "center");
    createLabel("tp-c-fader-lbl", "C", "tp-chroma-row");
    setFontSize("tp-c-fader-lbl", 10);
    setFlex("tp-c-fader-lbl", "width", 14);
    createFader("tp-c-fader", "horizontal", "tp-chroma-row");
    setFlex("tp-c-fader", "flex_grow", 1);
    setFlex("tp-c-fader", "height", 20);

    // L (lightness) fader
    createRow("tp-light-row", "tp-custom");
    setFlex("tp-light-row", "height", 24);
    setFlex("tp-light-row", "gap", 6);
    setFlex("tp-light-row", "align_items", "center");
    createLabel("tp-l-fader-lbl", "L", "tp-light-row");
    setFontSize("tp-l-fader-lbl", 10);
    setFlex("tp-l-fader-lbl", "width", 14);
    createFader("tp-l-fader", "horizontal", "tp-light-row");
    setFlex("tp-l-fader", "flex_grow", 1);
    setFlex("tp-l-fader", "height", 20);

    // OKLCH value display
    createRow("tp-oklch-vals", "tp-custom");
    setFlex("tp-oklch-vals", "height", 16);
    setFlex("tp-oklch-vals", "gap", 8);
    createLabel("tp-oklch-display", "L: 0.50  C: 0.100  H: 180", "tp-oklch-vals");
    setFontSize("tp-oklch-display", 9);
    setTextColor("tp-oklch-display", APP_TEXT_DIM);
}
wireTokenPopupGamut();

function onTpHclChange(hueChanged, forceApply) {
    if (tpPopupHclSyncActive || !tokenEditState.activeToken) return;
    tpPopupSliderPending = {
        h: getValue("tp-h-fader") * 360,
        c: getValue("tp-c-fader") * GAMUT_MAX_C,
        l: getValue("tp-l-fader"),
        hueChanged: !!hueChanged
    };

    var applyPending = function(commit) {
        if (!tpPopupSliderPending || !tokenEditState.activeToken) return;
        var state = tpPopupSliderPending;
        var mapped = OklchEngine.gamutMap(state.l, state.c, state.h);
        var dragging = !!tpPopupSliderDragKind && !commit;
        var renderHue = (dragging && state.hueChanged) ? quantizePalettePreviewHue(state.h) : state.h;
        applyTokenPopupOklch(mapped, {
            commit: !!commit,
            renderHue: renderHue,
            fullRedraw: !!commit || Math.abs(renderHue - tpGamutRenderHue) > 0.1,
            syncFaders: false
        });
        if (commit) updatePopupState(tokenEditState.activeToken);
    };

    if (forceApply || !tpPopupSliderDragKind) {
        if (tpPopupSliderApplyFrame) {
            cancelAnimationFrame(tpPopupSliderApplyFrame);
            tpPopupSliderApplyFrame = 0;
        }
        applyPending(true);
        return;
    }

    if (tpPopupSliderApplyFrame) return;
    tpPopupSliderApplyFrame = requestAnimationFrame(function() {
        tpPopupSliderApplyFrame = 0;
        applyPending(false);
    });
}
function beginTokenPopupSliderDrag(kind) {
    tpPopupSliderDragKind = kind;
}

function endTokenPopupSliderDrag(hueChanged) {
    if (!tpPopupSliderDragKind) return;
    tpPopupSliderDragKind = "";
    onTpHclChange(!!hueChanged, true);
}

function wireTokenPopupSliders() {
    on("tp-h-fader", "pointerdown", function() { beginTokenPopupSliderDrag("h"); });
    on("tp-h-fader", "pointerup", function() { endTokenPopupSliderDrag(true); });
    on("tp-h-fader", "pointercancel", function() { endTokenPopupSliderDrag(true); });
    on("tp-c-fader", "pointerdown", function() { beginTokenPopupSliderDrag("c"); });
    on("tp-c-fader", "pointerup", function() { endTokenPopupSliderDrag(false); });
    on("tp-c-fader", "pointercancel", function() { endTokenPopupSliderDrag(false); });
    on("tp-l-fader", "pointerdown", function() { beginTokenPopupSliderDrag("l"); });
    on("tp-l-fader", "pointerup", function() { endTokenPopupSliderDrag(false); });
    on("tp-l-fader", "pointercancel", function() { endTokenPopupSliderDrag(false); });
    on("tp-h-fader", "change", function() { onTpHclChange(true, false); });
    on("tp-c-fader", "change", function() { onTpHclChange(false, false); });
    on("tp-l-fader", "change", function() { onTpHclChange(false, false); });

    on("tp-custom-toggle", "click", function() {
        tpCustomOpen = !tpCustomOpen;
        setVisible("tp-custom", tpCustomOpen);
        setText("tp-custom-lbl", tpCustomOpen ? "\u25bc Custom color picker" : "\u25b6 Custom color picker");
        if (tpCustomOpen && tokenEditState.activeToken) {
            var hex = (JSON.parse(getThemeJson()).colors || {})[tokenEditState.activeToken] || '#808080';
            var oklch = OklchEngine.hexToOklch(hex);
            applyTokenPopupSliderShaders(oklch.H);
            updateTokenPopupCustomUi(oklch, oklch.H, true, true);
        }
        layout();
        if (tokenEditState.activeSwatchId) positionTokenPopup(tokenEditState.activeSwatchId);
        layout();
    });
}
wireTokenPopupSliders();

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
    setText("tp-token-name", tokenName);
    setText("tp-hex-input", hex.toUpperCase());
    setValue("tp-alpha-fdr", hexAlpha(hex));
    setText("tp-alpha-val", hexAlpha(hex).toFixed(1));
    setOpacity("tp-token-modified", isTokenModified(tokenName) ? 1.0 : 0.0);
    // D2: Sync HCL faders and gamut triangle
    if (tpCustomOpen) {
        var oklch = OklchEngine.hexToOklch(hex);
        updateTokenPopupCustomUi(oklch, oklch.H, true, true);
    }
    // Undo/redo button opacity
    var h = tokenHistory(tokenName);
    var showHistory = h.cursor > 0 || h.stack.length > 1 || !!tokenEditState.modified[tokenName];
    setVisible("tp-undo-row", showHistory);
    setOpacity("tp-btn-0", h.cursor > 0 ? 1.0 : 0.3);
    setOpacity("tp-btn-1", h.cursor < h.stack.length - 1 ? 1.0 : 0.3);
    setOpacity("tp-btn-2", tokenEditState.modified[tokenName] ? 1.0 : 0.3);
}

function positionTokenPopup(anchorId) {
    if (!anchorId) return;
    var rect = getLayoutRect(anchorId);
    var popupRect = getLayoutRect("token-popup");
    var rootSize = getRootSize();
    var viewportW = rootSize && rootSize.width ? rootSize.width : 1100;
    var viewportH = rootSize && rootSize.height ? rootSize.height : 700;
    var popupW = popupRect && popupRect.width ? popupRect.width : TOKEN_POPUP_W;
    var popupH = popupRect && popupRect.height ? popupRect.height : (tpCustomOpen ? 620 : 420);
    var margin = 8;
    var gap = 6;
    var rightSpace = viewportW - rect.right - margin;
    var leftSpace = rect.x - margin;
    var belowSpace = viewportH - rect.y - margin;
    var aboveSpace = rect.bottom - margin;

    var popX = rect.right + gap;
    if (rightSpace < popupW && leftSpace > rightSpace) popX = rect.x - popupW - gap;
    if (popX + popupW > viewportW - margin) popX = viewportW - popupW - margin;
    if (popX < margin) popX = margin;

    var popY = rect.y;
    if (belowSpace < popupH && aboveSpace > belowSpace) {
        popY = rect.bottom - popupH;
    }
    if (popY + popupH > viewportH - margin) popY = viewportH - popupH - margin;
    if (popY < margin) popY = margin;

    setTop("token-popup", popY);
    setLeft("token-popup", popX);
}

function openTokenPopup(tokenName, swatchId, gIdx, tIdx) {
    tokenHistory(tokenName);
    tokenEditState.activeToken = tokenName;
    tokenEditState.activeSwatchId = swatchId;
    rebuildPopupPalette();
    updatePopupState(tokenName);
    setVisible("tp-backdrop", true);
    setVisible("token-popup", true);
    layout();
    positionTokenPopup(swatchId);
    layout();
}

function closeTokenPopup() {
    tokenEditState.activeToken = null;
    tokenEditState.activeSwatchId = null;
    tpCustomOpen = false;
    setVisible("tp-custom", false);
    setText("tp-custom-lbl", "\u25b6 Custom color picker");
    setVisible("token-popup", false);
    setVisible("tp-backdrop", false);
    layout();
}

// ── Accent hue slider handler ────────────────────────────────────
function wireAccentHueAndSelectors() {
    on("accent-hue", "pointerdown", function() {
        var accent = OklchEngine.hexToOklch(currentAccent);
        accentHueBaseL = accent.L;
        accentHueBaseC = accent.C;
        accentHuePreviewCache = {};
        accentHueDragActive = true;
    });
    on("accent-hue", "pointerup", function() {
        accentHueDragActive = false;
        scheduleAccentHueApply(true);
    });
    on("accent-hue", "pointercancel", function() {
        accentHueDragActive = false;
        scheduleAccentHueApply(true);
    });
    on("accent-hue", "change", function(val) {
        accentHuePendingHue = val * 360;
        scheduleAccentHueApply(false);
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
            applyPaletteThemeDiff(palette, currentPaletteMode);
            updateTokenSwatches();
            refreshPaletteEditorsFromPalette(palette, true, true);
            markCurrentModeDirty();
            layout();
        }
    });

    // Dark/Light mode handler
    on("mode-selector", "select", function(idx) {
        switchPaletteMode(idx === 0 ? "dark" : "light");
        refreshPaletteEditorsFromPalette(PaletteSystem.create(currentAccent, currentHarmony), true, true);
        pushThemeSnapshot();
        layout();
    });

    // ── CENTER PANEL (Preview) ───────────────────────────────────────
    createCol("center-panel", "main-area");
    setFlex("center-panel", "flex_grow", 1);
    setFlex("center-panel", "flex_shrink", 1);
    setFlex("center-panel", "min_width", 420);  // Issue 5: prevent scrollbar behind content
    setFlex("center-panel", "padding", 16);
    setFlex("center-panel", "gap", 8);
    setBackground("center-panel", APP_BG);

    createCol("preview-shell", "center-panel");
    setFlex("preview-shell", "flex_grow", 1);
    setFlex("preview-shell", "gap", 0);
    setBackground("preview-shell", APP_PANEL);
    setBorder("preview-shell", APP_BORDER, 1, 12);

    createRow("preview-chrome", "preview-shell");
    setFlex("preview-chrome", "height", 26);
    setFlex("preview-chrome", "padding_left", 10);
    setFlex("preview-chrome", "padding_right", 10);
    setFlex("preview-chrome", "gap", 6);
    setFlex("preview-chrome", "align_items", "center");
    setBackground("preview-chrome", APP_PANEL_RAISED);
}
wireAccentHueAndSelectors();
