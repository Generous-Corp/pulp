// Smoke test for @pulp/react — confirms the package builds, imports cleanly,
// and exposes its primary public surface. Behavioural tests (host-config
// reconciler dispatch, prop-applier coverage) live in dedicated suites.

import { describe, it, expect } from 'vitest';
import * as pulpReact from '../src/index.js';

describe('@pulp/react smoke', () => {
  it('module is defined and exports something', () => {
    expect(pulpReact).toBeDefined();
    expect(Object.keys(pulpReact).length).toBeGreaterThan(0);
  });

  it('exposes the root + render entry points', () => {
    expect(typeof pulpReact.createRoot).toBe('function');
    expect(typeof pulpReact.render).toBe('function');
    expect(typeof pulpReact.unmount).toBe('function');
  });

  it('exposes the core intrinsic components', () => {
    // Spot-check a representative slice of the intrinsic surface that the
    // bridge contract is built around. If any of these disappear, downstream
    // plugin code (Spectr et al.) breaks.
    expect(typeof pulpReact.View).toBe('function');
    expect(typeof pulpReact.Row).toBe('function');
    expect(typeof pulpReact.Col).toBe('function');
    expect(typeof pulpReact.Panel).toBe('function');
    expect(typeof pulpReact.Label).toBe('function');
    expect(typeof pulpReact.Button).toBe('function');
    expect(typeof pulpReact.Spectrum).toBe('function');
    expect(typeof pulpReact.Knob).toBe('function');
  });

  it('exposes createMockBridge for downstream tests', () => {
    expect(typeof pulpReact.createMockBridge).toBe('function');
    const bridge = pulpReact.createMockBridge();
    expect(bridge).toBeDefined();
    expect(Array.isArray(bridge.calls)).toBe(true);
    expect(typeof bridge.install).toBe('function');
    expect(typeof bridge.uninstall).toBe('function');
    expect(typeof bridge.reset).toBe('function');
  });

  it('createRoot returns a PulpContainer with the expected shape', () => {
    const root = pulpReact.createRoot('test-root');
    expect(root).toBeDefined();
    expect(root.rootId).toBe('test-root');
    expect(typeof root.nextId).toBe('number');
  });
});
