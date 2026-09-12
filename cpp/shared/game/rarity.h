#pragma once
// Rarity: the single axis both mobs and items are graded on.
//
// Ten tiers, common through apex. Everything that scales with rarity scales
// off the index, so there is one ordered table and no string comparisons in
// any hot path.

#include <array>
#include <cstdint>
#include <string>

#include "shared/core/types.h"

namespace flix {

enum class Rarity : std::uint8_t {
    Common = 0, Uncommon, Rare, Epic, Legendary,
    Mythic, Ultra, Super, Unique, Apex,
    Count
};

inline constexpr int kRarityCount = static_cast<int>(Rarity::Count);

inline constexpr std::array<const char*, kRarityCount> kRarityNames = {
    "common", "uncommon", "rare", "epic", "legendary",
    "mythic", "ultra", "super", "unique", "apex",
};

/// Display names, used on drop labels and inventory tooltips.
inline constexpr std::array<const char*, kRarityCount> kRarityLabels = {
    "Common", "Uncommon", "Rare", "Epic", "Legendary",
    "Mythic", "Ultra", "Super", "Unique", "Apex",
};

/// Tier colours. These carry meaning at a glance -- a player reads rarity from
/// the border colour long before the label -- so they are shared verbatim by
/// the drop glow, the inventory border, and the mob outline.
inline constexpr std::array<std::uint32_t, kRarityCount> kRarityColors = {
    0x7EEF6Du,  // common     green
    0xFFE65Du,  // uncommon   yellow
    0x4D52E3u,  // rare       blue
    0x861FDEu,  // epic       purple
    0xDE1F1Fu,  // legendary  red
    0x1FDBDEu,  // mythic     cyan
    0xFF2B75u,  // ultra      pink
    0x2BFFA3u,  // super      mint
    0xEEEEEEu,  // unique     white
    0xFF00FFu,  // apex       magenta
};

inline constexpr int rarityIndex(Rarity r) { return static_cast<int>(r); }
inline constexpr const char* rarityName(Rarity r) {
    return rarityIndex(r) < kRarityCount ? kRarityNames[rarityIndex(r)] : "common";
}
inline constexpr const char* rarityLabel(Rarity r) {
    return rarityIndex(r) < kRarityCount ? kRarityLabels[rarityIndex(r)] : "Common";
}
inline constexpr std::uint32_t rarityColor(Rarity r) {
    return rarityIndex(r) < kRarityCount ? kRarityColors[rarityIndex(r)] : 0xFFFFFFu;
}

/// Parses a config/database rarity string. Unknown text reads as Common, so a
/// hand-edited save or an older record degrades instead of failing to load.
Rarity parseRarity(const std::string& name);

inline constexpr Rarity upgradeRarity(Rarity r) {
    const int i = rarityIndex(r);
    return i + 1 < kRarityCount ? static_cast<Rarity>(i + 1) : r;
}

inline constexpr Rarity downgradeRarity(Rarity r) {
    const int i = rarityIndex(r);
    return i > 0 ? static_cast<Rarity>(i - 1) : r;
}

inline constexpr Rarity clampRarity(int index) {
    return static_cast<Rarity>(clamp(index, 0, kRarityCount - 1));
}

/// The tier at which a mob stops being scenery and becomes an EVENT.
///
/// One line, shared by everything that has an opinion about bosses: the
/// spawner announces at it and places such a mob as a live entity wherever it
/// rolled, the mob AI keeps it thinking at full rate however far from anybody
/// it stands, and the bot controller rallies a raid on it. ULTRA IS BELOW IT
/// on purpose -- an ultra is a hard mob you find, a super is a thing the whole
/// server is told about.
inline constexpr bool isBossRarity(Rarity r) {
    return rarityIndex(r) >= rarityIndex(Rarity::Super);
}

// ---------------------------------------------------------------------------
// Stat scaling
// ---------------------------------------------------------------------------

/// Mob health per tier. Superlinear on purpose: each tier is meant to be a
/// wall, not a gentle step, so a common-tier loadout cannot chip down a
/// legendary by patience alone.
inline constexpr std::array<double, kRarityCount> kMobHealthScale = {
    1.0, 3.75, 13.5, 54.0, 324.0, 3159.0, 126830.0, 2374000.0, 1e7, 1e9,
};

/// Mob damage per tier -- a clean 3x ladder, so a tier above you roughly
/// triples what one touch costs.
inline constexpr std::array<double, kRarityCount> kMobDamageScale = {
    1.0, 3.0, 9.0, 27.0, 81.0, 243.0, 729.0, 2187.0, 6561.0, 1968300.0,
};

/// Mob body size per tier. Grows far more slowly than health so a mythic is
/// intimidating without filling the screen.
inline constexpr std::array<double, kRarityCount> kMobSizeScale = {
    1.5, 1.65, 1.95, 2.58, 4.5, 7.5, 10.5, 16.777216, 26.8435456, 42.949673,
};

/// Petal damage and health per tier: a flat 3x ladder matching mob damage, so
/// upgrading a petal one tier keeps pace with the mobs one tier up.
inline double petalStatScale(Rarity r) {
    return std::pow(3.0, rarityIndex(r));
}

/// Healing scales 3x per tier only to mythic, then sqrt(3) per tier.
///
/// Healing that kept tripling would outpace every damage source in the game
/// at the top tiers and make a maxed loadout unkillable; the softer tail keeps
/// high-rarity healing strong without ending combat.
inline double petalHealScale(Rarity r) {
    constexpr int kMythic = static_cast<int>(Rarity::Mythic);
    const int i = rarityIndex(r);
    if (i <= kMythic) return std::pow(3.0, i);
    return std::pow(3.0, kMythic) * std::pow(std::sqrt(3.0), i - kMythic);
}

/// Passive player modifiers (luck, magnetism, extra max health) scale linearly
/// from 1x at common to 4x at unique. Geometric scaling here would make a
/// single high-tier utility petal worth more than a whole loadout.
inline double petalModifierScale(Rarity r) {
    constexpr double kUniqueIndex = static_cast<double>(static_cast<int>(Rarity::Unique));
    return 1.0 + (rarityIndex(r) / kUniqueIndex) * 3.0;
}

// ---------------------------------------------------------------------------
// Crafting and drop rolls
// ---------------------------------------------------------------------------

/// Probability that a craft from `from` to the next tier succeeds. Halves each
/// tier from a base of 64%, so common->uncommon is routine and the top tiers
/// are a genuine gamble.
inline double craftSuccessChance(Rarity from) {
    return 0.64 / std::pow(2.0, rarityIndex(from));
}

/// Chance a drop rolls one tier above the mob that dropped it.
inline double dropUpgradeChance(Rarity mobRarity) {
    if (rarityIndex(mobRarity) >= kRarityCount - 1) return 0.0;
    return craftSuccessChance(mobRarity) / 3.0;
}

/// Chance a drop rolls one tier below the mob that dropped it.
inline double dropDowngradeChance(Rarity mobRarity) {
    const int i = rarityIndex(mobRarity);
    if (i <= 0) return 0.0;
    // The TypeScript formula uses the crafting ladder as a percentage (64,
    // 32, ...), then uses the result as a probability. craftSuccessChance()
    // is stored as a fraction for actual crafting, so restore that scale here.
    return 1.0 /
           (1.0 + craftSuccessChance(static_cast<Rarity>(i - 1)) * 100.0);
}

/// How much of a slow lands on a mob: full at equal rarity, a third per tier
/// the mob is above the slow's source, never more than full. Out-rareing a mob
/// buys reliability, never a slow stronger than the petal was designed for.
inline double stallPower(Rarity source, Rarity target) {
    return std::min(1.0, std::pow(3.0, rarityIndex(source) - rarityIndex(target)));
}

} // namespace flix
