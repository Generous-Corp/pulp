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

}  // namespace pulp::host
