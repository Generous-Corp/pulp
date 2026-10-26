#pragma once

/// @file paragraph_cache.hpp
/// Process-wide cache of laid-out SkParagraphs for SkiaCanvas text.
///
/// Every `fill_text` / `stroke_text` that goes through SkParagraph shapes and
/// lays out its run, and a Label paints its text on every frame, so a static
/// panel re-shapes every label on every paint. The canvas is constructed per
/// frame, which is why the cache is process-wide rather than per canvas.
///
/// The key is everything the paragraph build reads: the text, the resolved
/// family stack, the primary typeface, size, weight, slant, letter spacing,
/// direction, OpenType features, and `font_registration_generation()`. A font
/// registration therefore makes every earlier entry unreachable, and the next
/// lookup drops them. The paragraph is laid out at infinite width with left
/// alignment and no strut or decoration, so wrap width, text-align and line
/// height are not inputs; the caller aligns the run.
///
/// The fill or stroke paint is not part of the key. A cached paragraph is
/// built without a foreground paint and drawn through a painter that applies
/// the caller's paint to each glyph run, so one entry serves every colour,
/// gradient, shadow and filter the same text is drawn with. Updating a
/// paragraph's foreground paint in place would not work: SkParagraph caches
/// its text blobs, with their paints, on the first paint.
///
/// Bounded by entry count and by an estimate of each entry's CPU heap
/// footprint (the GPU text-blob cache keeps its own budget). Eviction is a
/// segmented LRU: a new entry starts on probation and is protected once it is
/// looked up again, and probation is evicted first. Text that changes every
/// frame, such as a meter readout, therefore cycles through probation without
/// pushing out the labels that are repainted unchanged.
///
/// Windows can paint on different threads, so the map is guarded by a mutex
/// and every use of an entry holds that entry's own mutex: SkParagraph
/// mutates itself while painting and in queries. Entries are destroyed outside
/// the map lock.

#ifdef PULP_HAS_SKIA

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace pulp::canvas {

class ParagraphCache {
  public:
    static ParagraphCache& instance();

    struct Stats {
        std::uint64_t hits = 0;            ///< Lookups served without building.
        std::uint64_t builds = 0;          ///< Paragraphs built (miss or cache disabled).
        std::uint64_t evictions = 0;       ///< Entries dropped for the entry or byte bound.
        std::uint64_t invalidations = 0;   ///< Entries dropped by a font registration.
        std::size_t entries = 0;           ///< Live entries.
        std::size_t protected_entries = 0; ///< Live entries looked up since they were built.
        std::size_t bytes = 0;             ///< Estimated heap footprint of live entries.
        std::size_t max_entries = 0;
        std::size_t max_bytes = 0;
    };
    Stats stats() const;
    void reset_stats();

    /// Drop every entry.
    void clear();

    /// When false, every lookup builds a fresh paragraph and nothing is stored,
    /// which is the uncached path for A/B comparison.
    void set_enabled(bool enabled);
    bool enabled() const;

    /// Bound the cache. Shrinking evicts immediately.
    void set_limits(std::size_t max_entries, std::size_t max_bytes);

    /// Called with the text of every paragraph built, cached or not, so a
    /// benchmark can tell which runs a frame shaped. Pass an empty function to
    /// remove it. Runs on the painting thread, outside the cache's lock.
    using BuildObserver = std::function<void(std::string_view text)>;
    void set_build_observer(BuildObserver observer);

    /// Estimated CPU heap footprint of one entry holding @p text_bytes of
    /// UTF-8, fitted to measured SkParagraph allocations with headroom.
    static std::size_t estimated_entry_bytes(std::size_t text_bytes) noexcept;

    // About 1000 short labels fill 16 MiB, so the two bounds bind for short and
    // long text respectively.
    static constexpr std::size_t kDefaultMaxEntries = 1024;
    static constexpr std::size_t kDefaultMaxBytes = 16u * 1024u * 1024u;

  private:
    ParagraphCache() = default;
};

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
