#pragma once

#include "haiku_remote/surface.hpp"

#include <memory>
#include <string_view>

namespace haiku_remote {

class TextEngine {
public:
    TextEngine();
    ~TextEngine();
    TextEngine(const TextEngine&) = delete;
    TextEngine& operator=(const TextEngine&) = delete;

    [[nodiscard]] bool available() const;

    // `delta` is optional; when non-null it widens each character's advance the
    // way Haiku's own layout engine does, so both the painted spacing and the
    // returned advance -- which becomes the pen position replied to the server
    // -- match what the server would have computed locally.
    float width(std::string_view text, const Font& font,
                const EscapementDelta* delta = nullptr);
    // `disable_hinting` loads glyphs with FT_LOAD_NO_HINTING. It exists so a
    // caller can build a hinting-invariant reference image: a glyph rasterised
    // under a non-identity FT_Set_Transform is effectively unhinted, so a hinted
    // render at a matching ppem is not a fair reference for it on a heavily
    // hinted face -- the ink counts diverge. Rendering that reference unhinted
    // too makes the comparison font- and platform-invariant. Production callers
    // leave it false and keep the server's hinted metrics.
    float draw(std::string_view text, Point baseline, const DrawState& state,
               Surface& surface, const EscapementDelta* delta = nullptr,
               bool disable_hinting = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace haiku_remote
