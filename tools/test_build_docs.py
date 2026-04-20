#!/usr/bin/env python3
"""Tests for tools/build-docs.py inline markdown conversion.

Regression-tests the renderer for markdown patterns that previously
broke: backtick-wrapped link text ([`foo`](url)) shattered under the
old code-span-first pipeline. These tests pin the ordering contract
so future refactors can't re-introduce the same bug.
"""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location(
    "build_docs",
    _HERE / "build-docs.py",
)
assert _SPEC and _SPEC.loader
bd = importlib.util.module_from_spec(_SPEC)
sys.modules["build_docs"] = bd
_SPEC.loader.exec_module(bd)


def render(md: str) -> str:
    """Convert a single-line markdown string to HTML and return it trimmed."""
    html_out = bd.md_to_html(md)
    # markdown_to_html wraps in <p>...</p>; strip for easier assertions.
    return html_out.strip()


class CodeBlockTests(unittest.TestCase):
    def test_fenced_block_has_no_leading_newline(self):
        # Regression: the renderer used to emit <pre><code> and the first
        # code line as separate list entries, then '\n'.join() injected a
        # newline between them. <pre> preserves whitespace, so the user saw
        # a phantom blank line at the top of every code block.
        md = "```bash\ngit clone foo\ncd foo\n```"
        html_out = bd.md_to_html(md)
        # Must NOT start the code body with a newline
        self.assertNotIn("<code class=\"language-bash\">\n", html_out)
        self.assertNotIn("<code>\n", html_out)
        # Must contain the code text directly after the opening tag
        self.assertIn(
            '<code class="language-bash">git clone foo\ncd foo</code>',
            html_out,
        )

    def test_fenced_block_wrapped_in_figure_with_copy_button(self):
        md = "```python\nprint('hi')\n```"
        html_out = bd.md_to_html(md)
        self.assertIn('<figure class="code-block">', html_out)
        self.assertIn('class="copy-btn"', html_out)
        self.assertIn('aria-label="Copy code to clipboard"', html_out)
        self.assertIn('data-copy-label="Copy"', html_out)
        self.assertIn('data-copied-label="Copied"', html_out)

    def test_fenced_block_without_language_omits_language_label(self):
        md = "```\nfoo\n```"
        html_out = bd.md_to_html(md)
        self.assertNotIn('class="code-lang"', html_out)
        # But still wraps in figure + has copy button
        self.assertIn('<figure class="code-block">', html_out)
        self.assertIn('class="copy-btn"', html_out)

    def test_fenced_block_with_language_includes_language_label(self):
        md = "```bash\necho hi\n```"
        html_out = bd.md_to_html(md)
        self.assertIn('<span class="code-lang" aria-hidden="true">bash</span>', html_out)


class LinkRenderingTests(unittest.TestCase):
    def test_plain_link_renders(self):
        html = render("See [LICENSE.md](https://example.com/LICENSE.md) for the full text.")
        self.assertIn('<a href="https://example.com/LICENSE.md">LICENSE.md</a>', html)

    def test_backtick_wrapped_link_text_renders_as_code_inside_link(self):
        # Regression: [`DEPENDENCIES.md`](url) used to shatter into
        # literal `[`, `<code>DEPENDENCIES.md</code>`, `](url)`.
        html = render(
            "Entries here must stay in sync with "
            "[`DEPENDENCIES.md`](https://github.com/danielraffel/pulp/blob/main/DEPENDENCIES.md)."
        )
        # The link must survive as a single <a> tag with <code> inside.
        self.assertIn(
            '<a href="https://github.com/danielraffel/pulp/blob/main/DEPENDENCIES.md">'
            '<code>DEPENDENCIES.md</code></a>',
            html,
        )
        # And there must NOT be a stray literal '](url)' or dangling bracket.
        self.assertNotIn("](https://github.com", html)
        self.assertNotIn("](url)", html)

    def test_multiple_backtick_links_in_one_line(self):
        html = render(
            "See [`A.md`](http://a) and [`B.md`](http://b) and [`C.md`](http://c)."
        )
        self.assertIn('<a href="http://a"><code>A.md</code></a>', html)
        self.assertIn('<a href="http://b"><code>B.md</code></a>', html)
        self.assertIn('<a href="http://c"><code>C.md</code></a>', html)

    def test_code_span_outside_link_still_works(self):
        html = render("Call `foo()` then follow [docs](http://x) for details.")
        self.assertIn("<code>foo()</code>", html)
        self.assertIn('<a href="http://x">docs</a>', html)

    def test_backtick_inside_link_text_does_not_leak_into_later_code_span(self):
        html = render("[`A`](http://a) and then `separate code span`.")
        self.assertIn('<a href="http://a"><code>A</code></a>', html)
        self.assertIn("<code>separate code span</code>", html)

    def test_relative_md_link_rewrites_to_html(self):
        html = render("See [`modules.md`](modules.md) for details.")
        self.assertIn('<a href="modules.html"><code>modules.md</code></a>', html)

    def test_relative_md_link_preserves_anchor(self):
        html = render("See [`modules.md`](../reference/modules.md#format-adapters).")
        self.assertIn(
            '<a href="modules.html#format-adapters"><code>modules.md</code></a>',
            html,
        )

    def test_plain_inline_code_still_works(self):
        html = render("Use `std::vector` for dynamic arrays.")
        self.assertIn("<code>std::vector</code>", html)

    def test_bold_and_italic_still_work_outside_links(self):
        html = render("**bold** and *italic* and `code`.")
        self.assertIn("<strong>bold</strong>", html)
        self.assertIn("<em>italic</em>", html)
        self.assertIn("<code>code</code>", html)

    def test_link_syntax_inside_code_span_stays_literal(self):
        # Regression (Codex post-merge sweep wave 3, PR #575 follow-up):
        # `[x](y)` inside a code span must render as literal text inside
        # <code>, NOT as a link. Before this fix the link regex ran first
        # and transformed the inside of the backticks into <a href="y">x</a>,
        # which the code-span pass then HTML-escaped into literal
        # <code>&lt;a href="y"&gt;x&lt;/a&gt;</code>.
        html = render("Docs showing the literal form `[text](url)` here.")
        self.assertIn("<code>[text](url)</code>", html)
        # Must NOT have turned the inside of the code span into an anchor
        # tag, escaped or otherwise.
        self.assertNotIn('<a href="url">text</a>', html)
        self.assertNotIn("&lt;a href=", html)

    def test_markdown_link_syntax_inside_code_span_with_md_url(self):
        # .md-rewriting must also not fire inside a code span. The link
        # rewriter rewrites foo.md → foo.html; if it ran inside a code
        # span the user would see the wrong literal in the docs.
        html = render("Example: `[docs](modules.md)` renders as-is.")
        self.assertIn("<code>[docs](modules.md)</code>", html)
        self.assertNotIn('<a href="modules.html">', html)


if __name__ == "__main__":
    unittest.main()
