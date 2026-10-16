#!/usr/bin/env node
// Bounded validation for the authored fresh-plugin fixture. This deliberately
// starts from source HTML and never consumes a generated artifact as input.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { readFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { execFile } from "node:child_process";
import { promisify } from "node:util";

const exec = promisify(execFile);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const fixtureDefault = path.join(root, "test/fixtures/import-differential/17-fresh-plugin-panel.html");
const interactionsDefault = path.join(root, "test/fixtures/import-differential/17-fresh-plugin-panel.interactions.json");
const expectedReceiptDefault = path.join(root, "test/fixtures/import-differential/17-fresh-plugin-panel.receipt.json");
const capture = path.join(root, "tools/import-design/browser_capture/capture.mjs");

function sha256(bytes) { return createHash("sha256").update(bytes).digest("hex"); }
function arg(name, fallback = "") {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] ?? "" : fallback;
}
function browserPath() {
  const explicit = process.env.PULP_DESIGN_BROWSER || process.env.PULP_BROWSER;
  if (explicit) return explicit;
  const current = path.join(process.env.PULP_HOME || path.join(os.homedir(), ".pulp"),
    "tools/chrome-for-testing/current.json");
  const record = JSON.parse(readFileSync(current, "utf8"));
  assert.equal(typeof record.executable, "string");
  return path.join(path.dirname(current), record.executable);
}
async function runCapture(input, output, browser, withInteractions, interactionFile) {
  const args = [capture, "capture", "--browser", browser, "--input", input,
    "--root", path.dirname(input), "--output", output,
    "--initial-width", "480", "--initial-height", "320", "--dpr", "2",
    "--timeout-ms", "30000"];
  if (withInteractions) args.push("--interactions", interactionFile);
  return exec(process.execPath, args, { maxBuffer: 1024 * 1024 });
}
async function main() {
  const importer = arg("--importer");
  assert(importer, "--importer /absolute/path/to/pulp-import-design is required");
  const fixture = arg("--fixture", fixtureDefault);
  const interactions = arg("--interactions", interactionsDefault);
  const expectedReceiptDefaultForFixture = path.join(path.dirname(fixture), `${path.basename(fixture, path.extname(fixture))}.receipt.json`);
  const expectedReceiptPath = arg("--expected-receipt", fixture === fixtureDefault ? expectedReceiptDefault : expectedReceiptDefaultForFixture);
  const negativeSelector = arg("--negative-selector", 'id="toggle"');
  const negativeId = arg("--negative-id", "toggle-mutated");
  const browser = browserPath();
  const interactionPlan = JSON.parse(await readFile(interactions, "utf8"));
  const interactionCount = Array.isArray(interactionPlan.actions) ? interactionPlan.actions.length : 0;
  assert(interactionCount > 0, "interaction plan must contain a non-empty actions array");
  const source = await readFile(fixture);
  const sourceSha = sha256(source);
  const tmp = await mkdtemp(path.join(os.tmpdir(), "pulp-fresh-plugin-"));
  try {
    const browserOut = path.join(tmp, "browser");
    // execFile resolves only after the capture process has exited. The
    // capture helper performs its browser teardown in its finally block, so
    // reading these artifacts after this await is a process-level lifecycle
    // control, not a claim about native widget lifetime.
    const browserCaptureResult = await runCapture(fixture, browserOut, browser, true, interactions);
    const browserPng = await readFile(path.join(browserOut, "browser.png"));
    const interactionReport = await readFile(path.join(browserOut, "interaction-report.json"));
    const envelope = JSON.parse(await readFile(path.join(browserOut, "capture.json"), "utf8"));
    assert.equal(envelope.provenance.interactions.action_count, interactionCount);
    assert.equal(envelope.provenance.source.sha256, sourceSha);

    const nativeOut = path.join(tmp, "native.js");
    const native = await exec(importer, ["--from", "claude", "--file", fixture,
      "--emit", "js", "--output", nativeOut, "--validate",
      "--screenshot-backend", "skia", "--reference", path.join(browserOut, "browser.png"),
      "--render-size", "480x320",
      "--no-bridge-scaffold"],
      { maxBuffer: 4 * 1024 * 1024 });
    const nativeBytes = await readFile(nativeOut);
    assert(nativeBytes.length > 0);
    // The importer writes this alongside the generated artifact when --validate
    // runs. Retain the actual native Skia PNG as evidence; hashing the JS
    // artifact alone cannot prove that a native render occurred.
    const nativeRenderNames = (await readdir(tmp)).filter((name) => name.endsWith("-render.png"));
    assert.equal(nativeRenderNames.length, 1, `expected one native render PNG, found ${nativeRenderNames.join(", ")}`);
    const nativeRenderPath = path.join(tmp, nativeRenderNames[0]);
    const nativeRenderPng = await readFile(nativeRenderPath);
    assert(nativeRenderPng.length > 0);
    const importerOutput = `${native.stdout}\n${native.stderr}`;
    const similarityMatch = importerOutput.match(/Similarity:\s+(\d+)%\s+\((\d+)\/(\d+) pixels differ, mean error:\s+([0-9.]+)/);
    assert(similarityMatch, `native comparator result missing from importer output:\n${importerOutput}`);
    const [, similarityPercent, diffPixels, totalPixels, meanError] = similarityMatch;
    const negative = path.join(tmp, "negative.html");
    const sourceText = source.toString();
    assert(sourceText.includes(negativeSelector), `negative selector not found: ${negativeSelector}`);
    const mutated = sourceText.replace(negativeSelector, `id="${negativeId}"`);
    assert.notEqual(mutated, sourceText);
    await writeFile(negative, mutated);
    await assert.rejects(runCapture(negative, path.join(tmp, "negative-browser"), browser, true, interactions));

    const receipt = {
      schema: "pulp-fresh-plugin-validation-v3",
      fixture: path.basename(fixture),
      scope: { behavior: "panel-toggle", keyed_identity_reorder: "deferred" },
      source_sha256: sourceSha,
      source_bytes: source.length,
      browser: {
        backend: "chromium",
        viewport: { width: 480, height: 320, dpr: 2 },
        screenshot_sha256: sha256(browserPng),
        screenshot_bytes: browserPng.length,
        interaction_report_sha256: sha256(interactionReport),
        interaction_count: interactionCount,
        lifecycle: {
          capture_process_exited_cleanly: true,
          artifacts_read_after_process_exit: true,
          browser_health_status: envelope.provenance.health.status,
          runtime_exception_count: envelope.provenance.health.runtime_exceptions.length,
        },
      },
      native: {
        backend: "skia",
        artifact_sha256: sha256(nativeBytes),
        artifact_bytes: nativeBytes.length,
        render_sha256: sha256(nativeRenderPng),
        render_bytes: nativeRenderPng.length,
        lifecycle: {
          import_process_exited_cleanly: true,
          artifacts_read_after_process_exit: true,
        },
        comparator: {
          reference: "browser.screenshot_sha256",
          similarity_percent: Number(similarityPercent),
          diff_pixels: Number(diffPixels),
          total_pixels: Number(totalPixels),
          mean_error: Number(meanError),
        },
      },
      planted_negative: { mutated_id: negativeId, browser_capture_rejected: true },
    };
    const expectedReceipt = JSON.parse(await readFile(expectedReceiptPath, "utf8"));
    assert.deepEqual(receipt, expectedReceipt,
      "fresh-plugin receipt differs from the committed deterministic baseline");
    const out = arg("--receipt", path.join(process.cwd(), "fresh-plugin-validation.receipt.json"));
    await writeFile(out, JSON.stringify(receipt, null, 2) + "\n");
    process.stdout.write(JSON.stringify(receipt, null, 2) + "\n");
  } finally {
    await rm(tmp, { recursive: true, force: true });
  }
}
main().catch((error) => { console.error(error.stack || error); process.exitCode = 1; });
