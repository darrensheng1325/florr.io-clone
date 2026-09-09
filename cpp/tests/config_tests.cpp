#include "test.h"

#include "shared/core/json.h"
#include "shared/game/config.h"

#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace flix;

namespace {

// The test binary runs from wherever ctest puts it and the content files are
// not staged next to it, so every path is derived from this source file's own
// location rather than from the working directory.
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
    return {};   // loadFiles() then reports it as an unreadable file
}

// CMake compiles sources by absolute path, so the first candidate normally
// wins. The rest cover a hand-run binary, whose __FILE__ is whatever relative
// path the compiler was given, and the build tree, where the content is staged
// into ./data.
std::string mobsPath() {
    static const std::string path = firstExisting({
        testsDir() + "/../../src/mobs.json",
        "data/mobs.json", "../src/mobs.json", "../../src/mobs.json", "src/mobs.json",
    });
    return path;
}

std::string petalsPath() {
    static const std::string path = firstExisting({
        testsDir() + "/../../src/petals.json",
        "data/petals.json", "../src/petals.json", "../../src/petals.json", "src/petals.json",
    });
    return path;
}

std::string xpPath() {
    static const std::string path = firstExisting({
        testsDir() + "/../data/mob_xp.json",
        "data/mob_xp.json", "../data/mob_xp.json", "cpp/data/mob_xp.json",
    });
    return path;
}

/// The shipped content, loaded once: parsing 220KB of JSON with inline SVG in
/// every entry is not something to repeat per test case.
struct Shipped {
    ContentRegistry registry;
    bool ok = false;
    std::string error;
};

const Shipped& shipped() {
    static const Shipped state = [] {
        Shipped s;
        s.ok = s.registry.loadFiles(mobsPath(), petalsPath(), xpPath(), s.error);
        return s;
    }();
    return state;
}

std::string tempDir() {
    const char* env = std::getenv("TMPDIR");
    std::string base = (env != nullptr && *env != '\0') ? env : "/tmp";
    if (base.back() != '/') base.push_back('/');
    base += "flix_config_tests";
    mkdir(base.c_str(), 0755);   // already exists is fine
    return base;
}

std::string tempPath(const char* name) { return tempDir() + "/" + name; }

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return out.good();
}

bool readText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool warned(const ContentRegistry& r, const std::string& needle) {
    for (const std::string& w : r.warnings()) {
        if (w.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool finite(const MobStats& s) {
    return std::isfinite(s.health) && std::isfinite(s.damage) && std::isfinite(s.radius) &&
           std::isfinite(s.mass) && std::isfinite(s.speed) && std::isfinite(s.xp) &&
           std::isfinite(s.aggroRange) && std::isfinite(s.attackCooldownMillis) &&
           std::isfinite(s.poisonPerSecond) && std::isfinite(s.poisonDurationMillis) &&
           std::isfinite(s.visualScale) && std::isfinite(s.spawnWeight);
}

bool finite(const PetalStats& s) {
    return std::isfinite(s.damage) && std::isfinite(s.health) && std::isfinite(s.reloadMillis) &&
           std::isfinite(s.poisonPerSecond) && std::isfinite(s.poisonDurationMillis) &&
           std::isfinite(s.heal) && std::isfinite(s.healChargeMillis) &&
           std::isfinite(s.passiveHealPerSecond) && std::isfinite(s.knockback) &&
           std::isfinite(s.shield) && std::isfinite(s.slowFactor) &&
           std::isfinite(s.slowDurationMillis) && std::isfinite(s.radius) &&
           std::isfinite(s.damageIntervalMillis) && std::isfinite(s.cameraZoom) &&
           std::isfinite(s.visualScale) &&
           std::isfinite(s.modifiers.maxHealth) && std::isfinite(s.modifiers.speed) &&
           std::isfinite(s.modifiers.range) && std::isfinite(s.modifiers.rotationSpeed) &&
           std::isfinite(s.modifiers.playerRadius) && std::isfinite(s.modifiers.damage) &&
           std::isfinite(s.modifiers.luck) && std::isfinite(s.modifiers.magnetism) &&
           std::isfinite(s.modifiers.aggroRadius) &&
           std::isfinite(s.modifiers.petalAttractionRadius) &&
           std::isfinite(s.modifiers.poisonArmor);
}

// A pair of tiny content files, written to disk, for the failure modes the
// shipped data happens not to contain.
struct Synthetic {
    std::string mobs;
    std::string petals;
    std::string xp;
};

bool loadSynthetic(ContentRegistry& out, const Synthetic& files, std::string& error) {
    const std::string mobs = tempPath("synthetic_mobs.json");
    const std::string petals = tempPath("synthetic_petals.json");
    const std::string xp = tempPath("synthetic_xp.json");
    if (!writeText(mobs, files.mobs) || !writeText(petals, files.petals) ||
        !writeText(xp, files.xp)) {
        error = "could not write the synthetic content";
        return false;
    }
    return out.loadFiles(mobs, petals, xp, error);
}

} // namespace

// ---------------------------------------------------------------------------
// Loading the shipped content
// ---------------------------------------------------------------------------

TEST(shipped_content_loads) {
    const Shipped& s = shipped();
    if (!s.ok) std::printf("    (load error: %s)\n", s.error.c_str());
    CHECK(s.ok);
    CHECK_EQ(s.registry.mobCount(), std::size_t(51));
    // 74 written in petals.json plus one generated egg for each of the 49 mobs
    // that is not a pet and has no hand-written egg -- exactly what the browser
    // build appends to BASE_PETAL_CONFIGS at import time.
    CHECK_EQ(s.registry.petalCount(), std::size_t(123));
    CHECK(s.registry.loaded());
    CHECK(s.registry.contentHash() != 0u);
}

TEST(the_catalogue_is_the_files_own_order_with_the_eggs_appended) {
    const ContentRegistry& r = shipped().registry;
    const std::vector<std::uint16_t>& order = r.petalDisplayOrder();
    CHECK_EQ(order.size(), r.petalCount());

    // Every index exactly once: a catalogue that repeats or drops an entry is
    // a shop with a duplicate card or a missing one.
    std::vector<bool> seen(r.petalCount(), false);
    for (const std::uint16_t index : order) {
        CHECK(index < r.petalCount());
        CHECK(!seen[index]);
        seen[index] = true;
    }

    // petals.json's own first two keys, which is what the browser's
    // Object.keys(PETAL_CONFIG) yields -- NOT the alphabetical order the wire
    // indices are assigned in, which would open the shop on `air`.
    CHECK_EQ(r.petal(order[0]).id, std::string("basic"));
    CHECK_EQ(r.petal(order[1]).id, std::string("rose"));

    // The eggs come last, because that is where the reference appends them.
    CHECK_EQ(r.petal(order[73]).id, std::string("shell"));
    CHECK(r.petal(order[74]).id.size() > 4);
    CHECK_EQ(r.petal(order[74]).id.substr(r.petal(order[74]).id.size() - 4), std::string("_egg"));

    // A generated egg carries the mob's colour in its art and hatches the pet
    // variant where the mob has one.
    const std::uint16_t egg = r.petalIndex("soldier_ant_egg");
    CHECK(egg != kInvalidIndex);
    const PetalConfig& p = r.petal(egg);
    CHECK_EQ(p.name, std::string("Soldier Ant Egg"));
    CHECK_EQ(p.petMobId, std::string("soldier_ant_pet"));
    CHECK(p.image.find(r.mob(r.mobIndex("soldier_ant")).color) != std::string::npos);
    CHECK(!p.isAdminPetal);
    // A pet is not something that lays an egg.
    CHECK_EQ(r.petalIndex("soldier_ant_pet_egg"), kInvalidIndex);
}

TEST(light_and_pollen_gain_petals_with_rarity) {
    const ContentRegistry& r = shipped().registry;
    const std::uint16_t light = r.petalIndex("light");
    const std::uint16_t pollen = r.petalIndex("pollen");
    CHECK(light != kInvalidIndex);
    CHECK(pollen != kInvalidIndex);

    // RARITY_OVERRIDES, not a scaling rule: these are literal per-tier counts.
    CHECK_EQ(r.petalStats(light, Rarity::Common).count, 1);
    CHECK_EQ(r.petalStats(light, Rarity::Epic).count, 3);
    CHECK_EQ(r.petalStats(light, Rarity::Mythic).count, 5);
    CHECK_EQ(r.petalStats(pollen, Rarity::Legendary).count, 3);
    CHECK_EQ(r.petalStats(pollen, Rarity::Ultra).count, 5);
    CHECK_EQ(r.petalStats(pollen, Rarity::Apex).count, 7);
    // Everything else stays flat across the ladder.
    const std::uint16_t basic = r.petalIndex("basic");
    CHECK_EQ(r.petalStats(basic, Rarity::Apex).count, r.petal(basic).count);
}

TEST(every_shipped_entry_parses_into_something_usable) {
    const ContentRegistry& r = shipped().registry;
    for (std::uint16_t i = 0; i < r.mobCount(); ++i) {
        const MobConfig& m = r.mob(i);
        CHECK(!m.id.empty());
        CHECK(!m.name.empty());
        CHECK(!m.image.empty());          // the SVG survives the load verbatim
        CHECK(m.health >= 0.0);
        CHECK(m.size >= 0.0);
        CHECK(m.speed >= 0.0);
        CHECK(m.sectionMask < (1u << kSectionCount));
        CHECK_EQ(r.mobIndex(m.id), i);
    }
    for (std::uint16_t i = 0; i < r.petalCount(); ++i) {
        const PetalConfig& p = r.petal(i);
        CHECK(!p.id.empty());
        CHECK(!p.name.empty());
        CHECK(!p.image.empty());
        CHECK(std::isfinite(p.damage));
        CHECK(p.size > 0.0);
        CHECK(p.count >= 0);
        CHECK_EQ(r.petalIndex(p.id), i);
    }
}

TEST(no_derived_stat_is_ever_nan_or_infinite) {
    // The whole point of sanitising on load: one infinity reaching a position
    // takes a region of the world with it, and it can never be recovered from.
    const ContentRegistry& r = shipped().registry;
    for (std::uint16_t i = 0; i < r.mobCount(); ++i) {
        for (int tier = 0; tier < kRarityCount; ++tier) {
            CHECK(finite(r.mobStats(i, static_cast<Rarity>(tier))));
        }
    }
    for (std::uint16_t i = 0; i < r.petalCount(); ++i) {
        for (int tier = 0; tier < kRarityCount; ++tier) {
            CHECK(finite(r.petalStats(i, static_cast<Rarity>(tier))));
        }
    }
}

TEST(indices_are_sorted_dense_and_stable_across_loads) {
    const ContentRegistry& first = shipped().registry;
    ContentRegistry second;
    std::string error;
    CHECK(second.loadFiles(mobsPath(), petalsPath(), xpPath(), error));

    CHECK_EQ(first.mobCount(), second.mobCount());
    CHECK_EQ(first.petalCount(), second.petalCount());
    CHECK_EQ(first.contentHash(), second.contentHash());

    // Sorted key order is what makes the index safe to put on the wire.
    for (std::uint16_t i = 1; i < first.mobCount(); ++i) {
        CHECK(first.mob(static_cast<std::uint16_t>(i - 1)).id < first.mob(i).id);
    }
    for (std::uint16_t i = 0; i < first.mobCount(); ++i) {
        CHECK_EQ(first.mob(i).id, second.mob(i).id);
        CHECK_EQ(second.mobIndex(first.mob(i).id), i);
    }
    for (std::uint16_t i = 1; i < first.petalCount(); ++i) {
        CHECK(first.petal(static_cast<std::uint16_t>(i - 1)).id < first.petal(i).id);
    }
    for (std::uint16_t i = 0; i < first.petalCount(); ++i) {
        CHECK_EQ(first.petal(i).id, second.petal(i).id);
    }
}

TEST(unknown_ids_and_out_of_range_indices_are_safe) {
    const ContentRegistry& r = shipped().registry;
    CHECK_EQ(r.mobIndex("no_such_mob"), kInvalidIndex);
    CHECK_EQ(r.petalIndex("no_such_petal"), kInvalidIndex);
    CHECK_EQ(r.mobIndex(""), kInvalidIndex);
    CHECK_EQ(r.petalIndex(""), kInvalidIndex);

    // These indices arrive from the wire; a corrupt one must cost a
    // wrong-looking entity, not the process.
    CHECK_EQ(r.mob(kInvalidIndex).id, std::string("<unknown>"));
    CHECK_EQ(r.petal(kInvalidIndex).id, std::string("<unknown>"));
    CHECK(finite(r.mobStats(kInvalidIndex, Rarity::Apex)));
    CHECK(finite(r.petalStats(kInvalidIndex, Rarity::Apex)));
    CHECK(!r.mobStats(kInvalidIndex, Rarity::Common).spawnable());
    CHECK(!r.petal(kInvalidIndex).breakable);
}

// ---------------------------------------------------------------------------
// Rarity scaling
// ---------------------------------------------------------------------------

TEST(mob_stats_scale_across_rarities) {
    const ContentRegistry& r = shipped().registry;
    const std::uint16_t bee = r.mobIndex("bee");
    CHECK(bee != kInvalidIndex);

    const MobStats common = r.mobStats(bee, Rarity::Common);
    CHECK_NEAR(common.health, 35.0, 1e-9);                 // config health, 1x
    CHECK_NEAR(common.damage, 50.0, 1e-9);
    CHECK_NEAR(common.radius, 1.0 * kMobSizeScale[0] * kMobBaseRadius, 1e-9);
    CHECK_NEAR(common.mass, kMobSizeScale[0] * kMobSizeScale[0], 1e-9);
    CHECK_NEAR(common.aggroRange, 100.0, 1e-9);
    CHECK(common.spawnable());

    for (int tier = 0; tier < kRarityCount; ++tier) {
        const MobStats s = r.mobStats(bee, static_cast<Rarity>(tier));
        const std::size_t t = static_cast<std::size_t>(tier);
        CHECK_NEAR(s.health, 35.0 * kMobHealthScale[t], std::fabs(s.health) * 1e-9);
        CHECK_NEAR(s.damage, 50.0 * kMobDamageScale[t], std::fabs(s.damage) * 1e-9);
        CHECK_NEAR(s.radius, kMobSizeScale[t] * kMobBaseRadius, 1e-9);
        // Speed and aggro range are deliberately flat: a rare bee is tougher,
        // not faster.
        CHECK_NEAR(s.speed, common.speed, 1e-9);
        CHECK_NEAR(s.aggroRange, 100.0, 1e-9);
    }
}

TEST(mob_speed_is_converted_to_units_per_second) {
    const ContentRegistry& r = shipped().registry;
    // TypeScript advances a mob by `speed * ENEMY_SPEED_MULTIPLIER` on each of
    // its 30 ticks a second, and that multiplier is 2 -- so one config point is
    // 60 world units a second, and a bee's authored 0.5 is 30.
    const MobStats bee = r.mobStats(r.mobIndex("bee"), Rarity::Common);
    CHECK_NEAR(bee.speed, 30.0, 1e-9);
    // Bees are one of the reference's player-speed chasers, but that override
    // replaces the PURSUIT step only: an unprovoked bee still drifts at its
    // own 30, which is why the flower's speed lives on a separate field.
    CHECK(bee.playerSpeedChaser);
    CHECK_NEAR(bee.chaseSpeed, kPlayerMaxSpeed, 1e-9);

    const MobStats hole = r.mobStats(r.mobIndex("ant_hole"), Rarity::Common);
    CHECK_NEAR(hole.speed, 0.0, 1e-9);
    CHECK(!hole.playerSpeedChaser);
    CHECK_NEAR(hole.chaseSpeed, 0.0, 1e-9);
}

TEST(petal_stats_scale_across_rarities) {
    const ContentRegistry& r = shipped().registry;
    const std::uint16_t basic = r.petalIndex("basic");
    CHECK(basic != kInvalidIndex);

    for (int tier = 0; tier < kRarityCount; ++tier) {
        const Rarity rarity = static_cast<Rarity>(tier);
        const PetalStats s = r.petalStats(basic, rarity);
        CHECK_NEAR(s.damage, 10.0 * petalStatScale(rarity), std::fabs(s.damage) * 1e-9);
        CHECK_NEAR(s.health, 10.0 * petalStatScale(rarity), std::fabs(s.health) * 1e-9);
        CHECK_NEAR(s.reloadMillis, 1200.0, 1e-9);   // reload is flat across tiers
        CHECK(s.breakable);
    }

    // Healing rides the softer curve, or a maxed loadout is unkillable.
    const std::uint16_t rose = r.petalIndex("rose");
    CHECK(rose != kInvalidIndex);
    CHECK_NEAR(r.petalStats(rose, Rarity::Common).heal, 10.0, 1e-9);
    CHECK_NEAR(r.petalStats(rose, Rarity::Apex).heal, 10.0 * petalHealScale(Rarity::Apex), 1e-6);
    CHECK(r.petalStats(rose, Rarity::Apex).heal < 10.0 * petalStatScale(Rarity::Apex));
    CHECK(r.petal(rose).defendOnly);
}

TEST(player_modifiers_scale_by_kind) {
    const ContentRegistry& r = shipped().registry;

    // Additive: clover's luck scales straight up, 1x at common to 4x at unique.
    const std::uint16_t clover = r.petalIndex("clover");
    CHECK_NEAR(r.petalStats(clover, Rarity::Common).modifiers.luck, 0.08, 1e-12);
    CHECK_NEAR(r.petalStats(clover, Rarity::Unique).modifiers.luck, 1.5, 1e-12);

    // Multiplicative: only the bonus above 1.0 scales, so +10% becomes +40%.
    const std::uint16_t cactus = r.petalIndex("cactus");
    CHECK_NEAR(r.petalStats(cactus, Rarity::Common).modifiers.maxHealth, 1.1, 1e-12);
    CHECK_NEAR(r.petalStats(cactus, Rarity::Unique).modifiers.maxHealth, 1.4, 1e-12);

    // A non-positive multiplier is a sign flip, not a bonus: yin_yang reverses
    // the ring at every tier and never spins it four times as fast backwards.
    const std::uint16_t yinYang = r.petalIndex("yin_yang");
    CHECK_NEAR(r.petalStats(yinYang, Rarity::Common).modifiers.rotationSpeed, -1.0, 1e-12);
    CHECK_NEAR(r.petalStats(yinYang, Rarity::Apex).modifiers.rotationSpeed, -1.0, 1e-12);

    // A petal with no playerModifiers block is neutral in every field.
    const PetalStats basic = r.petalStats(r.petalIndex("basic"), Rarity::Apex);
    CHECK(!basic.modifiers.any);
    CHECK_NEAR(basic.modifiers.maxHealth, 1.0, 1e-12);
    CHECK_NEAR(basic.modifiers.luck, 0.0, 1e-12);
}

TEST(special_petal_geometry_and_timers_follow_rarity_overrides) {
    const ContentRegistry& r = shipped().registry;
    const std::uint16_t web = r.petalIndex("web");
    CHECK_NEAR(r.petalStats(web, Rarity::Common).webRadius, 90.0, 1e-12);
    CHECK_NEAR(r.petalStats(web, Rarity::Unique).webRadius, 198.0, 1e-12);

    const std::uint16_t sponge = r.petalIndex("sponge");
    CHECK_NEAR(r.petalStats(sponge, Rarity::Common).spongeDamageDurationMillis, 1000.0, 1e-12);
    CHECK_NEAR(r.petalStats(sponge, Rarity::Mythic).spongeDamageDurationMillis, 3500.0, 1e-12);

    const std::uint16_t lentil = r.petalIndex("lentil");
    CHECK_NEAR(r.petalStats(lentil, Rarity::Uncommon).attractionForce, 2889.0, 1e-12);
    CHECK_NEAR(r.petalStats(lentil, Rarity::Apex).modifiers.petalAttractionRadius, 100.0, 1e-12);
}

TEST(min_rarity_makes_a_mob_unspawnable_below_its_tier) {
    const ContentRegistry& r = shipped().registry;
    const std::uint16_t centipede = r.mobIndex("evil_centipede");
    CHECK(centipede != kInvalidIndex);
    CHECK_EQ(rarityIndex(r.mob(centipede).minRarity), rarityIndex(Rarity::Rare));
    CHECK(r.mob(centipede).sectionMask != 0);   // it does belong to a section

    CHECK(!r.mobStats(centipede, Rarity::Common).spawnable());
    CHECK(!r.mobStats(centipede, Rarity::Uncommon).spawnable());
    CHECK(r.mobStats(centipede, Rarity::Rare).spawnable());
    CHECK(r.mobStats(centipede, Rarity::Apex).spawnable());
    CHECK_EQ(r.mobStats(centipede, Rarity::Common).sectionMask, std::uint16_t(0));
    CHECK_EQ(r.mobStats(centipede, Rarity::Rare).sectionMask, r.mob(centipede).sectionMask);

    // An empty `section` list is its own kind of unspawnable and is unaffected.
    const std::uint16_t spawner = r.mobIndex("item_spawner");
    CHECK(!r.mobStats(spawner, Rarity::Apex).spawnable());
}

TEST(mob_xp_derives_apex_and_falls_back_to_one) {
    const ContentRegistry& r = shipped().registry;
    const MobConfig& bee = r.mob(r.mobIndex("bee"));
    CHECK_NEAR(bee.xp[static_cast<std::size_t>(Rarity::Common)], 1.0, 1e-9);
    CHECK_NEAR(bee.xp[static_cast<std::size_t>(Rarity::Unique)], 6800000.0, 1e-6);
    // The tables stop at unique; apex takes the same 3x step the stat tables do.
    CHECK_NEAR(bee.xp[static_cast<std::size_t>(Rarity::Apex)], 6800000.0 * 3.0, 1e-6);
    CHECK_NEAR(r.mobStats(r.mobIndex("bee"), Rarity::Apex).xp, 20400000.0, 1e-6);

    // A mob the table never mentions awards 1 at every tier rather than 0.
    const MobConfig& sun = r.mob(r.mobIndex("sun"));
    for (int tier = 0; tier < kRarityCount; ++tier) {
        CHECK_NEAR(sun.xp[static_cast<std::size_t>(tier)], 1.0, 1e-9);
    }
    CHECK(warned(r, "mob 'sun': has no XP table"));
}

TEST(cross_references_resolve_to_indices) {
    const ContentRegistry& r = shipped().registry;

    const MobConfig& hornet = r.mob(r.mobIndex("hornet"));
    CHECK(hornet.projectile.present);
    CHECK_EQ(hornet.projectile.ammoPetalIndex, r.petalIndex("hornet_missile"));
    CHECK_EQ(rarityIndex(hornet.projectile.ammoRarity), rarityIndex(Rarity::Uncommon));

    const MobConfig& queen = r.mob(r.mobIndex("queen_ant"));
    CHECK(queen.periodicSpawn.present);
    CHECK_EQ(queen.periodicSpawn.mobIndex, r.mobIndex("soldier_ant"));
    CHECK_EQ(queen.periodicSpawn.rarityOffset, -1);
    CHECK_NEAR(queen.periodicSpawn.intervalMillis, 2000.0, 1e-9);

    const MobConfig& hole = r.mob(r.mobIndex("ant_hole"));
    CHECK_EQ(hole.initialSpawns.size(), std::size_t(6));
    CHECK_EQ(hole.spawnWaves.size(), std::size_t(9));
    for (const std::uint16_t child : hole.initialSpawns) CHECK(child != kInvalidIndex);
    for (const auto& wave : hole.spawnWaves) {
        CHECK(!wave.empty());
        for (const std::uint16_t child : wave) CHECK(child < r.mobCount());
    }

    const MobConfig& glitchFlower = r.mob(r.mobIndex("glitch_flower"));
    CHECK(glitchFlower.petalRing.present);
    CHECK_EQ(glitchFlower.petalRing.petalIndex, r.petalIndex("glitch"));
    CHECK_EQ(glitchFlower.petalRing.count, 5);

    const PetalConfig& egg = r.petal(r.petalIndex("egg"));
    CHECK_EQ(egg.petMobIndex, r.mobIndex("bee"));
    CHECK_EQ(rarityIndex(egg.petMobRarity), rarityIndex(Rarity::Common));
}

TEST(enums_and_colours_are_parsed_not_stored_as_text) {
    const ContentRegistry& r = shipped().registry;
    CHECK(r.mob(r.mobIndex("bee")).ai == AiKind::Passive);
    CHECK(r.mob(r.mobIndex("hornet")).ai == AiKind::Hostile);
    CHECK(r.mob(r.mobIndex("moth")).ai == AiKind::Neutral);
    CHECK(r.mob(r.mobIndex("sandstorm")).ai == AiKind::Sandstorm);

    CHECK_EQ(r.mob(r.mobIndex("bee")).colorRgba, 0xEBE834FFu);
    CHECK_EQ(r.mob(r.mobIndex("sun")).lightColorRgba, 0xFFFF00FFu);

    // The bubble petal is genuinely a transparent fill and means it; alpha has
    // to survive or it draws as an opaque white blob.
    CHECK_EQ(r.petal(r.petalIndex("bubble")).colorRgba, 0xFFFFFF00u);

    CHECK_EQ(r.petal(r.petalIndex("cutter")).equipFlags, std::uint8_t(EquipCutter));
    // Both bits, and the second one is DERIVED from the id: petals.json says
    // plain "Cutter" because the browser build's loader throws on a name its
    // frozen enum lacks. Losing this leaves the cyan blade painted black.
    CHECK_EQ(r.petal(r.petalIndex("lightning_cutter")).equipFlags,
             std::uint8_t(EquipCutter | EquipLightningCutter));
    // The body-damage grant rides the petal damage ladder: 3x a tier, so it
    // matches what a basic petal of the same rarity hits for.
    CHECK_NEAR(r.petalStats(r.petalIndex("cutter"), Rarity::Common).bodyDamage, 10.0, 1e-9);
    CHECK_NEAR(r.petalStats(r.petalIndex("cutter"), Rarity::Unique).bodyDamage, 65610.0, 1e-9);
    CHECK_EQ(r.petal(r.petalIndex("third_eye")).equipFlags, std::uint8_t(EquipThirdEye));
    CHECK_EQ(r.petal(r.petalIndex("observer")).equipFlags, std::uint8_t(EquipObserver));
    CHECK_EQ(r.petal(r.petalIndex("antennae")).equipFlags, std::uint8_t(EquipAntennae));
    CHECK_EQ(r.petal(r.petalIndex("sparkle")).equipFlags, std::uint8_t(EquipTest1));
    CHECK_EQ(r.petal(r.petalIndex("basic")).equipFlags, std::uint8_t(EquipNone));
}

// ---------------------------------------------------------------------------
// The dirty data the shipped files actually contain
// ---------------------------------------------------------------------------

TEST(dirty_absurd_visual_offset_is_caught) {
    const ContentRegistry& r = shipped().registry;
    for (const char* id : {"third_eye", "antennae", "observer"}) {
        const PetalConfig& p = r.petal(r.petalIndex(id));
        CHECK(p.hidden);
        CHECK_NEAR(p.visualOffsetY, 0.0, 1e-12);
        CHECK(std::isfinite(p.visualOffsetY));
    }
    CHECK(warned(r, "petal 'third_eye': visualOffsetY -1e+100"));
}

TEST(dirty_null_damage_and_health_are_caught) {
    const ContentRegistry& r = shipped().registry;
    const PetalConfig& infinity = r.petal(r.petalIndex("infinity"));
    CHECK_NEAR(infinity.damage, 0.0, 1e-12);
    CHECK(std::isfinite(infinity.damage));
    // A null health is "no health pool", not "zero hit points": zeroing it
    // would break these petals on the first tick they exist.
    CHECK(!infinity.breakable);
    CHECK_NEAR(infinity.health, 0.0, 1e-12);
    CHECK(warned(r, "petal 'infinity': damage is null"));
    CHECK(warned(r, "petal 'infinity': health is null"));

    CHECK(!r.petal(r.petalIndex("cutter")).breakable);
    CHECK(!r.petalStats(r.petalIndex("gas"), Rarity::Mythic).breakable);
    CHECK(r.petal(r.petalIndex("basic")).breakable);
}

TEST(finite_negative_damage_keeps_typescript_semantics) {
    const ContentRegistry& r = shipped().registry;
    const std::uint16_t glitch = r.petalIndex("glitch");
    CHECK_NEAR(r.petal(glitch).damage, -1.0, 1e-12);
    for (int tier = 0; tier < kRarityCount; ++tier) {
        CHECK(r.petalStats(glitch, static_cast<Rarity>(tier)).damage < 0.0);
    }
}

TEST(dirty_enormous_cooldown_is_caught) {
    const ContentRegistry& r = shipped().registry;
    const std::uint16_t yggdrasil = r.petalIndex("yggdrasil");
    // 2048000ms is 34 minutes: a dead slot, not a slow one.
    CHECK(r.petal(yggdrasil).cooldownMillis <= 60000.0);
    // The derived TypeScript rarity table deliberately overrides the clamped
    // authored value with its 512s / 2^tier sequence.
    CHECK_NEAR(r.petalStats(yggdrasil, Rarity::Common).reloadMillis, 512000.0, 1e-9);
    CHECK(warned(r, "petal 'yggdrasil': cooldown is 2.048e+06"));
}

TEST(dirty_negative_speed_is_caught) {
    const ContentRegistry& r = shipped().registry;
    const std::uint16_t moth = r.mobIndex("moth");
    CHECK(r.mob(moth).speed > 0.0);
    CHECK_NEAR(r.mob(moth).speed, 2.4, 1e-12);
    CHECK(r.mobStats(moth, Rarity::Common).speed > 0.0);
    CHECK(warned(r, "mob 'moth': speed is -2.4"));
}

TEST(finite_enormous_damage_keeps_typescript_semantics) {
    const ContentRegistry& r = shipped().registry;
    CHECK_NEAR(r.petal(r.petalIndex("sparkle")).damage, 9999999999.0, 1e-9);
}

TEST(finite_zero_placeholder_mobs_keep_typescript_semantics) {
    const ContentRegistry& r = shipped().registry;
    for (const char* id : {"bush", "leafbug", "mantis"}) {
        const MobConfig& m = r.mob(r.mobIndex(id));
        CHECK_NEAR(m.size, 0.0, 1e-12);
        CHECK_NEAR(m.health, 0.0, 1e-12);
        CHECK_NEAR(m.visualScale, 0.0, 1e-12);
        CHECK_NEAR(r.mobStats(r.mobIndex(id), Rarity::Common).radius, 0.0, 1e-12);
    }
}

TEST(a_spread_angle_is_read_as_radians_however_odd_the_number_looks) {
    const ContentRegistry& r = shipped().registry;
    const ProjectileSpec& spec = r.petal(r.petalIndex("flower")).projectile;
    CHECK(spec.present);
    CHECK_EQ(spec.count, 5);
    // `flower` ships 72, which LOOKS like degrees and is not. The reference
    // reads the field as radians (`projectileConfig.spreadAngle || 0.2`) and
    // hands it straight to `(i - (count - 1) / 2) * spreadAngle`, so its five
    // shots wrap the circle several times over rather than closing a tidy
    // five-point star. Converting it here would aim the volley at other mobs,
    // so the number is taken exactly as written and nothing is warned about.
    CHECK_NEAR(spec.spreadAngle, 72.0, 1e-9);
    CHECK(!warned(r, "projectile spreadAngle"));

    // An unstated spread is the reference's 0.2 default, not a zero that would
    // stack a whole volley on one bearing.
    const ProjectileSpec& missile = r.petal(r.petalIndex("missile")).projectile;
    CHECK(missile.present);
    CHECK_NEAR(missile.spreadAngle, 0.0, 1e-9);   // authored 0, kept at 0
    CHECK_NEAR(r.petal(r.petalIndex("peas")).projectile.spreadAngle, 1.5708, 1e-9);
}

TEST(poison_without_a_duration_remains_inert_like_typescript) {
    const ContentRegistry& r = shipped().registry;
    const PetalConfig& gas = r.petal(r.petalIndex("gas"));
    CHECK(gas.poisonPerSecond > 0.0);
    CHECK_NEAR(gas.poisonDurationMillis, 0.0, 1e-12);

    // Poison is stated per millisecond in the JSON and per second here.
    const PetalConfig& iris = r.petal(r.petalIndex("iris"));
    CHECK_NEAR(iris.poisonPerSecond, 9.0, 1e-9);
    CHECK_NEAR(r.petalStats(r.petalIndex("iris"), Rarity::Rare).poisonPerSecond,
               9.0 * petalStatScale(Rarity::Rare), 1e-9);
}

// ---------------------------------------------------------------------------
// Failure modes the shipped data does not contain
// ---------------------------------------------------------------------------

TEST(synthetic_dirty_values_are_sanitised) {
    Synthetic files;
    files.mobs = R"JSON({
      "wreck": {
        "name": "Wreck", "description": "d", "color": "#112233", "image": "<svg/>",
        "damage": -25, "health": -50, "size": -3, "speed": -7, "cooldown": 99999999,
        "range": -100, "visual_scale": 0, "ai_type": "telepathic",
        "section": [0, 99, "two"], "spawn_weight": -1,
        "poison": -1, "poisonDuration": -5,
        "initial_spawns": ["ghost", "wreck"],
        "spawn_waves": [["wreck"], "not a wave"],
        "projectile": {"count": 0, "distance": 1e400, "speed": -5, "petalType": "nope"},
        "periodic_spawn": {"mobType": "ghost", "intervalMs": 0, "maxAlive": 3}
      },
      "huge": {
        "name": "Huge", "description": "d", "color": "not a colour", "image": "<svg/>",
        "damage": 1e400, "health": 1e400, "size": 1e400, "speed": 1e400,
        "cooldown": 1e400, "range": 1e400, "ai_type": "hostile", "section": [1],
        "random_size": [4, 1]
      },
      "not_an_object": 7
    })JSON";
    files.petals = R"JSON({
      "junk": {
        "name": "Junk", "description": "d", "color": "#000000", "image": "<svg/>",
        "damage": -5, "health": -9, "size": 0, "cooldown": 1e400, "count": -3,
        "visualOffsetY": 1e400, "slowFactor": 4, "cameraZoom": 0, "visual_scale": 1e400,
        "playerModifiers": {"luck": 1e400, "maxHealth": -2, "telekinesis": 3},
        "radiation": {"radius": -1, "intervalMs": 0},
        "petMobType": "ghost", "petMobRarity": "legendaryish"
      },
      "sane": {
        "name": "Sane", "description": "d", "color": "#ffffff", "image": "<svg/>",
        "damage": 4, "health": 4, "size": 1, "cooldown": 1000, "count": 1,
        "visual_scale": 2.5
      }
    })JSON";
    files.xp = R"JSON({"wreck": {"common": -3, "unique": 100}})JSON";

    ContentRegistry r;
    std::string error;
    CHECK(loadSynthetic(r, files, error));
    if (!error.empty()) std::printf("    (error: %s)\n", error.c_str());

    // The non-object entry is skipped and does not leave a hole in the indices.
    CHECK_EQ(r.mobCount(), std::size_t(2));
    CHECK_EQ(r.mobIndex("not_an_object"), kInvalidIndex);
    CHECK_EQ(r.mobIndex("huge"), std::uint16_t(0));
    CHECK_EQ(r.mobIndex("wreck"), std::uint16_t(1));
    CHECK(warned(r, "not an object; skipped"));

    const MobConfig& wreck = r.mob(r.mobIndex("wreck"));
    CHECK_NEAR(wreck.damage, 0.0, 1e-12);
    CHECK_NEAR(wreck.health, 0.0, 1e-12);
    CHECK_NEAR(wreck.size, 0.0, 1e-12);
    CHECK_NEAR(wreck.speed, 7.0, 1e-12);          // magnitude kept, sign dropped
    CHECK(wreck.cooldownMillis <= 600000.0);
    CHECK_NEAR(wreck.range, 0.0, 1e-12);
    CHECK(wreck.ai == AiKind::Neutral);           // an unknown behaviour
    CHECK_EQ(wreck.sectionMask, std::uint16_t(1));// 99 and "two" dropped, 0 kept
    CHECK_NEAR(wreck.poisonPerSecond, 0.0, 1e-12);
    CHECK(warned(r, "ai_type 'telepathic'"));
    CHECK(warned(r, "section 99"));

    // Dangling references are dropped rather than kept as a bad index.
    CHECK_EQ(wreck.initialSpawns.size(), std::size_t(1));
    CHECK_EQ(wreck.initialSpawns[0], r.mobIndex("wreck"));
    CHECK_EQ(wreck.spawnWaves.size(), std::size_t(1));
    CHECK(!wreck.periodicSpawn.present);
    CHECK_EQ(wreck.projectile.ammoPetalIndex, kInvalidIndex);
    CHECK(wreck.periodicSpawn.intervalMillis > 0.0);   // never a spawn every tick
    CHECK(warned(r, "'ghost' is not defined"));

    const MobConfig& huge = r.mob(r.mobIndex("huge"));
    CHECK(std::isfinite(huge.damage));
    CHECK(std::isfinite(huge.health));
    CHECK(std::isfinite(huge.size));
    CHECK(std::isfinite(huge.speed));
    CHECK_EQ(huge.colorRgba, kOpaqueWhite);
    // A reversed random_size pair is a range all the same.
    CHECK(huge.randomSizeMin <= huge.randomSizeMax);
    CHECK(warned(r, "not a colour"));

    const PetalConfig& junk = r.petal(r.petalIndex("junk"));
    CHECK_NEAR(junk.damage, -5.0, 1e-12);
    CHECK_NEAR(junk.health, 0.0, 1e-12);
    CHECK(junk.breakable);            // it stated a number, it just stated a bad one
    CHECK(junk.size > 0.0);
    CHECK(junk.cooldownMillis <= 60000.0);
    CHECK_EQ(junk.count, 0);
    CHECK(junk.hidden);               // an infinite offset is not an offset
    CHECK(junk.slowFactor <= 1.0);    // a slow may not speed its victim up
    CHECK(junk.cameraZoom > 0.0);
    CHECK(std::isfinite(junk.visualScale));   // art scale, but still a number

    // visual_scale is read from petals.json exactly as it is from mobs.json,
    // and it moves the artwork only: `size`, and every reach derived from it,
    // is what the petal was authored with.
    const PetalConfig& sane = r.petal(r.petalIndex("sane"));
    CHECK_NEAR(sane.visualScale, 2.5, 1e-12);
    CHECK_NEAR(sane.size, 1.0, 1e-12);
    CHECK_NEAR(r.petalStats(r.petalIndex("sane"), Rarity::Rare).visualScale, 2.5, 1e-12);
    CHECK_NEAR(r.petalStats(r.petalIndex("sane"), Rarity::Rare).radius, 10.0, 1e-12);
    CHECK_NEAR(r.petalStats(r.petalIndex("sane"), Rarity::Rare).size, 1.0, 1e-12);
    CHECK(std::isfinite(junk.modifiers.luck));
    CHECK(!junk.radiation.present);
    CHECK_EQ(junk.petMobIndex, kInvalidIndex);
    CHECK_EQ(rarityIndex(junk.petMobRarity), rarityIndex(Rarity::Common));
    CHECK(warned(r, "unknown rarity 'legendaryish'"));
    CHECK(warned(r, "unknown key 'telekinesis'"));

    for (std::uint16_t i = 0; i < r.mobCount(); ++i) {
        for (int tier = 0; tier < kRarityCount; ++tier) {
            CHECK(finite(r.mobStats(i, static_cast<Rarity>(tier))));
        }
    }
    for (std::uint16_t i = 0; i < r.petalCount(); ++i) {
        for (int tier = 0; tier < kRarityCount; ++tier) {
            CHECK(finite(r.petalStats(i, static_cast<Rarity>(tier))));
        }
    }

    // A negative XP entry is refused; the tier keeps its fallback.
    CHECK_NEAR(wreck.xp[static_cast<std::size_t>(Rarity::Common)], 1.0, 1e-12);
    CHECK_NEAR(wreck.xp[static_cast<std::size_t>(Rarity::Apex)], 300.0, 1e-9);
}

TEST(a_missing_file_fails_without_losing_the_loaded_content) {
    ContentRegistry r;
    std::string error;
    CHECK(r.loadFiles(mobsPath(), petalsPath(), xpPath(), error));
    const std::size_t before = r.mobCount();
    const std::uint32_t hashBefore = r.contentHash();

    // A failed reload must leave a running server with the content it had.
    CHECK(!r.load("/definitely/not/a/directory", error));
    CHECK(!error.empty());
    CHECK_EQ(r.mobCount(), before);
    CHECK_EQ(r.contentHash(), hashBefore);
    CHECK(r.mobIndex("bee") != kInvalidIndex);

    // A readable mobs file and a missing petals file fails just as cleanly.
    CHECK(!r.loadFiles(mobsPath(), tempPath("no_such_petals.json"), xpPath(), error));
    CHECK(!error.empty());
    CHECK_EQ(r.mobCount(), before);
}

TEST(malformed_or_empty_content_fails_cleanly) {
    ContentRegistry r;
    std::string error;

    Synthetic broken;
    broken.mobs = "{ \"bee\": ";           // truncated
    broken.petals = "{}";
    broken.xp = "{}";
    CHECK(!loadSynthetic(r, broken, error));
    CHECK(!error.empty());

    Synthetic wrongShape;
    wrongShape.mobs = "[1, 2, 3]";          // an array, not a table of mobs
    wrongShape.petals = "{}";
    wrongShape.xp = "{}";
    error.clear();
    CHECK(!loadSynthetic(r, wrongShape, error));
    CHECK(!error.empty());

    Synthetic empty;
    empty.mobs = "{}";
    empty.petals = "{}";
    empty.xp = "{}";
    error.clear();
    CHECK(!loadSynthetic(r, empty, error));
    CHECK(!error.empty());
    CHECK(!r.loaded());

    // A broken XP file is survivable: it costs balance, not structure.
    Synthetic badXp;
    badXp.mobs = R"JSON({"a": {"name": "A", "health": 3, "damage": 1, "size": 1,
        "speed": 1, "cooldown": 1, "range": 1, "description": "", "color": "#fff",
        "image": "<svg/>", "ai_type": "passive", "section": [0]}})JSON";
    badXp.petals = R"JSON({"p": {"name": "P", "health": 3, "damage": 1, "size": 1,
        "cooldown": 1, "count": 1, "description": "", "color": "#fff", "image": "<svg/>"}})JSON";
    badXp.xp = "{ not json";
    error.clear();
    CHECK(loadSynthetic(r, badXp, error));
    CHECK_EQ(r.mobCount(), std::size_t(1));
    CHECK_NEAR(r.mobStats(0, Rarity::Apex).xp, 1.0, 1e-12);
    CHECK_EQ(r.mob(0).colorRgba, 0xFFFFFFFFu);   // "#fff" is the short form
}

TEST(content_hash_follows_the_bytes) {
    ContentRegistry a;
    ContentRegistry b;
    std::string error;
    CHECK(a.loadFiles(mobsPath(), petalsPath(), xpPath(), error));
    CHECK(b.loadFiles(mobsPath(), petalsPath(), xpPath(), error));
    CHECK_EQ(a.contentHash(), b.contentHash());

    // One byte of difference in either file has to be visible at the
    // handshake, or the client shows stats the server is not simulating.
    std::string petals;
    CHECK(readText(petalsPath(), petals));
    const std::string edited = tempPath("edited_petals.json");
    CHECK(writeText(edited, petals + "\n"));
    ContentRegistry c;
    CHECK(c.loadFiles(mobsPath(), edited, xpPath(), error));
    CHECK(c.contentHash() != a.contentHash());
    CHECK_EQ(c.petalCount(), a.petalCount());   // same content, different bytes
}

TEST(global_content_registry_loads_from_one_directory) {
    // load() takes a directory; the build stages all three files into one.
    const std::string dir = tempDir();
    std::string mobs, petals, xp;
    CHECK(readText(mobsPath(), mobs));
    CHECK(readText(petalsPath(), petals));
    CHECK(readText(xpPath(), xp));
    CHECK(writeText(dir + "/mobs.json", mobs));
    CHECK(writeText(dir + "/petals.json", petals));
    CHECK(writeText(dir + "/mob_xp.json", xp));

    std::string error;
    CHECK(loadContent(dir, error));
    CHECK(error.empty());
    CHECK_EQ(content().mobCount(), std::size_t(51));
    CHECK_EQ(content().petalCount(), std::size_t(123));
    // Same three files in the same order as the shipped registry, so the two
    // must agree on every index and on the hash.
    CHECK_EQ(content().contentHash(), shipped().registry.contentHash());
    CHECK_EQ(content().mobIndex("bee"), shipped().registry.mobIndex("bee"));

    // A failed reload of the global registry keeps what it already had.
    CHECK(!loadContent(dir + "/nowhere", error));
    CHECK(!error.empty());
    CHECK_EQ(content().mobCount(), std::size_t(51));

    // A trailing slash names the same directory.
    CHECK(loadContent(dir + "/", error));
    CHECK_EQ(content().mobCount(), std::size_t(51));
}
