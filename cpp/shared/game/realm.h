#pragma once
// Realms: the game's separate coordinate systems.
//
// The overworld, the PVP arena and the daily maze are three different places,
// and each one has its own coordinate space with its own origin at (0, 0). An
// entity carries WHICH space its position is in (Transform::realm), and nothing
// in one realm can see, touch, target, push or pick up anything in another:
// the broadphase keeps one grid per realm, replication only streams a viewer
// its own realm, and every terrain question is asked about a realm.
//
// The reference put the arena at (150000, 150000) and the maze at
// (200000, 200000) inside ONE world space, and lived with what that costs: a
// world clamp that drags a maze body back to the map's edge unless every
// clamp remembers the exception, a section lookup that answers nonsense for
// a far-off point, a minimap that has nothing to draw, f32 positions on the
// wire losing precision past 2^17, and a broadphase whose border cells collect
// everything outside the map. Separate spaces make all of that structurally
// impossible rather than individually guarded against.

#include <cstdint>

#include "shared/core/types.h"

namespace flix {

enum class Realm : std::uint8_t {
    Overworld = 0,   ///< the 60000x60000 tile map
    Arena = 1,       ///< the PVP ring
    Maze = 2,        ///< the daily maze
};

inline constexpr int kRealmCount = 3;

/// A byte off the wire or out of a file, made safe.
inline constexpr Realm realmFromByte(std::uint8_t value) {
    return value < static_cast<std::uint8_t>(kRealmCount) ? static_cast<Realm>(value)
                                                          : Realm::Overworld;
}

inline constexpr std::size_t realmIndex(Realm realm) { return static_cast<std::size_t>(realm); }

/// A position that knows which space it is in. What the per-tick player lists
/// carry, so a distance test between a mob and "the players" can never pair a
/// maze mob with an overworld flower that happens to share its numbers.
struct RealmPoint {
    Vec2 position;
    Realm realm = Realm::Overworld;
};

/// The spawn-picker id each realm is asked for by name. The overworld has
/// none: any other string names a biome inside it.
inline constexpr const char* kArenaSpawnChoice = "pvp";
inline constexpr const char* kMazeSpawnChoice = "maze";

// ---------------------------------------------------------------------------
// The arena
// ---------------------------------------------------------------------------
//
// A disc, as the reference's is (PVP_ARENA_RADIUS 2500), sitting in its own
// space with a margin of void around it so every coordinate stays positive.
// The margin is what the client paints dark around the ring.

inline constexpr double kArenaRadius = 2500.0;
inline constexpr double kArenaMargin = 500.0;
inline constexpr Vec2 kArenaCentre{kArenaRadius + kArenaMargin, kArenaRadius + kArenaMargin};
/// The square the arena space spans; what its broadphase grid is sized to.
inline constexpr double kArenaWorldSize = 2.0 * (kArenaRadius + kArenaMargin);
/// Where a flower joining the arena appears: 1500 east of the centre, as the
/// reference's PVP_ARENA_SPAWN is.
inline constexpr Vec2 kArenaSpawn{kArenaCentre.x + 1500.0, kArenaCentre.y};

/// True inside the ring, edge inclusive -- the reference's isInPvpArena.
inline constexpr bool insideArena(Vec2 p) {
    return distanceSq(p, kArenaCentre) <= kArenaRadius * kArenaRadius;
}

/// The reference's PVP rules that are numbers: a flat 100 max health for
/// everyone in the ring (PVP_MAX_HEALTH), and a quarter of what was looted
/// there surviving the walk out (PVP_INVENTORY_KEEP_RATIO).
inline constexpr double kArenaMaxHealth = 100.0;
inline constexpr double kArenaInventoryKeepRatio = 0.25;

} // namespace flix
