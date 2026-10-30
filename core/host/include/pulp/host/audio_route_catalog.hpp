#pragma once

// Host-owned audio source discovery.  A plug-in never enumerates a DAW's
// project tracks from its realtime processor; the host supplies the catalog
// and connects one of its entries to a declared input bus.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::host {

enum class AudioRouteKind : std::uint8_t {
    PhysicalInput,
    Track,
    Instrument,
    Bus,
    File,
    Internal,
};

struct AudioRouteInfo {
    std::string stable_id;
    std::string display_name;
    AudioRouteKind kind = AudioRouteKind::Internal;
    int channels = 0;
    bool available = true;
};

/// Control-thread source catalog supplied by a Pulp host (standalone, Forge,
/// or another graph host). External DAWs do not expose their project catalog
/// through AU/VST3/CLAP; those adapters still expose the declared buses and
/// let the DAW own its routing menu.
class AudioRouteCatalog {
  public:
    using ChangeCallback = std::function<void()>;
    using ConnectCallback = std::function<bool(const AudioRouteInfo&, std::size_t bus_index)>;
    using ConnectChannelCallback = std::function<bool(const AudioRouteInfo&, std::size_t bus_index,
                                                      std::size_t channel_index)>;
    using DisconnectCallback = std::function<bool(std::size_t bus_index)>;

    virtual ~AudioRouteCatalog() = default;
    virtual std::vector<AudioRouteInfo> enumerate() const = 0;
    virtual void set_change_callback(ChangeCallback callback) = 0;

    /// Install the host graph/bus binding invoked by connect(). This runs on
    /// the control thread; the callback owns the actual graph mutation and
    /// publishes the resulting audio buffers to the realtime processor.
    /// Format adapters that cannot retarget project routes leave it unset.
    virtual void set_connect_callback(ConnectCallback callback) {
        (void)callback;
    }

    /// Install the host graph binding for an individual channel connection.
    /// Hosts that only support whole-bus routing may leave this unset.
    virtual void set_connect_channel_callback(ConnectChannelCallback callback) {
        (void)callback;
    }

    /// Install the host graph binding for disconnect(). Returning false keeps
    /// the catalog selection because the graph did not accept the mutation.
    virtual void set_disconnect_callback(DisconnectCallback callback) {
        (void)callback;
    }

    /// Connect a host-owned source to a declared processor input bus. Hosts
    /// that cannot retarget routes from the plug-in UI keep the default false
    /// and expose their own routing menu instead.
    virtual bool connect(std::string_view stable_id, std::size_t bus_index) {
        (void)stable_id;
        (void)bus_index;
        return false;
    }

    /// Connect one channel of a declared input bus. Hosts that do not expose
    /// independent channel routing inherit the bus-level connection behavior.
    virtual bool connect_channel(std::string_view stable_id, std::size_t bus_index,
                                 std::size_t channel_index) {
        (void)channel_index;
        return connect(stable_id, bus_index);
    }

    virtual bool disconnect(std::size_t bus_index) {
        (void)bus_index;
        return false;
    }

    /// Return the stable source currently connected to a bus, or empty when
    /// the host has not selected one. This is control-thread/UI state.
    virtual std::string connected_route(std::size_t bus_index) const {
        (void)bus_index;
        return {};
    }

    /// Return the stable source currently connected to one bus channel. The
    /// default preserves compatibility with hosts that only route whole buses.
    virtual std::string connected_route_channel(std::size_t bus_index,
                                                std::size_t channel_index) const {
        (void)channel_index;
        return connected_route(bus_index);
    }

    /// Return the stable IDs selected for each declared input bus. Hosts may
    /// store these IDs in their project/plugin state and restore them after a
    /// catalog is rebuilt. The vector index is the bus index; an empty string
    /// means that bus has no selected route.
    virtual std::vector<std::string> selected_route_ids() const {
        return {};
    }

    /// Restore previously saved stable IDs on the control thread. Invalid,
    /// unavailable, or rejected routes are ignored and do not disturb the
    /// existing selection. Hosts should call this after set_routes().
    virtual void restore_selected_route_ids(std::span<const std::string> ids) {
        (void)ids;
    }
};

/// Small host-side implementation for standalone/Forge hosts. It owns only
/// control-thread catalog state; audio adapters consume the resulting bus
/// connection and never enumerate or mutate this object from realtime code.
class MutableAudioRouteCatalog final : public AudioRouteCatalog {
  public:
    std::vector<AudioRouteInfo> enumerate() const override {
        return routes_;
    }

    void set_routes(std::vector<AudioRouteInfo> routes) {
        // Preserve selections whose stable IDs still exist, including routes
        // that are temporarily unavailable. Hosts commonly rebuild the
        // catalog for hot-plug/project edits; retaining the stable identity
        // lets the picker show an unavailable checkmarked source and lets the
        // host reconnect it when it returns. The graph binding remains the
        // authority for whether audio can currently flow.
        std::vector<std::string> retained;
        retained.reserve(selected_.size());
        for (const auto& selected : selected_) {
            // Keep the ID even when the route is absent from this snapshot.
            // A physical device or project track can disappear for one
            // refresh and return later; clearing it here loses the user's
            // selection and makes state restore impossible. `connect()` still
            // rejects absent/unavailable routes, so this is only identity,
            // never an audio-path claim.
            retained.push_back(selected);
        }
        std::vector<std::array<std::string, 2>> retained_channels;
        retained_channels.reserve(selected_channels_.size());
        for (const auto& selected : selected_channels_)
            retained_channels.push_back(selected);
        routes_ = std::move(routes);
        selected_ = std::move(retained);
        selected_channels_ = std::move(retained_channels);
        if (connect_channel_callback_) {
            for (std::size_t bus_index = 0; bus_index < selected_channels_.size(); ++bus_index) {
                for (std::size_t channel_index = 0; channel_index < 2; ++channel_index) {
                    const auto& selected = selected_channels_[bus_index][channel_index];
                    if (selected.empty())
                        continue;
                    const auto it = std::find_if(
                        routes_.begin(), routes_.end(), [&](const AudioRouteInfo& route) {
                            return route.stable_id == selected && route.available &&
                                   channel_index < static_cast<std::size_t>(route.channels);
                        });
                    if (it != routes_.end())
                        (void)connect_channel_callback_(*it, bus_index, channel_index);
                }
            }
        } else if (connect_callback_) {
            for (std::size_t bus_index = 0; bus_index < selected_.size(); ++bus_index) {
                const auto it =
                    std::find_if(routes_.begin(), routes_.end(), [&](const AudioRouteInfo& route) {
                        return route.stable_id == selected_[bus_index] && route.available &&
                               route.channels > 0;
                    });
                if (it != routes_.end())
                    (void)connect_callback_(*it, bus_index);
            }
        }
        notify();
    }

    void set_change_callback(ChangeCallback callback) override {
        callback_ = std::move(callback);
    }

    void set_connect_callback(ConnectCallback callback) override {
        connect_callback_ = std::move(callback);
    }

    void set_connect_channel_callback(ConnectChannelCallback callback) override {
        connect_channel_callback_ = std::move(callback);
    }

    void set_disconnect_callback(DisconnectCallback callback) override {
        disconnect_callback_ = std::move(callback);
    }

    bool connect(std::string_view stable_id, std::size_t bus_index) override {
        if (bus_index == std::numeric_limits<std::size_t>::max() ||
            bus_index >= selected_.max_size())
            return false;
        const auto it =
            std::find_if(routes_.begin(), routes_.end(), [stable_id](const AudioRouteInfo& route) {
                return route.stable_id == stable_id && route.available && route.channels > 0;
            });
        if (it == routes_.end())
            return false;
        ensure_bus(bus_index);
        const bool feeds_right = it->channels > 1;
        if (connect_channel_callback_) {
            // A whole-bus binding replaces both channels. A source that cannot
            // feed the right channel must first drop any independent right
            // binding, which only the bus-level disconnect can do.
            const bool stale_right = !selected_channels_[bus_index][1].empty() && !feeds_right;
            if (stale_right) {
                if (!disconnect_callback_ || !disconnect_callback_(bus_index))
                    return false;
                selected_[bus_index].clear();
                selected_channels_[bus_index] = {};
            }
            if (!connect_channel_callback_(*it, bus_index, 0)) {
                if (stale_right)
                    notify();
                return false;
            }
            selected_[bus_index] = it->stable_id;
            selected_channels_[bus_index][0] = it->stable_id;
            if (!feeds_right) {
                selected_channels_[bus_index][1].clear();
            } else if (connect_channel_callback_(*it, bus_index, 1)) {
                selected_channels_[bus_index][1] = it->stable_id;
            }
            notify();
            return !feeds_right || !selected_channels_[bus_index][1].empty();
        }
        if (connect_callback_ && !connect_callback_(*it, bus_index))
            return false;
        set_whole_bus(bus_index, *it);
        notify();
        return true;
    }

    bool connect_channel(std::string_view stable_id, std::size_t bus_index,
                         std::size_t channel_index) override {
        if (bus_index == std::numeric_limits<std::size_t>::max() ||
            bus_index >= selected_.max_size() || channel_index >= 2)
            return false;
        const auto it =
            std::find_if(routes_.begin(), routes_.end(), [stable_id](const AudioRouteInfo& route) {
                return route.stable_id == stable_id && route.available && route.channels > 0;
            });
        if (it == routes_.end())
            return false;
        if (channel_index >= static_cast<std::size_t>(it->channels))
            return false;
        if (connect_channel_callback_) {
            if (!connect_channel_callback_(*it, bus_index, channel_index))
                return false;
        } else if (connect_callback_ && channel_index == 0) {
            // The legacy callback rebinds the whole bus, so the catalog must
            // record a whole-bus selection rather than a lone left channel.
            if (!connect_callback_(*it, bus_index))
                return false;
            ensure_bus(bus_index);
            set_whole_bus(bus_index, *it);
            notify();
            return true;
        } else if (connect_callback_) {
            // A legacy whole-bus callback cannot bind an independent channel.
            return false;
        }
        ensure_bus(bus_index);
        selected_channels_[bus_index][channel_index] = it->stable_id;
        if (channel_index == 0)
            selected_[bus_index] = it->stable_id;
        notify();
        return true;
    }

    bool disconnect(std::size_t bus_index) override {
        const bool bus_selected = bus_index < selected_.size() && !selected_[bus_index].empty();
        const bool channel_selected =
            bus_index < selected_channels_.size() &&
            std::any_of(
                selected_channels_[bus_index].begin(), selected_channels_[bus_index].end(),
                [](const std::string& selected) { return !selected.empty(); });
        if (!bus_selected && !channel_selected)
            return false;
        if (disconnect_callback_ && !disconnect_callback_(bus_index))
            return false;
        if (bus_index < selected_.size())
            selected_[bus_index].clear();
        if (bus_index < selected_channels_.size())
            selected_channels_[bus_index] = {};
        notify();
        return true;
    }

    std::string connected_route(std::size_t bus_index) const override {
        return bus_index < selected_.size() ? selected_[bus_index] : std::string{};
    }

    std::string connected_route_channel(std::size_t bus_index,
                                        std::size_t channel_index) const override {
        if (bus_index >= selected_channels_.size() || channel_index >= 2)
            return {};
        return selected_channels_[bus_index][channel_index];
    }

    std::vector<std::string> selected_route_ids() const override {
        return selected_;
    }

    void restore_selected_route_ids(std::span<const std::string> ids) override {
        for (std::size_t bus_index = 0; bus_index < ids.size(); ++bus_index) {
            if (!ids[bus_index].empty())
                (void)connect(ids[bus_index], bus_index);
        }
    }

  private:
    void ensure_bus(std::size_t bus_index) {
        if (selected_.size() <= bus_index)
            selected_.resize(bus_index + 1);
        if (selected_channels_.size() <= bus_index)
            selected_channels_.resize(bus_index + 1);
    }

    void set_whole_bus(std::size_t bus_index, const AudioRouteInfo& route) {
        selected_[bus_index] = route.stable_id;
        selected_channels_[bus_index][0] = route.stable_id;
        selected_channels_[bus_index][1] = route.channels > 1 ? route.stable_id : std::string{};
    }

    void notify() {
        if (callback_)
            callback_();
    }

    std::vector<AudioRouteInfo> routes_;
    std::vector<std::string> selected_;
    std::vector<std::array<std::string, 2>> selected_channels_;
    ChangeCallback callback_;
    ConnectCallback connect_callback_;
    ConnectChannelCallback connect_channel_callback_;
    DisconnectCallback disconnect_callback_;
};

} // namespace pulp::host
