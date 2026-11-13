#pragma once

/// @file
/// `pulp::vellum::VisualPortFeed`: Pulp's `format::VisualPort` presented as a
/// `vellum::value::ValueFeed`. The two shapes match one to one, so this only
/// renames types: channel indices, latest-wins reads and publish counts pass
/// straight through, and the single-reader-thread rule is the port's.
///
/// One limit is the feed's own: `read_events` copies through a fixed stack
/// block (reads stay allocation-free), so it returns at most
/// `kMaxEventsPerRead` occurrences however large the caller's span is. The
/// limit equals the most events one Pulp audio block can publish, so nothing
/// a Pulp port reports is dropped.

#include <vellum/value/value_feed.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace pulp::format {
class VisualPort;
}

namespace pulp::vellum {

class VisualPortFeed final : public ::vellum::value::ValueFeed {
  public:
    /// The most occurrences one `read_events` call returns: the per-block
    /// bound of Pulp's events channels (`dataplane::EventFrame::kMaxEvents`).
    static constexpr std::size_t kMaxEventsPerRead = 1024;

    /// The port must outlive the feed.
    explicit VisualPortFeed(format::VisualPort& port) noexcept : port_(port) {}

    [[nodiscard]] std::size_t channel_count() const noexcept override;
    [[nodiscard]] ::vellum::value::ChannelInfo channel(std::size_t index) const noexcept override;
    bool read_scalar(std::size_t index, float& value) noexcept override;
    std::size_t read_meter(std::size_t index, std::span<float> rms,
                           std::span<float> peak) noexcept override;
    std::size_t read_vector(std::size_t index, std::span<float> samples) noexcept override;
    std::size_t read_events(std::size_t index,
                            std::span<::vellum::value::ChannelEvent> events) noexcept override;
    [[nodiscard]] std::uint32_t publish_count(std::size_t index) const noexcept override;

  private:
    format::VisualPort& port_;
};

} // namespace pulp::vellum
