// PulpViews.swift — SwiftUI views for Pulp plugin UIs
// These provide a native Apple UI path as an alternative to the JS/GPU rendering engine.

import SwiftUI

// MARK: - Knob

/// A rotary knob control for continuous parameters.
public struct PulpKnob: View {
    @ObservedObject var parameter: PulpParameter
    var size: CGFloat = 60

    public init(parameter: PulpParameter, size: CGFloat = 60) {
        self.parameter = parameter
        self.size = size
    }

    public var body: some View {
        VStack(spacing: 4) {
            ZStack {
                // Track arc
                Circle()
                    .trim(from: 0.15, to: 0.85)
                    .rotation(.degrees(90))
                    .stroke(Color.gray.opacity(0.3), lineWidth: 3)

                // Value arc
                Circle()
                    .trim(from: 0.15, to: 0.15 + 0.7 * CGFloat(parameter.normalizedValue))
                    .rotation(.degrees(90))
                    .stroke(Color.accentColor, lineWidth: 3)

                // Value text
                Text(parameter.displayString)
                    .font(.system(size: size * 0.18, weight: .medium, design: .monospaced))
                    .foregroundColor(.primary)
            }
            .frame(width: size, height: size)
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        let delta = Float(-value.translation.height / 200)
                        let newNorm = max(0, min(1, parameter.normalizedValue + delta))
                        parameter.beginGesture()
                        parameter.normalizedValue = newNorm
                        parameter.value = parameter.minValue + newNorm * (parameter.maxValue - parameter.minValue)
                    }
                    .onEnded { _ in
                        parameter.endGesture()
                    }
            )

            Text(parameter.name)
                .font(.system(size: 10))
                .foregroundColor(.secondary)
        }
    }
}

// MARK: - Slider

/// A linear slider for continuous parameters.
public struct PulpSlider: View {
    @ObservedObject var parameter: PulpParameter

    public init(parameter: PulpParameter) {
        self.parameter = parameter
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(parameter.name)
                    .font(.caption)
                Spacer()
                Text(parameter.displayString)
                    .font(.caption.monospacedDigit())
                    .foregroundColor(.secondary)
            }
            Slider(
                value: Binding(
                    get: { parameter.normalizedValue },
                    set: { newValue in
                        parameter.normalizedValue = newValue
                        parameter.value = parameter.minValue + newValue * (parameter.maxValue - parameter.minValue)
                    }
                ),
                in: 0...1,
                onEditingChanged: { editing in
                    if editing {
                        parameter.beginGesture()
                    } else {
                        parameter.endGesture()
                    }
                }
            )
        }
    }
}

// MARK: - Toggle

/// A toggle for boolean parameters (step >= 1, range 0-1).
public struct PulpToggle: View {
    @ObservedObject var parameter: PulpParameter

    public init(parameter: PulpParameter) {
        self.parameter = parameter
    }

    public var body: some View {
        Toggle(parameter.name, isOn: Binding(
            get: { parameter.value > 0.5 },
            set: { newValue in
                parameter.beginGesture()
                parameter.value = newValue ? 1.0 : 0.0
                parameter.normalizedValue = newValue ? 1.0 : 0.0
                parameter.endGesture()
            }
        ))
        .font(.caption)
    }
}

// MARK: - Auto UI

/// Automatically generates a SwiftUI view for all parameters in a store.
/// Uses knobs for continuous params and toggles for boolean params.
public struct PulpAutoUI: View {
    @ObservedObject var store: PulpParameterStore

    public init(store: PulpParameterStore) {
        self.store = store
    }

    public var body: some View {
        ScrollView {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 80))], spacing: 16) {
                ForEach(store.parameters) { param in
                    if param.step >= 1.0 && param.minValue == 0 && param.maxValue == 1 {
                        PulpToggle(parameter: param)
                    } else {
                        PulpKnob(parameter: param)
                    }
                }
            }
            .padding()
        }
    }
}

// MARK: - Plugin Editor Container

/// Container view that wraps a plugin's SwiftUI editor with standard chrome.
public struct PulpEditorView<Content: View>: View {
    let pluginName: String
    let content: Content

    public init(pluginName: String, @ViewBuilder content: () -> Content) {
        self.pluginName = pluginName
        self.content = content()
    }

    public var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Text(pluginName)
                    .font(.headline)
                Spacer()
            }
            .padding(.horizontal)
            .padding(.vertical, 8)
            #if os(macOS)
            .background(Color(.windowBackgroundColor).opacity(0.5))
            #else
            .background(Color(.systemBackground).opacity(0.5))
            #endif

            // Content
            content
        }
    }
}
