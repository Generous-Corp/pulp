"""Pinned visual-harness dependency metadata.

These constants mirror ``tools/deps/manifest.json`` so tests and tooling can
fail early when the deterministic rendering stack drifts without an explicit
manifest update.

Skia is pinned at chrome/m153 via the danielraffel/skia-builder fork
(see manifest.json determinism.skia_builder_fork). The fork tracks
upstream olilarkin's tag pattern and adds iOS/visionOS/mac-x86_64
slices upstream does not. ``SKIA_COMMIT`` and ``SKIA_BUILDER_REF`` bind the
published archive to the exact Skia source and skia-builder workflow revision.
"""

from __future__ import annotations

SKIA_BRANCH = "chrome/m153"
SKIA_COMMIT = "8b8c3872fbc03f025855db96ce683f34ec98a815"
SKIA_BUILDER_REF = "1f8c8d2c343f360a653bce92d11f8ded9a515208"
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
    # The chrome/m153 release ships linux-arm64 alongside the other slices; must
    # stay in lockstep with manifest.json (guarded by
    # test_skia_linux_arm64_asset.py).
    "linux-arm64": (
        "a829984ce35141ac1e8f608e29496f69ad24bd2e5215f2899a071dc6c2e0ed0e"
    ),
    "linux-x64": (
        "b132db47979f116a2b35720c6e4e8c7128505499e52b218cc64546f87b0bb363"
    ),
    # The mac-arm64 key selects the verified universal archive: its arm64 Dawn
    # slice retains the verified 13.0 deployment target.
    "mac-arm64": (
        "0ebfe03a209ceefe47edfeae70c3cc6c499583b74f35a26140ea55bad7f1e5a9"
    ),
    "mac-universal": (
        "0ebfe03a209ceefe47edfeae70c3cc6c499583b74f35a26140ea55bad7f1e5a9"
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
