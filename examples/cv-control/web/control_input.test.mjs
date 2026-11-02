import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { sendControlSample, sendMultiValueSample } from "./control_input.mjs";

const events = [];
const adapter = { scheduleMidi: (...bytes) => events.push(bytes) };
sendControlSample(adapter, 1, true);
sendControlSample(adapter, .5, true);
sendControlSample(adapter, Number.NaN, true);
sendControlSample(adapter, .7, false);
assert.deepEqual(events[0], [0xEF, 0x7F, 0x7F]);
assert.deepEqual(events[1], [0xEF, 0, 0x60]);
assert.deepEqual(events[2], [0xEF, 0, 0]);
assert.deepEqual(events[3], [0xEF, 0, 0]);

const sysex = [];
const sysexAdapter = { sendSysex: (bytes) => sysex.push(bytes) };
assert.equal(sendMultiValueSample(sysexAdapter, [0.1, 0.2, 0.3], { sequence: 7 }), true);
assert.equal(sysex.length, 1);
assert.equal(sysex[0][0], 0xf0);
assert.equal(sysex[0].at(-1), 0xf7);
assert.equal(new DataView(sysex[0].buffer).getUint8(34), 3);
assert.equal(sendMultiValueSample(sysexAdapter, new Array(17).fill(0)), false);
assert.equal(sendMultiValueSample(sysexAdapter, [Number.NaN]), false);

// The processor imports a sibling DSP module; verify assembly actually
// supplies it instead of merely passing an unused DSP URL to the adapter.
const temp = await mkdtemp(join(tmpdir(), "pulp-cv-wam-"));
try {
  const source = join(temp, "source.js");
  const output = join(temp, "out");
  await writeFile(source, "export default function dsp() {}\n");
  execFileSync(process.execPath, [fileURLToPath(new URL("./assemble.mjs", import.meta.url)),
    source, output]);
  assert.equal(await readFile(join(output, "wam-dsp.js"), "utf8"),
    "export default function dsp() {}\n");
  const processor = await readFile(join(output, "wam-processor.js"), "utf8");
  assert.match(processor, /from "\.\/wam-dsp\.js"/);
  const runtime = await readFile(join(output, "wam-runtime.mjs"), "utf8");
  assert.match(runtime, /mem\.grow\(/); // copy Pulp's current runtime, not a stale vendor copy
} finally {
  await rm(temp, { recursive: true, force: true });
}
