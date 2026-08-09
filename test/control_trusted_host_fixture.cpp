#include <cstdlib>

namespace {
const volatile char kStandalone[] = "PULP_STANDALONE_COMPONENT_V1";
const volatile char kShipping[] = "PULP_INSPECT_SHIPPING_MANIFEST_V1";
const volatile char kProfile[] = "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1";
const volatile char kManifest[] =
    "PULP_CONTROL_MANIFEST_SHA256_1fa8468f587e2205f95933744a12b73d14d69f0aa298c6897911cebbca73a6c2_"
    "V1";
const volatile char kCapability[] = "PULP_INSPECT_CAPABILITY_DEV_PULP_INSTANCE_READ_1_V1";
} // namespace

int main() {
    return kStandalone[0] == 'P' && kShipping[0] == 'P' && kProfile[0] == 'P' &&
                   kManifest[0] == 'P' && kCapability[0] == 'P'
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
