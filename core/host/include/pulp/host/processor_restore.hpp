#pragma once

#include <pulp/format/processor_node_adapter.hpp>

#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace pulp::host {

struct ProcessorTypeKey {
    std::string type_id;
    int type_version = 1;
    friend bool operator==(const ProcessorTypeKey&, const ProcessorTypeKey&) = default;
};

enum class ProcessorRestoreCode {
    Restored,
    InvalidDescriptor,
    DuplicateProcessorType,
    InvalidRecord,
    StateLimitExceeded,
    UnknownProcessorType,
    ProcessorTypeDisallowed,
    UnsupportedVersion,
    ProcessorConstructionFailed,
    ProcessorInstanceNotFresh,
    ProcessorPortMismatch,
    ProcessorCapabilityMismatch,
    ProcessorStateRestoreFailed,
};

/// Stable names for machine-readable diagnostics; messages are supplementary.
const char* processor_restore_code_name(ProcessorRestoreCode code) noexcept;

/// Application-owned, exact-version registration. No discovery or migration
/// occurs. The envelope version is independent of the StateStore schema: the
/// embedded parameter version must match the factory's store version exactly.
/// Register another exact type version explicitly when required.
struct ProcessorRestoreDescriptor {
    ProcessorTypeKey type;
    int state_schema_version = 1;
    int input_channels = 0;
    int output_channels = 0;
    format::NodeCapabilities capabilities;
    /// Trusted control-thread code. Return a newly created, unprepared instance
    /// without retaining it or exposing it to another thread. Aliases to live or
    /// candidate instances are rejected before any parameter/custom restoration.
    std::function<std::shared_ptr<format::ProcessorNodeInstance>()> create;
};

/// Already decoded two-part state. The caller owns these spans for the duration
/// of restore_batch(). An eventual file reader must bound encoded input BEFORE
/// decoding; these limits do not make an unbounded decoder safe.
struct ProcessorRestoreRecord {
    std::uint32_t node_id = 0;
    ProcessorTypeKey type;
    int state_schema_version = 1;
    int input_channels = 0;
    int output_channels = 0;
    std::span<const std::uint8_t> parameters;
    std::span<const std::uint8_t> custom;
};

struct RestoredProcessor {
    std::uint32_t node_id = 0;
    std::shared_ptr<format::ProcessorNodeInstance> instance;
};

struct ProcessorRestoreResult {
    ProcessorRestoreCode code = ProcessorRestoreCode::InvalidRecord;
    std::uint32_t node_id = 0;
    ProcessorTypeKey type;
    /// Empty on every failure, including a failure after an earlier restore.
    /// Success transfers all unprepared instances to the caller in input order.
    std::vector<RestoredProcessor> processors;
    bool ok() const noexcept {
        return code == ProcessorRestoreCode::Restored;
    }
};

/// Control-thread building block for hosts restoring a set of Processor nodes.
/// Descriptors cannot be replaced or removed. Finish registration before use;
/// concurrent registration, restoration, or factory reentry is unsupported.
///
/// This is NOT a graph-file loader or a live graph transaction: it does not
/// capture state, parse JSON/base64, migrate, prepare, validate graph edges, or
/// publish. GraphSerializer still cannot persist Processor nodes. Applications
/// can add a successful batch to an isolated candidate via add_processor_node().
/// A later failure must discard that candidate without changing the live graph.
class ProcessorRestoreRegistry {
  public:
    static constexpr std::size_t max_nodes = 4096;
    static constexpr std::size_t max_state_part_bytes = 8 * 1024 * 1024;
    static constexpr std::size_t max_total_state_bytes = 32 * 1024 * 1024;

    ProcessorRestoreCode register_type(ProcessorRestoreDescriptor descriptor);

    /// Validates the entire batch and its explicit allowlist before executing
    /// factories; constructs and checks all identities before restoring either
    /// payload. StateStore is bound by ProcessorNodeInstance::create(). Both
    /// restore calls must succeed. Failed candidates die here, off the audio
    /// thread. Trusted callbacks must not mutate external/live state themselves.
    ProcessorRestoreResult restore_batch(std::span<const ProcessorRestoreRecord> records,
                                         std::span<const ProcessorTypeKey> allowlist) const;

  private:
    std::map<std::pair<std::string, int>, ProcessorRestoreDescriptor> descriptors_;
};

} // namespace pulp::host
