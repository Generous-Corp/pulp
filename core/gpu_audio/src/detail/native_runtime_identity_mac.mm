#include "shared_io_provider_identity.hpp"

#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>

#include "dawn/native/MetalBackend.h"

#include <CoreFoundation/CoreFoundation.h>
#include <cstring>
#include <iomanip>
#include <optional>
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

struct PciIdentity {
    std::uint64_t vendor_id = 0;
    std::uint64_t device_id = 0;
};

std::uint64_t registry_property(io_registry_entry_t entry, CFStringRef key) {
    CFTypeRef value =
        IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault,
                                        kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (value == nullptr || CFGetTypeID(value) != CFDataGetTypeID()) {
        if (value != nullptr)
            CFRelease(value);
        return 0;
    }
    const auto* bytes = CFDataGetBytePtr(static_cast<CFDataRef>(value));
    const auto length = CFDataGetLength(static_cast<CFDataRef>(value));
    std::uint64_t result = 0;
    if (bytes != nullptr && length >= static_cast<CFIndex>(sizeof(std::uint32_t))) {
        std::uint32_t value32 = 0;
        std::memcpy(&value32, bytes, sizeof(value32));
        result = value32;
    }
    CFRelease(value);
    return result;
}

std::optional<PciIdentity> pci_identity(id<MTLDevice> device) {
    if (device == nil || device.registryID == 0)
        return std::nullopt;
    CFMutableDictionaryRef matching = IORegistryEntryIDMatching(device.registryID);
    if (matching == nullptr)
        return std::nullopt;
    const io_registry_entry_t accelerator =
        IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (accelerator == IO_OBJECT_NULL)
        return std::nullopt;
    io_registry_entry_t parent = IO_OBJECT_NULL;
    const kern_return_t parent_status =
        IORegistryEntryGetParentEntry(accelerator, kIOServicePlane, &parent);
    IOObjectRelease(accelerator);
    if (parent_status != KERN_SUCCESS || parent == IO_OBJECT_NULL)
        return std::nullopt;
    const PciIdentity result{registry_property(parent, CFSTR("vendor-id")),
                             registry_property(parent, CFSTR("device-id"))};
    IOObjectRelease(parent);
    if (result.vendor_id == 0)
        return std::nullopt;
    return result;
}

NativeRuntimeIdentity observe_native_metal_device_impl(id<MTLDevice> device,
                                                       std::string_view selected_name,
                                                       std::uint64_t selected_vendor_id,
                                                       std::uint64_t selected_device_id) noexcept {
    NativeRuntimeIdentity identity;
    if (device == nil || selected_vendor_id != 0x106b || selected_name.empty() ||
        utf8_name(device.name) != selected_name || device.registryID == 0)
        return identity;

    // Dawn reports PCI IDs when the IORegistry exposes them. Require an exact
    // match whenever it does; if Dawn has no device ID (common on Apple
    // Silicon), the Dawn-selected native device pointer remains the authority.
    const auto pci = pci_identity(device);
    if (selected_device_id != 0 &&
        (!pci || pci->vendor_id != selected_vendor_id || pci->device_id != selected_device_id))
        return identity;
    if (pci && pci->vendor_id != selected_vendor_id)
        return identity;

    identity.authenticated = true;
    identity.name = utf8_name(device.name);
    identity.backend = "Metal";
    identity.registry_id = device.registryID;
    identity.revision = registry_revision(identity.registry_id);
    return identity;
}

} // namespace

NativeRuntimeIdentity observe_native_metal(std::string_view selected_name,
                                           std::uint64_t selected_vendor_id,
                                           std::uint64_t selected_device_id) noexcept {
    NativeRuntimeIdentity identity;
    @autoreleasepool {
        if (selected_vendor_id != 0x106b || selected_name.empty())
            return identity;

        // This metadata-only entry point is retained for diagnostics. An exact
        // Dawn-device pointer is used by the provider path below; enumeration
        // remains fail-closed when names are ambiguous.
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        id<MTLDevice> device = nil;
        for (id<MTLDevice> candidate in devices) {
            const auto pci = selected_device_id ? pci_identity(candidate) : std::nullopt;
            if (utf8_name(candidate.name) == selected_name &&
                (!selected_device_id || (pci && pci->device_id == selected_device_id))) {
                if (device != nil)
                    return identity;
                device = candidate;
            }
        }
        if (device == nil)
            return identity;
        identity = observe_native_metal_device_impl(device, selected_name, selected_vendor_id,
                                                    selected_device_id);
    }
    return identity;
}

NativeRuntimeIdentity observe_native_metal_device(void* dawn_device, std::string_view selected_name,
                                                  std::uint64_t selected_vendor_id,
                                                  std::uint64_t selected_device_id) noexcept {
    if (dawn_device == nullptr)
        return {};
    @autoreleasepool {
        const auto device =
            dawn::native::metal::GetMTLDevice(reinterpret_cast<WGPUDevice>(dawn_device));
        return observe_native_metal_device_impl(device, selected_name, selected_vendor_id,
                                                selected_device_id);
    }
}

} // namespace pulp::gpu_audio::detail
