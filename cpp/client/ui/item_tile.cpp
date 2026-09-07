#include "client/ui/item_tile.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "client/ui/draw.h"
#include "client/ui/menu_widgets.h"
#include "client/ui/text.h"
#include "shared/game/components.h"
#include "shared/game/config.h"

namespace flix::ui {

namespace {

/// The plate: a 60x60 rounded rect in the darker shade with a SHARP 50x50 of
/// the rarity colour inside it. Two fills rather than a stroke, so the visible
/// border is a full 5 design units instead of the 2.5 a centred stroke leaves.
constexpr double kPlateSide = 60.0;
constexpr double kPlateRadius = 3.0;
constexpr double kFaceSide = 50.0;

/// The drop's backdrop, from gardn's render_drop: 3 units proud of the plate
/// on every side, at a quarter black.
constexpr double kShadowSide = 68.0;
constexpr double kShadowRadius = 4.0;
constexpr double kShadowAlpha = 0.15;

constexpr double kNameSize = 12.0;
constexpr double kNameBaseline = 20.0;
constexpr double kNameStroke = 3.0;
/// The stack count, top-right, tilted as the reference shot draws it. A count
/// lying flat on a square plate reads as part of the artwork; the tilt is what
/// makes it a sticker on the tile instead.
constexpr double kBadgeSize = 16.0;
constexpr double kBadgeTiltRadians = -14.0 * kPi / 180.0;
/// The corner it hangs off, in the design cell. The plate's own corner rather
/// than the face's: the tilt swings the text's right end a couple of units
/// past the anchor, which lands it flush with the plate's edge.
constexpr double kBadgeAnchorX = 26.0;
constexpr double kBadgeAnchorY = -26.0;

constexpr double kHoverAlpha = 0.15;
constexpr double kDisabledAlpha = 0.6;
constexpr std::uint32_t kDisabledFill = 0x3A3A3Au;

/// The wedge is swept from a 90-unit radius so its straight edges leave the
/// plate rather than ending inside it; the clip is what makes it a corner.
constexpr double kReloadRadius = 90.0;
constexpr double kReloadAlpha = 0.25;

/// gardn's `clump_radius` (10) over its clustered petals' radius (7).
constexpr double kClusterRingRatio = 10.0 / 7.0;

/// gardn's icon radius for every petal both games have, ported from
/// PETAL_DATA in ~/gardn/Shared/StaticData.cc. Rarity-invariant there, and so
/// here: gardn calls radius a tactical knob, not a power knob.
///
/// This table exists because `size` and gardn's `radius` are not the same
/// quantity and no single multiplier reconciles them. Scaling by size alone
/// (kPetalIconSize) is exact for nineteen of these and close for a dozen more,
/// but it draws basic at twice gardn's, stinger, peas, pollen and sand at
/// 1.43x, and third eye, dahlia, corn, square and cactus at half to two
/// thirds -- which is exactly the "some too big, some too small" it reads as.
/// The comments below are the ones where the two disagree; a row with no
/// comment is one the fallback would already have got right, kept so the table
/// is a straight transcription rather than a diff nobody can check.
///
/// A petal gardn does not have -- every egg, and this game's own additions --
/// is not in here and falls back to kPetalIconSize. Add a row when gardn
/// grows one, never to hand-tune an icon: this is a port, not a taste.
struct GardnIconRadius {
    const char* id;
    double radius;
};
constexpr GardnIconRadius kGardnIconRadius[] = {
    {"antennae", 12.5},  // size 1 -> 10
    {"basic", 10},  // size 2 -> 20
    {"bone", 12},  // size 1 -> 10
    {"bubble", 12},  // size 1 -> 10
    {"cactus", 15},  // size 1 -> 10
    {"corn", 16},  // size 1 -> 10
    {"cutter", 40},  // size 7 -> 70
    {"dahlia", 7},  // size 0.33 -> 3.3
    {"dandelion", 10},
    {"egg", 12.5},  // size 1 -> 10
    {"faster", 7},  // size 0.5 -> 5
    {"heaviest", 12},  // size 1.3 -> 13
    {"honey", 11},
    {"iris", 7},
    {"leaf", 10},
    {"lightning", 10},
    {"lotus", 12},
    {"magnet", 10},
    {"missile", 10},
    {"moon", 50},  // size 2.6 -> 26
    {"observer", 12.5},  // size 1 -> 10
    {"peas", 7},  // size 1 -> 10
    {"pincer", 10},
    {"pollen", 7},  // size 1 -> 10
    {"powder", 10},
    {"rice", 13},
    {"rock", 12},  // size 1 -> 10
    {"rose", 10},  // size 0.9 -> 9
    {"sand", 7},  // size 1 -> 10
    {"shell", 10},  // size 1.1 -> 11
    {"soil", 10},
    {"sponge", 12},  // size 1 -> 10
    {"square", 15},  // size 1 -> 10
    {"starfish", 8},  // size 1 -> 10
    {"stick", 15},  // size 1.3 -> 13
    {"stinger", 7},  // size 1 -> 10
    {"third_eye", 20},  // size 1 -> 10
    {"uranium", 10},
    {"web", 10},  // size 1.2 -> 12
    {"wing", 10},
    {"yggdrasil", 12},  // size 1 -> 10
    {"yin_yang", 10},
    {"yucca", 10},
};

/// The icon diameter for one petal, in design units.
///
/// The id lookup is resolved once into a table indexed by petal index: the
/// content registry is loaded before anything draws and never reloaded, and a
/// panel of sixty tiles would otherwise run sixty string scans a frame.
double iconDiameter(std::uint16_t petalIndex, double sizeStat) {
    static const std::vector<double> byIndex = [] {
        std::vector<double> out(content().petalCount(), 0.0);
        for (std::uint16_t i = 0; i < content().petalCount(); ++i) {
            const std::string& id = content().petal(i).id;
            for (const GardnIconRadius& row : kGardnIconRadius) {
                if (id == row.id) {
                    out[i] = row.radius * 2.0;
                    break;
                }
            }
        }
        return out;
    }();
    if (petalIndex < byIndex.size() && byIndex[petalIndex] > 0) return byIndex[petalIndex];
    return kPetalIconSize * (sizeStat > 0 ? sizeStat : 1.0);
}

/// gardn's smootherstep on the remaining fraction: the sweep eases in and out
/// instead of ticking round at a constant rate.
double smootherStep(double t) {
    return t * t * t * (t * (6.0 * t - 15.0) + 10.0);
}

} // namespace

void drawPetalCluster(Canvas& canvas, const SpriteCache& sprites, std::uint16_t petalIndex,
                      double sizeStat, int count, double cx, double cy, double maxDiameter,
                      double timeSeconds) {
    if (petalIndex == kNoPetal || !sprites.petalDrawable(petalIndex)) return;

    const double diameter = iconDiameter(petalIndex, sizeStat);
    // A configured count below one means "not a stack" -- third eye, antennae
    // and the observer all declare zero and are drawn as a single icon.
    const int drawCount = count >= 1 ? count : 1;
    // gardn spaces a cluster on a ring a little wider than the petal itself:
    // its clustered petals are radius 7 on a `clump_radius` of 10. Kept as a
    // RATIO rather than gardn's fixed 10, because this game authors petal size
    // per petal -- a fixed ring would leave a large petal's cluster fused into
    // a blob and a small one's scattered.
    const double ring = drawCount > 1 ? diameter * 0.5 * kClusterRingRatio : 0.0;

    // Shrink only what would overflow. A cluster that already fits keeps its
    // natural size, which is the whole point of drawing petals to scale.
    const double clusterDiameter = ring * 2.0 + diameter;
    const double fit =
        (maxDiameter > 0 && clusterDiameter > maxDiameter) ? maxDiameter / clusterDiameter : 1.0;

    if (drawCount == 1) {
        sprites.drawPetal(canvas, petalIndex, cx, cy, diameter * fit, 0.0, timeSeconds);
        return;
    }

    for (int i = 0; i < drawCount; ++i) {
        const double angle = (static_cast<double>(i) / drawCount) * kTau;
        // Rotated to its own angle, so the ring reads as petals facing outward
        // rather than a row of identical stamps.
        sprites.drawPetal(canvas, petalIndex, cx + std::cos(angle) * ring * fit,
                          cy + std::sin(angle) * ring * fit, diameter * fit, angle, timeSeconds);
    }
}

void drawItemTile(Canvas& canvas, const SpriteCache& sprites, Rect rect, const ItemTile& tile) {
    const double side = std::min(rect.w, rect.h);
    if (side <= 0.0 || tile.alpha <= 0.0) return;
    const double scale = side / kItemTileDesign;

    const bool filled = !tile.empty && tile.petalIndex != kNoPetal;
    const std::uint32_t base = tile.empty ? tile.emptyFill : rarityColor(tile.rarity);
    const std::uint32_t border =
        tile.empty ? tile.emptyBorder : hsvScale(base, kItemTilePlateShade);

    canvas.save();
    canvas.translate(static_cast<float>(rect.x + rect.w * 0.5),
                     static_cast<float>(rect.y + rect.h * 0.5));
    canvas.scale(static_cast<float>(scale), static_cast<float>(scale));
    if (tile.alpha < 1.0) canvas.setGlobalAlpha(static_cast<float>(tile.alpha));

    if (tile.shadow) {
        setFill(canvas, kInk, kShadowAlpha);
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(-kShadowSide * 0.5),
                         static_cast<float>(-kShadowSide * 0.5), static_cast<float>(kShadowSide),
                         static_cast<float>(kShadowSide), static_cast<float>(kShadowRadius));
        canvas.fill();
    }

    setFill(canvas, border);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(-kPlateSide * 0.5), static_cast<float>(-kPlateSide * 0.5),
                     static_cast<float>(kPlateSide), static_cast<float>(kPlateSide),
                     static_cast<float>(kPlateRadius));
    canvas.fill();
    setFill(canvas, base);
    canvas.fillRect(static_cast<float>(-kFaceSide * 0.5), static_cast<float>(-kFaceSide * 0.5),
                    static_cast<float>(kFaceSide), static_cast<float>(kFaceSide));

    if (tile.hovered) {
        setFill(canvas, kPaper, kHoverAlpha);
        canvas.fillRect(static_cast<float>(-kFaceSide * 0.5), static_cast<float>(-kFaceSide * 0.5),
                        static_cast<float>(kFaceSide), static_cast<float>(kFaceSide));
    }

    // Everything below is clipped to the face, which is what turns the reload
    // wedge into a corner sweep and keeps a long name inside the plate.
    canvas.save();
    canvas.beginPath();
    canvas.rect(static_cast<float>(-kFaceSide * 0.5), static_cast<float>(-kFaceSide * 0.5),
                static_cast<float>(kFaceSide), static_cast<float>(kFaceSide));
    canvas.clip();

    if (filled && tile.reload < 1.0) {
        const double remaining = smootherStep(1.0 - std::max(0.0, tile.reload));
        setFill(canvas, kInk, kReloadAlpha);
        canvas.beginPath();
        canvas.moveTo(0.0f, 0.0f);
        canvas.arc(0.0f, 0.0f, static_cast<float>(kReloadRadius),
                   static_cast<float>(-kPi * 0.5 - remaining * kPi * 10.0),
                   static_cast<float>(-kPi * 0.5 - remaining * kPi * 8.0), false);
        canvas.closePath();
        canvas.fill();
    }

    if (filled) {
        const PetalStats stats = content().petalStats(tile.petalIndex, tile.rarity);
        canvas.save();
        canvas.translate(0.0f, static_cast<float>(-kItemTileIconRise));
        canvas.scale(static_cast<float>(kItemTileIconScale),
                     static_cast<float>(kItemTileIconScale));
        drawPetalCluster(canvas, sprites, tile.petalIndex, stats.size, stats.count, 0.0, 0.0,
                         kItemTileIconCap, tile.timeSeconds);
        canvas.restore();
    }

    if (filled && tile.showName) {
        const std::string name = tile.nameOverride.empty()
                                     ? titleCase(content().petal(tile.petalIndex).name)
                                     : tile.nameOverride;
        if (!name.empty()) {
            TextStyle label;
            label.bold = true;
            label.size = kNameSize;
            // Shrink to the face rather than clipping: a truncated petal name
            // reads as a different petal.
            const double measured = measure(name, kNameSize, true);
            if (measured > kFaceSide) {
                label.size = std::max(6.0, kNameSize * kFaceSide / measured);
            }
            label.fill = kPaper;
            label.stroke = kInk;
            // In design units, so the outline lands at a constant 3 screen px
            // whatever size the tile is drawn at.
            label.strokeWidth = kNameStroke / scale;
            label.align = Align::Centre;
            label.baseline = Baseline::Middle;
            text(canvas, name, 0.0, kNameBaseline, label);
        }
    }

    canvas.restore();  // unclip

    // The badge sits OUTSIDE the face clip, so it reaches the plate's own
    // corner the way the reference's does. Inside it, the count was held a
    // border's width in from the edge and read as floating over the icon
    // rather than as a label pinned to the tile.
    if (!tile.badge.empty()) {
        TextStyle badge;
        badge.bold = true;
        badge.size = kBadgeSize;
        badge.fill = kPaper;
        badge.stroke = kInk;
        badge.strokeWidth = kNameStroke / scale;
        badge.align = Align::Right;
        badge.baseline = Baseline::Top;
        badge.roundJoin = true;
        // Rotated about the corner it is anchored to, so the tilt lifts the
        // text's head to the right and swings its tail down and away.
        canvas.save();
        canvas.translate(static_cast<float>(kBadgeAnchorX), static_cast<float>(kBadgeAnchorY));
        canvas.rotate(static_cast<float>(kBadgeTiltRadians));
        text(canvas, tile.badge, 0.0, 0.0, badge);
        canvas.restore();
    }

    // The selection ring goes OUTSIDE the clip, or the half of it that lands
    // on the plate's border is eaten.
    if (tile.selected) {
        setStroke(canvas, kPaper);
        canvas.setLineWidth(3.0f);
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(-kPlateSide * 0.5 + 1.5),
                         static_cast<float>(-kPlateSide * 0.5 + 1.5),
                         static_cast<float>(kPlateSide - 3.0),
                         static_cast<float>(kPlateSide - 3.0), static_cast<float>(kPlateRadius));
        canvas.stroke();
    }

    if (tile.disabled) {
        setFill(canvas, kDisabledFill, kDisabledAlpha);
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(-kPlateSide * 0.5),
                         static_cast<float>(-kPlateSide * 0.5), static_cast<float>(kPlateSide),
                         static_cast<float>(kPlateSide), static_cast<float>(kPlateRadius));
        canvas.fill();
    }

    canvas.restore();
}

} // namespace flix::ui
