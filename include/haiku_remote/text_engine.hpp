#pragma once

#include "haiku_remote/surface.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
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

    // The FreeType this build rasterises with, read as the server reads its
    // own for RP_GLYPH_RASTERIZER: canonical_glyph_rasterizer for the vendored
    // third_party/freetype, whatever the host has for a
    // HAIKU_REMOTE_FREETYPE=system build.
    [[nodiscard]] static GlyphRasterizer rasterizer();
    // "FreeType 2.6.3, no LCD filtering, TrueType interpreter v35".
    [[nodiscard]] static std::string describe(const GlyphRasterizer& rasterizer);

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
        // The font carried an RP_CAP_FONT_IDENTITY block and the face it names
        // was found here by name or by file identity -- as opposed to a trait
        // choice standing in for it. `exact` is true whenever this is.
        bool named = false;
        // ...and the file found carries the server's own 'head' checksum and
        // revision at the server's face index: the same bytes, so the same
        // advances and the same glyphs.
        bool identical = false;
    };
    [[nodiscard]] FaceChoice selected_face(const Font& font);
    // The files selection searches for exactly `font`'s pitch and style, in
    // order, before any relaxation: the environment override, then the one
    // family trait selection draws every style of that pitch from -- or, only
    // on a host where discovery found no family of the pitch at all, the
    // built-in table. The family is chosen by a fixed rule over the files'
    // own names and styles, never by directory scan order, so regular, bold
    // and italic are cuts of one family.
    //
    // Exposed so a test can ask "does a file this engine would open carry the
    // style?" independently of which one it picked
    // -- the only fair precondition for an exact-style check, because a face
    // elsewhere on disk (macOS ships Helvetica Neue Condensed Bold, Avenir Next
    // Condensed, Arial Narrow) belongs to a family selection never searches.
    [[nodiscard]] std::vector<std::string> candidate_files(const Font& font);

    // Whether a file with `identity`'s own bytes -- the 'head' checksum and
    // revision at its face index -- is on this host (fetched or not). False for
    // an identity without a checksum: nothing could prove it.
    [[nodiscard]] bool has_identical_face(const FontIdentity& identity);

    // Font files fetched while engines are running (font_fetch.hpp). Every
    // engine, on any thread, indexes them before it next resolves a face, and
    // re-resolves each font that had not found the server's own file.
    // Thread-safe; a path already added is ignored.
    static void add_font_files(const std::vector<std::string>& paths);
    // Bumped by add_font_files: how many files have been added in this process.
    [[nodiscard]] static std::size_t font_files_generation();

    // A hermetic font set, for tests: with `directories` set, an engine created
    // afterwards searches exactly those directories (and add_font_files) --
    // no host font directory, no per-user one, no fetched-fonts cache, no
    // HAIKU_REMOTE_FONT_DIR, no HAIKU_REMOTE_FONT / HAIKU_REMOTE_MONO_FONT and
    // no built-in table of host paths -- so what it draws cannot depend on the
    // fonts a host has installed. std::nullopt restores the normal search.
    // Read once per engine, when it first resolves a face; thread-safe.
    static void set_font_search_override(
        std::optional<std::vector<std::string>> directories);

    // `delta` is optional; when non-null it widens each character's advance the
    // way Haiku's own layout engine does, so both the painted spacing and the
    // returned advance -- which becomes the pen position replied to the server
    // -- match what the server would have computed locally.
    float width(std::string_view text, const Font& font,
                const EscapementDelta* delta = nullptr);

    // Every caret stop of `text`, from ONE layout pass: what a text field needs
    // to place its caret, its selection and its scroll, and to draw only the
    // glyphs in view, without measuring a growing prefix per code point (which
    // made a 1 MiB paste hang the library, found by PR #97). offsets[k] is the byte
    // offset of the k-th code point (offsets.back() is the end of the text);
    // caret[k] is exactly width(text.substr(0, offsets[k])); pen[k] (one fewer)
    // is where glyph k is placed -- caret[k] plus the kerning with the glyph
    // before it, so a run drawn from glyph k at pen[k] lands where the whole
    // string would have put it.
    struct Stops {
        std::vector<std::size_t> offsets;
        std::vector<float> caret;
        std::vector<float> pen;
    };
    [[nodiscard]] Stops stops(std::string_view text, const Font& font);
    // The byte length of the longest code-point prefix of `text` whose width
    // is at most `max_width`. It lays out only as far as that prefix (plus
    // one), so eliding a long string costs what is shown, not what is held.
    [[nodiscard]] std::size_t fit(std::string_view text, const Font& font,
                                  float max_width);

    // What layout has cost so far: how many strings were laid out (by width,
    // draw, stops or fit) and how many code points that walked in total. The
    // operation count the library's text-field tests pin; not used otherwise.
    struct Stats {
        std::uint64_t layouts = 0;
        std::uint64_t code_points = 0;
    };
    [[nodiscard]] const Stats& stats() const;
    // `disable_hinting` loads glyphs with FT_LOAD_NO_HINTING. It exists so a
    // caller can build a hinting-invariant comparison: a transformed glyph is
    // hinted at the font's own ppem and then transformed, as app_server does,
    // so a 2x view scale and a render at 2x the ppem are hinted differently and
    // their ink diverges by however much the face's hinting moves it. With
    // hinting off in both, they share one outline. Production callers leave it
    // false and keep the server's hinted metrics.
    float draw(std::string_view text, Point baseline, const DrawState& state,
               Surface& surface, const EscapementDelta* delta = nullptr,
               bool disable_hinting = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace haiku_remote
