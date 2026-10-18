#pragma once

// Real font bytes for the font-fetch tests (font_fetch_tests.cpp and the
// connect flow's stage in connect_tests.cpp): a committed TrueType file, with
// its 'head' checkSumAdjustment patched to a value no host has, so it is a
// "guest" font the client must fetch. FreeType is the oracle for every field
// read back: the patch is checked with FT_Get_Sfnt_Table, not with the parser
// under test.

#include "haiku_remote/types.hpp"

#include "test_fonts.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace font_fixture {

namespace fs = std::filesystem;
using haiku_remote::FontIdentity;

inline std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_file(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// FreeType's reading of a file's 'head': the oracle for every real file here.
struct Head {
    std::string family;
    std::string style;
    std::uint32_t checksum = 0;
    std::uint32_t revision = 0;
};

inline std::optional<Head> freetype_head(const fs::path& path)
{
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0)
        return std::nullopt;
    std::optional<Head> result;
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.string().c_str(), 0, &face) == 0) {
        if (const auto* head = static_cast<const TT_Header*>(
                FT_Get_Sfnt_Table(face, FT_SFNT_HEAD))) {
            Head h;
            h.family = face->family_name != nullptr ? face->family_name : "";
            h.style = face->style_name != nullptr ? face->style_name : "";
            h.checksum = static_cast<std::uint32_t>(head->CheckSum_Adjust);
            h.revision = static_cast<std::uint32_t>(head->Font_Revision);
            result = h;
        }
        FT_Done_Face(face);
    }
    FT_Done_FreeType(library);
    return result;
}

// A plain TrueType file for real bytes: the committed Noto Sans
// (tests/test_fonts.hpp), not one found on the host -- which used to make these
// tests skip on a host without fonts and draw from a different file on every
// host that had some. Null only for a checkout missing tests/fonts, which the
// callers fail on rather than skip.
inline std::optional<fs::path> committed_ttf()
{
    const fs::path path = test_fonts::noto_sans();
    const std::string bytes = read_file(path);
    if (bytes.size() < 12 || bytes.substr(0, 4) != std::string("\0\1\0\0", 4)
        || !freetype_head(path)) {
        return std::nullopt;
    }
    return path;
}

inline std::uint32_t read_be32(std::string_view b, std::size_t at)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) << 24)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 8)
        | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3]));
}

inline void put_be32(std::string& b, std::size_t at, std::uint32_t v)
{
    b[at] = static_cast<char>(v >> 24);
    b[at + 1] = static_cast<char>(v >> 16);
    b[at + 2] = static_cast<char>(v >> 8);
    b[at + 3] = static_cast<char>(v);
}

// The file with its 'head' checkSumAdjustment replaced: a face no host
// has, whose bytes still open in FreeType (which does not verify the field).
inline std::string with_checksum(std::string bytes, std::uint32_t checksum)
{
    const unsigned tables = (static_cast<unsigned char>(bytes[4]) << 8)
        | static_cast<unsigned char>(bytes[5]);
    for (unsigned i = 0; i < tables; ++i) {
        const std::size_t record = 12 + 16 * i;
        if (bytes.compare(record, 4, "head") == 0) {
            put_be32(bytes, read_be32(bytes, record + 8) + 8, checksum);
            break;
        }
    }
    return bytes;
}

// A guest font: the file patched to `checksum`, under `guest_root` at
// `guest_path`, and the identity a server would send for it -- read back by
// FreeType, so the patch itself is checked.
struct GuestFont {
    FontIdentity identity;
    std::string bytes;
    fs::path local;
};

inline std::optional<GuestFont> make_guest_font(const fs::path& source,
                                         const fs::path& guest_root,
                                         const std::string& guest_path,
                                         std::uint32_t checksum)
{
    GuestFont font;
    font.bytes = with_checksum(read_file(source), checksum);
    font.local = guest_root / guest_path.substr(1);
    write_file(font.local, font.bytes);
    const auto head = freetype_head(font.local);
    if (!head || head->checksum != checksum)
        return std::nullopt;
    font.identity.family = head->family;
    font.identity.style = head->style;
    font.identity.face_index = 0;
    font.identity.checksum_adjustment = head->checksum;
    font.identity.font_revision = head->revision;
    font.identity.path = guest_path;
    return font;
}


} // namespace font_fixture
