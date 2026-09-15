#include <cstdlib>

namespace {
const volatile char kStandalone[] = "PULP_STANDALONE_COMPONENT_V1";
const volatile char kShipping[] = "PULP_INSPECT_SHIPPING_MANIFEST_V1";
const volatile char kProfile[] = "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1";
const volatile char kManifest[] =
    "PULP_CONTROL_MANIFEST_SHA256_4fe8bdb87ce76fdd8eb613140baf9cc2ded9299593260a4f7017c4a2e0e984cb_"
    "V1";
const volatile char kCapability[] = "PULP_INSPECT_CAPABILITY_SESSION_DESCRIBE_V1";
} // namespace

int main() {
    return kStandalone[0] == 'P' && kShipping[0] == 'P' && kProfile[0] == 'P' &&
                   kManifest[0] == 'P' && kCapability[0] == 'P'
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
