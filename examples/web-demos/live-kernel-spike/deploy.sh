#!/usr/bin/env bash
# Pulp Live — M1 — stage the self-contained static site and deploy to Cloudflare
# Pages WITHOUT any COOP/COEP headers (the whole point: the single-thread kernel
# needs no cross-origin isolation, so it runs on plain static hosting).
#
# The staged site is FLAT — editor.html becomes index.html and every asset path
# is relative — so the exact same folder is also GitHub-Pages-ready: drop it at a
# repo path and it works with zero configuration, no _headers, no server.
#
# Usage: ./deploy.sh [--project NAME] [--branch BRANCH]
#   (requires: emsdk active for build.sh, and `wrangler` authed as the owner)
set -euo pipefail

SPIKE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="pulp-live-compiler"
BRANCH="main"
while [ $# -gt 0 ]; do
  case "$1" in
    --project) PROJECT="$2"; shift 2 ;;
    --branch)  BRANCH="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

# 1. ensure the kernel wasm exists (reproduce with build.sh if not)
if [ ! -f "$SPIKE_DIR/dist/lk_kernel.wasm" ]; then
  echo "building kernel wasm ..."
  bash "$SPIKE_DIR/build.sh"
fi

# 2. stage a flat, self-contained site
SITE="$(mktemp -d)/pulp-live"
mkdir -p "$SITE/dist"
cp "$SPIKE_DIR/editor.html"    "$SITE/index.html"
cp "$SPIKE_DIR/editor.mjs"     "$SITE/editor.mjs"
cp "$SPIKE_DIR/lk-dsl.mjs"     "$SITE/lk-dsl.mjs"
cp "$SPIKE_DIR/lk-worklet.js"  "$SITE/lk-worklet.js"
cp "$SPIKE_DIR/dist/lk_kernel.wasm" "$SITE/dist/lk_kernel.wasm"
# NOTE: deliberately NO _headers file — proving no COOP/COEP is required.

echo "staged flat site at: $SITE"
ls -la "$SITE"

# 3. deploy to Cloudflare Pages (no headers)
echo "deploying to Cloudflare Pages project '$PROJECT' (branch '$BRANCH') ..."
wrangler pages deploy "$SITE" --project-name "$PROJECT" --branch "$BRANCH" --commit-dirty=true
