// Pulp Style Designer — JS-defined UI matching ai-style-designer layout
// Reference: ~/Code/ai-style-designer/Tools/theme-designer.html
// Hot-reloadable: edit and save to see changes instantly.

function bootstrapTheme() {
    setTheme("dark");
}
bootstrapTheme();

// Wrap __requestFrame__ to use the callback registry pattern
// (C++ can't receive JS functions directly — use ID-based dispatch)
var __origRequestFrame__ = __requestFrame__;
function installFrameCallbackShim() {
    __requestFrame__ = function(fn) {
        var id = __frameNextId__++;
        __frameCallbacks__[id] = fn;
        return __origRequestFrame__(id);
    };
}
installFrameCallbackShim();

// ═══════════════════════════════════════════════════════════════════
// Color/palette/app state
// ═══════════════════════════════════════════════════════════════════
var currentAccent = '#89B4FA';
var currentHarmony = 'monochromatic';
var currentPaletteMode = 'dark';
var msgCount = 0;

// ═══════════════════════════════════════════════════════════════════
// D1: Per-token edit state
// ═══════════════════════════════════════════════════════════════════
var tokenEditState = {
    history: {},       // { "bg.primary": { original, stack: [hex...], cursor: 0 } }
    modified: {},      // { "bg.primary": true }
    activeToken: null,
    activeSwatchId: null
};
var oppositeModeVariants = { dark: null, light: null };

function captureModeVariant(mode, titleOverride) {
    return {
        mode: resolvePaletteMode(mode || currentPaletteMode),
        title: titleOverride || getText("theme-name-label"),
        themeJson: getThemeJson(),
        accent: currentAccent,
        harmony: currentHarmony
    };
}

function refreshOppositeModeButton() {
    var label = "Generate Opposite Mode";
    if (typeof tokenGroups !== "undefined") {
        var targetMode = resolvePaletteMode(currentPaletteMode === "light" ? "dark" : "light");
        label = (oppositeModeVariants[targetMode] ? "Switch to " : "Generate ")
            + (targetMode === "light" ? "Light Mode" : "Dark Mode");
    }
    try { setText("gen-opposite-lbl", label); } catch (e) {}
}

function markCurrentModeDirty() {
    var mode = resolvePaletteMode(currentPaletteMode);
    oppositeModeVariants[mode] = captureModeVariant(mode);
    oppositeModeVariants[mode === "light" ? "dark" : "light"] = null;
    refreshOppositeModeButton();
}

function applyModeVariant(variant) {
    if (!variant || !variant.themeJson) return false;
    var mode = syncPaletteMode(resolvePaletteMode(variant.mode));
    currentAccent = variant.accent || currentAccent;
    currentHarmony = variant.harmony || currentHarmony;
    setSelected("harmony-selector", harmonySelectorIndex(currentHarmony));
    setTheme(mode);
    applyTokenDiff(variant.themeJson);
    var accentOklch = OklchEngine.hexToOklch(currentAccent);
    accentHuePendingHue = accentOklch.H;
    setValue("accent-hue", accentOklch.H / 360);
    if (variant.title) setText("theme-name-label", variant.title);
    updateTokenSwatches();
    refreshPaletteEditorsFromPalette(PaletteSystem.create(currentAccent, currentHarmony), true, false);
    refreshOppositeModeButton();
    layout();
    return true;
}

function generateModeVariant(targetMode, titleOverride) {
    var mode = syncPaletteMode(resolvePaletteMode(targetMode));
    setTheme(mode);
    var palette = PaletteSystem.create(currentAccent, currentHarmony);
    applyPaletteThemeDiff(palette, mode);
    refreshPaletteEditorsFromPalette(palette, true, false);
    updateTokenSwatches();
    if (titleOverride) setText("theme-name-label", titleOverride);
    oppositeModeVariants[mode] = captureModeVariant(mode, getText("theme-name-label"));
    refreshOppositeModeButton();
    layout();
    return true;
}

function switchPaletteMode(targetMode, options) {
    options = options || {};
    var currentMode = resolvePaletteMode(currentPaletteMode);
    var resolvedTarget = resolvePaletteMode(targetMode);
    if (resolvedTarget === currentMode) {
        refreshOppositeModeButton();
        return true;
    }

    oppositeModeVariants[currentMode] = captureModeVariant(currentMode);
    if (applyModeVariant(oppositeModeVariants[resolvedTarget])) return true;

    var generatedTitle = options.generatedTitle;
    if (!generatedTitle) {
        generatedTitle = resolvedTarget === "light" ? "Generated Light" : "Generated Dark";
    }
    return generateModeVariant(resolvedTarget, generatedTitle);
}

function tokenHistory(name) {
    if (!tokenEditState.history[name]) {
        var themeColors = JSON.parse(getThemeJson()).colors || {};
        tokenEditState.history[name] = {
            original: themeColors[name] || '#000000',
            stack: [themeColors[name] || '#000000'],
            cursor: 0
        };
    }
    return tokenEditState.history[name];
}

function pushTokenEdit(name, hex) {
    var h = tokenHistory(name);
    h.stack = h.stack.slice(0, h.cursor + 1);
    h.stack.push(hex);
    h.cursor = h.stack.length - 1;
    setTokenModifiedState(name, hex);
}

function setTokenModifiedState(name, hex) {
    var h = tokenHistory(name);
    tokenEditState.modified[name] = (hex !== h.original);
    if (!tokenEditState.modified[name]) delete tokenEditState.modified[name];
}

function applyTokenColor(name, hex, options) {
    options = options || {};
    if (hex && hex.charAt(0) === "#") hex = hex.toUpperCase();
    var swatchId = null;
    for (var g = 0; g < tokenGroups.length; g++) {
        for (var t = 0; t < tokenGroups[g].tokens.length; t++) {
            if (tokenGroups[g].tokens[t] === name) {
                swatchId = "tok-" + g + "-" + t + "-sw";
                break;
            }
        }
        if (swatchId) break;
    }
    if (options.recordHistory === false) setTokenModifiedState(name, hex);
    else pushTokenEdit(name, hex);
    var obj = { colors: {} };
    obj.colors[name] = hex;
    applyTokenDiff(JSON.stringify(obj));
    if (options.snapshot !== false) pushThemeSnapshot();
    // Flash: briefly show accent color on swatch
    if (swatchId && options.flash !== false) setBackground(swatchId, APP_ACCENT);
    if (options.relayout !== false) layout();
    updateTokenSwatches();
    markCurrentModeDirty();
    updateModifiedCount();
    updateAllTokenNameDisplays();
    if (tokenEditState.activeToken && options.refreshPopup !== false) updatePopupState(tokenEditState.activeToken);
    if (options.relayout !== false) layout();
}

function updateModifiedCount() {
    var n = 0;
    for (var k in tokenEditState.modified) n++;
    setText("status-text", n > 0 ? n + " token" + (n === 1 ? "" : "s") + " modified" : "0 tokens modified");
}

function resetTokenColor(name) {
    var h = tokenHistory(name);
    var orig = h.original;
    h.stack = [orig];
    h.cursor = 0;
    delete tokenEditState.modified[name];
    var obj = { colors: {} };
    obj.colors[name] = orig;
    applyTokenDiff(JSON.stringify(obj));
    pushThemeSnapshot();
    updateTokenSwatches();
    markCurrentModeDirty();
    updateModifiedCount();
    updateAllTokenNameDisplays();
    if (tokenEditState.activeToken === name) updatePopupState(name);
    layout();
}

function updateAllTokenNameDisplays() {
    for (var g = 0; g < tokenGroups.length; g++) {
        for (var t = 0; t < tokenGroups[g].tokens.length; t++) {
            var name = tokenGroups[g].tokens[t];
            var rowId = "tok-" + g + "-" + t;
            var labelId = "tok-" + g + "-" + t + "-name";
            var markerId = "tok-" + g + "-" + t + "-mod";
            var resetId = "tok-" + g + "-" + t + "-reset";
            var swatchId = "tok-" + g + "-" + t + "-sw";
            if (tokenEditState.modified[name]) {
                setText(labelId, name);
                setTextColor(labelId, APP_ACCENT);
                setTextColor(markerId, APP_ACCENT_HOVER);
                setOpacity(markerId, 1.0);
                setVisible(resetId, true);
                setBorder(swatchId, APP_ACCENT_HOVER, 1, 4);
                setBackground(rowId, "#8f6cff14");
            } else {
                setText(labelId, name);
                setTextColor(labelId, APP_TEXT);
                setOpacity(markerId, 0.0);
                setVisible(resetId, false);
                setBorder(swatchId, APP_BORDER, 1, 4);
                setBackground(rowId, 'transparent');
            }
        }
    }
}

function isTokenModified(name) {
    return !!tokenEditState.modified[name];
}

// ═══════════════════════════════════════════════════════════════════
// App colors (matching original --app-* CSS variables)
// ═══════════════════════════════════════════════════════════════════
var APP_BG      = '#18181f';
var APP_SURFACE = '#1e1e26';
var APP_PANEL   = '#242429';
var APP_PANEL_RAISED = '#2a2a31';
var APP_BORDER  = '#2e2e36';
var APP_TEXT    = '#d4d4dc';
var APP_TEXT_DIM = '#808090';
var APP_ACCENT  = '#aa88ff';
var APP_ACCENT_HOVER = '#bf9fff';
var previewReady = false;
var activePreviewTab = 0;
var previewTabHoverIndex = -1;

function previewThemeColor(token, fallback) {
    var colors = JSON.parse(getThemeJson()).colors || {};
    return colors[token] || fallback;
}

var previewTabContent = [
    { title: "Panel content area", subtitle: "With divider and secondary text" },
    { title: "Audio routing", subtitle: "Meters, waveforms, and controls align here" },
    { title: "MIDI mapping", subtitle: "Assignments and ranges preview here" },
    { title: "About this preset", subtitle: "Metadata, export details, and notes" }
];

function refreshPreviewThemeBase() {
    if (!previewReady) return;

    var bgPrimary = previewThemeColor("bg.primary", APP_BG);
    var bgSecondary = previewThemeColor("bg.secondary", APP_PANEL);
    var bgSurface = previewThemeColor("bg.surface", APP_PANEL);
    var bgElevated = previewThemeColor("bg.elevated", APP_PANEL_RAISED);
    var textPrimary = previewThemeColor("text.primary", APP_TEXT);
    var textSecondary = previewThemeColor("text.secondary", APP_TEXT_DIM);
    var textDisabled = previewThemeColor("text.disabled", APP_TEXT_DIM);
    var accentPrimary = previewThemeColor("accent.primary", APP_ACCENT);
    var accentError = previewThemeColor("accent.error", "#f38ba8");
    var focusRing = previewThemeColor("focus.ring", accentPrimary);
    var gradientStart = previewThemeColor("gradient.start", accentPrimary);
    var gradientEnd = previewThemeColor("gradient.end", previewThemeColor("accent.secondary", accentPrimary));
    var controlFill = previewThemeColor("control.fill", accentPrimary);
    var controlBorder = previewThemeColor("control.border", APP_BORDER);
    var divider = previewThemeColor("divider", APP_BORDER);
    var overlayBg = previewThemeColor("overlay.bg", applyHexAlpha(bgElevated, 0.82));
    var modalBg = previewThemeColor("modal.bg", bgSurface);
    var modalBorder = previewThemeColor("modal.border", controlBorder);
    var tooltipBg = previewThemeColor("tooltip.bg", modalBg);
    var tooltipText = previewThemeColor("tooltip.text", textPrimary);

    setBackground("center-panel", bgPrimary);
    setBackground("preview-shell", bgSurface);
    setBorder("preview-shell", divider, 1, 12);
    setBackground("preview-chrome", bgElevated);
    setTextColor("preview-chrome-title", textSecondary);
    setBackground("preview-chrome-divider", divider);
    setBackground("preview-scroll", bgPrimary);

    var headerIds = [
        "foundations-header", "controls-header", "data-header", "layout-header",
        "tabs-header", "overlays-header", "states-header", "effects-header",
        "showcase-header", "waveform2-header"
    ];
    for (var hi = 0; hi < headerIds.length; hi++) setTextColor(headerIds[hi], textSecondary);

    setTextColor("th-heading", textPrimary);
    setTextColor("th-body", textPrimary);
    setTextColor("th-caption", textSecondary);
    setTextColor("toggle-on-label", textPrimary);
    setTextColor("toggle-off-label", textPrimary);
    setTextColor("panel-title", textPrimary);
    setTextColor("panel-sub", textSecondary);
    setTextColor("spinner-text", textSecondary);

    setBackground("layout-header-line", divider);
    setBackground("panel-divider", divider);
    setBackground("tab-bar-preview-line", "transparent");

    for (var bi = 0; bi < 4; bi++) {
        setBorder("bg-sw-" + bi, divider, 1, 4);
        setTextColor("bg-sw-" + bi + "-l", textPrimary);
    }

    setBackground("btn-normal", bgSurface);
    setBorder("btn-normal", controlBorder, 1, 6);
    setTextColor("btn-normal-label", textPrimary);

    setBackground("btn-hover", bgElevated);
    setBorder("btn-hover", controlBorder, 1, 6);
    setTextColor("btn-hover-label", textPrimary);

    setBackground("btn-action", accentPrimary);
    setBorder("btn-action", accentPrimary, 0, 6);
    setTextColor("btn-action-label", "#ffffff");

    setBackground("btn-disabled", bgSecondary);
    setBorder("btn-disabled", controlBorder, 1, 6);
    setTextColor("btn-disabled-label", textDisabled);

    setBackground("sample-input", bgSurface);
    setBackground("sample-placeholder", bgSurface);
    setBackground("sample-combo", bgSurface);
    setBorder("sample-input", controlBorder, 1, 6);
    setBorder("sample-placeholder", controlBorder, 1, 6);
    setBorder("sample-combo", controlBorder, 1, 6);
    setTextColor("sample-input", textPrimary);
    setTextColor("sample-placeholder", textSecondary);
    setTextColor("sample-combo-label", textPrimary);
    setTextColor("sample-combo-caret", textSecondary);

    setBackground("tb1", bgSurface);
    setBorder("tb1", controlBorder, 1, 6);

    setBackground("panel-content", bgSurface);
    setBorder("panel-content", divider, 1, 6);

    setBackground("overlay-row", overlayBg);
    setBorder("overlay-row", divider, 1, 8);

    setBackground("dialog-card", modalBg);
    setBorder("dialog-card", modalBorder, 1, 8);
    setTextColor("dialog-title", textPrimary);
    setTextColor("dialog-msg", textSecondary);
    setBackground("dialog-cancel", bgElevated);
    setBorder("dialog-cancel", modalBorder, 1, 4);
    setTextColor("dialog-cancel-l", textSecondary);
    setBackground("dialog-accept", accentPrimary);
    setBorder("dialog-accept", accentPrimary, 0, 4);
    setTextColor("dialog-accept-l", "#ffffff");

    setBackground("ctx-menu", tooltipBg);
    setBorder("ctx-menu", modalBorder, 1, 6);
    for (var ci = 0; ci < 5; ci++) {
        if (ci === 2) setBackground("ctx-sep-" + ci, divider);
        else setTextColor("ctx-" + ci + "-l", tooltipText);
    }
    setBackground("ctx-1", applyHexAlpha(accentPrimary, 0.16));
    setTextColor("ctx-1-l", accentPrimary);
    setTextColor("ctx-4-l", accentError);

    var stateBgs = [bgSurface, bgElevated, accentPrimary, bgSurface, bgSecondary];
    var stateBorders = [controlBorder, controlBorder, accentPrimary, previewThemeColor("focus.ring", accentPrimary), controlBorder];
    var stateText = [textPrimary, textPrimary, "#ffffff", textPrimary, textDisabled];
    for (var si = 0; si < 5; si++) {
        setBackground("state-" + si, stateBgs[si]);
        setBorder("state-" + si, stateBorders[si], 1, 4);
        setTextColor("state-" + si + "-l", stateText[si]);
    }
    setOpacity("state-4", 0.5);

    for (var ei = 0; ei < 4; ei++) {
        setBackground("effect-" + ei, bgSurface);
        setBorder("effect-" + ei, controlBorder, 1, 6);
        setTextColor("effect-" + ei + "-l", textPrimary);
    }
    setBackground("effect-0-chip", bgElevated);
    setBorder("effect-0-chip", divider, 1, 4);
    setBoxShadow("effect-0-chip", 0, 3, 8, 0, applyHexAlpha(bgPrimary, 0.58));
    setBackground("effect-1-chip", focusRing);
    setBorder("effect-1-chip", focusRing, 0, 4);
    setBoxShadow("effect-1-chip", 0, 0, 10, 0, applyHexAlpha(focusRing, 0.52));
    for (var blurIdx = 0; blurIdx < 3; blurIdx++) {
        setBackground("effect-2-chip-" + blurIdx, controlFill);
    }
    setOpacity("effect-2-chip-0", 0.24);
    setOpacity("effect-2-chip-1", 0.48);
    setOpacity("effect-2-chip-2", 0.8);
    setBackground("effect-3-chip-0", gradientStart);
    setBackground("effect-3-chip-1", accentPrimary);
    setBackground("effect-3-chip-2", gradientEnd);
}

function refreshPreviewTabs(skipLayout) {
    if (!previewReady) return;
    var activeUnderline = previewThemeColor("tab.active", APP_ACCENT);
    var inactiveText = previewThemeColor("tab.inactive", APP_TEXT_DIM);
    var activeText = previewThemeColor("text.primary", APP_TEXT);
    var activeBg = applyHexAlpha(activeUnderline, 0.18);
    var hoverBg = applyHexAlpha(activeUnderline, 0.12);
    for (var ti = 0; ti < 4; ti++) {
        var isActive = ti === activePreviewTab;
        var isHovered = ti === previewTabHoverIndex;
        setTextColor("ptab-" + ti + "-l", (isActive || isHovered) ? activeText : inactiveText);
        setBackground("ptab-" + ti, isActive ? activeBg : (isHovered ? hoverBg : "transparent"));
        setBackground("ptab-" + ti + "-line", isActive ? activeUnderline : "transparent");
    }
    setText("panel-title", previewTabContent[activePreviewTab].title);
    setText("panel-sub", previewTabContent[activePreviewTab].subtitle);
    if (!skipLayout) layout();
}

function setPreviewActiveTab(idx, skipLayout) {
    if (!previewReady) return;
    activePreviewTab = idx;
    previewTabHoverIndex = -1;
    refreshPreviewTabs(skipLayout);
}

function refreshPreviewLayoutSection() {
    if (!previewReady) return;
    var cardEmpty = previewThemeColor("card.empty", APP_PANEL);
    var cardLoading = previewThemeColor("card.loading", "#3e4245");
    var cardReady = previewThemeColor("card.ready", APP_PANEL);
    var cardError = previewThemeColor("card.error", "#4a1f28");
    var success = previewThemeColor("accent.success", "#4CAF50");
    var error = previewThemeColor("accent.error", "#F44336");
    var spinner = previewThemeColor("spinner", APP_ACCENT);
    var divider = previewThemeColor("divider", APP_BORDER);
    var textSecondary = previewThemeColor("text.secondary", APP_TEXT_DIM);

    setBackground("card-1", cardEmpty);
    setBackground("card-2", cardLoading);
    setBackground("card-3", cardReady);
    setBackground("card-4", cardError);
    setBorder("card-1", divider, 1, 8);
    setBorder("card-2", divider, 1, 8);
    setBorder("card-3", success, 1, 8);
    setBorder("card-4", error, 1, 8);
    setTextColor("card-1-label", textSecondary);
    setTextColor("card-2-label", textSecondary);
    setTextColor("card-3-label", textSecondary);
    setTextColor("card-4-label", error);
    setBackground("card-3-badge", success);
    setBackground("card-4-badge", error);
    setTextColor("card-3-badge-lbl", "#ffffff");
    setTextColor("card-4-badge-lbl", "#ffffff");
    setTextColor("card-2-spinner", spinner);
    setBackground("panel-divider", divider);
    setTextColor("spinner-icon", spinner);
    setTextColor("spinner-text", previewThemeColor("text.secondary", APP_TEXT_DIM));
    setPreviewActiveTab(activePreviewTab, true);
}

