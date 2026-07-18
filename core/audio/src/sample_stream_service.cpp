#include <pulp/audio/sample_stream_service.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace pulp::audio {

struct SampleStreamCacheService::SlotRecord {
    std::uint64_t publish_sequence = 0;
    std::uint64_t fill_serial = 0;
    std::uint64_t fill_registration_epoch = 0;
    std::uint64_t fill_start_frame = 0;
    std::uint64_t fill_frame_count = 0;
};

struct SampleStreamCacheService::SourceGenerationRecord {
    std::uint64_t source_id = 0;
    std::uint64_t highest_generation = 0;
};

struct SampleStreamCacheService::Source {
    SampleStreamCacheSourceConfig config{};
    SampleMemoryLease page_lease;
    SampleStreamWindow window;
    FrameReader reader;
    std::vector<float*> channel_ptrs;
    std::vector<SlotRecord> slots;
    std::uint64_t next_publish_sequence = 1;
    std::uint64_t retire_after_audio_generation = 0;
    std::uint64_t registration_epoch = 0;
    std::uint32_t async_reservations = 0;
};

enum class SampleStreamCacheService::PageReservationStatus : std::uint8_t {
    Reserved,
    RetiredVictim,
    WaitingForGeneration,
    Unavailable,
};

struct SampleStreamCacheService::PageReservation {
    PageReservationStatus status = PageReservationStatus::Unavailable;
    std::uint32_t page_index = 0;
    bool reused_retired = false;
};

SampleStreamCacheService::SampleStreamCacheService() = default;
SampleStreamCacheService::~SampleStreamCacheService() = default;

bool SampleStreamCacheService::prepare(
    const SampleStreamCacheServiceConfig& config) {
    release();
    if (config.scheduler_capacity == 0 || config.source_identity_capacity == 0 ||
        config.maximum_async_reservations_per_source == 0 ||
        (!config.memory_governor && config.page_memory_budget_bytes == 0)) {
        return false;
    }
    if (!scheduler_.prepare(config.scheduler_capacity)) return false;
    try {
        source_generations_.reserve(config.source_identity_capacity);
    } catch (...) {
        scheduler_.reset();
        return false;
    }
    if (config.memory_governor) {
        memory_governor_ = config.memory_governor;
    } else {
        if (!internal_memory_governor_.prepare(config.page_memory_budget_bytes)) {
            scheduler_.reset();
            return false;
        }
        memory_governor_ = internal_memory_governor_.handle();
    }
    maximum_async_reservations_per_source_ =
        config.maximum_async_reservations_per_source;
    source_identity_capacity_ = config.source_identity_capacity;
    stats_.source_identity_capacity = source_identity_capacity_;
    prepared_ = true;
    return true;
}

void SampleStreamCacheService::release() noexcept {
    sources_.clear();
    source_generations_.clear();
    scheduler_.reset();
    memory_governor_ = {};
    (void)internal_memory_governor_.release();
    active_audio_generation_ = 0;
    completed_audio_generation_ = 0;
    published_page_retirement_epoch_.store(0, std::memory_order_relaxed);
    completed_page_retirement_epoch_.store(0, std::memory_order_relaxed);
    next_page_retirement_epoch_ = 1;
    maximum_async_reservations_per_source_ = 1;
    source_identity_capacity_ = 0;
    prepared_ = false;
    stats_ = {};
}

bool SampleStreamCacheService::prepared() const noexcept {
    return prepared_;
}

std::uint64_t SampleStreamCacheService::begin_audio_page_read() const noexcept {
    return published_page_retirement_epoch_.load(std::memory_order_acquire);
}

void SampleStreamCacheService::complete_audio_page_read(
    std::uint64_t epoch) noexcept {
    completed_page_retirement_epoch_.store(epoch, std::memory_order_release);
}

bool SampleStreamCacheService::update_audio_generations(
    std::uint64_t active_audio_generation,
    std::uint64_t completed_audio_generation) noexcept {
    if (active_audio_generation == 0 ||
        completed_audio_generation > active_audio_generation ||
        active_audio_generation < active_audio_generation_ ||
        completed_audio_generation < completed_audio_generation_) {
        ++stats_.invalid_audio_generation_updates;
        return false;
    }
    active_audio_generation_ = active_audio_generation;
    completed_audio_generation_ = completed_audio_generation;
    return true;
}

SampleStreamSourceAddResult SampleStreamCacheService::add_source(
    const SampleStreamCacheSourceConfig& config,
    FrameReader reader) {
    if (!prepared_ || !valid_source_config(config) || !reader) {
        return {SampleStreamSourceAddStatus::InvalidConfig, {}};
    }
    if (find_source_id(config.token.source_id) != nullptr) {
        return {SampleStreamSourceAddStatus::DuplicateSource, {}};
    }
    const auto generation_record = std::find_if(
        source_generations_.begin(),
        source_generations_.end(),
        [&config](const SourceGenerationRecord& record) noexcept {
            return record.source_id == config.token.source_id;
        });
    if (generation_record != source_generations_.end() &&
        config.token.source_generation <= generation_record->highest_generation) {
        return {SampleStreamSourceAddStatus::StaleGeneration, {}};
    }
    if (generation_record == source_generations_.end() &&
        source_generations_.size() >= source_identity_capacity_) {
        ++stats_.source_identity_capacity_rejections;
        return {SampleStreamSourceAddStatus::SourceIdentityCapacityExceeded, {}};
    }

    const auto bytes = page_storage_bytes(config);
    if (!bytes || !memory_governor_) {
        return {SampleStreamSourceAddStatus::BudgetExceeded, {}};
    }
    auto page_reservation = memory_governor_.reserve(
        SampleMemoryCategory::Page, *bytes);
    if (!page_reservation.acquired()) {
        return {SampleStreamSourceAddStatus::BudgetExceeded, {}};
    }

    try {
        auto source = std::make_unique<Source>();
        source->config = config;
        source->page_lease = std::move(page_reservation.lease);
        if (next_registration_epoch_ == 0) {
            return {SampleStreamSourceAddStatus::AllocationFailed, {}};
        }
        source->registration_epoch = next_registration_epoch_++;
        source->reader = std::move(reader);
        source->channel_ptrs.resize(config.channels);
        source->slots.resize(config.cache_page_count);
        if (!source->window.prepare({
                .channels = config.channels,
                .page_count = config.cache_page_count,
                .page_frames = config.page_frames,
            })) {
            return {SampleStreamSourceAddStatus::AllocationFailed, {}};
        }

        const auto view = make_view(*source);
        sources_.push_back(std::move(source));
        if (generation_record == source_generations_.end()) {
            try {
                source_generations_.push_back({
                    .source_id = config.token.source_id,
                    .highest_generation = config.token.source_generation,
                });
                stats_.source_identity_count = source_generations_.size();
            } catch (...) {
                sources_.pop_back();
                return {SampleStreamSourceAddStatus::AllocationFailed, {}};
            }
        } else {
            generation_record->highest_generation = config.token.source_generation;
        }
        stats_.reserved_page_bytes += *bytes;
        stats_.source_count = sources_.size();
        return {SampleStreamSourceAddStatus::Added, view};
    } catch (...) {
        return {SampleStreamSourceAddStatus::AllocationFailed, {}};
    }
}

SampleStreamSourceRetireStatus
SampleStreamCacheService::retire_source_after_asset_unpublish(
    SampleStreamSourceToken token) noexcept {
    if (active_audio_generation_ == 0) {
        return SampleStreamSourceRetireStatus::InvalidGeneration;
    }
    auto* source = find_source(token);
    if (source == nullptr) return SampleStreamSourceRetireStatus::StaleSource;
    if (source->retire_after_audio_generation != 0) {
        return SampleStreamSourceRetireStatus::AlreadyScheduled;
    }
    source->retire_after_audio_generation = active_audio_generation_;
    ++stats_.sources_retire_scheduled;
    return SampleStreamSourceRetireStatus::Scheduled;
}

bool SampleStreamCacheService::discard_unpublished_source(
    SampleStreamSourceToken token) noexcept {
    const auto found = std::find_if(
        sources_.begin(),
        sources_.end(),
        [token](const auto& source) noexcept {
            return source->config.token.source_id == token.source_id &&
                   source->config.token.source_generation ==
                       token.source_generation;
        });
    if (found == sources_.end() || (*found)->async_reservations != 0 ||
        (*found)->retire_after_audio_generation != 0) {
        return false;
    }
    scheduler_.cancel_source_generation(token.source_id,
                                        token.source_generation);
    const auto bytes = page_storage_bytes((*found)->config).value_or(0);
    stats_.reserved_page_bytes -= std::min(stats_.reserved_page_bytes, bytes);
    sources_.erase(found);
    stats_.source_count = sources_.size();
    return true;
}

bool SampleStreamCacheService::retirement_watermark_reached(
    SampleStreamSourceToken token) const noexcept {
    const auto* source = find_source(token);
    return source != nullptr && source->retire_after_audio_generation != 0 &&
           completed_audio_generation_ >= source->retire_after_audio_generation;
}

bool SampleStreamCacheService::contains_source(
    SampleStreamSourceToken token) const noexcept {
    return find_source(token) != nullptr;
}

std::size_t SampleStreamCacheService::collect_retired_sources() noexcept {
    std::size_t collected = 0;
    for (auto source = sources_.begin(); source != sources_.end();) {
        const auto retire_after = (*source)->retire_after_audio_generation;
        if (retire_after == 0 || completed_audio_generation_ < retire_after) {
            ++source;
            continue;
        }
        if ((*source)->async_reservations != 0) {
            ++source;
            continue;
        }
        scheduler_.cancel_source_generation(
            (*source)->config.token.source_id,
            (*source)->config.token.source_generation);
        const auto bytes = page_storage_bytes((*source)->config).value_or(0);
        stats_.reserved_page_bytes -= std::min(stats_.reserved_page_bytes, bytes);
        source = sources_.erase(source);
        ++collected;
    }
    stats_.source_count = sources_.size();
    stats_.sources_collected += collected;
    return collected;
}

SampleStreamScheduleStatus SampleStreamCacheService::request_page(
    const SampleStreamPageDemand& demand) noexcept {
    auto* source = find_source(demand.source);
    if (source == nullptr) {
        ++stats_.stale_requests;
        return SampleStreamScheduleStatus::Invalid;
    }
    const auto request = canonical_request(*source, demand);
    if (!request) return SampleStreamScheduleStatus::Invalid;
    return scheduler_.submit_or_refresh(*request);
}

std::size_t SampleStreamCacheService::cancel_requester(
    SampleStreamRequesterToken requester) noexcept {
    if (requester.requester_id == 0 || requester.requester_generation == 0) {
        return 0;
    }
    return scheduler_.cancel_requester(requester.requester_id,
                                       requester.requester_generation);
}

std::size_t SampleStreamCacheService::cancel_pending_source_demands(
    SampleStreamSourceToken source) noexcept {
    if (source.source_id == 0 || source.source_generation == 0) return 0;
    return scheduler_.cancel_source_generation(source.source_id,
                                               source.source_generation);
}

SampleStreamAsyncReserveResult
SampleStreamCacheService::reserve_async_page() noexcept {
    const auto request = scheduler_.most_urgent_if(
        [this](const SampleStreamPageRequest& candidate) noexcept {
            const auto* source = find_source(
                {candidate.source_id, candidate.source_generation});
            return source == nullptr ||
                   (source->async_reservations <
                        maximum_async_reservations_per_source_ &&
                    !has_matching_filling_page(*source, candidate));
        });
    if (!request) {
        return {scheduler_.stats().pending == 0
                    ? SampleStreamAsyncReserveStatus::Idle
                    : SampleStreamAsyncReserveStatus::SourceInFlight,
                {}};
    }

    auto* source = find_source({request->source_id, request->source_generation});
    if (source == nullptr) {
        scheduler_.complete_page(*request);
        ++stats_.stale_requests;
        return {SampleStreamAsyncReserveStatus::StaleSource, {}};
    }
    if (source->async_reservations >= maximum_async_reservations_per_source_) {
        return {SampleStreamAsyncReserveStatus::SourceInFlight, {}};
    }
    const auto ready = source->window.ready_page_for_frame(
        request->source_generation, request->start_frame);
    if (ready.valid) {
        source->slots[ready.page_index].publish_sequence =
            source->next_publish_sequence++;
        scheduler_.complete_page(*request);
        ++stats_.already_ready;
        return {SampleStreamAsyncReserveStatus::AlreadyReady, {}};
    }

    const auto serial = take_reservation_serial();
    if (serial == 0) {
        return {SampleStreamAsyncReserveStatus::SerialExhausted, {}};
    }
    const auto page = reserve_page(*source, serial);
    if (page.status != PageReservationStatus::Reserved) {
        if (page.status == PageReservationStatus::RetiredVictim) {
            ++stats_.pages_retired;
            return {SampleStreamAsyncReserveStatus::PageRetired, {}};
        }
        if (page.status == PageReservationStatus::WaitingForGeneration) {
            ++stats_.retire_waits;
            return {SampleStreamAsyncReserveStatus::WaitingForAudioGeneration, {}};
        }
        ++stats_.no_page_available;
        return {SampleStreamAsyncReserveStatus::NoPageAvailable, {}};
    }
    if (page.reused_retired) ++stats_.retired_pages_reused;

    ++source->async_reservations;
    stats_.async_reservations_high_water = std::max<std::uint64_t>(
        stats_.async_reservations_high_water, source->async_reservations);
    auto& slot = source->slots[page.page_index];
    slot.fill_registration_epoch = source->registration_epoch;
    slot.fill_start_frame = request->start_frame;
    slot.fill_frame_count = request->frame_count;
    const auto view = make_view(*source);
    return {
        SampleStreamAsyncReserveStatus::Reserved,
        {
            .source = source->config.token,
            .registration = view.registration,
            .registration_epoch = source->registration_epoch,
            .reservation_serial = serial,
            .start_frame = request->start_frame,
            .frame_count = request->frame_count,
            .page_index = page.page_index,
            .final_page = request->frame_count ==
                source->config.total_frames - request->start_frame,
            .request = *request,
        },
    };
}

bool SampleStreamCacheService::commit_async_dispatch(
    const SampleStreamAsyncReservation& reservation) noexcept {
    auto* source = matching_async_source(reservation);
    if (source == nullptr || !matching_filling_slot(*source, reservation)) {
        return false;
    }
    scheduler_.complete_page(reservation.request);
    ++stats_.decode_calls;
    return true;
}

bool SampleStreamCacheService::async_reservation_has_interest(
    const SampleStreamAsyncReservation& reservation) const noexcept {
    return scheduler_.has_page_interest(reservation.request);
}

SampleStreamAsyncCompletionStatus
SampleStreamCacheService::cancel_async_reservation(
    const SampleStreamAsyncReservation& reservation) noexcept {
    auto* source = find_source(reservation.source);
    if (source == nullptr) return SampleStreamAsyncCompletionStatus::StaleSource;
    if (!registration_matches(*source, reservation)) {
        return SampleStreamAsyncCompletionStatus::StaleRegistration;
    }
    if (!matching_filling_slot(*source, reservation)) {
        return SampleStreamAsyncCompletionStatus::StaleReservation;
    }
    if (!cancel_fill(*source,
                     reservation.page_index,
                     reservation.reservation_serial)) {
        return SampleStreamAsyncCompletionStatus::StaleReservation;
    }
    finish_async_reservation(*source);
    return SampleStreamAsyncCompletionStatus::Canceled;
}

SampleStreamAsyncCompletionStatus
SampleStreamCacheService::publish_async_reservation(
    const SampleStreamAsyncReservation& reservation,
    BufferView<const float> decoded) noexcept {
    auto* source = find_source(reservation.source);
    if (source == nullptr) return SampleStreamAsyncCompletionStatus::StaleSource;
    if (!registration_matches(*source, reservation)) {
        return SampleStreamAsyncCompletionStatus::StaleRegistration;
    }
    if (!matching_filling_slot(*source, reservation)) {
        return SampleStreamAsyncCompletionStatus::StaleReservation;
    }
    if (decoded.num_channels() != source->config.channels ||
        decoded.num_samples() < reservation.frame_count ||
        !source->window.copy_to_filling_page(reservation.page_index,
                                             decoded,
                                             reservation.frame_count)) {
        cancel_fill(*source,
                    reservation.page_index,
                    reservation.reservation_serial);
        finish_async_reservation(*source);
        ++stats_.read_failures;
        return SampleStreamAsyncCompletionStatus::CopyFailed;
    }
    if (!source->window.publish_page(
            reservation.page_index,
            {
                .stream_generation = reservation.source.source_generation,
                .start_frame = reservation.start_frame,
                .valid_frames = reservation.frame_count,
                .final_page = reservation.final_page,
            })) {
        cancel_fill(*source,
                    reservation.page_index,
                    reservation.reservation_serial);
        finish_async_reservation(*source);
        ++stats_.publish_failures;
        return SampleStreamAsyncCompletionStatus::PublishFailed;
    }
    auto& slot = source->slots[reservation.page_index];
    slot.fill_serial = 0;
    slot.publish_sequence = source->next_publish_sequence++;
    finish_async_reservation(*source);
    ++stats_.pages_published;
    return SampleStreamAsyncCompletionStatus::Published;
}

SampleStreamServiceStatus SampleStreamCacheService::service_once() noexcept {
    const auto request = scheduler_.most_urgent();
    if (!request) return SampleStreamServiceStatus::Idle;

    auto* source = find_source({request->source_id, request->source_generation});
    if (source == nullptr) {
        scheduler_.complete_page(*request);
        ++stats_.stale_requests;
        return SampleStreamServiceStatus::StaleSource;
    }

    const auto start_frame = request->start_frame;
    const auto ready = source->window.ready_page_for_frame(
        request->source_generation, start_frame);
    if (ready.valid) {
        source->slots[ready.page_index].publish_sequence =
            source->next_publish_sequence++;
        scheduler_.complete_page(*request);
        ++stats_.already_ready;
        return SampleStreamServiceStatus::AlreadyReady;
    }

    const auto reservation_serial = take_reservation_serial();
    if (reservation_serial == 0) {
        return SampleStreamServiceStatus::NoPageAvailable;
    }
    const auto reservation = reserve_page(*source, reservation_serial);
    if (reservation.status != PageReservationStatus::Reserved) {
        if (reservation.status == PageReservationStatus::RetiredVictim) {
            ++stats_.pages_retired;
            return SampleStreamServiceStatus::PageRetired;
        }
        if (reservation.status == PageReservationStatus::WaitingForGeneration) {
            ++stats_.retire_waits;
            return SampleStreamServiceStatus::WaitingForAudioGeneration;
        }
        ++stats_.no_page_available;
        return SampleStreamServiceStatus::NoPageAvailable;
    }
    const auto page_slot = reservation.page_index;
    if (reservation.reused_retired) ++stats_.retired_pages_reused;

    scheduler_.complete_page(*request);

    for (std::uint32_t channel = 0; channel < source->config.channels; ++channel) {
        source->channel_ptrs[channel] =
            source->window.writable_channel_data(page_slot, channel);
        if (source->channel_ptrs[channel] == nullptr) {
            cancel_fill(*source, page_slot, reservation_serial);
            ++stats_.read_failures;
            return SampleStreamServiceStatus::ReadFailed;
        }
    }

    BufferView<float> destination(source->channel_ptrs.data(),
                                  source->config.channels,
                                  static_cast<std::size_t>(request->frame_count));
    std::uint64_t decoded = 0;
    ++stats_.decode_calls;
    try {
        decoded = source->reader(start_frame, destination, request->frame_count);
    } catch (...) {
        decoded = 0;
    }

    if (decoded != request->frame_count) {
        cancel_fill(*source, page_slot, reservation_serial);
        ++stats_.read_failures;
        return SampleStreamServiceStatus::ReadFailed;
    }

    const bool final_page =
        request->frame_count == source->config.total_frames - start_frame;
    if (!source->window.publish_page(
            page_slot,
            {
                .stream_generation = request->source_generation,
                .start_frame = start_frame,
                .valid_frames = request->frame_count,
                .final_page = final_page,
            })) {
        cancel_fill(*source, page_slot, reservation_serial);
        ++stats_.publish_failures;
        return SampleStreamServiceStatus::PublishFailed;
    }

    source->slots[page_slot].publish_sequence = source->next_publish_sequence++;
    source->slots[page_slot].fill_serial = 0;
    ++stats_.pages_published;
    return SampleStreamServiceStatus::Published;
}

SampleStreamSchedulerStats SampleStreamCacheService::scheduler_stats() const noexcept {
    return scheduler_.stats();
}

SampleStreamCacheServiceStats SampleStreamCacheService::stats() const noexcept {
    auto snapshot = stats_;
    if (memory_governor_) snapshot.memory = memory_governor_.stats();
    return snapshot;
}

bool SampleStreamCacheService::valid_source_config(
    const SampleStreamCacheSourceConfig& config) noexcept {
    return config.token.source_id != 0 && config.token.source_generation != 0 &&
           config.channels != 0 && config.total_frames != 0 &&
           config.page_frames != 0 && config.cache_page_count != 0 &&
           config.page_frames <= std::numeric_limits<std::size_t>::max();
}

std::optional<std::uint64_t> SampleStreamCacheService::page_storage_bytes(
    const SampleStreamCacheSourceConfig& config) noexcept {
    return checked_sample_storage_bytes(config.channels,
                                        config.page_frames,
                                        config.cache_page_count);
}

SampleStreamCacheSourceView SampleStreamCacheService::make_view(
    Source& source) const noexcept {
    return {
        .token = source.config.token,
        .window = &source.window,
        .total_frames = source.config.total_frames,
        .page_frames = source.config.page_frames,
        .registration_epoch = source.registration_epoch,
        .registration = SampleStreamSourceRegistrationProof(
            source.config.token,
            &source.window,
            source.config.total_frames,
            source.config.page_frames,
            source.registration_epoch),
        .memory_governor_epoch = memory_governor_.epoch(),
    };
}

SampleStreamCacheService::Source* SampleStreamCacheService::find_source_id(
    std::uint64_t source_id) noexcept {
    const auto found = std::find_if(
        sources_.begin(),
        sources_.end(),
        [source_id](const auto& source) noexcept {
            return source->config.token.source_id == source_id;
        });
    return found == sources_.end() ? nullptr : found->get();
}

const SampleStreamCacheService::Source*
SampleStreamCacheService::find_source_id(std::uint64_t source_id) const noexcept {
    const auto found = std::find_if(
        sources_.begin(),
        sources_.end(),
        [source_id](const auto& source) noexcept {
            return source->config.token.source_id == source_id;
        });
    return found == sources_.end() ? nullptr : found->get();
}

SampleStreamCacheService::Source* SampleStreamCacheService::find_source(
    SampleStreamSourceToken token) noexcept {
    auto* source = find_source_id(token.source_id);
    if (source == nullptr ||
        source->config.token.source_generation != token.source_generation) {
        return nullptr;
    }
    return source;
}

const SampleStreamCacheService::Source* SampleStreamCacheService::find_source(
    SampleStreamSourceToken token) const noexcept {
    const auto* source = find_source_id(token.source_id);
    if (source == nullptr ||
        source->config.token.source_generation != token.source_generation) {
        return nullptr;
    }
    return source;
}

bool SampleStreamCacheService::registration_matches(
    const Source& source,
    const SampleStreamAsyncReservation& reservation) noexcept {
    return reservation.registration_epoch == source.registration_epoch &&
           reservation.registration.matches(
               source.config.token,
               &source.window,
               source.config.total_frames,
               source.config.page_frames,
               source.registration_epoch);
}

bool SampleStreamCacheService::matching_filling_slot(
    const Source& source,
    const SampleStreamAsyncReservation& reservation) noexcept {
    return reservation.page_index < source.slots.size() &&
           source.async_reservations != 0 &&
           source.slots[reservation.page_index].fill_serial ==
               reservation.reservation_serial &&
           source.slots[reservation.page_index].fill_registration_epoch ==
               reservation.registration_epoch &&
           source.slots[reservation.page_index].fill_start_frame ==
               reservation.start_frame &&
           source.slots[reservation.page_index].fill_frame_count ==
               reservation.frame_count &&
           source.window.page_state(reservation.page_index) ==
               SampleStreamPageState::Filling;
}

bool SampleStreamCacheService::has_matching_filling_page(
    const Source& source,
    const SampleStreamPageRequest& request) noexcept {
    for (std::uint32_t page = 0; page < source.slots.size(); ++page) {
        const auto& slot = source.slots[page];
        if (slot.fill_registration_epoch == source.registration_epoch &&
            slot.fill_start_frame == request.start_frame &&
            slot.fill_frame_count == request.frame_count &&
            source.window.page_state(page) == SampleStreamPageState::Filling) {
            return true;
        }
    }
    return false;
}

SampleStreamCacheService::Source*
SampleStreamCacheService::matching_async_source(
    const SampleStreamAsyncReservation& reservation) noexcept {
    auto* source = find_source(reservation.source);
    if (source == nullptr || !registration_matches(*source, reservation)) {
        return nullptr;
    }
    return source;
}

std::optional<SampleStreamPageRequest>
SampleStreamCacheService::canonical_request(
    const Source& source,
    const SampleStreamPageDemand& demand) noexcept {
    if (demand.requester.requester_id == 0 ||
        demand.requester.requester_generation == 0 ||
        demand.page_index >
            std::numeric_limits<std::uint64_t>::max() /
                source.config.page_frames) {
        return std::nullopt;
    }
    const auto start_frame = demand.page_index * source.config.page_frames;
    if (start_frame >= source.config.total_frames) return std::nullopt;
    const auto frame_count = std::min(source.config.page_frames,
                                      source.config.total_frames - start_frame);
    return SampleStreamPageRequest{
        .source_id = source.config.token.source_id,
        .source_generation = source.config.token.source_generation,
        .requester_id = demand.requester.requester_id,
        .requester_generation = demand.requester.requester_generation,
        .page_index = demand.page_index,
        .start_frame = start_frame,
        .frame_count = frame_count,
        .resident_source_frames = demand.resident_source_frames,
        .consumption_frames_per_second = demand.consumption_frames_per_second,
        .demand_class = demand.demand_class,
    };
}

SampleStreamCacheService::PageReservation
SampleStreamCacheService::reserve_page(
    Source& source,
    std::uint64_t reservation_serial) noexcept {
    const auto completed_page_retirement_epoch =
        completed_page_retirement_epoch_.load(std::memory_order_acquire);
    for (std::uint32_t page = 0; page < source.window.page_count(); ++page) {
        if (source.window.page_state(page) == SampleStreamPageState::Empty &&
            source.window.begin_fill_page(page)) {
            source.slots[page] = {};
            source.slots[page].fill_serial = reservation_serial;
            return {PageReservationStatus::Reserved, page, false};
        }
    }

    bool has_retired_page = false;
    for (std::uint32_t page = 0; page < source.window.page_count(); ++page) {
        const auto state = source.window.page_state(page);
        if (state != SampleStreamPageState::Retired &&
            state != SampleStreamPageState::Retiring) {
            continue;
        }
        has_retired_page = true;
        if (state == SampleStreamPageState::Retired &&
            source.window.begin_fill_page(
                page, completed_page_retirement_epoch)) {
            source.slots[page] = {};
            source.slots[page].fill_serial = reservation_serial;
            return {PageReservationStatus::Reserved, page, true};
        }
    }
    if (has_retired_page) {
        return {PageReservationStatus::WaitingForGeneration, 0, false};
    }

    if (active_audio_generation_ == 0 || next_page_retirement_epoch_ == 0) {
        return {};
    }

    std::optional<std::uint32_t> victim;
    for (std::uint32_t page = 0; page < source.window.page_count(); ++page) {
        if (source.window.page_state(page) != SampleStreamPageState::Ready) {
            continue;
        }
        if (!victim ||
            source.slots[page].publish_sequence <
                source.slots[*victim].publish_sequence) {
            victim = page;
        }
    }
    if (!victim) return {};
    const auto retirement_epoch = next_page_retirement_epoch_++;
    if (!source.window.retire_page(*victim, retirement_epoch)) return {};
    // Publish only after Ready -> Retired. A callback that entered before
    // retirement captured the previous epoch and cannot release this page;
    // at least one callback entering after retirement must complete first.
    published_page_retirement_epoch_.store(retirement_epoch,
                                            std::memory_order_release);
    return {PageReservationStatus::RetiredVictim, *victim, false};
}

std::uint64_t SampleStreamCacheService::take_reservation_serial() noexcept {
    if (next_reservation_serial_ == 0) return 0;
    return next_reservation_serial_++;
}

bool SampleStreamCacheService::cancel_fill(
    Source& source,
    std::uint32_t page_index,
    std::uint64_t reservation_serial) noexcept {
    if (page_index >= source.slots.size() ||
        source.slots[page_index].fill_serial != reservation_serial ||
        source.window.page_state(page_index) != SampleStreamPageState::Filling) {
        return false;
    }
    if (!source.window.cancel_fill_page(page_index)) return false;
    source.slots[page_index].fill_serial = 0;
    return true;
}

bool SampleStreamCacheService::finish_async_reservation(Source& source) noexcept {
    if (source.async_reservations == 0) return false;
    --source.async_reservations;
    return true;
}

} // namespace pulp::audio
