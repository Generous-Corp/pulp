// SPDX-License-Identifier: MIT
#pragma once

namespace pulp::import_design {

/// Selects the post-intake passes that are valid for the authoritative input.
/// Browser capture has already resolved source layout and assets; source-parsed
/// lanes still need the legacy analysis and sidecar pipeline.
class ImportPreparationPolicy {
public:
    static constexpr ImportPreparationPolicy source_parsed() {
        return ImportPreparationPolicy(Origin::source_parsed);
    }
    static constexpr ImportPreparationPolicy captured_frame() {
        return ImportPreparationPolicy(Origin::captured_frame);
    }

    [[nodiscard]] constexpr bool is_captured_frame() const noexcept {
        return origin_ == Origin::captured_frame;
    }
    [[nodiscard]] constexpr bool runs_source_analysis() const noexcept {
        return origin_ == Origin::source_parsed;
    }
    [[nodiscard]] constexpr bool refreshes_source_assets() const noexcept {
        return origin_ == Origin::source_parsed;
    }
    [[nodiscard]] constexpr bool emits_source_sidecars() const noexcept {
        return origin_ == Origin::source_parsed;
    }

private:
    enum class Origin {
        source_parsed,
        captured_frame,
    };

    explicit constexpr ImportPreparationPolicy(Origin origin)
        : origin_(origin) {}

    Origin origin_;
};

}  // namespace pulp::import_design
