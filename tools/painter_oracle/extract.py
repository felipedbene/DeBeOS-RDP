#!/usr/bin/env python3
"""Extract app_server's own function bodies for the Painter oracle.

The oracle must run the server's code, not a transcription of it -- a
hand-copied Painter would be one more implementation to get wrong, and a test
against it would be self-consistent rather than independent. So this script
cuts the definitions the oracle needs out of the Haiku tree, byte for byte, and
writes them into one translation unit that compiles against the shim
declarations in shim/.

Usage: extract.py HAIKU_TREE OUTPUT.cpp [--text]

--text also takes Painter's string drawing (TEXT_FUNCTIONS), for the text
oracle, which builds every unit with ORACLE_TEXT (shim/Painter.h).

A definition is found by its qualified name at the start of a line (Haiku style
puts the return type on the line above), and runs to the first line that is
exactly "}". Every overload of a listed name is taken. The script fails loudly
if a listed name is missing, so a refactor upstream cannot silently drop a
function and leave the oracle testing nothing.
"""

import re
import sys

PAINTER = "src/servers/app/drawing/Painter/Painter.cpp"
PAINTER_H = "src/servers/app/drawing/Painter/Painter.h"
TRANSFORMABLE = "src/servers/app/drawing/Painter/Transformable.cpp"
ENGINE = "src/servers/app/drawing/DrawingEngine.cpp"
BITMAP_PAINTER = "src/servers/app/drawing/Painter/bitmap_painter/BitmapPainter.cpp"
COLOR_CONVERSION = "src/kits/interface/ColorConversion.cpp"

PAINTER_FUNCTIONS = [
    "Painter::Bounds",
    "Painter::ConstrainClipping",
    "Painter::SetTransform",
    "Painter::SetHighColor",
    "Painter::SetLowColor",
    "Painter::SetDrawingMode",
    "Painter::SetBlendingMode",
    "Painter::SetPenSize",
    "Painter::SetStrokeMode",
    "Painter::SetFillRule",
    "Painter::SetPattern",
    "Painter::StrokeLine",
    "Painter::StraightLine",
    "Painter::StrokeTriangle",
    "Painter::FillTriangle",
    "Painter::DrawPolygon",
    "Painter::DrawBezier",
    "Painter::DrawShape",
    "Painter::StrokeRect",
    "Painter::FillRect",
    "Painter::FillRectVerticalGradient",
    "Painter::FillRectNoClipping",
    "Painter::StrokeRoundRect",
    "Painter::FillRoundRect",
    "Painter::AlignEllipseRect",
    "Painter::DrawEllipse",
    "Painter::StrokeArc",
    "Painter::FillArc",
    "Painter::FillRegion",
    "Painter::_Align",
    "Painter::_Clipped",
    "Painter::_UpdateDrawingMode",
    "Painter::_SetRendererColor",
    "Painter::_DrawTriangle",
    "Painter::_IterateShapeData",
    "Painter::_InvertRect32",
    "Painter::_BlendRect32",
    "Painter::_BoundingBox",
    "agg_line_cap_mode_for",
    "agg_line_join_mode_for",
    "Painter::_StrokePath",
    "Painter::_FillPath",
    "Painter::_RasterizePath",
    "Painter::_CalcLinearGradientTransform",
    "Painter::_CalcRadialGradientTransform",
    "Painter::_MakeGradient",
]

PAINTER_H_FUNCTIONS = [
    "Painter::TransformAndClipRect",
    "Painter::ClipRect",
    "Painter::AlignAndClipRect",
    "Painter::TransformAlignAndClipRect",
    "Painter::AlignRect",
]

TRANSFORMABLE_FUNCTIONS = [
    "min4",
    "max4",
    "Transformable::operator=",
    "Transformable::Reset",
    "Transformable::IsIdentity",
    "Transformable::Transform",
    "Transformable::TransformBounds",
]

ENGINE_FUNCTIONS = [
    "make_rect_valid",
    "extend_by_stroke_width",
    "DrawingEngine::DrawArc",
    "DrawingEngine::DrawBezier",
    "DrawingEngine::DrawEllipse",
    "DrawingEngine::DrawPolygon",
    "DrawingEngine::StrokePoint",
    "DrawingEngine::StrokeLine",
    "DrawingEngine::StrokeRect",
    "DrawingEngine::FillRect",
    "DrawingEngine::FillRegion",
    "DrawingEngine::DrawRoundRect",
    "DrawingEngine::DrawShape",
    "DrawingEngine::DrawTriangle",
    "DrawingEngine::StrokeLineArray",
    "DrawingEngine::DrawBitmap",
]

# _ConvertColorSpace and _TransparentMagicToAlpha are left out: every bitmap
# scene is B_RGBA32, for which the conversion returns before doing anything
# (BitmapPainter.cpp:261-262), and oracle.cpp supplies a definition that
# aborts if a scene ever needs more. DrawBitmapNoScale, -NearestNeighbor,
# -Bilinear and -Generic are headers, compiled as they are.
BITMAP_PAINTER_FUNCTIONS = [
    "Painter::BitmapPainter::BitmapPainter",
    "Painter::BitmapPainter::Draw",
    "Painter::BitmapPainter::_DetermineTransform",
    "Painter::BitmapPainter::_HasScale",
    "Painter::BitmapPainter::_HasAffineTransform",
    "Painter::BitmapPainter::_HasAlphaMask",
    "Painter::BitmapPainter::_ConvertColorSpace",
    "Painter::BitmapPainter::_TransparentMagicToAlpha",
]

# The text oracle's Painter entry points. SetFont(const DrawState*) is left
# out: it reads the DrawState, which the oracle sets through Painter's own
# setters instead, as for every other scene; SetFont(const ServerFont&) is the
# same two calls (Painter.cpp:482-497).
TEXT_FUNCTIONS = [
    "Painter::SetFont",
    "Painter::DrawString",
]

TEXT_TRANSFORMABLE_FUNCTIONS = [
    "Transformable::IsTranslationOnly",
    "Transformable::TranslateBy",
    "Transformable::RotateBy",
    "Transformable::ShearBy",
]

DEFINITION = re.compile(r"^([A-Za-z_][A-Za-z0-9_:~]*(?:operator\s*\S+?)?)\(")


def read(tree, path):
    with open(f"{tree}/{path}", encoding="utf-8") as f:
        return f.read().split("\n")


def functions(tree, path, names, skip=()):
    """Every definition of every name in `names`, with its preamble."""
    lines = read(tree, path)
    wanted = set(names)
    found = set()
    out = [f"// ---- extracted from {path} ----"]
    i = 0
    while i < len(lines):
        match = DEFINITION.match(lines[i])
        name = match.group(1).replace(" ", "") if match else None
        if name in wanted:
            start = i
            while start > 0:
                previous = lines[start - 1]
                if (not previous.strip() or previous.startswith("//")
                        or previous.startswith("}") or previous.startswith("#")
                        or previous.rstrip().endswith(";")):
                    break
                start -= 1
            end = i
            while lines[end].rstrip() != "}":
                end += 1
            text = "\n".join(lines[start:end + 1])
            if not any(s in text for s in skip):
                out.append(f"#line {start + 1} \"{path}\"")
                out.extend(lines[start:end + 1])
                out.append("")
            found.add(name)
            i = end + 1
            continue
        i += 1
    missing = wanted - found
    if missing:
        sys.exit(f"extract.py: {path}: not found: {', '.join(sorted(missing))}")
    return out


def block(tree, path, first, last):
    """The lines from the one starting with `first` to the one equal to `last`."""
    lines = read(tree, path)
    for i, line in enumerate(lines):
        if line.startswith(first):
            end = i
            while lines[end].rstrip() != last:
                end += 1
            return ([f"#line {i + 1} \"{path}\""] + lines[i:end + 1] + [""])
    sys.exit(f"extract.py: {path}: no line starting with {first!r}")


def main():
    if len(sys.argv) not in (3, 4) or sys.argv[3:] not in ([], ["--text"]):
        sys.exit(__doc__)
    tree, output = sys.argv[1], sys.argv[2]
    text = sys.argv[3:] == ["--text"]
    out = [
        "// Generated by tools/painter_oracle/extract.py from the Haiku tree.",
        "// Do not edit: every line below is app_server's own source.",
        '#include "oracle_prelude.h"',
        "",
    ]
    # The member-access macros and CHECK_CLIPPING at the top of Painter.cpp.
    out += block(tree, PAINTER, "#define CHECK_CLIPPING\t", "#define fCurve\t\t\t\t\tfInternal.fCurve")
    out += block(tree, PAINTER, "class SolidPatternGuard {", "};")
    out += functions(tree, PAINTER_H, PAINTER_H_FUNCTIONS)
    # StrokeLine(BPoint, BPoint) and friends are all wanted; DrawString is
    # not listed, so it is never pulled in. DrawBitmap comes after the
    # BitmapPainter below, which it needs complete.
    out += functions(tree, PAINTER, PAINTER_FUNCTIONS)
    if text:
        out += functions(tree, PAINTER, TEXT_FUNCTIONS,
                         skip=("const DrawState* state",))
    out += functions(tree, TRANSFORMABLE, TRANSFORMABLE_FUNCTIONS,
                     skip=("Transformable& other)", "Transform(const BPoint& point)"))
    if text:
        out += functions(tree, TRANSFORMABLE, TEXT_TRANSFORMABLE_FUNCTIONS)
    # The engine needs the Painter macros undefined: fBuffer and friends are
    # Painter members, and DrawingEngine.cpp has none of them.
    out += ["#undef fBuffer", "#undef fPixelFormat", "#undef fBaseRenderer",
            "#undef fUnpackedScanline", "#undef fPackedScanline",
            "#undef fRasterizer", "#undef fRenderer", "#undef fRendererBin",
            "#undef fSubpixPackedScanline", "#undef fSubpixUnpackedScanline",
            "#undef fSubpixRasterizer", "#undef fSubpixRenderer",
            "#undef fMaskedUnpackedScanline", "#undef fClippedAlphaMask",
            "#undef fPath", "#undef fCurve", ""]
    # BitmapPainter.cpp and its DrawBitmap*.h headers spell out
    # fPainter->fInternal.fBuffer and aggInterface.fBuffer, which the macros
    # above would mangle, so they follow the #undefs. Its own #include lines
    # come first, verbatim.
    out += [f"// ---- #include lines of {BITMAP_PAINTER} ----"]
    out += [line for line in read(tree, BITMAP_PAINTER)
            if line.startswith("#include")]
    # _ConvertColorSpace converts a B_RGB32 bitmap with BBitmap::ImportBits.
    # The per-pixel work is ColorConversion.cpp's innermost ConvertBits
    # template, taken verbatim; the colour-space dispatchers above it (every
    # overload taking a `void *dstBits`) would pull in the palette machinery
    # and are not taken -- shim/BitmapImport.h follows them by hand for the
    # one conversion the scenes reach.
    out += block(tree, COLOR_CONVERSION, "typedef uint32 (readFunc)",
                 "typedef void (writeFunc)(uint8 **dest, uint8 *data, int32 index);")
    convert = functions(tree, COLOR_CONVERSION, ["ConvertBits"],
                        skip=("void *dstBits",))
    if not any("dstByte *dstBits" in line for line in convert):
        sys.exit(f"extract.py: {COLOR_CONVERSION}: no per-pixel ConvertBits")
    out += convert
    out += ['#include "BitmapImport.h"', ""]
    out += functions(tree, BITMAP_PAINTER, BITMAP_PAINTER_FUNCTIONS)
    out += functions(tree, PAINTER, ["Painter::DrawBitmap"])
    out += block(tree, ENGINE, "class DrawTransaction {", "};")
    out += functions(tree, ENGINE, ENGINE_FUNCTIONS)
    with open(output, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
