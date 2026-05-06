// pulp #1486 — 5-scenario validation harness (§4 of the spec).
//
//   S1 Pure regen, no source change                  → tweaks survive
//   S2 Designer changed colors, structure preserved  → tweaks survive
//   S3 Designer added a new sibling section          → existing IDs preserved
//   S4 Designer deleted a tweaked section            → drift flags orphaned tweak
//   S5 Designer reordered sections                   → content-hash strategy survives
//
// Acceptance for the Phase 1 spike: 4 of 5 pass. (S5 with content-hash
// is expected to pass because the hash is depth+text-driven, not
// path-driven.)

import { describe, it, expect } from 'vitest';
import {
    lowerClaudeDesignHtml,
    applyTweaks,
    diff,
    emptyTweaksFile,
    type TweaksFile,
    type IRNode,
} from '../../src/index.js';

// Minimal HTML fixtures. Phase 1's adapter handles inline `style`
// attributes — these fixtures exercise the typed paint / text fields.
const HOMEPAGE_V1 = `
<div>
  <header style="background-color: #0066ff; padding: 16px;">
    <h1 style="color: white; font-size: 32px;">Welcome</h1>
  </header>
  <section style="padding: 24px;">
    <p style="font-size: 14px; color: #333;">Sign up to get started.</p>
    <button style="background-color: #0066ff; color: white; padding: 12px;">Subscribe</button>
  </section>
</div>
`;

// S2: designer changes the homepage's primary color from blue to green.
const HOMEPAGE_V2_COLOR_CHANGE = `
<div>
  <header style="background-color: #00cc66; padding: 16px;">
    <h1 style="color: white; font-size: 32px;">Welcome</h1>
  </header>
  <section style="padding: 24px;">
    <p style="font-size: 14px; color: #333;">Sign up to get started.</p>
    <button style="background-color: #00cc66; color: white; padding: 12px;">Subscribe</button>
  </section>
</div>
`;

// S3: designer adds a new "footer" section after the existing section.
const HOMEPAGE_V3_NEW_SECTION = `
<div>
  <header style="background-color: #0066ff; padding: 16px;">
    <h1 style="color: white; font-size: 32px;">Welcome</h1>
  </header>
  <section style="padding: 24px;">
    <p style="font-size: 14px; color: #333;">Sign up to get started.</p>
    <button style="background-color: #0066ff; color: white; padding: 12px;">Subscribe</button>
  </section>
  <footer style="padding: 8px; background-color: #f5f5f5;">
    <p style="font-size: 12px; color: #999;">© 2026 Pulp</p>
  </footer>
</div>
`;

// S4: designer deletes the tweaked section (the button's parent).
const HOMEPAGE_V4_DELETE = `
<div>
  <header style="background-color: #0066ff; padding: 16px;">
    <h1 style="color: white; font-size: 32px;">Welcome</h1>
  </header>
</div>
`;

// S5: designer reorders the header and section.
const HOMEPAGE_V5_REORDER = `
<div>
  <section style="padding: 24px;">
    <p style="font-size: 14px; color: #333;">Sign up to get started.</p>
    <button style="background-color: #0066ff; color: white; padding: 12px;">Subscribe</button>
  </section>
  <header style="background-color: #0066ff; padding: 16px;">
    <h1 style="color: white; font-size: 32px;">Welcome</h1>
  </header>
</div>
`;

// ── Helpers ──────────────────────────────────────────────────────────

function findByText(root: IRNode, text: string): IRNode | null {
    if (root.text?.text === text) return root;
    for (const c of root.children) {
        const f = findByText(c, text);
        if (f) return f;
    }
    return null;
}

function makeTweaks(forAnchor: string, override: Record<string, unknown>): TweaksFile {
    const base = emptyTweaksFile('0.78.1', '2026-05-05T17:03:22Z');
    base.tweaks[forAnchor] = override;
    base.meta.anchorStrategy = 'content-hash';
    return base;
}

// ── S1 ───────────────────────────────────────────────────────────────

describe('S1 — Pure regen, no source change', () => {
    it('tweaks survive trivially when re-importing the same source', async () => {
        const v1 = await lowerClaudeDesignHtml(HOMEPAGE_V1);
        const button = findByText(v1, 'Subscribe');
        expect(button).not.toBeNull();
        const tweaks = makeTweaks(button!.stable_anchor_id, {
            'paint.backgroundColor': '#ff00aa',
        });

        // Re-import identical source.
        const v2 = await lowerClaudeDesignHtml(HOMEPAGE_V1);
        const tweaked = applyTweaks(v2, tweaks);
        const tweakedButton = findByText(tweaked, 'Subscribe');
        expect(tweakedButton?.paint?.backgroundColor).toBe('#ff00aa');
        expect(tweaked.meta?.orphaned_tweaks).toBeUndefined();
    });
});

// ── S2 ───────────────────────────────────────────────────────────────

describe('S2 — Designer changed colors, dev tweak survives', () => {
    it("tweak survives when source colors change but structure doesn't", async () => {
        const v1 = await lowerClaudeDesignHtml(HOMEPAGE_V1);
        const button = findByText(v1, 'Subscribe');
        const buttonAnchor = button!.stable_anchor_id;
        // Dev tweaks the button's font-size locally.
        const tweaks = makeTweaks(buttonAnchor, { 'text.fontSize': 18 });

        // Designer changes primary color blue → green; structure
        // identical. content-hash anchors don't depend on color, so the
        // button's anchor stays stable.
        const v2 = await lowerClaudeDesignHtml(HOMEPAGE_V2_COLOR_CHANGE);
        const v2Button = findByText(v2, 'Subscribe');
        expect(v2Button?.stable_anchor_id).toBe(buttonAnchor);

        const tweaked = applyTweaks(v2, tweaks);
        const tweakedButton = findByText(tweaked, 'Subscribe');
        expect(tweakedButton?.text?.fontSize).toBe(18);
        // Designer's color change comes through.
        expect(tweakedButton?.paint?.backgroundColor).toBe('#00cc66');
        expect(tweaked.meta?.orphaned_tweaks).toBeUndefined();
    });
});

// ── S3 ───────────────────────────────────────────────────────────────

describe('S3 — Designer added a new sibling section', () => {
    it('existing IDs preserved; new section gets a new ID', async () => {
        const v1 = await lowerClaudeDesignHtml(HOMEPAGE_V1);
        const button = findByText(v1, 'Subscribe');
        const buttonAnchor = button!.stable_anchor_id;
        const tweaks = makeTweaks(buttonAnchor, { 'paint.opacity': 0.85 });

        const v3 = await lowerClaudeDesignHtml(HOMEPAGE_V3_NEW_SECTION);
        // Existing button anchor should still be present.
        const v3Button = findByText(v3, 'Subscribe');
        expect(v3Button?.stable_anchor_id).toBe(buttonAnchor);
        // New footer-paragraph has a different anchor.
        const newP = findByText(v3, '© 2026 Pulp');
        expect(newP?.stable_anchor_id).toBeDefined();
        expect(newP?.stable_anchor_id).not.toBe(buttonAnchor);

        const tweaked = applyTweaks(v3, tweaks);
        expect(findByText(tweaked, 'Subscribe')?.paint?.opacity).toBe(0.85);
        expect(tweaked.meta?.orphaned_tweaks).toBeUndefined();
    });
});

// ── S4 ───────────────────────────────────────────────────────────────

describe('S4 — Designer deleted a tweaked section', () => {
    it('drift report flags the orphaned tweak', async () => {
        const v1 = await lowerClaudeDesignHtml(HOMEPAGE_V1);
        const button = findByText(v1, 'Subscribe');
        const buttonAnchor = button!.stable_anchor_id;
        const tweaks = makeTweaks(buttonAnchor, { 'paint.backgroundColor': '#ff00aa' });

        const v4 = await lowerClaudeDesignHtml(HOMEPAGE_V4_DELETE);
        const v4Button = findByText(v4, 'Subscribe');
        expect(v4Button).toBeNull(); // section was deleted

        const tweaked = applyTweaks(v4, tweaks);
        expect(tweaked.meta?.orphaned_tweaks).toBeDefined();
        expect(tweaked.meta?.orphaned_tweaks?.[buttonAnchor]).toEqual({
            'paint.backgroundColor': '#ff00aa',
        });

        const drifts = diff(v1, tweaked);
        const orphan = drifts.find((d) => d.kind === 'orphaned-tweak');
        expect(orphan).toBeDefined();
        expect(orphan?.anchor).toBe(buttonAnchor);
    });
});

// ── S5 ───────────────────────────────────────────────────────────────

describe('S5 — Designer reordered sections', () => {
    it('content-hash strategy preserves anchors across reorder', async () => {
        const v1 = await lowerClaudeDesignHtml(HOMEPAGE_V1);
        const button = findByText(v1, 'Subscribe');
        const buttonAnchor = button!.stable_anchor_id;
        const tweaks = makeTweaks(buttonAnchor, { 'paint.backgroundColor': '#ff00aa' });

        // V5 swaps header and section's order — same content, same depths.
        const v5 = await lowerClaudeDesignHtml(HOMEPAGE_V5_REORDER);
        const v5Button = findByText(v5, 'Subscribe');
        // Content-hash anchor is depth+tag+role+text — depth-from-root
        // for the button is the same in both versions (root → section
        // → button = depth 2), so the anchor should be stable.
        expect(v5Button?.stable_anchor_id).toBe(buttonAnchor);

        const tweaked = applyTweaks(v5, tweaks);
        expect(findByText(tweaked, 'Subscribe')?.paint?.backgroundColor).toBe('#ff00aa');
    });
});

// ── Acceptance summary ───────────────────────────────────────────────

describe('Phase 1 acceptance — 4-of-5 scenarios pass', () => {
    it('all five scenarios run without throwing', async () => {
        // Smoke check the harness itself: every scenario completes.
        await lowerClaudeDesignHtml(HOMEPAGE_V1);
        await lowerClaudeDesignHtml(HOMEPAGE_V2_COLOR_CHANGE);
        await lowerClaudeDesignHtml(HOMEPAGE_V3_NEW_SECTION);
        await lowerClaudeDesignHtml(HOMEPAGE_V4_DELETE);
        await lowerClaudeDesignHtml(HOMEPAGE_V5_REORDER);
    });
});
