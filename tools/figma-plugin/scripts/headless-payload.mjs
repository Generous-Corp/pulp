export function buildHeadlessPayload(bundle, targetNodeId, faithfulVector) {
  const targetAssignment = targetNodeId === undefined
    ? "globalThis.__pulp_target_node_id = void 0;"
    : `globalThis.__pulp_target_node_id = ${JSON.stringify(targetNodeId)};`;

  return [
    bundle.replace(/\s+$/, ""),
    targetAssignment,
    `globalThis.__pulp_faithful_vector = ${faithfulVector ? "true" : "false"};`,
    "try {",
    "  eval(globalThis.__pulp_packed_src);",
    "  return await globalThis.__pulp_headless_result;",
    "} finally {",
    "  globalThis.__pulp_packed_src = void 0;",
    "  globalThis.__pulp_headless_result = void 0;",
    "  globalThis.__pulp_target_node_id = void 0;",
    "  globalThis.__pulp_faithful_vector = void 0;",
    "}",
  ].join("\n") + "\n";
}
