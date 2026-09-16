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
    float width(std::string_view text, const Font& font);
    float draw(std::string_view text, Point baseline, const DrawState& state,
               Surface& surface);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace haiku_remote
