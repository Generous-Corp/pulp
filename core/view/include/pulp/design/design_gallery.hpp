#pragma once

// pulp::design — design-system gallery: card model + review-artifact emitters.
//
// A "gallery card" is any example/kit source file that opts in with a first-line
// magic comment. There is no registry to keep in sync — the tag lives in the
// file, so a card is discovered by a grep of the source tree and survives a
// move or rename:
//
//   // @dsCard group=knobs viewport=120x120
//   // @startingPoint
//
// `pulp design gallery` walks a root, parses these tags, renders each card at
// its declared viewport (via the screenshot tool, Skia backend), and emits one
// review artifact per pass — a deterministic JSON manifest plus a self-contained
// HTML grid grouped by `group`. That standing artifact replaces hunting loose
// PNGs after an import/design pass, and doubles as a visual-regression inventory.
//
// This translation unit is deliberately pure: it parses the card model and
// serializes the manifest/HTML, but does no filesystem or render work. The CLI
// owns the walk, the render shell-out, and the content-hash cache; it hands the
// emitters a `png_rel` callback so this layer never learns about paths on disk.
// Kept separate for the same reason design_fidelity_ledger.cpp is separate from
// the checks: the pure model stays trivially testable and free of GPU/IO.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::design {

/// Format id stamped into the manifest JSON; bump on a non-additive shape change.
inline constexpr std::string_view kGalleryFormatVersion = "2026.07-design-gallery-v1";

/// The group a card falls into when its tag omits `group=`.
inline constexpr std::string_view kGalleryUngrouped = "ungrouped";

/// One tagged card discovered by its leading `@dsCard` magic comment.
struct GalleryCard {
    std::string file;             ///< path as discovered (relative to the scan root)
    std::string group;            ///< `group=<name>`; empty tag value -> kGalleryUngrouped
    int width = 0;                ///< viewport width in points (from `viewport=WxH`)
    int height = 0;               ///< viewport height in points
    bool starting_point = false;  ///< `@startingPoint` present -> a `pulp create --from` seed
    std::string content_hash;     ///< gallery_content_hash() of the source bytes; caller-filled
};

/// Parse the leading magic comments of one source file. `file_head` is the first
/// few lines (the CLI reads a bounded prefix). Returns nullopt when there is no
/// `@dsCard` tag carrying a valid `viewport=WxH` — an untagged or malformed file
/// is simply not a card, never an error. `group=` is optional (defaults to
/// kGalleryUngrouped); `@startingPoint` anywhere in the head sets the flag. The
/// returned card's `content_hash` is left empty for the caller to fill.
std::optional<GalleryCard> parse_gallery_card(std::string_view file_head,
                                              std::string_view path);

/// Deterministic 64-bit FNV-1a hash (lowercase hex) of a card's source bytes.
/// The render cache is keyed on (file, viewport, this hash): unchanged source at
/// an unchanged viewport reuses the prior PNG instead of re-rendering. Exposed so
/// the CLI and the tests share exactly one algorithm.
std::string gallery_content_hash(std::string_view bytes);

/// Stable order: by group, then by file. The gallery diffs cleanly across passes.
void sort_cards(std::vector<GalleryCard>& cards);

/// Deterministic JSON manifest: format_version, a total, and one entry per group
/// (sorted, with a per-group count) listing its cards. `png_rel(card)` yields the
/// rendered PNG path recorded for a card, or "" when it was not rendered — this
/// function itself touches no filesystem.
std::string gallery_manifest_json(
    const std::vector<GalleryCard>& cards,
    const std::function<std::string(const GalleryCard&)>& png_rel);

/// Self-contained HTML review page: one section per group, each card shown at its
/// declared viewport with its rendered PNG (via `png_rel`) and a starting-point
/// badge. No external assets or scripts, so it opens straight from disk. A card
/// whose `png_rel` is empty renders a "not rendered" placeholder rather than a
/// broken image.
std::string gallery_html(
    const std::vector<GalleryCard>& cards,
    const std::function<std::string(const GalleryCard&)>& png_rel);

}  // namespace pulp::design
