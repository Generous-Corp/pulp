// SPDX-License-Identifier: MIT
#pragma once

#include "kit_commands.hpp"
#include "kit_json_helpers.hpp"
#include "json_writer.hpp"

namespace pulp::cli::kit {

void validate_publish_policy(KitValidationResult& result, const JsonValue& manifest);
void validate_registry_manifest(KitValidationResult& result,
                                const JsonValue& registry_manifest,
                                const std::string& expected_sha);
std::string publish_badges_json(const KitValidationResult& result,
                                bool registry_manifest_checked);
std::string compatibility_json(const JsonValue& manifest);

} // namespace pulp::cli::kit
