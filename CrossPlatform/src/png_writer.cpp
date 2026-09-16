#include "haiku_remote/png_writer.hpp"

#include <cstdio>
#include <png.h>
#include <vector>

namespace haiku_remote {

bool write_png(const Surface& surface, const std::string& path, std::string& error)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        error = "could not open output file";
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png == nullptr ? nullptr : png_create_info_struct(png);
    if (png == nullptr || info == nullptr) {
        if (png != nullptr)
            png_destroy_write_struct(&png, nullptr);
        std::fclose(file);
        error = "could not create PNG encoder";
        return false;
    }
    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
        error = "PNG encoding failed";
        return false;
    }
    png_init_io(png, file);
    png_set_IHDR(png, info, surface.width(), surface.height(), 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(surface.width() * surface.height() * 4));
    std::vector<png_bytep> rows(static_cast<std::size_t>(surface.height()));
    const auto source = surface.pixels();
    for (int y = 0; y < surface.height(); ++y) {
        rows[static_cast<std::size_t>(y)] =
            rgba.data() + static_cast<std::size_t>(y * surface.width() * 4);
        for (int x = 0; x < surface.width(); ++x) {
            const auto offset = static_cast<std::size_t>((y * surface.width() + x) * 4);
            rgba[offset] = source[offset + 2];
            rgba[offset + 1] = source[offset + 1];
            rgba[offset + 2] = source[offset];
            rgba[offset + 3] = source[offset + 3];
        }
    }
    png_write_image(png, rows.data());
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
    return true;
}

} // namespace haiku_remote
