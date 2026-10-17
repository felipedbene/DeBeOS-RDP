// RP_CAP_FONT_IDENTITY and server-faithful text layout.
//
// The oracle here is FreeType driven the way app_server drives it, restated in
// this file from the server's source and sharing no code with text_engine.cpp:
//
//   - one glyph per UTF8ToCharCode code, no shaping
//     (GlyphLayoutEngine::LayoutGlyphs, src/servers/app/font/
//     GlyphLayoutEngine.h:256-372);
//   - the face sized to the *truncated* size (FontEngine::Init,
//     src/servers/app/font/FontEngine.cpp:688-690);
//   - glyphs loaded with FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL when subpixel
//     antialiasing is off (FontEngine::PrepareGlyph, FontEngine.cpp:486-490),
//     rendered FT_RENDER_MODE_NORMAL, advanced by the hinted advance;
//   - each cached bitmap placed at AGG's iround of the pen position
//     (agg_scanline_storage_aa.h:684-685, agg_basics.h:188-191);
//   - coverage blended with B_OP_OVER's BLEND (src/servers/app/drawing/Painter/
//     drawing_modes/DrawingMode.h:28-36).
//
// The face it draws is chosen by this file too: any installed face whose family
// is *not* the one trait selection would pick, so a client that ignores the
// identity block draws a different face and fails every raster check.

#include "haiku_remote/protocol.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace haiku_remote;

namespace {

int checks = 0;
int failures = 0;
int skips = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void skip(std::string_view message, std::string_view reason)
{
    ++skips;
    std::cout << "SKIP: " << message << " (" << reason << ")\n";
}

std::string lower(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// -- The wire ---------------------------------------------------------------

// One whole RP_SET_FONT frame with an identity block, spelled out byte by byte
// from RemoteMessage::AddFont and RemoteMessage::AddFontIdentity -- not built
// with Writer, so a change to the field order on either side shows up here.
const std::vector<std::uint8_t> golden_set_font = {
    48, 0, 91, 0, 0, 0,             // RP_SET_FONT, 91 bytes including the header
    7, 0, 0, 0,                     // token 7
    0,                              // direction
    0,                              // encoding
    0, 0, 0, 0,                     // flags
    2,                              // spacing: B_BITMAP_SPACING
    0x00, 0x00, 0xb4, 0x42,         // shear 90.0f
    0, 0, 0, 0,                     // rotation
    0, 0, 0, 0,                     // false bold width
    0x00, 0x00, 0x40, 0x41,         // size 12.0f
    0x20, 0x00,                     // face B_BOLD_FACE
    0x05, 0x00, 0x03, 0x00,         // familyAndStyle: family 3, style 5
    9, 0, 0, 0, 'N', 'o', 't', 'o', ' ', 'S', 'a', 'n', 's',
    4, 0, 0, 0, 'B', 'o', 'l', 'd',
    0x01, 0x00, 0x02, 0x00,         // faceIndex: face 1, named instance 2
    0x78, 0x56, 0x34, 0x12,         // head checkSumAdjustment
    0x00, 0x80, 0x02, 0x00,         // head fontRevision 2.5
    0x01,                           // renderFlags: hinting, no subpixel
    11, 0, 0, 0, '/', 'b', 'o', 'o', 't', '/', 'a', '.', 't', 't', 'f',
    0xee, 0xee, 0xee,               // a later revision's fields
};

void test_the_identity_block_decodes_from_golden_bytes()
{
    Framer framer;
    const auto messages = framer.feed(golden_set_font);
    check(messages.size() == 1 && messages[0].op == Op::set_font,
          "the golden RP_SET_FONT frames as exactly one RP_SET_FONT");
    if (messages.size() != 1)
        return;
    Reader reader(messages[0].payload);
    check(reader.i32() == 7, "the token comes first");
    const Font font = reader.font();
    check(font.spacing == bitmap_spacing && font.size == 12.0f
              && font.face == 0x0020 && font.family == 3 && font.style == 5,
          "the 29-byte font record is unchanged by the block behind it");
    const FontIdentity id = reader.font_identity();
    check(id.family == "Noto Sans", "family name");
    check(id.style == "Bold", "style name");
    check(id.face_index == 0x00020001u, "face index, named instance high");
    check(id.checksum_adjustment == 0x12345678u, "head checkSumAdjustment");
    check(id.font_revision == 0x00028000u, "head fontRevision");
    check(id.hinting && !id.subpixel, "renderFlags bit 0 is hinting, bit 1 subpixel");
    check(id.path == "/boot/a.ttf", "server-side path");
    check(reader.remaining() == 3,
          "bytes after the last known field are left for a later revision");
}

void test_the_capability_bit_is_bit_five()
{
    // RemoteProtocol.h: RP_CAP_FONT_IDENTITY = 1 << 5 (bit 4 is
    // RP_CAP_SECURE_ATTENTION). Pinned as a literal, not compared with itself.
    check(cap_font_identity == 0x20u, "RP_CAP_FONT_IDENTITY is 0x20");
}

// -- The oracle -------------------------------------------------------------

struct OracleFace {
    std::string path;
    std::string family;
    std::string style;
    std::uint32_t checksum = 0;
    std::uint32_t revision = 0;
};

constexpr std::string_view sample = "Office AVA fi Hamburgefonstiv";

bool covers(FT_Face face, std::string_view text)
{
    for (const unsigned char c : text)
        if (FT_Get_Char_Index(face, c) == 0)
            return false;
    return true;
}

// The first static, scalable, proportional face under the standard font
// directories -- in sorted path order, so the choice is deterministic -- whose
// family is not `avoid` and which covers the sample.
std::optional<OracleFace> find_other_face(FT_Library library,
                                          std::string_view avoid,
                                          bool need_kerning = false)
{
    std::vector<std::string> paths;
    std::error_code ec;
    for (const char* dir : {"/boot/system/data/fonts", "/usr/share/fonts",
                            "/System/Library/Fonts", "/Library/Fonts",
                            "C:/Windows/Fonts"}) {
        if (!std::filesystem::is_directory(dir, ec))
            continue;
        for (std::filesystem::recursive_directory_iterator it(
                 dir, std::filesystem::directory_options::skip_permission_denied,
                 ec), end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto ext = lower(it->path().extension().string());
            if (it->is_regular_file(ec) && (ext == ".ttf" || ext == ".otf"))
                paths.push_back(it->path().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    // HAIKU_REMOTE_TEST_FACE pins the face, to reproduce a failure seen with
    // one in particular (the --torture mock names a serif, for instance).
    if (const char* pinned = std::getenv("HAIKU_REMOTE_TEST_FACE"); pinned != nullptr
        && !need_kerning) {
        paths.insert(paths.begin(), pinned);
    }
    for (const auto& path : paths) {
        FT_Face face = nullptr;
        if (FT_New_Face(library, path.c_str(), 0, &face) != 0)
            continue;
        const bool usable = face->family_name != nullptr
            && face->style_name != nullptr && FT_IS_SCALABLE(face)
            && !FT_HAS_MULTIPLE_MASTERS(face) && !FT_IS_FIXED_WIDTH(face)
            && face->num_faces == 1
            && lower(face->family_name) != lower(avoid)
            && lower(face->family_name).find("mono") == std::string::npos
            && FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0
            && covers(face, sample) && (!need_kerning || FT_HAS_KERNING(face));
        if (usable) {
            OracleFace result;
            result.path = path;
            result.family = face->family_name;
            result.style = face->style_name;
            if (const auto* head = static_cast<const TT_Header*>(
                    FT_Get_Sfnt_Table(face, FT_SFNT_HEAD))) {
                result.checksum = static_cast<std::uint32_t>(head->CheckSum_Adjust);
                result.revision = static_cast<std::uint32_t>(head->Font_Revision);
            }
            FT_Done_Face(face);
            if (result.checksum != 0)
                return result;
            continue;
        }
        FT_Done_Face(face);
    }
    return std::nullopt;
}

std::string family_of(FT_Library library, const std::string& path, long index)
{
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), index, &face) != 0)
        return {};
    std::string result = face->family_name != nullptr ? face->family_name : "";
    FT_Done_Face(face);
    return result;
}

int agg_iround(double value)
{
    return static_cast<int>(value < 0.0 ? value - 0.5 : value + 0.5);
}

// The final pen x app_server reaches for `text` in `path` at `size`: hinted
// advances, the B_CHAR_SPACING unhinted advance times the untruncated size, or
// B_STRING_SPACING kerning -- through FT_Get_Kerning with character codes, as
// FontEngine::GetKerning passes them (FontEngine.cpp:621-637).
double oracle_width(FT_Library library, const std::string& path,
                    std::string_view text, float size, std::uint8_t spacing,
                    bool hinting = true)
{
    // Without hinting a font goes down glyph_ren_outline
    // (FontCacheEntry::_RenderTypeFor), still FT_LOAD_TARGET_NORMAL, plus
    // FT_LOAD_NO_HINTING (FontEngine::PrepareGlyph).
    const FT_Int32 load = FT_LOAD_TARGET_NORMAL
        | (hinting ? FT_LOAD_DEFAULT : FT_LOAD_NO_HINTING);
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face) != 0)
        return -1;
    FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    const auto ppem = static_cast<FT_UInt>(static_cast<unsigned>(size * 64.0) >> 6);
    FT_Set_Pixel_Sizes(face, ppem, ppem);
    double x = 0;
    double advance = 0;
    unsigned char last = 0;
    for (const unsigned char c : text) {
        if (spacing == string_spacing && last != 0 && FT_HAS_KERNING(face)) {
            FT_Vector kerning {};
            FT_Get_Kerning(face, last, c, FT_KERNING_DEFAULT, &kerning);
            advance += kerning.x / 64.0;
        }
        x += advance;
        const FT_UInt index = FT_Get_Char_Index(face, c);
        if (spacing == char_spacing) {
            FT_Load_Glyph(face, index,
                          load | FT_LOAD_NO_HINTING | FT_LOAD_NO_SCALE);
            advance = static_cast<double>(face->glyph->advance.x)
                / face->units_per_EM * size;
        } else {
            FT_Load_Glyph(face, index, load);
            advance = face->glyph->advance.x / 64.0;
        }
        last = c;
    }
    x += advance;
    FT_Done_Face(face);
    return x;
}

// `text` drawn by app_server in black over white with B_OP_OVER, gray8 glyphs,
// at `baseline`: the expected surface, as rows of {r, g, b}.
std::vector<Color> oracle_raster(FT_Library library, const std::string& path,
                                 std::string_view text, float size,
                                 Point baseline, int width, int height)
{
    std::vector<Color> pixels(static_cast<std::size_t>(width * height),
                              Color {255, 255, 255, 255});
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face) != 0)
        return pixels;
    FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    const auto ppem = static_cast<FT_UInt>(static_cast<unsigned>(size * 64.0) >> 6);
    FT_Set_Pixel_Sizes(face, ppem, ppem);
    double x = 0;
    double advance = 0;
    for (const unsigned char c : text) {
        x += advance;
        const FT_UInt index = FT_Get_Char_Index(face, c);
        if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL) != 0
            || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            advance = 0;
            continue;
        }
        advance = face->glyph->advance.x / 64.0;
        const auto& bitmap = face->glyph->bitmap;
        const int left = agg_iround(baseline.x + x) + face->glyph->bitmap_left;
        const int top = agg_iround(baseline.y) - face->glyph->bitmap_top;
        for (unsigned row = 0; row < bitmap.rows; ++row) {
            for (unsigned column = 0; column < bitmap.width; ++column) {
                const int cover = bitmap.buffer[row * bitmap.pitch + column];
                const int px = left + static_cast<int>(column);
                const int py = top + static_cast<int>(row);
                if (cover == 0 || px < 0 || py < 0 || px >= width || py >= height)
                    continue;
                auto& d = pixels[static_cast<std::size_t>(py * width + px)];
                // DrawingMode.h:28-36, BLEND with a black source.
                const auto blend = [cover](std::uint8_t channel) {
                    return static_cast<std::uint8_t>(
                        (((0 - channel) * cover) + (channel << 8)) >> 8);
                };
                d = {blend(d.r), blend(d.g), blend(d.b), 255};
            }
        }
    }
    FT_Done_Face(face);
    return pixels;
}

bool same_pixels(const Surface& surface, const std::vector<Color>& expected,
                 int* mismatches = nullptr)
{
    int count = 0;
    for (int y = 0; y < surface.height(); ++y) {
        for (int x = 0; x < surface.width(); ++x) {
            const auto got = surface.pixel(x, y);
            const auto& want =
                expected[static_cast<std::size_t>(y * surface.width() + x)];
            if (got.r != want.r || got.g != want.g || got.b != want.b)
                ++count;
        }
    }
    if (mismatches != nullptr)
        *mismatches = count;
    return count == 0;
}

std::shared_ptr<const FontIdentity> identity_of(const OracleFace& face,
                                                bool subpixel = false)
{
    auto id = std::make_shared<FontIdentity>();
    id->family = face.family;
    id->style = face.style;
    id->checksum_adjustment = face.checksum;
    id->font_revision = face.revision;
    id->hinting = true;
    id->subpixel = subpixel;
    id->path = "/boot/system/data/fonts/ttfonts/whatever.ttf";
    return id;
}

// -- A session, end to end --------------------------------------------------

constexpr int surface_width = 360;
constexpr int surface_height = 60;
constexpr float sample_size = 15.0f;
// Fractional on both axes: app_server rounds a cached glyph's position half
// away from zero, so a client that floors lands every glyph a pixel off.
constexpr Point sample_baseline {10.5f, 40.5f};

void append_font_record(Writer& writer, float size, std::uint8_t spacing)
{
    writer.u8(0);       // direction
    writer.u8(0);       // encoding
    writer.u32(0);      // flags
    writer.u8(spacing);
    writer.f32(neutral_font_shear);
    writer.f32(0);      // rotation
    writer.f32(0);      // false bold width
    writer.f32(size);
    writer.u16(0);      // face
    writer.u32(0x00070002u);
}

void append_identity(Writer& writer, const FontIdentity& id)
{
    writer.string(id.family);
    writer.string(id.style);
    writer.u32(id.face_index);
    writer.u32(id.checksum_adjustment);
    writer.u32(id.font_revision);
    writer.u8(static_cast<std::uint8_t>((id.hinting ? 1 : 0)
                                        | (id.subpixel ? 2 : 0)));
    writer.string(id.path);
}

Surface session_render(bool negotiate, const FontIdentity& id,
                       std::string_view text)
{
    Session session(surface_width, surface_height,
                    [](std::span<const std::uint8_t>) { return true; });
    session.set_compression_offered(false);
    session.start();
    if (negotiate) {
        Writer ack(Op::hello_ack);
        ack.u32(protocol_version);
        ack.u32(cap_font_identity);
        session.ingest(ack.finish());
    }
    constexpr std::int32_t token = 7;
    Writer mode(Op::set_drawing_mode);
    mode.i32(token);
    mode.u32(1); // B_OP_OVER
    session.ingest(mode.finish());
    Writer fill(Op::fill_rect_color);
    fill.i32(token);
    fill.f32(0);
    fill.f32(0);
    fill.f32(surface_width - 1);
    fill.f32(surface_height - 1);
    fill.u8(255); fill.u8(255); fill.u8(255); fill.u8(255);
    session.ingest(fill.finish());
    Writer high(Op::set_high_color);
    high.i32(token);
    high.u8(0); high.u8(0); high.u8(0); high.u8(255);
    session.ingest(high.finish());
    Writer font(Op::set_font);
    font.i32(token);
    append_font_record(font, sample_size, bitmap_spacing);
    append_identity(font, id);
    session.ingest(font.finish());
    Writer draw(Op::draw_string);
    draw.i32(token);
    draw.point(sample_baseline);
    draw.string(text);
    draw.boolean(false);
    session.ingest(draw.finish());
    Surface result(surface_width, surface_height);
    result.clear({255, 255, 255, 255});
    for (int y = 0; y < surface_height; ++y)
        for (int x = 0; x < surface_width; ++x)
            result.set_pixel(x, y, session.surface().pixel(x, y));
    return result;
}

struct Fixture {
    FT_Library library = nullptr;
    std::optional<OracleFace> named;
    // A face with a legacy 'kern' table, for B_STRING_SPACING.
    std::optional<OracleFace> kerned;
    std::string trait_path;
    ~Fixture()
    {
        if (library != nullptr)
            FT_Done_FreeType(library);
    }
};

Fixture& fixture()
{
    static Fixture instance = [] {
        Fixture f;
        FT_Init_FreeType(&f.library);
        TextEngine engine;
        Font trait;
        trait.size = sample_size;
        const auto choice = engine.selected_face(trait);
        f.trait_path = choice.path;
        const auto trait_family = family_of(f.library, choice.path, choice.index);
        f.named = find_other_face(f.library, trait_family);
        f.kerned = find_other_face(f.library, trait_family, true);
        return f;
    }();
    return instance;
}

void test_a_named_face_draws_exactly_as_app_server_draws_it()
{
    auto& f = fixture();
    if (!f.named || f.trait_path.empty()) {
        skip("a named face draws exactly as app_server draws it",
             "this host has no second font family to name");
        return;
    }
    std::cout << "note: named face " << f.named->family << " / "
              << f.named->style << " (" << f.named->path << "); trait face "
              << f.trait_path << '\n';
    const auto expected = oracle_raster(f.library, f.named->path, sample,
                                        sample_size, sample_baseline,
                                        surface_width, surface_height);
    const auto trait_expected = oracle_raster(f.library, f.trait_path, sample,
                                              sample_size, sample_baseline,
                                              surface_width, surface_height);
    check(expected != trait_expected,
          "precondition: the named face and the trait face rasterise"
          " differently, so the comparison below can tell them apart");

    const auto drawn = session_render(true, *identity_of(*f.named), sample);
    int mismatches = 0;
    check(same_pixels(drawn, expected, &mismatches),
          "RP_SET_FONT naming a face draws the string pixel-identical to"
          " FreeType driven with app_server's flags, layout and placement ("
              + std::to_string(mismatches) + " pixels differ)");
}

void test_the_block_is_ignored_unless_negotiated()
{
    auto& f = fixture();
    if (!f.named) {
        skip("the identity block is ignored unless negotiated",
             "this host has no second font family to name");
        return;
    }
    const auto expected = oracle_raster(f.library, f.named->path, sample,
                                        sample_size, sample_baseline,
                                        surface_width, surface_height);
    const auto drawn = session_render(false, *identity_of(*f.named), sample);
    check(!same_pixels(drawn, expected),
          "without RP_CAP_FONT_IDENTITY agreed, trailing RP_SET_FONT bytes do"
          " not select a face");

    // What it does draw is the identity-less font, from the trait choice.
    TextEngine engine;
    DrawState state;
    state.high = {0, 0, 0, 255};
    state.drawing_mode = DrawingMode::over;
    state.font.size = sample_size;
    state.font.spacing = bitmap_spacing;
    state.font.family = 7;
    state.font.style = 2;
    Surface reference(surface_width, surface_height);
    reference.clear({255, 255, 255, 255});
    engine.draw(sample, sample_baseline, state, reference);
    check(std::ranges::equal(drawn.pixels(), reference.pixels()),
          "an unnegotiated RP_SET_FONT draws exactly what the bare record draws");
}

// -- Resolution --------------------------------------------------------------

void test_resolution_prefers_the_servers_file()
{
    auto& f = fixture();
    if (!f.named) {
        skip("resolution by identity", "this host has no second font family");
        return;
    }
    std::vector<std::string> said;
    TextEngine engine([&](std::string_view line) { said.emplace_back(line); });
    Font font;
    font.size = sample_size;

    font.identity = identity_of(*f.named);
    auto choice = engine.selected_face(font);
    check(choice.path == f.named->path && choice.index == 0,
          "the named face's own file is chosen");
    check(choice.named && choice.identical && choice.exact,
          "and reported as the server's identical file");
    check(said.empty(), "an identical file is chosen silently");

    // Same names, other bytes: still that face, but said out loud, once.
    auto other = std::make_shared<FontIdentity>(*identity_of(*f.named));
    other->checksum_adjustment ^= 0x5a5a5a5au;
    font.identity = other;
    choice = engine.selected_face(font);
    (void)engine.selected_face(font);
    check(choice.path == f.named->path && choice.named && !choice.identical,
          "a name match with another checksum is the same face, not the same"
          " file");
    check(said.size() == 1
              && said[0].find("not the same file") != std::string::npos,
          "and that is said exactly once");

    // The file's bytes win over its name.
    auto renamed = std::make_shared<FontIdentity>(*identity_of(*f.named));
    renamed->family = "No Such Family 4242";
    renamed->style = "Nonexistent";
    font.identity = renamed;
    choice = engine.selected_face(font);
    check(choice.path == f.named->path && choice.identical,
          "a file found by its 'head' identity is used whatever it is called");

    // Neither name nor bytes: the trait choice, said once.
    said.clear();
    auto missing = std::make_shared<FontIdentity>();
    missing->family = "No Such Family 4242";
    missing->style = "Regular";
    missing->checksum_adjustment = 0x0badf00du;
    font.identity = missing;
    choice = engine.selected_face(font);
    (void)engine.selected_face(font);
    check(!choice.named && choice.path == f.trait_path,
          "an unknown face falls back to the trait choice");
    check(said.size() == 1
              && said[0].find("not installed here") != std::string::npos,
          "and the fallback is said exactly once");
}

// -- Layout --------------------------------------------------------------------

void test_widths_follow_app_servers_layout()
{
    auto& f = fixture();
    if (!f.named) {
        skip("layout widths", "this host has no second font family");
        return;
    }
    TextEngine engine;
    Font font;
    font.size = sample_size;
    font.identity = identity_of(*f.named);

    for (const std::uint8_t spacing : {char_spacing, string_spacing,
                                       bitmap_spacing, fixed_spacing}) {
        font.spacing = spacing;
        const auto want = static_cast<float>(
            oracle_width(f.library, f.named->path, sample, sample_size, spacing));
        check(engine.width(sample, font) == want,
              "the width at spacing " + std::to_string(spacing)
                  + " is app_server's: " + std::to_string(want) + ", measured "
                  + std::to_string(engine.width(sample, font)));
    }

    // The ppem is the size truncated, not rounded.
    font.spacing = bitmap_spacing;
    font.size = 15.75f;
    const auto truncated = static_cast<float>(
        oracle_width(f.library, f.named->path, sample, 15.0f, bitmap_spacing));
    const auto rounded = static_cast<float>(
        oracle_width(f.library, f.named->path, sample, 16.0f, bitmap_spacing));
    check(truncated != rounded,
          "precondition: 15 and 16 ppem lay the sample out differently");
    check(engine.width(sample, font) == truncated,
          "a 15.75 font is laid out at 15 ppem, as FontEngine::Init sizes it");
}

// B_STRING_SPACING kerns with FT_Get_Kerning(face, lastCharCode, charCode) --
// character codes where FreeType wants glyph indices (FontEngine::GetKerning,
// FontEngine.cpp:621-637, called from GlyphLayoutEngine.h:317-318). Whatever
// that looks up is what the server lays out with, so the test looks for a pair
// of printable characters for which it is non-zero, and lays that out.
void test_string_spacing_kerns_the_way_app_server_does()
{
    auto& f = fixture();
    if (!f.kerned) {
        skip("B_STRING_SPACING kerning", "no installed face has a 'kern' table");
        return;
    }
    FT_Face face = nullptr;
    std::string pair;
    const auto ppem = static_cast<FT_UInt>(sample_size);
    if (FT_New_Face(f.library, f.kerned->path.c_str(), 0, &face) == 0) {
        FT_Set_Pixel_Sizes(face, ppem, ppem);
        for (unsigned a = 0x21; a < 0x7f && pair.empty(); ++a) {
            for (unsigned b = 0x21; b < 0x7f; ++b) {
                FT_Vector kerning {};
                if (FT_Get_Kerning(face, a, b, FT_KERNING_DEFAULT, &kerning) == 0
                    && kerning.x != 0) {
                    pair = {static_cast<char>(a), static_cast<char>(b)};
                    break;
                }
            }
        }
        FT_Done_Face(face);
    }
    if (pair.empty()) {
        skip("B_STRING_SPACING kerning",
             "the kerned face kerns no printable character-code pair");
        return;
    }
    const std::string text = "x" + pair + pair + "x";
    TextEngine engine;
    Font font;
    font.size = sample_size;
    font.identity = identity_of(*f.kerned);
    font.spacing = string_spacing;
    const auto kerned = static_cast<float>(
        oracle_width(f.library, f.kerned->path, text, sample_size, string_spacing));
    const auto unkerned = static_cast<float>(
        oracle_width(f.library, f.kerned->path, text, sample_size, bitmap_spacing));
    check(kerned != unkerned,
          "precondition: \"" + text + "\" kerns in " + f.kerned->path);
    check(engine.width(text, font) == kerned,
          "B_STRING_SPACING kerns by character code, as app_server does: "
              + std::to_string(kerned) + ", measured "
              + std::to_string(engine.width(text, font)));
}

// HINTING_MODE_OFF (or monospaced-only, for a proportional font) reaches the
// client as renderFlags bit 0 clear, and lays out on unhinted advances.
void test_the_servers_hinting_decision_is_honoured()
{
    auto& f = fixture();
    if (!f.named) {
        skip("hinting decision", "this host has no second font family");
        return;
    }
    TextEngine engine;
    Font font;
    font.size = sample_size;
    auto unhinted = std::make_shared<FontIdentity>(*identity_of(*f.named));
    unhinted->hinting = false;
    font.identity = unhinted;
    const auto want = static_cast<float>(oracle_width(
        f.library, f.named->path, sample, sample_size, bitmap_spacing, false));
    const auto hinted = static_cast<float>(oracle_width(
        f.library, f.named->path, sample, sample_size, bitmap_spacing, true));
    check(want != hinted,
          "precondition: hinted and unhinted advances differ in this face");
    check(engine.width(sample, font) == want,
          "a font the server does not hint is laid out on unhinted advances");
}

// The hinted advance of one code in the face at `path`/`index`, or -1 when the
// face has no glyph for it.
double oracle_advance(FT_Library library, const std::string& path, long index,
                      std::uint32_t code, float size)
{
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), index, &face) != 0)
        return -1;
    FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    const auto ppem = static_cast<FT_UInt>(static_cast<unsigned>(size * 64.0) >> 6);
    FT_Set_Pixel_Sizes(face, ppem, ppem);
    double result = -1;
    if (const FT_UInt glyph = FT_Get_Char_Index(face, code); glyph != 0
        && FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL)
            == 0) {
        result = face->glyph->advance.x / 64.0;
    }
    FT_Done_Face(face);
    return result;
}

std::string hex_code(std::uint32_t code)
{
    char text[16];
    std::snprintf(text, sizeof text, "%04X", static_cast<unsigned>(code));
    return text;
}

std::string utf8(std::uint32_t code)
{
    std::string out;
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xc0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        out += static_cast<char>(0xe0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
    return out;
}

// A code the font has no glyph for comes from the first fallback family that
// has one -- "Noto Sans" leads GlyphLayoutEngine::PopulateFallbacks' list
// (GlyphLayoutEngine.h:418-426), tried at the font's own style first -- with
// that face's advance (FontCacheEntry::CreateGlyph, FontCacheEntry.cpp:
// 296-348). Without the fallback it would be the font's .notdef.
void test_a_missing_glyph_comes_from_the_fallback_family()
{
    auto& f = fixture();
    if (!f.named) {
        skip("glyph fallback", "this host has no second font family");
        return;
    }
    // Every "Noto Sans" face at the named face's style or Regular, static or
    // a variable font's named instance: the engine may pick any of them, and
    // they need not agree, so the check is "one of these, exactly".
    struct Candidate { std::string path; long index; };
    std::vector<Candidate> noto;
    std::error_code ec;
    for (const char* dir : {"/boot/system/data/fonts", "/usr/share/fonts",
                            "/System/Library/Fonts", "/Library/Fonts",
                            "C:/Windows/Fonts"}) {
        if (!std::filesystem::is_directory(dir, ec))
            continue;
        for (std::filesystem::recursive_directory_iterator it(
                 dir, std::filesystem::directory_options::skip_permission_denied,
                 ec), end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto ext = lower(it->path().extension().string());
            if (!it->is_regular_file(ec) || (ext != ".ttf" && ext != ".otf"))
                continue;
            const std::string path = it->path().string();
            FT_Face face = nullptr;
            if (FT_New_Face(f.library, path.c_str(), 0, &face) != 0)
                continue;
            const long instances = face->style_flags >> 16;
            FT_Done_Face(face);
            for (long instance = 0; instance <= instances; ++instance) {
                const long index = instance << 16;
                if (FT_New_Face(f.library, path.c_str(), index, &face) != 0)
                    continue;
                const std::string family = face->family_name ? face->family_name : "";
                const std::string style = face->style_name ? face->style_name : "";
                FT_Done_Face(face);
                if (family == "Noto Sans"
                    && (style == f.named->style || style == "Regular")) {
                    noto.push_back({path, index});
                }
            }
        }
    }
    if (noto.empty()) {
        skip("glyph fallback", "no Noto Sans face is installed");
        return;
    }
    // A code the named face lacks and the first candidate has.
    std::uint32_t code = 0;
    FT_Face named = nullptr;
    if (FT_New_Face(f.library, f.named->path.c_str(), 0, &named) == 0) {
        FT_Select_Charmap(named, FT_ENCODING_UNICODE);
        for (std::uint32_t c = 0xa1; c < 0x2000 && code == 0; ++c) {
            if (FT_Get_Char_Index(named, c) == 0
                && oracle_advance(f.library, noto[0].path, noto[0].index, c,
                                  sample_size) > 0) {
                code = c;
            }
        }
        FT_Done_Face(named);
    }
    if (code == 0) {
        skip("glyph fallback", "the named face lacks nothing Noto Sans has");
        return;
    }
    const std::string text = "a" + utf8(code) + "a";
    const double a = oracle_advance(f.library, f.named->path, 0, 'a', sample_size);
    TextEngine engine;
    Font font;
    font.size = sample_size;
    font.identity = identity_of(*f.named);
    const float measured = engine.width(text, font);
    bool matched = false;
    for (const auto& candidate : noto) {
        const double middle = oracle_advance(f.library, candidate.path,
                                             candidate.index, code, sample_size);
        if (middle > 0 && measured == static_cast<float>(a + middle + a))
            matched = true;
    }
    check(matched,
          "U+" + hex_code(code) + ", missing from the named face, is laid"
          " out with a Noto Sans advance (measured " + std::to_string(measured)
              + ")");
}

void test_unshaped_layout_keeps_every_character()
{
    // "ffi" is a ligature in most faces that have one, and HarfBuzz would set
    // one glyph for it; app_server sets three. The width of three hinted
    // advances is the server's answer whether or not the face ligates.
    auto& f = fixture();
    if (!f.named) {
        skip("unshaped layout", "this host has no second font family");
        return;
    }
    TextEngine engine;
    Font font;
    font.size = sample_size;
    font.identity = identity_of(*f.named);
    const auto want = static_cast<float>(
        oracle_width(f.library, f.named->path, "affine", sample_size,
                     bitmap_spacing));
    check(engine.width("affine", font) == want,
          "\"affine\" advances by six glyphs' hinted advances");
}

} // namespace

int main()
{
    test_the_identity_block_decodes_from_golden_bytes();
    test_the_capability_bit_is_bit_five();
    test_a_named_face_draws_exactly_as_app_server_draws_it();
    test_the_block_is_ignored_unless_negotiated();
    test_resolution_prefers_the_servers_file();
    test_widths_follow_app_servers_layout();
    test_unshaped_layout_keeps_every_character();
    test_string_spacing_kerns_the_way_app_server_does();
    test_the_servers_hinting_decision_is_honoured();
    test_a_missing_glyph_comes_from_the_fallback_family();
    if (failures == 0) {
        std::cout << "PASS - " << checks << " font checks";
        if (skips > 0)
            std::cout << " (" << skips << " SKIPPED, see SKIP: lines)";
        std::cout << '\n';
        return 0;
    }
    std::cerr << failures << " of " << checks << " font checks failed\n";
    return 1;
}
