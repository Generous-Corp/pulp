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
        self.name = String(cString: info.name ?? pulpEmptyCString)
        self.unit = String(cString: info.unit ?? pulpEmptyCString)
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

// MARK: - Generated-view parameter resolution

/// How an exact-name parameter lookup resolves. There is no stable string
/// param key in Pulp's state model today (StateStore indexes by numeric ID,
/// `ParamInfo` carries only a display `name`), so imported SwiftUI views
/// resolve a generated binding key by exact `PulpParameter.name` match. The
/// `missing` / `duplicate` cases are surfaced (never silently bound to the
/// first match) so a renamed or ambiguous parameter is visible, not wrong.
public enum PulpParameterResolution {
    case resolved(PulpParameter)
    case missing(name: String)
    case duplicate(name: String, count: Int)
}

/// The match outcome independent of any concrete `PulpParameter`. Split out so
/// the exact-name rule is unit-testable without the C bridge that backs
/// `PulpParameter` construction.
public enum PulpParameterMatch: Equatable {
    case resolved
    case missing
    case duplicate(Int)
}

/// Pure exact-name classifier: how a lookup of `name` resolves over
/// `candidateNames`. Case-sensitive, no trimming, no normalization.
public func pulpClassifyParameterName(_ name: String,
                                      in candidateNames: [String]) -> PulpParameterMatch {
    let count = candidateNames.reduce(0) { $0 + ($1 == name ? 1 : 0) }
    switch count {
    case 0:  return .missing
    case 1:  return .resolved
    default: return .duplicate(count)
    }
}

/// The small shared protocol imported SwiftUI views depend on for binding, so
/// generated code and `PulpViews.swift` don't drift on resolution semantics.
public protocol PulpParameterResolving: AnyObject {
    func resolveParameter(named name: String) -> PulpParameterResolution
}

extension PulpParameterStore: PulpParameterResolving {
    /// Resolve by exact `PulpParameter.name`. 0 matches → `.missing`,
    /// exactly 1 → `.resolved`, more than 1 → `.duplicate` (never the first).
    public func resolveParameter(named name: String) -> PulpParameterResolution {
        let matches = parameters.filter { $0.name == name }
        switch matches.count {
        case 0:  return .missing(name: name)
        case 1:  return .resolved(matches[0])
        default: return .duplicate(name: name, count: matches.count)
        }
    }
}
