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
// The root has to claim the window, or every child lays out against zero and
// the whole shell huddles in the top corner.
setFlex("root", "width", "100%");
setFlex("root", "height", "100%");

// ── tabs ─────────────────────────────────────────────────────────────────────
//
// The tabs and the composer are one shape, not two. An earlier prototype gave
// them top-only radii and no bottom border -- every part of which says "these
// join" -- and then held them 12px apart, so they read as floating buttons
// above an unrelated box. They share an edge here.

const tabs = createRow("tabs", "root");
setFlex("tabs", "align_items", "flex_end");
setFlex("tabs", "padding_left", 20);
setFlex("tabs", "padding_top", 18);

function tab(id, parent, title, sub, active) {
    const t = createRow(id, parent);
    setBackground(id, active ? C.panel : C.appBg);
    setBorder(id, active ? C.lineStrong : C.line, 1);
    setCornerRadius(id, 12);
    setFlex(id, "padding_left", 16);
    setFlex(id, "padding_right", 16);
    setFlex(id, "padding_top", 9);
    setFlex(id, "padding_bottom", 9);
    setFlex(id, "align_items", "center");

    createLabel(id + "-name", title, id);
    setFontFamily(id + "-name", FONT);
    setFontSize(id + "-name", 13.5);
    setFontWeight(id + "-name", 600);
    setTextColor(id + "-name", active ? C.textStrong : C.muted);

    createLabel(id + "-sub", sub, id);
    setFontFamily(id + "-sub", MONO);
    setFontSize(id + "-sub", 9.5);
    setLetterSpacing(id + "-sub", 0.08);
    setTextColor(id + "-sub", C.faint);
    setFlex(id + "-sub", "margin_left", 9);
    return t;
}

tab("tab-module", "tabs", "Module", "ONE PANEL", mode === "module");
tab("tab-patch", "tabs", "Patch", "A WHOLE RACK", mode === "patch");

// ── body: chat on the left, preview on the right ─────────────────────────────

const body = createRow("body", "root");
setFlex("body", "flex_grow", 1);
setFlex("body", "padding_left", 20);
setFlex("body", "padding_right", 20);
setFlex("body", "padding_bottom", 20);

// ── chat ─────────────────────────────────────────────────────────────────────

const chat = createCol("chat", "body");
setBackground("chat", C.panel);
setBorder("chat", C.lineStrong, 1);
// Square top-left: this is the selected tab's panel, and the corner is the joint.
setCornerRadius("chat", 16);
setFlex("chat", "width", 380);
setFlex("chat", "flex_direction", "column");

const history = createScrollView("history", "chat");
setFlex("history", "flex_grow", 1);
setFlex("history", "padding", 16);

/// One wiring line, as the explanation prints it. Monospace because the arrows
/// have to align down the column for the shape of a patch to be readable.
function wiringLine(id, from, fromPort, to, toPort, color) {
    const row = createRow(id, "history");
    setFlex(id, "align_items", "center");
    setFlex(id, "padding_top", 2);
    setFlex(id, "padding_bottom", 2);

    const dot = createPanel(id + "-dot", id);
    setBackground(id + "-dot", color);
    setCornerRadius(id + "-dot", 3);
    setFlex(id + "-dot", "width", 6);
    setFlex(id + "-dot", "height", 6);
    setFlex(id + "-dot", "margin_right", 10);

    createLabel(id + "-text", from + " " + fromPort + " → " + to + " " + toPort, id);
    setFontFamily(id + "-text", MONO);
    setFontSize(id + "-text", 12.5);
    setTextColor(id + "-text", C.text);
    return row;
}

/// The short musical reason a cable exists. Secondary to the connection above
/// it -- this is the teaching, and it should read as an aside rather than
/// competing with the wiring itself.
function wiringWhy(id, why) {
    createLabel(id, why, "history");
    setFontFamily(id, FONT);
    setFontSize(id, 12);
    setTextColor(id, C.muted);
    setFlex(id, "margin_left", 16);
    setFlex(id, "margin_bottom", 6);
    return l;
}

function roleHeader(id, role) {
    createLabel(id, ROLE[role].label, "history");
    setFontFamily(id, MONO);
    setFontSize(id, 10);
    setLetterSpacing(id, 0.1);
    setTextColor(id, ROLE[role].color);
    setFlex(id, "margin_top", 12);
    setFlex(id, "margin_bottom", 4);
    return l;
}

// ── composer ─────────────────────────────────────────────────────────────────

const composer = createCol("composer", "chat");
setBackground("composer", C.raised);
setBorder("composer", C.lineStrong, 1);
setCornerRadius("composer", 14);
setFlex("composer", "margin", 12);
setFlex("composer", "padding", 14);

const input = createTextEditor("prompt", "composer");
setFontFamily("prompt", FONT);
setFontSize("prompt", 14);
setTextColor("prompt", C.text);
setBackground("prompt", C.raised);
setFlex("prompt", "min_height", 46);

const actions = createRow("actions", "composer");
setFlex("actions", "align_items", "center");
setFlex("actions", "margin_top", 10);

// Two rows: tools while composing, then the two ways to send. One row cannot
// hold four buttons in a 380px column without clipping their labels, and a
// button whose label is cut is worse than one placed a line lower.
const sendRow = createRow("send-row", "composer");
setFlex("send-row", "align_items", "center");
setFlex("send-row", "justify_content", "flex_end");
setFlex("send-row", "margin_top", 8);

function button(id, parent, label, kind) {
    const b = createToggleButton(id, parent);
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
button("btn-mention", "actions", "@", "ghost");
button("btn-random", "actions", mode === "patch" ? "Random patch" : "Random", "ghost");



// Two buttons rather than an inferred intent chip. A chip guesses and the user
// still has to notice the guess before sending; a labelled button just says.
// The asymmetry decides it: an unwanted answer costs nothing, an unwanted
// rebuild rewrites their work. See DECISIONS.md.
button("btn-ask", "send-row", "Ask", "ghost");
button("btn-build", "send-row", mode === "patch" ? "Build patch" : "Build module", "primary");

createLabel("composer-hint", "ASK ANSWERS · BUILD REWRITES. TWO BUTTONS SO NOTHING IS INFERRED.", "composer");
setFontFamily("composer-hint", MONO);
setFontSize("composer-hint", 9.5);
setLetterSpacing("composer-hint", 0.06);
setTextColor("composer-hint", C.faint);
setFlex("composer-hint", "margin_top", 8);
setFlex("composer-hint", "flex_wrap", "wrap");

// ── preview ──────────────────────────────────────────────────────────────────
//
// In module mode this is the panel being built. In patch mode it is the rack:
// real vendor panel images at true widths with the cables drawn over them.
// A rack is a wide strip and this pane is not, so it fits rather than crops --
// the explanation beside it describes the whole patch, so the whole patch has
// to be visible.

const preview = createCol("preview", "body");
setBackground("preview", C.sunken);
setBorder("preview", C.line, 1);
setCornerRadius("preview", 16);
setFlex("preview", "flex_grow", 1);
setFlex("preview", "margin_left", 14);

const previewBar = createRow("preview-bar", "preview");
setFlex("preview-bar", "padding", 12);
setFlex("preview-bar", "align_items", "center");

createLabel("preview-title", mode === "patch" ? "PATCH" : "PANEL", "preview-bar");
setFontFamily("preview-title", MONO);
setFontSize("preview-title", 10);
setLetterSpacing("preview-title", 0.1);
setTextColor("preview-title", C.faint);

// Says how much of what is shown is exact. A preview that quietly guesses at
// jack positions would teach a wiring that is not there, so when any module is
// unmapped or has no image the caption says so rather than the picture
// implying a precision it does not have.
createLabel("preview-fidelity", "", "preview-bar");
setFontFamily("preview-fidelity", MONO);
setFontSize("preview-fidelity", 9.5);
setTextColor("preview-fidelity", C.faint);
setFlex("preview-fidelity", "margin_left", 12);

const rack = createCanvas("rack", "preview");
setFlex("rack", "flex_grow", 1);

// ── mode ─────────────────────────────────────────────────────────────────────

function setMode(next) {
    mode = next;
    setBackground("tab-module", mode === "module" ? C.panel : C.appBg);
    setBackground("tab-patch", mode === "patch" ? C.panel : C.appBg);
    setTextColor("tab-module-name", mode === "module" ? C.textStrong : C.muted);
    setTextColor("tab-patch-name", mode === "patch" ? C.textStrong : C.muted);
    setLabel("btn-random", mode === "patch" ? "Random patch" : "Random");
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

// ── mention picker ───────────────────────────────────────────────────────────
//
// Typing @ searches 4,705 modules across 543 plugins, indexed locally so it is
// instant and works offline. Three states, and only one of them can be wired:
//
//   ✓ ready    installed
//   ↓ free     free, not installed
//   $ premium  costs money, or comes with VCV+
//
// The copy never says "you need to buy this". We cannot tell an unsynced
// module someone already owns from one they have never bought, and telling
// somebody to purchase what they own is a worse error than telling them to
// check. See DECISIONS.md.

const mention = createPanel("mention", "chat");
setBackground("mention", C.raised);
setBorder("mention", C.lineStrong, 1);
setCornerRadius("mention", 14);
setFlex("mention", "padding", 6);
setVisible("mention", false);   // shown while an @ query is live

const mentionQuery = createRow("mention-query", "mention");
setFlex("mention-query", "align_items", "center");
setFlex("mention-query", "padding", 10);

createLabel("mention-at", "@", "mention-query");
setFontFamily("mention-at", MONO);
setFontSize("mention-at", 13);
setTextColor("mention-at", C.accent);

createLabel("mention-text", "", "mention-query");
setFontFamily("mention-text", MONO);
setFontSize("mention-text", 13);
setTextColor("mention-text", C.textStrong);
setFlex("mention-text", "margin_left", 4);

createLabel("mention-count", "", "mention-query");
setFontFamily("mention-count", MONO);
setFontSize("mention-count", 11);
setTextColor("mention-count", C.faint);
setFlex("mention-count", "margin_left", "auto");

const mentionList = createScrollView("mention-list", "mention");
setFlex("mention-list", "max_height", 260);

const STATE = {
    ready:   { mark: "✓", color: "#3FCF77", note: "" },
    free:    { mark: "↓", color: "#5E78FF",
               note: "install from Rack's Library, then rescan" },
    premium: { mark: "$", color: "#F6B847",
               note: "own it? sync it in Rack's Library. Otherwise buy it, or VCV+" },
};

/// One result. Only `ready` can go into a patch; the others are an offer.
///
/// Not a wall: a patch that names a module the user lacks still opens in Rack,
/// which lists what is missing, offers the Library, and keeps the cables so
/// installing later completes it. So this reads as a better option while
/// building is cheap, never as a refusal.
function mentionRow(id, state, plugin, model, name, tags) {
    const row = createRow(id, "mention-list");
    setFlex(id, "align_items", "center");
    setFlex(id, "padding_left", 12);
    setFlex(id, "padding_right", 12);
    setFlex(id, "padding_top", 8);
    setFlex(id, "padding_bottom", 8);
    setCornerRadius(id, 9);

    createLabel(id + "-mark", STATE[state].mark, id);
    setFontFamily(id + "-mark", MONO);
    setFontSize(id + "-mark", 12);
    setTextColor(id + "-mark", STATE[state].color);
    setFlex(id + "-mark", "width", 18);

    createLabel(id + "-slug", plugin + "/" + model, id);
    setFontFamily(id + "-slug", MONO);
    setFontSize(id + "-slug", 12);
    setTextColor(id + "-slug", state === "ready" ? C.text : C.muted);

    createLabel(id + "-name", name, id);
    setFontFamily(id + "-name", FONT);
    setFontSize(id + "-name", 12.5);
    setTextColor(id + "-name", state === "ready" ? C.textStrong : C.muted);
    setFlex(id + "-name", "margin_left", 10);

    createLabel(id + "-tags", tags, id);
    setFontFamily(id + "-tags", MONO);
    setFontSize(id + "-tags", 10);
    setTextColor(id + "-tags", C.faint);
    setFlex(id + "-tags", "margin_left", "auto");
    return row;
}

/// Shown when a capability the request needs is not installed at all. Cheap to
/// produce -- it is tag matching over local data, so nothing is spent finding
/// out -- and it happens before any generation rather than after.
const gap = createPanel("capability-gap", "history");
setBackground("capability-gap", C.panel);
setBorder("capability-gap", C.line, 1);
setCornerRadius("capability-gap", 12);
setFlex("capability-gap", "padding", 12);
setVisible("capability-gap", false);

createLabel("capability-gap-text", "", "capability-gap");
setFontFamily("capability-gap-text", FONT);
setFontSize("capability-gap-text", 12.5);
setTextColor("capability-gap-text", C.text);

// ── settings ─────────────────────────────────────────────────────────────────
//
// Forge's AI settings are inherited wholesale -- account menu, providers,
// first-run gating, reasoning effort, credentials. Only the model roles differ,
// and for a reason: Forge splits Engineering from UI designer because its
// interface is JavaScript a model writes. Here a panel is emitted
// deterministically from its manifest, so there is no UI model to pick and
// nothing to run in parallel with the DSP.
//
// The split that fits is by artifact:
//   Module engineer — one call returning the manifest and the DSP together.
//   Patch designer  — reading an inventory, choosing modules, wiring them, and
//                     explaining why. Musical judgement more than C++, and
//                     plausibly a different model.

const settings = createPanel("settings", "root");
setBackground("settings", C.panel);
setBorder("settings", C.lineStrong, 1);
setCornerRadius("settings", 16);
setFlex("settings", "padding", 18);
setVisible("settings", false);

function settingRow(id, title, detail, value) {
    const row = createRow(id, "settings");
    setFlex(id, "align_items", "center");
    setFlex(id, "padding_top", 11);
    setFlex(id, "padding_bottom", 11);
    setFlex(id, "border_bottom", 1);

    createLabel(id + "-title", title, id);
    setFontFamily(id + "-title", FONT);
    setFontSize(id + "-title", 14);
    setFontWeight(id + "-title", 600);
    setTextColor(id + "-title", C.textStrong);

    createLabel(id + "-detail", detail, id);
    setFontFamily(id + "-detail", FONT);
    setFontSize(id + "-detail", 12);
    setTextColor(id + "-detail", C.muted);

    createLabel(id + "-value", value, id);
    setFontFamily(id + "-value", MONO);
    setFontSize(id + "-value", 12);
    setTextColor(id + "-value", C.muted);
    setFlex(id + "-value", "margin_left", "auto");
    return row;
}

settingRow("set-engineer", "Module engineer",
           "Writes the manifest and the DSP", "—");
settingRow("set-designer", "Patch designer",
           "Chooses modules, wires them, and explains why", "—");
settingRow("set-effort", "Reasoning effort", "Low → Max", "Medium");

// Per-patch, because the right answer changes per patch. Persisted with the
// project so reopening restores how it was built.
settingRow("set-prefer", "Prefer my own modules",
           "Or let the whole installed library compete evenly", "on");
settingRow("set-create", "Build a module when one is missing",
           "Off by default: patch building is seconds, module building is a "
           + "minute-plus compile", "off");
settingRow("set-depth", "Explanation depth",
           "Terse · Standard · Learning", "Standard");
