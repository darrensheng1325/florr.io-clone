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
    Overworld = 0,   ///< the first world map, maps/world.tmj
    Arena = 1,       ///< the PVP ring
    Maze = 2,        ///< the daily maze
    // 3 and up are the OTHER world maps, one realm each, in the order
    // WorldMaps loaded them. See worldRealm() below.
};

/// How many realms can exist at once, and therefore how long every per-realm
/// array is. A cap rather than a count: the world maps are discovered at
/// runtime, and a fixed-size array indexed by realm is what keeps the
/// broadphase and the terrain grids allocation-free per tick.
///
/// Sixty-four because the realm travels as a byte and the shipped set is
/// already forty-seven maps -- the overworld, the sewers and five temporary
/// maps per biome. Raising it costs one broadphase Layer and one tile grid
/// header per realm, and nothing on the wire; a realm nothing was staged for
/// keeps a one-cell layer and an empty grid.
inline constexpr int kMaxRealms = 64;

/// How many world MAPS can be staged: every realm that is not the arena or the
/// maze.
inline constexpr int kMaxWorldMaps = kMaxRealms - 2;

/// A byte off the wire or out of a file, made safe.
inline constexpr Realm realmFromByte(std::uint8_t value) {
    return value < static_cast<std::uint8_t>(kMaxRealms) ? static_cast<Realm>(value)
                                                         : Realm::Overworld;
}

inline constexpr std::size_t realmIndex(Realm realm) { return static_cast<std::size_t>(realm); }

/// The realm a world map occupies, by the slot WorldMaps loaded it into.
///
/// Slot 0 is the overworld and keeps realm 0, because that is the default a
/// Transform is born with and the value every test and every legacy record
/// already carries. The rest start after the two special realms.
inline constexpr Realm worldRealm(int mapSlot) {
    return mapSlot <= 0 ? Realm::Overworld : static_cast<Realm>(mapSlot + 2);
}

/// The map slot behind a realm, or -1 for the arena and the maze -- the two
/// realms that are generated rather than authored and have no map file.
inline constexpr int worldMapSlot(Realm realm) {
    if (realm == Realm::Overworld) return 0;
    if (realm == Realm::Arena || realm == Realm::Maze) return -1;
    return static_cast<int>(realm) - 2;
}

/// True when a realm is one of the authored maps, i.e. has a tile grid and an
/// annotation layer of its own.
inline constexpr bool isWorldRealm(Realm realm) { return worldMapSlot(realm) >= 0; }

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
