#include "server/systems/mode_spawning.h"

#include <algorithm>
#include <cmath>

namespace flix {

int ModeSpawner::mazePopulationTarget() {
    const double raw = std::round(activeMaze().floorCellCount() * kMazeMobsPerFloorCell);
    return static_cast<int>(std::min<double>(kMazeMaxMobs, raw));
}

void ModeSpawner::resolvePools(const ContentRegistry& content) {
    if (!arenaPoolReady_) {
        arenaPool_.clear();
        for (const ArenaMobEntry& entry : kArenaMobPool) {
            const std::uint16_t index = content.mobIndex(entry.id);
            // A pool entry the content does not define is skipped, not fatal:
            // the ring still fills from whatever it does define.
            if (index == kInvalidIndex) continue;
            arenaPool_.push_back({index, entry.weight});
        }
        arenaPoolReady_ = true;
    }

    const MazeBiome biome = activeMaze().biome();
    if (!mazePoolReady_ || mazePoolBiome_ != biome) {
        mazePool_.clear();
        // The maze borrows the roster of the overworld section its biome is
        // drawn from: a garden maze is full of garden mobs.
        const int section = kMazeBiomeSections[static_cast<std::size_t>(biome)];
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << section);
        for (std::uint16_t i = 0; i < content.mobCount(); ++i) {
            const MobConfig& config = content.mob(i);
            if ((config.sectionMask & bit) == 0) continue;
            bool excluded = false;
            for (const char* id : kMazeExcludedMobs) {
                if (config.id == id) excluded = true;
            }
            // A body segment is only ever laid out behind its head.
            for (std::uint16_t h = 0; h < content.mobCount() && !excluded; ++h) {
                if (content.mob(h).segmentBodyIndex == i) excluded = true;
            }
            if (excluded) continue;
            mazePool_.push_back({i, config.spawnWeight > 0.0 ? config.spawnWeight : 1.0});
        }
        mazePoolBiome_ = biome;
        mazePoolReady_ = true;
    }
}

std::uint16_t ModeSpawner::pickWeighted(const std::vector<PoolEntry>& pool, Rng& rng) const {
    double total = 0.0;
    for (const PoolEntry& entry : pool) total += entry.weight;
    if (!(total > 0.0)) return kInvalidIndex;
    double roll = rng.unit() * total;
    for (const PoolEntry& entry : pool) {
        roll -= entry.weight;
        if (roll <= 0.0) return entry.mobIndex;
    }
    return pool.back().mobIndex;
}

Rarity ModeSpawner::rollArenaTier(Rng& rng) const {
    double total = 0.0;
    for (const double w : kArenaTierWeights) total += w;
    double roll = rng.unit() * total;
    for (std::size_t i = 0; i < kArenaTierWeights.size(); ++i) {
        roll -= kArenaTierWeights[i];
        if (roll <= 0.0) return static_cast<Rarity>(i);
    }
    return Rarity::Mythic;
}

bool ModeSpawner::nearPlayer(Realm realm, Vec2 at, double gap) const {
    for (const RealmPoint& player : players_) {
        if (player.realm != realm) continue;
        if (distanceSq(player.position, at) < gap * gap) return true;
    }
    return false;
}

bool ModeSpawner::nearMob(World& world, const SpatialGrid& grid, Realm realm, Vec2 at,
                          double radius, double gap) {
    // Own radius plus clearance; the grid has already accounted for every
    // other body's size by filing it fat.
    grid.query(realm, at, radius + gap, candidates_);
    for (const Entity candidate : candidates_) {
        if (!world.has<MobTag>(candidate)) continue;
        const Transform* transform = world.tryGet<Transform>(candidate);
        const Body* body = world.tryGet<Body>(candidate);
        if (transform == nullptr || body == nullptr) continue;
        const double minDist = radius + body->radius + gap;
        if (distanceSq(transform->position, at) < minDist * minDist) return true;
    }
    return false;
}

bool ModeSpawner::mazeBodyFits(const Maze& maze, Vec2 at, double radius) {
    if (!maze.isFloor(at)) return false;
    const double r = std::min(radius, kMazeCellSize - 10.0);
    return maze.isFloor({at.x - r, at.y}) && maze.isFloor({at.x + r, at.y}) &&
           maze.isFloor({at.x, at.y - r}) && maze.isFloor({at.x, at.y + r});
}

void ModeSpawner::count(World& world, const std::vector<RealmPoint>& players) {
    census_ = Census{};
    players_ = players;
    for (const RealmPoint& player : players_) {
        if (player.realm == Realm::Arena) ++census_.arenaPlayers;
        if (player.realm == Realm::Maze) ++census_.mazePlayers;
    }
    census_.mazeTarget = mazePopulationTarget();

    Query<MobTag, Transform, MobType> mobs{world};
    mobs.without<Dead, Pet>();
    mobs.each([&](Entity e, MobTag&, Transform& transform, MobType& type) {
        if (transform.realm == Realm::Arena) ++census_.arenaMobs;
        if (transform.realm != Realm::Maze) return;
        ++census_.mazeMobs;
        // A centipede's body shares its head's tier, and only the head counts
        // as a boss -- or one ultra centipede would satisfy the boss cap
        // several times over and starve the other room.
        if (world.has<BodySegment>(e) && !world.get<BodySegment>(e).head) return;
        if (rarityIndex(type.rarity) >= rarityIndex(Rarity::Ultra)) ++census_.mazeBosses;
    });
}

void ModeSpawner::run(World& world, const Terrain& terrain, const ContentRegistry& content,
                      SpawnSystem& spawner, const SpatialGrid& grid,
                      const std::vector<RealmPoint>& players, Rng& rng, double nowMillis) {
    if (nowMillis < nextPassMillis_) return;
    nextPassMillis_ = nowMillis + kModeSpawnIntervalMillis;

    resolvePools(content);
    count(world, players);

    if (census_.arenaPlayers > 0) runArena(world, terrain, content, spawner, grid, rng, nowMillis);
    if (census_.mazePlayers > 0) {
        runMaze(world, terrain, content, spawner, grid, rng, nowMillis);
        runMazeBosses(world, terrain, content, spawner, rng, nowMillis);
    }
}

void ModeSpawner::runArena(World& world, const Terrain& terrain, const ContentRegistry& content,
                           SpawnSystem& spawner, const SpatialGrid& grid, Rng& rng,
                           double nowMillis) {
    if (arenaPool_.empty()) return;
    const int target = std::min(kArenaMaxMobs, census_.arenaPlayers * kArenaMobsPerPlayer);
    const int needed = std::min(kArenaSpawnsPerPass, target - census_.arenaMobs);
    for (int i = 0; i < needed; ++i) {
        const std::uint16_t mobIndex = pickWeighted(arenaPool_, rng);
        if (mobIndex == kInvalidIndex) break;
        const Rarity tier = rollArenaTier(rng);
        const double radius = content.mobStats(mobIndex, tier).radius;
        // Uniform over the disc the body fits in, as the reference samples it.
        const double maxR = kArenaRadius - radius - kArenaSpawnEdgeMargin;
        if (maxR <= 0.0) continue;
        bool placed = false;
        for (int attempt = 0; attempt < 20 && !placed; ++attempt) {
            const Vec2 at = kArenaCentre + rng.insideCircle(maxR);
            if (nearPlayer(Realm::Arena, at, kArenaSpawnPlayerGap)) continue;
            if (nearMob(world, grid, Realm::Arena, at, radius, kArenaSpawnMobGap)) continue;
            if (spawner.spawnMob(world, terrain, content, mobIndex, tier, at, Realm::Arena,
                                 nowMillis, rng) != NULL_ENTITY) {
                ++census_.arenaMobs;
                placed = true;
            }
        }
    }
}

void ModeSpawner::runMaze(World& world, const Terrain& terrain, const ContentRegistry& content,
                          SpawnSystem& spawner, const SpatialGrid& grid, Rng& rng,
                          double nowMillis) {
    if (mazePool_.empty()) return;
    const Maze& maze = activeMaze();
    const int dim = maze.gridDim();
    if (dim <= 0) return;

    const int needed = std::min(kMazeSpawnsPerPass, census_.mazeTarget - census_.mazeMobs);
    for (int i = 0; i < needed; ++i) {
        const std::uint16_t mobIndex = pickWeighted(mazePool_, rng);
        if (mobIndex == kInvalidIndex) break;
        // Placed at the common size and re-checked at the tier's: higher tiers
        // are much bigger than the estimate the placement was made with.
        const double commonRadius = content.mobStats(mobIndex, Rarity::Common).radius;

        for (int attempt = 0; attempt < kMazeSpawnAttempts; ++attempt) {
            const int gx = static_cast<int>(rng.below(static_cast<std::uint32_t>(dim)));
            const int gy = static_cast<int>(rng.below(static_cast<std::uint32_t>(dim)));
            if (maze.cellValue(gx, gy) != 1) continue;   // plain floor cells only
            const Vec2 at{kMazeOriginX + (gx + 0.2 + rng.unit() * 0.6) * kMazeCellSize,
                          kMazeOriginY + (gy + 0.2 + rng.unit() * 0.6) * kMazeCellSize};
            if (!mazeBodyFits(maze, at, commonRadius)) continue;
            if (nearPlayer(Realm::Maze, at, kMazeSpawnPlayerGap)) continue;
            if (nearMob(world, grid, Realm::Maze, at, commonRadius, kMazeSpawnMobGap)) continue;

            // Depth zone -> tier, with a little jitter either way. Never above
            // mythic: ultras are the boss pass's alone.
            const int zone = maze.zoneOfCell(gx, gy);
            if (zone < 0) continue;
            int tierIndex = zone;
            const double roll = rng.unit();
            if (roll < kMazeTierUpChance) tierIndex = std::min(kMazeZoneCount - 1, tierIndex + 1);
            else if (roll < kMazeTierUpChance + kMazeTierDownChance) tierIndex = std::max(0, tierIndex - 1);
            const Rarity tier = clampRarity(tierIndex);

            const double tierRadius = content.mobStats(mobIndex, tier).radius;
            if (tierRadius > commonRadius && !mazeBodyFits(maze, at, tierRadius)) continue;

            if (spawner.spawnMob(world, terrain, content, mobIndex, tier, at, Realm::Maze,
                                 nowMillis, rng) != NULL_ENTITY) {
                ++census_.mazeMobs;
            }
            break;
        }
    }
}

void ModeSpawner::runMazeBosses(World& world, const Terrain& terrain,
                                const ContentRegistry& content, SpawnSystem& spawner, Rng& rng,
                                double nowMillis) {
    if (mazePool_.empty()) return;
    const Maze& maze = activeMaze();
    int toSpawn = kMazeBossCount - census_.mazeBosses;
    if (toSpawn <= 0) return;

    for (const Vec2 spot : maze.bossSpots()) {
        if (toSpawn <= 0) break;
        // Never on someone's head, and one boss per room.
        if (nearPlayer(Realm::Maze, spot, kMazeBossPlayerGap)) continue;
        bool bossHere = false;
        Query<MobTag, Transform, MobType> mobs{world};
        mobs.without<Dead, Pet>();
        mobs.each([&](Entity e, MobTag&, Transform& transform, MobType& type) {
            if (bossHere || transform.realm != Realm::Maze) return;
            if (rarityIndex(type.rarity) < rarityIndex(Rarity::Ultra)) return;
            if (world.has<BodySegment>(e) && !world.get<BodySegment>(e).head) return;
            if (distanceSq(transform.position, spot) < kMazeBossRoomRadius * kMazeBossRoomRadius) {
                bossHere = true;
            }
        });
        if (bossHere) continue;

        const std::uint16_t mobIndex = pickWeighted(mazePool_, rng);
        if (mobIndex == kInvalidIndex) break;
        // spawnMob lays a centipede's body out behind its head by itself.
        if (spawner.spawnMob(world, terrain, content, mobIndex, Rarity::Ultra, spot, Realm::Maze,
                             nowMillis, rng) == NULL_ENTITY) {
            continue;
        }
        ++census_.mazeBosses;
        --toSpawn;
    }
}

void ModeSpawner::clearMaze(World& world, CommandBuffer& commands) {
    doomed_.clear();
    Query<MobTag, Transform> mobs{world};
    mobs.each([&](Entity e, MobTag&, Transform& transform) {
        if (transform.realm == Realm::Maze) doomed_.push_back(e);
    });
    // Destroyed, not killed: a mob removed by the rotation was not fought, so
    // it pays out nothing.
    for (const Entity e : doomed_) commands.destroy(e);
    mazePoolReady_ = false;
}

} // namespace flix
