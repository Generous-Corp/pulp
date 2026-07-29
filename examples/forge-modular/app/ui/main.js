// Forge Modular — the app shell.
//
// Two things get built here and they are not the same activity, so the shell
// says which one you are in and changes with it. A module is one panel; a
// patch is a whole rack, and patch mode is as much a teaching surface as a
// building one — the chat explains what was wired and why.
//
// Ink & Signal, shared with Forge, so the two read as one family.

const C = {
    appBg:      "#161A21",
    sunken:     "#0E1116",
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

// Cable colours are Rack's own, assigned by role at generation time, so the
// preview and Rack agree while the role still reads. See DECISIONS.md.
const ROLE = {
    audio:      { label: "AUDIO",        color: "#00b56e" },
    pitch:      { label: "PITCH & GATE", color: "#3695ef" },
    modulation: { label: "MODULATION",   color: "#8b4ade" },
    output:     { label: "OUTPUT",       color: "#ffb437" },
};

let mode = "module";      // "module" | "patch"

// ── shell ────────────────────────────────────────────────────────────────────

const root = createCol("root");
setBackground("root", C.appBg);
setFlex("root", "flex_direction", "column");

// ── tabs ─────────────────────────────────────────────────────────────────────
//
// The tabs and the composer are one shape, not two. An earlier prototype gave
// them top-only radii and no bottom border -- every part of which says "these
// join" -- and then held them 12px apart, so they read as floating buttons
// above an unrelated box. They share an edge here.

const tabs = createRow("tabs");
setFlex("tabs", "align_items", "flex_end");
setFlex("tabs", "padding_left", 20);
setFlex("tabs", "padding_top", 18);

function tab(id, title, sub, active) {
    const t = createRow(id);
    setBackground(id, active ? C.panel : C.appBg);
    setBorder(id, active ? C.lineStrong : C.line, 1);
    setCornerRadius(id, 12);
    setFlex(id, "padding_left", 16);
    setFlex(id, "padding_right", 16);
    setFlex(id, "padding_top", 9);
    setFlex(id, "padding_bottom", 9);
    setFlex(id, "align_items", "center");

    const label = createLabel(id + "-name");
    setText(id + "-name", title);
    setFontFamily(id + "-name", FONT);
    setFontSize(id + "-name", 13.5);
    setFontWeight(id + "-name", 600);
    setTextColor(id + "-name", active ? C.textStrong : C.muted);

    const hint = createLabel(id + "-sub");
    setText(id + "-sub", sub);
    setFontFamily(id + "-sub", MONO);
    setFontSize(id + "-sub", 9.5);
    setLetterSpacing(id + "-sub", 0.08);
    setTextColor(id + "-sub", C.faint);
    setFlex(id + "-sub", "margin_left", 9);
    return t;
}

tab("tab-module", "Module", "ONE PANEL", mode === "module");
tab("tab-patch", "Patch", "A WHOLE RACK", mode === "patch");

// ── body: chat on the left, preview on the right ─────────────────────────────

const body = createRow("body");
setFlex("body", "flex_grow", 1);
setFlex("body", "padding_left", 20);
setFlex("body", "padding_right", 20);
setFlex("body", "padding_bottom", 20);

// ── chat ─────────────────────────────────────────────────────────────────────

const chat = createCol("chat");
setBackground("chat", C.panel);
setBorder("chat", C.lineStrong, 1);
// Square top-left: this is the selected tab's panel, and the corner is the joint.
setCornerRadius("chat", 16);
setFlex("chat", "width", 380);
setFlex("chat", "flex_direction", "column");

const history = createScrollView("history");
setFlex("history", "flex_grow", 1);
setFlex("history", "padding", 16);

/// One wiring line, as the explanation prints it. Monospace because the arrows
/// have to align down the column for the shape of a patch to be readable.
function wiringLine(id, from, fromPort, to, toPort, color) {
    const row = createRow(id);
    setFlex(id, "align_items", "center");
    setFlex(id, "padding_top", 2);
    setFlex(id, "padding_bottom", 2);

    const dot = createPanel(id + "-dot");
    setBackground(id + "-dot", color);
    setCornerRadius(id + "-dot", 3);
    setFlex(id + "-dot", "width", 6);
    setFlex(id + "-dot", "height", 6);
    setFlex(id + "-dot", "margin_right", 10);

    const text = createLabel(id + "-text");
    setText(id + "-text", from + " " + fromPort + " → " + to + " " + toPort);
    setFontFamily(id + "-text", MONO);
    setFontSize(id + "-text", 12.5);
    setTextColor(id + "-text", C.text);
    return row;
}

/// The short musical reason a cable exists. Secondary to the connection above
/// it -- this is the teaching, and it should read as an aside rather than
/// competing with the wiring itself.
function wiringWhy(id, why) {
    const l = createLabel(id);
    setText(id, why);
    setFontFamily(id, FONT);
    setFontSize(id, 12);
    setTextColor(id, C.muted);
    setFlex(id, "margin_left", 16);
    setFlex(id, "margin_bottom", 6);
    return l;
}

function roleHeader(id, role) {
    const l = createLabel(id);
    setText(id, ROLE[role].label);
    setFontFamily(id, MONO);
    setFontSize(id, 10);
    setLetterSpacing(id, 0.1);
    setTextColor(id, ROLE[role].color);
    setFlex(id, "margin_top", 12);
    setFlex(id, "margin_bottom", 4);
    return l;
}

// ── composer ─────────────────────────────────────────────────────────────────

const composer = createCol("composer");
setBackground("composer", C.raised);
setBorder("composer", C.lineStrong, 1);
setCornerRadius("composer", 14);
setFlex("composer", "margin", 12);
setFlex("composer", "padding", 14);

const input = createTextEditor("prompt");
setFontFamily("prompt", FONT);
setFontSize("prompt", 14);
setTextColor("prompt", C.text);
setBackground("prompt", C.raised);
setFlex("prompt", "min_height", 46);

const actions = createRow("actions");
setFlex("actions", "align_items", "center");
setFlex("actions", "margin_top", 10);

function button(id, label, kind) {
    const b = createToggleButton(id);
    setBackground(id, kind === "primary" ? C.accent : C.panel);
    setBorder(id, kind === "primary" ? C.accent : C.line, 1);
    setCornerRadius(id, 10);
    setFlex(id, "padding_left", 15);
    setFlex(id, "padding_right", 15);
    setFlex(id, "padding_top", 9);
    setFlex(id, "padding_bottom", 9);
    setFlex(id, "margin_right", 8);
    setLabel(id, label);
    setTextColor(id, kind === "primary" ? C.onAccent : C.text);
    setFontFamily(id, FONT);
    setFontSize(id, 13);
    setFontWeight(id, 600);
    return b;
}

// @ opens the mention picker over 4,705 modules; Random follows the mode.
button("btn-mention", "@", "ghost");
button("btn-random", mode === "patch" ? "Random patch" : "Random module", "ghost");

const spacer = createRow("actions-gap");
setFlex("actions-gap", "flex_grow", 1);

// Two buttons rather than an inferred intent chip. A chip guesses and the user
// still has to notice the guess before sending; a labelled button just says.
// The asymmetry decides it: an unwanted answer costs nothing, an unwanted
// rebuild rewrites their work. See DECISIONS.md.
button("btn-ask", "Ask", "ghost");
button("btn-build", mode === "patch" ? "Build patch" : "Build module", "primary");

const hint = createLabel("composer-hint");
setText("composer-hint", "ASK ANSWERS · BUILD REWRITES. TWO BUTTONS SO NOTHING IS INFERRED.");
setFontFamily("composer-hint", MONO);
setFontSize("composer-hint", 9.5);
setLetterSpacing("composer-hint", 0.06);
setTextColor("composer-hint", C.faint);
setFlex("composer-hint", "margin_top", 8);

// ── preview ──────────────────────────────────────────────────────────────────
//
// In module mode this is the panel being built. In patch mode it is the rack:
// real vendor panel images at true widths with the cables drawn over them.
// A rack is a wide strip and this pane is not, so it fits rather than crops --
// the explanation beside it describes the whole patch, so the whole patch has
// to be visible.

const preview = createCol("preview");
setBackground("preview", C.sunken);
setBorder("preview", C.line, 1);
setCornerRadius("preview", 16);
setFlex("preview", "flex_grow", 1);
setFlex("preview", "margin_left", 14);

const previewBar = createRow("preview-bar");
setFlex("preview-bar", "padding", 12);
setFlex("preview-bar", "align_items", "center");

const previewTitle = createLabel("preview-title");
setText("preview-title", mode === "patch" ? "PATCH" : "PANEL");
setFontFamily("preview-title", MONO);
setFontSize("preview-title", 10);
setLetterSpacing("preview-title", 0.1);
setTextColor("preview-title", C.faint);

// Says how much of what is shown is exact. A preview that quietly guesses at
// jack positions would teach a wiring that is not there, so when any module is
// unmapped or has no image the caption says so rather than the picture
// implying a precision it does not have.
const fidelity = createLabel("preview-fidelity");
setText("preview-fidelity", "");
setFontFamily("preview-fidelity", MONO);
setFontSize("preview-fidelity", 9.5);
setTextColor("preview-fidelity", C.faint);
setFlex("preview-fidelity", "margin_left", 12);

const rack = createCanvas("rack");
setFlex("rack", "flex_grow", 1);

// ── mode ─────────────────────────────────────────────────────────────────────

function setMode(next) {
    mode = next;
    setBackground("tab-module", mode === "module" ? C.panel : C.appBg);
    setBackground("tab-patch", mode === "patch" ? C.panel : C.appBg);
    setTextColor("tab-module-name", mode === "module" ? C.textStrong : C.muted);
    setTextColor("tab-patch-name", mode === "patch" ? C.textStrong : C.muted);
    setLabel("btn-random", mode === "patch" ? "Random patch" : "Random module");
    setLabel("btn-build", mode === "patch" ? "Build patch" : "Build module");
    setText("preview-title", mode === "patch" ? "PATCH" : "PANEL");
}

/// Called by the host once a patch has been laid out. `exact` is false when
/// any module lacks an image or a port map.
function setFidelity(modules, exact, unmapped) {
    setText("preview-fidelity", exact
        ? modules + " MODULES · EXACT"
        : modules + " MODULES · " + unmapped + " NOT YET MAPPED");
}

setMode(mode);
