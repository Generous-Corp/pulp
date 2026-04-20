#!/usr/bin/env python3
"""build-docs.py — Generate a static docs site from Pulp's docs/ tree.

Reads docs/status/docs-index.yaml for navigation structure.
Converts Markdown to HTML with a simple regex-based converter.
Outputs to build/site/ (or a custom directory via --output).

No external dependencies — stdlib only.
"""

import argparse
import html
import os
import re
import shutil
import sys
from pathlib import Path


# ── Markdown to HTML converter (stdlib only) ─────────────────────────────────

def md_to_html(md: str) -> str:
    """Convert Markdown to HTML. Handles headings, code blocks, tables,
    lists, bold, italic, inline code, links, and paragraphs."""
    lines = md.split('\n')
    out = []
    in_code = False
    in_table = False
    in_ul = False
    in_ol = False
    code_lang = ''
    code_cls = ''
    code_lines = []
    para = []

    def flush_para():
        if para:
            text = ' '.join(para)
            out.append(f'<p>{inline(text)}</p>')
            para.clear()

    def flush_list():
        nonlocal in_ul, in_ol
        if in_ul:
            out.append('</ul>')
            in_ul = False
        if in_ol:
            out.append('</ol>')
            in_ol = False

    def flush_table():
        nonlocal in_table
        if in_table:
            out.append('</tbody></table>')
            in_table = False

    def inline(text):
        # Order matters: links FIRST (before code-span split) so link text
        # containing backticks — e.g. [`DEPENDENCIES.md`](...) — is kept
        # intact. Previous order split on backticks first, which shattered
        # the [...](...) brackets and left literal `[` / `](url)` in the
        # output. Inside link text we still honour backticks as code.
        def rewrite_link(m):
            link_text, url = m.group(1), m.group(2)
            if '.md' in url and not url.startswith('http'):
                url = url.replace('.md', '.html')
                # Strip path prefixes — all pages are at root level
                # But preserve anchors: modules.html#format → modules.html#format
                anchor = ''
                if '#' in url:
                    url, anchor = url.split('#', 1)
                    anchor = '#' + anchor
                url = url.split('/')[-1] + anchor
            # Render backticks inside link text as inline <code>.
            inner_parts = re.split(r'(`[^`]+`)', link_text)
            rendered = []
            for ip in inner_parts:
                if ip.startswith('`') and ip.endswith('`'):
                    rendered.append(f'<code>{html.escape(ip[1:-1])}</code>')
                else:
                    rendered.append(ip)
            return f'<a href="{url}">{"".join(rendered)}</a>'
        text = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', rewrite_link, text)

        # Code spans (after links, so backticks inside links were already handled)
        parts = re.split(r'(`[^`]+`)', text)
        result = []
        for part in parts:
            if part.startswith('`') and part.endswith('`'):
                result.append(f'<code>{html.escape(part[1:-1])}</code>')
            else:
                p = part
                p = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', p)
                p = re.sub(r'__(.+?)__', r'<strong>\1</strong>', p)
                p = re.sub(r'\*(.+?)\*', r'<em>\1</em>', p)
                p = re.sub(r'_(.+?)_', r'<em>\1</em>', p)
                result.append(p)
        return ''.join(result)

    i = 0
    while i < len(lines):
        line = lines[i]

        # Fenced code blocks
        if line.strip().startswith('```'):
            if not in_code:
                flush_para()
                flush_list()
                flush_table()
                code_lang = line.strip()[3:].strip()
                code_cls = f' class="language-{html.escape(code_lang)}"' if code_lang else ''
                code_lines = []
                in_code = True
            else:
                # Emit the whole block as a single `out` entry so the outer
                # '\n'.join() doesn't inject a blank line between <pre><code>
                # and the first line of code (which would appear as a phantom
                # top-of-block blank line because <pre> preserves whitespace).
                lang_label_html = (
                    f'<span class="code-lang" aria-hidden="true">{html.escape(code_lang)}</span>'
                    if code_lang else ''
                )
                copy_btn_html = (
                    '<button type="button" class="copy-btn" '
                    'aria-label="Copy code to clipboard" '
                    'data-copy-label="Copy" data-copied-label="Copied">'
                    '<svg width="14" height="14" viewBox="0 0 16 16" fill="none" '
                    'stroke="currentColor" stroke-width="1.5" aria-hidden="true">'
                    '<rect x="4" y="4" width="9" height="9" rx="1.5"/>'
                    '<path d="M10 4V3a1 1 0 0 0-1-1H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h1"/>'
                    '</svg>'
                    '<span class="copy-btn-label">Copy</span>'
                    '</button>'
                )
                code_body = '\n'.join(code_lines)
                out.append(
                    f'<figure class="code-block">{lang_label_html}{copy_btn_html}'
                    f'<pre><code{code_cls}>{code_body}</code></pre></figure>'
                )
                code_lines = []
                code_lang = ''
                in_code = False
            i += 1
            continue

        if in_code:
            code_lines.append(html.escape(line))
            i += 1
            continue

        stripped = line.strip()

        # Empty line
        if not stripped:
            flush_para()
            flush_list()
            flush_table()
            i += 1
            continue

        # Headings
        m = re.match(r'^(#{1,6})\s+(.+)$', stripped)
        if m:
            flush_para()
            flush_list()
            flush_table()
            level = len(m.group(1))
            text = m.group(2)
            slug = re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')
            out.append(f'<h{level} id="{slug}">{inline(text)} <a class="permalink" href="#{slug}" data-pagefind-ignore>#</a></h{level}>')
            i += 1
            continue

        # Tables
        if '|' in stripped and stripped.startswith('|'):
            cells = [c.strip() for c in stripped.split('|')[1:-1]]
            if cells:
                # Check if next line is separator
                if not in_table:
                    flush_para()
                    flush_list()
                    # Check if this is a header row (next line is ---)
                    if i + 1 < len(lines) and re.match(r'^\|[\s\-:|]+\|$', lines[i + 1].strip()):
                        out.append('<table><thead><tr>')
                        for c in cells:
                            out.append(f'<th>{inline(c)}</th>')
                        out.append('</tr></thead><tbody>')
                        in_table = True
                        i += 2  # skip separator
                        continue
                    else:
                        out.append('<table><tbody>')
                        in_table = True
                if in_table:
                    # Skip separator rows
                    if re.match(r'^[\s\-:|]+$', stripped.replace('|', '')):
                        i += 1
                        continue
                    out.append('<tr>')
                    for c in cells:
                        out.append(f'<td>{inline(c)}</td>')
                    out.append('</tr>')
                    i += 1
                    continue

        # Unordered list
        m = re.match(r'^(\s*)[-*]\s+(.+)$', stripped)
        if m:
            flush_para()
            flush_table()
            if not in_ul:
                flush_list()
                out.append('<ul>')
                in_ul = True
            out.append(f'<li>{inline(m.group(2))}</li>')
            i += 1
            continue

        # Ordered list
        m = re.match(r'^(\s*)\d+\.\s+(.+)$', stripped)
        if m:
            flush_para()
            flush_table()
            if not in_ol:
                flush_list()
                out.append('<ol>')
                in_ol = True
            out.append(f'<li>{inline(m.group(2))}</li>')
            i += 1
            continue

        # Horizontal rule
        if re.match(r'^[-*_]{3,}$', stripped):
            flush_para()
            flush_list()
            flush_table()
            out.append('<hr>')
            i += 1
            continue

        # Regular text → paragraph
        flush_list()
        flush_table()
        para.append(stripped)
        i += 1

    flush_para()
    flush_list()
    flush_table()

    return '\n'.join(out)


# ── YAML parser (minimal, stdlib only) ────────────────────────────────────────

def parse_docs_index(path: Path) -> list[dict]:
    """Parse docs-index.yaml into a list of {slug, path, kind, summary}."""
    entries = []
    current = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip()
            if not line.strip() or line.strip().startswith('#'):
                continue
            m = re.match(r'\s+-\s+slug:\s+(.+)', line)
            if m:
                if current:
                    entries.append(current)
                current = {'slug': m.group(1).strip()}
                continue
            for key in ('path', 'kind', 'summary'):
                m = re.match(rf'\s+{key}:\s+(.+)', line)
                if m:
                    current[key] = m.group(1).strip()
        if current:
            entries.append(current)
    return entries


# ── Navigation ────────────────────────────────────────────────────────────────

NAV_SECTIONS = [
    ('', [
        ('index', 'Home'),
    ]),
    ('About', [
        ('vision', 'Vision'),
        ('overview', 'Overview'),
        ('architecture', 'Architecture'),
    ]),
    ('Getting Started', [
        ('getting-started', 'Getting Started'),
        ('build', 'Building'),
        ('testing', 'Testing'),
        ('docs-maintenance', 'Docs Maintenance'),
    ]),
    ('Support', [
        ('capabilities', 'Capabilities'),
    ]),
    ('Modules', [
        ('modules', 'Module Reference'),
    ]),
    ('Examples', [
        ('examples-index', 'Example Gallery'),
        ('examples', 'Example Walkthroughs'),
        ('example-pulp-gain', 'PulpGain'),
        ('example-pulp-tone', 'PulpTone'),
        ('example-pulp-effect', 'PulpEffect'),
        ('example-pulp-compressor', 'PulpCompressor'),
        ('example-pulp-synth', 'PulpSynth'),
        ('example-pulp-drums', 'PulpDrums'),
        ('example-pulp-sampler', 'PulpSampler'),
        ('example-pulp-pluck', 'PulpPluck'),
        ('example-ui-preview', 'UI Preview'),
    ]),
    ('CLI & CMake', [
        ('cli', 'CLI Reference'),
        ('cmake', 'CMake Reference'),
    ]),
    ('Guides', [
        ('web-plugins', 'Web Plugins'),
        ('gpu-validation', 'GPU Validation'),
    ]),
    ('Policies', [
        ('code-style', 'Code Style'),
        ('agent-rules', 'Agent Rules'),
        ('licensing', 'Licensing & Acknowledgements'),
    ]),
]


def build_nav_html(current_slug: str, base_url: str) -> str:
    """Build the sidebar navigation HTML."""
    parts = []
    for section_title, items in NAV_SECTIONS:
        if section_title:
            parts.append(f'<div class="nav-section">{html.escape(section_title)}</div>')
        parts.append('<ul>')
        for slug, label in items:
            active = ' class="active"' if slug == current_slug else ''
            parts.append(f'<li><a href="{base_url}{slug}.html"{active}>{html.escape(label)}</a></li>')
        parts.append('</ul>')
    return '\n'.join(parts)


# ── HTML template ─────────────────────────────────────────────────────────────

def page_html(title: str, content: str, nav: str, base_url: str, branch: str) -> str:
    return f'''<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)} — Pulp</title>
<style>
:root {{
  --bg: #1a1a2e;
  --surface: #16213e;
  --surface2: #0f3460;
  --text: #e0e0e0;
  --text-muted: #a0a0b0;
  --accent: #e94560;
  --accent2: #533483;
  --link: #64b5f6;
  --border: #2a2a4a;
  --code-bg: #0d1117;
  --nav-width: 240px;
}}
* {{ margin: 0; padding: 0; box-sizing: border-box; }}
body {{
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  background: var(--bg);
  color: var(--text);
  line-height: 1.6;
}}
a {{ color: var(--link); text-decoration: none; }}
a:hover {{ text-decoration: underline; }}

/* Header */
.header {{
  background: var(--surface);
  border-bottom: 1px solid var(--border);
  padding: 12px 24px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  z-index: 100;
  height: 48px;
}}
.alpha-badge {{
  font-size: 11px;
  color: #c9a227;
  border: 1px solid #c9a22744;
  border-radius: 4px;
  padding: 2px 8px;
  white-space: nowrap;
}}
.header-left {{
  display: flex;
  align-items: center;
  gap: 16px;
}}
.header h1 {{
  font-size: 18px;
  font-weight: 700;
  color: var(--accent);
  letter-spacing: 1px;
}}
.branch-badge {{
  font-size: 11px;
  padding: 2px 8px;
  border-radius: 10px;
  background: var(--accent2);
  color: #ddd;
  font-weight: 500;
}}
.header-right a {{
  color: var(--text-muted);
  font-size: 13px;
  margin-left: 16px;
}}

/* Sidebar */
.sidebar {{
  position: fixed;
  top: 48px;
  left: 0;
  bottom: 0;
  width: var(--nav-width);
  background: var(--surface);
  border-right: 1px solid var(--border);
  overflow-y: auto;
  padding: 16px 0;
}}
.nav-section {{
  font-size: 11px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 1px;
  color: var(--text-muted);
  padding: 12px 20px 4px;
}}
.sidebar ul {{
  list-style: none;
  margin: 0 0 8px;
}}
.sidebar li a {{
  display: block;
  padding: 4px 20px;
  font-size: 13px;
  color: var(--text);
  border-left: 3px solid transparent;
}}
.sidebar li a:hover {{
  background: var(--surface2);
  text-decoration: none;
}}
.sidebar li a.active {{
  border-left-color: var(--accent);
  color: var(--accent);
  background: rgba(233, 69, 96, 0.08);
}}

/* Content */
.content {{
  margin-left: var(--nav-width);
  margin-top: 48px;
  padding: 32px 48px;
  max-width: 900px;
}}
.content h1 {{ font-size: 28px; margin: 0 0 16px; color: #fff; }}
.content h2 {{ font-size: 22px; margin: 32px 0 12px; color: #fff; border-bottom: 1px solid var(--border); padding-bottom: 8px; }}
.content h3 {{ font-size: 17px; margin: 24px 0 8px; color: #ddd; }}
.content h4 {{ font-size: 15px; margin: 20px 0 6px; color: #ccc; }}
.content p {{ margin: 8px 0; }}
.content ul, .content ol {{ margin: 8px 0 8px 24px; }}
.content li {{ margin: 4px 0; }}
.content hr {{ border: none; border-top: 1px solid var(--border); margin: 24px 0; }}

/* Code */
.content code {{
  font-family: "SF Mono", "Fira Code", monospace;
  font-size: 13px;
  background: var(--code-bg);
  padding: 2px 6px;
  border-radius: 4px;
}}
.content pre {{
  background: var(--code-bg);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 16px;
  overflow-x: auto;
  margin: 0;
}}
.content pre code {{
  background: none;
  padding: 0;
  font-size: 13px;
  line-height: 1.5;
}}
/* Code block wrapper for copy button + language label */
.content figure.code-block {{
  position: relative;
  margin: 12px 0;
}}
.content figure.code-block .code-lang {{
  position: absolute;
  top: 6px;
  left: 12px;
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  opacity: 0.5;
  font-family: "SF Mono", Menlo, Consolas, monospace;
  pointer-events: none;
  user-select: none;
  z-index: 1;
}}
.content figure.code-block .copy-btn {{
  position: absolute;
  top: 6px;
  right: 6px;
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 4px 8px;
  font-size: 11px;
  font-family: inherit;
  color: inherit;
  background: var(--code-bg);
  border: 1px solid var(--border);
  border-radius: 4px;
  cursor: pointer;
  opacity: 0.5;
  transition: opacity 120ms ease, background 120ms ease;
  z-index: 2;
}}
.content figure.code-block:hover .copy-btn,
.content figure.code-block .copy-btn:focus-visible {{
  opacity: 1;
}}
.content figure.code-block .copy-btn:hover {{
  background: var(--bg);
}}
.content figure.code-block .copy-btn.copied {{
  opacity: 1;
  color: #4caf50;
  border-color: #4caf50;
}}
.content figure.code-block .copy-btn svg {{
  flex-shrink: 0;
}}
@media (prefers-reduced-motion: reduce) {{
  .content figure.code-block .copy-btn {{
    transition: none;
  }}
}}

/* Tables */
.content table {{
  border-collapse: collapse;
  width: 100%;
  margin: 12px 0;
  font-size: 14px;
}}
.content th, .content td {{
  padding: 8px 12px;
  text-align: left;
  border: 1px solid var(--border);
}}
.content th {{
  background: var(--surface2);
  font-weight: 600;
  color: #fff;
}}
.content tr:nth-child(even) {{
  background: rgba(255,255,255,0.02);
}}

/* Strong in tables for status badges */
.content td strong {{
  font-weight: 600;
}}

/* Permalink anchors */
.permalink {{
  color: var(--text-muted);
  font-size: 0.7em;
  opacity: 0;
  transition: opacity 0.15s;
  text-decoration: none;
  margin-left: 6px;
}}
h1:hover .permalink, h2:hover .permalink, h3:hover .permalink,
h4:hover .permalink {{ opacity: 0.6; }}
.permalink:hover {{ opacity: 1 !important; color: var(--accent); }}

/* Mobile hamburger */
.menu-toggle {{
  display: none;
  background: none;
  border: none;
  color: var(--text);
  font-size: 22px;
  cursor: pointer;
  padding: 4px 8px;
}}

/* Responsive */
@media (max-width: 768px) {{
  .menu-toggle {{ display: block; }}
  .header-left .alpha-badge {{ display: none; }}
  .sidebar {{
    transform: translateX(-100%);
    transition: transform 0.2s ease;
    z-index: 99;
    width: 260px;
  }}
  .sidebar.open {{ transform: translateX(0); }}
  .content {{ margin-left: 0; padding: 20px 16px; }}
  .content h1 {{ font-size: 22px; }}
  .content h2 {{ font-size: 18px; }}
  .content table {{ font-size: 12px; display: block; overflow-x: auto; }}
  .content pre {{ font-size: 12px; }}
  .header-search {{ max-width: none; margin: 0 8px; }}
}}
.header-search {{
  flex: 1;
  max-width: 360px;
  margin: 0 16px;
  position: relative;     /* anchor the absolutely-positioned drawer */
}}
.header-search .pagefind-ui {{
  --pagefind-ui-scale: 0.7;
  --pagefind-ui-primary: #e94560;
  --pagefind-ui-text: #e0e0e0;
  --pagefind-ui-background: #0d1117;
  --pagefind-ui-border: #2a2a4a;
  --pagefind-ui-tag: #0f3460;
  --pagefind-ui-border-width: 1px;
  --pagefind-ui-border-radius: 6px;
  --pagefind-ui-font: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
}}
/* Detach the results drawer from the fixed-height header so it opens
   as an overlay below the input instead of pushing the input up and
   rendering transparently over the page body. */
.header-search .pagefind-ui__drawer {{
  position: absolute;
  top: calc(100% + 6px);
  left: 0;
  right: 0;
  max-height: calc(100vh - 96px);
  overflow-y: auto;
  background: #0d1117;
  border: 1px solid #2a2a4a;
  border-radius: 8px;
  box-shadow: 0 12px 32px rgba(0, 0, 0, 0.6);
  z-index: 101;           /* header is 100; drawer sits above its siblings */
  padding: 8px 12px;
}}
/* When the drawer is empty (no input yet) keep it fully collapsed so
   the empty overlay box doesn't ghost under the input. */
.header-search .pagefind-ui__drawer:empty,
.header-search .pagefind-ui__drawer.pagefind-ui__hidden {{
  display: none;
}}
.pagefind-ui--reset mark,
.pagefind-ui mark {{
  background: #e94560;
  color: #ffffff;
}}
</style>
<link href="{base_url}pagefind/pagefind-ui.css" rel="stylesheet">
</head>
<body>
<header class="header">
  <div class="header-left">
    <button class="menu-toggle" onclick="document.querySelector('.sidebar').classList.toggle('open')">☰</button>
    <h1>PULP</h1>
    <span class="branch-badge">{html.escape(branch)}</span>
    <span class="alpha-badge">Alpha — under active development</span>
  </div>
  <div class="header-search">
    <div id="search"></div>
  </div>
  <div class="header-right">
    <a href="https://github.com/danielraffel/pulp">GitHub</a>
  </div>
</header>
<nav class="sidebar">
{nav}
</nav>
<main class="content" data-pagefind-body data-pagefind-meta="title:{html.escape(title)}">
{content}
</main>
<script src="{base_url}pagefind/pagefind-ui.js" onerror="window.__pagefindMissing=true"></script>
<script>
// Pagefind is only built in the docs-deploy CI workflow; local previews
// via `python3 tools/build-docs.py` or `pulp docs build-site` don't
// produce the `pagefind/` directory, so PagefindUI is undefined and the
// page throws ReferenceError. Guard the init so local previews render
// the rest of the site without the search widget.
if (typeof PagefindUI !== 'undefined' && !window.__pagefindMissing) {{
  new PagefindUI({{
    element: "#search",
    showSubResults: true,
    showImages: false
  }});
}}
</script>
<script>
// Persist sidebar scroll position across page loads
(function() {{
  var sidebar = document.querySelector('.sidebar');
  var key = 'pulp-nav-scroll';
  if (sidebar) {{
    var saved = sessionStorage.getItem(key);
    if (saved) sidebar.scrollTop = parseInt(saved, 10);
    sidebar.addEventListener('scroll', function() {{
      sessionStorage.setItem(key, sidebar.scrollTop);
    }});
  }}
  // Close mobile nav on link click
  var links = document.querySelectorAll('.sidebar a');
  links.forEach(function(a) {{
    a.addEventListener('click', function() {{
      sidebar.classList.remove('open');
    }});
  }});
}})();
</script>
<script>
// Copy-to-clipboard on fenced code blocks. Each figure.code-block got a
// <button class="copy-btn"> inserted server-side; we wire click → copy the
// adjacent <pre><code> text → flip the button label to "Copied" for 2s.
(function() {{
  var buttons = document.querySelectorAll('figure.code-block .copy-btn');
  buttons.forEach(function(btn) {{
    btn.addEventListener('click', async function() {{
      var fig = btn.closest('figure.code-block');
      if (!fig) return;
      var code = fig.querySelector('pre code');
      if (!code) return;
      var text = code.innerText;
      var label = btn.querySelector('.copy-btn-label');
      var original = btn.dataset.copyLabel || 'Copy';
      var copied = btn.dataset.copiedLabel || 'Copied';
      try {{
        if (navigator.clipboard && window.isSecureContext) {{
          await navigator.clipboard.writeText(text);
        }} else {{
          // Fallback for insecure contexts / older browsers
          var ta = document.createElement('textarea');
          ta.value = text;
          ta.style.position = 'fixed';
          ta.style.opacity = '0';
          document.body.appendChild(ta);
          ta.select();
          document.execCommand('copy');
          document.body.removeChild(ta);
        }}
        btn.classList.add('copied');
        btn.setAttribute('aria-label', 'Copied to clipboard');
        if (label) label.textContent = copied;
        setTimeout(function() {{
          btn.classList.remove('copied');
          btn.setAttribute('aria-label', 'Copy code to clipboard');
          if (label) label.textContent = original;
        }}, 2000);
      }} catch (err) {{
        // Surface the failure briefly rather than silently
        if (label) label.textContent = 'Failed';
        setTimeout(function() {{
          if (label) label.textContent = original;
        }}, 2000);
      }}
    }});
  }});
}})();
</script>
</body>
</html>'''


# ── Landing page ──────────────────────────────────────────────────────────────

def build_landing_page(base_url: str) -> str:
    return f'''<h1>Pulp Documentation</h1>
<p>Pulp is a cross-platform framework for building audio plugins and applications.
MIT-licensed. No royalties. No copyleft.</p>

<h2>What is supported today</h2>
<ul>
<li><strong>Formats</strong>: VST3, AU v2, AUv3, CLAP, LV2, WAMv2, WebCLAP, standalone, headless</li>
<li><strong>Platforms</strong>: macOS, Windows (WASAPI, NSIS), Linux (ALSA, JACK, LV2), Browser (WASM)</li>
<li><strong>Audio</strong>: CoreAudio, WASAPI, ALSA, JACK, Web Audio API — device I/O, buffer processing, file read/write</li>
<li><strong>MIDI</strong>: CoreMIDI, Win32 MIDI, ALSA MIDI, Web MIDI, MIDI 2.0 UMP, MPE</li>
<li><strong>DSP</strong>: 30+ signal processors (oscillator, biquad, SVF, ladder, FIR, TPT, compressor, reverb, delay, chorus, phaser, FFT, convolver, and more)</li>
<li><strong>Parameters</strong>: thread-safe, automatable, serializable, with CLAP modulation, presets, undo/redo</li>
<li><strong>GPU rendering</strong>: Dawn (Metal/D3D12/Vulkan) + Skia Graphite on all platforms</li>
<li><strong>View system</strong>: TextEditor, ComboBox, TabPanel, ListBox, TreeView, and more — flex layout, JS scripting, hot-reload</li>
<li><strong>Plugin hosting</strong>: PluginScanner, PluginSlot, SignalGraph for DAW-like apps</li>
<li><strong>Testing</strong>: 1622+ automated tests across 13 subsystems</li>
<li><strong>Shipping</strong>: codesign, notarization, DMG/PKG (macOS), NSIS (Windows), .deb (Linux), appcast</li>
</ul>

<h2>Where to start</h2>
<ul>
<li><a href="{base_url}getting-started.html">Getting Started</a> — build your first plugin step by step</li>
<li><a href="{base_url}examples-index.html">Examples</a> — browse 25 example projects by category</li>
<li><a href="{base_url}capabilities.html">Capabilities</a> — full capability matrix with status</li>
<li><a href="{base_url}overview.html">Overview</a> — what Pulp is and how it is organized</li>
</ul>

<h2>Reference</h2>
<ul>
<li><a href="{base_url}modules.html">Modules</a> — 13 subsystems with status, dependencies, and key headers</li>
<li><a href="{base_url}api/">API Reference</a> — Doxygen-generated class and function documentation</li>
<li><a href="{base_url}cli.html">CLI Reference</a> — <code>pulp</code> command reference</li>
<li><a href="{base_url}cmake.html">CMake Reference</a> — <code>pulp_add_plugin()</code> and build system functions</li>
<li><a href="{base_url}licensing.html">Licensing &amp; Acknowledgements</a> — dependencies, standards, attribution</li>
<li><a href="{base_url}architecture.html">Architecture</a> — subsystem dependencies, thread model, GPU stack</li>
</ul>

<h2>Guides</h2>
<ul>
<li><a href="{base_url}build.html">Building</a> — requirements, options, platform notes</li>
<li><a href="{base_url}testing.html">Testing</a> — running tests, validation, writing tests</li>
<li><a href="{base_url}web-plugins.html">Web Plugins</a> — WAMv2, WebCLAP, browser demos</li>
<li><a href="{base_url}docs-maintenance.html">Docs Maintenance</a> — how docs stay consistent with code</li>
</ul>

<h2>Policies</h2>
<ul>
<li><a href="{base_url}code-style.html">Code Style</a> — coding standards and architectural rules</li>
<li><a href="{base_url}agent-rules.html">Agent Rules</a> — contribution rules for AI agents</li>
</ul>
'''


# ── Build ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description='Build Pulp docs site')
    parser.add_argument('--output', '-o', default='build/site',
                        help='Output directory (default: build/site)')
    parser.add_argument('--base-url', default='/pulp/',
                        help='Base URL path (default: /pulp/)')
    parser.add_argument('--branch', default=None,
                        help='Branch name for badge (auto-detected if omitted)')
    args = parser.parse_args()

    # Find project root
    script_dir = Path(__file__).resolve().parent
    root = script_dir.parent
    docs_dir = root / 'docs'
    output_dir = Path(args.output)
    if not output_dir.is_absolute():
        output_dir = root / output_dir
    base_url = args.base_url
    if not base_url.endswith('/'):
        base_url += '/'

    # Auto-detect branch
    branch = args.branch
    if not branch:
        try:
            import subprocess
            branch = subprocess.check_output(
                ['git', 'branch', '--show-current'],
                cwd=root, text=True
            ).strip()
        except Exception:
            branch = 'main'

    # Parse docs index
    index_path = docs_dir / 'status' / 'docs-index.yaml'
    if not index_path.exists():
        print(f'Error: {index_path} not found', file=sys.stderr)
        return 1
    entries = parse_docs_index(index_path)

    # Clean and create output
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)

    built = 0

    # Build each doc page
    for entry in entries:
        slug = entry['slug']
        doc_path = docs_dir / entry.get('path', '')
        if not doc_path.exists():
            print(f'  SKIP: {entry.get("path", "?")} (file not found)')
            continue

        md_content = doc_path.read_text(encoding='utf-8')
        html_content = md_to_html(md_content)
        nav = build_nav_html(slug, base_url)

        # Extract title from first heading
        title_match = re.match(r'^#\s+(.+)', md_content)
        title = title_match.group(1) if title_match else slug.replace('-', ' ').title()

        page = page_html(title, html_content, nav, base_url, branch)
        out_file = output_dir / f'{slug}.html'
        out_file.write_text(page, encoding='utf-8')
        built += 1

    # Build landing page (index.html)
    nav = build_nav_html('index', base_url)
    landing = build_landing_page(base_url)
    index_page = page_html('Documentation', landing, nav, base_url, branch)
    (output_dir / 'index.html').write_text(index_page, encoding='utf-8')
    built += 1

    # Copy assets if they exist
    assets_dir = docs_dir / 'assets'
    if assets_dir.exists():
        shutil.copytree(assets_dir, output_dir / 'assets')

    print(f'Built {built} pages in {output_dir}')
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
