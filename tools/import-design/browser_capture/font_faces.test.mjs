// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import test from "node:test";

import {
  capturePageFontFaces,
  fontMimeType,
  PAGE_FONT_FACES_SCHEMA,
} from "./font_faces.mjs";

const PREFIX = "http://127.0.0.1:4100/token/";

// A page whose style sheets declare `rules`, serving `files` (URL -> bytes)
// to the capture's in-page reads, and counting them.
function fontPage(rules, files = {}) {
  const reads = [];
  return {
    reads,
    async call(method, params) {
      assert.equal(method, "Runtime.evaluate");
      if (!params.awaitPromise) return { result: { value: rules } };
      const url = JSON.parse(/fetch\(("[^"]*")/.exec(params.expression)[1]);
      reads.push(url);
      const bytes = files[url];
      return { result: { value: bytes
        ? { status: 200, data_base64: Buffer.from(bytes).toString("base64") }
        : { status: 404 } } };
    },
  };
}

const rule = (family, sources, extra = {}) => ({
  family, weight: "400", style: "normal", unicode_range: "", sources,
  family_loaded: true, ...extra,
});
const source = (authored, url, format = "") => ({ authored, url, format });
const sha = (text) => createHash("sha256").update(text).digest("hex");

test("a face binds the first source that answered, with its rule's descriptors", async () => {
  const regular = `${PREFIX}fonts/Jost-Regular.ttf`;
  const page = fontPage([
    rule("Pulp Page Face", [
      source("fonts/gone.woff2", `${PREFIX}fonts/gone.woff2`, "woff2"),
      source("fonts/Jost-Regular.ttf", regular, "truetype"),
    ], { weight: "300 800", style: "italic", unicode_range: "U+0-FF" }),
  ], { [regular]: "regular bytes" });
  const statuses = new Map([[`${PREFIX}fonts/gone.woff2`, 404], [regular, 200]]);
  const record = await capturePageFontFaces(page, { privatePrefix: PREFIX, statuses });
  assert.equal(record.schema, PAGE_FONT_FACES_SCHEMA);
  assert.deepEqual(record.faces, [{
    family: "Pulp Page Face", weight: "300 800", style: "italic", unicode_range: "U+0-FF",
    format: "truetype", source: "pulp-capture:///fonts/Jost-Regular.ttf",
    sha256: sha("regular bytes"),
  }]);
  assert.deepEqual(record.assets.map((asset) => [asset.sha256, asset.mime_type,
    Buffer.from(asset.data_base64, "base64").toString()]),
  [[sha("regular bytes"), "font/ttf", "regular bytes"]]);
});

test("rules that share a file carry its bytes once", async () => {
  const shared = `${PREFIX}fonts/variable.woff2`;
  const page = fontPage([
    rule("Variable", [source("fonts/variable.woff2", shared)], { weight: "400" }),
    rule("Variable", [source("fonts/variable.woff2", shared)], { weight: "700" }),
  ], { [shared]: "variable" });
  const record = await capturePageFontFaces(page,
    { privatePrefix: PREFIX, statuses: new Map([[shared, 200]]) });
  assert.equal(record.faces.length, 2);
  assert.equal(record.assets.length, 1);
  assert.equal(record.assets[0].mime_type, "font/woff2");
  assert.deepEqual(page.reads, [shared]);
});

test("a face nothing fetched binds nothing", async () => {
  const page = fontPage([rule("Unused",
    [source("fonts/never.ttf", `${PREFIX}fonts/never.ttf`)], { family_loaded: false })]);
  const record = await capturePageFontFaces(page, { privatePrefix: PREFIX });
  assert.deepEqual(record.faces, []);
  assert.deepEqual(page.reads, []);
});

test("a staged font file the page names and does not have fails the capture by name",
  async () => {
    const missing = `${PREFIX}fonts/absent.ttf`;
    const page = fontPage([rule("Pulp Page Face", [source("fonts/absent.ttf", missing)])]);
    await assert.rejects(
      capturePageFontFaces(page,
        { privatePrefix: PREFIX, statuses: new Map([[missing, 404]]) }),
      (error) => error.code === "capture-font-missing" &&
        error.message.includes('"fonts/absent.ttf"') &&
        error.message.includes("pulp-capture:///fonts/absent.ttf") &&
        error.message.includes("Pulp Page Face") &&
        !error.message.includes("127.0.0.1"));
  });

test("a remote font is read only when the import allows the network", async () => {
  const remote = "https://fonts.example.com/face.woff2";
  const rules = [rule("Remote", [source(remote, remote, "woff2")])];
  const statuses = new Map([[remote, 200]]);
  const offline = fontPage(rules, { [remote]: "remote" });
  assert.deepEqual(
    (await capturePageFontFaces(offline, { privatePrefix: PREFIX, statuses })).faces, []);
  assert.deepEqual(offline.reads, []);

  const online = fontPage(rules, { [remote]: "remote" });
  const record = await capturePageFontFaces(online,
    { privatePrefix: PREFIX, statuses, allowNetwork: true });
  assert.equal(record.faces[0].source, "https://fonts.example.com/face.woff2");
  assert.deepEqual(online.reads, [remote]);
});

test("inline data fonts are carried, and Blob fonts are left to the materialized document",
  async () => {
    const data = "data:font/woff2;base64,AAAA";
    const blob = "blob:http://127.0.0.1:4100/1234";
    const page = fontPage([
      rule("Inline", [source(data, data)]),
      rule("Bundled", [source(blob, blob)]),
    ], { [data]: "inline" });
    const record = await capturePageFontFaces(page,
      { privatePrefix: PREFIX, statuses: new Map([[blob, 200]]) });
    assert.deepEqual(record.faces.map((face) => [face.family, face.source]),
      [["Inline", "data:"]]);
    assert.deepEqual(page.reads, [data]);
  });

test("a font's MIME type follows its format hint, then its extension", () => {
  assert.equal(fontMimeType("woff2", "x.ttf"), "font/woff2");
  assert.equal(fontMimeType("opentype", ""), "font/otf");
  assert.equal(fontMimeType("", "fonts/a.WOFF?v=2"), "font/woff");
  assert.equal(fontMimeType("", "fonts/a"), "font/ttf");
});
