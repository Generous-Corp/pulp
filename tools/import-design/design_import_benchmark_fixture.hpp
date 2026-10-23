#pragma once

// Shared surface of the design-import benchmark: the fixture interface every
// lane implements, and the fixture built from an importer-produced DesignIR.
// The IR-bound fixture lives in its own translation unit so the benchmark's
// driver, receipt and synthetic lanes do not share a file with the retained
// slot experiment.

#include <pulp/view/design_ir.hpp>
#include <pulp/view/view.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {
class Label;
} // namespace pulp::view

namespace pulp::design_import_bench {

inline constexpr int kFixtureWidth = 760;
inline constexpr int kFixtureHeight = 420;

class BenchmarkFixture {
  public:
    virtual ~BenchmarkFixture() = default;
    virtual view::View& root() = 0;
    virtual void step(int frame) = 0;
    virtual int js_evaluation_count() const = 0;
    virtual std::uint64_t bridge_call_count() const {
        return 0;
    }
    // Anchor-keyed text slots whose View changed identity across an update,
    // and slots whose text did not read back as written. Only fixtures that
    // drive keyed slots report non-zero.
    virtual std::uint64_t slot_identity_change_count() const {
        return 0;
    }
    virtual std::uint64_t slot_text_mismatch_count() const {
        return 0;
    }
    // Untimed check that the update `step(frame)` made is what the tree now
    // holds. run_phase calls it outside every clock, so verification cost
    // never lands in update or frame time.
    virtual void verify(int /*frame*/) {}
};

// Materializes a DesignIR and fails closed on any import error, so a
// benchmark never measures a tree the importer reported as wrong. Warnings
// (a design that states no widget palette, say) do not change the tree's
// structure; their codes are returned so the receipt shows every one.
std::unique_ptr<view::View> materialize_checked(const view::DesignIR& ir, float width,
                                                float height, std::string_view lane,
                                                std::vector<std::string>* warning_codes);

// An importer-produced DesignIR, with the provenance that binds a receipt to
// the exact bytes measured.
struct LoadedIr {
    view::DesignIR ir;
    std::string sha256;
    std::string file_name;
    float width = static_cast<float>(kFixtureWidth);
    float height = static_cast<float>(kFixtureHeight);
    std::vector<std::string> slot_anchors;
};

LoadedIr load_ir(const std::filesystem::path& path);

std::string slot_text(std::size_t slot, int frame);

// The retained-plan experiment over real importer output. Each anchored text
// node is a typed slot. The retained lane resolves every slot to its Label
// once and writes through it; the rebuild lane is the control, writing the
// same text into a copy of the IR and rematerializing the whole tree. Both
// read every slot back by anchor after the update, so a slot that rebound to
// a different View, or kept stale text, is counted rather than assumed away.
class IrBoundFixture final : public BenchmarkFixture {
  public:
    IrBoundFixture(const std::filesystem::path& path, bool rebuild_each_step);

    view::View& root() override {
        return *root_;
    }
    int js_evaluation_count() const override {
        return 0;
    }
    std::uint64_t slot_identity_change_count() const override {
        return identity_changes_;
    }
    std::uint64_t slot_text_mismatch_count() const override {
        return text_mismatches_;
    }
    view::View& tree() {
        return *root_;
    }
    const LoadedIr& loaded() const {
        return loaded_;
    }
    const std::vector<std::string>& warning_codes() const {
        return warning_codes_;
    }

    void step(int frame) override;
    void verify(int frame) override;

  private:
    struct Slot {
        std::uint64_t instance_id = 0;
    };

    void materialize(const view::DesignIR& ir);
    void resolve_slots();

    LoadedIr loaded_;
    bool rebuild_each_step_ = false;
    std::unique_ptr<view::View> root_;
    std::vector<Slot> slots_;
    std::vector<view::Label*> resolved_;
    std::vector<view::Label*> retained_;
    std::vector<std::string> warning_codes_;
    std::uint64_t identity_changes_ = 0;
    std::uint64_t text_mismatches_ = 0;
};

} // namespace pulp::design_import_bench
