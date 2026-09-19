import { existsSync } from 'node:fs';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import os from 'node:os';
import { chromium } from 'playwright-core';
import { serve } from './serve.mjs';

function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  if (i < 0) return fallback;
  if (!process.argv[i + 1] || process.argv[i + 1].startsWith('--')) throw new Error(`Missing value: ${name}`);
  return process.argv[i + 1];
}
const required = name => { const v = arg(name); if (!v) throw new Error(`Required: ${name}`); return resolve(v); };
const repo = resolve(arg('--repo', fileURLToPath(new URL('../../..', import.meta.url))));
const options = { repo, wamDir: required('--wam-dir'), wclapDir: required('--wclap-dir'),
  baselineWamDir: required('--baseline-wam-dir'), baselineWclapDir: required('--baseline-wclap-dir') };
const reportPath = required('--report');
const hash = data => createHash('sha256').update(data).digest('hex');
const report = { pass: false, sourceHead: execFileSync('git', ['rev-parse', 'HEAD'], { cwd: repo, encoding: 'utf8' }).trim(),
  sourceDirty: Boolean(execFileSync('git', ['status', '--porcelain'], { cwd: repo, encoding: 'utf8' }).trim()),
  trackedDiffSha256: hash(execFileSync('git', ['diff', 'HEAD'], { cwd: repo })),
  environment: { node: process.version, platform: os.platform(), architecture: os.arch() }, artifacts: {}, checks: [] };
let browser, server;
try {
  for (const [name, path] of Object.entries({
    regionWam: resolve(options.wamDir, 'SampleRegionAllpassWorklet.js'),
    regionWclap: resolve(options.wclapDir, 'SampleRegionAllpass.wasm'),
    baselineWam: resolve(options.baselineWamDir, 'PulpGainWorklet.js'),
    baselineWclap: resolve(options.baselineWclapDir, 'PulpGain.wasm'),
  })) report.artifacts[name] = { path, sha256: hash(await readFile(path)) };
  const executablePath = [arg('--browser'), process.env.PLAYWRIGHT_CHROMIUM_PATH, process.env.CHROME_PATH,
    '/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/usr/bin/google-chrome', '/usr/bin/chromium'].filter(Boolean).find(existsSync);
  if (!executablePath) throw new Error('Chrome not found: set --browser or CHROME_PATH');
  server = await serve(options);
  browser = await chromium.launch({ executablePath, headless: true });
  report.environment.browser = await browser.version();
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(String(e)));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(server.url, { waitUntil: 'load' });
  await page.waitForFunction(() => window.__proof, null, { timeout: 180000 });
  const proof = await page.evaluate(() => window.__proof);
  Object.assign(report, proof, { browserErrors: errors });
  if (!proof.pass || errors.length) throw new Error(proof.error || errors.join('\n') || 'Browser assertions failed');
  console.log(`PASS: ${proof.checks.length} real browser assertions`);
} catch (error) {
  report.pass = false; report.error = String(error.stack || error); process.exitCode = 1;
  console.error(report.error);
} finally {
  if (browser) await browser.close();
  if (server) await server.close();
  await mkdir(dirname(reportPath), { recursive: true });
  await writeFile(reportPath, JSON.stringify(report, null, 2) + '\n');
}
