// SPDX-License-Identifier: MIT

// The colour Chromium paints across the page canvas when neither the root nor
// the body has a background of its own.
//
// CSS leaves that canvas to the user agent, and Chromium paints it with the
// `Canvas` system colour of the root element's used color-scheme: white for a
// light root, a dark grey for `color-scheme: dark` or a
// `<meta name="color-scheme" content="dark">` page. Neither is in the DOM
// snapshot, which records the transparent backgrounds and nothing else, so the
// capture asks Chromium itself: a probe given the root's computed
// color-scheme and a `Canvas` background reads back the colour Chromium
// resolves. The probe runs after every frame has been captured and is removed
// in the same task, so no captured artifact can see it.

export const DOCUMENT_CANVAS_SCHEMA = "pulp-document-canvas-v1";

/// An opaque `rgb(r, g, b)` with integer channels, or null for anything else.
/// The canvas is always painted opaque, so a translucent or unparsed answer
/// is not a canvas colour and is not recorded as one.
export function opaqueCanvasColor(value) {
  const match = /^rgb\((\d{1,3}), (\d{1,3}), (\d{1,3})\)$/u.exec(String(value ?? ""));
  if (!match) return null;
  const channels = match.slice(1, 4).map(Number);
  if (channels.some((channel) => channel > 255)) return null;
  return `rgb(${channels.join(", ")})`;
}

const PROBE_EXPRESSION = `(() => {
  const root = document.documentElement;
  if (!root) return null;
  const scheme = getComputedStyle(root).getPropertyValue('color-scheme');
  const probe = document.createElement('pulp-document-canvas-probe');
  // Inline !important outranks any author rule that could reach the probe.
  probe.style.setProperty('display', 'none', 'important');
  probe.style.setProperty('color-scheme', scheme, 'important');
  probe.style.setProperty('background-color', 'Canvas', 'important');
  root.appendChild(probe);
  try {
    return {
      root_color_scheme: scheme,
      canvas: getComputedStyle(probe).getPropertyValue('background-color'),
    };
  } finally {
    probe.remove();
  }
})()`;

/// The report the capture writes into the DOM snapshot. `canvas_color` is
/// null when Chromium's answer could not be read as an opaque colour.
export async function evaluateDocumentCanvas(cdp) {
  let value = null;
  try {
    const evaluated = await cdp.call("Runtime.evaluate", {
      expression: PROBE_EXPRESSION,
      returnByValue: true,
    });
    if (!evaluated.exceptionDetails) value = evaluated.result?.value ?? null;
  } catch {
    value = null;
  }
  return {
    schema: DOCUMENT_CANVAS_SCHEMA,
    root_color_scheme: typeof value?.root_color_scheme === "string"
      ? value.root_color_scheme : null,
    canvas_color: opaqueCanvasColor(value?.canvas),
  };
}
