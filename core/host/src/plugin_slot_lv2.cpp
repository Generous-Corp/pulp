// LV2 plugin slot implementation.
//
// LV2 plugins ship as a bundle directory containing one or more .so files
// plus TTL manifests. This loader:
//   1. Finds the .so file referenced for the plugin URI (scanning bundle).
//   2. dlopen's it, resolves lv2_descriptor(index), picks the descriptor
//      whose URI matches PluginInfo.unique_id (or index 0 if empty).
//   3. Parses the bundle's TTL files with a tiny regex-based scanner to
//      discover audio input/output port indices (lv2:AudioPort +
//      lv2:InputPort / lv2:OutputPort, and lv2:index). We deliberately
//      don't pull in lilv — that adds serd/sord/sratom/lilv (~15 MB of
//      third-party copyleft-adjacent code) and the discovery is simple
//      enough to get right with a regex for the MVP port set.
//   4. Wires LV2_Descriptor's connect_port/activate/run/deactivate/cleanup
//      into the PluginSlot interface.
//
// Parameter automation (lv2:ControlPort), MIDI (LV2 atom ports), worker
// extension, state extension, and editors remain follow-up work.

#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/log.hpp>

#include <lv2/core/lv2.h>

#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace pulp::host {
namespace {

namespace fs = std::filesystem;

struct PortRole {
    int index = -1;
    bool is_audio = false;
    bool is_input = false;
};

// Scan every .ttl file in the bundle, concatenate content, and extract audio
// port roles. We look for port stanzas containing:
//    a lv2:AudioPort , lv2:InputPort   (or OutputPort)
//    lv2:index N
// This matches the conventional LV2 style. TTL blank-node syntax varies —
// we only support the common "[ a lv2:…Port, lv2:…Port ; lv2:index N ; … ]"
// pattern here.
std::vector<PortRole> discover_audio_ports(const std::string& bundle_dir) {
    std::vector<PortRole> out;
    std::string all;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(bundle_dir, ec)) {
        if (entry.path().extension() != ".ttl") continue;
        std::ifstream f(entry.path());
        if (!f) continue;
        std::string buf((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        all += buf;
        all += "\n";
    }
    // Find port stanzas: a bracketed block that declares a lv2:AudioPort
    // alongside lv2:InputPort / lv2:OutputPort, and an lv2:index.
    std::regex stanza(R"(\[\s*((?:.|\n)*?)\s*\])");
    auto begin = std::sregex_iterator(all.begin(), all.end(), stanza);
    auto end = std::sregex_iterator();
    std::regex audio_re(R"(lv2:AudioPort)");
    std::regex in_re(R"(lv2:InputPort)");
    std::regex out_re(R"(lv2:OutputPort)");
    std::regex idx_re(R"(lv2:index\s+(\d+))");
    for (auto it = begin; it != end; ++it) {
        const std::string& body = it->str(1);
        if (!std::regex_search(body, audio_re)) continue;
        std::smatch idx_m;
        if (!std::regex_search(body, idx_m, idx_re)) continue;
        PortRole role;
        role.index = std::stoi(idx_m[1]);
        role.is_audio = true;
        role.is_input = std::regex_search(body, in_re);
        // Note: a port can be both lv2:InputPort and lv2:OutputPort in TTL
        // only in pathological cases; otherwise treat !input as output.
        out.push_back(role);
    }
    return out;
}

// Find a .so file inside an LV2 bundle. If multiple .so files exist we pick
// the first; proper handling would require TTL's lv2:binary clause.
std::string resolve_lv2_binary(const std::string& bundle_dir) {
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(bundle_dir, ec)) {
        auto ext = entry.path().extension().string();
        if (ext == ".so" || ext == ".dylib") return entry.path().string();
    }
    return {};
}

class Lv2Slot final : public PluginSlot {
public:
    Lv2Slot(PluginInfo info, void* handle, const LV2_Descriptor* desc,
            std::vector<PortRole> port_roles)
        : info_(std::move(info)),
          handle_(handle),
          desc_(desc),
          port_roles_(std::move(port_roles)) {
        for (auto& r : port_roles_) {
            if (r.is_audio && r.is_input) num_audio_inputs_++;
            else if (r.is_audio && !r.is_input) num_audio_outputs_++;
        }
    }

    ~Lv2Slot() override {
        release();
        if (instance_ && desc_ && desc_->cleanup) {
            desc_->cleanup(instance_);
            instance_ = nullptr;
        }
        if (handle_) {
            dlclose(handle_);
            handle_ = nullptr;
        }
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return desc_ != nullptr; }

    bool prepare(double sample_rate, int max_block_size) override {
        if (!desc_) return false;
        if (instance_ && desc_->cleanup) { desc_->cleanup(instance_); instance_ = nullptr; }
        instance_ = desc_->instantiate(desc_, sample_rate,
                                       info_.path.c_str(), nullptr);
        if (!instance_) {
            runtime::log_error("LV2Slot: instantiate failed for '{}'", info_.name);
            return false;
        }
        max_block_size_ = max_block_size;
        scratch_in_.assign((size_t)num_audio_inputs_ * (size_t)max_block_size, 0.f);
        scratch_out_.assign((size_t)num_audio_outputs_ * (size_t)max_block_size, 0.f);
        if (desc_->activate) desc_->activate(instance_);
        active_ = true;
        return true;
    }

    void release() override {
        if (!instance_) return;
        if (active_ && desc_->deactivate) desc_->deactivate(instance_);
        active_ = false;
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 const midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ParameterEventQueue& /*param_events*/,
                 int num_samples) override {
        if (!instance_ || !active_ || bypassed_.load(std::memory_order_relaxed)) {
            for (size_t c = 0; c < output.num_channels(); ++c) {
                auto* dst = output.channel_ptr(c);
                if (c < input.num_channels()) {
                    std::memcpy(dst, input.channel_ptr(c), sizeof(float) * (size_t)num_samples);
                } else {
                    std::memset(dst, 0, sizeof(float) * (size_t)num_samples);
                }
            }
            return;
        }

        // Stage inputs into our scratch buffer so we can connect_port with a
        // stable pointer per port. For ports we don't have audio data for,
        // we leave the scratch at its current contents (caller may be
        // running a generator; still fine to feed silence).
        int ai = 0;
        int ao = 0;
        for (auto& r : port_roles_) {
            if (!r.is_audio) continue;
            if (r.is_input) {
                float* p = scratch_in_.data() + (size_t)ai * (size_t)max_block_size_;
                if ((size_t)ai < input.num_channels()) {
                    std::memcpy(p, input.channel_ptr((size_t)ai),
                                sizeof(float) * (size_t)num_samples);
                } else {
                    std::memset(p, 0, sizeof(float) * (size_t)num_samples);
                }
                desc_->connect_port(instance_, (uint32_t)r.index, p);
                ++ai;
            } else {
                float* p = scratch_out_.data() + (size_t)ao * (size_t)max_block_size_;
                desc_->connect_port(instance_, (uint32_t)r.index, p);
                ++ao;
            }
        }
        desc_->run(instance_, (uint32_t)num_samples);
        // Copy outputs back into the host's BufferView.
        ao = 0;
        for (auto& r : port_roles_) {
            if (!r.is_audio || r.is_input) continue;
            if ((size_t)ao < output.num_channels()) {
                std::memcpy(output.channel_ptr((size_t)ao),
                            scratch_out_.data() + (size_t)ao * (size_t)max_block_size_,
                            sizeof(float) * (size_t)num_samples);
            }
            ++ao;
        }
    }

    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}

    void set_bypass(bool b) override { bypassed_.store(b, std::memory_order_relaxed); }
    bool is_bypassed() const override { return bypassed_.load(std::memory_order_relaxed); }

    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }

    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

private:
    PluginInfo info_;
    void* handle_ = nullptr;
    const LV2_Descriptor* desc_ = nullptr;
    LV2_Handle instance_ = nullptr;
    std::vector<PortRole> port_roles_;
    int num_audio_inputs_ = 0;
    int num_audio_outputs_ = 0;
    int max_block_size_ = 0;
    std::vector<float> scratch_in_;
    std::vector<float> scratch_out_;
    std::atomic<bool> bypassed_{false};
    bool active_ = false;
};

} // namespace

std::unique_ptr<PluginSlot> load_lv2_plugin(const PluginInfo& info) {
    std::error_code ec;
    if (!fs::is_directory(info.path, ec)) {
        runtime::log_error("LV2 load: path is not a bundle directory: '{}'", info.path);
        return nullptr;
    }
    std::string bin = resolve_lv2_binary(info.path);
    if (bin.empty()) {
        runtime::log_error("LV2 load: no .so/.dylib found in bundle '{}'", info.path);
        return nullptr;
    }
    void* handle = dlopen(bin.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        const char* err = dlerror();
        runtime::log_error("LV2 load: dlopen failed for '{}': {}", bin, err ? err : "unknown");
        return nullptr;
    }
    using DescriptorFn = const LV2_Descriptor* (*)(uint32_t);
    auto* get_desc = reinterpret_cast<DescriptorFn>(dlsym(handle, "lv2_descriptor"));
    if (!get_desc) {
        runtime::log_error("LV2 load: lv2_descriptor missing in '{}'", bin);
        dlclose(handle);
        return nullptr;
    }

    const LV2_Descriptor* chosen = nullptr;
    const char* wanted_uri = info.unique_id.empty() ? nullptr : info.unique_id.c_str();
    for (uint32_t i = 0; ; ++i) {
        const LV2_Descriptor* d = get_desc(i);
        if (!d) break;
        if (!wanted_uri) { chosen = d; break; }
        if (d->URI && std::strcmp(d->URI, wanted_uri) == 0) { chosen = d; break; }
    }
    if (!chosen) {
        runtime::log_error("LV2 load: no matching descriptor in '{}'", bin);
        dlclose(handle);
        return nullptr;
    }

    auto roles = discover_audio_ports(info.path);
    if (roles.empty()) {
        runtime::log_warn("LV2 load: no audio ports found in bundle TTLs for '{}'; "
                          "plugin will load but process() will be a pass-through", info.path);
    }

    PluginInfo filled = info;
    if (filled.unique_id.empty() && chosen->URI) filled.unique_id = chosen->URI;
    if (filled.name.empty()) filled.name = fs::path(info.path).stem().string();

    return std::make_unique<Lv2Slot>(std::move(filled), handle, chosen, std::move(roles));
}

} // namespace pulp::host
