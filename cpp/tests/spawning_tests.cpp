#include "test.h"

#include "server/systems/loot.h"
#include "server/systems/spawning.h"

#include <sys/stat.h>

#include <algorithm>
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
                      "section":[0],"spawn_weight":1},
            "beta":  {"name":"Beta","health":10,"damage":1,"size":1,"speed":1,
                      "section":[0],"spawn_weight":3},
            "gamma": {"name":"Gamma","health":10,"damage":1,"size":1,"speed":1,
                      "section":[0],"spawn_weight":6},
            "delta": {"name":"Delta","health":10,"damage":1,"size":1,"speed":1,
                      "section":[1],"spawn_weight":100},
            "ghost": {"name":"Ghost","health":10,"damage":1,"size":1,"speed":1,
                      "section":[0],"spawn_weight":0}
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
        const std::uint16_t picked = spawner.chooseMobType(content, 0, rng);
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

TEST(a_section_with_nothing_in_it_yields_no_mob_type) {
    const ContentRegistry& content = synthetic();
    SpawnSystem spawner;
    Rng rng(1);
    // Section 1 holds only delta; sections 2..8 hold nothing at all.
    CHECK_EQ(spawner.chooseMobType(content, 1, rng), content.mobIndex("delta"));
    for (int section = 2; section < kSectionCount; ++section) {
        CHECK_EQ(spawner.chooseMobType(content, section, rng), kInvalidIndex);
    }
    // Out-of-range section indices are answered, not asserted on: the caller
    // derives them from a position that may be outside the map.
    CHECK_EQ(spawner.chooseMobType(content, -1, rng), kInvalidIndex);
    CHECK_EQ(spawner.chooseMobType(content, kSectionCount, rng), kInvalidIndex);
}

TEST(natural_rarity_drift_can_reach_ultra_but_no_higher) {
    const ContentRegistry& content = shipped();
    const MobConfig& bee = content.mob(content.mobIndex("bee"));
    Rng rng(9001);

    bool sawCommon = false;
    bool sawMythic = false;
    bool sawUltra = false;
    for (int i = 0; i < 20000; ++i) {
        const Rarity r = SpawnSystem::rollRarity(bee, rng);
        CHECK(rarityIndex(r) <= rarityIndex(Rarity::Ultra));
        sawCommon = sawCommon || r == Rarity::Common;
        sawMythic = sawMythic || r == Rarity::Mythic;
        sawUltra = sawUltra || r == Rarity::Ultra;
    }
    // The tails of the table are reachable, so the ceiling above is a real
    // bound and not an artefact of never rolling high.
    CHECK(sawCommon);
    CHECK(sawMythic);
    CHECK(sawUltra);
}

TEST(natural_rarity_respects_min_rarity) {
    const ContentRegistry& content = shipped();
    const MobConfig& evil = content.mob(content.mobIndex("evil_centipede"));
    CHECK_EQ(evil.minRarity, Rarity::Rare);

    Rng rng(77);
    for (int i = 0; i < 5000; ++i) {
        const Rarity r = SpawnSystem::rollRarity(evil, rng);
        CHECK(rarityIndex(r) >= rarityIndex(Rarity::Rare));
        CHECK(rarityIndex(r) <= rarityIndex(Rarity::Ultra));
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

TEST(population_converges_to_the_target_near_a_player) {
    Sim sim;
    const std::vector<Vec2> players{kCentre};

    for (int i = 0; i < 400; ++i) sim.tick(players);

    const int near = sim.mobsWithin(kCentre, kSpawnRingMax + kSpawnScatterRadius);
    CHECK(near >= kMobsPerPlayer);
    // Nest escorts can push a little past the target; nothing may push past the
    // section cap.
    CHECK(near <= kMaxMobsPerSection);
    CHECK(sim.spawner.census().mobs <= kSectionTargetPopulation);

    // And it holds: a converged population does not keep creeping upward.
    const int settled = sim.mobCount();
    for (int i = 0; i < 400; ++i) sim.tick(players);
    CHECK(sim.mobCount() <= settled + kMaxNestChildren);
}

TEST(ambient_mobs_spawn_inside_the_buffered_viewport) {
    Sim sim;
    // Section 6 (the sewers) is the one neighbourhood with no nests in it, so
    // every mob here came from the ambient roll and the ring bound is exact --
    // an escort is deliberately placed next to its nest and would not be.
    const Vec2 sewers{10000.0, 50000.0};
    const std::vector<Vec2> players{sewers};
    for (int i = 0; i < 200; ++i) sim.tick(players);

    Query<MobTag, Transform> mobs{sim.world};
    int checked = 0;
    mobs.each([&](Entity, MobTag&, Transform& t) {
        ++checked;
        const double d = distance(t.position, sewers);
        CHECK(d >= kMinSpawnDistance);
        CHECK(std::abs(t.position.x - sewers.x) <=
              kSpawnViewportHalfWidth + kSpawnScatterRadius);
        CHECK(std::abs(t.position.y - sewers.y) <=
              kSpawnViewportHalfHeight + kSpawnScatterRadius);
    });
    CHECK(checked > 0);
}

TEST(a_crowd_of_players_cannot_exceed_the_global_cap) {
    Sim sim;
    std::vector<Vec2> players;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            players.push_back(Vec2{3500.0 + x * 7500.0, 3500.0 + y * 7500.0});
        }
    }
    // Sixty-four TypeScript-sized neighbourhoods want 1024 mobs between them.
    CHECK(static_cast<int>(players.size()) * kMobsPerPlayer > kMaxLiveMobs);

    for (int i = 0; i < 600; ++i) sim.tick(players);

    CHECK(sim.mobCount() <= kMaxLiveMobs + kMaxNestChildren);
    CHECK(sim.spawner.census().mobs > kMobsPerPlayer);
    for (int section = 0; section < kSectionCount; ++section) {
        CHECK(sim.spawner.census().perSection[static_cast<std::size_t>(section)] <= kMaxMobsPerSection);
    }
}

TEST(mobs_nobody_has_been_near_are_recycled) {
    Sim sim;
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
    // viewport box is tested, and a mob outside all of them is recycled once
    // it has been unseen for the grace period.
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

TEST(section_filtering_follows_the_config) {
    Sim sim;
    const std::vector<Vec2> players{kCentre};
    for (int i = 0; i < 300; ++i) sim.tick(players);

    Query<MobTag, MobType> mobs{sim.world};
    int checked = 0;
    mobs.each([&](Entity, MobTag&, MobType& type) {
        ++checked;
        const MobConfig& config = shipped().mob(type.configIndex);
        // Section 4 is the whole neighbourhood: the spawn ring is 2400 units
        // and the section is 20000 across.
        CHECK((config.sectionMask & (1u << 4)) != 0);
    });
    CHECK(checked > 0);
}

TEST(no_mob_is_placed_inside_a_wall) {
    Sim sim;
    // A solid block of wall that the spawn ring overlaps. Solid rather than
    // scattered so the test asserts the invariant instead of the push-out
    // solver's tolerance for pathological geometry.
    for (int ty = 96; ty <= 104; ++ty) {
        for (int tx = 103; tx <= 109; ++tx) sim.terrain.setTile(tx, ty, Tile::Wall);
    }
    const Vec2 player = Terrain::tileCenter(100, 100);
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
    // Counted off the nest rather than off the world: the ambient filler is
    // running too, and its spawns are nothing to do with this hole.
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
// Biome spawn tables
// ---------------------------------------------------------------------------
//
// The map does not only say which TIER belongs where. A biome may name the mob
// outright, and that is the only way a mob the ambient roll refuses ever
// reaches the world: `target_dummy` declares no spawn weight and is marked
// neverAmbient, so the four dummy biomes are the whole of its existence. A
// parser that keeps the tier and drops the name leaves the DPS row unbuilt and
// nothing else about the world looks wrong.

namespace {

std::string mapBundlePath() {
    return firstExisting({testsDir() + "/../data/map_bundle.ts",
                          testsDir() + "/../../src/map_bundle.ts", "data/map_bundle.ts",
                          "../src/map_bundle.ts", "src/map_bundle.ts"});
}

const MapData& shippedMap() {
    static const MapData map = [] {
        MapData m;
        std::string error;
        m.load(mapBundlePath(), error);
        return m;
    }();
    return map;
}

/// Every biome row that names a mob, so the test asserts against the map
/// rather than against a hard-coded list that the map is free to outgrow.
struct NamedRow {
    Rect bounds;
    Rarity tier;
    std::string mobType;
};

std::vector<NamedRow> namedBiomeRows() {
    std::vector<NamedRow> rows;
    for (const MapElement& element : shippedMap().elements()) {
        if (element.kind != MapElementKind::Biome) continue;
        for (const BiomeSpawnEntry& entry : element.spawnTable) {
            if (entry.mobType.empty()) continue;
            rows.push_back(NamedRow{element.bounds, entry.tier, entry.mobType});
        }
    }
    return rows;
}

} // namespace

TEST(a_biome_spawn_table_keeps_the_mob_it_names) {
    if (!shippedMap().loaded()) {
        ::testing::reportFailure(__FILE__, __LINE__,
                                 "no map bundle at " + mapBundlePath());
        return;
    }
    const std::vector<NamedRow> rows = namedBiomeRows();
    // Nine dummy rows plus the legendary hornet. A parser that drops `mobType`
    // leaves this at zero, which is exactly the bug this guards.
    CHECK(rows.size() >= 10);

    int dummyRows = 0;
    for (const NamedRow& row : rows) {
        if (row.mobType == "target_dummy") ++dummyRows;
    }
    CHECK_EQ(dummyRows, 9);
}

TEST(the_dummy_biomes_actually_build_the_dps_row) {
    if (!shippedMap().loaded()) return;
    const std::uint16_t dummy = shipped().mobIndex("target_dummy");
    CHECK(dummy != kInvalidIndex);
    // The premise of the whole mechanism: the ambient roll will never produce
    // one, so if the biome table does not, nothing does.
    CHECK(shipped().mob(dummy).neverAmbient);

    Sim sim;
    sim.spawner.mapData = &shippedMap();

    // Standing in the common/uncommon dummy biome, which sits inside the big
    // common spawn rectangle -- so it is the ZONE fill that has to honour it,
    // not the neighbourhood fill.
    const Rect biome = [&] {
        for (const NamedRow& row : namedBiomeRows()) {
            if (row.mobType == "target_dummy") return row.bounds;
        }
        return Rect{};
    }();
    const Vec2 at{biome.x + biome.w * 0.5, biome.y + biome.h * 0.5};

    const std::vector<Vec2> players{at};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    int dummies = 0;
    std::vector<Rarity> tiers;
    Query<MobTag, MobType> mobs{sim.world};
    mobs.each([&](Entity, MobTag&, MobType& type) {
        if (type.configIndex != dummy) return;
        ++dummies;
        tiers.push_back(type.rarity);
    });
    CHECK(dummies > 0);

    // Permanent and unkillable, so a duplicate would stand there forever: one
    // of each rarity per section, no more.
    for (std::size_t i = 0; i < tiers.size(); ++i) {
        for (std::size_t j = i + 1; j < tiers.size(); ++j) {
            CHECK(tiers[i] != tiers[j]);
        }
    }

    // And every tier is one a dummy row actually declares. The four dummy
    // biomes between them cover common..unique and the big common rectangle
    // sampled above overlaps several, so the set is wider than one biome --
    // but a DRIFTED tier would fall outside it, and drift is exactly what a
    // permanent fixture must not take (a tier nothing asked for is one the
    // one-per-section check never cleared).
    std::vector<Rarity> declared;
    for (const NamedRow& row : namedBiomeRows()) {
        if (row.mobType == "target_dummy") declared.push_back(row.tier);
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

/// Writes a one-zone Tiled map whose spawn polygon covers most of section 0,
/// carrying `distribution` verbatim as its `mobs` property.
std::string writeZoneMap(const std::string& name, const std::string& distribution) {
    const std::string path = std::string("/tmp/") + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    // One tile cell, because MapData only wants the annotations and a map has
    // to have a tile layer to be a map. Terrain is the thing that insists on a
    // full 200x200 grid, and these tests give the Sim its own flat one.
    out << R"({
      "type": "map", "orientation": "orthogonal", "infinite": false,
      "width": 1, "height": 1, "tilewidth": 300, "tileheight": 300,
      "tilesets": [], "layers": [
        {"type": "tilelayer", "name": "terrain", "width": 1, "height": 1, "data": [0]},
        {"type": "objectgroup", "name": "spawns", "objects": [
          {"id": 1, "class": "spawn", "x": 2000, "y": 2000,
           "polygon": [{"x":0,"y":0},{"x":14000,"y":0},{"x":14000,"y":14000},{"x":0,"y":14000}],
           "properties": [
             {"name": "spawnType", "type": "string", "value": "common"},
             {"name": "mobs", "type": "string", "value": ")"
        << distribution << R"("}
           ]}
        ]}
      ]})";
    return path;
}

/// Every ambient mob in the world, by config id.
std::vector<std::string> spawnedMobIds(Sim& sim) {
    std::vector<std::string> ids;
    Query<MobTag, MobType> mobs{sim.world};
    mobs.each([&](Entity, MobTag&, MobType& type) {
        ids.push_back(shipped().mob(type.configIndex).id);
    });
    return ids;
}

} // namespace

TEST(a_zone_that_names_a_mob_spawns_only_that_mob) {
    const std::string path = writeZoneMap("flix_zone_named.tmj", "hornet 100%");
    MapData map;
    std::string error;
    if (!map.loadWorldMap(path, error) || map.elements().size() != 1) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK(map.elements()[0].mobDistribution.size() == 1);

    Sim sim;
    sim.spawner.mapData = &map;
    const std::vector<Vec2> players{{9000, 9000}};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    const std::vector<std::string> ids = spawnedMobIds(sim);
    CHECK(!ids.empty());
    for (const std::string& id : ids) CHECK_EQ(id, std::string("hornet"));
    std::remove(path.c_str());
}

TEST(a_zone_preset_borrows_another_sections_roster) {
    // The zone sits in the Garden, and asks for the Ocean. This is the whole
    // point of a preset: the nine mob-spawn sections become a palette a zone
    // can draw from, rather than nine places the mobs are stuck in.
    const std::string path = writeZoneMap("flix_zone_preset.tmj", "ocean 100%");
    MapData map;
    std::string error;
    if (!map.loadWorldMap(path, error) || map.elements().empty()) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK(map.elements()[0].mobDistribution[0].presetSection == 3);

    Sim sim;
    sim.spawner.mapData = &map;
    const std::vector<Vec2> players{{9000, 9000}};
    for (int i = 0; i < 400; ++i) sim.tick(players);

    const std::vector<std::string> ids = spawnedMobIds(sim);
    CHECK(!ids.empty());
    // Ocean's roster, and none of the Garden's -- a bee here would mean the
    // preset was ignored and the point's own section rolled instead.
    for (const std::string& id : ids) {
        const std::uint16_t index = shipped().mobIndex(id);
        CHECK(index != kInvalidIndex);
        CHECK(shipped().mobStats(index, Rarity::Common).spawnsIn(3));
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
    if (!map.loadWorldMap(path, error) || map.elements().empty()) {
        std::fprintf(stderr, "[test] %s did not load: %s\n", path.c_str(), error.c_str());
        CHECK(false);
        return;
    }

    Sim sim;
    sim.spawner.mapData = &map;
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
