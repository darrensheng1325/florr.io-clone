#include "client/render/mob_art.h"

#include <algorithm>
#include <cmath>

#include "client/ui/draw.h"
#include "client/ui/theme.h"
#include "shared/core/types.h"

namespace flix {

namespace {

/// gardn's seeded PRNG, transcribed from `Helpers/Math.cc`.
///
/// The point of it is that a rock's facets are a function of its RADIUS and
/// nothing else: two clients drawing the same rock cut it the same way without
/// a byte about the shape crossing the wire, and one rock keeps its own outline
/// for as long as it lives rather than shimmering into a new one every frame.
class SeedGenerator {
public:
    explicit SeedGenerator(std::uint32_t seed) : seed_(seed) {}

    /// [0, 1).
    double next() {
        seed_ *= 167436543u;
        seed_ += 5832385u;
        seed_ *= (76372345u + seed_);
        seed_ += 937323u;
        return static_cast<double>(seed_ % 65536u) / 65536.0;
    }

    /// (-1, 1).
    double binext() { return next() * 2.0 - 1.0; }

private:
    std::uint32_t seed_;
};

/// The most facets or spines a body is cut into.
///
/// gardn's counts are `4 + radius/10` and `5 + radius/10`, over mobs whose
/// radius never passes 60. Ours reach 43x their base size at the top of the
/// rarity ladder, where those formulae would ask for hundreds of vertices to
/// draw detail finer than the outline around it. The cap binds only past a
/// radius of ~600 -- a mob wider than half the viewport -- and everything below
/// it is gardn's own number.
constexpr int kMaxFacets = 64;

int facetCount(double radius, int base) {
    // Clamped before the conversion, not after: a radius large enough to
    // overflow the int is nonsense rather than a very detailed rock, and the
    // conversion itself would be undefined.
    const double n = base + std::min(radius, 10.0 * kMaxFacets) / 10.0;
    return std::clamp(static_cast<int>(n), base, kMaxFacets);
}

void roundStrokes(Canvas& canvas, double width) {
    canvas.setLineWidth(static_cast<float>(width));
    canvas.setLineCap("round");
    canvas.setLineJoin("round");
}

/// The outline every gardn body wears: its own fill at 0.8 HSV value, which
/// for an opaque colour is the channels scaled.
std::uint32_t outlineOf(std::uint32_t rgb) { return ui::shade(rgb, 0.8); }

// ---------------------------------------------------------------------------
// The painters
// ---------------------------------------------------------------------------

void paintRock(Canvas& canvas, const MobArtAttributes& attr) {
    const double radius = attr.radius;
    const int sides = facetCount(radius, 4);

    // Seeded off the radius alone, in 64 bits so that a boss-sized rock wraps
    // rather than overflowing the conversion.
    SeedGenerator gen(static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(std::floor(radius)) * 1957264ull + 295726ull));

    // gardn's 10% of the radius. Past its own size range that would exceed the
    // facet it displaces -- the chord shrinks towards a constant as the sides
    // multiply while 0.1r does not -- and the outline crosses itself into a
    // scribble. A quarter of the chord is well clear of every deflection the
    // reference range produces, so this is gardn's rock wherever gardn has one.
    const double chord = 2.0 * radius * std::sin(kPi / sides);
    const double deflection = std::min(radius * 0.1, chord * 0.25);

    ui::setFill(canvas, attr.baseColor);
    ui::setStroke(canvas, outlineOf(attr.baseColor));
    roundStrokes(canvas, 5.0);
    canvas.beginPath();
    {
        const double x = radius + gen.binext() * deflection;
        const double y = gen.binext() * deflection;
        canvas.moveTo(static_cast<float>(x), static_cast<float>(y));
    }
    for (int i = 1; i < sides; ++i) {
        const double angle = kTau * i / sides;
        const double x = std::cos(angle) * radius + gen.binext() * deflection;
        const double y = std::sin(angle) * radius + gen.binext() * deflection;
        canvas.lineTo(static_cast<float>(x), static_cast<float>(y));
    }
    canvas.closePath();
    canvas.fill();
    canvas.stroke();
}

void paintCactus(Canvas& canvas, const MobArtAttributes& attr) {
    const double radius = attr.radius;
    const int spines = facetCount(radius, 5);
    const double step = kTau / spines;

    // The spines first, so the body is laid over their roots and only the 10
    // units that clear it show. gardn rotates the canvas between spines and
    // fills them all at once; this rotates the POINTS, because a path here is
    // flattened with the transform in force when it is filled rather than the
    // one in force when each point was added.
    ui::setFill(canvas, 0x222222u);
    canvas.beginPath();
    for (int i = 0; i < spines; ++i) {
        const double angle = step * i;
        const double c = std::cos(angle), s = std::sin(angle);
        const auto point = [&](double x, double y) {
            canvas.lineTo(static_cast<float>(x * c - y * s), static_cast<float>(x * s + y * c));
        };
        canvas.moveTo(static_cast<float>((10.0 + radius) * c), static_cast<float>((10.0 + radius) * s));
        point(0.5 + radius, 3.0);
        point(0.5 + radius, -3.0);
        point(10.0 + radius, 0.0);
    }
    canvas.fill();

    ui::setFill(canvas, attr.baseColor);
    ui::setStroke(canvas, outlineOf(attr.baseColor));
    roundStrokes(canvas, 5.0);
    canvas.beginPath();
    canvas.moveTo(static_cast<float>(radius), 0.0f);
    for (int i = 0; i < spines; ++i) {
        // Each lobe bulges IN to 0.9r between two spines, which is what makes
        // the silhouette read as segments rather than as a circle with hairs.
        const double base = step * i;
        canvas.quadraticCurveTo(
            static_cast<float>(radius * 0.9 * std::cos(base + step * 0.5)),
            static_cast<float>(radius * 0.9 * std::sin(base + step * 0.5)),
            static_cast<float>(radius * std::cos(base + step)),
            static_cast<float>(radius * std::sin(base + step)));
    }
    canvas.fill();
    canvas.stroke();
}

void paintSandstorm(Canvas& canvas, const MobArtAttributes& attr) {
    const double radius = attr.radius;
    // Three hexagons at three different fractions of the same phase, so the
    // layers shear against each other instead of turning as one body. The
    // shape does not scale with the radius -- only the picture does -- which is
    // why the line width has to: it is what rounds the corners off.
    const double spin = attr.animation / 3.0;
    roundStrokes(canvas, radius / 5.0);

    const double shades[3] = {1.0, 0.9, 0.8};
    const double scales[3] = {1.0, 2.0 / 3.0, 1.0 / 3.0};
    canvas.save();
    for (int layer = 0; layer < 3; ++layer) {
        canvas.rotate(static_cast<float>(spin));
        const std::uint32_t color = ui::shade(attr.baseColor, shades[layer]);
        ui::setFill(canvas, color);
        ui::setStroke(canvas, color);
        const double r = radius * scales[layer];
        canvas.beginPath();
        canvas.moveTo(static_cast<float>(r), 0.0f);
        for (int i = 1; i <= 6; ++i) {
            const double angle = kTau * i / 6.0;
            canvas.lineTo(static_cast<float>(std::cos(angle) * r),
                          static_cast<float>(std::sin(angle) * r));
        }
        canvas.fill();
        canvas.stroke();
    }
    canvas.restore();
}

void paintScorpion(Canvas& canvas, const MobArtAttributes& attr) {
    // Drawn at gardn's own design size and scaled to the body it belongs to:
    // unlike the rock and the cactus this one has a fixed anatomy, and it is
    // here for the other reason -- the claws and legs move, which no document
    // in mobs.json can do.
    canvas.save();
    canvas.scale(static_cast<float>(attr.radius / 35.0), static_cast<float>(attr.radius / 35.0));

    const double swing = std::sin(attr.animation);

    // --- claws -------------------------------------------------------------
    // One at a time, each under its own rotation: gardn banks both into one
    // path and fills once, which this canvas cannot reproduce (see paintCactus)
    // and which is the same picture anyway -- the two are disjoint and share
    // every style.
    ui::setFill(canvas, 0x333333u);
    ui::setStroke(canvas, 0x333333u);
    roundStrokes(canvas, 7.0);
    for (int side = 0; side < 2; ++side) {
        const double sign = side == 0 ? 1.0 : -1.0;
        canvas.save();
        canvas.rotate(static_cast<float>(-0.05 * swing * sign));
        canvas.beginPath();
        canvas.moveTo(5.0f, static_cast<float>(10.5 * sign));
        canvas.quadraticCurveTo(30.0f, static_cast<float>(21.5 * sign), 50.0f,
                                static_cast<float>(10.5 * sign));
        canvas.quadraticCurveTo(30.0f, static_cast<float>(14.0 * sign), 5.0f,
                                static_cast<float>(3.5 * sign));
        canvas.closePath();
        canvas.fill();
        canvas.stroke();
        canvas.restore();
    }

    // --- legs --------------------------------------------------------------
    // Eight, four a side, each a curve out of the body's centre. The endpoint
    // takes the sine on X and the cosine on Y, which is what lays them along
    // the flanks rather than around the whole body.
    ui::setStroke(canvas, 0x333333u);
    roundStrokes(canvas, 5.0);
    canvas.beginPath();
    const double legAngles[8] = {
        -kPi + 0.7 + std::sin(attr.animation) * 0.15,
        -kPi + 0.233 + std::cos(attr.animation) * 0.15,
        -kPi - 0.233 + std::sin(attr.animation) * 0.15,
        -kPi - 0.7 - std::cos(attr.animation) * 0.15,
        -0.7 - std::sin(attr.animation) * 0.15,
        -0.233 + std::cos(attr.animation) * 0.15,
        0.233 - std::sin(attr.animation) * 0.15,
        0.7 - std::cos(attr.animation) * 0.15,
    };
    for (const double angle : legAngles) {
        const double c = std::cos(angle) * 37.0;
        const double s = std::sin(angle) * 37.0;
        canvas.moveTo(0.0f, 0.0f);
        canvas.quadraticCurveTo(static_cast<float>(s * 0.7), static_cast<float>(c * 0.5),
                                static_cast<float>(s), static_cast<float>(c));
    }
    canvas.stroke();

    // --- body --------------------------------------------------------------
    ui::setFill(canvas, attr.baseColor);
    ui::setStroke(canvas, outlineOf(attr.baseColor));
    canvas.beginPath();
    canvas.moveTo(0.0f, -30.0f);
    canvas.quadraticCurveTo(40.0f, -20.0f, 40.0f, 0.0f);
    canvas.quadraticCurveTo(40.0f, 20.0f, 0.0f, 30.0f);
    canvas.quadraticCurveTo(-40.0f, 35.0f, -40.0f, 0.0f);
    canvas.quadraticCurveTo(-40.0f, -35.0f, 0.0f, -30.0f);
    canvas.fill();
    canvas.stroke();

    // The plates across the back, in the body's own outline colour.
    canvas.setLineWidth(7.0f);
    canvas.beginPath();
    canvas.moveTo(22.0f, -12.0f);
    canvas.quadraticCurveTo(26.0f, 0.0f, 22.0f, 12.0f);
    canvas.moveTo(7.0f, -18.0f);
    canvas.quadraticCurveTo(10.5f, 0.0f, 7.0f, 18.0f);
    canvas.moveTo(-7.0f, -18.0f);
    canvas.quadraticCurveTo(-10.5f, 0.0f, -7.0f, 18.0f);
    canvas.moveTo(-22.0f, -15.0f);
    canvas.quadraticCurveTo(-27.0f, 0.0f, -22.0f, 15.0f);
    canvas.stroke();

    // --- tail --------------------------------------------------------------
    canvas.setLineWidth(5.0f);
    canvas.beginPath();
    canvas.moveTo(-45.0f, 0.0f);
    canvas.bezierCurveTo(-44.9098f, 9.5f, -41.6136f, 14.25f, -32.4196f, 14.2f);
    canvas.bezierCurveTo(-23.2258f, 14.15f, -12.0197f, 9.0f, -8.2491f, 0.0f);
    canvas.bezierCurveTo(-12.0197f, -9.0f, -23.2258f, -14.15f, -32.4196f, -14.2f);
    canvas.bezierCurveTo(-41.6136f, -14.25f, -44.9098f, -9.5f, -45.0f, 0.0f);
    canvas.closePath();
    canvas.fill();
    canvas.stroke();

    canvas.beginPath();
    canvas.moveTo(-37.0f, -5.0f);
    canvas.quadraticCurveTo(-36.0f, 0.0f, -37.0f, 5.0f);
    canvas.moveTo(-27.0f, 5.0f);
    canvas.quadraticCurveTo(-25.0f, 0.0f, -27.0f, -5.0f);
    canvas.stroke();

    // The sting, laid over the tail pointing back up the body.
    ui::setFill(canvas, 0x333333u);
    ui::setStroke(canvas, 0x222222u);
    canvas.beginPath();
    canvas.moveTo(-5.7491f, 0.0f);
    canvas.lineTo(-12.7491f, -7.0f);
    canvas.lineTo(-12.7491f, 7.0f);
    canvas.lineTo(-5.7491f, 0.0f);
    canvas.fill();
    canvas.stroke();

    canvas.restore();
}

} // namespace

MobArt mobArtFor(const std::string& image) {
    if (image.empty() || image[0] != '$') return MobArt::None;
    const std::string name = image.substr(1);
    if (name == "rock") return MobArt::Rock;
    if (name == "cactus") return MobArt::Cactus;
    if (name == "sandstorm") return MobArt::Sandstorm;
    if (name == "scorpion") return MobArt::Scorpion;
    return MobArt::None;
}

void paintMobArt(Canvas& canvas, MobArt art, const MobArtAttributes& attr) {
    if (attr.radius <= 0.0) return;
    switch (art) {
        case MobArt::Rock:      paintRock(canvas, attr); break;
        case MobArt::Cactus:    paintCactus(canvas, attr); break;
        case MobArt::Sandstorm: paintSandstorm(canvas, attr); break;
        case MobArt::Scorpion:  paintScorpion(canvas, attr); break;
        case MobArt::None:      break;
    }
}

} // namespace flix
