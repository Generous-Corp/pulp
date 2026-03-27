# Strategic Response to iPlug3 Gap Analysis

Date: 2026-03-24

## Decision Summary

| Gap | Priority | Action | Timeline |
|-----|----------|--------|----------|
| **Multi-bus support** | CRITICAL | Extend Processor interface NOW | Before Phase 6 |
| **SDL3 for windowing** | HIGH | Investigate in explore/ branch | Phase 6 start |
| **libremidi** | MEDIUM | Adopt when adding Win/Linux | Phase 4 expansion |
| **Screenshot capture** | MEDIUM | Add to inspector/MCP | Phase 7 |
| **Python/Node bindings** | LOW | Plan for Phase 9+ | Future |
| **DSL support (FAUST)** | LOW | Optional module, Phase 9+ | Future |
| **WebCLAP/WAMv2** | LOW | When specs stabilize | Phase 9+ |
| **Compute shaders** | LOW | Exploration territory | Phase 9 |
| **C++20 Concepts** | LOW | Current virtual approach is fine | No change |

## Immediate Action Item: Multi-Bus Support

### What Changes

1. `PluginDescriptor` gains a bus configuration:
```cpp
struct BusInfo {
    std::string name;
    int default_channels = 2;
    bool optional = false;  // true for sidechain
};

struct PluginDescriptor {
    // ... existing fields ...
    std::vector<BusInfo> input_buses = {{.name = "Main In", .default_channels = 2}};
    std::vector<BusInfo> output_buses = {{.name = "Main Out", .default_channels = 2}};
};
```

2. `Processor::process()` gains multi-bus buffer access
3. Format adapters map to native bus APIs
4. Existing plugins continue to work with default single-bus config

### What Doesn't Change

- StateStore, parameters, bindings — unchanged
- Entry macros — unchanged
- Headless processing — unchanged
- Build system — unchanged
- All existing tests — should still pass

## Key Insight: Convergent Evolution

Both Pulp and iPlug3 independently chose:
- Dawn for WebGPU
- Skia Graphite for 2D
- QuickJS/V8/JSC for scripting
- CMake for builds
- CHOC for utilities
- C++20 standard
- AI-first design philosophy
- Headless-first architecture
- Permissive licensing

This strong convergence validates Pulp's architectural decisions. The rendering stack choice is correct.

## Where Pulp Can Win

1. **Ship first** — iPlug3 has no public code. Working code beats manifestos.
2. **Swift advantage** — no other framework offers Swift-native AUv3 + SwiftUI
3. **Open development** — transparency builds trust and community
4. **Documentation** — iPlug's sparse docs are a known weakness
5. **MCP/CLI** — first to ship AI agent integration wins the narrative
6. **Clear MIT license** — iPlug3's license is "TBD"
