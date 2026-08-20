import assert from "node:assert/strict";
import { test } from "node:test";

import { tokenizeHighlightLine } from "./highlight-tokens.mjs";

test("preserves hostile permalink text as text-only token payloads", () => {
  const input = `<script src=x onerror=alert(1)>&lt;/script>`;
  let id = 0;
  const { parts } = tokenizeHighlightLine(input, 0, null, () => id++);
  assert.equal(parts.map((part) => part.text).join(""), input);
  assert.ok(parts.every((part) => !part.className || /^[a-z0-9-]+$/.test(part.className)));
  assert.ok(parts.every((part) => part.dataset === null || /^\d+$/.test(part.dataset.nid)));
});

test("retains numeric metadata used by scrubbing", () => {
  let id = 0;
  const { parts, tokens } = tokenizeHighlightLine("gain(db: +3db)", 10, "gain", () => id++);
  assert.equal(parts.map((part) => part.text).join(""), "gain(db: +3db)");
  assert.deepEqual(tokens, [{
    id: 0, start: 19, len: 4, value: 3, unit: "db", verb: "gain", key: "db", noteMul: false,
  }]);
});
