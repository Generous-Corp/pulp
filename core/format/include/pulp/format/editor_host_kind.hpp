#pragma once

#include <cstdint>

namespace pulp::format {

/// Where a processor's editor lives, as far as the keyboard is concerned.
///
/// `plugin`: a DAW or other host owns the window and the keyboard. Plain keys
/// are the host's (transport, Musical Typing, key commands); an editor should
/// consume them only while its own focused field or open popup needs them.
///
/// `standalone`: the Pulp standalone app owns the window, so the editor may
/// bind plain-key global shortcuts.
enum class EditorHostKind : std::uint8_t { plugin, standalone };

}  // namespace pulp::format
