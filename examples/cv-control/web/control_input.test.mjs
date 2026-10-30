import assert from "node:assert/strict";
import { sendControlSample } from "./control_input.mjs";

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
