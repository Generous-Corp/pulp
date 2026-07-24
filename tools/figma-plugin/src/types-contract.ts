// Compile-only assertions for invariants that must survive schema type
// generation. This file emits no runtime code.

import type { Node } from "./types.generated";

const nativeNode: Node = {
  type: "frame",
  name: "Native",
  figma_node_id: "1:2",
};

const syntheticNode: Node = {
  type: "frame",
  name: "<multi-export>",
  synthetic: true,
};

// @ts-expect-error Every node must have a native or synthetic identity.
const missingIdentity: Node = { type: "frame", name: "Missing" };

// @ts-expect-error Synthetic wrappers cannot claim a native Figma node id.
const conflictingIdentity: Node = {
  type: "frame",
  name: "Conflicting",
  synthetic: true,
  figma_node_id: "1:2",
};

void nativeNode;
void syntheticNode;
void missingIdentity;
void conflictingIdentity;
