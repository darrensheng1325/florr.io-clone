#include "test.h"

#include "server_harness.h"

#include "client/ui/draw.h"
#include "client/ui/text.h"
#include "client/ui/text_cache.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// What the run cache is allowed to change.
//
// It bakes a run once and blits the pixels back, so it is not bit-exact with
// the rasterizer by construction: the pen is snapped to a quarter of a device
// pixel and, for a rotated run, the angle to a bucket. Both of those are
// sub-pixel, and this pins them there -- a bake that landed on the wrong pixel,
// turned the wrong way, or cropped its own ink would move far more than this
// allows.

using namespace flix;
using flix::testsupport::dataDir;

namespace {

constexpr int kWidth = 220;
constexpr int kHeight = 90;

struct Shot {
    std::vector<std::uint8_t> rgba;
};

ui::TextStyle tallyStyle() {
    ui::TextStyle style;
    style.size = 12.0;
    style.bold = true;
    style.fill = 0xFFFFFFu;
    style.stroke = 0x222222u;
    style.strokeWidth = 3.0;
    style.roundJoin = true;
    return style;
}

/// Draws one run at `angle` radians about (x, y), either through the cache or
/// straight at the rasterizer.
Shot render(const std::string& s, double angle, bool cached) {
    Canvas canvas = Canvas::createVirtual(kWidth, kHeight);
    const ui::TextStyle style = tallyStyle();
    canvas.save();
    canvas.translate(60.0f, 45.0f);
    canvas.rotate(static_cast<float>(angle));
    // Both sides take a run whose pen is already resolved, so the only thing
    // that differs between them is which painter puts the ink down.
    if (cached) {
        ui::paintRun(canvas, s, 0.0, 0.0, style, 1.0, 1.0, false);
    } else {
        ui::paintRunDirect(canvas, s, 0.0, 0.0, style, style.strokeWidth, 1.0, 1.0, false);
    }
    canvas.restore();
    return Shot{canvas.getImageData(0, 0, kWidth, kHeight)};
}

struct Diff {
    double differingFraction = 0;
    int maxDelta = 0;
    std::size_t inkPixels = 0;
};

Diff compare(const Shot& a, const Shot& b) {
    Diff d;
    const std::size_t count = std::min(a.rgba.size(), b.rgba.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i + 3 < count; i += 4) {
        int worst = 0;
        for (int c = 0; c < 4; ++c) {
            worst = std::max(worst, std::abs(static_cast<int>(a.rgba[i + c]) -
                                             static_cast<int>(b.rgba[i + c])));
        }
        if (worst > 0) ++differing;
        d.maxDelta = std::max(d.maxDelta, worst);
        if (a.rgba[i + 3] > 8) ++d.inkPixels;
    }
    d.differingFraction = static_cast<double>(differing) / (count / 4);
    return d;
}

} // namespace

TEST(text_cache_bakes_a_rotated_run_where_the_rasterizer_would_put_it) {
    std::string error;
    CHECK(ui::Fonts::init(dataDir(), error));
    ui::clearTextCache();

    // The bestiary's tally tilt, the item badge's, and a right angle -- the
    // last because a rotation that leaves the axes swapped is where a bounds
    // pass that forgot to turn with the ink shows up as a crop.
    for (const double angle : {19.0 * 3.14159265358979 / 180.0, -0.22, 1.5707963}) {
        const Shot direct = render("x2.1k", angle, false);
        const Shot cached = render("x2.1k", angle, true);
        const Diff d = compare(direct, cached);
        // The run really did draw -- otherwise two blank surfaces would agree.
        CHECK(d.inkPixels > 100);
        // Measured 0.55-0.72% of pixels at a worst channel delta of 11, all of
        // it on glyph edges. The bounds are the nearest round numbers above.
        CHECK(d.differingFraction < 0.015);
        CHECK(d.maxDelta <= 32);
    }
}

TEST(text_cache_reuses_one_entry_for_a_repeated_rotated_run) {
    std::string error;
    CHECK(ui::Fonts::init(dataDir(), error));
    ui::clearTextCache();

    // A grid of tilted tallies is what the bestiary draws; the point of the
    // cache is that the second row of them costs a blit, not a bake.
    for (int i = 0; i < 24; ++i) render("x9", 0.33, true);

    std::size_t entries = 0, bytes = 0;
    ui::textCacheStats(entries, bytes);
    CHECK(entries == 1);
    CHECK(bytes > 0);
}

TEST(text_cache_still_declines_a_sheared_transform) {
    std::string error;
    CHECK(ui::Fonts::init(dataDir(), error));
    ui::clearTextCache();

    Canvas canvas = Canvas::createVirtual(kWidth, kHeight);
    canvas.save();
    canvas.translate(60.0f, 45.0f);
    // Non-uniform: the ink would have to be resampled, so this must fall
    // through to the direct path and leave the cache empty.
    canvas.scale(1.0f, 2.5f);
    ui::paintRun(canvas, "x9", 0.0, 0.0, tallyStyle(), 1.0, 1.0, false);
    canvas.restore();

    std::size_t entries = 0, bytes = 0;
    ui::textCacheStats(entries, bytes);
    CHECK(entries == 0);
}
