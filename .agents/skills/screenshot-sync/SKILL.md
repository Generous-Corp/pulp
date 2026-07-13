---
name: screenshot-sync
description: >
  Keep a plugin/demo repo's screenshots in sync with its UX. When editor / UI /
  design-token code changes in an opted-in repo (one carrying
  .pulp/screenshots.toml), re-capture and refresh every place each shot is
  consumed — README images, web-demo Open Graph (og.png) images, and gallery
  thumbnails — consistently across the WAM and WCLAP demo sites. Two capture
  routes: native headless (render_to_png / pulp-screenshot, Skia) for README /
  native-editor shots, and live-web headless Chrome (playwright-core) for
  WAM/WCLAP demo OG + gallery images. Backfills demos missing OG images. This is
  a PORTING / consistency tool — it re-shoots a repo's OWN UI, it never clones
  another product's art.
requires:
  tools:
    - python3          # screenshot_sync_check.py (Python 3.11+ for tomllib)
    - node             # playwright-core (web capture route)
  files:
    - tools/scripts/screenshot_sync_check.py
    - core/view/include/pulp/view/screenshot.hpp
    - examples/web-demos/wclap-build/cloudflare/assemble-gallery.mjs
---

# screenshot-sync

Keep a repo's screenshots in lockstep with its UI. When the UX changes, the
README shot, the web-demo Open Graph preview (`og.png`), and the gallery
thumbnail all have to change with it — this skill re-captures them and the
`screenshot_sync_check.py` gate fails a PR that forgot to.

Read the [`screenshot`](../screenshot/SKILL.md) skill first for the raw capture
mechanics (backends, the image-compositing trap, the content-floor oracle,
vision-probe). This skill is the *sync/consume orchestration* on top of it.

## When this applies

- A repo carrying `.pulp/screenshots.toml` had a UX-path change and the
  screenshot-sync gate (pre-push / CI) or PostToolUse hint fired, OR
- "update the screenshots / OG images / gallery thumbs", "backfill the WCLAP OG
  images", "the demo preview image is stale / links don't unfurl".

## 0. Confirm opt-in

Screenshots are a **plugin-with-releases** concern, not a core/tooling one.
Check for `.pulp/screenshots.toml` at the repo root. **Absent → the repo is not
opted in; stop.** Do not invent screenshots for a library/tool/core repo. Opt-in
is per-repo by the file's presence — the same pattern
`~/.config/pulp/daw-smoke.toml` and `.shipyard.local/config.toml` use.

Default opt-in set: the `packaged-demo` and `packaged-examples` consumers
(gpu-nam, bendr, superconvolver, spectral-lab, tempo-sampler, classic-effects,
pulp-example-plugins). Everything else — pulp core included — stays off.

## The manifest: `.pulp/screenshots.toml`

One file per opted-in repo, at its root. It lists the UX paths that should
trigger a re-shoot and, for each screenshot **target**, its capture route and
every **place the image is consumed** — so one re-shoot fans out to README + OG
+ gallery and they cannot drift.

```toml
schema_version = 1

# UX paths whose change should trigger a re-shoot (repo-relative, gitignore-style
# globs — the same dialect skill_path_map.json uses). A design-token change
# reskins every widget, so a trigger hit invalidates ALL targets in the repo.
[trigger]
paths = [
  "src/**/*_editor.cpp",
  "ui/**/*.js",
  "assets/design-system/**/*.css",
  "assets/design-system/**/*.tokens.json",
]

# ── Native README / editor shot (capture route A) ──
[[target]]
id      = "mono-synth-editor"
route   = "native"                 # render_to_png / pulp-screenshot, Skia/gpu
tool    = "pulp-mono-synth-shot"   # per-editor dumper target (brew_shots.cpp pattern)
size    = "720x460"
scale   = 2.0                      # match the native editor baseline (test_editors.cpp)
backend = "skia"                   # or "gpu" for requires_gpu_host() views
consumes = [
  { kind = "readme", path = "docs/img/mono-synth.png" },
]

# ── Web demo OG + gallery (capture route B) ──
[[target]]
id                  = "mono-synth-web"
route               = "web"
url                 = "example-plugins/mono-synth/index.html"
device_scale_factor = 2
consumes = [
  { kind = "og",      path = "example-plugins/mono-synth/og.png" },
  { kind = "gallery", path = "docs/gallery/mono-synth.png" },
  { kind = "og_meta", html = "example-plugins/mono-synth/index.html", rel = "./og.png" },
]

[[target]]
id                  = "example-gallery-web"
route               = "web"
url                 = "example-plugins/index.html"
device_scale_factor = 2
# A target may narrow the global trigger to just the paths that affect IT:
trigger = [ "ui/**/*.js", "assets/design-system/**" ]
consumes = [
  { kind = "og",      path = "example-plugins/og.png" },
  { kind = "og_meta", html = "example-plugins/index.html", rel = "./og.png" },
]
```

Schema:
- `schema_version` — currently `1`.
- `[trigger].paths` — global UX globs; a match invalidates every target unless a
  target sets its own `trigger` list.
- `[[target]]` — `id` (required, unique), `route` (`native` | `web`, required),
  optional `trigger` (per-target override), plus route-specific keys
  (`tool`/`size`/`scale`/`backend` for native; `url`/`device_scale_factor` for
  web) that document how to re-capture.
- `consumes[]` — `{ kind, path }` or `{ kind, html, rel }`:
  - `readme` / `gallery` — the PNG a markdown/gallery references.
  - `og` — the PNG the page's `og:image` / `twitter:image` point at.
  - `og_meta` — the HTML head that must declare the OG/Twitter block pointing at
    that og.png (this is what backfills a demo missing OG images).

The gate verifies the `readme` / `gallery` / `og` **PNG paths** were refreshed
when a trigger fired; `og_meta` is documentation for the head-template edit.

## 1. Identify stale targets

```bash
python3 tools/scripts/screenshot_sync_check.py --base origin/main --mode=report
```

It lists each `[[target]]` a diff invalidated. Read the hook output if it already
fired. A token change lists every target.

## 2. Route A — native README / editor shots

For a plugin's native editor rendered to a PNG with no window. See the
`screenshot` skill for backend selection; the essentials:

- Prefer a per-editor dumper target (the `pulp-bitches-brew` repo's
  `ui/brew_shots.cpp` → `brew-shots` pattern; each plugin repo copies it as `pulp-<name>-shot`,
  named by the manifest `tool =`), or `pulp-screenshot --demo` for SDK demos.
- **Skia backend for asset-heavy UIs** — the CoreGraphics canvas can't composite
  file images and renders filenames as placeholder text (the classic
  "broken import" that is really just the backend).
- **`gpu` backend (offscreen Dawn+Skia `HeadlessSurface`)** for
  `requires_gpu_host()` views and any headless/CI/SSH capture — never the live
  `WindowHost::capture_png()` headless (it needs an interactive WindowServer
  session and hangs).
- Match the native editor baseline **scale 2.0** (`test_editors.cpp` in the
  example repos) so README shots align with the visual-regression baselines.
- Assert `ScreenshotStats::passes_content_floor` — never commit a blank /
  near-blank PNG (a "file written" check misses the empty-frame bug).
- Write each PNG to its `consumes.kind = readme|gallery` path.

`pulp-screenshot` is an Apple-only target; the per-editor dumper is the portable
route and must be added to each plugin repo.

## 3. Route B — WAM / WCLAP web demo OG + gallery

The WAM/WCLAP plugin UI is a WASM build running in a browser, so the faithful
capture is a real browser rendering the demo page — **not** native
`render_to_png`. Pulp already ships this path (playwright-core against the system
Chrome, no browser download); the OG generators reuse it.

**Convention (match WAM exactly).** WAM's OG image is a **device-scale-2
element screenshot of the demo page itself**, saved as a colocated `og.png`:
- per-plugin page → screenshot the started editor panel (`#panel`) — ~1280×1234
- gallery page → screenshot the card grid (`.wrap`) — ~1720×2002

The reference generators:
- WAM (GitHub Pages, single gallery): `web/gen-og-images.mjs` (renders each page,
  presses the player's `window.__start()` seam, waits for `window.__demo.started`,
  screenshots the element at `deviceScaleFactor: 2` with `--mute-audio
  --autoplay-policy=no-user-gesture-required`) + `web/gen-og.mjs` (injects the
  `og:image`/`twitter:card=summary_large_image` block only when a sibling `og.png`
  exists) — both in `pulp-example-plugins` / `pulp-classic-effects`.
- WCLAP (Cloudflare Pages, combined `/example-plugins/` + `/classic-effects/`
  galleries): generated by
  `examples/web-demos/wclap-build/cloudflare/assemble-gallery.mjs` in pulp core.
  Its `pluginPage()` / `galleryPage()` emit the OG head block; the sibling
  `gen-og-images.mjs` renders each page's `og.png`. WCLAP pages need
  **cross-origin isolation (COOP/COEP)** to instantiate the threaded-wasm shared
  memory, so serve them with `cloudflare/serve-headers.mjs` (which applies the
  `_headers` rules), never a plain static server.

Steps:
1. Build/assemble the demo Pages tree (`assemble-gallery.mjs` for WCLAP; the
   per-plugin `.wasm` modules are the only heavyweight input — they can be
   rebuilt with wasi-sdk or fetched from the live deploy).
2. Serve it (WCLAP: `serve-headers.mjs` for COOP/COEP).
3. Screenshot each page at `deviceScaleFactor: 2`, waiting on the ready
   predicate. Do NOT resume audio for its own sake — the demos are
   click-to-start; if a shot needs the started editor, drive the `__start()`
   seam under `--mute-audio` (see the local-dev audio etiquette in CLAUDE.md).
4. Save as colocated `og.png`; assert it is non-blank before trusting it.
5. Ensure the page head declares `og:image` / `og:url` / `twitter:card` /
   `twitter:image` → the colocated og.png (the `og_meta` consume). Edit the
   shared page template once, not each page by hand.

## 4. Consistency + backfill

- Both galleries mirror the SAME example-plugin repos, so keep the OG head block
  and the og.png-generation step a **shared template** (WAM: `gen-og.mjs`'s
  `ogBlock()`; WCLAP: `assemble-gallery.mjs`'s page emitters) so WAM and WCLAP
  can't drift.
- Backfilling a demo that lacks OG images = adding the `og:image` / `og:url` /
  `twitter:card=summary_large_image` / `twitter:image` lines to the page
  template + generating the colocated og.png. Social crawlers fetch the raw
  HTML and do NOT run JS, so the tags must be present at rest.

## 5. Verify + hand off

- Re-run `screenshot_sync_check.py --mode=report` → clean.
- Vision-probe before reading any capture for a visual judgment (screenshot skill).
- Commit the PNGs + any head-template edit in the SAME PR as the UX change, or
  add a bypass trailer on the tip commit:

  ```
  Screenshot-Sync: skip target=<id|all> reason="..."
  ```

  Legitimate uses: a provably-invisible refactor, a docs-only touch that trips a
  glob, or a batch whose shots regenerate in a follow-up. Same philosophy as
  `Skill-Update: skip` / `Version-Bump: skip`.

## The gate (three layers, mirrors skill-sync)

`tools/scripts/screenshot_sync_check.py` reads the repo's `.pulp/screenshots.toml`,
diffs the change range against `[trigger].paths`, and checks each triggered
target's consumed PNGs were refreshed (or bypassed). It DETECTS staleness; it
does not capture (capturing is heavy + non-deterministic across machines — the
re-shoot is the explicit step above). Wired exactly like the skill-sync gate:

| Layer | Where | Mode |
|-------|-------|------|
| 1 hint | PostToolUse (`hooks/scripts/cli-plugin-sync.sh`) | `--mode=hint` — advisory |
| 2 report | `.githooks/pre-push` | `--mode=report` — enforcing (demote with `PULP_DISABLE_PREPUSH_GATES=1`) |
| 3 CI | `.github/workflows/version-skill-check.yml` | `--mode=report` — authoritative, no env demotion |

The check script ships from pulp core and is installed into consumer repos the
same way the githooks are (`tools/scripts/install-githooks.sh`); the trigger
map is repo-local (`.pulp/screenshots.toml`), because each plugin repo has its
own layout.

## Gotchas

- **A NEW page must be registered in `gen-og-images.mjs` by hand.** Only the
  *galleries* auto-discover their demos (they read the `plugins = [...]` array back
  out of the assembled `index.html`). Every non-gallery page — the standalone pages,
  `super-convolver`, `super-convolver-gpu` — is a **hardcoded entry**. Add a page to
  `assemble-gallery.mjs` without adding it there and `assemble-gallery.mjs` still
  bakes an absolute `og:image` URL for it, pointing at a PNG that nobody ever
  generated. And **Cloudflare serves a missing asset as its 404 page with HTTP 200**,
  so this does not fail as a broken link — it fails as a **blank unfurl**, and a
  naive `curl -w %{http_code}` check says `200` and confirms the lie. That is the
  exact bug that shipped. `check-og-images.mjs` (file exists, real PNG magic bytes,
  aspect ≈ 1.91:1) and `check-unfurl.mjs` (fetch the LIVE URL as a crawler and
  follow the `og:image`) are the backstops — but the fix is to register the page.
  Doubly easy to miss when the page is emitted **conditionally**, like
  `super-convolver-gpu`, which only appears when its GPU DSP build tree exists.
- **Cache-bust the main-thread player entry ONLY — never the worklet processor
  or the DSP module.** An AudioWorklet processor's *registered name* is derived
  from the URL it was added from, so the main thread and the worklet must agree
  on exactly one URL. A `?v=` on either side forks the name and the node never
  constructs — the page loads, the UI paints, and there is simply no audio. The
  corollary bites in the other direction too: **every module the `?v=` IS stamped
  on must be an input to the hash** (`assemble-gallery.mjs`'s `playerVersion()`
  hashes the shell, widgets, both adapters, and the UI entry). Add a bustable
  module without adding it to the hash and its next change ships behind a stale
  cache entry — an OG shot regenerated against a page that is still serving the
  old bundle is the worst version of this, because it looks correct.
- **A page's UI module may be optional; a screenshot of the fallback is not a
  bug.** The gallery assembler skips the both-ABI SuperConvolver section entirely
  when the WAM build tree (`--wam-build`) is absent, and builds pages with **no
  custom UI** when the wasm UI module (`--ui-build`) is absent — the player then
  renders its generated parameter grid. A browser with no WebGL2 context lands on
  that same grid at runtime. So before re-shooting a demo, confirm which UI the
  page actually mounted; otherwise you will capture the parameter grid, commit it
  as the plugin's og.png, and have "wrong" screenshots with a green build.
- **CoreGraphics can't composite file images** → filename-as-text placeholders;
  use the Skia backend for anything with assets.
- **Headless GPU** → the offscreen `gpu` backend, not the live host (which hangs
  headless).
- **WCLAP needs cross-origin isolation** (COOP/COEP) to instantiate; WAM does
  not — different serve step per route. Serve WCLAP with `serve-headers.mjs`.
- **Social crawlers don't run JS** — OG tags must be static in the head at rest.
- **Demo-site HTML/OG templates for WAM live in the consumer repos**
  (`pulp-example-plugins` / `pulp-classic-effects` `web/gen-og*.mjs`); the WCLAP
  combined-site template lives in pulp core (`cloudflare/assemble-gallery.mjs`).
- **This is a porting/consistency tool.** It re-shoots the repo's own UI. Never
  fabricate or copy another product's screenshots.
