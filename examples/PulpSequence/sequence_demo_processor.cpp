#include "sequence_demo_processor.hpp"

#include <pulp/midi/block_ops.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/sequence/sequence_processor.hpp>
#include <pulp/timeline/model.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::examples {
namespace {

class InlineCompileExecutor final : public playback::CompileExecutor {
  public:
    bool submit(std::unique_ptr<playback::CompileTask> task,
                std::chrono::steady_clock::time_point) override {
        if (!task)
            return false;
        while (task->run_slice({
                   std::chrono::steady_clock::now() + std::chrono::seconds(1),
                   4096,
               }) == playback::CompileTaskStatus::Pending) {
        }
        return true;
    }
};

std::shared_ptr<const timeline::Project> make_demo_project() {
    std::vector<timeline::NoteEvent> notes;
    notes.reserve(16);
    for (std::uint64_t index = 0; index < 16; ++index) {
        notes.push_back({
            {100 + index},
            {static_cast<std::int64_t>(index) * timebase::kTicksPerQuarter},
            {timebase::kTicksPerQuarter / 2},
            0xc000,
            static_cast<std::uint8_t>(60 + index % 5),
            0,
        });
    }
    auto content = timeline::NoteContent::create(std::move(notes));
    if (!content)
        return {};
    const timebase::TickDuration duration{16 * timebase::kTicksPerQuarter};
    auto clip = timeline::Clip::create({20}, {0}, duration, std::move(content).value());
    if (!clip)
        return {};
    auto track = timeline::Track::create({10}, "embedded sequence", {std::move(clip).value()});
    if (!track)
        return {};
    auto sequence = timeline::Sequence::create(
        {2}, "root", duration, std::vector<timeline::Track>{std::move(track).value()});
    if (!sequence)
        return {};
    auto project = timeline::Project::create({
        {1},
        "Pulp Sequence",
        10'000,
        {2},
        {},
        {std::move(sequence).value()},
    });
    if (!project)
        return {};
    return std::make_shared<const timeline::Project>(std::move(project).value());
}

class PulpSequenceDemoProcessor final : public format::Processor {
  public:
    PulpSequenceDemoProcessor()
        : project_(make_demo_project()),
          compiler_(store_, compile_executor_, std::chrono::microseconds(0)) {}

    ~PulpSequenceDemoProcessor() override {
        stop_logger();
    }

    format::PluginDescriptor descriptor() const override {
        sequence::SequenceProcessorConfig config;
        config.name = "Pulp Sequence";
        config.bundle_id = "com.pulp.sequence-demo";
        config.output_channels = 2;
        sequence::SequenceProcessor descriptor_source(store_, config);
        return descriptor_source.descriptor();
    }

    void define_parameters(state::StateStore&) override {}

    void prepare(const format::PrepareContext& context) override {
        stop_logger();
        delegate_.reset();
        was_playing_ = false;
        active_note_count_ = 0;
        active_notes_.fill(0);
        if (!project_ || context.sample_rate <= 0.0 || !std::isfinite(context.sample_rate))
            return;

        const auto rounded_rate = static_cast<std::uint64_t>(std::llround(context.sample_rate));
        if (rounded_rate == 0 ||
            std::abs(context.sample_rate - static_cast<double>(rounded_rate)) > 1.0e-9)
            return;
        const timebase::TempoMap editable_map;
        auto compiled = timebase::CompiledTempoMap::compile(editable_map, {rounded_rate, 1});
        if (!compiled)
            return;
        auto tempo_map =
            std::make_shared<const timebase::CompiledTempoMap>(std::move(compiled).value());
        auto assets = playback::DecodedAudioAssetPool::create({});
        if (!assets)
            return;

        playback::ProgramCompileRequest request;
        request.project = project_;
        request.sequence_id = {2};
        request.tempo_map = tempo_map;
        request.audio_assets = std::move(assets).value();
        request.document_revision = ++revision_;
        request.dirty.all = true;
        if (!compiler_.submit(std::move(request)) || compiler_.status().has_error)
            return;

        sequence::SequenceProcessorConfig config;
        config.name = "Pulp Sequence";
        config.bundle_id = "com.pulp.sequence-demo";
        config.output_channels = 2;
        delegate_ = std::make_unique<sequence::SequenceProcessor>(store_, config);
        delegate_->prepare(context);
        if (!delegate_->ready()) {
            delegate_.reset();
            return;
        }
        start_logger();
        runtime::log_info("[seq-loop] loaded events=32 len_qn=16.000");
    }

    void release() override {
        stop_logger();
        if (delegate_)
            delegate_->release();
        delegate_.reset();
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const format::ProcessContext& context) override {
        if (!delegate_) {
            output.clear();
            return;
        }
        delegate_->process(output, input, midi_in, midi_out, context);
        update_active_notes(midi_out);

        if (context.is_playing && !was_playing_)
            push({LogRecordKind::Play});
        was_playing_ = context.is_playing;
        if (!context.is_playing)
            return;

        const auto observation = delegate_->last_observation();
        LogRecord record;
        record.kind = LogRecordKind::Block;
        record.host_qn = context.position_beats;
        record.sequence_qn = static_cast<double>(observation.timeline_tick_start.value) /
                             static_cast<double>(timebase::kTicksPerQuarter);
        record.active_notes = active_note_count_;
        record.jump =
            observation.discontinuity || context.transport_jump || context.reset_requested;
        record.dropout = !delegate_->ready() || !observation.valid ||
                         midi::midi_block_has_drops(midi_out) ||
                         queue_overflow_.exchange(false, std::memory_order_relaxed);
        push(record);
    }

    bool has_editor() const override {
        return false;
    }

  private:
    enum class LogRecordKind : std::uint8_t { Play, Block };
    struct LogRecord {
        LogRecordKind kind = LogRecordKind::Block;
        double host_qn = 0.0;
        double sequence_qn = 0.0;
        std::uint32_t active_notes = 0;
        bool jump = false;
        bool dropout = false;
    };

    static constexpr std::uint32_t kLogCapacity = 2048;

    bool push(LogRecord record) noexcept {
        const auto write = write_index_.load(std::memory_order_relaxed);
        const auto next = (write + 1) % kLogCapacity;
        if (next == read_index_.load(std::memory_order_acquire)) {
            queue_overflow_.store(true, std::memory_order_relaxed);
            return false;
        }
        log_records_[write] = record;
        write_index_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(LogRecord& record) noexcept {
        const auto read = read_index_.load(std::memory_order_relaxed);
        if (read == write_index_.load(std::memory_order_acquire))
            return false;
        record = log_records_[read];
        read_index_.store((read + 1) % kLogCapacity, std::memory_order_release);
        return true;
    }

    void start_logger() {
        logger_stop_.store(false, std::memory_order_release);
        read_index_.store(0, std::memory_order_relaxed);
        write_index_.store(0, std::memory_order_relaxed);
        queue_overflow_.store(false, std::memory_order_relaxed);
        logger_ = std::thread([this] {
            LogRecord record;
            for (;;) {
                bool consumed = false;
                while (pop(record)) {
                    consumed = true;
                    if (record.kind == LogRecordKind::Play) {
                        runtime::log_info("[seq-loop] play");
                    } else {
                        runtime::log_info("[seq-loop] blk host_qn={:.6f} "
                                          "seq_qn={:.6f} active={} jump={} "
                                          "dropout={}",
                                          record.host_qn, record.sequence_qn, record.active_notes,
                                          record.jump ? 1 : 0, record.dropout ? 1 : 0);
                    }
                }
                if (logger_stop_.load(std::memory_order_acquire) &&
                    read_index_.load(std::memory_order_acquire) ==
                        write_index_.load(std::memory_order_acquire))
                    break;
                if (!consumed)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }

    void stop_logger() {
        logger_stop_.store(true, std::memory_order_release);
        if (logger_.joinable())
            logger_.join();
    }

    void update_active_notes(const midi::MidiBuffer& events) noexcept {
        for (const auto& event : events) {
            if (!event.is_note_on() && !event.is_note_off())
                continue;
            const auto key = static_cast<std::size_t>(event.channel()) * 128 + event.note();
            if (event.is_note_on() && event.velocity() != 0) {
                if (active_notes_[key] == 0)
                    ++active_note_count_;
                if (active_notes_[key] != std::numeric_limits<std::uint16_t>::max())
                    ++active_notes_[key];
            } else if (active_notes_[key] != 0) {
                --active_notes_[key];
                if (active_notes_[key] == 0 && active_note_count_ != 0)
                    --active_note_count_;
            }
        }
    }

    std::shared_ptr<const timeline::Project> project_;
    playback::PlaybackProgramStore store_;
    InlineCompileExecutor compile_executor_;
    playback::PlaybackProgramCompiler compiler_;
    std::unique_ptr<sequence::SequenceProcessor> delegate_;
    std::uint64_t revision_ = 0;

    std::array<std::uint16_t, 16 * 128> active_notes_{};
    std::uint32_t active_note_count_ = 0;
    bool was_playing_ = false;

    std::array<LogRecord, kLogCapacity> log_records_{};
    std::atomic<std::uint32_t> read_index_{0};
    std::atomic<std::uint32_t> write_index_{0};
    std::atomic<bool> queue_overflow_{false};
    std::atomic<bool> logger_stop_{true};
    std::thread logger_;
};

} // namespace

std::unique_ptr<format::Processor> create_pulp_sequence() {
    return std::make_unique<PulpSequenceDemoProcessor>();
}

} // namespace pulp::examples
