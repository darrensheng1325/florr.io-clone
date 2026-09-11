#pragma once
// Spawn difficulty: the ONE number a patch of ground uses to say how hard it is.
//
// A spawn band on a map carries a `difficulty`, an open-ended number starting at
// zero, and that difficulty is what decides the RARITY of the mobs the band
// grows. It replaced a band naming a tier outright (`spawnType: rare`), which
// could only ever say one of ten things and could not say "mostly ultras with
// the odd super in them" at all.
//
// The mapping is: difficulty -> a continuous TIER VALUE t by piecewise-linear
// interpolation through the anchor table below -> a two-tier blend, floor(t)
// with probability 1-frac(t) and floor(t)+1 with probability frac(t). So a
// fractional t is a MIXTURE of the two tiers either side of it, which is how
// one number says "98% ultra, 2% super".
//
// Nothing here knows about maps, mobs or the ECS: it is arithmetic over a
// rarity, so the server's spawner, the client's minimap and the tests all read
// the same curve rather than each keeping an opinion about what a difficulty
// means.

#include <array>
#include <cmath>
#include <cstddef>

#include "shared/core/types.h"
#include "shared/game/rarity.h"

namespace flix {

/// One point on the difficulty curve: at this difficulty, the ground spawns at
/// this tier value (0 = common, 6 = ultra, 7 = super, 8 = unique, 9 = apex).
struct DifficultyAnchor {
    double difficulty = 0.0;
    double tier = 0.0;
};

/// THE CURVE. Retune the game's whole progression here and nowhere else.
///
/// The four anchors are the design statement, written out so the numbers can be
/// read back off them:
///
///   difficulty   0 -> t = 0.00  ->  100% common
///   difficulty 100 -> t = 6.02  ->   98% ultra, 2% super
///   difficulty 200 -> t = 7.00  ->  100% super
///   difficulty 300 -> t = 8.05  ->   95% unique, 5% apex
///
/// t is linear in difficulty BETWEEN anchors, so difficulty 50 is halfway
/// between common and ultra in tier value (t = 3.01: epic, with a per-cent of
/// legendary in it) rather than halfway along the rarity ladder by count.
///
/// Below the first anchor the curve clamps -- negative difficulty is difficulty
/// zero, and "fully common" means fully common. ABOVE the last anchor it
/// CONTINUES the final segment's slope toward apex and clamps t at 9: a
/// difficulty of 400 must never quietly mean the same as 300, so the tail is a
/// ramp rather than a cap. On these numbers the final slope is 0.0105 tiers per
/// point, so t reaches apex at difficulty 390.48.
inline constexpr std::array<DifficultyAnchor, 4> kDifficultyAnchors = {{
    {0.0, 0.00},
    {100.0, 6.02},
    {200.0, 7.00},
    {300.0, 8.05},
}};

/// The luck a spawn is charged to when nothing owns it.
///
/// One rather than zero: the reference's neutral modifier is 1.0
/// (src/server/shared/playerModifiers.ts:50), and every point above it is what
/// a clover buys.
inline constexpr double kNeutralSpawnLuck = 1.0;

/// How far one point of luck above neutral nudges the tier value UP.
///
/// A hundredth of a tier per point, which is the upward half of the old
/// applyTierDrift (2% + luck*1% chance of one tier up) expressed as an
/// expected gain. It is deliberately zero AT neutral luck, so the anchors above
/// are exactly what an unlucky-but-not-cursed player sees, and there is no
/// downward drift at all: a difficulty-0 band is fully common for everyone, and
/// a clover still buys a percentage point of the tier above wherever they
/// stand.
inline constexpr double kTierValuePerLuckPoint = 0.01;

/// The highest tier value: apex.
inline constexpr double kMaxTierValue = static_cast<double>(kRarityCount - 1);

/// The tier value `difficulty` spawns at. See kDifficultyAnchors.
constexpr double tierValueForDifficulty(double difficulty) {
    // `!(x > y)` rather than `<=` so a NaN difficulty reads as the floor
    // instead of walking off the end of the table.
    if (!(difficulty > kDifficultyAnchors.front().difficulty)) {
        return kDifficultyAnchors.front().tier;
    }
    for (std::size_t i = 1; i < kDifficultyAnchors.size(); ++i) {
        const DifficultyAnchor& from = kDifficultyAnchors[i - 1];
        const DifficultyAnchor& to = kDifficultyAnchors[i];
        if (difficulty <= to.difficulty) {
            const double span = to.difficulty - from.difficulty;
            const double along = span > 0.0 ? (difficulty - from.difficulty) / span : 1.0;
            return from.tier + along * (to.tier - from.tier);
        }
    }
    // Past the last anchor: the final segment's slope, continued, clamped at
    // apex. A bigger number always means at least as dangerous.
    const DifficultyAnchor& from = kDifficultyAnchors[kDifficultyAnchors.size() - 2];
    const DifficultyAnchor& to = kDifficultyAnchors.back();
    const double span = to.difficulty - from.difficulty;
    const double slope = span > 0.0 ? (to.tier - from.tier) / span : 0.0;
    const double tier = to.tier + (difficulty - to.difficulty) * slope;
    return tier < kMaxTierValue ? tier : kMaxTierValue;
}

/// The inverse: the difficulty at which the ground reaches tier value `tier`.
///
/// Used to state a threshold in the language designers use ("no door stands on
/// ground that rolls rare") without hard-coding a difficulty that the anchor
/// table above is free to move.
constexpr double difficultyForTierValue(double tier) {
    if (!(tier > kDifficultyAnchors.front().tier)) return kDifficultyAnchors.front().difficulty;
    for (std::size_t i = 1; i < kDifficultyAnchors.size(); ++i) {
        const DifficultyAnchor& from = kDifficultyAnchors[i - 1];
        const DifficultyAnchor& to = kDifficultyAnchors[i];
        if (tier <= to.tier) {
            const double span = to.tier - from.tier;
            const double along = span > 0.0 ? (tier - from.tier) / span : 1.0;
            return from.difficulty + along * (to.difficulty - from.difficulty);
        }
    }
    const DifficultyAnchor& from = kDifficultyAnchors[kDifficultyAnchors.size() - 2];
    const DifficultyAnchor& to = kDifficultyAnchors.back();
    const double span = to.tier - from.tier;
    const double slope = span > 0.0 ? (to.difficulty - from.difficulty) / span : 0.0;
    return to.difficulty + (tier - to.tier) * slope;
}

/// Ground at or above this difficulty CAN roll rare, which is more than a fresh
/// flower can fight.
///
/// The one threshold that answers "is this ground safe to put a body down on":
/// a pickable door, a bot's birthplace and the beginner-band fallback a
/// door-less map uses all stay strictly below it.
///
/// It is the last tier value whose blend cannot contain a rare -- t = 1, the
/// difficulty at which the ground is pure uncommon -- and NOT t = 2. On a
/// blended scale those are a whole tier apart in the wrong direction: t = 2 is
/// where the ground is ENTIRELY rare, so a band just under it (t = 1.99,
/// difficulty 33.0) is 98.7% rare and would have counted as safe. A rare mob
/// carries 13.5x a common's health and 9x its damage; one of those beside a
/// level-one flower is the thing this constant exists to prevent, and "99% of
/// the time" is not a reprieve.
///
/// Derived from the curve rather than written as 16.6, so retuning the anchors
/// moves it too.
inline constexpr double kDangerousGroundDifficulty =
    difficultyForTierValue(static_cast<double>(rarityIndex(Rarity::Rare)) - 1.0);

/// The two-tier blend a tier value spawns: `upper` with probability
/// `upperChance`, `lower` otherwise.
struct TierMix {
    Rarity lower = Rarity::Common;
    Rarity upper = Rarity::Common;
    double upperChance = 0.0;
};

/// The blend at a tier value. Pure, so the distribution can be asserted on
/// without an Rng.
inline TierMix tierMixForTierValue(double tier) {
    const double clamped = clamp(tier, 0.0, kMaxTierValue);
    const int lower = static_cast<int>(std::floor(clamped));
    if (lower >= kRarityCount - 1) return TierMix{Rarity::Apex, Rarity::Apex, 0.0};
    return TierMix{clampRarity(lower), clampRarity(lower + 1),
                   clamped - static_cast<double>(lower)};
}

/// The blend a difficulty spawns, at neutral luck.
inline TierMix tierMixForDifficulty(double difficulty) {
    return tierMixForTierValue(tierValueForDifficulty(difficulty));
}

/// How much luck adds to a tier value. Never negative. See
/// kTierValuePerLuckPoint.
inline double luckTierDrift(double luck) {
    return std::max(0.0, luck - kNeutralSpawnLuck) * kTierValuePerLuckPoint;
}

/// One roll of a blend.
inline Rarity rollTierMix(const TierMix& mix, Rng& rng) {
    if (!(mix.upperChance > 0.0)) return mix.lower;
    return rng.chance(mix.upperChance) ? mix.upper : mix.lower;
}

/// THE spawn roll: the rarity a mob appearing on difficulty-`difficulty` ground
/// comes out at, for a player of this luck.
inline Rarity rollSpawnRarity(double difficulty, double luck, Rng& rng) {
    return rollTierMix(tierMixForTierValue(tierValueForDifficulty(difficulty) +
                                          luckTierDrift(luck)),
                       rng);
}

/// The tier a difficulty mostly produces -- the more likely half of its blend.
/// What the minimap paints a band in, so a band reads as the tier a player will
/// actually meet in it.
inline Rarity dominantTierForDifficulty(double difficulty) {
    const TierMix mix = tierMixForDifficulty(difficulty);
    return mix.upperChance > 0.5 ? mix.upper : mix.lower;
}

} // namespace flix
