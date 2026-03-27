# Animation Worktree Build Investigation

Date: 2026-03-27
Subject: `/Users/danielraffel/worktrees/pulp-animation-tweening-and-bridge`
Branch: `feature/pulp-animation-tweening-and-bridge`
Scope: investigate the quoted build diagnosis; no source changes

## Question

The worktree branch was reported as having a build problem explained like this:

- the main repo has `catch2-src` and `choc-src` but no `webgpu-*` dirs
- therefore the main repo does not really fetch Dawn/WebGPU
- the worktree is fetching Dawn with `GIT_TAG main` and failing
- quick fix: configure the worktree with GPU disabled because the animation work does not need GPU

The goal of this note is to determine what is true, what is misleading, and what advice should be given to the agent.

## Findings

### 1. The claim about the main repo not fetching WebGPU is false

Main repo evidence:

- top-level `CMakeLists.txt` declares `webgpu` via `FetchContent`
- main repo `build/CMakeCache.txt` reports `PULP_ENABLE_GPU:BOOL=ON`
- main repo has `build/_deps/webgpu-src`

Observed on disk in the main repo:

- `build/_deps/webgpu-src`
- `build/_deps/webgpu-build`
- `build/_deps/webgpu-subbuild`

So the statement “the main repo has no webgpu dirs” is simply wrong for the current repo state.

### 2. The animation worktree is already configured with GPU disabled

In the animation worktree:

- `build/CMakeCache.txt` reports `PULP_ENABLE_GPU:BOOL=OFF`
- `build/_deps/` contains `catch2`, `choc`, `clap`, `lv2`, and `sdl3`
- there are no `webgpu-*` directories in that specific build tree

That does **not** mean the project does not use WebGPU.

It means:

- this specific build directory was configured with GPU off
- therefore CMake did not fetch `webgpu` for that build

So the quoted “quick fix” was not just a suggestion. It has already happened in the current worktree build tree.

### 3. The worktree currently builds successfully with GPU off

I ran:

```bash
cmake --build build -j4
```

in the worktree, and it completed successfully.

I also ran targeted tests:

```bash
ctest --test-dir build --output-on-failure -R 'animation|frame-clock|widget-animation|widget-bridge|theme'
```

These passed.

So the current state is:

- the worktree is not presently blocked on the GPU fetch problem in its existing `build/` tree
- the CPU/non-GPU build is already good enough to compile and run the relevant new tests

### 4. The fetch diagnosis is partly directionally right, but too simplistic

Two parts are reasonable:

- using `GIT_TAG main` for `webgpu` is not reproducible and can be flaky
- if the branch only changes view/theme/animation logic, GPU-off can be a temporary build-unblock tactic

But the quoted diagnosis still overstates and confuses a few things:

1. It confuses “this build dir has GPU disabled” with “the project does not fetch WebGPU.”
2. It ignores that the main repo already has a working GPU-enabled build cache.
3. It treats the fetch problem as the current blocker, even though the worktree now builds.
4. It implies the top-level `webgpu` fetch is the whole story, but `WebGPU-distribution` also pulls platform runtime artifacts internally.

### 5. Reusing the main repo's fetched WebGPU source is a real option

I tested a fresh configure in a separate build directory using:

```bash
cmake -S . -B build-gpu-check \
  -DPULP_ENABLE_GPU=ON \
  -DFETCHCONTENT_SOURCE_DIR_WEBGPU=/Users/danielraffel/Code/pulp/build/_deps/webgpu-src \
  -DFETCHCONTENT_UPDATES_DISCONNECTED_WEBGPU=ON
```

That got past the original “fetch webgpu repo” step and reached normal dependency population.

This is useful because it shows a better path than “just turn GPU off forever”:

- keep the current CPU build for iteration if needed
- use a separate GPU-enabled build dir for real GPU validation
- reuse known-good fetched source from the main repo where appropriate

## What Is True

- The top-level `webgpu` dependency is currently pinned to `GIT_TAG main`, which is brittle.
- The worktree `build/` directory is configured with `PULP_ENABLE_GPU=OFF`.
- With GPU off, the animation branch currently builds and the relevant new tests pass.

## What Is Not True

- It is not true that the main repo “does not fetch Dawn/WebGPU.”
- It is not true that the worktree is currently blocked on the GPU fetch issue in its existing `build/` directory.
- It is not correct to infer repo-wide behavior from one worktree build cache that already has GPU disabled.

## Recommendation

Do **not** advise the agent to treat “GPU off” as the final answer if the branch is meant to support animation in a GPU-oriented UI framework.

Instead, advise the agent like this:

### Near-term advice

1. Keep the current `build/` directory as the fast iteration path.
   - It already has `PULP_ENABLE_GPU=OFF`
   - it builds successfully
   - the new animation-related tests pass there

2. Be honest about what that proves.
   - It proves the animation/view/theme/bridge logic compiles and passes tests in the non-GPU path.
   - It does **not** prove GPU-enabled rendering paths are validated.

3. Do not globally disable GPU in source as part of this branch.
   - If GPU is off, it should remain a configure-time choice, not a permanent source-level retreat.

### GPU validation advice

If the agent wants to validate that the branch also configures against the GPU path, tell them to use a fresh build dir rather than mutating the current one:

```bash
cmake -S . -B build-gpu \
  -DPULP_ENABLE_GPU=ON \
  -DFETCHCONTENT_SOURCE_DIR_WEBGPU=/Users/danielraffel/Code/pulp/build/_deps/webgpu-src \
  -DFETCHCONTENT_UPDATES_DISCONNECTED_WEBGPU=ON
```

Then build only what is needed.

This is preferable because:

- the existing `build/` tree is already cached as GPU-off
- the main repo already has a known-good fetched `webgpu-src`
- it avoids pretending that GPU support is irrelevant

### If GPU-on still fails

If the GPU-enabled configure/build still fails after reusing the known-good fetched source:

- treat that as a separate build-hygiene issue
- document it honestly
- do not let it erase the fact that the core animation branch itself already builds and tests in the CPU path
- decide separately whether the branch should be merged as “animation foundation” or held for GPU validation

## What To Tell The Agent

Recommended message:

> Your diagnosis is partly off. The main repo absolutely does fetch WebGPU/Dawn; it already has a GPU-enabled build cache with `build/_deps/webgpu-src`, and `PULP_ENABLE_GPU` is ON there. In your worktree, the existing `build/` tree is already configured with `PULP_ENABLE_GPU=OFF`, and that build now succeeds, so the old fetch failure is not the current blocker. Keep using the current `build/` tree for fast iteration and tests, but don’t present that as final GPU validation. If you want to validate GPU as well, use a fresh build dir and point `FETCHCONTENT_SOURCE_DIR_WEBGPU` at the main repo’s cached `webgpu-src` rather than flipping the whole branch into “GPU doesn’t matter.”

## Practical Conclusion

The right interpretation is:

- **for iteration:** GPU-off is acceptable in the current worktree build dir
- **for correctness of messaging:** do not say the project does not use/fetch WebGPU
- **for final branch quality:** do not confuse “builds with GPU off” with “GPU path is validated”

That is the line I would hold with the agent.
