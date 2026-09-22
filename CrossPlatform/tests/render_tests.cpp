// Render-fidelity tests: line caps and the three RP_SET_FONT attributes that
// used to be decoded and dropped.
//
// The cap expectations below are *hardcoded golden vectors*, measured once on a
// 64x64 surface and written down, not derived from the code under test -- so
// they can and do fail when the mapping is broken. The text expectations are
// relations between two renders of the same string (rotated versus not), which
// collapse to equality the moment the transform stops being applied.

#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

using namespace haiku_remote;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct InkBox {
    int left = 0;
    int top = 0;
    int right = -1;
    int bottom = -1;
    int count = 0;

    [[nodiscard]] int width() const { return right - left + 1; }
    [[nodiscard]] int height() const { return bottom - top + 1; }
};

// Everything darker than mid-grey counts as ink; the surfaces below start
// white and are drawn on in black.
InkBox measure_ink(const Surface& surface)
{
    InkBox box;
    bool first = true;
    for (int y = 0; y < surface.height(); ++y) {
        for (int x = 0; x < surface.width(); ++x) {
            if (surface.pixel(x, y).r >= 200)
                continue;
            ++box.count;
            if (first) {
                box.left = box.right = x;
                box.top = box.bottom = y;
                first = false;
                continue;
            }
            box.left = std::min(box.left, x);
            box.right = std::max(box.right, x);
            box.top = std::min(box.top, y);
            box.bottom = std::max(box.bottom, y);
        }
    }
    return box;
}

// `apply_cap == false` leaves DrawState::line_cap at its default, which is what
// a view sees before any RP_SET_STROKE_MODE arrives.
InkBox stroke(Point from, Point to, std::uint32_t wire_cap, bool apply_cap)
{
    Surface surface(64, 64);
    surface.clear({255, 255, 255, 255});
    DrawState state;
    state.pen_size = 9;
    state.high = {0, 0, 0, 255};
    if (apply_cap)
        state.line_cap = wire_cap;
    surface.line(from, to, state.high, &state, true);
    return measure_ink(surface);
}

// RP_SET_STROKE_MODE carries Haiku's cap_mode verbatim: B_ROUND_CAP == 0,
// B_BUTT_CAP == 3, B_SQUARE_CAP == 4 (headers/os/interface/InterfaceDefs.h;
// the in-tree JavaScript client maps the same three numbers).
constexpr std::uint32_t wire_round = 0;
constexpr std::uint32_t wire_butt = 3;
constexpr std::uint32_t wire_square = 4;
// 1 and 2 are B_MITER_JOIN and B_BEVEL_JOIN: legal joins, never legal caps.
constexpr std::uint32_t wire_illegal = 7;

const Point vertical_from {20, 20};
const Point vertical_to {20, 40};
const Point diagonal_from {20, 20};
const Point diagonal_to {40, 40};

// Golden vectors, pen size 9 on a 64x64 surface.
//
// BUTT ends exactly on the endpoints: pixel centres sit at y + 0.5, so the
// parameter stays inside [0, 1] for rows 20 through 39 and nothing outside them
// is inked. ROUND and SQUARE deliberately overshoot -- by the pen radius and by
// radius-along-the-direction respectively -- and that overshoot is the whole
// visible difference between the three modes.
constexpr int vertical_butt_top = 20;
constexpr int vertical_butt_bottom = 39;
constexpr int vertical_butt_ink = 200;
constexpr int vertical_round_top = 16;
constexpr int vertical_round_bottom = 43;
constexpr int vertical_round_ink = 260;
constexpr int vertical_square_top = 15;
constexpr int vertical_square_bottom = 44;
constexpr int vertical_square_ink = 300;

constexpr int diagonal_butt_low = 17;
constexpr int diagonal_butt_high = 42;
constexpr int diagonal_butt_ink = 266;
constexpr int diagonal_round_ink = 320;
constexpr int diagonal_square_ink = 344;

void test_vertical_caps_use_haikus_numbering()
{
    const auto butt = stroke(vertical_from, vertical_to, wire_butt, true);
    check(butt.top == vertical_butt_top && butt.bottom == vertical_butt_bottom,
          "wire cap 3 (B_BUTT_CAP) inks exactly the endpoint row span");
    check(butt.count == vertical_butt_ink,
          "wire cap 3 (B_BUTT_CAP) inks the butt reference pixel count");

    const auto round = stroke(vertical_from, vertical_to, wire_round, true);
    check(round.top == vertical_round_top
              && round.bottom == vertical_round_bottom,
          "wire cap 0 (B_ROUND_CAP) overshoots both endpoints by the radius");
    check(round.count == vertical_round_ink,
          "wire cap 0 (B_ROUND_CAP) inks the round reference pixel count");
    check(round.height() > butt.height(),
          "a round end is strictly longer than a butt end");

    const auto square = stroke(vertical_from, vertical_to, wire_square, true);
    check(square.top == vertical_square_top
              && square.bottom == vertical_square_bottom,
          "wire cap 4 (B_SQUARE_CAP) extends a full radius past each endpoint");
    check(square.count == vertical_square_ink,
          "wire cap 4 (B_SQUARE_CAP) inks the square reference pixel count");
    check(square.height() > butt.height(),
          "a square end is strictly longer than a butt end");
}

void test_an_illegal_cap_falls_back_to_butt()
{
    const auto illegal = stroke(vertical_from, vertical_to, wire_illegal, true);
    check(illegal.top == vertical_butt_top
              && illegal.bottom == vertical_butt_bottom
              && illegal.count == vertical_butt_ink,
          "an illegal wire cap renders as butt, not as unbounded ink");

    // The regression this pins down: an unmatched cap used to take the nearest
    // point on the *infinite* line, so ink filled the bounding span the
    // rasteriser had already padded by radius + 1 in each axis.
    const auto padded_span = (vertical_to.y - vertical_from.y)
        + 2 * (9 / 2 + 1);
    check(illegal.height() < static_cast<int>(padded_span),
          "an illegal wire cap does not fill the padded bounding span");
}

void test_diagonal_caps_do_not_exceed_the_butt_reference()
{
    const auto butt = stroke(diagonal_from, diagonal_to, wire_butt, true);
    check(butt.top == diagonal_butt_low && butt.bottom == diagonal_butt_high
              && butt.left == diagonal_butt_low
              && butt.right == diagonal_butt_high,
          "a diagonal butt end inks exactly the endpoint span in both axes");
    check(butt.count == diagonal_butt_ink,
          "a diagonal butt end matches the butt reference pixel count");
    check(butt.count <= diagonal_butt_ink,
          "a diagonal butt end never exceeds the butt reference pixel count");

    const auto round = stroke(diagonal_from, diagonal_to, wire_round, true);
    const auto square = stroke(diagonal_from, diagonal_to, wire_square, true);
    check(round.count == diagonal_round_ink,
          "a diagonal round end matches the round reference pixel count");
    check(square.count == diagonal_square_ink,
          "a diagonal square end matches the square reference pixel count");
    check(round.count > butt.count && square.count > round.count,
          "diagonal butt < round < square in inked area");
}

void test_the_default_cap_is_butt()
{
    // app_server's own DrawState starts at B_BUTT_CAP
    // (src/servers/app/DrawState.cpp:62), and under the correct numbering 0
    // means ROUND -- so a default of 0 would silently round every stroke drawn
    // before the first RP_SET_STROKE_MODE.
    const auto defaulted = stroke(vertical_from, vertical_to, 0, false);
    check(defaulted.top == vertical_butt_top
              && defaulted.bottom == vertical_butt_bottom
              && defaulted.count == vertical_butt_ink,
          "with no RP_SET_STROKE_MODE applied, strokes render butt");

    const auto explicit_butt = stroke(vertical_from, vertical_to, wire_butt,
                                      true);
    check(defaulted.count == explicit_butt.count
              && defaulted.top == explicit_butt.top
              && defaulted.bottom == explicit_butt.bottom,
          "the default cap is indistinguishable from an explicit B_BUTT_CAP");
}

// -- Font attributes ------------------------------------------------------

constexpr const char* sample_text = "Hamburgefonstiv";
constexpr float sample_size = 18;

struct TextRender {
    Surface surface {260, 260};
    InkBox ink;
    float advance = 0;
};

TextRender render_text(TextEngine& engine, Point baseline, float rotation,
                       float shear, float false_bold_width,
                       bool leave_shear_defaulted = false)
{
    TextRender result;
    result.surface.clear({255, 255, 255, 255});
    DrawState state;
    state.high = {0, 0, 0, 255};
    state.font.size = sample_size;
    state.font.rotation = rotation;
    if (!leave_shear_defaulted)
        state.font.shear = shear;
    state.font.false_bold_width = false_bold_width;
    result.advance = engine.draw(sample_text, baseline, state, result.surface);
    result.ink = measure_ink(result.surface);
    return result;
}

void test_rotation_turns_a_horizontal_label_vertical()
{
    TextEngine engine;
    check(engine.available(), "FreeType is available for the font tests");

    const Point plain_baseline {20, 100};
    const auto plain = render_text(engine, plain_baseline, 0, 90, 0);
    check(plain.ink.count > 0, "an unrotated string inks the surface");
    check(plain.ink.width() > 3 * plain.ink.height(),
          "an unrotated string is wide and short");

    // Baseline picked so a 90-degree (counter-clockwise) string stays on the
    // surface: it marches upwards from its baseline.
    const Point rotated_baseline {100, 170};
    const auto rotated = render_text(engine, rotated_baseline, 90, 90, 0);
    check(rotated.ink.count > 0, "a rotated string inks the surface");
    check(rotated.ink.height() > 3 * rotated.ink.width(),
          "a string rotated 90 degrees is tall and narrow");

    // The shape of the box is not on its own enough: rotating only the
    // *baseline* and leaving each glyph upright produces a tall narrow column
    // too (measured 16x149 against the correct 19x147 -- indistinguishable by
    // aspect ratio). So map the unrotated ink box through the rotation by hand
    // and demand the rotated render land on it.
    //
    // A point at baseline-relative FreeType offset (u, v), v pointing up, lands
    // at screen (bx + u, by - v) unrotated. Rotated 90 degrees
    // counter-clockwise it becomes (bx - v, by - u) -- so a plain *screen*
    // offset (sx, sy) maps to a rotated screen offset (sy, -sx).
    const int plain_low_x = plain.ink.left - static_cast<int>(plain_baseline.x);
    const int plain_high_x = plain.ink.right - static_cast<int>(plain_baseline.x);
    const int plain_low_y = plain.ink.top - static_cast<int>(plain_baseline.y);
    const int plain_high_y =
        plain.ink.bottom - static_cast<int>(plain_baseline.y);
    const int expected_left = static_cast<int>(rotated_baseline.x) + plain_low_y;
    const int expected_right =
        static_cast<int>(rotated_baseline.x) + plain_high_y;
    const int expected_top =
        static_cast<int>(rotated_baseline.y) - plain_high_x;
    const int expected_bottom =
        static_cast<int>(rotated_baseline.y) - plain_low_x;
    // One pixel of slack per edge for the floor() of a fractional pen position.
    constexpr int slack = 2;
    check(std::abs(rotated.ink.left - expected_left) <= slack
              && std::abs(rotated.ink.right - expected_right) <= slack,
          "the rotated ink sits where rotating the glyph outlines puts it (x)");
    check(std::abs(rotated.ink.top - expected_top) <= slack
              && std::abs(rotated.ink.bottom - expected_bottom) <= slack,
          "the rotated ink sits where rotating the glyph outlines puts it (y)");
}

void test_string_width_ignores_rotation()
{
    // ServerFont::StringWidth uses StringWidthConsumer, whose Finish(x, y)
    // keeps a bare x with no embedded transform -- unlike AGGTextRenderer's
    // StringRenderer::Finish, which does transform the reported pen position.
    // The RP_STRING_WIDTH reply must match the server's own fallback, so
    // rotation must not move it.
    TextEngine engine;
    Font upright;
    upright.size = sample_size;
    Font rotated = upright;
    rotated.rotation = 90;
    Font sheared = upright;
    sheared.shear = 60;
    Font emboldened = upright;
    emboldened.false_bold_width = 1.5f;

    const float reference = engine.width(sample_text, upright);
    check(reference > 0, "an upright string has a positive width");
    check(engine.width(sample_text, rotated) == reference,
          "RP_STRING_WIDTH is unchanged by rotation, as on the server");
    check(engine.width(sample_text, sheared) == reference,
          "RP_STRING_WIDTH is unchanged by shear, as on the server");
    check(engine.width(sample_text, emboldened) == reference,
          "false bold changes glyph weight, not the advance");
}

void test_neutral_shear_is_ninety_degrees()
{
    TextEngine engine;
    // Haiku's neutral shear is 90, not 0: app_server shears by (90 - Shear()).
    const auto neutral = render_text(engine, {20, 100}, 0, neutral_font_shear,
                                     0);
    const auto defaulted = render_text(engine, {20, 100}, 0, 0, 0, true);
    check(neutral.ink.count > 0, "a neutrally sheared string inks the surface");
    check(std::equal(neutral.surface.pixels().begin(),
                     neutral.surface.pixels().end(),
                     defaulted.surface.pixels().begin()),
          "shear 90 is pixel-identical to a default-constructed Font");

    // And shear is genuinely read: a slanted one must differ.
    const auto slanted = render_text(engine, {20, 100}, 0, 60, 0);
    check(!std::equal(neutral.surface.pixels().begin(),
                      neutral.surface.pixels().end(),
                      slanted.surface.pixels().begin()),
          "shear 60 renders differently from neutral shear");
    check(slanted.ink.left < neutral.ink.left,
          "a positive slant leans the glyphs leftwards at the top");
}

void test_false_bold_thickens_the_glyphs()
{
    TextEngine engine;
    const auto plain = render_text(engine, {20, 100}, 0, neutral_font_shear, 0);
    const auto bold = render_text(engine, {20, 100}, 0, neutral_font_shear,
                                  1.5f);
    check(plain.ink.count > 0, "the unemboldened string inks the surface");
    check(bold.ink.count > plain.ink.count,
          "false_bold_width > 0 inks more pixels at the same size");
    check(bold.advance == plain.advance,
          "false_bold_width leaves the reported advance alone");
}

} // namespace

int main()
{
    test_vertical_caps_use_haikus_numbering();
    test_an_illegal_cap_falls_back_to_butt();
    test_diagonal_caps_do_not_exceed_the_butt_reference();
    test_the_default_cap_is_butt();
    test_rotation_turns_a_horizontal_label_vertical();
    test_string_width_ignores_rotation();
    test_neutral_shear_is_ninety_degrees();
    test_false_bold_thickens_the_glyphs();
    if (failures == 0) {
        std::cout << "PASS - " << checks << " render checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " render checks failed\n";
    return 1;
}
