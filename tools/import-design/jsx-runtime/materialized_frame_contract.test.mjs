import assert from 'node:assert/strict';
import test from 'node:test';
import { resolveMaterializedFrames } from './materialized_frame_contract.mjs';

test('keeps full Chromium paint separate from an inset behavior frame', () => {
  const frames = resolveMaterializedFrames({ root: {
    style: { width: 1320, height: 860 },
    attributes: {
      browser_authored_frame_x: '26.0465087890625',
      browser_authored_frame_y: '0',
      browser_authored_frame_width: '1227.906982421875',
      browser_authored_frame_height: '800',
    },
  }});
  assert.deepEqual(frames.visual, { left: 0, top: 0, width: 1320, height: 860 });
  assert.deepEqual(frames.behavior, {
    left: 26.0465087890625, top: 0,
    width: 1227.906982421875, height: 800,
  });
});

test('legacy uncropped captures use the visual frame for behavior', () => {
  const frames = resolveMaterializedFrames({ root: {
    style: { width: '1000', height: '600' }, attributes: {},
  }});
  assert.deepEqual(frames.visual, { left: 0, top: 0, width: 1000, height: 600 });
  assert.deepEqual(frames.behavior, { left: 0, top: 0, width: 1000, height: 600 });
});

test('lays out in authored CSS pixels and applies the capture affine once', () => {
  const frames = resolveMaterializedFrames({ root: {
    style: { width: 1320, height: 860 }, attributes: {
      browser_authored_frame_x: '26.046511627906966',
      browser_authored_frame_width: String(1320 * 800 / 860),
      browser_authored_frame_height: '800',
    },
  }}, {
    authored_box: { width: 1320, height: 860 },
    captured_transform: {
      a: 800 / 860, b: 0, c: 0, d: 800 / 860,
      e: 26.046511627906966, f: 0,
    },
  });
  assert.deepEqual(frames.behavior, {
    left: 0, top: 0, width: 1320, height: 860,
    transform: {
      a: 800 / 860, b: 0, c: 0, d: 800 / 860,
      e: 26.046511627906966, f: 0,
    },
  });
});

test('rejects a missing visual frame', () => {
  assert.throws(() => resolveMaterializedFrames({ root: { style: {} } }),
                /visual frame/);
});
