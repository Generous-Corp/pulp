#pragma once

#include "delay_params.hpp"
#include "pulp_delay_ui_tokens.hpp"

#include <pulp/state/store.hpp>

namespace pulp::examples::delay::ui {

/// Immutable authored character-to-accent mapping.
///
/// The resolver owns no mutable colour state: every paint reads the current
/// Character atomically from StateStore, then resolves through this one table.
class CharacterPalette {
  public:
    explicit CharacterPalette(const state::StateStore& store) : store_(&store) {}

    static constexpr canvas::Color accent_for(Character character) noexcept {
        switch (character) {
        case Character::clean:
            return canvas::Color::rgba8(0x16, 0xDA, 0xC2);
        case Character::vintage:
            return canvas::Color::rgba8(0xA9, 0x7B, 0xFF);
        case Character::bbd:
            return canvas::Color::rgba8(0xFF, 0x4F, 0x4F);
        case Character::tape:
        default:
            return canvas::Color::rgba8(0xB8, 0xE6, 0x35);
        }
    }

    canvas::Color accent() const noexcept {
        return accent_for(character_from_param(store_->get_value(kCharacter)));
    }

    canvas::Color soft() const noexcept {
        return accent().interpolate(color::panel, 0.25f);
    }

    canvas::Color dim() const noexcept {
        return accent().interpolate(color::panel_deep, 0.68f);
    }

  private:
    const state::StateStore* store_ = nullptr;
};

} // namespace pulp::examples::delay::ui
