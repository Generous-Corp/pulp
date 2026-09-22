// SPDX-License-Identifier: MIT

// Bounded comparison of a complete, single-line LTR ASCII range. Neither
// selection bands nor DOM fragments establish vertical ink or baseline parity.
export function compareNativeSelection(browser, native, join, tolerance = 0.25) {
  if (browser?.schema !== 'pulp-text-diagnostics-v1' ||
      native?.schema !== 'pulp-native-selection-diagnostics-v1' ||
      !browser.capture_basis || !Array.isArray(browser.runs) || !Array.isArray(native.runs) ||
      browser.coordinate_space !== 'primary-document CSS pixels; snapshot coordinates' ||
      native.coordinate_space !== 'observer-root logical pixels; untransformed only' ||
      join?.capture_basis !== browser.capture_basis ||
      join?.native_input !== native.input ||
      !Array.isArray(join?.native_origin_in_browser) ||
      join.native_origin_in_browser.length !== 2 ||
      !join.native_origin_in_browser.every(Number.isFinite) ||
      !Array.isArray(join.runs) || !Number.isFinite(tolerance) || tolerance < 0)
    throw new Error('Invalid or unbound browser/native join');
  const seenBrowser = new Set(), seenNative = new Set();
  const rows = join.runs.map(pair => {
    if (seenBrowser.has(pair.browser_run) || seenNative.has(pair.native_anchor))
      throw new Error('Join must be one-to-one');
    seenBrowser.add(pair.browser_run); seenNative.add(pair.native_anchor);
    const references = browser.runs.filter(run => run.id === pair.browser_run);
    const candidates = native.runs.filter(run => run.anchor === pair.native_anchor);
    const result = { browser_run: pair.browser_run, native_anchor: pair.native_anchor,
      vertical_geometry: { status: 'unavailable', reason: 'Selection bands and CDP fragments have different vertical meanings' },
      resolved_native_faces: { status: 'unavailable', reason: 'Label exposes the paint font request, not the selected backend face' } };
    if (references.length !== 1 || candidates.length !== 1)
      return { ...result, status: 'incomparable', reason: 'Missing or ambiguous identity' };
    const a = references[0], b = candidates[0];
    const fragments = a.metrics.fragment_boxes;
    const fragment = fragments?.value?.[0], line = b.lines?.[0];
    if (a.text !== b.text || !/^[\x21-\x7e](?:[\x20-\x7e]*[\x21-\x7e])?$/.test(a.text) ||
        pair.layout_contract !== 'untransformed-horizontal-ltr-single-line' ||
        !b.coordinate_supported || !b.measured || b.lines?.length !== 1 ||
        fragments?.status !== 'observed' || fragments.value.length !== 1 ||
        fragment.start !== 0 || fragment.length !== a.text.length ||
        line.start_utf8 !== 0 || line.end_utf8 !== b.text.length ||
        !Array.isArray(line.caret_x) || line.caret_x.length < 2 ||
        !line.caret_x.every(Number.isFinite) ||
        line.caret_x.some((x, i) => i > 0 && x < line.caret_x[i - 1]) ||
        line.byte_offsets?.length !== line.caret_x.length ||
        line.byte_offsets[0] !== 0 || line.byte_offsets.at(-1) !== b.text.length ||
        fragment.bounds?.length !== 4 || fragment.bounds[2] < 0 ||
        !fragment.bounds.every(Number.isFinite))
      return { ...result, status: 'incomparable', reason: 'Unsupported text, range, coordinate mapping or paint evidence' };
    const reference = [fragment.bounds[0], fragment.bounds[0] + fragment.bounds[2]];
    const candidate = [line.caret_x[0], line.caret_x.at(-1)]
      .map(x => x + join.native_origin_in_browser[0]);
    const delta = candidate.map((x, i) => x - reference[i]);
    const faces = a.metrics.resolved_faces;
    const names = faces?.status === 'observed' ? faces.value.flatMap(face =>
      [face.family_name, face.post_script_name]).filter(Boolean) : [];
    return { ...result, status: 'compared',
      horizontal_range: { status: delta.every(x => Math.abs(x) <= tolerance) ? 'match' : 'mismatch',
        reference, candidate, delta, units: 'CSS px', meaning: 'full-range horizontal extent; not glyph ink' },
      paint_font_request: { status: names.length === 0 ? 'unavailable'
        : names.includes(b.paint_font_request) ? 'match' : 'mismatch',
        native_request: b.paint_font_request, browser_resolved_names: names,
        meaning: 'exact-name membership only; aliases may differ; not native resolved-face proof' } };
  });
  return { schema: 'pulp-browser-native-selection-comparison-v1',
    capture_basis: browser.capture_basis, native_input: native.input,
    native_origin_in_browser: join.native_origin_in_browser,
    tolerance_css_px: tolerance,
    join_validation: 'explicit caller-owned fixture correspondence; not inferred from matching text',
    coverage: { browser_runs: browser.runs.length, native_runs: native.runs.length, joined_runs: rows.length },
    rows };
}
