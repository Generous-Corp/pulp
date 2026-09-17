#pragma once

#include <pulp/host/sample_region_parameters.hpp>
#include <pulp/host/sample_region_plan.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pulp::host {

// The identity of one persistent scalar-kernel state cell.  Runtime edits may
// retain a cell only when every field is identical; changing an exact kernel
// version deliberately creates fresh state.
struct SampleRegionStateKey {
    SampleRegionId region_id = 0;
    NodeId node = 0;
    std::string type_id;
    int version = 0;

    friend bool operator==(const SampleRegionStateKey&, const SampleRegionStateKey&) = default;
    friend bool operator<(const SampleRegionStateKey& lhs,
                          const SampleRegionStateKey& rhs) noexcept;
};

class SampleRegionStateCell final {
  public:
    SampleRegionStateCell(const SampleRegionStateCell&) = delete;
    SampleRegionStateCell& operator=(const SampleRegionStateCell&) = delete;
    ~SampleRegionStateCell();

    static std::shared_ptr<SampleRegionStateCell> create(SampleRegionStateKey key,
                                                         const SampleKernelDescriptor& descriptor,
                                                         const PreparedSampleKernelConfig& config,
                                                         double sample_rate,
                                                         std::uint32_t max_block_size) noexcept;

    const SampleRegionStateKey& key() const noexcept {
        return key_;
    }
    const SampleKernelDescriptor& descriptor() const noexcept {
        return descriptor_;
    }
    const PreparedSampleKernelConfig& config() const noexcept {
        return config_;
    }
    void* data() noexcept {
        return storage_;
    }
    const void* data() const noexcept {
        return storage_;
    }
    std::size_t size() const noexcept {
        return descriptor_.state_size;
    }
    std::size_t alignment() const noexcept {
        return descriptor_.state_alignment;
    }
    void reset() noexcept;

  private:
    SampleRegionStateCell(SampleRegionStateKey key, SampleKernelDescriptor descriptor,
                          PreparedSampleKernelConfig config, void* storage) noexcept;

    SampleRegionStateKey key_;
    SampleKernelDescriptor descriptor_;
    PreparedSampleKernelConfig config_;
    void* storage_ = nullptr;
    bool constructed_ = false;
};

// One execution lineage is shared by every snapshot that adopts any cell from
// that lineage.  Admission spans the complete graph dispatch and worker join.
class SampleRegionExecutionDomain final {
  public:
    class Admission final {
      public:
        Admission() = default;
        Admission(const Admission&) = delete;
        Admission& operator=(const Admission&) = delete;
        Admission(Admission&& other) noexcept;
        Admission& operator=(Admission&& other) noexcept;
        ~Admission();

        explicit operator bool() const noexcept {
            return domain_ != nullptr;
        }

      private:
        friend class SampleRegionExecutionDomain;
        explicit Admission(SampleRegionExecutionDomain* domain) noexcept : domain_(domain) {}
        void release() noexcept;
        SampleRegionExecutionDomain* domain_ = nullptr;
    };

    SampleRegionExecutionDomain() = default;
    SampleRegionExecutionDomain(const SampleRegionExecutionDomain&) = delete;
    SampleRegionExecutionDomain& operator=(const SampleRegionExecutionDomain&) = delete;

    // Fails immediately on contention and rejects an older binding after a
    // newer generation has been admitted.  It never spins or blocks.
    Admission try_admit(std::uint64_t binding_generation) noexcept;
    std::uint64_t adopted_generation() const noexcept {
        return adopted_generation_.load(std::memory_order_relaxed);
    }

  private:
    friend class Admission;
    void leave() noexcept {
        busy_.clear(std::memory_order_release);
    }
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> adopted_generation_{0};
};

class SampleRegionStateBank final {
  public:
    using CellMap = std::map<SampleRegionStateKey, std::shared_ptr<SampleRegionStateCell>>;

    SampleRegionStateBank(const SampleRegionStateBank&) = delete;
    SampleRegionStateBank& operator=(const SampleRegionStateBank&) = delete;

    static std::shared_ptr<SampleRegionStateBank>
    create_fresh(std::span<const PreparedSampleRegionPlan> plans, double sample_rate,
                 std::uint32_t max_block_size, std::uint64_t binding_generation) noexcept;

    // Builds a new immutable bank view. Exact-key cells and the execution
    // domain are shared without dereferencing, copying, or resetting cell bytes.
    static std::shared_ptr<SampleRegionStateBank>
    adopt(std::span<const PreparedSampleRegionPlan> plans, const SampleRegionStateBank& live,
          double sample_rate, std::uint32_t max_block_size,
          std::uint64_t binding_generation) noexcept;

    std::shared_ptr<SampleRegionStateCell> cell(const SampleRegionStateKey& key) const noexcept;
    std::span<const std::shared_ptr<SampleRegionStateCell>> ordered_cells() const noexcept {
        return ordered_cells_;
    }
    SampleRegionExecutionDomain& domain() const noexcept {
        return *domain_;
    }
    std::uint64_t binding_generation() const noexcept {
        return binding_generation_;
    }
    std::uint64_t state_bytes() const noexcept {
        return state_bytes_;
    }
    bool was_retained(const SampleRegionStateKey& key) const noexcept;
    void reset() noexcept;

  private:
    SampleRegionStateBank(std::shared_ptr<SampleRegionExecutionDomain> domain,
                          std::uint64_t binding_generation) noexcept;
    static bool populate(SampleRegionStateBank& bank,
                         std::span<const PreparedSampleRegionPlan> plans,
                         const SampleRegionStateBank* live, double sample_rate,
                         std::uint32_t max_block_size) noexcept;

    std::shared_ptr<SampleRegionExecutionDomain> domain_;
    std::uint64_t binding_generation_ = 0;
    std::uint64_t state_bytes_ = 0;
    CellMap cells_;
    std::map<SampleRegionStateKey, bool> retained_;
    std::vector<std::shared_ptr<SampleRegionStateCell>> ordered_cells_;
};

struct SampleRegionRuntimeReceipt {
    SampleRegionId region_id = 0;
    SampleRegionResourceStats resources;
    std::uint64_t physical_executor_bytes = 0;
    std::uint64_t private_boundary_copy_bytes = 0;
    std::uint32_t scalar_slots = 0;
    std::uint32_t retained_state_cells = 0;
    std::uint32_t fresh_state_cells = 0;
};

class PreparedSampleRegion final {
  public:
    PreparedSampleRegion(const PreparedSampleRegion&) = delete;
    PreparedSampleRegion& operator=(const PreparedSampleRegion&) = delete;

    static std::shared_ptr<PreparedSampleRegion>
    create(PreparedSampleRegionPlan plan, std::shared_ptr<SampleRegionStateBank> bank,
           const SampleRegionParameterBinding* parameters) noexcept;

    SampleRegionId region_id() const noexcept {
        return plan_.region_id;
    }
    const PreparedSampleRegionPlan& plan() const noexcept {
        return plan_;
    }
    const SampleRegionRuntimeReceipt& receipt() const noexcept {
        return receipt_;
    }
    const std::shared_ptr<SampleRegionStateBank>& bank() const noexcept {
        return bank_;
    }

    // Block callback used by the one fused quotient binding. All storage is
    // prepared up front; the call is allocation-free and lock-free.
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 int num_samples) noexcept;

  private:
    struct BoundarySlot {
        std::uint32_t boundary_index = 0;
        std::uint32_t slot = 0;
    };
    struct KernelRuntime {
        const PreparedSampleKernelBinding* binding = nullptr;
        std::shared_ptr<SampleRegionStateCell> cell_keepalive;
        void* state = nullptr;
    };

    PreparedSampleRegion(PreparedSampleRegionPlan plan, std::shared_ptr<SampleRegionStateBank> bank,
                         const SampleRegionParameterBinding* parameters) noexcept;
    bool finish_prepare() noexcept;

    PreparedSampleRegionPlan plan_;
    std::shared_ptr<SampleRegionStateBank> bank_;
    const SampleRegionParameterBinding* parameters_ = nullptr;
    std::vector<KernelRuntime> kernels_;
    std::vector<float> scalar_slots_;
    std::vector<float> promoted_values_;
    std::vector<BoundarySlot> input_boundaries_;
    std::vector<BoundarySlot> output_boundaries_;
    SampleRegionRuntimeReceipt receipt_;
};

} // namespace pulp::host
