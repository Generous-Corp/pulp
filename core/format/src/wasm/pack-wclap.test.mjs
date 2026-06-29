// Self-contained test for pack-wclap.mjs (the WebCLAP bundle packager).
//
// Exercises the file-level behavior with a dummy module — no real wasm or
// toolchain needed — so it can run anywhere: bundle layout, the mandatory
// module.wasm rename, resource copying, the .tar.gz archive, and the
// unknown-args guard. Run: node pack-wclap.test.mjs   (exit 0 = PASS).
import { mkdtemp, writeFile, mkdir, readFile, stat, rm } from "node:fs/promises";
import { existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const PACK = fileURLToPath(new URL("./pack-wclap.mjs", import.meta.url));
let failures = 0;
const check = (cond, msg) => { if (!cond) { console.error("  ✗ " + msg); failures++; } else console.log("  ✓ " + msg); };
const run = (args) => spawnSync(process.execPath, [PACK, ...args], { encoding: "utf8" });

const work = await mkdtemp(join(tmpdir(), "wclap-pack-"));
try {
  // A dummy "module" stands in for a real .wasm — the packager is byte-agnostic.
  const src = join(work, "PulpGain.wasm");
  await writeFile(src, Buffer.from([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 1, 2, 3]));
  const resDir = join(work, "res");
  await mkdir(resDir, { recursive: true });
  await writeFile(join(resDir, "Init.json"), '{"name":"Init"}');
  const out = join(work, "out");
  await mkdir(out, { recursive: true });

  const r = run(["--wasm", src, "--name", "PulpGain", "--out", out, "--resources", resDir, "--tar"]);
  check(r.status === 0, "exit 0 on a valid pack");
  const bundle = join(out, "PulpGain.wclap");
  check(existsSync(join(bundle, "module.wasm")), "bundle contains module.wasm");
  check((await readFile(join(bundle, "module.wasm"))).equals(await readFile(src)), "module.wasm matches source bytes");
  check(existsSync(join(bundle, "Init.json")), "resources copied into bundle");
  check(existsSync(join(out, "PulpGain.wclap.tar.gz")), ".tar.gz archive produced");
  check((await stat(join(out, "PulpGain.wclap.tar.gz"))).size > 0, ".tar.gz is non-empty");

  // The tar's top-level entry must be the <name>.wclap/ directory.
  const list = spawnSync("tar", ["-tzf", join(out, "PulpGain.wclap.tar.gz")], { encoding: "utf8" });
  check(/PulpGain\.wclap\/module\.wasm/.test(list.stdout), "archive holds PulpGain.wclap/module.wasm");

  // A symlink in resources is rejected (the WASI FS may not support symlinks).
  const { symlink } = await import("node:fs/promises");
  const symRes = join(work, "res-sym");
  await mkdir(symRes, { recursive: true });
  await symlink(src, join(symRes, "link.wasm"));
  check(run(["--wasm", src, "--name", "X", "--out", out, "--resources", symRes]).status === 1,
    "symlink in resources → exit 1");

  // Missing required args → usage error (exit 2).
  check(run(["--name", "X"]).status === 2, "missing --wasm → exit 2");
  // Missing wasm file → failure (exit 1).
  check(run(["--wasm", join(work, "nope.wasm"), "--name", "X", "--out", out]).status === 1, "missing wasm file → exit 1");

  console.log(failures === 0 ? "PASS: pack-wclap.mjs" : `FAIL: ${failures} check(s) failed`);
  process.exit(failures === 0 ? 0 : 1);
} finally {
  await rm(work, { recursive: true, force: true });
}
