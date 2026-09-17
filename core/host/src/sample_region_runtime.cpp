#include <pulp/host/sample_region_runtime.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <tuple>
#include <utility>

namespace pulp::host {

bool operator<(const SampleRegionStateKey& lhs, const SampleRegionStateKey& rhs) noexcept {
    return std::tie(lhs.region_id, lhs.node, lhs.type_id, lhs.version) <
           std::tie(rhs.region_id, rhs.node, rhs.type_id, rhs.version);
}

SampleRegionStateCell::SampleRegionStateCell(SampleRegionStateKey key,
                                             SampleKernelDescriptor descriptor,
                                             PreparedSampleKernelConfig config,
                                             void* storage) noexcept
    : key_(std::move(key)), descriptor_(std::move(descriptor)), config_(config),
      storage_(storage) {}

SampleRegionStateCell::~SampleRegionStateCell() {
    if (constructed_ && descriptor_.destroy != nullptr)
        descriptor_.destroy(storage_);
    if (storage_ != nullptr) {
        ::operator delete(storage_, std::align_val_t(descriptor_.state_alignment));
    }
}

std::shared_ptr<SampleRegionStateCell>
SampleRegionStateCell::create(SampleRegionStateKey key, const SampleKernelDescriptor& descriptor,
                              const PreparedSampleKernelConfig& config, double sample_rate,
                              std::uint32_t max_block_size) noexcept {
    if (descriptor.state_size == 0 || descriptor.construct == nullptr ||
        descriptor.destroy == nullptr || descriptor.reset == nullptr ||
        descriptor.state_alignment == 0 ||
        (descriptor.state_alignment & (descriptor.state_alignment - 1)) != 0 ||
        !descriptor.is_valid_registration()) {
        return {};
    }
    void* storage = nullptr;
    try {
        storage =
            ::operator new(descriptor.state_size, std::align_val_t(descriptor.state_alignment));
        auto cell = std::unique_ptr<SampleRegionStateCell>(
            new SampleRegionStateCell(std::move(key), descriptor, config, storage));
        storage = nullptr;
        auto result = std::shared_ptr<SampleRegionStateCell>(std::move(cell));
        const SampleKernelPrepareContext context{sample_rate, max_block_size, config};
        if (!descriptor.construct(result->storage_, context))
            return {};
        result->constructed_ = true;
        return result;
    } catch (...) {
        if (storage != nullptr)
            ::operator delete(storage, std::align_val_t(descriptor.state_alignment));
        return {};
    }
}

void SampleRegionStateCell::reset() noexcept {
    if (constructed_ && descriptor_.reset != nullptr)
        descriptor_.reset(storage_);
}

SampleRegionExecutionDomain::Admission::Admission(Admission&& other) noexcept
    : domain_(std::exchange(other.domain_, nullptr)) {}

SampleRegionExecutionDomain::Admission&
SampleRegionExecutionDomain::Admission::operator=(Admission&& other) noexcept {
    if (this != &other) {
        release();
        domain_ = std::exchange(other.domain_, nullptr);
    }
    return *this;
}

SampleRegionExecutionDomain::Admission::~Admission() {
    release();
}

void SampleRegionExecutionDomain::Admission::release() noexcept {
    if (domain_ != nullptr) {
        domain_->leave();
        domain_ = nullptr;
    }
}

SampleRegionExecutionDomain::Admission
SampleRegionExecutionDomain::try_admit(std::uint64_t binding_generation) noexcept {
    if (binding_generation == 0 || busy_.test_and_set(std::memory_order_acquire))
        return {};
    const auto adopted = adopted_generation_.load(std::memory_order_relaxed);
    if (binding_generation < adopted) {
        busy_.clear(std::memory_order_release);
        return {};
    }
    if (binding_generation > adopted)
        adopted_generation_.store(binding_generation, std::memory_order_relaxed);
    return Admission(this);
}

SampleRegionStateBank::SampleRegionStateBank(std::shared_ptr<SampleRegionExecutionDomain> domain,
                                             std::uint64_t binding_generation) noexcept
    : domain_(std::move(domain)), binding_generation_(binding_generation) {}

namespace {

SampleRegionStateKey state_key(const PreparedSampleRegionPlan& plan,
                               const PreparedSampleKernelBinding& kernel) {
    return {plan.region_id, kernel.node, kernel.descriptor.type_id, kernel.descriptor.version};
}

bool has_slot_range(std::uint32_t offset, std::uint32_t count, std::uint32_t slot_count) noexcept {
    return offset <= slot_count && count <= slot_count - offset;
}

bool config_is_valid(const PreparedSampleKernelBinding& kernel,
                     std::size_t promoted_parameter_count) noexcept {
    const auto& config = kernel.config;
    const auto expected_kind = [&] {
        switch (kernel.descriptor.authored_config_kind) {
        case SampleKernelConfigKind::None:
            return PreparedSampleKernelConfigKind::None;
        case SampleKernelConfigKind::BoundaryIndex:
            return PreparedSampleKernelConfigKind::BoundaryIndex;
        case SampleKernelConfigKind::FiniteConstant:
            return PreparedSampleKernelConfigKind::FiniteConstant;
        case SampleKernelConfigKind::PromotedParameterId:
            return PreparedSampleKernelConfigKind::PromotedParameterIndex;
        case SampleKernelConfigKind::Invalid:
            return PreparedSampleKernelConfigKind::Invalid;
        }
        return PreparedSampleKernelConfigKind::Invalid;
    };
    if (config.kind != expected_kind())
        return false;
    switch (config.kind) {
    case PreparedSampleKernelConfigKind::None:
        return config.boundary_or_parameter_index == 0 && config.constant == 0.0f;
    case PreparedSampleKernelConfigKind::BoundaryIndex:
        return config.constant == 0.0f;
    case PreparedSampleKernelConfigKind::FiniteConstant:
        return config.boundary_or_parameter_index == 0 && std::isfinite(config.constant);
    case PreparedSampleKernelConfigKind::PromotedParameterIndex:
        return config.constant == 0.0f &&
               config.boundary_or_parameter_index < promoted_parameter_count;
    case PreparedSampleKernelConfigKind::Invalid:
        return false;
    }
    return false;
}

bool descriptors_match_for_retention(const SampleKernelDescriptor& lhs,
                                     const SampleKernelDescriptor& rhs) noexcept {
    return lhs.abi_version == rhs.abi_version && lhs.type_id == rhs.type_id &&
           lhs.version == rhs.version && lhs.num_input_ports == rhs.num_input_ports &&
           lhs.num_output_ports == rhs.num_output_ports && lhs.causality == rhs.causality &&
           lhs.scope == rhs.scope && lhs.authored_config_kind == rhs.authored_config_kind &&
           lhs.state_size == rhs.state_size && lhs.state_alignment == rhs.state_alignment &&
           lhs.construct == rhs.construct && lhs.reset == rhs.reset && lhs.destroy == rhs.destroy &&
           lhs.process == rhs.process && lhs.delay_publish == rhs.delay_publish &&
           lhs.delay_commit == rhs.delay_commit && lhs.latency_samples == rhs.latency_samples;
}

bool configs_match_for_retention(const PreparedSampleKernelConfig& lhs,
                                 const PreparedSampleKernelConfig& rhs) noexcept {
    return lhs.kind == rhs.kind &&
           lhs.boundary_or_parameter_index == rhs.boundary_or_parameter_index &&
           lhs.constant == rhs.constant;
}

bool add_allocation_bytes(std::uint64_t& total, std::size_t count,
                          std::size_t element_size) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (element_size != 0 && count > maximum / element_size)
        return false;
    const auto bytes = static_cast<std::uint64_t>(count) * element_size;
    if (total > maximum - bytes)
        return false;
    total += bytes;
    return true;
}

constexpr std::string_view kInputBoundaryType = "pulp.core.sample-region.input";
constexpr std::string_view kOutputBoundaryType = "pulp.core.sample-region.output";

} // namespace

bool SampleRegionStateBank::populate(SampleRegionStateBank& bank,
                                     std::span<const PreparedSampleRegionPlan> plans,
                                     const SampleRegionStateBank* live, double sample_rate,
                                     std::uint32_t max_block_size) noexcept {
    try {
        for (const auto& plan : plans) {
            for (const auto& kernel : plan.kernels) {
                if (kernel.descriptor.state_size == 0)
                    continue;
                auto key = state_key(plan, kernel);
                std::shared_ptr<SampleRegionStateCell> cell;
                if (live != nullptr) {
                    const auto found = live->cells_.find(key);
                    if (found != live->cells_.end() &&
                        descriptors_match_for_retention(found->second->descriptor(),
                                                        kernel.descriptor) &&
                        configs_match_for_retention(found->second->config(), kernel.config)) {
                        cell = found->second;
                    }
                }
                const bool retained = static_cast<bool>(cell);
                if (!cell) {
                    cell = SampleRegionStateCell::create(key, kernel.descriptor, kernel.config,
                                                         sample_rate, max_block_size);
                    if (!cell)
                        return false;
                }
                const auto bytes = static_cast<std::uint64_t>(cell->size());
                if (bank.state_bytes_ > std::numeric_limits<std::uint64_t>::max() - bytes)
                    return false;
                bank.state_bytes_ += bytes;
                auto [it, inserted] = bank.cells_.emplace(std::move(key), cell);
                if (!inserted)
                    return false;
                bank.retained_.emplace(it->first, retained);
                bank.ordered_cells_.push_back(std::move(cell));
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::shared_ptr<SampleRegionStateBank>
SampleRegionStateBank::create_fresh(std::span<const PreparedSampleRegionPlan> plans,
                                    double sample_rate, std::uint32_t max_block_size,
                                    std::uint64_t binding_generation) noexcept {
    if (binding_generation == 0 || binding_generation == std::numeric_limits<std::uint64_t>::max())
        return {};
    try {
        auto bank = std::shared_ptr<SampleRegionStateBank>(new SampleRegionStateBank(
            std::make_shared<SampleRegionExecutionDomain>(), binding_generation));
        return populate(*bank, plans, nullptr, sample_rate, max_block_size) ? bank : nullptr;
    } catch (...) {
        return {};
    }
}

std::shared_ptr<SampleRegionStateBank> SampleRegionStateBank::adopt(
    std::span<const PreparedSampleRegionPlan> plans, const SampleRegionStateBank& live,
    double sample_rate, std::uint32_t max_block_size, std::uint64_t binding_generation) noexcept {
    if (binding_generation == 0 ||
        live.binding_generation_ == std::numeric_limits<std::uint64_t>::max() ||
        binding_generation <= live.binding_generation_)
        return {};
    try {
        auto bank = std::shared_ptr<SampleRegionStateBank>(
            new SampleRegionStateBank(live.domain_, binding_generation));
        return populate(*bank, plans, &live, sample_rate, max_block_size) ? bank : nullptr;
    } catch (...) {
        return {};
    }
}

std::shared_ptr<SampleRegionStateCell>
SampleRegionStateBank::cell(const SampleRegionStateKey& key) const noexcept {
    const auto found = cells_.find(key);
    return found == cells_.end() ? std::shared_ptr<SampleRegionStateCell>{} : found->second;
}

bool SampleRegionStateBank::was_retained(const SampleRegionStateKey& key) const noexcept {
    const auto found = retained_.find(key);
    return found != retained_.end() && found->second;
}

void SampleRegionStateBank::reset() noexcept {
    for (const auto& cell : ordered_cells_)
        cell->reset();
}

PreparedSampleRegion::PreparedSampleRegion(PreparedSampleRegionPlan plan,
                                           std::shared_ptr<SampleRegionStateBank> bank,
                                           const SampleRegionParameterBinding* parameters) noexcept
    : plan_(std::move(plan)), bank_(std::move(bank)), parameters_(parameters) {}

std::shared_ptr<PreparedSampleRegion>
PreparedSampleRegion::create(PreparedSampleRegionPlan plan,
                             std::shared_ptr<SampleRegionStateBank> bank,
                             const SampleRegionParameterBinding* parameters) noexcept {
    if (!bank || plan.region_id == 0)
        return {};
    try {
        auto result = std::shared_ptr<PreparedSampleRegion>(
            new PreparedSampleRegion(std::move(plan), std::move(bank), parameters));
        return result->finish_prepare() ? result : nullptr;
    } catch (...) {
        return {};
    }
}

bool PreparedSampleRegion::finish_prepare() noexcept {
    try {
        if (plan_.region_id == 0 || !bank_)
            return false;
        scalar_slots_.assign(plan_.scalar_slot_count, 0.0f);
        promoted_values_.assign(plan_.promoted_parameters.size(), 0.0f);
        std::vector<bool> occupied_slots(plan_.scalar_slot_count, false);
        std::map<NodeId, std::size_t> kernel_indexes;
        kernels_.reserve(plan_.kernels.size());
        for (std::size_t index = 0; index < plan_.kernels.size(); ++index) {
            const auto& kernel = plan_.kernels[index];
            if (kernel.node == 0 || !kernel_indexes.emplace(kernel.node, index).second)
                return false;
            if (!kernel.descriptor.is_valid_registration() ||
                !config_is_valid(kernel, plan_.promoted_parameters.size()) ||
                !has_slot_range(kernel.input_slot_offset, kernel.descriptor.num_input_ports,
                                plan_.scalar_slot_count) ||
                !has_slot_range(kernel.output_slot_offset, kernel.descriptor.num_output_ports,
                                plan_.scalar_slot_count)) {
                return false;
            }
            const auto mark_slots = [&](std::uint32_t offset, std::uint32_t count) {
                for (std::uint32_t slot = offset; slot != offset + count; ++slot) {
                    if (occupied_slots[slot])
                        return false;
                    occupied_slots[slot] = true;
                }
                return true;
            };
            if (!mark_slots(kernel.input_slot_offset, kernel.descriptor.num_input_ports) ||
                !mark_slots(kernel.output_slot_offset, kernel.descriptor.num_output_ports)) {
                return false;
            }

            KernelRuntime runtime{&kernel, {}, nullptr};
            if (kernel.descriptor.state_size != 0) {
                runtime.cell_keepalive = bank_->cell(state_key(plan_, kernel));
                if (!runtime.cell_keepalive ||
                    !descriptors_match_for_retention(runtime.cell_keepalive->descriptor(),
                                                     kernel.descriptor)) {
                    return false;
                }
                runtime.state = runtime.cell_keepalive->data();
            }
            kernels_.push_back(std::move(runtime));
            if (kernel.config.kind == PreparedSampleKernelConfigKind::BoundaryIndex) {
                if (kernel.descriptor.type_id == kInputBoundaryType &&
                    kernel.descriptor.num_input_ports == 1 &&
                    kernel.descriptor.num_output_ports == 1) {
                    input_boundaries_.push_back(
                        {kernel.config.boundary_or_parameter_index, kernel.input_slot_offset});
                } else if (kernel.descriptor.type_id == kOutputBoundaryType &&
                           kernel.descriptor.num_input_ports == 1 &&
                           kernel.descriptor.num_output_ports == 1) {
                    output_boundaries_.push_back(
                        {kernel.config.boundary_or_parameter_index, kernel.output_slot_offset});
                } else {
                    return false;
                }
            }
        }
        if (!std::all_of(occupied_slots.begin(), occupied_slots.end(),
                         [](bool occupied) { return occupied; })) {
            return false;
        }
        auto by_index = [](const BoundarySlot& a, const BoundarySlot& b) {
            return a.boundary_index < b.boundary_index;
        };
        std::sort(input_boundaries_.begin(), input_boundaries_.end(), by_index);
        std::sort(output_boundaries_.begin(), output_boundaries_.end(), by_index);
        const auto contiguous = [](const std::vector<BoundarySlot>& boundaries) {
            for (std::size_t i = 0; i < boundaries.size(); ++i) {
                if (boundaries[i].boundary_index != i)
                    return false;
            }
            return true;
        };
        if (input_boundaries_.empty() || output_boundaries_.empty() ||
            !contiguous(input_boundaries_) || !contiguous(output_boundaries_)) {
            return false;
        }

        std::vector<bool> input_slots_written(plan_.scalar_slot_count, false);
        for (const auto& boundary : input_boundaries_)
            input_slots_written[boundary.slot] = true;
        for (const auto& transfer : plan_.transfers) {
            const auto source = kernel_indexes.find(transfer.source);
            const auto destination = kernel_indexes.find(transfer.destination);
            if (source == kernel_indexes.end() || destination == kernel_indexes.end())
                return false;
            const auto& source_binding = plan_.kernels[source->second];
            const auto& destination_binding = plan_.kernels[destination->second];
            if (transfer.source_port >= source_binding.descriptor.num_output_ports ||
                transfer.destination_port >= destination_binding.descriptor.num_input_ports ||
                transfer.source_slot != source_binding.output_slot_offset + transfer.source_port ||
                transfer.destination_slot !=
                    destination_binding.input_slot_offset + transfer.destination_port ||
                input_slots_written[transfer.destination_slot]) {
                return false;
            }
            input_slots_written[transfer.destination_slot] = true;
        }
        for (const auto& kernel : plan_.kernels) {
            for (std::uint32_t port = 0; port < kernel.descriptor.num_input_ports; ++port) {
                if (!input_slots_written[kernel.input_slot_offset + port])
                    return false;
            }
        }

        std::vector<bool> delay_published(plan_.kernels.size(), false);
        std::vector<bool> combinational_processed(plan_.kernels.size(), false);
        std::vector<bool> delay_committed(plan_.kernels.size(), false);
        const auto validate_order = [&](const std::vector<std::size_t>& order,
                                        SampleKernelCausality causality, std::vector<bool>& seen) {
            for (const auto index : order) {
                if (index >= plan_.kernels.size() || seen[index] ||
                    plan_.kernels[index].descriptor.causality != causality) {
                    return false;
                }
                seen[index] = true;
            }
            return true;
        };
        if (!validate_order(plan_.delay_publish_order, SampleKernelCausality::OneSampleDelay,
                            delay_published) ||
            !validate_order(plan_.combinational_order, SampleKernelCausality::Combinational,
                            combinational_processed) ||
            !validate_order(plan_.delay_commit_order, SampleKernelCausality::OneSampleDelay,
                            delay_committed)) {
            return false;
        }
        for (std::size_t index = 0; index < plan_.kernels.size(); ++index) {
            const auto delay =
                plan_.kernels[index].descriptor.causality == SampleKernelCausality::OneSampleDelay;
            if ((delay && (!delay_published[index] || !delay_committed[index])) ||
                (!delay && !combinational_processed[index])) {
                return false;
            }
        }
        std::vector<PreparedSampleRegionOperation> expected_operations;
        expected_operations.reserve(plan_.operations.size());
        const auto append_transfers = [&](NodeId source) {
            for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
                if (plan_.transfers[index].source == source) {
                    expected_operations.push_back(
                        {PreparedSampleRegionOperationKind::Transfer, index});
                }
            }
        };
        for (const auto index : plan_.delay_publish_order) {
            expected_operations.push_back({PreparedSampleRegionOperationKind::DelayPublish, index});
            append_transfers(plan_.kernels[index].node);
        }
        for (const auto index : plan_.combinational_order) {
            expected_operations.push_back({PreparedSampleRegionOperationKind::Process, index});
            append_transfers(plan_.kernels[index].node);
        }
        for (const auto index : plan_.delay_commit_order) {
            expected_operations.push_back({PreparedSampleRegionOperationKind::DelayCommit, index});
        }
        if (expected_operations != plan_.operations)
            return false;

        const SampleFrameContext storage_frame{
            promoted_values_.data(), static_cast<std::uint32_t>(promoted_values_.size()), 0};
        for (const auto& runtime : kernels_) {
            const auto& kernel = *runtime.binding;
            if (!sample_kernel_storage_is_disjoint(
                    kernel.descriptor, runtime.state, kernel.config, storage_frame,
                    scalar_slots_.data() + kernel.input_slot_offset,
                    scalar_slots_.data() + kernel.output_slot_offset)) {
                return false;
            }
        }
        receipt_.region_id = plan_.region_id;
        receipt_.resources = plan_.resources;
        receipt_.private_boundary_copy_bytes = 0;
        receipt_.scalar_slots = plan_.scalar_slot_count;
        if (!add_allocation_bytes(receipt_.physical_executor_bytes, scalar_slots_.capacity(),
                                  sizeof(float)) ||
            !add_allocation_bytes(receipt_.physical_executor_bytes, promoted_values_.capacity(),
                                  sizeof(float)) ||
            !add_allocation_bytes(receipt_.physical_executor_bytes, kernels_.capacity(),
                                  sizeof(KernelRuntime)) ||
            !add_allocation_bytes(receipt_.physical_executor_bytes, input_boundaries_.capacity(),
                                  sizeof(BoundarySlot)) ||
            !add_allocation_bytes(receipt_.physical_executor_bytes, output_boundaries_.capacity(),
                                  sizeof(BoundarySlot))) {
            return false;
        }
        for (const auto& kernel : kernels_) {
            if (!kernel.cell_keepalive)
                continue;
            if (bank_->was_retained(kernel.cell_keepalive->key()))
                ++receipt_.retained_state_cells;
            else
                ++receipt_.fresh_state_cells;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void PreparedSampleRegion::process(audio::BufferView<float>& output,
                                   const audio::BufferView<const float>& input,
                                   int num_samples) noexcept {
    if (num_samples <= 0 || static_cast<std::size_t>(num_samples) > output.num_samples() ||
        static_cast<std::size_t>(num_samples) > input.num_samples())
        return;
    for (std::size_t i = 0; i < promoted_values_.size(); ++i) {
        promoted_values_[i] =
            parameters_ != nullptr ? parameters_->value(plan_.promoted_parameters[i]) : 0.0f;
    }
    for (int frame_index = 0; frame_index < num_samples; ++frame_index) {
        std::fill(scalar_slots_.begin(), scalar_slots_.end(), 0.0f);
        for (const auto& boundary : input_boundaries_) {
            if (boundary.slot >= scalar_slots_.size())
                continue;
            scalar_slots_[boundary.slot] =
                boundary.boundary_index < input.num_channels()
                    ? input.channel_ptr(boundary.boundary_index)[frame_index]
                    : 0.0f;
        }
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            output.channel_ptr(channel)[frame_index] = 0.0f;
        const SampleFrameContext frame{promoted_values_.data(),
                                       static_cast<std::uint32_t>(promoted_values_.size()),
                                       static_cast<std::uint32_t>(frame_index)};
        for (const auto& operation : plan_.operations) {
            switch (operation.kind) {
            case PreparedSampleRegionOperationKind::DelayPublish: {
                if (operation.index >= kernels_.size())
                    break;
                const auto& runtime = kernels_[operation.index];
                const auto& kernel = *runtime.binding;
                kernel.descriptor.delay_publish(runtime.state, kernel.config,
                                                scalar_slots_.data() + kernel.output_slot_offset);
                break;
            }
            case PreparedSampleRegionOperationKind::Process: {
                if (operation.index >= kernels_.size())
                    break;
                const auto& runtime = kernels_[operation.index];
                const auto& kernel = *runtime.binding;
                kernel.descriptor.process(runtime.state, kernel.config, frame,
                                          scalar_slots_.data() + kernel.input_slot_offset,
                                          scalar_slots_.data() + kernel.output_slot_offset);
                break;
            }
            case PreparedSampleRegionOperationKind::Transfer: {
                if (operation.index >= plan_.transfers.size())
                    break;
                const auto& transfer = plan_.transfers[operation.index];
                scalar_slots_[transfer.destination_slot] = scalar_slots_[transfer.source_slot];
                break;
            }
            case PreparedSampleRegionOperationKind::DelayCommit: {
                if (operation.index >= kernels_.size())
                    break;
                const auto& runtime = kernels_[operation.index];
                const auto& kernel = *runtime.binding;
                kernel.descriptor.delay_commit(runtime.state, kernel.config,
                                               scalar_slots_.data() + kernel.input_slot_offset);
                break;
            }
            }
        }
        for (const auto& boundary : output_boundaries_) {
            if (boundary.boundary_index < output.num_channels() &&
                boundary.slot < scalar_slots_.size()) {
                output.channel_ptr(boundary.boundary_index)[frame_index] =
                    scalar_slots_[boundary.slot];
            }
        }
    }
}

} // namespace pulp::host
