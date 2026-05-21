
// ── Color System: OKLCH Shade Ramps ──────────────────────────────
var paletteNames = ["Accent", "Neutral", "Success", "Warning", "Error"];
var paletteKeys  = ["accent", "neutral", "success", "warning", "error"];
var paletteValueFormats = ["OKLCH", "OKLCH", "OKLCH", "OKLCH", "OKLCH"];
var paletteApplyFrames = {};
var paletteApplyStates = {};
var paletteShaderHueBuckets = {};
var paletteSliderDragActive = {};
var palettePreviewUpdateTimes = {};
var paletteThemeApplyTimes = {};
var paletteShaderUpdateTimes = {};
var paletteLiveRampCache = {};
var paletteLivePreviewRefs = {};
var accentHueDragActive = false;
var accentHueApplyFrame = 0;
var accentHueLastApplyTime = 0;
var accentHuePreviewBucket = -1;
var accentHuePreviewCache = {};
var initialAccentOklch = OklchEngine.hexToOklch(currentAccent);
var accentHuePendingHue = initialAccentOklch.H;
var accentHueBaseL = initialAccentOklch.L;
var accentHueBaseC = initialAccentOklch.C;

function hexToRgbParts(hex) {
    if (!hex || hex.charAt(0) !== "#") return { r: 0, g: 0, b: 0, a: 255 };
    return {
        r: parseInt(hex.slice(1, 3), 16) || 0,
        g: parseInt(hex.slice(3, 5), 16) || 0,
        b: parseInt(hex.slice(5, 7), 16) || 0,
        a: (hex.length >= 9 ? (parseInt(hex.slice(7, 9), 16) || 0) : 255)
    };
}

function normalizeOpaqueHex(hex) {
    if (!hex || hex.charAt(0) !== "#") return "#000000";
    return ("#" + hex.slice(1, 7)).toUpperCase();
}

function hexAlpha(hex) {
    return hexToRgbParts(hex).a / 255;
}

function applyHexAlpha(hex, alpha) {
    var base = normalizeOpaqueHex(hex);
    var a = Math.max(0, Math.min(1, alpha));
    if (a >= 0.995) return base;
    var alphaHex = Math.round(a * 255).toString(16).toUpperCase();
    if (alphaHex.length < 2) alphaHex = "0" + alphaHex;
    return base + alphaHex;
}

function shaderVec3FromHex(hex) {
    var rgb = hexToRgbParts(hex);
    return "float3(" + (rgb.r / 255).toFixed(4) + ", " + (rgb.g / 255).toFixed(4) + ", " + (rgb.b / 255).toFixed(4) + ")";
}

function buildPaletteHueSliderShader() {
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "float3 hsv2rgb(float3 c) {",
        "  float4 K = float4(1.0, 2.0/3.0, 1.0/3.0, 3.0);",
        "  float3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);",
        "  return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);",
        "}",
        "float roundedRectPx(float2 p, float2 center, float2 size, float radius) {",
        "  float2 q = abs(p - center) - size * 0.5 + float2(radius, radius);",
        "  return length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - radius;",
        "}",
        "half4 main(float2 coord) {",
        "  float2 uv = coord / max(resolution, float2(1.0, 1.0));",
        "  float aa = 1.0;",
        "  float trackH = clamp(resolution.y * 0.28, 4.0, 6.0);",
        "  float radius = trackH * 0.5;",
        "  float2 center = float2(resolution.x * 0.5, resolution.y * 0.5);",
        "  float2 outerSize = float2(max(resolution.x - 2.0, 8.0), trackH + 2.0);",
        "  float2 innerSize = float2(max(resolution.x - 4.0, 6.0), trackH);",
        "  float outerMask = 1.0 - smoothstep(0.0, aa, roundedRectPx(coord, center, outerSize, radius + 1.0));",
        "  float innerMask = 1.0 - smoothstep(0.0, aa, roundedRectPx(coord, center, innerSize, radius));",
        "  float borderMask = max(outerMask - innerMask, 0.0);",
        "  float3 ramp = hsv2rgb(float3(clamp(uv.x, 0.0, 1.0), 0.78, 0.98));",
        "  float gloss = clamp(1.0 - abs(coord.y - (center.y - trackH * 0.18)) / max(trackH * 0.9, 1.0), 0.0, 1.0);",
        "  ramp = mix(ramp * 0.94, min(ramp * 1.04 + float3(0.035, 0.035, 0.035) * gloss, float3(1.0, 1.0, 1.0)), 0.42);",
        "  float thumbR = clamp(resolution.y * 0.34, 5.0, 7.0);",
        "  float thumbX = mix(thumbR, resolution.x - thumbR, clamp(value, 0.0, 1.0));",
        "  float thumbDist = length(coord - float2(thumbX, center.y));",
        "  float shadowDist = length(coord - float2(thumbX, center.y + 1.5));",
        "  float thumbShadow = 1.0 - smoothstep(thumbR * 0.92, thumbR * 1.45, shadowDist);",
        "  float thumb = 1.0 - smoothstep(thumbR - 0.9, thumbR + 0.9, thumbDist);",
        "  float ring = 1.0 - smoothstep(thumbR - 2.0, thumbR - 0.3, thumbDist);",
        "  float highlight = 1.0 - smoothstep(thumbR * 0.18, thumbR * 0.62, length(coord - float2(thumbX - thumbR * 0.35, center.y - thumbR * 0.35)));",
        "  float3 color = mix(float3(0.21, 0.22, 0.26), float3(0.10, 0.11, 0.14), outerMask * 0.38);",
        "  color = mix(color, float3(0.18, 0.19, 0.23), borderMask);",
        "  color = mix(color, ramp, innerMask);",
        "  color = mix(color, float3(0.05, 0.05, 0.07), thumbShadow * 0.18);",
        "  color = mix(color, float3(0.96, 0.97, 0.99), thumb);",
        "  color = mix(color, float3(0.54, 0.56, 0.64), ring * (1.0 - thumb));",
        "  color += float3(0.04, 0.04, 0.05) * highlight * thumb;",
        "  float alpha = max(outerMask, thumbShadow);",
        "  return half4(half3(color) * half(alpha), half(alpha));",
        "}"
    ].join("\n");
}

function buildPaletteChromaSliderShader(hue) {
    var gray = shaderVec3FromHex(OklchEngine.oklchToHex(0.78, 0.0, hue));
    var saturated = shaderVec3FromHex(OklchEngine.oklchToHex(0.78, 0.24, hue));
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "float roundedRectPx(float2 p, float2 center, float2 size, float radius) {",
        "  float2 q = abs(p - center) - size * 0.5 + float2(radius, radius);",
        "  return length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - radius;",
        "}",
        "half4 main(float2 coord) {",
        "  float2 uv = coord / max(resolution, float2(1.0, 1.0));",
        "  float aa = 1.0;",
        "  float trackH = clamp(resolution.y * 0.28, 4.0, 6.0);",
        "  float radius = trackH * 0.5;",
        "  float2 center = float2(resolution.x * 0.5, resolution.y * 0.5);",
        "  float2 outerSize = float2(max(resolution.x - 2.0, 8.0), trackH + 2.0);",
        "  float2 innerSize = float2(max(resolution.x - 4.0, 6.0), trackH);",
        "  float outerMask = 1.0 - smoothstep(0.0, aa, roundedRectPx(coord, center, outerSize, radius + 1.0));",
        "  float innerMask = 1.0 - smoothstep(0.0, aa, roundedRectPx(coord, center, innerSize, radius));",
        "  float borderMask = max(outerMask - innerMask, 0.0);",
        "  float3 gray = " + gray + ";",
        "  float3 sat = " + saturated + ";",
        "  float3 ramp = mix(gray, sat, smoothstep(0.0, 1.0, clamp(uv.x, 0.0, 1.0)));",
        "  float gloss = clamp(1.0 - abs(coord.y - (center.y - trackH * 0.18)) / max(trackH * 0.9, 1.0), 0.0, 1.0);",
        "  ramp = mix(ramp * 0.94, min(ramp * 1.04 + float3(0.035, 0.035, 0.035) * gloss, float3(1.0, 1.0, 1.0)), 0.38);",
        "  float thumbR = clamp(resolution.y * 0.34, 5.0, 7.0);",
        "  float thumbX = mix(thumbR, resolution.x - thumbR, clamp(value, 0.0, 1.0));",
        "  float thumbDist = length(coord - float2(thumbX, center.y));",
        "  float shadowDist = length(coord - float2(thumbX, center.y + 1.5));",
        "  float thumbShadow = 1.0 - smoothstep(thumbR * 0.92, thumbR * 1.45, shadowDist);",
        "  float thumb = 1.0 - smoothstep(thumbR - 0.9, thumbR + 0.9, thumbDist);",
        "  float ring = 1.0 - smoothstep(thumbR - 2.0, thumbR - 0.3, thumbDist);",
        "  float highlight = 1.0 - smoothstep(thumbR * 0.18, thumbR * 0.62, length(coord - float2(thumbX - thumbR * 0.35, center.y - thumbR * 0.35)));",
        "  float3 color = mix(float3(0.21, 0.22, 0.26), float3(0.10, 0.11, 0.14), outerMask * 0.38);",
        "  color = mix(color, float3(0.18, 0.19, 0.23), borderMask);",
        "  color = mix(color, ramp, innerMask);",
        "  color = mix(color, float3(0.05, 0.05, 0.07), thumbShadow * 0.18);",
        "  color = mix(color, float3(0.96, 0.97, 0.99), thumb);",
        "  color = mix(color, float3(0.54, 0.56, 0.64), ring * (1.0 - thumb));",
        "  color += float3(0.04, 0.04, 0.05) * highlight * thumb;",
        "  float alpha = max(outerMask, thumbShadow);",
        "  return half4(half3(color) * half(alpha), half(alpha));",
        "}"
    ].join("\n");
}

function applyPaletteSliderShaders(paletteIdx, hue) {
    setWidgetShader("ramp-" + paletteIdx + "-h-fdr", buildPaletteHueSliderShader());
    setWidgetShader("ramp-" + paletteIdx + "-c-fdr", buildPaletteChromaSliderShader(hue));
}

function quantizePalettePreviewHue(hue) {
    return Math.round(hue / 12) * 12;
}

function quantizePaletteShaderHue(hue) {
    return Math.round(hue / 18) * 18;
}

function updatePaletteShadeWidgets(paletteIdx, ramp) {
    var steps = ShadeGenerator.STEPS;
    for (var s = 0; s < steps.length; s++) {
        setBackground("ramp-" + paletteIdx + "-s" + s, ramp[steps[s]].hex);
    }
    setBackground("ramp-" + paletteIdx + "-dot", ramp[500].hex);

    var stepIdxs = [0, 2, 4, 5, 7, 9];
    for (var ls = 0; ls < 6; ls++) {
        setBackground("ramp-" + paletteIdx + "-lg-" + ls, ramp[steps[stepIdxs[ls]]].hex);
    }
}

function getPaletteLiveRamp(paletteIdx, mappedL, mappedC, hue, draggingPreview) {
    var key = [
        paletteIdx,
        draggingPreview ? mappedL.toFixed(2) : mappedL.toFixed(3),
        draggingPreview ? mappedC.toFixed(3) : mappedC.toFixed(4),
        draggingPreview ? quantizePalettePreviewHue(hue) : hue.toFixed(1)
    ].join("|");
    var cached = paletteLiveRampCache[key];
    if (cached) return cached;
    cached = ShadeGenerator.generateRamp(mappedL, mappedC, draggingPreview ? quantizePalettePreviewHue(hue) : hue);
    paletteLiveRampCache[key] = cached;
    return cached;
}

function flushPaletteThemeApply(paletteIdx, forceApply) {
    paletteApplyFrames[paletteIdx] = 0;
    var state = paletteApplyStates[paletteIdx];
    if (!state) return;
    paletteThemeApplyTimes[paletteIdx] = performance.now();
    var dragging = !!paletteSliderDragActive[paletteIdx];
    var liveRamp = getPaletteLiveRamp(paletteIdx, state.mappedL, state.mappedC, state.h, dragging && !forceApply);
    var liveAccentHex = liveRamp[500].hex;

    if (state.pKey === "accent") {
        accentHuePendingHue = state.h;
        if (!dragging || forceApply) {
            currentAccent = liveAccentHex;
        }
        if (forceApply) {
            setValue("accent-hue", state.h / 360);
        }
    }

    if (dragging && !forceApply) {
        if (paletteLivePreviewRefs[paletteIdx] !== liveRamp) {
            paletteLivePreviewRefs[paletteIdx] = liveRamp;
            updatePaletteShadeWidgets(paletteIdx, liveRamp);
            if (expandedPalette === paletteIdx) {
                updatePaletteValueDisplay(paletteIdx,
                    { L: state.mappedL, C: state.mappedC, H: state.h },
                    liveRamp[500].hex);
            }
        }
        return;
    }

    paletteLivePreviewRefs[paletteIdx] = liveRamp;

    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    if (state.pKey !== "accent") {
        palette[state.pKey] = liveRamp;
    }

    applyPaletteThemeDiff(palette, currentPaletteMode);
    updateTokenSwatches();
    if (forceApply) markCurrentModeDirty();
    if (state.pKey === "accent") {
        refreshPaletteMiniRamps(palette);
    } else {
        updatePaletteShadeWidgets(paletteIdx, palette[state.pKey]);
    }
}

function schedulePaletteThemeApply(paletteIdx, pKey, h, mappedL, mappedC, forceApply) {
    paletteApplyStates[paletteIdx] = { pKey: pKey, h: h, mappedL: mappedL, mappedC: mappedC };
    if (forceApply) {
        if (paletteApplyFrames[paletteIdx]) {
            cancelAnimationFrame(paletteApplyFrames[paletteIdx]);
            paletteApplyFrames[paletteIdx] = 0;
        }
        flushPaletteThemeApply(paletteIdx, true);
        return;
    }
    if (paletteApplyFrames[paletteIdx]) return;
    paletteApplyFrames[paletteIdx] = requestAnimationFrame(function() {
        flushPaletteThemeApply(paletteIdx, false);
    });
}

function getPaletteValueFields(format, oklch, hex) {
    var rgb = hexToRgbParts(hex);
    if (format === "RGB") {
        return {
            labels: ["R", "G", "B"],
            values: [String(rgb.r), String(rgb.g), String(rgb.b)]
        };
    }
    if (format === "HEX") {
        return {
            labels: ["R", "G", "B"],
            values: [
                hex.slice(1, 3).toUpperCase(),
                hex.slice(3, 5).toUpperCase(),
                hex.slice(5, 7).toUpperCase()
            ]
        };
    }
    return {
        labels: ["L", "C", "H"],
        values: [
            (oklch.L * 100).toFixed(1),
            oklch.C.toFixed(3),
            oklch.H.toFixed(1)
        ]
    };
}

function updatePaletteValueDisplay(paletteIdx, oklch, hex) {
    var display = getPaletteValueFields(paletteValueFormats[paletteIdx] || "OKLCH", oklch, hex);
    for (var i = 0; i < 3; i++) {
        setText("ramp-" + paletteIdx + "-value-" + i + "-key", display.labels[i]);
        setText("ramp-" + paletteIdx + "-value-" + i + "-text", display.values[i]);
    }
}

// Color system: palette rows with expandable gamut editor
// Clicking a palette row toggles the expanded editor (gamut triangle + sliders + shades)
var expandedPalette = -1;  // -1 = all collapsed, click a row to expand

function buildShadeRamps() {
    paletteLiveRampCache = {};
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    var steps = ShadeGenerator.STEPS;

    for (var p = 0; p < paletteNames.length; p++) {
        var rampId = "ramp-" + p;
        removeWidget(rampId);

        // Container for palette row + editor
        createCol(rampId, "color-section");
        setFlex(rampId, "gap", 4);
        setFlex(rampId, "height", PALETTE_COLLAPSED_HEIGHT);
        setFlex(rampId, "flex_shrink", 0);

        // Palette row: dot + name + shade ramp (clickable to expand)
        var rowId = rampId + "-header";
        createRow(rowId, rampId);
        setFlex(rowId, "height", 24);
        setFlex(rowId, "flex_shrink", 0);
        setFlex(rowId, "gap", 6);
        setFlex(rowId, "align_items", "center");
        registerClick(rowId);

        var ramp = palette[paletteKeys[p]];

        // Base color dot (clickable to expand)
        var dotId = rampId + "-dot";
        createCol(dotId, rowId);
        setFlex(dotId, "width", 14);
        setFlex(dotId, "height", 14);
        setFlex(dotId, "min_width", 14);
        setFlex(dotId, "min_height", 14);
        setFlex(dotId, "max_width", 14);
        setFlex(dotId, "max_height", 14);
        setFlex(dotId, "flex_grow", 0);
        setFlex(dotId, "flex_shrink", 0);
        setBackground(dotId, ramp[500].hex);
        setBorder(dotId, ramp[500].hex, 0, 7);
        registerClick(dotId);

        // Name (clickable to expand)
        createLabel(rampId + "-name", paletteNames[p], rowId);
        setFontSize(rampId + "-name", 10);
        setTextColor(rampId + "-name", APP_TEXT_DIM);
        setFlex(rampId + "-name", "min_width", 64);
        setFlex(rampId + "-name", "flex_grow", 1);
        setTextOverflow(rampId + "-name", "ellipsis");
        registerClick(rampId + "-name");

        // Mini ramp
        createRow(rampId + "-row", rowId);
        setFlex(rampId + "-row", "width", 98);
        setFlex(rampId + "-row", "flex_shrink", 0);
        setFlex(rampId + "-row", "gap", 1);
        setFlex(rampId + "-row", "height", 12);

        for (var s = 0; s < steps.length; s++) {
            var shadeId = rampId + "-s" + s;
            createCol(shadeId, rampId + "-row");
            setFlex(shadeId, "width", 8);
            setFlex(shadeId, "height", 12);
            setBackground(shadeId, ramp[steps[s]].hex);
            setBorder(shadeId, APP_BORDER, 0, 1);
            // Click shade → expand this palette with that shade's color
            registerClick(shadeId);
        }

        // Expanded editor section (hidden unless this palette is expanded)
        var editorId = rampId + "-editor";
        createCol(editorId, rampId);
        setFlex(editorId, "gap", 4);
        setFlex(editorId, "padding_left", 4);
        setFlex(editorId, "padding_top", 2);
        setFlex(editorId, "padding_bottom", 2);
        setFlex(editorId, "flex_shrink", 0);
        applyPaletteExpandedLayout(p, p === expandedPalette);

        // Gamut triangle canvas (draggable for dot positioning)
        var gamutWrapId = rampId + "-gamut-wrap";
        createCol(gamutWrapId, editorId);
        setPosition(gamutWrapId, "relative");
        setFlex(gamutWrapId, "width", 270);
        setFlex(gamutWrapId, "height", 130);
        setFlex(gamutWrapId, "flex_shrink", 0);

        var gamutId = rampId + "-gamut";
        createCanvas(gamutId, gamutWrapId);
        setFlex(gamutId, "width", 270);
        setFlex(gamutId, "height", 130);
        setBackground(gamutId, APP_SURFACE);
        setPointerEvents(gamutId, "none");

        var gamutOverlayId = rampId + "-gamut-overlay";
        createCanvas(gamutOverlayId, gamutWrapId);
        setPosition(gamutOverlayId, "absolute");
        setTop(gamutOverlayId, 0);
        setLeft(gamutOverlayId, 0);
        setFlex(gamutOverlayId, "width", 270);
        setFlex(gamutOverlayId, "height", 130);
        registerPointer(gamutOverlayId);
        (function(idx, pKey) {
            // Handle both pointerdown and pointermove (drag) on gamut
            var dragCount = 0;
            var dragging = false;
            function onGamutPointer(evt) {
                if (evt.type === "pointermove" && !dragging) return;
                var gw = 270, gh = 130;
                var x = evt.offsetX, y = evt.offsetY;
                var L = Math.max(0, Math.min(1, x / gw));
                var C = Math.max(0, Math.min(0.4, (1 - y / gh) * 0.4));
                var h = getValue("ramp-" + idx + "-h-fdr") * 360;
                var mapped = OklchEngine.gamutMap(L, C, h);
                setValue("ramp-" + idx + "-c-fdr", Math.min(mapped.C / 0.4, 1));
                renderPaletteGamut(idx, h, mapped.L, mapped.C, evt.type === 'pointerdown');
                updatePaletteValueDisplay(idx, mapped, OklchEngine.oklchToHex(mapped.L, mapped.C, h));
                // Throttle palette rebuild during drag (every 3rd event)
                dragCount++;
                if (dragCount % 3 === 0 || evt.type === 'pointerdown') {
                    if (pKey === "accent") currentAccent = OklchEngine.oklchToHex(mapped.L, mapped.C, h);
                    var palette = PaletteSystem.create(currentAccent, currentHarmony);
                    if (pKey !== "accent") palette[pKey] = ShadeGenerator.generateRamp(mapped.L, mapped.C, h);
                    applyTokenDiff(PaletteSystem.toThemeDiff(palette));
                    updateTokenSwatches();
                    var rampData = palette[pKey]; var stepsArr = ShadeGenerator.STEPS;
                    for (var ss = 0; ss < stepsArr.length; ss++) setBackground("ramp-" + idx + "-s" + ss, rampData[stepsArr[ss]].hex);
                    setBackground("ramp-" + idx + "-dot", rampData[500].hex);
                    var si2 = [0,2,4,5,7,9];
                    for (var ls = 0; ls < 6; ls++) setBackground("ramp-" + idx + "-lg-" + ls, rampData[stepsArr[si2[ls]]].hex);
                }
            }
            on(gamutOverlayId, "pointerdown", function(e) {
                dragging = true;
                dragCount = 0;
                nativeSetPointerCapture(gamutOverlayId, e && e.pointerId ? e.pointerId : 0);
                onGamutPointer(e);
            });
            on(gamutOverlayId, "pointermove", onGamutPointer);
            on(gamutOverlayId, "pointerup", function(e) {
                dragging = false;
                nativeReleasePointerCapture(gamutOverlayId, e && e.pointerId ? e.pointerId : 0);
            });
            on(gamutOverlayId, "pointercancel", function(e) {
                dragging = false;
                nativeReleasePointerCapture(gamutOverlayId, e && e.pointerId ? e.pointerId : 0);
            });
        })(p, paletteKeys[p]);

        // H — hue slider with inline gradient track
        var hRowId = rampId + "-h-row";
        createRow(hRowId, editorId);
        setFlex(hRowId, "height", 20);
        setFlex(hRowId, "gap", 4);
        setFlex(hRowId, "align_items", "center");
        createLabel(rampId + "-h-lbl", "H", hRowId);
        setFontSize(rampId + "-h-lbl", 9);
        setFlex(rampId + "-h-lbl", "width", 12);
        createFader(rampId + "-h-fdr", "horizontal", hRowId);
        setFlex(rampId + "-h-fdr", "flex_grow", 1);
        setFlex(rampId + "-h-fdr", "height", 18);
        setWidgetShader(rampId + "-h-fdr", buildPaletteHueSliderShader());

        // C — chroma slider with inline gradient track
        var cRowId = rampId + "-c-row";
        createRow(cRowId, editorId);
        setFlex(cRowId, "height", 20);
        setFlex(cRowId, "gap", 4);
        setFlex(cRowId, "align_items", "center");
        createLabel(rampId + "-c-lbl", "C", cRowId);
        setFontSize(rampId + "-c-lbl", 9);
        setFlex(rampId + "-c-lbl", "width", 12);
        createFader(rampId + "-c-fdr", "horizontal", cRowId);
        setFlex(rampId + "-c-fdr", "flex_grow", 1);
        setFlex(rampId + "-c-fdr", "height", 18);
        setWidgetShader(rampId + "-c-fdr", buildPaletteChromaSliderShader(OklchEngine.hexToOklch(ramp[500].hex).H));

        createCol(rampId + "-contrast-btn", cRowId);
        setFlex(rampId + "-contrast-btn", "width", 28);
        setFlex(rampId + "-contrast-btn", "height", 18);
        setFlex(rampId + "-contrast-btn", "justify_content", "center");
        setFlex(rampId + "-contrast-btn", "align_items", "center");
        setBackground(rampId + "-contrast-btn", APP_PANEL_RAISED);
        setBorder(rampId + "-contrast-btn", APP_BORDER, 1, 8);
        createLabel(rampId + "-contrast-lbl", "Aa", rampId + "-contrast-btn");
        setFontSize(rampId + "-contrast-lbl", 10);
        setTextColor(rampId + "-contrast-lbl", APP_TEXT_DIM);
        setPointerEvents(rampId + "-contrast-lbl", "none");
        registerClick(rampId + "-contrast-btn");
        (function(idx, pKey) {
            on("ramp-" + idx + "-contrast-btn", "click", function() {
                var palette = PaletteSystem.create(currentAccent, currentHarmony);
                var base = palette[pKey][500];
                showContrastModal(paletteNames[idx], base.hex);
            });
        })(p, paletteKeys[p]);

        // Color value format selector + value fields
        var valuesRowId = rampId + "-values";
        createRow(valuesRowId, editorId);
        setFlex(valuesRowId, "height", 34);
        setFlex(valuesRowId, "gap", 8);
        setFlex(valuesRowId, "align_items", "flex_end");

        var formatId = rampId + "-format";
        createCombo(formatId, valuesRowId);
        setItems(formatId, ["OKLCH", "HEX", "RGB"]);
        setSelected(formatId, 0);
        setFlex(formatId, "width", 72);
        setFlex(formatId, "height", 26);
        setFlex(formatId, "flex_shrink", 0);

        for (var vf = 0; vf < 3; vf++) {
            var fieldId = rampId + "-value-" + vf;
            createCol(fieldId, valuesRowId);
            setFlex(fieldId, "width", 56);
            setFlex(fieldId, "height", 32);
            setFlex(fieldId, "gap", 2);
            setFlex(fieldId, "flex_shrink", 0);

            createLabel(fieldId + "-key", vf === 0 ? "L" : (vf === 1 ? "C" : "H"), fieldId);
            setFontSize(fieldId + "-key", 8);
            setTextColor(fieldId + "-key", APP_TEXT_DIM);
            setFlex(fieldId + "-key", "height", 10);

            createCol(fieldId + "-box", fieldId);
            setFlex(fieldId + "-box", "height", 20);
            setFlex(fieldId + "-box", "justify_content", "center");
            setFlex(fieldId + "-box", "align_items", "center");
            setBackground(fieldId + "-box", APP_PANEL_RAISED);
            setBorder(fieldId + "-box", APP_BORDER, 1, 6);
            createLabel(fieldId + "-text", vf === 0 ? "50.0" : (vf === 1 ? "0.100" : "180.0"), fieldId + "-box");
            setFontSize(fieldId + "-text", 10);
        }

        (function(idx) {
            on("ramp-" + idx + "-format", "select", function(selIdx) {
                paletteValueFormats[idx] = selIdx === 1 ? "HEX" : (selIdx === 2 ? "RGB" : "OKLCH");
                var palette = PaletteSystem.create(currentAccent, currentHarmony);
                var base = palette[paletteKeys[idx]][500];
                updatePaletteValueDisplay(idx, OklchEngine.hexToOklch(base.hex), base.hex);
            });
        })(p);

        // Compact shade swatches with WCAG badges
        createRow(rampId + "-shades", editorId);
        setFlex(rampId + "-shades", "gap", 4);
        setFlex(rampId + "-shades", "height", 34);
        setFlex(rampId + "-shades", "flex_shrink", 0);
        for (var ls = 0; ls < 6; ls++) {
            var lsId = rampId + "-lg-" + ls;
            var stepIdx = [0, 2, 4, 5, 7, 9][ls];
            var shadeHex = ramp[steps[stepIdx]].hex;
            createCol(lsId, rampId + "-shades");
            setFlex(lsId, "width", 40);
            setFlex(lsId, "min_width", 40);
            setFlex(lsId, "max_width", 40);
            setFlex(lsId, "height", 30);
            setFlex(lsId, "flex_grow", 0);
            setFlex(lsId, "flex_shrink", 0);
            setBackground(lsId, shadeHex);
            setBorder(lsId, APP_BORDER, 0, 10);
            setFlex(lsId, "justify_content", "flex_end");
            setFlex(lsId, "align_items", "center");
            setFlex(lsId, "padding_bottom", 4);
            var ratio = OklchEngine.contrastRatio(shadeHex, "#ffffff");
            var badgeText = ratio.toFixed(1) + ":1";
            createCol(lsId + "-pill", lsId);
            setFlex(lsId + "-pill", "height", 14);
            setFlex(lsId + "-pill", "padding_left", 5);
            setFlex(lsId + "-pill", "padding_right", 5);
            setFlex(lsId + "-pill", "justify_content", "center");
            setFlex(lsId + "-pill", "align_items", "center");
            setBackground(lsId + "-pill", ratio > 4.5 ? "#00000038" : "#ffffff88");
            setBorder(lsId + "-pill", ratio > 4.5 ? "#ffffff18" : "#00000014", 1, 7);
            createLabel(lsId + "-badge", badgeText, lsId + "-pill");
            setFontSize(lsId + "-badge", 7);
            setTextColor(lsId + "-badge", ratio > 4.5 ? "#ffffff" : "#111111");
        }

        // Draw gamut + set faders if this palette is expanded
        if (p === expandedPalette) {
            var base = ramp[500];
            var oklch = OklchEngine.hexToOklch(base.hex);
            renderPaletteGamut(p, oklch.H, oklch.L, oklch.C);
            applyPaletteSliderShaders(p, oklch.H);
            paletteShaderHueBuckets[p] = quantizePaletteShaderHue(oklch.H);
            setValue(rampId + "-h-fdr", oklch.H / 360);
            setValue(rampId + "-c-fdr", Math.min(oklch.C / 0.4, 1));
            updatePaletteValueDisplay(p, oklch, base.hex);
        }

        // Click dot or name → toggle expand (just visibility, no rebuild)
        (function(idx) {
            var toggleExpand = function() {
                if (expandedPalette === idx) {
                    // Collapse — shrink container back
                    applyPaletteExpandedLayout(idx, false);
                    expandedPalette = -1;
                } else {
                    // Collapse previous
                    if (expandedPalette >= 0) {
                        applyPaletteExpandedLayout(expandedPalette, false);
                    }
                    // Expand — grow container for editor content
                    expandedPalette = idx;
                    applyPaletteExpandedLayout(idx, true);
                    // Render gamut triangle for this palette
                    var pal = PaletteSystem.create(currentAccent, currentHarmony);
                    var pKey = paletteKeys[idx];
                    var base = pal[pKey][500];
                    var oklch = OklchEngine.hexToOklch(base.hex);
                    renderPaletteGamut(idx, oklch.H, oklch.L, oklch.C);
                    applyPaletteSliderShaders(idx, oklch.H);
                    paletteShaderHueBuckets[idx] = quantizePaletteShaderHue(oklch.H);
                    setValue("ramp-" + idx + "-h-fdr", oklch.H / 360);
                    setValue("ramp-" + idx + "-c-fdr", Math.min(oklch.C / 0.4, 1));
                    updatePaletteValueDisplay(idx, oklch, base.hex);
                }
                updateLeftPanelScrollMetrics();
                layout();
            };
            on(rowId, "click", toggleExpand);
            on("ramp-" + idx + "-dot", "click", toggleExpand);
            on("ramp-" + idx + "-name", "click", toggleExpand);
            // Shade clicks also expand + position dot at that shade's color
            for (var si = 0; si < ShadeGenerator.STEPS.length; si++) {
                (function(paletteIdx, shadeStep) {
                    on("ramp-" + paletteIdx + "-s" + shadeStep, "click", function() {
                        var pal = PaletteSystem.create(currentAccent, currentHarmony);
                        var shade = pal[paletteKeys[paletteIdx]][ShadeGenerator.STEPS[shadeStep]];
                        var oklch = OklchEngine.hexToOklch(shade.hex);
                        // Expand if not already
                        if (expandedPalette >= 0 && expandedPalette !== paletteIdx) {
                            applyPaletteExpandedLayout(expandedPalette, false);
                        }
                        expandedPalette = paletteIdx;
                        applyPaletteExpandedLayout(paletteIdx, true);
                        renderPaletteGamut(paletteIdx, oklch.H, oklch.L, oklch.C);
                        applyPaletteSliderShaders(paletteIdx, oklch.H);
                        paletteShaderHueBuckets[paletteIdx] = quantizePaletteShaderHue(oklch.H);
                        setValue("ramp-" + paletteIdx + "-h-fdr", oklch.H / 360);
                        setValue("ramp-" + paletteIdx + "-c-fdr", Math.min(oklch.C / 0.4, 1));
                        updatePaletteValueDisplay(paletteIdx, oklch, shade.hex);
                        updateLeftPanelScrollMetrics();
                        layout();
                    });
                })(idx, si);
            }
        })(p);

        // H/C slider change handlers — update gamut, dot, accent, and preview
        (function(idx, pKey) {
            function onPaletteSliderChange(hueChanged, forceApply) {
                var h = getValue("ramp-" + idx + "-h-fdr") * 360;
                var c = getValue("ramp-" + idx + "-c-fdr") * 0.4;
                var mapped = OklchEngine.gamutMap(0.55, c, h);
                var dragging = !!paletteSliderDragActive[idx];
                var now = performance.now();
                var previewHue = hueChanged ? quantizePalettePreviewHue(h) : h;
                if (dragging && !forceApply) {
                    renderPaletteGamutOverlay(idx, h, mapped.L, mapped.C);
                } else {
                    palettePreviewUpdateTimes[idx] = now;
                    renderPaletteGamut(idx, previewHue, mapped.L, mapped.C, false);
                }
                if (hueChanged) {
                    var shaderHue = quantizePaletteShaderHue(h);
                    var lastShader = paletteShaderUpdateTimes[idx] || 0;
                    if (paletteShaderHueBuckets[idx] !== shaderHue && (!dragging || forceApply || (now - lastShader) >= 160)) {
                        paletteShaderHueBuckets[idx] = shaderHue;
                        paletteShaderUpdateTimes[idx] = now;
                        applyPaletteSliderShaders(idx, shaderHue);
                    }
                }
                if (!dragging || forceApply) {
                    updatePaletteValueDisplay(idx, mapped, OklchEngine.oklchToHex(mapped.L, mapped.C, h));
                }
                schedulePaletteThemeApply(idx, pKey, h, mapped.L, mapped.C, !!forceApply);
            }
            function beginSliderDrag() {
                paletteSliderDragActive[idx] = true;
            }
            function endSliderDrag(hueChanged) {
                if (!paletteSliderDragActive[idx]) return;
                paletteSliderDragActive[idx] = false;
                onPaletteSliderChange(hueChanged, true);
            }
            on("ramp-" + idx + "-h-fdr", "pointerdown", beginSliderDrag);
            on("ramp-" + idx + "-h-fdr", "pointerup", function() { endSliderDrag(true); });
            on("ramp-" + idx + "-h-fdr", "pointercancel", function() { endSliderDrag(true); });
            on("ramp-" + idx + "-c-fdr", "pointerdown", beginSliderDrag);
            on("ramp-" + idx + "-c-fdr", "pointerup", function() { endSliderDrag(false); });
            on("ramp-" + idx + "-c-fdr", "pointercancel", function() { endSliderDrag(false); });
            on("ramp-" + idx + "-h-fdr", "change", function() { onPaletteSliderChange(true, false); });
            on("ramp-" + idx + "-c-fdr", "change", function() { onPaletteSliderChange(false, false); });
        })(p, paletteKeys[p]);
    }
    updateColorSectionLayout();
    updateLeftPanelScrollMetrics();
    if (tokenEditState.activeToken) rebuildPopupPalette();
}

// Render gamut triangle for a specific palette editor
// Render chroma gradient for a palette editor (gray → saturated at current hue)
function renderChromaGradient(paletteIdx, hue) {
    var cGradId = "ramp-" + paletteIdx + "-c-grad";
    canvasClear(cGradId);
    var w = 260;
    var steps = 32;
    for (var cs = 0; cs < steps; cs++) {
        var c = (cs / steps) * 0.4;
        var hex = OklchEngine.oklchToHex(0.6, c, hue);
        canvasRect(cGradId, (cs / steps) * w, 0, (w / steps) + 1, 12, hex);
    }
}

// fullRedraw=true renders the color grid; false only redraws dot overlay
var gamutCache = {};  // cache hue → avoid full redraw during drag
var gamutBoundaryCache = {};

function computeGamutBoundary(hue, steps) {
    var boundary = [];
    for (var bx = 0; bx <= steps; bx++) {
        var L = bx / steps;
        var lo = 0, hi = 0.4;
        for (var bi = 0; bi < 16; bi++) {
            var mid = (lo + hi) / 2;
            if (OklchEngine.isInGamut(L, mid, hue)) lo = mid; else hi = mid;
        }
        boundary.push(lo);
    }
    return boundary;
}

function renderPaletteGamutOverlay(paletteIdx, hue, dotL, dotC) {
    var overlayId = "ramp-" + paletteIdx + "-gamut-overlay";
    var w = 270, h = 130;
    canvasClear(overlayId);
    if (dotL === undefined || dotC === undefined) return;
    var dx = dotL * w;
    var dy = (1 - dotC / 0.4) * h;
    canvasStrokeLine(overlayId, dx, 0, dx, h, '#ffffff15', 0.5);
    canvasStrokeLine(overlayId, 0, dy, w, dy, '#ffffff15', 0.5);
    canvasFillCircle(overlayId, dx, dy, 9, '#00000040');
    canvasFillCircle(overlayId, dx, dy, 7, '#ffffffdd');
    canvasFillCircle(overlayId, dx, dy, 4, OklchEngine.oklchToHex(dotL, dotC, hue));
}

function renderPaletteGamut(paletteIdx, hue, dotL, dotC, fullRedraw) {
    var gamutId = "ramp-" + paletteIdx + "-gamut";
    var w = 270, h = 130;

    // Full redraw: render the entire color grid (expensive)
    if (fullRedraw !== false || gamutCache[paletteIdx] !== hue) {
        gamutCache[paletteIdx] = hue;
        canvasClear(gamutId);
        canvasRect(gamutId, 0, 0, w, h, APP_SURFACE);

        var bSteps = 720;
        var boundary = computeGamutBoundary(hue, bSteps);
        gamutBoundaryCache[paletteIdx] = boundary;

        // Render the gamut with per-column gradients and the exact boundary
        // height. This removes the stepped/pixelated top edge from the old
        // cell-grid renderer and tracks the HTML reference more closely.
        var cols = 360;
        var colW = w / cols;
        for (var gx = 0; gx < cols; gx++) {
            var L = gx / (cols - 1);
            var boundaryIdx = Math.min(bSteps, Math.max(0, Math.round((gx / (cols - 1)) * bSteps)));
            var maxC = boundary[boundaryIdx];
            var topY = (1 - maxC / 0.4) * h;
            if (topY > 0.35) {
                canvasRect(gamutId, gx * colW, 0, colW + 0.55, topY + 0.35, APP_SURFACE);
            }
            if (maxC > 0.001) {
                var topHex = OklchEngine.oklchToHex(L, Math.max(0.015, maxC * 0.96), hue);
                var bottomHex = OklchEngine.oklchToHex(L, 0.0001, hue);
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
        canvasSetLineWidth(gamutId, 2.4);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, (1 - boundary[0] / 0.4) * h);
        for (var bx2 = 1; bx2 <= bSteps; bx2++) {
            canvasLineTo(gamutId, (bx2 / bSteps) * w, (1 - boundary[bx2] / 0.4) * h);
        }
        canvasStrokePath(gamutId);

        canvasSetStrokeColor(gamutId, '#ffffff33');
        canvasSetLineWidth(gamutId, 1.05);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, (1 - boundary[0] / 0.4) * h);
        for (var bx2a = 1; bx2a <= bSteps; bx2a++) {
            canvasLineTo(gamutId, (bx2a / bSteps) * w, (1 - boundary[bx2a] / 0.4) * h);
        }
        canvasStrokePath(gamutId);

        canvasSetStrokeColor(gamutId, '#ffffff10');
        canvasSetLineWidth(gamutId, 0.8);
        canvasBeginPath(gamutId);
        canvasMoveTo(gamutId, 0, h);
        canvasLineTo(gamutId, 0, (1 - boundary[0] / 0.4) * h);
        for (var bx3 = 1; bx3 <= bSteps; bx3++) {
            canvasLineTo(gamutId, (bx3 / bSteps) * w, (1 - boundary[bx3] / 0.4) * h);
        }
        canvasLineTo(gamutId, w, h);
        canvasStrokePath(gamutId);
    }

    renderPaletteGamutOverlay(paletteIdx, hue, dotL, dotC);
}

buildShadeRamps();

function refreshPaletteMiniRamps(palette) {
    var steps = ShadeGenerator.STEPS;
    for (var p = 0; p < paletteKeys.length; p++) {
        var ramp = palette[paletteKeys[p]];
        for (var s = 0; s < steps.length; s++) {
            setBackground("ramp-" + p + "-s" + s, ramp[steps[s]].hex);
        }
        setBackground("ramp-" + p + "-dot", ramp[500].hex);
    }
}

function harmonySelectorIndex(mode) {
    var modes = ["monochromatic", "analogous", "complementary", "splitComplementary", "none"];
    for (var i = 0; i < modes.length; i++) {
        if (modes[i] === mode) return i;
    }
    return 0;
}

function resolvePaletteMode(mode) {
    return mode === "light" ? "light" : "dark";
}

function syncPaletteMode(mode) {
    var resolved = resolvePaletteMode(mode);
    currentPaletteMode = resolved;
    setSelected("mode-selector", resolved === "light" ? 1 : 0);
    return resolved;
}

function applyPaletteThemeDiff(palette, mode) {
    applyTokenDiff(PaletteSystem.toThemeDiff(palette, resolvePaletteMode(mode)));
}

function refreshPaletteEditorsFromPalette(palette, fullRedraw, collapseExpanded) {
    refreshPaletteMiniRamps(palette);
    refreshExpandedPaletteEditorFromPalette(palette, !!fullRedraw);
    if (collapseExpanded && expandedPalette >= 0) {
        applyPaletteExpandedLayout(expandedPalette, false);
        expandedPalette = -1;
        updateLeftPanelScrollMetrics();
    }
}

function applyPaletteConfiguration(config) {
    currentAccent = config.accent || currentAccent;
    currentHarmony = config.harmony || currentHarmony;
    setSelected("harmony-selector", harmonySelectorIndex(currentHarmony));
    var mode = syncPaletteMode(resolvePaletteMode(config.theme || currentPaletteMode));
    setTheme(config.theme || mode);
    if (config.templateIndex !== undefined) setSelected("template-selector", config.templateIndex);
    if (config.presetIndex !== undefined) setSelected("preset-selector", config.presetIndex);
    if (config.title) setText("theme-name-label", config.title);

    var accentOklch = OklchEngine.hexToOklch(currentAccent);
    accentHuePendingHue = accentOklch.H;
    setValue("accent-hue", accentOklch.H / 360);

    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    applyPaletteThemeDiff(palette, mode);
    updateTokenSwatches();
    refreshPaletteEditorsFromPalette(palette, true, true);
    markCurrentModeDirty();
    if (config.snapshot !== false) pushThemeSnapshot();
    layout();
}

function buildAccentPreviewPalette(accentL, accentC, accentH, harmonyMode) {
    var neutral = ColorHarmony.deriveNeutral(accentH, harmonyMode || "monochromatic");
    return {
        accent: ShadeGenerator.generateRamp(accentL, accentC, accentH),
        neutral: ShadeGenerator.generateRamp(neutral.L, neutral.C, neutral.H)
    };
}

function getAccentHuePreviewData(accentL, accentC, accentH) {
    var previewHue = quantizePalettePreviewHue(accentH);
    var cacheKey = [
        currentHarmony,
        accentL.toFixed(4),
        accentC.toFixed(4),
        previewHue
    ].join("|");
    var cached = accentHuePreviewCache[cacheKey];
    if (cached) return cached;

    var neutral = ColorHarmony.deriveNeutral(previewHue, currentHarmony);
    cached = {
        accent: { L: accentL, C: accentC, H: previewHue },
        accentRamp: ShadeGenerator.generateRamp(accentL, accentC, previewHue),
        neutral: neutral,
        neutralRamp: ShadeGenerator.generateRamp(neutral.L, neutral.C, neutral.H)
    };
    accentHuePreviewCache[cacheKey] = cached;
    return cached;
}

function refreshAccentHuePreview(accentL, accentC, accentH) {
    var previewData = getAccentHuePreviewData(accentL, accentC, accentH);
    updatePaletteShadeWidgets(0, previewData.accentRamp);
    updatePaletteShadeWidgets(1, previewData.neutralRamp);

    if (!accentHueDragActive && expandedPalette === 1) {
        renderPaletteGamutOverlay(1, previewData.neutral.H, previewData.neutral.L, previewData.neutral.C);
        updatePaletteValueDisplay(1, previewData.neutral, previewData.neutralRamp[500].hex);
        setValue("ramp-1-h-fdr", previewData.neutral.H / 360);
        setValue("ramp-1-c-fdr", Math.min(previewData.neutral.C / 0.4, 1));
    }

    if (expandedPalette === 0) {
        renderPaletteGamutOverlay(0, previewData.accent.H, previewData.accent.L, previewData.accent.C);
        updatePaletteValueDisplay(0, previewData.accent, previewData.accentRamp[500].hex);
        if (!accentHueDragActive) {
            setValue("ramp-0-h-fdr", previewData.accent.H / 360);
            setValue("ramp-0-c-fdr", Math.min(previewData.accent.C / 0.4, 1));
        }
    }
}

function refreshExpandedPaletteEditorFromPalette(palette, fullRedraw) {
    if (expandedPalette < 0) return;
    var pKey = paletteKeys[expandedPalette];
    var base = palette[pKey][500];
    var oklch = OklchEngine.hexToOklch(base.hex);
    if (accentHueDragActive && !fullRedraw) {
        renderPaletteGamutOverlay(expandedPalette, oklch.H, oklch.L, oklch.C);
    } else {
        renderPaletteGamut(expandedPalette, oklch.H, oklch.L, oklch.C, fullRedraw);
    }
    var shaderHue = quantizePaletteShaderHue(oklch.H);
    if (!accentHueDragActive || fullRedraw || paletteShaderHueBuckets[expandedPalette] !== shaderHue) {
        applyPaletteSliderShaders(expandedPalette, shaderHue);
        paletteShaderHueBuckets[expandedPalette] = shaderHue;
    }
    setValue("ramp-" + expandedPalette + "-h-fdr", oklch.H / 360);
    setValue("ramp-" + expandedPalette + "-c-fdr", Math.min(oklch.C / 0.4, 1));
    updatePaletteValueDisplay(expandedPalette, oklch, base.hex);
}

function flushAccentHueApply(forceApply) {
    accentHueApplyFrame = 0;
    accentHueLastApplyTime = performance.now();
    var nextAccentHex = OklchEngine.oklchToHex(accentHueBaseL, accentHueBaseC, accentHuePendingHue);
    if (!accentHueDragActive || forceApply) {
        currentAccent = nextAccentHex;
        setValue("accent-hue", accentHuePendingHue / 360);
    }
    if (accentHueDragActive && !forceApply) {
        var previewHue = quantizePalettePreviewHue(accentHuePendingHue);
        if (previewHue !== accentHuePreviewBucket) {
            accentHuePreviewBucket = previewHue;
            refreshAccentHuePreview(accentHueBaseL, accentHueBaseC, accentHuePendingHue);
        }
        return;
    }
    accentHuePreviewBucket = -1;
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    refreshPaletteEditorsFromPalette(palette, !!forceApply, false);
    applyPaletteThemeDiff(palette, currentPaletteMode);
    updateTokenSwatches();
    markCurrentModeDirty();
}

function scheduleAccentHueApply(forceApply) {
    if (forceApply) {
        if (accentHueApplyFrame) {
            cancelAnimationFrame(accentHueApplyFrame);
            accentHueApplyFrame = 0;
        }
        flushAccentHueApply(true);
        return;
    }
    if (accentHueApplyFrame) return;
    accentHueApplyFrame = requestAnimationFrame(function() {
        flushAccentHueApply(false);
    });
}

// "Generate Opposite Mode" button below palette rows
function buildOppositeModeButton() {
    createCol("gen-opposite-btn", "color-section");
    setFlex("gen-opposite-btn", "height", 32);
    setFlex("gen-opposite-btn", "justify_content", "center");
    setFlex("gen-opposite-btn", "align_items", "center");
    setBackground("gen-opposite-btn", APP_ACCENT);
    setBorder("gen-opposite-btn", APP_ACCENT, 0, 6);
    createLabel("gen-opposite-lbl", "Generate Opposite Mode", "gen-opposite-btn");
    setFontSize("gen-opposite-lbl", 11);
    setTextColor("gen-opposite-lbl", "#ffffff");
    setPointerEvents("gen-opposite-lbl", "none");
    registerClick("gen-opposite-btn");
    on("gen-opposite-btn", "click", function() {
        var sourceMode = resolvePaletteMode(currentPaletteMode);
        var targetMode = resolvePaletteMode(sourceMode === "light" ? "dark" : "light");
        switchPaletteMode(targetMode, {
            generatedTitle: targetMode === "light" ? "Generated Light" : "Generated Dark"
        });
        pushThemeSnapshot();
        refreshOppositeModeButton();
        layout();
        showToast("Switched to " + (targetMode === "light" ? "Light" : "Dark") + " mode");
    });
    refreshOppositeModeButton();
}
buildOppositeModeButton();

// #60: Save/Load palette buttons
function serializePaletteConfiguration() {
    return JSON.stringify({
        title: getText("theme-name-label"),
        accent: currentAccent,
        harmony: currentHarmony,
        mode: currentPaletteMode,
        palette: PaletteSystem.create(currentAccent, currentHarmony)
    });
}

function applySerializedPaletteConfiguration(json) {
    if (!json || json.length < 2) return false;
    try {
        var data = JSON.parse(json);
        applyPaletteConfiguration({
            title: data.title || undefined,
            accent: data.accent || currentAccent,
            harmony: data.harmony || currentHarmony,
            theme: data.mode || data.theme || currentPaletteMode,
            snapshot: false
        });
        return true;
    } catch (e) {
        showToast("Palette file invalid");
        return false;
    }
}

function normalizePaletteSavePath(path) {
    if (!path || path.length === 0) return "/tmp/pulp-palette.colorsystem.json";
    if (path.toLowerCase().slice(-5) === ".json") return path;
    return path + ".colorsystem.json";
}

function writeTextFile(path, content) {
    exec("cat > " + shellQuote(path) + " << 'PULPEOF'\n" + content + "\nPULPEOF");
}

function readTextFile(path) {
    return exec("cat " + shellQuote(path) + " 2>/dev/null");
}

function buildPaletteSaveLoadRow() {
    createRow("palette-io-row", "color-section");
    setFlex("palette-io-row", "height", 26);
    setFlex("palette-io-row", "gap", 6);

    createCol("palette-save-btn", "palette-io-row");
    setFlex("palette-save-btn", "flex_grow", 1);
    setFlex("palette-save-btn", "height", 26);
    setFlex("palette-save-btn", "justify_content", "center");
    setFlex("palette-save-btn", "align_items", "center");
    setBorder("palette-save-btn", APP_BORDER, 1, 4);
    createLabel("palette-save-lbl", "Save", "palette-save-btn");
    setFontSize("palette-save-lbl", 10);
    setPointerEvents("palette-save-lbl", "none");
    registerClick("palette-save-btn");
    on("palette-save-btn", "click", function() {
        var path = normalizePaletteSavePath(showSaveDialog("Save Palette", "Color System JSON", "json"));
        writeTextFile(path, serializePaletteConfiguration());
        showToast("Palette saved to " + path);
    });

    createCol("palette-load-btn", "palette-io-row");
    setFlex("palette-load-btn", "flex_grow", 1);
    setFlex("palette-load-btn", "height", 26);
    setFlex("palette-load-btn", "justify_content", "center");
    setFlex("palette-load-btn", "align_items", "center");
    setBorder("palette-load-btn", APP_BORDER, 1, 4);
    createLabel("palette-load-lbl", "Load", "palette-load-btn");
    setFontSize("palette-load-lbl", 10);
    setPointerEvents("palette-load-lbl", "none");
    registerClick("palette-load-btn");
    on("palette-load-btn", "click", function() {
        var path = showOpenDialog("Load Palette", "Color System JSON", "json");
        if (!path || path.length === 0) path = "/tmp/pulp-palette.colorsystem.json";
        var json = readTextFile(path);
        if (json && json.length > 10) {
            if (applySerializedPaletteConfiguration(json)) {
                showToast("Palette loaded from " + path);
            }
        } else {
            showToast("No palette file found");
        }
    });

    updateTokenFilterLayout("");
    updateLeftPanelScrollMetrics();
}
buildPaletteSaveLoadRow();

// ── Color Picker (legacy — hidden, replaced by inline palette editor)
var pickerVisible = false;
var pickerColor = { L: 0, C: 0, H: 0, hex: '#000000' };

function buildLegacyColorPicker() {
    createCol("color-picker", "");
    setFlex("color-picker", "width", 280);
    setFlex("color-picker", "height", 200);
    setFlex("color-picker", "padding", 12);
    setFlex("color-picker", "gap", 8);
    setBackground("color-picker", APP_PANEL);
    setBorder("color-picker", APP_BORDER, 1, 8);
    setBoxShadow("color-picker", 0, 4, 12, 0, "#00000060");
    setVisible("color-picker", false);

    // Color preview swatch
    createCol("picker-preview", "color-picker");
    setFlex("picker-preview", "height", 40);
    setBorder("picker-preview", APP_BORDER, 1, 6);

    // OKLCH values row
    createRow("picker-values", "color-picker");
    setFlex("picker-values", "height", 16);
    setFlex("picker-values", "gap", 12);
    setFlex("picker-values", "align_items", "center");

    createLabel("picker-l-label", "L: 0.00", "picker-values");
    setFontSize("picker-l-label", 10);
    setFlex("picker-l-label", "width", 60);

    createLabel("picker-c-label", "C: 0.000", "picker-values");
    setFontSize("picker-c-label", 10);
    setFlex("picker-c-label", "width", 70);

    createLabel("picker-h-label", "H: 0.0", "picker-values");
    setFontSize("picker-h-label", 10);
    setFlex("picker-h-label", "width", 60);

    // Hex value
    createRow("picker-hex-row", "color-picker");
    setFlex("picker-hex-row", "height", 20);
    setFlex("picker-hex-row", "align_items", "center");

    createLabel("picker-hex", "#000000", "picker-hex-row");
    setFontSize("picker-hex", 12);

    // H slider
    createRow("picker-h-row", "color-picker");
    setFlex("picker-h-row", "height", 20);
    setFlex("picker-h-row", "gap", 6);
    setFlex("picker-h-row", "align_items", "center");

    createLabel("picker-h-lbl", "H", "picker-h-row");
    setFontSize("picker-h-lbl", 10);
    setFlex("picker-h-lbl", "width", 14);

    createFader("picker-h-fader", "horizontal", "picker-h-row");
    setFlex("picker-h-fader", "flex_grow", 1);
    setFlex("picker-h-fader", "height", 16);

    // C slider
    createRow("picker-c-row", "color-picker");
    setFlex("picker-c-row", "height", 20);
    setFlex("picker-c-row", "gap", 6);
    setFlex("picker-c-row", "align_items", "center");

    createLabel("picker-c-lbl", "C", "picker-c-row");
    setFontSize("picker-c-lbl", 10);
    setFlex("picker-c-lbl", "width", 14);

    createFader("picker-c-fader", "horizontal", "picker-c-row");
    setFlex("picker-c-fader", "flex_grow", 1);
    setFlex("picker-c-fader", "height", 16);

    // L slider
    createRow("picker-l-row", "color-picker");
    setFlex("picker-l-row", "height", 20);
    setFlex("picker-l-row", "gap", 6);
    setFlex("picker-l-row", "align_items", "center");

    createLabel("picker-l-lbl", "L", "picker-l-row");
    setFontSize("picker-l-lbl", 10);
    setFlex("picker-l-lbl", "width", 14);

    createFader("picker-l-fader", "horizontal", "picker-l-row");
    setFlex("picker-l-fader", "flex_grow", 1);
    setFlex("picker-l-fader", "height", 16);
}
buildLegacyColorPicker();

function showColorPicker(hex) {
    var oklch = OklchEngine.hexToOklch(hex);
    pickerColor = { L: oklch.L, C: oklch.C, H: oklch.H, hex: hex };

    setBackground("picker-preview", hex);
    setText("picker-l-label", "L: " + oklch.L.toFixed(2));
    setText("picker-c-label", "C: " + oklch.C.toFixed(3));
    setText("picker-h-label", "H: " + oklch.H.toFixed(1));
    setText("picker-hex", hex);
    setValue("picker-h-fader", oklch.H / 360);
    setValue("picker-c-fader", Math.min(oklch.C / 0.4, 1));
    setValue("picker-l-fader", oklch.L);

    setVisible("color-picker", true);
    pickerVisible = true;
    layout();
}

function hideColorPicker() {
    setVisible("color-picker", false);
    pickerVisible = false;
    layout();
}

function updatePickerFromSliders() {
    var h = getValue("picker-h-fader") * 360;
    var c = getValue("picker-c-fader") * 0.4;
    var l = getValue("picker-l-fader");
    var mapped = OklchEngine.gamutMap(l, c, h);
    var hex = OklchEngine.oklchToHex(mapped.L, mapped.C, mapped.H);

    pickerColor = { L: mapped.L, C: mapped.C, H: mapped.H, hex: hex };
    setBackground("picker-preview", hex);
    setText("picker-l-label", "L: " + mapped.L.toFixed(2));
    setText("picker-c-label", "C: " + mapped.C.toFixed(3));
    setText("picker-h-label", "H: " + mapped.H.toFixed(1));
    setText("picker-hex", hex);
}

function wireLegacyColorPicker() {
    on("picker-h-fader", "change", function() { updatePickerFromSliders(); });
    on("picker-c-fader", "change", function() { updatePickerFromSliders(); });
    on("picker-l-fader", "change", function() { updatePickerFromSliders(); });
}
wireLegacyColorPicker();
