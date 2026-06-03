// RUIF-4 opt-in Rust UI provider lane. Built only with
// PULP_BUILD_NATIVE_UI_RUST_TESTS=ON, so default builds never require Rust.

#include <pulp/native_components/native_ui.h>
#include <pulp/view/design_import.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace pulp::view;

extern "C" const pulp_native_ui_provider_v1* pulp_native_ui_entry_v1(void);

static_assert(std::is_standard_layout_v<pulp_native_ui_byte_span_v1>);
static_assert(std::is_standard_layout_v<pulp_native_ui_provider_request_v1>);
static_assert(std::is_standard_layout_v<pulp_native_ui_provider_result_v1>);
static_assert(std::is_standard_layout_v<pulp_native_ui_provider_v1>);
static_assert(offsetof(pulp_native_ui_provider_request_v1, strict_mode)
              % alignof(uint32_t) == 0);
static_assert(offsetof(pulp_native_ui_provider_result_v1, owned_result)
              > offsetof(pulp_native_ui_provider_result_v1, canonical_design_ir_json));

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string span_to_string(pulp_native_ui_byte_span_v1 span) {
    if (span.bytes == nullptr || span.byte_len == 0) return {};
    return {reinterpret_cast<const char*>(span.bytes), span.byte_len};
}

bool native_ui_provider_compatible(const pulp_native_ui_provider_v1* provider) {
    return provider != nullptr
        && provider->abi_version == PULP_NATIVE_UI_ABI_VERSION
        && provider->size >= offsetof(pulp_native_ui_provider_v1, free_result) + sizeof(provider->free_result)
        && provider->provider_id != nullptr
        && provider->provider_version != nullptr
        && provider->import_design != nullptr
        && provider->free_result != nullptr;
}

class NativeUiResultGuard {
public:
    explicit NativeUiResultGuard(const pulp_native_ui_provider_v1* provider)
        : provider_(provider) {}

    ~NativeUiResultGuard() {
        if (provider_ != nullptr && provider_->free_result != nullptr)
            provider_->free_result(&result_);
    }

    NativeUiResultGuard(const NativeUiResultGuard&) = delete;
    NativeUiResultGuard& operator=(const NativeUiResultGuard&) = delete;

    pulp_native_ui_provider_result_v1* out() { return &result_; }
    const pulp_native_ui_provider_result_v1& result() const { return result_; }

private:
    const pulp_native_ui_provider_v1* provider_ = nullptr;
    pulp_native_ui_provider_result_v1 result_{};
};

bool has_diagnostic(const std::vector<ImportDiagnostic>& diagnostics,
                    const std::string& code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

std::string layout_json(View& view, float width, float height) {
    view.set_bounds({0, 0, width, height});
    view.layout_children();
    return dump_layout_tree(view,
                            {.surface = "ruif-rust-provider",
                             .fixture = "elysium",
                             .viewport_width = width,
                             .viewport_height = height});
}

class RustNativeUiProvider final : public NativeDesignProvider {
public:
    explicit RustNativeUiProvider(const pulp_native_ui_provider_v1* provider)
        : provider_(provider) {}

    std::string provider_id() const override {
        return provider_ != nullptr ? span_to_string(provider_->provider_id()) : std::string{};
    }

    std::string provider_version() const override {
        return provider_ != nullptr ? span_to_string(provider_->provider_version()) : std::string{};
    }

    NativeDesignProviderOutput import_design(const NativeDesignProviderRequest& request) override {
        NativeDesignProviderOutput output;
        if (!native_ui_provider_compatible(provider_)) {
            output.ok = false;
            output.failure_reason = "Rust native UI provider vtable is incompatible";
            return output;
        }

        const auto strict_mode = request.strict_mode ? 1u : 0u;
        pulp_native_ui_provider_request_v1 abi_request{
            .size = sizeof(pulp_native_ui_provider_request_v1),
            .abi_version = PULP_NATIVE_UI_ABI_VERSION,
            .canonical_source_design_ir_json = {
                reinterpret_cast<const uint8_t*>(request.canonical_source_design_ir_json.data()),
                request.canonical_source_design_ir_json.size()},
            .source_design_hash = {
                reinterpret_cast<const uint8_t*>(request.source_design_hash.data()),
                request.source_design_hash.size()},
            .strict_mode = strict_mode,
            .reserved = 0};

        NativeUiResultGuard result_guard(provider_);
        const auto status = provider_->import_design(&abi_request, result_guard.out());
        const auto& abi_result = result_guard.result();
        output.ok = status == PULP_NATIVE_UI_OK && abi_result.status == PULP_NATIVE_UI_OK;
        output.canonical_design_ir_json = span_to_string(abi_result.canonical_design_ir_json);
        output.provenance.provider_id = span_to_string(abi_result.provider_id);
        output.provenance.provider_version = span_to_string(abi_result.provider_version);
        output.provenance.source_design_hash = span_to_string(abi_result.source_design_hash);
        output.failure_reason = span_to_string(abi_result.failure_reason);
        if (!output.ok && output.failure_reason.empty())
            output.failure_reason = "Rust native UI provider import failed";
        return output;
    }

private:
    const pulp_native_ui_provider_v1* provider_ = nullptr;
};

} // namespace

TEST_CASE("Rust native UI provider ABI vtable is compatible",
          "[rust-ui][provider][ruif-4]") {
    const auto* provider = pulp_native_ui_entry_v1();
    REQUIRE(native_ui_provider_compatible(provider));
    REQUIRE(span_to_string(provider->provider_id()) == "test.rust.elysium");
    REQUIRE(span_to_string(provider->provider_version()) == "0.1.0");
}

TEST_CASE("Rust native UI adapter rejects incompatible vtables",
          "[rust-ui][provider][ruif-4]") {
    pulp_native_ui_provider_v1 provider{};
    REQUIRE_FALSE(native_ui_provider_compatible(nullptr));
    REQUIRE_FALSE(native_ui_provider_compatible(&provider));

    provider.abi_version = PULP_NATIVE_UI_ABI_VERSION;
    provider.size = offsetof(pulp_native_ui_provider_v1, free_result);
    provider.provider_id = []() { return pulp_native_ui_byte_span_v1{}; };
    provider.provider_version = []() { return pulp_native_ui_byte_span_v1{}; };
    provider.import_design = [](const pulp_native_ui_provider_request_v1*,
                                pulp_native_ui_provider_result_v1*) -> pulp_native_ui_status {
        return PULP_NATIVE_UI_ERR_UNSUPPORTED;
    };
    provider.free_result = [](pulp_native_ui_provider_result_v1*) {};
    REQUIRE_FALSE(native_ui_provider_compatible(&provider));

    provider.size = sizeof(pulp_native_ui_provider_v1);
    provider.abi_version = PULP_NATIVE_UI_ABI_VERSION + 1;
    REQUIRE_FALSE(native_ui_provider_compatible(&provider));
}

TEST_CASE("Rust native UI provider returns ELYSIUM canonical DesignIR",
          "[rust-ui][provider][ruif-4][elysium]") {
    const auto fixture_path = std::filesystem::path(PULP_REPO_ROOT)
        / "planning/artifacts/rust-ui/ruif-1/cpp-baseline/ir/elysium.designir.json";
    const auto source_json = read_text(fixture_path);
    auto ir = parse_design_ir_json(source_json);
    const auto canonical = serialize_design_ir(ir);

    auto rust_provider = std::make_shared<RustNativeUiProvider>(pulp_native_ui_entry_v1());
    NativeDesignProviderRegistry registry;
    REQUIRE(registry.register_provider(rust_provider));

    std::vector<ImportDiagnostic> diagnostics;
    NativeDesignProviderAttempt attempt;
    auto rust_view = build_native_view_tree_with_provider(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &diagnostics},
        {.mode = NativeDesignProviderMode::provider_strict,
         .provider_id = "test.rust.elysium",
         .registry = &registry,
         .source_design_hash = "elysium-fixture",
         .attempt_out = &attempt,
         // ELYSIUM currently repeats source stable_anchor_id values; keep this
         // compatibility override fixture-local. Provider anchors still remain
         // hard validation errors and the default source-anchor policy is strict.
         .reject_duplicate_source_anchors = false});

    REQUIRE(rust_view != nullptr);
    REQUIRE(attempt.provider_attempted);
    REQUIRE_FALSE(attempt.fallback_used);
    REQUIRE(attempt.provider_id == "test.rust.elysium");
    REQUIRE(attempt.provider_version == "0.1.0");
    REQUIRE(attempt.source_design_hash == "elysium-fixture");
    REQUIRE(has_diagnostic(diagnostics, "provider-duplicate-source-anchor"));

    NativeDesignProviderOutput output = rust_provider->import_design(
        {.source_ir = &ir,
         .source_manifest = &ir.asset_manifest,
         .canonical_source_design_ir_json = canonical,
         .source_design_hash = "elysium-fixture",
         .strict_mode = true});
    REQUIRE(output.ok);
    REQUIRE(output.canonical_design_ir_json == canonical);

    auto cxx_view = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(cxx_view != nullptr);
    const auto cxx_layout = layout_json(*cxx_view, 1000.0f, 600.0f);
    const auto rust_layout = layout_json(*rust_view, 1000.0f, 600.0f);
    LayoutTreeDiff diff;
    REQUIRE(layout_tree_snapshots_equivalent(cxx_layout, rust_layout, {}, &diff));
}
