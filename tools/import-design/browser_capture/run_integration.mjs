// SPDX-License-Identifier: MIT
// Runs the real-browser integration files under `node --test` with a file
// concurrency sized to the machine that runs them.
//
// Every concurrent file drives its own Chrome. On a 3-vCPU gate VM three
// captures at once starve each other past the 20-second CDP deadline
// (`browser-capture-timeout ... stalled=Page.captureScreenshot`), so below
// SMALL_MACHINE_CORES the files run one at a time. The width is decided when
// the suite runs, not when it is configured: a build directory configured on
// one VM shape can be tested on another.
import { spawnSync } from "node:child_process";
import os from "node:os";
import process from "node:process";
import { fileURLToPath } from "node:url";

export const SMALL_MACHINE_CORES = 6;
export const WIDE_CONCURRENCY = 3;

// The cores the suite may use: the tartci guest's lease when it publishes one,
// otherwise what Node reports for this process.
export function availableCores(env = process.env, available = os.availableParallelism()) {
  const guest = Number.parseInt(env.TARTCI_GUEST_CORES ?? "", 10);
  if (Number.isInteger(guest) && guest > 0) {
    return guest;
  }
  return available;
}

export function integrationConcurrency(cores) {
  return cores >= SMALL_MACHINE_CORES ? WIDE_CONCURRENCY : 1;
}

function main(files) {
  if (files.length === 0) {
    process.stderr.write("run_integration.mjs: no test files given\n");
    return 2;
  }
  const cores = availableCores();
  const concurrency = integrationConcurrency(cores);
  process.stdout.write(`[browser-capture] ${files.length} integration file(s), ` +
    `--test-concurrency=${concurrency} (cores=${cores})\n`);
  const result = spawnSync(process.execPath,
    ["--test", `--test-concurrency=${concurrency}`, ...files], { stdio: "inherit" });
  if (result.error) {
    process.stderr.write(`run_integration.mjs: ${result.error.message}\n`);
    return 1;
  }
  return result.status ?? 1;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  process.exitCode = main(process.argv.slice(2));
}
