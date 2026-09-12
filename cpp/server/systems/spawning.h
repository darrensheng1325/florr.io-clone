#pragma once
// Keeping the world populated: what appears, where, and what is recycled.
//
// THE AUTHOR DRAWS WHERE THE MOBS ARE. A map carries SPAWN BANDS -- shapes on
// an object layer, each with a `difficulty` -- and a band owns a population of
// its own, sized by the area of its outline and stocked while somebody can see
// it. That is the only source of ambient mobs there is. Ground with no band
// over it grows nothing, ever, so a map its author has drawn no band on has no
// mobs at all: that is a fact about the data, not a fault in this file, and
// the map's load line says so out loud when it happens.
//
// Recycling is the other half, and it is still a moving neighbourhood: only
// the bands somebody is looking at are stocked, and a mob nobody has been near
// for the grace period is destroyed. The tick cost therefore follows the
// number of players rather than the area of the world.
//
// One ceiling sits above all of it -- a global live-mob cap, so a crowd of
// players spread over many bands cannot multiply the population without bound.
//
// The ARENA and the MAZE are not map ground: they are generated, they carry no
// object layer and no band covers them, and ModeSpawner (mode_spawning.h)
// populates each one whole. Nothing in this file spawns there.

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "server/replication.h"
#include "shared/core/types.h"
#include "shared/core/world.h"
#include "shared/game/components.h"
#include "shared/game/config.h"
#include "shared/game/constants.h"
#include "shared/game/difficulty.h"
#include "shared/game/map_elements.h"
#include "shared/game/rarity.h"
#include "shared/game/terrain.h"

namespace flix {

// ---------------------------------------------------------------------------
// Components owned by this system
// ---------------------------------------------------------------------------

/// Bookkeeping for a mob the population controller owns.
///
/// Carrying it is what makes a mob "ambient". A pet, a boss placed by a
/// script, or anything else spawned outside this system has none, so it is
/// neither counted against the caps nor recycled out from under its owner.
struct AmbientMob {
    /// Last time the mob was inside any player's buffered viewport. Stamped at
    /// spawn, so a mob placed into an empty world still gets the full grace period rather
    /// than being recycled on the very next pass.
    double lastNearPlayerMillis = 0;
};

/// A nest working through the escalating `spawn_waves` list in its config.
///
/// Distinct from Spawner, which repeats ONE child type on a timer: a wave is a
/// heterogeneous group and the list gets harder as it goes. Kept as its own
/// component rather than widened into Spawner because two mobs in the whole
/// data set have waves, and every other nest would carry the empty vector.
struct NestWaves {
    std::uint16_t mobIndex = 0;      ///< the nest's own config, where the list lives
    /// The health the nest had when its bands were last weighed.
    ///
    /// A wave is released by DAMAGE, never by a clock: the band is a pure
    /// function of how much health is left, so an untouched hole stays silent
    /// and one big hit fires every band it crossed at once. That is the whole
    /// shape of the ant-hole fight.
    double previousHealth = 0;
    /// Deepest band released so far. Nothing is scheduled from it -- the band
    /// is recomputed from current health on every drop -- it is the nest's
    /// phase, for anything reporting on a hole.
    std::uint16_t nextWave = 0;
    /// Live escorts from previous waves, pruned as they die. Holding handles
    /// rather than a count is what makes the brood survive a mob dying: a stale
    /// counter would leak the slot forever.
    std::vector<Entity> children;
};

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

/// The DEFAULT buffered viewport: half a 1920x1080 screen plus 500 units of
/// buffer on each side. It is the box a mob has to be inside to count as seen,
/// and it is what decides which bands are worth stocking.
///
/// A default, for a player whose client reported no viewport at all. The
/// reference sizes the keep-alive box off each client's OWN reported viewport
/// (src/server/playerState.ts:1041), so an ultrawide flower keeps a wider strip
/// of the map alive around it.
inline constexpr double kSpawnViewportHalfWidth = kViewportWidth * 0.5 + kViewportBuffer;
inline constexpr double kSpawnViewportHalfHeight = kViewportHeight * 0.5 + kViewportBuffer;

/// The one hard ceiling: what a full server actually costs. Every spawn path
/// tests it, the bands included, so a hundred bands in view cannot between them
/// put more than this in the world.
inline constexpr int kMaxLiveMobs = 900;

/// Escorts stand off their nest by this much plus up to another body radius,
/// on a bearing of their own. The gap is the only difference the reference
/// draws between the guard a hole opens with and the waves it sends afterwards
/// (src/server/enemySpawner.ts:955, src/server.ts:1639).
inline constexpr double kInitialEscortGap = 30.0;
inline constexpr double kWaveEscortGap = 10.0;

/// No spawn lands closer than this to ANY player, not just the one whose view
/// of the band made it fill. Two players standing together would otherwise
/// spawn into each other's laps.
inline constexpr double kMinSpawnDistance = 100.0;
inline constexpr double kMinMobSpawnSpacing = 80.0;
inline constexpr double kPreliminarySpawnRadius = 20.0;

/// Radius counted as "a player has been here", and how long a mob survives
/// without one. The radius is wider than the active radius so a player pacing
/// the edge of a group does not cause it to blink out and back.
inline constexpr double kMobDespawnDelayMillis = 30000.0;

/// The census runs on its own cadence rather than every tick: it is
/// O(mobs x players), and at 30Hz nothing about the population changes fast
/// enough to need it more often than this.
inline constexpr double kPopulationIntervalMillis = 500.0;

/// Cadence a wave nest used to send on, kept as the interval anything
/// stepping a nest through its bands walks by. The tick itself reads it
/// nowhere: a wave follows the hole's health, not a clock (see NestWaves).
inline constexpr double kNestWaveIntervalMillis = 15000.0;

/// Escorts one wave-nest is expected to have out at once. A hole is capped
/// nowhere -- every band it crosses fires in full -- so this bounds nothing;
/// it is the headroom a section holding a nest is measured against.
inline constexpr int kMaxNestChildren = 12;

/// A body segment trails nine tenths of a body DIAMETER behind the one in
/// front, which is close enough that a centipede reads as one animal rather
/// than a string of beads. Only the initial lay-out uses it; from the next
/// tick the chain pass holds the segments at the spacing recorded on them.
inline constexpr double kCentipedeSegmentSpacingScale = 0.9;

/// How deep nesting may go. A nest whose escorts are themselves nests is legal
/// data and would otherwise recurse until the world ran out of memory.
inline constexpr int kMaxNestDepth = 2;

/// The map's spawn bands are the whole of the ambient population. A band
/// declares its DIFFICULTY, which is the whole rarity progression of the map --
/// walking from the beginner corner into a difficulty-100 band is walking from
/// one shape into another -- and it owns the mobs standing inside it
/// (src/server/spawnZoneManager.ts).
inline constexpr double kZoneIntervalMillis = 1000.0;

/// A zone fills the moment it enters someone's view, then alternates between a
/// big refill and a thin trickle. The long gap is what makes an area a player
/// has just cleared stay cleared for a while, and the short one is what keeps
/// it from being empty when they turn around.
inline constexpr double kZoneWaveIntervalMillis = 45000.0;
inline constexpr double kZoneTrickleIntervalMillis = 4000.0;
inline constexpr int kZoneTrickleMin = 1;
inline constexpr int kZoneTrickleMax = 2;

/// Ceiling on one zone's spawns per pass, so a large rectangle entering view
/// is staggered over several seconds rather than arriving as one packet.
inline constexpr int kZoneSpawnsPerPass = 12;

/// Attempts allowed per band spawn before it gives up on this pass. A point in
/// a lake, in a wall or in somebody's lap is thrown away rather than nudged, so
/// this is what decides how thin a band over bad terrain goes.
inline constexpr int kZonePlacementAttempts = 60;

/// The density a full band is aimed at: the reference's 9000 mobs over its
/// 60000-unit square, which is what a band's target population is derived from
/// (and what mode_spawning.h sizes a maze corridor by), so a stocked band reads
/// as ordinary ground rather than as a pit.
inline constexpr double kTargetMobDensity = 9000.0 / (kWorldSize * kWorldSize);

/// The tier at which a spawn is worth telling the whole server about. Supers,
/// uniques and apexes are events; everything below is scenery.
///
/// There is no boss PASS any more -- bosses come from band difficulty, and a
/// difficulty-200 band is full of supers by design -- so this is a property of
/// the spawn that happened rather than of a scheduler.
inline constexpr Rarity kAnnouncedRarity = Rarity::Super;

// ---------------------------------------------------------------------------
// SpawnSystem
// ---------------------------------------------------------------------------

class SpawnSystem {
public:
    /// What the last population pass saw. Read by tests and by anything that
    /// wants to report server load; the controller itself keeps no other state
    /// about the world, because the world is the state.
    struct Census {
        int mobs = 0;                                  ///< ambient mobs alive
        int spawnedTotal = 0;                          ///< cumulative, since construction
        int despawnedTotal = 0;
    };

    /// One player's neighbourhood, as this system has to see it.
    ///
    /// The runtime hands the pass positions, and two of the reference's rules
    /// need more than a coordinate: the box that keeps a mob alive is the size
    /// the CLIENT reported (src/server/playerState.ts:1041), and the tier a
    /// band rolls is biased by the nearest flower's luck
    /// (src/server/enemySpawner.ts:855-869). Both live on the flower's own
    /// entity, so each position is paired back up with the player standing on
    /// it. A position matching no player -- a harness driving the spawner with
    /// bare coordinates -- keeps the reference's defaults, which is the
    /// 1920x1080 flower it assumes.
    struct Viewer {
        Vec2 position;
        /// The realm this flower is standing in. Spawn bands only serve
        /// viewers in their OWN map: two maps' coordinates overlap
        /// numerically, and a band that ignored this would stock a second map
        /// because somebody was standing at the same numbers in the first.
        Realm realm = Realm::Overworld;
        /// Half the reported viewport plus the spawn buffer, per axis: the box
        /// a mob has to be inside to count as seen, and the box a band has to
        /// overlap to be worth stocking.
        Vec2 half{kSpawnViewportHalfWidth, kSpawnViewportHalfHeight};
        double luck = kNeutralSpawnLuck;
    };

    /// The live-mob ceiling this pass will not spawn past.
    ///
    /// A variable rather than kMaxLiveMobs directly because the admin console
    /// can raise or lower it at runtime (`/admin set_max_enemies`), which is
    /// the one knob an operator reaches for when a box is struggling. Every
    /// spawn path tests it, so lowering it stops new mobs immediately and lets
    /// the existing population drain rather than culling anything.
    int mobCap = kMaxLiveMobs;

    /// Assigns the wire id for every entity this system creates.
    ///
    /// Null in a unit test, where nothing replicates. The runtime MUST point it
    /// at the server's one allocator: a mob without a NetId is simulated
    /// correctly and is invisible to every client, which is the most confusing
    /// possible failure.
    NetIdAllocator* netIds = nullptr;

    /// Every staged map's annotation layer, or null.
    ///
    /// Spawn bands are geography, and they are the only source of ambient mobs
    /// there is: every map is populated entirely by the bands its author drew.
    ///
    /// Null in a unit test and in any harness with no map to read -- and with
    /// no maps there are no bands, so such a harness gets an EMPTY world.
    /// Anything that wants mobs hands this a map with a band on it, or places
    /// them itself through spawnMob().
    const WorldMaps* worldMaps = nullptr;

    /// A boss the last pass admitted, for whoever owns the chat channel.
    ///
    /// The reference announces supers and uniques with a per-player line whose
    /// wording depends on where that player is standing; this system has no
    /// view of the socket list, so it reports rather than broadcasts. Anything
    /// below kAnnouncedRarity -- an ultra included -- is deliberately silent
    /// and never appears here.
    struct BossSpawn {
        std::uint16_t mobIndex = 0;
        Rarity rarity = Rarity::Super;
        Vec2 position;
        /// The map it appeared on. A band on ANY staged world map announces --
        /// difficulty is what makes bosses now, and every map has difficulty --
        /// so `position` is only comparable with a player's after this matches.
        Realm realm = Realm::Overworld;
    };
    /// Drained by the runtime. Bounded rather than unbounded, so a server that
    /// never drains it keeps the newest announcements instead of growing.
    std::vector<BossSpawn> bossSpawns;

    /// `players` is every connected flower with a body; the bands are stocked
    /// for the ones standing on an authored MAP, and the census keeps a mob in
    /// another realm alive for as long as anyone is in that realm at all --
    /// the arena and the maze are populated by ModeSpawner, whole, not by
    /// viewport and not by band.
    void run(World& world, const Terrain& terrain, const ContentRegistry& content,
             const std::vector<RealmPoint>& players, Rng& rng, double nowMillis, double dt,
             CommandBuffer& commands);

    /// Places one mob, with its nest escorts if it has any, and returns it.
    /// NULL_ENTITY when `mobIndex` names nothing.
    ///
    /// `position` is a request, not a promise: it is pushed out of the terrain
    /// before use, so a caller may hand over a point in a wall and still get a
    /// mob standing somewhere legal.
    Entity spawnMob(World& world, const Terrain& terrain, const ContentRegistry& content,
                    std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                    double nowMillis, Rng& rng);

    /// The weighted type roll over ONE mob group: each member's weight, over
    /// the members that exist at this tier.
    ///
    /// kInvalidIndex when the group is empty, unknown, or admits nothing at
    /// this tier -- all three are "no spawn this attempt" rather than a
    /// fallback, because a fallback here would put one map's mobs on another.
    std::uint16_t chooseGroupMob(const ContentRegistry& content, std::uint16_t group,
                                 Rarity rarity, Rng& rng) const;

    /// The rarity one mob spawns at on difficulty-`difficulty` ground: the
    /// difficulty curve's own roll (shared/game/difficulty.h), then raised to
    /// the mob's own min_rarity floor.
    ///
    /// The floor is the mob's, not the ground's: `evil_centipede` does not
    /// exist below rare, so asking difficulty 0 for one still gets a rare.
    static Rarity rollRarity(const MobConfig& config, double difficulty, double luck, Rng& rng);

    /// How dangerous the ground at `at` is: the difficulty of the first band
    /// covering it, and zero anywhere else.
    ///
    /// Zero is not "the default tier of open ground" -- open ground grows
    /// nothing at all now -- it is the answer for a point no band claims, which
    /// is the honest reading of "nothing here was ever rolled". The band fill
    /// does not call this (it has its band in hand and reads its difficulty
    /// directly); it is the query anything JUDGING a mob against the ground it
    /// stands on asks.
    double difficultyAt(Realm realm, Vec2 at) const;

    const Census& census() const { return census_; }

private:
    /// One `spawn` shape and where it is in its fill cycle.
    ///
    /// The cycle resets the moment the shape leaves every viewport, so a
    /// player walking back into an area they cleared an hour ago finds it
    /// freshly stocked rather than mid-trickle.
    struct SpawnZone {
        /// The outline's bounding box: what the viewport test and the point
        /// sampler both work in.
        Rect bounds;
        /// The outline itself, or empty when the zone is a plain rectangle.
        /// Copied off the map element rather than pointed at it, because these
        /// zones outlive nothing but they are rebuilt from a MapData the system
        /// does not own.
        std::vector<Vec2> polygon;
        /// How dangerous this band is; what every spawn inside it is rolled
        /// against. See shared/game/difficulty.h.
        double difficulty = 0.0;
        /// Which map this band belongs to. Bands are gathered from every
        /// staged map, so this is what keeps one map's band from stocking
        /// another's identical coordinates.
        Realm realm = Realm::Overworld;
        /// What this band spawns: weighted rows of group names and mob ids,
        /// already resolved against the content. Empty means the map's own
        /// `defaultMobGroup`.
        std::vector<ZoneMobEntry> mobs;
        /// `mobs` resolved to indices, in the same order: a group index when
        /// the name is a group, a mob index when it is a mob, and
        /// kInvalidIndex when the content defines neither.
        ///
        /// Resolved once when the zones are built rather than per spawn: the
        /// lookup is a hash probe per row and a busy band rolls several times
        /// a second.
        struct ResolvedRow {
            std::uint16_t group = kInvalidIndex;
            std::uint16_t mob = kInvalidIndex;
            double weight = 1.0;
        };
        std::vector<ResolvedRow> resolved;
        /// The population this band is aimed at: kTargetMobDensity over the
        /// area of its own outline.
        int targetMobs = 1;
        bool initialized = false;
        double lastWaveMillis = 0;
        double lastTrickleMillis = 0;
        /// Spawns still owed from an initial fill or a wave, drained a chunk
        /// per pass rather than all at once.
        int pendingFill = 0;
    };

    void bind(World& world);

    /// Pairs each position the caller handed over with the flower standing on
    /// it. The list stays the caller's -- it decides WHO drives the population
    /// -- and this only fills in what a bare coordinate cannot say.
    void gatherViewers(World& world, const std::vector<RealmPoint>& players);

    /// Counts what is alive, refreshes every mob's "last seen by somebody"
    /// stamp, and destroys the ones nobody has been near for the grace period.
    /// It also rebuilds the placement record the band fill spaces itself
    /// against, which is why it runs before one.
    void takeCensus(const ContentRegistry& content, const std::vector<Viewer>& viewers,
                    double nowMillis, CommandBuffer& commands);
    void runNests(World& world, const Terrain& terrain, const ContentRegistry& content,
                  Rng& rng, double nowMillis);
    void expireEscorts(double dt, CommandBuffer& commands);

    /// Stocks every spawn band a player can see, at the tier the map declares
    /// for it. THE ONLY ambient spawn path there is: a point no band covers is
    /// never sampled by anything, so it never grows a mob.
    void runSpawnZones(World& world, const Terrain& terrain, const ContentRegistry& content,
                       const std::vector<Viewer>& viewers, Rng& rng, double nowMillis);

    /// One mob inside `zone`, or NULL_ENTITY when the rectangle had nowhere to
    /// put it. The tier comes from the zone's own difficulty.
    Entity spawnInZone(World& world, const Terrain& terrain, const ContentRegistry& content,
                       const SpawnZone& zone, const std::vector<Viewer>& viewers, Rng& rng,
                       double nowMillis);

    /// True when this section already holds a mob of this type and tier that
    /// nothing will ever clear away.
    ///
    /// Only asked about `neverAmbient` mobs, which is the target dummy: it
    /// never despawns and is effectively unkillable, so a duplicate that slips
    /// through is permanent. One of each rarity per section, checked against
    /// the FINAL tier after the mob's own floor (src/server/enemySpawner.ts:112).
    /// True when a `neverAmbient` fixture of this type and tier already
    /// stands in `realm` -- in `section` of it when the realm is the
    /// overworld, anywhere on the map otherwise (the section grid is the
    /// overworld's alone).
    bool permanentFixtureExists(World& world, std::uint16_t mobIndex, Rarity rarity, Realm realm,
                                int section);

    /// Queues one boss for whoever owns the chat channel, when the spawn was
    /// notable enough to be worth one: kAnnouncedRarity and up, and never a
    /// permanent fixture (a super target dummy is a DPS post, not an event).
    void announceIfNotable(const ContentRegistry& content, std::uint16_t mobIndex, Rarity rarity,
                           Vec2 position, Realm realm);

    void rebuildZones(const ContentRegistry& content);

    /// The mob a band fill should place: the band's own distribution when it
    /// declares one, and otherwise whatever `at` would grow anyway -- the
    /// region under it, then the map's default.
    std::uint16_t chooseZoneMobType(const ContentRegistry&, const SpawnZone&, Vec2 at, Rarity,
                                    Rng&);

    /// The distribution `at` grows, ignoring difficulty bands: the mob region
    /// covering it, or the map's `defaultMobGroup`. kInvalidIndex when neither
    /// names anything the content has.
    ///
    /// Only ever asked about a point INSIDE a band -- a band with no `mobs` of
    /// its own falls through to here -- which is what makes a region a
    /// statement about which roster belongs where rather than a second
    /// population driver.
    std::uint16_t chooseRegionMobAt(const ContentRegistry&, Realm, Vec2 at, Rarity, Rng&);

    /// One weighted roll over already-resolved rows, shared by bands and
    /// regions: both hold the same table and mean the same thing by it.
    std::uint16_t rollResolvedRows(const ContentRegistry&,
                                   const std::vector<SpawnZone::ResolvedRow>& rows, Rarity,
                                   Rng&) const;
    int countMobsInZone(const SpawnZone& zone) const;

    /// True when a body of `halfSize` at `position` would touch a mob the last
    /// census saw, with `extraGap` of clearance on top. The one scan behind
    /// every spacing test a band fill makes.
    bool crowdedAt(Realm realm, Vec2 position, double halfSize, double extraGap) const;

    Entity spawnMobAt(World& world, const Terrain& terrain, const ContentRegistry& content,
                      std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                      double nowMillis, Rng& rng, int depth);

    /// Lays a centipede's body out behind its head, each segment linked to the
    /// one in front. Driven from spawnMobAt so that every path to a head --
    /// band fill, nest, script, pet -- gets the chain, which is why no caller
    /// can produce a lone head.
    ///
    /// The chain trails off whatever shape placed the head, and a long animal
    /// routinely reaches over its band's edge. That is not a mob spawned
    /// outside a band; it is the rest of one spawned inside it.
    void spawnBodyChain(World& world, const Terrain& terrain, const ContentRegistry& content,
                        Entity head, const MobConfig& config, Rarity rarity, Vec2 headPosition,
                        Realm realm, double headAngle, double nowMillis, Rng& rng, int depth);

    /// One escort at an already-chosen spot, leashed to `parent`. Where that
    /// spot is belongs to the caller: a hole's guards and its waves stand off
    /// it on a bearing of their own, while a queen's soldiers come out
    /// directly behind her.
    Entity spawnEscort(World& world, const Terrain& terrain, const ContentRegistry& content,
                       std::uint16_t childIndex, Rarity nestRarity, Vec2 at, Realm realm,
                       Entity parent, double nowMillis, Rng& rng, int depth);

    World* boundWorld_ = nullptr;
    std::optional<Query<MobTag, Transform, Body, MobType, AmbientMob>> ambient_;
    std::optional<Query<AmbientMob, Lifetime>> escorts_;
    std::optional<Query<Transform, MobType, Spawner>> spawners_;
    std::optional<Query<Transform, MobType, NestWaves>> waveNests_;
    /// Every mob, ambient or not: the boss census counts what is alive in the
    /// world, and a boss placed by a script is still a boss.
    std::optional<Query<MobTag, Transform, MobType>> allMobs_;
    /// The flowers themselves, for the viewport and the luck a position does
    /// not carry.
    std::optional<Query<PlayerTag, Transform>> playerBodies_;

    Census census_;
    double nextPopulationMillis_ = 0;

    /// Rebuilt when `worldMaps` or the content changes, which in the server is
    /// once. Bands from EVERY staged map, each tagged with its realm.
    std::vector<SpawnZone> zones_;
    /// The mob regions, in map order. Same shape as a band and rebuilt beside
    /// them, but kept apart because they own no population: a region says which
    /// ROSTER belongs to a patch of map, and a band standing on it with no
    /// `mobs` of its own asks it what to grow. Drawing a region over empty
    /// ground spawns nothing -- which is the whole difference between "this
    /// ground is dangerous" and "this ground is the desert".
    std::vector<SpawnZone> regions_;
    /// Names a band asked for that the content defines neither a group nor a
    /// mob for, so each is reported once rather than on every attempt.
    std::set<std::string> unknownZoneMobs_;
    const WorldMaps* zoneMaps_ = nullptr;
    std::uint32_t zoneContentHash_ = 0;
    /// Starts due, so the first tick stocks the zones a player can already see
    /// rather than waiting out an interval first.
    double nextZoneMillis_ = 0;

    /// The players, the placement record and the despawn list, kept as members
    /// so the pass does not allocate once it has run a few times.
    std::vector<Viewer> viewers_;
    std::vector<Viewer> worldViewers_;
    /// Whether anyone at all stands in each realm this pass. What keeps an
    /// arena or maze mob alive: those realms are populated whole, so "near a
    /// player" there means "someone is in here".
    std::array<bool, kMaxRealms> realmOccupied_{};
    struct MobPlacement { Vec2 position; double radius = 0; Realm realm = Realm::Overworld; };
    std::vector<MobPlacement> mobPlacements_;
    std::vector<Entity> doomed_;
    std::vector<Entity> scratchChildren_;
};

} // namespace flix

FLIX_COMPONENT(flix::AmbientMob);
FLIX_COMPONENT(flix::NestWaves);
