import PulpWAM from '/host/wam-plugin.js';
import { WebClapHost } from '/host/wclap-host.mjs';

const checks = [];
const check = (condition, name) => {
  checks.push({ name, pass: Boolean(condition) });
  document.querySelector('#log').textContent += `\n${condition ? 'PASS' : 'FAIL'} ${name}`;
  if (!condition) throw new Error(name);
};
const near = (a, b, tolerance = 1e-5) => Number.isFinite(a) && Math.abs(a - b) <= tolerance;
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const assertAudio = (actual, expected, name) => check(actual.length === expected.length && actual.every((v, i) => near(v, expected[i])), name);
const RATE = 48000, PARAM = 2901, SYNTHETIC_BYPASS = 1883404656;
const signal = n => Float32Array.from({ length: n }, (_, i) => Math.fround(0.2 * Math.sin(i * 0.17) + (i === 0 ? 0.25 : 0)));
function oracle(input, coefficient, history = { x: 0, y: 0 }) {
  return Float32Array.from(input, x => {
    const y = coefficient * x + history.x - coefficient * history.y;
    history.x = x; history.y = y;
    return y;
  });
}
async function bytes(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Artifact fetch failed: ${url}: ${response.status}`);
  return new Uint8Array(await response.arrayBuffer());
}
function catalog(params, wam, name) {
  // The Processor exposes exactly one promoted region parameter. Some format
  // adapters synthesize their standard bypass control at the adapter boundary;
  // that control is permitted only as one trailing entry and can never add a
  // second promoted region identity.
  const ids = params.map(p => Number(p.id));
  check(ids.length >= 1 && ids.length <= 2 && ids[0] === PARAM
    && ids.filter(id => id === PARAM).length === 1
    && ids.slice(1).every(id => id === SYNTHETIC_BYPASS), `${name} exact promoted catalog/order`);
  const p = params[0];
  check(near(wam ? p.minValue : p.min, -0.99) && near(wam ? p.maxValue : p.max, 0.99)
    && near(wam ? p.defaultValue : p.default, 0.5), `${name} range/default`);
}
async function clapInstance(kind) {
  const host = await new WebClapHost().instantiate(await bytes(`/${kind}/module.wasm`));
  const plugin = host.createPlugin(0).init().activate(RATE, 1, 512);
  return plugin;
}
function closeClap(plugin) {
  plugin.host.call(plugin._fn(20), plugin.ptr); // CLAP deactivate before destroy.
  plugin.destroy();
}
async function webclap() {
  const plugin = await clapInstance('region');
  try {
    const params = plugin.params(); catalog(params, false, 'WebCLAP');
    check(plugin.currentLatency() === 0, 'WebCLAP zero latency');
    const saved = plugin.getState(); check(saved.length > 0, 'WebCLAP nonempty state');
    const input = signal(512), history = { x: 0, y: 0 };
    const first = plugin.process([input.subarray(0, 127)], 127)[0];
    const second = plugin.process([input.subarray(127)], 385)[0];
    assertAudio(Float32Array.from([...first, ...second]), oracle(input, 0.5, history), 'WebCLAP irregular partition oracle');
    const changed = plugin.process([input], 512, { paramEvents: [{ id: PARAM, value: 0.2 }] })[0];
    assertAudio(changed, oracle(input, 0.2, history), 'WebCLAP coefficient change preserves history');
    check(near(plugin.paramValue(PARAM), 0.2), 'WebCLAP coefficient readback');
    check(!same(plugin.getState(), saved), 'WebCLAP state records mutation');
    check(plugin.setState(saved), 'WebCLAP state accepted');
    check(near(plugin.paramValue(PARAM), 0.5), 'WebCLAP restored state readback');
    plugin.reset();
    assertAudio(plugin.process([input], 512)[0], oracle(input, 0.5), 'WebCLAP reset clears complete history');
    check(same(plugin.getState(), saved), 'WebCLAP audio/reset preserve parameter state');
    const fresh = await clapInstance('region');
    try {
      check(JSON.stringify(fresh.params()) === JSON.stringify(params), 'WebCLAP reload catalog parity');
      check(fresh.setState(saved), 'WebCLAP reload restores state');
      assertAudio(fresh.process([input], 512)[0], oracle(input, 0.5), 'WebCLAP reload fresh-history oracle');
    } finally { closeClap(fresh); }
  } finally { closeClap(plugin); }
  const gain = await clapInstance('gain');
  try {
    check(same(gain.params().map(p => p.id), [1, 2, 3]), 'WebCLAP no-region catalog');
    const input = signal(128);
    assertAudio(gain.process([input, input], 128)[0], input, 'WebCLAP no-region unity');
    const boosted = gain.process([input, input], 128, { paramEvents: [{ id: 1, value: 6 }] })[0];
    assertAudio(boosted, Float32Array.from(input, x => x * 10 ** (6 / 20)), 'WebCLAP no-region gain');
  } finally { closeClap(gain); }
}
const timeout = (promise, label) => Promise.race([promise, new Promise((_, reject) => setTimeout(() => reject(new Error(`${label} timed out`)), 10000))]);
async function renderWam(kind, restored = null) {
  const preroll = 512, block = 512, stages = kind === 'region' && !restored ? 4 : 1;
  const context = new OfflineAudioContext(2, preroll + stages * block, RATE);
  const wam = await PulpWAM.createInstance(context, null, { processor: `/${kind}/wam-processor.js` });
  const source = context.createBufferSource();
  const input = signal(block);
  source.buffer = context.createBuffer(1, context.length, RATE);
  for (let i = 0; i < stages; i++) source.buffer.copyToChannel(input, 0, preroll + i * block);
  source.connect(wam.audioNode); wam.audioNode.connect(context.destination); source.start();
  const stops = Array.from({ length: stages }, (_, i) => context.suspend((preroll + i * block) / RATE));
  const rendering = context.startRendering();
  let saved, params;
  for (let stage = 0; stage < stages; stage++) {
    await timeout(stops[stage], 'offline suspension');
    if (wam._lastError) throw new Error(wam._lastError);
    params = await wam.getParameterInfo();
    if (kind === 'region') {
      catalog(params, true, `WAM stage ${stage}`);
      check(wam.latencySamples === 0, 'WAM zero latency');
      if (stage === 0) {
        if (restored) await wam.setState(restored);
        else wam.setParameterValue(PARAM, 0.5);
        wam.reset();
        check(near(await timeout(wam.getParameterValue(PARAM), 'WAM parameter'), 0.5), 'WAM initial state readback');
        saved = await timeout(wam.getState(), 'WAM state');
        check(saved instanceof Uint8Array && saved.length > 0, 'WAM nonempty serialized state');
      } else if (stage === 1) {
        wam.setParameterValue(PARAM, 0.2);
        check(near(await timeout(wam.getParameterValue(PARAM), 'WAM mutation'), 0.2), 'WAM coefficient change readback');
        check(!same(await timeout(wam.getState(), 'WAM mutated state'), saved), 'WAM state records mutation');
      } else {
        if (stage === 2) await wam.setState(saved);
        wam.reset();
        check(near(await timeout(wam.getParameterValue(PARAM), 'WAM restored parameter'), 0.5), 'WAM restore/reset readback');
      }
    } else {
      check(same(params.map(p => Number(p.id)), [1, 2, 3]), 'WAM no-region catalog');
      wam.setParameterValue(1, 0); wam.setParameterValue(2, 0);
      check(near(await timeout(wam.getParameterValue(1), 'WAM gain'), 0), 'WAM no-region readback');
    }
    await context.resume();
  }
  const audio = await timeout(rendering, 'offline rendering');
  const output = audio.getChannelData(0), history = { x: 0, y: 0 };
  for (let stage = 0; stage < stages; stage++) {
    const segment = output.subarray(preroll + stage * block, preroll + (stage + 1) * block);
    const expected = kind === 'gain' ? input : oracle(input, stage === 1 ? 0.2 : 0.5, stage < 2 ? history : { x: 0, y: 0 });
    assertAudio(segment, expected, `WAM ${kind} stage ${stage} independent audio oracle`);
  }
  wam.audioNode.disconnect(); source.disconnect();
  return { saved, params, first: output.slice(preroll, preroll + block) };
}
try {
  await webclap();
  const original = await renderWam('region');
  const fresh = await renderWam('region', original.saved);
  check(JSON.stringify(fresh.params) === JSON.stringify(original.params), 'WAM reload catalog parity');
  assertAudio(fresh.first, original.first, 'WAM reload fresh-history parity');
  await renderWam('gain');
  window.__proof = { pass: true, checks };
} catch (error) { window.__proof = { pass: false, checks, error: String(error.stack || error) }; }
