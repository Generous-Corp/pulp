// SPDX-License-Identifier: MIT

// Persistent source identity for materialized imports.
//
// The browser capture's structural path is useful for replay, but it is not a
// source identity: inserting a sibling changes every later ordinal.  This
// sidecar is the small, deterministic bridge between an owned source tree and
// those capture paths.  A source record is identified by
// (component, local_path, content_hash), while the assigned id survives a
// path or content edit when the remaining evidence identifies one prior row.
// Historical keys are retained as aliases so a removed component can return
// without silently receiving a recycled id.

import { createHash } from 'node:crypto';

export const MATERIALIZED_IDS_MAP_SCHEMA = 'pulp-materialized-ids-map-v1';
export const MATERIALIZED_IDS_MAP_VERSION = 1;

const MAX_ENTRIES = 65536;
const MAX_ALIASES_PER_ENTRY = 64;
const SOURCE_ID = /^[a-z][a-z0-9._:-]{1,127}$/;
const CONTENT_HASH = /^[a-f0-9]{64}$/;
const COMPONENT_SEGMENT = /^[A-Za-z][A-Za-z0-9_.:@+-]{0,127}$/;
const PATH_SEGMENT = /^[A-Za-z0-9_.:@#()[\],+~-]{1,256}$/;

const emptyMap = () => ({
  schema: MATERIALIZED_IDS_MAP_SCHEMA,
  version: MATERIALIZED_IDS_MAP_VERSION,
  entries: [],
});

function invalid(label, detail) {
  throw new Error(`ids.map.json ${label} ${detail}`);
}

function stringField(value, label, { max = 512 } = {}) {
  if (typeof value !== 'string' || value.length === 0 || value.length > max)
    invalid(label, 'is invalid');
  return value;
}

function normalizeComponent(value, label = 'component') {
  const component = stringField(value, label, { max: 512 });
  const segments = component.split('/');
  if (segments.some(segment => !COMPONENT_SEGMENT.test(segment)))
    invalid(label, 'is invalid');
  return segments.join('/');
}

function normalizeLocalPath(value, label = 'local_path') {
  const localPath = stringField(value, label, { max: 1024 });
  if (localPath.startsWith('/') || localPath.includes('\\'))
    invalid(label, 'must be a relative slash-separated path');
  const segments = localPath.split('/');
  if (segments.some(segment => segment === '.' || segment === '..' ||
      !PATH_SEGMENT.test(segment))) {
    invalid(label, 'must be a safe relative path');
  }
  return segments.join('/');
}

function normalizeContentHash(value, label = 'content_hash') {
  const hash = stringField(value, label, { max: 64 });
  if (!CONTENT_HASH.test(hash)) invalid(label, 'must be lowercase SHA-256');
  return hash;
}

function normalizeSourceId(value, label = 'id') {
  const id = stringField(value, label, { max: 128 });
  if (!SOURCE_ID.test(id)) invalid(label, 'is invalid');
  return id;
}

function sourceRecord(value, label = 'entry') {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    invalid(label, 'is invalid');
  return {
    component: normalizeComponent(value.component ?? value.component_name ??
      value.source_component_name, `${label}.component`),
    local_path: normalizeLocalPath(value.local_path ?? value.localPath ??
      value.source_local_path, `${label}.local_path`),
    content_hash: normalizeContentHash(value.content_hash ??
      value.contentHash, `${label}.content_hash`),
  };
}

/** Return the canonical, collision-safe key for a source record. */
export function materializedSourceKey(value, label = 'source') {
  return JSON.stringify(sourceRecord(value, label));
}

function sameSource(a, b) {
  return a.component === b.component && a.local_path === b.local_path &&
    a.content_hash === b.content_hash;
}

function sourceKeyFromNormalized(record) {
  return JSON.stringify(record);
}

function sortRecords(a, b) {
  const left = sourceKeyFromNormalized(a);
  const right = sourceKeyFromNormalized(b);
  return left < right ? -1 : left > right ? 1 : 0;
}

function cloneRecord(record) {
  return {
    component: record.component,
    local_path: record.local_path,
    content_hash: record.content_hash,
  };
}

function normalizeAliases(value, label, current) {
  if (value === undefined) return [];
  if (!Array.isArray(value) || value.length > MAX_ALIASES_PER_ENTRY)
    invalid(`${label}.aliases`, 'is invalid');
  const aliases = value.map((alias, index) => sourceRecord(
    alias, `${label}.aliases[${index}]`));
  const seen = new Set();
  for (const alias of aliases) {
    const key = sourceKeyFromNormalized(alias);
    if (sameSource(alias, current) || !seen.add(key))
      invalid(`${label}.aliases`, 'contains a duplicate current or historical key');
  }
  return aliases.sort(sortRecords);
}

/**
 * Validate and canonicalize an ids.map.json object.
 *
 * Entries and aliases are sorted and every source key and id is unique.  The
 * returned object contains only the versioned contract fields, so serializing
 * it is byte deterministic even when a caller supplied a different property
 * order or input order.
 */
export function normalizeMaterializedIdsMap(value) {
  if (value === undefined || value === null) return emptyMap();
  if (!value || typeof value !== 'object' || Array.isArray(value))
    invalid('document', 'is invalid');
  if (value.schema !== MATERIALIZED_IDS_MAP_SCHEMA ||
      value.version !== MATERIALIZED_IDS_MAP_VERSION)
    invalid('document', 'has an unsupported schema or version');
  if (!Array.isArray(value.entries) || value.entries.length > MAX_ENTRIES)
    invalid('entries', 'is invalid');

  const ids = new Set();
  const keys = new Set();
  const entries = value.entries.map((raw, index) => {
    if (!raw || typeof raw !== 'object' || Array.isArray(raw))
      invalid(`entries[${index}]`, 'is invalid');
    const current = sourceRecord(raw, `entries[${index}]`);
    const id = normalizeSourceId(raw.id, `entries[${index}].id`);
    if (ids.has(id)) invalid(`entries[${index}].id`, 'is duplicated');
    ids.add(id);
    const aliases = normalizeAliases(raw.aliases,
      `entries[${index}]`, current);
    const all = [current, ...aliases];
    for (const source of all) {
      const key = sourceKeyFromNormalized(source);
      if (keys.has(key)) invalid(`entries[${index}]`,
        'reuses a source key already assigned to another id');
      keys.add(key);
    }
    return { ...current, id, ...(aliases.length ? { aliases } : {}) };
  });

  entries.sort((a, b) => sortRecords(a, b) || a.id.localeCompare(b.id));
  return {
    schema: MATERIALIZED_IDS_MAP_SCHEMA,
    version: MATERIALIZED_IDS_MAP_VERSION,
    entries,
  };
}

/** Serialize a canonical map with a trailing newline for checked-in sidecars. */
export function serializeMaterializedIdsMap(value) {
  return `${JSON.stringify(normalizeMaterializedIdsMap(value), null, 2)}\n`;
}

function explicitSourceId(value) {
  const source = value?.source && typeof value.source === 'object'
    ? value.source : value;
  const candidate = source?.data_pulp_id ?? source?.pulp_id ??
    source?.source_id ?? source?.stable_id;
  return candidate === undefined || candidate === null || candidate === ''
    ? '' : normalizeSourceId(candidate, 'source id');
}

/**
 * Extract the three source-owned fields from a binding or source row.
 *
 * Structural capture fields (`path`, `anchor`, `backend_node_id`) are
 * intentionally ignored.  Callers must provide an owned local path and a
 * content hash before this sidecar can assign an imported identity.
 */
export function materializedSourceRecord(value, label = 'source') {
  const source = value?.source && typeof value.source === 'object'
    ? { ...value, ...value.source } : value;
  if (!source || typeof source !== 'object' || Array.isArray(source))
    return null;
  const component = source.component ?? source.component_name ??
    source.source_component_name;
  const localPath = source.local_path ?? source.localPath ??
    source.source_local_path;
  const contentHash = source.content_hash ?? source.contentHash;
  // Structural capture rows may carry only part of the source metadata. They
  // remain positional until a later normalization stage can supply the full
  // identity tuple; do not turn an incomplete row into a hard import failure.
  if ([component, localPath, contentHash].some(field =>
    field === undefined || field === null || field === ''))
    return null;
  return sourceRecord(source, label);
}

function indexMap(map) {
  const byKey = new Map();
  const byId = new Map();
  const currentByComponentHash = new Map();
  const currentByComponentPath = new Map();
  for (const entry of map.entries) {
    const current = sourceRecord(entry);
    const key = sourceKeyFromNormalized(current);
    byKey.set(key, entry);
    byId.set(entry.id, entry);
    for (const alias of entry.aliases ?? [])
      byKey.set(sourceKeyFromNormalized(alias), entry);
    const hashKey = `${current.component}\0${current.content_hash}`;
    const pathKey = `${current.component}\0${current.local_path}`;
    if (!currentByComponentHash.has(hashKey)) currentByComponentHash.set(hashKey, []);
    if (!currentByComponentPath.has(pathKey)) currentByComponentPath.set(pathKey, []);
    currentByComponentHash.get(hashKey).push(entry);
    currentByComponentPath.get(pathKey).push(entry);
  }
  return { byKey, byId, currentByComponentHash, currentByComponentPath };
}

function addAlias(entry, prior) {
  if (sameSource(entry, prior)) return;
  const aliases = entry.aliases ?? [];
  if (!aliases.some(alias => sameSource(alias, prior))) aliases.push(cloneRecord(prior));
  entry.aliases = aliases.sort(sortRecords);
}

function mintId(record) {
  // The map is an authored identity boundary. Use a cryptographic digest for
  // minted ids so an adversarial or merely unlucky pair of source keys cannot
  // collide through the capture contract's shorter runtime hash.
  const digest = createHash('sha256')
    .update(`pulp-source-id\0${sourceKeyFromNormalized(record)}`, 'utf8')
    .digest('hex');
  return `pulp-source-${digest}`;
}

function uniqueCandidate(candidates, label) {
  const ids = [...new Set(candidates.map(entry => entry.id))];
  if (ids.length > 1) invalid(label, 'matches multiple prior ids');
  return ids.length === 1 ? ids[0] : '';
}

/**
 * Assign source ids and update a prior map.
 *
 * Exact keys win.  When an exact key is absent, one content match or one
 * component-local-path match may carry an id through a source edit.  Multiple
 * candidates are rejected rather than guessed, which is the critical
 * fail-closed behavior for repeated components and same-signature siblings.
 */
export function assignMaterializedSourceIds(sources, previous = undefined) {
  if (!Array.isArray(sources)) invalid('sources', 'must be an array');
  const map = normalizeMaterializedIdsMap(previous);
  const entries = map.entries.map(entry => ({
    ...cloneRecord(entry), id: entry.id,
    ...(entry.aliases?.length ? { aliases: entry.aliases.map(cloneRecord) } : {}),
  }));
  // `indexMap` must point at these mutable entry objects.  Re-normalizing here
  // would clone them a second time, so path/content matches would update an
  // object that is no longer present in the returned `entries` array.
  const working = { ...map, entries };
  const index = indexMap(working);
  const rows = sources.map((value, index) => ({
    index,
    record: materializedSourceRecord(value, `sources[${index}]`) ||
      sourceRecord(value, `sources[${index}]`),
    explicit: explicitSourceId(value),
  }));

  const unique = new Map();
  for (const row of rows) {
    const key = sourceKeyFromNormalized(row.record);
    const prior = unique.get(key);
    if (prior && prior.explicit && row.explicit && prior.explicit !== row.explicit)
      invalid(`sources[${row.index}]`, 'has a conflicting explicit id');
    if (prior) {
      if (row.explicit && !prior.explicit) prior.explicit = row.explicit;
    } else unique.set(key, row);
  }

  const assigned = new Map();
  const status = new Map();
  const ordered = [...unique.values()].sort((a, b) => sortRecords(a.record, b.record));
  for (const row of ordered) {
    const key = sourceKeyFromNormalized(row.record);
    const exact = index.byKey.get(key);
    let id = exact?.id || '';
    let disposition = exact ? 'exact' : '';
    if (row.explicit) {
      if (id && id !== row.explicit)
        invalid(`sources[${row.index}]`, 'explicit id disagrees with ids.map.json');
      id = row.explicit;
      disposition = exact ? 'exact-explicit' : 'explicit';
    } else if (!id) {
      const hashCandidates = index.currentByComponentHash.get(
        `${row.record.component}\0${row.record.content_hash}`) ?? [];
      const pathCandidates = index.currentByComponentPath.get(
        `${row.record.component}\0${row.record.local_path}`) ?? [];
      const hashId = uniqueCandidate(hashCandidates, `sources[${row.index}]`);
      const pathId = uniqueCandidate(pathCandidates, `sources[${row.index}]`);
      if (hashId && pathId && hashId !== pathId)
        invalid(`sources[${row.index}]`, 'has conflicting content and path matches');
      id = hashId || pathId;
      if (!id) id = mintId(row.record);
      disposition = hashId ? 'content-match' : pathId ? 'path-match' : 'minted';
    }
    const owner = index.byId.get(id);
    if (owner && owner.id === id && !sameSource(owner, row.record) &&
        !owner.aliases?.some(alias => sameSource(alias, row.record))) {
      // A content/path match intentionally updates the one owner.  An
      // explicit id cannot steal an unrelated row.
      if (row.explicit || disposition === 'minted')
        invalid(`sources[${row.index}]`, 'reuses an id owned by another source');
    }
    const already = assigned.get(id);
    if (already && already !== key)
      invalid(`sources[${row.index}]`, 'would assign one id to multiple source keys');
    assigned.set(id, key);
    status.set(key, { id, disposition });

    if (!owner) {
      const created = { ...cloneRecord(row.record), id };
      entries.push(created);
      index.byId.set(id, created);
      index.byKey.set(key, created);
      index.currentByComponentHash.set(
        `${row.record.component}\0${row.record.content_hash}`,
        [...(index.currentByComponentHash.get(
          `${row.record.component}\0${row.record.content_hash}`) ?? []), created]);
      index.currentByComponentPath.set(
        `${row.record.component}\0${row.record.local_path}`,
        [...(index.currentByComponentPath.get(
          `${row.record.component}\0${row.record.local_path}`) ?? []), created]);
    } else if (!sameSource(owner, row.record)) {
      const prior = {
        component: owner.component,
        local_path: owner.local_path,
        content_hash: owner.content_hash,
      };
      // A historical key can become current again after a source edit is
      // reverted. Remove the promoted key from aliases before retaining the
      // displaced current record; otherwise canonical validation sees the new
      // current key duplicated as both current and historical.
      owner.aliases = (owner.aliases ?? []).filter(alias =>
        !sameSource(alias, row.record) && !sameSource(alias, prior));
      owner.component = row.record.component;
      owner.local_path = row.record.local_path;
      owner.content_hash = row.record.content_hash;
      addAlias(owner, prior);
      index.byKey.set(key, owner);
    }
  }

  const normalized = normalizeMaterializedIdsMap({
    schema: MATERIALIZED_IDS_MAP_SCHEMA,
    version: MATERIALIZED_IDS_MAP_VERSION,
    entries,
  });
  return {
    map: normalized,
    assignments: rows.map(row => {
      const key = sourceKeyFromNormalized(row.record);
      const result = status.get(key);
      return { index: row.index, key, id: result.id, disposition: result.disposition };
    }),
  };
}

/**
 * Apply source ids to binding rows before schema-v2 normalization.
 * Bindings without complete source metadata are left untouched; callers can
 * then report them as positional rather than silently inventing lineage.
 */
export function applyMaterializedIdsMap(document, previous = undefined) {
  if (!document || typeof document !== 'object' || Array.isArray(document))
    invalid('document', 'is invalid');
  const kinds = ['semantic', 'layout', 'text', 'paint', 'canvas'];
  const rows = [];
  const locations = [];
  for (const kind of kinds) {
    const list = document[`${kind}_bindings`];
    if (list === undefined) continue;
    if (!Array.isArray(list)) invalid(`${kind}_bindings`, 'must be an array');
    list.forEach((binding, index) => {
      if (!materializedSourceRecord(binding)) return;
      rows.push(binding);
      locations.push({ kind, index });
    });
  }
  if (rows.length === 0) {
    return { document: { ...document }, idsMap: normalizeMaterializedIdsMap(previous),
      assignments: [] };
  }
  const resolved = assignMaterializedSourceIds(rows, previous);
  const output = { ...document };
  for (const kind of kinds) {
    if (Array.isArray(document[`${kind}_bindings`]))
      output[`${kind}_bindings`] = document[`${kind}_bindings`].map(binding => ({ ...binding }));
  }
  for (const location of locations) {
    const assignment = resolved.assignments.find(item => item.index ===
      locations.indexOf(location));
    if (!assignment) continue;
    const binding = output[`${location.kind}_bindings`][location.index];
    delete binding.id;
    binding.data_pulp_id = assignment.id;
  }
  return { document: output, idsMap: resolved.map, assignments: resolved.assignments };
}
