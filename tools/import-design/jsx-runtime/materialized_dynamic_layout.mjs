// A captured state describes one DOM shape. Added or removed children invalidate
// its positional geometry, including the parent's captured intrinsic size.
export function materializedDynamicLayoutScope(scope, bindings, values, pathIndex,
                                               nodeAtPath, elementChildren, nodeTag) {
  if (!scope) return new Set();
  const rootBinding = bindings.find(binding =>
    nodeAtPath(binding, values, pathIndex) === scope);
  if (!rootBinding) return new Set();
  const prefix = rootBinding.path;
  const inside = path => path.length >= prefix.length && prefix.every((step, i) =>
    step.index === path[i].index && step.tag === path[i].tag);
  const expected = new Map();
  for (const binding of bindings) {
    if (!inside(binding.path)) continue;
    const relative = binding.path.slice(prefix.length);
    for (let depth = 0; depth <= relative.length; ++depth) {
      const key = JSON.stringify(relative.slice(0, depth));
      if (!expected.has(key)) expected.set(key, new Map());
      if (depth < relative.length) {
        const step = relative[depth];
        expected.get(key).set(step.index, step.tag);
      }
    }
  }
  const subtree = new Set();
  let changed = false;
  function visit(node, path, paintOnly = false) {
    subtree.add(node);
    const children = elementChildren(node, pathIndex.registrySet);
    // SVG primitives carry paint geometry rather than layout bindings.
    if (!paintOnly && nodeTag(node) !== 'svg') {
      const captured = expected.get(JSON.stringify(path));
      if (!captured || captured.size !== children.length) changed = true;
      children.forEach((child, index) => {
        const tag = nodeTag(child);
        if (captured?.get(index) !== tag) changed = true;
        visit(child, [...path, { tag, index }]);
      });
    } else {
      children.forEach(child => visit(child, [], true));
    }
  }
  visit(scope, []);
  return changed ? subtree : new Set();
}

export function restoreMaterializedLayout(node, bridge) {
  const id = String(node.__pulpId || node.id || '');
  if (!id) return;
  const authored = node.__pulpAuthoredLayout__ || {};
  const style = node.style || {};
  const value = (key, fallback) => {
    const live = style[key];
    return live !== undefined && live !== '' ? live : authored[key] ?? fallback;
  };
  bridge.setPosition(id, value('position', 'relative'));
  bridge.setLeft(id, value('left', 'auto'));
  bridge.setTop(id, value('top', 'auto'));
  bridge.setFlex(id, 'width', value('width', 'auto'));
  bridge.setFlex(id, 'height', value('height', 'auto'));
  if (typeof bridge.clearCapturedLineBoxes === 'function') {
    bridge.clearCapturedLineBoxes(String(node.__pulpTextTargetId || id));
    for (const target of node.__pulpAnonymousTextTargets || [])
      bridge.clearCapturedLineBoxes(String(target.id));
  }
}
