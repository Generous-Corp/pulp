#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/processor_restore.hpp>

#include <algorithm>
#include <set>

namespace pulp::host {
namespace {
bool valid_identity(std::string_view id) {
    if (id.empty() || id.size() > 255 || id.find('.') == std::string_view::npos)
        return false;
    bool segment_start = true;
    for (const unsigned char c : id) {
        if (c == '.') {
            if (segment_start)
                return false;
            segment_start = true;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-'))
                return false;
            segment_start = false;
        }
    }
    return !segment_start;
}

bool same_capabilities(const format::NodeCapabilities& a, const format::NodeCapabilities& b) {
    return a.supports_mpe == b.supports_mpe && a.supports_ump == b.supports_ump &&
           a.supports_f64_audio == b.supports_f64_audio &&
           a.consumes_audio_rate_modulations == b.consumes_audio_rate_modulations &&
           a.writes_input_buffers == b.writes_input_buffers;
}

ProcessorRestoreResult failure(ProcessorRestoreCode code, const ProcessorRestoreRecord& record) {
    return {code, record.node_id, record.type, {}};
}
} // namespace

const char* processor_restore_code_name(ProcessorRestoreCode code) noexcept {
    switch (code) {
#define PULP_RESTORE_CODE(name)                                                                    \
    case ProcessorRestoreCode::name:                                                               \
        return #name
        PULP_RESTORE_CODE(Restored);
        PULP_RESTORE_CODE(InvalidDescriptor);
        PULP_RESTORE_CODE(DuplicateProcessorType);
        PULP_RESTORE_CODE(InvalidRecord);
        PULP_RESTORE_CODE(StateLimitExceeded);
        PULP_RESTORE_CODE(UnknownProcessorType);
        PULP_RESTORE_CODE(ProcessorTypeDisallowed);
        PULP_RESTORE_CODE(UnsupportedVersion);
        PULP_RESTORE_CODE(ProcessorConstructionFailed);
        PULP_RESTORE_CODE(ProcessorInstanceNotFresh);
        PULP_RESTORE_CODE(ProcessorPortMismatch);
        PULP_RESTORE_CODE(ProcessorCapabilityMismatch);
        PULP_RESTORE_CODE(ProcessorStateRestoreFailed);
#undef PULP_RESTORE_CODE
    }
    return "InvalidRecord";
}

ProcessorRestoreCode
ProcessorRestoreRegistry::register_type(ProcessorRestoreDescriptor descriptor) {
    if (!valid_identity(descriptor.type.type_id) || descriptor.type.type_version <= 0 ||
        descriptor.state_schema_version <= 0 || descriptor.input_channels < 0 ||
        descriptor.output_channels < 0 || !descriptor.create)
        return ProcessorRestoreCode::InvalidDescriptor;
    const auto key = std::make_pair(descriptor.type.type_id, descriptor.type.type_version);
    if (descriptors_.contains(key))
        return ProcessorRestoreCode::DuplicateProcessorType;
    descriptors_.emplace(key, std::move(descriptor));
    return ProcessorRestoreCode::Restored;
}

ProcessorRestoreResult
ProcessorRestoreRegistry::restore_batch(std::span<const ProcessorRestoreRecord> records,
                                        std::span<const ProcessorTypeKey> allowlist) const {
    if (records.size() > max_nodes)
        return {ProcessorRestoreCode::StateLimitExceeded, 0, {}, {}};
    std::set<std::uint32_t> ids;
    std::size_t total = 0;
    std::vector<const ProcessorRestoreDescriptor*> types;
    types.reserve(records.size());
    for (const auto& record : records) {
        if (record.node_id == 0 || !ids.insert(record.node_id).second ||
            !valid_identity(record.type.type_id) || record.type.type_version <= 0 ||
            record.state_schema_version <= 0 || record.input_channels < 0 ||
            record.output_channels < 0)
            return failure(ProcessorRestoreCode::InvalidRecord, record);
        for (const auto bytes : {record.parameters, record.custom}) {
            if (bytes.size() > max_state_part_bytes || bytes.size() > max_total_state_bytes - total)
                return failure(ProcessorRestoreCode::StateLimitExceeded, record);
            total += bytes.size();
        }
        const auto found = descriptors_.find({record.type.type_id, record.type.type_version});
        if (found == descriptors_.end()) {
            const auto same_id = descriptors_.lower_bound({record.type.type_id, 0});
            return failure(same_id != descriptors_.end() &&
                                   same_id->first.first == record.type.type_id
                               ? ProcessorRestoreCode::UnsupportedVersion
                               : ProcessorRestoreCode::UnknownProcessorType,
                           record);
        }
        if (std::find(allowlist.begin(), allowlist.end(), record.type) == allowlist.end())
            return failure(ProcessorRestoreCode::ProcessorTypeDisallowed, record);
        const auto& type = found->second;
        if (record.state_schema_version != type.state_schema_version)
            return failure(ProcessorRestoreCode::UnsupportedVersion, record);
        if (record.input_channels != type.input_channels ||
            record.output_channels != type.output_channels)
            return failure(ProcessorRestoreCode::ProcessorPortMismatch, record);
        types.push_back(&type);
    }

    std::vector<RestoredProcessor> candidates;
    candidates.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        const auto& type = *types[index];
        std::shared_ptr<format::ProcessorNodeInstance> instance;
        try {
            instance = type.create();
        } catch (...) {
            return failure(ProcessorRestoreCode::ProcessorConstructionFailed, record);
        }
        if (!instance)
            return failure(ProcessorRestoreCode::ProcessorConstructionFailed, record);
        // The batch owns every candidate exclusively. A live graph, a prior
        // candidate, or a factory-retained handle makes the count exceed one.
        if (instance.use_count() != 1)
            return failure(ProcessorRestoreCode::ProcessorInstanceNotFresh, record);
        const auto& actual = instance->descriptor();
        if (actual.default_input_channels() != type.input_channels ||
            actual.default_output_channels() != type.output_channels)
            return failure(ProcessorRestoreCode::ProcessorPortMismatch, record);
        if (!same_capabilities(actual.effective_capabilities(), type.capabilities))
            return failure(ProcessorRestoreCode::ProcessorCapabilityMismatch, record);
        instance->set_graph_type_version(type.type.type_version);
        instance->set_graph_type_id(type.type.type_id);
        instance->set_graph_state_schema_version(record.state_schema_version);
        const auto parameter_version = state::serialized_state_version(record.parameters);
        if (!parameter_version)
            return failure(ProcessorRestoreCode::ProcessorStateRestoreFailed, record);
        // The envelope and parameter schemas are independent. This initial
        // exact-version surface must not enter StateStore's migration path or
        // its backwards-compatible acceptance of older parameter blobs.
        if (*parameter_version != instance->state_store().state_version())
            return failure(ProcessorRestoreCode::UnsupportedVersion, record);
        candidates.push_back({record.node_id, std::move(instance)});
    }
    // A later factory may have retained an earlier candidate through a weak
    // handle. Recheck the whole set before mutating the first instance.
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (candidates[index].instance.use_count() != 1)
            return failure(ProcessorRestoreCode::ProcessorInstanceNotFresh, records[index]);
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        auto& instance = *candidates[index].instance;
        const auto& record = records[index];
        try {
            if (!instance.state_store().deserialize(record.parameters) ||
                !instance.processor().deserialize_plugin_state(record.custom))
                return failure(ProcessorRestoreCode::ProcessorStateRestoreFailed, record);
        } catch (...) {
            return failure(ProcessorRestoreCode::ProcessorStateRestoreFailed, record);
        }
    }
    return {ProcessorRestoreCode::Restored, 0, {}, std::move(candidates)};
}

} // namespace pulp::host
