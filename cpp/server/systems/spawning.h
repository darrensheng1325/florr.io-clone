#pragma once
// Keeping the world populated: what appears, where, and what is recycled.
//
// A 60000x60000 map cannot hold a full population. What is simulated instead
// is a moving neighbourhood: mobs appear inside each player's buffered
// viewport, and a mob nobody has
// been near for long enough is recycled. The map is dense exactly where
// someone is looking and empty everywhere else, so the tick cost scales with
// the number of players rather than with the area of the world.
//
// Two ceilings sit above that. A per-section cap keeps one biome from being
// stripped to feed another, and a global cap keeps a crowd of players from
// multiplying the population without bound -- twenty players standing apart
// each want their own neighbourhood, and without the cap they would get it.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "server/replication.h"
#include "shared/core/types.h"
#include "shared/core/world.h"
#include "shared/game/components.h"
#include "shared/game/config.h"
#include "shared/game/constants.h"
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

/// TypeScript's target density is 9000 mobs over a 60000x60000 world. A default
/// 1920x1080 viewport plus 500 units of buffer on each side therefore wants 16.
///
/// All three are the DEFAULTS, for a player whose client reported no viewport
/// at all. The reference sizes the keep-alive box, the rectangle a spawn is
/// sampled from and the target itself off each client's OWN reported viewport
/// (src/server/playerState.ts:1041, src/server/enemySpawner.ts:573-583), so an
/// ultrawide flower is owed more mobs over a wider rectangle.
inline constexpr int kMobsPerPlayer = 16;
inline constexpr double kSpawnViewportHalfWidth = kViewportWidth * 0.5 + kViewportBuffer;
inline constexpr double kSpawnViewportHalfHeight = kViewportHeight * 0.5 + kViewportBuffer;

/// The luck a spawn is charged to when nothing owns it. TypeScript's neutral
/// value is one rather than zero, and every point of it buys another
/// percentage point of tier upgrade on top of the base two
/// (src/server/shared/playerModifiers.ts:50, src/server/enemySpawner.ts:775).
inline constexpr double kNeutralSpawnLuck = 1.0;

/// Hard ceilings. The global one is what a full server actually costs; the
/// per-section one stops a party camping one biome from owning the whole
/// budget.
inline constexpr int kMaxLiveMobs = 900;
inline constexpr int kMaxMobsPerSection = 260;

/// The population the ambient spawner aims a section at. Below the hard cap on
/// purpose: nest escorts push past the target but never past the cap.
inline constexpr int kSectionTargetPopulation = 180;

/// Compatibility bound used by diagnostics; placement itself samples the
/// buffered rectangle above, just like samplePointInViewport().
inline constexpr double kSpawnRingMax = 1800.0;

/// Escorts stand off their nest by this much plus up to another body radius,
/// on a bearing of their own. The gap is the only difference the reference
/// draws between the guard a hole opens with and the waves it sends afterwards
/// (src/server/enemySpawner.ts:955, src/server.ts:1639).
inline constexpr double kInitialEscortGap = 30.0;
inline constexpr double kWaveEscortGap = 10.0;

/// No spawn lands closer than this to ANY player, not just the one whose
/// neighbourhood asked for it. Two players standing together would otherwise
/// spawn into each other's view.
inline constexpr double kMinSpawnDistance = 100.0;
inline constexpr double kMinMobSpawnSpacing = 80.0;
inline constexpr double kPreliminarySpawnRadius = 20.0;

/// Radius counted as "a player has been here", and how long a mob survives
/// without one. The radius is wider than the active radius so a player pacing
/// the edge of a group does not cause it to blink out and back.
inline constexpr double kMobDespawnDelayMillis = 30000.0;

/// The census and spawn pass run on their own cadence rather than every tick:
/// it is O(mobs x players), and at 30Hz nothing about the population changes
/// fast enough to need it more often than this.
inline constexpr double kPopulationIntervalMillis = 500.0;

/// Spawns granted to one player per pass. A trickle rather than a burst, so a
/// player who has just cleared an area watches it refill instead of being
/// surrounded a tick later.
inline constexpr int kMaxSpawnsPerPass = 3;

/// Ant Hell's roster is nothing but ants, and its soldiers chase at the
/// flower's own top speed: at open-world throughput they stack up faster than
/// anyone can cut through them. Three attempts in ten that land in the section
/// are thrown away, which is what makes it repopulate visibly slower than
/// Garden or Desert during a fight.
inline constexpr int kAntHellSection = 4;
inline constexpr double kAntHellSpawnScale = 0.7;

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

/// How far from where it was asked for a mob may end up. Zero in the
/// population pass, which rejects a bad point instead of moving it -- the
/// figure is the slack anything measuring a spawn against its request allows.
inline constexpr double kSpawnScatterRadius = 240.0;

/// Attempts allowed per requested spawn before the pass gives up on it. Every
/// sample is a fresh point in the player's own viewport, and a point in a lake
/// or a walled-off pocket is thrown away rather than nudged, so the count is
/// what decides how thin the population goes in bad terrain.
inline constexpr int kSpawnPlacementAttempts = 100;

/// The map's spawn rectangles are a SECOND population driver, independent of
/// the neighbourhood fill. The rectangle declares the tier that belongs in it,
/// which is the whole rarity progression of the map -- walking from the
/// beginner corner into the mythic band is walking from one rectangle into
/// another. The density fill stays out of them entirely
/// (src/server/spawnZoneManager.ts, src/server/enemySpawner.ts:756).
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

/// Attempts allowed per zone spawn. Lower than the neighbourhood fill's,
/// because a rectangle is a much smaller haystack than a viewport.
inline constexpr int kZonePlacementAttempts = 60;

/// The density a full zone is aimed at: the same 9000-mobs-over-the-world
/// figure the open map is held at, so a populated rectangle reads as ordinary
/// ground rather than as a pit.
inline constexpr double kTargetMobDensity = 9000.0 / (kWorldSize * kWorldSize);

/// Boss upkeep. One ultra is kept alive at all times, every section is kept
/// stocked with a super, and a unique is rolled for whenever a super exists.
/// The pass is slow on purpose: a boss is meant to be hunted, not farmed.
inline constexpr double kBossIntervalMillis = 60000.0;
inline constexpr double kSuperInUltraZoneChance = 0.75;
inline constexpr double kUniqueSpawnChance = 0.25;

/// The one place `super` appears without the boss pass: an ultra rectangle
/// rolls it in a hundred.
inline constexpr double kUltraZoneSuperChance = 0.01;

/// Attempts a boss gets at stepping out of a player's lap, and at landing
/// inside the slice of a zone that lies in the section being filled.
inline constexpr int kBossPlacementAttempts = 50;
inline constexpr int kZoneSectionAttempts = 50;

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
        std::array<int, kSectionCount> perSection{};
        int spawnedTotal = 0;                          ///< cumulative, since construction
        int despawnedTotal = 0;
    };

    /// One player's neighbourhood, as this system has to see it.
    ///
    /// The runtime hands the pass positions, and two of the reference's rules
    /// need more than a coordinate: the box that keeps a mob alive is the size
    /// the CLIENT reported (src/server/playerState.ts:1041), and the tier
    /// drift is biased by that player's luck (src/server/enemySpawner.ts:775).
    /// Both live on the flower's own entity, so each position is paired back
    /// up with the player standing on it. A position matching no player -- a
    /// harness driving the spawner with bare coordinates -- keeps the
    /// reference's defaults, which is the 1920x1080 flower it assumes.
    struct Viewer {
        Vec2 position;
        /// Half the reported viewport plus the spawn buffer, per axis: the box
        /// a mob has to be inside to count as seen, the rectangle this
        /// player's spawns are sampled from, and the area the target is
        /// derived from.
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

    /// The map's annotation layer, or null.
    ///
    /// Spawn rectangles are geography: without them every square of the world
    /// rolls the same tier spread and the map has no progression at all. Null
    /// in a unit test and in any harness that has no bundle to read, which
    /// leaves the neighbourhood fill running alone over the whole map.
    const MapData* mapData = nullptr;

    /// A boss the last pass admitted, for whoever owns the chat channel.
    ///
    /// The reference announces supers and uniques with a per-player line whose
    /// wording depends on where that player is standing; this system has no
    /// view of the socket list, so it reports rather than broadcasts. Ultras
    /// are deliberately silent and never appear here.
    struct BossSpawn {
        std::uint16_t mobIndex = 0;
        Rarity rarity = Rarity::Super;
        Vec2 position;
    };
    /// Drained by the runtime. Bounded rather than unbounded, so a server that
    /// never drains it keeps the newest announcements instead of growing.
    std::vector<BossSpawn> bossSpawns;

    /// `players` is every connected flower with a body; the neighbourhood
    /// fill, the zones and the boss pass serve the OVERWORLD ones, and the
    /// census keeps a mob in another realm alive for as long as anyone is in
    /// that realm at all -- the arena and the maze are populated by
    /// ModeSpawner, whole, not by viewport.
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

    /// The weighted type roll for one section: spawn_weight over the mobs whose
    /// `section` list contains it. kInvalidIndex when the section has none.
    std::uint16_t chooseMobType(const ContentRegistry& content, int section, Rng& rng);
    std::uint16_t chooseMobType(const ContentRegistry& content, int section,
                                Rarity rarity, Rng& rng);

    /// Natural tier roll plus the reference's upgrade-first/downgrade-second
    /// drift, then raised for a direct spawn of a min-rarity mob.
    static Rarity rollRarity(const MobConfig& config, Rng& rng);
    static Rarity rollNaturalRarity(int section, double luck, Rng& rng);

    const Census& census() const { return census_; }

    /// Runs the boss pass on the next tick instead of waiting out its timer.
    ///
    /// The admin console's `/admin spawn_special_mobs` asks for exactly this
    /// and nothing more: the pass itself decides what is missing -- one ultra,
    /// a super per bare section, a unique beside a super -- so forcing the
    /// clock forward is the whole command, and duplicating that reasoning at
    /// the call site would be a second answer to the same question.
    void requestSpecialPass() { nextBossMillis_ = 0; }

private:
    /// One `spawn` rectangle and where it is in its fill cycle.
    ///
    /// The cycle resets the moment the rectangle leaves every viewport, so a
    /// player walking back into an area they cleared an hour ago finds it
    /// freshly stocked rather than mid-trickle.
    struct SpawnZone {
        Rect bounds;
        Rarity tier = Rarity::Common;
        /// The 3x3 sections this rectangle touches. The density fill asks
        /// "am I in a zone?" of every candidate point it samples, and with 148
        /// rectangles on the map that test is worth reducing to one integer
        /// compare for the eight ninths of them that are nowhere near.
        std::uint16_t sections = 0;
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

    void takeCensus(const ContentRegistry& content, const std::vector<Viewer>& viewers,
                    double nowMillis, CommandBuffer& commands);
    void fillNeighbourhoods(World& world, const Terrain& terrain, const ContentRegistry& content,
                            const std::vector<Viewer>& viewers, Rng& rng, double nowMillis);
    void runNests(World& world, const Terrain& terrain, const ContentRegistry& content,
                  Rng& rng, double nowMillis);
    void expireEscorts(double dt, CommandBuffer& commands);

    /// Stocks every spawn rectangle a player can see, at the tier the map
    /// declares for it. The other half of the same rule lives in
    /// placementAllowed, which keeps the neighbourhood fill out of them.
    void runSpawnZones(World& world, const Terrain& terrain, const ContentRegistry& content,
                       const std::vector<Viewer>& viewers, Rng& rng, double nowMillis);

    /// One mob inside `zone`, or NULL_ENTITY when the rectangle had nowhere to
    /// put it. The tier is the zone's own, never a natural roll.
    Entity spawnInZone(World& world, const Terrain& terrain, const ContentRegistry& content,
                       const SpawnZone& zone, const std::vector<Viewer>& viewers, Rng& rng,
                       double nowMillis);

    /// Tier and mob from a biome's own spawn table, mirroring the reference's
    /// selectBiomeSpawn (src/server/enemySpawner.ts:477).
    ///
    /// A biome that declares a table OVERRIDES both the section roll and the
    /// spawn rectangle it sits in -- the reference tests the biome first in
    /// both of its spawn paths -- and a row that names a mob pins the spawn to
    /// it. That naming is the whole reason the target dummy exists: it is
    /// `neverAmbient`, so the section roll will never produce one, and the
    /// map's four dummy biomes are the only thing that ever asks for it.
    ///
    /// False when the table names a mob the content does not have, or when the
    /// section admits nothing at the rolled tier -- both "no spawn this
    /// attempt" rather than a fallback, as the reference has them.
    bool chooseBiomeSpawn(const ContentRegistry& content, const MapElement& biome, int section,
                          double luck, Rng& rng, std::uint16_t& typeOut, Rarity& rarityOut);

    /// True when this section already holds a mob of this type and tier that
    /// nothing will ever clear away.
    ///
    /// Only asked about `neverAmbient` mobs, which is the target dummy: it
    /// never despawns and is effectively unkillable, so a duplicate that slips
    /// through is permanent. One of each rarity per section, checked against
    /// the FINAL tier after any drift (src/server/enemySpawner.ts:112).
    bool permanentFixtureExists(World& world, std::uint16_t mobIndex, Rarity rarity, int section);

    /// Keeps the world's boss population topped up: one ultra, one super per
    /// section, and a unique rolled for whenever a super is out.
    void runSpecialMobs(World& world, const Terrain& terrain, const ContentRegistry& content,
                        const std::vector<Viewer>& viewers, Rng& rng, double nowMillis);

    /// Places one boss in a rectangle of the tier's own kind. `targetSection`
    /// is the section a super is being spawned FOR (-1 otherwise), and
    /// `superSections` vetoes a final position whose section already has one.
    Entity spawnSpecialMob(World& world, const Terrain& terrain, const ContentRegistry& content,
                           Rarity tier, int targetSection,
                           const std::array<bool, kSectionCount>* superSections,
                           const std::vector<Viewer>& viewers, Rng& rng, double nowMillis);

    /// Queues one boss for whoever owns the chat channel.
    void announceBoss(std::uint16_t mobIndex, Rarity rarity, Vec2 position);

    void rebuildZones();

    /// Mobs the last census saw inside `bounds`, inclusive on every edge as
    /// the reference's own count is.
    int countMobsInZone(const Rect& bounds) const;

    /// True when a body of `halfSize` at `position` would touch a mob the last
    /// census saw, with `extraGap` of clearance on top. One scan behind the
    /// spacing test, the finalizer's re-test and the boss's own check.
    bool crowdedAt(Vec2 position, double halfSize, double extraGap) const;

    bool inAnySpawnZone(Vec2 position, int section) const;

    /// A uniform point in some rectangle of that tier, or false when the map
    /// declares none. `...InSection` restricts it to the part of the rectangle
    /// that lies inside one 20000-unit section.
    bool randomPointInZoneType(Rarity tier, Rng& rng, Vec2& out) const;
    bool randomPointInZoneTypeInSection(Rarity tier, int section, Rng& rng, Vec2& out) const;

    /// True when `position` is somewhere a new mob may legally appear.
    bool placementAllowed(const Terrain& terrain, const std::vector<Viewer>& viewers,
                          Vec2 position, int& sectionOut) const;

    Entity spawnMobAt(World& world, const Terrain& terrain, const ContentRegistry& content,
                      std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                      double nowMillis, Rng& rng, int depth);

    /// Lays a centipede's body out behind its head, each segment linked to the
    /// one in front. Driven from spawnMobAt so that every path to a head --
    /// ambient roll, nest, script, pet -- gets the chain, which is why no
    /// caller can produce a lone head.
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

    void rebuildCandidates(const ContentRegistry& content);

    /// Mobs potentially eligible in one section. Rarity-dependent section
    /// overrides are applied by chooseMobType after the tier has been rolled.
    struct SectionCandidates {
        std::vector<std::uint16_t> mobs;
        std::vector<double> cumulative;
    };

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

    std::array<SectionCandidates, kSectionCount> candidates_;
    const ContentRegistry* candidateContent_ = nullptr;
    std::uint32_t candidateHash_ = 0;

    Census census_;
    double nextPopulationMillis_ = 0;

    /// Rebuilt when `mapData` changes, which in the server is once.
    std::vector<SpawnZone> zones_;
    const MapData* zoneMap_ = nullptr;
    /// Both start due, so the first tick stocks the zones a player can already
    /// see and puts the world's ultra out rather than waiting a minute for it.
    double nextZoneMillis_ = 0;
    double nextBossMillis_ = 0;
    /// Whether the boot-time boss pass has happened. It is the one pass that
    /// runs over an empty server.
    bool bossPassRan_ = false;

    /// Per-player neighbourhood counts, the players themselves and the despawn
    /// list, kept as members so the pass does not allocate once it has run a
    /// few times.
    std::vector<Viewer> viewers_;
    std::vector<Viewer> worldViewers_;
    /// Whether anyone at all stands in each realm this pass. What keeps an
    /// arena or maze mob alive: those realms are populated whole, so "near a
    /// player" there means "someone is in here".
    std::array<bool, kRealmCount> realmOccupied_{};
    std::vector<int> neighbours_;
    struct MobPlacement { Vec2 position; double radius = 0; };
    std::vector<MobPlacement> mobPlacements_;
    std::vector<Entity> doomed_;
    std::vector<Entity> scratchChildren_;
};

} // namespace flix

FLIX_COMPONENT(flix::AmbientMob);
FLIX_COMPONENT(flix::NestWaves);
