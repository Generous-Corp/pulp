// Forge Modular — the app shell.
//
// Built to match design/prototype/reference-render.png. That render is the
// specification: an earlier version of this file was written from a reading of
// the prototype rather than from the picture, and ended up matching neither it
// nor Forge. Screenshot the standalone and compare before believing any change
// here is right.
//
// The home screen is a rail, a hero, a composer and a project shelf. The
// chat-and-preview split is a *second* screen, reached after building —
// collapsing the two is what made the previous attempt line up with nothing.
//
// Ink & Signal throughout, shared with Forge, because these are one brand.

const C = {
    appBg:      "#0F1217",
    rail:       "#12161C",
    surface:    "#161A21",
    panel:      "#1E2530",
    raised:     "#28303C",
    line:       "#2B3340",
    lineStrong: "#3A4351",
    accent:     "#16DAC2",
    accentDeep: "#10B6A3",
    violet:     "#8B6CF5",
    amber:      "#F6B847",
    leaf:       "#3FCF77",
    indigo:     "#5E78FF",
    textStrong: "#F3F6F9",
    text:       "#D6DCE4",
    muted:      "#939CA9",
    faint:      "#646D7A",
    onAccent:   "#052320",
};
const FONT = "Jost";
const MONO = "JetBrains Mono";

let mode = "module";      // "module" | "patch"

// ── root: rail on the left, everything else to its right ─────────────────────

const root = createRow("root");
setBackground("root", C.appBg);
setFlex("root", "width", "100%");
setFlex("root", "height", "100%");

// ── the rail ─────────────────────────────────────────────────────────────────
//
// Forge's rail, with the two icons that differ: module and patch, which is what
// this product makes instead of plugins.

const rail = createCol("rail", "root");
setBackground("rail", C.rail);
setFlex("rail", "width", 64);
setFlex("rail", "align_items", "center");
setFlex("rail", "padding_top", 14);
setFlex("rail", "flex_direction", "column");

// The logo tile is the accent square Forge uses, not a button.
const brand = createRow("rail-brand", "rail");
setBackground("rail-brand", C.accent);
setCornerRadius("rail-brand", 12);
setFlex("rail-brand", "width", 40);
setFlex("rail-brand", "height", 40);
setFlex("rail-brand", "align_items", "center");
setFlex("rail-brand", "justify_content", "center");
createLabel("rail-brand-mark", "⠿", "rail-brand");
setTextColor("rail-brand-mark", C.onAccent);
setFontSize("rail-brand-mark", 17);

function railIcon(id, glyph, active, marginTop) {
    const b = createRow(id, "rail");
    setBackground(id, active ? C.raised : C.rail);
    setCornerRadius(id, 12);
    setFlex(id, "width", 40);
    setFlex(id, "height", 40);
    setFlex(id, "margin_top", marginTop);
    setFlex(id, "align_items", "center");
    setFlex(id, "justify_content", "center");
    createLabel(id + "-glyph", glyph, id);
    setTextColor(id + "-glyph", active ? C.accent : C.faint);
    setFontSize(id + "-glyph", 16);
    return b;
}

railIcon("rail-home", "⌂", true, 14);
railIcon("rail-module", "▯", false, 6);
railIcon("rail-patch", "⑂", false, 6);
railIcon("rail-settings", "◎", false, 6);

const railGap = createCol("rail-gap", "rail");
setFlex("rail-gap", "flex_grow", 1);

railIcon("rail-install", "⤓", false, 0);
railIcon("rail-account", "☺", false, 6);
setFlex("rail-account", "margin_bottom", 14);

// ── everything right of the rail ─────────────────────────────────────────────

const main = createCol("main", "root");
setFlex("main", "flex_grow", 1);
setFlex("main", "flex_direction", "column");

// ── top bar ──────────────────────────────────────────────────────────────────

const topbar = createRow("topbar", "main");
setFlex("topbar", "align_items", "center");
setFlex("topbar", "padding_left", 18);
setFlex("topbar", "padding_right", 18);
setFlex("topbar", "height", 46);

createLabel("topbar-name", "Forge Modular", "topbar");
setFontFamily("topbar-name", FONT);
setFontSize("topbar-name", 15);
setFontWeight("topbar-name", 700);
setTextColor("topbar-name", C.textStrong);

/// A small monospace chip: the artifact summary on the left, Rack's state and
/// the formats this build provides on the right.
function chip(id, parent, text, color, marginLeft) {
    const c = createRow(id, parent);
    setBackground(id, C.surface);
    setBorder(id, C.line, 1);
    setCornerRadius(id, 7);
    setFlex(id, "padding_left", 9);
    setFlex(id, "padding_right", 9);
    setFlex(id, "padding_top", 4);
    setFlex(id, "padding_bottom", 4);
    setFlex(id, "margin_left", marginLeft);
    setFlex(id, "align_items", "center");
    createLabel(id + "-text", text, id);
    setFontFamily(id + "-text", MONO);
    setFontSize(id + "-text", 10);
    setLetterSpacing(id + "-text", 0.07);
    setTextColor(id + "-text", color);
    return c;
}

chip("chip-artifact", "topbar", "MODULE · 12 HP", C.muted, 12);

const topGap = createRow("topbar-gap", "topbar");
setFlex("topbar-gap", "flex_grow", 1);

// Whether Rack is running is the one piece of state the whole product hangs
// on, so it is stated rather than discovered when a button fails.
createLabel("rack-status", "● RACK NOT RUNNING", "topbar");
setFontFamily("rack-status", MONO);
setFontSize("rack-status", 10);
setLetterSpacing("rack-status", 0.07);
setTextColor("rack-status", C.faint);

chip("chip-vcv", "topbar", "VCV", C.faint, 12);
chip("chip-vst3", "topbar", "VST3", C.faint, 6);
chip("chip-standalone", "topbar", "STANDALONE", C.faint, 6);

// ── hero ─────────────────────────────────────────────────────────────────────

const hero = createCol("hero", "main");
setFlex("hero", "flex_grow", 1);
setFlex("hero", "align_items", "center");
setFlex("hero", "justify_content", "center");
setFlex("hero", "flex_direction", "column");
setBackground("hero", C.surface);

createLabel("hero-eyebrow", "FORGE MODULAR · FOR VCV RACK", "hero");
setFontFamily("hero-eyebrow", MONO);
setFontSize("hero-eyebrow", 11);
setLetterSpacing("hero-eyebrow", 0.18);
setTextColor("hero-eyebrow", C.muted);

createLabel("hero-title", "What should the module do?", "hero");
setFontFamily("hero-title", FONT);
setFontSize("hero-title", 44);
setFontWeight("hero-title", 700);
setTextColor("hero-title", C.textStrong);
setFlex("hero-title", "margin_top", 12);

createLabel("hero-sub", "One Eurorack panel — knobs, jacks and the DSP behind them.", "hero");
setFontFamily("hero-sub", FONT);
setFontSize("hero-sub", 15);
setTextColor("hero-sub", C.muted);
setFlex("hero-sub", "margin_top", 10);

// ── tabs, joined to the composer ─────────────────────────────────────────────
//
// One shape, not two. The tabs carry top-only radii and no bottom border, and
// the composer's top-left corner is square, because they meet there.

const tabs = createRow("tabs", "hero");
setFlex("tabs", "align_items", "flex_end");
setFlex("tabs", "width", 1000);
setFlex("tabs", "max_width", 1000);
setFlex("tabs", "flex_grow", 0);
setFlex("tabs", "flex_shrink", 0);
setFlex("tabs", "margin_top", 34);

function tab(id, glyph, title, sub, active) {
    const t = createRow(id, "tabs");
    setBackground(id, active ? C.raised : C.panel);
    setBorder(id, active ? C.lineStrong : C.line, 1);
    setCornerRadius(id, 12);
    setFlex(id, "padding_left", 16);
    setFlex(id, "padding_right", 16);
    setFlex(id, "padding_top", 10);
    setFlex(id, "padding_bottom", 10);
    setFlex(id, "align_items", "center");

    createLabel(id + "-glyph", glyph, id);
    setFontSize(id + "-glyph", 13);
    setTextColor(id + "-glyph", active ? C.textStrong : C.faint);

    createLabel(id + "-name", title, id);
    setFontFamily(id + "-name", FONT);
    setFontSize(id + "-name", 13.5);
    setFontWeight(id + "-name", 600);
    setTextColor(id + "-name", active ? C.textStrong : C.muted);
    setFlex(id + "-name", "margin_left", 8);

    createLabel(id + "-sub", sub, id);
    setFontFamily(id + "-sub", MONO);
    setFontSize(id + "-sub", 9.5);
    setLetterSpacing(id + "-sub", 0.08);
    setTextColor(id + "-sub", C.faint);
    setFlex(id + "-sub", "margin_left", 9);
    return t;
}

tab("tab-module", "▯", "Module", "ONE PANEL", true);
tab("tab-patch", "⑂", "Patch", "A WHOLE RACK", false);

// ── composer ─────────────────────────────────────────────────────────────────

const composer = createCol("composer", "hero");
setBackground("composer", C.raised);
setBorder("composer", C.lineStrong, 1);
setCornerRadius("composer", 16);
setFlex("composer", "width", 1000);
setFlex("composer", "max_width", 1000);
setFlex("composer", "flex_grow", 0);
setFlex("composer", "flex_shrink", 0);
setFlex("composer", "padding", 20);

const prompt = createTextEditor("prompt", "composer");
setFontFamily("prompt", FONT);
setFontSize("prompt", 16);
setTextColor("prompt", C.muted);
setBackground("prompt", C.raised);
setFlex("prompt", "min_height", 30);

const actions = createRow("actions", "composer");
setFlex("actions", "align_items", "center");
setFlex("actions", "margin_top", 18);

/// A composer button: glyph then label, because the render's buttons read as an
/// icon and a word rather than as text alone.
function button(id, parent, glyph, label, kind, width) {
    const b = createRow(id, parent);
    setBackground(id, kind === "primary" ? C.accent : C.panel);
    setBorder(id, kind === "primary" ? C.accent : C.line, 1);
    setCornerRadius(id, 11);
    setFlex(id, "padding_left", kind === "icon" ? 13 : 17);
    setFlex(id, "padding_right", kind === "icon" ? 13 : 17);
    setFlex(id, "padding_top", 11);
    setFlex(id, "padding_bottom", 11);
    setFlex(id, "align_items", "center");
    setFlex(id, "flex_grow", 0);
    setFlex(id, "flex_shrink", 0);
    setFlex(id, "justify_content", "center");
    setFlex(id, "height", 44);
    // Sized rather than left to content: a content-sized row here grew to fill
    // the composer and pushed its siblings off the panel's right edge.
    setFlex(id, "width", width);

    createLabel(id + "-glyph", glyph, id);
    setFontSize(id + "-glyph", 13);
    setTextColor(id + "-glyph", kind === "primary" ? C.onAccent : C.muted);

    createLabel(id + "-label", label, id);
    setFontFamily(id + "-label", FONT);
    setFontSize(id + "-label", 14);
    setFontWeight(id + "-label", 600);
    setTextColor(id + "-label", kind === "primary" ? C.onAccent : C.text);
    setFlex(id + "-label", "margin_left", 9);
    return b;
}

button("btn-mention", "actions", "@", "", "icon", 44);
setFlex("btn-mention", "margin_right", 9);
button("btn-random", "actions", "⚄", "Random module", "ghost", 186);

const actionsGap = createRow("actions-gap", "actions");
setFlex("actions-gap", "flex_grow", 1);

// Two labelled buttons rather than an inferred intent chip: a chip guesses and
// the user still has to notice the guess, while an unwanted rebuild rewrites
// their work and an unwanted answer costs nothing.
button("btn-ask", "actions", "?", "Ask", "ghost", 96);
setFlex("btn-ask", "margin_right", 10);
button("btn-build", "actions", "⚒", "Build module", "primary", 176);

createLabel("composer-hint",
            "↵  ASK ANSWERS · BUILD REWRITES. TWO BUTTONS SO NOTHING IS INFERRED.",
            "composer");
setFontFamily("composer-hint", MONO);
setFontSize("composer-hint", 9.5);
setLetterSpacing("composer-hint", 0.06);
setTextColor("composer-hint", C.faint);
setFlex("composer-hint", "margin_top", 14);

// ── project shelf ────────────────────────────────────────────────────────────

const shelf = createCol("shelf", "main");
setBackground("shelf", C.appBg);
setFlex("shelf", "height", 300);
setFlex("shelf", "padding_left", 22);
setFlex("shelf", "padding_right", 22);
setFlex("shelf", "padding_top", 16);
setFlex("shelf", "flex_direction", "column");

const shelfBar = createRow("shelf-bar", "shelf");
setFlex("shelf-bar", "align_items", "center");

function shelfTab(id, title, active) {
    const t = createRow(id, "shelf-bar");
    setFlex(id, "margin_right", 22);
    setFlex(id, "padding_bottom", 8);
    createLabel(id + "-text", title, id);
    setFontFamily(id + "-text", FONT);
    setFontSize(id + "-text", 16);
    setFontWeight(id + "-text", 600);
    setTextColor(id + "-text", active ? C.textStrong : C.faint);
    return t;
}

shelfTab("shelf-patches", "Patches", true);
shelfTab("shelf-modules", "My modules", false);

const shelfGap = createRow("shelf-gap", "shelf-bar");
setFlex("shelf-gap", "flex_grow", 1);

button("btn-library", "shelf-bar", "▥", "Module library  →", "ghost");

const cards = createRow("cards", "shelf");
setFlex("cards", "margin_top", 16);

/// A project card. The count line is the honest summary of a patch: how many
/// modules and how many cables, which is what tells you its size at a glance.
function card(id, kind, title, count, tint) {
    const c = createCol(id, "cards");
    setBackground(id, C.panel);
    setBorder(id, C.line, 1);
    setCornerRadius(id, 12);
    setFlex(id, "width", 232);
    setFlex(id, "margin_right", 16);
    setFlex(id, "flex_direction", "column");

    const art = createRow(id + "-art", id);
    setBackground(id + "-art", tint);
    setCornerRadius(id + "-art", 12);
    setFlex(id + "-art", "height", 108);

    createLabel(id + "-kind", kind, id);
    setFontFamily(id + "-kind", MONO);
    setFontSize(id + "-kind", 9.5);
    setLetterSpacing(id + "-kind", 0.09);
    setTextColor(id + "-kind", C.faint);
    setFlex(id + "-kind", "margin_left", 12);
    setFlex(id + "-kind", "margin_top", 10);

    createLabel(id + "-title", title, id);
    setFontFamily(id + "-title", FONT);
    setFontSize(id + "-title", 15);
    setFontWeight(id + "-title", 600);
    setTextColor(id + "-title", C.textStrong);
    setFlex(id + "-title", "margin_left", 12);
    setFlex(id + "-title", "margin_top", 4);

    createLabel(id + "-count", count, id);
    setFontFamily(id + "-count", FONT);
    setFontSize(id + "-count", 12.5);
    setTextColor(id + "-count", C.muted);
    setFlex(id + "-count", "margin_left", 12);
    setFlex(id + "-count", "margin_top", 2);
    setFlex(id + "-count", "margin_bottom", 12);
    return c;
}

card("card-1", "PATCH", "Ambient drone", "8 modules · 9 cables", "#16403D");
card("card-2", "PATCH", "Krell garden", "11 modules · 17 cables", "#2A2551");
card("card-3", "PATCH", "Acid mutation", "9 modules · 14 cables", "#3D2A16");
card("card-4", "PATCH", "Bouncing ball", "5 modules · 7 cables", "#16351F");

// ── mode ─────────────────────────────────────────────────────────────────────

function setMode(next) {
    mode = next;
    const isPatch = mode === "patch";
    setBackground("tab-module", isPatch ? C.panel : C.raised);
    setBackground("tab-patch", isPatch ? C.raised : C.panel);
    setTextColor("tab-module-name", isPatch ? C.muted : C.textStrong);
    setTextColor("tab-patch-name", isPatch ? C.textStrong : C.muted);
    setText("hero-title", isPatch ? "What should the patch do?"
                                  : "What should the module do?");
    setText("hero-sub", isPatch
        ? "A whole rack — modules, cables, and why each one is there."
        : "One Eurorack panel — knobs, jacks and the DSP behind them.");
    setText("btn-random-label", isPatch ? "Random patch" : "Random module");
    setText("btn-build-label", isPatch ? "Build patch" : "Build module");
    setText("chip-artifact-text", isPatch ? "PATCH · A WHOLE RACK"
                                          : "MODULE · 12 HP");
}

/// Called by the host once it knows. Rack's state is what the whole product
/// depends on, so it is reported rather than left to be discovered when a
/// button quietly fails.
function setRackStatus(running) {
    setText("rack-status", running ? "● RACK RUNNING" : "● RACK NOT RUNNING");
    setTextColor("rack-status", running ? C.leaf : C.faint);
}

setMode(mode);
