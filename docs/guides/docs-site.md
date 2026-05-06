# Documentation Site

How the Pulp documentation site at `www.generouscorp.com/pulp/` is built and deployed.

## Architecture

The docs site is a static HTML site generated from Markdown files in `docs/` by **MkDocs Material**. Before #577 (completed 2026-04-21), the site was rendered by a custom stdlib-only Python script at `tools/build-docs.py`; that generator has been retired in favour of Material for the admonitions, tabs, Mermaid support, hot-reload dev server, and built-in search that come for free with the Squidfunk ecosystem. `docs/status/docs-index.yaml` still drives the page list so slug-based URLs stay stable.

```
docs/*.md  →  mkdocs build  →  build/site/*.html  →  GitHub Pages
                  ↑
                  ├── tools/mkdocs_hooks.py  (pre-build drift checks +
                  │                           flat URL rewrite)
                  └── tools/build-api-docs.sh → build/site/api/
```

## How It Works

1. **Source**: Markdown files in `docs/` (guides, reference, concepts, examples, policies).
2. **Index**: `docs/status/docs-index.yaml` defines the slug-to-path mapping; `tools/mkdocs_hooks.py` reads it in `on_files` to rewrite each page's URL to `/pulp/{slug}.html` at the site root (so historical deep-links keep resolving).
3. **Pre-build gates**: `tools/mkdocs_hooks.py` `on_pre_build` runs `tools/docs_generate.py check` and `tools/check-docs-consistency.py`. A drift fails the build.
4. **Build**: `mkdocs build` with the config in `mkdocs.yml` renders Material HTML.
5. **API docs**: `tools/build-api-docs.sh` runs Doxygen on public headers, output merged into `build/site/api/`. The script pulls the current SDK version from `CMakeLists.txt` and injects it as Doxygen's `PROJECT_NUMBER` so `/api/` always shows the right release.
6. **Install scripts**: `tools/install/install.sh` and `install.ps1` are copied to `build/site/` root so the one-liner installer keeps working.
7. **Deploy**: GitHub Actions uploads `build/site/` as a Pages artifact.

## GitHub Pages Configuration

- **Build type**: GitHub Actions workflow (not branch-based)
- **Source branch**: `main`
- **Workflow**: `.github/workflows/docs-deploy.yml`
- **Custom domain**: `www.generouscorp.com` (configured at org/user level in GitHub settings, not via CNAME file)
- **Base URL**: `/pulp/` (the repo deploys to a subpath, not the domain root)
- **Visibility**: Public (Pages are public even while the repo is private)

There is no `gh-pages` branch. The workflow uses `actions/upload-pages-artifact` and `actions/deploy-pages` to deploy directly.

## URL Structure

```
www.generouscorp.com/pulp/                    → index.html (landing page)
www.generouscorp.com/pulp/getting-started.html → docs/guides/getting-started.md
www.generouscorp.com/pulp/capabilities.html    → docs/reference/capabilities.md
www.generouscorp.com/pulp/api/                 → Doxygen API reference
www.generouscorp.com/pulp/install.sh           → tools/install/install.sh
www.generouscorp.com/pulp/install.ps1          → tools/install/install.ps1
```

All doc pages are flattened to the root level (no subdirectory structure in URLs). The slug in `docs-index.yaml` becomes the filename, preserved across the Material migration by `tools/mkdocs_hooks.py`.

## When It Deploys

The workflow triggers on pushes to `main` that touch:
- `docs/**`
- `mkdocs.yml`, `requirements-docs.txt`, `tools/mkdocs_hooks.py`
- `core/**/include/**` (public headers affect API docs)
- `tools/build-api-docs.sh`
- `tools/install/install.sh` or `tools/install/install.ps1`
- `docs/doxygen/**`
- `.github/workflows/docs-deploy.yml`

It can also be triggered manually via `workflow_dispatch`.

## Building Locally

```bash
# One-time setup
pip install -r requirements-docs.txt

# Build the deployed site shape
mkdocs build --site-dir build/site

# Hot-reload dev server (Material's killer feature vs. the old generator)
mkdocs serve

# Build API reference (requires Doxygen; auto-picks SDK version from CMakeLists.txt)
./tools/build-api-docs.sh
```

`mkdocs serve` watches `docs/` and `mkdocs.yml` and refreshes the browser on save.

## Adding a New Page

1. Create `docs/<category>/<slug>.md`.
2. Add an entry to `docs/status/docs-index.yaml` (slug + path) if you want a stable flat URL.
3. Push to main — the deploy workflow picks it up automatically.

If you skip step 2, MkDocs still publishes the file at its nested path (e.g. `/pulp/guides/<slug>.html`). The `mkdocs_hooks.py` flattener only renames pages listed in `docs-index.yaml`.

## Install Script Hosting

The install scripts are hosted on the same Pages site. The docs-deploy workflow copies them:

```yaml
# In .github/workflows/docs-deploy.yml
- name: Copy install scripts
  run: |
    cp tools/install/install.sh build/site/install.sh
    cp tools/install/install.ps1 build/site/install.ps1
```

This means `curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh` works as the one-liner installer. The scripts download pre-built CLI binaries from GitHub Releases (built by `.github/workflows/release-cli.yml`). They install Pulp only; they must not install Shipyard, GitHub CLI (`gh`), or other source-checkout contributor tooling.

All install script references must use `https://www.generouscorp.com/pulp/install.sh` (with `www.`).
