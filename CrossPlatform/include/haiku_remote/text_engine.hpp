#pragma once

#include "haiku_remote/surface.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace haiku_remote {

class TextEngine {
public:
    // Diagnostics sink, one line per call. Face selection is the one place in
    // this class that can hand back a plausible-looking lie -- a width measured
    // with a style the server never asked for -- so a substituted or missing
    // face is reported here rather than left to be discovered by measuring.
    using Log = std::function<void(std::string_view)>;

    TextEngine();
    explicit TextEngine(Log log);
    ~TextEngine();
    TextEngine(const TextEngine&) = delete;
    TextEngine& operator=(const TextEngine&) = delete;

    void set_log(Log log);

    [[nodiscard]] bool available() const;

    // What face selection resolved for `font`: the file it opened, and the
    // style that file really carries. `exact` is false when no file on this host
    // carried the requested style and a substitute was measured instead;
    // `path` is empty when nothing opened at all, which is when width() falls
    // through to its codepoints * size * 0.6 estimate.
    //
    // Exposed because "the reply is a number and the number is plausible" is not
    // evidence that the requested face was measured. On a monospaced family
    // every style has the same advance, so no width assertion can tell a real
    // italic from a substituted regular there at all (#38).
    //
    // `exact` covers the three axes that select a file here -- bold, italic and
    // condensed. B_LIGHT_FACE and B_HEAVY_FACE also select a style in
    // app_server and are not represented at all; those are reported through the
    // log instead.
    struct FaceChoice {
        std::string path;
        // Face index within a .ttc collection; 0 for a single-face file.
        long index = 0;
        bool bold = false;
        bool italic = false;
        bool condensed = false;
        bool exact = false;
    };
    [[nodiscard]] FaceChoice selected_face(const Font& font);

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
