// Test for wam_build_report.mjs (the WAMv2 web-build report generator).
//
// Needs a built WAM module (the generator reads its real descriptor +
// parameters), so it takes --wasm or auto-locates the example build. It runs the
// generator in both modes and asserts the report contract: the generated-controls
// strategy, the parameter binding targets, and the native-editor fallback note.
//
// Usage: node wam_build_report.test.mjs [--wasm <plugin.wasm>]   (exit 0 = PASS)
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const GEN = fileURLToPath(new URL("./wam_build_report.mjs", import.meta.url));
function arg(flag) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : null;
}
const repoRoot = fileURLToPath(new URL("../../../../", import.meta.url));
const wasm = arg("--wasm") ||
  [join(repoRoot, "examples/web-demos/wasm-build/build/PulpGain.wasm")].find(existsSync);
if (!wasm || !existsSync(wasm)) {
  console.error("SKIP: no built WAM module found — build PulpGain-wam first or pass --wasm.");
  process.exit(2);
}

let failures = 0;
const check = (cond, msg) => { if (!cond) { console.error("  ✗ " + msg); failures++; } else console.log("  ✓ " + msg); };

const work = await mkdtemp(join(tmpdir(), "wam-report-"));
try {
  const gen = async (extra) => {
    const outPath = join(work, "r.json");
    const r = spawnSync(process.execPath, [GEN, "--wasm", wasm, "--out", outPath, ...extra], { encoding: "utf8" });
    check(r.status === 0, `generator exit 0 (${extra.join(" ") || "default"})`);
    return JSON.parse(await readFile(outPath, "utf8"));
  };

  const base = await gen([]);
  check(base.plugin === "PulpGain", "plugin name is PulpGain");
  check(base.format === "wam", "format is wam");
  check(base.ui.strategy === "generated-controls", "ui strategy is generated-controls");
  check(base.ui.nativeEditorDeclared === false, "nativeEditorDeclared false by default");
  check(Array.isArray(base.parameters) && base.parameters.length >= 1, "parameters listed");
  const p = base.parameters[0];
  check(p && "id" in p && "label" in p && "minValue" in p && "maxValue" in p,
    "parameter has id/label/min/max (binding target contract)");
  check(/binding targets/.test(base.ui.note), "note explains the param binding-target contract");

  const ed = await gen(["--native-editor"]);
  check(ed.ui.nativeEditorDeclared === true, "nativeEditorDeclared true with --native-editor");
  check(/native editor/i.test(ed.ui.note) && /generated/i.test(ed.ui.note),
    "native-editor note explains the generated-controls fallback");
  check(JSON.stringify(ed.parameters) === JSON.stringify(base.parameters),
    "parameter binding targets are identical regardless of editor declaration");

  console.log(failures === 0 ? "PASS: wam_build_report.mjs" : `FAIL: ${failures} check(s)`);
  process.exit(failures === 0 ? 0 : 1);
} finally {
  await rm(work, { recursive: true, force: true });
}
