"""Dependency-free Rack module identity shared by generation and measurement."""

AUDIO_MODELS = {("Core", "AudioInterface"), ("Core", "AudioInterface2"),
                ("Core", "AudioInterface16"), ("Core", "Audio"),
                ("Core", "Audio2"), ("Core", "Audio8"), ("Core", "Audio16")}


def is_audio_interface(module: dict) -> bool:
    if (module.get("plugin"), module.get("model")) in AUDIO_MODELS:
        return True
    # Rack has renamed these models across versions. Restrict the fallback to
    # Core Audio* so Core MIDI and utility modules never become listeners.
    return (module.get("plugin") == "Core" and
            str(module.get("model", "")).startswith("Audio"))
