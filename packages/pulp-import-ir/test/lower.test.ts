// lowerClaudeDesignHtml must track consumed outer-tag length independently
// of whether `rawHtml` is retained on the BuildNode. In non-preserve mode
// `makeNode` stores `rawHtml: ''`, so parser progress cannot depend on
// `node.rawHtml.length`.

import { describe, it, expect } from 'vitest';
import { lowerClaudeDesignHtml } from '../src/adapters/claude-design-html/lower.js';

describe('lowerClaudeDesignHtml — preserveRawSource: false', () => {
    it('terminates on a multi-child fragment without preserving raw source', async () => {
        // This shape can hang if parser progress is tied to retained rawHtml.
        // If it regresses, vitest's per-test timeout fails the suite hard
        // rather than hanging indefinitely.
        const html = `<div><span>one</span><span>two</span></div>`;
        const ir = await Promise.race([
            lowerClaudeDesignHtml(html, { preserveRawSource: false }),
            new Promise((_, reject) =>
                setTimeout(() => reject(new Error('parse timed out')), 3000),
            ),
        ]);
        expect(ir).toBeDefined();
        expect((ir as { tag: string }).tag).toBe('View');
        expect((ir as { children: unknown[] }).children.length).toBe(2);
    }, 5000);

    it('terminates on a multi-root fragment when raw source is dropped', async () => {
        // Multi-root path: the synthetic-wrapper-View case forces
        // parseFragment to drive the loop directly. We use different
        // opening and closing tags so matchSingleOuterTag does NOT collapse
        // the input into a single root.
        const html = `<header>a</header><section>b</section><footer>c</footer>`;
        const ir = await Promise.race([
            lowerClaudeDesignHtml(html, { preserveRawSource: false }),
            new Promise((_, reject) =>
                setTimeout(() => reject(new Error('parse timed out')), 3000),
            ),
        ]);
        expect(ir).toBeDefined();
        const root = ir as {
            tag: string;
            children: { raw_source: { outerHtml: string } }[];
            raw_source: { outerHtml: string };
        };
        expect(root.tag).toBe('View');
        expect(root.children.length).toBe(3);
        // Each parsed child has its raw_source.outerHtml dropped — the
        // synthetic-wrapper View itself is constructed outside the
        // makeNode path that honors `preserveRawSource`, so its own
        // raw_source isn't blanked here. We assert per-child blanking
        // — that's the surface parseFragment + makeNode controls.
        for (const child of root.children) {
            expect(child.raw_source.outerHtml).toBe('');
        }
    }, 5000);

    it('produces equivalent structure with and without preserveRawSource', async () => {
        const html = `<section><h1>Title</h1><p>Body</p></section>`;
        const withRaw = await lowerClaudeDesignHtml(html, { preserveRawSource: true });
        const withoutRaw = await lowerClaudeDesignHtml(html, { preserveRawSource: false });
        // Same shape; only raw_source.outerHtml differs.
        expect(withoutRaw.tag).toBe(withRaw.tag);
        expect(withoutRaw.children.length).toBe(withRaw.children.length);
        expect(withRaw.raw_source.outerHtml.length).toBeGreaterThan(0);
        expect(withoutRaw.raw_source.outerHtml).toBe('');
    }, 5000);
});
