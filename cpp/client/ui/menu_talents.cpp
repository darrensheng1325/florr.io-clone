// The talent tree.
//
// Five branches fan out from the flower, one per stat, each a chain of tiers
// on the rarity ladder. A branch walks outward in equal steps and turns a
// little more with each step past the third, which is what keeps ten nodes
// evenly spaced instead of crossing their neighbours. Second Chance is not a
// branch of its own: it forks off Flower Health at the tier it needs, and says
// so by growing out of that node.
//
// The fan is laid out in fixed pixels and is deliberately far larger than the
// card -- most of it starts off the edge. Dragging spins the whole tree about
// the flower, which is how the rest is reached, and the card clips the excess.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/world_view.h"
#include "shared/game/constants.h"

namespace flix {

using namespace flix::ui;

namespace {

/// Step between two tiers of a branch, and the node drawn at each step.
///
/// Absolute design units, never fitted to the card. Scaling the fan down to
/// make it fit would halve every node, every gap and every glyph, and would
/// defeat the point of a tree you spin: what fits is exactly what the
/// reference shows -- about four steps across the card, with a full node's
/// width of air between neighbours. The two move together: a step under twice
/// the node diameter puts adjacent tiers shoulder to shoulder.
constexpr double kBaseStep = 137.0;
constexpr double kNodeRadius = 35.0;
/// The node's rim, and the icon's full extent inside it. The glyph is drawn
/// almost node-wide -- a small mark centred in a large disc reads as a bullet
/// point rather than as the branch's own sign.
constexpr double kNodeRim = 5.0;
constexpr double kIconExtent = kNodeRadius * 0.89;

/// Curvature: nothing for the first three tiers, then a ramp to a third of a
/// right angle per step, easing off on the last one so a branch does not end
/// by folding back over itself.
constexpr double kMaxTurn = kPi * 0.37;

constexpr double kAvatarRadius = 47.0;
/// The pivot sits near the BOTTOM of the card, not in the middle: the fan
/// opens upward into the panel, and the flower anchors it to the lower edge.
constexpr double kPivotAboveBottom = 70.0;

constexpr double kDragThreshold = 5.0;
/// How long the card takes to slide up into place.
constexpr double kOpenSeconds = 0.3;
/// Radians of spin per pixel of horizontal drag. A constant, not a fraction of
/// the card, so the tree turns at the same rate in any window.
constexpr double kRotationPerPixel = 0.008;

/// Card geometry. The panel draws its own chrome rather than borrowing the
/// shared close button and chip: this one's close plate is translucent black
/// and its reset button keeps a radius the shared inlay would flatten.
constexpr double kCardRadius = 6.0;
constexpr double kCloseGlyphPad = 8.0;
constexpr double kResetWidth = 70.0;
constexpr double kResetHeight = 28.0;
/// The reset button and the stat lines sit closer to the card's edge than
/// kMenuPadding puts the other panels' chrome: the tree runs under both, and
/// every unit of margin is a unit of fan hidden behind an opaque button.
constexpr double kChromeInset = 10.0;

/// The TP counter: a disc in the card's own border colour with the number
/// nearly filling it, and the unit spelled out beside it at half that weight.
/// Deliberately larger than the title -- the one number a player opens this
/// panel to read is how many points they have to spend.
constexpr double kBadgeRadius = 21.0;
/// The badge's centre, in from the card's top-left corner on both axes. Its
/// own inset rather than kMenuPadding plus a radius: the disc is bigger than
/// the chips the shared padding was chosen for, and hanging it off the corner
/// by eye is what keeps it clear of both the border and the title.
constexpr double kBadgeInset = 38.0;
constexpr double kBadgeTextSize = 33.0;
constexpr double kBadgeLabelSize = 21.0;
constexpr double kCostTextSize = 19.0;
/// The two lines beside the flower, and the drop from one baseline to the next.
constexpr double kStatTextSize = 13.0;
constexpr double kStatLineGap = 21.0;

/// Connector weights, and the dash the locked ones are drawn with. The dash is
/// measured for ROUND caps, which lengthen every painted segment and shorten
/// every gap by one line width.
constexpr double kLinkWidth = 7.0;
constexpr double kLinkLockedWidth = 6.0;
constexpr float kLinkDash = 8.0f;
constexpr float kLinkDashGap = 11.0f;

constexpr std::uint32_t kFlowerBody = 0xFBE878u;
constexpr std::uint32_t kCostRed = 0xED706Bu;
constexpr std::uint32_t kStatGreen = 0x7EEF6Du;
constexpr std::uint32_t kLockedFill = 0x777777u;
constexpr std::uint32_t kLockedBorder = 0x606060u;
constexpr std::uint32_t kCallToAction = 0xFFE65Du;
constexpr std::uint32_t kStatGrey = 0xE0E0E0u;
constexpr std::uint32_t kStatusGrey = 0xAAAAAAu;
constexpr std::uint32_t kCloseGlyph = 0xE8D8D8u;
constexpr std::uint32_t kResetBorder = 0xA52B24u;
constexpr std::uint32_t kResetFill = 0xCC362Du;
constexpr std::uint32_t kResetHoverFill = 0xE0463Cu;

/// How far a node's border is taken below its own fill. Shallower than the
/// shared darken() default: at a 5-unit rim on a saturated fill, a quarter
/// reads as a black ring rather than as the same colour in shadow.
constexpr double kRimShade = 0.20;
/// How much of its rarity colour the next affordable tier shows through the
/// locked grey. A tint, not the colour itself: the node has not been bought,
/// and painting it as though it had is the one thing this panel must not say.
constexpr double kAvailableTint = 0.30;

/// Linear blend, `amount` of `top` over `bottom`.
constexpr std::uint32_t mixColor(std::uint32_t bottom, std::uint32_t top, double amount) {
    const auto channel = [=](int shift) -> std::uint32_t {
        const double b = static_cast<double>((bottom >> shift) & 0xFF);
        const double t = static_cast<double>((top >> shift) & 0xFF);
        return static_cast<std::uint32_t>(b + (t - b) * amount + 0.5);
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

/// Second Chance's two tiers each spell out what they grant; there is no
/// multiplier to quote.
constexpr std::array<const char*, 2> kSecondChanceEffects = {
    "0.3s invulnerability at 1 HP (60s cd)",
    "1.5s invulnerability at 1 HP (30s cd)",
};

/// Body damage is never surfaced to the client, so the panel quotes the same
/// fixed base the browser falls back to rather than inventing a level curve.
constexpr double kDisplayBodyDamage = 25.0;

/// Every text call in this panel round-joins its outline. That is not the
/// shared default -- panelTitle() and chip() leave the ambient miter join in
/// place -- and at a 4px stroke on 22px bold the difference is visible spikes
/// off the corners of the glyphs, so the panel builds its own styles.
TextStyle panelText(double size, std::uint32_t fill, double strokeWidth) {
    TextStyle style;
    style.size = size;
    style.bold = true;
    style.fill = fill;
    style.strokeWidth = strokeWidth;
    style.roundJoin = true;
    return style;
}

/// The stat lines' own abbreviation: lower-case, and it stops at millions.
/// Deliberately not the shared `abbreviate`, whose K/M/B/T suffixes would
/// print a different string for the same number than the browser does.
std::string abbreviateStat(double value) {
    const double n = std::round(value);
    char buffer[32];
    if (n < 1000.0) {
        std::snprintf(buffer, sizeof buffer, "%.0f", n);
        return buffer;
    }
    const bool millions = n >= 1e6;
    const double scaled = n / (millions ? 1e6 : 1e3);
    // A whole number keeps no decimal at all: "2k", not "2.0k".
    std::snprintf(buffer, sizeof buffer, scaled == std::floor(scaled) ? "%.0f%s" : "%.1f%s", scaled,
                  millions ? "m" : "k");
    return buffer;
}

/// CSS `ease-out` -- cubic-bezier(0, 0, 0.58, 1) -- at time `t`.
///
/// Solved, not approximated. The usual stand-in `1 - (1 - t)^3` runs a fifth
/// of the travel ahead of the real curve at the midpoint, and a fifth of a
/// viewport is most of a card: the slide would arrive early and stop dead
/// rather than easing in.
double easeOut(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    // The bezier is x(u) = u^2(1.74 - 0.74u), y(u) = u^2(3 - 2u). Bisected for
    // u rather than solved with Newton, whose step is undefined at u = 0 where
    // dx/du vanishes; x is monotonic, so sixteen halvings land well inside a
    // pixel of a 720-px slide.
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 16; ++i) {
        const double u = (lo + hi) * 0.5;
        if (u * u * (1.74 - 0.74 * u) < t) lo = u;
        else hi = u;
    }
    const double u = (lo + hi) * 0.5;
    return u * u * (3.0 - 2.0 * u);
}

/// The pupil offset the avatar looks around with, in the flower's own
/// radius-25 space.
///
/// The live smoothed offset of the player's own flower while there is a world
/// to read one from, so the avatar aims where the player does. Outside the
/// world -- and for a component that has never moved off zero -- it falls back
/// to a glance to the right, which is what the browser's `|| 2` does and what
/// the lobby therefore shows.
Vec2 avatarEye(const WorldView& view) {
    const auto found = view.entities().find(view.self().netId);
    if (found == view.entities().end()) return {2.0, 0.0};
    const RemoteEntity& self = found->second;
    return {self.eyeX != 0.0 ? self.eyeX : 2.0, self.eyeY};
}

/// Where a branch starts, in the fan. Five branches, first one straight up.
double branchAngle(int branchIndex, int branchCount) {
    return -kPi * 0.5 + (kTau / branchCount) * branchIndex;
}

/// The one tooltip row that says what a tier is worth.
std::string effectLine(SkillId skill, int tier) {
    if (skill == SkillId::SecondChance) {
        const auto at = static_cast<std::size_t>(tier);
        return at < kSecondChanceEffects.size() ? kSecondChanceEffects[at] : std::string{};
    }
    char buffer[48];
    if (skill == SkillId::Absorbing) {
        std::snprintf(buffer, sizeof buffer, "%.0f%% absorb XP",
                      scaleAt(kAbsorbSkillScale, tier) * 100.0);
        return buffer;
    }
    // The effect curve for every branch, including the three whose own numbers
    // follow the gentler stat curve. That is what the browser quotes, and a
    // tooltip promising 190% where the panel is showing 480% would be worse.
    std::snprintf(buffer, sizeof buffer, "%.0f%% multiplier",
                  scaleAt(kEffectSkillScale, tier) * 100.0);
    return buffer;
}

/// The card's `box-shadow: 0 4px 20px rgba(0, 0, 0, 0.5)`.
///
/// A CSS blur radius of 20 is a Gaussian of sigma 10, so the shadow's alpha at
/// distance d outside its edge is `0.5 * 0.5 * erfc(d / (sigma * sqrt2))`.
/// Painted as disjoint rings at exactly that alpha rather than as stacked
/// fills: rings do not overlap, so each one's alpha is the final answer, and
/// only the fringe is ever rasterised instead of the whole card fifteen times.
void dropShadow(Canvas& canvas, Rect card, double radius) {
    constexpr double kBand = 2.0;
    constexpr int kOuterBands = 12;
    /// Rings inside the shadow's edge, so the strip the +4px offset exposes
    /// below the card is not painted as if it were outside the shadow.
    constexpr int kInnerBands = 3;
    const double falloff = 10.0 * std::sqrt(2.0);

    canvas.save();
    canvas.setLineCap("butt");
    canvas.setLineJoin("round");
    canvas.setLineWidth(static_cast<float>(kBand));
    for (int i = -kInnerBands; i < kOuterBands; ++i) {
        const double d = (i + 0.5) * kBand;
        setStroke(canvas, kInk, 0.25 * std::erfc(d / falloff));
        canvas.beginPath();
        canvas.roundRect(static_cast<float>(card.x - d), static_cast<float>(card.y + 4.0 - d),
                         static_cast<float>(card.w + d * 2.0), static_cast<float>(card.h + d * 2.0),
                         static_cast<float>(radius + d));
        canvas.stroke();
    }
    canvas.restore();
}

/// One blade of the absorption rotor: a comma that starts thin at the hub and
/// widens to a rounded tip, swept a fixed arc around the centre.
///
/// Filled as a ribbon rather than stroked, because the taper IS the shape --
/// a constant-width stroke of the same spiral reads as a snail's shell, which
/// is what this icon used to be.
void rotorBlade(Canvas& canvas, Vec2 at, double radius, double from, double sweep) {
    constexpr int kSteps = 10;
    const double hub = radius * 0.18;
    const double tip = radius * 0.30;

    Vec2 left[kSteps + 1];
    Vec2 right[kSteps + 1];
    for (int i = 0; i <= kSteps; ++i) {
        const double t = static_cast<double>(i) / kSteps;
        const double angle = from + sweep * t;
        // The spine runs from the hub to the rim; the half-width grows with it,
        // so the blade is a wedge with a rounded end rather than a bar.
        const Vec2 spine = at + Vec2::fromAngle(angle, hub + (radius - tip * 0.6 - hub) * t);
        const double half = radius * (0.055 + 0.155 * t);
        const Vec2 normal = Vec2::fromAngle(angle + kPi * 0.5, half);
        left[i] = spine + normal;
        right[i] = spine - normal;
    }

    canvas.beginPath();
    canvas.moveTo(static_cast<float>(left[0].x), static_cast<float>(left[0].y));
    for (int i = 1; i <= kSteps; ++i) {
        canvas.lineTo(static_cast<float>(left[i].x), static_cast<float>(left[i].y));
    }
    for (int i = kSteps; i >= 0; --i) {
        canvas.lineTo(static_cast<float>(right[i].x), static_cast<float>(right[i].y));
    }
    canvas.closePath();
    canvas.fill();

    const Vec2 end = at + Vec2::fromAngle(from + sweep, radius - tip * 0.6);
    canvas.fillCircle(static_cast<float>(end.x), static_cast<float>(end.y),
                      static_cast<float>(radius * 0.21));
}

/// A rounded plus. Two overlapping capsules rather than one traced outline:
/// the corners the reference rounds are the four outer ends, and every one of
/// them is the end of a bar.
void roundedCross(Canvas& canvas, Vec2 at, double extent, double thickness) {
    const double half = extent * 0.5;
    const double bar = thickness * 0.5;
    const auto round = static_cast<float>(thickness * 0.22);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(at.x - bar), static_cast<float>(at.y - half),
                     static_cast<float>(thickness), static_cast<float>(extent), round);
    canvas.fill();
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(at.x - half), static_cast<float>(at.y - bar),
                     static_cast<float>(extent), static_cast<float>(thickness), round);
    canvas.fill();
}

void drawIcon(Canvas& canvas, SkillId id, Vec2 at, double size) {
    // `size` is the glyph's FULL extent, not a radius and not a font size, so
    // every branch's mark occupies the same square of the node however it is
    // built. Everything below is a fraction of it.
    const double half = size * 0.5;

    canvas.save();
    setFill(canvas, kPaper);
    setStroke(canvas, kPaper);
    canvas.setLineCap("round");
    canvas.setLineJoin("round");

    switch (id) {
        case SkillId::PlayerHealth: {   // a medical cross
            roundedCross(canvas, at, size, size * 0.37);
            break;
        }
        case SkillId::Damage: {         // a five-petal flower
            for (int i = 0; i < 5; ++i) {
                const double a = (i / 5.0) * kTau - kPi * 0.5;
                const Vec2 petal = at + Vec2::fromAngle(a, half * 0.58);
                canvas.beginPath();
                // The long axis points OUTWARD -- radiusX is measured along the
                // ellipse's own rotation -- so five petals read as a flower
                // rather than as a clover with its lobes touching.
                canvas.ellipse(static_cast<float>(petal.x), static_cast<float>(petal.y),
                               static_cast<float>(half * 0.42), static_cast<float>(half * 0.29),
                               static_cast<float>(a), 0.0f, static_cast<float>(kTau));
                canvas.fill();
            }
            canvas.fillCircle(static_cast<float>(at.x), static_cast<float>(at.y),
                              static_cast<float>(half * 0.30));
            break;
        }
        case SkillId::PetalHealth: {    // a reload arrow: a thick ring, cut open
            const double weight = size * 0.29;
            const double ring = half - weight * 0.5;
            canvas.setLineWidth(static_cast<float>(weight));
            canvas.setLineCap("butt");
            canvas.beginPath();
            // A narrow slot, not a half-open crescent: the gap says "this comes
            // back round", where a 90-degree mouth would just read as a C. The
            // sweep runs the LONG way -- clockwise from just below the slot all
            // the way round to just above it -- so the arc is the ring and the
            // gap is what is left over, not the other way about.
            canvas.arc(static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(ring),
                       static_cast<float>(0.33), static_cast<float>(kTau - 0.20), false);
            canvas.stroke();
            // The arrow head: a square block filling the corner above the ring's
            // upper end, flush with its outer edge.
            canvas.fillRect(static_cast<float>(at.x + half * 0.19), static_cast<float>(at.y - half),
                            static_cast<float>(half * 0.81), static_cast<float>(half * 0.94));
            canvas.setLineCap("round");
            break;
        }
        case SkillId::Healing: {        // a heart, drawn as an outline
            const double s = half * 0.92;
            canvas.setLineWidth(static_cast<float>(size * 0.11));
            canvas.beginPath();
            canvas.moveTo(static_cast<float>(at.x), static_cast<float>(at.y + s * 0.86));
            canvas.bezierCurveTo(static_cast<float>(at.x + s * 1.35), static_cast<float>(at.y - s * 0.10),
                                 static_cast<float>(at.x + s * 0.62), static_cast<float>(at.y - s * 1.05),
                                 static_cast<float>(at.x), static_cast<float>(at.y - s * 0.24));
            canvas.bezierCurveTo(static_cast<float>(at.x - s * 0.62), static_cast<float>(at.y - s * 1.05),
                                 static_cast<float>(at.x - s * 1.35), static_cast<float>(at.y - s * 0.10),
                                 static_cast<float>(at.x), static_cast<float>(at.y + s * 0.86));
            canvas.stroke();
            break;
        }
        case SkillId::SecondChance: {   // a shield
            const double s = half * 0.92;
            canvas.beginPath();
            canvas.moveTo(static_cast<float>(at.x), static_cast<float>(at.y + s));
            canvas.lineTo(static_cast<float>(at.x - s * 0.78), static_cast<float>(at.y + s * 0.09));
            canvas.lineTo(static_cast<float>(at.x - s * 0.78), static_cast<float>(at.y - s * 0.45));
            canvas.quadraticCurveTo(static_cast<float>(at.x), static_cast<float>(at.y - s * 1.0),
                                    static_cast<float>(at.x + s * 0.78), static_cast<float>(at.y - s * 0.45));
            canvas.lineTo(static_cast<float>(at.x + s * 0.78), static_cast<float>(at.y + s * 0.09));
            canvas.closePath();
            canvas.fill();
            break;
        }
        case SkillId::Absorbing: {      // a rotor drawing inward
            constexpr int kBlades = 8;
            for (int i = 0; i < kBlades; ++i) {
                rotorBlade(canvas, at, half, (i / static_cast<double>(kBlades)) * kTau, 0.62);
            }
            canvas.fillCircle(static_cast<float>(at.x), static_cast<float>(at.y),
                              static_cast<float>(half * 0.15));
            break;
        }
        default: break;
    }
    canvas.restore();
}

/// The flower at the middle of the fan.
///
/// Drawn by scaling into the same 25-unit face space the world renderer uses,
/// so the avatar and the body it upgrades are the same flower rather than two
/// drifting approximations of one. Under that transform the stroke widths
/// scale too, which is where the 2px eye rim and the 3px mouth come from.
void drawAvatar(Canvas& canvas, Vec2 at, double radius, Vec2 eye) {
    const auto eyeX = static_cast<float>(eye.x);
    const auto eyeY = static_cast<float>(eye.y);
    constexpr float kTauF = static_cast<float>(kTau);

    canvas.save();
    canvas.translate(static_cast<float>(at.x), static_cast<float>(at.y));
    canvas.scale(static_cast<float>(radius / 25.0), static_cast<float>(radius / 25.0));

    setFill(canvas, shade(kFlowerBody, 0.8));
    canvas.beginPath();
    canvas.arc(0, 0, 26.5f, 0, kTauF);
    canvas.fill();
    setFill(canvas, kFlowerBody);
    canvas.beginPath();
    canvas.arc(0, 0, 23.5f, 0, kTauF);
    canvas.fill();

    canvas.save();
    setFill(canvas, kInk);
    canvas.beginPath();
    canvas.ellipse(-7, -4.8f, 3.2f, 6.5f, 0, 0, kTauF);
    canvas.moveTo(10.2f, -4.8f);
    canvas.ellipse(7, -4.8f, 3.2f, 6.5f, 0, 0, kTauF);
    canvas.fill();
    // Pupils and rim are drawn clipped to the eyes, so only the inner half of
    // the outline shows and a pupil at the edge is cut off rather than
    // floating outside the eye.
    canvas.clip();
    setFill(canvas, kPaper);
    canvas.beginPath();
    canvas.arc(-7 + eyeX, -4.8f + eyeY, 3, 0, kTauF);
    canvas.arc(7 + eyeX, -4.8f + eyeY, 3, 0, kTauF);
    canvas.fill();
    setStroke(canvas, kInk);
    canvas.setLineWidth(1.0f);
    canvas.beginPath();
    canvas.ellipse(-7, -4.8f, 3.2f, 6.5f, 0, 0, kTauF);
    canvas.stroke();
    canvas.beginPath();
    canvas.ellipse(7, -4.8f, 3.2f, 6.5f, 0, 0, kTauF);
    canvas.stroke();
    canvas.restore();

    setStroke(canvas, 0x222222u);
    canvas.setLineWidth(1.5f);
    canvas.setLineCap("round");
    canvas.beginPath();
    canvas.moveTo(-6, 10);
    canvas.quadraticCurveTo(0, 14.5f, 6, 10);
    canvas.stroke();
    canvas.restore();
}

} // namespace

double TalentsPanel::preferredWidth() { return 600.0; }

void TalentsPanel::reset() {
    laidOut_ = false;
    rotation_ = 0;
    dragging_ = false;
    dragMoved_ = false;
    pressedNode_ = -1;
    openLerp_ = 0;
    confirmingReset_ = false;
}

void TalentsPanel::layout() {
    // Positions are in the tree's own space, with the flower at the origin, so
    // the panel can move without relaying the whole fan out.
    nodes_.clear();

    // Every main branch's node positions, so the sub-branch can fork off one.
    std::vector<std::vector<Vec2>> trunkPoints;
    std::vector<std::vector<double>> trunkAngles;

    constexpr int kBranchCount = kSkillCount - 1;   // Second Chance is a fork
    trunkPoints.resize(kBranchCount);
    trunkAngles.resize(kBranchCount);

    for (int branch = 0; branch < kBranchCount; ++branch) {
        const auto id = static_cast<SkillId>(branch);
        const int tiers = skillTierCount(id);
        Vec2 cursor{0, 0};
        double heading = branchAngle(branch, kBranchCount);

        for (int tier = 0; tier < tiers; ++tier) {
            // Straight out for three steps, then a widening turn. A branch that
            // curved from the first step would cross its neighbours; one that
            // never curved would leave the panel by tier five.
            if (tier >= 3) {
                const double ramp = std::min(1.0, (tier - 3 + 1) / 4.0);
                const double ease = tier == tiers - 1 ? 1.5 : 1.0;
                heading += kMaxTurn * ramp * ease;
            }
            cursor += Vec2::fromAngle(heading, kBaseStep);
            trunkPoints[static_cast<std::size_t>(branch)].push_back(cursor);
            trunkAngles[static_cast<std::size_t>(branch)].push_back(heading);
            nodes_.push_back({id, tier, cursor, {}});
        }
    }

    const auto parent = static_cast<std::size_t>(kSecondChanceParent);
    // The fork hangs off the node that unlocks it -- the rare one -- so the
    // prerequisite is legible as a shape and not only as tooltip text.
    const int forkTier = rarityIndex(kSecondChanceRequirement);
    if (forkTier >= 0 && forkTier < static_cast<int>(trunkPoints[parent].size())) {
        Vec2 cursor = trunkPoints[parent][static_cast<std::size_t>(forkTier)];
        double heading = trunkAngles[parent][static_cast<std::size_t>(forkTier)] - kPi / 3.0;
        for (int tier = 0; tier < skillTierCount(SkillId::SecondChance); ++tier) {
            cursor += Vec2::fromAngle(heading, kBaseStep);
            nodes_.push_back({SkillId::SecondChance, tier, cursor, {}});
        }
    }

    laidOut_ = true;
}

bool TalentsPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Profile& profile = ctx.net.profile();
    const Rect panel = ctx.bounds;
    const SkillSet& skills = profile.skills;
    const int points = profile.talentPoints();

    if (!laidOut_) layout();

    // The card slides up from a full window below, ease-out over 300ms. Only
    // the opening half: the menu system drops a closed panel the frame it is
    // closed, so there is nothing left of this one to slide back out.
    openLerp_ = std::min(1.0, openLerp_ + ctx.dt / kOpenSeconds);
    const double slide = (1.0 - easeOut(openLerp_)) * canvas.height();
    // Everything below is laid out where the card comes to rest, and the whole
    // pass is translated by `slide` at the end. Hit-testing the cursor against
    // resting geometry therefore means lifting it by the same amount, which is
    // what keeps a mid-slide click landing on the button it is drawn under --
    // the browser's card, being a transformed element, hit-tests where it is
    // drawn too.
    const Vec2 mouse{ctx.mouse().x, ctx.mouse().y - slide};
    // A drag, on the other hand, is measured in WINDOW coordinates: while the
    // card is still sliding it travels under a cursor that has not moved at
    // all, and that must not add up to a spin.
    const Vec2 windowMouse = ctx.mouse();

    const Rect closeRect = closeButtonRect(panel);
    const Rect resetRect{panel.right() - kChromeInset - kResetWidth,
                         panel.bottom() - kChromeInset - kResetHeight, kResetWidth, kResetHeight};
    const bool closeHovered = closeRect.contains(mouse);
    const bool resetHovered = resetRect.contains(mouse);
    // Leaving the button disarms it, so the armed state is never painted in
    // the resting colour: the only two fills the browser has are rest and
    // hover, and the cursor is on the button for the whole confirm.
    if (!resetHovered) confirmingReset_ = false;

    // A press anywhere can still become a spin, so a drag is live from the
    // press and only counts once it clears the threshold -- measured in both
    // axes, because a press dragged straight down is a spin attempt and not a
    // click. Once it counts it stays counted: a gesture that wandered 100px
    // and came back is still a drag, and releasing it must not buy whatever it
    // happened to land back on.
    if (dragging_ && !dragMoved_ &&
        distanceSq(windowMouse, dragPress_) >= kDragThreshold * kDragThreshold) {
        dragMoved_ = true;
    }
    if (dragging_ && dragMoved_) {
        rotation_ = rotationAtAnchor_ + (windowMouse.x - dragPress_.x) * kRotationPerPixel;
    }

    const Vec2 centre{panel.x + panel.w * 0.5, panel.bottom() - kPivotAboveBottom};
    const double cosR = std::cos(rotation_);
    const double sinR = std::sin(rotation_);
    for (Node& node : nodes_) {
        node.screen = {centre.x + node.local.x * cosR - node.local.y * sinR,
                       centre.y + node.local.x * sinR + node.local.y * cosR};
    }

    // Exactly one node hovers. Walking the tree backwards picks the one drawn
    // last, which is the one visibly on top wherever two overlap.
    int hovered = -1;
    if (!dragMoved_ && panel.contains(mouse)) {
        for (int i = static_cast<int>(nodes_.size()) - 1; i >= 0; --i) {
            if (distance(mouse, nodes_[static_cast<std::size_t>(i)].screen) <= kNodeRadius) {
                hovered = i;
                break;
            }
        }
    }

    canvas.save();
    canvas.translate(0.0f, static_cast<float>(slide));

    dropShadow(canvas, panel, kCardRadius);
    panelCard(canvas, panel, kTalentsSkin, kMenuBorder, kCardRadius);

    // Everything else is drawn inside the card, tooltip included: a branch
    // that runs past the edge is meant to be spun back into view rather than
    // painted over the world, and the browser gets the same clip for free
    // from its canvas element's own rect.
    canvas.save();
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(panel.x), static_cast<float>(panel.y),
                     static_cast<float>(panel.w), static_cast<float>(panel.h),
                     static_cast<float>(kCardRadius));
    canvas.clip();

    // --- connectors --------------------------------------------------------
    canvas.setLineCap("round");
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        const bool prereqMet =
            node.skill != SkillId::SecondChance || skills.secondChanceUnlocked();
        const bool unlocked = prereqMet && node.tier <= skills.level(node.skill);

        Vec2 from = centre;
        if (node.tier > 0) {
            from = nodes_[i - 1].screen;
        } else if (node.skill == SkillId::SecondChance) {
            const int forkTier = rarityIndex(kSecondChanceRequirement);
            for (const Node& candidate : nodes_) {
                if (candidate.skill == kSecondChanceParent && candidate.tier == forkTier) {
                    from = candidate.screen;
                    break;
                }
            }
        }

        // Both states are the card's own border colour, and only the LINE tells
        // them apart: solid for a tier already bought, dashed for one that is
        // not. Colouring the bought ones green instead would put a second,
        // brighter palette behind a fan whose nodes are already ten colours,
        // and the branch would stop reading as one continuous run.
        setStroke(canvas, kTalentsSkin.border);
        canvas.setLineWidth(static_cast<float>(unlocked ? kLinkWidth : kLinkLockedWidth));
        canvas.setLineDash(unlocked ? std::vector<float>{}
                                    : std::vector<float>{kLinkDash, kLinkDashGap});
        canvas.beginPath();
        canvas.moveTo(static_cast<float>(from.x), static_cast<float>(from.y));
        canvas.lineTo(static_cast<float>(node.screen.x), static_cast<float>(node.screen.y));
        canvas.stroke();
    }
    canvas.setLineDash({});
    canvas.setLineCap("butt");

    // --- nodes -------------------------------------------------------------
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        const bool prereqMet =
            node.skill != SkillId::SecondChance || skills.secondChanceUnlocked();
        const bool unlocked = prereqMet && node.tier <= skills.level(node.skill);
        const int cost = kTierCost[static_cast<std::size_t>(node.tier)];
        const bool available =
            prereqMet && node.tier == skills.level(node.skill) + 1 && points >= cost;

        std::uint32_t fill = kLockedFill;
        if (unlocked) {
            fill = rarityColor(clampRarity(node.tier));
        } else if (available) {
            // A TINT of the tier's own colour over the locked grey, so the one
            // node a click would actually buy names its rarity without ever
            // being mistaken for a node already owned.
            fill = mixColor(kLockedFill, rarityColor(clampRarity(node.tier)), kAvailableTint);
        }
        const std::uint32_t border = fill == kLockedFill ? kLockedBorder : darken(fill, kRimShade);

        disc(canvas, node.screen, kNodeRadius, border, border, 0.0);
        disc(canvas, node.screen, kNodeRadius - kNodeRim, fill, fill, 0.0);
        if (static_cast<int>(i) == hovered && (unlocked || available)) {
            disc(canvas, node.screen, kNodeRadius - kNodeRim, kPaper, kPaper, 0.0, 0.2);
        }
        drawIcon(canvas, node.skill, node.screen, kIconExtent);

        // Only on a tier still to be bought. A price over an owned node is a
        // number with nothing to do: the point of the label is what a click
        // would cost, and every one of them shouting a receipt turns the fan
        // into a field of red digits.
        if (!unlocked) {
            TextStyle price = panelText(kCostTextSize, kCostRed, 4.0);
            price.align = Align::Centre;
            text(canvas, std::to_string(cost), node.screen.x + kNodeRadius * 0.7,
                 node.screen.y - kNodeRadius * 0.85, price);
        }
    }
    drawAvatar(canvas, centre, kAvatarRadius, avatarEye(ctx.net.view()));

    // --- header ------------------------------------------------------------
    TextStyle title = panelText(kMenuTitleSize, kPaper, 4.0);
    title.align = Align::Centre;
    title.baseline = Baseline::Top;
    text(canvas, "Talents", panel.x + panel.w * 0.5, panel.y + kMenuPadding, title);

    // A disc in the card's own border colour rather than the dark inlay the
    // other panels' chips wear: it is a shadow under the number, not a plate
    // the number sits on, and it is drawn only so the digit has something to
    // separate it from the fan running behind the header.
    const Vec2 badge{panel.x + kBadgeInset, panel.y + kBadgeInset};
    disc(canvas, badge, kBadgeRadius, kTalentsSkin.border, kTalentsSkin.border, 0.0);
    // Both sit a pixel below the badge's middle, which is where the browser
    // puts them and what stops the digits reading as high in the plate.
    const double badgeMiddle = badge.y + 1.0;
    TextStyle badgeText = panelText(kBadgeTextSize, kPaper, 5.0);
    badgeText.align = Align::Centre;
    text(canvas, std::to_string(points), badge.x, badgeMiddle, badgeText);
    TextStyle badgeLabel = panelText(kBadgeLabelSize, kPaper, 4.0);
    badgeLabel.align = Align::Left;
    text(canvas, "TP", badge.x + kBadgeRadius + 12.0, badgeMiddle, badgeLabel);

    // A translucent black plate, not the maroon one the other panels wear, and
    // hover moves the glyph rather than the plate under it.
    setFill(canvas, kInk, 0.25);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(closeRect.x), static_cast<float>(closeRect.y),
                     static_cast<float>(closeRect.w), static_cast<float>(closeRect.h), 4.0f);
    canvas.fill();
    setStroke(canvas, closeHovered ? kPaper : kCloseGlyph);
    canvas.setLineWidth(3.0f);
    canvas.setLineCap("round");
    canvas.beginPath();
    canvas.moveTo(static_cast<float>(closeRect.x + kCloseGlyphPad),
                  static_cast<float>(closeRect.y + kCloseGlyphPad));
    canvas.lineTo(static_cast<float>(closeRect.right() - kCloseGlyphPad),
                  static_cast<float>(closeRect.bottom() - kCloseGlyphPad));
    canvas.moveTo(static_cast<float>(closeRect.right() - kCloseGlyphPad),
                  static_cast<float>(closeRect.y + kCloseGlyphPad));
    canvas.lineTo(static_cast<float>(closeRect.x + kCloseGlyphPad),
                  static_cast<float>(closeRect.bottom() - kCloseGlyphPad));
    canvas.stroke();
    canvas.setLineCap("butt");

    // --- stats and reset ---------------------------------------------------
    // The base is the live max health the server sent, the same number the
    // browser reads off the local player. Petals, the arena and the server's
    // own level curve all move it, so a curve evaluated here would quote a
    // flower nobody is playing; only the lobby has no world to read one from,
    // and there the level curve stands in.
    //
    // Scaled by the EFFECT curve rather than the stat curve, which is a
    // deliberate choice in the reference and not a bug to correct here: these
    // two lines advertise what the tree is worth, and the gentler curve reads
    // as no progress at all.
    const SelfState& self = ctx.net.view().self();
    const double baseHealth =
        self.maxHealth > 0.0 ? self.maxHealth : maxHealthForLevel(profile.level);
    const double health = baseHealth * skills.effectScale(SkillId::PlayerHealth);
    const double bodyDamage = kDisplayBodyDamage * skills.effectScale(SkillId::Damage);

    // Anchored to the FLOWER, not to the card's corner: these two lines say
    // what the flower in the middle of the fan is currently worth, and reading
    // them off the far edge of a card whose middle is that flower makes them
    // look like a footer. Label and value are separate runs so the label can
    // carry the stat's colour while the number stays white -- one white number
    // beside a coloured word is legible at 13 units where a whole coloured
    // sentence is not.
    TextStyle statLabel = panelText(kStatTextSize, kStatGreen, 3.0);
    statLabel.baseline = Baseline::Alphabetic;
    TextStyle statValue = panelText(kStatTextSize, kPaper, 3.0);
    statValue.baseline = Baseline::Alphabetic;

    const double statX = centre.x + kAvatarRadius + 12.0;
    const auto statLine = [&](const std::string& label, const std::string& value, double y,
                              std::uint32_t color) {
        statLabel.fill = color;
        text(canvas, label, statX, y, statLabel);
        text(canvas, value, statX + textWidth(canvas, label, kStatTextSize, true), y, statValue);
    };
    statLine("Flower Health: ", abbreviateStat(health), centre.y - 4.0, kStatGreen);
    statLine("Body Damage: ", abbreviateStat(bodyDamage), centre.y + kStatLineGap - 4.0, kStatGrey);

    // Drawn here rather than through chip(): the inner plate keeps a radius of
    // 4 inside the outer 5, where inlaid()'s radius - 2 would flatten it to 3.
    setFill(canvas, kResetBorder);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(resetRect.x), static_cast<float>(resetRect.y),
                     static_cast<float>(resetRect.w), static_cast<float>(resetRect.h), 5.0f);
    canvas.fill();
    setFill(canvas, resetHovered ? kResetHoverFill : kResetFill);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(resetRect.x + 2.0), static_cast<float>(resetRect.y + 2.0),
                     static_cast<float>(resetRect.w - 4.0), static_cast<float>(resetRect.h - 4.0),
                     4.0f);
    canvas.fill();
    TextStyle resetLabel = panelText(14.0, kPaper, 3.0);
    resetLabel.align = Align::Centre;
    text(canvas, "Reset", resetRect.x + resetRect.w * 0.5, resetRect.y + resetRect.h * 0.5 + 1.0,
         resetLabel);

    // --- tooltip -----------------------------------------------------------
    if (hovered >= 0) {
        const Node& node = nodes_[static_cast<std::size_t>(hovered)];
        const auto skillAt = static_cast<std::size_t>(node.skill);
        const Rarity rarity = clampRarity(node.tier);
        const bool prereqMet =
            node.skill != SkillId::SecondChance || skills.secondChanceUnlocked();
        const bool unlocked = prereqMet && node.tier <= skills.level(node.skill);
        const int cost = kTierCost[static_cast<std::size_t>(node.tier)];
        const bool nextTier = node.tier == skills.level(node.skill) + 1;
        const bool available = prereqMet && nextTier && points >= cost;

        std::vector<TooltipLine> lines;
        lines.push_back({kSkillLabels[skillAt], 20.0, kPaper});
        lines.push_back({rarityLabel(rarity), 14.0, rarityColor(rarity)});
        lines.push_back({effectLine(node.skill, node.tier), 12.0, kPaper, 10.0});
        lines.push_back({"Cost: " + std::to_string(cost) + " TP", 12.0, kPaper});

        if (!prereqMet) {
            lines.push_back({std::string("Requires ") + rarityName(kSecondChanceRequirement) + " " +
                                 kSkillLabels[static_cast<std::size_t>(kSecondChanceParent)],
                             12.0, kCostRed});
        } else if (unlocked) {
            lines.push_back({"Unlocked", 12.0, kStatGreen});
        } else if (available) {
            lines.push_back({"Click to unlock", 12.0, kCallToAction});
        } else if (nextTier) {
            lines.push_back({"Need " + std::to_string(cost - points) + " more TP", 12.0, kCostRed});
        } else {
            lines.push_back({"Locked", 12.0, kStatusGrey});
        }

        // Above the node and centred on it, not trailing the cursor: a box
        // that follows the pointer covers the node being read.
        const Vec2 size = measureTooltip(lines);
        const double tx = clamp(node.screen.x - size.x * 0.5, panel.x + 4.0,
                                panel.right() - 4.0 - size.x);
        double ty = node.screen.y - kNodeRadius - size.y - 8.0;
        if (ty < panel.y + 4.0) ty = node.screen.y + kNodeRadius + 8.0;
        paintTooltip(canvas, tx, ty, lines);
    }

    canvas.restore();   // the card's clip
    canvas.restore();   // the slide

    // --- input -------------------------------------------------------------
    if (ctx.pressed() && panel.contains(mouse)) {
        // The chrome acts on the press; only a node click is deferred, because
        // it is the one gesture that might still turn into a spin.
        if (closeHovered) return false;
        if (resetHovered) {
            // Two clicks, because a reset cannot be undone and there is no
            // modal here to raise. The caption never changes.
            if (confirmingReset_) {
                ctx.net.requestResetSkills();
                confirmingReset_ = false;
            } else {
                confirmingReset_ = true;
            }
            return true;
        }
        dragging_ = true;
        dragMoved_ = false;
        dragPress_ = windowMouse;
        // The click belongs to the node the PRESS landed on. Releasing over a
        // different node after the tree has spun under the cursor is not a
        // click on that one.
        pressedNode_ = hovered;
        rotationAtAnchor_ = rotation_;
    }

    if (dragging_ && !ctx.window.mouseDown(MouseButton::Left)) {
        dragging_ = false;
        // A spin is not a click on whatever happened to be under it.
        if (!dragMoved_ && pressedNode_ >= 0) {
            const Node& node = nodes_[static_cast<std::size_t>(pressedNode_)];
            // Every node is asked for, affordable or not and however far up the
            // branch. The server is the one that decides, and its refusal comes
            // back as a notice that says why -- which is the answer the player
            // wanted. Filtering here instead makes the same click do nothing at
            // all and explain nothing.
            ctx.net.requestUpgradeSkill(node.skill, node.tier);
        }
        dragMoved_ = false;
        pressedNode_ = -1;
    }
    return true;
}

} // namespace flix
