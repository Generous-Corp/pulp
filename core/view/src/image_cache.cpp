#include <pulp/view/image_cache.hpp>

#include <algorithm>

namespace pulp::view {

const DecodedImage* ImageCache::get(const std::string& uri) {
    std::lock_guard<std::mutex> lock(mu_);
    if (auto it = entries_.find(uri); it != entries_.end()) {
        ++stats_.hits;
        it->second.last_used = ++tick_;
        return &it->second.image;
    }
    ++stats_.misses;
    if (!decoder_) return nullptr;

    auto decoded = decoder_(uri);
    if (!decoded) return nullptr;

    Entry e;
    e.image = *decoded;
    e.last_used = ++tick_;
    total_bytes_ += e.image.bytes;
    auto [it, _] = entries_.emplace(uri, std::move(e));
    try_trim_();
    // try_trim_ may evict `it` itself if the new entry alone exceeds
    // the budget. Re-lookup to return a valid pointer or nullptr.
    it = entries_.find(uri);
    return it == entries_.end() ? nullptr : &it->second.image;
}

void ImageCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [_, e] : entries_) {
        if (releaser_) releaser_(e.image);
    }
    entries_.clear();
    total_bytes_ = 0;
}

ImageCacheStats ImageCache::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    ImageCacheStats out = stats_;
    out.entry_count = entries_.size();
    out.total_bytes = total_bytes_;
    return out;
}

void ImageCache::evict_one_() {
    if (entries_.empty()) return;
    auto oldest = entries_.begin();
    for (auto it = std::next(entries_.begin()); it != entries_.end(); ++it) {
        if (it->second.last_used < oldest->second.last_used) oldest = it;
    }
    total_bytes_ -= oldest->second.image.bytes;
    if (releaser_) releaser_(oldest->second.image);
    entries_.erase(oldest);
    ++stats_.evictions;
}

void ImageCache::try_trim_() {
    if (budget_ == 0) return;
    while (total_bytes_ > budget_ && !entries_.empty()) {
        evict_one_();
    }
}

}  // namespace pulp::view
