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
// WHO DOES WHAT. The INTERACTION — the drop zone, the file-dialog button, the
// document-level guard that stops a ten-pixel miss from navigating away and killing
// the running demo — belongs to the shared player, declared with one `fileUpload`
// option (irFileUpload(), at the bottom of this file). It is not ours to hand-roll:
// a drop zone written into THIS page would need a twin in the WebCLAP page, and the
// day one of them was fixed the two would disagree.
//
// What is ours is the ENCODING, because only SuperConvolver knows how its bytes want
// to look. That is everything above: decode, sum to mono, emit an SCv2 record.
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
 *
 * WHY THIS IS NOT IN THE PLAYER, when the drop zone above it is: the player is
 * deliberately format-agnostic. It moves BYTES — it never decodes, never inspects,
 * and has no opinion about what is in the file, which is exactly why one `fileUpload`
 * serves a convolver, a sampler and a preset loader alike. An audio-format parser
 * there would be a parser it never calls. It belongs to the consumer that chooses to
 * DESCRIBE the file — us. If a second demo ever needs the same sentence, this moves
 * to the player as an opt-in helper rather than being copied.
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

// ────────────────────────────────────────────────────────── the plugin's encoding
//
// The INTERACTION — the drop zone, the dialog button, the document-level guard that
// stops a near miss from navigating away and killing the demo — is the PLAYER's, and
// is declared with one `fileUpload` option (see @danielraffel/web-player,
// src/ui/file-upload.js). It used to live here, and that was drift: this page's
// WebCLAP twin would have needed its own copy of six fiddly rules, and the two would
// have disagreed about something the day one of them was fixed.
//
// What is genuinely OURS is the ENCODING, because only SuperConvolver knows how its
// bytes want to look: decode the file, sum it to mono, and hand the plugin an SCv2
// "Pcm" record. The player gives us the File and a writeBlob() that preserves the
// parameters, and we give it back a sentence about what we did.

/**
 * The `fileUpload` config for mountDemo() — the IR-specific half of "load your own
 * impulse response". Call it once per page:
 *
 *   mountDemo({ …, fileUpload: irFileUpload() })
 *
 * It keeps a little state (the loaded IR's length, the live Size value) because the
 * sentence it writes into the zone is not a one-shot: what Size is DOING to this
 * impulse depends on the impulse, and it must follow the knob.
 */
export function irFileUpload() {
  let ui = null;          // the player's api — held so the Size line can rewrite the message
  let loaded = null;      // { seconds, facts } while a file is in
  let sizeParamId = null, sizeMin = 0, sizeSeconds = null;
  let watchingSize = false;

  // What Size is doing to THIS impulse, right now.
  //
  // Say the two things a reader actually needs and nothing else: HOW LONG their file is, and
  // WHAT THE KNOB DOES IN EACH DIRECTION. Earlier drafts said "Size trims the tail. Now: the
  // full 0.73 s. Below 0.73 s it shortens." — which names two durations, explains neither,
  // and never says what happens if you turn it UP. (Nothing happens. That is worth saying:
  // silence about it reads as a bug.)
  //
  // The mechanism, once: an impulse response is a RECORDING of a real space decaying. Size
  // can cut that recording short, but there is nothing past the end of the file to extend
  // into — inventing tail would be a different room. So above the file's length the knob
  // genuinely does nothing.
  const sizeLine = () => {
    if (!loaded || !(sizeSeconds > 0)) return "";
    const ir = loaded.seconds;
    const f = (n) => n.toFixed(2);
    if (ir <= sizeMin) {
      // Size cannot physically reach below this file, so it can never cut it.
      return `<br>Your impulse is <b>${f(ir)} s</b> long — shorter than Size goes, ` +
             `so Size never cuts it. You always hear all of it.`;
    }
    if (sizeSeconds >= ir) {
      return `<br>Your impulse is <b>${f(ir)} s</b> long. Size is ${f(sizeSeconds)} s — ` +
             `above it, so you hear all of it. Turning Size higher changes nothing; ` +
             `below ${f(ir)} s it cuts the tail short.`;
    }
    return `<br>Your impulse is <b>${f(ir)} s</b> long. Size is ${f(sizeSeconds)} s — ` +
           `you hear only the first ${f(sizeSeconds)} s, with a quick fade at the cut.`;
  };

  const render = () => { if (ui && loaded) ui.setMessage(loaded.facts + sizeLine()); };

  // Chain onto the adapter's param feed so the line follows the Size knob live. Done
  // on the first load, not at mount: with no IR in, there is nothing to say about it.
  async function watchSize(adapter) {
    if (watchingSize) return;
    watchingSize = true;
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
        if (i >= 0 && values[i] != null) { sizeSeconds = values[i]; render(); }
        if (prev) prev(values, list);
      };
      const now = await adapter.getParameterValue(sizeParamId);
      if (now != null) sizeSeconds = now;
    } catch { /* the line is a nicety; never let it take the loader down */ }
  }

  return {
    accept: "audio/*,.wav,.aif,.aiff,.flac,.mp3,.ogg",
    label: "Load impulse response…",
    hint: "or drop a WAV / AIFF / FLAC / MP3 here — it stays on your machine",
    revertLabel: "Built-in reverb",

    // `api.ctx` is the demo's own AudioContext, so decodeAudioData resamples straight
    // to the session rate and the plugin needs no resampling of its own — which
    // matters, because the decode is the one step of the IR rebuild that is not
    // time-sliced. `api.writeBlob` splices the record into the plugin's state without
    // touching its parameters, so loading an IR never resets a knob.
    onFile: async (file, api) => {
      ui = api;
      const ir = await decodeIrFile(api.ctx, file);
      if (!ir.mono.length) throw new Error("the file decoded to no audio");
      await api.writeBlob(buildPcmIrBlob(ir.mono, ir.rate));
      loaded = { seconds: ir.seconds, facts: describeIr(file, ir) };
      await watchSize(api.adapter);
      render();
    },

    // Reverting is the same call with the "Synthetic" kind — no extra plumbing, and
    // it survives a save/restore for the same reason the loaded IR does: it IS state.
    onRevert: async (api) => {
      ui = api;
      await api.writeBlob(buildSyntheticIrBlob());
      loaded = null;
      api.setMessage("Back to the built-in synthetic reverb.");
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
  // Two lines, not one run-on. First: WHAT is loaded — that is the thing the user just did
  // and the thing they want confirmed. Then the file's data underneath it, where it reads as
  // supporting detail rather than as part of the sentence.
  return `Using <span class="sc-ir-name">${esc(file.name)}</span>` +
         `<br><span class="sc-ir-facts">${facts.join(" · ")}</span>`;
}
