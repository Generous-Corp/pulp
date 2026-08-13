import { existsSync, readFileSync, realpathSync, statSync } from 'node:fs';
import { dirname, isAbsolute, relative, resolve, sep } from 'node:path';

const validId = /^[A-Za-z0-9._-]+$/;
const validEvent = /^[A-Za-z][A-Za-z0-9:_-]*$/;
const maxAtlasBytes = 1024 * 1024;
const maxStates = 64;
const maxIdLength = 128;
const maxSelectorLength = 4096;
const maxActivationSteps = 32;
const maxEventLength = 64;
const maxEventDataBytes = 64 * 1024;

function boundedSelector(value) {
  return typeof value === 'string' && value.length > 0 &&
    value.length <= maxSelectorLength;
}

export function loadMaterializedStateAtlas(
  atlasPath, { visualAuthority = 'reference' } = {}) {
  if (!atlasPath) return [];
  if (visualAuthority !== 'reference' && visualAuthority !== 'native') {
    throw new Error('visual authority must be reference or native');
  }

  const atlasBytes = readFileSync(atlasPath);
  if (atlasBytes.length > maxAtlasBytes) {
    throw new Error(`state atlas exceeds ${maxAtlasBytes} bytes`);
  }
  const atlas = JSON.parse(atlasBytes.toString('utf8'));
  if (atlas.schema !== 'pulp-materialized-state-atlas-v1' ||
      atlas.version !== 1 || !Array.isArray(atlas.states) ||
      atlas.states.length === 0 || atlas.states.length > maxStates) {
    throw new Error(
      `state atlas must contain 1-${maxStates} pulp-materialized-state-atlas-v1 states`);
  }

  const atlasRoot = realpathSync(dirname(atlasPath));
  const ids = new Set();
  return atlas.states.map((state, index) => {
    const id = String(state?.id || '');
    const image = visualAuthority === 'reference' ? String(state?.image || '') : '';
    if (id.length > maxIdLength || !validId.test(id) || ids.has(id)) {
      throw new Error(`state atlas entry ${index} has an invalid or duplicate id`);
    }
    ids.add(id);

    let imagePath = '';
    if (visualAuthority === 'reference') {
      const unresolvedImage = resolve(atlasRoot, image);
      if (image === '' || !existsSync(unresolvedImage)) {
        throw new Error(
          `state atlas entry ${id} image does not exist: ${unresolvedImage}`);
      }
      imagePath = realpathSync(unresolvedImage);
      const imageRelative = relative(atlasRoot, imagePath);
      if (imageRelative === '..' || imageRelative.startsWith(`..${sep}`) ||
          isAbsolute(imageRelative) || !statSync(imagePath).isFile()) {
        throw new Error(`state atlas entry ${id} image escapes the atlas directory`);
      }
    }

    const match = state?.match;
    if (match !== undefined && (typeof match !== 'object' || match === null ||
        !boundedSelector(match.selector) ||
        (match.ancestor !== undefined &&
         !boundedSelector(match.ancestor)))) {
      throw new Error(`state atlas entry ${id} has an invalid match contract`);
    }

    const activate = state?.activate;
    if (activate !== undefined &&
        (!Array.isArray(activate) || activate.length === 0 ||
         activate.length > maxActivationSteps)) {
      throw new Error(`state atlas entry ${id} has an invalid activation contract`);
    }
    const activation = (activate || []).map((step, stepIndex) => {
      if (typeof step !== 'object' || step === null ||
          !boundedSelector(step.selector) ||
          (step.event !== undefined &&
           (typeof step.event !== 'string' || step.event.length === 0 ||
            step.event.length > maxEventLength || !validEvent.test(step.event))) ||
          (step.data !== undefined &&
           (typeof step.data !== 'object' || step.data === null ||
            Array.isArray(step.data)))) {
        throw new Error(
          `state atlas entry ${id} activation step ${stepIndex} is invalid`);
      }
      if (step.data !== undefined &&
          Buffer.byteLength(JSON.stringify(step.data), 'utf8') > maxEventDataBytes) {
        throw new Error(
          `state atlas entry ${id} activation step ${stepIndex} data is too large`);
      }
      return {
        selector: step.selector,
        event: step.event || 'click',
        data: step.data || null,
      };
    });

    return {
      id,
      image: imagePath,
      match: match === undefined ? null : {
        selector: match.selector,
        ancestor: match.ancestor || '',
      },
      activate: activation,
    };
  });
}
