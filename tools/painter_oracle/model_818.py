#!/usr/bin/env python3
"""Write a DrawBitmapBilinear.h with Haiku-Graviton #818's proposed scale.

Usage: model_818.py HAIKU_TREE OUTPUT_DIR [crop|crop-int]

#818: DrawBitmapBilinear::Draw indexes a cropped source by the whole bitmap's
extent, `(i + offset) * (srcWidth - 1) / (srcWidth * scaleX - 1)`, then shifts
the index by the crop's origin, so the crop is traversed too fast and its last
column and row repeat. The correction under discussion indexes by the crop:

  crop      (i + offset) * (cropWidth - 1) / destinationRect.Width(), with
            cropWidth = (destinationRect.Width() + 1) / scaleX, a double (the
            crop's extent: BitmapPainter.cpp:197 sets fScaleX from it)
  crop-int  (i + offset) * (cropWidth - 1) / (cropWidth * scaleX - 1), with
            cropWidth that extent rounded to a uint32 -- the original
            expression with srcWidth replaced, so an uncropped draw is the
            same arithmetic as before

and the same for y. The bounds clamp of #817 stays as it is. This is a model
for deciding #818 with numbers, not the server: build.sh puts OUTPUT_DIR first
on the include path only when ORACLE_MODEL_818 is set, and the goldens never
come from it.
"""

import sys

HEADER = "src/servers/app/drawing/Painter/bitmap_painter/DrawBitmapBilinear.h"


def main():
    tree, out = sys.argv[1], sys.argv[2]
    variant = sys.argv[3] if len(sys.argv) > 3 else "crop"
    with open(f"{tree}/{HEADER}", encoding="utf-8") as f:
        text = f.read()
    if variant == "crop":
        extent = {
            "X": "const double cropWidth = (destinationRect.Width() + 1) / scaleX;",
            "Y": "const double cropHeight = (destinationRect.Height() + 1) / scaleY;",
        }
        index = {
            "X": "(i + filterData.fIndexOffsetX) * (cropWidth - 1)\n"
                 "\t\t\t\t/ destinationRect.Width();",
            "Y": "(i + filterData.fIndexOffsetY) * (cropHeight - 1)\n"
                 "\t\t\t\t/ destinationRect.Height();",
        }
    elif variant == "crop-int":
        extent = {
            "X": "const uint32 cropWidth\n\t\t\t= (uint32)round((destinationRect.Width() + 1) / scaleX);",
            "Y": "const uint32 cropHeight\n\t\t\t= (uint32)round((destinationRect.Height() + 1) / scaleY);",
        }
        index = {
            "X": "(i + filterData.fIndexOffsetX) * (cropWidth - 1)\n"
                 "\t\t\t\t/ (cropWidth * scaleX - 1);",
            "Y": "(i + filterData.fIndexOffsetY) * (cropHeight - 1)\n"
                 "\t\t\t\t/ (cropHeight * scaleY - 1);",
        }
    else:
        sys.exit(f"model_818.py: unknown variant {variant}")
    replacements = [
        ("(i + filterData.fIndexOffsetX) * (srcWidth - 1)\n"
         "\t\t\t\t/ (srcWidth * scaleX - 1);", index["X"]),
        ("(i + filterData.fIndexOffsetY) * (srcHeight - 1)\n"
         "\t\t\t\t/ (srcHeight * scaleY - 1);", index["Y"]),
        ("\t\tconst int32 xMaxIndex =",
         f"\t\t// Haiku-Graviton #818 model ({variant})\n"
         f"\t\t{extent['X']}\n\t\t{extent['Y']}\n\t\tconst int32 xMaxIndex ="),
    ]
    for old, new in replacements:
        if text.count(old) != 1:
            sys.exit(f"model_818.py: {HEADER} does not have the expected code;"
                     f" looked for:\n{old}")
        text = text.replace(old, new)
    with open(f"{out}/DrawBitmapBilinear.h", "w", encoding="utf-8") as f:
        f.write(text)


if __name__ == "__main__":
    main()
