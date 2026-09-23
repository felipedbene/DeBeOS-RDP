#include "haiku_remote/text_engine.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_LCD_FILTER_H
#include FT_OUTLINE_H
#include <hb-ft.h>
#include <hb.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <numbers>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace haiku_remote {
namespace {

// headers/os/interface/Font.h:80-89. Only the bits that pick a *style* matter
// here; the decoration bits (underscore, negative, outlined, strikeout) are
// applied at draw time by app_server and change no advance.
constexpr std::uint16_t italic_face = 0x0001;
constexpr std::uint16_t bold_face = 0x0020;
constexpr std::uint16_t condensed_face = 0x0080;
// Weight bits that Haiku selects a style with -- GetStyleMatchingFace() masks
// them in alongside bold/italic/condensed (src/servers/app/font/
// FontFamily.cpp:224-226) -- but that this client has no file axis for. They
// are named in the diagnostic below instead of being dropped in silence.
constexpr std::uint16_t light_face = 0x0100;
constexpr std::uint16_t heavy_face = 0x0200;
constexpr std::uint8_t fixed_spacing = 3;

// RP_SET_FONT carries `shear`, `rotation` and `false_bold_width` -- app_server
// honours all three locally (src/servers/app/drawing/Painter/
// AGGTextRenderer.cpp SetFont) and there is no server-side fallback that
// rasterises transformed text for a remote view, so if the client ignores them
// the attributes are simply lost.
//
// `rotation` is degrees counter-clockwise and `shear` is degrees with 90 as
// neutral. Both leave the *layout* alone: Haiku lays glyphs out along an
// unrotated horizontal baseline and then pushes the whole positioned outline
// through one embedded transform (AGGTextRenderer::StringRenderer passes the
// untransformed x/y to InitAdaptors and transforms the resulting path). Since
// transform(G + v) == transform(G) + transform(v), transforming the glyph
// outline via FT_Set_Transform and transforming the accumulated baseline
// offset is the same composition.
bool font_is_transformed(const Font& font)
{
    return font.rotation != 0.0f || font.shear != neutral_font_shear;
}

constexpr double ft_fixed_one = 65536.0;

// RP_SET_TRANSFORM's six floats reach DrawState::transform unvalidated
// (RemoteMessage.cpp AddTransform writes them verbatim), so fold NaN to zero and
// clamp the magnitude before the cast: converting a double outside FT_Fixed's
// range is undefined behaviour, which the cos/sin callers below could never hit
// but a wire-supplied scale can.
constexpr double ft_matrix_limit = 4096.0;

FT_Fixed to_ft_fixed(double value)
{
    if (std::isnan(value))
        return 0;
    return static_cast<FT_Fixed>(
        std::clamp(value, -ft_matrix_limit, ft_matrix_limit) * ft_fixed_one);
}

// Mirrors ServerFont::GetTransformedFace() (src/servers/app/ServerFont.cpp):
// a rotation matrix times a shear matrix, in FreeType 16.16 fixed point. At
// shear == 90 the shear matrix is exactly the identity, because cos(90) rounds
// to 0 in 16.16 -- so a font with no rotation and neutral shear is untouched.
FT_Matrix font_matrix(const Font& font)
{
    const double degrees_to_radians = std::numbers::pi / 180.0;
    const double rotation = static_cast<double>(font.rotation)
        * degrees_to_radians;
    FT_Matrix rotate;
    rotate.xx = to_ft_fixed(std::cos(rotation));
    rotate.xy = to_ft_fixed(-std::sin(rotation));
    rotate.yx = to_ft_fixed(std::sin(rotation));
    rotate.yy = to_ft_fixed(std::cos(rotation));

    const double shear = static_cast<double>(font.shear) * degrees_to_radians;
    FT_Matrix result;
    result.xx = to_ft_fixed(1.0);
    result.xy = to_ft_fixed(-std::cos(shear));
    result.yx = 0;
    result.yy = to_ft_fixed(1.0);

    // FT_Matrix_Multiply(a, b) computes `b = a * b`.
    FT_Matrix_Multiply(&rotate, &result);
    return result;
}

// The *linear* part of a view transform, in the y-up space FT_Set_Transform
// works in.
//
// DrawState::transform holds Haiku's BAffineTransform exactly as Painter
// installs it -- translate(-offset) * affine * translate(offset)
// (src/servers/app/drawing/Painter/Painter.cpp:372-383) -- and that whole
// bracket is what DrawState::map_point reproduces. So the translation is the
// baseline's business and only sx/shx/shy/sy belong in the glyph outline.
//
// FreeType's y axis points up and the screen's points down, so converting a
// screen-space linear map into font space is a conjugation by diag(1, -1):
// the two cross terms change sign, the two diagonal terms do not. The rotation
// arm of `font_matrix` above is the same conversion done by hand -- app_server
// builds its embedded transform as RotateBy(-rotation) in screen space
// (AGGTextRenderer.cpp:81-82) and ServerFont::GetTransformedFace builds the
// matching FreeType matrix with +sin on yx (ServerFont.cpp:406-409).
FT_Matrix view_matrix(const Transform& transform)
{
    FT_Matrix result;
    result.xx = to_ft_fixed(transform.sx);
    result.xy = to_ft_fixed(-transform.shx);
    result.yx = to_ft_fixed(-transform.shy);
    result.yy = to_ft_fixed(transform.sy);
    return result;
}

// Whether the view transform does anything a translation cannot. This is the
// client's half of StringRenderer::NeedsVector(), which is
// `!fTransform.IsTranslationOnly()` over the combined embedded * baseline *
// view transform (AGGTextRenderer.cpp:146): a translation-only transform leaves
// the server on its hinted-bitmap fast path, offset by transform(0,0)
// (AGGTextRenderer.cpp:229-236), and leaves this client's glyphs untouched too.
bool view_is_linear_identity(const Transform& transform)
{
    return transform.sx == 1 && transform.shy == 0
        && transform.shx == 0 && transform.sy == 1;
}

// A resource bound that arrives with the transform. Moving the transform into
// the outline means a wire-supplied scale now multiplies the glyph's *pixel*
// dimensions, and FT_Render_Glyph allocates width * rows bytes -- three times
// that in LCD mode. Nothing legible survives past this size on a surface capped
// at Surface::max_dimension, so an outline wider or taller than this is skipped
// rather than rendered.
constexpr FT_Pos max_glyph_extent = 4096;

bool glyph_is_renderable(FT_GlyphSlot slot)
{
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
        return true;
    FT_BBox box;
    FT_Outline_Get_CBox(&slot->outline, &box);
    // 26.6 fixed point.
    return (box.xMax - box.xMin) <= max_glyph_extent * 64
        && (box.yMax - box.yMin) <= max_glyph_extent * 64;
}

// Applies a 16.16 FT_Matrix to a point in FreeType's y-up space.
std::pair<float, float> transform_point(const FT_Matrix& matrix, float x,
                                        float y)
{
    const double dx = x;
    const double dy = y;
    return {
        static_cast<float>(
            (static_cast<double>(matrix.xx) * dx
             + static_cast<double>(matrix.xy) * dy) / ft_fixed_one),
        static_cast<float>(
            (static_cast<double>(matrix.yx) * dx
             + static_cast<double>(matrix.yy) * dy) / ft_fixed_one),
    };
}

// Applies RP_SET_FONT's `false_bold_width` to a loaded outline, between
// FT_Load_Glyph and FT_Render_Glyph -- app_server's equivalent is
// `fContour.width(font.FalseBoldWidth() * 2.0)` in AGGTextRenderer::SetFont,
// an outward contour of the glyph path. Stock software reaches this for glow
// and outline passes (Tracker's TextWidget, HaikuControlLook, MediaPlayer's
// SubtitleBitmap), not just font demos. Emboldening changes glyph weight only;
// it does not change the advance, so no width reply moves.
//
// Returns true when rendering should proceed, so it can sit in the existing
// load-and-render conjunction.
bool embolden_glyph(FT_GlyphSlot slot, FT_Pos strength)
{
    if (strength > 0 && slot->format == FT_GLYPH_FORMAT_OUTLINE)
        FT_Outline_EmboldenXY(&slot->outline, strength, strength);
    return true;
}

// Always called, never skipped: the FT_Face cache is shared between fonts, and
// FT_Set_Transform is face state, so an earlier rotated font's matrix would
// otherwise stay installed and rotate an unrotated string.
void install_transform(FT_Face face, const FT_Matrix& matrix, bool transformed)
{
    if (transformed) {
        FT_Matrix copy = matrix;
        FT_Set_Transform(face, &copy, nullptr);
    } else {
        FT_Set_Transform(face, nullptr, nullptr);
    }
}

struct FaceKey {
    bool mono = false;
    bool bold = false;
    bool italic = false;
    bool condensed = false;

    friend bool operator<(const FaceKey& a, const FaceKey& b)
    {
        return std::tie(a.mono, a.bold, a.italic, a.condensed)
            < std::tie(b.mono, b.bold, b.italic, b.condensed);
    }
};

FaceKey face_key(const Font& font)
{
    return FaceKey {
        font.spacing == fixed_spacing,
        (font.face & bold_face) != 0,
        (font.face & italic_face) != 0,
        (font.face & condensed_face) != 0,
    };
}

// Which of a family's eight files a key wants.
constexpr std::size_t style_slot(const FaceKey& key)
{
    return (key.bold ? 1u : 0u) | (key.italic ? 2u : 0u)
        | (key.condensed ? 4u : 0u);
}

constexpr std::size_t style_slots = 8;

// One font family, as the eight files it may provide, indexed by style_slot():
// regular, bold, italic, bold italic, then the same four condensed. A null entry
// means the family ships no file for that combination -- Cantarell has no
// italic, Noto Sans Mono has no italic, DejaVu Sans Mono has no condensed -- and
// that family is skipped for that style rather than answering with its regular
// face.
//
// Haiku conflates Italic and Oblique into the one B_ITALIC_FACE
// (FontStyle::_TranslateStyleToFace, src/servers/app/font/FontStyle.cpp:250-252)
// and so does this table: a family spelling it either way fills the same slot.
//
// Whole families are listed, in preference order, so a styled face is the styled
// face *of the family the regular face came from* whenever that family has one.
// The previous list mixed them -- proportional regular resolved to Noto Sans
// while proportional bold resolved to Cantarell-Bold -- and measured here at
// 12px over "Hamburgefonstiv" that was 96.875px regular against 96.000px bold:
// bold text laid out *narrower* than the same string in regular. Same family,
// the answer is 105.875px.
struct FamilyFiles {
    bool mono;
    std::array<const char*, style_slots> files;
};

constexpr FamilyFiles font_families[] = {
    // Noto Sans: the only complete proportional family on a stock Linux host,
    // and already the source of the regular face, so it stays first.
    {false, {
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Italic.ttf",
        "/usr/share/fonts/google-noto/NotoSans-BoldItalic.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Condensed.ttf",
        "/usr/share/fonts/google-noto/NotoSans-CondensedBold.ttf",
        "/usr/share/fonts/google-noto/NotoSans-CondensedItalic.ttf",
        "/usr/share/fonts/google-noto/NotoSans-CondensedBoldItalic.ttf",
    }},
    {false, {
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-BoldOblique.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSansCondensed.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSansCondensed-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSansCondensed-Oblique.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/"
            "DejaVuSansCondensed-BoldOblique.ttf",
    }},
    // Cantarell ships Regular and Bold and nothing slanted, which is why it may
    // only ever answer those two slots.
    {false, {
        "/usr/share/fonts/cantarell/Cantarell-Regular.otf",
        "/usr/share/fonts/cantarell/Cantarell-Bold.otf",
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    }},
    // The Windows and macOS rows are the documented stock filenames and have not
    // been opened on those hosts from here. They fail safely if one is wrong:
    // a path that does not exist is skipped, the relaxation ladder takes over,
    // and the substitution is reported rather than passed off as the real face.
    {false, {
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/arialbd.ttf",
        "C:/Windows/Fonts/ariali.ttf",
        "C:/Windows/Fonts/arialbi.ttf",
        "C:/Windows/Fonts/arialn.ttf",
        "C:/Windows/Fonts/arialnb.ttf",
        "C:/Windows/Fonts/arialni.ttf",
        "C:/Windows/Fonts/arialnbi.ttf",
    }},
    // A .ttc collection: one path, four styles at four face *indices*, which
    // open_matching() below searches. Opening index 0 and stopping -- all this
    // code used to do -- gets Helvetica Regular for an italic request no matter
    // how the candidate list is written.
    {false, {
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        nullptr, nullptr, nullptr, nullptr,
    }},
    {true, {
        "/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansMono-Bold.ttf",
        nullptr, nullptr,
        "/usr/share/fonts/google-noto/NotoSansMono-Condensed.ttf",
        "/usr/share/fonts/google-noto/NotoSansMono-CondensedBold.ttf",
        nullptr, nullptr,
    }},
    {true, {
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Oblique.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-BoldOblique.ttf",
        nullptr, nullptr, nullptr, nullptr,
    }},
    {true, {
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/consolab.ttf",
        "C:/Windows/Fonts/consolai.ttf",
        "C:/Windows/Fonts/consolaz.ttf",
        nullptr, nullptr, nullptr, nullptr,
    }},
    {true, {
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Menlo.ttc",
        nullptr, nullptr, nullptr, nullptr,
    }},
};

// The files that could carry exactly `key`, best first. The environment
// override leads, as it always has, but it is now style-matched like any other
// candidate -- pointing HAIKU_REMOTE_FONT at a regular file no longer makes it
// the answer to an italic query.
std::vector<std::string> font_candidates(const FaceKey& key)
{
    std::vector<std::string> result;
    if (const char* configured = std::getenv(
            key.mono ? "HAIKU_REMOTE_MONO_FONT" : "HAIKU_REMOTE_FONT")) {
        result.emplace_back(configured);
    }
    const std::size_t slot = style_slot(key);
    for (const auto& family : font_families) {
        if (family.mono != key.mono || family.files[slot] == nullptr)
            continue;
        result.emplace_back(family.files[slot]);
    }
    return result;
}

// Styles to try when the requested one has no file anywhere, closest first.
// Every rung is still matched exactly against the file's own style, so what we
// end up with is known rather than assumed, and FaceChoice::exact records that
// it was a substitution.
//
// The order drops the cheapest axis first, cheap meaning least damage to the
// *metrics* -- which is the whole point of the reply this feeds. Measured here
// at 12px over "Hamburgefonstiv" in Noto Sans, against 96.875px regular: italic
// is 93.875 (3.0px away), bold 105.875 (9.0px) and condensed 80.922 (16.0px).
std::vector<FaceKey> style_relaxations(const FaceKey& key)
{
    std::vector<FaceKey> result {key};
    if (key.italic) {
        FaceKey next = result.back();
        next.italic = false;
        result.push_back(next);
    }
    if (key.bold) {
        FaceKey next = result.back();
        next.bold = false;
        result.push_back(next);
    }
    if (key.condensed) {
        FaceKey next = result.back();
        next.condensed = false;
        result.push_back(next);
    }
    return result;
}

struct FaceStyle {
    bool bold = false;
    bool italic = false;
    bool condensed = false;
};

bool contains_word(const char* haystack, std::string_view needle)
{
    if (haystack == nullptr)
        return false;
    std::string lowered = haystack;
    for (char& c : lowered)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lowered.find(needle) != std::string_view::npos;
}

// The style a file really carries. FreeType sets FT_STYLE_FLAG_ITALIC for an
// Oblique file as well as an Italic one, the same conflation Haiku makes turning
// a style name into face bits (FontStyle::_TranslateStyleToFace,
// src/servers/app/font/FontStyle.cpp:250-252). Condensed has no FreeType flag,
// so it comes off the name -- which is where Haiku reads it from too (same
// function, :256-257).
//
// Both names are searched, because families disagree about where the word goes:
// Noto Sans and DejaVu Sans put it in the *style* ("Noto Sans" / "Condensed",
// read off the files here), while Arial Narrow puts it in the *family* and calls
// its style "Regular". Looking only at the style name would reject the narrow
// file as un-condensed and then report a substitution that had actually been
// found.
FaceStyle style_of(FT_Face face)
{
    FaceStyle result;
    result.bold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    result.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    result.condensed = contains_word(face->style_name, "condensed")
        || contains_word(face->style_name, "narrow")
        || contains_word(face->family_name, "condensed")
        || contains_word(face->family_name, "narrow");
    return result;
}

// Exact, the way app_server compares: GetStyleMatchingFace() accepts a style
// only when `style->Face() == face` over the style-selecting bits
// (src/servers/app/font/FontFamily.cpp:228-235), and ServerFont::SetFace returns
// B_ERROR rather than approximating when nothing in the family matches
// (src/servers/app/ServerFont.cpp:334-371). Anything looser is how a regular
// face came to answer an italic query.
bool style_matches(const FaceStyle& have, const FaceKey& want)
{
    return have.bold == want.bold && have.italic == want.italic
        && have.condensed == want.condensed;
}

// Opens `path` and returns the face inside it carrying exactly `want`, or
// nullptr. `want == nullptr` accepts face index 0 whatever its style.
//
// The loop over num_faces is what makes the macOS candidates mean anything: a
// .ttc collection holds Regular, Bold, Oblique and Bold Oblique in one file and
// FT_New_Face(..., 0, ...) always returns the first of them.
FT_Face open_matching(FT_Library library, const std::string& path,
                      const FaceKey* want, long& index_out)
{
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face) != 0)
        return nullptr;
    if (want == nullptr || style_matches(style_of(face), *want)) {
        index_out = 0;
        return face;
    }
    const long count = face->num_faces;
    FT_Done_Face(face);
    for (long index = 1; index < count; ++index) {
        if (FT_New_Face(library, path.c_str(), index, &face) != 0)
            continue;
        if (style_matches(style_of(face), *want)) {
            index_out = index;
            return face;
        }
        FT_Done_Face(face);
    }
    return nullptr;
}

const char* style_name(const FaceKey& key)
{
    static const char* const names[style_slots] = {
        "Regular", "Bold", "Italic", "Bold Italic",
        "Condensed", "Condensed Bold", "Condensed Italic",
        "Condensed Bold Italic",
    };
    return names[style_slot(key)];
}

// The exact set of scalars Haiku's layout engine charges `delta.space` to:
// GlyphLayoutEngine::IsWhiteSpace(), src/servers/app/font/GlyphLayoutEngine.h
// lines 226-243. Everything else -- including a NUL or a malformed byte -- is
// charged `delta.nonspace`.
bool is_haiku_white_space(std::uint32_t code)
{
    switch (code) {
    case 0x0009: // tab
    case 0x000a: // line feed
    case 0x000b: // vertical tab
    case 0x000c: // form feed
    case 0x000d: // carriage return
    case 0x0020: // space
    case 0x00a0: // non-breaking space
    case 0x2028: // line separator
    case 0x2029: // paragraph separator
        return true;
    default:
        return false;
    }
}

// Decodes the UTF-8 scalar at `offset`, reporting its byte length. A truncated
// or malformed sequence is reported as its lead byte alone: classification only
// has to be right for the well-formed whitespace scalars above, and advancing a
// single byte keeps this walk in step with HarfBuzz's cluster byte offsets.
std::uint32_t decode_utf8(std::string_view text, std::size_t offset,
                          std::size_t& length)
{
    const auto lead = static_cast<unsigned char>(text[offset]);
    length = 1;
    std::size_t continuations = 0;
    std::uint32_t code = lead;
    if ((lead & 0xf8u) == 0xf0u) {
        continuations = 3;
        code = lead & 0x07u;
    } else if ((lead & 0xf0u) == 0xe0u) {
        continuations = 2;
        code = lead & 0x0fu;
    } else if ((lead & 0xe0u) == 0xc0u) {
        continuations = 1;
        code = lead & 0x1fu;
    } else {
        return lead;
    }

    if (offset + continuations >= text.size())
        return lead;
    for (std::size_t i = 1; i <= continuations; ++i) {
        const auto byte = static_cast<unsigned char>(text[offset + i]);
        if ((byte & 0xc0u) != 0x80u)
            return lead;
        code = code << 6 | (byte & 0x3fu);
    }
    length = continuations + 1;
    return code;
}

// The extra advance an escapement_delta adds for each character of `text`, as
// (byte offset of the character, extra advance) pairs in byte order.
std::vector<std::pair<std::size_t, float>> character_deltas(
    std::string_view text, const EscapementDelta& delta)
{
    std::vector<std::pair<std::size_t, float>> result;
    for (std::size_t offset = 0; offset < text.size();) {
        std::size_t length = 1;
        const auto code = decode_utf8(text, offset, length);
        result.emplace_back(
            offset, is_haiku_white_space(code) ? delta.space : delta.nonspace);
        offset += length;
    }
    return result;
}

} // namespace

struct TextEngine::Impl {
    FT_Library library = nullptr;
    // Keyed by the *requested* style, and populated even when nothing resolved:
    // a negative result is as worth remembering as a positive one, and it is
    // what makes the diagnostic below fire exactly once per style rather than
    // once per measured string.
    struct Resolved {
        FT_Face face = nullptr;
        FaceChoice choice;
    };
    std::map<FaceKey, Resolved> faces;
    const Resolved unavailable;
    Log log;
    // Face bits already named in a diagnostic. Deliberately *not* folded into
    // the `faces` key: B_LIGHT_FACE and B_REGULAR_FACE resolve to the same
    // FaceKey, so a cache hit would have swallowed the warning for the light
    // one -- which is the same class of silence this change is about.
    std::uint16_t reported_unhonoured = 0;

    Impl()
    {
        if (FT_Init_FreeType(&library) != 0) {
            library = nullptr;
        } else {
            // Match Haiku's default subpixel renderer filtering.
            (void)FT_Library_SetLcdFilter(library, FT_LCD_FILTER_DEFAULT);
        }
    }

    ~Impl()
    {
        for (const auto& [key, resolved] : faces) {
            (void)key;
            if (resolved.face != nullptr)
                FT_Done_Face(resolved.face);
        }
        if (library != nullptr)
            FT_Done_FreeType(library);
    }

    // Walks the relaxation ladder, then -- only if every rung failed -- accepts
    // any file that opens at all, so an environment override pointing at an
    // unusual style still works as the last resort it has always been.
    Resolved select(const FaceKey& key) const
    {
        Resolved result;
        for (const auto& rung : style_relaxations(key)) {
            for (const auto& path : font_candidates(rung)) {
                if (!std::filesystem::exists(path))
                    continue;
                long index = 0;
                FT_Face face = open_matching(library, path, &rung, index);
                if (face == nullptr)
                    continue;
                result.face = face;
                result.choice.path = path;
                result.choice.index = index;
                result.choice.bold = rung.bold;
                result.choice.italic = rung.italic;
                result.choice.condensed = rung.condensed;
                result.choice.exact = style_matches(
                    FaceStyle {rung.bold, rung.italic, rung.condensed}, key);
                return result;
            }
        }

        const FaceKey plain {key.mono, false, false, false};
        for (const auto& path : font_candidates(plain)) {
            if (!std::filesystem::exists(path))
                continue;
            long index = 0;
            FT_Face face = open_matching(library, path, nullptr, index);
            if (face == nullptr)
                continue;
            const FaceStyle have = style_of(face);
            result.face = face;
            result.choice.path = path;
            result.choice.index = index;
            result.choice.bold = have.bold;
            result.choice.italic = have.italic;
            result.choice.condensed = have.condensed;
            result.choice.exact = style_matches(have, key);
            return result;
        }
        return result;
    }

    // Loud, and once per distinct style because `faces` caches the miss. A
    // substituted face means the width replied to the server is the width of a
    // style the server did not ask for, and RP_CAP_STRING_WIDTH_REPLY makes the
    // server take that answer as authoritative for layout -- so it has to be
    // said out loud rather than discovered by looking at the screen. app_server
    // itself refuses rather than approximates (ServerFont::SetFace returns
    // B_ERROR, src/servers/app/ServerFont.cpp:334-371), so there is nothing here
    // to match by guessing quietly.
    void report_style(const FaceKey& key, const FaceChoice& choice) const
    {
        if (!log)
            return;
        const char* pitch = key.mono ? "fixed-pitch" : "proportional";
        if (choice.path.empty()) {
            log(std::string("font: no usable font file for the ") + pitch + ' '
                + style_name(key)
                + " style; string widths will be estimated, not measured, and"
                  " will not match the server's layout");
        } else if (!choice.exact) {
            const FaceKey got {key.mono, choice.bold, choice.italic,
                               choice.condensed};
            log(std::string("font: no ") + pitch + ' ' + style_name(key)
                + " face on this host; measuring with " + style_name(got)
                + " instead (" + choice.path
                + ") -- widths replied for this font are another style's");
        }
    }

    // B_LIGHT_FACE and B_HEAVY_FACE pick a style in app_server and have no file
    // axis here, so they are dropped -- but said out loud once each, rather than
    // dropped quietly the way italic used to be.
    void report_unhonoured(const Font& font)
    {
        const auto unhonoured = static_cast<std::uint16_t>(
            font.face & (light_face | heavy_face) & ~reported_unhonoured);
        if (unhonoured == 0)
            return;
        reported_unhonoured |= unhonoured;
        if (!log)
            return;
        log(std::string("font: face bits ")
            + (unhonoured == light_face ? "B_LIGHT_FACE"
               : unhonoured == heavy_face ? "B_HEAVY_FACE"
                                          : "B_LIGHT_FACE|B_HEAVY_FACE")
            + " pick a style in app_server but have no file axis here; widths"
              " replied for this font are another weight's");
    }

    const Resolved& resolve(const Font& font)
    {
        if (library == nullptr)
            return unavailable;
        report_unhonoured(font);
        const FaceKey key = face_key(font);
        if (const auto found = faces.find(key); found != faces.end())
            return found->second;
        const Resolved resolved = select(key);
        report_style(key, resolved.choice);
        return faces.emplace(key, resolved).first->second;
    }

    FT_Face face_for(const Font& font) { return resolve(font).face; }

    struct Shaped {
        FT_Face face = nullptr;
        std::vector<hb_glyph_info_t> glyphs;
        std::vector<hb_glyph_position_t> positions;
        // Escapement-delta advance charged to each glyph, parallel to `glyphs`.
        std::vector<float> extra;
        // `advance` is the *untransformed* horizontal advance, which is what
        // the server's own answer to the same question is: ServerFont::
        // StringWidth uses StringWidthConsumer, whose `Finish(x, y)` keeps a
        // bare `x` with no embedded transform applied -- unlike
        // AGGTextRenderer's StringRenderer::Finish, which does transform the
        // reported pen position. Rotation therefore must NOT change this.
        float advance = 0;
        // Embedded rotate/shear transform for `face`, already installed on it.
        FT_Matrix matrix {};
        bool transformed = false;
    };

    // Charges every character's escapement delta to the last glyph of the
    // cluster it belongs to, so the extra advance lands between clusters, and
    // folds the same amounts into the total advance.
    //
    // Haiku adds the delta to a character's advance *after* the glyph is
    // emitted (GlyphLayoutEngine.h:349-352) and then folds the final advance
    // into the pen position it reports (GlyphLayoutEngine.h:367-369) -- so the
    // last character's delta moves the pen past the last glyph even though it
    // shifts no ink. Summing every character's delta into `advance` reproduces
    // that exactly.
    //
    // Haiku emits one glyph per character, so its rule is per-glyph; HarfBuzz
    // may map several characters to one glyph (a ligature) or one character to
    // several. Grouping by cluster keeps the total identical to Haiku's in
    // either case, and is per-glyph whenever the mapping is one to one.
    // `shaped.extra` arrives sized to the glyph run and zeroed.
    static void charge_delta(Shaped& shaped, std::string_view text,
                             const EscapementDelta& delta)
    {
        if (shaped.glyphs.empty() || text.empty())
            return;

        // Byte offset -> last glyph carrying that offset as its cluster.
        std::vector<int> cluster_owner(text.size(), -1);
        for (std::size_t i = 0; i < shaped.glyphs.size(); ++i) {
            const std::size_t at = shaped.glyphs[i].cluster;
            if (at < text.size())
                cluster_owner[at] = static_cast<int>(i);
        }

        // A character before the first cluster start (HarfBuzz should not
        // produce one, but nothing here depends on that) is charged to the
        // first glyph rather than dropped.
        int owner = 0;
        for (const auto& [offset, extra] : character_deltas(text, delta)) {
            if (cluster_owner[offset] >= 0)
                owner = cluster_owner[offset];
            shaped.extra[static_cast<std::size_t>(owner)] += extra;
            shaped.advance += extra;
        }
    }

    Shaped shape(std::string_view text, const Font& font,
                 const EscapementDelta* delta)
    {
        Shaped result;
        result.face = face_for(font);
        if (result.face == nullptr)
            return result;
        const auto pixel_size = static_cast<FT_UInt>(
            std::max(1L, std::lround(font.size)));
        FT_Set_Pixel_Sizes(result.face, 0, pixel_size);
        // Shaping runs on an *untransformed* face, and the matrix is only
        // handed to `draw` to install around the render loop.
        //
        // This is not a style choice. Measured here: with the matrix installed
        // before hb_shape, a 90-degree rotated "Hamburgefonstiv" at size 18
        // came back with a total advance of -0.19 px instead of 147.81 and
        // every glyph piled into a 19x16 box, because HarfBuzz's horizontal
        // advances came back rotated -- it reports no vertical advance for a
        // horizontal run, so the rotated advance simply vanished. It also made
        // `width()` rotation-dependent, which would contradict the server's own
        // fallback (ServerFont::StringWidth is untransformed).
        //
        // It matches app_server too: FontEngine::Init never calls
        // FT_Set_Transform, so Haiku's layout face is untransformed and only
        // the rendering path carries the embedded transform.
        //
        // The reset is unconditional because FT_Set_Transform is face state and
        // the face cache is shared between fonts: a previous rotated font's
        // matrix left installed would silently rotate this string's advances.
        result.transformed = font_is_transformed(font);
        result.matrix = font_matrix(font);
        install_transform(result.face, result.matrix, false);

        hb_font_t* hb_font = hb_ft_font_create_referenced(result.face);
        const auto load_flags = (font.flags & 0x00000001u) == 0
            ? FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD
            : FT_LOAD_DEFAULT | FT_LOAD_TARGET_MONO;
        // Haiku lays out text with FreeType's hinted advances. HarfBuzz
        // otherwise uses fractional linear advances, which makes terminal text
        // wider than the background cell rectangles sent by app_server.
        hb_ft_font_set_load_flags(hb_font, load_flags);
        hb_ft_font_changed(hb_font);
        hb_buffer_t* buffer = hb_buffer_create();
        hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0,
                           static_cast<int>(text.size()));
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(hb_font, buffer, nullptr, 0);

        unsigned count = 0;
        const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
        const auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
        result.glyphs.assign(infos, infos + count);
        result.positions.assign(positions, positions + count);
        for (const auto& position : result.positions)
            result.advance += static_cast<float>(position.x_advance) / 64.0f;
        result.extra.assign(result.glyphs.size(), 0.0f);
        if (delta != nullptr)
            charge_delta(result, text, *delta);

        hb_buffer_destroy(buffer);
        hb_font_destroy(hb_font);
        return result;
    }
};

TextEngine::TextEngine()
    : impl_(std::make_unique<Impl>())
{
}

TextEngine::TextEngine(Log log)
    : impl_(std::make_unique<Impl>())
{
    impl_->log = std::move(log);
}

TextEngine::~TextEngine() = default;

void TextEngine::set_log(Log log)
{
    impl_->log = std::move(log);
}

bool TextEngine::available() const
{
    return impl_->library != nullptr;
}

TextEngine::FaceChoice TextEngine::selected_face(const Font& font)
{
    return impl_->resolve(font).choice;
}

float TextEngine::width(std::string_view text, const Font& font,
                        const EscapementDelta* delta)
{
    const auto shaped = impl_->shape(text, font, delta);
    if (shaped.face != nullptr)
        return shaped.advance;
    std::size_t codepoints = 0;
    for (const unsigned char c : text)
        if ((c & 0xc0) != 0x80)
            ++codepoints;
    float estimate = static_cast<float>(codepoints) * font.size * 0.6f;
    if (delta != nullptr) {
        // No face, so no glyphs to charge -- but the delta is the server's own
        // arithmetic, not the font's, so it still belongs in the total.
        for (const auto& [offset, extra] : character_deltas(text, *delta)) {
            (void)offset;
            estimate += extra;
        }
    }
    return estimate;
}

float TextEngine::draw(std::string_view text, Point baseline,
                       const DrawState& state, Surface& surface,
                       const EscapementDelta* delta)
{
    const auto shaped = impl_->shape(text, state.font, delta);
    if (shaped.face == nullptr)
        return width(text, state.font, delta);

    // The view transform belongs in the glyph *outline*, not in the pixels the
    // outline rasterised to. Forward-mapping a finished raster through
    // map_point() lands each source pixel on one destination pixel and never
    // writes the gaps between them, so a scaled string came out as a lattice of
    // dots instead of a scaled string (issue #21).
    //
    // app_server puts it in the outline for the same reason: a transform that is
    // not translation-only makes StringRenderer::NeedsVector() true
    // (AGGTextRenderer.cpp:146), which selects glyph_ren_outline over the cached
    // bitmap (FontCacheEntry.cpp:429-435), and the outline is then pushed
    // through `transform = embedded; transform.TranslateBy(baseLine); transform
    // *= fViewTransformation` before the rasteriser sees it
    // (AGGTextRenderer.cpp:379-391).
    //
    // FT_Matrix_Multiply(a, b) computes `b = a * b`, so this composes to
    // view * embedded -- the server's order, with the font's own rotate/shear
    // innermost. A view transform with an identity linear part composes exactly
    // (FT_MulFix by 0x10000 is lossless), so the untransformed path is
    // untouched.
    FT_Matrix render_matrix = shaped.matrix;
    FT_Matrix view = view_matrix(state.transform);
    FT_Matrix_Multiply(&view, &render_matrix);
    const bool render_transformed = shaped.transformed
        || !view_is_linear_identity(state.transform);

    // Installed for the render pass only -- `shape` deliberately left the face
    // untransformed so HarfBuzz's advances stay in layout space. `shape` resets
    // it on every call, so it need not be undone here.
    install_transform(shaped.face, render_matrix, render_transformed);

    // The view transform's *translation* stays out of the matrix and rides on
    // the baseline instead, which is what the server does: it translates by the
    // baseline inside the transform and then multiplies the view transform in
    // (AGGTextRenderer.cpp:379-381), so the baseline lands at
    // viewTransformation(baseLine). DrawState::map_point is that mapping,
    // including Painter::SetTransform's translate(-offset)/translate(+offset)
    // bracket around the affine (Painter.cpp:372-383), and it returns the point
    // unchanged for an identity transform.
    const Point origin = state.map_point(baseline);

    // Accumulated baseline offset in FreeType's y-up space, *before* the
    // embedded rotate/shear transform -- exactly the x/y Haiku's
    // GlyphLayoutEngine hands its consumer. Both the glyph outline (via
    // FT_Set_Transform above) and this offset go through the same matrix, which
    // is what makes a rotated string march off along the rotated baseline
    // instead of staying horizontal.
    float offset_x = 0;
    float offset_y = 0;
    // 26.6 fixed point, the units FT_Outline_EmboldenXY works in.
    const auto embolden_strength = static_cast<FT_Pos>(
        std::lround(static_cast<double>(state.font.false_bold_width) * 64.0));
    for (std::size_t i = 0; i < shaped.glyphs.size(); ++i) {
        const auto& info = shaped.glyphs[i];
        const auto& position = shaped.positions[i];
        const bool antialias = (state.font.flags & 0x00000001u) == 0;
        const auto load_flags = antialias
            ? FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD
            : FT_LOAD_DEFAULT | FT_LOAD_TARGET_MONO;
        const auto render_mode = antialias
            ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_MONO;
        if (FT_Load_Glyph(shaped.face, info.codepoint, load_flags) == 0
            && embolden_glyph(shaped.face->glyph, embolden_strength)
            && glyph_is_renderable(shaped.face->glyph)
            && FT_Render_Glyph(shaped.face->glyph, render_mode) == 0) {
            const auto& glyph = *shaped.face->glyph;
            const float glyph_x = offset_x
                + static_cast<float>(position.x_offset) / 64.0f;
            const float glyph_y = offset_y
                + static_cast<float>(position.y_offset) / 64.0f;
            const auto [placed_x, placed_y] = render_transformed
                ? transform_point(render_matrix, glyph_x, glyph_y)
                : std::pair<float, float> {glyph_x, glyph_y};
            const int origin_x = static_cast<int>(
                std::floor(origin.x + placed_x)) + glyph.bitmap_left;
            const int origin_y = static_cast<int>(
                std::floor(origin.y - placed_y)) - glyph.bitmap_top;
            const auto pitch = static_cast<unsigned>(std::abs(glyph.bitmap.pitch));
            for (unsigned row = 0; row < glyph.bitmap.rows; ++row) {
                const auto source_row = glyph.bitmap.pitch >= 0
                    ? row : glyph.bitmap.rows - 1 - row;
                const auto* pixels = glyph.bitmap.buffer + source_row * pitch;
                if (glyph.bitmap.pixel_mode == FT_PIXEL_MODE_LCD) {
                    const unsigned pixel_width = glyph.bitmap.width / 3;
                    for (unsigned column = 0; column < pixel_width; ++column) {
                        const auto red = pixels[column * 3];
                        const auto green = pixels[column * 3 + 1];
                        const auto blue = pixels[column * 3 + 2];
                        if (red == 0 && green == 0 && blue == 0)
                            continue;
                        surface.paint_subpixel_coverage(
                            origin_x + static_cast<int>(column),
                            origin_y + static_cast<int>(row),
                            state.high, state, red, green, blue);
                    }
                } else if (glyph.bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                    for (unsigned column = 0; column < glyph.bitmap.width; ++column) {
                        if ((pixels[column / 8] & (0x80u >> (column & 7))) == 0)
                            continue;
                        surface.paint_coverage(
                            origin_x + static_cast<int>(column),
                            origin_y + static_cast<int>(row),
                            state.high, state, 255, true);
                    }
                } else {
                    for (unsigned column = 0; column < glyph.bitmap.width; ++column) {
                        const auto coverage = pixels[column];
                        if (coverage == 0)
                            continue;
                        surface.paint_coverage(
                            origin_x + static_cast<int>(column),
                            origin_y + static_cast<int>(row),
                            state.high, state, coverage, true);
                    }
                }
            }
        }
        offset_x += static_cast<float>(position.x_advance) / 64.0f
            + shaped.extra[i];
        offset_y += static_cast<float>(position.y_advance) / 64.0f;
    }
    // The untransformed advance, matching ServerFont::StringWidth (see the note
    // on Shaped::advance). The caller turns it into a pen position of
    // {where.x + advance, where.y}, which is still wrong for a rotated string
    // -- app_server's StringRenderer::Finish transforms that point. Fixing it
    // means changing the RP_DRAW_STRING reply in session.cpp; that file is not
    // touched here.
    return shaped.advance;
}

} // namespace haiku_remote
