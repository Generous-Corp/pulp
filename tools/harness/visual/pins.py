"""Pinned visual-harness dependency metadata.

These constants mirror ``tools/deps/manifest.json`` so tests and tooling can
fail early when the deterministic rendering stack drifts without an explicit
manifest update.

Skia is pinned at chrome/m157 via the danielraffel/skia-builder fork
(see manifest.json determinism.skia_builder_fork). The fork tracks
upstream olilarkin's tag pattern and adds iOS/visionOS/mac-x86_64
slices upstream does not. ``SKIA_COMMIT`` and ``SKIA_BUILDER_REF`` bind the
published archive to the exact Skia source and skia-builder workflow revision.
"""

from __future__ import annotations

SKIA_BRANCH = "chrome/m157"
SKIA_COMMIT = "b94213a11b3a2edadfae89f0c57a7f4ab72772c9"
SKIA_BUILDER_REF = "f710b2834eaf37dbf1bd39408f9361d10d85eda2"
SKIA_BUILDER_FORK = "https://github.com/danielraffel/skia-builder"
SKIA_PYTHON_SMOKE_VERSION = "144.0.post2"

FONT_SHA256 = {
    "external/fonts/Inter-Regular.ttf": (
        "40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82"
    ),
    "external/fonts/JetBrainsMono-Regular.ttf": (
        "a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f"
    ),
}

RELEASE_ASSET_SHA256 = {
    # The chrome/m157 release ships linux-arm64 alongside the other slices; must
    # stay in lockstep with manifest.json (guarded by
    # test_skia_linux_arm64_asset.py).
    "linux-arm64": (
        "e591268124be249b2a2f56c5042e1e7865bd04556d509c6e4937a04251964435"
    ),
    "linux-x64": (
        "bf74ae1bfd5abd9f49e57c3c6ae6d94c580bbbfe575256fa5077272e1ce0c187"
    ),
    # The mac-arm64 key selects the verified universal archive: its arm64 Dawn
    # slice retains the verified 13.0 deployment target.
    "mac-arm64": (
        "bb24c74f05699fa084df6d68ecb9a427802c3df49ce85d8a7dc696aeb9e19a86"
    ),
    "mac-universal": (
        "bb24c74f05699fa084df6d68ecb9a427802c3df49ce85d8a7dc696aeb9e19a86"
    ),
}


# Recorded identity of the committed raster goldens, keyed by fixture id. The
# byte-for-byte golden lives beside its fixture; this digest is the reviewable
# copy of that identity, so a golden rewritten by mistake shows up as a digest
# change in the diff rather than as an opaque binary blob.
#
# Produced by the pinned skia-python wheel (SKIA_PYTHON_SMOKE_VERSION) rendering
# the fixture's declared ops. A different pin renders different bytes, which is
# why the loader refuses to render with an unpinned Skia.
RASTER_GOLDEN_SHA256 = {
    "canvas2d/raster-determinism": (
        "9d6b81079fbbb5ac26fe31d4af8d0b011da661fda4a894b9717f38d32c27add8"
    ),
}

# Hosts on which the digests above have actually been observed, keyed the way
# raster.platform_key() reports them.
#
# darwin-arm64 and darwin-x86_64 were both measured directly: the same pinned
# wheel, rendering the same fixture, in separate processes, produced identical
# bytes on each. That settles the instruction-set axis on Apple silicon (the
# x86_64 reading was taken under Rosetta translation).
#
# linux-x86_64 is deliberately absent. No Linux execution path existed on the
# host where the golden was recorded, so cross-operating-system byte identity is
# UNPROVEN: fontconfig/CoreText, libc, and the wheel's own build differ between
# the two, and any of them could change the raster. The Linux lane of the visual
# harness compares against the same golden and reports its computed digest, so
# the first Linux run settles the question in one direction or the other.
RASTER_GOLDEN_VERIFIED_PLATFORMS = ("darwin-arm64", "darwin-x86_64")
