#pragma once

// Host-owned audio source discovery.  A plug-in never enumerates a DAW's
// project tracks from its realtime processor; the host supplies the catalog
// and connects one of its entries to a declared input bus.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <utility>

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

    virtual ~AudioRouteCatalog() = default;
    virtual std::vector<AudioRouteInfo> enumerate() const = 0;
    virtual void set_change_callback(ChangeCallback callback) = 0;

    /// Connect a host-owned source to a declared processor input bus. Hosts
    /// that cannot retarget routes from the plug-in UI keep the default false
    /// and expose their own routing menu instead.
    virtual bool connect(std::string_view stable_id, std::size_t bus_index) {
        (void)stable_id;
        (void)bus_index;
        return false;
    }

    /// Return the stable source currently connected to a bus, or empty when
    /// the host has not selected one. This is control-thread/UI state.
    virtual std::string connected_route(std::size_t bus_index) const {
        (void)bus_index;
        return {};
    }
};

/// Small host-side implementation for standalone/Forge hosts. It owns only
/// control-thread catalog state; audio adapters consume the resulting bus
/// connection and never enumerate or mutate this object from realtime code.
class MutableAudioRouteCatalog final : public AudioRouteCatalog {
public:
    std::vector<AudioRouteInfo> enumerate() const override { return routes_; }

    void set_routes(std::vector<AudioRouteInfo> routes) {
        routes_ = std::move(routes);
        selected_.clear();
        notify();
    }

    void set_change_callback(ChangeCallback callback) override {
        callback_ = std::move(callback);
    }

    bool connect(std::string_view stable_id, std::size_t bus_index) override {
        const auto it = std::find_if(routes_.begin(), routes_.end(),
            [stable_id](const AudioRouteInfo& route) {
                return route.stable_id == stable_id && route.available &&
                       route.channels > 0;
            });
        if (it == routes_.end()) return false;
        if (selected_.size() <= bus_index) selected_.resize(bus_index + 1);
        selected_[bus_index] = it->stable_id;
        notify();
        return true;
    }

    std::string connected_route(std::size_t bus_index) const override {
        return bus_index < selected_.size() ? selected_[bus_index] : std::string{};
    }

private:
    void notify() {
        if (callback_) callback_();
    }

    std::vector<AudioRouteInfo> routes_;
    std::vector<std::string> selected_;
    ChangeCallback callback_;
};

}  // namespace pulp::host
