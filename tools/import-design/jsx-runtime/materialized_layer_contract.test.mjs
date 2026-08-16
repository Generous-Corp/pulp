import test from 'node:test';
import assert from 'node:assert/strict';

import {
  MATERIALIZED_BACKGROUND_Z,
  MATERIALIZED_STATE_ATLAS_Z,
  MATERIALIZED_BEHAVIOR_Z,
  validateMaterializedLayerContract,
} from './materialized_layer_contract.mjs';

test('materialized background cannot obscure captured state paint', () => {
  assert.equal(validateMaterializedLayerContract(), true);
  assert.ok(MATERIALIZED_BACKGROUND_Z < MATERIALIZED_STATE_ATLAS_Z);
});

test('transparent behavior remains the top interaction plane', () => {
  assert.ok(MATERIALIZED_STATE_ATLAS_Z < MATERIALIZED_BEHAVIOR_Z);
});
