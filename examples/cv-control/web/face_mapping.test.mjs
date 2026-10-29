import assert from "node:assert/strict";
import { FaceMapping } from "./face_mapping.mjs";
const m = new FaceMapping(.6, .5);
assert.equal(m.submit({sequence: 1, mouthOpen: 1, confidence: .9, tracked: true}), .5);
assert.equal(m.submit({sequence: 2, mouthOpen: 0, confidence: .2, tracked: true}), .25);
m.expire();
assert.equal(m.value, 0);
assert.equal(m.tracked, false);
console.log("PASS: browser face mapping replay/loss controls");
