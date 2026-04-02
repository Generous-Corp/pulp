#pragma once

// FaustProcessor — bridges FAUST-generated C++ DSP classes into Pulp's Processor model.
//
// FAUST offline codegen produces a class derived from ::dsp with:
//   - getNumInputs() / getNumOutputs()
//   - buildUserInterface(UI*) — declares parameters via UI callbacks
//   - compute(int count, float** inputs, float** outputs)
//   - metadata(Meta*) — key/value metadata pairs
//
// FaustProcessor acts as both the FAUST UI builder (to reflect parameters)
// and the Pulp Processor (to map into StateStore and process audio).

#include <pulp/dsl/dsl_processor.hpp>
#include <pulp/dsl/faust_base.hpp>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pulp::dsl {

// Minimal FAUST UI interface for parameter reflection.
// FaustProcessor implements this to discover parameters from buildUserInterface().
class PulpFaustUI : public UI {
public:
    struct Zone {
        FAUSTFLOAT* zone_ptr;       // Pointer into FAUST DSP's float field
        DslParamDescriptor desc;
        state::ParamID param_id;    // Assigned during define_parameters()
    };

    // UI interface (called by FAUST-generated buildUserInterface)
    void openTabBox(const char* label) override { push_group(label); }
    void openHorizontalBox(const char* label) override { push_group(label); }
    void openVerticalBox(const char* label) override { push_group(label); }
    void closeBox() override { pop_group(); }

    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone,
                             FAUSTFLOAT init, FAUSTFLOAT min,
                             FAUSTFLOAT max, FAUSTFLOAT step) override {
        add_zone(label, zone, init, min, max, step);
    }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone,
                           FAUSTFLOAT init, FAUSTFLOAT min,
                           FAUSTFLOAT max, FAUSTFLOAT step) override {
        add_zone(label, zone, init, min, max, step);
    }
    void addNumEntry(const char* label, FAUSTFLOAT* zone,
                     FAUSTFLOAT init, FAUSTFLOAT min,
                     FAUSTFLOAT max, FAUSTFLOAT step) override {
        add_zone(label, zone, init, min, max, step);
    }
    void addButton(const char* label, FAUSTFLOAT* zone) override {
        add_zone(label, zone, 0.0f, 0.0f, 1.0f, 1.0f);
    }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override {
        add_zone(label, zone, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    // Passive outputs (bargraphs) — read-only, not mapped to params
    void addHorizontalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addVerticalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}

    // Metadata for current zone
    void declare(FAUSTFLOAT* zone, const char* key, const char* value) override {
        if (zone) {
            zone_metadata_[zone][key] = value;
        }
    }

    const std::vector<Zone>& zones() const { return zones_; }

private:
    std::vector<Zone> zones_;
    std::vector<std::string> group_stack_;
    std::map<FAUSTFLOAT*, std::map<std::string, std::string>> zone_metadata_;

    void push_group(const char* label) {
        if (label && label[0]) {
            group_stack_.emplace_back(label);
        }
    }

    void pop_group() {
        if (!group_stack_.empty()) {
            group_stack_.pop_back();
        }
    }

    std::string current_group() const {
        std::string result;
        for (const auto& g : group_stack_) {
            if (!result.empty()) result += "/";
            result += g;
        }
        return result;
    }

    void add_zone(const char* label, FAUSTFLOAT* zone,
                  float init, float min, float max, float step) {
        Zone z;
        z.zone_ptr = zone;
        z.desc.name = label ? label : "";
        z.desc.group = current_group();
        z.desc.min = min;
        z.desc.max = max;
        z.desc.default_value = init;
        z.desc.step = step;
        z.param_id = 0;  // Assigned later

        // Check for unit metadata
        auto it = zone_metadata_.find(zone);
        if (it != zone_metadata_.end()) {
            auto uit = it->second.find("unit");
            if (uit != it->second.end()) {
                z.desc.unit = uit->second;
            }
        }

        zones_.push_back(std::move(z));
    }
};

// Minimal FAUST Meta interface for metadata extraction
class PulpFaustMeta : public Meta {
public:
    void declare(const char* key, const char* value) override {
        if (key && value) {
            metadata_[key] = value;
        }
    }

    const std::string& get(const std::string& key, const std::string& fallback = "") const {
        auto it = metadata_.find(key);
        return it != metadata_.end() ? it->second : fallback;
    }

    const std::map<std::string, std::string>& all() const { return metadata_; }

private:
    std::map<std::string, std::string> metadata_;
};

// Template wrapper: T must be a FAUST-generated class derived from ::dsp.
// This is the concrete FaustProcessor that ships as a Pulp Processor.
template <typename FaustDsp>
class FaustProcessor : public DslProcessor {
public:
    FaustProcessor() {
        dsl_name_ = "faust";
        faust_dsp_ = std::make_unique<FaustDsp>();

        // Extract metadata
        faust_dsp_->metadata(&meta_);

        // Reflect parameters via UI
        faust_dsp_->buildUserInterface(&ui_);

        // Populate DSL param descriptors
        for (const auto& zone : ui_.zones()) {
            dsl_params_.push_back(zone.desc);
        }

        // Extract bus layout
        bus_layout_.num_inputs = faust_dsp_->getNumInputs();
        bus_layout_.num_outputs = faust_dsp_->getNumOutputs();
        bus_layout_.accepts_midi = false; // Basic FAUST — no MIDI mapping yet
    }

    // --- Processor interface ---

    format::PluginDescriptor descriptor() const override {
        std::string name = meta_.get("name", "FAUST Plugin");
        std::string author = meta_.get("author", "FAUST");

        format::PluginDescriptor desc;
        desc.name = name;
        desc.manufacturer = author;
        desc.bundle_id = "com.pulp.faust." + sanitize_id(name);
        desc.version = meta_.get("version", "1.0.0");
        desc.category = (bus_layout_.num_inputs == 0)
                             ? format::PluginCategory::Instrument
                             : format::PluginCategory::Effect;

        if (bus_layout_.num_inputs > 0) {
            desc.input_buses = {{"Audio In", bus_layout_.num_inputs}};
        }
        desc.output_buses = {{"Audio Out", bus_layout_.num_outputs}};
        desc.accepts_midi = bus_layout_.accepts_midi;
        desc.produces_midi = false;
        desc.tail_samples = 0;

        return desc;
    }

    void define_parameters(state::StateStore& store) override {
        auto& zones = const_cast<std::vector<PulpFaustUI::Zone>&>(ui_.zones());

        // Build groups from unique group paths
        std::map<std::string, int> group_ids;
        int next_group_id = 1;

        for (auto& zone : zones) {
            if (!zone.desc.group.empty() && group_ids.find(zone.desc.group) == group_ids.end()) {
                group_ids[zone.desc.group] = next_group_id;
                store.add_group({next_group_id, zone.desc.group, 0});
                next_group_id++;
            }
        }

        // Register parameters with sequential IDs starting at 1
        state::ParamID next_id = 1;
        for (auto& zone : zones) {
            state::ParamInfo info;
            info.id = next_id;
            info.name = zone.desc.name;
            info.unit = zone.desc.unit;
            info.range = {zone.desc.min, zone.desc.max, zone.desc.default_value, zone.desc.step};

            if (!zone.desc.group.empty()) {
                auto it = group_ids.find(zone.desc.group);
                if (it != group_ids.end()) {
                    info.group_id = it->second;
                }
            }

            store.add_parameter(info);
            zone.param_id = next_id;
            next_id++;
        }
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        max_block_size_ = ctx.max_buffer_size;
        faust_dsp_->init(static_cast<int>(sample_rate_));
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>& input,
        midi::MidiBuffer&,
        midi::MidiBuffer&,
        const format::ProcessContext& ctx) override
    {
        // Sync StateStore → FAUST zone pointers
        for (const auto& zone : ui_.zones()) {
            *zone.zone_ptr = state().get_value(zone.param_id);
        }

        int n = ctx.num_samples;
        if (n <= 0) n = static_cast<int>(output.num_samples());

        // Build raw pointer arrays for FAUST compute()
        const int num_in = bus_layout_.num_inputs;
        const int num_out = bus_layout_.num_outputs;

        // FAUST expects float** — build arrays on stack for small channel counts
        std::vector<float*> in_ptrs(num_in);
        std::vector<float*> out_ptrs(num_out);

        for (int ch = 0; ch < num_in; ++ch) {
            in_ptrs[ch] = const_cast<float*>(input.channel_ptr(static_cast<std::size_t>(ch)));
        }
        for (int ch = 0; ch < num_out; ++ch) {
            out_ptrs[ch] = output.channel_ptr(static_cast<std::size_t>(ch));
        }

        faust_dsp_->compute(n, in_ptrs.data(), out_ptrs.data());
    }

    void release() override {
        // FAUST DSP doesn't have a release, but we could instanceClear if needed
    }

    bool has_editor() const override { return false; } // No custom UI for FAUST plugins

private:
    std::unique_ptr<FaustDsp> faust_dsp_;
    PulpFaustUI ui_;
    PulpFaustMeta meta_;
    double sample_rate_ = 48000.0;
    int max_block_size_ = 512;

    static std::string sanitize_id(const std::string& name) {
        std::string result;
        for (char c : name) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else if (c == ' ' || c == '-' || c == '_') {
                if (!result.empty() && result.back() != '-') {
                    result += '-';
                }
            }
        }
        return result;
    }
};

} // namespace pulp::dsl
