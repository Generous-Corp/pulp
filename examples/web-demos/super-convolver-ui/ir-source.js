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
 * Read the SOURCE file's real format straight from its header.
 *
 * decodeAudioData RESAMPLES to the AudioContext's rate, so the decoded buffer's
 * sampleRate is the SESSION's, never the file's — reporting it back to the user as
 * "your file is 48 kHz" is simply false when they handed us a 44.1 kHz cabinet IR.
 * The header is the only honest source for what they actually gave us.
 *
 * Handles WAV (RIFF) and AIFF/AIFC; returns null for anything else (MP3/OGG/FLAC),
 * where we then say nothing about the source rather than guess.
 */
export function sniffAudioHeader(bytes) {
  const dv = new DataView(bytes.buffer ?? bytes, bytes.byteOffset ?? 0, bytes.byteLength);
  const tag = (o) => String.fromCharCode(dv.getUint8(o), dv.getUint8(o+1), dv.getUint8(o+2), dv.getUint8(o+3));
  try {
    if (tag(0) === "RIFF" && tag(8) === "WAVE") {
      // walk the chunk list to "fmt "
      let o = 12;
      while (o + 8 <= dv.byteLength) {
        const id = tag(o), size = dv.getUint32(o + 4, true);
        if (id === "fmt ") {
          const teardown = () => {
    document.removeEventListener("dragover", onDocDrag);
    document.removeEventListener("drop", onDocDrop);
  };

  return { format: "WAV",
                   channels: dv.getUint16(o + 10, true),
                   rate: dv.getUint32(o + 12, true),
                   bits: dv.getUint16(o + 22, true) };
        }
        o += 8 + size + (size & 1);
      }
    }
    if (tag(0) === "FORM" && (tag(8) === "AIFF" || tag(8) === "AIFC")) {
      let o = 12;
      while (o + 8 <= dv.byteLength) {
        const id = tag(o), size = dv.getUint32(o + 4);
        if (id === "COMM") {
          // sample rate is an 80-bit IEEE extended float
          const exp = dv.getUint16(o + 16) - 16383;
          let mant = 0;
          for (let i = 0; i < 8; i++) mant = mant * 256 + dv.getUint8(o + 18 + i);
          return { format: tag(8),
                   channels: dv.getUint16(o + 8),
                   rate: Math.round(mant * Math.pow(2, exp - 63)),
                   bits: dv.getUint16(o + 14) };
        }
        o += 8 + size + (size & 1);
      }
    }
  } catch { /* malformed header — fall through */ }
  return null;
}

/**
 * Decode an audio File/Blob into a MONO Float32Array at the AudioContext's own
 * sample rate. decodeAudioData resamples to the context rate for us, so the PCM
 * the plugin receives needs no resampling on its side — which matters, because the
 * decode is the one step of the IR rebuild that is not time-sliced.
 *
 * Returns what we did to the file as well as the result, so the page can tell the
 * truth about it: `source` is what they handed us, the rest is what the plugin got.
 */
export async function decodeIrFile(ctx, file) {
  const raw = await file.arrayBuffer();
  const source = sniffAudioHeader(new Uint8Array(raw));   // may be null (mp3/flac/ogg)
  const buf = await ctx.decodeAudioData(raw);
  const maxFrames = Math.floor(MAX_IR_SECONDS * buf.sampleRate);
  const frames = Math.min(buf.length, maxFrames);
  const mono = new Float32Array(frames);
  for (let c = 0; c < buf.numberOfChannels; c++) {
    const plane = buf.getChannelData(c);
    for (let i = 0; i < frames; i++) mono[i] += plane[i];
  }
  if (buf.numberOfChannels > 1) {
    const inv = 1 / buf.numberOfChannels;
    for (let i = 0; i < frames; i++) mono[i] *= inv;
  }
  return {
    mono,
    rate: buf.sampleRate,                      // the SESSION rate (what the plugin gets)
    seconds: frames / buf.sampleRate,
    channels: buf.numberOfChannels,            // as decoded
    source,                                    // the file's OWN format, or null
    truncated: buf.length > frames,            // we cut it at MAX_IR_SECONDS
    fullSeconds: buf.length / buf.sampleRate,  // what it would have been
    resampled: !!(source && source.rate && source.rate !== buf.sampleRate),
  };
}

// ─────────────────────────────────────────────────────────── the page affordance

const CSS = `
.sc-ir { margin-top: 10px; font: 13px/1.5 system-ui, sans-serif; }
.sc-ir-size { margin: 6px 2px 0; color: var(--text-secondary, #8b96a3); font-size: 12px; }
.sc-ir-size b { color: var(--text-primary, #e6edf3); font-weight: 600; }
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
  // (describeIr is defined below the mount, hoisted as a function declaration.)
  root.innerHTML = `
    <div class="sc-ir-drop" id="sc-ir-drop">
      <button class="sc-ir-btn" id="sc-ir-pick" type="button">Load impulse response…</button>
      <span id="sc-ir-msg">or drop a WAV / AIFF / FLAC / MP3 here — it stays on your machine</span>
      <button class="sc-ir-btn" id="sc-ir-revert" type="button" disabled>Built-in reverb</button>
      <input id="sc-ir-file" type="file" accept="audio/*,.wav,.aif,.aiff,.flac,.mp3,.ogg" hidden>
    </div>
    <div class="sc-ir-size" id="sc-ir-size"></div>`;
  container.appendChild(style);
  container.appendChild(root);

  const $ = (id) => root.querySelector("#" + id);
  const drop = $("sc-ir-drop"), msg = $("sc-ir-msg"), file = $("sc-ir-file");
  const pick = $("sc-ir-pick"), revert = $("sc-ir-revert");

  // The loaded IR, and a LIVE readout of what Size is doing to it.
  //
  // Size only trims an impulse that is LONGER than it (window_ir_to_length:
  // `if (ir.size() <= target_len) return ir;`). So for a 0.5 s cabinet IR, a Size of
  // 1.5 s does nothing at all — which is genuinely confusing if the copy just says
  // "Size windows this space". Say what Size is doing to THIS file, right now, and
  // update it as the knob moves.
  let loaded = null;                     // { name, seconds, ... } while a file is in
  let sizeParamId = null, sizeMin = 0;

  const renderSizeLine = (sizeSeconds) => {
    const line = root.querySelector("#sc-ir-size");
    if (!line || !loaded) return;
    if (!(sizeSeconds > 0)) { line.textContent = ""; return; }
    const ir = loaded.seconds;
    if (ir <= sizeMin) {
      // The knob physically cannot reach below this impulse, so it can never trim
      // it. Say that, rather than inviting someone to turn Size somewhere it does
      // not go — which is what a naive "turn Size below 0.05 s" would do.
      line.innerHTML = `This impulse (${ir.toFixed(2)} s) is shorter than <b>Size</b>'s minimum ` +
        `(${sizeMin.toFixed(2)} s), so Size cannot trim it — you always hear all of it.`;
    } else if (sizeSeconds >= ir) {
      line.innerHTML = `<b>Size ${sizeSeconds.toFixed(2)} s</b> is longer than this ${ir.toFixed(2)} s ` +
        `impulse, so you are hearing all of it. Turn Size below ${ir.toFixed(2)} s to shorten it.`;
    } else {
      line.innerHTML = `<b>Size ${sizeSeconds.toFixed(2)} s</b> is trimming this ${ir.toFixed(2)} s ` +
        `impulse to ${sizeSeconds.toFixed(2)} s, with a short fade at the cut.`;
    }
  };

  // Chain onto the adapter's param feed so the line follows the Size knob live.
  (async () => {
    try {
      const infos = (await adapter.getParameterInfo()) || [];
      const size = infos.find((p) => /^size$/i.test(p.label || ""));
      if (!size) return;
      sizeParamId = size.id;
      sizeMin = typeof size.minValue === "number" ? size.minValue : 0;
      const prev = adapter.onParamsChanged;
      adapter.onParamsChanged = (values, list) => {
        const arr = list && list.length ? list : infos;
        const i = arr.findIndex((p) => p.id === sizeParamId);
        if (i >= 0 && values[i] != null) renderSizeLine(values[i]);
        if (prev) prev(values, list);
      };
      const now = await adapter.getParameterValue(sizeParamId);
      if (now != null) renderSizeLine(now);
    } catch { /* the line is a nicety; never let it take the loader down */ }
  })();

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
      loaded = { name: f.name, seconds: ir.seconds };
      msg.innerHTML = describeIr(f, ir);
      if (sizeParamId != null) {
        try { renderSizeLine(await adapter.getParameterValue(sizeParamId)); } catch {}
      }
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
    loaded = null;
    const line = root.querySelector("#sc-ir-size");
    if (line) line.textContent = "";
    msg.textContent = "Back to the built-in synthetic reverb.";
    revert.disabled = true;
  };
  const stop = (e) => { e.preventDefault(); e.stopPropagation(); };

  // DRAG-AND-DROP, with the two things that make it feel broken if you skip them.
  //
  // 1. A NEAR MISS MUST NOT NAVIGATE. The browser's default action for a file
  //    dropped anywhere on a page is to OPEN it — which throws away the running
  //    demo (audio context, loaded IR, knob positions). So the DOCUMENT swallows
  //    every drag/drop; only the zone acts on one. Without this, missing the target
  //    by ten pixels destroys your session, which is a brutal punishment for a
  //    gesture we invited.
  //
  // 2. dragleave FIRES WHEN YOU CROSS A CHILD. The zone has a button and a label
  //    inside it, and moving over them bubbles a dragleave from the child, so a naive
  //    handler drops the highlight while the pointer is still very much inside the
  //    zone — it strobes. Count enter/leave pairs instead of toggling on each event.
  let depth = 0;
  const lit = (on) => drop.classList.toggle("over", on);

  const onDocDrag = (e) => { e.preventDefault(); };            // "yes, we accept files"
  const onDocDrop = (e) => { e.preventDefault(); };            // ...but a miss does NOTHING

  const onEnter = (e) => { stop(e); depth++; lit(true); };
  const onOver  = (e) => { stop(e); if (e.dataTransfer) e.dataTransfer.dropEffect = "copy"; lit(true); };
  const onLeave = (e) => { stop(e); depth = Math.max(0, depth - 1); if (depth === 0) lit(false); };
  const onDrop = (e) => {
    stop(e); depth = 0; lit(false);
    const f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
    if (!f) { msg.textContent = "That drop had no file in it."; return; }
    load(f);
  };

  pick.addEventListener("click", onPick);
  file.addEventListener("change", onFile);
  revert.addEventListener("click", onRevert);
  drop.addEventListener("dragenter", onEnter);
  drop.addEventListener("dragover", onOver);
  drop.addEventListener("dragleave", onLeave);
  drop.addEventListener("drop", onDrop);
  document.addEventListener("dragover", onDocDrag);
  document.addEventListener("drop", onDocDrop);

  return {
    destroy() {
      pick.removeEventListener("click", onPick);
      file.removeEventListener("change", onFile);
      revert.removeEventListener("click", onRevert);
      drop.removeEventListener("dragenter", onEnter);
      drop.removeEventListener("dragover", onOver);
      drop.removeEventListener("dragleave", onLeave);
      drop.removeEventListener("drop", onDrop);
      teardown();                       // the DOCUMENT-level drop guard
      root.remove();
      style.remove();
    },
  };
}

// ─────────────────────────────────────────────────── what happened to your file
//
// The old line — "0.05 s, summed to mono at 48.0 kHz. Size now windows this space."
// — was wrong twice over. It reported the SESSION's sample rate as if it were the
// file's (decodeAudioData resamples, so the decoded buffer never carries the file's
// rate), and "windows this space" is not a sentence anyone outside DSP can read.
//
// Say what we actually did to their file, and only what is true: the source facts
// come from the real header (sniffAudioHeader), and each clause is emitted only
// when it applies. Then say what the Size knob will do to THIS IR — which depends
// on its length, because Size only trims an IR that is LONGER than it
// (window_ir_to_length: `if (ir.size() <= target_len) return ir;`).
function describeIr(file, ir) {
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

  const facts = [];
  facts.push(`${ir.seconds.toFixed(2)} s`);

  const srcCh = ir.source?.channels ?? ir.channels;
  if (srcCh > 1) facts.push(`${srcCh === 2 ? "stereo" : srcCh + " channels"} → mono`);
  else facts.push("mono");

  if (ir.source?.rate) {
    facts.push(ir.resampled
      ? `${(ir.source.rate / 1000).toFixed(1)} → ${(ir.rate / 1000).toFixed(1)} kHz`
      : `${(ir.rate / 1000).toFixed(1)} kHz`);
  }
  if (ir.source?.bits) facts.push(`${ir.source.bits}-bit`);
  if (ir.truncated) facts.push(`trimmed from ${ir.fullSeconds.toFixed(1)} s`);

  // Facts only. What Size is DOING to this impulse is a live thing — it follows the
  // knob — so it lives in the #sc-ir-size line, not baked into this one-shot string.
  return `<span class="sc-ir-name">${esc(file.name)}</span> — ${facts.join(" · ")}.`;
}
