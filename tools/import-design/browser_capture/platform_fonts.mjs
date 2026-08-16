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

  const runs = interrogated.map((candidate) => ({
    ...candidate,
    requested: requestedFont(snapshot, styleNames, candidate.layout_index),
    resolved: fontsByBackend.get(candidate.backend_node_id) ?? null,
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
