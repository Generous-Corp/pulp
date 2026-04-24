#include <Python.h>

extern "C" PyObject* PyInit_pulp();

int main() {
    if (PyImport_AppendInittab("pulp", &PyInit_pulp) == -1) {
        return 1;
    }

    Py_Initialize();

    const char* script = R"PY(
import math
import pulp

param_range = pulp.ParamRange(0.0, 1.0, 0.5)
assert math.isclose(param_range.normalize(0.5), 0.5)
assert math.isclose(param_range.denormalize(0.25), 0.25)

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
assert math.isclose(info.range.default_value, 0.5)

midi = pulp.MidiBuffer()
assert midi.empty()
note_on = pulp.MidiEvent.note_on(1, 60, 100)
note_on.sample_offset = 3
midi.add(note_on)
midi.add(pulp.MidiEvent.note_off(1, 60))
assert midi.size() == 2
assert not midi.empty()
midi.clear()
assert midi.empty()
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
