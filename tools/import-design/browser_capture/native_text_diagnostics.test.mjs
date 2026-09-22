// SPDX-License-Identifier: MIT
import test from 'node:test';
import assert from 'node:assert/strict';
import { compareNativeSelection } from './native_text_diagnostics.mjs';
function fixture() {
  const browser = { schema: 'pulp-text-diagnostics-v1', capture_basis: 'basis', coordinate_space: 'primary-document CSS pixels; snapshot coordinates', runs: [{
    id: 'r', text: 'AV', metrics: {
      fragment_boxes: { status: 'observed', value: [{ start: 0, length: 2, bounds: [20, 30, 18, 17] }] },
      resolved_faces: { status: 'observed', value: [{ family_name: 'Arial', post_script_name: 'ArialMT' }] },
    } }] };
  const native = { schema: 'pulp-native-selection-diagnostics-v1', input: 'fixture.json', coordinate_space: 'observer-root logical pixels; untransformed only', runs: [{
    anchor: 'a', text: 'AV', paint_font_request: 'Arial', measured: true, coordinate_supported: true,
    lines: [{ start_utf8: 0, end_utf8: 2, caret_x: [10, 19, 28], byte_offsets: [0, 1, 2], selection_top: -999 }],
  }] };
  const join = { capture_basis: 'basis', native_input: 'fixture.json', native_origin_in_browser: [10, 20],
    runs: [{ browser_run: 'r', native_anchor: 'a', layout_contract: 'untransformed-horizontal-ltr-single-line' }] };
  return [browser, native, join];
}
test('explicit coordinate join compares horizontal range without equating vertical boxes', () => {
  const [row] = compareNativeSelection(...fixture()).rows;
  assert.equal(row.horizontal_range.status, 'match');
  assert.equal(row.paint_font_request.status, 'match');
  assert.equal(row.vertical_geometry.status, 'unavailable');
  assert.equal(row.resolved_native_faces.status, 'unavailable');
});
test('font request and translated native geometry controls are distinct', () => {
  const args = fixture();
  args[1].runs[0].paint_font_request = 'Courier New';
  args[1].runs[0].lines[0].caret_x = [22, 31, 40];
  const row = compareNativeSelection(...args).rows[0];
  assert.equal(row.paint_font_request.status, 'mismatch');
  assert.equal(row.horizontal_range.status, 'mismatch');
  assert.deepEqual(row.horizontal_range.delta, [12, 12]);
});
test('unpainted, transformed, non-ASCII, partial and ambiguous joins fail closed', () => {
  for (const mutate of [
    a => { a[1].runs[0].measured = false; },
    a => { delete a[1].runs[0].lines; },
    a => { a[1].runs[0].lines[0].caret_x = [28, 19, 10]; },
    a => { a[1].runs[0].coordinate_supported = false; },
    a => { a[0].runs[0].text = a[1].runs[0].text = 'é'; },
    a => { a[1].runs[0].lines[0].end_utf8 = 1; },
    a => { a[1].runs.push(a[1].runs[0]); },
  ]) { const args = fixture(); mutate(args); assert.equal(compareNativeSelection(...args).rows[0].status, 'incomparable'); }
});
test('foreign input and duplicate join cannot establish correspondence', () => {
  const args = fixture(); args[2].native_input = 'different.json';
  assert.throws(() => compareNativeSelection(...args), /unbound/);
  const duplicate = fixture(); duplicate[2].runs.push(duplicate[2].runs[0]);
  assert.throws(() => compareNativeSelection(...duplicate), /one-to-one/);
});
