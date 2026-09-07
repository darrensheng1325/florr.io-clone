#pragma once
// The map's annotations: spawn zones, biomes and teleporters.
//
// `map_bundle.ts` carries two things. The tile grid, which Terrain already
// reads, says what is solid. MAP_ELEMENTS says what the map MEANS -- which
// stretch of ground is the beginner's field, which rectangle is the desert,
// which tier of mob belongs where. Terrain deliberately knows none of that:
// geometry and meaning are separate questions, and only one of them is needed
// per tick.
//
// This exists because a player has to spawn somewhere sane. Dropping them at
// the centre of the world puts them in the middle of the legendary and mythic
// bands, which is a place a level-1 flower cannot survive and cannot walk out
// of. The map already says where the common ground is; this reads it.

#include <cstdint>
#include <string>
#include <vector>

#include "shared/core/types.h"
#include "shared/game/components.h"
#include "shared/game/rarity.h"

namespace flix {

class Terrain;

/// One row of a biome's own spawn table.
///
/// The `mobType` is the reason this is a struct rather than a bare tier. A row
/// that names a mob pins the spawn to it, and that is the ONLY way a mob the
/// ambient roll refuses ever reaches the world: `target_dummy` declares no
/// spawn weight and is marked `neverAmbient`, so the map's four dummy biomes
/// are the whole of its existence. Parsing the tier and dropping the name
/// makes every one of those biomes spawn ordinary garden mobs instead, and the
/// DPS row is simply never built.
struct BiomeSpawnEntry {
    Rarity tier = Rarity::Common;
    /// Relative weight within its own table. The bundle writes one on every
    /// row; a row without one still takes a share rather than being silently
    /// unreachable.
    double weight = 1.0;
    /// The mob this row pins the spawn to, or empty for "whatever this section
    /// admits at that tier".
    std::string mobType;
};

enum class MapElementKind : std::uint8_t {
    Other = 0,
    Spawn,
    Biome,
    Teleporter,
};

/// One annotation, in world coordinates. The bundle's numbers are already
/// world units -- there is no scale factor to apply, whatever the browser
/// build's SCALE_FACTOR of 1 might suggest is coming.
struct MapElement {
    MapElementKind kind = MapElementKind::Other;
    Rect bounds;

    /// Spawn zones only: the mob tier that belongs in this rectangle.
    Rarity spawnTier = Rarity::Common;
    bool hasSpawnTier = false;

    /// Biomes only.
    std::string biomeName;
    /// This biome's own spawn table. Empty means the biome declared no table,
    /// which is NOT the same as declaring an empty one: a biome without a
    /// table falls back to the global tiers, so it can hold anything and is
    /// never safe to spawn in.
    std::vector<BiomeSpawnEntry> spawnTable;
    bool hasSpawnTable = false;

    /// Teleporters only: where the pad drops the flower. A pad without one is
    /// scenery -- the reference skips it before it even measures the distance.
    Vec2 teleportTo;
    bool hasTeleportTo = false;

    Vec2 centre() const { return {bounds.x + bounds.w * 0.5, bounds.y + bounds.h * 0.5}; }
};

/// One live mob body, as a spawn candidate has to see it.
///
/// The annotation layer has no view of the ECS and should not grow one, so a
/// caller that wants the reference's two crowd tests hands over the discs it
/// already has. Passing none keeps the geometry test and skips those two,
/// which is what a mob-less harness and the client want.
struct MobDisc {
    Vec2 position;
    double radius = 0.0;
};

/// How a biome is named and coloured on the spawn picker.
///
/// Presentation next to the data it describes, as the section biome table in
/// terrain.h already is: there is one map, and one set of names for it. A
/// biome the table does not know still gets a button -- its own id, title
/// cased, in grey -- because the map is edited more often than this list.
struct BiomeDisplay {
    const char* label;
    std::uint32_t color;
};

BiomeDisplay biomeDisplay(const std::string& biomeName);

/// The map's annotation layer, loaded once beside the tile grid.
class MapData {
public:
    /// Reads MAP_ELEMENTS out of `map_bundle.ts`. The array is plain JSON
    /// inside a TypeScript literal, so it is sliced out and handed to the JSON
    /// parser rather than being re-lexed here.
    ///
    /// A bundle without the array is not an error: the annotation layer is
    /// optional, and a server that loses it falls back to the middle of the
    /// map rather than refusing to start.
    bool load(const std::string& bundlePath, std::string& errorOut);

    bool loaded() const { return !elements_.empty(); }
    const std::vector<MapElement>& elements() const { return elements_; }

    /// Where a player joining without a preference should appear.
    ///
    /// A `common` spawn zone, preferring section 0 -- the top-left corner is
    /// where the map's beginner ground is, and a player who picked nothing
    /// should land there rather than wherever the first zone in file order
    /// happens to be. Falls back to the centre of the map when the annotation
    /// layer is missing entirely.
    ///
    /// `mobs` is every live mob body the candidate has to be clear of. It is
    /// optional only because the geometry half is useful without a world; a
    /// live server that omits it drops fresh flowers on top of whatever is
    /// standing there.
    Vec2 defaultSpawn(Rng&, const Terrain&, const std::vector<MobDisc>* mobs = nullptr) const;

    /// Where a player who asked for `biomeName` should appear, or false when
    /// that biome has no area safe enough to drop someone into.
    bool spawnInBiome(const std::string& biomeName, Rng&, const Terrain&, Vec2& out,
                      const std::vector<MobDisc>* mobs = nullptr) const;

    /// Picks a point inside ONE element the caller has already chosen.
    ///
    /// The two spawn pickers above each own a policy -- beginner ground first,
    /// or a named biome -- and the bot population has a third: it samples the
    /// whole set a player could legitimately appear in (every common zone and
    /// every safe biome) uniformly, so bots turn up spread over the map rather
    /// than stacked in the corner a fresh account starts in. That is a
    /// different policy over the same placement test, so the test is exposed
    /// rather than a third policy being added here.
    bool spawnInElement(const MapElement&, Rng&, const Terrain&, Vec2& out,
                        const std::vector<MobDisc>* mobs = nullptr) const;

    /// The biomes spawnInBiome() would actually accept, in map order. This is
    /// the SERVER's list: a destination it can honour without dropping the
    /// arrival somewhere lethal.
    const std::vector<std::string>& spawnableBiomes() const { return spawnableBiomes_; }

    /// Every biome the map names, in map order -- the PICKER's list. The
    /// browser's title screen offers each one with no tier test whatever, so
    /// filtering here would drop a button the reference still draws as soon as
    /// a biome's spawn table is raised above uncommon.
    const std::vector<std::string>& pickableBiomes() const { return pickableBiomes_; }

    /// True when a biome's own spawn table admits nothing above uncommon. A
    /// biome with no table at all is never safe -- it inherits the world's
    /// tiers, which go all the way up.
    static bool safeForSpawn(const MapElement&);

    /// The biome covering `at`, or null. First match in MAP ORDER, and the
    /// rectangle is inclusive on every edge, both as the reference's own
    /// lookup is (src/server/enemySpawner.ts:182). A biome is returned whether
    /// or not it declares a spawn table -- the caller is the one that decides
    /// what an empty table means.
    const MapElement* biomeAt(Vec2 at) const;

    /// What one tick of teleporter interaction did to a flower.
    struct TeleportStep {
        /// Where the flower ends up: pulled toward a pad, or on the far side.
        Vec2 position;
        /// Element index whose charge-up began this tick, or -1. The caller
        /// owns the wire event; the pad's dwell and destination are read back
        /// out of elements()[entered].
        int entered = -1;
        /// The jump fired this tick and `position` is the destination.
        bool teleported = false;
        /// The flower stepped off the pad it was charging, cancelling it.
        bool exited = false;
    };

    /// Runs every pad against one flower for one tick, as the reference's
    /// per-player teleporter pass does.
    ///
    /// A pad is a well, not a trigger: the suction reaches well past the pad
    /// and is strong enough to beat a mob's shove, the pad has to be HELD for
    /// a full second, and the jump locks the flower out of every pad -- the
    /// suction included -- for five, so it does not fall straight back through
    /// the one it arrived on. Only the first pad the flower is standing on
    /// gets to act, but every pad's suction is applied on the way there.
    TeleportStep stepTeleporters(Vec2 centre, double deltaSeconds, double nowMillis,
                                 TeleporterState& state) const;

private:
    /// Picks a point inside `area` a flower can safely be dropped on: no tile
    /// its BODY would overlap is solid, no mob is standing there, and the spot
    /// is not already crowded. False when fifty tries found nothing, which
    /// happens -- some zones are drawn over terrain that later became a wall.
    bool findOpenPoint(const Rect& area, Rng&, const Terrain&, Vec2& out,
                       const std::vector<MobDisc>* mobs) const;

    std::vector<MapElement> elements_;
    std::vector<std::string> spawnableBiomes_;
    std::vector<std::string> pickableBiomes_;
};

} // namespace flix
