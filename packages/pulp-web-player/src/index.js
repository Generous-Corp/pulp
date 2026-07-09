// @pulp/web-player — a reusable, skinnable, host-agnostic web player for Pulp
// (WAMv2 / WebCLAP / …) plugins.
//
// Public entry. This wires the host-agnostic shell (shell.js) to the default WAM
// adapter (adapters/wam.js) so a consumer can `mountDemo({...})` with nothing but
// a built plugin's dsp/processor URLs. The shell itself imports no backend — an
// alternate ABI or a test stub is supplied via `opts.createAdapter`.
//
//   import { mountDemo } from "@pulp/web-player";
//   mountDemo({
//     root: document.getElementById("app"),
//     title: "My Plugin",
//     dspUrl: "./wam-dsp.js", processorUrl: "./wam-processor.js",
//     mode: "audio-effect", paramRows: 1,
//     // skinnability (all optional): supply YOUR design-token contract + font.
//     tokensHref: "/my-tokens.css", fontHref: "/my-fonts.css", theme: "auto",
//   });
//
// See README.md for the ~15-token design-token contract and the host-adapter
// interface (adapters/adapter.d.ts).

import { mountDemo as mountShell } from "./shell.js";
import { createWamAdapter } from "./adapters/wam.js";

/**
 * Mount a Pulp web-player demo. Identical to the shell's mountDemo, but injects
 * the default WAM adapter when the caller doesn't supply their own.
 * @param {Object} opts
 * @returns {Promise<Object>} the `window.__player` test/inspection handle.
 */
export function mountDemo(opts = {}) {
  return mountShell({
    createAdapter: opts.createAdapter || createWamAdapter,
    ...opts,
  });
}

// The default adapter + factory type live here for consumers building their own
// (e.g. a WebCLAP adapter of the same shape — see adapters/adapter.d.ts).
export { createWamAdapter };

// The canvas widget set, for consumers who want to build custom UI from the same
// native-styled knob/fader/toggle/combo/meter primitives.
export { createWidget, kindFor, formatValue } from "./widgets/index.js";
export { initModality } from "./widgets/base.js";

// The host-agnostic shell entry, for consumers who ALWAYS pass their own adapter
// and don't want the WAM backend pulled in as the default.
export { mountShell };

export default mountDemo;
