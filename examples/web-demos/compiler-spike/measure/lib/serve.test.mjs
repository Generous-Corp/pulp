import assert from "node:assert/strict";
import { mkdirSync, symlinkSync, writeFileSync } from "node:fs";
import { mkdtemp, rm } from "node:fs/promises";
import { request } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";

import { LOOPBACK_HOST } from "../../../tools/local-http-security.mjs";
import { startServer } from "./serve.mjs";

let fixture;
let root;
let server;

function get(path) {
  return new Promise((resolveResponse, reject) => {
    const req = request({ host: LOOPBACK_HOST, port: server.address().port, path }, (res) => {
      const chunks = [];
      res.on("data", (chunk) => chunks.push(chunk));
      res.on("end", () => resolveResponse({ status: res.statusCode, body: Buffer.concat(chunks).toString() }));
    });
    req.on("error", reject);
    req.end();
  });
}

before(async () => {
  fixture = await mkdtemp(join(tmpdir(), "pulp-measure-server-"));
  root = join(fixture, "site");
  mkdirSync(root);
  writeFileSync(join(root, "index.html"), "fixture");
  writeFileSync(join(fixture, "secret.txt"), "secret");
  symlinkSync(join(fixture, "secret.txt"), join(root, "escape.txt"));
  server = await startServer(0, root);
});

after(async () => {
  await new Promise((resolveClose) => server.close(resolveClose));
  await rm(fixture, { recursive: true, force: true });
});

test("serves contained files on loopback", async () => {
  assert.equal(server.address().address, LOOPBACK_HOST);
  assert.deepEqual(await get("/"), { status: 200, body: "fixture" });
});

test("rejects traversal, symlink escape, malformed encoding, and reflection", async () => {
  assert.deepEqual(await get("/%2e%2e/secret.txt"), { status: 400, body: "bad request" });
  assert.deepEqual(await get("/escape.txt"), { status: 404, body: "not found" });
  assert.deepEqual(await get("/%E0%A4%A"), { status: 400, body: "bad request" });
  const reflected = await get("/missing%3Cscript%3E.js?x=%3Cscript%3E");
  assert.deepEqual(reflected, { status: 400, body: "bad request" });
  assert.equal(reflected.body.includes("script"), false);
});
