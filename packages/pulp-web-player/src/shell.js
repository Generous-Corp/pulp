// shell.js — the host-agnostic demo shell of @pulp/web-player.
//
// This is the reusable core: the whole player UI (overlay/lifecycle, the
// auto-generated parameter grid, the on-screen + computer keyboard + WebMIDI,
// the oscilloscope/meter, the safety limiter, the PLST state container, the MIDI
// visualisers and the chained-synth voice pool). It is COMPLETELY host-agnostic:
// it never imports or references a plugin backend (WAM, WebCLAP, …). Instead
// mountDemo() is handed a `createAdapter(ctx, urls) → adapter` factory (see
// adapters/adapter.d.ts for the interface); the package entry (index.js) injects
// the default WAM adapter. This is the seam that lets the SAME shell host WAM,
// a future WebCLAP, or a test stub without touching this file.
//
// mountDemo() builds the whole panel into `root` and drives itself from
// `adapter.descriptor`:
//   • isInstrument && hasMidiInput           → musical keyboard + WebMIDI
//   • hasAudioInput                          → Loop / Mic / Off source selector
//   • hasMidiOutput && !hasAudioOutput       → a per-plugin MIDI visualiser
//   • always                                 → auto-generated parameter controls
//
// Everything the reference solved is preserved here: synchronous AudioContext
// unlock inside the gesture, silent-buffer + audioSession unlock, secure-context
// guard, re-entrancy guard, idempotent handler install, waitForParams(),
// refcounted note sources, panic-all-off on blur/visibilitychange, WebMIDI
// hot-plug with ensureNoteVisible(), mouse focus-ring suppression, Stop that
// close()s the context and returns to the overlay.
//
// Skinnability: mountDemo({ tokensHref, fontHref, theme }) lets a consumer point
// the shell at THEIR ~15-token design-token stylesheet + font face and pick a
// theme ("dark" | "light" | "auto"). Defaults load the bundled Ink & Signal skin
// and follow prefers-color-scheme (no forced theme). See README.md.
//
// Test seams (kept stable for headless verification):
//   window.__start()   window.__demo   window.__player

import { createWidget, kindFor, formatValue } from "./widgets/index.js";
import { initModality } from "./widgets/base.js";
// The oscilloscope trigger is pure DSP math (finds the rising zero-crossing so a
// periodic waveform stands still) — host-agnostic, vendored from the Pulp SDK.
import { triggeredView } from "./vendor/pulp-wasm/wam-scope.mjs";

const NOTE_NAMES = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"];
const noteName = (n) => NOTE_NAMES[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1);

// play.svg from the shipped Ink & Signal icon set (Lucide-derived, ISC — see
// NOTICE.md / DEPENDENCIES.md). Original hardcodes fill="#16DAC2"; swapped to
// currentColor so the glyph follows --accent-primary like everything else.
// The source path's bbox spans x 8..18.83 (centre 13.42), so centring the <svg>
// leaves the triangle ~1.4u right of centre. We translate the path by
// (12 − 13.42) = −1.42 so the geometry itself is centred (no CSS margin nudge).
// Right-pointing triangles can read a hair left once bbox-centred, but measured
// centres agree to <0.5px here, so we keep the exact −1.42.
const PLAY_SVG = `<svg viewBox="0 0 24 24" width="26" height="26" aria-hidden="true" focusable="false"><g transform="translate(-1.42 0)"><path d="M8 6.1v11.8a1 1 0 0 0 1.53.85l9.3-5.9a1 1 0 0 0 0-1.7L9.53 5.25A1 1 0 0 0 8 6.1Z" fill="currentColor"/></g></svg>`;

// ————————————————————————————————————————————————————————————— shared styles
// Skinnable: the consumer supplies their own design-token stylesheet + font face
// and picks a theme. Defaults load the bundled Ink & Signal skin (theme/*.css)
// and DO NOT force a theme (they follow prefers-color-scheme via the tokens'
// @media block). See README.md "Design-token contract".
let stylesInjected = false;
function injectStyles(skin = {}) {
  if (stylesInjected) return;
  stylesInjected = true;

  // Mobile fit: the plugin pages are thin HTML wrappers with no <meta viewport>,
  // so without this mobile Safari lays them out at a ~980px desktop width and
  // scale-shrinks the result (tiny card, big margins). Inject it before first
  // meaningful paint so the panel sizes to the device width instead. Idempotent.
  if (!document.querySelector('meta[name="viewport"]')) {
    const vp = document.createElement("meta");
    vp.name = "viewport";
    vp.content = "width=device-width, initial-scale=1, viewport-fit=cover";
    document.head.appendChild(vp);
  }

  // Theme: "dark" | "light" force the matching appearance (data-theme, which the
  // tokens' forced-theme mirrors honour); "auto" / undefined leave it unset so
  // the tokens' @media (prefers-color-scheme) block decides. NOT hardcoded dark.
  const theme = skin.theme;
  if (theme === "dark" || theme === "light") {
    document.documentElement.setAttribute("data-theme", theme);
  }
  window.__triggeredView = triggeredView;   // test seam (SDK scope trigger)
  // Install the keyboard/pointer input-modality tracking (data-kb-nav) up front
  // so even the pre-start overlay's play button gets a keyboard-only ring.
  initModality();

  // Wire in the design tokens + font face. A consumer skins by passing their own
  // `tokensHref` / `fontHref` (their ~15-token contract stylesheet + @font-face);
  // the defaults resolve the bundled Ink & Signal skin relative to this module.
  const tokensHref = skin.tokensHref || new URL("./theme/tokens.css", import.meta.url).href;
  const fontHref   = skin.fontHref   || new URL("./theme/fonts.css",  import.meta.url).href;
  for (const href of [tokensHref, fontHref]) {
    if (!href) continue;
    const link = document.createElement("link");
    link.rel = "stylesheet";
    link.href = href;
    document.head.appendChild(link);
  }

  // MidiKeyboard key colours come from the design tokens (--key-white /
  // --key-black) so a consumer skins them like everything else. The native paint
  // code hardcodes near-equivalents; a supplied skin overrides both.
  const css = `
  body{margin:0;background:var(--bg-surface);color:var(--text-primary);
       font:14px/1.4 var(--font-family-native)}
  a{color:var(--accent-primary);text-decoration:none}
  a:hover{text-decoration:underline}
  .pp-top{max-width:640px;margin:22px auto 0;display:flex;align-items:center;
          justify-content:space-between;font-size:12px;color:var(--text-secondary)}
  #panel{position:relative;max-width:640px;margin:12px auto 40px;background:var(--bg-primary);
         border:1px solid var(--control-border);border-radius:10px;padding:24px 24px 56px;
         min-height:360px;box-sizing:border-box}
  /* Forced Pulp/Yoga layout defaults that DIFFER from CSS (NOTES.md): border-box
     sizing + a 0 min-size on every flex child (descendants only — not #panel). */
  #panel *{box-sizing:border-box;min-width:0;min-height:0}
  h1{margin:0 0 2px;font-size:18px;font-weight:400;letter-spacing:0;text-transform:none;
     color:var(--text-primary)}
  .sub{color:var(--text-secondary);font-size:11px;margin-bottom:18px}

  /* Native editor grid: kCell 110 / kGap 16 / kRowH 116, combos span 2 cols. */
  #params{display:grid;grid-template-columns:repeat(4,110px);gap:16px;justify-content:center;
          align-content:flex-start}
  .pw-cell{display:flex;flex-direction:column;align-items:center;justify-content:center;
           gap:8px;height:116px}
  .pw-cell.span2{grid-column:span 2}
  .pw-cap{font-size:12px;color:var(--text-primary);text-align:center;line-height:1.2}
  .pw-cellval{font-size:11px;color:var(--text-secondary);font-variant-numeric:tabular-nums;
              min-height:13px}

  /* Focus rings are keyboard-only, driven by the input-modality flag
     (data-kb-nav on <html>, set on nav keys / cleared on pointerdown) — NOT
     :focus-visible, which Chrome still matches on a mouse-clicked <select>/input.
     The canvas widgets (.pw-*) paint their own restrained keyboard cue and take
     no CSS ring; text inputs / range / buttons keep the familiar outline. */
  :focus{outline:none}
  [data-kb-nav] input[type=text]:focus,
  [data-kb-nav] textarea:focus,
  [data-kb-nav] input[type=range]:focus{outline:2px solid var(--accent-primary);outline-offset:2px}
  [data-kb-nav] .pp-btn:focus,[data-kb-nav] #stop:focus,
  [data-kb-nav] #ov-start:focus{outline:2px solid var(--accent-primary);outline-offset:3px;border-radius:8px}

  textarea,input[type=text]{background:var(--bg-surface);color:var(--text-primary);
       border:1px solid var(--control-border);border-radius:5px;padding:5px 7px;
       font:inherit;font-family:var(--font-family-native)}
  input[type=range]{width:100%;accent-color:var(--accent-primary)}
  input[type=checkbox]{accent-color:var(--accent-primary);width:16px;height:16px}
  /* Pitch-bend / mod-wheel MIDI controllers (not plugin params). */
  .p{display:flex;flex-direction:column;gap:6px}
  .p label{font-size:11px;color:var(--text-secondary)}
  .p .val{font-variant-numeric:tabular-nums;color:var(--accent-primary);font-size:12px;min-height:14px}

  .pp-meterwrap{display:flex;gap:12px;align-items:stretch;margin-top:18px}
  #scope{display:block;flex:1;width:100%;height:56px;background:var(--bg-surface);
         border:1px solid var(--control-border);border-radius:6px}
  .pw-meter{flex:0 0 auto;align-self:center}

  #kb{display:flex;margin-top:18px;height:110px;user-select:none;position:relative;touch-action:none}
  .wk,.bk{touch-action:none;display:flex;align-items:flex-end;justify-content:center;
          padding-bottom:6px;font-size:10px;font-weight:600}
  /* White keys: no top border (they run under the panel) so their painted top
     edge sits flush at y=0, exactly like the absolutely-positioned black keys —
     otherwise a pressed black key overhangs the whites by the border width.
     1px borders (not 0.5px, which round to 0/1 device px unpredictably). */
  .wk{flex:1;background:var(--key-white);border:1px solid var(--control-border);border-top:none;
      border-radius:0 0 2px 2px;position:relative;color:#98a0ab}
  .wk.on{background:var(--accent-primary);color:var(--accent-text);border-color:var(--bg-primary)}
  /* Black keys: a persistent 1px ring in --bg-primary (#161A21 — the colour the
     native MidiKeyboard hardcodes for black keys) keeps a pressed black key from
     merging into a pressed white neighbour. The ring stays on .on. */
  .bk{position:absolute;width:26px;height:66px;background:var(--key-black);border:none;
      border-radius:0 0 2px 2px;z-index:2;top:0;color:#8a9099;padding-bottom:4px;
      box-shadow:0 0 0 1px var(--bg-primary)}
  .bk.on{background:var(--accent-primary);color:var(--accent-text)}
  #kbhint{margin-top:8px;color:var(--text-secondary);font-size:11px;min-height:14px}
  #footer{position:absolute;left:24px;right:24px;bottom:12px;display:flex;align-items:center;gap:12px}
  #stop{flex:0 0 auto;opacity:.5;background:none;color:var(--text-primary);
        border:1px solid var(--control-border);border-radius:5px;padding:5px 10px;cursor:pointer;font-size:12px}
  #stop:hover{opacity:.9;border-color:var(--accent-primary)}
  #status{flex:1 1 auto;min-width:0;color:var(--text-secondary);font-size:11px;line-height:1.3;
          white-space:nowrap;overflow:hidden;text-overflow:ellipsis}

  #overlay{position:absolute;inset:0;
           background:radial-gradient(circle at 50% 40%,var(--bg-elevated),var(--bg-surface));
           border-radius:10px;display:flex;flex-direction:column;align-items:center;justify-content:center;
           cursor:pointer;z-index:10;gap:14px}
  #ov-name{margin:0;font-size:22px;color:var(--text-primary)}
  #ov-desc{color:var(--text-secondary);margin:0;font-size:13px;max-width:360px;text-align:center;line-height:1.4}
  #ov-start{width:64px;height:64px;border-radius:50%;background:var(--bg-elevated);
            border:1px solid var(--control-border);display:flex;align-items:center;justify-content:center;
            color:var(--accent-primary);cursor:pointer;margin:4px 0;
            transition:transform .12s ease-out,filter .12s ease-out,box-shadow .12s ease-out}
  #overlay:hover #ov-start,#ov-start:hover{transform:scale(1.06);filter:brightness(1.12);
            box-shadow:0 0 0 7px color-mix(in srgb,var(--accent-primary) 13%,transparent)}
  #ov-hint{color:var(--text-secondary);letter-spacing:.08em;font-size:12px}

  .pp-section{margin-top:18px}
  .pp-row{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
  .pp-lab{font-size:11px;color:var(--text-secondary)}
  .pp-btn{background:none;color:var(--text-primary);border:1px solid var(--control-border);
          border-radius:5px;padding:6px 12px;cursor:pointer;font:inherit;font-family:var(--font-family-native)}
  .pp-btn:hover{border-color:var(--accent-primary)}
  .pp-note{display:flex;flex-direction:column;align-items:center;gap:6px;flex:1;
           background:var(--bg-surface);border:1px solid var(--control-border);border-radius:8px;padding:14px}
  .pp-note .big{font-size:30px;font-weight:700;color:var(--accent-primary);font-variant-numeric:tabular-nums}
  .pp-note .cap{font-size:11px;color:var(--text-secondary);letter-spacing:.08em;text-transform:uppercase}
  .pp-lanes{display:flex;flex-direction:column;gap:3px;margin-top:6px}
  .pp-lane{display:flex;align-items:center;gap:8px;font-size:11px;color:var(--text-secondary)}
  .pp-lane .bar{flex:1;height:16px;background:var(--bg-surface);border:1px solid var(--control-border);
                border-radius:4px;position:relative;overflow:hidden}
  .pp-lane .bar .fill{position:absolute;inset:0;background:var(--accent-primary);opacity:0;
                      transition:opacity .12s;display:flex;align-items:center;padding-left:8px;
                      color:var(--accent-text);font-weight:600}
  .pp-lane.on .fill{opacity:.9}
  .pp-lane .ch{flex:0 0 60px;font-variant-numeric:tabular-nums}
  #log{margin-top:6px;height:220px;overflow-y:auto;background:var(--bg-surface);border:1px solid var(--control-border);
       border-radius:6px;padding:8px;font:11px/1.5 ui-monospace,Menlo,monospace}
  #log .ev{display:flex;gap:10px;white-space:nowrap}
  #log .ev .nm{color:var(--accent-primary);flex:0 0 84px}
  #log .ev .ch{color:var(--text-secondary);flex:0 0 44px}
  #log .ev .by{color:var(--text-primary)}
  .pp-hex{width:100%;font:12px/1.5 ui-monospace,Menlo,monospace;min-height:44px}
  .pp-tip{color:var(--text-secondary);font-size:11px;margin-top:6px;line-height:1.5}
  .pp-recv{background:var(--bg-surface);border:1px solid var(--control-border);border-radius:6px;padding:10px;
           font:12px/1.5 ui-monospace,Menlo,monospace;color:var(--accent-primary);min-height:20px;
           word-break:break-all}

  /* Mobile fit: with the viewport meta in place the card is now device-width, so
     reclaim the wide desktop margins/padding and let the fixed native param grid
     (4 x 110px) wrap when it can't fit the screen instead of overflowing. */
  @media (max-width: 680px) {
    .pp-top{margin:14px 12px 0}
    #panel{margin:10px 8px 32px;padding:18px 12px 44px;border-radius:12px}
    #params{grid-template-columns:repeat(auto-fit,110px);column-gap:8px}
  }

  /* Mobile touch hygiene — the plugin UI is a control surface, not a document
     (this is why the cmajor demos disable selection too). On iOS a long-press on
     a knob label or a key was popping the text-selection "Copy · Find Selection"
     callout, and a quick double-tap on a control was zooming the page. Suppress
     BOTH on the UI chrome, while keeping: pinch-zoom (accessibility) + scroll
     (touch-action:manipulation disables only the non-standard double-tap zoom),
     and selection where a value is genuinely worth copying (SysEx echo, MIDI
     log) or editing (the memo / hex fields). */
  body{-webkit-tap-highlight-color:transparent;touch-action:manipulation}
  #panel,.pp-top{-webkit-user-select:none;user-select:none;-webkit-touch-callout:none}
  #panel textarea,#panel input[type=text],#panel .pp-recv,#panel #log{
    -webkit-user-select:text;user-select:text;-webkit-touch-callout:default}`;
  const el = document.createElement("style");
  el.textContent = css;
  document.head.appendChild(el);
}

// ————————————————————————————————————————————————— procedural musical loop
// An 8-bar Am–F–C–G progression at 96 BPM: a rotating eighth-note arpeggio, a
// developing A-minor-pentatonic top-line melody, a warm sustained pad, a
// rounded kick and a soft band-limited shaker. Every event's tail is written
// modulo the buffer length (writes wrap with % len), so the loop is
// mathematically seamless — no discontinuity, no click at the boundary — and
// the 8 bars evolve enough not to feel repetitive. No external asset, no
// permission prompt — this is the default source for effects.
function synthLoop(ctx) {
  const bpm = 96, beat = 60 / bpm, eighth = beat / 2;
  const bars = 8, seconds = beat * 4 * bars;
  const sr = ctx.sampleRate;
  const len = Math.floor(seconds * sr);
  const buf = ctx.createBuffer(2, len, sr);
  const L = buf.getChannelData(0), R = buf.getChannelData(1);
  const mtof = (m) => 440 * Math.pow(2, (m - 69) / 12);
  const TAU = 2 * Math.PI;
  // Pitched voice. A short attack ramp and a release fade make every tone start
  // AND end at zero amplitude, and each sample is written modulo len so any tail
  // crossing the buffer end continues at the start. Together those give a truly
  // seamless loop (the periodic extension is continuous across the boundary).
  const voice = (start, dur, midi, amp, opts = {}) => {
    const { decay = 6, atk = 0.006, rel = 0.03, vib = 0, pan = 0,
            partials = [[1, 1], [2, 0.3]] } = opts;
    const f = mtof(midi);
    const n = Math.floor(dur * sr), s0 = Math.floor(start * sr);
    const th = (pan + 1) * Math.PI / 4;            // equal-power pan, pan in [-1,1]
    const gl = amp * Math.cos(th), gr = amp * Math.sin(th);
    for (let i = 0; i < n; i++) {
      const t = i / sr;
      const a = Math.min(1, t / atk);
      const r = Math.min(1, (dur - t) / rel);
      const env = a * r * Math.exp(-t * decay);
      const vf = vib ? (1 + 0.004 * Math.sin(TAU * 5 * t)) : 1;
      const ph = TAU * f * t * vf;
      let s = 0;
      for (let p = 0; p < partials.length; p++) s += partials[p][1] * Math.sin(ph * partials[p][0]);
      const j = (s0 + i) % len;
      L[j] += s * env * gl; R[j] += s * env * gr;
    }
  };
  // Rounded kick: pitch drop, soft attack ramp, decays to ~0 inside its window
  // (no truncation step at the tail). Supports the groove, never dominates.
  const kick = (start, amp) => {
    const dur = 0.30, n = Math.floor(dur * sr), s0 = Math.floor(start * sr);
    for (let i = 0; i < n; i++) {
      const t = i / sr;
      const f = 95 * Math.exp(-t * 24) + 44;
      const a = Math.min(1, t / 0.004);
      const r = Math.min(1, (dur - t) / 0.04);
      const env = a * r * Math.exp(-t * 12);
      const v = Math.sin(TAU * f * t) * env * amp;
      const j = (s0 + i) % len;
      L[j] += v; R[j] += v;
    }
  };
  // Soft shaker: white noise through a one-pole low-pass then high-pass (a
  // band-limited "tss"), Hann-windowed so it starts and ends exactly at zero.
  // Replaces the old raw-Math.random() hat that fired every eighth and sounded
  // static-y — gentle, and it never clicks or breaks the seam.
  const shaker = (start, dur, amp) => {
    const n = Math.floor(dur * sr), s0 = Math.floor(start * sr);
    let lp = 0, xPrev = 0, yPrev = 0;
    for (let i = 0; i < n; i++) {
      const x = Math.random() * 2 - 1;
      lp += (x - lp) * 0.45;                        // tame the harsh top end
      const hp = 0.9 * (yPrev + lp - xPrev);        // drop rumble -> airy band
      xPrev = lp; yPrev = hp;
      const w = 0.5 - 0.5 * Math.cos(TAU * i / (n - 1));   // Hann window
      const v = hp * w * amp;
      const j = (s0 + i) % len;
      L[j] += v; R[j] += v;
    }
  };
  // Am – F – C – G, one chord per bar, cycled twice across the 8-bar loop.
  const chords = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]];
  const bassMidi = [45, 41, 48, 43];                 // A2 F2 C3 G2
  const arpShape = [0, 1, 2, 3, 2, 3, 1, 2];         // contour, rotated per bar
  // A-minor-pentatonic top line: a developing 2-notes-per-bar phrase that never
  // repeats within the loop and resolves back to its first note.
  const penta = [69, 72, 74, 76, 79, 81];            // A4 C5 D5 E5 G5 A5
  const melody = [0, 1, 3, 2, 1, 3, 2, 1, 3, 4, 5, 4, 3, 2, 1, 0];
  for (let bar = 0; bar < bars; bar++) {
    const ci = bar % 4, chord = chords[ci], t0 = bar * 4 * beat;
    const pool = [chord[0], chord[1], chord[2], chord[0] + 12];
    // Bass: root of the bar, sustained.
    voice(t0, beat * 3.6, bassMidi[ci], 0.17, { decay: 1.6, partials: [[1, 1], [2, 0.18]] });
    // Warm pad: two upper chord tones, very soft, spread across the stereo field.
    voice(t0, beat * 3.8, chord[1] + 12, 0.055, { decay: 0.9, atk: 0.05, pan: -0.35, partials: [[1, 1], [2, 0.25], [3, 0.12]] });
    voice(t0, beat * 3.8, chord[2] + 12, 0.055, { decay: 0.9, atk: 0.05, pan: 0.35, partials: [[1, 1], [2, 0.25], [3, 0.12]] });
    // Arp: eighth notes, contour rotated per bar so no two bars are identical.
    for (let e = 0; e < 8; e++) {
      const deg = arpShape[(e + bar) % 8];
      voice(t0 + e * eighth, eighth * 1.7, pool[deg % pool.length] + 12, 0.15,
            { decay: 5.5, pan: (e % 2 ? 0.25 : -0.25) });
    }
    // Melody: two half notes per bar, gentle vibrato, just right of centre.
    for (let m = 0; m < 2; m++) {
      voice(t0 + m * 2 * beat, beat * 2.0, penta[melody[bar * 2 + m]], 0.2,
            { decay: 2.4, atk: 0.012, vib: 1, pan: 0.15, partials: [[1, 1], [2, 0.22], [3, 0.08]] });
    }
    // Light groove: kick on beats 1 & 3, soft shaker on the offbeats.
    kick(t0, 0.34);
    kick(t0 + 2 * beat, 0.34);
    for (let e = 1; e < 8; e += 2) shaker(t0 + e * eighth, 0.06, 0.05);
  }
  // Normalise to 0.7 (headroom: effects add gain — delay feedback, filter
  // resonance, compressor makeup — so the source must leave room).
  let peak = 0;
  for (let i = 0; i < len; i++) peak = Math.max(peak, Math.abs(L[i]), Math.abs(R[i]));
  if (peak > 0) { const g = 0.7 / peak; for (let i = 0; i < len; i++) { L[i] *= g; R[i] *= g; } }
  return buf;
}

// ——————————————————————————————————————————— plugin_state_io "PLST" envelope
// The SDK composes host-facing plugin state with the SAME format the native
// VST3/AU/CLAP builds use (core/format/src/plugin_state_io.cpp):
//   [ "PLST" ][u32 version=1][u32 store_len][u32 plugin_len]  (16-byte header)
//   [ store bytes (starts "PULP") ][ plugin bytes ]
//   [u32 crc32 over everything above]                          (4-byte footer)
// When the plugin owns no extra state, serialize() returns the BARE store blob
// (no envelope), so a container we read may be either shape.
// state-memo plugin blob: [u32 version=1][u32 memo_len][memo bytes]
const ENV_MAGIC = [0x50, 0x4c, 0x53, 0x54]; // "PLST"
const STORE_MAGIC = [0x50, 0x55, 0x4c, 0x50]; // "PULP"
const ENV_VERSION = 1, ENV_HEADER = 16, ENV_FOOTER = 4;

// zlib CRC-32 — byte-identical to crc32_simple in core/state/src/store.cpp.
function crc32(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i];
    for (let j = 0; j < 8; j++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (~crc) >>> 0;
}
const has4 = (b, m, o = 0) => b.length >= o + 4 && b[o] === m[0] && b[o+1] === m[1] && b[o+2] === m[2] && b[o+3] === m[3];

function parseContainer(bytes) {
  // Bare StateStore blob (plugin owned no extra state): all params, no plugin.
  if (has4(bytes, STORE_MAGIC)) return { params: bytes.slice(), plugin: new Uint8Array(0) };
  if (!has4(bytes, ENV_MAGIC)) throw new Error("not a PLST envelope");
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const storeLen = dv.getUint32(8, true);
  const pluginLen = dv.getUint32(12, true);
  const params = bytes.slice(ENV_HEADER, ENV_HEADER + storeLen);
  const plugin = bytes.slice(ENV_HEADER + storeLen, ENV_HEADER + storeLen + pluginLen);
  return { params, plugin };
}
function buildContainer(params, plugin) {
  // Mirror plugin_state_io::serialize: a bare store blob when there is no plugin
  // payload, else the versioned + CRC'd PLST envelope.
  if (!plugin || plugin.length === 0) return params.slice();
  const out = new Uint8Array(ENV_HEADER + params.length + plugin.length + ENV_FOOTER);
  const dv = new DataView(out.buffer);
  out.set(ENV_MAGIC, 0);
  dv.setUint32(4, ENV_VERSION, true);
  dv.setUint32(8, params.length, true);
  dv.setUint32(12, plugin.length, true);
  out.set(params, ENV_HEADER);
  out.set(plugin, ENV_HEADER + params.length);
  dv.setUint32(out.length - ENV_FOOTER, crc32(out.subarray(0, out.length - ENV_FOOTER)), true);
  return out;
}
function readMemoFromPlugin(plugin) {
  if (plugin.length < 8) return "";
  const dv = new DataView(plugin.buffer, plugin.byteOffset, plugin.byteLength);
  const memoLen = dv.getUint32(4, true);
  return new TextDecoder().decode(plugin.slice(8, 8 + memoLen));
}
function buildMemoPlugin(text) {
  const memo = new TextEncoder().encode(text);
  const out = new Uint8Array(8 + memo.length);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, 1, true);          // version
  dv.setUint32(4, memo.length, true);
  out.set(memo, 8);
  return out;
}

// ————————————————————————————————————————————————— host adapter (interface)
// The shell is host-agnostic: it never talks to a plugin backend directly, only
// to a small "host adapter" object supplied by `opts.createAdapter`. The default
// WAM implementation lives in adapters/wam.js; a future WebCLAP — or a test stub
// — just supplies another factory of the same shape. The FULL interface is
// documented in adapters/adapter.d.ts. In brief, the factory is
//   createAdapter(ctx, { dsp, processor }) → Promise<adapter>
// and the adapter exposes:
//   descriptor, audioNode, getParameterInfo, setParameterValue,
//   getParameterValue, scheduleMidi, sendSysex, getState, setState,
//   onMidiOut (assignable), onParamsChanged (assignable),
//   createSecondary(urls) → Promise<adapter>, destroy().
// Every host call the shell makes goes through `adapter.*` (held in S.wam and the
// voice pool); no backend name appears anywhere in this file.

// ————————————————————————————————————————————————————————————————— main
export async function mountDemo(opts) {
  injectStyles({ tokensHref: opts.tokensHref, fontHref: opts.fontHref, theme: opts.theme });
  const root = opts.root || document.body;
  const title = opts.title || "Pulp Plugin";
  const subtitle = opts.subtitle || "";
  // The gallery is ONE level up from a plugin page (/pulp-example-plugins/<name>/
  // → /pulp-example-plugins/). "../../" would escape to the domain root, which on
  // a custom-domain deploy is a different site.
  const galleryHref = opts.galleryHref || "../index.html";

  // A subtle link to this plugin's source. Prefer an explicit opt; else read a
  // <meta name="pulp:source"> that the page carries (gen-og injects one pointing
  // at the plugin's own repo folder). Absent → no link, so a standalone page
  // that sets no meta simply shows none.
  const sourceHref = opts.sourceHref
    || document.querySelector('meta[name="pulp:source"]')?.getAttribute("content")
    || null;
  const sourceLink = sourceHref
    ? ` &middot; <a href="${sourceHref}" target="_blank" rel="noopener">source</a>`
    : "";

  document.title = `${title} — Pulp web demo`;

  // Host-adapter seam. The shell drives ONLY this factory (and the adapter it
  // returns), never a backend directly. REQUIRED — the package entry (index.js)
  // injects the default WAM adapter; a test or an alternate ABI passes its own
  // `opts.createAdapter(ctx, urls) → adapter`.
  const makeAdapter = opts.createAdapter;
  if (typeof makeAdapter !== "function") {
    throw new Error(
      "mountDemo: opts.createAdapter is required. Import mountDemo from " +
      "@pulp/web-player (which injects the WAM adapter) or pass your own factory.");
  }

  // ABI label in the header ("Pulp WAM demo"). Overridable so a WebCLAP or other
  // adapter can name its own backend + docs link.
  const hostLabel = opts.hostLabel || "WAM";
  const hostDocsHref = opts.hostDocsHref || "https://www.webaudiomodules.com/docs/intro/";

  const coarse = matchMedia("(hover: none) and (pointer: coarse)").matches;
  const startWord = coarse ? "Tap to start" : "Click to start";

  root.innerHTML = `
    <div class="pp-top">
      <a href="${galleryHref}">&larr; Gallery</a>
      <span>Pulp <a href="${hostDocsHref}" target="_blank" rel="noopener">${hostLabel}</a> demo${sourceLink}</span>
    </div>
    <div id="panel" class="pulp">
      <h1>${title}</h1>
      <div class="sub">${subtitle}</div>
      <div id="params"></div>
      <div id="body"></div>
      <div id="footer">
        <button id="stop">Stop Audio</button>
        <div id="status"></div>
      </div>
      <div id="overlay" role="dialog" aria-label="Start ${title}">
        <h2 id="ov-name">${title}</h2>
        ${subtitle ? `<p id="ov-desc">${subtitle}</p>` : ""}
        <div id="ov-start" role="button" tabindex="0" aria-label="Start audio">${PLAY_SVG}</div>
        <div id="ov-hint">${startWord}</div>
      </div>
    </div>`;

  const $ = (s) => root.querySelector(s);
  const status = (m) => { $("#status").textContent = m; };

  // ——— shared demo state
  const S = {
    ctx: null, wam: null, synth: null, synthCtx: null, analyser: null,
    inputGain: opts.inputGain ?? 1, inputTrim: null, limiter: null,
    meterWidget: null, onEvent: null, seedMemo: null, shellMode: null,
    starting: false, handlersInstalled: false, meterToken: 0,
    loopBuffer: null, loopSource: null, micStream: null, micNode: null, micSink: null,
    held: new Map(),                    // note -> Set of source tags
    typingBase: 48, chainSynth: false,
    // Chained-synth voice pool. MonoSynth is monophonic, so each MIDI channel
    // the plugin emits (MPE Spreader gives every held note its own channel)
    // needs its own voice to sound at the same time.
    pool: [], voiceByChan: new Map(), voiceClock: 0,
    descriptor: null, params: null, mode: null,
  };

  // Test seam / inspectable state.
  const demo = window.__demo = {
    started: false, descriptor: null, params: null, peak: 0,
    midiOut: [],       // [{status,d1,d2,bytes:[...]}] most-recent-last
    notesHeld: () => [...S.held.keys()],
  };

  // ——————————————————————————————————————————— parameter controls
  // Auto-generated from the param JSON, rendered as the native Ink & Signal
  // editor: canvas widgets (knob/fader/toggle/combo) on the kCell/kGap/kRowH
  // grid, each with its caption below. A per-demo opts.widgets map (keyed by
  // param id or label) can force a widget kind, e.g. { "gain": "fader" }.
  function buildParams(params) {
    const host = $("#params");
    host.innerHTML = "";
    const registry = (window.__widgets = {});
    for (const raw of params) {
      // Some plugins expose a stepped choice as a plain int with no labels
      // (e.g. synth-presets' Program / Waveform). A per-demo opts.choices map
      // (keyed by id or label) supplies the names so it renders as a named combo.
      const names = opts.choices && (opts.choices[raw.id] ?? opts.choices[raw.label]);
      const p = names ? { ...raw, type: "choice", labels: names } : raw;
      const override = opts.widgets && (opts.widgets[p.id] ?? opts.widgets[p.label]);
      const kind = kindFor(p, override);

      const cell = document.createElement("div");
      cell.className = "pw-cell" + (kind === "combo" ? " span2" : "");

      // Faders carry no built-in numeric readout — add one; knobs/combos/toggles
      // display their own value (centre readout / field text / thumb position).
      const entry = { kind, el: null, param: p, valEl: null, lastUser: 0 };
      const onChange = (v) => {
        entry.lastUser = performance.now();               // suppress the sync poll briefly
        S.wam?.setParameterValue(p.id, v);
        if (entry.valEl) entry.valEl.textContent = formatValue(p, v);
      };

      const widget = createWidget(kind, { param: p, value: p.defaultValue, onChange });
      entry.el = widget;
      widget.dataset.pid = p.id;
      cell.appendChild(widget);

      const cap = document.createElement("div");
      cap.className = "pw-cap";
      cap.textContent = p.label;
      cell.appendChild(cap);

      if (kind === "fader") {
        entry.valEl = document.createElement("div");
        entry.valEl.className = "pw-cellval";
        entry.valEl.textContent = formatValue(p, p.defaultValue);
        cell.appendChild(entry.valEl);
      }

      host.appendChild(cell);
      S.wam?.setParameterValue(p.id, p.defaultValue);   // seed default (parity with before)
      registry[p.id] = entry;
    }
  }

  // Follow parameter changes the plugin makes to ITSELF — synth-with-presets
  // rewrites its ADSR knobs when Program changes, from inside process(). The
  // native editor learns this through bind_parameter; on the web the SDK pushes
  // it: the worklet polls wam_param_epoch() once per block (one wasm call) and
  // posts the values only when it actually moves. This is a push, not a poll —
  // no latency floor, no idle traffic.
  //
  // A control the user is actively editing is skipped, so a late-arriving update
  // can never fight a drag. widget.setValue() repaints only (it does not fire
  // onChange), so this cannot feed back into the plugin.
  function startParamSync() {
    S.wam.onParamsChanged = (values, params) => {
      const reg = window.__widgets || {};
      params.forEach((param, i) => {
        const e = reg[param.id];
        const v = values[i];
        if (!e?.el?.setValue || typeof v !== "number") return;
        if (document.activeElement === e.el) return;
        if (performance.now() - e.lastUser < 250) return;
        if (Math.abs(v - e.el.getValue()) <= 1e-4) return;
        e.el.setValue(v);
        if (e.valEl) e.valEl.textContent = formatValue(e.param, v);
      });
      demo.paramEpochUpdates = (demo.paramEpochUpdates || 0) + 1;   // test seam
    };
  }

  async function waitForParams(wam, timeoutMs = 3000) {
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      const p = await wam.getParameterInfo();
      if (p && p.length) return p;
      if (performance.now() > deadline) { console.warn("parameters never arrived"); return p || []; }
      await new Promise((r) => setTimeout(r, 25));
    }
  }

  // ——————————————————————————————————————————— MIDI plumbing (shared)
  // The sink for note-on/off. For instruments and MIDI effects alike it's the
  // plugin; a MIDI effect additionally re-emits on onMidiOut which drives a
  // visualiser (and, when chained, a MonoSynth).
  function sendMidi(status, d1, d2) { S.wam?.scheduleMidi(status, d1, d2, 0); }

  function noteOn(n, source = "pointer", velocity = 100) {
    if (!S.wam) return;
    let sources = S.held.get(n);
    if (!sources) {
      S.held.set(n, (sources = new Set()));
      sendMidi(0x90, n, velocity);
      keyEl(n)?.classList.add("on");
      S.onPlayed?.(n);                 // e.g. the transpose "Played" badge
    }
    sources.add(source);
  }
  function noteOff(n, source = "pointer") {
    if (!S.wam) return;
    const sources = S.held.get(n);
    if (!sources) return;
    sources.delete(source);
    if (sources.size) return;
    S.held.delete(n);
    sendMidi(0x80, n, 0);
    keyEl(n)?.classList.remove("on");
  }
  function allNotesOff() {
    for (const n of [...S.held.keys()]) {
      S.held.delete(n);
      sendMidi(0x80, n, 0);
      keyEl(n)?.classList.remove("on");
    }
  }

  // ——————————————————————————————————————————— on-screen keyboard
  const BASE_NOTE = 48, OCTAVES = 2;
  const WHITE = [0,2,4,5,7,9,11], BLACK = { 1:0, 3:1, 6:3, 8:4, 10:5 };
  const WHITE_KEYS = { a:0, s:2, d:4, f:5, g:7, h:9, j:11, k:12, l:14, ";":16, "'":17 };
  const BLACK_KEYS = { w:1, e:3, t:6, y:8, u:10, o:13, p:15 };
  const KEY_SEMITONE = { ...WHITE_KEYS, ...BLACK_KEYS };
  const TYPING_BASE_MIN = 0, TYPING_BASE_MAX = 96;
  const keyEl = (n) => root.querySelector(`[data-note="${n}"]`);
  const noteForKey = (key) => { const s = KEY_SEMITONE[key]; return s === undefined ? undefined : S.typingBase + s; };
  // A field where letter keys mean text, not notes: <textarea>, a text-like
  // <input>, or a contenteditable. Deliberately EXCLUDES checkbox/range/button
  // so those don't hijack musical typing when focused.
  const TEXT_INPUT_TYPES = new Set(["text","search","email","url","tel","password","number"]);
  function isTextEntry(el) {
    if (!el) return false;
    if (el.isContentEditable) return true;
    const tag = el.tagName;
    if (tag === "TEXTAREA") return true;
    if (tag === "INPUT") return TEXT_INPUT_TYPES.has((el.type || "text").toLowerCase());
    return false;
  }

  function renderKeyboard() {
    const kb = $("#kb");
    if (!kb) return;
    // Black keys are positioned in absolute px derived from kb.clientWidth. On
    // the FIRST paint (notably Safari, and before web fonts settle) that width
    // can still be 0 or pre-layout, which threw the black keys off-screen until a
    // manual refresh. If the width isn't known yet, defer to the next frame; a
    // ResizeObserver (installed once below) also re-renders on any later resize.
    if (!kb.clientWidth) { requestAnimationFrame(renderKeyboard); return; }
    kb.innerHTML = "";
    const whites = [];
    for (let oct = 0; oct < OCTAVES; oct++) for (const s of WHITE) whites.push(S.typingBase + oct*12 + s);
    whites.push(S.typingBase + OCTAVES*12);
    for (const note of whites) {
      const k = document.createElement("div");
      k.className = "wk"; k.dataset.note = note; kb.appendChild(k);
    }
    const whiteW = kb.clientWidth / whites.length;
    const blackW = Math.round(whiteW * 0.6);
    for (let oct = 0; oct < OCTAVES; oct++) {
      for (const [semi, whiteIdx] of Object.entries(BLACK)) {
        const note = S.typingBase + oct*12 + (+semi);
        const b = document.createElement("div");
        b.className = "bk"; b.dataset.note = note;
        b.style.width = `${blackW}px`;
        b.style.left = `${(oct*7 + (+whiteIdx) + 1) * whiteW - blackW/2}px`;
        kb.appendChild(b);
      }
    }
    labelKeys();
    for (const n of S.held.keys()) keyEl(n)?.classList.add("on");
  }
  function labelKeys() {
    for (const el of root.querySelectorAll("#kb .wk, #kb .bk")) el.textContent = "";
    for (const [key, semi] of Object.entries(KEY_SEMITONE)) {
      const el = keyEl(S.typingBase + semi);
      if (el) el.textContent = /[a-z]/.test(key) ? key.toUpperCase() : key;
    }
    const octave = Math.floor(S.typingBase / 12) - 1;
    const hint = $("#kbhint");
    if (hint) hint.textContent = `Octave C${octave} — Z / X to shift. Home row = white keys, row above = black.`;
  }
  function shiftOctave(delta) {
    const next = S.typingBase + delta * 12;
    if (next < TYPING_BASE_MIN || next > TYPING_BASE_MAX) return;
    allNotesOff(); S.typingBase = next; renderKeyboard();
  }
  function ensureNoteVisible(note) {
    const span = OCTAVES * 12;
    let base = S.typingBase;
    while (note < base && base - 12 >= TYPING_BASE_MIN) base -= 12;
    while (note > base + span && base + 12 <= TYPING_BASE_MAX) base += 12;
    if (base === S.typingBase) return;
    S.typingBase = base; renderKeyboard();
  }
  function installKeyboardHandlers() {
    if (S.handlersInstalled) return;
    S.handlersInstalled = true;
    const kb = $("#kb");
    // Re-lay-out the (absolutely-positioned) black keys whenever the keyboard's
    // width changes — first layout, web-font settle, window resize, or a mobile
    // orientation flip. rAF-debounced so a burst of resize ticks coalesces.
    if (typeof ResizeObserver !== "undefined") {
      let last = kb.clientWidth, pending = false;
      new ResizeObserver(() => {
        if (kb.clientWidth === last || pending) return;
        pending = true;
        requestAnimationFrame(() => { pending = false; last = kb.clientWidth; renderKeyboard(); });
      }).observe(kb);
    }
    kb.addEventListener("pointerdown", (e) => {
      const n = e.target.dataset?.note; if (!n) return;
      noteOn(+n);
      try { e.target.setPointerCapture(e.pointerId); } catch {}
    });
    kb.addEventListener("pointerup", (e) => { const n = e.target.dataset?.note; if (n) noteOff(+n); });
    kb.addEventListener("pointerleave", () => { for (const n of [...S.held.keys()]) noteOff(n, "pointer"); });
    addEventListener("keydown", (e) => {
      if (e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
      // Only suppress musical typing when a TEXT-ENTRY field has focus (the
      // state-memo textarea). A checkbox / range / toggle (e.g. the "Chain into
      // MonoSynth" checkbox, or pitch-bend/mod-wheel sliders) must NOT swallow
      // the keyboard — clicking one used to leave `e.target` an <input> and kill
      // typing until you clicked elsewhere.
      if (isTextEntry(e.target)) return;
      const key = e.key.toLowerCase();
      if (key === "z" || key === "x") { shiftOctave(key === "z" ? -1 : 1); return; }
      const n = noteForKey(key);
      if (n !== undefined) { e.preventDefault(); noteOn(n, "key:" + key); }
    });
    addEventListener("keyup", (e) => {
      const key = e.key.toLowerCase();
      const n = noteForKey(key);
      if (n !== undefined) noteOff(n, "key:" + key);
    });
  }
  addEventListener("blur", allNotesOff);
  addEventListener("contextmenu", allNotesOff);
  document.addEventListener("visibilitychange", () => { if (document.hidden) allNotesOff(); });

  function handleMidiMessage(data) {
    if (!S.wam || !data?.length) return;
    const st = data[0], type = st & 0xf0;
    if (type === 0x90 && data[2] > 0) { ensureNoteVisible(data[1]); noteOn(data[1], "midi", data[2]); }
    else if (type === 0x80 || (type === 0x90 && data[2] === 0)) noteOff(data[1], "midi");
    else S.wam.scheduleMidi(st, data[1] ?? 0, data[2] ?? 0, 0);
  }
  window.__midiMessage = handleMidiMessage;

  async function connectWebMidi(khz) {
    if (!navigator.requestMIDIAccess) { console.info("WebMIDI unavailable (Safari never shipped it)"); return; }
    let access;
    try { access = await navigator.requestMIDIAccess({ sysex: false }); }
    catch (err) { console.warn("WebMIDI permission denied or unavailable:", err); return; }
    const attach = (input) => { input.onmidimessage = ({ data }) => handleMidiMessage(data); };
    const named = () => [...access.inputs.values()].map((i) => i.name);
    for (const input of access.inputs.values()) attach(input);
    const report = () => {
      const names = named();
      status(names.length
        ? `${S.wam.descriptor.name} — ${khz} kHz · MIDI in: ${names.join(", ")}`
        : `${S.wam.descriptor.name} ready — ${khz} kHz · no MIDI device`);
    };
    report();
    access.onstatechange = (e) => {
      if (e.port.type === "input" && e.port.state === "connected") attach(e.port);
      report();
    };
  }

  // ——————————————————————————————————————————— analyser / oscilloscope + meter
  // Builds the scope + meter DOM. Callable before audio exists (the meter loop
  // reads S.analyser live), so the panel reaches its final height at load time.
  function ensureScope() {
    if (root.querySelector("#scope")) return;
    const wrap = document.createElement("div");
    wrap.className = "pp-meterwrap";
    const canvas = document.createElement("canvas");
    canvas.id = "scope"; canvas.width = 600; canvas.height = 56;
    wrap.appendChild(canvas);
    // A native-styled level meter beside the scope (WIDGETS.md "Meter").
    S.meterWidget = createWidget("meter", { width: 12, height: 56 });
    wrap.appendChild(S.meterWidget);
    $("#params").after(wrap);
  }
  const SCOPE_DRAW = 1024;                         // samples actually plotted

  function meter() {
    const token = ++S.meterToken;
    // Scope stroke colour from the design tokens, never a hardcoded brand hex:
    // prefer --waveform-line, fall back to --accent-primary, then currentColor
    // (which resolves against --text-primary), so an unskinned page still draws.
    const rootStyle = getComputedStyle(document.documentElement);
    const accent = rootStyle.getPropertyValue("--waveform-line").trim()
      || rootStyle.getPropertyValue("--accent-primary").trim()
      || "currentColor";
    let buf = null;                               // (re)allocated when an analyser appears
    const tick = () => {
      if (token !== S.meterToken) return;         // superseded by stop/restart
      const analyser = S.analyser;                // read live — chaining adds it late
      if (analyser) {
        if (!buf || buf.length !== analyser.fftSize) buf = new Float32Array(analyser.fftSize);
        analyser.getFloatTimeDomainData(buf);
        let p = 0;
        for (let i = 0; i < buf.length; i++) p = Math.max(p, Math.abs(buf[i]));
        demo.peak = Math.max(demo.peak, p);
        S.meterWidget?.push(p);
        const canvas = root.querySelector("#scope");
        const cctx = canvas?.getContext("2d");
        if (cctx) {
          // Triggered window from the SDK (wam-scope.mjs): a SCOPE_DRAW-sample
          // subarray starting at the first rising zero-crossing, so a periodic
          // waveform stands still frame-to-frame. Non-periodic input (loop/mic)
          // still animates — that's correct.
          const view = triggeredView(buf, SCOPE_DRAW);
          cctx.clearRect(0, 0, canvas.width, canvas.height);
          cctx.strokeStyle = accent; cctx.lineWidth = 1.5; cctx.beginPath();
          const step = view.length / canvas.width;
          for (let x = 0; x < canvas.width; x++) {
            const y = canvas.height / 2 - view[Math.floor(x * step)] * canvas.height * 0.45;
            x ? cctx.lineTo(x, y) : cctx.moveTo(x, y);
          }
          cctx.stroke();
        }
      }
      requestAnimationFrame(tick);
    };
    tick();
  }

  // ——————————————————————————————————————————— audio source selector
  function buildAudioSource() {
    const sec = document.createElement("div");
    sec.className = "pp-section";
    sec.innerHTML = `
      <div class="pp-row">
        <span class="pp-lab">Audio source</span>
        <select id="src">
          <option value="loop">Loop (synth arp)</option>
          <option value="mic">Microphone</option>
          <option value="off">Off</option>
        </select>
      </div>
      <div class="pp-tip">An 8-bar Am–F–C–G loop is synthesized in-page at 96 BPM — a rotating arpeggio, an evolving melody and a light groove; no external asset, no mic permission. Switch to Microphone to run your own input through the effect.</div>`;
    $("#body").appendChild(sec);
    const sel = $("#src");
    sel.addEventListener("change", () => setSource(sel.value));
  }
  // Safari's Web Audio AudioSession (navigator.audioSession) gates mic capture:
  // the default "playback" category REJECTS getUserMedia with InvalidStateError
  // ("AudioSession category is not compatible with audio capture"). Capture needs
  // "play-and-record"; playback-only should return to "playback" so iOS routes to
  // the main speaker (play-and-record can duck / route to the earpiece). No-op on
  // Chrome/Firefox, which don't implement the API.
  function setAudioSession(type) {
    try { if (navigator.audioSession) navigator.audioSession.type = type; } catch {}
  }
  function stopSource() {
    if (S.loopSource) { try { S.loopSource.stop(); } catch {} S.loopSource.disconnect(); S.loopSource = null; }
    if (S.micNode) { S.micNode.disconnect(); S.micNode = null; }
    if (S.micSink) { try { S.micSink.pause(); } catch {} S.micSink.srcObject = null; S.micSink = null; }
    if (S.micStream) {
      S.micStream.getTracks().forEach((t) => t.stop()); S.micStream = null;
      setAudioSession("playback");   // leaving mic → restore full-speaker playback
    }
  }
  async function setSource(kind) {
    stopSource();
    if (!S.ctx || !S.wam) return;
    if (kind === "loop") {
      if (!S.loopBuffer) S.loopBuffer = synthLoop(S.ctx);
      const src = S.ctx.createBufferSource();
      src.buffer = S.loopBuffer; src.loop = true;
      sourceTrim(S, src); src.start();
      S.loopSource = src;
      status(`${S.wam.descriptor.name} — loop running`);
    } else if (kind === "mic") {
      try {
        // Safari: the audio session must allow capture BEFORE getUserMedia, or it
        // rejects with InvalidStateError. Must run in the same user gesture.
        setAudioSession("play-and-record");
        const stream = await navigator.mediaDevices.getUserMedia({
          audio: { echoCancellation: false, autoGainControl: false, noiseSuppression: false } });
        S.micStream = stream;
        // getUserMedia can leave the AudioContext suspended (Safari).
        if (S.ctx.state !== "running") { try { await S.ctx.resume(); } catch {} }
        // Safari quirk 2: a MediaStreamAudioSourceNode outputs SILENCE unless the
        // stream is also consumed by a playing media element. Attach it to a
        // muted, inline <audio> sink to keep the stream "hot" (harmless in
        // Chrome/Firefox, which don't need it). Muted so it never doubles back.
        const sink = new Audio();
        sink.muted = true; sink.playsInline = true; sink.srcObject = stream;
        try { await sink.play(); } catch {}
        S.micSink = sink;
        S.micNode = S.ctx.createMediaStreamSource(stream);
        sourceTrim(S, S.micNode);
        status(`${S.wam.descriptor.name} — microphone live`);
      } catch (err) {
        console.warn("mic denied:", err);
        status("Microphone unavailable — falling back to Loop.");
        $("#src").value = "loop"; setSource("loop");
      }
    } else {
      status(`${S.wam.descriptor.name} — source off`);
    }
  }
  window.__setSource = setSource;

  // ——————————————————————————————————————————— MIDI-effect visualisers
  function classifyMidi(bytes) {
    const st = bytes[0], type = st & 0xf0, ch = (st & 0x0f);
    let name = "Other";
    if (type === 0x90 && bytes[2] > 0) name = "Note On";
    else if (type === 0x80 || (type === 0x90 && bytes[2] === 0)) name = "Note Off";
    else if (type === 0xB0) name = "CC";
    else if (type === 0xC0) name = "Program";
    else if (type === 0xD0) name = "Ch Press";
    else if (type === 0xA0) name = "Poly AT";
    else if (type === 0xE0) name = "Pitch Bend";
    else if (st === 0xF0) name = "SysEx";
    return { name, type, ch };
  }
  const hex = (arr) => [...arr].map((b) => b.toString(16).toUpperCase().padStart(2, "0")).join(" ");

  // Each visualiser returns an onEvent(bytes) handler; the plugin's onMidiOut is
  // routed to it (and, if chained, forwarded to a MonoSynth).
  function buildMidiViz(kind) {
    const body = $("#body");

    // Keyboard input drives the plugin (except sysex which has its own control).
    if (kind !== "sysex") {
      const kbSec = document.createElement("div");
      kbSec.className = "pp-section";
      kbSec.innerHTML = `<div id="kb"></div><div id="kbhint"></div>`;
      body.appendChild(kbSec);
    }

    // "Chain into MonoSynth" — audible, honestly captioned.
    let chainSec;
    if (kind !== "sysex" && opts.synthUrls) {
      chainSec = document.createElement("div");
      chainSec.className = "pp-section";
      chainSec.innerHTML = `
        <label class="pp-row" style="cursor:pointer">
          <input type="checkbox" id="chain">
          <span class="pp-lab">Chain the MIDI output into a MonoSynth (make it audible)</span>
        </label>
        <div class="pp-tip">Heads-up: this forwards the plugin's MIDI output on the <b>main thread</b> into a MonoSynth, which costs ~one audio block (~2.9 ms @ 44.1 kHz) of extra latency and re-relativizes each event's sample offset to the next block. A sample-accurate in-worklet plugin rack is planned; this toggle is a convenience for hearing the effect.</div>`;
      body.appendChild(chainSec);
      $("#chain").addEventListener("change", (e) => setChain(e.target.checked));
    }

    let onEvent = () => {};
    if (kind === "transpose") {
      const sec = document.createElement("div");
      sec.className = "pp-section pp-row";
      sec.innerHTML = `
        <div class="pp-note"><div class="cap">Played</div><div class="big" id="vin">—</div></div>
        <div class="pp-note"><div class="cap">Transposed out</div><div class="big" id="vout">—</div></div>`;
      body.appendChild(sec);
      S.onPlayed = (n) => { $("#vin").textContent = noteName(n); };
      onEvent = (bytes) => {
        const c = classifyMidi(bytes);
        if (c.name === "Note On") $("#vout").textContent = noteName(bytes[1]);
      };
    } else if (kind === "mpe") {
      const sec = document.createElement("div");
      sec.className = "pp-section";
      const lanes = ["<div class='pp-lab'>Each held note is re-emitted on its own MIDI channel:</div><div class='pp-lanes'>"];
      for (let ch = 1; ch <= 15; ch++)
        lanes.push(`<div class="pp-lane" data-ch="${ch}"><span class="ch">Ch ${ch + 1}</span><span class="bar"><span class="fill"></span></span></div>`);
      lanes.push("</div>");
      sec.innerHTML = lanes.join("");
      body.appendChild(sec);
      onEvent = (bytes) => {
        const c = classifyMidi(bytes);
        const lane = root.querySelector(`.pp-lane[data-ch="${c.ch}"]`);
        if (!lane) return;
        if (c.name === "Note On") { lane.classList.add("on"); lane.querySelector(".fill").textContent = noteName(bytes[1]); }
        else if (c.name === "Note Off") { lane.classList.remove("on"); lane.querySelector(".fill").textContent = ""; }
      };
    } else if (kind === "inspector") {
      const sec = document.createElement("div");
      sec.className = "pp-section";
      sec.innerHTML = `<div class="pp-lab">Live MIDI output log</div><div id="log"></div>`;
      body.appendChild(sec);
      const log = () => $("#log");
      onEvent = (bytes) => {
        const c = classifyMidi(bytes);
        const row = document.createElement("div");
        row.className = "ev";
        row.innerHTML = `<span class="nm">${c.name}</span><span class="ch">ch ${c.ch + 1}</span><span class="by">${hex(bytes)}</span>`;
        const l = log(); l.appendChild(row);
        while (l.childElementCount > 200) l.removeChild(l.firstChild);
        l.scrollTop = l.scrollHeight;
      };
    } else if (kind === "sysex") {
      const sec = document.createElement("div");
      sec.className = "pp-section";
      sec.innerHTML = `
        <div class="pp-lab">SysEx payload (hex, F0 … F7)</div>
        <textarea id="hexin" class="pp-hex">F0 7D 01 02 03 F7</textarea>
        <div class="pp-row" style="margin-top:8px">
          <button class="pp-btn" id="sendhex">Send SysEx</button>
          <span class="pp-lab" id="senterr"></span>
        </div>
        <div class="pp-lab" style="margin-top:12px">Echoed back from the plugin</div>
        <div class="pp-recv" id="recv">—</div>`;
      body.appendChild(sec);
      $("#sendhex").addEventListener("click", () => sendSysexFromField());
      onEvent = (bytes) => { $("#recv").textContent = hex(bytes); };
    }
    return onEvent;
  }

  function sendSysexFromField() {
    const txt = $("#hexin").value.trim();
    const parts = txt.split(/[\s,]+/).filter(Boolean);
    const bytes = new Uint8Array(parts.length);
    for (let i = 0; i < parts.length; i++) {
      const v = parseInt(parts[i], 16);
      if (Number.isNaN(v) || v < 0 || v > 255) { $("#senterr").textContent = `bad byte "${parts[i]}"`; return; }
      bytes[i] = v;
    }
    $("#senterr").textContent = "";
    S.wam.sendSysex(bytes);
  }
  window.__sendSysex = (arr) => S.wam?.sendSysex(arr instanceof Uint8Array ? arr : new Uint8Array(arr));

  // How many chained-synth voices to pre-warm. A monophonic chain (transpose)
  // only ever emits on one channel, so one voice suffices; MPE Spreader hands
  // every held note its own channel, so it needs a small polyphonic pool.
  const POLY_VOICES = opts.midiViz === "mpe" ? 6 : 1;

  // Attach a pool of MonoSynth voices and forward MIDI into them, routed by the
  // channel each event carries. The processor name is derived per module URL
  // (processorNameForUrl in wam-runtime.mjs), so every voice can share ONE
  // AudioContext (S.ctx) with the MIDI effect — no separate graph per voice.
  async function setChain(on) {
    S.chainSynth = on;
    if (on) {
      if (!S.analyser) {
        S.analyser = S.ctx.createAnalyser(); S.analyser.fftSize = 4096;   // capture window > SCOPE_DRAW so the trigger has slack
        connectOutput(S);
      }
      if (!S.pool.length) {
        // Pre-warm the whole pool up front (a deliberate toggle can absorb the
        // one-time spin-up) so a chord plays instantly rather than dropping the
        // first note on each channel while its voice loads.
        S.pool = await Promise.all(Array.from({ length: POLY_VOICES }, async () => {
          const synth = await S.wam.createSecondary(opts.synthUrls);
          synth.audioNode.connect(S.analyser);
          return { synth, ch: null, t: 0 };
        }));
        S.synth = S.pool[0]?.synth || null;   // back-compat handle (tests / teardown)
      } else {
        for (const v of S.pool) v.synth.audioNode.connect(S.analyser);
      }
      ensureScope();
    } else {
      for (const v of S.pool) { try { v.synth.audioNode.disconnect(); } catch {} }
    }
  }

  // Map a MIDI channel to a pool voice, keeping a stable assignment so repeated
  // notes on the same channel reuse one voice; steal the least-recently-used
  // voice when a chord needs more channels than the pool has.
  function voiceForChannel(ch) {
    const existing = S.voiceByChan.get(ch);
    if (existing !== undefined) { S.pool[existing].t = ++S.voiceClock; return S.pool[existing].synth; }
    let pick = 0, oldest = Infinity;
    for (let i = 0; i < S.pool.length; i++) {
      if (S.pool[i].ch === null) { pick = i; oldest = -1; break; }
      if (S.pool[i].t < oldest) { oldest = S.pool[i].t; pick = i; }
    }
    const v = S.pool[pick];
    if (v.ch !== null) S.voiceByChan.delete(v.ch);
    v.ch = ch; v.t = ++S.voiceClock; S.voiceByChan.set(ch, pick);
    return v.synth;
  }

  // ——————————————————————————————————————————— state-memo UI
  function buildStateMemo() {
    const sec = document.createElement("div");
    sec.className = "pp-section";
    sec.innerHTML = `
      <div class="pp-lab">Free-text memo (lives in the plugin's saved state)</div>
      <textarea id="memo" class="pp-hex" style="min-height:70px" placeholder="Type a note to store in plugin state…"></textarea>
      <div class="pp-row" style="margin-top:8px">
        <button class="pp-btn" id="memosave">Save state</button>
        <button class="pp-btn" id="memoload">Load state</button>
        <span class="pp-lab" id="memostat"></span>
      </div>
      <div class="pp-tip">Save snapshots <code>wam.getState()</code> (the versioned, CRC-checked <code>PLST</code> envelope — the same format the native VST3/AU/CLAP builds use). Edit the text, then Load to restore it via <code>wam.setState()</code> — the memo survives the round-trip.</div>`;
    $("#body").appendChild(sec);
    let saved = null;
    const memo = () => $("#memo");
    const setStat = (m) => { $("#memostat").textContent = m; };

    async function pushMemo(text) {
      const st = await S.wam.getState();
      let params = new Uint8Array(0);
      try { params = parseContainer(st).params; } catch {}
      S.wam.setState(buildContainer(params, buildMemoPlugin(text)));
    }
    async function pullMemo() {
      const st = await S.wam.getState();
      try { return readMemoFromPlugin(parseContainer(st).plugin); } catch { return ""; }
    }

    let t = null;
    memo().addEventListener("input", () => { clearTimeout(t); t = setTimeout(() => pushMemo(memo().value), 120); });
    $("#memosave").addEventListener("click", async () => {
      await pushMemo(memo().value);
      saved = await S.wam.getState();
      setStat(`saved (${saved.length} bytes)`);
    });
    $("#memoload").addEventListener("click", async () => {
      if (!saved) { setStat("nothing saved yet"); return; }
      S.wam.setState(saved);
      await new Promise((r) => setTimeout(r, 30));
      memo().value = await pullMemo();
      setStat("loaded — memo restored");
    });

    window.__memo = {
      get: pullMemo,
      set: async (txt) => { memo().value = txt; await pushMemo(txt); },
      save: async () => { await pushMemo(memo().value); saved = await S.wam.getState(); return saved.length; },
      load: async () => { if (!saved) return null; S.wam.setState(saved); await new Promise((r) => setTimeout(r, 30)); memo().value = await pullMemo(); return memo().value; },
    };
    // Seed the field from whatever the plugin currently holds. The section DOM is
    // built at load (before S.wam exists), so defer the seed to audio activation.
    const seed = () => { if (S.wam) pullMemo().then((txt) => { memo().value = txt; }); };
    S.seedMemo = seed;
    seed();
  }

  // ——————————————————————————————————————————— instrument extras
  function buildControllers() {
    const sec = document.createElement("div");
    sec.className = "pp-section pp-row";
    sec.innerHTML = `
      <div class="p" style="flex:1"><label>Pitch Bend</label>
        <input type="range" id="pb" min="0" max="16383" value="8192"><span class="val">center</span></div>
      <div class="p" style="flex:1"><label>Mod Wheel (CC1)</label>
        <input type="range" id="mw" min="0" max="127" value="0"><span class="val" id="mwv">0</span></div>`;
    $("#body").appendChild(sec);
    const pb = $("#pb"), mw = $("#mw");
    const sendBend = (v) => { sendMidi(0xE0, v & 0x7f, (v >> 7) & 0x7f); };
    pb.addEventListener("input", () => sendBend(+pb.value));
    const recenter = () => { pb.value = 8192; sendBend(8192); };
    pb.addEventListener("pointerup", recenter);
    pb.addEventListener("keyup", recenter);
    mw.addEventListener("input", () => { sendMidi(0xB0, 1, +mw.value); $("#mwv").textContent = mw.value; });
  }

  // ——————————————————————————————————————————— layout shell (built at load)
  // The plugin's parameter metadata only arrives from the worklet AFTER start,
  // so real param widgets can't exist up front. But every OTHER section (scope,
  // meter, keyboard, audio source, memo, MIDI viz) has a fixed size we can build
  // immediately, and the param grid's height is reserved via #params min-height
  // from the per-demo row count. Result: the panel is at its FINAL height on the
  // first paint, so the start overlay isn't squished and nothing jumps on start.
  function presumedMode() {
    if (opts.mode) return opts.mode;
    if (opts.midiViz) return "midi-effect";
    if (opts.stateMemo) return "audio-effect";
    if (opts.controllers) return "instrument";
    return "instrument"; // gain/mono-synth set opts.mode explicitly; this is a safe default
  }
  function reserveParamGrid(rows) {
    if (!rows || rows < 1) return;
    const h = rows * 116 + (rows - 1) * 16;   // kRowH 116 / kGap 16
    $("#params").style.minHeight = h + "px";
  }
  function buildBodyShell(mode) {
    if (mode === "instrument") {
      ensureScope();
      const kbSec = document.createElement("div");
      kbSec.className = "pp-section";
      kbSec.innerHTML = `<div id="kb"></div><div id="kbhint"></div>`;
      $("#body").appendChild(kbSec);
      if (opts.controllers) buildControllers();
      installKeyboardHandlers();
      renderKeyboard();
    } else if (mode === "audio-effect") {
      ensureScope();
      buildAudioSource();
      if (opts.stateMemo) buildStateMemo();
    } else if (mode === "midi-effect") {
      S.onEvent = buildMidiViz(opts.midiViz);
      if (opts.midiViz !== "sysex") { installKeyboardHandlers(); renderKeyboard(); }
    }
  }

// Output safety. A demo page must never be able to hurt someone wearing
// headphones. Nothing here is a claim about the DSP — the analyser taps the
// plugin's RAW output, so the scope and meter still show exactly what the
// plugin produced, clipping included. The limiter sits AFTER the tap, purely
// between the page and the speakers.
//
// This is not hypothetical: `wah` ships a +20 dB default Gain on a resonant
// filter (Gain default 20, range 0..20 dB; Q default 10) and peaks around 8.7
// through a backing loop — roughly +19 dBFS.
//
// A DynamicsCompressorNode with a high ratio, a low knee and a 0 dB-ish
// threshold is Web Audio's only built-in limiter. It is not transparent, but
// it only engages above -1 dBFS, which nothing well-behaved reaches.
function createSafetyLimiter(ctx) {
  const limiter = ctx.createDynamicsCompressor();
  limiter.threshold.value = -1;    // dBFS — engage only when we're about to clip
  limiter.knee.value = 0;          // hard
  limiter.ratio.value = 20;        // effectively a brickwall
  limiter.attack.value = 0.001;
  limiter.release.value = 0.05;
  return limiter;
}

// Connect the analyser tap to the speakers through the limiter. Idempotent:
// the chain demos call this again when a synth is added late.
function connectOutput(S) {
  if (!S.limiter) {
    S.limiter = createSafetyLimiter(S.ctx);
    S.limiter.connect(S.ctx.destination);
  }
  try { S.analyser.disconnect(); } catch {}
  S.analyser.connect(S.limiter);
}


  // Source trim, in front of the plugin. Some effects add a lot of gain by
  // design — wah's "Gain" is not makeup gain at all, it is the resonance factor
  // of the resonant-lowpass (it appears only as tan(wc/2)/g in the denominator),
  // so turning it down does not tame the effect, it REMOVES the wah. The right
  // knob to turn is the source level, exactly as you would gain-stage in a DAW.
  // Set per demo with `inputGain` (linear).
  function sourceTrim(S, node) {
    if (!S.inputTrim) {
      S.inputTrim = S.ctx.createGain();
      S.inputTrim.gain.value = S.inputGain ?? 1;
      S.inputTrim.connect(S.wam.audioNode);
    }
    node.connect(S.inputTrim);
  }

  // Wire the audio graph to the already-built shell (called from start()).
  async function activateAudio(mode) {
    if (mode === "instrument") {
      S.analyser = S.ctx.createAnalyser(); S.analyser.fftSize = 4096;   // capture window > SCOPE_DRAW so the trigger has slack
      S.wam.audioNode.connect(S.analyser); connectOutput(S);
    } else if (mode === "audio-effect") {
      S.analyser = S.ctx.createAnalyser(); S.analyser.fftSize = 4096;   // capture window > SCOPE_DRAW so the trigger has slack
      S.wam.audioNode.connect(S.analyser); connectOutput(S);
      S.seedMemo?.();
      $("#src").value = "loop"; await setSource("loop");
    } else if (mode === "midi-effect") {
      S.wam.onMidiOut = (events) => {
        for (const ev of events) {
          const bytes = ev.bytes;
          demo.midiOut.push({ status: bytes[0], d1: bytes[1], d2: bytes[2], bytes: [...bytes] });
          if (demo.midiOut.length > 512) demo.midiOut.shift();
          S.onEvent?.(bytes);
          // Route each event to the voice for its channel so a spread chord
          // sounds every note at once (a single mono voice would drop all but
          // the last). Non-note messages (CC/bend) follow their channel's voice.
          if (S.chainSynth && S.pool.length) {
            voiceForChannel(bytes[0] & 0x0f).scheduleMidi(bytes[0], bytes[1] ?? 0, bytes[2] ?? 0, 0);
          }
        }
      };
    }
  }

  // ——————————————————————————————————————————— lifecycle
  async function start() {
    if (S.starting || S.ctx) return;
    S.starting = true;
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!window.isSecureContext || !AC || !("audioWorklet" in AC.prototype)) {
      S.starting = false;
      status("AudioWorklet needs a secure context — use https:// or localhost.");
      return;
    }
    $("#overlay").style.display = "none";

    S.ctx = new AC();
    S.ctx.resume();
    const unlock = S.ctx.createBufferSource();
    unlock.buffer = S.ctx.createBuffer(1, 1, S.ctx.sampleRate);
    unlock.connect(S.ctx.destination); unlock.start(0);
    try { navigator.audioSession.type = "playback"; } catch {}

    S.wam = await makeAdapter(S.ctx, { dsp: opts.dspUrl, processor: opts.processorUrl });
    if (S.ctx.state !== "running") await S.ctx.resume();

    const params = await waitForParams(S.wam);
    S.descriptor = S.wam.descriptor;
    S.params = params;
    demo.descriptor = S.wam.descriptor;
    demo.params = params;

    const d = S.wam.descriptor || {};
    S.mode = opts.mode
      || (d.hasMidiOutput && !d.hasAudioOutput ? "midi-effect"
        : (d.isInstrument && d.hasMidiInput ? "instrument"
          : (d.hasAudioInput ? "audio-effect" : "instrument")));

    // The body shell was built at load for the presumed mode; if the real mode
    // differs (only possible when a demo omits opts.mode), build it now.
    if (S.mode !== S.shellMode) { $("#body").innerHTML = ""; buildBodyShell(S.mode); S.shellMode = S.mode; }

    buildParams(params);           // replaces the reserved (empty) param grid

    // A demo page may pick a musical starting point without touching the
    // plugin's own defaults. `wah` ships Gain defaulting to +20 dB (the top of
    // its 0..20 dB range) on a resonant filter, which is a fine thing for a
    // plugin to allow and a terrible thing to open a web page on.
    // Keyed by parameter LABEL so a page never hardcodes a numeric id.
    for (const [label, value] of Object.entries(opts.initialParams || {})) {
      const param = params.find((q) => q.label === label);
      if (!param) { console.warn(`initialParams: no parameter named "${label}"`); continue; }
      S.wam.setParameterValue(param.id, value);
      const entry = (window.__widgets || {})[param.id];
      entry?.el?.setValue?.(value);
      if (entry?.valEl) entry.valEl.textContent = formatValue(param, value);
    }
    await activateAudio(S.mode);   // wire the audio graph to the existing shell

    demo.started = true; S.starting = false;
    const khz = +(S.ctx.sampleRate / 1000).toFixed(1);
    status(`${S.wam.descriptor.name} ready — ${khz} kHz`);
    if (S.mode === "instrument" || (S.mode === "midi-effect" && opts.midiViz !== "sysex")) connectWebMidi(khz);
    meter();
    startParamSync();
  }

  // Arm the start overlay. The whole overlay is clickable (large target); the
  // play affordance is also a keyboard button (Enter/Space). start() is the
  // user-activation gesture and is re-entrancy guarded, so double activation
  // never builds two AudioContexts.
  function armOverlay() {
    $("#overlay").addEventListener("click", start, { once: true });
  }
  $("#ov-start").addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") { e.preventDefault(); start(); }
  });

  // Build the panel to its FINAL height at load so the overlay isn't squished
  // and the panel doesn't jump when audio starts (#3). The param grid's height
  // is reserved from the per-demo row count; the real widgets replace it on start.
  S.shellMode = presumedMode();
  reserveParamGrid(opts.paramRows);
  buildBodyShell(S.shellMode);
  armOverlay();

  $("#stop").addEventListener("click", async () => {
    if (!S.ctx) return;
    allNotesOff();
    stopSource();
    S.meterToken++;                    // stop the meter/scope loop
    S.meterWidget?.reset();
    await S.ctx.close();
    if (S.synthCtx) { try { await S.synthCtx.close(); } catch {} }
    S.ctx = null; S.wam = null; S.synth = null; S.synthCtx = null; S.analyser = null;
    S.pool = []; S.voiceByChan.clear(); S.voiceClock = 0;
    S.limiter = null; S.inputTrim = null;   // belong to the closed context; rebuilt on next start
    S.chainSynth = false; S.starting = false;
    demo.started = false;
    $("#overlay").style.display = "flex";
    armOverlay();
    status("");
  });

  // Test seams.
  window.__start = start;
  // Test seam: gain reduction the safety limiter is applying, in dB (0 = idle).
  window.__limiterReductionDb = () => S.limiter?.reduction ?? null;
  // Test seam: read/replace the source trim while running, to calibrate a demo.
  window.__inputTrim = (v) => {
    if (v !== undefined && S.inputTrim) S.inputTrim.gain.value = v;
    return S.inputTrim?.gain.value ?? null;
  };
  window.__player = {
    setParam: (id, v) => S.wam?.setParameterValue(id, v),
    getParam: (id) => S.wam?.getParameterValue(id),
    noteOn: (n, v = 100) => noteOn(n, "test", v),
    noteOff: (n) => noteOff(n, "test"),
    sendSysex: (arr) => S.wam?.sendSysex(arr instanceof Uint8Array ? arr : new Uint8Array(arr)),
    state: S, demo,
  };

  return window.__player;
}
