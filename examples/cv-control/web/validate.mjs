// Browser proof of the assembled WAM DSP and the shared C++ control path.
// The page's real camera provider remains a manual permission/hardware check.
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { createServer } from "node:net";
import { dirname, resolve } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright-core";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../../..");
for (const name of ["wam-dsp.js", "wam-processor.js", "wam-runtime.mjs"]) {
  assert.ok(existsSync(resolve(here, "wam", name)), `assemble.mjs must create ${name}`);
}

const browserPath = [process.env.CHROME_PATH, "/usr/bin/google-chrome",
  "/usr/bin/chromium", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"]
  .find((path) => path && existsSync(path));
assert.ok(browserPath, "set CHROME_PATH to a Chrome/Chromium executable");

const port = await new Promise((resolvePort, reject) => {
  const listener = createServer();
  listener.once("error", reject);
  listener.listen(0, "127.0.0.1", () => {
    const available = listener.address().port;
    listener.close(() => resolvePort(available));
  });
});
const server = spawn("python3", ["-m", "http.server", String(port), "--bind", "127.0.0.1",
  "--directory", root], { stdio: "ignore" });
let browser;
try {
  await sleep(400);
  browser = await chromium.launch({ executablePath: browserPath, headless: true });
  const page = await browser.newPage();
  page.on("pageerror", (error) => console.error("browser error:", error));
  await page.goto(`http://127.0.0.1:${port}/examples/cv-control/web/`);
  const levels = await page.evaluate(async () => {
    const { createWamAdapter } = await import("/packages/pulp-web-player/src/adapters/wam.js");
    const { sendControlSample } = await import("/examples/cv-control/web/control_input.mjs");
    async function render(value) {
      const ctx = new OfflineAudioContext(2, 8192, 48000);
      const adapter = await createWamAdapter(ctx, {
        dsp: "./wam/wam-dsp.js", processor: "./wam/wam-processor.js"
      });
      adapter.audioNode.connect(ctx.destination);
      adapter.setParameterValue(100, 0.2);
      adapter.scheduleMidi(0x90, 60, 100);
      sendControlSample(adapter, value, true);
      const audio = await ctx.startRendering();
      adapter.destroy();
      // The DSP initializes asynchronously; compare the steady render tail.
      const all = audio.getChannelData(0);
      const samples = all.subarray(all.length / 2);
      let sum = 0;
      for (const sample of samples) sum += sample * sample;
      return Math.sqrt(sum / samples.length);
    }
    return [await render(0), await render(1)];
  });
  assert.ok(Number.isFinite(levels[0]) && levels[0] > 0.001, `silent WAM: ${levels}`);
  assert.ok(levels[1] > levels[0] * 1.1, `control did not modulate WAM: ${levels}`);
  console.log(`CV WAM browser smoke passed: baseline=${levels[0]}, tracked=${levels[1]}`);
} finally {
  await browser?.close();
  server.kill("SIGTERM");
}
