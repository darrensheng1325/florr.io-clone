#pragma once
// The map's annotations: spawn bands, player spawn points and teleporters --
// and the ART the client draws the map with.
//
// A map file carries three things. The tile layers the author painted, whose
// COLLISION Terrain derives and whose PICTURES this file carries through for
// the renderer. And the object layers, which say what the map MEANS -- which
// stretch of ground grows which mobs, where a joining player is put down, and
// which pad leads to which other map. Terrain deliberately knows none of the
// last two: geometry and meaning are separate questions, and only one of them
// is needed per tick.
//
// The art rides here rather than over the wire on purpose. Every client
// already loads every staged map for its annotations, and the pictures are the
// one part of a map that is not authoritative -- what is solid comes from the
// server, what it looks like comes from the file both ends ship.
//
// There is one MapData per world map, and WorldMaps at the bottom of this file
// owns them all. Every realm that is not the arena or the maze has exactly one
// of each -- one tile grid inside Terrain, one MapData here -- and the realm
// id is the index that ties them together.

#include <cstdint>
#include <string>
#include <vector>

#include "shared/core/json.h"
#include "shared/core/types.h"
#include "shared/game/components.h"
#include "shared/game/difficulty.h"
#include "shared/game/rarity.h"
#include "shared/game/realm.h"
#include "shared/game/tiled_map.h"

namespace flix {

class Terrain;
class WorldMaps;

/// One row of a spawn band's mob distribution.
///
/// A row is just a NAME and a weight, because the map layer has no view of the
/// content registry and must not grow one: whether `garden` is a mob group or
/// a mob id is a question only mobs.json can answer, and it is answered once,
/// at spawn time, by the spawner. Resolving it here would make the map depend
/// on the content and would freeze the answer at load, which is exactly what
/// made the old nine hard-coded section presets impossible to add to.
struct ZoneMobEntry {
    /// A mob GROUP (content.h) if the content defines one by this name, and a
    /// mob id otherwise. Groups win, so naming a group is never ambiguous.
    std::string name;
    /// Relative weight. Authored as percentages, but nothing requires them to
    /// sum to a hundred: they are normalised against each other.
    double weight = 1.0;
};

/// Parses a spawn band's mob distribution: `garden 50% hornet 50%`.
///
/// A sequence of `name weight` pairs, commas optional and percent signs
/// optional. A row with no weight takes 1, so a bare `hornet` is a band of
/// nothing but hornets.
///
/// Text that parses to nothing yields an empty list, which the spawner reads
/// as "no distribution" and answers with the map's own default group.
/// `warningOut`, when given, collects what was skipped, because a mistyped
/// distribution is otherwise a band that silently spawns the wrong thing.
std::vector<ZoneMobEntry> parseMobDistribution(const std::string& text, std::string* warningOut);

/// True when `at` is inside a zone outline, boundary INCLUDED.
///
/// An empty `polygon` means the outline is `bounds` itself. The boundary counts
/// as inside for both shapes, because the rectangles these replaced were tested
/// inclusively on every edge -- a mob standing exactly on a zone's border has
/// always been in that zone, and a polygon that dropped it would be a silent
/// behaviour change at every seam between two difficulty bands.
bool zoneContains(const Rect& bounds, const std::vector<Vec2>& polygon, Vec2 at);

/// The outline's area.
double zoneArea(const Rect& bounds, const std::vector<Vec2>& polygon);

enum class MapElementKind : std::uint8_t {
    Other = 0,
    Spawn,         ///< a mob band: a tier and a distribution over some ground
    PlayerSpawn,   ///< a rectangle a PLAYER may be put down in
    Teleporter,    ///< a pad, which leads to another map
};

/// One annotation, in world coordinates. The numbers are already world units --
/// one Tiled pixel is one world unit, so there is no scale factor to apply.
struct MapElement {
    MapElementKind kind = MapElementKind::Other;

    /// The element's bounding box. When `polygon` is set these are its AABB
    /// rather than its shape: every broadphase question -- is this zone near a
    /// viewport, is it worth looking at -- is asked of the box, and only
    /// containment, area and point sampling go to the outline. Keeping the box
    /// means none of those had to change.
    Rect bounds;

    /// The zone's outline in world coordinates, or empty when the outline IS
    /// `bounds`.
    ///
    /// Mob bands are polygons: a difficulty band follows a coastline or a canyon,
    /// and a rectangle over one of those either spills mobs onto the next
    /// tier's ground or leaves a wedge of its own permanently empty. Player
    /// spawn points are rectangles and teleporters are points.
    std::vector<Vec2> polygon;

    /// Mob bands only: HOW DANGEROUS this band is, on the open-ended scale
    /// shared/game/difficulty.h defines. 0 is fully common, 100 is ultras,
    /// 200 supers, 300 uniques; in between is a blend of two tiers.
    ///
    /// Orthogonal to `mobDistribution`, which says WHAT spawns: a band is
    /// "difficulty 60" and "half garden, half hornet" at the same time, and the
    /// bands are where the map's difficulty progression lives.
    ///
    /// A `spawn` object WITHOUT a difficulty is a mob REGION rather than a
    /// band: see isMobRegion() below. Difficulty ZERO is a real answer -- a
    /// band that says `difficulty: 0` is a band of commons -- so it is the
    /// PRESENCE of the property, not its value, that makes a band.
    double difficulty = 0.0;
    bool hasDifficulty = false;

    /// What this shape spawns, as weighted rows of group names and mob ids.
    /// Empty on a band means "whatever the region under it says".
    std::vector<ZoneMobEntry> mobDistribution;

    /// True when this `spawn` object owns a POPULATION: a difficulty band,
    /// stocked to a density of its own.
    ///
    /// These are the ONLY source of ambient mobs on a map. Ground no band
    /// covers grows nothing at all, so a map with no band on it is an empty
    /// map -- deliberately, and its load line says so.
    bool isSpawnBand() const { return kind == MapElementKind::Spawn && hasDifficulty; }

    /// True when it only says WHAT lives on this ground, and owns nothing.
    ///
    /// The two questions a map answers about a patch of ground are how
    /// dangerous it is and what grows there, and they do not have the same
    /// shape: danger runs in bands along a coastline, while "this is the
    /// desert" covers a whole quarter of the map. A region is the second
    /// question on its own -- a `spawn` object with a `mobs` distribution and
    /// no `difficulty`. It owns no population and spawns nothing: a band
    /// standing on it with no `mobs` of its own asks it what to grow, and a
    /// region drawn over ground no band covers grows nothing whatsoever.
    ///
    /// This is what replaced sectionAt() as the spawner's question. The nine
    /// sections used to decide what lived where implicitly, by geography
    /// nobody could move; a region says it, in the map, in a shape an author
    /// can drag.
    bool isMobRegion() const {
        return kind == MapElementKind::Spawn && !hasDifficulty && !mobDistribution.empty();
    }

    /// Player spawn points only: the id a teleporter or a saved preference
    /// names this point by. Unique within its map; WorldMaps qualifies it with
    /// the map's id to make it unique across the server.
    std::string spawnId;
    /// Player spawn points only: what the title screen's button says. Empty
    /// means the button falls back to the id, title cased.
    std::string label;
    /// Player spawn points only: the button's colour, 0xRRGGBB.
    std::uint32_t color = 0xCCCCCCu;
    bool hasColor = false;
    /// Player spawn points only: the ground artwork the title screen tiles
    /// behind the picker while this button is chosen, as a file name in the
    /// data directory. Empty falls back to the spawn id, so a point called
    /// `desert` gets desert.svg without saying so.
    std::string backdrop;
    /// Player spawn points only: which biome the picker files this door
    /// under. The picker is two rows -- a row of biomes, then the doors of the
    /// chosen biome -- because forty-odd doors do not fit in one. Empty falls
    /// back to the map's own `biome` property, then to the map's id.
    std::string biome;
    /// Player spawn points only: whether the title screen OFFERS this door.
    /// A biome's sublevels are entered from its main area, through a pad, so
    /// their doors are arrival points for teleporters and nothing more; only
    /// the main area's door is a button. Defaults to true, so a door an author
    /// draws is pickable unless they say otherwise.
    bool pickable = true;
    /// Player spawn points only: where this button sits in the row. Buttons
    /// sort by this and then by map order, so a map can put its beginner
    /// ground first without being the first map loaded.
    double order = 0.0;

    /// Teleporters only: the id of the map this pad leads to. A pad naming no
    /// map is scenery -- it charges up and goes nowhere, which is reported at
    /// load rather than at the moment a player stands on it.
    std::string targetMap;
    /// Teleporters only: the spawn point in `targetMap` the pad arrives at.
    /// Empty means the target map's default spawn.
    std::string targetSpawn;
    /// Teleporters only: an explicit arrival point in the target map's
    /// coordinates, for a pad that wants somewhere no spawn rectangle covers.
    /// `targetSpawn` wins when both are given.
    Vec2 teleportTo;
    bool hasTeleportTo = false;

    /// The centre of the BOUNDING BOX, which for a concave outline can be a
    /// point outside the zone. Deliberately so: this is what orders zones and
    /// attributes a spawn to the nearest player, neither of which wants a
    /// centroid, and a rectangle's centre could already land inside a wall.
    /// Anything that needs a point a body can stand on goes through
    /// MapData::spawnInElement.
    Vec2 centre() const { return {bounds.x + bounds.w * 0.5, bounds.y + bounds.h * 0.5}; }

    /// True when `at` is inside the outline, boundary included.
    bool contains(Vec2 at) const { return zoneContains(bounds, polygon, at); }

    /// The outline's area, which is what a band's mob target is scaled by.
    double area() const { return zoneArea(bounds, polygon); }
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

/// The annotation layer of ONE world map, loaded once beside its tile grid.
class MapData {
public:
    /// Reads the object layers and the art of a Tiled `.tmj`. The one map
    /// loader there is; see shared/game/tiled_map.h.
    ///
    /// `realm` is the realm this map IS. Everything here that touches terrain
    /// asks about that realm, so a MapData can never accidentally test a point
    /// against another map's walls.
    ///
    /// The map's id is its file stem unless setId() already named it, so a map
    /// loaded straight off a path -- a test, a tool -- still resolves the
    /// defaults below rather than falling back to an empty string.
    bool loadTiled(const std::string& path, std::string& errorOut,
                   Realm realm = Realm::Overworld);

    bool loaded() const { return !elements_.empty(); }
    const std::vector<MapElement>& elements() const { return elements_; }

    /// The bands clause of this map's load line: how many bands it carries and
    /// the difficulty range they span, each end named with the tier it
    /// resolves to -- or, when the map carries none, the sentence saying so.
    ///
    /// Split out of loadTiled so a test can hold the exact words to the light.
    /// The words matter: bands are the ONLY source of ambient mobs, so a
    /// bandless map is an empty world, and the line is the only thing standing
    /// between that and a reader deciding the spawner is broken.
    std::string bandSummary() const;

    /// Which realm this map's coordinates are in.
    Realm realm() const { return realm_; }

    /// The map's id -- its file stem, which is what a teleporter's `targetMap`
    /// names and what qualifies a spawn point id.
    const std::string& id() const { return id_; }
    void setId(std::string id) { id_ = std::move(id); }

    /// The `.tmj` this was read from, as it was passed to loadTiled(). Empty
    /// for a map a harness built in memory.
    ///
    /// Kept because the file is the only place a map's COLLISION SHAPES live: a
    /// client is handed the coarse grid over the wire and rebuilds the exact
    /// geometry from its own copy of the map (Terrain::loadCollisionShapes),
    /// and it needs to be able to say which file that was without the manifest
    /// being read a second time somewhere else.
    const std::string& sourcePath() const { return sourcePath_; }

    /// What the spawn picker calls this map, from its `displayName` property.
    /// Empty falls back to the id.
    const std::string& displayName() const { return displayName_; }

    /// The biome this map belongs to: what the picker files its doors under
    /// when a door does not say for itself.
    ///
    /// From the map's `biome` property, DEFAULTING TO THE MAP ID. A one-biome
    /// map should not have to say its own name twice, and every map that never
    /// declares one still lands in a tab of its own rather than in a nameless
    /// bucket shared with every other silent map.
    const std::string& biome() const { return biome_; }

    /// The mob group a spawn band with no `mobs` distribution of its own, and
    /// no mob region under it, spawns from.
    ///
    /// From the map's `defaultMobGroup` property, DEFAULTING TO biome() -- so
    /// `garden.tmj` grows garden mobs with nothing authored at all. A group
    /// mobs.json does not define is reported once by the spawner, which is
    /// where the content registry is; it cannot be checked here.
    const std::string& defaultMobGroup() const { return defaultMobGroup_; }

    /// The map's size in TILES. The map decides: a corridor level is a hundred
    /// tiles across and the overworld is sixty-four, and nothing here assumes
    /// either.
    int width() const { return width_; }
    int height() const { return height_; }

    /// Every distinct artwork this map's tiles name, by bare file name. A
    /// cell's `art` indexes into this; the renderer resolves a name to a
    /// sprite once and keeps it.
    const std::vector<std::string>& artFiles() const { return artFiles_; }

    /// The map's tile layers, BOTTOM TO TOP: the order to draw them in. Each
    /// holds width()*height() cells, row-major. Layer names are carried for
    /// tooling and mean nothing to the game.
    const std::vector<TiledLayer>& layers() const { return layers_; }

    /// The cell at a tile coordinate on one layer, or an empty cell when the
    /// coordinate is off the map. Bounds-checked because the renderer walks a
    /// viewport, not a grid, and a camera past the edge is normal.
    TiledCell cellAt(std::size_t layer, int tx, int ty) const;

    /// The map's player spawn points, in button order (`order`, then map
    /// order). This is the picker's list AND the teleporter's: a pad arrives
    /// at one of these by id.
    const std::vector<const MapElement*>& playerSpawns() const { return playerSpawns_; }

    /// The spawn point called `spawnId`, or null.
    const MapElement* playerSpawn(const std::string& spawnId) const;

    /// Where a player joining this map without naming a spawn point should
    /// appear: the first spawn point in button order, or failing that a common
    /// mob band, or failing that the middle of the map.
    ///
    /// `mobs` is every live mob body the candidate has to be clear of. It is
    /// optional only because the geometry half is useful without a world; a
    /// live server that omits it drops fresh flowers on top of whatever is
    /// standing there.
    Vec2 defaultSpawn(Rng&, const Terrain&, const std::vector<MobDisc>* mobs = nullptr) const;

    /// Where a player who asked for `spawnId` should appear, or false when this
    /// map has no such spawn point with room to drop someone into.
    bool spawnAt(const std::string& spawnId, Rng&, const Terrain&, Vec2& out,
                 const std::vector<MobDisc>* mobs = nullptr) const;

    /// Picks a point inside ONE element the caller has already chosen.
    ///
    /// The two spawn pickers above each own a policy, and the bot population
    /// has a third: it samples the whole set a player could legitimately appear
    /// in uniformly, so bots turn up spread over the map rather than stacked in
    /// the corner a fresh account starts in. That is a different policy over
    /// the same placement test, so the test is exposed rather than a third
    /// policy being added here.
    bool spawnInElement(const MapElement&, Rng&, const Terrain&, Vec2& out,
                        const std::vector<MobDisc>* mobs = nullptr) const;

    /// What one tick of teleporter interaction did to a flower.
    struct TeleportStep {
        /// Where the flower ends up -- pulled toward a pad, or unchanged.
        /// A pad that FIRED does not set this: the destination is in another
        /// map, and only the caller can move a body between realms.
        Vec2 position;
        /// Element index whose charge-up began this tick, or -1. The caller
        /// owns the wire event; the pad's dwell and destination are read back
        /// out of elements()[entered].
        int entered = -1;
        /// The pad that fired this tick, or -1. Its destination is
        /// elements()[fired]'s targetMap/targetSpawn, which WorldMaps resolves.
        int fired = -1;
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
    /// Turns one MAP_ELEMENTS-shaped JSON array into elements_, and derives the
    /// spawn point list from it. Both formats funnel through here, so there is
    /// one answer to what an annotation means rather than two.
    void adopt(const Json& array);

    /// Resets everything a load replaces, so a failed load cannot leave half
    /// of the previous map behind.
    void reset(Realm realm);

    /// Picks a point inside `area`'s OUTLINE a flower can safely be dropped on:
    /// no tile its BODY would overlap is solid, no mob is standing there, and
    /// the spot is not already crowded. False when fifty tries found nothing,
    /// which happens -- some zones are drawn over terrain that later became a
    /// wall, and a polygon covering little of its own bounding box needs more
    /// luck than a rectangle did.
    bool findOpenPoint(const MapElement& area, Rng&, const Terrain&, Vec2& out,
                       const std::vector<MobDisc>* mobs) const;

    std::vector<MapElement> elements_;
    /// Pointers INTO elements_, in button order. Rebuilt by adopt(), and
    /// elements_ is never touched afterwards, so they stay valid.
    std::vector<const MapElement*> playerSpawns_;
    Realm realm_ = Realm::Overworld;
    std::string id_;
    std::string sourcePath_;
    std::string displayName_;
    std::string biome_;
    std::string defaultMobGroup_;
    /// The map's art, straight out of the file. Copied rather than referenced
    /// because the TiledMap that parsed it is a load-time scratch object and
    /// the renderer reads these every frame.
    std::vector<std::string> artFiles_;
    std::vector<TiledLayer> layers_;
    int width_ = 0;
    int height_ = 0;
};

/// One entry of the spawn picker: a player spawn rectangle, wherever it is.
///
/// The picker's row used to be built out of the map's biome rectangles, which
/// meant every button was a place mobs lived and every place mobs lived wanted
/// to be a button. These are drawn for the picker and for nothing else, so a
/// map can offer two doors into one biome, or none at all.
struct SpawnChoice {
    /// What a client asks for by name: `<map id>:<spawn id>`, or just the
    /// spawn id when it is unique across every map. Stable across restarts,
    /// because it is authored rather than derived from load order.
    std::string id;
    std::string label;
    std::uint32_t color = 0xCCCCCCu;
    Realm realm = Realm::Overworld;
    /// Index into that map's elements(), or -1 for a choice with no rectangle
    /// behind it -- the client synthesises three of those for the default and
    /// the two realms that have no map file.
    int element = 0;
    /// The ground artwork the title screen tiles behind this choice.
    std::string backdrop;
    /// The picker's first row: which biome this door is filed under.
    std::string biome;
    /// False for a sublevel's door: reached through a pad from its biome's
    /// main area, never offered by the picker, and joinable by name only by
    /// an admin session (tooling and screenshots).
    bool pickable = true;
};

/// Every world map the server is running, and the realm each one is.
///
/// The maps are listed in `maps.json` and loaded in the order it gives, which
/// is what makes a realm id mean the same map on the client as on the server.
/// Discovering them by scanning the directory would make that order depend on
/// a file system, and a client and a server that disagree about which realm is
/// which put players in the wrong world.
class WorldMaps {
public:
    /// Loads every map named by `<dataDir>/maps.json` into `terrain` and into
    /// a MapData of its own.
    ///
    /// `terrain` may be NULL, which loads the ANNOTATIONS only. That is what
    /// the client wants: its tile grids arrive over the wire, authoritative,
    /// and reading a second copy off disk would give it two answers about what
    /// is solid. What it does need from the files is what the map MEANS --
    /// which spawn points the picker offers, and where the teleporter dots go
    /// on the minimap.
    ///
    /// Without a manifest this loads exactly one map -- the world -- from
    /// whichever format the directory was staged with, which is what every
    /// test harness and the offline build get.
    bool load(const std::string& dataDir, Terrain* terrain, std::string& errorOut);

    /// Installs a single already-loaded map as the overworld. For harnesses
    /// that build their world in memory rather than from a directory.
    void adoptSingle(MapData map);

    /// Installs several already-loaded maps, `maps[i]` as worldRealm(i). The
    /// same harness use as adoptSingle(), for a test that needs two realms.
    void adoptMaps(std::vector<MapData> maps);

    int count() const { return static_cast<int>(maps_.size()); }
    bool empty() const { return maps_.empty(); }

    /// The map in a realm, or null for the arena, the maze and any realm no
    /// map was staged for.
    const MapData* forRealm(Realm realm) const;
    MapData* forRealm(Realm realm);

    /// The realm a map id names, or Realm::Overworld with `found` false.
    Realm realmOfId(const std::string& mapId, bool& found) const;

    /// The maps, in load order. `maps()[i]` is realm `worldRealm(i)`.
    const std::vector<MapData>& maps() const { return maps_; }

    /// Every PICKABLE player spawn rectangle on every map, in button order.
    /// What the title screen draws and what a join request is matched
    /// against. A door marked `pickable = false` -- a biome sublevel's -- is
    /// not here: it is reached through a pad, and MapData::playerSpawn()
    /// still finds it for that.
    const std::vector<SpawnChoice>& spawnChoices() const { return spawnChoices_; }

    /// The choice `id` names, or null. Accepts both the qualified
    /// `<map>:<spawn>` form and a bare spawn id that only one map defines.
    /// PICKABLE doors only: this is what a join request is matched against.
    const SpawnChoice* choice(const std::string& id) const;

    /// Every door on every map, pickable or not, in the same order and with
    /// the same ids as spawnChoices() would give them. What a pad arrives at
    /// and what an admin may join by name; a normal join never resolves
    /// through this.
    const std::vector<SpawnChoice>& doors() const { return doors_; }

    /// The door `id` names, pickable or not, or null. Same id forms as
    /// choice().
    const SpawnChoice* door(const std::string& id) const;

    /// Where a teleporter leads: the realm and the arrival point.
    struct Destination {
        Realm realm = Realm::Overworld;
        Vec2 position;
    };

    /// Resolves a pad's `targetMap`/`targetSpawn` into somewhere a body can be
    /// put down. False when the pad names a map that is not staged, which is a
    /// map bug rather than a runtime condition -- it is reported at load.
    bool resolveTeleporter(const MapElement& pad, Rng&, const Terrain&, Destination& out,
                           const std::vector<MobDisc>* mobs = nullptr) const;

    /// Every pad whose destination this server cannot honour, as sentences a
    /// server operator can act on. Empty on a healthy set of maps.
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    /// Rebuilds spawnChoices_ and warnings_ from the loaded maps.
    void index();

    std::vector<MapData> maps_;
    std::vector<SpawnChoice> spawnChoices_;
    std::vector<SpawnChoice> doors_;
    std::vector<std::string> warnings_;
};

} // namespace flix
