#include "test.h"

#include "server/systems/loot.h"
#include "server/systems/spawning.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace flix;

namespace {

// The test binary runs from wherever ctest puts it, so every content path is
// derived from this source file's own location rather than from the working
// directory. Same trick as config_tests.cpp.
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

const ContentRegistry& shipped() {
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

std::string tempPath(const char* name) {
    const char* env = std::getenv("TMPDIR");
    std::string base = (env != nullptr && *env != '\0') ? env : "/tmp";
    if (base.back() != '/') base.push_back('/');
    base += "flix_spawning_tests";
    mkdir(base.c_str(), 0755);
    return base + "/" + name;
}

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return out.good();
}

/// A registry over four invented mobs, so the weighted roll can be measured
/// against numbers a test chose rather than against whatever the shipped data
/// happens to say this week.
const ContentRegistry& synthetic() {
    static const ContentRegistry registry = [] {
        ContentRegistry r;
        const std::string mobs = tempPath("mobs.json");
        const std::string petals = tempPath("petals.json");
        writeText(mobs, R"({
            "alpha": {"name":"Alpha","health":10,"damage":1,"size":1,"speed":1,
                      "groups":{"meadow":1}},
            "beta":  {"name":"Beta","health":10,"damage":1,"size":1,"speed":1,
                      "groups":{"meadow":3}},
            "gamma": {"name":"Gamma","health":10,"damage":1,"size":1,"speed":1,
                      "groups":{"meadow":6}},
            "delta": {"name":"Delta","health":10,"damage":1,"size":1,"speed":1,
                      "groups":{"dunes":100}},
            "ghost": {"name":"Ghost","health":10,"damage":1,"size":1,"speed":1,
                      "groups":{"meadow":0}}
        })");
        writeText(petals, R"({
            "basic": {"name":"Basic","damage":5,"health":5,"size":1,"cooldown":1000,"count":1}
        })");
        std::string error;
        r.loadFiles(mobs, petals, "", error);
        return r;
    }();
    return registry;
}

/// The staged maps, as the server would load them. Defined with the band
/// tests further down; declared here because the fill test wants them too.
const WorldMaps& shippedMaps();

/// The AUTHORED fixture map -- bands of several difficulties and a region,
/// written by this file. Defined with the band tests for the same reason. See
/// its definition for why the shipped map cannot stand in for it any more.
const MapData& authoredMap();
const WorldMaps& authoredMaps();

/// Fills `out` with a one-map overworld carrying nothing but `bands`: one
/// `difficulty` band per rectangle, each naming `mobs`.
///
/// Every test below that wants ambient mobs at all needs one of these. A Sim
/// with no map has no bands, and a world with no bands grows NOTHING -- that
/// is the invariant this file pins further down, and the reason a population
/// test can no longer just stand a viewer on open ground and wait.
void makeBandedWorld(WorldMaps& out, const std::vector<Rect>& bands, const std::string& mobs,
                     double difficulty = 0.0);

/// The population a band of these bounds is stocked to: the same figure
/// SpawnSystem derives from kTargetMobDensity, so a test can say "its target"
/// rather than write a number down.
int bandTarget(const Rect& bounds);

/// The centre of the first spawn band on `map`.
///
/// Where a test that wants the SHIPPED map to grow something has to stand its
/// viewer: only a band somebody can see is stocked, and the author moves the
/// bands as the map is balanced, so the spot is read out of the file rather
/// than written down here.
Vec2 firstBandCentre(const MapData& map);

/// A world plus everything the spawner needs to be driven one tick at a time.
struct Sim {
    World world;
    CommandBuffer commands{world};
    Terrain terrain;
    SpawnSystem spawner;
    Rng rng{0xC0FFEEu};
    double now = 0;

    /// The tests speak in bare overworld coordinates; the system wants to know
    /// which realm each one is in.
    static std::vector<RealmPoint> overworld(const std::vector<Vec2>& players) {
        std::vector<RealmPoint> points;
        points.reserve(players.size());
        for (const Vec2 p : players) points.push_back({p, Realm::Overworld});
        return points;
    }

    void tick(const std::vector<Vec2>& players) {
        spawner.run(world, terrain, shipped(), overworld(players), rng, now, net::kTickSeconds,
                    commands);
        commands.flush();
        now += net::kTickMillis;
    }

    /// Advances the clock without simulating the gap. Used to reach a timeout
    /// without paying for the thousand ticks in between.
    void jump(double millis, const std::vector<Vec2>& players) {
        now += millis;
        spawner.run(world, terrain, shipped(), overworld(players), rng, now, 0.0, commands);
        commands.flush();
    }

    int mobCount() {
        Query<MobTag> mobs{world};
        return static_cast<int>(mobs.count());
    }

    int mobsWithin(Vec2 centre, double radius) {
        Query<MobTag, Transform> mobs{world};
        int n = 0;
        const double r2 = radius * radius;
        mobs.each([&](Entity, MobTag&, Transform& t) {
            if (distanceSq(t.position, centre) <= r2) ++n;
        });
        return n;
    }

    /// Every record the bands are holding in `realm`.
    ///
    /// The world is not the whole population any more. A band stocks itself to
    /// its full target wherever it is and whoever is looking, and only the
    /// part of it somebody's viewport reaches is an entity; a test that counts
    /// MobTags and stops there is measuring the viewport rather than the map.
    std::vector<SpawnSystem::LatentSite> latent(Realm realm = Realm::Overworld) {
        std::vector<SpawnSystem::LatentSite> out;
        spawner.latentSites(realm, out);
        return out;
    }

    /// The whole population standing inside `bounds`, awake or not: what a
    /// band's target is stated against.
    int populationIn(const Rect& bounds) {
        int n = mobsIn(bounds);
        for (const SpawnSystem::LatentSite& site : latent()) {
            if (bounds.contains(site.position)) ++n;
        }
        return n;
    }

    /// Mobs standing inside `bounds`, whatever put them there.
    int mobsIn(const Rect& bounds) {
        Query<MobTag, Transform> mobs{world};
        int n = 0;
        mobs.each([&](Entity, MobTag&, Transform& t) { n += bounds.contains(t.position) ? 1 : 0; });
        return n;
    }

    /// Mobs inside `bounds` that a band fill PLACED: the escorts and body
    /// segments their parents brought with them are left out, because they are
    /// not spawns the band asked for and are not counted against its target.
    int bandPlacedIn(const Rect& bounds) {
        Query<MobTag, Transform> mobs{world};
        int n = 0;
        mobs.each([&](Entity e, MobTag&, Transform& t) {
            if (bounds.contains(t.position) && !isChildOfAPlacedMob(e)) ++n;
        });
        return n;
    }

    /// True when this mob is the CHILD of something a band placed rather than
    /// a spawn of its own: a nest's escort or a wave, leashed to its parent,
    /// or a centipede's body segment trailing its head. Both are laid out on
    /// their parent's geometry and legitimately reach over a band's edge, so
    /// both are exempt from "every ambient mob stands inside a band".
    bool isChildOfAPlacedMob(Entity e) {
        if (world.has<HoleTether>(e)) return true;
        const BodySegment* link = world.tryGet<BodySegment>(e);
        return link != nullptr && !link->head;
    }
};

/// The centre of section 4, comfortably away from every section border.
const Vec2 kCentre{30000.0, 30000.0};

void rebuildGrid(World& world, SpatialGrid& grid) {
    grid.clear();
    Query<Transform, Body> bodies{world};
    bodies.each([&](Entity e, Transform& t, Body& b) { grid.insert(e, Realm::Overworld, t.position, b.radius); });
}

Entity makePlayer(World& world, Vec2 position, double magnetism = 0.0, std::uint32_t netId = 0) {
    const Entity e = world.create();
    world.add<PlayerTag>(e);
    world.add<Transform>(e, Transform{position, 0.0});
    world.add<Body>(e, Body{kPlayerBaseRadius, 1.0});
    world.add<Health>(e, Health{100.0, 100.0, 0.0, 0.0});
    PlayerModifiers mods;
    mods.magnetism = magnetism;
    world.add<PlayerModifiers>(e, mods);
    if (netId != 0) world.add<NetId>(e, NetId{netId});
    return e;
}

/// A mob standing at `at`, already dead, with `contributors` credited.
Entity makeCorpse(World& world, std::uint16_t mobIndex, Rarity rarity, Vec2 at, Entity killer,
                  const std::vector<Entity>& contributors) {
    const Entity e = world.create();
    world.add<MobTag>(e);
    world.add<Transform>(e, Transform{at, 0.0});
    world.add<MobType>(e, MobType{mobIndex, rarity, 1.0});
    Bounty bounty;
    for (const Entity c : contributors) bounty.credit(c, 10.0);
    world.add<Bounty>(e, std::move(bounty));
    world.add<Dead>(e, Dead{killer});
    return e;
}

std::vector<Entity> liveDrops(World& world) {
    Query<DropTag, DropItem> drops{world};
    return drops.collect();
}

} // namespace

// ---------------------------------------------------------------------------
// Content sanity -- everything below is meaningless without it
// ---------------------------------------------------------------------------

TEST(spawning_tests_have_content) {
    CHECK(shipped().loaded());
    CHECK(synthetic().loaded());
    CHECK_EQ(synthetic().mobCount(), std::size_t(5));
}

// ---------------------------------------------------------------------------
// Type and tier selection
// ---------------------------------------------------------------------------

TEST(weighted_choice_matches_the_configured_weights) {
    const ContentRegistry& content = synthetic();
    SpawnSystem spawner;
    Rng rng(4242);

    const std::uint16_t alpha = content.mobIndex("alpha");
    const std::uint16_t beta = content.mobIndex("beta");
    const std::uint16_t gamma = content.mobIndex("gamma");
    const std::uint16_t ghost = content.mobIndex("ghost");

    constexpr int kSamples = 60000;
    int counts[3] = {0, 0, 0};
    int ghostCount = 0;
    for (int i = 0; i < kSamples; ++i) {
        const std::uint16_t picked =
            spawner.chooseGroupMob(content, content.mobGroupIndex("meadow"), Rarity::Common, rng);
        if (picked == alpha) ++counts[0];
        else if (picked == beta) ++counts[1];
        else if (picked == gamma) ++counts[2];
        else if (picked == ghost) ++ghostCount;
    }

    // 1 : 3 : 6 out of a total of 10.
    CHECK_NEAR(counts[0] / double(kSamples), 0.10, 0.01);
    CHECK_NEAR(counts[1] / double(kSamples), 0.30, 0.015);
    CHECK_NEAR(counts[2] / double(kSamples), 0.60, 0.015);
    // A zero spawn_weight is how the data says "never rolled" -- that is what
    // keeps centipede body segments from spawning as loose mobs.
    CHECK_EQ(ghostCount, 0);
}

TEST(a_group_with_nothing_in_it_yields_no_mob_type) {
    const ContentRegistry& content = synthetic();
    SpawnSystem spawner;
    Rng rng(1);
    // The two groups the fixture defines are the only two that exist: a group
    // is the union of the names the mobs use, so there is no third to be empty.
    CHECK_EQ(content.mobGroupCount(), std::size_t(2));
    CHECK_EQ(spawner.chooseGroupMob(content, content.mobGroupIndex("dunes"), Rarity::Common, rng),
             content.mobIndex("delta"));
    // A name nothing claims is answered, not asserted on: it comes off a map
    // file, where a typo must cost a band its mobs rather than the process.
    CHECK_EQ(content.mobGroupIndex("sewers"), kInvalidIndex);
    CHECK_EQ(spawner.chooseGroupMob(content, kInvalidIndex, Rarity::Common, rng), kInvalidIndex);
    CHECK_EQ(spawner.chooseGroupMob(content, 9999, Rarity::Common, rng), kInvalidIndex);
}

// ---------------------------------------------------------------------------
// The difficulty curve
// ---------------------------------------------------------------------------
//
// THE INVARIANT these replaced: a spawn's rarity is a function of the GROUND'S
// DIFFICULTY, not of a global "natural spread" table and not of a tier a band
// names outright. A difficulty maps to a continuous tier value through the
// anchor table in shared/game/difficulty.h, and the spawn is a blend of the two
// tiers either side of it. The four anchors below are the design statement the
// user wrote, so they are asserted as stated -- over a big sample, because a
// blend is a distribution and not a value.

namespace {

/// The measured share of each rarity over `samples` rolls of one difficulty at
/// neutral luck.
std::array<double, kRarityCount> rolledSpread(double difficulty, int samples,
                                              std::uint64_t seed = 4242) {
    Rng rng(seed);
    std::array<int, kRarityCount> counts{};
    for (int i = 0; i < samples; ++i) {
        ++counts[static_cast<std::size_t>(
            rarityIndex(rollSpawnRarity(difficulty, kNeutralSpawnLuck, rng)))];
    }
    std::array<double, kRarityCount> shares{};
    for (std::size_t i = 0; i < counts.size(); ++i) {
        shares[i] = static_cast<double>(counts[i]) / static_cast<double>(samples);
    }
    return shares;
}

/// The expected tier index of a difficulty, straight off the pure blend.
double expectedTier(double difficulty) {
    const TierMix mix = tierMixForDifficulty(difficulty);
    return rarityIndex(mix.lower) * (1.0 - mix.upperChance) +
           rarityIndex(mix.upper) * mix.upperChance;
}

} // namespace

TEST(the_difficulty_curve_hits_the_four_authored_anchors) {
    // Difficulty 0 is FULLY common: no drift up, no drift down, nothing else in
    // it at all. "Fully common means fully common" is why there is no downward
    // drift in the curve and why the luck nudge is zero at neutral luck.
    const auto zero = rolledSpread(0.0, 200000);
    CHECK_NEAR(zero[rarityIndex(Rarity::Common)], 1.0, 1e-12);

    // 100 -> 98% ultra, 2% super.
    const auto hundred = rolledSpread(100.0, 200000);
    CHECK_NEAR(hundred[rarityIndex(Rarity::Ultra)], 0.98, 0.005);
    CHECK_NEAR(hundred[rarityIndex(Rarity::Super)], 0.02, 0.005);

    // 200 -> nothing but supers.
    const auto twoHundred = rolledSpread(200.0, 200000);
    CHECK_NEAR(twoHundred[rarityIndex(Rarity::Super)], 1.0, 1e-12);

    // 300 -> 95% unique, 5% apex.
    const auto threeHundred = rolledSpread(300.0, 200000);
    CHECK_NEAR(threeHundred[rarityIndex(Rarity::Unique)], 0.95, 0.005);
    CHECK_NEAR(threeHundred[rarityIndex(Rarity::Apex)], 0.05, 0.005);

    // And the pure blend agrees with the sample, so everything below can be
    // asserted without paying for two hundred thousand rolls.
    CHECK_NEAR(tierValueForDifficulty(0.0), 0.00, 1e-12);
    CHECK_NEAR(tierValueForDifficulty(100.0), 6.02, 1e-12);
    CHECK_NEAR(tierValueForDifficulty(200.0), 7.00, 1e-12);
    CHECK_NEAR(tierValueForDifficulty(300.0), 8.05, 1e-12);
}

TEST(safe_ground_is_ground_that_cannot_roll_a_rare) {
    // kDangerousGroundDifficulty is the line a door, a bot's birthplace and the
    // beginner-band fallback all stay under, so what matters is what the blend
    // JUST UNDER it can contain -- not what the blend AT it mostly is.
    //
    // The bug this pins: stating the line at t = 2 ("rare") reads as "the
    // ground is rare here", but on a blended curve t = 1.99 is already 98.7%
    // rare, and every one of those difficulties counted as SAFE. The line
    // belongs at the top of pure uncommon instead.
    const TierMix justUnder = tierMixForDifficulty(std::nextafter(
        kDangerousGroundDifficulty, 0.0));
    CHECK(rarityIndex(justUnder.lower) < rarityIndex(Rarity::Rare));
    CHECK(rarityIndex(justUnder.upper) < rarityIndex(Rarity::Rare));
    CHECK_NEAR(tierValueForDifficulty(kDangerousGroundDifficulty), 1.0, 1e-9);

    // And a hair above it, a rare is on the table. If it were not, the line
    // would simply be too low and safe ground would be needlessly small.
    const TierMix justOver = tierMixForDifficulty(std::nextafter(
        kDangerousGroundDifficulty, 1000.0));
    CHECK(rarityIndex(justOver.upper) >= rarityIndex(Rarity::Rare));

    // Sampled, so the claim is about what actually spawns and not only about
    // the arithmetic: nothing rare comes out of the last safe difficulty.
    const auto spread = rolledSpread(std::nextafter(kDangerousGroundDifficulty, 0.0), 50000);
    for (int tier = rarityIndex(Rarity::Rare); tier < kRarityCount; ++tier) {
        CHECK_NEAR(spread[static_cast<std::size_t>(tier)], 0.0, 1e-12);
    }
}

TEST(a_higher_difficulty_never_spawns_a_lower_tier) {
    // Monotonicity, over the whole authored range and past the end of it. A
    // curve that dipped anywhere would make a stretch of a map's progression
    // run backwards, which is the one thing a difficulty scale must not do.
    double previous = -1.0;
    for (double difficulty = -200.0; difficulty <= 600.0; difficulty += 0.25) {
        const double expected = expectedTier(difficulty);
        if (expected < previous - 1e-12) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "expected tier fell at difficulty " +
                                         std::to_string(difficulty));
            break;
        }
        previous = expected;
        // And the two tiers of a blend are always adjacent, so a difficulty
        // never produces something two tiers from what it says.
        const TierMix mix = tierMixForDifficulty(difficulty);
        CHECK(rarityIndex(mix.upper) - rarityIndex(mix.lower) <= 1);
    }
}

TEST(difficulty_clamps_below_zero_and_ramps_past_three_hundred) {
    // Below zero clamps: there is nothing gentler than fully common.
    for (const double difficulty : {-1.0, -50.0, -10000.0}) {
        const TierMix mix = tierMixForDifficulty(difficulty);
        CHECK(mix.lower == Rarity::Common);
        CHECK_EQ(mix.upperChance, 0.0);
    }

    // Above 300 CONTINUES the 200-to-300 slope instead of behaving like 300: a
    // bigger number must always mean at least as dangerous, and eventually
    // means apex. On the shipped anchors the slope is 0.0105 tiers per point,
    // so apex is reached at difficulty 390.48 and never before it.
    CHECK(tierValueForDifficulty(350.0) > tierValueForDifficulty(300.0));
    CHECK_NEAR(tierValueForDifficulty(350.0), 8.05 + 50.0 * 0.0105, 1e-12);
    CHECK(tierValueForDifficulty(389.0) < kMaxTierValue);
    CHECK_NEAR(tierValueForDifficulty(400.0), kMaxTierValue, 1e-12);
    CHECK_NEAR(tierValueForDifficulty(1e6), kMaxTierValue, 1e-12);

    // At the top the blend is apex alone rather than an apex/nothing pair.
    const TierMix top = tierMixForDifficulty(500.0);
    CHECK(top.lower == Rarity::Apex);
    CHECK(top.upper == Rarity::Apex);
    CHECK_EQ(top.upperChance, 0.0);
    Rng rng(7);
    for (int i = 0; i < 1000; ++i) {
        CHECK(rollSpawnRarity(500.0, kNeutralSpawnLuck, rng) == Rarity::Apex);
    }
}

TEST(luck_shifts_the_curve_upward_and_never_down) {
    // Neutral luck is exactly the anchors -- asserted above -- so the only
    // thing left is that luck moves UP and only up. A clover buys a hundredth
    // of a tier per point, which at difficulty zero is the chance of an
    // uncommon rather than a common.
    CHECK_EQ(luckTierDrift(kNeutralSpawnLuck), 0.0);
    CHECK_EQ(luckTierDrift(0.0), 0.0);            // cursed is not punished
    CHECK_EQ(luckTierDrift(-100.0), 0.0);
    CHECK(luckTierDrift(kNeutralSpawnLuck + 1.0) > 0.0);

    Rng rng(11);
    int uncommons = 0;
    const int samples = 200000;
    for (int i = 0; i < samples; ++i) {
        const Rarity r = rollSpawnRarity(0.0, kNeutralSpawnLuck + 1.0, rng);
        CHECK(rarityIndex(r) <= rarityIndex(Rarity::Uncommon));
        if (r == Rarity::Uncommon) ++uncommons;
    }
    // One point of luck, one hundredth of a tier.
    CHECK_NEAR(static_cast<double>(uncommons) / samples, 0.01, 0.003);

    // A luck-free roll at the same difficulty stays fully common, so the shift
    // above is luck and not noise.
    const auto neutral = rolledSpread(0.0, 20000);
    CHECK_NEAR(neutral[rarityIndex(Rarity::Common)], 1.0, 1e-12);
}

TEST(min_rarity_still_floors_a_named_mob_whatever_the_ground_says) {
    const ContentRegistry& content = shipped();
    const MobConfig& evil = content.mob(content.mobIndex("evil_centipede"));
    CHECK_EQ(evil.minRarity, Rarity::Rare);

    // Difficulty zero is fully common ground, and an evil centipede still
    // cannot exist below rare: the floor is a property of the MOB, which is why
    // min_rarity survived the removal of the rarity-zone scheme.
    Rng rng(77);
    for (int i = 0; i < 5000; ++i) {
        CHECK(SpawnSystem::rollRarity(evil, 0.0, kNeutralSpawnLuck, rng) == Rarity::Rare);
    }
    // A mob with no floor takes the ground's answer unchanged.
    const MobConfig& bee = content.mob(content.mobIndex("bee"));
    for (int i = 0; i < 5000; ++i) {
        CHECK(SpawnSystem::rollRarity(bee, 0.0, kNeutralSpawnLuck, rng) == Rarity::Common);
        CHECK(SpawnSystem::rollRarity(bee, 200.0, kNeutralSpawnLuck, rng) == Rarity::Super);
    }
}

TEST(a_direct_spawn_below_min_rarity_is_raised_to_it) {
    Sim sim;
    const std::uint16_t evil = shipped().mobIndex("evil_centipede");
    const Entity e = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), evil, Rarity::Common,
                                          kCentre, Realm::Overworld, 0.0, sim.rng);
    CHECK(e != NULL_ENTITY);
    CHECK_EQ(sim.world.get<MobType>(e).rarity, Rarity::Rare);
    // ...and a tier above it is left alone.
    const Entity high = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), evil,
                                             Rarity::Legendary, kCentre, Realm::Overworld, 0.0, sim.rng);
    CHECK_EQ(sim.world.get<MobType>(high).rarity, Rarity::Legendary);
}

TEST(an_unknown_mob_index_spawns_nothing) {
    Sim sim;
    CHECK_EQ(sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), kInvalidIndex, Rarity::Common,
                                  kCentre, Realm::Overworld, 0.0, sim.rng),
             NULL_ENTITY);
    CHECK_EQ(sim.world.size(), std::size_t(0));
}

// ---------------------------------------------------------------------------
// Population control
// ---------------------------------------------------------------------------
//
// THE INVARIANT these tests are written against: every ambient mob in the
// world stands inside a spawn BAND, or is the escort or body segment of one
// that does. There is no second population driver behind the bands -- the
// per-viewer density fill that used to stock the ground an author had not
// drawn on is gone -- so a viewer on open ground is owed nothing, and a test
// that wants mobs has to put a band under them.

TEST(a_band_converges_to_the_population_its_own_area_buys) {
    // What "converges" means now. A band's target is kTargetMobDensity over
    // the area of its outline; it stocks itself to that whoever is looking,
    // and then holds. The number is derived here the same way the spawner
    // derives it, so a change to the density is a change to both.
    //
    // POPULATION, not entities. A band is the size of several screens, so most
    // of what it holds is latent at any moment and counting MobTags would be
    // counting the flower's viewport.
    const Rect band{kCentre.x - 3000.0, kCentre.y - 3000.0, 6000.0, 6000.0};
    WorldMaps maps;
    makeBandedWorld(maps, {band}, "garden 100%");
    Sim sim;
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{kCentre};

    for (int i = 0; i < 400; ++i) sim.tick(players);

    const int target = bandTarget(band);
    CHECK(target > 20);   // a band worth measuring convergence against
    // Most of the way there rather than exactly there: stocking is drained a
    // chunk per pass and a sample in somebody's lap is thrown away.
    CHECK(sim.populationIn(band) >= target * 3 / 4);
    // And never past it. The bound is on what the BAND placed: a hole's brood
    // and a centipede's body are children of a mob the band asked for rather
    // than spawns the band asked for, and a garden roster is full of both.
    CHECK(sim.bandPlacedIn(band) + static_cast<int>(sim.latent().size()) <= target);

    // Some of it is awake, because somebody is standing in the middle of it.
    // A band that stocked itself and woke none of it would satisfy every
    // count above and be an empty field to play in.
    CHECK(sim.mobsIn(band) > 0);

    // It holds: a converged population does not keep creeping upward. The
    // whole world this time, escorts included, because a nest going on
    // producing forever is exactly what this would catch.
    const int settled = sim.mobCount() + static_cast<int>(sim.latent().size());
    for (int i = 0; i < 400; ++i) sim.tick(players);
    CHECK(sim.mobCount() + static_cast<int>(sim.latent().size()) <=
          settled + kMaxNestChildren);
}

TEST(the_whole_map_is_stocked_whether_or_not_anybody_is_looking) {
    // The rule this file is built around. A band holds the population its own
    // area buys wherever it is drawn: nobody has to be near it, nobody has to
    // have visited it, and the flower below never goes anywhere near the
    // second one.
    const Rect near{kCentre.x - 3000.0, kCentre.y - 3000.0, 6000.0, 6000.0};
    const Rect away{kCentre.x + 12000.0, kCentre.y + 12000.0, 6000.0, 6000.0};
    WorldMaps maps;
    makeBandedWorld(maps, {near, away}, "bee 100%");
    Sim sim;
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{kCentre};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    // A dozen screens away, never seen, and full.
    CHECK(sim.populationIn(away) >= bandTarget(away) * 3 / 4);
    // And not one unit of that costs the tick anything: there is no entity in
    // it at all. That is the whole trade -- the map is the size of its area,
    // the simulation is the size of the viewports.
    CHECK_EQ(sim.mobsIn(away), 0);

    // The band under the flower is just as full, and some of it is awake.
    CHECK(sim.populationIn(near) >= bandTarget(near) * 3 / 4);
    CHECK(sim.mobsIn(near) > 0);
    // Awake means the part they can SEE, not the whole band: a 6000-unit band
    // is several screens across.
    CHECK(sim.mobsIn(near) < sim.populationIn(near));
}

TEST(a_mob_nobody_is_near_becomes_a_record_again_and_comes_back) {
    // The other direction, and what makes the population stable rather than
    // churning: walking away does not DESTROY the mobs behind you, it puts
    // them back to sleep where they stand. Walk back and the same band is
    // still full.
    const Rect band{kCentre.x - 1500.0, kCentre.y - 1500.0, 3000.0, 3000.0};
    WorldMaps maps;
    makeBandedWorld(maps, {band}, "bee 100%");
    Sim sim;
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> here{kCentre};
    for (int i = 0; i < 400; ++i) sim.tick(here);

    const int awake = sim.mobsIn(band);
    const int population = sim.populationIn(band);
    CHECK(awake > 0);

    // Off to the other side of the map, and wait out the grace period. Twice,
    // because a mob that was still in view on the pass the flower left gets
    // its full period from then.
    const std::vector<Vec2> elsewhere{Vec2{5000.0, 5000.0}};
    sim.tick(elsewhere);
    sim.jump(kMobDespawnDelayMillis + 1000.0, elsewhere);
    sim.jump(kMobDespawnDelayMillis + 1000.0, elsewhere);

    // Nothing there is simulated any more, and nothing was LOST: the same
    // population is standing there, all of it asleep.
    CHECK_EQ(sim.mobsIn(band), 0);
    CHECK(sim.populationIn(band) >= population - 5);
    CHECK(sim.spawner.census().demotedTotal >= awake);

    // Walk back. It is awake again within a population pass or two, and it did
    // not have to be spawned from nothing to get there.
    const int promotedBefore = sim.spawner.census().promotedTotal;
    for (int i = 0; i < 60; ++i) sim.tick(here);
    CHECK(sim.mobsIn(band) > 0);
    CHECK(sim.spawner.census().promotedTotal > promotedBefore);
}

TEST(ground_a_player_has_just_cleared_does_not_refill_in_front_of_them) {
    // The map is full everywhere and tops itself up the moment anything dies,
    // so something has to stop the replacements appearing on the screen of the
    // player who just cleared it. That is what a record's ready time is for:
    // one placed where nobody was looking is awake at once, and one placed
    // inside a live viewport waits out kInViewRespawnMin..Max first.
    const Rect band{kCentre.x - 3000.0, kCentre.y - 3000.0, 6000.0, 6000.0};
    WorldMaps maps;
    makeBandedWorld(maps, {band}, "bee 100%");
    Sim sim;
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{kCentre};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    const Rect screen{kCentre.x - kSpawnViewportHalfWidth, kCentre.y - kSpawnViewportHalfHeight,
                      kSpawnViewportHalfWidth * 2.0, kSpawnViewportHalfHeight * 2.0};
    const int cleared = sim.mobsIn(screen);
    CHECK(cleared > 2);

    // Kill everything the flower can see, the way a player would.
    Query<MobTag, Transform> mobs{sim.world};
    mobs.each([&](Entity e, MobTag&, Transform& transform) {
        if (screen.contains(transform.position)) sim.world.destroy(e);
    });
    CHECK_EQ(sim.mobsIn(screen), 0);

    // Two seconds later the band has already replaced them in its books --
    // the map does not wait to be full -- and the flower's own screen has not.
    for (int i = 0; i < 60; ++i) sim.tick(players);
    CHECK(sim.populationIn(band) >= (bandTarget(band) * 3) / 4);
    CHECK(sim.mobsIn(screen) <= cleared / 2);
}

TEST(a_killed_mob_is_replaced_where_it_died_rather_than_anywhere_in_its_band) {
    // The difference between a band that is evenly full and one that is full
    // on paper. These bands are millions of square units and a viewport is a
    // few: hand a casualty's slot back to the band at large and a player
    // farming one corner strips it while the far side of the same band quietly
    // goes over density. The map would still report itself full the whole
    // time, which is exactly the failure that is hard to see.
    const Rect band{kCentre.x - 6000.0, kCentre.y - 6000.0, 12000.0, 12000.0};
    WorldMaps maps;
    makeBandedWorld(maps, {band}, "bee 100%");
    Sim sim;
    sim.spawner.worldMaps = &maps;

    // One corner of it, a long way from the middle. The watched box is the
    // kill radius plus the scatter, so every honest replacement lands inside
    // it -- and it is a fourteenth of the band, so a band-wide replacement
    // almost never does.
    const Vec2 corner{band.x + 2000.0, band.y + 2000.0};
    constexpr double kKillRadius = 900.0;
    const double watchedHalf = kKillRadius + kRespawnScatter + 100.0;
    const Rect watched{corner.x - watchedHalf, corner.y - watchedHalf, watchedHalf * 2.0,
                       watchedHalf * 2.0};

    const std::vector<Vec2> visiting{corner};
    for (int i = 0; i < 300; ++i) sim.tick(visiting);
    const int before = sim.populationIn(watched);
    CHECK(before > 3);

    // Farm it, the way a player does: marked Dead so the spawner meets them as
    // casualties rather than as mobs that simply vanished, then reaped as the
    // runtime reaps them.
    int killed = 0;
    for (int round = 0; round < 8; ++round) {
        std::vector<Entity> doomed;
        Query<MobTag, Transform> mobs{sim.world};
        mobs.each([&](Entity e, MobTag&, Transform& transform) {
            if (sim.world.has<Dead>(e)) return;
            if (distance(transform.position, corner) > kKillRadius) return;
            doomed.push_back(e);
        });
        // Marked outside the walk: adding a component moves the entity to
        // another archetype, which relocates the rows the walk is holding.
        for (const Entity e : doomed) sim.world.add<Dead>(e, Dead{NULL_ENTITY});
        killed += static_cast<int>(doomed.size());
        sim.tick(visiting);
        for (const Entity e : doomed) {
            if (sim.world.isAlive(e)) sim.world.destroy(e);
        }
        // Long enough for the replacements to come back: they wait out
        // kInViewRespawnMin..Max first, because this is somebody's screen.
        for (int i = 0; i < 500; ++i) sim.tick(visiting);
    }
    CHECK(killed > 15);

    // Every one of those slots came back to this corner. Spread over the band
    // instead, they would have gone to a hundred and forty million square
    // units of somewhere else and left the corner permanently thin.
    CHECK(sim.populationIn(watched) >= before - 3);
}

TEST(a_boss_is_a_live_mob_wherever_it_rolled_and_never_sleeps) {
    // Supers and up are the exception: they are placed as entities whatever
    // corner of the map their band is in, they are never put back to sleep,
    // and they are never recycled. A boss is an event -- chat was told, the
    // bots were sent -- so it has to be standing there when somebody arrives.
    const Rect band{kCentre.x + 12000.0, kCentre.y + 12000.0, 1500.0, 1500.0};
    WorldMaps maps;
    makeBandedWorld(maps, {band}, "bee 100%", 200.0);   // difficulty 200: all supers
    Sim sim;
    sim.spawner.worldMaps = &maps;
    // A flower on the far side of the map: the band is never in anyone's view.
    const std::vector<Vec2> players{Vec2{5000.0, 5000.0}};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    CHECK(sim.mobsIn(band) > 0);
    // Not one of them is a record. A boss the spawner had quietly filed away
    // is a boss the announcement promised and the raid cannot find.
    for (const SpawnSystem::LatentSite& site : sim.latent()) {
        CHECK(rarityIndex(site.rarity) < rarityIndex(kAnnouncedRarity));
    }

    // And they outlast the grace period with nobody anywhere near them.
    const int standing = sim.mobsIn(band);
    sim.jump(kMobDespawnDelayMillis + 1000.0, players);
    sim.jump(kMobDespawnDelayMillis + 1000.0, players);
    CHECK_EQ(sim.mobsIn(band), standing);
}

TEST(a_boss_announces_itself_whatever_placed_it) {
    // Not just a band stocking itself: the arena, a nest, a script and the
    // operator console all reach the world through spawnMob, and the
    // announcement is made THERE. That is what makes "every boss is announced"
    // a property of the spawner rather than a thing each caller has to
    // remember.
    Sim sim;
    const std::uint16_t bee = shipped().mobIndex("bee");
    CHECK(bee != kInvalidIndex);

    const Entity boss = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), bee, Rarity::Super,
                                             kCentre, Realm::Overworld, sim.now, sim.rng);
    CHECK(boss != NULL_ENTITY);
    CHECK_EQ(sim.spawner.bossSpawns.size(), std::size_t(1));
    if (!sim.spawner.bossSpawns.empty()) {
        const SpawnSystem::BossSpawn& announced = sim.spawner.bossSpawns.front();
        // The entity rides with it, so the runtime can hand the bot controller
        // something it can follow rather than a coordinate that is already
        // stale by the time the raid sets off.
        CHECK_EQ(announced.entity, boss);
        CHECK(announced.rarity == Rarity::Super);
        CHECK(announced.mobIndex == bee);
    }

    // Everything below the line is scenery and says nothing, an ultra
    // included.
    sim.spawner.bossSpawns.clear();
    sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), bee, Rarity::Ultra,
                         kCentre + Vec2{500, 0}, Realm::Overworld, sim.now, sim.rng);
    CHECK(sim.spawner.bossSpawns.empty());
}

TEST(a_band_stocks_its_own_outline_and_never_the_open_ground_beside_it) {
    // This replaces "ambient mobs spawn inside the buffered viewport". The
    // viewport decides WHETHER a band fills, not WHERE its mobs land: the band
    // here sits off to one side of the flower, well inside their viewport, and
    // the open ground between the two stays empty because nothing fills it.
    //
    // `bee 100%` on purpose: bees nest nothing and tow no body chain, so every
    // mob in the world is a spawn this pass placed and the bound is exact.
    const Rect band{kCentre.x + 800.0, kCentre.y - 1000.0, 2000.0, 2000.0};
    WorldMaps maps;
    makeBandedWorld(maps, {band}, "bee 100%");
    Sim sim;
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{kCentre};
    for (int i = 0; i < 200; ++i) sim.tick(players);

    Query<MobTag, Transform> mobs{sim.world};
    int checked = 0;
    mobs.each([&](Entity, MobTag&, Transform& t) {
        ++checked;
        if (!band.contains(t.position)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "a mob stands outside the only band on the map, at " +
                                         std::to_string(t.position.x) + "," +
                                         std::to_string(t.position.y));
        }
        // Still nobody's lap, whoever the band filled for.
        CHECK(distance(t.position, kCentre) >= kMinSpawnDistance);
    });
    CHECK(checked > 0);
    // The rest of the viewport -- the same screen, one step outside the band --
    // is empty ground and stays that way.
    CHECK_EQ(sim.mobsIn(Rect{kCentre.x - 1400.0, kCentre.y - 1000.0, 2000.0, 2000.0}), 0);
}

TEST(a_crowd_of_players_cannot_exceed_the_global_cap) {
    // Sixty-four flowers standing apart, each with a band of their own. The
    // global cap is the one ceiling left above the bands, and it is what stops
    // a full server from multiplying the population by the number of people on
    // it.
    std::vector<Vec2> players;
    std::vector<Rect> bands;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const Vec2 at{3500.0 + x * 7500.0, 3500.0 + y * 7500.0};
            players.push_back(at);
            bands.push_back(Rect{at.x - 3000.0, at.y - 3000.0, 6000.0, 6000.0});
        }
    }
    WorldMaps maps;
    makeBandedWorld(maps, bands, "garden 100%");
    Sim sim;
    sim.spawner.worldMaps = &maps;
    // Between them the bands want several times what the server will allow.
    CHECK(static_cast<int>(bands.size()) * bandTarget(bands.front()) > kMaxLiveMobs);

    for (int i = 0; i < 600; ++i) sim.tick(players);

    CHECK(sim.mobCount() <= kMaxLiveMobs + kMaxNestChildren);
    // And the cap is what stopped it, not an empty world.
    CHECK(sim.spawner.census().mobs > kMaxLiveMobs / 2);
}

TEST(mobs_nobody_has_been_near_stop_being_entities) {
    // The recycler, which is where a mob STOPS being simulated. What happens
    // to it then depends on whether a band is holding a slot for it -- a band
    // mob goes back to being a record where it stands, an escort or a fixture
    // is simply gone -- and that half is
    // a_mob_nobody_is_near_becomes_a_record_again_and_comes_back's business.
    // This one pins the timing: the grace period, and that an empty viewer
    // list is permissive rather than a purge.
    const Rect band{kCentre.x - 3000.0, kCentre.y - 3000.0, 6000.0, 6000.0};
    WorldMaps maps;
    makeBandedWorld(maps, {band}, "garden 100%");
    Sim sim;
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{kCentre};
    for (int i = 0; i < 200; ++i) sim.tick(players);
    const int populated = sim.mobsWithin(kCentre, 4000.0);
    CHECK(populated > 0);
    const int despawnedBefore = sim.spawner.census().despawnedTotal;

    // An EMPTY viewer list is permissive, not a purge. The reference's
    // near-a-player test answers TRUE for every point when it saw no player at
    // all, so an unattended server keeps its population rather than emptying
    // itself and handing the next arrival a barren map.
    const std::vector<Vec2> nobody;
    sim.jump(kMobDespawnDelayMillis + 1000.0, nobody);
    sim.jump(kMobDespawnDelayMillis + 1000.0, nobody);
    CHECK_EQ(sim.mobsWithin(kCentre, 4000.0), populated);
    CHECK_EQ(sim.spawner.census().despawnedTotal, despawnedBefore);

    // A player who WALKS AWAY is what starts the clock: every flower's own
    // viewport box is tested, and a mob outside all of them stops being an
    // entity once it has been unseen for the grace period.
    const std::vector<Vec2> elsewhere{Vec2{90000.0, 90000.0}};
    sim.tick(elsewhere);
    CHECK_EQ(sim.mobsWithin(kCentre, 4000.0), populated);   // not immediately

    // Twice: nests are ticked before the census, so a nest on the way out can
    // still have placed one escort this pass, and that escort's own grace
    // period starts now.
    sim.jump(kMobDespawnDelayMillis + 1000.0, elsewhere);
    sim.jump(kMobDespawnDelayMillis + 1000.0, elsewhere);
    CHECK_EQ(sim.mobsWithin(kCentre, 4000.0), 0);
    CHECK(sim.spawner.census().despawnedTotal >= despawnedBefore + populated);
}

TEST(the_shipped_map_grows_its_own_biomes_roster) {
    // The shape of the game's own data: garden.tmj draws art, a door and a
    // handful of bands, and declares no map properties at all -- so the map's
    // biome falls back to its id and its default mob group falls back to its
    // biome. That group is what a band with no `mobs` of its own asks for, and
    // it is also what the bands the author HAS written name. A map that
    // resolved to an empty group would stand a player in an empty world,
    // silently, which is what this pins.
    if (!shippedMaps().forRealm(Realm::Overworld)) {
        ::testing::reportFailure(__FILE__, __LINE__, "the shipped maps did not load");
        return;
    }
    const MapData& world = *shippedMaps().forRealm(Realm::Overworld);
    CHECK_EQ(world.defaultMobGroup(), std::string("garden"));
    const std::uint16_t garden = shipped().mobGroupIndex(world.defaultMobGroup());
    CHECK(garden != kInvalidIndex);
    // The author adds bands, and gives some of them rosters of their own, as
    // the map is balanced. What every band owes is that each row it names is
    // something the content actually defines: a group, or a mob id. A row that
    // resolves to neither is a typo the fill would answer with an empty band.
    for (const MapElement& element : world.elements()) {
        if (!element.isSpawnBand() && !element.isMobRegion()) continue;
        for (const ZoneMobEntry& row : element.mobDistribution) {
            if (shipped().mobGroupIndex(row.name) == kInvalidIndex &&
                shipped().mobIndex(row.name) == kInvalidIndex) {
                ::testing::reportFailure(__FILE__, __LINE__,
                                         "a band on the shipped map names \"" + row.name +
                                             "\", which is neither a mob group nor a mob");
            }
        }
    }

    Sim sim;
    sim.spawner.worldMaps = &shippedMaps();
    // Standing where the author drew a band, because a band is the only thing
    // that spawns anything: elsewhere on this map there is nothing to measure.
    // The Sim's terrain is its own flat grid, so this is about which GROUP the
    // band asks for, not about walls.
    const Vec2 at = firstBandCentre(world);
    const std::vector<Vec2> players{at};
    for (int i = 0; i < 300; ++i) sim.tick(players);

    int checked = 0;
    Query<MobTag, MobType, Transform> mobs{sim.world};
    mobs.each([&](Entity e, MobTag&, MobType& type, Transform& transform) {
        // An escort is not an ambient spawn. `ant_hole` IS a garden mob, and
        // what it puts in the world is a hell of ants that are in no garden
        // group at all -- the band chose the hole, the hole chose them. Same
        // exemption the hard-band test makes, and for the same reason.
        if (sim.isChildOfAPlacedMob(e)) return;
        ++checked;
        const MobConfig& config = shipped().mob(type.configIndex);
        bool member = false;
        for (const MobGroupMember& entry : config.groups) member |= entry.group == garden;
        // Body segments follow their head into the world and belong to no
        // group of their own; everything else answers for itself.
        if (config.id.find("_body") == std::string::npos && !member) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "not a garden mob: " + config.id);
        }
    });
    CHECK(checked > 0);
}

TEST(the_region_under_a_spawn_decides_its_group) {
    // Authored here rather than read out of the shipped map: the shipped map
    // has no regions at all now, and this is a test of the SPAWNER, not of the
    // game's art. See authoredMap().
    if (!authoredMap().loaded()) {
        ::testing::reportFailure(__FILE__, __LINE__, "the authored fixture map did not load");
        return;
    }
    Sim sim;
    sim.spawner.worldMaps = &authoredMaps();
    const std::vector<Vec2> players{kCentre};
    for (int i = 0; i < 300; ++i) sim.tick(players);

    // The band over kCentre names no mobs of its own, so it asks the ground
    // under it -- and the region covering the whole map answers with the ant
    // hell's roster. Only that band is judged: the map also carries the dummy
    // row and a hornet band, both of which name their own rows and asked this
    // region for nothing.
    const std::uint16_t antHell = shipped().mobGroupIndex("ant_hell");
    CHECK(antHell != kInvalidIndex);
    const Rect regionBand{24000.0, 24000.0, 12000.0, 12000.0};
    Query<MobTag, MobType, Transform> mobs{sim.world};
    int checked = 0;
    mobs.each([&](Entity, MobTag&, MobType& type, Transform& transform) {
        if (!regionBand.contains(transform.position)) return;
        ++checked;
        const MobConfig& config = shipped().mob(type.configIndex);
        bool member = false;
        for (const MobGroupMember& entry : config.groups) member |= entry.group == antHell;
        // Body segments follow their head into the world and belong to no
        // group of their own; everything else answers for itself.
        if (config.id.find("_body") == std::string::npos && !member) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "not an ant hell mob: " + config.id + " " +
                                         rarityName(type.rarity) + " at " +
                                         std::to_string(transform.position.x) + "," +
                                         std::to_string(transform.position.y));
        }
    });
    CHECK(checked > 0);
}

TEST(no_mob_is_placed_inside_a_wall) {
    const Vec2 player = Terrain::tileCenter(100, 100);
    // A band over the flower, because nothing else places a mob any more, and a
    // solid block of wall inside that band. Solid rather than scattered so the
    // test asserts the invariant instead of the push-out solver's tolerance for
    // pathological geometry.
    const Rect band{player.x - 3000.0, player.y - 3000.0, 6000.0, 6000.0};
    WorldMaps maps;
    makeBandedWorld(maps, {band}, "garden 100%");
    Sim sim;
    sim.spawner.worldMaps = &maps;
    for (int ty = 96; ty <= 104; ++ty) {
        for (int tx = 103; tx <= 109; ++tx) sim.terrain.setTile(tx, ty, Tile::Wall);
    }
    const std::vector<Vec2> players{player};

    for (int i = 0; i < 300; ++i) sim.tick(players);

    Query<MobTag, Transform> mobs{sim.world};
    int checked = 0;
    mobs.each([&](Entity, MobTag&, Transform& t) {
        ++checked;
        CHECK(!sim.terrain.blocked(t.position, Realm::Overworld));
    });
    CHECK(checked > 0);

    // The same holds for a caller that asks for a spot in the middle of the
    // wall: a request is a request, not a promise.
    const std::uint16_t ant = shipped().mobIndex("soldier_ant");
    for (int i = 0; i < 100; ++i) {
        const Vec2 inWall = Terrain::tileCenter(106, 100) + sim.rng.insideCircle(200.0);
        const Entity e = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), ant,
                                              Rarity::Common, inWall, Realm::Overworld, 0.0, sim.rng);
        CHECK(e != NULL_ENTITY);
        CHECK(!sim.terrain.blocked(sim.world.get<Transform>(e).position, Realm::Overworld));
    }
}

TEST(a_spawned_mob_carries_everything_the_simulation_needs) {
    Sim sim;
    const std::uint16_t bee = shipped().mobIndex("bee");
    const Entity e = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), bee, Rarity::Rare,
                                          kCentre, Realm::Overworld, 1234.0, sim.rng);
    CHECK(e != NULL_ENTITY);
    CHECK(sim.world.has<MobTag>(e));
    CHECK(sim.world.has<Motion>(e));
    CHECK(sim.world.has<Knockback>(e));
    CHECK(sim.world.has<HitCooldowns>(e));
    CHECK(sim.world.has<Afflictions>(e));
    CHECK(sim.world.has<AmbientMob>(e));

    const MobStats stats = shipped().mobStats(bee, Rarity::Rare);
    CHECK_NEAR(sim.world.get<Health>(e).max, stats.health, 1e-9);
    CHECK_NEAR(sim.world.get<Health>(e).current, stats.health, 1e-9);
    CHECK_NEAR(sim.world.get<Body>(e).radius, stats.radius, 1e-9);
    CHECK_NEAR(sim.world.get<Bounty>(e).xp, stats.xp, 1e-9);
    CHECK_EQ(sim.world.get<Replicated>(e).kind, net::EntityKind::Mob);
    CHECK_EQ(sim.world.get<Replicated>(e).typeIndex, bee);
    // No allocator wired up: the mob simulates and simply is not replicated.
    CHECK(!sim.world.has<NetId>(e));
    CHECK_NEAR(sim.world.get<AmbientMob>(e).lastNearPlayerMillis, 1234.0, 1e-9);
}

TEST(random_size_jitters_the_body_and_nothing_else) {
    Sim sim;
    // `sandstorm` ships random_size [1, 2].
    const std::uint16_t sandstorm = shipped().mobIndex("sandstorm");
    const MobConfig& config = shipped().mob(sandstorm);
    CHECK(config.randomSizeMax > config.randomSizeMin);

    const MobStats stats = shipped().mobStats(sandstorm, Rarity::Common);
    // `random_size` is an ABSOLUTE size range, not a factor, so the reference
    // divides the roll by the config's own nominal `size` before using it as a
    // multiplier. Sandstorm is size 1.5 with a [1, 2] range, so its bodies come
    // out between 0.667x and 1.333x -- not between 1x and 2x.
    const double lowest = config.randomSizeMin / config.size;
    const double highest = config.randomSizeMax / config.size;
    CHECK_NEAR(lowest, 1.0 / 1.5, 1e-12);
    CHECK_NEAR(highest, 2.0 / 1.5, 1e-12);

    bool sawSmall = false;
    bool sawLarge = false;
    for (int i = 0; i < 200; ++i) {
        const Entity e = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), sandstorm,
                                              Rarity::Common, kCentre, Realm::Overworld, 0.0, sim.rng);
        const double jitter = sim.world.get<MobType>(e).sizeJitter;
        CHECK(jitter >= lowest);
        CHECK(jitter <= highest);
        CHECK_NEAR(sim.world.get<Body>(e).radius, stats.radius * jitter, 1e-9);
        // Mass is NOT jittered. It is derived from the config size and the
        // rarity step alone (`mass = size * size` in the stat table), so a
        // sandstorm that rolled a big body is exactly as easy to knock back as
        // one that rolled a small one.
        CHECK_NEAR(sim.world.get<Body>(e).mass, stats.mass, 1e-9);
        sawSmall = sawSmall || jitter < 0.8;
        sawLarge = sawLarge || jitter > 1.2;
    }
    CHECK(sawSmall);
    CHECK(sawLarge);

    // A mob with no random_size gets exactly its configured size.
    const Entity bee = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(),
                                            shipped().mobIndex("bee"), Rarity::Common, kCentre, Realm::Overworld,
                                            0.0, sim.rng);
    CHECK_NEAR(sim.world.get<MobType>(bee).sizeJitter, 1.0, 1e-12);
}

// ---------------------------------------------------------------------------
// Nests
// ---------------------------------------------------------------------------

TEST(a_nest_places_its_initial_escorts) {
    Sim sim;
    const std::uint16_t hole = shipped().mobIndex("ant_hole");
    const MobConfig& config = shipped().mob(hole);
    CHECK(config.initialSpawns.size() == std::size_t(6));

    const Entity nest = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), hole,
                                             Rarity::Common, kCentre, Realm::Overworld, 0.0, sim.rng);
    CHECK(nest != NULL_ENTITY);
    CHECK_EQ(sim.mobCount(), 1 + static_cast<int>(config.initialSpawns.size()));
    CHECK(sim.world.has<NestWaves>(nest));
    // The nest itself is still addressable after spawning six escorts, which is
    // the archetype-relocation trap this ordering exists to avoid.
    CHECK_EQ(sim.world.get<MobType>(nest).configIndex, hole);
}

TEST(a_nest_sends_its_waves_as_it_is_worn_down_and_holds_at_the_last) {
    Sim sim;
    const std::uint16_t hole = shipped().mobIndex("ant_hole");
    const std::size_t waveCount = shipped().mob(hole).spawnWaves.size();
    CHECK(waveCount > 1);
    const int lastWave = static_cast<int>(waveCount) - 1;

    const Entity nest = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), hole,
                                             Rarity::Common, kCentre, Realm::Overworld, 0.0, sim.rng);
    const std::vector<Vec2> players{kCentre};
    // Counted off the nest rather than off the world, so a band fill running
    // beside it could never be mistaken for one of this hole's waves.
    const auto escortCount = [&] {
        return sim.world.get<NestWaves>(nest).children.size();
    };
    const std::size_t afterInitial = escortCount();

    // A hole answers DAMAGE, not a clock. Each wave hangs off an HP threshold
    // (gardn's kAntHole) and every band crossed on the way down fires, so a
    // hole nobody is hitting sends nothing however long it stands there.
    sim.now += kNestWaveIntervalMillis * 5.0;
    sim.tick(players);
    CHECK_EQ(escortCount(), afterInitial);
    CHECK_EQ(sim.world.get<NestWaves>(nest).nextWave, 0);

    // Half its health off releases every band it crossed on the way there.
    sim.world.get<Health>(nest).current = sim.world.get<Health>(nest).max * 0.5;
    sim.now += net::kTickMillis;
    sim.tick(players);
    const int halfway = static_cast<int>(sim.world.get<NestWaves>(nest).nextWave);
    CHECK(halfway > 0);
    CHECK(halfway < lastWave);
    CHECK(escortCount() > afterInitial);
    // Every one of them is tethered to the hole that sent it, so leading them
    // away cannot strip it of its defenders.
    for (const Entity escort : sim.world.get<NestWaves>(nest).children) {
        CHECK(sim.world.has<HoleTether>(escort));
        CHECK_EQ(sim.world.get<HoleTether>(escort).hole, nest);
    }

    // Healing only moves the mark: a rise in health sends nothing, which is
    // what stops a regenerating hole from emptying its list into the world.
    const std::size_t beforeHeal = escortCount();
    sim.world.get<Health>(nest).current = sim.world.get<Health>(nest).max;
    sim.now += net::kTickMillis;
    sim.tick(players);
    CHECK_EQ(escortCount(), beforeHeal);
    CHECK_EQ(static_cast<int>(sim.world.get<NestWaves>(nest).nextWave), halfway);

    // Worn to nothing in one blow. The band index is clamped at both ends, so
    // an overkill that drives health far negative sends the rest of the list
    // once rather than spinning millions of skipped iterations.
    const std::vector<Entity> escorts = sim.world.get<NestWaves>(nest).children;
    for (const Entity escort : escorts) sim.world.destroy(escort);
    sim.world.get<Health>(nest).current = -1e6;
    sim.now += net::kTickMillis;
    sim.tick(players);
    CHECK_EQ(static_cast<int>(sim.world.get<NestWaves>(nest).nextWave), lastWave);
    CHECK(escortCount() > 0);
}

TEST(a_periodic_nest_holds_its_escort_cap_and_expires_them) {
    Sim sim;
    const std::uint16_t queen = shipped().mobIndex("queen_ant");
    const PeriodicSpawnSpec& spec = shipped().mob(queen).periodicSpawn;
    CHECK(spec.present);
    CHECK(spec.maxAlive > 0);
    CHECK(spec.lifetimeMillis > 0);

    const Entity nest = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), queen,
                                             Rarity::Rare, kCentre, Realm::Overworld, 0.0, sim.rng);
    CHECK(sim.world.has<Spawner>(nest));
    const std::vector<Vec2> players{kCentre};

    // Long enough for many intervals; the cap must hold regardless.
    for (int i = 0; i < 1500; ++i) sim.tick(players);
    const int live = static_cast<int>(sim.world.get<Spawner>(nest).children.size());
    CHECK(live <= spec.maxAlive);
    CHECK(live > 0);

    // Escorts carry a lifetime, and it is this system that runs it down.
    Query<Pet> pets{sim.world};
    CHECK_EQ(pets.count(), std::size_t(0));
    Query<AmbientMob, Lifetime> timed{sim.world};
    CHECK(timed.count() > 0);
}

TEST(a_dead_nest_stops_producing) {
    Sim sim;
    const std::uint16_t queen = shipped().mobIndex("queen_ant");
    const Entity nest = sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), queen,
                                             Rarity::Rare, kCentre, Realm::Overworld, 0.0, sim.rng);
    sim.world.add<Dead>(nest, Dead{NULL_ENTITY});

    const std::vector<Vec2> players{kCentre};
    const int before = sim.mobCount();
    for (int i = 0; i < 200; ++i) sim.tick(players);
    // The ambient roll keeps working, but the corpse spawned none of it: its
    // own escort list never grew.
    CHECK_EQ(sim.world.get<Spawner>(nest).children.size(), std::size_t(0));
    CHECK(sim.mobCount() >= before);
}

// ---------------------------------------------------------------------------
// Drop tables
// ---------------------------------------------------------------------------

TEST(the_drop_table_links_cleanly_against_the_shipped_content) {
    DropTables tables;
    tables.link(shipped());
    // Every id in the table is one the content defines. A line that does not
    // resolve is a data bug, not something to discover at runtime.
    if (!tables.unresolved().empty()) {
        std::printf("    (unresolved: %s)\n", tables.unresolved().front().c_str());
    }
    CHECK(tables.unresolved().empty());

    CHECK_EQ(tables.forMob(shipped().mobIndex("bee")).size(), std::size_t(4));
    CHECK_EQ(tables.forMob(shipped().mobIndex("starfish")).size(), std::size_t(2));
    // TypeScript synthesises a guaranteed common egg even for a mob with no
    // authored table. Only an index off the end has no table at all.
    CHECK_EQ(tables.forMob(shipped().mobIndex("dust")).size(), std::size_t(1));
    CHECK(tables.forMob(kInvalidIndex).empty());

    CHECK(tables.linkedTo(shipped()));
    tables.link(shipped());   // idempotent
    CHECK(tables.unresolved().empty());
}

TEST(drop_rarity_uses_authored_rows_for_common_and_uncommon_mobs) {
    Rng rng(31337);
    for (int i = 0; i < 40000; ++i) {
        const Rarity r = LootSystem::rollDropRarity(Rarity::Rare, Rarity::Common, rng);
        const int delta = rarityIndex(r) - rarityIndex(Rarity::Rare);
        CHECK(delta >= -1 && delta <= 1);
        const Rarity uncommon =
            LootSystem::rollDropRarity(Rarity::Common, Rarity::Uncommon, rng);
        CHECK(rarityIndex(uncommon) >= rarityIndex(Rarity::Common));
        CHECK(rarityIndex(uncommon) <= rarityIndex(Rarity::Uncommon));
    }
}

TEST(drop_rarity_applies_mob_floors_and_the_apex_item_cap) {
    Rng rng(5);
    for (int i = 0; i < 2000; ++i) {
        const Rarity rare = LootSystem::rollDropRarity(Rarity::Common, Rarity::Rare, rng);
        CHECK(rarityIndex(rare) >= rarityIndex(Rarity::Uncommon));
        CHECK(rarityIndex(rare) <= rarityIndex(Rarity::Rare));

        const Rarity apex = LootSystem::rollDropRarity(Rarity::Apex, Rarity::Apex, rng);
        CHECK(rarityIndex(apex) >= rarityIndex(Rarity::Super));
        CHECK(rarityIndex(apex) <= rarityIndex(Rarity::Unique));
    }
}

// ---------------------------------------------------------------------------
// Loot: drops, eligibility, pickup, expiry
// ---------------------------------------------------------------------------

TEST(a_killed_mob_drops_from_its_own_table) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(11);

    const std::uint16_t starfish = shipped().mobIndex("starfish");
    const std::uint16_t starfishPetal = shipped().petalIndex("starfish");
    const std::uint16_t starfishEgg = shipped().petalIndex("starfish_egg");
    const Entity player = makePlayer(world, kCentre + Vec2{5000, 0});

    for (int i = 0; i < 40; ++i) {
        makeCorpse(world, starfish, Rarity::Uncommon, kCentre, player, {player});
    }
    loot.run(world, grid, shipped(), rng, 1000.0, net::kTickSeconds, commands, events);
    commands.flush();

    const std::vector<Entity> drops = liveDrops(world);
    // Uncommon mobs drop every row: the authored starfish plus its generated
    // guaranteed egg.
    CHECK_EQ(drops.size(), std::size_t(80));
    int petals = 0;
    int eggs = 0;
    for (const Entity drop : drops) {
        const DropItem& item = world.get<DropItem>(drop);
        if (item.configIndex == starfishPetal) ++petals;
        if (item.configIndex == starfishEgg) ++eggs;
        CHECK_EQ(item.eligible.size(), std::size_t(1));
        CHECK_EQ(item.eligible.front(), player);
        CHECK(item.pickedUpBy.empty());
        const Vec2 offset = world.get<Transform>(drop).position - kCentre;
        CHECK(std::abs(offset.x) <= 50.0);
        CHECK(std::abs(offset.y) <= 50.0);
        CHECK(world.has<DropTag>(drop));
        CHECK_EQ(world.get<Replicated>(drop).kind, net::EntityKind::Drop);
    }
    CHECK_EQ(petals, 40);
    CHECK_EQ(eggs, 40);
}

TEST(a_mob_pays_out_exactly_once_however_long_its_corpse_lingers) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(12);

    const Entity player = makePlayer(world, kCentre + Vec2{5000, 0});
    makeCorpse(world, shipped().mobIndex("starfish"), Rarity::Common, kCentre, player, {player});

    loot.run(world, grid, shipped(), rng, 0.0, net::kTickSeconds, commands, events);
    commands.flush();
    const std::size_t first = liveDrops(world).size();
    CHECK_EQ(first, std::size_t(2));

    for (int i = 0; i < 5; ++i) {
        loot.run(world, grid, shipped(), rng, 40.0 * i, net::kTickSeconds, commands, events);
        commands.flush();
    }
    CHECK_EQ(liveDrops(world).size(), first);
}

TEST(a_pet_dying_is_not_loot) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(13);

    const Entity owner = makePlayer(world, kCentre);
    const Entity pet = makeCorpse(world, shipped().mobIndex("starfish"), Rarity::Common, kCentre,
                                  owner, {owner});
    world.add<Pet>(pet, Pet{owner, 0});

    loot.run(world, grid, shipped(), rng, 0.0, net::kTickSeconds, commands, events);
    commands.flush();
    CHECK_EQ(liveDrops(world).size(), std::size_t(0));
}

TEST(common_mobs_roll_each_drop_row_at_most_once) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(14);

    const std::uint16_t flower = shipped().mobIndex("glitch_flower");
    const Entity player = makePlayer(world, kCentre + Vec2{5000, 0});
    DropTables tables;
    tables.link(shipped());
    const int rowCount = static_cast<int>(tables.forMob(flower).size());

    int mostSeen = 0;
    for (int i = 0; i < 300; ++i) {
        makeCorpse(world, flower, Rarity::Common, kCentre, player, {player});
        loot.run(world, grid, shipped(), rng, 0.0, net::kTickSeconds, commands, events);
        commands.flush();
        const int produced = static_cast<int>(liveDrops(world).size());
        mostSeen = std::max(mostSeen, produced);
        CHECK(produced <= rowCount);
        for (const Entity drop : liveDrops(world)) world.destroy(drop);
    }
    CHECK_EQ(mostSeen, rowCount);
}

TEST(a_non_contributor_can_never_take_an_eligible_players_drop) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(15);

    const Entity fighter = makePlayer(world, kCentre + Vec2{4000, 0}, 0.0, 1);
    const Entity bystander = makePlayer(world, kCentre, 0.0, 2);
    (void)bystander;
    const Entity drop = loot.spawnDrop(world, shipped().petalIndex("rose"), Rarity::Rare, kCentre, Realm::Overworld,
                                       {fighter}, 0.0);
    CHECK(drop != NULL_ENTITY);

    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 100.0, 0.0, commands, events);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(0));
    CHECK(world.isAlive(drop));

    // Eligibility never turns into a timed free-for-all.
    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 1e9, 0.0, commands, events);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(0));
    CHECK(world.isAlive(drop));
}

TEST(a_contributor_may_take_a_reserved_drop_at_once) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(16);
    NetIdAllocator ids;
    loot.netIds = &ids;

    const Entity fighter = makePlayer(world, kCentre, 0.0, 7);
    const Entity drop = loot.spawnDrop(world, shipped().petalIndex("rose"), Rarity::Common,
                                       kCentre, Realm::Overworld, {fighter}, 0.0);
    // A wired allocator is what makes a drop visible to a client at all.
    CHECK(world.has<NetId>(drop));
    const std::uint32_t dropNetId = world.get<NetId>(drop).value;

    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 10.0, 0.0, commands, events);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(1));
    CHECK_EQ(loot.pickups().front().player, fighter);
    // The event carries both ids so the client can fly the item to the flower.
    CHECK_EQ(events.events().size(), std::size_t(1));
    if (!events.events().empty()) {
        CHECK_EQ(events.events().front().kind, net::EventKind::PickedUp);
        CHECK_EQ(events.events().front().netId, dropNetId);
        CHECK_EQ(events.events().front().otherNetId, 7u);
    }
}

TEST(an_unreserved_drop_is_free_for_anyone) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(17);

    const Entity passerby = makePlayer(world, kCentre);
    loot.spawnDrop(world, shipped().petalIndex("rose"), Rarity::Common, kCentre, Realm::Overworld, {}, 0.0);
    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 0.0, 0.0, commands, events);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(1));
    CHECK_EQ(loot.pickups().front().player, passerby);
    CHECK(world.isAlive(liveDrops(world).front()));
}

TEST(each_player_can_take_an_unrestricted_drop_once) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(18);

    makePlayer(world, kCentre, 0.0, 1);
    makePlayer(world, kCentre, 0.0, 2);
    loot.spawnDrop(world, shipped().petalIndex("rose"), Rarity::Common, kCentre, Realm::Overworld, {}, 0.0);

    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 0.0, 0.0, commands, events);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(2));
    CHECK_EQ(liveDrops(world).size(), std::size_t(1));

    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 1.0, 0.0, commands, events);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(0));
}

TEST(magnetism_widens_the_pickup_radius_without_moving_the_drop) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(19);

    const Vec2 dropAt = kCentre + Vec2{300.0, 0.0};
    const Entity player = makePlayer(world, kCentre, 0.0);
    const Entity drop = loot.spawnDrop(world, shipped().petalIndex("rose"), Rarity::Common, dropAt, Realm::Overworld,
                                       {player}, 0.0);
    CHECK(300.0 > kDropPickupRadius);

    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 0.0, 0.0, commands, events);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(0));
    // Out of reach and untouched: nothing dragged it toward the player.
    CHECK(world.get<Transform>(drop).position == dropAt);

    world.get<PlayerModifiers>(player).magnetism = 400.0;
    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 0.0, 0.0, commands, events);
    // Still where it was dropped at the moment it was claimed -- the destroy is
    // deferred, so this reads the drop as the pickup left it.
    CHECK(world.get<Transform>(drop).position == dropAt);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(1));
    CHECK(!world.isAlive(drop));
}

TEST(a_dead_player_picks_nothing_up) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(20);

    const Entity player = makePlayer(world, kCentre);
    world.get<Health>(player).current = 0.0;
    loot.spawnDrop(world, shipped().petalIndex("rose"), Rarity::Common, kCentre, Realm::Overworld, {}, 0.0);

    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 0.0, 0.0, commands, events);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(0));
}

TEST(drops_expire) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(21);

    const Entity drop = loot.spawnDrop(world, shipped().petalIndex("rose"), Rarity::Common, kCentre, Realm::Overworld,
                                       {}, 0.0);
    constexpr double lifetime = kDropLifetimeByRarity[rarityIndex(Rarity::Common)];
    CHECK_NEAR(world.get<Lifetime>(drop).remainingSeconds, lifetime, 1e-12);

    double now = 0.0;
    const int ticks = static_cast<int>(lifetime * net::kTicksPerSecond) - 2;
    for (int i = 0; i < ticks; ++i) {
        loot.run(world, grid, shipped(), rng, now, net::kTickSeconds, commands, events);
        commands.flush();
        now += net::kTickMillis;
    }
    CHECK(world.isAlive(drop));

    for (int i = 0; i < 4; ++i) {
        loot.run(world, grid, shipped(), rng, now, net::kTickSeconds, commands, events);
        commands.flush();
        now += net::kTickMillis;
    }
    CHECK(!world.isAlive(drop));
}

TEST(a_drop_is_collectable_on_the_tick_it_was_created_in) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(22);

    // The player is standing on the corpse, with magnetism to spare.
    const Entity player = makePlayer(world, kCentre, 500.0, 1);
    makeCorpse(world, shipped().mobIndex("starfish"), Rarity::Common, kCentre, player, {player});
    rebuildGrid(world, grid);

    loot.run(world, grid, shipped(), rng, 0.0, net::kTickSeconds, commands, events);
    commands.flush();
    // Taken on the spot. The reference rolls a mob's drops inside the very
    // player step that then tests pickups -- resolvePlayerPetals kills it and
    // resolvePlayerItemPickups runs a few lines later in the same function --
    // so a magnet flower standing on its own kill collects the item before any
    // snapshot could have carried it. Holding the drop back for a tick would
    // be a different game: it is the pickup CUE, which carries the drop's
    // position and look, that gives the client something to animate.
    CHECK_EQ(loot.pickups().size(), std::size_t(2));
    CHECK_EQ(liveDrops(world).size(), std::size_t(0));

    // ...and it is taken exactly once: the second pass finds nothing left.
    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 40.0, net::kTickSeconds, commands, events);
    commands.flush();
    CHECK_EQ(loot.pickups().size(), std::size_t(0));
}

TEST(the_pickup_cue_carries_the_drops_look_not_just_its_id) {
    // The cue is the ONLY thing a same-tick pickup leaves behind: the entity
    // is created and destroyed inside one tick, so no snapshot ever carried
    // it and the client has never heard of that net id. A cue naming only the
    // id is unplayable for exactly the flowers that collect the most -- an
    // apex observer alone reaches 437 units -- and an unplayable cue is what
    // "mobs stopped dropping loot" looks like from the seat.
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(2201);
    NetIdAllocator ids;
    loot.netIds = &ids;

    const Entity player = makePlayer(world, kCentre, 500.0, 7);
    (void)player;
    const std::uint16_t rose = shipped().petalIndex("rose");
    const Entity drop =
        loot.spawnDrop(world, rose, Rarity::Epic, kCentre, Realm::Overworld, {}, 0.0);
    CHECK(drop != NULL_ENTITY);
    const std::uint32_t dropNetId = world.get<NetId>(drop).value;
    rebuildGrid(world, grid);

    loot.run(world, grid, shipped(), rng, 0.0, net::kTickSeconds, commands, events);
    commands.flush();

    const WireEvent* cue = nullptr;
    for (const WireEvent& event : events.events()) {
        if (event.kind == net::EventKind::PickedUp) cue = &event;
    }
    CHECK(cue != nullptr);
    if (cue == nullptr) return;
    CHECK_EQ(cue->netId, dropNetId);
    CHECK_EQ(cue->otherNetId, std::uint32_t(7));
    CHECK_EQ(cue->position.x, kCentre.x);
    CHECK_EQ(cue->position.y, kCentre.y);
    // The look: the petal index in `amount` and the tier in `flag`, the two
    // fixed fields this kind has no other use for.
    CHECK_EQ(static_cast<std::uint16_t>(cue->amount), rose);
    CHECK_EQ(static_cast<int>(cue->flag), rarityIndex(Rarity::Epic));
}

TEST(the_pickup_callback_sees_what_the_list_sees) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(23);

    std::vector<LootSystem::Pickup> seen;
    loot.onPickup = [&](const LootSystem::Pickup& p) { seen.push_back(p); };

    const Entity player = makePlayer(world, kCentre);
    loot.spawnDrop(world, shipped().petalIndex("rose"), Rarity::Epic, kCentre, Realm::Overworld, {}, 0.0);
    rebuildGrid(world, grid);
    loot.run(world, grid, shipped(), rng, 0.0, 0.0, commands, events);
    commands.flush();

    CHECK_EQ(seen.size(), std::size_t(1));
    CHECK_EQ(seen.front().player, player);
    CHECK_EQ(seen.front().rarity, Rarity::Epic);
    CHECK_EQ(loot.pickups().size(), seen.size());

    // The list is per-run, not cumulative: a runtime that drains it every tick
    // must never be handed yesterday's pickups again.
    loot.run(world, grid, shipped(), rng, 40.0, 0.0, commands, events);
    CHECK_EQ(loot.pickups().size(), std::size_t(0));
}

TEST(a_dead_contributor_is_still_credited_but_a_non_player_is_not) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng(24);

    const Entity player = makePlayer(world, kCentre + Vec2{5000, 0});
    const Entity turret = world.create();   // something that damages but is not a player
    world.add<MobTag>(turret);

    const Entity corpse = makeCorpse(world, shipped().mobIndex("starfish"), Rarity::Common, kCentre,
                                     player, {player, turret});
    (void)corpse;
    loot.run(world, grid, shipped(), rng, 0.0, net::kTickSeconds, commands, events);
    commands.flush();

    const std::vector<Entity> drops = liveDrops(world);
    CHECK_EQ(drops.size(), std::size_t(2));
    for (const Entity drop : drops) {
        CHECK_EQ(world.get<DropItem>(drop).eligible.size(), std::size_t(1));
        CHECK_EQ(world.get<DropItem>(drop).eligible.front(), player);
    }
}

// ---------------------------------------------------------------------------
// Bands that name their mobs
// ---------------------------------------------------------------------------
//
// The map does not only say which TIER belongs where. A band may name a mob
// outright, and that is the only way a mob the group roll refuses ever reaches
// the world: `target_dummy` is in no group and is marked neverAmbient, so the
// dummy bands are the whole of its existence. A parser that keeps the tier
// and drops the name leaves the DPS row unbuilt and nothing else about the
// world looks wrong.

namespace {

const WorldMaps& shippedMaps() {
    static const WorldMaps maps = [] {
        WorldMaps m;
        std::string error;
        // The manifest is what marks a staged data directory; its directory is
        // the one WorldMaps wants. FLIX_TEST_DATA_DIR first: that is the
        // directory the build stages, and the only one that has the maps in it.
        const std::string manifest = firstExisting({
#ifdef FLIX_TEST_DATA_DIR
            std::string(FLIX_TEST_DATA_DIR) + "/maps.json",
#endif
            testsDir() + "/../build/data/maps.json", "data/maps.json", "../data/maps.json"});
        const std::string dir = manifest.substr(0, manifest.find_last_of('/'));
        if (!m.load(dir, nullptr, error)) {
            std::fprintf(stderr, "[test] the shipped maps did not load: %s\n", error.c_str());
        }
        return m;
    }();
    return maps;
}

// ---------------------------------------------------------------------------
// An AUTHORED map, written here
// ---------------------------------------------------------------------------
//
// The shipped map is hand-drawn art with one door on it and no annotations at
// all: no difficulty bands, no mob regions. That is the new normal -- an author
// paints a map and the mobs follow from its `defaultMobGroup` -- and it means
// the shipped data can no longer stand in for "a map with bands on it".
//
// It used to: world.tmj carried two hundred bands, nine regions and nine
// target-dummy rows, and every test below read its coverage out of the game's
// own content. Everything those tests pinned is still true of the SPAWNER, so
// the bands they need are authored here instead, in the same Tiled shape a map
// file has. The map is a few cells wide and its objects are laid out over
// sixty thousand units, because MapData reads the annotations and the Sim
// brings its own flat terrain -- the tile layer is a formality.

/// Writes `body` to /tmp/<name>, with the minimal tileset every map must name
/// beside it. Returns the path.
std::string writeTiledFixture(const std::string& name, const std::string& body) {
    const std::string dir = std::string("/tmp/flix-spawn-fixture-") + std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0755);
    {
        std::ofstream tileset(dir + "/fixture.tsj", std::ios::binary | std::ios::trunc);
        tileset << R"({"name": "fixture", "type": "tileset", "version": "1.10",
 "tilewidth": 300, "tileheight": 300, "tilecount": 1, "columns": 0,
 "grid": {"orientation": "orthogonal", "width": 300, "height": 300},
 "tiles": [{"id": 0, "image": "tiles/grass_c_0.svg", "imagewidth": 300, "imageheight": 300}]})";
    }
    const std::string path = dir + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    return path;
}

/// The 1x1 map body every fixture here shares: one empty cell, one tileset,
/// whatever `spawns` objects the caller wrote, and whatever MAP properties it
/// wants (a `defaultMobGroup`, for the tests about a band with no roster of
/// its own).
std::string fixtureMapBody(const std::string& spawns, const std::string& properties = {}) {
    std::string out = R"({
      "type": "map", "orientation": "orthogonal", "infinite": false,
      "width": 1, "height": 1, "tilewidth": 300, "tileheight": 300,)";
    if (!properties.empty()) out += "\n      \"properties\": [" + properties + "],";
    out += R"(
      "tilesets": [{"firstgid": 1, "source": "fixture.tsj"}], "layers": [
        {"type": "tilelayer", "name": "ground", "width": 1, "height": 1, "data": [0]},
        {"type": "objectgroup", "name": "spawns", "objects": [)" + spawns + R"(]}
      ]})";
    return out;
}

/// One `spawn` object: a rectangle, an optional DIFFICULTY, and a distribution.
/// No difficulty makes it a mob REGION rather than a band -- difficulty zero is
/// a band of commons, which is why the marker for "no difficulty" is a missing
/// property rather than a zero.
std::string spawnObject(int id, double x, double y, double w, double h, bool hasDifficulty,
                        double difficulty, const std::string& mobs) {
    std::string out = "{\"id\": " + std::to_string(id) + ", \"class\": \"spawn\", \"x\": " +
                      std::to_string(x) + ", \"y\": " + std::to_string(y) + ", \"width\": " +
                      std::to_string(w) + ", \"height\": " + std::to_string(h) +
                      ", \"properties\": [";
    if (hasDifficulty) {
        out += "{\"name\": \"difficulty\", \"type\": \"float\", \"value\": " +
               std::to_string(difficulty) + "},";
    }
    out += "{\"name\": \"mobs\", \"type\": \"string\", \"value\": \"" + mobs + "\"}]}";
    return out;
}

/// The DPS row's difficulties: the one difficulty per tier at which that tier
/// is EXACTLY what the ground grows (an integer tier value, so the blend is one
/// tier with no fraction in it). Nine rungs, common through unique -- apex is
/// the top of the curve and a tenth dummy would need a difficulty past the
/// clamp.
inline constexpr int kDummyRowRungs = 9;
const std::array<double, kDummyRowRungs>& dummyRowDifficulties() {
    static const std::array<double, kDummyRowRungs> table = [] {
        std::array<double, kDummyRowRungs> out{};
        for (int i = 0; i < kDummyRowRungs; ++i) {
            out[static_cast<std::size_t>(i)] = difficultyForTierValue(i);
        }
        return out;
    }();
    return table;
}
/// A `spawn` object with no difficulty: a mob region.
std::string regionObject(int id, double x, double y, double w, double h,
                         const std::string& mobs) {
    return spawnObject(id, x, y, w, h, false, 0.0, mobs);
}

/// A `spawn` object WITH a difficulty: a band.
std::string bandObject(int id, double x, double y, double w, double h, double difficulty,
                       const std::string& mobs) {
    return spawnObject(id, x, y, w, h, true, difficulty, mobs);
}

void makeBandedWorld(WorldMaps& out, const std::vector<Rect>& bands, const std::string& mobs,
                     double difficulty) {
    std::string objects;
    int id = 1;
    for (const Rect& band : bands) {
        if (!objects.empty()) objects += ",";
        objects += bandObject(id++, band.x, band.y, band.w, band.h, difficulty, mobs);
    }
    const std::string path = writeTiledFixture("flix_banded_world.tmj", fixtureMapBody(objects));
    MapData map;
    map.setId("banded");
    std::string error;
    if (!map.loadTiled(path, error)) {
        std::fprintf(stderr, "[test] the banded fixture world did not load: %s\n", error.c_str());
    }
    std::remove(path.c_str());
    out.adoptSingle(map);
}

int bandTarget(const Rect& bounds) {
    // Parenthesised exactly as SpawnSystem::rebuildZones has it -- density
    // times AREA -- because the two associations round differently: a 6000-unit
    // square comes out 90 one way and 90.000000000000014 (so 91 after the
    // ceiling) the other, and this figure is compared against a live count.
    return std::max(1, static_cast<int>(std::ceil(kTargetMobDensity * (bounds.w * bounds.h))));
}

Vec2 firstBandCentre(const MapData& map) {
    for (const MapElement& element : map.elements()) {
        if (!element.isSpawnBand()) continue;
        return {element.bounds.x + element.bounds.w * 0.5,
                element.bounds.y + element.bounds.h * 0.5};
    }
    return {};
}

/// The authored overworld the band tests run against.
///
///   * a mob REGION over the whole world, naming the ant hell's roster;
///   * a big difficulty-0 band in the top-left, with one target-dummy band per
///     rung nested inside it -- the DPS row, which is the one thing on the map
///     that is spawned because a band NAMES it rather than because a group
///     rolled it;
///   * a difficulty-66 hornet band (legendary ground), so a named row exists at
///     a tier the difficulty-0 ground around it would never produce;
///   * a difficulty-83 and a difficulty-100 plot -- mythic and ultra ground --
///     well away from the region test's viewer, so one test's high-tier mobs
///     are not another's neighbourhood.
const MapData& authoredMap() {
    static const MapData map = [] {
        std::string objects = regionObject(1, 0, 0, 60000, 60000, "ant_hell 100%");
        objects += "," + bandObject(2, 1000, 1000, 16000, 16000, 0.0, "");
        // One dummy band per rung of the ladder, in a row inside the band
        // above. The difficulties are the curve's own:
        // dummyRowDifficulties()[i] is the difficulty whose tier value is
        // exactly i, so that band grows nothing but tier i.
        int id = 10;
        for (int i = 0; i < kDummyRowRungs; ++i) {
            objects += "," + bandObject(id++, 1500.0 + i * 1600.0, 1500.0, 1200.0, 1200.0,
                                        dummyRowDifficulties()[static_cast<std::size_t>(i)],
                                        "target_dummy 100%");
        }
        objects += "," + bandObject(30, 2000, 8000, 6000, 6000,
                                    difficultyForTierValue(rarityIndex(Rarity::Legendary)),
                                    "hornet 100%");
        // kCentre (30000, 30000) is inside the first of these, so the
        // neighbourhood tests have a band over them.
        objects += "," + bandObject(40, 24000, 24000, 12000, 12000,
                                    difficultyForTierValue(rarityIndex(Rarity::Mythic)), "");
        objects += "," + bandObject(41, 46000, 2000, 10000, 10000, 100.0, "");

        const std::string path = writeTiledFixture("authored.tmj", fixtureMapBody(objects));
        MapData out;
        out.setId("authored");
        std::string error;
        if (!out.loadTiled(path, error)) {
            std::fprintf(stderr, "[test] the authored fixture map did not load: %s\n",
                         error.c_str());
        }
        std::remove(path.c_str());
        return out;
    }();
    return map;
}

const WorldMaps& authoredMaps() {
    static const WorldMaps maps = [] {
        WorldMaps m;
        m.adoptSingle(authoredMap());
        return m;
    }();
    return maps;
}

/// Every band row that names a mob, so the test asserts against the map
/// rather than against a hard-coded list that the map is free to outgrow.
struct NamedRow {
    Rect bounds;
    double difficulty = 0.0;
    std::string mobType;
};

std::vector<NamedRow> namedBandRows() {
    std::vector<NamedRow> rows;
    for (const MapElement& element : authoredMap().elements()) {
        if (!element.isSpawnBand()) continue;
        for (const ZoneMobEntry& entry : element.mobDistribution) {
            // A row is a group or a mob; only the mobs are of interest here.
            if (shipped().mobGroupIndex(entry.name) != kInvalidIndex) continue;
            rows.push_back(NamedRow{element.bounds, element.difficulty, entry.name});
        }
    }
    return rows;
}

} // namespace

TEST(a_band_keeps_the_mob_it_names) {
    if (!authoredMap().loaded()) {
        ::testing::reportFailure(__FILE__, __LINE__, "the authored fixture map did not load");
        return;
    }
    const std::vector<NamedRow> rows = namedBandRows();
    // Nine dummy rows plus the legendary-ground hornets. A parser that drops
    // the name leaves this at zero, which is exactly the bug this guards.
    CHECK(rows.size() >= 10);

    int dummyRows = 0;
    for (const NamedRow& row : rows) {
        if (row.mobType == "target_dummy") ++dummyRows;
    }
    CHECK_EQ(dummyRows, 9);
}

TEST(the_dummy_bands_actually_build_the_dps_row) {
    if (!authoredMap().loaded()) {
        ::testing::reportFailure(__FILE__, __LINE__, "the authored fixture map did not load");
        return;
    }
    const std::uint16_t dummy = shipped().mobIndex("target_dummy");
    CHECK(dummy != kInvalidIndex);
    // The premise of the whole mechanism: the group roll will never produce
    // one, so if the band does not, nothing does.
    CHECK(shipped().mob(dummy).neverAmbient);
    CHECK(shipped().mob(dummy).groups.empty());

    Sim sim;
    sim.spawner.worldMaps = &authoredMaps();

    // Standing in the common dummy band, which sits inside the big common
    // spawn band: two bands over one square, and the inner one has to be the
    // one that answers for what grows there.
    const Rect band = [&] {
        for (const NamedRow& row : namedBandRows()) {
            if (row.mobType == "target_dummy") return row.bounds;
        }
        return Rect{};
    }();
    const Vec2 at{band.x + band.w * 0.5, band.y + band.h * 0.5};

    const std::vector<Vec2> players{at};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    int dummies = 0;
    std::vector<Rarity> tiers;
    std::vector<Vec2> where;
    Query<MobTag, MobType, Transform> mobs{sim.world};
    mobs.each([&](Entity, MobTag&, MobType& type, Transform& transform) {
        if (type.configIndex != dummy) return;
        ++dummies;
        tiers.push_back(type.rarity);
        where.push_back(transform.position);
    });
    CHECK(dummies > 0);
    CHECK_EQ(dummyRowDifficulties().size(), std::size_t{kDummyRowRungs});

    // Permanent and unkillable, so a duplicate would stand there forever: one
    // of each rarity per section, no more.
    for (std::size_t i = 0; i < tiers.size(); ++i) {
        for (std::size_t j = i + 1; j < tiers.size(); ++j) {
            if (tiers[i] == tiers[j]) {
                ::testing::reportFailure(__FILE__, __LINE__,
                                         std::string("two ") + rarityName(tiers[i]) +
                                             " dummies: " + std::to_string(where[i].x) + "," +
                                             std::to_string(where[i].y) + " and " +
                                             std::to_string(where[j].x) + "," +
                                             std::to_string(where[j].y));
            }
        }
    }

    // And every tier is one a dummy band's DIFFICULTY actually produces. The
    // dummy bands between them cover common..unique and the viewport sampled
    // above overlaps several, so the set is wider than one band -- but a tier
    // no band's difficulty can reach would fall outside it, and each of these
    // bands sits on an exact rung of the curve, so its blend is one tier with
    // no fraction in it.
    std::vector<Rarity> declared;
    for (const NamedRow& row : namedBandRows()) {
        if (row.mobType != "target_dummy") continue;
        const TierMix mix = tierMixForDifficulty(row.difficulty);
        declared.push_back(mix.lower);
        if (mix.upperChance > 0.0) declared.push_back(mix.upper);
    }
    for (const Rarity tier : tiers) {
        CHECK(std::find(declared.begin(), declared.end(), tier) != declared.end());
    }
}

TEST(a_target_dummy_is_smaller_than_the_wild_mob_of_its_tier) {
    const std::uint16_t dummy = shipped().mobIndex("target_dummy");
    if (dummy == kInvalidIndex) return;

    // A common dummy matches a common mob exactly; by unique it is three
    // quarters of one. Straight tier scaling turns the top of the DPS row into
    // a wall, which is why the reference ramps it (src/mobs.ts:212).
    Sim sim;
    Rng rng(99);
    const Entity common =
        sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), dummy, Rarity::Common,
                             kCentre, Realm::Overworld, 0.0, rng);
    const Entity unique =
        sim.spawner.spawnMob(sim.world, sim.terrain, shipped(), dummy, Rarity::Unique,
                             kCentre + Vec2{4000.0, 0.0}, Realm::Overworld, 0.0, rng);
    CHECK(common != NULL_ENTITY);
    CHECK(unique != NULL_ENTITY);

    CHECK_NEAR(sim.world.get<MobType>(common).sizeJitter, 1.0, 1e-12);
    CHECK_NEAR(sim.world.get<MobType>(unique).sizeJitter, 0.75, 1e-12);
    CHECK_NEAR(sim.world.get<Body>(unique).radius,
               shipped().mobStats(dummy, Rarity::Unique).radius * 0.75, 1e-9);
}

// ---------------------------------------------------------------------------
// Zone mob distributions
// ---------------------------------------------------------------------------
//
// A zone says WHAT it spawns as weighted rows of section presets and named
// mobs. These drive the whole path -- the authored string, through the Tiled
// map, into MapData, into a running spawn pass -- because the parser agreeing
// with itself proves nothing about which mobs come out.

namespace {

/// Writes a one-zone Tiled map whose spawn polygon covers a 14000-unit square,
/// carrying `distribution` verbatim as its `mobs` property. Difficulty 0, so
/// every mob it grows is a common and these tests are about WHICH mob. Same one-cell
/// shape as fixtureMapBody() above -- a map must name a tileset and carry a
/// tile layer to be a map, and the Sim brings its own flat terrain.
std::string writeZoneMap(const std::string& name, const std::string& distribution) {
    const std::string polygon =
        R"({"id": 1, "class": "spawn", "x": 2000, "y": 2000,
            "polygon": [{"x":0,"y":0},{"x":14000,"y":0},{"x":14000,"y":14000},{"x":0,"y":14000}],
            "properties": [
              {"name": "difficulty", "type": "int", "value": 0},
              {"name": "mobs", "type": "string", "value": ")" + distribution + R"("}
            ]})";
    return writeTiledFixture(name, fixtureMapBody(polygon));
}

/// Every ambient mob a band has put on the map, by config id: the entities and
/// the records alike. Which of the two a given mob is right now is a fact about
/// where the flower is standing, not about what the band grows.
std::vector<std::string> spawnedMobIds(Sim& sim) {
    std::vector<std::string> ids;
    Query<MobTag, MobType> mobs{sim.world};
    mobs.each([&](Entity, MobTag&, MobType& type) {
        ids.push_back(shipped().mob(type.configIndex).id);
    });
    for (const SpawnSystem::LatentSite& site : sim.latent()) {
        ids.push_back(shipped().mob(site.mobIndex).id);
    }
    return ids;
}

} // namespace

TEST(a_zone_that_names_a_mob_spawns_only_that_mob) {
    const std::string path = writeZoneMap("flix_zone_named.tmj", "hornet 100%");
    MapData map;
    std::string error;
    if (!map.loadTiled(path, error) || map.elements().size() != 1) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK(map.elements()[0].mobDistribution.size() == 1);

    Sim sim;
    WorldMaps maps;
    maps.adoptSingle(map);
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{{9000, 9000}};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    const std::vector<std::string> ids = spawnedMobIds(sim);
    CHECK(!ids.empty());
    for (const std::string& id : ids) CHECK_EQ(id, std::string("hornet"));
    std::remove(path.c_str());
}

TEST(a_zone_group_borrows_another_biomes_roster) {
    // The zone sits in the top-left corner, and asks for the Ocean. This is
    // the whole point of a group: mobs.json's groups are a palette a band can
    // draw from, rather than nine places the mobs are stuck in.
    const std::string path = writeZoneMap("flix_zone_group.tmj", "ocean 100%");
    MapData map;
    std::string error;
    if (!map.loadTiled(path, error) || map.elements().empty()) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK_EQ(map.elements()[0].mobDistribution[0].name, std::string("ocean"));
    const std::uint16_t ocean = shipped().mobGroupIndex("ocean");
    CHECK(ocean != kInvalidIndex);

    Sim sim;
    WorldMaps maps;
    maps.adoptSingle(map);
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{{9000, 9000}};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    const std::vector<std::string> ids = spawnedMobIds(sim);
    CHECK(!ids.empty());
    // Ocean's roster, and none of the Garden's -- a bee here would mean the
    // group was ignored and something else rolled instead.
    for (const std::string& id : ids) {
        const std::uint16_t index = shipped().mobIndex(id);
        CHECK(index != kInvalidIndex);
        bool member = false;
        for (const MobGroupMember& entry : shipped().mob(index).groups) member |= entry.group == ocean;
        CHECK(member);
    }
    std::remove(path.c_str());
}

TEST(a_distribution_splits_in_roughly_the_authored_proportion) {
    // 80/20, over enough spawns that a working split cannot look like a broken
    // one. The bound is loose on purpose: this is asserting that the weights
    // are honoured at all, not pinning the RNG.
    const std::string path = writeZoneMap("flix_zone_split.tmj", "hornet 80% bee 20%");
    MapData map;
    std::string error;
    if (!map.loadTiled(path, error) || map.elements().empty()) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }

    Sim sim;
    WorldMaps maps;
    maps.adoptSingle(map);
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{{9000, 9000}};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    int hornets = 0;
    int bees = 0;
    for (const std::string& id : spawnedMobIds(sim)) {
        if (id == "hornet") ++hornets;
        else if (id == "bee") ++bees;
        else CHECK_EQ(id, std::string("hornet"));   // nothing else may appear
    }
    CHECK(hornets + bees > 20);
    CHECK(hornets > bees);
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Realm isolation
// ---------------------------------------------------------------------------
//
// Every map is its own coordinate space. A band on a biome map is stocked for
// the flower standing on THAT map, through that map's own walls, and the
// overworld's own census never reads another map's numbers as its own. Each of these pins one place the spawner used to compare positions
// across realms.

namespace {

/// writeZoneMap()'s band -- one common band naming hornets over
/// (2000,2000)-(16000,16000) -- loaded as `realm`.
bool loadHornetBandAs(const std::string& name, Realm realm, MapData& out) {
    const std::string path = writeZoneMap(name, "hornet 100%");
    std::string error;
    const bool ok = out.loadTiled(path, error, realm) && out.elements().size() == 1;
    if (!ok) std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
    std::remove(path.c_str());
    return ok;
}

/// A flat, open `side`-tile grid installed as `realm`'s map.
void installOpenGrid(Terrain& terrain, Realm realm, int side) {
    std::vector<std::uint8_t> tiles(static_cast<std::size_t>(side) * side,
                                    static_cast<std::uint8_t>(Tile::Ground));
    CHECK(terrain.setTiles(tiles, side, side, realm));
}

int mobsInRealm(World& world, Realm realm) {
    int n = 0;
    Query<MobTag, Transform> mobs{world};
    mobs.each([&](Entity, MobTag&, Transform& t) { n += t.realm == realm ? 1 : 0; });
    return n;
}

} // namespace

TEST(the_border_band_is_measured_against_the_maps_own_extent) {
    // The band of ground along the map edge is off limits to spawning: a mob
    // standing in it is half inside the boundary wall. It is a fraction of the
    // MAP's size, and the map says what that is.
    //
    // This used to be measured against a compile-time 60000-unit square, which
    // was the overworld's size when there was only ever one shape of overworld.
    // The shipped map is 19200 units across, so the right and bottom bands sat
    // forty thousand units outside the world and never rejected anything --
    // mobs spawned flush against those two walls while the left and top were
    // correctly refused. A small map makes it obvious: here the whole map is
    // 24 tiles, so the far band is most of the way across an old constant's
    // idea of the world.
    Sim sim;
    const int side = 24;   // 7200 units
    installOpenGrid(sim.terrain, Realm::Overworld, side);
    const Vec2 extent = sim.terrain.realmExtent(Realm::Overworld);
    CHECK_NEAR(extent.x, side * kTileSize, 1e-6);

    // One band over the whole little map, so there is something to place at
    // all: the question here is WHERE a band fill may put a mob, not what.
    WorldMaps maps;
    makeBandedWorld(maps, {Rect{0.0, 0.0, extent.x, extent.y}}, "garden 100%");
    sim.spawner.worldMaps = &maps;

    // Flowers hugging the FAR two walls, which is the half of the band an
    // oversized constant stops guarding -- the near walls are refused either
    // way and are the control.
    std::vector<Vec2> players;
    for (int i = 1; i <= 3; ++i) {
        players.push_back({extent.x - 150.0, extent.y * i / 4.0});
        players.push_back({extent.x * i / 4.0, extent.y - 150.0});
    }
    for (int i = 0; i < 600; ++i) sim.tick(players);

    int placed = 0;
    int inNearBand = 0;
    int inFarBand = 0;
    const auto judge = [&](Vec2 at) {
        ++placed;
        if (at.x < kWorldBoundaryThreshold || at.y < kWorldBoundaryThreshold) ++inNearBand;
        if (at.x > extent.x - kWorldBoundaryThreshold ||
            at.y > extent.y - kWorldBoundaryThreshold) {
            ++inFarBand;
        }
    };
    Query<MobTag, Transform> mobs{sim.world};
    mobs.each([&](Entity e, MobTag&, Transform& transform) {
        if (transform.realm != Realm::Overworld) return;
        // What the BAND placed. A nest's escorts ring the nest and a long mob's
        // segments trail its head, so neither is a point this pass ever sampled
        // -- and a centipede reversing into the edge wall is movement, not
        // placement.
        if (sim.isChildOfAPlacedMob(e)) return;
        judge(transform.position);
    });
    // The records too, and they are the larger half: the rule under test is
    // about where a band may SAMPLE, and most of what it samples never becomes
    // an entity while one flower stands there.
    for (const SpawnSystem::LatentSite& site : sim.latent()) judge(site.position);
    // Enough mobs that an empty far band means the rule held rather than that
    // nothing was placed at all.
    CHECK(placed >= 40);
    CHECK_EQ(inNearBand, 0);
    CHECK_EQ(inFarBand, 0);

}

TEST(a_band_on_another_map_is_stocked_through_that_maps_own_terrain) {
    // Two maps with the SAME band at the SAME numbers: the overworld's and a
    // second map's. The flower stands on the second map; only its band may
    // fill, and it must fill through the second map's grid -- the overworld
    // is walled over under those numbers, which used to starve it.
    const Realm other = worldRealm(1);
    MapData overworld;
    MapData second;
    if (!loadHornetBandAs("flix_band_realm0.tmj", Realm::Overworld, overworld) ||
        !loadHornetBandAs("flix_band_realm1.tmj", other, second)) {
        CHECK(false);
        return;
    }
    std::vector<MapData> maps;
    maps.push_back(std::move(overworld));
    maps.push_back(std::move(second));
    WorldMaps worldMaps;
    worldMaps.adoptMaps(std::move(maps));
    CHECK(worldMaps.forRealm(other) != nullptr);

    Sim sim;
    sim.spawner.worldMaps = &worldMaps;
    installOpenGrid(sim.terrain, other, 60);   // 18000 units: the band fits
    // The overworld under the whole band is solid.
    for (int ty = 6; ty <= 54; ++ty) {
        for (int tx = 6; tx <= 54; ++tx) sim.terrain.setTile(tx, ty, Tile::Wall, Realm::Overworld);
    }
    CHECK(sim.terrain.blocked({9000, 9000}, Realm::Overworld));
    CHECK(!sim.terrain.blocked({9000, 9000}, other));

    const std::vector<RealmPoint> players{{{9000, 9000}, other}};
    for (int i = 0; i < 400; ++i) {
        sim.spawner.run(sim.world, sim.terrain, shipped(), players, sim.rng, sim.now,
                        net::kTickSeconds, sim.commands);
        sim.commands.flush();
        sim.now += net::kTickMillis;
    }

    // Stocked on the second map, and nowhere else: the overworld's band at
    // the same numbers has nobody looking at it.
    CHECK(mobsInRealm(sim.world, other) > 0);
    CHECK_EQ(mobsInRealm(sim.world, Realm::Overworld), 0);

    // Every one on that map's own open ground, inside the band, and spaced
    // against the others placed in the same pass -- the placement record has
    // to carry the realm for that.
    const Rect band = worldMaps.forRealm(other)->elements()[0].bounds;
    std::vector<Vec2> placed;
    Query<MobTag, Transform, Body> mobs{sim.world};
    mobs.each([&](Entity, MobTag&, Transform& t, Body& body) {
        CHECK(t.realm == other);
        CHECK(!sim.terrain.blocked(t.position, other));
        CHECK(band.contains(t.position));
        for (const Vec2 earlier : placed) {
            CHECK(distance(earlier, t.position) >= body.radius);   // never stacked
        }
        placed.push_back(t.position);
    });

    // And the flower's own presence keeps them: nobody on the overworld means
    // nothing there to recycle, and the band's mobs are near their viewer.
    sim.spawner.run(sim.world, sim.terrain, shipped(), players, sim.rng,
                    sim.now + kMobDespawnDelayMillis + 1000.0, 0.0, sim.commands);
    sim.commands.flush();
    CHECK(mobsInRealm(sim.world, other) > 0);
}

TEST(a_flower_on_another_map_does_not_stock_the_overworld_at_its_numbers) {
    // The overworld's band alone, and a flower standing at the band's numbers
    // on a map that has no band at all: nothing may spawn anywhere.
    MapData overworld;
    if (!loadHornetBandAs("flix_band_only_realm0.tmj", Realm::Overworld, overworld)) {
        CHECK(false);
        return;
    }
    const Realm other = worldRealm(1);
    std::vector<MapData> maps;
    maps.push_back(std::move(overworld));
    maps.push_back(MapData{});
    WorldMaps worldMaps;
    worldMaps.adoptMaps(std::move(maps));

    Sim sim;
    sim.spawner.worldMaps = &worldMaps;
    installOpenGrid(sim.terrain, other, 60);
    const std::vector<RealmPoint> players{{{9000, 9000}, other}};
    for (int i = 0; i < 200; ++i) {
        sim.spawner.run(sim.world, sim.terrain, shipped(), players, sim.rng, sim.now,
                        net::kTickSeconds, sim.commands);
        sim.commands.flush();
        sim.now += net::kTickMillis;
    }
    CHECK_EQ(sim.mobCount(), 0);
}

TEST(a_hard_band_is_where_every_boss_in_the_world_comes_from) {
    // THE INVARIANT that replaced the boss pass. There is no scheduler keeping
    // one ultra alive in the world and one super per section any more: a band's
    // DIFFICULTY is the only thing that produces a high-tier mob, so a
    // difficulty-200 band is full of supers and a map with no hard band has no
    // bosses at all. Those two rules cannot coexist with a pass that rations
    // one ultra to the whole world, which is why the pass is gone.
    //
    // Also checks what survived: the ANNOUNCEMENT. A super, unique or apex is
    // queued for the chat channel from the band fill, which is now the spawn
    // path that produces them.
    const std::string path = writeTiledFixture(
        "flix_hard_band.tmj",
        fixtureMapBody(bandObject(1, 2000, 2000, 14000, 14000, 200.0, "hornet 100%")));
    MapData map;
    std::string error;
    if (!map.loadTiled(path, error) || map.elements().size() != 1) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK(map.elements()[0].isSpawnBand());
    CHECK_NEAR(map.elements()[0].difficulty, 200.0, 1e-9);

    Sim sim;
    WorldMaps maps;
    maps.adoptSingle(map);
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{{9000, 9000}};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    int mobs = 0;
    Query<MobTag, MobType> live{sim.world};
    live.each([&](Entity, MobTag&, MobType& type) {
        ++mobs;
        // Difficulty 200 is the super anchor: nothing else may come out of it.
        if (type.rarity != Rarity::Super) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     std::string("a difficulty-200 band spawned a ") +
                                         rarityName(type.rarity));
        }
    });
    CHECK(mobs > 0);
    // And every one of them was worth announcing. The queue is bounded, so this
    // is "some were queued", not "one per mob".
    CHECK(!sim.spawner.bossSpawns.empty());
    for (const SpawnSystem::BossSpawn& boss : sim.spawner.bossSpawns) {
        CHECK(rarityIndex(boss.rarity) >= rarityIndex(kAnnouncedRarity));
        CHECK(boss.realm == Realm::Overworld);
    }
    std::remove(path.c_str());
}

TEST(an_announced_boss_carries_the_map_it_spawned_on) {
    // The chat line is personalised: a player in the boss's own SECTION is told
    // it spawned, everyone else that it spawned somewhere. A section is the
    // OVERWORLD's grid, so the boss's position only means anything once its
    // realm is known -- and since the boss pass was deleted, a band on ANY
    // staged map can queue one. Without the realm, a super filling a band on
    // map two was announced as "here" to flowers standing at the same numbers
    // on the overworld, which they cannot even reach.
    const Realm other = worldRealm(1);
    const std::string path = writeTiledFixture(
        "flix_hard_band_realm1.tmj",
        fixtureMapBody(bandObject(1, 2000, 2000, 14000, 14000, 200.0, "hornet 100%")));
    MapData second;
    std::string error;
    if (!second.loadTiled(path, error, other) || second.elements().size() != 1) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    std::vector<MapData> maps;
    maps.push_back(MapData{});   // an empty overworld: no band, no bosses
    maps.push_back(std::move(second));
    WorldMaps worldMaps;
    worldMaps.adoptMaps(std::move(maps));

    Sim sim;
    sim.spawner.worldMaps = &worldMaps;
    installOpenGrid(sim.terrain, other, 60);
    const std::vector<RealmPoint> players{{{9000, 9000}, other}};
    for (int i = 0; i < 400; ++i) {
        sim.spawner.run(sim.world, sim.terrain, shipped(), players, sim.rng, sim.now,
                        net::kTickSeconds, sim.commands);
        sim.commands.flush();
        sim.now += net::kTickMillis;
    }
    CHECK(mobsInRealm(sim.world, other) > 0);
    CHECK(!sim.spawner.bossSpawns.empty());
    for (const SpawnSystem::BossSpawn& boss : sim.spawner.bossSpawns) {
        CHECK(boss.realm == other);
    }
    std::remove(path.c_str());
}

TEST(a_soft_band_announces_nothing) {
    // The other half of the announcement rule: ordinary ground is silent, so
    // the chat line still means something. Difficulty 0 is fully common.
    const std::string path = writeTiledFixture(
        "flix_soft_band.tmj",
        fixtureMapBody(bandObject(1, 2000, 2000, 14000, 14000, 0.0, "hornet 100%")));
    MapData map;
    std::string error;
    if (!map.loadTiled(path, error)) {
        CHECK(false);
        return;
    }
    Sim sim;
    WorldMaps maps;
    maps.adoptSingle(map);
    sim.spawner.worldMaps = &maps;
    const std::vector<Vec2> players{{9000, 9000}};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    int mobs = 0;
    Query<MobTag, MobType> live{sim.world};
    live.each([&](Entity, MobTag&, MobType& type) {
        ++mobs;
        CHECK(type.rarity == Rarity::Common);
    });
    CHECK(mobs > 0);
    CHECK(sim.spawner.bossSpawns.empty());
    std::remove(path.c_str());
}

TEST(the_shipped_map_never_spawns_above_the_difficulty_its_ground_declares) {
    // The SHIPPED data, end to end. The bands the author has brushed on are the
    // only ground that grows anything at all, and each one may only be as hard
    // as its own difficulty says. This is the sanity check on the whole curve:
    // the band over the one door is difficulty 0, so a fresh flower walks out
    // into commons.
    //
    // The ceiling is computed FROM THE FILE for each mob's own position, rather
    // than written down here, so an author raising a band's difficulty does not
    // make this test wrong -- it makes it check the new number.
    if (!shippedMaps().forRealm(Realm::Overworld)) {
        ::testing::reportFailure(__FILE__, __LINE__, "the shipped maps did not load");
        return;
    }
    const MapData& world = *shippedMaps().forRealm(Realm::Overworld);

    Sim sim;
    sim.spawner.worldMaps = &shippedMaps();
    // Standing on one of the map's own bands; the Sim brings its own flat
    // terrain, so this is about the tier, not about walls.
    const std::vector<Vec2> players{firstBandCentre(world)};
    for (int i = 0; i < 300; ++i) sim.tick(players);

    // The hardest ground the file declares anywhere, and the ceiling that buys.
    // Nothing in the world may exceed it, wherever it has since wandered to.
    // Zero when the map carries no band at all -- which would also mean no
    // mobs, and the loop below would have nothing to walk.
    double hardest = 0.0;
    for (const MapElement& element : world.elements()) {
        if (element.isSpawnBand()) hardest = std::max(hardest, element.difficulty);
    }
    const int worldCeiling = rarityIndex(tierMixForDifficulty(hardest).upper);

    // How far a mob may have walked since it was placed. A mob is judged
    // against its OWN square only when it is this far inside that square's
    // ground -- otherwise it may have been born under a band and strolled out,
    // and the square it is standing on now never chose it.
    const double kWanderMargin = 4000.0;
    const auto clearOfEveryBand = [&](Vec2 at) {
        for (const MapElement& element : world.elements()) {
            if (!element.isSpawnBand()) continue;
            if (element.bounds.x - kWanderMargin < at.x && at.x < element.bounds.right() + kWanderMargin &&
                element.bounds.y - kWanderMargin < at.y && at.y < element.bounds.bottom() + kWanderMargin) {
                return false;
            }
        }
        return true;
    };

    int mobs = 0;
    int onDefaultGround = 0;
    int onBandedGround = 0;
    Query<MobTag, MobType, Transform> live{sim.world};
    live.each([&](Entity e, MobTag&, MobType& type, Transform& transform) {
        // An escort is not an ambient spawn, and the ground it stands on did not
        // choose it: a nest is placed by a band at that band's tier and then
        // lays its brood out on a ring around itself, which routinely reaches
        // over the band's edge onto ground no band covers. The nest answers for
        // the tier; the ring is just where the children fit. Same exemption --
        // and the same reason -- as the roster test above.
        if (sim.world.has<HoleTether>(e)) return;
        const double difficulty = sim.spawner.difficultyAt(transform.realm, transform.position);
        // The hardest thing this square can roll at neutral luck: the upper half
        // of its blend. A mob's own min_rarity still floors it above that -- that
        // is the mob's property, not the ground's.
        const TierMix mix = tierMixForDifficulty(difficulty);
        const MobConfig& config = shipped().mob(type.configIndex);
        // A body segment is the rest of the same animal as its head, laid out
        // along it and over whatever ground that reaches; the head was judged on
        // its own square.
        if (config.id.find("_body") != std::string::npos) return;
        ++mobs;
        if (rarityIndex(type.rarity) > std::max(worldCeiling, rarityIndex(config.minRarity))) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "the shipped map spawned a " + std::string(rarityName(type.rarity)) +
                                         " " + config.id + ", above anything the hardest ground on "
                                         "the map (difficulty " + std::to_string(hardest) + ") can roll");
        }
        if (difficulty != 0.0 || !clearOfEveryBand(transform.position)) {
            ++onBandedGround;
            return;
        }
        ++onDefaultGround;
        // Well away from every band, so whatever put this mob here, it is not
        // standing on ground that could have rolled it anything but a common.
        // Difficulty zero is FULLY common: not "mostly", and not "common unless
        // something drifted it".
        (void)mix;
        if (rarityIndex(type.rarity) > rarityIndex(config.minRarity)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "the shipped map spawned a " + std::string(rarityName(type.rarity)) +
                                         " " + config.id + " on difficulty-zero ground");
        }
    });
    CHECK(mobs > 0);
    // Every live mob was judged against a difficulty rather than skipped by a
    // lookup that found nothing. WHICH arm each fell into is the author's
    // business -- a mob born in a difficulty-0 band and one that wandered clear
    // of every band are both "difficulty zero" here.
    CHECK_EQ(onDefaultGround + onBandedGround, mobs);
    // The announcement queue. Whether the map is hard enough to announce
    // ANYTHING is the author's decision -- a band at difficulty 100 is ultras
    // with a couple of supers in it, and the super is the announcement -- so
    // what is pinned is the rule rather than the outcome: a line is queued only
    // for a rarity that both clears the announcement threshold and is one the
    // hardest ground on the map could actually roll. A queue with something
    // below kAnnouncedRarity in it is chat spam; one with something above
    // worldCeiling in it is a mob that came from nowhere on this map.
    if (worldCeiling < rarityIndex(kAnnouncedRarity)) {
        CHECK(sim.spawner.bossSpawns.empty());
    }
    for (const SpawnSystem::BossSpawn& announced : sim.spawner.bossSpawns) {
        CHECK(rarityIndex(announced.rarity) >= rarityIndex(kAnnouncedRarity));
        if (rarityIndex(announced.rarity) > worldCeiling) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "the shipped map announced a " +
                                         std::string(rarityName(announced.rarity)) + " " +
                                         shipped().mob(announced.mobIndex).id +
                                         ", above anything its hardest ground (difficulty " +
                                         std::to_string(hardest) + ") can roll");
        }
    }
}

TEST(a_band_is_the_only_ground_that_grows_anything) {
    // THE RULE, stated on one map. Its `defaultMobGroup` says garden, and one
    // difficulty-0 band is drawn on it. Inside the band: commons, from the
    // map's default group -- the band names no roster of its own, which is the
    // "difficulty and distribution are orthogonal" half of the rule. Outside
    // it, on ground the author drew nothing on: NOTHING, however long a flower
    // stands there.
    //
    // This test used to be `a_bands_difficulty_beats_the_maps_default`: the map
    // carried a `defaultDifficulty` of 100 and the second half asserted that
    // the open ground around the band grew ultras. That property is gone --
    // after the density fill was deleted nothing rolled against it, so a number
    // for "the ground no band covers" configured nothing -- and what the second
    // half pins now is that the same ground grows no mob at all.
    const std::string properties =
        R"({"name": "defaultMobGroup", "type": "string", "value": "garden"})";
    const std::string band = bandObject(1, 20000, 20000, 6000, 6000, 0.0, "");
    const std::string path =
        writeTiledFixture("flix_one_band.tmj", fixtureMapBody(band, properties));
    MapData map;
    std::string error;
    if (!map.loadTiled(path, error)) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK_EQ(map.defaultMobGroup(), std::string("garden"));
    CHECK(map.elements().size() == 1);
    CHECK(map.elements()[0].isSpawnBand());
    std::remove(path.c_str());

    WorldMaps maps;
    maps.adoptSingle(map);
    const std::uint16_t garden = shipped().mobGroupIndex("garden");
    CHECK(garden != kInvalidIndex);

    // Inside the band: commons, and drawn from the map's default group, because
    // the band names nothing of its own.
    {
        Sim sim;
        sim.spawner.worldMaps = &maps;
        const std::vector<Vec2> players{{23000, 23000}};
        for (int i = 0; i < 400; ++i) sim.tick(players);
        int mobs = 0;
        Query<MobTag, MobType, Transform> live{sim.world};
        live.each([&](Entity e, MobTag&, MobType& type, Transform& t) {
            if (!map.elements()[0].contains(t.position)) return;
            ++mobs;
            const MobConfig& config = shipped().mob(type.configIndex);
            if (rarityIndex(type.rarity) > rarityIndex(config.minRarity)) {
                ::testing::reportFailure(__FILE__, __LINE__,
                                         "a difficulty-0 band grew a " +
                                             std::string(rarityName(type.rarity)) + " " +
                                             config.id);
            }
            // The default group answered for the roster. An escort is the
            // hole's choice rather than the group's, and a body segment is in
            // no group at all.
            if (sim.isChildOfAPlacedMob(e) ||
                config.id.find("_body") != std::string::npos) {
                return;
            }
            bool member = false;
            for (const MobGroupMember& entry : config.groups) member |= entry.group == garden;
            if (!member) {
                ::testing::reportFailure(__FILE__, __LINE__,
                                         "a band with no roster of its own grew " + config.id +
                                             ", which is not in the map's default group");
            }
        });
        CHECK(mobs > 0);
    }

    // Outside every band, on the same map, for as long as the band took to
    // fill: nothing. Not commons, not one stray -- nothing samples this ground.
    {
        Sim sim;
        sim.spawner.worldMaps = &maps;
        const std::vector<Vec2> players{{40000, 40000}};
        for (int i = 0; i < 400; ++i) sim.tick(players);
        CHECK_EQ(sim.mobCount(), 0);
        CHECK_EQ(sim.spawner.census().spawnedTotal, 0);
    }
}

TEST(a_harness_with_no_map_at_all_grows_nothing) {
    // The `worldMaps == nullptr` contract, pinned. No maps means no bands, and
    // no bands means no mobs -- a bare-Terrain harness gets an EMPTY world and
    // has to place what it wants itself through spawnMob().
    //
    // Worth its own test because the opposite used to be true: the deleted
    // density fill reached chooseRegionMobAt with no map in hand and rolled
    // over every mob group so that such a harness got mobs anyway. A test
    // written against that behaviour now measures an empty world and passes
    // vacuously, so this is the one that states which way round it is.
    Sim sim;
    CHECK(sim.spawner.worldMaps == nullptr);
    const std::vector<Vec2> players{{9000, 9000}, {30000, 30000}};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    CHECK_EQ(sim.mobCount(), 0);
    CHECK_EQ(sim.spawner.census().spawnedTotal, 0);
}

TEST(a_map_with_no_band_at_all_grows_nothing) {
    // The consequence stated on its own, because it is the one that looks like
    // a bug: a map an author has drawn no band on is EMPTY. Not thinly
    // populated -- empty. The map here has everything else a map can have to
    // say about its mobs (a default group, and a mob region drawn over the
    // whole of it) and still grows nothing, because a region owns no population
    // and there is no longer a pass that fills the ground beside a band.
    const std::string properties =
        R"({"name": "defaultMobGroup", "type": "string", "value": "garden"})";
    const std::string region = regionObject(1, 0, 0, 60000, 60000, "garden 100%");
    const std::string path =
        writeTiledFixture("flix_no_band.tmj", fixtureMapBody(region, properties));
    MapData map;
    std::string error;
    if (!map.loadTiled(path, error)) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    std::remove(path.c_str());
    CHECK_EQ(map.defaultMobGroup(), std::string("garden"));
    CHECK(map.elements().size() == 1);
    CHECK(map.elements()[0].isMobRegion());
    bool anyBand = false;
    for (const MapElement& element : map.elements()) anyBand |= element.isSpawnBand();
    CHECK(!anyBand);
    // The load report has to say it out loud, because an empty world reads as a
    // broken spawner rather than as an unfinished map. This is the line the
    // author sees at boot and the only warning they get, so it is pinned here
    // rather than trusted: bandSummary() is exactly what the `[map]` line
    // prints for the bands clause.
    CHECK_EQ(map.bandSummary(), std::string("NO SPAWN BANDS -- no mobs will spawn on this map"));

    WorldMaps maps;
    maps.adoptSingle(map);
    Sim sim;
    sim.spawner.worldMaps = &maps;
    // Three flowers spread over the map, long enough for many fill passes.
    const std::vector<Vec2> players{{9000, 9000}, {30000, 30000}, {48000, 12000}};
    for (int i = 0; i < 600; ++i) sim.tick(players);

    CHECK_EQ(sim.mobCount(), 0);
    CHECK_EQ(sim.spawner.census().mobs, 0);
    CHECK_EQ(sim.spawner.census().spawnedTotal, 0);
}

TEST(every_ambient_mob_on_the_shipped_map_stands_inside_a_band) {
    // THE INVARIANT, driven over the game's own data with the real spawn pass.
    //
    // Every mob in the world is inside one of the bands the author drew, or is
    // the escort or body segment of one that is -- a hole's brood rings the
    // hole and a centipede's body trails its head, and both legitimately reach
    // over the band's edge onto ground nothing fills. There is no third case:
    // "somewhere else entirely" is what the deleted density fill used to
    // produce, and this is the test that would catch it coming back.
    if (!shippedMaps().forRealm(Realm::Overworld)) {
        ::testing::reportFailure(__FILE__, __LINE__, "the shipped maps did not load");
        return;
    }
    const MapData& world = *shippedMaps().forRealm(Realm::Overworld);

    // Every band, in map order. The author moves them; this reads them.
    std::vector<const MapElement*> bands;
    for (const MapElement& element : world.elements()) {
        if (element.isSpawnBand()) bands.push_back(&element);
    }
    if (bands.empty()) {
        // A legitimate state for the data to be in -- and then the map grows
        // nothing, which the bandless test above already pins. Nothing to
        // measure here.
        return;
    }

    Sim sim;
    sim.spawner.worldMaps = &shippedMaps();
    // One flower on each band, so every one of them is in view and fills.
    std::vector<Vec2> players;
    for (const MapElement* band : bands) {
        players.push_back({band->bounds.x + band->bounds.w * 0.5,
                           band->bounds.y + band->bounds.h * 0.5});
    }
    for (int i = 0; i < 400; ++i) sim.tick(players);

    int inABand = 0;
    int children = 0;
    int loose = 0;
    Query<MobTag, Transform> live{sim.world};
    live.each([&](Entity e, MobTag&, Transform& transform) {
        bool covered = false;
        for (const MapElement* band : bands) covered |= band->contains(transform.position);
        if (covered) {
            ++inABand;
            return;
        }
        if (sim.isChildOfAPlacedMob(e)) {
            ++children;
            return;
        }
        ++loose;
        ::testing::reportFailure(__FILE__, __LINE__,
                                 "an ambient mob stands on ground no band covers, at " +
                                     std::to_string(transform.position.x) + "," +
                                     std::to_string(transform.position.y));
    });
    CHECK(inABand > 0);
    CHECK_EQ(loose, 0);
    // Reported rather than asserted: whether the shipped roster has a nest or a
    // centipede in it on any given day is the author's business.
    std::printf("  shipped map: %d mobs inside a band, %d escorts/segments outside one, %d loose\n",
                inABand, children, loose);
}

TEST(a_neverambient_mob_never_comes_from_a_group_roll_however_hard_the_ground) {
    // The target dummy is in no group and is marked neverAmbient, so the group
    // roll can never produce one at any difficulty: the only way it reaches the
    // world is a band naming it outright (the DPS row above). Difficulty is a
    // new axis; it must not become a new back door.
    const ContentRegistry& content = shipped();
    const std::uint16_t dummy = content.mobIndex("target_dummy");
    CHECK(dummy != kInvalidIndex);
    CHECK(content.mob(dummy).neverAmbient);

    SpawnSystem spawner;
    Rng rng(5150);
    for (std::size_t group = 0; group < content.mobGroupCount(); ++group) {
        for (int tier = 0; tier < kRarityCount; ++tier) {
            for (int i = 0; i < 200; ++i) {
                const std::uint16_t rolled = spawner.chooseGroupMob(
                    content, static_cast<std::uint16_t>(group), clampRarity(tier), rng);
                CHECK(rolled != dummy);
            }
        }
    }
}
