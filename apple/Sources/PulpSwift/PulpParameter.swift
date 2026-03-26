// PulpParameter.swift — Swift wrapper for Pulp parameter system
// Provides ObservableObject bindings for SwiftUI integration

import Foundation
import Combine

/// A single plugin parameter observable by SwiftUI views.
public final class PulpParameter: ObservableObject, Identifiable {
    public let id: UInt32
    public let name: String
    public let unit: String
    public let minValue: Float
    public let maxValue: Float
    public let defaultValue: Float
    public let step: Float

    @Published public var value: Float {
        didSet {
            pulp_param_set(id, value)
        }
    }

    @Published public var normalizedValue: Float {
        didSet {
            pulp_param_set_normalized(id, normalizedValue)
        }
    }

    init(info: PulpParamInfo) {
        self.id = info.id
        self.name = String(cString: info.name)
        self.unit = String(cString: info.unit)
        self.minValue = info.min_value
        self.maxValue = info.max_value
        self.defaultValue = info.default_value
        self.step = info.step
        self.value = pulp_param_get(info.id)
        self.normalizedValue = pulp_param_get_normalized(info.id)
    }

    /// Begin a gesture (for undo grouping in the host).
    public func beginGesture() {
        pulp_param_begin_gesture(id)
    }

    /// End a gesture.
    public func endGesture() {
        pulp_param_end_gesture(id)
    }

    /// Reset to default value.
    public func reset() {
        pulp_param_reset(id)
        value = pulp_param_get(id)
        normalizedValue = pulp_param_get_normalized(id)
    }

    /// Sync from the C++ store (call periodically to detect host automation).
    public func poll() {
        let current = pulp_param_get(id)
        if abs(current - value) > 1e-7 {
            value = current
            normalizedValue = pulp_param_get_normalized(id)
        }
    }

    /// Formatted display string.
    public var displayString: String {
        if step >= 1.0 {
            return "\(Int(value))\(unit.isEmpty ? "" : " \(unit)")"
        } else {
            return String(format: "%.1f%@", value, unit.isEmpty ? "" : " \(unit)")
        }
    }
}


/// Observable store of all plugin parameters for SwiftUI.
public final class PulpParameterStore: ObservableObject {
    @Published public var parameters: [PulpParameter] = []

    public init() {
        reload()
    }

    /// Reload parameters from the C++ StateStore.
    public func reload() {
        let count = pulp_param_count()
        var params: [PulpParameter] = []
        for i in 0..<count {
            var info = PulpParamInfo()
            if pulp_param_info(Int32(i), &info) {
                params.append(PulpParameter(info: info))
            }
        }
        parameters = params
    }

    /// Poll all parameters for external changes (host automation).
    public func pollAll() {
        for param in parameters {
            param.poll()
        }
    }

    /// Get a parameter by ID.
    public func parameter(id: UInt32) -> PulpParameter? {
        parameters.first { $0.id == id }
    }
}
