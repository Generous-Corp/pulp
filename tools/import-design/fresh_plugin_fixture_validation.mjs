#!/usr/bin/env node
// Bounded validation for the authored fresh-plugin fixture. This deliberately
// starts from source HTML and never consumes a generated artifact as input.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { readFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { execFile } from "node:child_process";
import { promisify } from "node:util";

const exec = promisify(execFile);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const fixture = path.join(root, "test/fixtures/import-differential/17-fresh-plugin-panel.html");
const interactions = path.join(root, "test/fixtures/import-differential/17-fresh-plugin-panel.interactions.json");
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
async function runCapture(input, output, browser, withInteractions) {
  const args = [capture, "capture", "--browser", browser, "--input", input,
    "--root", path.dirname(input), "--output", output,
    "--initial-width", "480", "--initial-height", "320", "--dpr", "2",
    "--timeout-ms", "30000"];
  if (withInteractions) args.push("--interactions", interactions);
  return exec(process.execPath, args, { maxBuffer: 1024 * 1024 });
}
async function main() {
  const importer = arg("--importer");
  assert(importer, "--importer /absolute/path/to/pulp-import-design is required");
  const browser = browserPath();
  const source = await readFile(fixture);
  const sourceSha = sha256(source);
  const tmp = await mkdtemp(path.join(os.tmpdir(), "pulp-fresh-plugin-"));
  try {
    const browserOut = path.join(tmp, "browser");
    await runCapture(fixture, browserOut, browser, true);
    const browserPng = await readFile(path.join(browserOut, "browser.png"));
    const interactionReport = await readFile(path.join(browserOut, "interaction-report.json"));
    const envelope = JSON.parse(await readFile(path.join(browserOut, "capture.json"), "utf8"));
    assert.equal(envelope.provenance.interactions.action_count, 2);
    assert.equal(envelope.provenance.source.sha256, sourceSha);

    const nativeOut = path.join(tmp, "native.js");
    const native = await exec(importer, ["--from", "claude", "--file", fixture,
      "--emit", "js", "--output", nativeOut, "--validate",
      "--screenshot-backend", "skia", "--no-bridge-scaffold"],
      { maxBuffer: 4 * 1024 * 1024 });
    const nativeBytes = await readFile(nativeOut);
    assert(nativeBytes.length > 0);
    const negative = path.join(tmp, "negative.html");
    const mutated = source.toString().replace('id="toggle"', 'id="toggle-mutated"');
    await writeFile(negative, mutated);
    await assert.rejects(runCapture(negative, path.join(tmp, "negative-browser"), browser, true));

    const receipt = {
      schema: "pulp-fresh-plugin-validation-v1",
      fixture: "17-fresh-plugin-panel.html",
      source_sha256: sourceSha,
      source_bytes: source.length,
      browser: {
        backend: "chromium",
        viewport: { width: 480, height: 320, dpr: 2 },
        screenshot_sha256: sha256(browserPng),
        screenshot_bytes: browserPng.length,
        interaction_report_sha256: sha256(interactionReport),
        interaction_count: 2,
      },
      native: { backend: "skia", artifact_sha256: sha256(nativeBytes), artifact_bytes: nativeBytes.length },
      planted_negative: { mutated_id: "toggle-mutated", browser_capture_rejected: true },
    };
    const out = arg("--receipt", path.join(process.cwd(), "fresh-plugin-validation.receipt.json"));
    await writeFile(out, JSON.stringify(receipt, null, 2) + "\n");
    const expected = arg("--expected-receipt");
    if (expected) {
      const expectedReceipt = JSON.parse(await readFile(expected, "utf8"));
      assert.deepEqual(receipt, expectedReceipt,
        "fresh-plugin receipt differs from the committed deterministic baseline");
    }
    process.stdout.write(JSON.stringify(receipt, null, 2) + "\n");
  } finally {
    await rm(tmp, { recursive: true, force: true });
  }
}
main().catch((error) => { console.error(error.stack || error); process.exitCode = 1; });
