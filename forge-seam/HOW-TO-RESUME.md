# Resuming this work

The Forge worktree lives at `/tmp/forge-cur` and **`/tmp` does not survive a
reboot.** Everything durable is here in `forge-seam/`. To rebuild the working
state:

```bash
cd /Volumes/Workshop/Code/forge
git fetch origin
git worktree add /tmp/forge-cur origin/main        # 7f0999a when this was written
cd /tmp/forge-cur
git apply /Volumes/Workshop/Code/pulp-modular-rack/forge-seam/patches/0001-*.patch
cp -r /Volumes/Workshop/Code/pulp-modular-rack/forge-seam/test/* test/
# register the no-leak test in CMakeLists.txt (see ../README.md)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Code/pulp-sdk-forge-aumi"
cmake --build build --target forge-test-chrome-no-leak forge-test-chrome -j 8
./build/forge-test-chrome-no-leak     # expect 3/3
./build/forge-test-chrome             # expect 4711 assertions
```

**Do not work in `/Volumes/Workshop/Code/forge` directly.** Its working tree has
unresolved conflict markers in `src/chrome.cpp` (`UU`), and its local `main` is
431 commits behind `origin/main` — building from it produced two passes of wrong
visual work before that was noticed.

## Facts that cost time to find

- **`ForgeFxShell` is `final`.** No test double by subclassing it; that is why the
  live-path proofs here were manual. `ForgeModularShell` will be the first real
  `ForgeShell` subclass and makes them permanent.
- **`ForgeChrome` has no virtuals** and takes `ForgeShell&`. The extension point
  is the shell, never the chrome.
- **`ForgeShell`'s 14 virtuals are all DSP** — `process_audio`, `has_build`,
  `macro_descriptors`, `install_generated_bundle`, `current_sample_rate`. None
  are UI. That is why the seam had to be added rather than found.
- **`design_tokens.hpp` is the palette's source of truth**, not `chrome.cpp`.
  Lines are translucent (`#DCE8FA1F`), the hero is 42, `surface_sunken` exists.
- **Forge MIDI has no Standalone target** (CLAP + AU only), which is why the
  no-leak guard renders chrome directly instead of screenshotting apps.
- **`icon_kind` is not declared** where the composer row builder sits.
- **The product name is already config-driven**: `FORGE_IDENTITY_PRODUCT_NAME`,
  `FORGE_IDENTITY_MARK` and friends are `if(NOT DEFINED)` overrides in
  `cmake/ForgeIdentity.cmake`.

## Where the phases stand

| Phase | State |
|---|---|
| 0 — no-leak guard | **done**, fails on a 1px shared change, passes when reverted |
| 1 — the seam | **done**, 3 changes, 3 products byte-identical, each proven live |
| 2 — Forge Modular runs Forge's UI | next |
| 3–8 | not started; see `../SPEC-forge-modular.md` |
