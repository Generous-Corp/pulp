// An LV2 instrument that reports what its host gave it.
//
// Built as a real LV2 bundle (manifest.ttl, plugin.ttl, and this module) so the
// LV2 slot test hosts it exactly as it would any third-party plugin. It plays a
// note as a constant level (velocity / 127 times the Level port), echoes the
// MIDI it receives to its notify output, and writes what it observed to output
// control ports: the time:Position fields, the buf-size and sample-rate
// options, note-on and worker-response counts, the free-wheeling port, and the
// last patch:Set value it received.
//
// It needs urid:map, options:options, and worker:schedule. Every note-on
// schedules work; work() answers with note + 1000 and work_response() counts
// the answers. CC 20 makes the plugin change its own patch parameter to
// value / 10 and report it with patch:Set; patch:Get makes it report the
// current value. CC 21 schedules value * 100 requests at once, counting the
// ones the host refuses; CC 22 logs a trace message from run(); CC 24 emits a
// note-on stamped ten blocks in the future; CC 25 emits an object whose header
// claims more bytes than the sequence holds; CC 123 (All Notes Off) silences
// the voice. State saves the Amount property and the
// note-on count.
//
// Each patch property accepts only the atom type its rdfs:range declares
// (Amount Float, Count Int, Big Long, Flag Bool, Fine Double), so a host that
// sends the wrong type is visible as an unchanged value.

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <new>
#include <thread>

#define PULP_LV2_HOST_INSTRUMENT_URI "http://example.com/pulp/lv2-host-instrument"

namespace {

enum Port : std::uint32_t {
    kOutL = 0,
    kOutR,
    kControl,
    kNotify,
    kLevel,
    kLatency,
    kFreeWheel,
    kModCv,
    kSeenBpm,
    kSeenSpeed,
    kSeenBarBeat,
    kSeenBar,
    kSeenBeatUnit,
    kSeenBeatsPerBar,
    kSeenMaxBlock,
    kSeenSampleRate,
    kNoteOns,
    kWorkerResponses,
    kSeenFreeWheel,
    kSeenAmount,
    kTimeMessages,
    kSeenBeat,
    kSeenFps,
    kCutoff,
    kSeenCutoff,
    kSeenCount,
    kSeenBig,
    kSeenFlag,
    kSeenFine,
    kWorkerRefusals,
    kMalformedEvents,
    kPortCount
};

constexpr float kReportedLatency = 64.0f;

struct Urids {
    LV2_URID atom_float, atom_double, atom_int, atom_long, atom_bool, atom_object, atom_blank,
        atom_urid, atom_sequence, midi_event, time_position, time_speed, time_bar, time_bar_beat,
        time_beat, time_frames_per_second, time_beat_unit, time_beats_per_bar,
        time_beats_per_minute, patch_set, patch_get, patch_property, patch_value, amount, count,
        big, flag, fine, note_ons, buf_max, sample_rate, log_trace;
};

struct Instrument {
    LV2_URID_Map* map = nullptr;
    LV2_Worker_Schedule* schedule = nullptr;
    LV2_Log_Log* log = nullptr;
    Urids u{};
    LV2_Atom_Forge forge{};
    float* ports[kPortCount] = {};
    float max_block = 0.0f;
    float sample_rate = 0.0f;
    float level = 0.0f; // Current note's velocity / 127; zero when no note.
    float amount = 1.0f;
    std::int32_t note_ons = 0;
    std::int32_t worker_responses = 0;
    std::int32_t time_messages = 0;
    std::int32_t worker_refusals = 0;
    std::int32_t malformed_events = 0;
    bool report_amount = false;
};

void write_port(Instrument* self, Port port, float value) {
    if (self->ports[port])
        *self->ports[port] = value;
}

void forge_amount(Instrument* self, std::int64_t frame) {
    LV2_Atom_Forge_Frame obj;
    lv2_atom_forge_frame_time(&self->forge, frame);
    lv2_atom_forge_object(&self->forge, &obj, 0, self->u.patch_set);
    lv2_atom_forge_key(&self->forge, self->u.patch_property);
    lv2_atom_forge_urid(&self->forge, self->u.amount);
    lv2_atom_forge_key(&self->forge, self->u.patch_value);
    lv2_atom_forge_float(&self->forge, self->amount);
    lv2_atom_forge_pop(&self->forge, &obj);
}

float atom_number(const Urids& u, const LV2_Atom* a, float fallback) {
    if (!a)
        return fallback;
    if (a->type == u.atom_float)
        return reinterpret_cast<const LV2_Atom_Float*>(a)->body;
    if (a->type == u.atom_int)
        return static_cast<float>(reinterpret_cast<const LV2_Atom_Int*>(a)->body);
    if (a->type == u.atom_long)
        return static_cast<float>(reinterpret_cast<const LV2_Atom_Long*>(a)->body);
    return fallback;
}

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const* features) {
    auto* self = new (std::nothrow) Instrument();
    if (!self)
        return nullptr;
    const LV2_Options_Option* options = nullptr;
    for (int i = 0; features && features[i]; ++i) {
        if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
            self->map = static_cast<LV2_URID_Map*>(features[i]->data);
        else if (std::strcmp(features[i]->URI, LV2_WORKER__schedule) == 0)
            self->schedule = static_cast<LV2_Worker_Schedule*>(features[i]->data);
        else if (std::strcmp(features[i]->URI, LV2_LOG__log) == 0)
            self->log = static_cast<LV2_Log_Log*>(features[i]->data);
        else if (std::strcmp(features[i]->URI, LV2_OPTIONS__options) == 0)
            options = static_cast<const LV2_Options_Option*>(features[i]->data);
    }
    if (!self->map || !self->schedule || !options) {
        delete self;
        return nullptr;
    }
    LV2_URID_Map* m = self->map;
    auto& u = self->u;
    u.atom_float = m->map(m->handle, LV2_ATOM__Float);
    u.atom_double = m->map(m->handle, LV2_ATOM__Double);
    u.atom_bool = m->map(m->handle, LV2_ATOM__Bool);
    u.time_beat = m->map(m->handle, LV2_TIME__beat);
    u.time_frames_per_second = m->map(m->handle, LV2_TIME__framesPerSecond);
    u.count = m->map(m->handle, PULP_LV2_HOST_INSTRUMENT_URI "#count");
    u.big = m->map(m->handle, PULP_LV2_HOST_INSTRUMENT_URI "#big");
    u.flag = m->map(m->handle, PULP_LV2_HOST_INSTRUMENT_URI "#flag");
    u.fine = m->map(m->handle, PULP_LV2_HOST_INSTRUMENT_URI "#fine");
    u.log_trace = m->map(m->handle, LV2_LOG__Trace);
    u.atom_int = m->map(m->handle, LV2_ATOM__Int);
    u.atom_long = m->map(m->handle, LV2_ATOM__Long);
    u.atom_object = m->map(m->handle, LV2_ATOM__Object);
    u.atom_blank = m->map(m->handle, LV2_ATOM__Blank);
    u.atom_urid = m->map(m->handle, LV2_ATOM__URID);
    u.atom_sequence = m->map(m->handle, LV2_ATOM__Sequence);
    u.midi_event = m->map(m->handle, LV2_MIDI__MidiEvent);
    u.time_position = m->map(m->handle, LV2_TIME__Position);
    u.time_speed = m->map(m->handle, LV2_TIME__speed);
    u.time_bar = m->map(m->handle, LV2_TIME__bar);
    u.time_bar_beat = m->map(m->handle, LV2_TIME__barBeat);
    u.time_beat_unit = m->map(m->handle, LV2_TIME__beatUnit);
    u.time_beats_per_bar = m->map(m->handle, LV2_TIME__beatsPerBar);
    u.time_beats_per_minute = m->map(m->handle, LV2_TIME__beatsPerMinute);
    u.patch_set = m->map(m->handle, LV2_PATCH__Set);
    u.patch_get = m->map(m->handle, LV2_PATCH__Get);
    u.patch_property = m->map(m->handle, LV2_PATCH__property);
    u.patch_value = m->map(m->handle, LV2_PATCH__value);
    u.amount = m->map(m->handle, PULP_LV2_HOST_INSTRUMENT_URI "#amount");
    u.note_ons = m->map(m->handle, PULP_LV2_HOST_INSTRUMENT_URI "#noteOns");
    u.buf_max = m->map(m->handle, LV2_BUF_SIZE__maxBlockLength);
    u.sample_rate = m->map(m->handle, LV2_PARAMETERS__sampleRate);
    for (const LV2_Options_Option* o = options; o->key; ++o) {
        if (o->key == u.buf_max && o->type == u.atom_int)
            self->max_block = static_cast<float>(*static_cast<const std::int32_t*>(o->value));
        else if (o->key == u.sample_rate && o->type == u.atom_float)
            self->sample_rate = *static_cast<const float*>(o->value);
    }
    if (self->sample_rate == 0.0f)
        self->sample_rate = static_cast<float>(rate);
    lv2_atom_forge_init(&self->forge, m);
    return self;
}

void connect_port(LV2_Handle h, std::uint32_t port, void* data) {
    if (port < kPortCount)
        static_cast<Instrument*>(h)->ports[port] = static_cast<float*>(data);
}

void activate(LV2_Handle h) {
    static_cast<Instrument*>(h)->level = 0.0f;
}

void handle_object(Instrument* self, const LV2_Atom_Object* obj) {
    const auto& u = self->u;
    if (obj->body.otype == u.time_position) {
        ++self->time_messages;
        const LV2_Atom *speed = nullptr, *bar = nullptr, *bar_beat = nullptr, *unit = nullptr,
                       *per_bar = nullptr, *bpm = nullptr, *beat = nullptr, *fps = nullptr;
        lv2_atom_object_get(obj, u.time_speed, &speed, u.time_bar, &bar, u.time_bar_beat, &bar_beat,
                            u.time_beat_unit, &unit, u.time_beats_per_bar, &per_bar,
                            u.time_beats_per_minute, &bpm, u.time_beat, &beat,
                            u.time_frames_per_second, &fps, 0);
        write_port(self, kSeenSpeed, atom_number(u, speed, -1.0f));
        write_port(self, kSeenBar, atom_number(u, bar, -1.0f));
        write_port(self, kSeenBarBeat, atom_number(u, bar_beat, -1.0f));
        write_port(self, kSeenBeatUnit, atom_number(u, unit, -1.0f));
        write_port(self, kSeenBeatsPerBar, atom_number(u, per_bar, -1.0f));
        write_port(self, kSeenBpm, atom_number(u, bpm, -1.0f));
        // time:beat is a Double; anything else counts as absent.
        write_port(self, kSeenBeat,
                   beat && beat->type == u.atom_double
                       ? static_cast<float>(reinterpret_cast<const LV2_Atom_Double*>(beat)->body)
                       : -1.0f);
        write_port(self, kSeenFps, atom_number(u, fps, -1.0f));
    } else if (obj->body.otype == u.patch_set) {
        const LV2_Atom *property = nullptr, *value = nullptr;
        lv2_atom_object_get(obj, u.patch_property, &property, u.patch_value, &value, 0);
        if (!property || property->type != u.atom_urid || !value)
            return;
        const LV2_URID key = reinterpret_cast<const LV2_Atom_URID*>(property)->body;
        // Strict: a value of any type but the declared range is ignored.
        if (key == u.amount && value->type == u.atom_float) {
            self->amount = reinterpret_cast<const LV2_Atom_Float*>(value)->body;
            write_port(self, kSeenAmount, self->amount);
        } else if (key == u.count && value->type == u.atom_int) {
            write_port(self, kSeenCount,
                       static_cast<float>(reinterpret_cast<const LV2_Atom_Int*>(value)->body));
        } else if (key == u.big && value->type == u.atom_long) {
            write_port(self, kSeenBig,
                       static_cast<float>(reinterpret_cast<const LV2_Atom_Long*>(value)->body));
        } else if (key == u.flag && value->type == u.atom_bool) {
            write_port(self, kSeenFlag,
                       reinterpret_cast<const LV2_Atom_Bool*>(value)->body ? 1.0f : 0.0f);
        } else if (key == u.fine && value->type == u.atom_double) {
            write_port(self, kSeenFine,
                       static_cast<float>(reinterpret_cast<const LV2_Atom_Double*>(value)->body));
        }
    } else if (obj->body.otype == u.patch_get) {
        self->report_amount = true;
    }
}

void run(LV2_Handle h, std::uint32_t frames) {
    auto* self = static_cast<Instrument*>(h);
    const auto& u = self->u;

    auto* notify = reinterpret_cast<LV2_Atom_Sequence*>(self->ports[kNotify]);
    const std::uint32_t capacity = notify->atom.size;
    lv2_atom_forge_set_buffer(&self->forge, reinterpret_cast<std::uint8_t*>(notify), capacity);
    LV2_Atom_Forge_Frame seq;
    lv2_atom_forge_sequence_head(&self->forge, &seq, 0);

    const float gain = self->ports[kLevel] ? *self->ports[kLevel] : 1.0f;
    std::uint32_t frame = 0;
    auto render_to = [&](std::uint32_t end) {
        for (; frame < end && frame < frames; ++frame) {
            const float s = self->level * gain;
            self->ports[kOutL][frame] = s;
            self->ports[kOutR][frame] = s;
        }
    };

    const auto* control = reinterpret_cast<const LV2_Atom_Sequence*>(self->ports[kControl]);
    // A strict plugin checks that every event lies inside the sequence the
    // host declared; a partial event (header without body) does not.
    const auto* control_end =
        reinterpret_cast<const std::uint8_t*>(control) + sizeof(LV2_Atom) + control->atom.size;
    LV2_ATOM_SEQUENCE_FOREACH(control, ev) {
        if (reinterpret_cast<const std::uint8_t*>(&ev->body) + sizeof(LV2_Atom) +
                lv2_atom_pad_size(ev->body.size) >
            control_end) {
            ++self->malformed_events;
            break;
        }
        render_to(static_cast<std::uint32_t>(ev->time.frames));
        if (ev->body.type == u.midi_event) {
            const auto* msg = static_cast<const std::uint8_t*>(LV2_ATOM_BODY_CONST(&ev->body));
            // Echo every MIDI event to the host at the same frame.
            lv2_atom_forge_frame_time(&self->forge, ev->time.frames);
            lv2_atom_forge_atom(&self->forge, ev->body.size, u.midi_event);
            lv2_atom_forge_write(&self->forge, msg, ev->body.size);
            const std::uint8_t status = msg[0] & 0xF0;
            if (status == LV2_MIDI_MSG_NOTE_ON && ev->body.size == 3 && msg[2] > 0) {
                self->level = static_cast<float>(msg[2]) / 127.0f;
                ++self->note_ons;
                const std::int32_t note = msg[1];
                self->schedule->schedule_work(self->schedule->handle, sizeof(note), &note);
            } else if (status == LV2_MIDI_MSG_NOTE_OFF ||
                       (status == LV2_MIDI_MSG_NOTE_ON && ev->body.size == 3 && msg[2] == 0)) {
                self->level = 0.0f;
            } else if (status == LV2_MIDI_MSG_CONTROLLER && ev->body.size == 3 && msg[1] == 20) {
                self->amount = static_cast<float>(msg[2]) / 10.0f;
                forge_amount(self, ev->time.frames);
            } else if (status == LV2_MIDI_MSG_CONTROLLER && ev->body.size == 3 && msg[1] == 21) {
                // A flood the host's worker queue cannot hold. -1 answers 999,
                // which work_response() does not count.
                const std::int32_t flood = -1;
                for (int i = 0; i < msg[2] * 100; ++i)
                    if (self->schedule->schedule_work(self->schedule->handle, sizeof(flood),
                                                      &flood) != LV2_WORKER_SUCCESS)
                        ++self->worker_refusals;
            } else if (status == LV2_MIDI_MSG_CONTROLLER && ev->body.size == 3 && msg[1] == 22) {
                if (self->log)
                    self->log->printf(self->log->handle, u.log_trace, "trace from run() %d",
                                      msg[2]);
            } else if (status == LV2_MIDI_MSG_CONTROLLER && ev->body.size == 3 && msg[1] == 24) {
                const std::uint8_t late[3] = {0x90, 99, 99};
                lv2_atom_forge_frame_time(&self->forge, static_cast<std::int64_t>(frames) * 10);
                lv2_atom_forge_atom(&self->forge, sizeof(late), u.midi_event);
                lv2_atom_forge_write(&self->forge, late, sizeof(late));
            } else if (status == LV2_MIDI_MSG_CONTROLLER && ev->body.size == 3 && msg[1] == 25) {
                // Only the header is written; its size points past the end.
                lv2_atom_forge_frame_time(&self->forge, ev->time.frames);
                lv2_atom_forge_atom(&self->forge, 60000, u.atom_object);
            } else if (status == LV2_MIDI_MSG_CONTROLLER && ev->body.size == 3 &&
                       msg[1] == LV2_MIDI_CTL_ALL_NOTES_OFF) {
                self->level = 0.0f;
            }
        } else if (lv2_atom_forge_is_object_type(&self->forge, ev->body.type)) {
            handle_object(self, reinterpret_cast<const LV2_Atom_Object*>(&ev->body));
        }
    }
    render_to(frames);
    if (self->report_amount) {
        forge_amount(self, frames ? frames - 1 : 0);
        self->report_amount = false;
    }
    lv2_atom_forge_pop(&self->forge, &seq);

    write_port(self, kLatency, kReportedLatency);
    write_port(self, kSeenMaxBlock, self->max_block);
    write_port(self, kSeenSampleRate, self->sample_rate);
    write_port(self, kNoteOns, static_cast<float>(self->note_ons));
    write_port(self, kWorkerResponses, static_cast<float>(self->worker_responses));
    write_port(self, kTimeMessages, static_cast<float>(self->time_messages));
    write_port(self, kWorkerRefusals, static_cast<float>(self->worker_refusals));
    write_port(self, kMalformedEvents, static_cast<float>(self->malformed_events));
    write_port(self, kSeenCutoff, self->ports[kCutoff] ? *self->ports[kCutoff] : -1.0f);
    write_port(self, kSeenFreeWheel, self->ports[kFreeWheel] ? *self->ports[kFreeWheel] : -1.0f);
    if (self->ports[kModCv] && self->ports[kModCv][0] != 0.0f)
        write_port(self, kSeenFreeWheel, -2.0f); // A CV input that is not silent.
}

void cleanup(LV2_Handle h) {
    delete static_cast<Instrument*>(h);
}

LV2_Worker_Status work(LV2_Handle, LV2_Worker_Respond_Function respond,
                       LV2_Worker_Respond_Handle handle, std::uint32_t size, const void* data) {
    if (size != sizeof(std::int32_t))
        return LV2_WORKER_ERR_UNKNOWN;
    std::int32_t answer = 0;
    std::memcpy(&answer, data, sizeof(answer));
    // Flood requests are slow, so the host's queue fills behind them.
    if (answer < 0)
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    answer += 1000;
    return respond(handle, sizeof(answer), &answer);
}

LV2_Worker_Status work_response(LV2_Handle h, std::uint32_t size, const void* data) {
    auto* self = static_cast<Instrument*>(h);
    std::int32_t answer = 0;
    if (size == sizeof(answer)) {
        std::memcpy(&answer, data, sizeof(answer));
        if (answer >= 1000)
            ++self->worker_responses;
    }
    return LV2_WORKER_SUCCESS;
}

LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle handle,
                      std::uint32_t, const LV2_Feature* const*) {
    auto* self = static_cast<Instrument*>(h);
    store(handle, self->u.amount, &self->amount, sizeof(float), self->u.atom_float,
          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    store(handle, self->u.note_ons, &self->note_ons, sizeof(std::int32_t), self->u.atom_int,
          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    return LV2_STATE_SUCCESS;
}

LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve,
                         LV2_State_Handle handle, std::uint32_t, const LV2_Feature* const*) {
    auto* self = static_cast<Instrument*>(h);
    std::size_t size = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    if (const void* v = retrieve(handle, self->u.amount, &size, &type, &flags);
        v && type == self->u.atom_float && size == sizeof(float))
        std::memcpy(&self->amount, v, sizeof(float));
    if (const void* v = retrieve(handle, self->u.note_ons, &size, &type, &flags);
        v && type == self->u.atom_int && size == sizeof(std::int32_t))
        std::memcpy(&self->note_ons, v, sizeof(std::int32_t));
    return LV2_STATE_SUCCESS;
}

const void* extension_data(const char* uri) {
    static const LV2_Worker_Interface worker = {work, work_response, nullptr};
    static const LV2_State_Interface state = {save, restore};
    if (std::strcmp(uri, LV2_WORKER__interface) == 0)
        return &worker;
    if (std::strcmp(uri, LV2_STATE__interface) == 0)
        return &state;
    return nullptr;
}

const LV2_Descriptor kDescriptor = {
    PULP_LV2_HOST_INSTRUMENT_URI,
    instantiate,
    connect_port,
    activate,
    run,
    nullptr,
    cleanup,
    extension_data,
};

} // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(std::uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}
