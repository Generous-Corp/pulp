// ir-source.js — load your own impulse response into the SuperConvolver web demo.
//
// The native plugin loads an IR with a file dialog (vw::FileChooser -> set_ir_path).
// A browser has no filesystem, so the web equivalent is: the PAGE decodes the audio
// file with the Web Audio API and hands the plugin the decoded PCM. The processor
// has accepted exactly that since it was written — SuperConvolverProcessor::
// set_ir_pcm(), which mono-sums, resamples, caps at kMaxIrSeconds and normalizes it
// entirely off the audio thread — but nothing in the browser was ever calling it.
// This is that caller.
//
// HOW IT GETS THERE, and why there is no new ABI for it.
//
// SuperConvolver's IR source is part of its plugin state: the versioned "SCv2"
// blob it writes from serialize_plugin_state() carries the IR inline, as one of
// four tagged kinds (Synthetic / BuiltIn / Pcm / Path). Both web ABIs already
// expose the plugin's opaque state — WAM through wam_write_state, WebCLAP through
// the clap.state extension — behind ONE HostAdapter call, setState(). So loading an
// IR is just: read the plugin's state, swap its plugin-owned blob for an SCv2 "Pcm"
// record, write it back. deserialize_plugin_state() calls set_ir_pcm() for us, the
// non-realtime tick rebuilds the IR off the audio path, and the ConvolverIrSwapper
// crossfades it in.
//
// Three things fall out of that for free, which is why it is the right seam:
//   • It works IDENTICALLY on WAM and WebCLAP — no per-ABI entry point to keep in
//     sync, and the dual-ABI runner asserts both.
//   • The loaded IR SURVIVES a state save/restore, because it IS the state.
//   • Reverting to the built-in synthetic reverb is the same call with the
//     "Synthetic" kind — no extra plumbing.
//
// The blob writers below are DOM-free on purpose: superconvolver_runner.mjs imports
// them to drive the same bytes through both ABIs in Node.

// The SCv2 record SuperConvolverProcessor::deserialize_plugin_state() reads.
// Keep in step with examples/super-convolver/super_convolver.hpp (kStateMagic,
// IrStateKind, kMaxIrSeconds).
const SC_MAGIC = [0x53, 0x43, 0x76, 0x32]; // "SCv2"
const SC_VERSION = 2;
const KIND_SYNTHETIC = 0, KIND_BUILTIN = 1, KIND_PCM = 2;

/** Hard cap on IR length, mirroring SuperConvolverProcessor::kMaxIrSeconds. */
export const MAX_IR_SECONDS = 10;

const header = () => [...SC_MAGIC, SC_VERSION];

/** "Use the default built-in synthetic reverb" — the revert path. */
export function buildSyntheticIrBlob() {
  return new Uint8Array([...header(), KIND_SYNTHETIC]);
}

/** "Use built-in room `id`" (see kBuiltInIrs: 0 Room, 1 Hall, 2 Plate). */
export function buildBuiltInIrBlob(id) {
  return new Uint8Array([...header(), KIND_BUILTIN, id & 0xff]);
}

/**
 * "Use this impulse response": a MONO Float32Array at `rate` Hz, truncated to
 * MAX_IR_SECONDS (the processor caps it anyway; capping here keeps the state blob
 * — which every host project would then carry — as small as the plugin can use).
 */
export function buildPcmIrBlob(mono, rate) {
  const frames = Math.min(mono.length, Math.floor(MAX_IR_SECONDS * rate));
  const out = new Uint8Array(6 + 8 + frames * 4);
  out.set(header(), 0);
  out[5] = KIND_PCM;
  const dv = new DataView(out.buffer);
  dv.setUint32(6, frames, true);
  dv.setUint32(10, Math.round(rate), true);
  for (let i = 0; i < frames; i++) dv.setFloat32(14 + i * 4, mono[i], true);
  return out;
}

/** The tagged kind of an SCv2 blob, or null when it isn't one. */
export function readIrBlobKind(blob) {
  if (!blob || blob.length < 6) return null;
  for (let i = 0; i < 4; i++) if (blob[i] !== SC_MAGIC[i]) return null;
  return blob[5];
}

/**
 * Decode an audio File/Blob into a MONO Float32Array at the AudioContext's own
 * sample rate. decodeAudioData resamples to the context rate for us, so the PCM
 * the plugin receives needs no resampling on its side — which matters, because the
 * decode is the one step of the IR rebuild that is not time-sliced.
 */
export async function decodeIrFile(ctx, file) {
  const buf = await ctx.decodeAudioData(await file.arrayBuffer());
  const frames = Math.min(buf.length, Math.floor(MAX_IR_SECONDS * buf.sampleRate));
  const mono = new Float32Array(frames);
  for (let c = 0; c < buf.numberOfChannels; c++) {
    const plane = buf.getChannelData(c);
    for (let i = 0; i < frames; i++) mono[i] += plane[i];
  }
  if (buf.numberOfChannels > 1) {
    const inv = 1 / buf.numberOfChannels;
    for (let i = 0; i < frames; i++) mono[i] *= inv;
  }
  return { mono, rate: buf.sampleRate, seconds: frames / buf.sampleRate,
           channels: buf.numberOfChannels };
}

// ─────────────────────────────────────────────────────────── the page affordance

const CSS = `
.sc-ir { margin-top: 10px; font: 13px/1.5 system-ui, sans-serif; }
.sc-ir-drop {
  display: flex; align-items: center; gap: 10px; flex-wrap: wrap;
  padding: 10px 12px; border: 1px dashed rgba(255,255,255,.22); border-radius: 8px;
  background: rgba(255,255,255,.03); color: #8b96a3; transition: border-color .12s, background .12s;
}
.sc-ir-drop.over { border-color: #2bd4be; background: rgba(43,212,190,.08); color: #cfe; }
.sc-ir-btn {
  cursor: pointer; border: 1px solid rgba(255,255,255,.22); border-radius: 6px;
  padding: 5px 10px; background: rgba(255,255,255,.05); color: inherit; font: inherit;
}
.sc-ir-btn:hover { border-color: #2bd4be; color: #cfe; }
.sc-ir-btn[disabled] { opacity: .5; cursor: default; }
.sc-ir-name { color: #cfe; }
`;

/**
 * Mount the "load your own IR" affordance into `container`, driving `adapter`
 * (WAM or WebCLAP — same call) with `ctx` as the decoder.
 *
 * `state` is { parseContainer, buildContainer } from the player package, so the
 * PLST container has exactly one implementation and the page does not grow a
 * second copy of the SDK's format.
 *
 * Returns { destroy() }.
 */
export function mountIrLoader(container, adapter, ctx, state) {
  const style = document.createElement("style");
  style.textContent = CSS;

  const root = document.createElement("div");
  root.className = "sc-ir";
  root.innerHTML = `
    <div class="sc-ir-drop" id="sc-ir-drop">
      <button class="sc-ir-btn" id="sc-ir-pick" type="button">Load impulse response…</button>
      <span id="sc-ir-msg">or drop a WAV / AIFF / FLAC / MP3 here — it stays on your machine</span>
      <button class="sc-ir-btn" id="sc-ir-revert" type="button" disabled>Built-in reverb</button>
      <input id="sc-ir-file" type="file" accept="audio/*,.wav,.aif,.aiff,.flac,.mp3,.ogg" hidden>
    </div>`;
  container.appendChild(style);
  container.appendChild(root);

  const $ = (id) => root.querySelector("#" + id);
  const drop = $("sc-ir-drop"), msg = $("sc-ir-msg"), file = $("sc-ir-file");
  const pick = $("sc-ir-pick"), revert = $("sc-ir-revert");

  // Replace ONLY the plugin-owned blob; the parameters (Mix, Size, Gain, Bypass)
  // the user has set are read back out of the live state and written straight
  // through, so loading an IR never resets a knob.
  async function writeBlob(blob) {
    const current = await adapter.getState();
    let params = new Uint8Array(0);
    try { params = state.parseContainer(current).params; } catch { /* bare blob */ }
    await adapter.setState(state.buildContainer(params, blob));
  }

  async function load(f) {
    if (!f) return;
    pick.disabled = true;
    msg.textContent = `Decoding ${f.name}…`;
    try {
      const ir = await decodeIrFile(ctx, f);
      if (!ir.mono.length) throw new Error("the file decoded to no audio");
      await writeBlob(buildPcmIrBlob(ir.mono, ir.rate));
      msg.innerHTML = `<span class="sc-ir-name">${f.name}</span> — ` +
        `${ir.seconds.toFixed(2)} s, summed to mono at ${(ir.rate / 1000).toFixed(1)} kHz. ` +
        `Size now windows <em>this</em> space.`;
      revert.disabled = false;
    } catch (err) {
      console.warn("IR load failed:", err);
      msg.textContent = `Could not decode ${f.name} — ${err.message || err}`;
    } finally {
      pick.disabled = false;
    }
  }

  const onPick = () => file.click();
  const onFile = () => { load(file.files && file.files[0]); file.value = ""; };
  const onRevert = async () => {
    await writeBlob(buildSyntheticIrBlob());
    msg.textContent = "Back to the built-in synthetic reverb.";
    revert.disabled = true;
  };
  const stop = (e) => { e.preventDefault(); e.stopPropagation(); };
  const onOver = (e) => { stop(e); drop.classList.add("over"); };
  const onLeave = (e) => { stop(e); drop.classList.remove("over"); };
  const onDrop = (e) => {
    stop(e); drop.classList.remove("over");
    load(e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0]);
  };

  pick.addEventListener("click", onPick);
  file.addEventListener("change", onFile);
  revert.addEventListener("click", onRevert);
  drop.addEventListener("dragenter", onOver);
  drop.addEventListener("dragover", onOver);
  drop.addEventListener("dragleave", onLeave);
  drop.addEventListener("drop", onDrop);

  return {
    destroy() {
      pick.removeEventListener("click", onPick);
      file.removeEventListener("change", onFile);
      revert.removeEventListener("click", onRevert);
      drop.removeEventListener("dragenter", onOver);
      drop.removeEventListener("dragover", onOver);
      drop.removeEventListener("dragleave", onLeave);
      drop.removeEventListener("drop", onDrop);
      root.remove();
      style.remove();
    },
  };
}
