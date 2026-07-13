#!/usr/bin/env node
// file-upload.test.mjs — the guard for mountDemo({ fileUpload }).
//
// The drop zone lives in the SHARED shell, so a WAM demo and a WebCLAP demo get it
// from one code path and cannot drift. These tests pin the six behaviours that,
// each one skipped, make a drop zone feel broken — and in particular the two that
// are easy to get subtly wrong and impossible to notice in review:
//
//   • the DOCUMENT-level guard, without which a ten-pixel miss NAVIGATES to the
//     file and destroys the running demo (audio context, loaded state, knobs), and
//   • depth-counted dragenter/dragleave, without which crossing a child inside the
//     zone strobes the highlight.
//
// NOTE ON EVENT ORDER — this is the trap. The REAL browser order when the pointer
// crosses from the zone into a child of the zone is `dragenter` on the NEW target
// and THEN `dragleave` on the OLD one. A test that fires a bare `dragleave` is not
// reproducing a browser; it will "find" a flicker bug that does not exist. Every
// crossing below is modelled in the real order.
import { document, ok, failed, audioNode } from "./dom-shim.mjs";

const { mountDemo } = await import("../src/shell.js");
const { buildContainer, parseContainer } = await import("../src/state/plugin-state.js");

// ——— a mock adapter whose state is a real PLST round-trip target
function makeAdapter(initialState = new Uint8Array(0)) {
  let state = initialState;
  return {
    descriptor: { name: "Mock", isInstrument: false, hasAudioInput: true, hasAudioOutput: true },
    audioNode: audioNode(),
    getParameterInfo: async () => [
      { id: 1, label: "Size", type: "float", minValue: 0, maxValue: 1, defaultValue: 0.5 },
    ],
    setParameterValue() {},
    getParameterValue: async () => 0.5,
    scheduleMidi() {}, sendSysex() {},
    getState: async () => state,
    setState(b) { state = b; this.writes.push(b); },
    onMidiOut: null, onParamsChanged: null,
    createSecondary: async () => makeAdapter(),
    destroy() {},
    writes: [],
  };
}

const settle = () => new Promise((r) => setTimeout(r, 0));

async function mount(fileUpload, adapter = makeAdapter()) {
  // Release the previous mount FIRST. The document-level guard is process-wide, so
  // without this every case would inherit the last one's listeners — which is the
  // very leak rule 1's destroy() exists to prevent, and it would mask itself.
  globalThis.__player?.destroy?.();
  document.body.childNodes = [];
  const root = document.createElement("div");
  document.body.appendChild(root);
  let sessionCtx = null;              // the AudioContext the shell actually built
  await mountDemo({
    root, title: "Mock", subtitle: "drop zone", mode: "audio-effect", paramRows: 1,
    createAdapter: async (ctx) => { sessionCtx = ctx; return adapter; },
    fileUpload,
  });
  await globalThis.__start();
  await settle();
  return { root, adapter, zone: root.querySelector("#pp-fu"), ctx: () => sessionCtx };
}

// A drag event carrying a real-ish DataTransfer.
function dragEv(type, files = []) {
  const dt = { files, dropEffect: "none", types: files.length ? ["Files"] : ["text/plain"] };
  let defaultPrevented = false;
  return {
    type, dataTransfer: dt,
    get defaultPrevented() { return defaultPrevented; },
    preventDefault() { defaultPrevented = true; },
    stopPropagation() {},
  };
}
const fakeFile = (name = "room.wav", bytes = [1, 2, 3, 4]) => ({
  name,
  arrayBuffer: async () => new Uint8Array(bytes).buffer,
});

const CFG = { accept: "audio/*,.wav", label: "Load impulse response…", hint: "or drop a file here" };

// ——— 1. Both affordances exist. Drop is a shortcut, NOT a replacement — the button
//        is the only path that exists on a touch device.
{
  const { root, zone } = await mount(CFG);
  ok(!!zone, "zone: mounted into the panel");
  ok(!!root.querySelector("#pp-fu-pick"), "rule 6: the file-dialog button is present");
  ok(!!root.querySelector("#pp-fu-file"), "rule 6: a real <input type=file> backs the button");
  ok(root.querySelector("#pp-fu-file").getAttribute("accept") === CFG.accept,
     "zone: accept list is passed through to the input");
  ok(/Load impulse response/.test(root.querySelector("#pp-fu-pick").textContent),
     "zone: the button carries the configured label");
}

// ——— 2. THE DOCUMENT GUARD. A file dropped anywhere on the page must not navigate.
{
  const { root } = await mount(CFG);
  ok(document.listenerCount("dragover") === 1, "rule 1: document dragover guard is bound");
  ok(document.listenerCount("drop") === 1, "rule 1: document drop guard is bound");

  // A stray drop — a ten-pixel miss — must be swallowed, not acted on.
  const stray = dragEv("drop", [fakeFile()]);
  document.dispatchEvent(stray);
  ok(stray.defaultPrevented,
     "rule 1: a stray drop is preventDefault()ed (a near miss does NOT navigate away)");

  const strayOver = dragEv("dragover");
  document.dispatchEvent(strayOver);
  ok(strayOver.defaultPrevented, "rule 1: document dragover is preventDefault()ed");

  // ...and it must do NOTHING else: no load, no state write.
  await settle();
  ok(root.querySelector("#pp-fu-msg").innerHTML === "",
     "rule 1: a stray drop is inert — the zone does not react to it");
}

// ——— 3. Depth-counted highlight. Crossing a CHILD must not strobe.
{
  const { root, zone } = await mount(CFG);
  const lit = () => zone.classList.contains("over");
  const fire = (el, type, files) => { const e = dragEv(type, files); el.dispatchEvent(e); return e; };

  fire(zone, "dragenter");
  ok(lit(), "rule 2: highlight on after dragenter");

  // Pointer crosses INTO the button inside the zone. REAL browser order:
  // dragenter on the new target FIRST, then dragleave on the old one.
  fire(zone, "dragenter");          // enter (bubbled from the child)
  fire(zone, "dragleave");          // ...then leave the old target
  ok(lit(), "rule 2: highlight SURVIVES crossing a child (depth counted, not toggled)");

  // Now genuinely leave the zone.
  fire(zone, "dragleave");
  ok(!lit(), "rule 2: highlight clears only when depth returns to zero");

  // 3. The highlight is scoped to the ZONE, never the whole plugin.
  fire(zone, "dragenter");
  ok(zone.classList.contains("over"), "rule 3: the zone carries the highlight");
  ok(!root.querySelector("#panel").classList.contains("over"),
     "rule 3: the highlight is NOT applied to the whole plugin");
  fire(zone, "dragleave");
}

// ——— 4. dropEffect = copy (a copy badge, not a "no entry" sign).
//
// This rule can ONLY be pinned here, in the shim. In a real browser `dropEffect` is
// not settable on a SYNTHETIC DataTransfer — the spec honours it only during a real
// user-initiated drag, so `dt.dropEffect = "copy"` silently stays "none" and a
// browser-driven test "fails" a line of code that is perfectly correct. That is the
// same trap as the bare-dragleave one above: do not "fix" the shell to satisfy it.
{
  const { zone } = await mount(CFG);
  const e = dragEv("dragover", [fakeFile()]);
  zone.dispatchEvent(e);
  ok(e.dataTransfer.dropEffect === "copy", "rule 4: dragover sets dropEffect = copy");
  ok(e.defaultPrevented, "rule 4: dragover on the zone is preventDefault()ed");
}

// ——— 5. The empty drop is real: dragged text or a URL yields files[0] === undefined.
{
  const { root, zone } = await mount(CFG);
  zone.dispatchEvent(dragEv("drop", []));      // no file in the payload
  await settle();
  ok(/no file in it/i.test(root.querySelector("#pp-fu-msg").innerHTML),
     "rule 5: an empty drop is explained, not thrown");
}

// ——— 6. A real drop delivers the file, params-preserving, through PLST.
{
  const { root, zone, adapter } = await mount(CFG);
  zone.dispatchEvent(dragEv("drop", [fakeFile("room.wav")]));
  await settle(); await settle();
  ok(adapter.writes.length === 1, "drop: the file is written into the plugin's state");
  const written = adapter.writes[0];
  ok(written instanceof Uint8Array && written.length > 0, "drop: a PLST container reaches setState");
  ok(/room\.wav/.test(root.querySelector("#pp-fu-msg").innerHTML),
     "drop: the zone names the file it loaded");
}

// ——— 7. onFile override: the consumer owns the ENCODING (only the plugin knows how
//        its bytes want to look), and is handed the params-preserving write.
{
  const seen = [];
  let handedCtx = null;
  const { zone, adapter, ctx } = await mount({
    ...CFG,
    onFile: async (file, api) => {
      seen.push(file.name);
      handedCtx = api.ctx;
      ok(typeof api.writeBlob === "function", "onFile: handed the params-preserving writeBlob");
      ok(api.adapter === adapter, "onFile: handed the live adapter");
      await api.writeBlob(new Uint8Array([9, 9, 9]));
      api.setMessage("custom");
    },
  });
  zone.dispatchEvent(dragEv("drop", [fakeFile("ir.aiff")]));
  await settle(); await settle();
  ok(seen[0] === "ir.aiff", "onFile: the consumer's handler receives the dropped File");
  ok(adapter.writes.length === 1, "onFile: the consumer's blob reached the plugin");
  // The encoding an audio plugin wants is a DECODE, and decodeAudioData is a method
  // on the AudioContext — the SESSION's, so the PCM lands at the rate the plugin runs
  // at. Hand over the very context the adapter was built on, or every consumer either
  // smuggles one in through a closure or builds a second one at the wrong rate.
  ok(handedCtx && handedCtx === ctx(),
     "onFile: handed the SESSION AudioContext (the one the adapter was created on)");
}

// ——— 7b. LOADING A FILE MUST NOT RESET THE KNOBS.
//
// The plugin's state is one container: [params][plugin blob]. The lazy way to deliver
// a file is to setState() a container built around the new blob — which silently
// writes an EMPTY parameter block, and every knob the user has moved snaps back to
// its default the moment they drop a file. writeBlob() reads the live state, keeps
// its params byte-for-byte, and swaps ONLY the plugin's half.
{
  const params = new Uint8Array([0x50, 0x55, 0x4c, 0x50, 7, 7, 7, 7]);   // a "PULP" store blob
  const seeded = makeAdapter(buildContainer(params, new Uint8Array([1, 1])));
  const { zone, adapter } = await mount(CFG, seeded);

  zone.dispatchEvent(dragEv("drop", [fakeFile("room.wav", [0xaa, 0xbb, 0xcc])]));
  await settle(); await settle();

  ok(adapter.writes.length === 1, "params: the drop wrote the plugin's state exactly once");
  const after = parseContainer(adapter.writes[0]);
  ok(after.params.length === params.length && after.params.every((b, i) => b === params[i]),
     "params: the parameter block survives a file load BYTE-FOR-BYTE (the knobs do not reset)");
  ok(after.plugin.length === 3 && after.plugin[0] === 0xaa,
     "params: ...and only the plugin-owned blob is replaced, by the dropped file's bytes");
}

// ——— 8. destroy() takes the DOCUMENT guard back off. A leak here means a re-mounted
//        page stacks another pair of document listeners on every mount.
{
  await mount(CFG);
  ok(document.listenerCount("drop") === 1, "teardown: guard bound while mounted");
  globalThis.__player.destroy();
  ok(document.listenerCount("dragover") === 0, "rule 1: destroy() unbinds the document dragover guard");
  ok(document.listenerCount("drop") === 0, "rule 1: destroy() unbinds the document drop guard");
}

// ——— 9. No fileUpload opt ⇒ nothing rendered, and no document listeners leaked.
{
  const { root } = await mount(undefined);
  ok(!root.querySelector("#pp-fu"), "no fileUpload: no zone rendered");
  ok(document.listenerCount("drop") === 0, "no fileUpload: no document guard bound");
}

console.log(failed ? `\n${failed} assertion(s) FAILED`
                   : "\nfile upload (dialog + drop zone) intact — all assertions passed");
process.exit(failed ? 1 : 0);
