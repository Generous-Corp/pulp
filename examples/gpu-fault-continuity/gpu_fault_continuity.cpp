#include "gpu_fault_continuity.hpp"

// Test-only: this plug-in fires GPU faults on request and must never ship.
#if !defined(PULP_GPU_FAULT_TEST_ONLY)
#error "the GPU fault-continuity test plug-in builds only with PULP_GPU_FAULT_TEST_ONLY"
#endif
#include "gpu_fault_interposer.hpp"
#include "gpu_fault_rendezvous.hpp"
#include "gpu_fault_seam.hpp"
#include "gpu_fault_stimulus.hpp"

#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_status.hpp>
#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pulp::examples::gpu_fault {
namespace {

// Capture format, mirrored by tools/testing/daw-smoke/gpu_fault_continuity.py.
constexpr char kCaptureMagic[4] = {'P', 'G', 'F', 'C'};
// v2 appends to each record header the host time and the transport's
// cumulative delivery counters at the end of the record, so the reader can
// derive which path delivered every quantum.
constexpr std::uint32_t kCaptureVersion = 2;
constexpr std::uint64_t kPositionNone = UINT64_MAX;
constexpr std::uint32_t kFlagPlaying = 1u << 0;
constexpr std::uint32_t kFlagOffline = 1u << 1;
constexpr std::uint32_t kFlagFaultBlock = 1u << 2;
constexpr std::uint32_t kFlagSegmentStart = 1u << 3;
// A segment-start record carries the low 16 bits of the prepare it follows, so
// the reader can pair it with that prepare's `prepared` event even when a host
// prepares several times between two process calls (the VST3 adapter does).
constexpr std::uint32_t kPrepareIndexShift = 16;
constexpr std::size_t kRingBytes = 16u << 20;

struct RecordHeader {
    std::uint64_t position;
    std::uint32_t frames;
    std::uint32_t flags;
    std::uint64_t host_time_ns;
    std::uint64_t gpu_blocks;
    std::uint64_t fallback_blocks;
    std::uint64_t silence_blocks;
    std::uint64_t priming_blocks;
};
static_assert(sizeof(RecordHeader) == 56);

constexpr std::array<std::pair<FaultKind, const char*>, 8> kFaultNames{{
    {FaultKind::ForceLossBefore, "force_loss_before"},
    {FaultKind::ForceLossBetween, "force_loss_between"},
    {FaultKind::ForceLossAfter, "force_loss_after"},
    {FaultKind::DelayCompletion, "delay_completion"},
    {FaultKind::WorkerStall, "worker_stall"},
    {FaultKind::MissingCompletion, "missing_completion"},
    {FaultKind::RenderForceLoss, "render_force_loss"},
    {FaultKind::Control, "positive_control"},
}};

constexpr std::array<std::pair<Planted, const char*>, 3> kPlantedNames{{
    {Planted::NoFallback, "no_fallback"},
    {Planted::FallbackMisaligned, "fallback_misaligned"},
    {Planted::FallbackStale, "fallback_stale"},
}};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Run-directory files are opened without following a symlink, and must be
// regular files: the run directory is shared with the harness, and a planted
// link must not redirect a read or a write elsewhere.
int open_nofollow(const std::filesystem::path& path, int flags) {
    const int fd = ::open(path.c_str(), flags | O_NOFOLLOW | O_CLOEXEC, 0644);
    struct stat st{};
    if (fd >= 0 && (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool read_nofollow(const std::filesystem::path& path, std::string& out) {
    const int fd = open_nofollow(path, O_RDONLY);
    if (fd < 0)
        return false;
    out.clear();
    char buf[4096];
    ssize_t got = 0;
    while ((got = ::read(fd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(got));
    ::close(fd);
    return got == 0;
}

void write_all(int fd, const void* data, std::size_t size) {
    const auto* p = static_cast<const char*>(data);
    while (fd >= 0 && size > 0) {
        const ssize_t wrote = ::write(fd, p, size);
        if (wrote <= 0)
            return;
        p += wrote;
        size -= static_cast<std::size_t>(wrote);
    }
}

// The request file is written by the harness: {"fault": "...", "at_project_sample": N,
// "after_project_sample": M}. A flat object, so a key scan is enough.
std::string json_string(const std::string& text, const char* key) {
    const auto at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos)
        return {};
    const auto open = text.find('"', text.find(':', at) + 1);
    const auto close = open == std::string::npos ? open : text.find('"', open + 1);
    return close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
}

std::int64_t json_int(const std::string& text, const char* key) {
    const auto at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos)
        return -1;
    return std::strtoll(text.c_str() + text.find(':', at) + 1, nullptr, 10);
}

std::string escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20)
            out.push_back(c);
    }
    return out;
}

// Enum spellings follow the WP-A2 gpu-audio-health-read-v1 schema, so the
// plug-in's snapshots and the control op's read the same way.
const char* state_name(gpu_audio::GpuAudioProviderState s) {
    switch (s) {
    case gpu_audio::GpuAudioProviderState::Ready:
        return "ready";
    case gpu_audio::GpuAudioProviderState::Degraded:
        return "degraded";
    case gpu_audio::GpuAudioProviderState::Unavailable:
        return "unavailable";
    default:
        return "uninitialized";
    }
}

const char* reason_name(gpu_audio::GpuAudioRecoveryReason r) {
    using R = gpu_audio::GpuAudioRecoveryReason;
    switch (r) {
    case R::SequenceGap:
        return "sequence-gap";
    case R::InputSaturated:
        return "input-saturated";
    case R::ProviderFailure:
        return "provider-failure";
    case R::ProviderLost:
        return "provider-lost";
    case R::OfflineFence:
        return "offline-fence";
    case R::InvalidCallback:
        return "invalid-callback";
    case R::ProviderUnresponsive:
        return "provider-unresponsive";
    case R::ProviderReacquired:
        return "provider-reacquired";
    default:
        return "none";
    }
}

const char* provider_name(gpu_audio::GpuAudioProvider p) {
    switch (p) {
    case gpu_audio::GpuAudioProvider::Dawn:
        return "dawn";
    case gpu_audio::GpuAudioProvider::Metal:
        return "metal";
    case gpu_audio::GpuAudioProvider::Cpu:
        return "cpu";
    default:
        return "unknown";
    }
}

const char* path_name(gpu_audio::GpuAudioExecutionPath p) {
    switch (p) {
    case gpu_audio::GpuAudioExecutionPath::Staged:
        return "staged";
    case gpu_audio::GpuAudioExecutionPath::SharedMemory:
        return "shared_memory";
    case gpu_audio::GpuAudioExecutionPath::Cpu:
        return "cpu";
    default:
        return "unavailable";
    }
}

} // namespace

const char* fault_name(FaultKind kind) noexcept {
    for (const auto& [k, name] : kFaultNames)
        if (k == kind)
            return name;
    return "none";
}

FaultKind parse_fault(const std::string& name) noexcept {
    for (const auto& [k, n] : kFaultNames)
        if (name == n)
            return k;
    return FaultKind::None;
}

// ── CaptureRing ──────────────────────────────────────────────────────────

void CaptureRing::reset(std::size_t capacity) {
    buffer_.assign(capacity, 0);
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

void CaptureRing::write_bytes(std::size_t at, const void* src, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(src);
    const std::size_t cap = buffer_.size();
    const std::size_t start = at % cap;
    const std::size_t first = std::min(size, cap - start);
    std::memcpy(buffer_.data() + start, bytes, first);
    std::memcpy(buffer_.data(), bytes + first, size - first);
}

bool CaptureRing::push(const void* header, std::size_t header_size, const float* const* planes,
                       std::size_t plane_count, std::size_t frames) noexcept {
    const std::size_t need = header_size + plane_count * frames * sizeof(float);
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (buffer_.empty() || buffer_.size() - (head - tail) < need) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::size_t at = head;
    write_bytes(at, header, header_size);
    at += header_size;
    for (std::size_t p = 0; p < plane_count; ++p) {
        write_bytes(at, planes[p], frames * sizeof(float));
        at += frames * sizeof(float);
    }
    head_.store(at, std::memory_order_release);
    return true;
}

std::size_t CaptureRing::drain(std::vector<std::uint8_t>& out) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t size = head - tail;
    if (size == 0 || buffer_.empty())
        return 0;
    const std::size_t cap = buffer_.size();
    const std::size_t start = tail % cap;
    const std::size_t first = std::min(size, cap - start);
    out.insert(out.end(), buffer_.begin() + static_cast<std::ptrdiff_t>(start),
               buffer_.begin() + static_cast<std::ptrdiff_t>(start + first));
    out.insert(out.end(), buffer_.begin(),
               buffer_.begin() + static_cast<std::ptrdiff_t>(size - first));
    tail_.store(head, std::memory_order_release);
    return size;
}

// ── Processor ────────────────────────────────────────────────────────────

GpuFaultContinuityProcessor::GpuFaultContinuityProcessor() {
    editor_link_->processor = this;
    // The environment (REAPER) or, for hosts that cannot pass it on, the
    // rendezvous file (gpu_fault_rendezvous.hpp).
    const RunConfig config = resolve_run_config();
    run_dir_ = config.run_dir;
    request_path_ = config.request;
    ir_path_ = config.ir;
    nonce_ = config.nonce;
    config_source_ = config.source;
    gpu_off_ = config.provider_staged;
    // Fixed here, before any callback, so a run never captures a single host
    // input sample before switching to the internal stimulus.
    internal_stimulus_.store(config.internal_stimulus, std::memory_order_relaxed);
}

void GpuFaultContinuityProcessor::claim_run_dir(std::uint32_t sample_rate) {
    // REAPER may create several instances in one process (a scan, the insert),
    // all seeing the same environment. Exactly one, the first to prepare,
    // owns the run directory; the rest behave as plain convolvers.
    if (harness_ || run_dir_.empty())
        return;
    const auto claim = run_dir_ / "claim";
    // O_EXCL refuses an existing path, a symlink included.
    const int fd = ::open(claim.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    // The claim records which process won and when; a receipt binds both to
    // the host_process event below and to every `prepared` (WP-K).
    HostProcess process = current_host_process();
    process.claimed_unix_us = unix_now_us();
    const std::string record = claim_record(process);
    write_all(fd, record.data(), record.size());
    ::close(fd);
    // Start the capture and the event log without following a planted link.
    const int capture = open_nofollow(run_dir_ / "capture.bin", O_WRONLY | O_CREAT | O_TRUNC);
    if (capture < 0)
        return;
    const int events = open_nofollow(run_dir_ / "events.jsonl", O_WRONLY | O_CREAT | O_TRUNC);
    if (events < 0) {
        ::close(capture);
        return;
    }
    const std::uint32_t header[4] = {kCaptureVersion, sample_rate, kChannels, 0u};
    write_all(capture, kCaptureMagic, 4);
    write_all(capture, header, sizeof(header));
    ::close(capture);
    ::close(events);
    harness_ = true;
    // The host's log names the plug-in only in some formats (CLAP does, AU v2
    // does not), so the instance that owns the run says so itself; the
    // verifier looks for the plug-in's name in that log.
    runtime::log_info("gpu-fault-continuity: '{}' claimed the run directory {}", descriptor().name,
                      run_dir_.string());
    ring_.reset(kRingBytes);
    RunConfig config;
    config.source = config_source_;
    config.nonce = nonce_;
    emit(host_process_event(process, config));
    running_.store(true, std::memory_order_release);
    service_ = std::thread([this] { service_loop(); });
}

GpuFaultContinuityProcessor::~GpuFaultContinuityProcessor() {
    {
        std::lock_guard lock(editor_link_->mutex);
        editor_link_->processor = nullptr;
    }
    release_engine();
    running_.store(false, std::memory_order_release);
    if (service_.joinable())
        service_.join();
}

format::PluginDescriptor GpuFaultContinuityProcessor::descriptor() const {
    return {
        .name = "Pulp GPU Fault Continuity",
        .manufacturer = "Pulp",
        .bundle_id = "com.pulp.gpu-fault-continuity",
        .version = "0.1.0",
        .category = format::PluginCategory::Effect,
        .input_buses = {{"Audio In", kChannels}},
        .output_buses = {{"Audio Out", kChannels}},
        .accepts_midi = false,
        .produces_midi = false,
        .tail_samples = 0,
    };
}

int GpuFaultContinuityProcessor::latency_samples() const {
    // One staging quantum plus the transport's own reported PDC, read at
    // prepare so it follows the transport rather than a constant.
    return static_cast<int>(latency_samples_);
}

void GpuFaultContinuityProcessor::prepare(const format::PrepareContext& context) {
    release_engine();
    sample_rate_ = static_cast<std::uint32_t>(context.sample_rate);
    max_block_ = static_cast<std::uint32_t>(std::max(1, context.max_buffer_size));
    // The quantum is the host's whole prepared block, not a fraction of it.
    // The transport's lead is counted in quanta, so with a smaller quantum
    // several quanta run back to back inside one callback and the lead shrinks
    // to microseconds: in REAPER (512-frame callbacks, 128-frame quanta) the
    // GPU never delivered a single block and every one was CPU fallback.
    quantum_ = std::clamp(max_block_, kMinQuantum, kMaxQuantum);
    stage_in_.assign(static_cast<std::size_t>(kChannels) * quantum_, 0.0f);
    stage_out_.assign(static_cast<std::size_t>(kChannels) * quantum_, 0.0f);
    stage_prev_.assign(static_cast<std::size_t>(kChannels) * quantum_, 0.0f);
    scratch_in_.assign(static_cast<std::size_t>(kChannels) * max_block_, 0.0f);
    scratch_out_.assign(static_cast<std::size_t>(kChannels) * max_block_, 0.0f);
    in_ptrs_.assign(kChannels, nullptr);
    out_ptrs_.assign(kChannels, nullptr);
    stage_fill_ = 0;
    segment_start_ = true;
    ++prepare_index_;
    claim_run_dir(sample_rate_);
    prepare_engine();
}

void GpuFaultContinuityProcessor::prepare_engine() {
    std::lock_guard lock(engine_mutex_);
    engine_generation_ = prepare_index_;
    ir_.clear();
    if (!ir_path_.empty()) {
        std::string bytes;
        if (!read_nofollow(ir_path_, bytes))
            bytes.clear();
        ir_.resize(bytes.size() / sizeof(float));
        std::memcpy(ir_.data(), bytes.data(), ir_.size() * sizeof(float));
    }
    if (ir_.empty())
        ir_ = {1.0f};

    node_ = std::make_unique<gpu_audio::GpuConvolver>(kChannels, quantum_, sample_rate_, ir_);
    transport_ = std::make_unique<gpu_audio::GpuAudioTransport>();
    // SharedRequired: the shared-I/O GPU path or nothing. The spectral nodes'
    // shared-device refusal (WP-D) does not apply to the convolver, which is
    // why it is the h1 vehicle.
    std::string interposer_detail = "not a harness run";
    if (harness_) {
        if (!provider_fault_interposer_installed()) {
            // The provider installs Dawn's native table once per process, on
            // its first prepare. Let a throwaway node make that installation,
            // so the interposer can be in place before this node prepares and
            // see which device is this instance's.
            gpu_audio::GpuConvolver warm(1, quantum_, sample_rate_, {1.0f});
            if (warm.set_provider_policy(gpu_audio::GpuConvolver::ProviderPolicy::SharedRequired) &&
                warm.prepare())
                install_provider_fault_interposer(interposer_detail);
        }
        begin_provider_fault_target_capture();
    }
    // gpu_off is the path negative: the staged path must make the run
    // INCONCLUSIVE ("not on the shared-I/O path"), never a PASS.
    const auto policy = gpu_off_ ? gpu_audio::GpuConvolver::ProviderPolicy::StagedOnly
                                 : gpu_audio::GpuConvolver::ProviderPolicy::SharedRequired;
    const bool node_ok = node_->set_provider_policy(policy) && node_->prepare();
    if (harness_ && provider_fault_interposer_installed())
        end_provider_fault_target_capture(interposer_detail);
    target_device_ = provider_fault_target_device();
    const gpu_audio::GpuAudioTransport::Config config{
        .ring_blocks = 8,
        .run_worker_thread = true,
        .wake_on_write = true,
    };
    const bool transport_ok = node_ok && transport_->prepare(node_.get(), config);
    latency_samples_ =
        quantum_ + (transport_ok ? transport_->latency_samples()
                                 : gpu_audio::GpuConvolver::kLatencyBlocks * quantum_);
    const auto capability =
        transport_ok ? transport_->capability_report() : gpu_audio::GpuAudioCapabilityReport{};
    provider_name_ = provider_name(capability.provider);
    path_name_ = path_name(capability.path);
    shared_path_ =
        transport_ok && capability.path == gpu_audio::GpuAudioExecutionPath::SharedMemory;
    engine_ready_.store(transport_ok, std::memory_order_release);

    if (harness_) {
        // node/storage/timestamps name the configuration a verifier may need
        // to exclude; storage is the path the provider actually took.
        const char* storage = "none";
        if (capability.path == gpu_audio::GpuAudioExecutionPath::SharedMemory)
            storage = "shared";
        else if (capability.path == gpu_audio::GpuAudioExecutionPath::Staged)
            storage = "staged";
        const bool timestamps = transport_ok && transport_->status_snapshot().timestamps_available;
        char line[896];
        std::snprintf(
            line, sizeof(line),
            "{\"event\":\"prepared\",\"ok\":%s,\"pdc_samples\":%d,\"quantum\":%u,"
            "\"lead_blocks\":%u,\"ir_taps\":%zu,\"provider\":\"%s\",\"path\":\"%s\","
            "\"sample_rate\":%u,\"max_block\":%u,\"audio_fault_seam\":%s,"
            "\"seam_detail\":\"%s\",\"nonce\":\"%s\",\"pid\":%d,"
            "\"prepare_index\":%u,\"provider_device\":\"0x%llx\",\"node\":\"convolver\","
            "\"storage\":\"%s\",\"timestamps\":%s}",
            transport_ok ? "true" : "false", latency_samples(), quantum_,
            capability.prepared_lead_blocks, ir_.size(), provider_name_.c_str(), path_name_.c_str(),
            sample_rate_, max_block_, audio_fault_seam_available() ? "true" : "false",
            escape(interposer_detail).c_str(), escape(nonce_).c_str(), static_cast<int>(::getpid()),
            prepare_index_ & 0xFFFFu, static_cast<unsigned long long>(target_device_), storage,
            timestamps ? "true" : "false");
        emit(line);
    }
}

void GpuFaultContinuityProcessor::release() {
    release_engine();
}

void GpuFaultContinuityProcessor::release_engine() {
    engine_ready_.store(false, std::memory_order_release);
    std::lock_guard lock(engine_mutex_);
    // Under the engine lock, so an arm cannot slip in between this and the
    // release; disarming also ends a stall the worker is in.
    if (harness_)
        disarm_provider_fault();
    if (transport_)
        transport_->release();
    transport_.reset();
    node_.reset();
}

void GpuFaultContinuityProcessor::process(audio::BufferView<float>& output,
                                          const audio::BufferView<const float>& input,
                                          midi::MidiBuffer&, midi::MidiBuffer&,
                                          const format::ProcessContext& context) {
    const auto n = static_cast<std::uint32_t>(output.num_samples());
    const std::uint32_t out_ch = static_cast<std::uint32_t>(output.num_channels());
    const bool offline = context.process_mode == format::ProcessMode::Offline;
    const bool ready = engine_ready_.load(std::memory_order_acquire);
    const bool playing = context.is_playing && context.position_samples >= 0;
    const std::int64_t position = context.position_samples;

    std::uint32_t flags =
        (playing ? kFlagPlaying : 0u) | (offline ? kFlagOffline : 0u) |
        (segment_start_ ? kFlagSegmentStart | ((prepare_index_ & 0xFFFFu) << kPrepareIndexShift)
                        : 0u);
    segment_start_ = false;
    const std::int64_t fault_at = fault_at_.load(std::memory_order_acquire);
    bool fault_block = false;
    if (playing && fault_at >= position && fault_at < position + n &&
        armed_kind_.load(std::memory_order_acquire) != 0 &&
        !fire_pending_.load(std::memory_order_relaxed)) {
        flags |= kFlagFaultBlock;
        fire_offline_.store(offline, std::memory_order_relaxed);
        fire_generation_.store(prepare_index_, std::memory_order_relaxed);
        fire_pending_.store(true, std::memory_order_release);
        fault_block = true;
    }
    const auto planted = static_cast<Planted>(planted_.load(std::memory_order_relaxed));
    const bool internal = internal_stimulus_.load(std::memory_order_relaxed);
    if (playing)
        latest_position_.store(position + n, std::memory_order_release);

    const bool capture = harness_ && n <= max_block_;
    if (harness_ && !capture)
        ring_.note_drop();
    // The input this callback uses: the host's, or with the internal stimulus
    // (selected before the first callback) the plug-in's own signal keyed to
    // the project position, and silence while stopped. Host input is then never
    // read, so nothing from a host input (a live template's microphone) can
    // reach the node or the capture.
    float* in_plane[kChannels];
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        in_plane[ch] = scratch_in_.data() + static_cast<std::size_t>(ch) * max_block_;
        if (n > max_block_)
            continue;
        if (internal) {
            for (std::uint32_t i = 0; i < n; ++i)
                in_plane[ch][i] =
                    playing
                        ? internal_stimulus(static_cast<std::uint64_t>(position) + i, sample_rate_)
                        : 0.0f;
        } else if (ch < input.num_channels()) {
            std::copy_n(input.channel_ptr(ch), n, in_plane[ch]);
        } else {
            std::fill_n(in_plane[ch], n, 0.0f);
        }
    }
    const bool usable = n <= max_block_;

    // Stage host samples into the fixed transport quantum: the sample written
    // at slot f leaves exactly one quantum later, whatever the callback size.
    for (std::uint32_t i = 0; i < n; ++i) {
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            const std::size_t slot = static_cast<std::size_t>(ch) * quantum_ + stage_fill_;
            stage_in_[slot] = usable ? in_plane[ch][i] : 0.0f;
            if (ch < out_ch)
                output.channel_ptr(ch)[i] = stage_out_[slot];
        }
        if (++stage_fill_ == quantum_) {
            stage_fill_ = 0;
            run_quantum(ready, offline, planted);
        }
    }
    // A planted negative starts with the callback AFTER the fault block, so
    // every corrupted output sample is emitted after the fault sample and a
    // planted deviation can never begin before it.
    if (fault_block)
        planted_live_ = true;
    for (std::uint32_t ch = kChannels; ch < out_ch; ++ch)
        std::fill_n(output.channel_ptr(ch), n, 0.0f);

    if (capture) {
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            float* dst = scratch_out_.data() + static_cast<std::size_t>(ch) * max_block_;
            if (ch < out_ch)
                std::copy_n(output.channel_ptr(ch), n, dst);
            else
                std::fill_n(dst, n, 0.0f);
        }
        const auto d = ready ? transport_->delivery_snapshot()
                             : gpu_audio::GpuAudioTransport::DeliverySnapshot{};
        const RecordHeader header{playing ? static_cast<std::uint64_t>(position) : kPositionNone,
                                  n,
                                  flags,
                                  now_ns(),
                                  d.gpu_blocks,
                                  d.cpu_fallback_blocks,
                                  d.silence_blocks,
                                  d.priming_blocks};
        const float* planes[2 * kChannels];
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            planes[ch] = scratch_in_.data() + static_cast<std::size_t>(ch) * max_block_;
            planes[kChannels + ch] =
                scratch_out_.data() + static_cast<std::size_t>(ch) * max_block_;
        }
        ring_.push(&header, sizeof(header), planes, 2 * kChannels, n);
    }
}

void GpuFaultContinuityProcessor::run_quantum(bool ready, bool offline, Planted planted) noexcept {
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        in_ptrs_[ch] = stage_in_.data() + static_cast<std::size_t>(ch) * quantum_;
        out_ptrs_[ch] = stage_out_.data() + static_cast<std::size_t>(ch) * quantum_;
    }
    const bool live = planted_live_ && planted != Planted::Off;
    if (live && planted == Planted::FallbackStale && !stale_done_) {
        // Withhold this quantum's input from the node: its history goes stale
        // while the output stays continuous.
        std::fill(stage_in_.begin(), stage_in_.end(), 0.0f);
        stale_done_ = true;
    }
    const audio::BufferView<const float> q_in(in_ptrs_.data(), kChannels, quantum_);
    audio::BufferView<float> q_out(out_ptrs_.data(), kChannels, quantum_);
    const auto fallback_before = ready ? transport_->delivery_snapshot().cpu_fallback_blocks : 0;
    if (!ready)
        std::fill(stage_out_.begin(), stage_out_.end(), 0.0f);
    else if (offline)
        transport_->process_offline(q_in, q_out, quantum_);
    else
        transport_->process(q_in, q_out, quantum_);
    if (!live)
        return;
    if (planted == Planted::NoFallback && ready &&
        transport_->delivery_snapshot().cpu_fallback_blocks != fallback_before)
        std::fill(stage_out_.begin(), stage_out_.end(), 0.0f); // a dropout, not a fallback
    if (planted == Planted::FallbackMisaligned)
        stage_out_.swap(stage_prev_); // emit the previous quantum (one late), keep this one
}

// ── Service thread ───────────────────────────────────────────────────────

void GpuFaultContinuityProcessor::emit(const std::string& json_line) {
    std::lock_guard lock(events_mutex_);
    pending_events_.push_back(json_line);
}

std::string GpuFaultContinuityProcessor::status_json() const {
    std::lock_guard lock(engine_mutex_);
    if (!transport_)
        return "{\"provider_state\":\"uninitialized\",\"prepared\":false}";
    const auto s = transport_->status_snapshot();
    const auto d = transport_->delivery_snapshot();
    char buf[1400];
    std::snprintf(
        buf, sizeof(buf),
        "{\"schema_version\":%u,\"provider_state\":\"%s\",\"provider\":\"%s\","
        "\"recovery_reason\":\"%s\",\"recovery_sequence\":%s,\"produced_blocks\":%" PRIu64
        ",\"missed_blocks\":%" PRIu64 ",\"fallback_blocks\":%" PRIu64
        ",\"resynced_blocks\":%" PRIu64 ",\"input_dropped_frames\":%" PRIu64
        ",\"latency_samples\":%u,\"prepared_lead_blocks\":%u,\"epoch\":%" PRIu64
        ",\"last_block_us\":%.3f,\"delivery\":{\"gpu_blocks\":%" PRIu64
        ",\"cpu_fallback_blocks\":%" PRIu64 ",\"priming_blocks\":%" PRIu64
        ",\"silence_blocks\":%" PRIu64 "},\"source\":\"plugin_status_snapshot\"}",
        s.schema_version, state_name(s.provider_state), provider_name(s.provider),
        reason_name(s.recovery_reason),
        s.recovery_sequence == UINT64_MAX ? "null" : std::to_string(s.recovery_sequence).c_str(),
        s.produced_blocks, s.missed_blocks, s.fallback_blocks, s.resynced_blocks,
        s.input_dropped_frames, s.latency_samples, s.prepared_lead_blocks, s.epoch, s.last_block_us,
        d.gpu_blocks, d.cpu_fallback_blocks, d.priming_blocks, d.silence_blocks);
    return buf;
}

void GpuFaultContinuityProcessor::emit_health(const char* phase) {
    char head[160];
    std::snprintf(
        head, sizeof(head),
        "{\"event\":\"health\",\"phase\":\"%s\",\"project_sample\":%" PRId64 ",\"status\":", phase,
        latest_position_.load(std::memory_order_acquire));
    emit(std::string(head) + status_json() + "}");
}

void GpuFaultContinuityProcessor::poll_request() {
    // Not before the engine is prepared: the provider fault interposer is
    // installed during prepare, and an audio fault read earlier would be refused.
    std::error_code ec; // the service thread must never throw
    if (request_seen_ || request_path_.empty() || !engine_ready_.load(std::memory_order_acquire) ||
        !std::filesystem::exists(request_path_, ec))
        return;
    std::string body;
    if (!read_nofollow(request_path_, body))
        return;
    if (body.find('}') == std::string::npos)
        return; // still being written
    request_seen_ = true;
    const FaultKind kind = parse_fault(json_string(body, "fault"));
    const std::int64_t at = json_int(body, "at_project_sample");
    const std::int64_t after = json_int(body, "after_project_sample");
    const std::int64_t delay = json_int(body, "delay_ms");
    const bool audio = kind != FaultKind::RenderForceLoss && kind != FaultKind::Control;
    const std::string planted_name = json_string(body, "planted_negative");
    Planted planted = Planted::Off;
    for (const auto& [p, name] : kPlantedNames)
        if (planted_name == name)
            planted = p;
    std::string refusal;
    if (kind == FaultKind::None)
        refusal = "unknown fault kind";
    else if (!planted_name.empty() && planted == Planted::Off)
        refusal = "unknown planted negative";
    else if (at < 0)
        refusal = "missing at_project_sample";
    else if (audio && !audio_fault_seam_available() && !gpu_off_)
        // (With gpu_off the run goes ahead so its receipt can show the path
        // negative; the fault itself is refused when it fires.)
        refusal = audio_fault_unavailable_reason(kind);
    if (!refusal.empty()) {
        emit("{\"event\":\"fault_refused\",\"fault\":\"" + escape(json_string(body, "fault")) +
             "\",\"reason\":\"" + escape(refusal) + "\"}");
        return;
    }
    after_at_.store(after, std::memory_order_relaxed);
    // A delayed completion stalls the worker for longer than the lead; a worker
    // stall for seconds. Either way the callback must not notice.
    delay_ms_ = delay > 0
                    ? static_cast<std::uint32_t>(std::min<std::int64_t>(delay, kMaxProviderStallMs))
                    : (kind == FaultKind::WorkerStall ? 3000u : 100u);
    const bool internal = internal_stimulus_.load(std::memory_order_relaxed);
    planted_.store(static_cast<std::uint8_t>(planted), std::memory_order_relaxed);
    fault_at_.store(at, std::memory_order_release);
    armed_kind_.store(static_cast<std::uint8_t>(kind), std::memory_order_release);
    char line[320];
    std::snprintf(line, sizeof(line),
                  "{\"event\":\"fault_armed\",\"fault\":\"%s\",\"at_project_sample\":%" PRId64
                  ",\"after_project_sample\":%" PRId64
                  ",\"planted_negative\":%s,\"input\":\"%s\",\"gpu_off\":%s}",
                  fault_name(kind), at, after,
                  gpu_off_ ? "\"gpu_off\""
                           : (planted_name.empty() ? "null" : ("\"" + planted_name + "\"").c_str()),
                  internal ? "internal" : "host", gpu_off_ ? "true" : "false");
    emit(line);
}

void GpuFaultContinuityProcessor::fire_pending_fault() {
    if (fired_ || !fire_pending_.load(std::memory_order_acquire))
        return;
    fired_ = true;
    fired_ns_ = now_ns();
    const auto kind = static_cast<FaultKind>(armed_kind_.load(std::memory_order_acquire));
    emit_health("before");
    std::string detail;
    bool issued = true;
    if (kind == FaultKind::Control) {
        detail = "positive control: no fault injected";
    } else if (kind == FaultKind::RenderForceLoss) {
        std::lock_guard lock(hook_mutex_);
        issued = static_cast<bool>(render_loss_hook_);
        if (issued) {
            render_loss_requested_.store(true, std::memory_order_release);
            render_loss_hook_();
            detail = "queued to the editor's main thread";
        } else {
            detail = "no editor is open, so there is no render device to lose";
        }
    } else {
        // Armed under the engine lock and only for the engine whose callback
        // reached the fault block, so a host re-prepare in between cannot
        // leave the fault armed for (or aimed at) a different engine.
#if defined(PULP_GPU_FAULT_TEST_ONLY)
        if (before_arm_hook_)
            before_arm_hook_();
#endif
        std::lock_guard lock(engine_mutex_);
        if (!engine_ready_.load(std::memory_order_acquire) ||
            fire_generation_.load(std::memory_order_relaxed) != engine_generation_) {
            issued = false;
            detail = "the engine was re-prepared after the fault block; not arming a stale fault";
        } else {
            applied_before_ = provider_faults_applied();
            const auto result = inject_audio_fault(kind, delay_ms_);
            issued = result.accepted;
            detail = result.detail;
        }
    }
    char line[512];
    std::snprintf(
        line, sizeof(line),
        "{\"event\":\"%s\",\"fault\":\"%s\",\"at_project_sample\":%" PRId64
        ",\"issued_project_sample\":%" PRId64 ",\"offline\":%s,\"detail\":\"%s\"}",
        issued ? "fault_fired" : "fault_refused", fault_name(kind),
        fault_at_.load(std::memory_order_acquire), latest_position_.load(std::memory_order_acquire),
        fire_offline_.load(std::memory_order_relaxed) ? "true" : "false", escape(detail).c_str());
    emit(line);
}

gpu_audio::GpuAudioTransport::DeliverySnapshot GpuFaultContinuityProcessor::delivery() const {
    std::lock_guard lock(engine_mutex_);
    return transport_ ? transport_->delivery_snapshot()
                      : gpu_audio::GpuAudioTransport::DeliverySnapshot{};
}

void GpuFaultContinuityProcessor::set_render_loss_hook(std::function<void()> hook) {
    std::lock_guard lock(hook_mutex_);
    render_loss_hook_ = std::move(hook);
}

void GpuFaultContinuityProcessor::report_render_loss(bool performed, const std::string& detail) {
    render_loss_state_.store(performed ? 1 : 2, std::memory_order_release);
    emit(std::string("{\"event\":\"render_loss\",\"performed\":") + (performed ? "true" : "false") +
         ",\"detail\":\"" + escape(detail) + "\"}");
}

void GpuFaultContinuityProcessor::maybe_snapshot_after() {
    if (!fired_)
        return;
    const auto fired_kind = static_cast<FaultKind>(armed_kind_.load(std::memory_order_acquire));
    const bool audio_fault =
        fired_kind != FaultKind::RenderForceLoss && fired_kind != FaultKind::Control;
    if (audio_fault && !applied_reported_ && provider_faults_applied() > applied_before_) {
        // The interposer actually faulted the provider (not merely armed).
        applied_reported_ = true;
        applied_ns_ = now_ns();
        char line[256];
        std::snprintf(line, sizeof(line),
                      "{\"event\":\"provider_fault_applied\",\"point\":\"%s\","
                      "\"project_sample\":%" PRId64 ",\"delay_ms\":%u,"
                      "\"device\":\"0x%llx\",\"prepare_index\":%u,"
                      "\"host_time_ns\":%llu}",
                      provider_fault_point(), latest_position_.load(std::memory_order_acquire),
                      delay_ms_, static_cast<unsigned long long>(provider_fault_applied_device()),
                      prepare_index_ & 0xFFFFu,
                      static_cast<unsigned long long>(provider_fault_applied_time_ns()));
        emit(line);
    }
    const auto kind = static_cast<FaultKind>(armed_kind_.load(std::memory_order_acquire));
    if (!at_taken_) {
        // "at": once the editor has acted for a render loss, otherwise a
        // fixed 250 ms after injection, long enough for the worker to observe it.
        const bool render_done = render_loss_state_.load(std::memory_order_acquire) != 0;
        const std::uint64_t since = applied_reported_ ? applied_ns_ : fired_ns_;
        const bool waited = now_ns() - since > 250'000'000ull;
        if ((kind == FaultKind::RenderForceLoss && (render_done || waited)) ||
            (kind != FaultKind::RenderForceLoss && waited)) {
            at_taken_ = true;
            emit_health("at");
        }
    }
    const std::int64_t after = after_at_.load(std::memory_order_relaxed);
    if (at_taken_ && !after_taken_ && after >= 0 &&
        latest_position_.load(std::memory_order_acquire) >= after) {
        after_taken_ = true;
        emit_health("after");
    }
}

void GpuFaultContinuityProcessor::service_loop() {
    const int capture = open_nofollow(run_dir_ / "capture.bin", O_WRONLY | O_APPEND);
    const int events = open_nofollow(run_dir_ / "events.jsonl", O_WRONLY | O_APPEND);
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> lines;
    std::uint64_t reported_drops = 0;
    bool stopping = false;
    while (!stopping) {
        stopping = !running_.load(std::memory_order_acquire);
        poll_request();
        fire_pending_fault();
        maybe_snapshot_after();
        if (const auto drops = ring_.dropped(); drops != reported_drops) {
            reported_drops = drops;
            emit("{\"event\":\"capture_overflow\",\"dropped_records\":" + std::to_string(drops) +
                 "}");
        }
        bytes.clear();
        if (ring_.drain(bytes)) {
            write_all(capture, bytes.data(), bytes.size());
        }
        {
            std::lock_guard lock(events_mutex_);
            lines.swap(pending_events_);
        }
        for (const auto& line : lines) {
            write_all(events, line.data(), line.size());
            write_all(events, "\n", 1);
        }
        lines.clear();
        if (!stopping)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (capture >= 0)
        ::close(capture);
    if (events >= 0)
        ::close(events);
}

std::unique_ptr<format::Processor> create_gpu_fault_continuity() {
    return std::make_unique<GpuFaultContinuityProcessor>();
}

} // namespace pulp::examples::gpu_fault
