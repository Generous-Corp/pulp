// SPDX-License-Identifier: MIT
#include "isolated_clap_channel.hpp"
#include "isolated_clap_endpoint.hpp"
#include "plugin_slot_process_request.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>
#include <thread>

#if defined(PULP_ISOLATED_CLAP_SUPPORTED) && defined(__APPLE__) && defined(__aarch64__)
#include <fcntl.h>
#include <pulp/platform/child_process.hpp>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::host {
using namespace isolation;
using Clock = std::chrono::steady_clock;

namespace detail {
struct IsolatedClapSession {
    IsolatedClapOptions options;
    runtime::MemoryMappedFile mapping;
    Mapping* shared = nullptr;
    std::string mapping_path;
    const std::uint64_t generation;
    // One CAS publishes the first reason and sequence coherently. Status reads
    // acquire; callback/control/supervisor publishers release. Zero means healthy.
    std::atomic<std::uint64_t> fault{0};
    std::atomic<IsolationLifecycle> lifecycle{IsolationLifecycle::loaded};
    std::atomic<IsolationCleanup> cleanup{IsolationCleanup::pending};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> prepared{false};
    std::atomic<std::int64_t> cleanup_deadline_ns{0};
    int worker_pid = -1;
    std::mutex control_mutex;
    std::vector<std::uint8_t> checkpoint_bytes;
    std::string checkpoint_digest;
#if defined(PULP_ISOLATED_CLAP_SUPPORTED) && defined(__APPLE__) && defined(__aarch64__)
    platform::RetainedProcessCustody custody;
    platform::ChildProcess process;
    std::optional<platform::ChildProcessInputChannel> channel;
#endif
    explicit IsolatedClapSession(IsolatedClapOptions config, std::uint64_t id)
        : options(std::move(config)), generation(id) {}
    ~IsolatedClapSession() {
#if defined(PULP_ISOLATED_CLAP_SUPPORTED) && defined(__APPLE__) && defined(__aarch64__)
        if (!mapping_path.empty())
            ::unlink(mapping_path.c_str());
#endif
    }
    void fail(IsolationReason reason, std::uint64_t sequence = 0) noexcept {
        const auto word = (static_cast<std::uint64_t>(reason) << 56) |
                          std::min(sequence, (std::uint64_t{1} << 56) - 1);
        std::uint64_t expected = 0;
        fault.compare_exchange_strong(expected, word, std::memory_order_release,
                                      std::memory_order_relaxed);
    }
    IsolationStatus status() const noexcept {
        const auto word = fault.load(std::memory_order_acquire);
        return {static_cast<IsolationReason>(word >> 56),
                generation,
                word & ((std::uint64_t{1} << 56) - 1),
                word ? IsolationLifecycle::terminal : lifecycle.load(std::memory_order_acquire),
                cleanup.load(std::memory_order_acquire),
                worker_pid};
    }
    bool exchange(Command command, std::vector<std::uint8_t>* received = nullptr,
                  const std::vector<std::uint8_t>* sent = nullptr) {
#if defined(PULP_ISOLATED_CLAP_SUPPORTED) && defined(__APPLE__) && defined(__aarch64__)
        std::lock_guard lock(control_mutex);
        if (!channel || cleanup.load(std::memory_order_acquire) != IsolationCleanup::pending ||
            fault.load(std::memory_order_acquire) != 0)
            return false;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        ControlRecord request;
        request.command = command;
        request.generation = generation;
        request.payload_bytes = sent ? sent->size() : 0;
        const auto fd = static_cast<int>(channel->native_handle());
        ControlRecord reply;
        bool ok = send_record(fd, request, deadline);
        if (ok && sent)
            ok =
                transfer(fd, const_cast<std::uint8_t*>(sent->data()), sent->size(), true, deadline);
        ok = ok && receive_record(fd, reply, deadline) && reply.command == command &&
             reply.generation == generation && reply.payload_bytes <= kCheckpointBytes;
        if (ok && received && reply.result == 1) {
            received->resize(static_cast<std::size_t>(reply.payload_bytes));
            ok = transfer(fd, received->data(), received->size(), false, deadline);
        } else if (ok && reply.payload_bytes != 0)
            ok = false;
        if (!ok) {
            fail(Clock::now() >= deadline ? IsolationReason::lifecycle_timeout
                                          : IsolationReason::lifecycle_failed);
            return false;
        }
        return reply.result == 1;
#else
        (void)command;
        (void)received;
        (void)sent;
        return false;
#endif
    }
};
} // namespace detail

namespace {
using Session = detail::IsolatedClapSession;

// Intentionally lives to host exit: unresolved custody may never be discarded
// by static teardown. Capacity includes failed construction, before any spawn.
struct Registry {
    std::mutex mutex;
    std::array<std::shared_ptr<Session>, 2> reservations;
};
Registry& registry() {
    static auto* value = new Registry;
    return *value;
}

IsolationReason reserve(const std::shared_ptr<Session>& session) {
    auto& store = registry();
    std::lock_guard lock(store.mutex);
    for (const auto& entry : store.reservations) {
        if (entry && entry->cleanup.load(std::memory_order_acquire) != IsolationCleanup::pending)
            return IsolationReason::quarantine_active;
    }
    for (auto& entry : store.reservations) {
        if (!entry) {
            entry = session;
            return IsolationReason::none;
        }
    }
    return IsolationReason::registry_full;
}
void release_reservation(const std::shared_ptr<Session>& session) {
    auto& store = registry();
    std::lock_guard lock(store.mutex);
    for (auto& entry : store.reservations)
        if (entry == session)
            entry.reset();
}

void cleanup_session(const std::shared_ptr<Session>& session,
                     Clock::time_point deadline = Clock::now() + std::chrono::seconds(2)) {
#if defined(PULP_ISOLATED_CLAP_SUPPORTED) && defined(__APPLE__) && defined(__aarch64__)
    std::lock_guard lock(session->control_mutex);
    const auto observed = session->custody.retained_process_observation();
    if (observed.state == platform::RetainedProcessState::NotStarted) {
        session->cleanup.store(IsolationCleanup::complete, std::memory_order_release);
        release_reservation(session);
        return;
    }
    const auto result = session->custody.cleanup_retained_process_group(deadline);
    if (result.status == platform::RetainedCleanupStatus::Complete && result.child_reaped &&
        result.descendants_gone) {
        session->channel.reset();
        session->cleanup.store(IsolationCleanup::complete, std::memory_order_release);
        release_reservation(session);
    } else {
        session->cleanup.store(result.status == platform::RetainedCleanupStatus::CustodyLost
                                   ? IsolationCleanup::custody_lost
                                   : IsolationCleanup::incomplete,
                               std::memory_order_release);
    }
#else
    session->cleanup.store(IsolationCleanup::complete, std::memory_order_release);
    release_reservation(session);
#endif
}

void supervise(std::shared_ptr<Session> session) {
#if defined(PULP_ISOLATED_CLAP_SUPPORTED) && defined(__APPLE__) && defined(__aarch64__)
    while (!session->stop_requested.load(std::memory_order_acquire) &&
           session->fault.load(std::memory_order_acquire) == 0) {
        const auto observation = session->custody.retained_process_observation();
        if (observation.state != platform::RetainedProcessState::Running) {
            session->fail(IsolationReason::worker_exited);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Graceful vendor destruction is a lifecycle operation and has its own
    // five-second deadline. Fault cleanup skips vendor calls altogether.
    if (session->fault.load(std::memory_order_acquire) == 0)
        session->exchange(Command::destroy);
    session->fail(IsolationReason::stopped);
    session->prepared.store(false, std::memory_order_release);
    const auto deadline_ns = session->cleanup_deadline_ns.load(std::memory_order_acquire);
    cleanup_session(session, deadline_ns ? Clock::time_point(std::chrono::nanoseconds(deadline_ns))
                                         : Clock::now() + std::chrono::seconds(2));
#else
    (void)session;
#endif
}

class IsolatedSlot final : public detail::RequestDrivenSlot {
  public:
    explicit IsolatedSlot(std::shared_ptr<Session> session) : session_(std::move(session)) {
        info_.path = session_->options.selection.path;
        info_.unique_id = session_->options.selection.type.descriptor_id;
        info_.name = info_.unique_id;
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = info_.num_outputs = 2;
        PluginHostSettings settings;
        settings.deliver_transport = false;
        detail::RequestDrivenSlot::set_host_settings(settings);
    }
    ~IsolatedSlot() override {
        session_->stop_requested.store(true, std::memory_order_release);
    }
    const PluginInfo& info() const override {
        return info_;
    }
    const std::shared_ptr<Session>& session() const noexcept {
        return session_;
    }
    bool is_loaded() const override {
        return session_->fault.load(std::memory_order_acquire) == 0;
    }
    bool prepare(double rate, int frames) override {
        if (rate != 48000 || frames != 64 || ever_prepared_) {
            session_->fail(IsolationReason::unsupported);
            return false;
        }
        if (!session_->exchange(Command::prepare))
            return false;
        ever_prepared_ = true;
        session_->lifecycle.store(IsolationLifecycle::prepared, std::memory_order_release);
        session_->prepared.store(true, std::memory_order_release);
        return true;
    }
    void release() override {
        session_->stop_requested.store(true, std::memory_order_release);
        session_->fail(IsolationReason::stopped, endpoint_.sequence());
    }
    std::vector<HostParamInfo> parameters() const override {
        return {};
    }
    float get_parameter(std::uint32_t) const override {
        return 0;
    }
    void set_parameter(std::uint32_t, float) override {
        session_->fail(IsolationReason::unsupported);
    }
    void set_bypass(bool) override {
        session_->fail(IsolationReason::unsupported);
    }
    bool is_bypassed() const override {
        return false;
    }
    std::vector<std::uint8_t> save_state() const override {
        session_->fail(IsolationReason::unsupported);
        return {};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override {
        session_->fail(IsolationReason::unsupported);
        return false;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        session_->fail(IsolationReason::unsupported);
        return nullptr;
    }
    void destroy_editor_view() override {}
    int latency_samples() const override {
        return 64;
    }
    int tail_samples() const override {
        return 0;
    }
    void set_preferred_channel_layout(int inputs, int outputs) override {
        if (inputs != 2 || outputs != 2)
            session_->fail(IsolationReason::unsupported);
    }
    bool set_host_settings(const PluginHostSettings& settings) override {
        if (settings.deliver_transport || settings.prefer_double_precision ||
            settings.process_mode != format::ProcessMode::Realtime) {
            session_->fail(IsolationReason::unsupported);
            return false;
        }
        return detail::RequestDrivenSlot::set_host_settings(settings);
    }
    std::vector<std::uint8_t> serialize_dsp_state() const override {
        session_->fail(IsolationReason::unsupported);
        return {};
    }
    bool restore_dsp_state(const std::vector<std::uint8_t>&) override {
        session_->fail(IsolationReason::unsupported);
        return false;
    }
    bool reseed_dsp_state(std::uint64_t) override {
        session_->fail(IsolationReason::unsupported);
        return false;
    }

  private:
    void process_request(const detail::SlotProcessRequest& request) override {
        auto* input = request.audio->main_input();
        auto* output = request.audio->main_output();
        auto* shared = session_->shared;
        // Short messages clear without releasing heap storage. Variable-length
        // output sidecars must arrive empty: disposing caller-owned payloads
        // here could allocate or free on the callback. Nonempty sidecars refuse.
        request.midi_out->clear();
        auto silence = [&] {
            for (std::size_t b = 0; b < request.audio->outputs.size(); ++b) {
                auto& buffer = request.audio->outputs[b].buffer;
                for (std::size_t c = 0; c < buffer.num_channels(); ++c)
                    std::fill_n(buffer.channel_ptr(c), buffer.num_samples(), 0.0f);
            }
        };
        if (session_->fault.load(std::memory_order_acquire) != 0) {
            silence();
            return;
        }
        if (!session_->prepared.load(std::memory_order_acquire) || !shared ||
            request.num_samples != 64 || !input || !output || input->num_channels() != 2 ||
            output->num_channels() != 2 || input->num_samples() < 64 ||
            output->num_samples() < 64 || request.audio->inputs.size() != 1 ||
            request.audio->outputs.size() != 1 || request.transport || !request.midi_in->empty() ||
            !request.midi_in->sysex().empty() ||
            (request.midi_in->ump() && !request.midi_in->ump()->empty()) ||
            !request.midi_out->sysex().empty() ||
            (request.midi_out->ump() && !request.midi_out->ump()->empty()) ||
            !request.param_events->empty()) {
            silence();
            session_->fail(IsolationReason::unsupported, endpoint_.sequence());
            return;
        }
        const auto reason =
            endpoint_.render(*shared, session_->generation, output->channel_ptr(0),
                             output->channel_ptr(1), input->channel_ptr(0), input->channel_ptr(1));
        if (reason != IsolationReason::none)
            session_->fail(reason, endpoint_.sequence());
        // Ordinary ProcessStatus deliberately stays ok: terminal silence is a
        // completed slot block, so healthy graph siblings must keep executing.
    }
    std::shared_ptr<Session> session_;
    PluginInfo info_;
    CallbackEndpoint endpoint_;
    bool ever_prepared_ = false;
};

#if defined(PULP_ISOLATED_CLAP_SUPPORTED) && defined(__APPLE__) && defined(__aarch64__)
bool launch(const std::shared_ptr<Session>& session) {
    std::array<char, 256> path{};
    const auto temporary = (std::filesystem::temp_directory_path() / "pulp-clap-XXXXXXXX").string();
    if (temporary.size() >= path.size())
        return false;
    std::copy(temporary.begin(), temporary.end(), path.begin());
    const auto fd = ::mkstemp(path.data());
    if (fd < 0)
        return false;
    session->mapping_path = path.data();
    const bool sized = ::fchmod(fd, 0600) == 0 && ::ftruncate(fd, kMappingBytes) == 0;
    ::close(fd);
    if (!sized ||
        !session->mapping.open_no_follow(path.data(), runtime::MapMode::ReadWrite, kMappingBytes) ||
        session->mapping.size() != kMappingBytes)
        return false;
    session->shared = new (session->mapping.mutable_data()) Mapping{};
    session->shared->generation = session->generation;
    const auto identity = session->mapping.opened_file_identity();
    if (!identity.valid)
        return false;
    platform::ProcessOptions options;
    options.capture_stdout = options.capture_stderr = false;
    options.standard_input_timeout_ms = 5000;
    return session->process.start_retained_with_standard_input_channel(
        session->custody, session->options.worker_path,
        {session->mapping_path, session->options.selection.path,
         session->options.selection.type.descriptor_id, "MacClapStereo64V1"},
        [session, identity](int pid, platform::ChildProcessInputChannel channel,
                            Deadline deadline) {
            session->worker_pid = pid;
            const auto fd = static_cast<int>(channel.native_handle());
            int no_sigpipe = 1;
            if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)))
                return false;
            ControlRecord hello;
            hello.generation = session->generation;
            hello.payload_bytes = kMappingBytes;
            hello.volume = identity.volume;
            hello.file = identity.file;
            hello.file_generation = identity.generation;
            std::copy_n(session->options.plugin_sha256.data(), 64, hello.plugin_sha256.data());
            std::copy_n(session->options.worker_sha256.data(), 64, hello.worker_sha256.data());
            constexpr std::string_view build = PULP_CLAP_WORKER_BUILD;
            std::copy(build.begin(), build.end(), hello.build.begin());
            ControlRecord reply;
            if (!send_record(fd, hello, deadline) || !receive_record(fd, reply, deadline) ||
                reply.command != Command::hello || reply.result != 1 ||
                reply.generation != hello.generation ||
                reply.process_id != static_cast<std::uint64_t>(pid) ||
                reply.plugin_sha256 != hello.plugin_sha256 ||
                reply.worker_sha256 != hello.worker_sha256 || reply.build != hello.build ||
                reply.volume != hello.volume || reply.file != hello.file ||
                reply.file_generation != hello.file_generation ||
                reply.payload_bytes != kMappingBytes)
                return false;
            if (::unlink(session->mapping_path.c_str()) != 0)
                return false;
            session->mapping_path.clear();
            // The load phase receives its own five-second control deadline.
            deadline = Clock::now() + std::chrono::seconds(5);
            if (!receive_record(fd, reply, deadline) || reply.result != 2 ||
                reply.generation != hello.generation || reply.build != hello.build)
                return false;
            session->channel.emplace(std::move(channel));
            return true;
        },
        options);
}
#endif
} // namespace

struct IsolatedClapAccess {
    static std::shared_ptr<IsolatedClapControl> control(std::shared_ptr<Session> session) {
        return std::shared_ptr<IsolatedClapControl>(new IsolatedClapControl(std::move(session)));
    }
    static const std::shared_ptr<Session>& session(const IsolatedClapControl& control) {
        return control.session_;
    }
};
IsolatedClapControl::IsolatedClapControl(std::shared_ptr<Session> session)
    : session_(std::move(session)) {}
IsolatedClapControl::~IsolatedClapControl() {
    session_->stop_requested.store(true, std::memory_order_release);
}
IsolationStatus IsolatedClapControl::status() const noexcept {
    return session_->status();
}
bool IsolatedClapControl::pause() {
    if (!session_->exchange(Command::stop_processing))
        return false;
    session_->prepared.store(false, std::memory_order_release);
    session_->lifecycle.store(IsolationLifecycle::stopped, std::memory_order_release);
    return true;
}
bool IsolatedClapControl::checkpoint() {
    if (session_->prepared.load(std::memory_order_acquire))
        return false;
    std::vector<std::uint8_t> bytes;
    if (!session_->exchange(Command::checkpoint, &bytes))
        return false;
    const auto digest = runtime::sha256_hex(bytes.data(), bytes.size());
    std::lock_guard lock(session_->control_mutex);
    session_->checkpoint_bytes = std::move(bytes);
    session_->checkpoint_digest = digest;
    return true;
}
IsolationCleanup IsolatedClapControl::stop() {
    session_->stop_requested.store(true, std::memory_order_release);
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (session_->cleanup.load(std::memory_order_acquire) == IsolationCleanup::pending &&
           Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return session_->cleanup.load(std::memory_order_acquire);
}
IsolationCleanup IsolatedClapControl::cleanup(Clock::time_point deadline) {
    session_->cleanup_deadline_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count(),
        std::memory_order_release);
    session_->fail(IsolationReason::stopped);
    session_->stop_requested.store(true, std::memory_order_release);
    auto state = session_->cleanup.load(std::memory_order_acquire);
    if (state == IsolationCleanup::incomplete) {
        cleanup_session(session_, deadline);
        return session_->cleanup.load(std::memory_order_acquire);
    }
    while (state == IsolationCleanup::pending && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        state = session_->cleanup.load(std::memory_order_acquire);
    }
    return state;
}
std::vector<std::uint8_t> IsolatedClapControl::accepted_checkpoint() const {
    std::lock_guard lock(session_->control_mutex);
    return session_->checkpoint_bytes;
}

IsolatedClapResult load_isolated_clap(const IsolatedClapOptions& requested) {
#if defined(PULP_ISOLATED_CLAP_SUPPORTED) && defined(__APPLE__) && defined(__aarch64__)
    if (validate_plugin_type_identity(requested.selection.type) ||
        requested.plugin_sha256.size() != 64 || requested.worker_sha256.size() != 64)
        return {{}, {}, IsolationReason::identity_mismatch};
    std::error_code ec;
    auto options = requested;
    options.selection.path = std::filesystem::canonical(options.selection.path, ec).string();
    if (ec)
        return {{}, {}, IsolationReason::identity_mismatch};
    options.worker_path = std::filesystem::canonical(options.worker_path, ec).string();
    struct stat worker_stat{};
    if (ec || ::stat(options.worker_path.c_str(), &worker_stat) || !S_ISREG(worker_stat.st_mode) ||
        (worker_stat.st_mode & (S_IWGRP | S_IWOTH)) || ::access(options.worker_path.c_str(), X_OK))
        return {{}, {}, IsolationReason::worker_missing};
    if (runtime::sha256_file_hex(clap_hash_path(options.selection.path), std::uint64_t{1} << 32) !=
            options.plugin_sha256 ||
        runtime::sha256_file_hex(options.worker_path, std::uint64_t{1} << 32) !=
            options.worker_sha256)
        return {{}, {}, IsolationReason::identity_mismatch};
    auto random = runtime::secure_random_bytes(sizeof(std::uint64_t));
    if (!random)
        return {{}, {}, IsolationReason::launch_failed};
    std::uint64_t generation = 0;
    std::memcpy(&generation, random->data(), sizeof(generation));
    if (generation == 0)
        return {{}, {}, IsolationReason::launch_failed};
    auto session = std::make_shared<Session>(std::move(options), generation);
    const auto admission = reserve(session);
    if (admission != IsolationReason::none)
        return {{}, {}, admission};
    try {
        if (launch(session)) {
            auto control = IsolatedClapAccess::control(session);
            auto slot = std::make_unique<IsolatedSlot>(session);
            std::thread(supervise, session).detach();
            return {std::move(slot), std::move(control), IsolationReason::none};
        }
    } catch (...) {
        // Reservation owns custody even if allocation or thread creation fails.
    }
    session->fail(IsolationReason::launch_failed);
    cleanup_session(session);
    return {{}, {}, IsolationReason::launch_failed};
#else
    (void)requested;
    return {{}, {}, IsolationReason::unsupported};
#endif
}
IsolatedClapResult recover_isolated_clap(const IsolatedClapOptions& options,
                                         const IsolatedClapControl& old) {
    const auto& previous = IsolatedClapAccess::session(old);
    if (previous->cleanup.load(std::memory_order_acquire) != IsolationCleanup::complete)
        return {{}, {}, IsolationReason::quarantine_active};
    std::vector<std::uint8_t> bytes;
    {
        std::lock_guard lock(previous->control_mutex);
        if (options.selection.type != previous->options.selection.type ||
            options.plugin_sha256 != previous->options.plugin_sha256 ||
            previous->checkpoint_digest.empty() ||
            runtime::sha256_hex(previous->checkpoint_bytes.data(),
                                previous->checkpoint_bytes.size()) != previous->checkpoint_digest)
            return {{}, {}, IsolationReason::checkpoint_invalid};
        bytes = previous->checkpoint_bytes;
    }
    auto recovered = load_isolated_clap(options);
    if (recovered && (recovered.control->status().generation == previous->generation ||
                      !IsolatedClapAccess::session(*recovered.control)
                           ->exchange(Command::restore, nullptr, &bytes))) {
        // An incomplete cleanup keeps its finite reservation, so hand the control
        // back rather than strand it where nothing can finish the cleanup.
        if (recovered.control->stop() == IsolationCleanup::complete)
            return {{}, {}, IsolationReason::checkpoint_invalid};
        return {{}, std::move(recovered.control), IsolationReason::checkpoint_invalid};
    }
    if (recovered) {
        // The restored state is this generation's starting checkpoint, so a later
        // fault can recover again without pausing a node the graph is rendering.
        const auto& next = IsolatedClapAccess::session(*recovered.control);
        std::lock_guard lock(next->control_mutex);
        next->checkpoint_digest = runtime::sha256_hex(bytes.data(), bytes.size());
        next->checkpoint_bytes = std::move(bytes);
    }
    return recovered;
}
IsolatedNodeRecoveryResult recover_isolated_graph_node(SignalGraph& graph, NodeId node,
                                                       const IsolatedClapOptions& options,
                                                       IsolatedClapControl& old) {
    using Outcome = IsolatedNodeRecovery;
    const auto refuse = [](Outcome outcome, IsolationReason error = IsolationReason::none) {
        return IsolatedNodeRecoveryResult{outcome, error, nullptr};
    };
    // Share ownership of the published proxy (read under the graph lock), so it
    // cannot be freed or its address reused while it is identified here and
    // handed back for publication, even if another control thread releases or
    // re-publishes the graph meanwhile.
    const auto owner = graph.published_plugin_slot(node);
    const auto* published = dynamic_cast<const IsolatedSlot*>(owner.get());
    if (!published)
        return refuse(Outcome::not_isolated_node);
    const auto& previous = IsolatedClapAccess::session(old);
    if (published->session() != previous)
        return refuse(Outcome::control_mismatch);
    // Healthy workers are never preempted: cleanup() below would stop them.
    if (previous->fault.load(std::memory_order_acquire) == 0)
        return refuse(Outcome::not_terminal);
    {
        std::lock_guard lock(previous->control_mutex);
        if (options.selection.type != previous->options.selection.type ||
            options.plugin_sha256 != previous->options.plugin_sha256)
            return refuse(Outcome::identity_mismatch);
        if (previous->checkpoint_digest.empty())
            return refuse(Outcome::no_checkpoint);
    }
    // The supervisor normally finishes custody cleanup right after the fault;
    // this only bounds the wait and retries an incomplete earlier attempt.
    if (old.cleanup(Clock::now() + std::chrono::seconds(2)) != IsolationCleanup::complete)
        return refuse(Outcome::cleanup_incomplete);
    auto recovered = recover_isolated_clap(options, old);
    if (!recovered)
        return {Outcome::worker_failed, recovered.error, std::move(recovered.control)};
    auto control = std::move(recovered.control);
    if (graph.publish_isolated_recovery(node, *published, std::move(recovered.slot)) !=
        SignalGraph::SwapResult::Swapped) {
        // A complete stop releases the reservation; otherwise the caller needs the
        // control to finish cleanup() or later loads stay quarantine_active.
        if (control->stop() == IsolationCleanup::complete)
            control.reset();
        return {Outcome::publication_refused, IsolationReason::none, std::move(control)};
    }
    return {Outcome::recovered, IsolationReason::none, std::move(control)};
}
bool is_isolated_plugin_slot(const PluginSlot& slot) noexcept {
    return dynamic_cast<const IsolatedSlot*>(&slot) != nullptr;
}
bool same_isolated_plugin_binary(const PluginSlot& a, const PluginSlot& b) noexcept {
    const auto* left = dynamic_cast<const IsolatedSlot*>(&a);
    const auto* right = dynamic_cast<const IsolatedSlot*>(&b);
    if (!left || !right)
        return false;
    const auto& x = left->session()->options;
    const auto& y = right->session()->options;
    return x.selection.type == y.selection.type && x.plugin_sha256 == y.plugin_sha256;
}
} // namespace pulp::host
