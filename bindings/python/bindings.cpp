/// @file bindings.cpp
/// Python bindings for Pulp audio plugin framework via pybind11.
///
/// Exposes HeadlessHost, StateStore, BufferView, MidiBuffer, and DSP
/// processors to Python for AI/ML workflows, testing, and automation.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <pybind11/functional.h>

#include <pulp/format/headless.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/buffer.hpp>

namespace py = pybind11;
using namespace pulp;

PYBIND11_MODULE(pulp, m) {
    m.doc() = "Pulp audio plugin framework — Python bindings";

    // ── ParamRange ───────────────────────────────────────────────────────
    py::class_<state::ParamRange>(m, "ParamRange")
        .def(py::init<>())
        .def(py::init<float, float, float>(), py::arg("min"), py::arg("max"), py::arg("default_val"))
        .def_readwrite("min", &state::ParamRange::min)
        .def_readwrite("max", &state::ParamRange::max)
        .def_readwrite("default_value", &state::ParamRange::default_value)
        .def("normalize", &state::ParamRange::normalize)
        .def("denormalize", &state::ParamRange::denormalize);

    // ── ParamInfo ────────────────────────────────────────────────────────
    py::class_<state::ParamInfo>(m, "ParamInfo")
        .def(py::init<>())
        .def_readwrite("id", &state::ParamInfo::id)
        .def_readwrite("name", &state::ParamInfo::name)
        .def_readwrite("unit", &state::ParamInfo::unit)
        .def_readwrite("range", &state::ParamInfo::range)
        .def_readwrite("flags", &state::ParamInfo::flags);

    // ── StateStore ───────────────────────────────────────────────────────
    py::class_<state::StateStore>(m, "StateStore")
        .def("get_value", &state::StateStore::get_value)
        .def("set_value", &state::StateStore::set_value)
        .def("get_normalized", &state::StateStore::get_normalized)
        .def("set_normalized", &state::StateStore::set_normalized)
        .def("get_default", &state::StateStore::get_default)
        .def("reset_to_default", &state::StateStore::reset_to_default)
        .def("reset_all_to_defaults", &state::StateStore::reset_all_to_defaults)
        .def("param_count", &state::StateStore::param_count)
        .def("info", &state::StateStore::info, py::return_value_policy::reference)
        .def("all_params", [](const state::StateStore& s) {
            std::vector<state::ParamInfo> result;
            for (const auto& p : s.all_params()) result.push_back(p);
            return result;
        })
        .def("serialize", &state::StateStore::serialize)
        .def("deserialize", [](state::StateStore& s, py::bytes data) {
            std::string str = data;
            return s.deserialize(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(str.data()), str.size()));
        });

    // ── PluginDescriptor ─────────────────────────────────────────────────
    py::class_<format::PluginDescriptor>(m, "PluginDescriptor")
        .def_readonly("name", &format::PluginDescriptor::name)
        .def_readonly("manufacturer", &format::PluginDescriptor::manufacturer)
        .def_readonly("version", &format::PluginDescriptor::version)
        .def_readonly("bundle_id", &format::PluginDescriptor::bundle_id)
        .def_property_readonly("is_instrument", [](const format::PluginDescriptor& d) {
            return d.category == format::PluginCategory::Instrument;
        });

    // ── MidiEvent ────────────────────────────────────────────────────────
    py::class_<midi::MidiEvent>(m, "MidiEvent")
        .def_readwrite("sample_offset", &midi::MidiEvent::sample_offset)
        .def_static("note_on", &midi::MidiEvent::note_on)
        .def_static("note_off", &midi::MidiEvent::note_off,
            py::arg("channel"), py::arg("note"), py::arg("velocity") = 0)
        .def_static("cc", &midi::MidiEvent::cc)
        .def_static("pitch_bend", &midi::MidiEvent::pitch_bend);

    // ── MidiBuffer ───────────────────────────────────────────────────────
    py::class_<midi::MidiBuffer>(m, "MidiBuffer")
        .def(py::init<>())
        .def("add", &midi::MidiBuffer::add)
        .def("clear", &midi::MidiBuffer::clear)
        .def("size", &midi::MidiBuffer::size)
        .def("empty", &midi::MidiBuffer::empty);

    // ── HeadlessHost ─────────────────────────────────────────────────────
    py::class_<format::HeadlessHost>(m, "HeadlessHost")
        .def("prepare", &format::HeadlessHost::prepare,
            py::arg("sample_rate"), py::arg("max_buffer_size"),
            py::arg("input_channels") = 2, py::arg("output_channels") = 2)
        .def("release", &format::HeadlessHost::release)
        .def("state", static_cast<state::StateStore& (format::HeadlessHost::*)()>(
            &format::HeadlessHost::state), py::return_value_policy::reference)
        .def("descriptor", &format::HeadlessHost::descriptor,
            py::return_value_policy::reference)
        .def("save_state", [](const format::HeadlessHost& h) {
            auto data = h.save_state();
            return py::bytes(reinterpret_cast<const char*>(data.data()), data.size());
        })
        .def("load_state", [](format::HeadlessHost& h, py::bytes data) {
            std::string str = data;
            return h.load_state(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(str.data()), str.size()));
        })

        // NumPy-friendly process method
        .def("process_numpy", [](format::HeadlessHost& host,
                                  py::array_t<float> input) {
            auto buf = input.request();
            if (buf.ndim != 2) throw std::runtime_error("Input must be 2D (channels x frames)");

            int channels = static_cast<int>(buf.shape[0]);
            int frames = static_cast<int>(buf.shape[1]);

            // Create input buffer view
            std::vector<const float*> in_ptrs(static_cast<size_t>(channels));
            for (int ch = 0; ch < channels; ++ch) {
                in_ptrs[static_cast<size_t>(ch)] = static_cast<float*>(buf.ptr) + ch * frames;
            }
            audio::BufferView<const float> in_view(in_ptrs.data(), channels, frames);

            // Create output buffer
            py::array_t<float> output({channels, frames});
            auto out_buf = output.request();
            std::vector<float*> out_ptrs(static_cast<size_t>(channels));
            for (int ch = 0; ch < channels; ++ch) {
                out_ptrs[static_cast<size_t>(ch)] = static_cast<float*>(out_buf.ptr) + ch * frames;
            }
            audio::BufferView<float> out_view(out_ptrs.data(), channels, frames);

            host.process(out_view, in_view);
            return output;
        }, py::arg("input"),
        "Process audio using NumPy arrays. Input shape: (channels, frames). Returns output array.")

        // Process with MIDI
        .def("process_numpy_midi", [](format::HeadlessHost& host,
                                       py::array_t<float> input,
                                       midi::MidiBuffer& midi_in) {
            auto buf = input.request();
            if (buf.ndim != 2) throw std::runtime_error("Input must be 2D (channels x frames)");

            int channels = static_cast<int>(buf.shape[0]);
            int frames = static_cast<int>(buf.shape[1]);

            std::vector<const float*> in_ptrs(static_cast<size_t>(channels));
            for (int ch = 0; ch < channels; ++ch) {
                in_ptrs[static_cast<size_t>(ch)] = static_cast<float*>(buf.ptr) + ch * frames;
            }
            audio::BufferView<const float> in_view(in_ptrs.data(), channels, frames);

            py::array_t<float> output({channels, frames});
            auto out_buf = output.request();
            std::vector<float*> out_ptrs(static_cast<size_t>(channels));
            for (int ch = 0; ch < channels; ++ch) {
                out_ptrs[static_cast<size_t>(ch)] = static_cast<float*>(out_buf.ptr) + ch * frames;
            }
            audio::BufferView<float> out_view(out_ptrs.data(), channels, frames);

            midi::MidiBuffer midi_out;
            host.process(out_view, in_view, midi_in, midi_out);

            return py::make_tuple(output, midi_out);
        }, py::arg("input"), py::arg("midi_in"),
        "Process audio with MIDI. Returns (output_array, midi_out).");
}
