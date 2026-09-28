// SPDX-License-Identifier: MIT
// Runs the real-browser integration files under `node --test` with a file
// concurrency sized to the machine that runs them.
//
// Every concurrent file drives its own Chrome, and each capture needs about two
// cores to stay inside its 20-second CDP deadline: three captures at once on a
// 3-vCPU gate VM starved each other (`browser-capture-timeout ...
// stalled=Page.captureScreenshot`), while three on a 6-vCPU VM do not. So the
// width is one file per CORES_PER_BROWSER cores, at least one and at most the
// number of files: a larger VM finishes the suite sooner instead of idling
// cores it holds exclusively. The width is decided when the suite runs, not
// when it is configured: a build directory configured on one VM shape can be
// tested on another. Memory bounds it too: a concurrent Chrome is given
// MEMORY_MB_PER_BROWSER of the guest's declared lease.
import { spawnSync } from "node:child_process";
import os from "node:os";
import process from "node:process";
import { fileURLToPath } from "node:url";

export const CORES_PER_BROWSER = 2;
export const MEMORY_MB_PER_BROWSER = 2048;

function positiveInteger(value) {
  const parsed = Number.parseInt(value ?? "", 10);
  return Number.isInteger(parsed) && parsed > 0 ? parsed : undefined;
}

// The cores the suite may use: the tartci guest's lease when it publishes one,
// otherwise what Node reports for this process. Under ctest the suite shares
// the machine with other tests and holds only the slots its registration
// reserves (PULP_BROWSER_CAPTURE_RESERVED_CORES), so that reservation caps the
// machine's count: sizing to the whole VM would start browsers on cores other
// tests are using.
export function availableCores(env = process.env, available = os.availableParallelism()) {
  const machine = positiveInteger(env.TARTCI_GUEST_CORES) ?? available;
  const reserved = positiveInteger(env.PULP_BROWSER_CAPTURE_RESERVED_CORES);
  return reserved === undefined ? machine : Math.min(machine, reserved);
}

// The guest's declared memory lease, or undefined when none is declared.
export function declaredMemoryMb(env = process.env) {
  const memory = Number.parseInt(env.TARTCI_GUEST_MEM_MB ?? "", 10);
  return Number.isInteger(memory) && memory > 0 ? memory : undefined;
}

export function integrationConcurrency(cores, fileCount = Infinity, memoryMb = undefined) {
  let width = Math.floor(cores / CORES_PER_BROWSER);
  if (memoryMb !== undefined) {
    width = Math.min(width, Math.floor(memoryMb / MEMORY_MB_PER_BROWSER));
  }
  return Math.max(1, Math.min(width, fileCount));
}

function main(files) {
  if (files.length === 0) {
    process.stderr.write("run_integration.mjs: no test files given\n");
    return 2;
  }
  const cores = availableCores();
  const memoryMb = declaredMemoryMb();
  const concurrency = integrationConcurrency(cores, files.length, memoryMb);
  process.stdout.write(`[browser-capture] ${files.length} integration file(s), ` +
    `--test-concurrency=${concurrency} (cores=${cores}` +
    `${memoryMb === undefined ? "" : `, mem_mb=${memoryMb}`})\n`);
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
