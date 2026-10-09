#pragma once
#include <pulp/host/forge_param_descriptor.hpp>
namespace pulp::host::space::convolution {
ForgeNodeDescriptor descriptor();
#if defined(PULP_HOST_ENABLE_GPU_CONVOLUTION)
ForgeNodeDescriptor descriptor_with_gpu();
#endif
} // namespace pulp::host::space::convolution
namespace pulp::host::space::nonlin_ambience {
ForgeNodeDescriptor descriptor();
}
namespace pulp::host::space::cabinet {
ForgeNodeDescriptor descriptor();
}
