#include "shared_io_provider_identity.hpp"

namespace pulp::gpu_audio::detail {

NativeRuntimeIdentity observe_native_metal(std::string_view, std::uint64_t,
                                           std::uint64_t) noexcept {
    return {};
}

} // namespace pulp::gpu_audio::detail
