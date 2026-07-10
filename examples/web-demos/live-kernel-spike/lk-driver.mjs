// Pulp Live Kernel — S0 spike — main-thread driver (ES module).
//
// Boots the AudioContext + resident lk-processor, transfers the wasm BYTES in,
// posts the initial graph, and drives the two edit paths the spike must prove:
//   * PARAM edit      — zero-interruption set_param (sweep the ladder cutoff).
//   * STRUCTURAL edit — post a new graph blob + equal-power crossfade.
// It measures edit->sound latency on the AUDIO clock (appliedCtxTime captured in
// the worklet at the exact quantum the edit becomes audible, minus the
// audioContext.currentTime at send). Everything is exposed on window.__lk for the
// headless validator.

import { encodePlan, MUSICAL, T } from "./lk-patches.mjs";

const WASM_URL = "/examples/web-demos/live-kernel-spike/dist/lk_kernel.wasm";
const WORKLET_URL = "/examples/web-demos/live-kernel-spike/lk-worklet.js";

// A second, structurally-DIFFERENT patch for the swap: a darker two-osc drone
// (different node count + topology + tuning) so the swap is unmistakably audible.
const DRONE = {
  nodes: [
    { type: T.OSC,   params: [[0, 55.0], [1, 1], [2, 0.3]] },  // 0 saw
    { type: T.OSC,   params: [[0, 82.5], [1, 2], [2, 0.25]] }, // 1 square
    { type: T.MIXER, params: [[0, 0.5]] },                      // 2
    { type: T.LADDER, params: [[0, 320.0], [1, 0.7]] },         // 3 dark, resonant
    { type: T.DELAY, params: [[0, 0.28], [1, 0.45], [2, 0.4]] },// 4
    { type: T.GAIN,  params: [[0, -3.0]] },                      // 5 out
  ],
  edges: [
    { src: 0, dst: 2, dport: 0 }, { src: 1, dst: 2, dport: 1 },
    { src: 2, dst: 3 }, { src: 3, dst: 4 }, { src: 4, dst: 5 },
  ],
  output: 5,
};

const state = {
  ready: false, error: null, lastMeter: null,
  latencies: { param: [], structural: [] },
  planBuild: [], // buildMs per structural edit (RT-thread plan-build cost)
  current: "musical",
  _editSeq: 0, _pending: new Map(),
};
window.__lk = state;

let ctx, node;

function post(msg, transfer) { node.port.postMessage(msg, transfer || []); }

// Issue an edit and resolve when the worklet reports it applied (audible). The
// latency is a MAIN-THREAD wall-clock round-trip (send -> worklet applies at the
// next quantum -> 'applied' returns): an upper bound on true edit->sound, since
// the sound changes at the apply point, before the return hop. (AudioWorkletGlobal
// Scope has no performance.now, so sub-block timing must be taken on the main
// thread; plan-build cost itself is measured precisely offline in measure.mjs.)
function issueParamEdit(node_, paramId, value) {
  const editId = ++state._editSeq;
  state._pending.set(editId, { kind: "param", sendPerf: performance.now() });
  post({ type: "param", node: node_, paramId, value, editId });
  return editId;
}
function issueStructuralEdit(patch, fadeMs = 40) {
  const editId = ++state._editSeq;
  state._pending.set(editId, { kind: "structural", sendPerf: performance.now() });
  const blob = encodePlan(patch);
  post({ type: "structuralEdit", bytes: blob.buffer, fadeMs, editId }, [blob.buffer]);
  return editId;
}
state.doParamEdit = (v = 2600) => issueParamEdit(4, 0, v);       // MUSICAL ladder cutoff
state.doStructuralEdit = () => {
  const patch = state.current === "musical" ? DRONE : MUSICAL;
  state.current = state.current === "musical" ? "drone" : "musical";
  return issueStructuralEdit(patch, 40);
};

async function boot() {
  try {
    ctx = new AudioContext();
    if (ctx.state === "suspended") { try { await ctx.resume(); } catch {} }
    await ctx.audioWorklet.addModule(WORKLET_URL);
    node = new AudioWorkletNode(ctx, "lk-processor", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
    node.connect(ctx.destination);

    node.port.onmessage = (e) => {
      const m = e.data;
      if (m.type === "ready") {
        // post the initial graph (musical) and hard-activate it
        const blob = encodePlan(MUSICAL);
        post({ type: "structuralEdit", bytes: blob.buffer, fadeMs: 0, editId: 0 }, [blob.buffer]);
        state.ready = true;
      } else if (m.type === "meter") {
        state.lastMeter = m;
        const el = document.getElementById("status");
        if (el) el.textContent = `q=${m.quanta} t=${m.currentTime.toFixed(2)}s rms=${m.outRms.toFixed(3)} ${m.fading ? "[fading]" : ""}`;
      } else if (m.type === "applied") {
        const p = state._pending.get(m.editId);
        if (p) {
          const latencyMs = performance.now() - p.sendPerf;
          if (m.kind === "param") state.latencies.param.push(latencyMs);
          else { state.latencies.structural.push(latencyMs); state.planBuild.push(m.buildMs); }
          state._pending.delete(m.editId);
        }
      } else if (m.type === "error") {
        state.error = m.message; console.error("worklet:", m.message);
      }
    };

    const resp = await fetch(WASM_URL);
    const bytes = await resp.arrayBuffer();
    post({ type: "wasm", bytes }, [bytes]); // transfer the raw bytes
  } catch (err) {
    state.error = String(err && err.stack || err);
    console.error(err);
  }
}

// buttons
document.getElementById("btnParam")?.addEventListener("click", () => state.doParamEdit(2600 + Math.random() * 1200));
document.getElementById("btnSwap")?.addEventListener("click", () => state.doStructuralEdit());
document.getElementById("btnStart")?.addEventListener("click", () => { if (ctx?.state === "suspended") ctx.resume(); });

boot();
