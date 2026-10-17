#pragma once

// The application-chrome palette (DeBeOS-RDP issue #1, "Visual direction"):
// a warm neutral background with a cobalt, Haiku-blue accent. One definition so
// the library, the editor and any other client-side screen agree on colour --
// these values are pinned by exact pixel assertions in tests/library_tests.cpp.
//
// This is chrome only. Nothing here ever touches a remote-desktop pixel: the
// server's raw device pixels go through Session/Surface untouched (CLAUDE.md,
// "never let the host's colour management touch a pixel").
//
// Contrast (WCAG relative luminance, computed by hand):
//   text_primary on background  14.3:1
//   text_muted   on background   5.1:1  (AA for body text)
//   on_accent    on accent       6.5:1  (AA for button labels)
//   danger       on panel        6.1:1
//   focus_ring   on background   5.9:1  (WCAG 2.2 non-text contrast >= 3:1)

#include "haiku_remote/types.hpp"

namespace haiku_remote::ui_theme {

inline constexpr Color background {246, 243, 238, 255};   // warm paper
inline constexpr Color panel {255, 253, 250, 255};        // card surface
inline constexpr Color panel_selected {232, 239, 252, 255}; // cobalt-tinted card
inline constexpr Color field_background {255, 255, 255, 255};
inline constexpr Color border {214, 207, 197, 255};       // warm hairline
inline constexpr Color border_strong {176, 167, 155, 255};
inline constexpr Color accent {37, 89, 189, 255};         // cobalt / Haiku blue
inline constexpr Color accent_soft {222, 231, 249, 255};  // chip / hover fill
inline constexpr Color focus_ring {37, 89, 189, 255};     // == accent, 2px ring
inline constexpr Color text_primary {38, 34, 30, 255};
inline constexpr Color text_muted {110, 102, 92, 255};
inline constexpr Color danger {178, 50, 40, 255};
inline constexpr Color danger_soft {251, 233, 230, 255};
inline constexpr Color star_on {204, 138, 0, 255};
inline constexpr Color on_accent {255, 255, 255, 255};

} // namespace haiku_remote::ui_theme
