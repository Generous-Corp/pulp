#include <Python.h>

#include <pybind11/pybind11.h>

#include <pulp/format/headless.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

extern "C" PyObject* PyInit_pulp();

namespace {

namespace py = pybind11;

constexpr pulp::state::ParamID kHostParamId = 8101;
constexpr pulp::state::ParamID kStoreGainParamId = 8201;
constexpr pulp::state::ParamID kStoreMixParamId = 8202;

class BindingsSmokeProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "BindingsSmoke",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.bindings-smoke",
            .version = "1.2.3",
            .category = pulp::format::PluginCategory::Effect,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kHostParamId,
            .name = "Host Gain",
            .unit = "",
            .range = {0.0f, 1.0f, 0.25f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>& input,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {
        const auto channels = std::min(output.num_channels(), input.num_channels());
        const auto samples = std::min(output.num_samples(), input.num_samples());
        for (std::size_t ch = 0; ch < channels; ++ch) {
            auto out = output.channel(ch);
            auto in = input.channel(ch);
            std::copy_n(in.data(), samples, out.data());
        }
    }

    std::vector<uint8_t> serialize_plugin_state() const override {
        return {0x70, 0x79};
    }
};

std::unique_ptr<pulp::format::Processor> create_bindings_smoke_processor() {
    return std::make_unique<BindingsSmokeProcessor>();
}

void add_param(pulp::state::StateStore& store,
               pulp::state::ParamID id,
               const char* name,
               pulp::state::ParamRange range)
{
    store.add_parameter({
        .id = id,
        .name = name,
        .unit = "",
        .range = range,
    });
}

} // namespace

int main() {
    if (PyImport_AppendInittab("pulp", &PyInit_pulp) == -1) {
        return 1;
    }

    pulp::state::StateStore test_store;
    add_param(test_store, kStoreGainParamId, "Python Gain", {-60.0f, 24.0f, 0.0f});
    add_param(test_store, kStoreMixParamId, "Python Mix", {0.0f, 1.0f, 0.5f});

    pulp::format::HeadlessHost test_host(create_bindings_smoke_processor);

    Py_Initialize();

    try {
        py::module_ pulp_module = py::module_::import("pulp");
        pulp_module.attr("_test_state_store") =
            py::cast(&test_store, py::return_value_policy::reference);
        pulp_module.attr("_test_host") =
            py::cast(&test_host, py::return_value_policy::reference);
    } catch (const py::error_already_set&) {
        PyErr_Print();
        return 1;
    }

    const char* script = R"PY(
import math
import pulp

assert pulp.__doc__ == "Pulp audio plugin framework — Python bindings"

default_range = pulp.ParamRange()
assert math.isclose(default_range.min, 0.0)
assert math.isclose(default_range.max, 1.0)
assert math.isclose(default_range.default_value, 0.0)
assert math.isclose(default_range.normalize(-10.0), 0.0)
assert math.isclose(default_range.normalize(10.0), 1.0)
assert math.isclose(default_range.denormalize(-1.0), 0.0)
assert math.isclose(default_range.denormalize(2.0), 1.0)

param_range = pulp.ParamRange(0.0, 1.0, 0.5)
assert math.isclose(param_range.normalize(0.5), 0.5)
assert math.isclose(param_range.denormalize(0.25), 0.25)
param_range.min = -60.0
param_range.max = 24.0
param_range.default_value = -12.0
assert math.isclose(param_range.normalize(-60.0), 0.0)
assert math.isclose(param_range.normalize(24.0), 1.0)
assert math.isclose(param_range.denormalize(0.5), -18.0)
assert math.isclose(param_range.default_value, -12.0)

equal_range = pulp.ParamRange(3.0, 3.0, 3.0)
assert math.isclose(equal_range.normalize(3.0), 0.0)
assert math.isclose(equal_range.denormalize(0.75), 3.0)

info = pulp.ParamInfo()
info.id = 101
info.name = "Gain"
info.unit = "dB"
info.range = param_range
info.group_id = 7
assert info.id == 101
assert info.name == "Gain"
assert info.unit == "dB"
assert info.group_id == 7
assert math.isclose(info.range.min, -60.0)
assert math.isclose(info.range.max, 24.0)
assert math.isclose(info.range.default_value, -12.0)
info.range = pulp.ParamRange(-1.0, 1.0, 0.0)
assert math.isclose(info.range.denormalize(0.75), 0.5)
info.range.default_value = 0.25
assert math.isclose(info.range.default_value, 0.25)

midi = pulp.MidiBuffer()
assert midi.empty()
assert midi.size() == 0
midi.clear()
assert midi.empty()
note_on = pulp.MidiEvent.note_on(1, 60, 100)
assert note_on.sample_offset == 0
note_on.sample_offset = 3
midi.add(note_on)
assert midi.size() == 1
assert not midi.empty()
note_off_default_velocity = pulp.MidiEvent.note_off(channel=1, note=60)
note_off_default_velocity.sample_offset = 4
midi.add(note_off_default_velocity)
midi.add(pulp.MidiEvent.note_off(1, 60, 64))
midi.add(pulp.MidiEvent.cc(2, 74, 127))
midi.add(pulp.MidiEvent.pitch_bend(3, 8192))
assert midi.size() == 5
midi.clear()
assert midi.empty()
assert midi.size() == 0
midi.add(pulp.MidiEvent.cc(15, 1, 0))
assert midi.size() == 1
midi.clear()
midi.clear()
assert midi.empty()
try:
    pulp.MidiEvent.note_on(1, 60)
except TypeError:
    pass
else:
    raise AssertionError("note_on should require an explicit velocity")

store = pulp._test_state_store
assert store.param_count() == 2
assert [param.name for param in store.all_params()] == ["Python Gain", "Python Mix"]
assert store.info(8201).name == "Python Gain"
assert store.info(999999) is None
store.set_value(8201, -12.0)
store.set_normalized(8202, 0.75)
assert math.isclose(store.get_value(8201), -12.0)
assert math.isclose(store.get_normalized(8202), 0.75)
state_blob = bytes(store.serialize())
store.set_value(8201, 999.0)
store.reset_to_default(8202)
assert math.isclose(store.get_value(8201), 24.0)
assert math.isclose(store.get_value(8202), 0.5)
assert store.deserialize(state_blob)
assert math.isclose(store.get_value(8201), -12.0)
assert math.isclose(store.get_normalized(8202), 0.75)
assert not store.deserialize(b"bad")

host = pulp._test_host
host.prepare(48000.0, 16)
descriptor = host.descriptor()
assert descriptor.name == "BindingsSmoke"
assert descriptor.manufacturer == "PulpTest"
assert descriptor.bundle_id == "com.pulp.test.bindings-smoke"
assert descriptor.version == "1.2.3"
assert descriptor.is_instrument is False
host.release()
host.release()
host.prepare(48000.0, 16, input_channels=1, output_channels=1)

host_state = host.state()
assert host_state.param_count() == 1
assert math.isclose(host_state.get_default(8101), 0.25)
host_state.set_normalized(8101, 1.0)
saved = host.save_state()
assert isinstance(saved, bytes)
host_state.set_value(8101, 0.0)
assert host.load_state(saved)
assert math.isclose(host_state.get_value(8101), 1.0)
assert not host.load_state(b"not a valid host state")
host.release()
)PY";

    const int rc = PyRun_SimpleString(script);
    if (rc != 0) {
        PyErr_Print();
    }

    if (Py_FinalizeEx() < 0) {
        return 1;
    }

    return rc == 0 ? 0 : 1;
}
