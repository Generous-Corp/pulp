#include <pulp/host/sample_region_parameters.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_set>

namespace pulp::host {
namespace {

bool range_equal(const state::ParamRange& left, const state::ParamRange& right) noexcept {
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    return bits(left.min) == bits(right.min) && bits(left.max) == bits(right.max) &&
           bits(left.default_value) == bits(right.default_value) &&
           bits(left.step) == bits(right.step) && bits(left.skew) == bits(right.skew) &&
           left.symmetric_skew == right.symmetric_skew;
}

bool formatter_equal(const std::function<std::string(float)>& left,
                     const std::function<std::string(float)>& right) noexcept {
    if (static_cast<bool>(left) != static_cast<bool>(right))
        return false;
    if (!left)
        return true;
    if (left.target_type() != right.target_type())
        return false;
    using FunctionPointer = std::string (*)(float);
    const auto* left_pointer = left.target<FunctionPointer>();
    const auto* right_pointer = right.target<FunctionPointer>();
    return left_pointer == nullptr || (right_pointer != nullptr && *left_pointer == *right_pointer);
}

bool formatter_equal(const std::function<float(const std::string&)>& left,
                     const std::function<float(const std::string&)>& right) noexcept {
    if (static_cast<bool>(left) != static_cast<bool>(right))
        return false;
    if (!left)
        return true;
    if (left.target_type() != right.target_type())
        return false;
    using FunctionPointer = float (*)(const std::string&);
    const auto* left_pointer = left.target<FunctionPointer>();
    const auto* right_pointer = right.target<FunctionPointer>();
    return left_pointer == nullptr || (right_pointer != nullptr && *left_pointer == *right_pointer);
}

bool info_equal(const state::ParamInfo& left, const state::ParamInfo& right) noexcept {
    return left.id == right.id && left.name == right.name && left.unit == right.unit &&
           range_equal(left.range, right.range) && left.group_id == right.group_id &&
           formatter_equal(left.to_string, right.to_string) &&
           formatter_equal(left.from_string, right.from_string) && left.rate == right.rate &&
           std::bit_cast<std::uint32_t>(left.smoothing_ramp_seconds) ==
               std::bit_cast<std::uint32_t>(right.smoothing_ramp_seconds) &&
           left.designation == right.designation && left.is_trigger == right.is_trigger &&
           left.kind == right.kind && left.value_labels == right.value_labels;
}

bool finite_range(const state::ParamRange& range) noexcept {
    return std::isfinite(range.min) && std::isfinite(range.max) &&
           std::isfinite(range.default_value) && std::isfinite(range.step) &&
           std::isfinite(range.skew) && range.min <= range.max &&
           range.default_value >= range.min && range.default_value <= range.max &&
           range.step >= 0.0f && range.skew > 0.0f;
}

bool valid_promoted_info(const state::ParamInfo& info) noexcept {
    return info.id != 0 && !info.name.empty() && finite_range(info.range) && info.group_id == 0 &&
           !info.to_string && !info.from_string && info.rate == state::ParamRate::ControlRate &&
           std::bit_cast<std::uint32_t>(info.smoothing_ramp_seconds) == 0u &&
           info.designation == state::ParamDesignation::None && !info.is_trigger &&
           info.kind == state::ParamKind::Continuous && info.value_labels.empty();
}

bool valid_ordinary_info(const state::ParamInfo& info) noexcept {
    return info.id != 0 && !info.name.empty() && finite_range(info.range) &&
           std::isfinite(info.smoothing_ramp_seconds) && info.smoothing_ramp_seconds >= 0.0f;
}

} // namespace

SampleRegionParameterContract
SampleRegionParameterContract::from_regions(std::span<const SampleRegionDefinition> regions) {
    SampleRegionParameterContract result;
    result.error_.clear();
    std::unordered_set<state::ParamID> ids;
    std::unordered_set<SampleRegionId> region_ids;
    for (const auto& region : regions) {
        if (region.region_id == 0 || !region_ids.insert(region.region_id).second) {
            result.error_ = "sample-region identities are not unique";
            return result;
        }
        std::unordered_set<std::string> keys;
        for (const auto& parameter : region.promoted_parameters) {
            state::ParamInfo info;
            info.id = parameter.param_id;
            info.name = parameter.name;
            info.unit = parameter.unit;
            info.range = parameter.range;
            info.rate = parameter.rate;
            info.smoothing_ramp_seconds = parameter.smoothing_ramp_seconds;
            if (parameter.key.empty() || parameter.bound_node_id == 0 ||
                parameter.bound_port != 0 || !valid_promoted_info(info) ||
                !ids.insert(info.id).second || !keys.insert(parameter.key).second) {
                result.error_ = "promoted identity or metadata is not an exact v1 contract";
                result.entries_.clear();
                return result;
            }
            result.entries_.push_back({region.region_id, parameter.key, parameter.bound_node_id,
                                       parameter.bound_port, std::move(info)});
        }
    }
    std::sort(result.entries_.begin(), result.entries_.end(),
              [](const auto& left, const auto& right) { return left.info.id < right.info.id; });
    result.promoted_parameters_.reserve(result.entries_.size());
    for (const auto& entry : result.entries_)
        result.promoted_parameters_.push_back(entry.info);
    result.parameters_ = result.promoted_parameters_;
    result.valid_ = true;
    return result;
}

SampleRegionParameterContract
SampleRegionParameterContract::freeze(std::span<const state::ParamInfo> ordinary_parameters) const {
    if (!valid_)
        return *this;
    SampleRegionParameterContract result = *this;
    result.valid_ = false;
    result.frozen_ = false;
    result.error_.clear();
    result.parameters_.clear();
    result.parameters_.reserve(ordinary_parameters.size() + promoted_parameters_.size());
    std::unordered_set<state::ParamID> ids;
    for (const auto& parameter : ordinary_parameters) {
        if (!valid_ordinary_info(parameter) || !ids.insert(parameter.id).second) {
            result.error_ = "ordinary parameter manifest is invalid or contains duplicate IDs";
            return result;
        }
        result.parameters_.push_back(parameter);
    }
    for (const auto& parameter : promoted_parameters_) {
        if (!ids.insert(parameter.id).second) {
            result.error_ = "ordinary and promoted parameter IDs overlap";
            return result;
        }
        result.parameters_.push_back(parameter);
    }
    result.valid_ = true;
    result.frozen_ = true;
    return result;
}

bool SampleRegionParameterContract::matches_promoted(
    const SampleRegionParameterContract& other) const noexcept {
    if (!valid_ || !other.valid_ || entries_.size() != other.entries_.size())
        return false;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& left = entries_[i];
        const auto& right = other.entries_[i];
        if (left.region_id != right.region_id || left.key != right.key ||
            left.bound_node_id != right.bound_node_id || left.bound_port != right.bound_port ||
            !info_equal(left.info, right.info))
            return false;
    }
    return true;
}

bool SampleRegionParameterContract::store_equal_(const state::StateStore& store) const noexcept {
    if (!valid_ || !frozen_ || store.param_count() != parameters_.size())
        return false;
    const auto actual = store.all_params();
    for (std::size_t i = 0; i < parameters_.size(); ++i)
        if (!info_equal(parameters_[i], actual[i]))
            return false;
    return true;
}

std::unique_ptr<SampleRegionParameterBinding>
SampleRegionParameterContract::bind(state::StateStore& store) const {
    if (!store_equal_(store))
        return nullptr;
    return std::unique_ptr<SampleRegionParameterBinding>(
        new SampleRegionParameterBinding(*this, store));
}

std::unique_ptr<SampleRegionParameterOwner> SampleRegionParameterOwner::create(
    std::span<const state::ParamInfo> ordinary_parameters,
    const SampleRegionParameterContract& promoted_contract) noexcept {
    try {
        auto frozen = promoted_contract.freeze(ordinary_parameters);
        if (!frozen.valid() || !frozen.frozen())
            return nullptr;
        auto owner = std::unique_ptr<SampleRegionParameterOwner>(
            new SampleRegionParameterOwner(std::move(frozen)));
        for (const auto& parameter : owner->contract_.parameters())
            owner->store_.add_parameter(parameter);
        owner->binding_ = owner->contract_.bind(owner->store_);
        if (!owner->binding_)
            return nullptr;
        return owner;
    } catch (...) {
        return nullptr;
    }
}

} // namespace pulp::host
