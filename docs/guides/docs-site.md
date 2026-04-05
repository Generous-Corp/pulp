# Documentation Site

How the Pulp documentation site at `www.generouscorp.com/pulp/` is built and deployed.

## Architecture

The docs site is a static HTML site generated from Markdown files in `docs/`. There is no static site generator framework (no Jekyll, Hugo, etc.) — just a custom Python script.

```
docs/*.md  →  tools/build-docs.py  →  build/site/*.html  →  GitHub Pages
```

## How It Works

1. **Source**: Markdown files in `docs/` (guides, reference, concepts, examples, policies)
2. **Index**: `docs/status/docs-index.yaml` defines the navigation structure and slug-to-path mapping
3. **Build**: `tools/build-docs.py` converts Markdown to HTML using a regex-based converter (stdlib only, no dependencies)
4. **API docs**: `tools/build-api-docs.sh` runs Doxygen on public headers, output goes to `build/site/api/`
5. **Install scripts**: `tools/install/install.sh` and `install.ps1` are copied to `build/site/` root
6. **Deploy**: GitHub Actions uploads `build/site/` as a Pages artifact and deploys it

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

All doc pages are flattened to the root level (no subdirectory structure in URLs). The slug in `docs-index.yaml` becomes the filename.

## When It Deploys

The workflow triggers on pushes to `main` that touch:
- `docs/**`
- `core/**/include/**` (public headers affect API docs)
- `tools/build-docs.py` or `tools/build-api-docs.sh`
- `tools/install/install.sh` or `tools/install/install.ps1`
- `docs/doxygen/**`
- `.github/workflows/docs-deploy.yml`

It can also be triggered manually via `workflow_dispatch`.

## Building Locally

```bash
# Build the deployed site shape
python3 tools/build-docs.py --output build/site --base-url /pulp/

# Build API reference (requires Doxygen)
./tools/build-api-docs.sh

# For local preview, build with a root base URL
python3 tools/build-docs.py --output build/site-local --base-url /
cd build/site-local && python3 -m http.server 8000
# Open http://localhost:8000/
```

`build/site/` is the Pages artifact shape and keeps the deployed `/pulp/` base URL.
Use `build/site-local/` only for local preview.

## Adding a New Page

1. Create `docs/<category>/<slug>.md`
2. Add an entry to `docs/status/docs-index.yaml`
3. Push to main — the deploy workflow picks it up automatically

## Install Script Hosting

The install scripts are hosted on the same Pages site. The docs-deploy workflow copies them:

```yaml
# In .github/workflows/docs-deploy.yml
- name: Copy install scripts
  run: |
    cp tools/install/install.sh build/site/install.sh
    cp tools/install/install.ps1 build/site/install.ps1
```

This means `curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh` works as the one-liner installer. The scripts download pre-built CLI binaries from GitHub Releases (built by `.github/workflows/release-cli.yml`).

All install script references must use `https://www.generouscorp.com/pulp/install.sh` (with `www.`).
