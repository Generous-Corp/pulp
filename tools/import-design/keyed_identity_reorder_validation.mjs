#!/usr/bin/env node
// Measurement-only keyed identity/reorder harness. It validates the identity
// contract independently of importer or renderer behavior.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const fixtureDefault = path.join(root, "test/fixtures/import-differential/18-keyed-list-panel.html");
const identityDefault = path.join(root, "test/fixtures/import-differential/18-keyed-list-panel.identity.json");
const receiptDefault = path.join(root, "test/fixtures/import-differential/18-keyed-list-panel.receipt.json");

const sha256 = (value) => createHash("sha256").update(value).digest("hex");
const arg = (name, fallback) => {
  const i = process.argv.indexOf(name);
  return i >= 0 ? process.argv[i + 1] ?? fallback : fallback;
};
function digest(keys) { return sha256(Buffer.from(JSON.stringify(keys))); }
function validateKeys(keys, allowed) {
  assert(Array.isArray(keys) && keys.length > 0, "key sequence must be non-empty");
  assert.equal(new Set(keys).size, keys.length, "key sequence contains duplicates");
  assert(keys.every((key) => allowed.includes(key)), "key sequence contains an unknown key");
}

async function main() {
  const fixture = arg("--fixture", fixtureDefault);
  const identityPath = arg("--identity", identityDefault);
  const expectedPath = arg("--expected-receipt", receiptDefault);
  const receiptPath = arg("--receipt", path.join(process.cwd(), "keyed-identity-reorder.receipt.json"));
  const [source, identity] = await Promise.all([readFile(fixture), readFile(identityPath).then(JSON.parse)]);
  const keys = identity.keys;
  const reordered = identity.reordered_keys;
  const declared = [...source.toString().matchAll(/data-pulp-key="([^"]+)"/g)].map((match) => match[1]);
  assert.deepEqual(declared, keys, "identity manifest must match fixture keyed declarations");
  validateKeys(keys, keys);
  validateKeys(reordered, keys);
  assert.notDeepEqual(reordered, keys, "reorder must change sequence");
  assert.deepEqual([...reordered].sort(), [...keys].sort(), "reorder must preserve keyed identity set");

  const duplicate = [...keys.slice(0, 1), identity.duplicate_key, identity.duplicate_key];
  const unknown = [...keys.slice(0, -1), identity.unknown_key];
  assert.throws(() => validateKeys(duplicate, keys), /duplicates/);
  assert.throws(() => validateKeys(unknown, keys), /unknown key/);

  const receipt = {
    schema: "pulp-keyed-identity-reorder-v1",
    fixture: path.basename(fixture),
    source_sha256: sha256(source),
    source_bytes: source.length,
    initial: { keys, digest: digest(keys) },
    reordered: { keys: reordered, digest: digest(reordered), set_preserved: true },
    planted_negatives: {
      duplicate_key_rejected: true,
      unknown_key_rejected: true,
      duplicate_key: identity.duplicate_key,
      unknown_key: identity.unknown_key,
    },
  };
  const expected = JSON.parse(await readFile(expectedPath, "utf8"));
  assert.deepEqual(receipt, expected, "keyed identity receipt differs from deterministic baseline");
  await writeFile(receiptPath, JSON.stringify(receipt, null, 2) + "\n");
  process.stdout.write(JSON.stringify(receipt, null, 2) + "\n");
}
main().catch((error) => { console.error(error.stack || error); process.exitCode = 1; });
