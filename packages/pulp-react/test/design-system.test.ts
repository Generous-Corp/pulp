// Tests for the Ink & Signal design-system catalog. Verifies the catalog is
// well-formed and that every jsxTag it names is a real exported @pulp/react
// intrinsic, so the metadata can't drift from the actual bindings.

import { describe, it, expect } from 'vitest';
import {
    inkSignalCatalog, findComponent, componentsByCategory, FIGMA_FILE_KEY,
    type DesignCategory,
} from '../src/design-system.js';
import * as intrinsics from '../src/intrinsics.js';

const CATEGORIES: DesignCategory[] = [
    'controls', 'inputs', 'indicators', 'navigation',
    'containers', 'overlays', 'audio', 'feedback',
];

describe('Ink & Signal design-system catalog', () => {
    it('is non-empty with unique names', () => {
        expect(inkSignalCatalog.length).toBeGreaterThanOrEqual(30);
        const names = new Set(inkSignalCatalog.map((c) => c.name));
        expect(names.size).toBe(inkSignalCatalog.length);
    });

    it('every entry is fully populated', () => {
        for (const c of inkSignalCatalog) {
            expect(c.name, c.name).toBeTruthy();
            expect(c.nativeClass.startsWith('pulp::view::'), c.name).toBe(true);
            expect(c.usage, c.name).toBeTruthy();
            expect(c.reskinTokens.length, c.name).toBeGreaterThan(0);
            expect(CATEGORIES).toContain(c.category);
        }
    });

    it('every jsxTag is a real exported @pulp/react intrinsic', () => {
        for (const c of inkSignalCatalog) {
            if (c.jsxTag === null) continue;
            expect(
                typeof (intrinsics as Record<string, unknown>)[c.jsxTag],
                `${c.name} → <${c.jsxTag}> intrinsic`,
            ).toBe('function');
        }
    });

    it('every category is represented', () => {
        for (const cat of CATEGORIES) {
            expect(componentsByCategory(cat).length, cat).toBeGreaterThan(0);
        }
    });

    it('findComponent resolves by name and misses cleanly', () => {
        const knob = findComponent('Knob');
        expect(knob?.category).toBe('controls');
        expect(knob?.jsxTag).toBe('Knob');
        expect(findComponent('Nope')).toBeUndefined();
    });

    it('points at the Ink & Signal Figma library', () => {
        expect(FIGMA_FILE_KEY).toBe('q9iDYZzg86YrOQKr6I3bY0');
    });

    it('Badge, Stepper, and Pan are JS-bound', () => {
        for (const name of ['Badge', 'Stepper', 'Pan']) {
            const c = findComponent(name);
            expect(c, name).toBeDefined();
            expect(c?.jsxTag, name).toBe(name);
        }
    });
});
