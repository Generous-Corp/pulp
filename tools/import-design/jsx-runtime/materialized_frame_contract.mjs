function finiteNumber(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function finitePositive(value) {
  const number = finiteNumber(value);
  return number > 0 ? number : 0;
}

// A materialized import has two intentionally different coordinate spaces:
// the complete Chromium capture is the visible paint authority, while the
// authored application frame inside it owns executable behavior/layout.
// Keeping this distinction in one contract prevents state screenshots from
// being scaled into the authored frame or live controls from being laid out
// against host gutters.
export function resolveMaterializedFrames(ir, coordinateSpace = null) {
  const root = ir?.root || {};
  const attributes = root.attributes || {};
  const style = root.style || {};

  const visual = {
    left: 0,
    top: 0,
    width: finitePositive(style.width),
    height: finitePositive(style.height),
  };
  if (visual.width === 0 || visual.height === 0) {
    throw new Error('DesignIR root is missing a finite positive visual frame');
  }

  const legacyBehavior = {
    left: finiteNumber(attributes.browser_authored_frame_x),
    top: finiteNumber(attributes.browser_authored_frame_y),
    width: finitePositive(attributes.browser_authored_frame_width) || visual.width,
    height: finitePositive(attributes.browser_authored_frame_height) || visual.height,
  };
  if (legacyBehavior.width === 0 || legacyBehavior.height === 0) {
    throw new Error('DesignIR root is missing a finite positive authored frame');
  }
  const authored = coordinateSpace?.authored_box;
  const matrix = coordinateSpace?.captured_transform;
  if (!authored || !matrix) return { visual, behavior: legacyBehavior };
  const behavior = {
    left: 0, top: 0,
    width: finitePositive(authored.width),
    height: finitePositive(authored.height),
    transform: {
      a: finiteNumber(matrix.a, Number.NaN),
      b: finiteNumber(matrix.b, Number.NaN),
      c: finiteNumber(matrix.c, Number.NaN),
      d: finiteNumber(matrix.d, Number.NaN),
      e: finiteNumber(matrix.e, Number.NaN),
      f: finiteNumber(matrix.f, Number.NaN),
    },
  };
  if (behavior.width === 0 || behavior.height === 0 ||
      !Object.values(behavior.transform).every(Number.isFinite) ||
      Math.abs(behavior.transform.a * behavior.transform.d -
        behavior.transform.b * behavior.transform.c) < 1e-9) {
    throw new Error('materialized authored coordinate space is invalid');
  }
  return { visual, behavior };
}
