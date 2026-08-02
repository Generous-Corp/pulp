#pragma once

/// @file graphite_image_provider.hpp
/// The Recorder-scoped `skgpu::graphite::ImageProvider` Pulp installs so that a
/// raster-backed `SkImage` reaching Graphite is uploaded instead of dropped.
///
/// Graphite never uploads a non-Graphite-backed image on its own. Every time it
/// meets one while building a paint key it calls
/// `Recorder::clientImageProvider()->findOrCreate()`, and Skia's default
/// provider returns nothing — so the draw is discarded and the only trace is a
/// `[skia] WARNING - Couldn't convert SkImage to a Graphite-backed
/// representation` line. The draw call itself still reports success, which is
/// why the failure reads as "the renderer lost my artwork" rather than an error.
///
/// Pulp's own entry points (`draw_image_from_*`, fill/stroke patterns, CSS mask
/// images) pre-upload through `SkiaCanvas::ensure_gpu_image`, but that only
/// covers images Pulp itself constructs. Skia modules that build images
/// internally cannot be intercepted that way: `SkSVGDOM` base64-decodes
/// `<image href="data:...">` into a raster `SkImage` and draws it straight to
/// the canvas, so an SVG's embedded artwork rendered on the raster backend and
/// vanished on the GPU one. This provider closes the class of bug rather than
/// the instance — anything that reaches Graphite as raster now gets uploaded.
///
/// Ownership: one provider per `Recorder`, owned by the `SkiaSurface` that owns
/// that `Recorder`. Every cached texture therefore belongs to the Recorder that
/// created it, which is what makes the single-threaded assumption in the
/// `ImageProvider` docs hold — a provider is never shared between Recorders.
/// The owner must call `clear()` before the Recorder is destroyed so the cached
/// GPU images are released while their Recorder is still alive.

#ifdef PULP_HAS_SKIA

#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkRefCnt.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/ImageProvider.h"
#include "include/gpu/graphite/Recorder.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <utility>

namespace pulp::render {

class GraphiteImageProvider final : public skgpu::graphite::ImageProvider {
public:
    /// Upload results are cached so a static asset drawn every frame — an SVG
    /// backdrop, a sprite sheet — is uploaded once rather than per frame.
    /// Eviction is least-recently-used and bounded on both axes: a count, so a
    /// stream of tiny images cannot grow the map without limit, and a byte
    /// budget, because a single design backdrop can be 2560x1680 (~17 MB) and a
    /// count-only cap would let a handful of them pin hundreds of megabytes of
    /// GPU memory. Whichever bound binds first wins; one entry is always kept so
    /// an image larger than the whole budget still draws.
    static constexpr std::size_t kDefaultCapacity = 32;
    static constexpr std::size_t kDefaultByteBudget = 64u * 1024u * 1024u;

    explicit GraphiteImageProvider(std::size_t capacity = kDefaultCapacity,
                                   std::size_t byte_budget = kDefaultByteBudget)
        : capacity_(capacity == 0 ? 1 : capacity), byte_budget_(byte_budget) {}

    sk_sp<SkImage> findOrCreate(skgpu::graphite::Recorder* recorder,
                                const SkImage* image,
                                SkImage::RequiredProperties props) override {
        if (!recorder || !image) return nullptr;

        // `SkImage::uniqueID` comes from a monotonically increasing global
        // counter, so an ID is never reused and the cache cannot alias two
        // different images. Mipmap-ness is part of the key because Graphite
        // drops the draw if a mipmapped image was required and not returned.
        const Key key{image->uniqueID(), props.fMipmapped};
        if (auto it = index_.find(key); it != index_.end()) {
            entries_.splice(entries_.begin(), entries_, it->second);
            return it->second->image;
        }

        auto uploaded = SkImages::TextureFromImage(recorder, image, props);
        if (!uploaded) return nullptr;  // Graphite logs and drops — as before.

        const std::size_t bytes = uploaded->imageInfo().computeMinByteSize();
        entries_.push_front(Entry{key, uploaded, bytes});
        index_.emplace(key, entries_.begin());
        bytes_ += bytes;
        while (entries_.size() > 1 &&
               (entries_.size() > capacity_ || bytes_ > byte_budget_)) {
            bytes_ -= entries_.back().bytes;
            index_.erase(entries_.back().key);
            entries_.pop_back();
        }
        return uploaded;
    }

    /// Release every cached GPU image. Must run while the owning Recorder is
    /// still alive — the textures these images hold are provisioned by it.
    void clear() {
        index_.clear();
        entries_.clear();
        bytes_ = 0;
    }

    std::size_t size() const { return entries_.size(); }
    std::size_t bytes() const { return bytes_; }

private:
    struct Key {
        std::uint32_t image_id = 0;
        bool mipmapped = false;
        bool operator==(const Key& other) const {
            return image_id == other.image_id && mipmapped == other.mipmapped;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return (static_cast<std::size_t>(k.image_id) << 1) |
                   (k.mipmapped ? 1u : 0u);
        }
    };
    struct Entry {
        Key key;
        sk_sp<SkImage> image;
        std::size_t bytes = 0;
    };

    std::size_t capacity_;
    std::size_t byte_budget_;
    std::size_t bytes_ = 0;
    std::list<Entry> entries_;  // Front = most recently used.
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
};

}  // namespace pulp::render

#endif  // PULP_HAS_SKIA
