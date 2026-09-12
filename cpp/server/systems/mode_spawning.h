#pragma once
// Populating the two realms that are not an authored map.
//
// spawning.h populates a MAP, and it does it from the spawn bands its author
// drew: ground no band covers grows nothing there. That rule cannot reach the
// arena or the maze, because neither is authored -- both are GENERATED, they
// carry no object layer, and there is nowhere to draw a band. They are bounded
// rooms, and the reference populates them WHOLE
// (src/server/pvpArenaSpawner.ts, src/server/mazeSpawner.ts): the arena holds
// a fixed crowd that scales with the flowers fighting in it, and the maze
// carries the reference density across every corridor at once, so exploring
// deeper always finds something rather than a bubble around wherever the
// player happened to walk in.
//
// So this system is deliberately exempt from "no band, no mobs". If a realm
// here ever became a hand-authored map, its population would move to bands and
// this file would stop covering it.
//
// This system decides WHAT goes WHERE and WHEN; building the entity is still
// SpawnSystem::spawnMob's job, which is what keeps a maze mob the same object
// an overworld one is -- same components, same drops, same AI. The mobs it
// makes carry AmbientMob like any other, and SpawnSystem's census keeps them
// for as long as anyone is in their realm, then lets them drain.

#include <array>
#include <cstdint>
#include <vector>

#include "server/systems/spawning.h"
#include "shared/core/types.h"
#include "shared/core/world.h"
#include "shared/game/config.h"
#include "shared/game/rarity.h"
#include "shared/game/realm.h"
#include "shared/game/spatial.h"
#include "shared/game/terrain.h"

namespace flix {

// ---------------------------------------------------------------------------
// Arena tuning (src/server/pvpArenaSpawner.ts)
// ---------------------------------------------------------------------------

/// Garden-themed mobs plus the spider, which is deliberately the rarest -- it
/// is the standout threat in the ring.
struct ArenaMobEntry {
    const char* id;
    double weight;
};
inline constexpr std::array<ArenaMobEntry, 7> kArenaMobPool = {{
    {"bee", 3}, {"ladybug", 3}, {"rock", 2}, {"dandelion", 2},
    {"soldier_ant", 2}, {"hornet", 1}, {"spider", 1},
}};
inline constexpr std::array<double, 6> kArenaTierWeights = {{0.40, 0.30, 0.18, 0.08, 0.03, 0.01}};

inline constexpr int kArenaMobsPerPlayer = 12;
inline constexpr int kArenaMaxMobs = 60;
inline constexpr double kArenaSpawnPlayerGap = 300.0;
inline constexpr double kArenaSpawnMobGap = 80.0;
/// Mobs stay fully inside the ring: this much off the boundary on top of the
/// body's own radius.
inline constexpr double kArenaSpawnEdgeMargin = 40.0;
inline constexpr int kArenaSpawnsPerPass = 3;

// ---------------------------------------------------------------------------
// Maze tuning (src/server/mazeSpawner.ts)
// ---------------------------------------------------------------------------

/// Mobs per floor cell: the reference density (kTargetMobDensity, which is
/// also what a map's bands are sized by) over one cell's area, so a corridor
/// carries exactly a stocked band's mobs per walkable unit.
inline constexpr double kMazeMobsPerFloorCell = kTargetMobDensity * kMazeCellSize * kMazeCellSize;
/// Runaway guard above the derived target.
inline constexpr int kMazeMaxMobs = 1500;
/// No spawn lands on screen: farther than a viewport from every maze flower.
inline constexpr double kMazeSpawnPlayerGap = 1200.0;
inline constexpr double kMazeSpawnMobGap = 100.0;
inline constexpr int kMazeSpawnsPerPass = 40;
inline constexpr int kMazeSpawnAttempts = 40;
/// Ultras are the maze bosses, kept alive in the deepest rooms.
inline constexpr int kMazeBossCount = 2;
inline constexpr double kMazeBossPlayerGap = 1200.0;
inline constexpr double kMazeBossRoomRadius = 2000.0;
/// The chance the depth tier drifts one up, and one down, per spawn.
inline constexpr double kMazeTierUpChance = 0.08;
inline constexpr double kMazeTierDownChance = 0.20;

/// Types that never spawn in the maze whatever their section list says: wave
/// spawners would flood the corridors, and the utility mobs make no sense
/// there. Centipede bodies are excluded by structure -- a body only ever
/// follows a head.
inline constexpr std::array<const char*, 5> kMazeExcludedMobs = {{
    "ant_hole", "fire_ant_hole", "target_dummy", "item_spawner", "garbage",
}};

/// Both passes run on the population cadence: the arena's crowd and the
/// maze's corridors are counted, and topped up by a handful, twice a second.
inline constexpr double kModeSpawnIntervalMillis = kPopulationIntervalMillis;

class ModeSpawner {
public:
    /// What the last pass counted, for tests and the console.
    struct Census {
        int arenaPlayers = 0;
        int arenaMobs = 0;
        int mazePlayers = 0;
        int mazeMobs = 0;
        int mazeBosses = 0;
        int mazeTarget = 0;
    };

    /// `players` is every connected flower with a body, in whatever realm; the
    /// pass keeps the ones in the arena and the maze. `grid` is the tick's
    /// broadphase, for the too-close checks -- at maze density a linear scan
    /// per placement attempt is what made the reference's fill quadratic.
    void run(World& world, const Terrain& terrain, const ContentRegistry& content,
             SpawnSystem& spawner, const SpatialGrid& grid,
             const std::vector<RealmPoint>& players, Rng& rng, double nowMillis);

    /// Destroys every mob in the maze. Called on rotation: yesterday's mobs
    /// would be standing inside today's walls.
    void clearMaze(World& world, CommandBuffer& commands);

    /// Runs both passes on the next call regardless of the clock.
    void requestImmediatePass() { nextPassMillis_ = 0; }

    const Census& census() const { return census_; }

    /// The maze's live population target for the active layout.
    static int mazePopulationTarget();

private:
    struct PoolEntry {
        std::uint16_t mobIndex = 0;
        double weight = 1.0;
    };

    void resolvePools(const ContentRegistry& content);
    std::uint16_t pickWeighted(const std::vector<PoolEntry>& pool, Rng& rng) const;

    void count(World& world, const std::vector<RealmPoint>& players);
    void runArena(World& world, const Terrain& terrain, const ContentRegistry& content,
                  SpawnSystem& spawner, const SpatialGrid& grid, Rng& rng, double nowMillis);
    void runMaze(World& world, const Terrain& terrain, const ContentRegistry& content,
                 SpawnSystem& spawner, const SpatialGrid& grid, Rng& rng, double nowMillis);
    void runMazeBosses(World& world, const Terrain& terrain, const ContentRegistry& content,
                       SpawnSystem& spawner, Rng& rng, double nowMillis);

    bool nearPlayer(Realm realm, Vec2 at, double gap) const;
    bool nearMob(World& world, const SpatialGrid& grid, Realm realm, Vec2 at, double radius,
                 double gap);
    /// The centre and four compass points all on floor: the reference's
    /// mazeBodyFits.
    static bool mazeBodyFits(const Maze& maze, Vec2 at, double radius);
    Rarity rollArenaTier(Rng& rng) const;

    std::vector<PoolEntry> arenaPool_;
    std::vector<PoolEntry> mazePool_;
    bool arenaPoolReady_ = false;
    MazeBiome mazePoolBiome_ = MazeBiome::Garden;
    bool mazePoolReady_ = false;

    std::vector<RealmPoint> players_;
    std::vector<Entity> candidates_;
    std::vector<Entity> doomed_;
    Census census_;
    double nextPassMillis_ = 0;
};

} // namespace flix
