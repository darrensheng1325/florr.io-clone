#include "test.h"

#include "client/render/art_cache.h"

#include <string>

// The web tile cache rests on one property of the artwork: that a document
// without a SMIL timeline draws the same picture at every `timeSeconds`, so
// one rasterisation of it can stand in for every frame. `animated()` is what
// answers that, and getting it wrong would freeze a moving picture rather than
// merely cost frame time -- so it is pinned here, on the native build, where
// the cache itself is a deliberate no-op.

using namespace flix;

namespace {

const char* kStatic = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <rect width="100" height="100" fill="#3a7"/>
  <circle cx="50" cy="50" r="20" fill="#286"/>
</svg>)SVG";

const char* kAnimated = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <circle cx="50" cy="50" r="20" fill="#286">
    <animate attributeName="r" values="10;30;10" dur="2s" repeatCount="indefinite"/>
  </circle>
</svg>)SVG";

/// The timeline is on a nested node, which is where a check that only looked at
/// the root would miss it.
const char* kAnimatedChild = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <g transform="translate(10 10)">
    <g>
      <rect width="40" height="40" fill="#286">
        <animateTransform attributeName="transform" type="rotate"
                          values="0 20 20;360 20 20" dur="3s" repeatCount="indefinite"/>
      </rect>
    </g>
  </g>
</svg>)SVG";

} // namespace

TEST(svg_document_reports_whether_it_animates) {
    CHECK(!SvgDocument::fromString(kStatic).animated());
    CHECK(SvgDocument::fromString(kAnimated).animated());
    CHECK(SvgDocument::fromString(kAnimatedChild).animated());
}

TEST(art_cache_is_a_no_op_off_the_web) {
#ifndef __EMSCRIPTEN__
    // Caching these tiles was measured four times SLOWER against the software
    // rasterizer, so the native build must keep drawing the artwork. The
    // contract is that drawCachedArt draws nothing and says so, leaving the
    // caller's renderFitted fallback as the only thing that paints.
    const SvgDocument art = SvgDocument::fromString(kStatic);
    Canvas canvas = Canvas::createVirtual(64, 64);
    CHECK(!drawCachedArt(canvas, art, 0.0, 0.0, 64.0, 64.0));

    std::size_t entries = 1, bytes = 1;
    artCacheStats(entries, bytes);
    CHECK(entries == 0);
    CHECK(bytes == 0);
#endif
}
