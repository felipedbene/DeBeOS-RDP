#pragma once

// The committed faces in tests/fonts as the *only* fonts a test sees.
//
// A test of a rendering property -- glyph coverage, advances, a view
// transform, the identity block -- must measure the client, not the host's
// font directory: the same check gave three answers on Linux, Windows and
// macOS while it drew with whatever trait selection found there (#108). So the
// test binaries call pin() before anything resolves a face, and every engine
// they create searches tests/fonts alone (TextEngine::set_font_search_override).
//
// The tests that are *about* host font selection -- does a bold request open a
// bold file on this host? -- open a HostFonts scope instead, see the host's real
// directories, and skip when the host lacks the face they need.

#include "haiku_remote/text_engine.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace test_fonts {

inline std::filesystem::path dir()
{
#ifdef HAIKU_REMOTE_TEST_FONTS
    return HAIKU_REMOTE_TEST_FONTS;
#else
    return std::filesystem::path(__FILE__).parent_path() / "fonts";
#endif
}

// The committed files, by name (tests/fonts/README.md).
inline std::filesystem::path noto_sans() { return dir() / "NotoSans-Regular.ttf"; }
inline std::filesystem::path dejavu_sans() { return dir() / "DejaVuSans.ttf"; }
inline std::filesystem::path dejavu_sans_mono()
{
    return dir() / "DejaVuSansMono.ttf";
}

// Every engine created from now on sees tests/fonts and nothing else. Trait
// selection then lands on Noto Sans Regular (proportional) and DejaVu Sans
// Mono Book (fixed), every other style relaxing to those, on every host.
inline void pin()
{
    haiku_remote::TextEngine::set_font_search_override(
        std::vector<std::string> {dir().string()});
}

// Empty when an engine created now draws by trait with the committed faces --
// proportional regular and bold with Noto Sans Regular, fixed-pitch with DejaVu
// Sans Mono -- and otherwise what it chose instead. Every binary that pins
// checks this, so a pin that silently stopped pinning fails rather than
// quietly measuring the host again.
inline std::string pin_problem()
{
    haiku_remote::TextEngine engine;
    struct Case {
        std::uint16_t face;
        std::uint8_t spacing;
        std::filesystem::path want;
        const char* what;
    };
    // Face bits from Font.h (B_BOLD_FACE 0x20); spacing 3 is B_FIXED_SPACING.
    const Case cases[] = {
        {0, 0, noto_sans(), "proportional regular"},
        {0x0020, 0, noto_sans(), "proportional bold"},
        {0, 3, dejavu_sans_mono(), "fixed-pitch regular"},
    };
    std::string problem;
    for (const auto& item : cases) {
        haiku_remote::Font font;
        font.size = 12;
        font.face = item.face;
        font.spacing = item.spacing;
        const auto chosen = engine.selected_face(font);
        std::error_code ec;
        if (chosen.path.empty() || chosen.index != 0
            || !std::filesystem::equivalent(chosen.path, item.want, ec)) {
            problem += std::string(problem.empty() ? "" : "; ") + item.what + " drew from '"
                + chosen.path + "', not " + item.want.string();
        }
    }
    return problem;
}

// For the length of a scope, the host's own font search, for a test of host
// font selection. Engines must be created inside the scope.
struct HostFonts {
    HostFonts() { haiku_remote::TextEngine::set_font_search_override(std::nullopt); }
    ~HostFonts() { pin(); }
    HostFonts(const HostFonts&) = delete;
    HostFonts& operator=(const HostFonts&) = delete;
};

} // namespace test_fonts
