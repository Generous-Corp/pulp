// SPDX-License-Identifier: MIT

// The capture side and the runtime side must agree on one identity contract.
// v1 used positional arrays (`path` + `index`) as the only address.  They are
// useful for replay, but an insertion before a sibling changes every later
// address.  v2 keeps those arrays for the renderer and adds a deterministic
// id-addressed projection for import, diff, and re-import tooling.

export const MATERIALIZED_BINDING_KINDS = Object.freeze([
  'semantic', 'layout', 'text', 'paint', 'canvas',
]);

export const MATERIALIZED_BROWSER_DOCUMENT_V1 =
  'pulp-materialized-browser-document-v1';
export const MATERIALIZED_BROWSER_DOCUMENT_V2 =
  'pulp-materialized-browser-document-v2';

const MAX_BINDINGS_PER_KIND = 16384;
const MAX_BINDINGS_TOTAL = MAX_BINDINGS_PER_KIND * MATERIALIZED_BINDING_KINDS.length;
const STABLE_ID = /^[a-z][a-z0-9._:-]{1,127}$/;
const UINT64_MASK = 0xffffffffffffffffn;
const FNV_OFFSET = 0xcbf29ce484222325n;
const FNV_PRIME = 0x100000001b3n;

function hashIdentity(value) {
  const text = String(value);
  let hash = FNV_OFFSET;
  for (let index = 0; index < text.length; ++index) {
    hash ^= BigInt(text.charCodeAt(index));
    hash = (hash * FNV_PRIME) & UINT64_MASK;
  }
  return hash.toString(16).padStart(16, '0');
}

function canonicalPath(path) {
  if (!Array.isArray(path)) return '';
  return path.map((step) => `${String(step?.tag || '')}[${String(step?.index ?? '')}]`)
    .join('/');
}

function explicitIdentity(kind, binding) {
  // `data_pulp_id` is the capture spelling.  The aliases make the contract
  // usable by emitters that already carry the DOM attribute as `pulp_id` or
  // `stable_id`, without allowing an arbitrary object to influence identity.
  const explicit = binding?.data_pulp_id ?? binding?.pulp_id ?? binding?.stable_id;
  if (typeof explicit === 'string' && explicit.trim())
    return explicit.trim();

  const path = canonicalPath(binding?.path);
  if (kind === 'semantic') {
    return [binding?.anchor || '', binding?.backend_node_id || '',
      binding?.tag || '', binding?.name || ''].join('|');
  }
  if (kind === 'canvas') {
    return [binding?.anchor || '', binding?.backend_node_id || ''].join('|');
  }
  // A path is intentionally the fallback identity until capture can observe a
  // source-owned data-pulp-id.  The explicit id survives sibling insertion;
  // this fallback remains deterministic and is reported as positional by
  // callers that want to require source ids.
  return [binding?.anchor || '', path, binding?.tag || ''].join('|');
}

function normalizeId(kind, binding, mapKey = '') {
  const candidate = typeof binding?.id === 'string' && binding.id.length > 0
    ? binding.id : mapKey || explicitSourceId(binding);
  if (candidate && !STABLE_ID.test(candidate))
    throw new Error(`${kind} binding has invalid id`);
  const suffix = candidate || hashIdentity(explicitIdentity(kind, binding));
  // Prefixing the kind prevents accidental cross-list collisions and makes a
  // sidecar readable when it is inspected without the surrounding map.
  const id = suffix.startsWith(`pulp-${kind}-`)
    ? suffix : `pulp-${kind}-${suffix}`;
  if (!STABLE_ID.test(id)) throw new Error(`${kind} binding id is invalid`);
  return id;
}

function explicitSourceId(binding) {
  const explicit = binding?.data_pulp_id ?? binding?.pulp_id ?? binding?.stable_id;
  return typeof explicit === 'string' && explicit.trim() ? explicit.trim() : '';
}

function listFrom(document, kind) {
  const list = document?.[`${kind}_bindings`];
  if (list !== undefined && !Array.isArray(list))
    throw new Error(`${kind}_bindings must be an array`);
  if (Array.isArray(list)) return list;

  const map = document?.bindings_by_id?.[kind];
  if (map === undefined) return [];
  if (!map || typeof map !== 'object' || Array.isArray(map))
    throw new Error(`bindings_by_id.${kind} must be an object`);
  return Object.entries(map).map(([id, binding]) => {
    if (!binding || typeof binding !== 'object' || Array.isArray(binding))
      throw new Error(`${kind} binding ${id} is invalid`);
    return { ...binding, id: binding.id ?? id };
  });
}

function verifyMapMatchesList(document, kind, list) {
  const map = document?.bindings_by_id?.[kind];
  if (map === undefined) return;
  if (!map || typeof map !== 'object' || Array.isArray(map))
    throw new Error(`bindings_by_id.${kind} must be an object`);
  const expected = new Set(list.map((binding) => binding.id));
  const actual = Object.keys(map);
  if (actual.length !== expected.size || actual.some((id) => !expected.has(id)))
    throw new Error(`bindings_by_id.${kind} does not match ${kind}_bindings`);
  for (const [id, binding] of Object.entries(map)) {
    if (!binding || typeof binding !== 'object' || Array.isArray(binding) ||
        binding.id !== id) {
      throw new Error(`bindings_by_id.${kind}.${id} is invalid`);
    }
  }
}

/**
 * Normalize the five binding streams and produce their id-addressed v2 view.
 * The returned object is a shallow document copy; callers can safely retain
 * unrelated capture fields (assets, fonts, coordinate space, and so on).
 */
export function normalizeMaterializedBindingDocument(document, {
  upgradeSchema = false,
} = {}) {
  if (!document || typeof document !== 'object' || Array.isArray(document))
    throw new Error('materialized binding document is invalid');

  const used = new Set();
  const normalized = {};
  const byId = {};
  let total = 0;
  for (const kind of MATERIALIZED_BINDING_KINDS) {
    const source = listFrom(document, kind);
    if (source.length > MAX_BINDINGS_PER_KIND)
      throw new Error(`${kind}_bindings contains too many bindings`);
    const list = [];
    const map = {};
    for (const binding of source) {
      if (!binding || typeof binding !== 'object' || Array.isArray(binding))
        throw new Error(`${kind} binding is invalid`);
      const id = normalizeId(kind, binding);
      if (used.has(id)) throw new Error(`duplicate materialized binding id ${id}`);
      used.add(id);
      const value = { ...binding, id };
      list.push(value);
      map[id] = value;
      ++total;
    }
    verifyMapMatchesList(document, kind, list);
    normalized[`${kind}_bindings`] = list;
    byId[kind] = map;
  }
  if (total > MAX_BINDINGS_TOTAL)
    throw new Error('materialized document contains too many bindings');

  const result = {
    ...document,
    ...normalized,
    bindings_by_id: byId,
  };
  if (upgradeSchema) {
    result.schema = MATERIALIZED_BROWSER_DOCUMENT_V2;
    result.version = 2;
  }
  return result;
}

/** Return true for either document schema accepted by the importer. */
export function isMaterializedBrowserDocumentSchema(document) {
  return Boolean(document &&
    ((document.schema === MATERIALIZED_BROWSER_DOCUMENT_V1 &&
      document.version === 1) ||
     (document.schema === MATERIALIZED_BROWSER_DOCUMENT_V2 &&
      document.version === 2)));
}
