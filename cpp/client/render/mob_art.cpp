#include "client/render/mob_art.h"

#include <algorithm>
#include <cmath>

#include "client/ui/draw.h"
#include "client/ui/theme.h"
#include "shared/core/types.h"
#include "shared/game/constants.h"

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

/// The crab, ported from flooooio's MobRendererCrab.
///
/// Drawn at that renderer's own design radius of 25 and scaled to the body it
/// belongs to, exactly as the scorpion above is -- and here for the same
/// reason. A crab has eight legs that swing and two claws that open and close,
/// so its picture is a function of the walk phase and no document in mobs.json
/// can hold it. Facing is +x: the claws lead, the legs splay off the flanks.
void paintCrab(Canvas& canvas, const MobArtAttributes& attr) {
    canvas.save();
    canvas.scale(static_cast<float>(attr.radius / 25.0), static_cast<float>(attr.radius / 25.0));

    /// Legs, claws and the line around them: one dark shell-brown for all of
    /// them, as in the reference. Only the carapace takes the mob's own colour.
    constexpr std::uint32_t kLimb = 0x4D2621u;

    // --- legs --------------------------------------------------------------
    // Four a side. Each hangs off a hip spaced along the body's x axis, swings
    // about that hip on its own phase and bends once at the knee; the front
    // pair of a side bends forward and the back pair back, which is what stops
    // all eight from moving as one comb.
    //
    // The reference walks the canvas transform per leg -- translate to the hip,
    // rotate, draw, translate again to the knee -- and gets its points from it.
    // The points are computed here instead: this canvas bakes a path's points
    // at ADD time on the web backend and at FILL time natively, so a transform
    // moved inside a path is the one thing the two backends disagree about.
    ui::setStroke(canvas, kLimb);
    roundStrokes(canvas, 5.0);
    canvas.beginPath();
    for (int side = 0; side < 2; ++side) {
        const double flank = side == 0 ? -1.0 : 1.0;
        for (int i = 0; i < 4; ++i) {
            const double swing = 0.15 * std::sin(attr.animation + flank + 2.0 * i) + 0.15;
            const double legDir = i < 2 ? 1.0 : -1.0;
            const double hipX = 4.0 * i - 5.0;
            const double knee = legDir * 0.7 * (swing + 0.3);
            const double hip = swing * legDir;
            const double cosHip = std::cos(hip), sinHip = std::sin(hip);
            // Hip at the origin, knee 25 down the leg, foot 10 past the bend.
            const double footX = -10.0 * std::sin(knee);
            const double footY = 25.0 + 10.0 * std::cos(knee);
            const auto px = [&](double x, double y) {
                return static_cast<float>(x * cosHip - y * sinHip + hipX);
            };
            const auto py = [&](double x, double y) {
                return static_cast<float>((x * sinHip + y * cosHip) * flank);
            };
            canvas.moveTo(px(0.0, 0.0), py(0.0, 0.0));
            canvas.lineTo(px(0.0, 25.0), py(0.0, 25.0));
            canvas.lineTo(px(footX, footY), py(footX, footY));
        }
    }
    canvas.stroke();

    // --- claws -------------------------------------------------------------
    // One at a time under its own transform, the way the scorpion's are: the
    // two are disjoint, share every style, and the transform is set BEFORE the
    // path is opened rather than inside it.
    //
    // The reference carries a second, much longer path here -- the outline of
    // stroking this one at a width of 2, baked out. Filling this path and then
    // stroking it at 2 is that picture, and is the shape the baked one was
    // generated from.
    const double clawAngle = 0.15 * std::sin(attr.animation * 2.0) + 0.15;
    ui::setFill(canvas, kLimb);
    ui::setStroke(canvas, kLimb);
    roundStrokes(canvas, 2.0);
    for (int side = 0; side < 2; ++side) {
        const double flank = side == 0 ? -1.0 : 1.0;
        canvas.save();
        canvas.translate(12.0f, static_cast<float>(2.0 * flank));
        canvas.scale(1.0f, static_cast<float>(-flank));
        canvas.rotate(static_cast<float>(clawAngle));
        canvas.beginPath();
        canvas.moveTo(0.0f, -14.0f);
        canvas.quadraticCurveTo(11.0f, -20.0f, 16.0f, -9.0f);
        canvas.lineTo(11.0f, -12.0f);
        canvas.lineTo(13.0f, -7.0f);
        canvas.quadraticCurveTo(6.0f, -13.0f, 0.0f, -10.0f);
        canvas.lineTo(0.0f, -14.0f);
        canvas.closePath();
        canvas.fill();
        canvas.stroke();
        canvas.restore();
    }

    // --- carapace ----------------------------------------------------------
    // Taller than it is wide, and laid over the legs and the claw roots. The
    // reference fills a second baked band for the outline; a stroke at 4 is
    // that band.
    ui::setFill(canvas, attr.baseColor);
    ui::setStroke(canvas, outlineOf(attr.baseColor));
    roundStrokes(canvas, 4.0);
    canvas.beginPath();
    canvas.moveTo(0.0f, -23.0f);
    canvas.quadraticCurveTo(-7.4558f, -23.0f, -12.7279f, -16.2635f);
    canvas.quadraticCurveTo(-18.0f, -9.5269f, -18.0f, 0.0f);
    canvas.quadraticCurveTo(-18.0f, 9.5269f, -12.7279f, 16.2635f);
    canvas.quadraticCurveTo(-7.4558f, 23.0f, 0.0f, 23.0f);
    canvas.quadraticCurveTo(7.4558f, 23.0f, 12.7279f, 16.2635f);
    canvas.quadraticCurveTo(18.0f, 9.5269f, 18.0f, 0.0f);
    canvas.quadraticCurveTo(18.0f, -9.5269f, 12.7279f, -16.2635f);
    canvas.quadraticCurveTo(7.4558f, -23.0f, 0.0f, -23.0f);
    canvas.closePath();
    canvas.fill();
    canvas.stroke();

    // The two creases across the shell, in the carapace's own outline colour.
    canvas.beginPath();
    canvas.moveTo(-10.0f, 8.0f);
    canvas.quadraticCurveTo(0.0f, 3.0f, 10.0f, 8.0f);
    canvas.moveTo(-10.0f, -8.0f);
    canvas.quadraticCurveTo(0.0f, -3.0f, 10.0f, -8.0f);
    canvas.stroke();

    canvas.restore();
}

// --- the leech -------------------------------------------------------------
//
// One tube, painted a joint at a time. The reference strokes a polyline
// through every segment's centre at width 25 and again at 22; ours cannot see
// the polyline, so each segment strokes the ONE span it does know -- its own
// centre to its leader's -- and the spans abut into the same shape.
//
// Two rules make that work, and both are the chain pass's doing (placeFollower
// in mob_ai.cpp): a segment is held exactly `kSegmentSpacingPerRadius` radii
// behind its leader, and it is turned to point straight AT it with no turn
// limit. So the leader's centre is at (spacing, 0) in the segment's own frame,
// every frame, and a round-capped bar to that point lands its cap exactly on
// the leader's own body.
//
// There is deliberately no outline. The reference's is 0.75 units on a body
// 25 across -- under a pixel at any zoom the game plays at -- and drawing it
// per segment is the one thing this scheme cannot do: a segment's outline
// would paint over its leader's fill, and the seam that puts at every joint is
// exactly the bead-chain look the bar is here to avoid.

/// The leech's body colour, and the darker one its beak is drawn in.
constexpr std::uint32_t kLeechBeak = 0x292929u;

/// A segment: the joint between this body and the one it is following.
///
/// A segment whose leader has been killed is promoted to a chain of its own by
/// the server but still draws this bar, so a leech cut in half wears a short
/// nose until it despawns. That is the price of not knowing the chain, and it
/// is the same shape a segment that still HAD a leader would draw.
void paintLeechBody(Canvas& canvas, const MobArtAttributes& attr) {
    ui::setStroke(canvas, attr.baseColor);
    roundStrokes(canvas, attr.radius * 2.0);
    canvas.beginPath();
    canvas.moveTo(0.0f, 0.0f);
    canvas.lineTo(static_cast<float>(attr.radius * kSegmentSpacingPerRadius), 0.0f);
    canvas.stroke();
}

/// The head: the front cap of the tube, and the beak.
///
/// The head paints no bar -- there is nothing in front of it to reach -- and
/// needs none: the first segment's bar ends its cap on this disc.
void paintLeechHead(Canvas& canvas, const MobArtAttributes& attr) {
    // gardn's beak is drawn at a body radius of 12.5 and opens on an angle the
    // reference streams from the server. Nothing on our wire carries it, so it
    // rides the walk phase instead, exactly as the crab's claws do -- which
    // also means it chomps faster once the leech has locked on.
    const double beakAngle = 0.15 * std::sin(attr.animation) + 0.15;
    canvas.save();
    canvas.scale(static_cast<float>(attr.radius / 12.5), static_cast<float>(attr.radius / 12.5));

    // Beak first, body over it: the reference's order, which is what buries the
    // roots of both mandibles inside the head.
    ui::setStroke(canvas, kLeechBeak);
    roundStrokes(canvas, 4.0);
    for (int side = 0; side < 2; ++side) {
        const double sign = side == 0 ? -1.0 : 1.0;
        canvas.save();
        canvas.rotate(static_cast<float>(beakAngle * sign));
        canvas.beginPath();
        canvas.moveTo(0.0f, static_cast<float>(10.0 * sign));
        canvas.quadraticCurveTo(11.0f, static_cast<float>(10.0 * sign), 22.0f,
                                static_cast<float>(5.0 * sign));
        canvas.stroke();
        canvas.restore();
    }

    ui::setFill(canvas, attr.baseColor);
    canvas.beginPath();
    canvas.arc(0.0f, 0.0f, 12.5f, 0.0f, static_cast<float>(kTau), false);
    canvas.fill();

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
    if (name == "crab") return MobArt::Crab;
    if (name == "leech") return MobArt::LeechHead;
    if (name == "leech_body") return MobArt::LeechBody;
    return MobArt::None;
}

void paintMobArt(Canvas& canvas, MobArt art, const MobArtAttributes& attr) {
    if (attr.radius <= 0.0) return;
    switch (art) {
        case MobArt::Rock:      paintRock(canvas, attr); break;
        case MobArt::Cactus:    paintCactus(canvas, attr); break;
        case MobArt::Sandstorm: paintSandstorm(canvas, attr); break;
        case MobArt::Scorpion:  paintScorpion(canvas, attr); break;
        case MobArt::Crab:      paintCrab(canvas, attr); break;
        case MobArt::LeechHead: paintLeechHead(canvas, attr); break;
        case MobArt::LeechBody: paintLeechBody(canvas, attr); break;
        case MobArt::None:      break;
    }
}

} // namespace flix
