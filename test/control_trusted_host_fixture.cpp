#include "support/control_manifest_fixtures.hpp"

#include <cstdlib>

namespace {
const volatile char kStandalone[] = "PULP_STANDALONE_COMPONENT_V1";
const volatile char kShipping[] = "PULP_INSPECT_SHIPPING_MANIFEST_V1";
const volatile char kProfile[] = "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1";
const volatile pulp::test::ControlManifestMarker kManifest =
    pulp::test::kTrustedHostFixtureMarker;
const volatile char kCapability[] = "PULP_INSPECT_CAPABILITY_SESSION_DESCRIBE_V1";
} // namespace

int main() {
    return kStandalone[0] == 'P' && kShipping[0] == 'P' && kProfile[0] == 'P' &&
                   kManifest.bytes[0] == 'P' && kCapability[0] == 'P'
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
