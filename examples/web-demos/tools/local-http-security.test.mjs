import assert from "node:assert/strict";
import { mkdirSync, realpathSync, symlinkSync, writeFileSync } from "node:fs";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { after, before, test } from "node:test";
import { createServer } from "node:http";

import {
  decodeLocalRequestPath,
  LOOPBACK_HOST,
  resolveCanonicalAsset,
  sendFixedText,
} from "./local-http-security.mjs";

let fixture;
let root;
let outside;

before(async () => {
  fixture = await mkdtemp(join(tmpdir(), "pulp-local-http-security-"));
  root = join(fixture, "site");
  outside = join(fixture, "outside");
  mkdirSync(join(root, "nested"), { recursive: true });
  mkdirSync(outside);
  writeFileSync(join(root, "index.html"), "ok");
  writeFileSync(join(root, "nested", "app.js"), "ok");
  writeFileSync(join(outside, "secret.txt"), "secret");
  symlinkSync(join(outside, "secret.txt"), join(root, "escape.txt"));
});

after(async () => rm(fixture, { recursive: true, force: true }));

test("decodes ordinary paths without query-string reflection", () => {
  assert.deepEqual(decodeLocalRequestPath("/nested/app.js?name=%3Cscript%3E"), {
    pathname: "/nested/app.js",
    segments: ["nested", "app.js"],
  });
});

test("rejects traversal, encoded separators, malformed URLs, and unsafe names", () => {
  for (const requestUrl of [
    "/../secret.txt",
    "/%2e%2e/secret.txt",
    "/..%2fsecret.txt",
    "/..%5csecret.txt",
    "/nested%2fapp.js",
    "/%E0%A4%A",
    "/bad%00name",
    "/bad%3Cscript%3E.js",
    "/%252e%252e/secret.txt",
  ]) {
    assert.equal(decodeLocalRequestPath(requestUrl), null, requestUrl);
  }
});

test("local demo servers bind only to IPv4 loopback", async () => {
  const server = createServer((_req, res) => sendFixedText(res, 404, "not found"));
  await new Promise((resolveListen) => server.listen(0, LOOPBACK_HOST, resolveListen));
  try {
    assert.equal(server.address().address, LOOPBACK_HOST);
  } finally {
    await new Promise((resolveClose) => server.close(resolveClose));
  }
});

test("fixed error responses cannot reflect a hostile request target", () => {
  const writes = [];
  const response = {
    writeHead: (status, headers) => writes.push({ status, headers }),
    end: (body) => writes.push({ body }),
  };
  sendFixedText(response, 404, "not found");
  assert.deepEqual(writes[1], { body: "not found" });
  assert.equal(JSON.stringify(writes).includes("<script>"), false);
});

test("contains canonical assets and rejects symlinks outside the root", () => {
  assert.equal(
    resolveCanonicalAsset(root, ["nested", "app.js"]),
    realpathSync(resolve(root, "nested", "app.js")),
  );
  assert.equal(resolveCanonicalAsset(root, ["escape.txt"]), null);
  assert.equal(resolveCanonicalAsset(root, ["missing.js"]), null);
});

test("resolves a directory index only after canonical containment", () => {
  assert.equal(resolveCanonicalAsset(root, [], { indexFile: "index.html" }), realpathSync(resolve(root, "index.html")));
});

test("negative control: lexical prefix checks accept a sibling-prefix escape", () => {
  const siblingPrefixEscape = resolve(`${root}-attacker`, "secret.txt");
  assert.equal(siblingPrefixEscape.startsWith(root), true);
  assert.equal(resolveCanonicalAsset(root, ["..", `${root.split("/").at(-1)}-attacker`, "secret.txt"]), null);
});
