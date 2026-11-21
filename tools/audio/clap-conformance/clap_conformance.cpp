// SPDX-License-Identifier: MIT
//
// Headless CLAP host-conformance checker. Loads a .clap module and runs four
// checks, each pass/fail with the numbers behind it:
//
//   scan-cost         dlopen + entry init + factory descriptor reads, no plug-in
//                     created: instructions (macOS) and peak footprint growth.
//   automation-echo   host parameter events in; the plug-in must emit no
//                     parameter or gesture events back.
//   input-ports       process with fewer / zero input ports than declared must
//                     not crash and must produce finite output.
//   state-round-trip  save, load into a fresh instance, save again: bytes equal.
//
// Depends only on the CLAP headers and the C library; it links no Pulp code, so
// it measures the module as a host would.

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
int main() {
    std::fprintf(stderr, "clap-conformance: not supported on Windows\n");
    return 2;
}
#else
#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif

namespace {

struct Options {
    std::string path;
    std::string plugin_id; // empty: first plug-in
    bool json = false;
    std::string expect_fail; // exit 0 only if this check FAILS (negative controls)
    uint64_t budget_instructions = 500'000'000ull;
    uint64_t budget_footprint_mb = 32;
};

struct Result {
    std::string name;
    std::string status; // PASS | FAIL | SKIP
    std::string detail;
};

// ---- process metrics ------------------------------------------------------

uint64_t instructions_now() {
#if defined(__APPLE__)
    rusage_info_v4 ri{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) == 0)
        return ri.ri_instructions;
#endif
    return 0;
}

uint64_t peak_footprint_bytes() {
#if defined(__APPLE__)
    rusage_info_v4 ri{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) == 0)
        return ri.ri_lifetime_max_phys_footprint;
    return 0;
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<uint64_t>(ru.ru_maxrss) * 1024ull;
#endif
}

// ---- host -----------------------------------------------------------------

struct HostState {
    int request_flush = 0;
    int request_callback = 0;
    int request_restart = 0;
};

void host_request_restart(const clap_host_t* h) {
    static_cast<HostState*>(h->host_data)->request_restart++;
}
void host_request_process(const clap_host_t*) {}
void host_request_callback(const clap_host_t* h) {
    static_cast<HostState*>(h->host_data)->request_callback++;
}
void params_rescan(const clap_host_t*, clap_param_rescan_flags) {}
void params_clear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void params_request_flush(const clap_host_t* h) {
    static_cast<HostState*>(h->host_data)->request_flush++;
}
const clap_host_params_t kHostParams = {params_rescan, params_clear, params_request_flush};

const void* host_get_extension(const clap_host_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &kHostParams;
    return nullptr;
}

clap_host_t make_host(HostState* st) {
    clap_host_t h{};
    h.clap_version = CLAP_VERSION;
    h.host_data = st;
    h.name = "pulp-clap-conformance";
    h.vendor = "pulp";
    h.url = "";
    h.version = "1";
    h.get_extension = host_get_extension;
    h.request_restart = host_request_restart;
    h.request_process = host_request_process;
    h.request_callback = host_request_callback;
    return h;
}

// ---- event queues ---------------------------------------------------------

struct InQueue {
    std::vector<clap_event_param_value_t> ev;
};
uint32_t in_size(const clap_input_events_t* l) {
    return static_cast<uint32_t>(static_cast<InQueue*>(l->ctx)->ev.size());
}
const clap_event_header_t* in_get(const clap_input_events_t* l, uint32_t i) {
    return &static_cast<InQueue*>(l->ctx)->ev[i].header;
}

struct OutCount {
    int param_value = 0;
    int gesture_begin = 0;
    int gesture_end = 0;
    int total() const {
        return param_value + gesture_begin + gesture_end;
    }
};
bool out_push(const clap_output_events_t* l, const clap_event_header_t* h) {
    auto* c = static_cast<OutCount*>(l->ctx);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID) {
        if (h->type == CLAP_EVENT_PARAM_VALUE)
            c->param_value++;
        else if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN)
            c->gesture_begin++;
        else if (h->type == CLAP_EVENT_PARAM_GESTURE_END)
            c->gesture_end++;
    }
    return true;
}

// ---- module + instance ----------------------------------------------------

struct Module {
    void* dl = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
    std::string error;

    bool open(const std::string& path) {
        dl = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!dl) {
            error = std::string("dlopen failed: ") + dlerror();
            return false;
        }
        entry = static_cast<const clap_plugin_entry_t*>(dlsym(dl, "clap_entry"));
        if (!entry) {
            error = "no clap_entry symbol";
            return false;
        }
        if (!entry->init(path.c_str())) {
            error = "clap_entry.init returned false";
            return false;
        }
        factory =
            static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory) {
            error = "no plug-in factory";
            return false;
        }
        return true;
    }
};

struct Instance {
    HostState hs;
    clap_host_t host;
    const clap_plugin_t* p = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_audio_ports_t* ports = nullptr;
    std::vector<clap_param_info_t> infos;
    uint32_t in_ports = 0, out_ports = 0;
    std::vector<uint32_t> in_ch, out_ch;
    bool active = false;

    bool create(Module& m, const std::string& id, std::string& err) {
        host = make_host(&hs);
        p = m.factory->create_plugin(m.factory, &host, id.c_str());
        if (!p) {
            err = "create_plugin returned null";
            return false;
        }
        if (!p->init(p)) {
            err = "plugin init failed";
            return false;
        }
        params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
        ports = static_cast<const clap_plugin_audio_ports_t*>(
            p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
        if (params) {
            uint32_t n = params->count(p);
            for (uint32_t i = 0; i < n; ++i) {
                clap_param_info_t info{};
                if (params->get_info(p, i, &info))
                    infos.push_back(info);
            }
        }
        if (ports) {
            in_ports = ports->count(p, true);
            out_ports = ports->count(p, false);
            for (uint32_t i = 0; i < in_ports; ++i) {
                clap_audio_port_info_t pi{};
                ports->get(p, i, true, &pi);
                in_ch.push_back(pi.channel_count);
            }
            for (uint32_t i = 0; i < out_ports; ++i) {
                clap_audio_port_info_t pi{};
                ports->get(p, i, false, &pi);
                out_ch.push_back(pi.channel_count);
            }
        }
        if (!p->activate(p, 48000.0, 1, 512)) {
            err = "activate failed";
            return false;
        }
        active = true;
        if (!p->start_processing(p)) {
            err = "start_processing failed";
            return false;
        }
        return true;
    }

    void destroy() {
        if (!p)
            return;
        p->stop_processing(p);
        p->deactivate(p);
        p->destroy(p);
        p = nullptr;
    }
};

// One processed block. `inputs_mode`: 0 declared ports, 1 no ports (count 0,
// null), 2 declared count with zero channels and null data.
struct Block {
    static constexpr uint32_t kFrames = 256;
    std::vector<std::vector<float>> in_data, out_data;
    std::vector<std::vector<float*>> in_ptrs, out_ptrs;
    std::vector<clap_audio_buffer_t> in_bufs, out_bufs;

    explicit Block(const Instance& inst) {
        in_data.resize(inst.in_ports);
        in_ptrs.resize(inst.in_ports);
        in_bufs.resize(inst.in_ports);
        for (uint32_t i = 0; i < inst.in_ports; ++i) {
            in_data[i].assign(kFrames * std::max(1u, inst.in_ch[i]), 0.25f);
            for (uint32_t c = 0; c < inst.in_ch[i]; ++c)
                in_ptrs[i].push_back(in_data[i].data() + c * kFrames);
            in_bufs[i] = {in_ptrs[i].data(), nullptr, inst.in_ch[i], 0, 0};
        }
        out_data.resize(inst.out_ports);
        out_ptrs.resize(inst.out_ports);
        out_bufs.resize(inst.out_ports);
        for (uint32_t i = 0; i < inst.out_ports; ++i) {
            out_data[i].assign(kFrames * std::max(1u, inst.out_ch[i]), 0.0f);
            for (uint32_t c = 0; c < inst.out_ch[i]; ++c)
                out_ptrs[i].push_back(out_data[i].data() + c * kFrames);
            out_bufs[i] = {out_ptrs[i].data(), nullptr, inst.out_ch[i], 0, 0};
        }
    }

    bool output_finite() const {
        for (auto& v : out_data)
            for (float f : v)
                if (!std::isfinite(f))
                    return false;
        return true;
    }

    clap_process_status run(Instance& inst, InQueue& in, OutCount& out, int inputs_mode) {
        clap_input_events_t ie{&in, in_size, in_get};
        clap_output_events_t oe{&out, out_push};
        clap_process_t pr{};
        pr.frames_count = kFrames;
        pr.transport = nullptr;
        pr.in_events = &ie;
        pr.out_events = &oe;
        std::vector<clap_audio_buffer_t> zero_ch;
        if (inputs_mode == 0) {
            pr.audio_inputs = in_bufs.empty() ? nullptr : in_bufs.data();
            pr.audio_inputs_count = static_cast<uint32_t>(in_bufs.size());
        } else if (inputs_mode == 1) {
            pr.audio_inputs = nullptr;
            pr.audio_inputs_count = 0;
        } else {
            zero_ch.assign(inst.in_ports, clap_audio_buffer_t{nullptr, nullptr, 0, 0, 0});
            pr.audio_inputs = zero_ch.empty() ? nullptr : zero_ch.data();
            pr.audio_inputs_count = inst.in_ports;
        }
        pr.audio_outputs = out_bufs.empty() ? nullptr : out_bufs.data();
        pr.audio_outputs_count = static_cast<uint32_t>(out_bufs.size());
        return inst.p->process(inst.p, &pr);
    }
};

clap_event_param_value_t make_param_event(const clap_param_info_t& info, double value,
                                          uint32_t time) {
    clap_event_param_value_t e{};
    e.header.size = sizeof(e);
    e.header.time = time;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.header.flags = 0;
    e.param_id = info.id;
    e.cookie = info.cookie;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = value;
    return e;
}

bool automatable(const clap_param_info_t& i) {
    return (i.flags & CLAP_PARAM_IS_READONLY) == 0 && (i.flags & CLAP_PARAM_IS_HIDDEN) == 0;
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

// ---- checks ---------------------------------------------------------------

// Runs `fn` in a forked child; returns its exit code, or 128+signal on a crash.
template <class Fn> int run_in_child(Fn&& fn, std::string* captured = nullptr) {
    int fds[2];
    if (pipe(fds) != 0)
        return 255;
    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0)
        return 255;
    if (pid == 0) {
        close(fds[0]);
        std::string msg;
        int rc = fn(msg);
        if (!msg.empty()) {
            ssize_t w = write(fds[1], msg.data(), msg.size());
            (void)w;
        }
        _exit(rc);
    }
    close(fds[1]);
    std::string out;
    char buf[256];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (captured)
        *captured = out;
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return WEXITSTATUS(status);
}

std::string first_plugin_id(const clap_plugin_factory_t* f) {
    if (f->get_plugin_count(f) == 0)
        return {};
    return f->get_plugin_descriptor(f, 0)->id;
}

Result check_scan_cost(const Options& o) {
    Result r{"scan-cost", "FAIL", ""};
    std::string msg;
    int rc = run_in_child(
        [&](std::string& out) {
            const uint64_t i0 = instructions_now();
            const uint64_t f0 = peak_footprint_bytes();
            Module m;
            if (!m.open(o.path)) {
                out = m.error;
                return 2;
            }
            uint32_t n = m.factory->get_plugin_count(m.factory);
            for (uint32_t i = 0; i < n; ++i)
                (void)m.factory->get_plugin_descriptor(m.factory, i);
            const uint64_t i1 = instructions_now();
            const uint64_t f1 = peak_footprint_bytes();
            m.entry->deinit();
            out = fmt("%llu %llu %u", static_cast<unsigned long long>(i1 - i0),
                      static_cast<unsigned long long>(f1 > f0 ? f1 - f0 : 0), n);
            return 0;
        },
        &msg);
    if (rc != 0) {
        r.detail = rc >= 128 ? fmt("scan crashed (signal %d)", rc - 128) : "scan failed: " + msg;
        return r;
    }
    unsigned long long instr = 0, bytes = 0;
    unsigned n = 0;
    std::sscanf(msg.c_str(), "%llu %llu %u", &instr, &bytes, &n);
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::string instr_s = instr ? fmt("%.1f M instructions", instr / 1e6) : "instructions n/a";
    bool bad_instr = instr && instr > o.budget_instructions;
    bool bad_mem = mb > static_cast<double>(o.budget_footprint_mb);
    r.status = (bad_instr || bad_mem) ? "FAIL" : "PASS";
    r.detail = fmt("%u plug-in(s), %s (budget %.0f M), peak footprint +%.1f MB (budget %llu MB)", n,
                   instr_s.c_str(), o.budget_instructions / 1e6, mb,
                   static_cast<unsigned long long>(o.budget_footprint_mb));
    if (bad_instr || bad_mem)
        r.detail += "; a Processor is probably built during scan";
    return r;
}

Result check_automation_echo(Module& m, const std::string& id) {
    Result r{"automation-echo", "FAIL", ""};
    Instance inst;
    std::string err;
    if (!inst.create(m, id, err)) {
        r.detail = err;
        return r;
    }
    std::vector<clap_param_info_t> ps;
    for (auto& i : inst.infos)
        if (automatable(i))
            ps.push_back(i);
    if (ps.empty()) {
        inst.destroy();
        r.status = "SKIP";
        r.detail = "plug-in exposes no automatable parameters";
        return r;
    }
    Block blk(inst);
    OutCount out;
    int blocks = 0;
    for (int b = 0; b < 48; ++b) {
        InQueue q;
        if (b < 32) {
            for (auto& i : ps) {
                double span = i.max_value - i.min_value;
                double v = i.min_value + span * (0.5 + 0.4 * std::sin(0.3 * b + i.id));
                q.ev.push_back(make_param_event(i, v, 0));
                q.ev.push_back(make_param_event(i, v, 128));
            }
        }
        blk.run(inst, q, out, 0);
        if (inst.hs.request_callback) {
            inst.hs.request_callback = 0;
            inst.p->on_main_thread(inst.p);
        }
        ++blocks;
    }
    if (inst.params) {
        InQueue q;
        for (auto& i : ps)
            q.ev.push_back(make_param_event(i, i.default_value, 0));
        clap_input_events_t ie{&q, in_size, in_get};
        OutCount fo;
        clap_output_events_t oe{&fo, out_push};
        inst.params->flush(inst.p, &ie, &oe);
        out.param_value += fo.param_value;
        out.gesture_begin += fo.gesture_begin;
        out.gesture_end += fo.gesture_end;
    }
    r.detail =
        fmt("%zu param(s) driven over %d blocks; out-events: %d param-value, %d gesture-begin, "
            "%d gesture-end; host request_flush calls: %d",
            ps.size(), blocks, out.param_value, out.gesture_begin, out.gesture_end,
            inst.hs.request_flush);
    r.status = out.total() == 0 ? "PASS" : "FAIL";
    inst.destroy();
    return r;
}

Result check_input_ports(const Options& o, const std::string& id) {
    Result r{"input-ports", "FAIL", ""};
    std::string msg;
    int rc = run_in_child(
        [&](std::string& out) {
            Module m;
            if (!m.open(o.path)) {
                out = m.error;
                return 2;
            }
            Instance inst;
            std::string err;
            if (!inst.create(m, id, err)) {
                out = err;
                return 2;
            }
            if (inst.in_ports == 0) {
                out = "no input ports declared";
                return 3;
            }
            Block blk(inst);
            for (int mode = 1; mode <= 2; ++mode) {
                for (int b = 0; b < 4; ++b) {
                    InQueue q;
                    OutCount oc;
                    auto st = blk.run(inst, q, oc, mode);
                    if (st == CLAP_PROCESS_ERROR) {
                        out = fmt("process returned error with input mode %d", mode);
                        return 1;
                    }
                }
                if (!blk.output_finite()) {
                    out = fmt("non-finite output with input mode %d", mode);
                    return 1;
                }
            }
            out = fmt("%u declared input port(s)", inst.in_ports);
            return 0;
        },
        &msg);
    if (rc == 3) {
        r.status = "SKIP";
        r.detail = msg;
    } else if (rc == 0) {
        r.status = "PASS";
        r.detail = msg + "; processed with zero ports and with zero-channel ports, finite output";
    } else if (rc >= 128) {
        r.detail = fmt("crashed with signal %d when input ports were missing", rc - 128);
    } else {
        r.detail = msg;
    }
    return r;
}

struct Sink {
    std::vector<uint8_t> bytes;
};
int64_t sink_write(const clap_ostream_t* s, const void* buf, uint64_t n) {
    auto* k = static_cast<Sink*>(s->ctx);
    auto* b = static_cast<const uint8_t*>(buf);
    k->bytes.insert(k->bytes.end(), b, b + n);
    return static_cast<int64_t>(n);
}
struct Src {
    const std::vector<uint8_t>* bytes;
    size_t pos = 0;
};
int64_t src_read(const clap_istream_t* s, void* buf, uint64_t n) {
    auto* k = static_cast<Src*>(s->ctx);
    size_t take = std::min<size_t>(n, k->bytes->size() - k->pos);
    std::memcpy(buf, k->bytes->data() + k->pos, take);
    k->pos += take;
    return static_cast<int64_t>(take);
}

bool save_state(Instance& inst, std::vector<uint8_t>& out) {
    Sink sink;
    clap_ostream_t os{&sink, sink_write};
    if (!inst.state->save(inst.p, &os))
        return false;
    out = std::move(sink.bytes);
    return true;
}

Result check_state_round_trip(Module& m, const std::string& id) {
    Result r{"state-round-trip", "FAIL", ""};
    Instance a;
    std::string err;
    if (!a.create(m, id, err)) {
        r.detail = err;
        return r;
    }
    if (!a.state) {
        a.destroy();
        r.status = "SKIP";
        r.detail = "plug-in has no state extension";
        return r;
    }
    // Move every automatable parameter off its default so the state is not trivial.
    Block blk(a);
    InQueue q;
    for (auto& i : a.infos)
        if (automatable(i))
            q.ev.push_back(
                make_param_event(i, i.min_value + (i.max_value - i.min_value) * 0.37, 0));
    OutCount oc;
    blk.run(a, q, oc, 0);
    std::vector<uint8_t> s1, s2;
    if (!save_state(a, s1)) {
        a.destroy();
        r.detail = "state.save failed";
        return r;
    }
    Instance b;
    if (!b.create(m, id, err)) {
        a.destroy();
        r.detail = "second instance: " + err;
        return r;
    }
    Src src{&s1, 0};
    clap_istream_t is{&src, src_read};
    bool loaded = b.state->load(b.p, &is);
    bool saved = loaded && save_state(b, s2);
    a.destroy();
    b.destroy();
    if (!loaded) {
        r.detail = fmt("state.load failed on a fresh instance (%zu bytes)", s1.size());
        return r;
    }
    if (!saved) {
        r.detail = "state.save failed after load";
        return r;
    }
    if (s1 == s2) {
        r.status = "PASS";
        r.detail = fmt("state size %zu bytes, save/load/save bytes identical", s1.size());
    } else {
        size_t at = 0;
        while (at < s1.size() && at < s2.size() && s1[at] == s2[at])
            ++at;
        r.detail = fmt("save/load/save differs: %zu vs %zu bytes, first difference at byte %zu",
                       s1.size(), s2.size(), at);
    }
    return r;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\')
            o += '\\';
        o += c;
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--json")
            o.json = true;
        else if (a == "--expect-fail")
            o.expect_fail = next();
        else if (a == "--plugin-id")
            o.plugin_id = next();
        else if (a == "--budget-instructions")
            o.budget_instructions = std::strtoull(next(), nullptr, 10);
        else if (a == "--budget-footprint-mb")
            o.budget_footprint_mb = std::strtoull(next(), nullptr, 10);
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: pulp-clap-conformance <plugin.clap> [--plugin-id ID] [--json]\n"
                "       [--budget-instructions N] [--budget-footprint-mb N]\n"
                "       [--expect-fail CHECK]  exit 0 only if CHECK fails (negative controls)\n"
                "exit: 0 all checks pass, 1 a check failed, 2 module unusable\n");
            return 0;
        } else
            o.path = a;
    }
    if (o.path.empty()) {
        std::fprintf(stderr, "clap-conformance: no plug-in path (see --help)\n");
        return 2;
    }

    std::vector<Result> results;
    results.push_back(check_scan_cost(o));

    Module m;
    if (!m.open(o.path)) {
        std::fprintf(stderr, "clap-conformance: %s\n", m.error.c_str());
        return 2;
    }
    const std::string id = o.plugin_id.empty() ? first_plugin_id(m.factory) : o.plugin_id;
    if (id.empty()) {
        std::fprintf(stderr, "clap-conformance: module lists no plug-ins\n");
        return 2;
    }
    results.push_back(check_automation_echo(m, id));
    results.push_back(check_input_ports(o, id));
    results.push_back(check_state_round_trip(m, id));

    bool failed = false;
    for (auto& r : results)
        failed |= r.status == "FAIL";
    if (o.json) {
        std::printf("{\"plugin\":\"%s\",\"checks\":[", json_escape(id).c_str());
        for (size_t i = 0; i < results.size(); ++i)
            std::printf("%s{\"name\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"}", i ? "," : "",
                        results[i].name.c_str(), results[i].status.c_str(),
                        json_escape(results[i].detail).c_str());
        std::printf("]}\n");
    } else {
        std::printf("plug-in: %s\n", id.c_str());
        for (auto& r : results)
            std::printf("  %-4s %-17s %s\n", r.status.c_str(), r.name.c_str(), r.detail.c_str());
    }
    if (!o.expect_fail.empty()) {
        for (auto& r : results)
            if (r.name == o.expect_fail)
                return r.status == "FAIL" ? 0 : 1;
        return 2;
    }
    return failed ? 1 : 0;
}
#endif
