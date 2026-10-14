#include "shared_io_provider_identity.hpp"

#import <Metal/Metal.h>

#include <iomanip>
#include <sstream>
#include <string>

namespace pulp::gpu_audio::detail {
namespace {

std::string utf8_name(NSString* value) {
    if (value == nil)
        return {};
    const char* utf8 = value.UTF8String;
    return utf8 != nullptr ? std::string(utf8) : std::string{};
}

std::string registry_revision(std::uint64_t registry_id) {
    std::ostringstream stream;
    stream << "metal-registry-" << std::hex << std::nouppercase << registry_id;
    return stream.str();
}

} // namespace

NativeRuntimeIdentity observe_native_metal(std::string_view selected_name,
                                           std::uint64_t selected_vendor_id,
                                           std::uint64_t selected_device_id) noexcept {
    NativeRuntimeIdentity identity;
    @autoreleasepool {
        if (selected_vendor_id != 0x106b || selected_name.empty())
            return identity;

        // Enumerate the native devices instead of assuming the default device
        // is Dawn's high-performance selection. An exact, unique name match
        // plus Apple's vendor ID is the selected-adapter check; ambiguity
        // remains unavailable rather than manufacturing identity.
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        id<MTLDevice> device = nil;
        for (id<MTLDevice> candidate in devices) {
            if (utf8_name(candidate.name) == selected_name) {
                if (device != nil)
                    return identity;
                device = candidate;
            }
        }
        if (device == nil)
            return identity;
        const std::uint64_t registry_id = device.registryID;
        if (registry_id == 0)
            return identity;
        // Dawn's device ID is PCI metadata while Metal's registry ID is a
        // global native identity, so they are retained separately.
        (void)selected_device_id;
        identity.authenticated = true;
        identity.name = utf8_name(device.name);
        identity.backend = "Metal";
        identity.registry_id = registry_id;
        identity.revision = registry_revision(registry_id);
    }
    return identity;
}

} // namespace pulp::gpu_audio::detail
