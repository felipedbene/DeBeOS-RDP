// Host shim for src/servers/app/SystemPalette.h. Only the B_CMAP8 paths of
// DrawBitmapNoScale read the palette, and no oracle scene draws a B_CMAP8
// bitmap; see oracle.cpp.
#ifndef ORACLE_SHIM_SYSTEM_PALETTE_H
#define ORACLE_SHIM_SYSTEM_PALETTE_H

#include <GraphicsDefs.h>

const rgb_color* SystemPalette();

#endif
