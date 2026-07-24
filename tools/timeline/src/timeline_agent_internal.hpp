#pragma once

#include <pulp/playback/program_compiler.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace pulp::audio {
struct AudioFileData;
}

namespace pulp::tools::timeline::detail {

constexpr std::uint64_t kMaxAssetWorkingSetBytes = 512ull * 1024 * 1024;
constexpr std::uint64_t kMaxRenderPcmBytes = 512ull * 1024 * 1024;

OperationResult failure(std::string_view stage, std::string_view message,
                        std::string_view path = {}, int exit_code = 1);
std::string persistence_message(const pulp::timeline::PersistenceError& error);

struct LoadedProject {
    pulp::timeline::Project value;
    std::filesystem::path base_directory;
};

struct CompiledProject {
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map;
    playback::PlaybackProgramStore store;
};

runtime::Result<LoadedProject, pulp::timeline::PersistenceError>
load_project(const ProjectSource& source, const pulp::timeline::SchemaRegistry& registry);

runtime::Result<std::unique_ptr<CompiledProject>, playback::CompileError>
compile_project(const LoadedProject& loaded, std::uint32_t sample_rate);

std::uint64_t render_frame_count(const pulp::timeline::Sequence& sequence,
                                 const timebase::CompiledTempoMap& tempo_map,
                                 const playback::PlaybackProgram& program,
                                 std::uint32_t sample_rate);

std::string compile_error_message(const playback::CompileError& error);

enum class AtomicWriteOutcome : std::uint8_t {
    NotReplaced,
    ReplacedDurably,
    ReplacedButDirectorySyncFailed,
};

AtomicWriteOutcome write_wav_atomic(const std::filesystem::path& destination,
                                    const pulp::audio::AudioFileData& audio) noexcept;

} // namespace pulp::tools::timeline::detail
