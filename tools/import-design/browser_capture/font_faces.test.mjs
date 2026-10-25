// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import test from "node:test";

import {
  capturePageFontFaces,
  fontSignature,
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
// Font bytes as each container begins.
const ttf = (text) => `\u0000\u0001\u0000\u0000${text}`;
const woff2 = (text) => `wOF2${text}`;

test("a face binds the first source that answered, with its rule's descriptors", async () => {
  const regular = `${PREFIX}fonts/Jost-Regular.ttf`;
  const page = fontPage([
    rule("Pulp Page Face", [
      source("fonts/gone.woff2", `${PREFIX}fonts/gone.woff2`, "woff2"),
      source("fonts/Jost-Regular.ttf", regular, "truetype"),
    ], { weight: "300 800", style: "italic", unicode_range: "U+0-FF" }),
  ], { [regular]: ttf("regular bytes") });
  const statuses = new Map([[`${PREFIX}fonts/gone.woff2`, 404], [regular, 200]]);
  const record = await capturePageFontFaces(page, { privatePrefix: PREFIX, statuses });
  assert.equal(record.schema, PAGE_FONT_FACES_SCHEMA);
  assert.deepEqual(record.faces, [{
    family: "Pulp Page Face", weight: "300 800", style: "italic", unicode_range: "U+0-FF",
    format: "truetype", source: "pulp-capture:///fonts/Jost-Regular.ttf",
    sha256: sha(ttf("regular bytes")),
  }]);
  assert.deepEqual(record.assets.map((asset) => [asset.sha256, asset.mime_type,
    Buffer.from(asset.data_base64, "base64").toString()]),
  [[sha(ttf("regular bytes")), "font/ttf", ttf("regular bytes")]]);
  assert.deepEqual(record.dropped, []);
});

test("rules that share a file carry its bytes once", async () => {
  const shared = `${PREFIX}fonts/variable.woff2`;
  const page = fontPage([
    rule("Variable", [source("fonts/variable.woff2", shared)], { weight: "400" }),
    rule("Variable", [source("fonts/variable.woff2", shared)], { weight: "700" }),
  ], { [shared]: woff2("variable") });
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
  const offline = fontPage(rules, { [remote]: woff2("remote") });
  assert.deepEqual(
    (await capturePageFontFaces(offline, { privatePrefix: PREFIX, statuses })).faces, []);
  assert.deepEqual(offline.reads, []);

  const online = fontPage(rules, { [remote]: woff2("remote") });
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
    ], { [data]: ttf("inline") });
    const record = await capturePageFontFaces(page,
      { privatePrefix: PREFIX, statuses: new Map([[blob, 200]]) });
    assert.deepEqual(record.faces.map((face) => [face.family, face.source]),
      [["Inline", "data:"]]);
    assert.deepEqual(page.reads, [data]);
  });

test("a font is recognized by its own first bytes", () => {
  const bytes = (text) => Buffer.from(text, "latin1");
  assert.equal(fontSignature(bytes("\u0000\u0001\u0000\u0000...")).mimeType, "font/ttf");
  assert.equal(fontSignature(bytes("true....")).mimeType, "font/ttf");
  assert.equal(fontSignature(bytes("OTTO....")).mimeType, "font/otf");
  assert.equal(fontSignature(bytes("wOF2....")).mimeType, "font/woff2");
  assert.equal(fontSignature(bytes("wOFF....")).container, "woff");
  assert.equal(fontSignature(bytes("<html>")), null);
  assert.equal(fontSignature(bytes("wOF")), null);
});

test("bytes that are not a font, WOFF 1.0, and files Chromium rejected are dropped by name",
  async () => {
    const at = (name) => `${PREFIX}fonts/${name}`;
    const page = fontPage([
      rule("Html", [source("fonts/page.ttf", at("page.ttf"))]),
      rule("Woff", [source("fonts/old.woff", at("old.woff"), "woff")]),
      rule("Rejected", [source("fonts/bad.ttf", at("bad.ttf"))], { face_status: "error" }),
      rule("Otf", [source("fonts/face.otf", at("face.otf"), "opentype")]),
    ], {
      [at("page.ttf")]: "<!doctype html>",
      [at("old.woff")]: "wOFF....",
      [at("bad.ttf")]: ttf("broken"),
      [at("face.otf")]: "OTTO....",
    });
    const statuses = new Map(["page.ttf", "old.woff", "bad.ttf", "face.otf"]
      .map((name) => [at(name), 200]));
    const record = await capturePageFontFaces(page, { privatePrefix: PREFIX, statuses });
    assert.deepEqual(record.faces.map((face) => face.family), ["Otf"]);
    assert.equal(record.assets[0].mime_type, "font/otf");
    assert.deepEqual(record.dropped.map((face) => [face.family, face.source]), [
      ["Html", "pulp-capture:///fonts/page.ttf"],
      ["Woff", "pulp-capture:///fonts/old.woff"],
      ["Rejected", "pulp-capture:///fonts/bad.ttf"],
    ]);
    assert.match(record.dropped[0].reason, /no TrueType, OpenType, WOFF or WOFF2 signature/);
    assert.match(record.dropped[1].reason, /WOFF 1\.0/);
    assert.match(record.dropped[2].reason, /could not decode it/);
    // A file Chromium rejected is never read at all.
    assert.ok(!page.reads.includes(at("bad.ttf")));
  });

test("no source outside the staged project is read", async () => {
  const escaped = "http://127.0.0.1:4100/outside.ttf"; // url(../../outside.ttf)
  const file = "file:///etc/fonts/system.ttf";
  const plain = "http://fonts.example.com/face.ttf";
  const rules = [
    rule("Escaped", [source("../../outside.ttf", escaped)]),
    rule("File", [source(file, file)]),
    rule("Plain", [source(plain, plain)]),
  ];
  const files = { [escaped]: ttf("x"), [file]: ttf("y"), [plain]: ttf("z") };
  const statuses = new Map([[escaped, 200], [file, 200], [plain, 200]]);
  for (const allowNetwork of [false, true]) {
    const page = fontPage(rules, files);
    const record = await capturePageFontFaces(page,
      { privatePrefix: PREFIX, statuses, allowNetwork });
    assert.deepEqual(record.faces, [], `network ${allowNetwork}`);
    assert.deepEqual(page.reads, [], `network ${allowNetwork}`);
  }
});
