#pragma once
// Rasterised tile artwork, for the WEB build only.
//
// The ground, the textured map tiles and the title backdrop are all the same
// shape of work: one SvgDocument fitted into an axis-aligned box, repeated
// across the screen, identical every frame. Drawn straight, each tile hands the
// browser its whole tessellated geometry again -- on the title screen alone
// that is ~1,800 arcs and ~2,300 fills a frame for fifteen copies of one
// picture, against the ~90 drawImage calls the reference client spends on the
// same screen, because the reference bakes its static map into offscreen
// canvases and blits them.
//
// So: rasterise the document once at the size it is actually drawn, keep the
// OffscreenCanvas, and blit it thereafter.
//
// **This is deliberately web-only, and must stay that way.** The native build
// rasterises on the CPU, where `drawImage` inverse-maps and filters every pixel
// while the software rasterizer's opaque span path fills flat artwork at nearly
// memcpy speed. Caching these same tiles for the native client was measured at
// FOUR TIMES SLOWER and reverted. The browser is the opposite case: a blit is a
// GPU texture copy and the path work is what costs. `drawCachedArt` therefore
// compiles to `return false` off the web, and every caller keeps its direct
// `renderFitted` fallback.

#include "canvas.h"
#include "svg.h"

namespace flix {

/// Draws `art` fitted into the user-space box (x, y, w, h), through a cached
/// rasterisation of it.
///
/// Returns false when this document cannot be served from the cache -- it
/// animates, the box is degenerate, or the bitmap it would need is out of
/// range -- in which case NOTHING has been drawn and the caller must render the
/// document itself. Always false on a non-web build.
bool drawCachedArt(Canvas& canvas, const SvgDocument& art, double x, double y, double w,
                   double h);

/// Bitmaps held and the bytes they occupy, for the stats readout and the tests.
void artCacheStats(std::size_t& entries, std::size_t& bytes);

/// Drops every bitmap. For a display-scale change, which invalidates the size
/// every entry was baked at.
void clearArtCache();

} // namespace flix
