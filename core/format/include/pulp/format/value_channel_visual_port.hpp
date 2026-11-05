#pragma once

/// @file value_channel_visual_port.hpp
/// The default `VisualPort`, over a processor's `ValueChannelSet`.
///
/// A processor declares its named value channels once, before audio runs, and
/// publishes into them from `process()` (see pulp/dataplane/value_channel_set.hpp).
/// This port gives an editor read access to those channels through the
/// editor-host contract, so an editor built on any UI library reads the same
/// data a native Pulp view binds to, with no view layer linked.

#include <pulp/format/editor_host.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::dataplane {
class EventSource;
class MeterSource;
class ScalarSource;
class ValueChannelSet;
class VectorSource;
} // namespace pulp::dataplane

namespace pulp::format {

class Processor;

/// Reads a `ValueChannelSet` through the `VisualPort` contract.
///
/// The port resolves every declared channel once, at construction. That is
/// valid because a set's channels are fixed before audio runs: an index the
/// port hands out stays valid for the life of the set. Channel indices and
/// order match `ValueChannelSet::infos()`.
///
/// Lifetime: the set must outlive the port. A null set gives a port with no
/// channels, which is what a processor that declares none exposes.
///
/// Threading: the same one-reader-thread contract as the sources behind it.
/// Every read through one port, and every other read of the same set, happens
/// on one thread, normally the editor's frame tick. Reads are lock-free and
/// allocation-free; vector and event reads copy one frame on the stack.
class ValueChannelVisualPort final : public VisualPort {
  public:
    explicit ValueChannelVisualPort(const dataplane::ValueChannelSet* channels);

    /// The port over `processor.value_channels()`, or an empty port when the
    /// processor declares no channels.
    explicit ValueChannelVisualPort(Processor& processor);

    ValueChannelVisualPort(const ValueChannelVisualPort&) = delete;
    ValueChannelVisualPort& operator=(const ValueChannelVisualPort&) = delete;

    std::size_t channel_count() const noexcept override;
    VisualChannelInfo channel(std::size_t index) const noexcept override;
    bool read_scalar(std::size_t index, float& value) noexcept override;
    std::size_t read_meter(std::size_t index, std::span<float> rms,
                           std::span<float> peak) noexcept override;
    std::size_t read_vector(std::size_t index, std::span<float> samples) noexcept override;
    std::size_t read_events(std::size_t index, std::span<VisualEvent> events) noexcept override;
    std::uint32_t publish_count(std::size_t index) const noexcept override;

  private:
    struct Channel {
        VisualChannelInfo info;
        dataplane::ScalarSource* scalar = nullptr;
        dataplane::MeterSource* meter = nullptr;
        dataplane::VectorSource* vector = nullptr;
        dataplane::EventSource* events = nullptr;
    };

    const Channel* at(std::size_t index) const noexcept;

    std::vector<Channel> channels_;
};

} // namespace pulp::format
