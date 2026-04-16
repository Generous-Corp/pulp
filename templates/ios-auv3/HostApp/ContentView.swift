//
// Minimal SwiftUI host for an AUv3 Pulp plug-in (issue #250).
//
// Loads the plug-in's audio unit via AVAudioUnitComponentManager,
// instantiates an AVAudioEngine with a sampler source → the Pulp AU
// → output. The view exposes a play/stop button and lists the
// plug-in's parameters via the AVAudioUnit's parameterTree so the
// user can verify the extension loads without opening a full DAW.
//
// Plug-in authors can copy this file into HostApp/ and tweak the
// AudioComponentDescription to match their bundle's manifest.
//

import SwiftUI
import AVFoundation
import AudioToolbox
import CoreAudioTypes

#if os(iOS) || os(macOS)

@available(iOS 15.0, macOS 12.0, *)
struct ContentView: View {
    @StateObject private var host = PulpAUv3Host()

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(host.componentName ?? "(no AUv3 found — check Info.plist)")
                .font(.headline)

            HStack(spacing: 12) {
                Button(host.isPlaying ? "Stop" : "Play") {
                    host.toggle()
                }
                .disabled(host.audioUnit == nil)
                .buttonStyle(.borderedProminent)

                if !host.parameters.isEmpty {
                    Text("\(host.parameters.count) parameter(s)")
                        .foregroundStyle(.secondary)
                }
            }

            ForEach(host.parameters, id: \.address) { p in
                HStack {
                    Text(p.displayName).frame(maxWidth: .infinity, alignment: .leading)
                    Slider(
                        value: Binding(
                            get: { Double(p.value) },
                            set: { p.value = AUValue($0) }
                        ),
                        in: Double(p.minValue)...Double(p.maxValue)
                    )
                    Text(String(format: "%.2f", p.value))
                        .frame(width: 56, alignment: .trailing)
                        .monospacedDigit()
                }
            }

            Spacer()
        }
        .padding()
        .onAppear { host.discover() }
    }
}

@available(iOS 15.0, macOS 12.0, *)
@MainActor
final class PulpAUv3Host: ObservableObject {
    @Published var componentName: String? = nil
    @Published var parameters: [AUParameter] = []
    @Published var isPlaying = false
    var audioUnit: AUAudioUnit?

    private let engine = AVAudioEngine()
    private var node: AVAudioUnit?

    func discover() {
        // Customize these four tags to match the plug-in's
        // AudioComponents entry in HostApp/Info.plist.
        var desc = AudioComponentDescription()
        desc.componentType = kAudioUnitType_Effect
        desc.componentSubType = 0x50_75_5F_45           // "Pu_E" — override
        desc.componentManufacturer = 0x50_75_6C_70     // "Pulp" — override

        AVAudioUnit.instantiate(with: desc, options: []) { [weak self] node, error in
            guard let self = self, let node = node, error == nil else { return }
            Task { @MainActor in
                self.node = node
                self.audioUnit = node.auAudioUnit
                self.componentName = node.auAudioUnit.componentName
                if let tree = node.auAudioUnit.parameterTree {
                    self.parameters = tree.allParameters
                }
                self.engine.attach(node)
                // Effects need a source feeding them — otherwise they
                // silently process zeros. Instruments (aumu) drive
                // themselves. Heuristic: wire the engine's inputNode
                // as source for every AU type except MusicDevice.
                // (Codex P2 on PR #279.)
                if desc.componentType != kAudioUnitType_MusicDevice {
                    self.engine.connect(self.engine.inputNode, to: node, format: nil)
                }
                self.engine.connect(node, to: self.engine.mainMixerNode, format: nil)
            }
        }
    }

    func toggle() {
        if isPlaying {
            engine.stop()
            isPlaying = false
        } else {
            do {
                try engine.start()
                isPlaying = true
            } catch {
                print("engine start failed: \(error)")
            }
        }
    }
}

#endif
