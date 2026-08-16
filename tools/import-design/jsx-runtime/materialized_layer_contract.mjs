// Native paint-plane ordering for an executable Chromium materialization.
//
// The live behavior tree must remain above the captured paint authority so it
// owns hit testing. The state atlas must remain above the page background so
// an activated reference state cannot be hidden by an opaque body surface.
// Keep the values deliberately far apart: imported DesignIR nodes may carry
// authored z-index values of their own between the background and atlas.
export const MATERIALIZED_BACKGROUND_Z = -20000;
export const MATERIALIZED_STATE_ATLAS_Z = 10000;
export const MATERIALIZED_BEHAVIOR_Z = 20000;

export function validateMaterializedLayerContract() {
  if (!(MATERIALIZED_BACKGROUND_Z < MATERIALIZED_STATE_ATLAS_Z &&
        MATERIALIZED_STATE_ATLAS_Z < MATERIALIZED_BEHAVIOR_Z)) {
    throw new Error('materialized paint planes are not strictly ordered');
  }
  return true;
}
