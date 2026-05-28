//
// Minimal SwiftUI host for an AUv3 Pulp plug-in (issue #250).
//
// Loads the plug-in's audio unit via AVAudioUnitComponentManager,
// instantiates an AVAudioEngine with a sampler source → the Pulp AU
// → output. The view exposes a play/stop button and lists the
// plug-in's parameters via the AVAudioUnit's parameterTree so the
// user can verify the extension loads without opening a full DAW.
//
// Discovery strategy: rather than hardcode a single four-CC
// AudioComponentDescription, the host enumerates every AUv3 with
// manufacturer="Pulp" and picks the first match. This keeps the
// template usable for any Pulp plug-in — instrument (aumu) or
// effect (aufx) — without per-plug-in edits.
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
            Text(host.componentName ?? "(no Pulp AUv3 found — check Info.plist)")
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
        .onAppear {
            host.discover()
            // Auto-Play after 1.5s so the audio path lights up without a
            // tap — useful for `simctl io booted recordVideo` smoke runs.
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                if !host.isPlaying && host.audioUnit != nil { host.toggle() }
            }
        }
    }
}

@available(iOS 15.0, macOS 12.0, *)
@MainActor
final class PulpAUv3Host: ObservableObject {
    @Published var componentName: String? = nil
    @Published var parameters: [AUParameter] = []
    @Published var isPlaying = false
    var audioUnit: AUAudioUnit?

    // Pulp manufacturer code: 'Pulp' = 0x50756C70.
    private let manufacturerCode: OSType = 0x50_75_6C_70

    private let engine = AVAudioEngine()
    private var node: AVAudioUnit?
    private var midiBlock: AUScheduleMIDIEventBlock?

    func discover() {
        // Enumerate every AUv3 on the system, then filter to ones whose
        // manufacturer four-CC matches Pulp. This works for any Pulp
        // plug-in type — aumu (instrument), aufx (effect), aumf
        // (music-effect), aumi (MIDI processor) — without the host
        // template needing to know the specific four-CCs ahead of time.
        let manager = AVAudioUnitComponentManager.shared()
        let allUnits = manager.components(matching: AudioComponentDescription())
        let pulpUnits = allUnits.filter { $0.audioComponentDescription.componentManufacturer == manufacturerCode }

        // PULP_DISCOVER prints are deliberate triage helpers — keep them.
        // When the audio path silently fails, grep `PULP_` in the launch
        // log to identify which step in the chain broke. See
        // `.agents/skills/auv3/SKILL.md` "iOS AUv3 diagnostic recipe".
        print("PULP_DISCOVER_ALL: \(allUnits.count) total AUv3 components on this system")
        print("PULP_DISCOVER: matching Pulp manufacturer=\(pulpUnits.count)")
        for c in pulpUnits {
            print("PULP_DISCOVER:   - name=\(c.name) type=\(c.typeName) mfr=\(c.manufacturerName)")
        }

        guard let first = pulpUnits.first else {
            print("PULP_DISCOVER: no Pulp AUv3 found — extension may have failed to register")
            return
        }
        let firstName = first.name
        Task { @MainActor in self.componentName = "Found: \(firstName)" }

        let desc = first.audioComponentDescription

        // iOS cannot load AUv3 extensions in-process; must use
        // .loadOutOfProcess. The default `[]` returns OSStatus 4
        // ("Exec format" / instantiate failed). See Apple's
        // "Incorporating Audio Effects and Instruments" sample.
        #if os(iOS)
        let instOptions: AudioComponentInstantiationOptions = .loadOutOfProcess
        #else
        let instOptions: AudioComponentInstantiationOptions = []
        #endif

        AVAudioUnit.instantiate(with: desc, options: instOptions) { [weak self] node, error in
            if let err = error { print("PULP_INSTANTIATE_ERROR: \(err)") }
            guard let self = self, let node = node, error == nil else {
                print("PULP_INSTANTIATE: bailed (node=\(String(describing: node)) error=\(String(describing: error)))")
                return
            }
            Task { @MainActor in
                self.node = node
                self.audioUnit = node.auAudioUnit
                self.componentName = node.auAudioUnit.componentName
                print("PULP_INSTANTIATE_OK: \(node.auAudioUnit.componentName ?? "(nil)")")
                if let tree = node.auAudioUnit.parameterTree {
                    self.parameters = tree.allParameters
                }
                self.engine.attach(node)

                // Effects need a source feeding them — otherwise they
                // silently process zeros. Instruments (aumu) drive
                // themselves via MIDI events sent below in toggle().
                if desc.componentType != kAudioUnitType_MusicDevice {
                    self.engine.connect(self.engine.inputNode, to: node, format: nil)
                }
                self.engine.connect(node, to: self.engine.mainMixerNode, format: nil)

                // For an instrument (aumu), wire a MIDI event block so
                // toggle() can send a sustained note. Pattern from
                // Apple's SimplePlayEngine.InstrumentPlayer.
                if desc.componentType == kAudioUnitType_MusicDevice {
                    self.midiBlock = node.auAudioUnit.scheduleMIDIEventBlock
                    print("PULP_MIDI_BLOCK: \(self.midiBlock != nil ? "ready" : "unavailable")")
                }
            }
        }
    }

    func sendNote(noteOn: Bool, key: UInt8 = 60, velocity: UInt8 = 100) {
        guard let block = midiBlock else { print("PULP_NOTE: no midiBlock"); return }
        let status: UInt8 = noteOn ? 0x90 : 0x80
        let vel: UInt8 = noteOn ? velocity : 0
        let bytes = UnsafeMutablePointer<UInt8>.allocate(capacity: 3)
        bytes[0] = status; bytes[1] = key; bytes[2] = vel
        block(AUEventSampleTimeImmediate, 0, 3, bytes)
        bytes.deallocate()
        print("PULP_NOTE: \(noteOn ? "ON" : "OFF") key=\(key) vel=\(vel)")
    }

    func toggle() {
        if isPlaying {
            sendNote(noteOn: false)
            engine.stop()
            isPlaying = false
        } else {
            do {
                try engine.start()
                isPlaying = true
                // Trigger a sustained C4 (note 60) for the instrument
                // path so we hear something when Play is tapped. Effects
                // don't need MIDI — they're fed by inputNode in discover().
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in
                    self?.sendNote(noteOn: true, key: 60, velocity: 100)
                }
            } catch {
                print("engine start failed: \(error)")
            }
        }
    }
}

#endif
