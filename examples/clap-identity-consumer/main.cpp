// SPDX-License-Identifier: MIT
#include <iostream>
#include <pulp/host/plugin_slot.hpp>

// Explicit hosting operation: this executes the supplied plugin in this process.
int main(int argc, char** argv) {
    using namespace pulp::host;
    if (argc != 3)
        return 2;
    auto result = PluginSlot::load_exact({argv[1], {"clap", argv[2]}});
    if (!std::holds_alternative<std::unique_ptr<PluginSlot>>(result.value))
        return 3;
    auto slot = std::move(std::get<std::unique_ptr<PluginSlot>>(result.value));
    const auto receipt = slot->resolved_identity();
    if (!receipt || receipt->loaded.verification() != PluginIdentityVerification::loaded ||
        receipt->loaded.type() != receipt->requested)
        return 4;
    slot.reset();
    const auto canonical = canonical_plugin_identity(receipt->loaded);
    if (!std::holds_alternative<CanonicalPluginIdentity>(canonical))
        return 5;
    std::cout << std::get<CanonicalPluginIdentity>(canonical).bytes << '\n';
    const auto imported =
        parse_plugin_identity_evidence(serialize_plugin_identity_evidence(receipt->loaded));
    if (!imported || imported->verification() != PluginIdentityVerification::declared)
        return 6;
    const auto missing = PluginSlot::load_exact({argv[1], {"clap", "org.pulp.identity.absent"}});
    if (!std::holds_alternative<PluginIdentityError>(missing.value) ||
        std::get<PluginIdentityError>(missing.value) !=
            PluginIdentityError::descriptor_selection_missing)
        return 7;
    return 0;
}
