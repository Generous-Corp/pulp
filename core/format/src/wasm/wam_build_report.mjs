// Emit a "web-build report" for a WAMv2 plugin.
//
// When an existing Pulp plugin is built for the web, two things about its UI are
// worth stating explicitly, because they are easy to get wrong silently:
//
//   1. A NATIVE editor (Processor::create_view → a Skia/Dawn or platform view)
//      cannot run in a headless web build. The web host shows GENERATED controls
//      instead. A developer who imported or hand-built a native editor should
//      see, at build time, that the web target falls back to generated controls.
//
//   2. The plugin's PARAMETERS are the stable binding targets. A design-import
//      UI binds a knob/slider to a parameter by id; the WAM generated controls
//      are built from the same parameter metadata, so a binding to parameter id
//      N routes to the generated control for parameter id N. The report lists
//      those ids/labels/ranges so the binding contract is visible and testable.
//
// The report is data only — it reads the built module's descriptor + parameters
// through the same bridge the worklet/host use, so it cannot drift from what the
// plugin actually exposes. Whether the plugin has a native editor is a developer
// declaration (--native-editor), since a headless WAM build has no view to
// introspect.
//
// Usage:
//   node wam_build_report.mjs --wasm <plugin.wasm> --out <report.json> [--native-editor]
// Exit 0 on success; prints the report path.
import { readFileSync, writeFileSync } from "node:fs";
import { makeWasmImports, makeBridge } from "./wam-runtime.mjs";

function arg(flag, dflt = null) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const wasm = arg("--wasm");
const out = arg("--out");
const nativeEditor = process.argv.includes("--native-editor");

if (!wasm || !out) {
  console.error("usage: node wam_build_report.mjs --wasm <plugin.wasm> --out <report.json> [--native-editor]");
  process.exit(2);
}

let instance = new WebAssembly.Instance(
  new WebAssembly.Module(readFileSync(wasm)),
  makeWasmImports(() => instance.exports.memory));
const wam = makeBridge(instance.exports);
wam.callCtors(); // --no-entry build: construct the global bridge before init
if (!wam.init(48000, 128)) { console.error("FAIL: wam_init failed"); process.exit(1); }

const descriptor = JSON.parse(wam.descriptorJson());
const parameters = JSON.parse(wam.parametersJson()).map((p) => ({
  id: p.id, label: p.label, type: p.type, unit: p.unit,
  defaultValue: p.defaultValue, minValue: p.minValue, maxValue: p.maxValue,
}));

const note = nativeEditor
  ? "This plugin declares a native editor (Processor::create_view). A native editor " +
    "cannot run in a headless web build, so the web host renders generated parameter " +
    "controls instead. The parameters below are the binding targets a design-import UI " +
    "maps to by id."
  : "No native editor declared. The web host renders generated parameter controls for the " +
    "parameters below — the binding targets a design-import UI maps to by id.";

const report = {
  plugin: descriptor.name,
  format: "wam",
  descriptor,
  ui: {
    strategy: "generated-controls",
    nativeEditorDeclared: nativeEditor,
    note,
  },
  // The parameter contract that survives into the web build's generated controls.
  parameters,
};

writeFileSync(out, JSON.stringify(report, null, 2) + "\n");
console.log(`web-build report: ${out}`);
console.log(`  ${descriptor.name} (wam) — ${parameters.length} parameter binding target(s); ` +
            `native editor ${nativeEditor ? "declared → generated-controls fallback" : "not declared"}`);
console.log("PASS: web-build report written");
