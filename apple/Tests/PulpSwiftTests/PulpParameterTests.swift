import SwiftUI
import XCTest
@testable import PulpSwift

final class PulpParameterTests: XCTestCase {
    private var bridge: MockBridge!

    override func tearDown() {
        PulpBridgeRuntime.installTestBackend(nil)
        bridge = nil
        super.tearDown()
    }

    func testStoreIsEmptyWithoutInstalledBackend() {
        let store = PulpParameterStore()

        XCTAssertTrue(store.parameters.isEmpty)
        XCTAssertNil(store.parameter(id: 1))
    }

    func testReloadBuildsParametersAndLookupWorks() {
        installBridge(parameters: [
            .init(id: 1, name: "Gain", unit: "dB", minValue: -24, maxValue: 24, defaultValue: 0, step: 0.1),
            .init(id: 2, name: "Bypass", unit: "", minValue: 0, maxValue: 1, defaultValue: 0, step: 1),
        ])

        let store = PulpParameterStore()

        XCTAssertEqual(store.parameters.map(\.name), ["Gain", "Bypass"])
        XCTAssertEqual(store.parameter(id: 2)?.name, "Bypass")
    }

    func testDisplayStringFormatsFractionalAndIntegralParameters() {
        installBridge(parameters: [
            .init(id: 1, name: "Gain", unit: "dB", minValue: -24, maxValue: 24, defaultValue: 0, step: 0.1),
            .init(id: 2, name: "Voices", unit: "", minValue: 1, maxValue: 16, defaultValue: 8, step: 1),
        ])
        bridge.values = [1: 6.25, 2: 4]

        let store = PulpParameterStore()

        XCTAssertEqual(store.parameters[0].displayString, "6.2 dB")
        XCTAssertEqual(store.parameters[1].displayString, "4")
    }

    func testPollRefreshesValuesFromBridge() {
        installBridge(parameters: [
            .init(id: 1, name: "Mix", unit: "%", minValue: 0, maxValue: 100, defaultValue: 50, step: 0.1),
        ])
        let store = PulpParameterStore()
        bridge.values[1] = 72
        bridge.normalizedValues[1] = 0.72

        store.parameters[0].poll()

        XCTAssertEqual(store.parameters[0].value, 72, accuracy: 0.001)
        XCTAssertEqual(store.parameters[0].normalizedValue, 0.72, accuracy: 0.001)
        XCTAssertEqual(bridge.setCalls.map(\.id), [1])
        XCTAssertEqual(bridge.normalizedSetCalls.map(\.id), [1])
    }

    func testPollAllRefreshesEveryParameter() {
        installBridge(parameters: [
            .init(id: 1, name: "Drive", unit: "", minValue: 0, maxValue: 10, defaultValue: 5, step: 0.1),
            .init(id: 2, name: "Tone", unit: "", minValue: 0, maxValue: 1, defaultValue: 0.5, step: 0.1),
        ])
        let store = PulpParameterStore()
        bridge.values[1] = 7
        bridge.values[2] = 0.2
        bridge.normalizedValues[1] = 0.7
        bridge.normalizedValues[2] = 0.2

        store.pollAll()

        XCTAssertEqual(store.parameters[0].value, 7, accuracy: 0.001)
        XCTAssertEqual(store.parameters[1].value, 0.2, accuracy: 0.001)
    }

    func testResetAndGesturesForwardToBridge() {
        installBridge(parameters: [
            .init(id: 7, name: "Feedback", unit: "%", minValue: 0, maxValue: 100, defaultValue: 25, step: 0.1),
        ])
        let parameter = PulpParameterStore().parameters[0]
        bridge.values[7] = 25
        bridge.normalizedValues[7] = 0.25

        parameter.beginGesture()
        parameter.endGesture()
        parameter.reset()

        XCTAssertEqual(bridge.gestureBegan, [7])
        XCTAssertEqual(bridge.gestureEnded, [7])
        XCTAssertEqual(bridge.resets, [7])
        XCTAssertEqual(parameter.value, 25, accuracy: 0.001)
        XCTAssertEqual(parameter.normalizedValue, 0.25, accuracy: 0.001)
    }

    func testViewBodiesBuildFromParameterStore() {
        installBridge(parameters: [
            .init(id: 1, name: "Mix", unit: "%", minValue: 0, maxValue: 100, defaultValue: 50, step: 0.1),
            .init(id: 2, name: "Bypass", unit: "", minValue: 0, maxValue: 1, defaultValue: 0, step: 1),
        ])
        let store = PulpParameterStore()

        _ = PulpKnob(parameter: store.parameters[0]).body
        _ = PulpSlider(parameter: store.parameters[0]).body
        _ = PulpToggle(parameter: store.parameters[1]).body
        _ = PulpAutoUI(store: store).body
        _ = PulpEditorView(pluginName: "Test") { Text("Hello") }.body
    }

    private func installBridge(parameters: [PulpBridgeParameterInfo]) {
        bridge = MockBridge(parameters: parameters)
        PulpBridgeRuntime.installTestBackend(bridge.backend)
    }
}

private final class MockBridge {
    var parameters: [PulpBridgeParameterInfo]
    var values: [PulpParamID: Float]
    var normalizedValues: [PulpParamID: Float]
    var setCalls: [(id: PulpParamID, value: Float)] = []
    var normalizedSetCalls: [(id: PulpParamID, value: Float)] = []
    var gestureBegan: [PulpParamID] = []
    var gestureEnded: [PulpParamID] = []
    var resets: [PulpParamID] = []

    init(parameters: [PulpBridgeParameterInfo]) {
        self.parameters = parameters
        self.values = Dictionary(
            uniqueKeysWithValues: parameters.map { ($0.id, $0.defaultValue) })
        let normalizedPairs = parameters.map { parameter in
            let span = parameter.maxValue - parameter.minValue
            let normalized = span == 0 ? 0 : (parameter.defaultValue - parameter.minValue) / span
            return (parameter.id, normalized)
        }
        self.normalizedValues = Dictionary(uniqueKeysWithValues: normalizedPairs)
    }

    var backend: PulpBridgeBackend {
        PulpBridgeBackend(
            parameters: { self.parameters },
            getValue: { self.values[$0] ?? 0 },
            setValue: { id, value in
                self.values[id] = value
                self.setCalls.append((id, value))
            },
            getNormalizedValue: { self.normalizedValues[$0] ?? 0 },
            setNormalizedValue: { id, value in
                self.normalizedValues[id] = value
                self.normalizedSetCalls.append((id, value))
            },
            beginGesture: { self.gestureBegan.append($0) },
            endGesture: { self.gestureEnded.append($0) },
            resetToDefault: { id in
                self.resets.append(id)
                guard let parameter = self.parameters.first(where: { $0.id == id }) else {
                    return
                }
                self.values[id] = parameter.defaultValue
                let span = parameter.maxValue - parameter.minValue
                self.normalizedValues[id] =
                    span == 0 ? 0 : (parameter.defaultValue - parameter.minValue) / span
            }
        )
    }
}
