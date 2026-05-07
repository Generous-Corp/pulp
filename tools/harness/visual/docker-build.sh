#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

image="${PULP_VISUAL_IMAGE:-pulp-visual-harness}"
platform="${PULP_VISUAL_DOCKER_PLATFORM:-linux/amd64}"
progress="${PULP_VISUAL_DOCKER_PROGRESS:-plain}"
cache_root="${PULP_VISUAL_DOCKER_CACHE:-${HOME}/.cache/pulp/visual-harness/buildx}"
cache_parent="$(dirname "${cache_root}")"

mkdir -p "${cache_parent}"
next_cache="$(mktemp -d "${cache_root}.new.XXXXXX")"

cleanup() {
  rm -rf "${next_cache}"
}
trap cleanup EXIT

build_cmd=(
  docker buildx build
  --load
  --platform "${platform}"
  --progress "${progress}"
  -f "${repo_root}/ci/visual-harness.Dockerfile"
  -t "${image}"
  --cache-to "type=local,dest=${next_cache},mode=max"
)

if [ -f "${cache_root}/index.json" ]; then
  build_cmd+=(--cache-from "type=local,src=${cache_root}")
fi

build_cmd+=("$@" "${repo_root}")
"${build_cmd[@]}"

rm -rf "${cache_root}"
mv "${next_cache}" "${cache_root}"
trap - EXIT

echo "visual harness image: ${image}"
echo "visual harness cache: ${cache_root}"
