// SPDX-License-Identifier: MIT

// Which typeface Blink ACTUALLY shaped each text run with.
//
// `font-family` is a request, not an outcome. It is a prioritised list, any
// entry of which may be a webfont that failed to load, a family the host does
// not have, or a name that matches a face with different metrics than the one
// the design was authored against. The computed style records only the request,
// so a capture that carries `font-family: 'Jost', sans-serif` is silent on the
// one question a native renderer has to answer: did the reference pixels come
// from Jost, or from whatever the machine substituted?
//
// `CSS.getPlatformFontsForNode` answers it. Per face it reports the resolved
// `familyName` and `postScriptName`, whether it came from an `@font-face`
// (`isCustomFont`) or the host's own database, and `glyphCount` — how many
// glyphs that face actually contributed. The glyph counts are what make
// per-cluster fallback visible: a run that reports two faces was shaped by two,
// and the split says which characters each one drew.
//
// Without this, "our text is the wrong width" and "the reference was rendered
// in a different typeface than we bundled" produce identical evidence, and a
// font shipped to close the gap can widen it instead — by making Pulp resolve a
// family the reference never used.

/// Nodes to interrogate individually before the report stops enumerating.
///
/// A per-element round trip is cheap but not free, and a pathological document
/// can have tens of thousands of text runs. The cap bounds capture time; it is
/// recorded in the report so a truncated census never reads as a complete one.
const MAX_INTERROGATED_ELEMENTS = 4000;

/// The layout indices that laid out text, paired with the element whose
/// computed font produced it.
///
/// A text run's own DOM node is a `#text`, which has no computed style of its
/// own and which `CSS.getPlatformFontsForNode` cannot resolve a font for. Its
/// parent element is the one that carries `font-family`, so that is the node
/// the font question is asked of — and the node the answer belongs to.
function textBearingElements(snapshot) {
  const document = snapshot.documents?.[0];
  if (!document) return [];
  const layout = document.layout ?? {};
  const nodes = document.nodes ?? {};
  const nodeIndex = layout.nodeIndex ?? [];
  const text = layout.text ?? [];
  const parentIndex = nodes.parentIndex ?? [];
  const nodeType = nodes.nodeType ?? [];
  const backendNodeId = nodes.backendNodeId ?? [];

  const found = [];
  for (let i = 0; i < nodeIndex.length; i += 1) {
    if ((text[i] ?? -1) < 0) continue;
    const node = nodeIndex[i];
    if (node < 0 || node >= backendNodeId.length) continue;
    // A text run answers through its parent; an element that laid text out
    // directly (an `::before` string, an `<input>` value) answers for itself.
    const owner = nodeType[node] === 3 ? (parentIndex[node] ?? -1) : node;
    if (owner < 0 || owner >= backendNodeId.length) continue;
    const backend = backendNodeId[owner];
    if (typeof backend !== "number") continue;
    found.push({
      layout_index: i,
      node_index: node,
      owner_node_index: owner,
      backend_node_id: backend,
    });
  }
  return found;
}

/// The computed values that state what the run ASKED for, read out of the
/// snapshot's own style rows.
///
/// Recording the request beside the resolution is the whole point: a face named
/// alone cannot be compared to anything, while "asked Jost, got Helvetica" is a
/// finding. Names are read from `computedStyleNames` rather than a positional
/// guess, for the reason that list exists.
function requestedFont(snapshot, styleNames, layoutIndex) {
  const document = snapshot.documents?.[0];
  // The request order travels with the snapshot only once it is written to
  // disk; in memory it is still the caller's list. Reading `computedStyleNames`
  // off the live object silently found nothing and reported every run as having
  // asked for no font at all.
  const names = styleNames ?? [];
  const strings = snapshot.strings ?? [];
  const row = document?.layout?.styles?.[layoutIndex] ?? [];
  const read = (property) => {
    const at = names.indexOf(property);
    if (at < 0 || at >= row.length) return "";
    const index = row[at];
    return index >= 0 ? (strings[index] ?? "") : "";
  };
  return {
    font_family: read("font-family"),
    font_size: read("font-size"),
    font_weight: read("font-weight"),
    font_style: read("font-style"),
    letter_spacing: read("letter-spacing"),
  };
}

/// The primary font's line metrics as Blink resolved them for an element.
///
/// A text fragment's box is the content area Blink built from the primary
/// font's ascent and descent, and its baseline sits exactly `ascent` below
/// the box top. Those metrics are not derivable from the font file alone: Blink
/// rounds them, and on some platforms adjusts them per face before rounding.
/// Canvas `fontBoundingBoxAscent` / `fontBoundingBoxDescent` report the same
/// primary-font metrics for the same font request, and asking for them does
/// not touch the document, so the captured layout cannot be perturbed.
///
/// Runs in an isolated world so page script cannot shadow the canvas or style
/// APIs it relies on. Any request the canvas font cannot express exactly
/// answers null, and a consumer then falls back to its own metrics.
export const LINE_METRICS_FUNCTION = `function(pseudo) {
  if (!(this instanceof Element)) return null;
  const s = getComputedStyle(this, pseudo || null);
  if (s.fontSizeAdjust && s.fontSizeAdjust !== 'none') return null;
  const stretches = {
    '50%': 'ultra-condensed', '62.5%': 'extra-condensed',
    '75%': 'condensed', '87.5%': 'semi-condensed', '100%': 'normal',
    '112.5%': 'semi-expanded', '125%': 'expanded',
    '150%': 'extra-expanded', '200%': 'ultra-expanded',
  };
  const stretch = stretches[s.fontStretch];
  if (!stretch) return null;
  const canvas = new OffscreenCanvas(1, 1);
  const ctx = canvas.getContext('2d');
  if (!ctx) return null;
  const request = s.fontStyle + ' ' + s.fontWeight + ' ' + s.fontSize + ' ' +
                  s.fontFamily;
  const sentinel = ctx.font;
  ctx.font = request;
  // An unparseable request leaves the previous font in place, which would
  // report the canvas default's metrics as this element's.
  if (ctx.font === sentinel && request !== sentinel) return null;
  ctx.fontStretch = stretch;
  const m = ctx.measureText('H');
  const ascent = m.fontBoundingBoxAscent;
  const descent = m.fontBoundingBoxDescent;
  if (!Number.isFinite(ascent) || !Number.isFinite(descent)) return null;
  return { ascent, descent };
}`;

/// Where to ask for a run's computed font: the owning element itself, or, for
/// generated content (::before, ::after, ::marker), the element that hosts the
/// pseudo-element, asked for that pseudo-element's style. Null when the owner
/// cannot be addressed that way.
function metricsTarget(snapshot, candidate) {
  const document = snapshot.documents?.[0];
  const nodes = document?.nodes ?? {};
  const strings = snapshot.strings ?? [];
  const pseudo = nodes.pseudoType;
  const at = (pseudo?.index ?? []).indexOf(candidate.owner_node_index);
  if (at < 0) {
    return { backend: candidate.backend_node_id, pseudo: "" };
  }
  const name = strings[pseudo.value?.[at] ?? -1] ?? "";
  if (!["before", "after", "marker"].includes(name)) return null;
  const host = (nodes.parentIndex ?? [])[candidate.owner_node_index] ?? -1;
  const backend = (nodes.backendNodeId ?? [])[host];
  if (typeof backend !== "number") return null;
  return { backend, pseudo: `::${name}` };
}

function metricsKey(target) {
  return `${target.backend}|${target.pseudo}`;
}

async function measureLineMetrics(cdp, targets) {
  const metrics = new Map();
  if (targets.length === 0) return metrics;
  let contextId;
  try {
    const tree = await cdp.call("Page.getFrameTree");
    const frameId = tree.frameTree?.frame?.id;
    if (!frameId) return metrics;
    const world = await cdp.call("Page.createIsolatedWorld", {
      frameId, worldName: "pulp-line-metrics",
    });
    contextId = world.executionContextId;
  } catch {
    return metrics;
  }
  for (const target of targets) {
    let objectId;
    try {
      const resolved = await cdp.call("DOM.resolveNode", {
        backendNodeId: target.backend, executionContextId: contextId,
      });
      objectId = resolved.object?.objectId;
      if (!objectId) continue;
      const measured = await cdp.call("Runtime.callFunctionOn", {
        objectId,
        functionDeclaration: LINE_METRICS_FUNCTION,
        arguments: [{ value: target.pseudo }],
        returnByValue: true,
      });
      const value = measured.result?.value;
      if (value && Number.isFinite(value.ascent) &&
          Number.isFinite(value.descent)) {
        metrics.set(metricsKey(target),
                    { ascent: value.ascent, descent: value.descent });
      }
    } catch {
      // A node that cannot be resolved or measured answers nothing; its run
      // records null rather than a guess.
    } finally {
      if (objectId) {
        await cdp.call("Runtime.releaseObject", { objectId }).catch(() => {});
      }
    }
  }
  return metrics;
}

function normalizeFonts(fonts) {
  return (fonts ?? []).map((font) => ({
    family_name: font.familyName ?? "",
    post_script_name: font.postScriptName ?? "",
    is_custom_font: font.isCustomFont === true,
    glyph_count: font.glyphCount ?? 0,
  }));
}

/// Interrogate the page for the faces Blink resolved, and return the report
/// written beside the snapshot.
///
/// Runs while virtual time is still paused, alongside the other DOM-read
/// sidecars, so the answers describe the same frozen frame the screenshot does.
export async function evaluatePlatformFonts(cdp, snapshot, styleNames) {
  await cdp.call("DOM.enable");
  await cdp.call("CSS.enable");
  // The frontend node map is populated lazily; without a document to hang them
  // off, pushing backend ids resolves nothing.
  await cdp.call("DOM.getDocument", { depth: -1 });

  // A whole-document census by way of one subtree call is tempting and is NOT
  // trustworthy: `#document` and `<html>` both answer with an empty list rather
  // than an error, and `<body>` answers fully on one page and emptily on the
  // next depending on where the inline formatting contexts sit. An empty list
  // is indistinguishable from "this page used no fonts" — the one answer a page
  // with text can never truthfully give — so there is no such field here. The
  // census is summed from the per-run answers instead, and carries the
  // `truncated` flag that says whether it covers everything.

  const candidates = textBearingElements(snapshot);
  const truncated = candidates.length > MAX_INTERROGATED_ELEMENTS;
  const interrogated = truncated
    ? candidates.slice(0, MAX_INTERROGATED_ELEMENTS)
    : candidates;

  // Several runs commonly share one owner element (a wrapped paragraph, a
  // element with sibling text runs). Resolve each owner once and fan the
  // answer back out, so the cap counts elements rather than runs.
  const owners = [...new Set(interrogated.map((c) => c.backend_node_id))];
  const nodeIdByBackend = new Map();
  if (owners.length > 0) {
    try {
      const pushed = await cdp.call(
        "DOM.pushNodesByBackendIdsToFrontend", { backendNodeIds: owners });
      const ids = pushed.nodeIds ?? [];
      for (let i = 0; i < owners.length && i < ids.length; i += 1) {
        if (ids[i]) nodeIdByBackend.set(owners[i], ids[i]);
      }
    } catch {
      // Leaving the map empty reports every run unresolved rather than failing
      // the capture. `resolved` then reads zero against a non-zero `text_runs`,
      // which is a visible "the walk did not happen" — not a quiet empty
      // census that a reader could mistake for a fontless page.
    }
  }

  const fontsByBackend = new Map();
  for (const backend of owners) {
    const nodeId = nodeIdByBackend.get(backend);
    if (!nodeId) continue;
    try {
      const result = await cdp.call(
        "CSS.getPlatformFontsForNode", { nodeId });
      fontsByBackend.set(backend, normalizeFonts(result.fonts));
    } catch {
      // A node detached between the snapshot and this walk answers nothing;
      // it is reported as unresolved rather than as having no fonts.
    }
  }

  const targets = new Map();
  const targetOf = new Map();
  for (const candidate of interrogated) {
    const target = metricsTarget(snapshot, candidate);
    if (!target) continue;
    targets.set(metricsKey(target), target);
    targetOf.set(candidate.layout_index, metricsKey(target));
  }
  const lineMetrics = await measureLineMetrics(cdp, [...targets.values()]);

  const runs = interrogated.map((candidate) => ({
    ...candidate,
    requested: requestedFont(snapshot, styleNames, candidate.layout_index),
    resolved: fontsByBackend.get(candidate.backend_node_id) ?? null,
    line_metrics: lineMetrics.get(targetOf.get(candidate.layout_index)) ?? null,
  }));

  const resolvedRuns = runs.filter((run) => run.resolved !== null);
  // A face the page asked for by name and a face it was drawn with are
  // different questions; the summary answers the second, because the first is
  // already in every style row.
  const glyphsByFace = new Map();
  for (const run of resolvedRuns) {
    for (const face of run.resolved) {
      const key = `${face.family_name} ${face.post_script_name}`;
      const seen = glyphsByFace.get(key);
      if (seen) seen.glyph_count += face.glyph_count;
      else glyphsByFace.set(key, { ...face });
    }
  }

  return {
    schema: "pulp-browser-platform-fonts-v1",
    version: 1,
    summary: {
      text_runs: candidates.length,
      interrogated: interrogated.length,
      resolved: resolvedRuns.length,
      truncated,
    },
    // The census. Summed over the interrogated runs rather than asked of one
    // subtree, so it describes exactly the text this report covers — complete
    // when `truncated` is false, and honestly partial when it is true.
    faces_by_glyph_count: [...glyphsByFace.values()]
      .sort((a, b) => b.glyph_count - a.glyph_count),
    runs,
  };
}
