// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import {
  availableCores,
  integrationConcurrency,
  SMALL_MACHINE_CORES,
} from "./run_integration.mjs";

test("a 3-vCPU gate VM runs the browser files one at a time", () => {
  assert.equal(integrationConcurrency(availableCores({ TARTCI_GUEST_CORES: "3" }, 28)), 1);
  assert.equal(integrationConcurrency(SMALL_MACHINE_CORES - 1), 1);
});

test("a 6-core or wider machine keeps three files in flight", () => {
  assert.equal(integrationConcurrency(availableCores({ TARTCI_GUEST_CORES: "6" }, 2)), 3);
  assert.equal(integrationConcurrency(availableCores({ TARTCI_GUEST_CORES: "12" }, 2)), 3);
});

test("without a guest lease the process's own parallelism decides", () => {
  assert.equal(availableCores({}, 3), 3);
  assert.equal(availableCores({ TARTCI_GUEST_CORES: "" }, 10), 10);
  assert.equal(availableCores({ TARTCI_GUEST_CORES: "zero" }, 4), 4);
  assert.equal(availableCores({ TARTCI_GUEST_CORES: "0" }, 4), 4);
});

test("the launcher passes the derived width to node --test and its exit status back", () => {
  const launcher = fileURLToPath(new URL("./run_integration.mjs", import.meta.url));
  const dir = mkdtempSync(path.join(os.tmpdir(), "pulp-run-integration-"));
  try {
    const passing = path.join(dir, "passing.test.mjs");
    const failing = path.join(dir, "failing.test.mjs");
    writeFileSync(passing, 'import test from "node:test";\ntest("ok", () => {});\n');
    writeFileSync(failing, 'import test from "node:test";\n' +
      'test("no", () => { throw new Error("deliberate"); });\n');
    // NODE_TEST_CONTEXT would make the nested `node --test` report to this
    // runner instead of exiting with its own status, as it does under ctest.
    const { NODE_TEST_CONTEXT: _nested, ...env } = process.env;
    const run = (files, cores) => spawnSync(process.execPath, [launcher, ...files], {
      encoding: "utf8", env: { ...env, TARTCI_GUEST_CORES: cores },
    });
    // Two files that each log start, wait, then log end: one at a time the
    // log interleaves s,e,s,e; in flight together it reads s,s,e,e. This
    // observes the width node actually used, not the launcher's banner.
    const log = path.join(dir, "order.log");
    const slow = [1, 2].map((i) => {
      const file = path.join(dir, `slow${i}.test.mjs`);
      writeFileSync(file, 'import test from "node:test";\n' +
        'import { appendFileSync } from "node:fs";\n' +
        `test("slow", async () => { appendFileSync(${JSON.stringify(log)}, "s"); ` +
        "await new Promise((r) => setTimeout(r, 1500)); " +
        `appendFileSync(${JSON.stringify(log)}, "e"); });\n`);
      return file;
    });
    const small = run(slow, "3");
    assert.match(small.stdout, /--test-concurrency=1 \(cores=3\)/);
    assert.equal(small.status, 0);
    assert.equal(readFileSync(log, "utf8"), "sese");
    rmSync(log);
    const wide = run([...slow, passing, failing], "12");
    assert.match(wide.stdout, /--test-concurrency=3 \(cores=12\)/);
    assert.notEqual(wide.status, 0);
    assert.equal(readFileSync(log, "utf8"), "ssee");
    assert.equal(spawnSync(process.execPath, [launcher], { env }).status, 2);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
