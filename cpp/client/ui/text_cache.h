#pragma once
// The native build's stand-in for a browser's glyph cache.
//
// A browser keeps rasterized glyphs and reuses them; this client hands the
// rasterizer a freshly built Path2D of contours for every run, every frame,
// and then STROKES it as well as filling it. Measured in a game frame, that
// was half the frame -- and four fifths of it was the stroke pass, whose
// outline over a whole run puts hundreds of crossings on every scanline for
// the coverage walk to sort.
//
// So a run that has been drawn once is kept as the pixels it produced, keyed
// by everything that decides those pixels, and blitted next time. See
// paintRunCached for what "everything" is and why the key can be finite.

#include <string>

#include "canvas.h"

#include "client/ui/draw.h"

#ifndef __EMSCRIPTEN__

namespace flix::ui {

/// Paints a run through the raster cache. Returns false when this run cannot
/// be cached -- a rotated, skewed or non-uniform transform, a size outside the
/// range worth baking, or geometry the bounds pass does not recognise -- in
/// which case the caller must draw it directly instead.
///
/// `strokeWidth` is already resolved (the caller has applied the size ratio).
bool paintRunCached(Canvas& canvas, const std::string& s, double penX, double baseline,
                    const TextStyle& style, double strokeWidth, double strokeAlpha,
                    double fillAlpha, bool fillFirst);

/// Builds the run's outlines and strokes then fills them, straight onto the
/// canvas. What the cache bakes with, and what the caller falls back to.
void paintRunDirect(Canvas& canvas, const std::string& s, double penX, double baseline,
                    const TextStyle& style, double strokeWidth, double strokeAlpha,
                    double fillAlpha, bool fillFirst);

/// Drops every entry. For a font change -- the display scale does not need it,
/// being part of the key already.
void clearTextCache();

/// Entries held and bytes they occupy, for the stats readout and the tests.
void textCacheStats(std::size_t& entries, std::size_t& bytes);

} // namespace flix::ui

#endif   // __EMSCRIPTEN__
