#include "test.h"

#include "client/render/art_cache.h"
#include "client/render/mob_art.h"
#include "client/render/sprites.h"
#include "shared/game/config.h"

#include <fstream>
#include <string>
#include <vector>

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


namespace {

std::string testsDir() {
    const std::string path = __FILE__;
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string firstExisting(const std::vector<std::string>& candidates) {
    for (const std::string& candidate : candidates) {
        std::ifstream probe(candidate, std::ios::binary);
        if (probe) return candidate;
    }
    return {};
}

const ContentRegistry& shippedContent() {
    static const ContentRegistry registry = [] {
        ContentRegistry r;
        std::string error;
        r.loadFiles(firstExisting({testsDir() + "/../../src/mobs.json", "data/mobs.json",
                                   "../src/mobs.json", "../../src/mobs.json", "src/mobs.json"}),
                    firstExisting({testsDir() + "/../../src/petals.json", "data/petals.json",
                                   "../src/petals.json", "../../src/petals.json", "src/petals.json"}),
                    firstExisting({testsDir() + "/../data/mob_xp.json", "data/mob_xp.json",
                                   "../data/mob_xp.json", "cpp/data/mob_xp.json"}),
                    error);
        return r;
    }();
    return registry;
}

} // namespace

TEST(every_procedural_marker_in_the_shipped_mobs_compiles) {
    // The failure this pins is SILENT. An `image` naming a painter this build
    // does not have -- a typo, or a painter added to mobs.json and not to
    // mobArtFor -- is not a parse error and not a warning anybody reads: the
    // marker falls through to the document path, fails to parse as SVG, and
    // the mob draws as a flat rarity disc in the world, in the bestiary and on
    // its own tooltip.
    //
    // Asked of the '$' entries only. Four mobs ship a literally empty <svg/>
    // and are MEANT to draw nothing (the renderer paints the garbage pile
    // itself; the plot markers are invisible), so "declares an image" is not
    // the same question as "should draw something".
    const ContentRegistry& content = shippedContent();
    CHECK(content.loaded());
    SpriteCache sprites;
    sprites.build(content, "data");

    int markers = 0;
    for (std::uint16_t i = 0; i < content.mobCount(); ++i) {
        const MobConfig& config = content.mob(i);
        if (config.image.empty() || config.image[0] != '$') continue;
        ++markers;
        if (!sprites.mobDrawable(i)) {
            std::printf("    mob '%s': image \"%s\" names nothing this build can draw\n",
                        config.id.c_str(), config.image.c_str());
        }
        CHECK(sprites.mobDrawable(i));
    }
    // The count is not the point, but a lookup that quietly found no markers
    // at all would pass this test for the wrong reason.
    CHECK(markers >= 7);
}

TEST(the_procedural_markers_name_painters_this_build_has) {
    // `$sponge:` is the odd one out: a palette marker expanded into a document
    // rather than a painter name, which is why this asks mobArtFor about the
    // painters by name instead of about every '$' string in the file.
    CHECK(mobArtFor("$rock") == MobArt::Rock);
    CHECK(mobArtFor("$cactus") == MobArt::Cactus);
    CHECK(mobArtFor("$sandstorm") == MobArt::Sandstorm);
    CHECK(mobArtFor("$scorpion") == MobArt::Scorpion);
    CHECK(mobArtFor("$crab") == MobArt::Crab);
    CHECK(mobArtFor("$leech") == MobArt::LeechHead);
    CHECK(mobArtFor("$leech_body") == MobArt::LeechBody);
    CHECK(mobArtFor("$nothing_by_that_name") == MobArt::None);
    CHECK(mobArtFor("<svg/>") == MobArt::None);
    CHECK(mobArtFor("") == MobArt::None);
}
