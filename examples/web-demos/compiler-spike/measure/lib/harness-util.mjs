// harness-util.mjs — shared CLI + headless-Chrome plumbing for the harnesses.
//
// Node-only (imports fs/child bits via playwright). Keeps each harness driver
// small: arg parsing, Chrome discovery, and a launch helper that runs a page
// function against the served measure page and returns its JSON result.

import { existsSync } from "node:fs";

export function arg(flag, dflt = null) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
export function flag(name) { return process.argv.includes(name); }

const CHROME_CANDIDATES = () => [
  arg("--browser"),
  process.env.PLAYWRIGHT_CHROMIUM_PATH,
  process.env.CHROME_PATH,
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
  "/usr/bin/google-chrome", "/usr/bin/chromium-browser", "/usr/bin/chromium",
].filter(Boolean);

export function findChrome() {
  return CHROME_CANDIDATES().find((p) => existsSync(p)) || null;
}

// Dynamically import playwright-core so a missing install produces a clear
// harness error (exit non-zero) rather than a module-resolution stack trace.
export async function importChromium() {
  try {
    const mod = await import("playwright-core");
    return mod.chromium;
  } catch (e) {
    throw new Error(
      "playwright-core not installed. Run `npm install` in the measure/ dir " +
      "(or set NODE_PATH to a checkout that has it). Underlying: " + e.message);
  }
}

// Launch headless system Chrome, open `pageUrl`, wait until window.__ready, run
// `fn(page)` and return its value. Always closes the browser. `headed` shows it.
export async function withChromePage(pageUrl, fn, { headed = false, port = 8794 } = {}) {
  const chromium = await importChromium();
  const exe = findChrome();
  if (!exe) throw new Error("no Chrome/Chromium binary found (set CHROME_PATH or --browser)");
  const { startServer } = await import("./serve.mjs");
  const server = await startServer(port);
  const browser = await chromium.launch({
    executablePath: exe,
    headless: !headed,
    args: ["--autoplay-policy=no-user-gesture-required"],
  });
  try {
    const page = await browser.newPage();
    page.on("console", (m) => console.log("  [page]", m.text()));
    page.on("pageerror", (e) => console.log("  [pageerror]", e.message));
    await page.goto(pageUrl, { waitUntil: "load" });
    await page.waitForFunction(() => window.__ready === true || window.__error, null, { timeout: 20000 });
    const err = await page.evaluate(() => window.__error || null);
    if (err) throw new Error("page init failed: " + err);
    return await fn(page);
  } finally {
    await browser.close();
    server.close();
  }
}

// Print a standard PASS/FAIL line and set exit code. `pass` boolean, `line` text.
export function report(pass, line) {
  console.log((pass ? "PASS: " : "FAIL: ") + line);
  if (!pass) process.exitCode = 1;
}
