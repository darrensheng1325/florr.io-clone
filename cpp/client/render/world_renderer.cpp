#include "client/render/world_renderer.h"

#include <chrono>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "client/render/art_cache.h"
#include "client/ui/draw.h"
#include "client/ui/item_tile.h"
#include "shared/game/config.h"
#include "shared/game/components.h"
#include "shared/game/constants.h"
#include "shared/game/map_elements.h"
#include "shared/game/terrain.h"

namespace flix {

namespace {

/// A floating number ages by exactly one browser frame per frame there: it
/// rises one world unit and loses a hundredth of its alpha, so at 60 Hz it
/// lives 100 frames and climbs 100 units. Expressed in seconds so this client
/// looks the same at any refresh rate.
constexpr double kNumberLifeSeconds = 100.0 / 60.0;
constexpr double kNumberRise = 100.0;
/// Both spawn 20 world units above the body that was hit.
constexpr double kNumberSpawnRise = 20.0;
/// Damage to a flower is reported per hit at 20; damage to a mob is throttled
/// into a 100 ms bucket and shown at 16.
constexpr double kPlayerNumberSize = 20.0;
constexpr double kMobNumberSize = 16.0;
constexpr double kDamageTextThrottleSeconds = 0.1;
constexpr std::uint32_t kDamageTextColor = 0xFF0000u;

/// A mob balloons to three times its size and fades out over this, which is
/// the only feedback there is that something died.
constexpr double kDeathAnimationSeconds = 0.2;

/// Ceilings on the two per-mob tables the renderer keeps of its own accord.
constexpr std::size_t kMaxDyingMobs = 64;
/// Loot that vanished at once -- leaving a world drops every drop in view in
/// one frame -- animates out under the same kind of ceiling.
constexpr std::size_t kMaxDyingDrops = 64;
constexpr std::size_t kMaxMobShadows = 1024;

/// The window the browser build's server averages a dummy's DPS over.
constexpr double kDpsWindowSeconds = 10.0;

/// Explosion: a ring pair plus debris, all of it over one second.
constexpr double kExplosionLifeSeconds = 1.0;
constexpr std::uint32_t kExplosionOuter = 0xFF4500u;
constexpr std::uint32_t kExplosionInner = 0xFFD700u;
/// The browser build integrates its particles once per 60 Hz frame; both its
/// velocities and its 16 ms life step are therefore per frame, not per second.
constexpr double kFramesPerSecond = 60.0;

/// The flower artwork is drawn in its own radius-25 space and scaled by the
/// player's size multiplier alone. `entity.radius` is the gameplay hitbox,
/// which grows with level -- the body never does.
constexpr double kFlowerArtRadius = 25.0;

/// A petal's artwork is 20 world units of diameter per size unit -- gardn's
/// petal radius, and the scale the item tile states its icons in
/// (ui::kPetalArtSize). Its hit radius is the same 10 per size unit -- a petal
/// hits what it looks like it hits -- and is only ever drawn as a debug
/// circle, which is now a ring around the artwork rather than well outside it.
using ui::kPetalArtSize;
constexpr double kPetalHitSize = 10.0;
/// A projectile is drawn from the same petal artwork, filling its own body:
/// diameter is twice the radius the server replicated for it.
///
/// It has to come off the wire rather than out of the petal config, because a
/// shot's size is NOT its petal's. It is scaled by whatever fired it -- a
/// mob's shot by the shooter's body over kProjectileSizeDivisor, a flower's by
/// how much the flower has grown -- and the browser build draws mob shots from
/// exactly that scaled number (`projectile.size * 20`, game-objects.ts). Using
/// the config's unscaled `size` here drew every mob's ammunition at a stock
/// flower's calibre, three times the body it actually hits with.
constexpr double kProjectileArtSize = 20.0;
constexpr double kProjectileArtPerRadius = 2.0;

/// A ground drop is the same item tile the menus draw, at its design size and
/// with the shadow behind it. Nothing about it is derived from the drop's
/// pickup radius: every drop reads the same size, whatever petal is on it.
constexpr double kDropBackdropSide = ui::kItemTileDesign;
/// Slides in from 30-50 units away, unwinding a spin of up to half a turn.
constexpr double kDropSpawnSeconds = 0.4;
constexpr double kDropSpawnNear = 30.0;
constexpr double kDropSpawnSpread = 20.0;
/// Flies to whoever took it, shrinking and fading; or spins out where it lay.
constexpr double kDropPickupSeconds = 0.15;
constexpr double kDropDespawnSeconds = 0.3;
/// Loot never settles perfectly square: each drop rests tilted by up to this
/// many degrees either way. Seeded on the net id rather than rolled, so the
/// tilt is the same every frame, survives the drop leaving and re-entering
/// view, and is shared by the spawn, pickup and despawn animations.
constexpr double kDropRestTiltDegrees = 10.0;
/// Loot breathes where it lies: the reference scales it by 1 +- 3% off a sine
/// on the frame clock in milliseconds, i.e. 10 rad/s, and shares one phase
/// across every drop rather than giving each its own.
constexpr double kDropPulseRate = 10.0;
constexpr double kDropPulseAmount = 0.03;

double dropRestRotation(std::uint32_t netId) {
    // A cheap integer hash: consecutive net ids must not land on neighbouring
    // angles, or a burst of loot from one mob settles in a visible fan.
    std::uint32_t h = netId * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    const double unit = static_cast<double>(h % 2001u) / 2000.0;  // 0..1
    return (unit - 0.5) * 2.0 * kDropRestTiltDegrees * kPi / 180.0;
}

/// The high rarities shimmer. Rolled per drawn frame, per petal and per drop,
/// exactly as the browser build rolls it.
constexpr double kSparkleChance = 0.1;
constexpr int kSparkleCount = 8;
constexpr double kSparkleLifeSeconds = 3.0;
/// A drop throws a shorter, faster burst of the same particles when it lands.
constexpr int kDropBurstCount = 7;
constexpr double kDropBurstLifeSeconds = 0.7;
/// A petal's shimmer is the rarity colour blended halfway to white. A drop's
/// is the rarity colour itself -- only its alpha moves, so the grains read as
/// the drop's own rarity rather than as a wash of white.
constexpr double kSparkleWhiten = 0.5;
constexpr double kDropSparkleWhiten = 0.0;
/// A drop's grains are five times a petal's and vary widely in size, so it
/// throws far fewer of them: at this scale a petal's count would read as a
/// solid slab rather than as a scatter. Base plus a spread of twice the base
/// puts the mean at 5x the petal grain with a 3:1 spread between the smallest
/// and the largest.
///
/// Grains per second a drop emits, as a steady trickle. The shimmer used to
/// roll a 10%-per-frame chance to throw six at once -- six emissions a second,
/// six grains each -- so 36 a second is that same density arriving evenly
/// instead of in clumps.
constexpr double kDropSparkleRate = 36.0;
constexpr double kDropSparkleSpeed = 1.2;
constexpr double kDropSparkleSpeedSpread = 1.2;
constexpr double kDropSparkleLifeMs = 2000.0;
constexpr double kDropSparkleLifeSpreadMs = 1000.0;
/// A drop keeps roughly rate x life grains alive, so a screen of them is
/// bounded here rather than by the effect pool it no longer shares.
constexpr std::size_t kMaxDropSparkles = 512;
constexpr double kDropSparkleSize = 5.0;
constexpr double kDropSparkleSizeSpread = 10.0;
constexpr double kDropBurstSize = 7.5;
constexpr double kDropBurstSizeSpread = 15.0;

/// What tells a drop's shimmer apart from a petal's. A petal's grains keep the
/// browser build's spoked emission -- evenly spaced angles with a little
/// jitter -- because that is what the shipped build looks like. A drop's
/// scatter instead: with only a handful in flight at a time, evenly spaced
/// angles read as spokes rather than as a burst, so each grain picks its own
/// direction and its own facing.
struct SparkleStyle {
    double whiten = 0;
    bool square = false;
    bool scatter = false;
};
constexpr SparkleStyle kPetalSparkleStyle{kSparkleWhiten, false, false};
constexpr SparkleStyle kDropSparkleStyle{kDropSparkleWhiten, true, true};
/// A grain never paints solid: the shimmer sits over the body it came off.
constexpr double kSparkleAlpha = 0.6;

/// One shimmer grain, wherever it is pooled. It shrinks and fades on its own
/// clock rather than on any effect's, and a square one is walked as a
/// four-point path rather than pushed through the transform stack -- at this
/// size a grain is four lineTo calls, and a save/rotate/restore per grain
/// costs the rasterizer more than the square itself does.
void drawSparkleGrain(Canvas& canvas, const Camera& camera, const EffectParticle& p, bool square) {
    const double left = p.lifeSeconds / p.maxLifeSeconds;
    if (left <= 0) return;
    const Vec2 at = camera.worldToScreen(p.position);
    const double r = p.size * left * camera.zoom();
    ui::setFill(canvas, p.color, left * kSparkleAlpha);
    if (!square) {
        canvas.fillCircle(static_cast<float>(at.x), static_cast<float>(at.y),
                          static_cast<float>(r));
        return;
    }
    // Hard corners, no radius, turned to its own facing.
    const double c = std::cos(p.rotation) * r;
    const double s = std::sin(p.rotation) * r;
    canvas.beginPath();
    canvas.moveTo(static_cast<float>(at.x - c + s), static_cast<float>(at.y - s - c));
    canvas.lineTo(static_cast<float>(at.x + c + s), static_cast<float>(at.y + s - c));
    canvas.lineTo(static_cast<float>(at.x + c - s), static_cast<float>(at.y + s + c));
    canvas.lineTo(static_cast<float>(at.x - c - s), static_cast<float>(at.y - s + c));
    canvas.closePath();
    canvas.fill();
}

/// A poison tick is purple and stands 14 units to the right of the body, so a
/// petal hit landing in the same tick cannot stack on top of it.
constexpr std::uint32_t kPoisonTextColor = 0xCE76DBu;
constexpr double kPoisonNumberOffsetX = 14.0;

/// The ceiling the browser build's server clamps a flower's size modifier to.
constexpr double kMaxSizeMultiplier = 6.0;

/// The ALT rarity glow's reach past the petal's own artwork. The browser build
/// bakes it as a 16-unit shadow-blur pad; cpp_canvas has no blur, so it is the
/// same reach painted as the nested-disc ramp drawPetalGlow builds.
constexpr double kPetalGlowPad = 16.0;

/// A mob's bar never shrinks below the width a common hornet asks for, and is
/// always eight units tall.
constexpr double kMobBarMinWidth = 60.0;
constexpr double kMobBarHeight = 8.0;

/// Flower-shaped mobs. Neither colour is read off mob stats: the loader
/// overwrites every mob's colour with its rarity colour, and the whole point
/// of these two is that they look like flowers.
constexpr std::uint32_t kDiggerBodyColor = 0x999999u;
constexpr std::uint32_t kPetalRingBodyColor = 0xFFE763u;
/// The ring a petal_ring mob carries, as multiples of its own radius.
constexpr double kPetalRingOrbitScale = 2.4;
constexpr double kPetalRingPetalScale = 0.55;

/// A petal's `visual_scale`, as everything that paints one has to read it: art
/// only, and a zero (or an absent field) means "unscaled" rather than
/// "invisible", exactly as MobConfig::visualScale is treated.
double petalArtScale(const PetalConfig* config) {
    return (config && config->visualScale > 0) ? config->visualScale : 1.0;
}

/// How far off screen a teleporter still counts as visible. Its glow is 130
/// units wide, so it has to be drawn before its centre reaches the edge.
constexpr double kTeleporterCull = 140.0;

/// Spawn-shield yellow, and how long it takes to bleed back to health green
/// once the shield drops.
constexpr std::uint32_t kInvulnHealth = 0xFAFFC9u;
constexpr double kInvulnFadeSeconds = 0.5;

/// The base fill of each ground type's artwork, in ground-id order. Painted
/// only until the art has loaded, or if its file could not be read at all.
constexpr std::uint32_t kGroundFallback[kSectionCount] = {
    0x1EA761u,  // Garden
    0xEAE4D0u,  // Desert
    0xA31414u,  // Hel
    0x4AA7F7u,  // Ocean
    0x8E6140u,  // Ant Hell
    0x15A12Fu,  // Jungle
    0x633500u,  // Sewers
    0x000000u,  // Computer
    0x000000u,  // Unknown
};

/// One ground tile is 400 world units of artwork, drawn two units oversized so
/// that neighbouring tiles never leave a seam.
constexpr double kGroundTileSize = 400.0;
constexpr double kGroundOverlap = 2.0;

constexpr std::uint32_t kWaterFill = 0x4169E1u;
constexpr std::uint32_t kWaterBorder = 0x2A4FA0u;

constexpr std::uint32_t kTileColor(Tile tile) {
    switch (tile) {
        case Tile::Water: return kWaterFill;
        case Tile::Sand:  return 0xBBBBBBu;   // bridge: the base under its planks
        case Tile::Stone: return 0x786828u;   // sewage: a solid fill in its own art
        case Tile::Block: return 0x00FF00u;
        default: return 0x000000u;
    }
}

/// Damage is shown as a whole number however large it gets -- abbreviating it
/// would read as a different game from the browser build.
std::string formatDamage(double value) {
    return std::to_string(static_cast<long long>(std::llround(value)));
}

/// The browser build's formatNumber: one decimal and a magnitude letter past a
/// thousand. Only the dummy's DPS readout is written this way.
std::string formatCompact(double value) {
    static constexpr struct { double scale; const char* suffix; } kSteps[] = {
        {1e12, "T"}, {1e9, "B"}, {1e6, "M"}, {1e3, "K"},
    };
    char buf[32];
    for (const auto& step : kSteps) {
        if (value >= step.scale) {
            std::snprintf(buf, sizeof buf, "%.1f%s", value / step.scale, step.suffix);
            return buf;
        }
    }
    return std::to_string(static_cast<long long>(std::llround(value)));
}

/// The four tiers that shimmer in the browser build.
bool sparklingRarity(Rarity rarity) {
    return rarity == Rarity::Ultra || rarity == Rarity::Super || rarity == Rarity::Unique ||
           rarity == Rarity::Apex;
}

/// Jitter for a particle burst. Deliberately not reproducible across clients:
/// nothing seeded from it is simulated, and debris that matched frame for
/// frame on two machines would still look the same as debris that did not.
double randomUnit() {
    static std::uint32_t state = 0x2545F491u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<double>(state >> 8) * (1.0 / 16777216.0);
}

/// Straight per-channel lerp, rounded the way the browser build rounds it.
std::uint32_t lerpColor(std::uint32_t from, std::uint32_t to, double t) {
    const auto channel = [t](std::uint32_t a, std::uint32_t b) {
        return static_cast<std::uint32_t>(
            clamp(std::round(a + (static_cast<double>(b) - a) * t), 0.0, 255.0));
    };
    return (channel((from >> 16) & 0xFF, (to >> 16) & 0xFF) << 16) |
           (channel((from >> 8) & 0xFF, (to >> 8) & 0xFF) << 8) |
           channel(from & 0xFF, to & 0xFF);
}

std::uint32_t scaleColor(std::uint32_t rgb, double factor) {
    const auto channel = [factor](std::uint32_t c) {
        return static_cast<std::uint32_t>(clamp(std::round(c * factor), 0.0, 255.0));
    };
    return (channel((rgb >> 16) & 0xFF) << 16) |
           (channel((rgb >> 8) & 0xFF) << 8) |
           channel(rgb & 0xFF);
}

std::uint32_t mixWithWhite(std::uint32_t rgb, double amount) {
    const auto channel = [amount](std::uint32_t c) {
        return static_cast<std::uint32_t>(clamp(std::round(c + (255.0 - c) * amount), 0.0, 255.0));
    };
    return (channel((rgb >> 16) & 0xFF) << 16) |
           (channel((rgb >> 8) & 0xFF) << 8) |
           channel(rgb & 0xFF);
}

/// Same stable integer-hash shape used by the TypeScript glitch effect. The
/// C++ client has numeric network ids instead of socket-id strings, so the id
/// is the stable per-player seed.
double hash01(std::uint32_t a, std::uint32_t b) {
    std::uint32_t h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<double>(h ^ (h >> 16)) / 4294967296.0;
}

// The TypeScript wall texture is a 124-unit SVG rasterised into every
// 300-unit tile. Keeping the source coordinates here preserves the texture's
// global phase on both normal cells and their jagged edge protrusions.
constexpr double kWallTextureViewBox = 124.0;
constexpr std::uint32_t kWallFill = 0x99550Cu;
constexpr std::uint32_t kWallDotColor = 0x783F01u;
constexpr double kWallDotRadius = 5.1641;
constexpr std::array<Vec2, 5> kWallDots = {{
    {25.2109, 51.5391}, {105.5341, 25.5207}, {51.5308, 85.3607},
    {64.5341, 15.5207}, {103.5341, 102.5207},
}};
constexpr double kWallOverlap = 1.5;

enum class WallEdge { Top = 0, Bottom = 1, Left = 2, Right = 3 };

/// The outline this tile edge is DRAWN with, which is by construction the one
/// the server collides against: flix::jaggedEdge is the only generator, and
/// terrain.cpp's wall push-out calls the same function.
const JaggedEdge& jaggedPoints(int tileX, int tileY, WallEdge edge) {
    return jaggedEdge(tileX, tileY, static_cast<int>(edge));
}

/// Which sides this tile shows, from the same shared answer the server's wall
/// push-out uses. It must not be re-derived here: a local copy once tested
/// tileBlocks(), which counts water as blocking because water stops movement,
/// and so ruled every lake into 300-unit tiles with a shoreline drawn along
/// each interior seam.
bool tileEdgeExposed(const Terrain& terrain, int tileX, int tileY, WallEdge edge) {
    return jaggedEdgeExposed(terrain, tileX, tileY, static_cast<int>(edge));
}

Vec2 edgeBasePoint(double worldX, double worldY, WallEdge edge, double t) {
    switch (edge) {
        case WallEdge::Top: return {worldX + t, worldY};
        case WallEdge::Bottom: return {worldX + t, worldY + kTileSize};
        case WallEdge::Left: return {worldX, worldY + t};
        case WallEdge::Right: return {worldX + kTileSize, worldY + t};
    }
    return {};
}

Vec2 edgePoint(double worldX, double worldY, WallEdge edge, const JaggedEdgePoint& point) {
    switch (edge) {
        case WallEdge::Top: return {worldX + point.t, worldY - point.offset};
        case WallEdge::Bottom: return {worldX + point.t, worldY + kTileSize + point.offset};
        case WallEdge::Left: return {worldX - point.offset, worldY + point.t};
        case WallEdge::Right: return {worldX + kTileSize + point.offset, worldY + point.t};
    }
    return {};
}

void moveToScreen(Canvas& canvas, const Camera& camera, Vec2 world) {
    const Vec2 screen = camera.worldToScreen(world);
    canvas.moveTo(static_cast<float>(screen.x), static_cast<float>(screen.y));
}

void lineToScreen(Canvas& canvas, const Camera& camera, Vec2 world) {
    const Vec2 screen = camera.worldToScreen(world);
    canvas.lineTo(static_cast<float>(screen.x), static_cast<float>(screen.y));
}

void quadToScreen(Canvas& canvas, const Camera& camera, Vec2 control, Vec2 world) {
    const Vec2 c = camera.worldToScreen(control);
    const Vec2 p = camera.worldToScreen(world);
    canvas.quadraticCurveTo(static_cast<float>(c.x), static_cast<float>(c.y),
                            static_cast<float>(p.x), static_cast<float>(p.y));
}

/// Clips to a world rectangle. Every tiled draw needs one: the artwork inside
/// a 400-unit ground tile is free to overflow its own box (hel.svg rotates
/// squares straight out of it) and the browser build's rasterised tile crops
/// that off for free.
void clipWorldRect(Canvas& canvas, const Camera& camera, Rect world) {
    const Vec2 topLeft = camera.worldToScreen({world.x, world.y});
    canvas.beginPath();
    canvas.rect(static_cast<float>(topLeft.x), static_cast<float>(topLeft.y),
                static_cast<float>(world.w * camera.zoom()),
                static_cast<float>(world.h * camera.zoom()));
    canvas.clip();
}

/// The overlap of two world rectangles, empty when they do not meet.
Rect intersection(Rect a, Rect b) {
    const double x0 = std::max(a.left(), b.left());
    const double y0 = std::max(a.top(), b.top());
    const double x1 = std::min(a.right(), b.right());
    const double y1 = std::min(a.bottom(), b.bottom());
    return {x0, y0, std::max(0.0, x1 - x0), std::max(0.0, y1 - y0)};
}

/// Paint a globally-phased copy of TypeScript's wall pattern inside the
/// caller's clip. The clip makes this work for a rectangle and for a jagged
/// extension without restarting the dot pattern at every exposed edge.
void paintWallPattern(Canvas& canvas, const Camera& camera, Rect worldBounds) {
    const Vec2 topLeft = camera.worldToScreen({worldBounds.x, worldBounds.y});
    const double zoom = camera.zoom();
    ui::setFill(canvas, kWallFill);
    canvas.fillRect(static_cast<float>(topLeft.x), static_cast<float>(topLeft.y),
                    static_cast<float>(worldBounds.w * zoom),
                    static_cast<float>(worldBounds.h * zoom));

    const int tileX0 = static_cast<int>(std::floor(worldBounds.left() / kTileSize));
    const int tileY0 = static_cast<int>(std::floor(worldBounds.top() / kTileSize));
    const int tileX1 = static_cast<int>(std::floor(worldBounds.right() / kTileSize));
    const int tileY1 = static_cast<int>(std::floor(worldBounds.bottom() / kTileSize));
    const double textureScale = kTileSize / kWallTextureViewBox;
    ui::setFill(canvas, kWallDotColor);
    for (int ty = tileY0; ty <= tileY1; ++ty) {
        for (int tx = tileX0; tx <= tileX1; ++tx) {
            for (const Vec2 dot : kWallDots) {
                const Vec2 screen = camera.worldToScreen({
                    tx * kTileSize + dot.x * textureScale,
                    ty * kTileSize + dot.y * textureScale,
                });
                canvas.fillCircle(static_cast<float>(screen.x), static_cast<float>(screen.y),
                                  static_cast<float>(kWallDotRadius * textureScale * zoom));
            }
        }
    }
}

template <typename TraceShape>
void fillWallShape(Canvas& canvas, const Camera& camera, Rect bounds, TraceShape&& traceShape) {
    canvas.save();
    canvas.beginPath();
    traceShape();
    canvas.clip();
    paintWallPattern(canvas, camera, bounds);
    canvas.restore();
}

void drawWallTile(Canvas& canvas, const Camera& camera, int tileX, int tileY) {
    const Rect bounds{tileX * kTileSize - kWallOverlap, tileY * kTileSize - kWallOverlap,
                      kTileSize + kWallOverlap * 2.0, kTileSize + kWallOverlap * 2.0};
    fillWallShape(canvas, camera, bounds, [&] {
        const Vec2 topLeft = camera.worldToScreen({bounds.x, bounds.y});
        canvas.rect(static_cast<float>(topLeft.x), static_cast<float>(topLeft.y),
                    static_cast<float>(bounds.w * camera.zoom()),
                    static_cast<float>(bounds.h * camera.zoom()));
    });
}

void drawJaggedWallEdge(Canvas& canvas, const Camera& camera, int tileX, int tileY,
                        WallEdge edge) {
    // drawJaggedEdge() in TypeScript saves its canvas state, so its texture
    // and stroke settings never leak into later world rendering.
    canvas.save();
    const double worldX = tileX * kTileSize;
    const double worldY = tileY * kTileSize;
    const JaggedEdge& points = jaggedPoints(tileX, tileY, edge);

    Rect bounds{worldX, worldY, 0, 0};
    double minX = worldX, maxX = worldX, minY = worldY, maxY = worldY;
    const auto include = [&](Vec2 p) {
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    };
    for (const JaggedEdgePoint& point : points) {
        include(edgeBasePoint(worldX, worldY, edge, point.t));
        include(edgePoint(worldX, worldY, edge, point));
    }
    bounds = {minX, minY, maxX - minX, maxY - minY};

    const auto traceFill = [&] {
        moveToScreen(canvas, camera, edgeBasePoint(worldX, worldY, edge, points.front().t));
        for (const JaggedEdgePoint& point : points) {
            lineToScreen(canvas, camera, edgePoint(worldX, worldY, edge, point));
        }
        lineToScreen(canvas, camera, edgeBasePoint(worldX, worldY, edge, points.back().t));
        canvas.closePath();
    };
    fillWallShape(canvas, camera, bounds, traceFill);

    ui::setStroke(canvas, kWallDotColor);
    canvas.setLineWidth(static_cast<float>(3.0 * camera.zoom()));
    canvas.setLineCap("butt");
    canvas.setLineJoin("round");
    canvas.beginPath();
    moveToScreen(canvas, camera, edgePoint(worldX, worldY, edge, points.front()));
    for (std::size_t i = 1; i < points.size(); ++i) {
        lineToScreen(canvas, camera, edgePoint(worldX, worldY, edge, points[i]));
    }
    canvas.stroke();
    canvas.restore();
}

} // namespace

void WorldRenderer::ingestEvents(WorldView& view) {
    const auto isPlayer = [&view](std::uint32_t netId) {
        const auto it = view.entities().find(netId);
        return it != view.entities().end() && it->second.kind == net::EntityKind::Player;
    };
    // The dummy exists to be hit at, so it reports what it is being hit for.
    // The browser build measures that on the server; nothing carries it on the
    // wire here, so the same ten-second window is kept from the damage events
    // the client is already being sent.
    const auto isTargetDummy = [this, &view](std::uint32_t netId) {
        if (!content_) return false;
        const auto it = view.entities().find(netId);
        if (it == view.entities().end() || it->second.kind != net::EntityKind::Mob) return false;
        return content_->mob(it->second.typeIndex).id == "target_dummy";
    };
    const auto pushNumber = [this](Vec2 at, double value, double size, bool poison) {
        if (effects_.size() >= maxEffects) return;
        Effect e;
        e.kind = Effect::Kind::DamageNumber;
        // Reported 20 units above the body that produced it, and a poison tick
        // 14 units to the side of that so it cannot land under the petal hit
        // that arrived in the same tick.
        e.position = {at.x + (poison ? kPoisonNumberOffsetX : 0.0), at.y - kNumberSpawnRise};
        e.drift = {0, -kNumberRise};
        e.value = value;
        e.textSize = size;
        e.color = poison ? kPoisonTextColor : kDamageTextColor;
        e.lifeSeconds = kNumberLifeSeconds;
        effects_.push_back(std::move(e));
    };

    // Particles for the high-rarity shimmer and the burst a drop throws when
    // it lands. The browser build integrates these once per 60 Hz frame, so
    // its speeds are per frame and are converted here the way the explosion's
    // debris is.
    const auto pushSparkle = [this](Vec2 at, Rarity rarity, int count, double speedBase,
                                    double speedSpread, double lifeBase, double lifeSpread,
                                    double sizeBase, double sizeSpread, double lifetime,
                                    SparkleStyle style) {
        // Half the pool, not all of it. The browser build keeps its shimmer in
        // a separate array from its damage numbers; sharing one here without a
        // reservation lets a loadout of ultra petals fill the pool and silence
        // every number on screen, which is the one thing that must never go.
        if (effects_.size() >= maxEffects / 2) return;
        Effect e;
        e.kind = Effect::Kind::Sparkle;
        e.position = at;
        e.lifeSeconds = lifetime;
        e.squareParticles = style.square;
        const std::uint32_t color = mixWithWhite(rarityColor(rarity), style.whiten);
        e.particles.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const double angle =
                style.scatter ? randomUnit() * kTau : kTau * i / count + randomUnit() * 0.3;
            const double speed = (speedBase + randomUnit() * speedSpread) * kFramesPerSecond;
            const double life = (lifeBase + randomUnit() * lifeSpread) / 1000.0;
            EffectParticle particle;
            particle.position = {at.x + (randomUnit() - 0.5) * 4.0,
                                 at.y + (randomUnit() - 0.5) * 4.0};
            particle.velocity = {std::cos(angle) * speed, std::sin(angle) * speed};
            particle.lifeSeconds = particle.maxLifeSeconds = life;
            particle.size = sizeBase + randomUnit() * sizeSpread;
            particle.rotation = style.square ? randomUnit() * kTau : 0.0;
            particle.color = color;
            e.particles.push_back(particle);
        }
        effects_.push_back(std::move(e));
    };

    // One grain of a drop's shimmer. Emitted singly and continuously rather
    // than a burst at a time, so it goes straight into the flat pool with no
    // effect wrapping it: it shares nothing with the grains around it.
    const auto pushDropSparkle = [this](Vec2 at, Rarity rarity) {
        if (dropSparkles_.size() >= kMaxDropSparkles) return;
        const double angle = randomUnit() * kTau;
        const double speed =
            (kDropSparkleSpeed + randomUnit() * kDropSparkleSpeedSpread) * kFramesPerSecond;
        EffectParticle particle;
        particle.position = {at.x + (randomUnit() - 0.5) * 4.0,
                             at.y + (randomUnit() - 0.5) * 4.0};
        particle.velocity = {std::cos(angle) * speed, std::sin(angle) * speed};
        particle.lifeSeconds = particle.maxLifeSeconds =
            (kDropSparkleLifeMs + randomUnit() * kDropSparkleLifeSpreadMs) / 1000.0;
        particle.size = kDropSparkleSize + randomUnit() * kDropSparkleSizeSpread;
        particle.rotation = randomUnit() * kTau;
        particle.color = mixWithWhite(rarityColor(rarity), kDropSparkleStyle.whiten);
        dropSparkles_.push_back(particle);
    };

    for (const ViewEvent& event : view.events()) {
        switch (event.kind) {
            case net::EventKind::Damage: {
                if (isTargetDummy(event.netId)) {
                    dummyDamage_[event.netId].emplace_back(nowSeconds_, event.amount);
                }
                if (!options.damageNumbers) break;
                // A flower's own damage is never throttled: you must see every
                // hit you take. A mob's is, because a full ring lands eight
                // hits in one tick and eight stacked numbers read as noise.
                // Poison accumulates under its own key: sharing one bucket
                // would let a tick land inside a petal hit's throttle window
                // and repaint the whole total purple, or the reverse.
                const bool poison = (event.flag & net::DamagePoison) != 0;
                if (isPlayer(event.netId)) {
                    pushNumber(event.position, event.amount, kPlayerNumberSize, poison);
                    break;
                }
                const std::uint64_t key =
                    static_cast<std::uint64_t>(event.netId) | (poison ? (1ull << 32) : 0ull);
                const auto bucket = damageTextAt_.find(key);
                if (bucket != damageTextAt_.end() &&
                    nowSeconds_ - bucket->second < kDamageTextThrottleSeconds) {
                    damagePending_[key] += event.amount;
                    break;
                }
                double total = event.amount;
                const auto pending = damagePending_.find(key);
                if (pending != damagePending_.end()) {
                    total += pending->second;
                    damagePending_.erase(pending);
                }
                damageTextAt_[key] = nowSeconds_;
                if (total > 0) pushNumber(event.position, total, kMobNumberSize, poison);
                break;
            }
            case net::EventKind::Killed: {
                // Whatever was still accumulating dies with the target, so
                // flush it rather than losing the killing blow's number. Both
                // buckets: a mob can die with a petal hit and a poison tick
                // still pending.
                for (const bool poison : {false, true}) {
                    const std::uint64_t key = static_cast<std::uint64_t>(event.netId) |
                                              (poison ? (1ull << 32) : 0ull);
                    const auto pending = damagePending_.find(key);
                    if (pending != damagePending_.end()) {
                        if (options.damageNumbers && pending->second > 0) {
                            pushNumber(event.position, pending->second, kMobNumberSize, poison);
                        }
                        damagePending_.erase(pending);
                    }
                    damageTextAt_.erase(key);
                }
                dummyDamage_.erase(event.netId);

                // The snapshot erased the entity before this event was read, so
                // the death animation replays the last state it was DRAWN in.
                // A miss here is a petal or a drop, neither of which animates.
                const auto shadow = mobShadows_.find(event.netId);
                if (shadow == mobShadows_.end()) break;
                if (dying_.size() < kMaxDyingMobs) {
                    DyingMob mob;
                    mob.netId = shadow->second.netId;
                    mob.position = event.position;
                    mob.angle = shadow->second.angle;
                    mob.radius = shadow->second.radius;
                    mob.typeIndex = shadow->second.typeIndex;
                    mob.rarity = shadow->second.rarity;
                    dying_.push_back(mob);
                }
                mobShadows_.erase(shadow);
                break;
            }
            case net::EventKind::Explosion: {
                if (effects_.size() >= maxEffects) break;
                Effect e;
                e.kind = Effect::Kind::Explosion;
                e.position = event.position;
                e.radius = event.radius > 0 ? event.radius : 60;
                e.lifeSeconds = kExplosionLifeSeconds;

                const int count = static_cast<int>(clamp(e.radius / 5.0, 10.0, 50.0));
                e.particles.reserve(static_cast<std::size_t>(count));
                for (int i = 0; i < count; ++i) {
                    const double angle = kTau * i / count + randomUnit() * 0.5;
                    const double speed = (2.0 + randomUnit() * 3.0) * kFramesPerSecond;
                    const double life = (800.0 + randomUnit() * 400.0) / 1000.0;
                    EffectParticle p;
                    p.position = {e.position.x + (randomUnit() - 0.5) * 10.0,
                                  e.position.y + (randomUnit() - 0.5) * 10.0};
                    p.velocity = {std::cos(angle) * speed, std::sin(angle) * speed};
                    p.lifeSeconds = p.maxLifeSeconds = life;
                    p.size = 2.0 + randomUnit() * 3.0;
                    p.color = randomUnit() > 0.5 ? kExplosionOuter : kExplosionInner;
                    e.particles.push_back(p);
                }
                effects_.push_back(std::move(e));
                break;
            }
            case net::EventKind::PickedUp: {
                // The drop is erased from the snapshot in the same tick, so
                // the flight to its taker is played from the record kept here.
                const auto known = knownDrops_.find(event.netId);
                if (known == knownDrops_.end()) break;
                DyingDrop drop = known->second;
                drop.takerNetId = event.otherNetId;
                drop.ageSeconds = 0;
                if (dyingDrops_.size() < kMaxDyingDrops) dyingDrops_.push_back(drop);
                knownDrops_.erase(known);
                dropSpawns_.erase(event.netId);
                break;
            }
            default:
                break;
        }
    }
    view.events().clear();

    // --- drops -----------------------------------------------------------
    //
    // A drop's arrival and departure are both absences rather than events: it
    // simply appears in, and disappears from, the entity stream. Both have a
    // flourish, so both are recovered by diffing what is on screen against
    // what was last frame. Done AFTER the events above so a pickup has already
    // claimed its drop and is not mistaken for a timeout.
    for (const auto& entry : view.entities()) {
        const RemoteEntity& entity = entry.second;
        if (entity.kind != net::EntityKind::Drop) continue;
        DyingDrop& record = knownDrops_[entity.netId];
        const bool firstSight = record.netId == 0;
        record.netId = entity.netId;
        record.position = entity.position;
        record.typeIndex = entity.typeIndex;
        record.rarity = entity.rarity;
        record.seenThisFrame = true;
        if (!firstSight) continue;

        DropSpawn spawn;
        spawn.angle = randomUnit() * kTau;
        spawn.distance = kDropSpawnNear + randomUnit() * kDropSpawnSpread;
        spawn.rotation = (randomUnit() - 0.5) * kTau;
        dropSpawns_[entity.netId] = spawn;
        pushSparkle(entity.position, entity.rarity, kDropBurstCount, 3.0, 3.0, 500.0, 250.0,
                    kDropBurstSize, kDropBurstSizeSpread, kDropBurstLifeSeconds,
                    kDropSparkleStyle);
    }
    for (auto it = knownDrops_.begin(); it != knownDrops_.end();) {
        if (it->second.seenThisFrame) {
            it->second.seenThisFrame = false;
            ++it;
            continue;
        }
        // Gone without a pickup: it timed out where it lay.
        DyingDrop drop = it->second;
        drop.takerNetId = 0;
        drop.ageSeconds = 0;
        if (dyingDrops_.size() < kMaxDyingDrops) dyingDrops_.push_back(drop);
        dropSpawns_.erase(it->first);
        it = knownDrops_.erase(it);
    }

    // --- the high-rarity shimmer -----------------------------------------
    //
    // The browser build rolls this once per drawn frame per body, at 60 Hz.
    // Rolling it per frame here would emit at whatever rate the display runs
    // at, so the chance is scaled by how long the frame was: the shimmer then
    // looks the same at 60 Hz and at 144.
    const double sinceLast = clamp(nowSeconds_ - lastIngestSeconds_, 0.0, 0.25);
    lastIngestSeconds_ = nowSeconds_;
    const double chance = clamp(kSparkleChance * sinceLast * kFramesPerSecond, 0.0, 1.0);
    if (chance <= 0.0) return;
    for (const auto& entry : view.entities()) {
        const RemoteEntity& entity = entry.second;
        if (!sparklingRarity(entity.rarity)) continue;

        if (entity.kind == net::EntityKind::Drop) {
            // A steady stream, not a roll: the drop is owed a fraction of a
            // grain per frame and emits whenever that has added up to a whole
            // one, which keeps the rate the same at 60 Hz and at 144.
            auto known = knownDrops_.find(entity.netId);
            if (known == knownDrops_.end()) continue;
            double& credit = known->second.sparkleCredit;
            credit += kDropSparkleRate * sinceLast;
            while (credit >= 1.0) {
                credit -= 1.0;
                pushDropSparkle(entity.position, entity.rarity);
            }
            continue;
        }

        if (entity.kind != net::EntityKind::Petal) continue;
        if (randomUnit() >= chance) continue;
        pushSparkle(entity.position, entity.rarity, kSparkleCount, 0.5, 0.5, 2000.0, 1000.0, 1.0,
                    2.0, kSparkleLifeSeconds, kPetalSparkleStyle);
    }
}

void WorldRenderer::update(double dt) {
    nowSeconds_ += dt;
    for (Effect& e : effects_) {
        e.ageSeconds += dt;
        for (EffectParticle& p : e.particles) {
            p.position += p.velocity * dt;
            p.lifeSeconds -= dt;
        }
    }
    effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                                  [](const Effect& e) { return e.ageSeconds >= e.lifeSeconds; }),
                   effects_.end());

    for (EffectParticle& p : dropSparkles_) {
        p.position += p.velocity * dt;
        p.lifeSeconds -= dt;
    }
    dropSparkles_.erase(std::remove_if(dropSparkles_.begin(), dropSparkles_.end(),
                                       [](const EffectParticle& p) { return p.lifeSeconds <= 0; }),
                        dropSparkles_.end());

    for (auto& entry : dropSpawns_) entry.second.ageSeconds += dt;
    for (auto it = dropSpawns_.begin(); it != dropSpawns_.end();) {
        it = it->second.ageSeconds >= kDropSpawnSeconds ? dropSpawns_.erase(it) : std::next(it);
    }
    for (DyingDrop& drop : dyingDrops_) drop.ageSeconds += dt;
    dyingDrops_.erase(std::remove_if(dyingDrops_.begin(), dyingDrops_.end(),
                                     [](const DyingDrop& d) {
                                         return d.ageSeconds >= (d.takerNetId != 0
                                                                     ? kDropPickupSeconds
                                                                     : kDropDespawnSeconds);
                                     }),
                      dyingDrops_.end());

    for (DyingMob& mob : dying_) mob.ageSeconds += dt;
    dying_.erase(std::remove_if(dying_.begin(), dying_.end(),
                                [](const DyingMob& m) {
                                    return m.ageSeconds >= kDeathAnimationSeconds;
                                }),
                 dying_.end());

    // A target that left view mid-throttle would otherwise keep its bucket for
    // the session. Long past the window nothing more can arrive for it, and
    // anything still pending is under 100 ms of damage on something that has
    // stopped being hit.
    constexpr double kBucketIdleSeconds = 5.0;
    for (auto it = damageTextAt_.begin(); it != damageTextAt_.end();) {
        if (nowSeconds_ - it->second > kBucketIdleSeconds) {
            damagePending_.erase(it->first);
            it = damageTextAt_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = dummyDamage_.begin(); it != dummyDamage_.end();) {
        std::deque<std::pair<double, double>>& log = it->second;
        while (!log.empty() && nowSeconds_ - log.front().first > kDpsWindowSeconds) {
            log.pop_front();
        }
        if (log.empty()) {
            it = dummyDamage_.erase(it);
        } else {
            ++it;
        }
    }

    // Shadows exist only to give a Killed event something to animate, and a
    // mob that walked out of view will never produce one. Clearing wholesale
    // past the cap costs at worst one frame of death animations.
    if (mobShadows_.size() > kMaxMobShadows) mobShadows_.clear();
    if (mobEyes_.size() > kMaxMobShadows) mobEyes_.clear();
}

void WorldRenderer::drawGround(Canvas& canvas, const Camera& camera) const {
    drawGroundTiles(canvas, camera, Rect{0, 0, kWorldSize, kWorldSize}, -1);
}

/// Which ground artwork covers a point.
///
/// The map's `background` layer, when it has one. A map that predates the layer
/// -- the TypeScript bundle has no way to carry it -- falls back to the 3x3
/// section grid the layer replaced, so an old data directory still draws the
/// world it always did rather than a black one.
int WorldRenderer::groundIndexAt(Vec2 at) const {
    if (map_ != nullptr && map_->hasBackground()) return map_->groundAt(at);
    return sectionAt(at);
}

void WorldRenderer::drawGroundTiles(Canvas& canvas, const Camera& camera, Rect world,
                                    int fixedGround) const {
    const Rect visible = camera.visibleWorld(0);
    const double zoom = camera.zoom();

    // Anchored to the world origin, never to the camera: a tiling that moved
    // with the viewer would slide its pattern over the ground as you walk.
    const double startX = std::floor(visible.left() / kGroundTileSize) * kGroundTileSize;
    const double startY = std::floor(visible.top() / kGroundTileSize) * kGroundTileSize;
    const int tilesX = static_cast<int>(std::ceil(visible.w / kGroundTileSize)) + 1;
    const int tilesY = static_cast<int>(std::ceil(visible.h / kGroundTileSize)) + 1;

    for (int j = 0; j <= tilesY; ++j) {
        for (int i = 0; i <= tilesX; ++i) {
            const double tileX = startX + i * kGroundTileSize;
            const double tileY = startY + j * kGroundTileSize;
            // Sampled at the ground tile's CENTRE, which is what keeps a
            // 400-unit artwork tile whole over a 300-unit background grid: the
            // layer chooses the art, it does not chop it up.
            const int ground = fixedGround >= 0
                                   ? fixedGround
                                   : groundIndexAt({tileX + kGroundTileSize * 0.5,
                                                    tileY + kGroundTileSize * 0.5});
            // Outside the map, and anywhere the map paints no ground, there is
            // none -- the void stays the black the frame was cleared to.
            if (ground < 0) continue;

            // Origin floored and the tile drawn oversized, both in world units,
            // exactly as the browser build does inside its camera transform.
            const Rect tile{std::floor(tileX - kGroundOverlap * 0.5),
                            std::floor(tileY - kGroundOverlap * 0.5),
                            kGroundTileSize + kGroundOverlap, kGroundTileSize + kGroundOverlap};
            const Rect visiblePart = intersection(tile, world);
            if (visiblePart.w <= 0 || visiblePart.h <= 0) continue;

            const SvgDocument* art = sprites_ ? sprites_->groundArt(ground) : nullptr;
            canvas.save();
            // The clip is only ever doing something at the edge of the map: a
            // tile the world rect does not cut is drawn wholly inside its own
            // box either way, and building a full coverage mask for it was
            // most of what this loop spent on clipping.
            const bool cropped = visiblePart.w < tile.w || visiblePart.h < tile.h;
            if (cropped) clipWorldRect(canvas, camera, visiblePart);
            if (art) {
                const Vec2 at = camera.worldToScreen({tile.x, tile.y});
                // The zoom is already in the box, so the cache sees the size
                // this is really drawn at and bakes one bitmap per zoom step.
                if (!drawCachedArt(canvas, *art, at.x, at.y, tile.w * zoom, tile.h * zoom)) {
                    art->renderFitted(canvas, static_cast<float>(at.x), static_cast<float>(at.y),
                                      static_cast<float>(tile.w * zoom),
                                      static_cast<float>(tile.h * zoom), 0.0f);
                }
            } else {
                const Vec2 at = camera.worldToScreen({visiblePart.x, visiblePart.y});
                ui::setFill(canvas, kGroundFallback[ground]);
                canvas.fillRect(static_cast<float>(at.x), static_cast<float>(at.y),
                                static_cast<float>(visiblePart.w * zoom),
                                static_cast<float>(visiblePart.h * zoom));
            }
            canvas.restore();
        }
    }
}

namespace {

/// Canvas start angle for the quarter-circle fillet, keyed by the corner
/// code's (top, left) bits -- the mapping rrolf's RenderArena.c uses and the
/// browser build's maze-render.ts copies.
double filletStartAngle(int top, int left) {
    if (top == 0 && left == 1) return kPi * 0.5;
    if (top == 1 && left == 1) return kPi;
    if (top == 1 && left == 0) return kPi * 1.5;
    return 0.0;
}

/// Traces the wall shape of one rounded-corner cell (4-7 a convex floor
/// corner, 12-15 a concave wall corner) into the current path, in screen
/// space. The arc is one whole cell in radius and centred on the shared
/// corner vertex, which is exactly the circle Maze::cellBlocksPoint tests, so
/// what is drawn is what is collided with.
void traceMazeCorner(Canvas& canvas, const Camera& camera, double x0, double y0, double g,
                     int value) {
    const int left = (value >> 1) & 1;
    const int top = value & 1;
    const int concave = (value >> 3) & 1;
    const double cx = x0 + left * g;   // arc centre = the shared corner vertex
    const double cy = y0 + top * g;
    // A convex floor corner fills only the curved triangle at the vertex
    // opposite the arc centre; a concave wall corner fills the whole cell
    // minus that triangle. Both start their path accordingly.
    const double sx = concave ? cx : x0 + (1 - left) * g;
    const double sy = concave ? cy : y0 + (1 - top) * g;
    const double a0 = filletStartAngle(top, left);
    const Vec2 start = camera.worldToScreen({sx, sy});
    const Vec2 centre = camera.worldToScreen({cx, cy});
    canvas.moveTo(static_cast<float>(start.x), static_cast<float>(start.y));
    canvas.arc(static_cast<float>(centre.x), static_cast<float>(centre.y),
               static_cast<float>(g * camera.zoom()), static_cast<float>(a0),
               static_cast<float>(a0 + kPi * 0.5), false);
}

/// The reference's arena palette (src/graphics/pvp-arena.ts): gardn's "???"
/// zone, a plain grey floor with a faint dark grid, in a darker void.
constexpr std::uint32_t kArenaFloor = 0x777777u;
constexpr std::uint32_t kArenaVoid = 0x1A1A1Au;
constexpr double kArenaGridCell = 50.0;

} // namespace

void WorldRenderer::drawMaze(Canvas& canvas, const Camera& camera) const {
    // Beyond the maze's square there is nothing: the black the frame clears to.
    ui::setFill(canvas, 0x000000u);
    canvas.fillRect(0, 0, static_cast<float>(camera.viewportWidth()),
                    static_cast<float>(camera.viewportHeight()));

    const Maze& maze = activeMaze();
    const double size = maze.worldSize();
    const Rect square{kMazeOriginX, kMazeOriginY, size, size};
    const Rect visible = camera.visibleWorld(0);
    if (!visible.intersects(square)) return;

    // 1. Ground: the biome's own tiles, the way that biome's overworld is
    //    painted, clipped to the maze's square. One ground type for the whole
    //    maze -- it has no background layer of its own, and every cell of it is
    //    the same biome anyway.
    const int ground = kMazeBiomeSections[static_cast<std::size_t>(maze.biome())];
    drawGroundTiles(canvas, camera, square, ground);

    // 2. Walls: a single translucent black path, filled once. Filling cell by
    //    cell at partial alpha leaves antialiased hairline seams along every
    //    interior boundary of a contiguous wall mass; one nonzero fill
    //    rasterises the union with full coverage across shared edges.
    const double g = kMazeCellSize;
    const double zoom = camera.zoom();
    const int dim = maze.gridDim();
    const int minGx = std::max(0, static_cast<int>(std::floor((visible.left() - kMazeOriginX) / g)));
    const int maxGx = std::min(dim - 1, static_cast<int>(std::floor((visible.right() - kMazeOriginX) / g)));
    const int minGy = std::max(0, static_cast<int>(std::floor((visible.top() - kMazeOriginY) / g)));
    const int maxGy = std::min(dim - 1, static_cast<int>(std::floor((visible.bottom() - kMazeOriginY) / g)));

    canvas.save();
    clipWorldRect(canvas, camera, square);
    ui::setFill(canvas, 0x000000u, 0.2);
    canvas.beginPath();
    for (int gy = minGy; gy <= maxGy; ++gy) {
        for (int gx = minGx; gx <= maxGx; ++gx) {
            const int v = maze.cellValue(gx, gy);
            if (v == 1) continue;   // plain floor: nothing to draw
            const double x0 = kMazeOriginX + gx * g;
            const double y0 = kMazeOriginY + gy * g;
            if (v == 0) {
                const Vec2 at = camera.worldToScreen({x0, y0});
                canvas.rect(static_cast<float>(at.x), static_cast<float>(at.y),
                            static_cast<float>(g * zoom), static_cast<float>(g * zoom));
            } else {
                traceMazeCorner(canvas, camera, x0, y0, g, v);
            }
        }
    }
    canvas.fill();
    canvas.restore();
}

void WorldRenderer::drawArena(Canvas& canvas, const Camera& camera) const {
    const double zoom = camera.zoom();
    const Vec2 centre = camera.worldToScreen(kArenaCentre);
    const float radius = static_cast<float>(kArenaRadius * zoom);

    // Dark void everywhere; the floor below paints over it inside the ring.
    ui::setFill(canvas, kArenaVoid);
    canvas.fillRect(0, 0, static_cast<float>(camera.viewportWidth()),
                    static_cast<float>(camera.viewportHeight()));

    canvas.save();
    canvas.beginPath();
    canvas.arc(static_cast<float>(centre.x), static_cast<float>(centre.y), radius, 0.0f,
               static_cast<float>(kTau), false);
    canvas.clip();

    ui::setFill(canvas, kArenaFloor);
    canvas.fillRect(0, 0, static_cast<float>(camera.viewportWidth()),
                    static_cast<float>(camera.viewportHeight()));

    // The grid is aligned to the arena's centre so its lines hold still as
    // the camera moves. One screen pixel wide at every zoom, as the browser
    // draws it (lineWidth 1 / zoomLevel inside the world transform).
    const Rect visible = camera.visibleWorld(0);
    const Rect arena{kArenaCentre.x - kArenaRadius, kArenaCentre.y - kArenaRadius,
                     kArenaRadius * 2.0, kArenaRadius * 2.0};
    const Rect span = intersection(visible, arena);
    if (span.w > 0 && span.h > 0) {
        ui::setStroke(canvas, 0x000000u, 0.18);
        canvas.setLineWidth(1.0f);
        canvas.beginPath();
        const double firstX =
            kArenaCentre.x + std::ceil((span.left() - kArenaCentre.x) / kArenaGridCell) * kArenaGridCell;
        const double firstY =
            kArenaCentre.y + std::ceil((span.top() - kArenaCentre.y) / kArenaGridCell) * kArenaGridCell;
        for (double x = firstX; x <= span.right(); x += kArenaGridCell) {
            const Vec2 a = camera.worldToScreen({x, span.top()});
            const Vec2 b = camera.worldToScreen({x, span.bottom()});
            canvas.moveTo(static_cast<float>(a.x), static_cast<float>(a.y));
            canvas.lineTo(static_cast<float>(b.x), static_cast<float>(b.y));
        }
        for (double y = firstY; y <= span.bottom(); y += kArenaGridCell) {
            const Vec2 a = camera.worldToScreen({span.left(), y});
            const Vec2 b = camera.worldToScreen({span.right(), y});
            canvas.moveTo(static_cast<float>(a.x), static_cast<float>(a.y));
            canvas.lineTo(static_cast<float>(b.x), static_cast<float>(b.y));
        }
        canvas.stroke();
    }
    canvas.restore();

    // The boundary: a hard red ring -- bodies are clamped to it -- with a
    // paler line just inside.
    canvas.save();
    ui::setStroke(canvas, 0xFF3C3Cu, 0.65);
    canvas.setLineWidth(static_cast<float>(12.0 * zoom));
    canvas.strokeCircle(static_cast<float>(centre.x), static_cast<float>(centre.y), radius);
    ui::setStroke(canvas, 0xFFC8C8u, 0.4);
    canvas.setLineWidth(static_cast<float>(4.0 * zoom));
    canvas.strokeCircle(static_cast<float>(centre.x), static_cast<float>(centre.y),
                        static_cast<float>((kArenaRadius - 8.0) * zoom));
    canvas.restore();
}

void WorldRenderer::drawSmoothedTileEdge(Canvas& canvas, const Camera& camera, int tileX,
                                         int tileY, int edgeIndex) const {
    const WallEdge edge = static_cast<WallEdge>(edgeIndex);
    const JaggedEdge& points = jaggedPoints(tileX, tileY, edge);
    const std::size_t count = points.size();
    if (count < 3) return;

    const double worldX = tileX * kTileSize;
    const double worldY = tileY * kTileSize;
    const auto at = [&](std::size_t i) { return edgePoint(worldX, worldY, edge, points[i]); };
    const auto midpoint = [](Vec2 a, Vec2 b) { return Vec2{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5}; };
    // Each point is its own control point and the curve lands halfway to the
    // next one, which is what turns the shared jagged polyline into a shore.
    const auto traceCurve = [&] {
        for (std::size_t i = 1; i + 1 < count; ++i) {
            quadToScreen(canvas, camera, at(i), midpoint(at(i), at(i + 1)));
        }
        lineToScreen(canvas, camera, at(count - 1));
    };

    canvas.save();
    ui::setFill(canvas, kWaterFill);
    canvas.beginPath();
    moveToScreen(canvas, camera, edgeBasePoint(worldX, worldY, edge, points.front().t));
    lineToScreen(canvas, camera, at(0));
    traceCurve();
    lineToScreen(canvas, camera, edgeBasePoint(worldX, worldY, edge, points.back().t));
    canvas.closePath();
    canvas.fill();

    // The outline follows the curve only: closing it along the tile edge would
    // draw a hard line through the middle of the water.
    ui::setStroke(canvas, kWaterBorder);
    canvas.setLineWidth(static_cast<float>(2.0 * camera.zoom()));
    canvas.setLineJoin("round");
    canvas.beginPath();
    moveToScreen(canvas, camera, at(0));
    traceCurve();
    canvas.stroke();
    canvas.restore();
}

void WorldRenderer::drawTerrain(Canvas& canvas, const Camera& camera) const {
    // Everything outside the world is pure black: the browser build clears its
    // frame to it and simply skips any tile that falls off the map.
    ui::setFill(canvas, 0x000000u);
    canvas.fillRect(0, 0, static_cast<float>(camera.viewportWidth()),
                    static_cast<float>(camera.viewportHeight()));

    drawGround(canvas, camera);
    if (!terrain_) return;

    const Rect visible = camera.visibleWorld(kTileSize);
    const int x0 = std::max(0, static_cast<int>(std::floor(visible.left() / kTileSize)));
    const int y0 = std::max(0, static_cast<int>(std::floor(visible.top() / kTileSize)));
    const int x1 = std::min(kTilesPerAxis - 1,
                            static_cast<int>(std::floor(visible.right() / kTileSize)));
    const int y1 = std::min(kTilesPerAxis - 1,
                            static_cast<int>(std::floor(visible.bottom() / kTileSize)));

    const double zoom = camera.zoom();

    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            const Tile tile = terrain_->atTile(tx, ty);
            if (tile == Tile::Ground) continue;   // air: the ground shows through
            if (tile == Tile::Wall) {
                drawWallTile(canvas, camera, tx, ty);
                continue;
            }
            // The same 1.5-unit bleed on all four sides the wall tiles use.
            const Rect bounds{tx * kTileSize - kWallOverlap, ty * kTileSize - kWallOverlap,
                              kTileSize + kWallOverlap * 2.0, kTileSize + kWallOverlap * 2.0};
            const Vec2 at = camera.worldToScreen({bounds.x, bounds.y});
            const SvgDocument* art = sprites_ ? sprites_->tileArt(tile) : nullptr;
            if (art) {
                canvas.save();
                clipWorldRect(canvas, camera, bounds);
                if (!drawCachedArt(canvas, *art, at.x, at.y, bounds.w * zoom, bounds.h * zoom)) {
                    art->renderFitted(canvas, static_cast<float>(at.x), static_cast<float>(at.y),
                                      static_cast<float>(bounds.w * zoom),
                                      static_cast<float>(bounds.h * zoom), 0.0f);
                }
                canvas.restore();
            } else {
                ui::setFill(canvas, kTileColor(tile));
                canvas.fillRect(static_cast<float>(at.x), static_cast<float>(at.y),
                                static_cast<float>(bounds.w * zoom),
                                static_cast<float>(bounds.h * zoom));
            }
        }
    }

    // The web client draws every exposed side after all tile fills. Repeating
    // the order matters: protrusions are never hidden by a neighbouring fill.
    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            const Tile tile = terrain_->atTile(tx, ty);
            if (tile != Tile::Wall && tile != Tile::Water) continue;
            for (const WallEdge edge : {WallEdge::Top, WallEdge::Bottom,
                                        WallEdge::Left, WallEdge::Right}) {
                if (!tileEdgeExposed(*terrain_, tx, ty, edge)) continue;
                if (tile == Tile::Wall) {
                    drawJaggedWallEdge(canvas, camera, tx, ty, edge);
                } else {
                    drawSmoothedTileEdge(canvas, camera, tx, ty, static_cast<int>(edge));
                }
            }
        }
    }
}

void WorldRenderer::drawGlitched(Canvas& canvas, Vec2 screen, double radius, std::uint32_t seed,
                                 double timeSeconds,
                                 const std::function<void(Canvas&)>& body) const {
    // Glitch is a post-process, not a replacement skin: the SAME body is drawn
    // into a transparent buffer and recomposed as clipped, horizontally
    // displaced bands. That is what keeps it composable with the Pumpkin and
    // Robot skins, and what lets the glitch flower tear its petal ring along
    // with its face.
    const std::uint32_t bucket =
        static_cast<std::uint32_t>(std::max(0.0, std::floor(timeSeconds * 1000.0 / 70.0)));
    if (hash01(seed, bucket) >= 0.45) {
        canvas.save();
        canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
        body(canvas);
        canvas.restore();
        return;
    }

    const int half = std::max(16, static_cast<int>(std::ceil(radius * 2.0 + 24.0)));
    const int side = half * 2;
    if (!glitchBody_ || glitchSide_ < side) {
        glitchBody_ = std::make_unique<Canvas>(Canvas::createVirtual(side, side));
        glitchTint_ = std::make_unique<Canvas>(Canvas::createVirtual(side, side));
        glitchSide_ = side;
    }

    Canvas& buffer = *glitchBody_;
    const int bufferHalf = glitchSide_ / 2;
    buffer.clearRect(0, 0, static_cast<float>(glitchSide_), static_cast<float>(glitchSide_));
    buffer.save();
    buffer.translate(static_cast<float>(bufferHalf), static_cast<float>(bufferHalf));
    body(buffer);
    buffer.restore();

    constexpr int kBandCount = 9;
    for (int i = 0; i < kBandCount; ++i) {
        const double roll = hash01(seed ^ 0x5F3759DFu, bucket * 31u + static_cast<std::uint32_t>(i));
        if (roll < 0.06) continue;  // the occasional missing scanline band
        double dx = 0;
        if (roll < 0.36) {
            dx = (hash01(seed, bucket * 17u + static_cast<std::uint32_t>(i)) - 0.5) * radius * 0.9;
        }
        const double y0 = screen.y - bufferHalf + static_cast<double>(glitchSide_ * i) / kBandCount;
        const double y1 = screen.y - bufferHalf + static_cast<double>(glitchSide_ * (i + 1)) / kBandCount;
        canvas.save();
        canvas.beginPath();
        canvas.rect(static_cast<float>(screen.x - bufferHalf), static_cast<float>(y0),
                    static_cast<float>(glitchSide_), static_cast<float>(std::ceil(y1 - y0)));
        canvas.clip();
        canvas.drawCanvas(buffer, static_cast<float>(screen.x - bufferHalf + dx),
                          static_cast<float>(screen.y - bufferHalf));
        canvas.restore();
    }

    // The chromatic fringe: the same silhouette flattened to red and to cyan
    // and pulled apart. Only on the stronger bursts, so the effect breathes
    // instead of sitting at a constant intensity.
    const double fringe = hash01(seed ^ 0x27D4EB2Fu, bucket);
    if (fringe >= 0.65) return;
    // Capped in absolute pixels: scaled purely off the radius, a size-6 flower
    // pulls the copies a body-width apart and reads as three flowers.
    const double shift = std::min(6.0, (0.04 + fringe * 0.12) * radius);

    // The reference builds each copy by multiplying the body with a pure
    // primary and re-masking it to the body's own alpha. In a browser those
    // are native Canvas2D composite operations: keep the body and tint buffers
    // in the browser, never read their pixels into Wasm. The native client has
    // no compositing backend, so it keeps the equivalent CPU pixel pass.
#ifdef __EMSCRIPTEN__
    const auto buildTint = [&](Color colour) {
        glitchTint_->clearRect(0, 0, static_cast<float>(glitchSide_),
                               static_cast<float>(glitchSide_));
        glitchTint_->drawCanvas(buffer, 0, 0, static_cast<float>(glitchSide_),
                                static_cast<float>(glitchSide_));
        glitchTint_->setGlobalCompositeOperation("multiply");
        glitchTint_->setFillStyle(colour);
        glitchTint_->fillRect(0, 0, static_cast<float>(glitchSide_),
                              static_cast<float>(glitchSide_));
        glitchTint_->setGlobalCompositeOperation("destination-in");
        glitchTint_->drawCanvas(buffer, 0, 0, static_cast<float>(glitchSide_),
                                static_cast<float>(glitchSide_));
        glitchTint_->setGlobalCompositeOperation("source-over");
    };
#else
    const std::vector<std::uint8_t> silhouette =
        buffer.getImageData(0, 0, glitchSide_, glitchSide_);
    const auto buildTint = [&](bool keepRed) {
        glitchPixels_ = silhouette;
        for (std::size_t i = 0; i + 3 < glitchPixels_.size(); i += 4) {
            if (keepRed) {
                glitchPixels_[i + 1] = 0;
                glitchPixels_[i + 2] = 0;
            } else {
                glitchPixels_[i] = 0;
            }
        }
        glitchTint_->clearRect(0, 0, static_cast<float>(glitchSide_),
                               static_cast<float>(glitchSide_));
        glitchTint_->putImageData(glitchPixels_, glitchSide_, glitchSide_, 0, 0);
    };
#endif
    canvas.save();
#ifdef __EMSCRIPTEN__
    canvas.setGlobalCompositeOperation("lighter");
#endif
    canvas.setGlobalAlpha(0.36f);
#ifdef __EMSCRIPTEN__
    buildTint(Color{255, 0, 0});
#else
    buildTint(true);
#endif
    canvas.drawCanvas(*glitchTint_, static_cast<float>(screen.x - bufferHalf - shift),
                      static_cast<float>(screen.y - bufferHalf));
#ifdef __EMSCRIPTEN__
    buildTint(Color{0, 255, 255});
#else
    buildTint(false);
#endif
    canvas.drawCanvas(*glitchTint_, static_cast<float>(screen.x - bufferHalf + shift),
                      static_cast<float>(screen.y - bufferHalf));
    canvas.restore();
}

void WorldRenderer::drawFlower(Canvas& canvas, const RemoteEntity& entity, const Camera& camera,
                               Vec2 at, double timeSeconds) const {
    const double zoom = camera.zoom();
    const Vec2 screen = camera.worldToScreen(at);
    // The drawn flower is 25 world units times the petal-driven size
    // multiplier, and nothing else. entity.radius is the gameplay hitbox,
    // which grows with level; using it here made a high level flower visibly
    // larger than the browser build's, which never grows one.
    const double radius = kFlowerArtRadius * playerSizeMultiplier(entity) * zoom;
    if (radius <= 0.5) return;
    const double scale = radius / kFlowerArtRadius;

    const auto paintBody = [&](Canvas& target) {
        target.scale(static_cast<float>(scale), static_cast<float>(scale));
        drawFlowerBody(target, entity, timeSeconds);
    };

    if (entity.renderFlags & PlayerRenderGlitch) {
        drawGlitched(canvas, screen, radius, entity.netId, timeSeconds, paintBody);
        return;
    }
    canvas.save();
    canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
    paintBody(canvas);
    canvas.restore();
}

double WorldRenderer::playerSizeMultiplier(const RemoteEntity& entity) const {
    // The server multiplies the level's own hitbox radius by the loadout's
    // size modifiers before replicating it, so the multiplier is what is left
    // once the level is divided back out. Sending it separately would put the
    // same number on the wire twice and let the two disagree.
    const double base = playerRadiusForLevel(entity.level);
    if (base <= 0.0) return 1.0;
    // Bounded the way the browser build bounds it, so a corrupt radius cannot
    // scale a flower off the screen.
    return clamp(entity.radius / base, 0.0, kMaxSizeMultiplier);
}

std::uint32_t WorldRenderer::healthBarColor(const RemoteEntity& entity, double timeSeconds) const {
    // A flower that left view while shielded never gets its fade frame, so
    // bound the table rather than let one leak per disconnect.
    if (invulnFade_.size() > 256) invulnFade_.clear();

    // -1 means "still shielded"; any other value is the moment it ended, which
    // is what the fade back to green is measured from.
    if (entity.state & net::StateInvulnerable) {
        invulnFade_[entity.netId] = -1.0;
        return kInvulnHealth;
    }
    const auto it = invulnFade_.find(entity.netId);
    if (it == invulnFade_.end()) return ui::kHealth;
    if (it->second < 0.0) it->second = timeSeconds;
    const double t = clamp((timeSeconds - it->second) / kInvulnFadeSeconds, 0.0, 1.0);
    if (t >= 1.0) {
        invulnFade_.erase(it);
        return ui::kHealth;
    }
    return lerpColor(kInvulnHealth, ui::kHealth, t);
}

void WorldRenderer::drawPlayerPlate(Canvas& canvas, const RemoteEntity& entity,
                                    const Camera& camera, Vec2 at, double timeSeconds) const {
    const double zoom = camera.zoom();
    const Vec2 screen = camera.worldToScreen(at);
    const double size = playerSizeMultiplier(entity);
    // Every number below is a world unit, laid out exactly as the browser
    // build lays it out inside the camera transform. The whole plate sits
    // BELOW the flower centre.
    const double barY = screen.y + (kPlayerBaseRadius * size + 24.0) * zoom;
    const double left = screen.x - 30.0 * zoom;
    const double right = screen.x + 30.0 * zoom;

    if (options.names) {
        ui::TextStyle style;
        style.size = 12.0 * zoom;
        style.align = ui::Align::Left;
        // The browser build never sets a baseline in the world pass, so the
        // pen sits on the alphabetic baseline.
        style.baseline = ui::Baseline::Alphabetic;
        style.strokeWidth = 3.0 * zoom;
        ui::text(canvas, entity.name.empty() ? "Unnamed" : entity.name, left,
                 barY - 4.0 * zoom, style);
    }

    if (options.healthBars) {
        // Always drawn, even at full health: the bar is part of how a flower
        // reads, not a warning that appears once you are hurt.
        ui::setFill(canvas, ui::kHealthBack);
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(screen.x - 31.0 * zoom),
                         static_cast<float>(barY - zoom), static_cast<float>(62.0 * zoom),
                         static_cast<float>(10.0 * zoom), static_cast<float>(4.0 * zoom));
        canvas.fill();

        const double fill = clamp(entity.healthFraction, 0.0, 1.0) * 60.0 * zoom;
        if (fill > 0) {
            ui::setFill(canvas, healthBarColor(entity, timeSeconds));
            canvas.beginPath();
            canvas.roundRect(static_cast<float>(left), static_cast<float>(barY),
                             static_cast<float>(fill), static_cast<float>(8.0 * zoom),
                             static_cast<float>(4.0 * zoom));
            canvas.fill();
        }
    }

    if (options.names) {
        ui::TextStyle level;
        level.size = 10.0 * zoom;
        level.align = ui::Align::Right;
        level.baseline = ui::Baseline::Alphabetic;
        level.strokeWidth = 3.0 * zoom;
        // Tinted with the best rarity anywhere in that flower's loadout, which
        // is how a passing flower advertises what it is carrying.
        level.fill = rarityColor(entity.bestRarity);
        ui::text(canvas, "Lv. " + std::to_string(entity.level), right, barY + 20.0 * zoom,
                 level);

        // The guild tag mirrors the level label on the other side of the bar,
        // one point smaller so a five-character id and its brackets cannot
        // collide with it.
        if (!entity.guildName.empty()) {
            ui::TextStyle tag;
            tag.size = 8.0 * zoom;
            tag.align = ui::Align::Left;
            tag.baseline = ui::Baseline::Alphabetic;
            tag.strokeWidth = 2.0 * zoom;
            tag.fill = 0x27DADEu;
            ui::text(canvas, "[" + entity.guildName + "]", left, barY + 20.0 * zoom, tag);
        }
    }
}

void WorldRenderer::drawCorpse(Canvas& canvas, const RemoteEntity& entity, const Camera& camera,
                               Vec2 at, double timeSeconds) const {
    const double zoom = camera.zoom();
    if (kFlowerArtRadius * zoom <= 0.5) return;
    const Vec2 screen = camera.worldToScreen(at);

    canvas.save();
    canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
    // A corpse lies where it fell: rotated to its facing, at a fixed radius 25
    // whatever the flower's size multiplier was, and with no skin, no status
    // tint, no plate and no petals.
    canvas.rotate(static_cast<float>(entity.angle));
    canvas.scale(static_cast<float>(zoom), static_cast<float>(zoom));
    drawFace(canvas, FaceDeadEyes, entity.equipFlags, 0, 0, 15.0, timeSeconds);
    canvas.restore();
}

void WorldRenderer::drawPetalGlow(Canvas& canvas, double radius, std::uint32_t rgb, int bands,
                                  double centreAlpha, double kneeAlpha) const {
    if (radius <= 1.0) return;
    // The browser build bakes a three-stop radial gradient (for a petal: 0.6 at
    // the centre, 0.25 at 40%, 0 at the rim). cpp_canvas has no gradient, so
    // paint the same ramp as nested discs: each one's alpha is solved so that
    // the accumulated coverage over the discs still to come lands on the ramp's
    // value there.
    const int kBands = std::max(2, bands);
    const auto ramp = [radius, centreAlpha, kneeAlpha](double r) {
        const double knee = radius * 0.4;
        if (r <= knee) return centreAlpha + (kneeAlpha - centreAlpha) * (r / knee);
        return kneeAlpha * (1.0 - (r - knee) / (radius - knee));
    };
    double outerTarget = 0.0;
    for (int k = kBands; k >= 1; --k) {
        const double outer = radius * k / kBands;
        const double target = ramp(radius * (k - 0.5) / kBands);
        const double alpha = 1.0 - (1.0 - target) / (1.0 - outerTarget);
        outerTarget = target;
        if (alpha <= 0.002) continue;
        ui::setFill(canvas, rgb, alpha);
        canvas.fillCircle(0, 0, static_cast<float>(outer));
    }
}

void WorldRenderer::drawPetalSprite(Canvas& canvas, const RemoteEntity& entity,
                                    const Camera& camera, Vec2 at, double timeSeconds) const {
    const PetalConfig* config = content_ ? &content_->petal(entity.typeIndex) : nullptr;
    // Three entries in petals.json are pure modifiers with no artwork at all.
    if (config && config->hidden) return;

    const double zoom = camera.zoom();
    const Vec2 screen = camera.worldToScreen(at);
    // The drawn petal is 12 units per size unit, times the petal's own
    // `visual_scale`. entity.radius is the 20-unit gameplay reach, and
    // noPhysics petals carry no body at all, so neither is usable as a drawing
    // size -- and visual_scale must never reach either of them.
    const double artSize = (config ? config->size : 1.0) * petalArtScale(config);
    const double diameter = kPetalArtSize * artSize * zoom;
    if (diameter <= 0.5) return;

    // The sprite's own spin is the ring's shared phase and nothing else, so
    // every instance of one petal type on a flower points the same way rather
    // than fanning outward like spokes. entity.angle is the orbit POSITION and
    // must not be reused here.
    double rotation = 0;
    if (config && config->hasFixedDirection) {
        rotation = config->fixedDirection;
    } else {
        const double speed = (config && config->speed > 0) ? config->speed : 1.0;
        rotation = std::fmod(timeSeconds * kPetalSpinRate * speed, kTau) + kPi * 0.5;
    }

    canvas.save();
    canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
    if (config && config->emissive) {
        // Unrotated, exactly as the browser build draws it: the glow is round,
        // and spinning it would only cost time.
        const double lightRadius =
            (config->lightRadius > 0 ? config->lightRadius : kPetalArtSize * artSize * 3.0) * zoom;
        const std::uint32_t glow = !config->lightColor.empty()
                                       ? static_cast<std::uint32_t>(config->lightColorRgba >> 8)
                                       : static_cast<std::uint32_t>(config->colorRgba >> 8);
        drawPetalGlow(canvas, lightRadius, glow);
    }
    if (options.rarityGlow) {
        // ALT held: every petal wears its tier. The browser build bakes it as
        // a 16-unit shadow blur redrawn six times; with no blur here the same
        // reach is the nested-disc ramp, which reads as the same halo.
        drawPetalGlow(canvas, diameter * 0.5 + kPetalGlowPad * zoom, rarityColor(entity.rarity),
                      12, 0.75, 0.45);
    }
    canvas.rotate(static_cast<float>(rotation));
    // Both axes: a petal that declares an X offset is drawn off it in the
    // browser build too, and dropping one of the two silently moves it.
    if (config && (config->visualOffsetX != 0 || config->visualOffsetY != 0)) {
        canvas.translate(static_cast<float>(config->visualOffsetX * zoom),
                         static_cast<float>(config->visualOffsetY * zoom));
    }
    if (sprites_) {
        // The sprite cache falls back to a disc in the petal's own colour when
        // its artwork failed to compile, which is the useful fallback here.
        sprites_->drawPetal(canvas, entity.typeIndex, 0, 0, diameter, 0, timeSeconds);
    } else {
        ui::disc(canvas, {0, 0}, diameter * 0.5,
                 config ? static_cast<std::uint32_t>(config->colorRgba >> 8) : 0xFFFFFFu,
                 ui::kInk, zoom);
    }
    canvas.restore();
}

void WorldRenderer::drawFlowerBody(Canvas& canvas, const RemoteEntity& entity,
                                   double timeSeconds) const {
    // Registration order in player-skins.ts is Pumpkin, then Robot. Preserve
    // that deterministic priority when a saved account somehow has both bits.
    if (entity.renderFlags & PlayerRenderPumpkin) {
        drawPumpkin(canvas, entity);
    } else if (entity.renderFlags & PlayerRenderRobot) {
        drawRobot(canvas, entity);
    } else {
        drawDefaultFlower(canvas, entity, timeSeconds);
    }
}

void WorldRenderer::drawDefaultFlower(Canvas& canvas, const RemoteEntity& entity,
                                      double timeSeconds) const {
    // The server sends 14.5 idle and 4 while attacking or defending; the face
    // itself only ever sees the resulting curve.
    const bool active = (entity.faceFlags & (FaceAttacking | FaceDefending)) != 0;
    drawFace(canvas, entity.faceFlags, entity.equipFlags, entity.eyeX, entity.eyeY,
             active ? 4.0 : 14.5, timeSeconds);
}

void WorldRenderer::drawFace(Canvas& canvas, std::uint8_t faceFlags, std::uint8_t equipFlags,
                             double eyeX, double eyeY, double mouth, double timeSeconds,
                             std::uint32_t bodyColor) const {
    std::uint32_t baseColor = bodyColor;
    // The status precedence is intentional: corruption identifies a flower
    // that can hurt other players, so it must remain visible through poison.
    if (faceFlags & FaceHasCorruption) baseColor = 0xD91313u;
    else if (faceFlags & FacePoisoned) baseColor = 0xCE76DBu;
    else if (faceFlags & FaceDandelioned) baseColor = mixWithWhite(baseColor, 0.4);

    ui::setFill(canvas, scaleColor(baseColor, 0.8));
    canvas.beginPath();
    canvas.arc(0, 0, 26.5f, 0, static_cast<float>(kTau));
    canvas.fill();
    ui::setFill(canvas, baseColor);
    canvas.beginPath();
    canvas.arc(0, 0, 23.5f, 0, static_cast<float>(kTau));
    canvas.fill();

    const bool deadEyes = (faceFlags & FaceDeadEyes) != 0;
    canvas.save();
    ui::setFill(canvas, 0x000000u);
    ui::setStroke(canvas, 0x000000u);
    if (deadEyes) {
        constexpr float len = 4.0f;
        canvas.setLineWidth(3.0f);
        canvas.setLineCap("round");
        canvas.beginPath();
        canvas.moveTo(-7 - len, -4.8f - len); canvas.lineTo(-7 + len, -4.8f + len);
        canvas.moveTo(-7 + len, -4.8f - len); canvas.lineTo(-7 - len, -4.8f + len);
        canvas.moveTo(7 - len, -4.8f - len); canvas.lineTo(7 + len, -4.8f + len);
        canvas.moveTo(7 + len, -4.8f - len); canvas.lineTo(7 - len, -4.8f + len);
        canvas.stroke();
    } else if (faceFlags & FaceSquareEyes) {
        canvas.beginPath();
        canvas.rect(-10, -11.3f, 6, 13);
        canvas.rect(4, -11.3f, 6, 13);
        canvas.fill();
        canvas.clip();
        ui::setFill(canvas, 0xFFFFFFu);
        canvas.beginPath();
        canvas.rect(static_cast<float>(-10 + eyeX), static_cast<float>(-7.8 + eyeY), 6, 6);
        canvas.rect(static_cast<float>(4 + eyeX), static_cast<float>(-7.8 + eyeY), 6, 6);
        canvas.fill();
        ui::setStroke(canvas, 0x000000u);
        canvas.setLineWidth(1.0f);
        canvas.beginPath(); canvas.rect(-10, -11.3f, 6, 13); canvas.stroke();
        canvas.beginPath(); canvas.rect(4, -11.3f, 6, 13); canvas.stroke();
    } else {
        canvas.beginPath();
        canvas.ellipse(-7, -4.8f, 3.2f, 6.5f, 0, 0, static_cast<float>(kTau));
        canvas.moveTo(10.2f, -4.8f);
        canvas.ellipse(7, -4.8f, 3.2f, 6.5f, 0, 0, static_cast<float>(kTau));
        canvas.fill();
        canvas.clip();
        ui::setFill(canvas, 0xFFFFFFu);
        canvas.beginPath();
        canvas.arc(static_cast<float>(-7 + eyeX), static_cast<float>(-4.8 + eyeY),
                   3, 0, static_cast<float>(kTau));
        canvas.arc(static_cast<float>(7 + eyeX), static_cast<float>(-4.8 + eyeY),
                   3, 0, static_cast<float>(kTau));
        canvas.fill();
        ui::setStroke(canvas, 0x000000u);
        canvas.setLineWidth(1.0f);
        canvas.beginPath();
        canvas.ellipse(-7, -4.8f, 3.2f, 6.5f, 0, 0, static_cast<float>(kTau));
        canvas.stroke();
        canvas.beginPath();
        canvas.ellipse(7, -4.8f, 3.2f, 6.5f, 0, 0, static_cast<float>(kTau));
        canvas.stroke();
    }
    canvas.restore();

    ui::setStroke(canvas, 0x222222u);
    canvas.setLineWidth(1.5f);
    canvas.setLineCap("round");
    canvas.beginPath();
    canvas.moveTo(-6, 10);
    canvas.quadraticCurveTo(0, static_cast<float>(mouth), 6, 10);
    canvas.stroke();

    if (!deadEyes && mouth <= 8.0 && (faceFlags & FaceAttacking)) {
        canvas.save();
        canvas.translate(0, static_cast<float>(-mouth - 8.0));
        ui::setFill(canvas, baseColor);
        canvas.beginPath();
        canvas.moveTo(-12, 0); canvas.lineTo(12, 0); canvas.lineTo(0, 6); canvas.closePath();
        canvas.fill();
        canvas.restore();
    }

    if (equipFlags & (EquipAntennae | EquipObserver)) {
        canvas.save();
        canvas.translate(0, -35);
        ui::setFill(canvas, 0x333333u);
        ui::setStroke(canvas, 0x222222u);
        canvas.setLineWidth(3.0f);
        canvas.setLineCap("round");
        canvas.setLineJoin("round");
        canvas.beginPath();
        canvas.moveTo(5, 12.5f); canvas.quadraticCurveTo(10, -2.5f, 15, -12.5f);
        canvas.quadraticCurveTo(5, -2.5f, 5, 12.5f); canvas.closePath();
        canvas.moveTo(-5, 12.5f); canvas.quadraticCurveTo(-10, -2.5f, -15, -12.5f);
        canvas.quadraticCurveTo(-5, -2.5f, -5, 12.5f); canvas.closePath();
        canvas.fill();
        canvas.stroke();
        if (equipFlags & EquipObserver) {
            ui::setFill(canvas, 0xD01C1Du);
            canvas.beginPath(); canvas.arc(15, -12.5f, 2.5f, 0, static_cast<float>(kTau)); canvas.fill();
            canvas.beginPath(); canvas.arc(-15, -12.5f, 2.5f, 0, static_cast<float>(kTau)); canvas.fill();
        }
        canvas.restore();
    }

    if (equipFlags & EquipThirdEye) {
        const std::uint16_t thirdEye = content_ ? content_->petalIndex("third_eye") : kInvalidIndex;
        if (sprites_ && thirdEye != kInvalidIndex && sprites_->petalDrawable(thirdEye)) {
            sprites_->drawPetal(canvas, thirdEye, 0, -14, 13, 0, timeSeconds);
        } else {
            canvas.save();
            canvas.translate(0, -14);
            canvas.scale(0.5f, 0.5f);
            if (deadEyes) {
                constexpr float len = 4.0f;
                ui::setStroke(canvas, 0x222222u);
                canvas.setLineWidth(3.0f);
                canvas.setLineCap("round");
                canvas.beginPath();
                canvas.moveTo(-len, -len); canvas.lineTo(len, len);
                canvas.moveTo(len, -len); canvas.lineTo(-len, len);
                canvas.stroke();
            } else {
                ui::setFill(canvas, 0x222222u);
                ui::setStroke(canvas, 0x222222u);
                canvas.setLineWidth(1.0f);
                canvas.beginPath();
                canvas.ellipse(0, 0, 3.2f, 6.5f, 0, 0, static_cast<float>(kTau));
                canvas.fill(); canvas.stroke(); canvas.clip();
                ui::setFill(canvas, 0xFFFFFFu);
                canvas.beginPath();
                canvas.arc(static_cast<float>(eyeX), static_cast<float>(eyeY),
                           3, 0, static_cast<float>(kTau));
                canvas.fill();
            }
            canvas.restore();
        }
    }
}

void WorldRenderer::drawPumpkin(Canvas& canvas, const RemoteEntity& entity) const {
    canvas.save();
    // Stem
    ui::setFill(canvas, 0x5A7D34u); ui::setStroke(canvas, 0x3F5A24u);
    canvas.setLineWidth(2.0f); canvas.setLineJoin("round");
    canvas.beginPath();
    canvas.moveTo(-3, -23); canvas.lineTo(3, -23); canvas.lineTo(2, -31); canvas.lineTo(-2, -31);
    canvas.closePath(); canvas.fill(); canvas.stroke();

    ui::setFill(canvas, 0xA8490Du);
    canvas.beginPath(); canvas.arc(0, 0, 26, 0, static_cast<float>(kTau)); canvas.fill();
    ui::setFill(canvas, 0xE8731Fu);
    canvas.beginPath(); canvas.ellipse(0, 0, 24, 23, 0, 0, static_cast<float>(kTau)); canvas.fill();
    ui::setStroke(canvas, 0xC25A12u); canvas.setLineWidth(1.5f);
    for (const float x : {-11.0f, 11.0f}) {
        canvas.beginPath(); canvas.ellipse(x, 0, 6.5f, 22, 0, 0, static_cast<float>(kTau)); canvas.stroke();
    }

    const double eyeX = clamp(entity.eyeX, -2.0, 2.0) * 0.7;
    const double eyeY = clamp(entity.eyeY, -2.0, 2.0) * 0.5;
    ui::setFill(canvas, 0x3A1A02u);
    canvas.beginPath();
    canvas.moveTo(static_cast<float>(-12 + eyeX), static_cast<float>(-1 + eyeY));
    canvas.lineTo(static_cast<float>(-3 + eyeX), static_cast<float>(-1 + eyeY));
    canvas.lineTo(static_cast<float>(-7.5 + eyeX), static_cast<float>(-9 + eyeY)); canvas.closePath();
    canvas.moveTo(static_cast<float>(12 + eyeX), static_cast<float>(-1 + eyeY));
    canvas.lineTo(static_cast<float>(3 + eyeX), static_cast<float>(-1 + eyeY));
    canvas.lineTo(static_cast<float>(7.5 + eyeX), static_cast<float>(-9 + eyeY)); canvas.closePath();
    canvas.fill();
    canvas.beginPath();
    canvas.moveTo(-13, 6); canvas.lineTo(-9, 12); canvas.lineTo(-4.5f, 6); canvas.lineTo(0, 13);
    canvas.lineTo(4.5f, 6); canvas.lineTo(9, 12); canvas.lineTo(13, 6); canvas.lineTo(9, 9);
    canvas.lineTo(-9, 9); canvas.closePath(); canvas.fill();
    canvas.restore();
}

void WorldRenderer::drawRobot(Canvas& canvas, const RemoteEntity& entity) const {
    canvas.save();
    ui::setStroke(canvas, 0x7B8794u); canvas.setLineWidth(2.0f); canvas.setLineCap("round");
    canvas.beginPath(); canvas.moveTo(0, -21); canvas.lineTo(0, -31); canvas.stroke();
    ui::setFill(canvas, 0xD01C1Du); canvas.beginPath(); canvas.arc(0, -33, 3, 0, static_cast<float>(kTau)); canvas.fill();

    ui::setFill(canvas, 0x4B5563u); canvas.beginPath(); canvas.roundRect(-25, -22, 50, 44, 9); canvas.fill();
    ui::setFill(canvas, 0x9AA6B2u); canvas.beginPath(); canvas.roundRect(-22, -19, 44, 38, 7); canvas.fill();
    ui::setFill(canvas, 0x6B7280u);
    for (const Vec2 bolt : {Vec2{-17, -14}, Vec2{17, -14}, Vec2{-17, 15}, Vec2{17, 15}}) {
        canvas.beginPath(); canvas.arc(static_cast<float>(bolt.x), static_cast<float>(bolt.y), 1.8f, 0, static_cast<float>(kTau)); canvas.fill();
    }
    ui::setFill(canvas, 0x10141Au); canvas.beginPath(); canvas.roundRect(-17, -8, 34, 13, 5); canvas.fill();

    const double eyeX = clamp(entity.eyeX, -2.5, 2.5);
    const double eyeY = clamp(entity.eyeY, -1.5, 1.5);
    // The default flower colour is the TypeScript robot visor tint.
    ui::setFill(canvas, 0xFFE763u);
    canvas.setShadow(Color{255, 231, 99}, 6.0f);
    for (const float x : {-8.0f, 8.0f}) {
        canvas.beginPath();
        canvas.arc(static_cast<float>(x + eyeX), static_cast<float>(-1.5 + eyeY), 2.6f, 0, static_cast<float>(kTau));
        canvas.fill();
    }
    canvas.setShadow(Color{0, 0, 0, 0}, 0);

    ui::setStroke(canvas, 0x3A4250u); canvas.setLineWidth(1.2f);
    for (const float y : {10.0f, 14.0f}) {
        canvas.beginPath(); canvas.moveTo(-10, y); canvas.lineTo(10, y); canvas.stroke();
    }
    for (int x = -10; x <= 10; x += 5) {
        canvas.beginPath(); canvas.moveTo(static_cast<float>(x), 9); canvas.lineTo(static_cast<float>(x), 15); canvas.stroke();
    }
    canvas.restore();
}

void WorldRenderer::drawHitbox(Canvas& canvas, const RemoteEntity& entity, const Camera& camera,
                              Vec2 at) const {
    const double zoom = camera.zoom();
    const Vec2 screen = camera.worldToScreen(at);

    // A flower and its petals are the two things whose drawn size deliberately
    // differs from what they collide with, so both are shown in the browser
    // build's red at 2px. Everything else keeps the generic overlay.
    double radius = entity.radius * zoom;
    std::uint32_t color = 0xFF0000u;
    if (entity.kind == net::EntityKind::Player) {
        radius = kPlayerBaseRadius * playerSizeMultiplier(entity) * zoom;
    } else if (entity.kind == net::EntityKind::Petal && content_) {
        radius = kPetalHitSize * content_->petal(entity.typeIndex).size * zoom;
    } else if (entity.kind == net::EntityKind::Mob) {
        // A mob's circle is its COLLISION size, drawn in its own tier colour:
        // visual_scale moves the artwork and never the body.
        color = rarityColor(entity.rarity);
    } else if (entity.kind == net::EntityKind::Drop) {
        // A drop is picked up by walking a square over it, so its overlay is
        // the browser build's yellow 30-unit box rather than a circle.
        ui::setStroke(canvas, 0xFFFF00u);
        canvas.setLineWidth(static_cast<float>(2.0 * zoom));
        canvas.beginPath();
        canvas.rect(static_cast<float>(screen.x - 15.0 * zoom),
                    static_cast<float>(screen.y - 15.0 * zoom),
                    static_cast<float>(30.0 * zoom), static_cast<float>(30.0 * zoom));
        canvas.stroke();
        return;
    } else if (entity.kind == net::EntityKind::Projectile) {
        color = 0x00FFFFu;
    } else {
        ui::setStroke(canvas, 0xFF00FFu, 0.7);
        canvas.setLineWidth(1.5f);
        canvas.strokeCircle(static_cast<float>(screen.x), static_cast<float>(screen.y),
                            static_cast<float>(radius));
        return;
    }

    ui::setStroke(canvas, color);
    canvas.setLineWidth(static_cast<float>(2.0 * zoom));
    canvas.strokeCircle(static_cast<float>(screen.x), static_cast<float>(screen.y),
                        static_cast<float>(radius));
}

Vec2 WorldRenderer::mobEye(std::uint32_t netId, double angle) const {
    // A fixed fraction per FRAME, exactly as the browser build eases it -- the
    // eye of a flower-shaped mob is the only thing showing where it is headed,
    // and easing it per second instead changes how it tracks at any other
    // refresh rate.
    const Vec2 target{std::cos(angle) * 2.0, std::sin(angle) * 4.4};
    const auto it = mobEyes_.find(netId);
    if (it == mobEyes_.end()) {
        // First sight starts ON target: a mob popping in should not roll its
        // eyes into place from the origin.
        mobEyes_[netId] = target;
        return target;
    }
    it->second.x += (target.x - it->second.x) * 0.15;
    it->second.y += (target.y - it->second.y) * 0.15;
    return it->second;
}

const std::vector<std::uint16_t>& WorldRenderer::droppablePetals() const {
    if (!droppablePetals_.empty() || !content_) return droppablePetals_;
    for (std::uint16_t i = 0; i < content_->petalCount(); ++i) {
        const PetalConfig& petal = content_->petal(i);
        // The server's own drop rule: no admin petals, no cutters, and no egg
        // for a mob whose config forbids one.
        if (petal.isAdminPetal) continue;
        if (petal.id == "cutter" || petal.id == "lightning_cutter") continue;
        const std::string suffix = "_egg";
        if (petal.id.size() > suffix.size() &&
            petal.id.compare(petal.id.size() - suffix.size(), suffix.size(), suffix) == 0) {
            const std::uint16_t mob =
                content_->mobIndex(petal.id.substr(0, petal.id.size() - suffix.size()));
            if (mob != kInvalidIndex && content_->mob(mob).noEggDrop) continue;
        }
        droppablePetals_.push_back(i);
    }
    return droppablePetals_;
}

void WorldRenderer::drawGarbagePile(Canvas& canvas, Vec2 at, double baseSize,
                                    double timeSeconds) const {
    const std::vector<std::uint16_t>& eligible = droppablePetals();
    if (eligible.empty() || !sprites_ || !content_) return;

    // Seeded on where the pile stands, so every client builds the same pile out
    // of the same petals without a byte of it crossing the wire.
    const long long seed = static_cast<long long>(std::floor(at.x * 1000.0 + at.y * 1000.0));
    const int count = 5 + static_cast<int>(((seed % 5) + 5) % 5);
    for (int i = 0; i < count; ++i) {
        const long long petalSeed = ((seed + i * 1000LL) % 1000000LL + 1000000LL) % 1000000LL;
        const std::size_t pick = static_cast<std::size_t>(
            static_cast<double>(petalSeed) / 1000000.0 * static_cast<double>(eligible.size()));
        const std::uint16_t index = eligible[std::min(pick, eligible.size() - 1)];

        const double angle = static_cast<double>(i) / count * kTau;
        const double maxRadius = (baseSize * 0.5) * 0.8;
        const double radius = maxRadius * (0.7 + static_cast<double>(petalSeed % 300) / 1000.0);
        const double x = std::cos(angle) * radius;
        const double y = std::sin(angle) * radius + (i % 3) * 3.0;
        const double rotation = static_cast<double>(petalSeed % 360) * kPi / 180.0;
        const PetalConfig& config = content_->petal(index);
        const double size = baseSize * (0.6 + static_cast<double>(petalSeed % 200) / 1000.0) *
                            config.size * petalArtScale(&config);
        sprites_->drawPetal(canvas, index, x, y, size, rotation, timeSeconds);
    }
}

void WorldRenderer::drawDiggerMob(Canvas& canvas, const MobDraw& mob, double radius,
                                  double timeSeconds) const {
    const double scale = radius / kFlowerArtRadius;

    // The cutter goes down first so the blade sits behind the face. It is sized
    // and spun exactly like the one a player carries, so the two read as the
    // same object.
    if (sprites_ && content_) {
        const std::uint16_t cutter = content_->petalIndex("cutter");
        if (cutter != kInvalidIndex) {
            const PetalConfig& config = content_->petal(cutter);
            const double speed = config.speed > 0 ? config.speed : 1.0;
            const double size = kPetalArtSize * config.size * petalArtScale(&config) * scale;
            sprites_->drawPetal(canvas, cutter, 0, 0, size,
                                std::fmod(timeSeconds * kPetalSpinRate * speed, kTau), timeSeconds);
        }
    }

    canvas.save();
    canvas.scale(static_cast<float>(scale), static_cast<float>(scale));
    const Vec2 eye = mobEye(mob.netId, mob.angle);
    drawFace(canvas, FaceSquareEyes, EquipNone, eye.x, eye.y, 14.5, timeSeconds, kDiggerBodyColor);
    canvas.restore();
}

void WorldRenderer::drawPetalRingMob(Canvas& canvas, const MobConfig& config, const MobDraw& mob,
                                     double radius, double timeSeconds) const {
    const double scale = radius / kFlowerArtRadius;
    canvas.save();
    canvas.scale(static_cast<float>(scale), static_cast<float>(scale));
    const Vec2 eye = mobEye(mob.netId, mob.angle);
    drawFace(canvas, FaceSquareEyes, EquipNone, eye.x, eye.y, 14.5, timeSeconds,
             kPetalRingBodyColor);
    canvas.restore();

    const std::uint16_t index = config.petalRing.petalIndex;
    if (!sprites_ || !content_ || index == kInvalidIndex) return;
    const PetalConfig& petal = content_->petal(index);
    const int count = static_cast<int>(clamp(config.petalRing.count, 0, 16));
    if (count <= 0) return;

    // Every distance is a multiple of the mob's own radius, so the ring grows
    // with rarity along with the body and stays where the server damages from.
    const double orbit = radius * kPetalRingOrbitScale;
    const double size = radius * kPetalRingPetalScale * petal.size * petalArtScale(&petal);
    const double speed = petal.speed > 0 ? petal.speed : 1.0;
    const double rotation = std::fmod(timeSeconds * kPetalSpinRate * speed, kTau);
    const double step = kTau / count;
    for (int i = 0; i < count; ++i) {
        const double angle = i * step + rotation;
        sprites_->drawPetal(canvas, index, std::cos(angle) * orbit, std::sin(angle) * orbit, size,
                            0.0, timeSeconds);
    }
}

void WorldRenderer::drawMobBody(Canvas& canvas, const Camera& camera, const MobDraw& mob,
                                double clockSeconds) const {
    const MobConfig* config = content_ ? &content_->mob(mob.typeIndex) : nullptr;
    // A mob that has locked on beats its wings twice as fast, but only the two
    // kinds that actually chase: a passive mob fleeing is not excited, and a
    // sandstorm has no target to lock on to in the first place.
    const bool hurries = mob.chasing && config &&
                         (config->ai == AiKind::Neutral || config->ai == AiKind::Hostile);
    const double timeSeconds = hurries ? clockSeconds * 2.0 : clockSeconds;
    const double zoom = camera.zoom();
    const Vec2 screen = camera.worldToScreen(mob.position);

    const double visualScale = (config && config->visualScale > 0) ? config->visualScale : 1.0;
    double diameter = mob.radius * 2.0 * visualScale * zoom;
    double alpha = 1.0;
    if (mob.deathProgress >= 0.0) {
        const double p = clamp(mob.deathProgress, 0.0, 1.0);
        // Balloons to three times its size while it fades on a cubic curve,
        // which is what makes a kill read at a glance in a crowd.
        diameter *= 1.0 + p * 2.0;
        alpha = 1.0 - p * p * p;
    }
    if (diameter <= 0.5 || alpha <= 0.0) return;

    double rotation = (config && config->hideRotation) ? 0.0 : mob.angle;
    bool mirrored = config && config->reversed;
    // The server points `reversed` art backwards by adding pi to the facing.
    // The browser build MIRRORS it instead, which is a different transform for
    // anything asymmetric, so undo the half turn and reflect it here.
    if (mirrored && !(config && config->hideRotation)) rotation = mob.angle - kPi;

    canvas.save();
    if (alpha < 1.0) canvas.setGlobalAlpha(static_cast<float>(alpha));

    if (config && config->emissive) {
        // Radially symmetric, so the mob's own rotation never reaches it.
        const double lightRadius =
            (config->lightRadius > 0 ? config->lightRadius : mob.radius * 4.0) * zoom;
        canvas.save();
        canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
        // The sun lights 2000 units, which is the whole screen: sixteen bands
        // of it is sixteen full-screen fills a frame, so a glow that large is
        // painted coarsely. The browser build blits a baked sprite and never
        // pays this at all.
        drawPetalGlow(canvas, lightRadius, config->lightColorRgba >> 8,
                      lightRadius > 300.0 ? 6 : 16);
        canvas.restore();
    }

    static const std::string kNoId;
    const std::string& id = config ? config->id : kNoId;
    if (id == "garbage") {
        // Its entry in mobs.json is an empty document: the pile IS the artwork,
        // laid out in world units off the mob's COLLISION size -- the browser
        // build sizes the pile from that and never from the death scale.
        canvas.save();
        canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
        canvas.scale(static_cast<float>(mirrored ? -zoom : zoom), static_cast<float>(zoom));
        drawGarbagePile(canvas, mob.position, mob.radius * 2.0, timeSeconds);
        canvas.restore();
    } else if (id == "digger") {
        // A flower carrying a cutter rather than a bug, the way gardn draws it.
        canvas.save();
        canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
        drawDiggerMob(canvas, mob, diameter * 0.5, timeSeconds);
        canvas.restore();
    } else if (config && config->petalRing.present) {
        const double radius = diameter * 0.5;
        const auto paint = [&](Canvas& target) {
            drawPetalRingMob(target, *config, mob, radius, timeSeconds);
        };
        if (id == "glitch_flower") {
            // The wrapper has to cover the RING, not just the body: it sizes
            // its buffer from the radius it is handed.
            drawGlitched(canvas, screen, radius * (kPetalRingOrbitScale * 0.5 + 0.3),
                         mob.netId, timeSeconds, paint);
        } else {
            canvas.save();
            canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
            paint(canvas);
            canvas.restore();
        }
    } else if (sprites_ && sprites_->mobDrawable(mob.typeIndex)) {
        sprites_->drawMob(canvas, mob.typeIndex, screen.x, screen.y, diameter, rotation,
                          timeSeconds, mirrored);
    } else {
        // No artwork: the tier colour, which is at least the one fact about a
        // mob worth reading from across the screen.
        ui::disc(canvas, screen, diameter * 0.5, rarityColor(mob.rarity), ui::kInk, 2.0 * zoom);
    }
    canvas.restore();
}

void WorldRenderer::drawMobLabel(Canvas& canvas, const Camera& camera, const MobDraw& mob) const {
    const double zoom = camera.zoom();
    const Vec2 screen = camera.worldToScreen(mob.position);
    const MobConfig* config = content_ ? &content_->mob(mob.typeIndex) : nullptr;
    const double visualScale = (config && config->visualScale > 0) ? config->visualScale : 1.0;
    // The bar hangs off the DRAWN size, so visual_scale moves it along with
    // the artwork it labels.
    const double enemySize = mob.radius * 2.0 * visualScale;
    // A hornet is the smallest mob the bar is allowed to shrink to: below that
    // the name would be wider than the bar it labels.
    const double barWidth = std::max(enemySize, kMobBarMinWidth) * zoom;
    const double barHeight = kMobBarHeight * zoom;
    const double barY = screen.y + (enemySize * 0.5 + 8.0) * zoom;
    const double barX = screen.x - barWidth * 0.5;

    // The browser build bakes the name, the bar background and the tier into
    // one atlas cell four units wider than the bar on each side, so a name
    // longer than its own bar is CROPPED rather than spilling over the mob
    // beside it. Only the dummy's DPS line is drawn outside the cell.
    canvas.save();
    canvas.beginPath();
    canvas.rect(static_cast<float>(barX - 4.0 * zoom), static_cast<float>(barY - 18.0 * zoom),
                static_cast<float>(barWidth + 8.0 * zoom), static_cast<float>(44.0 * zoom));
    canvas.clip();

    if (options.names) {
        ui::TextStyle style;
        style.size = 12.0 * zoom;
        style.align = ui::Align::Left;
        // The browser build never sets a baseline in the world pass, so the pen
        // sits on the alphabetic baseline.
        style.baseline = ui::Baseline::Alphabetic;
        style.strokeWidth = 3.0 * zoom;
        static const std::string kUnknownMob = "?";
        ui::text(canvas, config ? config->name : kUnknownMob, barX, barY - 4.0 * zoom, style);
    }

    if (options.healthBars) {
        // Always drawn, even at full health: the bar, the name and the tier are
        // one block of text, and the block is how a mob is identified.
        ui::setFill(canvas, ui::kHealthBack);
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(barX - zoom), static_cast<float>(barY - zoom),
                         static_cast<float>(barWidth + 2.0 * zoom),
                         static_cast<float>(barHeight + 2.0 * zoom),
                         static_cast<float>(barHeight * 0.5));
        canvas.fill();

        const double fill = clamp(mob.healthFraction, 0.0, 1.0) * barWidth;
        if (fill > 0) {
            ui::setFill(canvas, ui::kHealth);
            canvas.beginPath();
            canvas.roundRect(static_cast<float>(barX), static_cast<float>(barY),
                             static_cast<float>(fill), static_cast<float>(barHeight),
                             static_cast<float>(barHeight * 0.5));
            canvas.fill();
        }
    }

    if (options.names) {
        ui::TextStyle tier;
        tier.size = 10.0 * zoom;
        tier.align = ui::Align::Right;
        tier.baseline = ui::Baseline::Alphabetic;
        tier.strokeWidth = 3.0 * zoom;
        tier.fill = rarityColor(mob.rarity);
        ui::text(canvas, rarityLabel(mob.rarity), barX + barWidth, barY + 20.0 * zoom, tier);
    }
    canvas.restore();

    // The dummy is the one mob that reports what it is being hit for, and it
    // reports a zero rather than disappearing when nothing is hitting it.
    if (config && config->id == "target_dummy") {
        double total = 0;
        const auto damage = dummyDamage_.find(mob.netId);
        if (damage != dummyDamage_.end()) {
            for (const auto& sample : damage->second) total += sample.second;
        }
        ui::TextStyle dps;
        dps.size = 10.0 * zoom;
        dps.align = ui::Align::Right;
        dps.baseline = ui::Baseline::Alphabetic;
        dps.strokeWidth = 2.0 * zoom;
        ui::text(canvas, "DPS: " + formatCompact(total / kDpsWindowSeconds), barX + barWidth,
                 barY + 34.0 * zoom, dps);
    }
}

void WorldRenderer::drawMapElements(Canvas& canvas, const Camera& camera,
                                    double timeSeconds) const {
    if (!map_) return;
    const double zoom = camera.zoom();

    if (options.rarityGlow) {
        // Under the walls, and only while the glow is held: this is a map the
        // player asks for, not a decoration.
        for (const MapElement& element : map_->elements()) {
            if (element.kind != MapElementKind::Spawn || !element.hasSpawnTier) continue;
            const Vec2 at = camera.worldToScreen({element.bounds.x, element.bounds.y});
            ui::setFill(canvas, rarityColor(element.spawnTier), 0.25);
            canvas.fillRect(static_cast<float>(at.x), static_cast<float>(at.y),
                            static_cast<float>(element.bounds.w * zoom),
                            static_cast<float>(element.bounds.h * zoom));
        }
    }

    // Everything else the annotation layer carries paints nothing: the browser
    // build's MAP_COLORS are all fully transparent and its spawn points draw
    // nothing at all. Only the teleporters are visible.
    const Rect visible = camera.visibleWorld(kTeleporterCull);
    for (const MapElement& element : map_->elements()) {
        if (element.kind != MapElementKind::Teleporter) continue;
        const Vec2 centre = element.centre();
        if (!visible.contains(centre)) continue;
        const Vec2 at = camera.worldToScreen(centre);

        canvas.save();
        canvas.translate(static_cast<float>(at.x), static_cast<float>(at.y));
        // The soft glow. cpp_canvas has no radial gradient, so the same three
        // stops are painted as the nested discs drawPetalGlow builds -- the
        // teleporter's OWN stops (0.3, 0.1, 0), not the petal ramp under a
        // blanket alpha, which lands a quarter too opaque at the knee.
        drawPetalGlow(canvas, 130.0 * zoom, 0xFFFFFFu, 16, 0.3, 0.1);

        constexpr struct { double size, speed, alpha; } kSquares[] = {
            {180.0, 0.8, 0.12}, {120.0, -1.3, 0.18}, {70.0, 1.8, 0.25},
        };
        for (const auto& square : kSquares) {
            canvas.save();
            canvas.rotate(static_cast<float>(timeSeconds * square.speed));
            ui::setFill(canvas, 0xFFFFFFu, square.alpha);
            const double side = square.size * zoom;
            canvas.fillRect(static_cast<float>(-side * 0.5), static_cast<float>(-side * 0.5),
                            static_cast<float>(side), static_cast<float>(side));
            canvas.restore();
        }

        // Twelve particles spiralling inward, spread by the golden angle so
        // they never bunch up into a visible spoke.
        for (int i = 0; i < 12; ++i) {
            const double seed = i * 137.508;
            const double progress = std::fmod(timeSeconds * 0.8 + seed, 3.0) / 3.0;
            const double angle = seed + timeSeconds * 0.5 + progress * 2.0;
            const double radius = (1.0 - progress) * 120.0 * zoom;
            ui::setFill(canvas, 0xFFFFFFu, progress * 0.7);
            canvas.fillCircle(static_cast<float>(std::cos(angle) * radius),
                              static_cast<float>(std::sin(angle) * radius),
                              static_cast<float>(((1.0 - progress) * 3.0 + 0.5) * zoom));
        }

        ui::setFill(canvas, 0xFFFFFFu, 0.9);
        canvas.fillCircle(0, 0, static_cast<float>(4.0 * zoom));
        canvas.restore();
    }
}

void WorldRenderer::drawEntity(Canvas& canvas, const RemoteEntity& entity, const Camera& camera,
                               Vec2 at, double timeSeconds) const {
    const double zoom = camera.zoom();
    const Vec2 screen = camera.worldToScreen(at);
    const double radius = entity.radius * zoom;

    switch (entity.kind) {
        case net::EntityKind::Player:
            if (entity.dead()) {
                // A corpse carries none of the living furniture: no plate, no
                // bar, no level, no petals.
                drawCorpse(canvas, entity, camera, at, timeSeconds);
                break;
            }
            // The plate goes down first so a grown flower paints over the top
            // of its own name rather than the other way round.
            drawPlayerPlate(canvas, entity, camera, at, timeSeconds);
            drawFlower(canvas, entity, camera, at, timeSeconds);
            break;

        case net::EntityKind::Mob: {
            MobDraw mob;
            mob.netId = entity.netId;
            mob.position = at;
            mob.angle = entity.angle;
            mob.radius = entity.radius;
            mob.typeIndex = entity.typeIndex;
            mob.rarity = entity.rarity;
            mob.healthFraction = entity.healthFraction;
            mob.chasing = (entity.state & net::StateChasing) != 0;
            drawMobBody(canvas, camera, mob, timeSeconds);

            // A Killed event arrives after the snapshot has already erased the
            // entity, so remember what the mob looked like while it is here.
            mobShadows_[entity.netId] = mob;
            // Bars, names and tiers all go down after every body, so a mob
            // drawn later cannot cover an earlier one's label.
            mobLabels_.push_back(mob);
            break;
        }

        case net::EntityKind::Petal:
            drawPetalSprite(canvas, entity, camera, at, timeSeconds);
            break;

        case net::EntityKind::Projectile: {
            // A projectile IS its petal: the same artwork, filling the body
            // the server gave it, turned to its heading.
            const PetalConfig* config = content_ ? &content_->petal(entity.typeIndex) : nullptr;
            // The config size is the fallback only: a spawn record always
            // carries a radius, so this is reached for a projectile whose body
            // the server declined to state at all.
            const double artUnits = entity.radius > 0.0
                                        ? entity.radius * kProjectileArtPerRadius
                                        : (config ? config->size : 1.0) * kProjectileArtSize;
            // The shot is the petal's artwork, so it grows with the petal's
            // visual_scale. entity.radius above is the body the server damages
            // from and stays where it is.
            const double diameter = artUnits * petalArtScale(config) * zoom;
            if (config && config->id == "gas" && entity.rarity == Rarity::Common) {
                // Gas is a cloud rather than a petal, and there can be hundreds
                // of it at once.
                ui::setFill(canvas, 0x00FF00u, 0.5);
                canvas.fillCircle(static_cast<float>(screen.x), static_cast<float>(screen.y),
                                  static_cast<float>(diameter * 0.5));
                break;
            }
            if (sprites_ && sprites_->petalDrawable(entity.typeIndex)) {
                sprites_->drawPetal(canvas, entity.typeIndex, screen.x, screen.y, diameter,
                                    entity.angle, timeSeconds);
            } else {
                ui::disc(canvas, screen, std::max(2.0, diameter * 0.5),
                         config ? static_cast<std::uint32_t>(config->colorRgba >> 8)
                                : rarityColor(entity.rarity),
                         ui::kPaper, 2.0 * zoom);
            }
            break;
        }

        case net::EntityKind::Drop: {
            // A drop lands with a flourish: it slides in from a random offset
            // and unwinds a random spin over 400 ms, easing out, settling on
            // its own slight tilt rather than perfectly square.
            Vec2 where = at;
            double rotation = dropRestRotation(entity.netId);
            const auto spawn = dropSpawns_.find(entity.netId);
            if (spawn != dropSpawns_.end()) {
                const double t = clamp(spawn->second.ageSeconds / kDropSpawnSeconds, 0.0, 1.0);
                const double eased = 1.0 - (1.0 - t) * (1.0 - t);
                const double offset = spawn->second.distance * (1.0 - eased);
                where += Vec2::fromAngle(spawn->second.angle, offset);
                rotation += spawn->second.rotation * (1.0 - eased);
            }
            drawDrop(canvas, camera, where, entity.typeIndex, entity.rarity, rotation, 1.0, 1.0,
                     timeSeconds);
            break;
        }

        case net::EntityKind::Effect: {
            // Replicated::typeIndex carries the field kind: pollen, web, or
            // radiation. The effect's rarity supplies its tier treatment.
            switch (static_cast<GroundEffectKind>(entity.typeIndex)) {
                case GroundEffectKind::Web: {
                    // Ten spokes out to the rim, then five concentric rings of
                    // quadratic segments sagging between them.
                    constexpr int kSpokes = 10;
                    constexpr int kLevels = 5;
                    canvas.save();
                    canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
                    canvas.rotate(static_cast<float>(entity.angle));
                    canvas.setGlobalAlpha(static_cast<float>(0x60 / 255.0));
                    ui::setStroke(canvas, 0xFFFFFFu);
                    canvas.setLineCap("round");
                    canvas.setLineWidth(static_cast<float>(std::max(1.0, radius * 0.075)));
                    canvas.beginPath();
                    for (int i = 0; i < kSpokes; ++i) {
                        const double angle = kTau * i / kSpokes;
                        canvas.moveTo(0, 0);
                        canvas.lineTo(static_cast<float>(std::cos(angle) * radius),
                                      static_cast<float>(std::sin(angle) * radius));
                    }
                    for (int j = 0; j < kLevels; ++j) {
                        const double ring = j * radius / kLevels;
                        const double sag = ring - radius / (2 * kLevels);
                        canvas.moveTo(static_cast<float>(ring), 0);
                        for (int i = 0; i < kSpokes; ++i) {
                            const double to = kTau * (i + 1) / kSpokes;
                            const double between = kPi * (2 * i + 1) / kSpokes;
                            canvas.quadraticCurveTo(static_cast<float>(std::cos(between) * sag),
                                                    static_cast<float>(std::sin(between) * sag),
                                                    static_cast<float>(std::cos(to) * ring),
                                                    static_cast<float>(std::sin(to) * ring));
                        }
                    }
                    canvas.stroke();
                    canvas.restore();
                    break;
                }
                case GroundEffectKind::Radiation: {
                    const std::uint32_t tint = 0x9B59B6u;
                    ui::setFill(canvas, tint, 0.22);
                    canvas.fillCircle(static_cast<float>(screen.x), static_cast<float>(screen.y),
                                      static_cast<float>(radius));
                    ui::setStroke(canvas, tint, 0.5);
                    canvas.setLineWidth(static_cast<float>(2.0 * zoom));
                    canvas.strokeCircle(static_cast<float>(screen.x), static_cast<float>(screen.y),
                                        static_cast<float>(radius));
                    break;
                }
                case GroundEffectKind::Poison:
                default:
                    ui::disc(canvas, screen, radius, 0xFFE763u, 0xCFBB50u, 3.0 * zoom);
                    break;
            }
            break;
        }
    }
}

void WorldRenderer::drawDrop(Canvas& canvas, const Camera& camera, Vec2 at,
                             std::uint16_t typeIndex, Rarity rarity, double rotation,
                             double scale, double alpha, double timeSeconds) const {
    // The pulse rides on top of whatever the animations asked for, so a drop
    // flying to its taker or spinning out keeps breathing as it goes.
    scale *= 1.0 + std::sin(timeSeconds * kDropPulseRate) * kDropPulseAmount;
    const double zoom = camera.zoom();
    if (scale <= 0.0 || alpha <= 0.0 || kDropBackdropSide * zoom * scale <= 1.0) return;
    if (!sprites_) return;
    const Vec2 screen = camera.worldToScreen(at);

    // A drop is one item tile, the same object the loadout bar and the
    // inventory draw, laid on the ground with its shadow under it. The camera
    // is folded into the transform rather than multiplied into each number:
    // the tile is written in its own 60-unit cell and only lines up when the
    // whole cell is scaled at once.
    const double cell = kDropBackdropSide * zoom * scale;
    canvas.save();
    canvas.translate(static_cast<float>(screen.x), static_cast<float>(screen.y));
    if (rotation != 0) canvas.rotate(static_cast<float>(rotation));

    ui::ItemTile tile;
    tile.petalIndex = typeIndex;
    tile.rarity = rarity;
    tile.shadow = true;
    tile.alpha = alpha;
    // Frame zero, not the frame clock: a drop's petal does not animate on the
    // ground, and the name rides with the plate so a spinning despawn spins
    // its label too.
    tile.timeSeconds = 0.0;
    ui::drawItemTile(canvas, *sprites_, {-cell * 0.5, -cell * 0.5, cell, cell}, tile);

    canvas.restore();
}

void WorldRenderer::drawEffects(Canvas& canvas, const Camera& camera) const {
    const double zoom = camera.zoom();

    // The drop shimmer first: it is ground-level glitter and must not sit over
    // a damage number or an explosion that happens to share its patch.
    for (const EffectParticle& p : dropSparkles_) drawSparkleGrain(canvas, camera, p, true);

    for (const Effect& e : effects_) {
        const double t = clamp(e.ageSeconds / e.lifeSeconds, 0.0, 1.0);

        switch (e.kind) {
            case Effect::Kind::DamageNumber: {
                const Vec2 world{e.position.x + e.drift.x * t, e.position.y + e.drift.y * t};
                const Vec2 screen = camera.worldToScreen(world);
                ui::TextStyle style;
                style.size = e.textSize * zoom;
                style.fill = e.color;
                style.align = ui::Align::Centre;
                style.baseline = ui::Baseline::Alphabetic;
                // No outline: a floating number is drawn plain, and its colour
                // alone is what says damage.
                style.strokeWidth = 0;
                // Linear over the whole life, exactly as the browser build's
                // per-frame alpha decrement works out to.
                canvas.setGlobalAlpha(static_cast<float>(1.0 - t));
                ui::text(canvas, "-" + formatDamage(e.value), screen.x, screen.y, style);
                canvas.setGlobalAlpha(1.0f);
                break;
            }
            case Effect::Kind::Explosion: {
                const Vec2 screen = camera.worldToScreen(e.position);
                canvas.setGlobalAlpha(static_cast<float>(1.0 - t));
                // Two rings expanding together, the inner one at half the
                // radius, then the debris over the top of both.
                ui::setStroke(canvas, kExplosionOuter);
                canvas.setLineWidth(static_cast<float>(3.0 * zoom));
                canvas.strokeCircle(static_cast<float>(screen.x), static_cast<float>(screen.y),
                                    static_cast<float>(e.radius * t * zoom));
                ui::setStroke(canvas, kExplosionInner);
                canvas.setLineWidth(static_cast<float>(1.0 * zoom));
                canvas.strokeCircle(static_cast<float>(screen.x), static_cast<float>(screen.y),
                                    static_cast<float>(e.radius * t * 0.5 * zoom));

                for (const EffectParticle& p : e.particles) {
                    const double left = p.lifeSeconds / p.maxLifeSeconds;
                    if (left <= 0) continue;
                    const Vec2 at = camera.worldToScreen(p.position);
                    canvas.setGlobalAlpha(static_cast<float>(left));
                    ui::setFill(canvas, p.color);
                    canvas.fillCircle(static_cast<float>(at.x), static_cast<float>(at.y),
                                      static_cast<float>(p.size * left * zoom));
                }
                canvas.setGlobalAlpha(1.0f);
                break;
            }
            case Effect::Kind::Sparkle: {
                // Particles only: the shimmer has no body of its own.
                for (const EffectParticle& p : e.particles) {
                    drawSparkleGrain(canvas, camera, p, e.squareParticles);
                }
                break;
            }
        }
    }
}

void WorldRenderer::draw(Canvas& canvas, const WorldView& view, const Camera& camera,
                         Vec2 selfDrawn, double timeSeconds) const {
    selfNetId_ = view.self().netId;
    realm_ = view.realm();
    draw(canvas, view.entities(), camera, selfDrawn, timeSeconds);
}

void WorldRenderer::draw(Canvas& canvas, const EntityMap& entities, const Camera& camera,
                         Vec2 selfDrawn, double timeSeconds) const {
    // What lies under the entities is the realm's business. The map's
    // annotations -- teleporters, spawn-zone tints -- are the overworld's
    // alone; the other two have no map.
    switch (realm_) {
        case Realm::Maze:
            drawMaze(canvas, camera);
            break;
        case Realm::Arena:
            drawArena(canvas, camera);
            break;
        case Realm::Overworld:
            drawTerrain(canvas, camera);
            drawMapElements(canvas, camera, timeSeconds);
            break;
    }

    const Rect visible = camera.visibleWorld(0);
    // A body is kept until its whole extent is off screen, and the margin grows
    // with the body: a super-tier mob is wider than any fixed margin, and
    // popping one out while half of it is still visible is the failure a fixed
    // margin produces.
    const auto onScreen = [&visible](Vec2 at, double radius) {
        const double margin = radius + std::max(radius * 2.0, 100.0);
        return at.x + margin >= visible.left() && at.x - margin <= visible.right() &&
               at.y + margin >= visible.top() && at.y - margin <= visible.bottom();
    };

    // Draw order is by kind, not by position, and follows the browser build's:
    // ground effects, mobs, then every flower with its petals over it, then
    // loot, and projectiles last of all. Petals BELOW players was the visible
    // error -- a petal passing in front of a flower went behind its face.
    static constexpr net::EntityKind kOrder[] = {
        net::EntityKind::Effect, net::EntityKind::Mob, net::EntityKind::Player,
        net::EntityKind::Petal, net::EntityKind::Drop, net::EntityKind::Projectile,
    };


    // Per-layer cost, reported through sectionTiming(). The clock is only read
    // at layer boundaries -- six reads a frame -- so measuring the frame does
    // not measurably change it.
    const auto sectionClock = [] {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    timing_ = SectionTiming{};
    double layerStarted = sectionClock();

    mobLabels_.clear();
    for (const net::EntityKind kind : kOrder) {
        for (const auto& entry : entities) {
            const RemoteEntity& entity = entry.second;
            if (entity.kind != kind) continue;

            // Every entity draws at its own interpolated position, petals
            // included: WorldView has already anchored each ring to the
            // flower its owner is DRAWN at (see RemoteEntity::ownerOffset), so
            // there is no correction to apply here. Applying one was the petal
            // shake -- the obvious `owner.position - owner.targetPosition`
            // fixup subtracts a value that stair-steps at the snapshot rate.
            Vec2 at = entity.isSelf() ? selfDrawn : entity.position;
            if (!onScreen(at, entity.radius)) continue;

            if (kind == net::EntityKind::Drop) ++timing_.itemCount;
            drawEntity(canvas, entity, camera, at, timeSeconds);
            if (options.hitboxes) drawHitbox(canvas, entity, camera, at);
        }

        // Loot the snapshot has already removed finishes its flight to the
        // player who took it, or spins out where it lay, in the layer its live
        // siblings were drawn in.
        if (kind == net::EntityKind::Drop) {
            for (const DyingDrop& drop : dyingDrops_) {
                const bool taken = drop.takerNetId != 0;
                const double t = clamp(drop.ageSeconds / (taken ? kDropPickupSeconds
                                                                : kDropDespawnSeconds),
                                       0.0, 1.0);
                Vec2 where = drop.position;
                double rotation = dropRestRotation(drop.netId);
                double scale = 1.0;
                double alpha = 1.0;
                if (taken) {
                    // Eased IN, and aimed at where the taker is NOW: a drop
                    // flying at a stale position visibly misses a moving
                    // flower over the 150 ms it is in the air.
                    const double eased = t * t;
                    Vec2 target = drop.position;
                    const auto taker = entities.find(drop.takerNetId);
                    if (taker != entities.end()) {
                        target = taker->second.isSelf() ? selfDrawn : taker->second.position;
                    }
                    where += (target - drop.position) * eased;
                    scale = 1.0 - eased * 0.7;
                    alpha = 1.0 - eased * 0.5;
                } else {
                    rotation += t * kTau;
                    alpha = 1.0 - t;
                    scale = 1.0 - t * 0.3;
                }
                if (!onScreen(where, kDropBackdropSide)) continue;
                ++timing_.itemCount;
                drawDrop(canvas, camera, where, drop.typeIndex, drop.rarity, rotation, scale,
                         alpha, timeSeconds);
            }
        }

        // The mobs the server has already destroyed finish their animation in
        // the same layer the live ones were drawn in, and take no label: a bar
        // over a corpse is the browser build's one suppression here.
        // Layer boundaries, in the draw order kOrder declares. Petals close
        // the mob layer because they are drawn onto the flowers they orbit;
        // splitting them off would report a cost the reference does not.
        if (kind == net::EntityKind::Petal || kind == net::EntityKind::Drop ||
            kind == net::EntityKind::Projectile) {
            const double now = sectionClock();
            const double spent = now - layerStarted;
            layerStarted = now;
            if (kind == net::EntityKind::Petal) timing_.mobsMillis += spent;
            else if (kind == net::EntityKind::Drop) timing_.itemsMillis += spent;
            else timing_.projectilesMillis += spent;
        }

        if (kind != net::EntityKind::Mob) continue;
        for (const DyingMob& dying : dying_) {
            if (!onScreen(dying.position, dying.radius)) continue;
            MobDraw mob;
            mob.netId = dying.netId;
            mob.position = dying.position;
            mob.angle = dying.angle;
            mob.radius = dying.radius;
            mob.typeIndex = dying.typeIndex;
            mob.rarity = dying.rarity;
            mob.deathProgress = clamp(dying.ageSeconds / kDeathAnimationSeconds, 0.0, 1.0);
            drawMobBody(canvas, camera, mob, timeSeconds);
        }
        for (const MobDraw& mob : mobLabels_) drawMobLabel(canvas, camera, mob);
    }

    drawEffects(canvas, camera);
}

} // namespace flix
