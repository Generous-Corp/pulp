import assert from "node:assert/strict";
import test from "node:test";

import { buildHeadlessPayload } from "./headless-payload.mjs";

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;

function assertStagingGlobalsCleared() {
  assert.equal(globalThis.__pulp_packed_src, undefined);
  assert.equal(globalThis.__pulp_headless_result, undefined);
  assert.equal(globalThis.__pulp_target_node_id, undefined);
  assert.equal(globalThis.__pulp_faithful_vector, undefined);
}

test("authored node ids remain data and staging globals are cleared", async () => {
  const hostileId = '26:3"; globalThis.__pulp_injected = true; //';
  const stub = `globalThis.__pulp_packed_src =
    "globalThis.__pulp_headless_result = Promise.resolve(" +
    "globalThis.__pulp_target_node_id);";`;
  const result = await new AsyncFunction(
    buildHeadlessPayload(stub, hostileId, true))();
  assert.equal(result, hostileId);
  assert.equal(globalThis.__pulp_injected, undefined);
  assertStagingGlobalsCleared();
});

test("staging globals are cleared when the headless bundle rejects", async () => {
  const stub = `globalThis.__pulp_packed_src =
    "globalThis.__pulp_headless_result = Promise.reject(new Error('boom'));";`;
  await assert.rejects(
    new AsyncFunction(buildHeadlessPayload(stub, "26:3", false))(),
    /boom/);
  assertStagingGlobalsCleared();
});
