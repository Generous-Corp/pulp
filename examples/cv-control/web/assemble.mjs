import { copyFile, mkdir, stat } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

// WAM's processor imports ./wam-dsp.js. The adapter's dsp URL is metadata;
// loading the processor alone cannot find a DSP in a different directory.
const here = dirname(fileURLToPath(import.meta.url));
const dsp = resolve(process.argv[2] || join(here,
  "../../web-demos/wasm-build/build/CvFaceToneWorklet.js"));
const out = resolve(process.argv[3] || join(here, "wam"));
const runtime = resolve(here, "../../../core/format/src/wasm");

await stat(dsp);
await mkdir(out, { recursive: true });
await copyFile(dsp, join(out, "wam-dsp.js"));
for (const name of ["wam-processor.js", "wam-runtime.mjs"]) {
  await copyFile(join(runtime, name), join(out, name));
}
console.log(`Assembled CV WAM assets in ${out}`);
