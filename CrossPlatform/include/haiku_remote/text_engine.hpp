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
    float draw(std::string_view text, Point baseline, const DrawState& state,
               Surface& surface, const EscapementDelta* delta = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace haiku_remote
