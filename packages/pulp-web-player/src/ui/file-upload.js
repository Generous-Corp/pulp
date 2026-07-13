// ————————————————————————————————————————————————— file upload: dialog + drop
//
// mountDemo({ fileUpload }) gives a plugin that takes a user-supplied file — a
// convolver's impulse response, a sample, a preset — BOTH a file-dialog button and
// a drag-and-drop zone. A visible button is not enough: people drag files onto
// anything that looks like a target. And drop is not enough either — it is not a
// gesture that exists on a phone, so the button is the only path on touch.
//
// This lives in the PLAYER, not in a demo page, for the same reason every other UX
// invariant does: both ABIs (WAM and WCLAP) mount this one shell, so they inherit
// identical behavior instead of each growing a copy and drifting on the details
// below — every one of which, skipped, makes a drop zone feel broken.
//
// The ENCODING is the plugin's business, not ours: only the plugin knows how its
// bytes want to look (the convolver wants decoded mono PCM at the session rate).
// So the consumer supplies `onFile`; we own the interaction and hand it the File
// plus the params-preserving write. With no `onFile`, we do the honest default —
// hand the plugin the file's raw bytes and let it decode them itself.

import { setPluginBlob } from "../state/plugin-state.js";

const esc = (s) => String(s).replace(/[&<>"]/g, (c) =>
  ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

/**
 * @param {object}     o
 * @param {Element}    o.host     the #fileup slot in the panel
 * @param {object}     o.adapter  the live HostAdapter
 * @param {AudioContext} o.ctx    the demo's live AudioContext (see `api.ctx` below)
 * @param {object}     o.cfg      opts.fileUpload — { accept, label, hint, revertLabel, onFile, onRevert }
 * @returns {{destroy():void}|null}
 */
export function mountFileUpload({ host, adapter, ctx, cfg }) {
  if (!host || !cfg) return null;

  const label = cfg.label || "Choose a file…";
  const hint = cfg.hint || "or drop one here";
  const accept = cfg.accept || "";

  host.innerHTML = `
    <div class="pp-fu" id="pp-fu">
      <button class="pp-fu-btn" id="pp-fu-pick" type="button">${esc(label)}</button>
      <span class="pp-fu-hint">${esc(hint)}</span>
      ${cfg.revertLabel ? `<button class="pp-fu-btn" id="pp-fu-revert" type="button" disabled>${esc(cfg.revertLabel)}</button>` : ""}
      <input id="pp-fu-file" type="file"${accept ? ` accept="${esc(accept)}"` : ""} hidden>
      <div class="pp-fu-msg" id="pp-fu-msg" role="status" aria-live="polite"></div>
    </div>`;

  const $ = (id) => host.querySelector("#" + id);
  const zone = $("pp-fu");
  const pick = $("pp-fu-pick");
  const input = $("pp-fu-file");
  const revert = $("pp-fu-revert");   // may be absent
  const msgEl = $("pp-fu-msg");

  const setMessage = (html) => { msgEl.innerHTML = html == null ? "" : String(html); };

  // Handed to the consumer so nobody re-derives the "loading a file must not reset
  // the knobs" trick: read the live state, keep its params, swap only the blob.
  //
  // `ctx` is here because the encoding an audio plugin wants is almost always a
  // DECODE, and decodeAudioData is a method on the AudioContext — the SESSION's, so
  // the PCM arrives already at the rate the plugin runs at. Without it every such
  // consumer would either smuggle a context in through an onReady closure (implicit,
  // and ordered by luck) or build a second AudioContext at the wrong sample rate.
  const api = {
    adapter,
    ctx,
    setMessage,
    writeBlob: (bytes) => setPluginBlob(adapter, bytes),
  };

  let busy = false;
  async function load(file) {
    // 5. THE EMPTY DROP IS REAL. dataTransfer.files[0] is undefined when someone
    //    drags selected text or a URL onto the zone. Say so; do not throw.
    if (!file) { setMessage("That drop had no file in it."); return; }
    if (busy) return;
    busy = true;
    pick.disabled = true;
    try {
      setMessage(`Loading <span class="pp-fu-name">${esc(file.name)}</span>…`);
      if (typeof cfg.onFile === "function") {
        await cfg.onFile(file, api);           // consumer owns the encoding
      } else {
        // Honest default: hand the plugin the raw bytes and let it decode them.
        await api.writeBlob(new Uint8Array(await file.arrayBuffer()));
        setMessage(`Loaded <span class="pp-fu-name">${esc(file.name)}</span>.`);
      }
      if (revert) revert.disabled = false;
    } catch (err) {
      console.warn("file upload failed:", err);
      setMessage(`Could not load <span class="pp-fu-name">${esc(file.name)}</span>.`);
    } finally {
      busy = false;
      pick.disabled = false;
    }
  }

  // ——— drag-and-drop
  //
  // 1. A NEAR MISS MUST NOT NAVIGATE. The browser's default action for a file
  //    dropped ANYWHERE on the page is to open it — which throws away the running
  //    demo: the audio context, the loaded state, every knob the user has moved. So
  //    the DOCUMENT swallows every drag/drop, and only the zone acts on one. Without
  //    this, missing the target by ten pixels destroys the session — a brutal
  //    punishment for a gesture we invited.
  //
  // 2. dragleave FIRES WHEN THE POINTER CROSSES A CHILD. The zone contains a button
  //    and a label, and moving over them bubbles a dragleave from the child — so a
  //    handler that toggles on each event drops the highlight while the pointer is
  //    still very much inside the zone, and it strobes. Count enter/leave pairs and
  //    only clear at zero.
  let depth = 0;
  const lit = (on) => zone.classList.toggle("over", on);       // 3. scoped to the ZONE
  const stop = (e) => { e.preventDefault(); e.stopPropagation(); };

  const onDocDrag = (e) => { e.preventDefault(); };            // "yes, we take files"
  const onDocDrop = (e) => { e.preventDefault(); };            // ...but a miss does NOTHING

  const onEnter = (e) => { stop(e); depth++; lit(true); };
  const onOver = (e) => {
    stop(e);
    // 4. Show a copy badge, not a "no entry" sign.
    if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
    lit(true);
  };
  const onLeave = (e) => { stop(e); depth = Math.max(0, depth - 1); if (depth === 0) lit(false); };
  const onDrop = (e) => {
    stop(e); depth = 0; lit(false);
    load(e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0]);
  };

  // 6. KEEP THE BUTTON. Drop is a shortcut, not a replacement — and on a touch
  //    device it is the only path there is.
  const onPick = () => input.click();
  const onInput = () => { load(input.files && input.files[0]); input.value = ""; };
  const onRevert = async () => {
    if (typeof cfg.onRevert !== "function") return;
    try {
      await cfg.onRevert(api);
      if (revert) revert.disabled = true;
    } catch (err) { console.warn("revert failed:", err); }
  };

  pick.addEventListener("click", onPick);
  input.addEventListener("change", onInput);
  if (revert) revert.addEventListener("click", onRevert);
  zone.addEventListener("dragenter", onEnter);
  zone.addEventListener("dragover", onOver);
  zone.addEventListener("dragleave", onLeave);
  zone.addEventListener("drop", onDrop);
  document.addEventListener("dragover", onDocDrag);
  document.addEventListener("drop", onDocDrop);

  return {
    destroy() {
      pick.removeEventListener("click", onPick);
      input.removeEventListener("change", onInput);
      if (revert) revert.removeEventListener("click", onRevert);
      zone.removeEventListener("dragenter", onEnter);
      zone.removeEventListener("dragover", onOver);
      zone.removeEventListener("dragleave", onLeave);
      zone.removeEventListener("drop", onDrop);
      // The document-level guard is the one that MUST come off — leave it bound and
      // a re-mounted page stacks another pair on every mount.
      document.removeEventListener("dragover", onDocDrag);
      document.removeEventListener("drop", onDocDrop);
      host.innerHTML = "";
    },
  };
}
