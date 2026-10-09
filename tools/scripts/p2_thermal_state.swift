// Emit one bounded, current Apple thermal-state observation for the P2 host
// preflight producer.  The Python consumer records this source verbatim and
// binds the observation to this checked-in source and the Swift tool digest.
// Foundation is intentionally the only API used here: pmset warning history
// is retained by the consumer as diagnostics, but is not current thermal
// state evidence.
import Foundation

let state = ProcessInfo.processInfo.thermalState
let stateName: String
switch state {
case .nominal:
    stateName = "nominal"
case .fair:
    stateName = "fair"
case .serious:
    stateName = "serious"
case .critical:
    stateName = "critical"
@unknown default:
    stateName = "unknown"
}

let formatter = ISO8601DateFormatter()
formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
let value: [String: Any] = [
    "schema": "pulp.gpu-audio.p2.thermal-observation.v1",
    "source": "Foundation.ProcessInfo.thermalState",
    "thermal_state": stateName,
    "thermal_state_code": state.rawValue,
    "sampled_at": formatter.string(from: Date()),
]

guard JSONSerialization.isValidJSONObject(value),
      let data = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]),
      let output = String(data: data, encoding: .utf8) else {
    FileHandle.standardError.write(Data("thermal observation serialization failed\n".utf8))
    exit(2)
}
print(output)
