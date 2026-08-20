#!/usr/bin/env node
// Regression coverage for DOM construction and the dependency-free DOM shim.
// Consumer-provided labels and links are data: they must never become markup or
// executable URL schemes in the shared WAM/WebCLAP shell.
import { document, ok, failed, audioNode, parseHTML } from "./dom-shim.mjs";

const { mountDemo } = await import("../src/shell.js");
const { createCombo } = await import("../src/widgets/combo.js");

function adapter() {
  return {
    descriptor: { name: "Mock", isInstrument: false, hasMidiOutput: true, hasAudioOutput: false },
    audioNode: audioNode(),
    getParameterInfo: async () => [
      { id: 1, label: "Bypass", type: "boolean", minValue: 0, maxValue: 1, defaultValue: 0 },
    ],
    setParameterValue() {}, getParameterValue: async () => 0,
    scheduleMidi() {}, sendSysex() {}, getState: async () => new Uint8Array(), setState() {},
    onMidiOut: null, onParamsChanged: null, createSecondary: async () => adapter(), destroy() {},
  };
}

document.body.childNodes = [];
const root = document.createElement("div");
document.body.appendChild(root);
const title = `Amp <img id="title-injection" src=x>`;
const subtitle = `<svg id="subtitle-injection"><script>bad()</script></svg>`;
const hostLabel = `<img id="host-injection" src=x>`;
await mountDemo({
  root, title, subtitle, hostLabel,
  galleryHref: `javascript:alert("gallery")`,
  hostDocsHref: `data:text/html,<script>alert("docs")</script>`,
  sourceHref: `javascript:alert("source")`,
  mode: "midi-effect", midiViz: "inspector", createAdapter: async () => adapter(),
});

ok(root.querySelector("h1").textContent === title, "shell: title is literal text");
ok(root.querySelector(".sub").textContent === subtitle, "shell: subtitle is literal text");
ok(!root.querySelector("#title-injection") && !root.querySelector("#subtitle-injection")
   && !root.querySelector("#host-injection") && !root.querySelector("#source-injection"),
   "shell: consumer strings cannot create elements");
ok(root.querySelector(".pp-top a").getAttribute("href") === "../index.html",
   "shell: an executable gallery URL falls back to the gallery default");
ok(!root.querySelector('.pp-top a[target="_blank"]'),
   "shell: invalid docs/source URLs are rendered as text or omitted, not links");
ok(root.querySelector("#overlay").getAttribute("aria-label") === `Start ${title}`,
   "shell: adversarial title remains literal in the accessible label");

// The mounted adapter is reachable through its installed callback; capture it
// explicitly with a second mount factory rather than adding a production seam.
globalThis.__player.destroy();
document.body.childNodes = [];
const midiRoot = document.createElement("div");
document.body.appendChild(midiRoot);
const midiAdapter = adapter();
await mountDemo({
  root: midiRoot, title: "MIDI", mode: "midi-effect", midiViz: "inspector",
  galleryHref: "../safe-gallery/", sourceHref: "https://github.com/Generous-Corp/pulp/tree/main/examples",
  createAdapter: async () => midiAdapter,
});
ok(midiRoot.querySelector("#pp-gallery").getAttribute("href") === "../safe-gallery/"
   && midiRoot.querySelector("#pp-source a").getAttribute("href")
     === "https://github.com/Generous-Corp/pulp/tree/main/examples",
   "shell: safe relative and HTTPS links preserve their intended destinations");
await globalThis.__start();
midiAdapter.onMidiOut([{ bytes: new Uint8Array([0x90, 60, 100]) }]);
const logRow = midiRoot.querySelector("#log .ev");
ok(logRow && logRow.children.length === 3, "MIDI log: event fields are separate DOM nodes");
ok(logRow.querySelector(".nm").textContent === "Note On"
   && logRow.querySelector(".ch").textContent === "ch 1"
   && logRow.querySelector(".by").textContent === "90 3C 64",
   "MIDI log: event data is written as text");

const combo = createCombo({
  param: { labels: [`Clean <img id="combo-injection" src=x>`, "Drive"], minValue: 0, step: 1 },
  value: 0, onChange() {},
});
document.body.appendChild(combo);
combo.dispatchEvent({ type: "pointerdown", preventDefault() {} });
const option = document.body.querySelector('.pw-combo-panel [role="option"]');
ok(option && option.children.length === 2, "combo: checkmark and label are separate DOM nodes");
ok(option.children[1].textContent === `Clean <img id="combo-injection" src=x>`,
   "combo: choice label is literal text");
ok(!document.body.querySelector("#combo-injection"), "combo: choice label cannot create elements");

const spaced = `<div${" ".repeat(50000)}id="spaced">ok</div>`;
let started = performance.now();
const parsed = parseHTML(spaced);
ok(performance.now() - started < 1000, "DOM shim: repeated attribute whitespace parses in bounded time");
ok(parsed[0]?.id === "spaced" && parsed[0]?.textContent === "ok",
   "DOM shim: attribute parsing semantics survive the linear scanner");

started = performance.now();
const malformed = document.body.querySelector("[".repeat(50000));
ok(performance.now() - started < 1000 && malformed === null,
   "DOM shim: repeated selector brackets are rejected in bounded time");

globalThis.__player.destroy();
console.log(failed ? `\n${failed} assertion(s) FAILED` : "\nDOM security regressions intact — all assertions passed");
process.exit(failed ? 1 : 0);
