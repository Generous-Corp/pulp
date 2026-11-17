# G-3 saved-identity v7: persisted authored node IDs

Status: design for the first v7 slice. The code in this change implements it;
the items under "Not in this slice" are follow-ups.

## Problem

A saved-identity v6 document names each node by a structural key,
`node:<type>:<name>[:<unique id> | :<type id>@<version>]:<ordinal>`, with the
ordinal counted among nodes that share the key, in persisted node id order.
That identity is recomputed from mutable fields, so it cannot follow an entity
through a rename (the key changes) or through the deletion of a same-named
neighbour (the survivor's ordinal changes and it takes the deleted node's
identity). Diff, history `port_changes` and subgraph membership inherit the
misattribution. The `:` delimiter is also ambiguous when a name contains it.

## The field

Every `GraphNode` carries `authored_id`, an opaque string. It is distinct from
the runtime `NodeId` (a lookup key that every restore renumbers), from the
type/version identity, and from any content digest.

- Spelling written by this implementation: `nid-` followed by 32 lowercase hex
  digits. Readers treat the value as opaque and accept any string of 1 to 128
  characters from `[A-Za-z0-9._~-]`, so a later generator can change the
  spelling without a format change.
- Allocation happens on the control thread, inside the graph mutation lock, at
  node creation: every `add_*_node` path (including Processor, custom, plugin
  placeholder, trigger and spectral nodes, and the nodes a subgraph template
  instantiates). A fresh ID is a SHA-256 of a per-graph 128-bit random salt
  (drawn once per graph, lazily) and a per-graph counter, so two sessions that
  edit the same document never mint the same ID for different nodes.
- The ID is immutable: `GraphNode` is only reachable through const accessors,
  no API changes it, and `SignalGraph::rename_node` changes the name only
  (for a Processor node, the plugin identity name follows the node name,
  because a restore rebuilds it from the node name).
- `GraphSerializer::saved_node_identities` returns these IDs. History, diff,
  `qualified_port_identities` and subgraph persistence key on them.

## Copy semantics

- A node created by any authoring call gets a new ID, including a node
  configured identically to an existing one and every node of a second
  instance of the same subgraph template.
- Restoring a v7 document (load, undo, redo, trial restore) keeps the IDs it
  records: it is the same authored document.
- A node record pasted into a v7 document with an ID that already appears is
  refused as a duplicate; there is no rule that silently renames it.
- There is no in-graph "duplicate node" API in this slice (see below).

## Format

A v7 document is the plain graph payload plus the envelope, with:

- `format_version: 7`. Readers older than this change refuse format 7, so an
  old binary cannot load a v7 document and silently drop its IDs. Format 7 is
  written only with the v7 envelope, and a reader refuses format 7 without it.
  The plain writer (`to_json`) is unchanged and never writes format 7.
- `identity_schema` and `logical_identities.profile` set to
  `pulp.graph.saved-identity.v7`.
- Each node record carries `authored_id`; each `logical_identities.identities`
  entry is `{id: <authored_id>, node_id}` and `bindings` maps the same ID to
  the same persisted node id. `subgraphs[].members` name authored IDs, and
  a member's position in that list is its stable template member key: a v7
  restore checks each member's type, port shape and Processor descriptor
  against the template node at that position, never its display name, so a
  renamed member restores. v6 member identities derive from names, so a v6
  restore still compares names.
- The ordinary reader (`from_json`, `from_file`) runs the same inspection
  rules on an original format 7 document, so an incomplete or inconsistent
  v7 envelope is refused there too.

Inspection of a v7 document validates binding consistency: every record has a
well-formed `authored_id`; IDs are unique; every node has exactly one identity
entry and one binding; each identity entry and binding name the record whose
`authored_id` they spell. It does not rebuild identities from names (a v7 ID
carries no structure) and it does not attest historical authorship.

## Version policy

- v6 stays readable with its original validation, unchanged: structural
  identity rebuild, ordinal checks, the after-load structural check in
  `from_saved_document`. A v6 node record carrying `authored_id` is refused
  (no v6 writer emitted one, and honouring it would be a guess).
- The writer emits only v7. There is no v6 writer and no downgrade path, so a
  v7 document is never silently written back as v6.
- `diff_graph_documents` refuses to compare a v6 document with a v7 document
  (the identities live in different spaces); migrate the v6 side first.
- An unknown future format (8 or later) and an unknown identity schema are
  refused.

## Migration v6 to v7

`GraphSerializer::migrate_saved_document` is data-only: it inspects the v6
document with v6 rules, then writes the v7 document and returns the
old-binding to new-ID map (`v6 identity -> authored_id`).

- The new ID of a node is `nid-` plus the first 32 hex digits of
  SHA-256(`pulp.graph.authored-id.migration.v1`, digest of the canonical
  source document, the node's v6 identity). The same document therefore
  always migrates to the same IDs, byte for byte.
- Continuity is never guessed across documents: any difference in the source
  document changes the digest and so every migrated ID. Two independently
  edited copies of a legacy document come out with disjoint IDs, and a diff
  between them reports replaced nodes rather than a guessed match.
- Loading a v6 document (`from_saved_document`, history, the CLI rewrite
  commands) assigns exactly the IDs `migrate_saved_document` would. Loading a
  plain document with no envelope assigns IDs by the same rule, keyed by the
  persisted node id.
- Migration never writes a file. A file changes only when a caller saves.

## Not in this slice

- A `SignalGraph::duplicate_node` that copies plugin, Processor and custom
  state into a new node (it would get a new ID by the rule above).
- Spectral graphs: the spectral writer has its own closed envelope and still
  writes no IDs (spectral persistence follow-up).
- MCP diff, a CLI migrate command, and the installed-consumer refresh beyond
  the existing consumers.
