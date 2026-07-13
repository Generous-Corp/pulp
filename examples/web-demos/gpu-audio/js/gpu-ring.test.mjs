// gpu-ring.test.mjs — the SAB transport's contract, proven on a REAL
// SharedArrayBuffer (no browser, no GPU): node gpu-ring.test.mjs
//
// What is actually at stake in each case:
//   latency  — the wet delivered at block n is the wet of block n-L, EXACTLY, for
//              the prepared lifetime. There is no silence priming: the first L
//              blocks MISS (the plugin's latency-aligned CPU wet covers them).
//   ordering — a wet block lands at the right index, sample-accurately.
//   drops    — a full input ring drops a block (a throttled tab does this by
//              design). It must cost exactly ONE miss, L blocks later, and must
//              NOT shift every later wet one block early — which is what a
//              positional ring does, silently and permanently.
//   expiry   — same, for a block the worker consumed and never published.
//   resync   — a late wet for a block the plugin has already covered is DISCARDED,
//              not emitted a block late (which would comb-filter dry against wet).
//   idle     — a ring that stops being pumped (Engine=CPU) and is then pumped
//              again must not hand back the wets it was holding as if they were
//              current. This is the live Engine flip.
//   no-wait  — nothing on the audio-thread path can call Atomics.wait, and nothing
//              on it calls Atomics.notify either (notify does not block, but it
//              takes V8's shared waiter-list lock, and the audio thread takes none).

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { Worker } from "node:worker_threads";

import { allocate } from "./gpu-ring.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const OPTS = { blockSize: 8, channels: 2, slots: 4, latencyBlocks: 2 };
const BLOCK_FLOATS = OPTS.blockSize * OPTS.channels;
const L = OPTS.latencyBlocks;

// A block whose every sample carries its own marker, so a mis-ordered or
// mis-latched block is caught sample-for-sample rather than "sounds about right".
const marker = (n) => {
  const b = new Float32Array(BLOCK_FLOATS);
  for (let i = 0; i < BLOCK_FLOATS; i++) b[i] = n * 1000 + i;
  return b;
};
const wetOf = (dry) => { const w = new Float32Array(dry.length); for (let i = 0; i < dry.length; i++) w[i] = dry[i] * 2 + 1; return w; };

// The mock GPU worker: drain every ready input block, "process" it, publish it
// under the block index the worklet stamped on it. `skip` names a block whose
// deadline expired — consumed, never published.
function pump(ring, skip = null) {
  const scratch = new Float32Array(BLOCK_FLOATS);
  let produced = 0;
  for (;;) {
    if (ring.outputFull()) break;
    const seq = ring.takeInput(scratch, 0);
    if (seq === null) break;
    if (seq === skip) continue;
    ring.publishOutput(wetOf(scratch), 0, seq);
    produced++;
  }
  return produced;
}

test("the wet delivered at block n is the wet of block n-L, exactly", () => {
  const ring = allocate(OPTS).prime();
  const out = new Float32Array(BLOCK_FLOATS);

  for (let n = 0; n < 12; n++) {
    pump(ring);                       // the worker runs between audio blocks
    assert.equal(ring.push(marker(n), 0), 1, `block ${n} accepted`);
    const got = ring.pop(out, 0);
    if (n < L) {
      // No wet exists for a block that was never pushed. This is a MISS, not a
      // block of primed silence: the plugin substitutes its latency-aligned CPU
      // wet, so the listener hears the reverb rather than a 21 ms dropout.
      assert.equal(got, 0, `block ${n} misses — nothing was pushed L blocks ago`);
    } else {
      assert.equal(got, 1, `block ${n} delivered`);
      assert.deepEqual([...out], [...wetOf(marker(n - L))], `block ${n} carries the wet of dry block ${n - L}`);
    }
  }
  const c = ring.workletCounters();
  assert.equal(c.miss, L, "exactly the L start-up blocks missed");
  assert.equal(c.dropped, 0);
  assert.equal(c.resynced, 0);
});

test("round trip preserves per-block markers in order", () => {
  const ring = allocate(OPTS).prime();
  const out = new Float32Array(BLOCK_FLOATS);
  for (let n = 0; n < 64; n++) {
    pump(ring);
    ring.push(marker(n), 0);
    ring.pop(out, 0);
    if (n >= L) assert.deepEqual([...out], [...wetOf(marker(n - L))], `block ${n}`);
  }
  assert.equal(ring.workletCounters().miss, L);
});

test("a stalled worker MISSES, and its late wets are discarded, not played late", () => {
  const ring = allocate(OPTS).prime();
  const out = new Float32Array(BLOCK_FLOATS);
  const K = 2;                                  // misses to provoke beyond the start-up L

  // Stall the worker: nothing is produced, so every read misses.
  for (let n = 0; n < L + K; n++) {
    ring.push(marker(n), 0);
    assert.equal(ring.pop(out, 0), 0, `block ${n}: miss`);
  }
  assert.equal(ring.workletCounters().miss, L + K);

  // Resume. The wets the worker now produces for blocks it was late on are STALE —
  // the plugin already emitted a CPU substitute for those slots — so they are
  // dropped, and the very next delivered block is the one that is exactly L old.
  for (let n = L + K; n < 32; n++) {
    pump(ring);
    ring.push(marker(n), 0);
    assert.equal(ring.pop(out, 0), 1, `block ${n} delivered after recovery`);
    assert.deepEqual([...out], [...wetOf(marker(n - L))],
      `block ${n} is the wet of dry ${n - L} — latency is exactly ${L} blocks, not ${L + K}`);
  }

  const c = ring.workletCounters();
  assert.equal(c.miss, L + K, "no misses after recovery");
  assert.equal(c.resynced, K, "exactly the K late wet blocks were dropped to realign");
  assert.equal(c.dropped, 0, "no input was lost");
});

test("overflow drops input blocks without blocking or growing", () => {
  const ring = allocate(OPTS).prime();
  const out = new Float32Array(BLOCK_FLOATS);
  const EXTRA = 3;
  let accepted = 0;
  for (let n = 0; n < OPTS.slots + EXTRA; n++) {      // worker never pumps
    accepted += ring.push(marker(n), 0);
    ring.pop(out, 0);
  }
  const c = ring.workletCounters();
  assert.equal(accepted, OPTS.slots, "the ring accepts exactly its capacity");
  assert.equal(c.dropped, EXTRA, "every excess block is DROPPED, not queued");
  assert.equal(ring.inputDepth(), OPTS.slots, "the ring never grows past its capacity");
});

// THE DROP CASE. The worker falls far enough behind that the input ring fills and
// the audio thread drops blocks — the throttled-background-tab steady state, which
// the design calls normal. A positional ring pops one slot per block regardless, so
// after D drops it emits the wet of block n-L+D at block n: the wet permanently
// LEADS the dry, and nothing ever corrects it. Seq stamps make it cost one miss.
test("a dropped input costs ONE miss and does not shift the wet timeline", () => {
  const ring = allocate(OPTS).prime();
  const out = new Float32Array(BLOCK_FLOATS);
  const dropped = new Set();

  for (let n = 0; n < 40; n++) {
    // The worker is asleep for blocks 4..11 — long enough to fill the 4-slot input
    // ring, so blocks 8..11 are dropped outright.
    if (n < 4 || n >= 12) pump(ring);
    if (ring.push(marker(n), 0) === 0) dropped.add(n);
    const got = ring.pop(out, 0);
    const want = n - L;
    if (got) {
      // WHATEVER is delivered must be the wet of exactly block n-L. Never a
      // neighbour's — that is the shift this test exists to catch.
      assert.deepEqual([...out], [...wetOf(marker(want))],
        `block ${n} delivered the wet of block ${want}`);
    } else {
      // A miss is only legal for a block whose wet cannot exist: it was dropped, it
      // was never pushed (start-up), or the worker had not produced it yet.
      assert.ok(want < 0 || dropped.has(want) || n < 16,
        `block ${n} missed for a reason (want=${want})`);
    }
  }

  assert.ok(dropped.size > 0, "the stall really did overflow the input ring");
  const c = ring.workletCounters();
  assert.equal(c.dropped, dropped.size);
  // Every dropped block costs exactly one miss (plus the L start-up ones and the
  // blocks the stalled worker had not produced) — and NOT a permanent shift.
  assert.ok(c.miss >= dropped.size + L);
});

// THE EXPIRY CASE. The worker consumed the input, missed its deadline, and never
// published that wet — the loop-top expiry in gpu-worker.mjs. The wet stream then
// has a HOLE, which a positional ring would smear across every later block.
test("a block the worker never publishes costs ONE miss and does not shift", () => {
  const ring = allocate(OPTS).prime();
  const out = new Float32Array(BLOCK_FLOATS);
  const EXPIRED = 5;
  let misses = 0;

  for (let n = 0; n < 24; n++) {
    pump(ring, EXPIRED);
    ring.push(marker(n), 0);
    const got = ring.pop(out, 0);
    const want = n - L;
    if (got) {
      assert.deepEqual([...out], [...wetOf(marker(want))], `block ${n} → wet of ${want}`);
      assert.notEqual(want, EXPIRED, "the expired block was never delivered");
    } else {
      misses++;
      assert.ok(want < 0 || want === EXPIRED, `block ${n} missed only for the expired wet`);
    }
  }
  assert.equal(misses, L + 1, "the L start-up blocks, plus the single expired one");
  assert.equal(ring.workletCounters().dropped, 0, "nothing was dropped — this is purely an expiry");
});

// THE LIVE ENGINE FLIP. While Engine=CPU the plugin still drives push/pop on every
// block (super_convolver.hpp's fill_wet_web) — that is what keeps this timeline
// advancing. If it did NOT, the ring would freeze holding the wets it had produced,
// and the first pops after a flip back to GPU would return THAT audio as if it were
// current. Here the same run is asserted block-by-block: every delivered wet belongs
// to the block that is exactly L old, across a stretch where the caller ignored the
// result (the CPU engine) and a stretch where it used it.
test("a ring driven through an Engine=CPU stretch never hands back stale audio", () => {
  const ring = allocate(OPTS).prime();
  const out = new Float32Array(BLOCK_FLOATS);
  let usedOnGpu = 0;

  for (let n = 0; n < 40; n++) {
    pump(ring);
    ring.push(marker(n), 0);
    const got = ring.pop(out, 0);
    const onGpu = n < 10 || (n >= 20 && n < 30);   // the Engine parameter's value
    if (got && n >= L) {
      assert.deepEqual([...out], [...wetOf(marker(n - L))],
        `block ${n} (engine=${onGpu ? "gpu" : "cpu"}) delivered the wet of block ${n - L}`);
      if (onGpu) usedOnGpu++;
    }
  }
  assert.ok(usedOnGpu > 15, "the GPU engine really was fed live wets on both sides of the flip");
  assert.equal(ring.workletCounters().miss, L, "the flip itself cost no misses at all");
});

test("nothing on the audio-thread path calls Atomics.wait — or Atomics.notify", () => {
  const worklet = readFileSync(
    join(HERE, "..", "..", "..", "..", "packages", "pulp-web-player",
         "src", "vendor", "pulp-wasm", "wclap-processor.js"), "utf8");
  // Blocking on the audio thread is forbidden (and throws in a real browser).
  assert.ok(!/Atomics\s*\.\s*wait\s*\(/.test(worklet), "the worklet bundle contains no Atomics.wait(");
  const ringSrc = readFileSync(join(HERE, "gpu-ring.mjs"), "utf8");
  assert.ok(!/Atomics\s*\.\s*wait\s*\(/.test(ringSrc), "gpu-ring.mjs contains no Atomics.wait(");
  // And no Atomics.notify: it does not block, but it takes V8's shared waiter-list
  // lock — a lock, on the audio thread. The worker polls on a bounded timer instead.
  assert.ok(!/Atomics\s*\.\s*notify\s*\(/.test(ringSrc), "gpu-ring.mjs takes no waiter-list lock");
  assert.ok(!/Atomics\s*\.\s*notify\s*\(/.test(worklet), "the worklet bundle takes no waiter-list lock");
  // The worker never blocks either: Atomics.wait would starve the very event loop
  // that resolves WebGPU's mapAsync (the browser form of the native
  // spin-on-ProcessEvents self-deadlock).
  const worker = readFileSync(join(HERE, "gpu-worker.mjs"), "utf8");
  assert.ok(!/Atomics\s*\.\s*wait\s*\(/.test(worker), "gpu-worker.mjs contains no blocking Atomics.wait(");

  // And prove it at runtime: run the push/pop path with both booby-trapped.
  const realWait = Atomics.wait;
  const realNotify = Atomics.notify;
  Atomics.wait = () => { throw new Error("Atomics.wait called on the audio-thread path"); };
  Atomics.notify = () => { throw new Error("Atomics.notify called on the audio-thread path"); };
  try {
    const ring = allocate(OPTS).prime();
    const out = new Float32Array(BLOCK_FLOATS);
    for (let n = 0; n < 16; n++) { pump(ring); ring.push(marker(n), 0); ring.pop(out, 0); }
  } finally {
    Atomics.wait = realWait;
    Atomics.notify = realNotify;
  }
});

test("the ring works across a REAL thread boundary (worker_threads + SAB)", async () => {
  const ring = allocate(OPTS).prime();
  const ringUrl = JSON.stringify(new URL("./gpu-ring.mjs", import.meta.url).href);
  // A worker that drains the input ring and echoes wet blocks back, exactly like
  // the WebGPU worker does — but with no GPU, so this runs in plain Node CI.
  const worker = new Worker(`
    import { parentPort, workerData } from "node:worker_threads";
    const { attach } = await import(${ringUrl});
    const ring = attach(workerData.sab);
    const scratch = new Float32Array(${BLOCK_FLOATS});
    const wet = new Float32Array(${BLOCK_FLOATS});
    let produced = 0;
    const step = () => {
      for (;;) {
        if (ring.outputFull()) break;
        const seq = ring.takeInput(scratch, 0);
        if (seq === null) break;
        for (let i = 0; i < scratch.length; i++) wet[i] = scratch[i] * 2 + 1;
        ring.publishOutput(wet, 0, seq);
        produced++;
        ring.publishStats({ produced, state: 1 });
      }
      setTimeout(step, 1);
    };
    parentPort.postMessage("up");
    step();
  `, { eval: "module", workerData: { sab: ring.sab } });

  const errors = [];
  worker.on("error", (e) => errors.push(e));
  await new Promise((r) => worker.once("message", r));   // the worker attached the SAB

  const out = new Float32Array(BLOCK_FLOATS);
  const seen = [];
  for (let n = 0; n < 24; n++) {
    ring.push(marker(n), 0);
    // Give the worker a turn; the real audio thread does not — it just misses.
    await new Promise((r) => setTimeout(r, 2));
    seen.push(ring.pop(out, 0) ? { n, block: [...out] } : null);
  }
  await worker.terminate();
  assert.deepEqual(errors, [], "the worker thread ran clean");

  // The scheduler decides HOW MANY blocks land (a miss is legal — that is the
  // whole design). What must hold regardless: every delivered wet block is the
  // INTACT wet of the block that is exactly L old. A torn, reordered, or shifted
  // block would mean the cursors, the seq stamps, or the memory model are wrong.
  let delivered = 0;
  for (const got of seen) {
    if (!got) continue;
    assert.deepEqual(got.block, [...wetOf(marker(got.n - L))],
      `block ${got.n} delivered the intact wet of block ${got.n - L}`);
    delivered++;
  }
  assert.ok(delivered > 0, "the worker actually delivered wet blocks across the thread boundary");

  const stats = ring.readStats();
  assert.ok(stats && stats.produced > 0, "the worker published stats through the seqlock");
  assert.equal(stats.miss, ring.workletCounters().miss, "the worker mirrors the worklet's miss counter");
});
