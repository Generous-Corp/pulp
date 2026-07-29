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

// Suggestions the Random button offers. Deliberately concrete: "a filter" is
// not a prompt anybody can judge, and the whole point of showing it before
// building is that it can be read and edited.
const RANDOM_MODULE = [
    "a 12 HP wavefolder with drive and symmetry, plus a CV input for the fold amount",
    "an 8 HP slew limiter with separate rise and fall, and an end-of-rise gate",
];
const RANDOM_PATCH = [
    "an ambient generative drone that never repeats",
    "a bouncing-ball rhythm that slows down as it settles",
];

// ── root: rail on the left, everything else to its right ─────────────────────

const root = createRow("root");
setBackground("root", "#07090C");
const shellPad = 10;
setFlex("root", "padding", shellPad);
setFlex("root", "width", "100%");
setFlex("root", "height", "100%");

// ── the rail ─────────────────────────────────────────────────────────────────
//
// Forge's rail, with the two icons that differ: module and patch, which is what
// this product makes instead of plugins.

// The rounded card in the render is a WINDOW shape, not a widget one. Rounding
// the columns that paint the edges gets us the radii, but the window behind
// them is opaque and square, so the corners do not read. Finishing this means
// a borderless rounded window on the host side; it is not reachable from here.
const shell = createRow("shell", "root");
setBackground("shell", C.appBg);
setFlex("shell", "flex_grow", 1);
setFlex("shell", "width", "100%");

const rail = createCol("rail", "shell");
setBackground("rail", C.rail);
setFlex("rail", "width", 64);
setFlex("rail", "align_items", "center");
setFlex("rail", "padding_top", 14);
setFlex("rail", "direction", "column");
setCornerRadius("rail", "TopLeft", 14);
setCornerRadius("rail", "BottomLeft", 14);

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


// ── icons ────────────────────────────────────────────────────────────────────
//
// Our own strokes on a 24x24 grid, not an imported set. The same reasoning as
// the Eurorack components: the design references a library whose licence we do
// not want to inherit, and these shapes are simple enough to draw. Stroke
// weight and cap style are what make them read as one family, so both are set
// centrally rather than per icon.

const ICON = {
    home:     "M3 11 L12 3 L21 11 M6 9 V21 H18 V9",
    module:   "M7 3 H17 V21 H7 Z M10 7 h4 M12 10 a1.6 1.6 0 1 0 .01 0" +
              " M10.4 16 a1 1 0 1 0 .01 0 M13.6 16 a1 1 0 1 0 .01 0",
    patch:    "M6 3 a3 3 0 1 0 .01 0 M6 6 v5 a5 5 0 0 0 5 5 h2 a5 5 0 0 1 5 5 M18 21 a3 3 0 1 0 .01 0",
    settings: "M12 3 a9 9 0 1 0 0.01 0 M12 9 a3 3 0 1 0 0.01 0",
    install:  "M12 3 v11 M8 11 l4 4 4-4 M4 19 h16",
    account:  "M12 4 a4 4 0 1 0 0.01 0 M4 21 c0-4 4-6 8-6 s8 2 8 6",
    at:       "M16 12 a4 4 0 1 0-4 4 M16 8 v5 a3 3 0 0 0 5 -2 A9 9 0 1 0 17 19",
    dice:     "M4 4 h16 v16 H4 Z" +
              " M8.2 8.2 a1 1 0 1 0 .01 0 M15.8 8.2 a1 1 0 1 0 .01 0" +
              " M12 12 a1 1 0 1 0 .01 0" +
              " M8.2 15.8 a1 1 0 1 0 .01 0 M15.8 15.8 a1 1 0 1 0 .01 0",
    hammer:   "M14 3 l7 7 -3 3 -7-7 Z M11 8 L3 16 v5 h5 l8-8",
    ask:      "M12 3 a9 9 0 1 0 0.01 0 M9.5 9.5 a2.5 2.5 0 1 1 3.2 2.4 c-.8.3-1.2.9-1.2 1.7 M12 17 h.01",
    library:  "M4 4 v16 M8 4 v16 M13 5 l5 15 M20 20 H3",
    arrow:    "M4 12 h15 M14 7 l5 5 -5 5",
};

/// One icon. Sized in points and coloured like text, because that is what it
/// stands in for -- a glyph that happens to be drawn rather than typeset.
function icon(id, parent, name, size, color) {
    createSvgPath(id, parent);
    setSvgViewBox(id, 24, 24);
    setSvgPath(id, ICON[name] || ICON.module);
    setSvgStroke(id, color);
    setSvgStrokeWidth(id, 1.7);
    setSvgFill(id, "transparent");
    setFlex(id, "width", size);
    setFlex(id, "height", size);
    setFlex(id, "flex_grow", 0);
    setFlex(id, "flex_shrink", 0);
    decorative(id);
}


/// Mark a control's contents as decoration.
///
/// hit_test() returns the DEEPEST view under the point, so a label or icon
/// inside a button swallows the click and the button never sees it -- clicking
/// the word "Build" would do nothing while clicking the padding beside it
/// worked. Contents are decoration; only the control itself is a target.
function decorative(id) {
    setPointerEvents(id, "none");
}

/// A label, which is decoration by default.
///
/// hit_test() returns the deepest view under a point, so any label inside a
/// control swallows the click. Marking them one at a time is how the tabs kept
/// the bug after the buttons were fixed -- so the default is inverted here and
/// text is never a click target unless it is asked to be.
function textLabel(id, text, parent) {
    createLabel(id, text, parent);
    decorative(id);
}

function railIcon(id, glyph, active, marginTop) {
    const b = createToggleButton(id, "rail");
    setFlex(id, "direction", "row");
    // The rail reads as icons, not buttons. An unselected tile takes the rail's
    // own colour on both background and border, or the rail becomes a grid of
    // boxes -- which is what it looked like before these were set.
    setToggleBackground(id, C.rail, C.raised);
    setToggleBorderColor(id, C.rail, C.raised);
    setBackground(id, active ? C.raised : C.rail);
    setCornerRadius(id, 12);
    setFlex(id, "width", 40);
    setFlex(id, "height", 40);
    setFlex(id, "margin_top", marginTop);
    setFlex(id, "align_items", "center");
    setFlex(id, "justify_content", "center");
    icon(id + "-glyph", id, glyph, 19, active ? C.accent : C.faint);
    return b;
}

railIcon("rail-home", "home", true, 14);
railIcon("rail-module", "module", false, 6);
railIcon("rail-patch", "patch", false, 6);
railIcon("rail-settings", "settings", false, 6);

const railGap = createCol("rail-gap", "rail");
setFlex("rail-gap", "flex_grow", 1);

railIcon("rail-install", "install", false, 0);
railIcon("rail-account", "account", false, 6);
setFlex("rail-account", "margin_bottom", 14);

// ── everything right of the rail ─────────────────────────────────────────────

const main = createCol("main", "shell");
setFlex("main", "flex_grow", 1);
setFlex("main", "direction", "column");
setCornerRadius("main", "TopRight", 14);
setCornerRadius("main", "BottomRight", 14);

// ── top bar ──────────────────────────────────────────────────────────────────

const topbar = createRow("topbar", "main");
setFlex("topbar", "align_items", "center");
setFlex("topbar", "padding_left", 18);
setFlex("topbar", "padding_right", 18);
setFlex("topbar", "height", 46);
setCornerRadius("topbar", "TopRight", 14);

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
setFlex("hero", "direction", "column");
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
    const t = createToggleButton(id, "tabs");
    setFlex(id, "direction", "row");
    setToggleBackground(id, C.panel, C.raised);
    setToggleBorderColor(id, C.line, C.lineStrong);
    setBackground(id, active ? C.raised : C.panel);
    setBorder(id, active ? C.lineStrong : C.line, 1);
    setCornerRadius(id, 12);
    setFlex(id, "padding_left", 16);
    setFlex(id, "padding_right", 16);
    setFlex(id, "padding_top", 10);
    setFlex(id, "padding_bottom", 10);
    setFlex(id, "align_items", "center");

    icon(id + "-glyph", id, glyph, 15, C.muted);
    setTextColor(id + "-glyph", active ? C.textStrong : C.faint);

    textLabel(id + "-name", title, id);
    setFontFamily(id + "-name", FONT);
    setFontSize(id + "-name", 13.5);
    setFontWeight(id + "-name", 600);
    setTextColor(id + "-name", active ? C.textStrong : C.muted);
    setFlex(id + "-name", "margin_left", 8);

    textLabel(id + "-sub", sub, id);
    setFontFamily(id + "-sub", MONO);
    setFontSize(id + "-sub", 9.5);
    setLetterSpacing(id + "-sub", 0.08);
    setTextColor(id + "-sub", C.faint);
    setFlex(id + "-sub", "margin_left", 9);
    return t;
}

tab("tab-module", "module", "Module", "ONE PANEL", true);
tab("tab-patch", "patch", "Patch", "A WHOLE RACK", false);

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
setPlaceholder("prompt",
    "A 12 HP wavefolder with drive and symmetry, plus a CV input for the fold amount.");

const actions = createRow("actions", "composer");
setFlex("actions", "align_items", "center");
setFlex("actions", "margin_top", 18);

/// A composer button: glyph then label, because the render's buttons read as an
/// icon and a word rather than as text alone.
function button(id, parent, glyph, label, kind, width) {
    const b = createToggleButton(id, parent);
    setFlex(id, "direction", "row");
    const fill = kind === "primary" ? C.accent : C.panel;
    const edge = kind === "primary" ? C.accent : C.line;
    setToggleBackground(id, fill, kind === "primary" ? C.accentDeep : C.raised);
    setToggleBorderColor(id, edge, edge);
    setBackground(id, fill);
    setBorder(id, edge, 1);
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

    icon(id + "-glyph", id, glyph, 16,
         kind === "primary" ? C.onAccent : C.muted);

    createLabel(id + "-label", label, id);
    setFontFamily(id + "-label", FONT);
    setFontSize(id + "-label", 14);
    setFontWeight(id + "-label", 600);
    setTextColor(id + "-label", kind === "primary" ? C.onAccent : C.text);
    setFlex(id + "-label", "margin_left", 9);
    decorative(id + "-label");
    return b;
}

button("btn-mention", "actions", "at", "", "icon", 44);
setFlex("btn-mention", "margin_right", 9);
button("btn-random", "actions", "dice", "Random module", "ghost", 186);

const actionsGap = createRow("actions-gap", "actions");
setFlex("actions-gap", "flex_grow", 1);

// Two labelled buttons rather than an inferred intent chip: a chip guesses and
// the user still has to notice the guess, while an unwanted rebuild rewrites
// their work and an unwanted answer costs nothing.
button("btn-ask", "actions", "ask", "Ask", "ghost", 96);
setFlex("btn-ask", "margin_right", 10);
button("btn-build", "actions", "hammer", "Build module", "primary", 176);
setBoxShadow("btn-build", 0, 0, 18, 2, "#16DAC255");

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
setFlex("shelf", "direction", "column");

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
    setFlex(id, "direction", "column");

    const art = createRow(id + "-art", id);
    setBackground(id + "-art", tint);
    setBackgroundGradient(id + "-art",
                          "linear-gradient(160deg, " + tint + " 0%, #12161C 100%)");
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
    // Published for the host: wire() reads this rather than keeping a second
    // copy of the mode that could disagree with the one the tabs set.
    setText("mode-state", mode);
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


// ── the working screen ───────────────────────────────────────────────────────
//
// A second screen, not a panel on the home screen. Collapsing the two is what
// made an earlier attempt line up with neither the design nor Forge: the home
// screen is a composer, and this is the transcript of a build beside the thing
// being built.
//
// The chat is not a log. Wiring is grouped by the role a module plays, and
// every line carries why the cable is there -- a patch you cannot read is a
// patch you cannot change.

const work = createRow("work", "main");
setFlex("work", "flex_grow", 1);
setVisible("work", false);

const chat = createCol("chat", "work");
setBackground("chat", C.surface);
setFlex("chat", "width", 460);
setFlex("chat", "flex_shrink", 0);
setFlex("chat", "direction", "column");
setFlex("chat", "padding", 22);

createLabel("chat-title", "Building", "chat");
setFontFamily("chat-title", FONT);
setFontSize("chat-title", 20);
setFontWeight("chat-title", 700);
setTextColor("chat-title", C.textStrong);

createLabel("chat-prompt", "", "chat");
setFontFamily("chat-prompt", FONT);
setFontSize("chat-prompt", 14);
setTextColor("chat-prompt", C.muted);
setFlex("chat-prompt", "margin_top", 6);

const chatBody = createCol("chat-body", "chat");
setFlex("chat-body", "flex_grow", 1);
setFlex("chat-body", "direction", "column");
setFlex("chat-body", "margin_top", 18);

/// One role heading -- Voice, Modulation, Output -- above the cables that serve
/// it. Grouping by role is what makes a rack legible; grouping by cable order
/// is just the order they happened to be made in.
function roleGroup(id, role) {
    const g = createCol(id, "chat-body");
    setFlex(id, "direction", "column");
    setFlex(id, "margin_bottom", 14);
    createLabel(id + "-role", role.toUpperCase(), id);
    setFontFamily(id + "-role", MONO);
    setFontSize(id + "-role", 10);
    setLetterSpacing(id + "-role", 0.16);
    setTextColor(id + "-role", C.accent);
    return g;
}

/// A wiring line: what was connected, then why. The why is the point -- it is
/// the difference between a patch somebody can edit and one they can only run.
function wireLine(id, parent, text, why) {
    const l = createCol(id, parent);
    setFlex(id, "direction", "column");
    setFlex(id, "margin_top", 8);
    createLabel(id + "-what", text, id);
    setFontFamily(id + "-what", FONT);
    setFontSize(id + "-what", 13);
    setTextColor(id + "-what", C.text);
    createLabel(id + "-why", why, id);
    setFontFamily(id + "-why", FONT);
    setFontSize(id + "-why", 12);
    setTextColor(id + "-why", C.faint);
    setFlex(id + "-why", "margin_top", 2);
    return l;
}

const preview = createCol("preview", "work");
setBackground("preview", C.appBg);
setFlex("preview", "flex_grow", 1);
setFlex("preview", "direction", "column");
setFlex("preview", "align_items", "center");
setFlex("preview", "justify_content", "center");

createLabel("preview-empty", "The rack appears here as it is wired.", "preview");
setFontFamily("preview-empty", FONT);
setFontSize("preview-empty", 14);
setTextColor("preview-empty", C.faint);


// ── the preview ──────────────────────────────────────────────────────────────
//
// The rack as it is wired, drawn from the geometry patch_layout.hpp computes:
// panels at their assigned x with HP-proportional widths, ports where the
// cartographer recorded them, cables between the two endpoints.
//
// A port the cartographer never mapped docks to the panel edge instead of
// being invented. That is drawn differently on purpose -- a cable that lands
// in the right place by accident teaches the wrong thing about the patch.

let previewShown = 0;

/// Draw a rack. `modules` are {slug, x, width, height, known}, x being px from
/// the rack's left edge as patch_layout.hpp assigns it.
///
/// Panels are laid out as a row with the gap between them expressed as a
/// margin, rather than positioned absolutely. The bridge exposes `start` and
/// `end` insets but no vertical one, so absolute placement can only control
/// one axis -- and a rack is a left-to-right run of panels anyway, which a row
/// models exactly.
///
/// Cables are not drawn yet for the same reason: an overlay needs both axes.
function showPatch(modules) {
    setVisible("preview-empty", false);
    const stage = "preview-stage-" + (++previewShown);
    createRow(stage, "preview");
    setFlex(stage, "align_items", "center");
    setFlex(stage, "flex_grow", 1);

    let cursor = 0;
    for (let i = 0; i < modules.length; ++i) {
        const m = modules[i];
        const id = stage + "-m" + i;
        createCol(id, stage);
        setFlex(id, "width", m.width);
        setFlex(id, "height", m.height);
        setFlex(id, "flex_shrink", 0);
        setFlex(id, "margin_left", Math.max(0, m.x - cursor));
        cursor = m.x + m.width;
        // A module we have never seen is drawn, but not as though we know it.
        setBackground(id, m.known ? C.panel : C.surface);
        setBorder(id, m.known ? C.lineStrong : C.line, 1);
        setCornerRadius(id, 3);

        createLabel(id + "-slug", m.slug, id);
        setFontFamily(id + "-slug", MONO);
        setFontSize(id + "-slug", 9);
        setLetterSpacing(id + "-slug", 0.1);
        setTextColor(id + "-slug", m.known ? C.muted : C.faint);
        setFlex(id + "-slug", "margin_top", 6);
        setFlex(id + "-slug", "margin_left", 5);
    }
}

/// Move between the composer and the build. setFlex(id,"display","none") is a
/// no-op in this bridge; setVisible is what hides.
function showScreen(name) {
    const working = name === "work";
    setVisible("hero", !working);
    setVisible("shelf", !working);
    setVisible("work", working);
}

/// Called as the build reports progress. The prompt is echoed so the screen
/// says what it is building without the user having to remember.
function beginBuild(promptText) {
    setText("chat-prompt", promptText);
    setText("chat-title", mode === "patch" ? "Wiring" : "Building");
    showScreen("work");
}

/// Surface a handler that threw.
///
/// __dispatch__ wraps every handler in a try/catch and, without this, drops the
/// error on the floor -- a button toggles, nothing happens, and nothing is
/// logged. A dead handler that says so is debuggable; a silent one is not.
function __dispatchError__(id, eventName, err) {
    setText("rack-status", "\u25cf HANDLER FAILED " + id + ": " + err);
    setTextColor("rack-status", C.amber);
}

// ── wiring ───────────────────────────────────────────────────────────────────
//
// A control that looks pressable and is not is worse than no control, so
// everything that paints as a button gets a handler here. Build and Ask are
// wired natively in shell.cpp because they need the engine; the rest is
// presentation and belongs with the presentation.

on("tab-module", "toggle", function (v) { if (v) setMode("module"); });
on("tab-patch",  "toggle", function (v) { if (v) setMode("patch"); });

/// The rail selects a destination. Home is the composer; module and patch are
/// the same two modes the tabs offer, reachable from either place so neither
/// feels like the only way in.
const RAIL = ["rail-home", "rail-module", "rail-patch", "rail-settings"];

function selectRail(id) {
    for (let i = 0; i < RAIL.length; ++i) {
        const on_ = RAIL[i] === id;
        setBackground(RAIL[i], on_ ? C.raised : C.rail);
        setSvgStroke(RAIL[i] + "-glyph", on_ ? C.accent : C.faint);
    }
}

// Navigation happens BEFORE the highlight, deliberately. __dispatch__ wraps
// handlers in a try/catch, so anything that throws while restyling the rail
// would silently swallow the navigation with it -- which is exactly what
// happened: the button toggled, the heading never moved, and nothing was
// logged. Cosmetics must not be able to block getting somewhere.
on("rail-home", "toggle", function (v) {
    if (!v) return;
    showScreen("home");
    selectRail("rail-home");
});
on("rail-module", "toggle", function (v) {
    if (!v) return;
    showScreen("home");
    setMode("module");
    selectRail("rail-module");
});
on("rail-patch", "toggle", function (v) {
    if (!v) return;
    showScreen("home");
    setMode("patch");
    selectRail("rail-patch");
});
on("rail-settings", "toggle", function (v) { if (v) selectRail("rail-settings"); });

// Random fills the composer rather than building straight away: a suggestion
// you cannot read before committing to it is a dice roll, not a prompt.
on("btn-random", "toggle", function (v) {
    if (!v) return;
    setText("prompt", mode === "patch" ? RANDOM_PATCH[0] : RANDOM_MODULE[0]);
});

// Not visible, but not a hack either -- it is the documented channel the host
// reads the mode through, and it is a label so it can be asserted in a test.
createLabel("mode-state", "module", "root");
setVisible("mode-state", false);

setMode(mode);
showScreen("home");
