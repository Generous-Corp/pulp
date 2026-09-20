#pragma once

#include <pulp/host/sample_region_authoring.hpp>
#include <pulp/state/store.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pulp::host {

class SampleRegionParameterBinding;
class SampleRegionParameterOwner;

struct SampleRegionParameterContractEntry {
    SampleRegionId region_id = 0;
    std::string key;
    NodeId bound_node_id = 0;
    PortIndex bound_port = 0;
    state::ParamInfo info;
};

// Value-owned parameter metadata derived from authored sample regions. A
// candidate contract contains promoted parameters only; freeze() combines it
// with the Processor's ordinary manifest and validates the whole surface before
// any StateStore registration can occur.
class SampleRegionParameterContract {
  public:
    SampleRegionParameterContract() = default;

    static SampleRegionParameterContract
    from_regions(std::span<const SampleRegionDefinition> regions);

    SampleRegionParameterContract
    freeze(std::span<const state::ParamInfo> ordinary_parameters) const;

    bool valid() const noexcept {
        return valid_;
    }
    bool frozen() const noexcept {
        return frozen_;
    }
    const std::string& error() const noexcept {
        return error_;
    }
    std::span<const SampleRegionParameterContractEntry> entries() const noexcept {
        return entries_;
    }
    std::span<const state::ParamInfo> promoted_parameters() const noexcept {
        return promoted_parameters_;
    }
    std::span<const state::ParamInfo> parameters() const noexcept {
        return parameters_;
    }
    bool matches_promoted(const SampleRegionParameterContract& other) const noexcept;

    std::unique_ptr<SampleRegionParameterBinding> bind(state::StateStore& store) const;

  private:
    friend class SampleRegionParameterBinding;
    friend class SampleRegionParameterOwner;

    // std::function has no general value equality. Empty callbacks and plain
    // function pointers are compared exactly; other callable objects use their
    // concrete target type as the stable metadata boundary.
    bool store_equal_(const state::StateStore& store) const noexcept;

    bool valid_ = false;
    bool frozen_ = false;
    std::string error_ = "parameter contract has not been constructed";
    std::vector<SampleRegionParameterContractEntry> entries_;
    std::vector<state::ParamInfo> promoted_parameters_;
    std::vector<state::ParamInfo> parameters_;
};

// Non-owning link between an immutable contract and its StateStore. The
// SampleRegionParameterOwner that supplies it must outlive the binding and
// every graph or prepared edit to which that binding is attached.
class SampleRegionParameterBinding {
  public:
    SampleRegionParameterBinding(const SampleRegionParameterBinding&) = delete;
    SampleRegionParameterBinding& operator=(const SampleRegionParameterBinding&) = delete;
    SampleRegionParameterBinding(SampleRegionParameterBinding&&) = delete;
    SampleRegionParameterBinding& operator=(SampleRegionParameterBinding&&) = delete;

    const SampleRegionParameterContract& contract() const noexcept {
        return *contract_;
    }
    const state::StateStore& store() const noexcept {
        return *store_;
    }
    state::StateStore& store() noexcept {
        return *store_;
    }
    float value(state::ParamID id) const noexcept {
        return store_->get_value(id);
    }

  private:
    friend class SampleRegionParameterContract;
    SampleRegionParameterBinding(const SampleRegionParameterContract& contract,
                                 state::StateStore& store) noexcept
        : contract_(&contract), store_(&store) {}

    const SampleRegionParameterContract* contract_ = nullptr;
    state::StateStore* store_ = nullptr;
};

// Owns the unpublished store, frozen manifest, and borrowed binding as one
// construction unit. create() returns null before anything can be published if
// validation or registration construction fails.
class SampleRegionParameterOwner {
  public:
    SampleRegionParameterOwner(const SampleRegionParameterOwner&) = delete;
    SampleRegionParameterOwner& operator=(const SampleRegionParameterOwner&) = delete;

    static std::unique_ptr<SampleRegionParameterOwner>
    create(std::span<const state::ParamInfo> ordinary_parameters,
           const SampleRegionParameterContract& promoted_contract) noexcept;

    const SampleRegionParameterContract& contract() const noexcept {
        return contract_;
    }
    const SampleRegionParameterBinding& binding() const noexcept {
        return *binding_;
    }
    state::StateStore& store() noexcept {
        return store_;
    }
    const state::StateStore& store() const noexcept {
        return store_;
    }

  private:
    explicit SampleRegionParameterOwner(SampleRegionParameterContract contract)
        : contract_(std::move(contract)) {}

    // Destruction order is binding, store, contract. The borrowed pointers are
    // therefore never observable after either owner has gone away.
    SampleRegionParameterContract contract_;
    state::StateStore store_;
    std::unique_ptr<SampleRegionParameterBinding> binding_;
};

} // namespace pulp::host
