#pragma once
// Keeping the world populated: what exists, where, and what is simulated.
//
// THE AUTHOR DRAWS WHERE THE MOBS ARE. A map carries SPAWN BANDS -- shapes on
// an object layer, each with a `difficulty` -- and a band owns a population of
// its own, sized by the area of its outline. That is the only source of
// ambient mobs there is. Ground with no band over it grows nothing, ever, so a
// map its author has drawn no band on has no mobs at all: that is a fact about
// the data, not a fault in this file, and the map's load line says so out loud
// when it happens.
//
// THE WHOLE MAP IS ALWAYS STOCKED. Every band holds its full population at all
// times, whether or not anybody is looking at it, so walking into a corner
// nobody has been to for an hour is walking into a full band rather than into
// an empty one that starts filling as you arrive.
//
// What makes that affordable is that a stocked mob is NOT AN ENTITY. A band's
// population is held as LatentMob records -- a position, a type and a tier,
// and nothing else -- and a record becomes a real entity only when somebody's
// viewport reaches it (promoteLatent), and goes back to being a record when
// nobody has been near it for the grace period (takeCensus). So the number of
// mobs the tick SIMULATES follows the number of players, exactly as it did
// when the far side of the map was simply empty, while the number of mobs the
// world CONTAINS follows its area. A latent mob costs one small struct and one
// distance test per population pass; it is in no query, no archetype, no
// broadphase and no snapshot.
//
// Two rules keep that population EVEN rather than merely correct in total. A
// mob nobody has been near goes back to being a record where it stands, not
// where the band would next have sampled; and a mob that was KILLED has its
// slot handed back within kRespawnScatter of where it died, after a short
// wait. Without the second one a player farming one corner of a band strips it
// while the far side of the same band quietly goes over density -- and the map
// reports itself full the whole time, which is the hardest kind of wrong to
// see.
//
// The exceptions are BOSSES -- super and above -- and the target dummies.
// Both are placed live and stay live wherever they stand, however long nobody
// looks at them. A boss is an EVENT: chat is told the moment one appears, the
// bot controller is told with it, and it waits where it spawned until
// something kills it.
//
// One ceiling sits above all of it -- a global cap on LIVE mobs, so a crowd of
// players spread over many bands cannot multiply the simulated population
// without bound. The latent population needs no such cap: it is the map's own
// area times a density.
//
// The ARENA and the MAZE are not map ground: they are generated, they carry no
// object layer and no band covers them, and ModeSpawner (mode_spawning.h)
// populates each one whole, with live entities. Nothing in this file stocks
// them, and nothing in them is ever latent.

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
    /// The spawn band that owns this mob, or kInvalidIndex when nothing does.
    ///
    /// It is what makes a live mob and a latent record the same thing seen in
    /// two states: the band counts both against its target, and a mob nobody
    /// has been near goes back into THIS band's records rather than
    /// disappearing. kInvalidIndex is everything a band did not place -- a
    /// nest's escorts, a centipede's body, the arena and maze populations, a
    /// mob an operator conjured -- and such a mob is recycled the old way,
    /// destroyed outright, because no band is keeping a slot for it.
    std::uint16_t zone = kInvalidIndex;
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
// The latent population
// ---------------------------------------------------------------------------

/// A mob that EXISTS but is not simulated.
///
/// All but the handful of mobs somebody can actually see are one of these. It
/// is the whole reason the map can be full everywhere: the far side of the
/// world costs a few dozen bytes per mob and one distance test per population
/// pass, instead of an entity in every query the tick runs.
///
/// It holds what a record has to remember to become the same mob later, and
/// deliberately no more. The size jitter, the health, the wounds and the
/// aggro are NOT among them: a promoted record arrives whole and freshly
/// rolled, exactly as the mob it replaced used to when the recycler destroyed
/// it and the band grew another. The realm and the band are not stored either
/// -- a record lives inside the band that owns it, which knows both.
struct LatentMob {
    Vec2 position;
    /// Earliest time this record may come to life.
    ///
    /// Zero for the vast majority: a record placed where nobody was looking is
    /// ready the instant it exists, which is what makes an unvisited band full
    /// the moment somebody walks into it. It is only the replacement for a mob
    /// killed IN somebody's view that waits, and the wait is what stops a
    /// cleared screen refilling in front of the player who cleared it.
    double readyMillis = 0;
    std::uint16_t mobIndex = kInvalidIndex;
    Rarity rarity = Rarity::Common;
};

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

/// The DEFAULT buffered viewport: half a 1920x1080 screen plus 500 units of
/// buffer on each side. It is the box a mob has to be inside to count as seen,
/// and it is what decides which bands are worth waking.
///
/// A default, for a player whose client reported no viewport at all. The
/// reference sizes the keep-alive box off each client's OWN reported viewport
/// (src/server/playerState.ts:1041), so an ultrawide flower keeps a wider strip
/// of the map alive around it.
inline constexpr double kSpawnViewportHalfWidth = kViewportWidth * 0.5 + kViewportBuffer;
inline constexpr double kSpawnViewportHalfHeight = kViewportHeight * 0.5 + kViewportBuffer;

/// How much WIDER the box that wakes a latent mob is than the box that keeps a
/// live one awake.
///
/// The two differ on purpose, and the gap is the hysteresis of the whole
/// scheme: with one box, a player pacing its edge would promote and demote the
/// same mob over and over. It also covers the population pass's own cadence --
/// a flower crosses a few hundred units between passes, and a mob has to be
/// alive before it is on screen rather than as it arrives there.
inline constexpr double kLatentWakeMargin = 700.0;

/// The one hard ceiling: what a full server actually costs to SIMULATE. Every
/// path that creates an entity tests it, so a hundred bands in view cannot
/// between them put more than this in the world.
///
/// It says nothing about how many mobs the world holds: the map is stocked to
/// its own area whatever this is, and the rest of that population is latent.
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
/// without one before it goes back to being a record. The radius is wider than
/// the active radius so a player pacing the edge of a group does not cause it
/// to blink out and back.
inline constexpr double kMobDespawnDelayMillis = 30000.0;

/// The census runs on its own cadence rather than every tick: it is
/// O(mobs x players), and at 30Hz nothing about the population changes fast
/// enough to need it more often than this. The promotion pass rides with it,
/// which is what kLatentWakeMargin is sized against.
inline constexpr double kPopulationIntervalMillis = 500.0;

/// Cadence a wave nest used to send on, kept as the interval anything
/// stepping a nest through its bands walks by. The tick itself reads it
/// nowhere: a wave follows the hole's health, not a clock (see NestWaves).
inline constexpr double kNestWaveIntervalMillis = 15000.0;

/// Escorts one wave-nest is expected to have out at once. A hole is capped
/// nowhere -- every band it crosses fires in full -- so this bounds nothing;
/// it is the headroom a section holding a nest is measured against.
inline constexpr int kMaxNestChildren = 12;

/// How deep nesting may go. A nest whose escorts are themselves nests is legal
/// data and would otherwise recurse until the world ran out of memory.
inline constexpr int kMaxNestDepth = 2;

/// The map's spawn bands are the whole of the ambient population. A band
/// declares its DIFFICULTY, which is the whole rarity progression of the map --
/// walking from the beginner corner into a difficulty-100 band is walking from
/// one shape into another -- and it owns the mobs standing inside it
/// (src/server/spawnZoneManager.ts).
inline constexpr double kZoneIntervalMillis = 1000.0;

/// Records one band may add per stocking pass.
///
/// It is what spreads a cold start over a few seconds rather than paying for a
/// whole map in one tick. Generous, because a record is cheap: the expensive
/// part of stocking is the rejection sampling, not what it produces.
inline constexpr int kZoneStockPerPass = 32;

/// Records one band may bring to life per population pass.
///
/// Bounds the one genuinely bursty moment in the scheme -- a player arriving
/// somewhere by door or teleporter, with a whole band's worth of records
/// suddenly inside their viewport. They wake over the next few passes instead
/// of all on the tick they were noticed.
inline constexpr int kZoneWakePerPass = 24;

/// How far from where a mob was lost its replacement is placed.
///
/// The band's population is returned TO THE PLACE IT WAS TAKEN FROM, which is
/// what keeps the density even rather than merely the total right. Spread the
/// deficit uniformly over the outline instead -- these bands are tens of
/// millions of square units and a viewport is six -- and a player farming one
/// spot strips it while the far side of the same band quietly goes over
/// density: the map says it is full and the screen says it is empty.
///
/// About half a screen, so a replacement is near enough to restore the
/// neighbourhood and far enough not to appear on the exact spot the last one
/// died on.
inline constexpr double kRespawnScatter = 900.0;

/// How long a replacement for a mob killed inside somebody's viewport waits
/// before it may appear, rolled per record.
///
/// This is the old wave-and-trickle rhythm, restated where it belongs: the
/// window over which a cleared screen seeps back rather than snapping back.
/// Ground nobody was looking at is stocked with no delay at all, which is the
/// whole point of the change -- the map does not wait to be full.
///
/// It is the one number that trades the map's stated density against how a
/// fight feels, and the trade is arithmetic rather than taste: a screen holds
/// about five mobs at this density, and a player killing one every ten seconds
/// keeps (kill rate x mean delay) of its slots waiting at any moment. At the
/// old band's 4..45 seconds that was half the screen permanently empty. Kept
/// short enough that the dip is a dip rather than the normal state, and long
/// enough that nothing appears in the hole the last kill left.
inline constexpr double kInViewRespawnMinMillis = 3000.0;
inline constexpr double kInViewRespawnMaxMillis = 12000.0;

/// Attempts allowed per placement before it gives up on this pass. A point in
/// a lake, in a wall or in somebody's lap is thrown away rather than nudged, so
/// this is what decides how thin a band over bad terrain goes.
inline constexpr int kZonePlacementAttempts = 60;

/// The density a full band is aimed at: the reference's 9000 mobs over its
/// 60000-unit square, which is what a band's target population is derived from
/// (and what mode_spawning.h sizes a maze corridor by), so a stocked band reads
/// as ordinary ground rather than as a pit.
inline constexpr double kTargetMobDensity = 9000.0 / (kWorldSize * kWorldSize);

/// The tier at which a spawn is worth telling the whole server about, and at
/// which a mob is simulated wherever it stands. Supers, uniques and apexes are
/// events; everything below is scenery.
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
        int latent = 0;                                ///< stocked but not simulated
        int spawnedTotal = 0;                          ///< cumulative, since construction
        int despawnedTotal = 0;
        int promotedTotal = 0;                         ///< records brought to life
        int demotedTotal = 0;                          ///< live mobs put back to records
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
        /// The realm this flower is standing in. Two maps' coordinates overlap
        /// numerically, and a viewer filed under the wrong one would wake a
        /// second map because somebody was standing at the same numbers in the
        /// first.
        Realm realm = Realm::Overworld;
        /// Half the reported viewport plus the spawn buffer, per axis: the box
        /// a mob has to be inside to count as seen.
        Vec2 half{kSpawnViewportHalfWidth, kSpawnViewportHalfHeight};
        double luck = kNeutralSpawnLuck;
    };

    /// The live-mob ceiling this pass will not spawn past.
    ///
    /// A variable rather than kMaxLiveMobs directly because the admin console
    /// can raise or lower it at runtime (`/admin set_max_enemies`), which is
    /// the one knob an operator reaches for when a box is struggling. Every
    /// path that creates an entity tests it, so lowering it stops new mobs
    /// immediately and lets the existing population drain rather than culling
    /// anything. It does not shrink the map's population, only how much of it
    /// is awake at once.
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
    /// Every path that creates a mob reports through here -- a band stocking
    /// itself, a nest, the arena, an operator's console -- so there is exactly
    /// one place a boss can come from as far as the rest of the server is
    /// concerned, and no way to add a spawn path that quietly appears without
    /// a word. The reference announces supers and uniques with a per-player
    /// line whose wording depends on where that player is standing; this
    /// system has no view of the socket list, so it reports rather than
    /// broadcasts. Anything below kAnnouncedRarity -- an ultra included -- is
    /// deliberately silent and never appears here.
    struct BossSpawn {
        /// The boss itself, so the runtime can hand the bot controller
        /// something it can follow rather than a coordinate that is already
        /// stale. Alive when it is queued; check before use, because a boss
        /// can die before the queue is drained.
        Entity entity = NULL_ENTITY;
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

    /// `players` is every flower with a body -- bots included, because a bot
    /// standing in a band is something the band has to be awake for. The
    /// BANDS do not care: every band is stocked to its full population whether
    /// or not anybody is anywhere near it. What the list decides is which of
    /// that population is awake, and the census keeps a mob in another realm
    /// alive for as long as anyone is in that realm at all -- the arena and the
    /// maze are populated by ModeSpawner, whole, not by viewport and not by
    /// band.
    void run(World& world, const Terrain& terrain, const ContentRegistry& content,
             const std::vector<RealmPoint>& players, Rng& rng, double nowMillis, double dt,
             CommandBuffer& commands);

    /// Places one mob, with its nest escorts if it has any, and returns it.
    /// NULL_ENTITY when `mobIndex` names nothing.
    ///
    /// `position` is a request, not a promise: it is pushed out of the terrain
    /// before use, so a caller may hand over a point in a wall and still get a
    /// mob standing somewhere legal.
    ///
    /// A boss placed this way announces itself like any other, which is what
    /// makes "every boss is announced" a property of the system rather than of
    /// each caller.
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

    /// How many mobs stand on the map without being simulated, and what and
    /// where they are.
    ///
    /// The world is not the whole state any more -- most of the population is
    /// in here -- so anything asserting on how full the map is, or reporting
    /// it to an operator, has to read both. `latentAt` walks every band, which
    /// is a diagnostic price, not a tick price.
    int latentCount() const;
    /// Every record in `realm`, appended to `out` with the band it belongs to.
    struct LatentSite {
        Vec2 position;
        std::uint16_t mobIndex = kInvalidIndex;
        Rarity rarity = Rarity::Common;
        double difficulty = 0.0;
    };
    void latentSites(Realm realm, std::vector<LatentSite>& out) const;

private:
    /// One `spawn` shape, its population, and the part of that population that
    /// is not currently a mob.
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
        /// area of its own outline. `latent.size() + liveMobs` is measured
        /// against it, and the band stocks itself until they meet.
        int targetMobs = 1;
        /// This band's share of the world that is not simulated. Held HERE
        /// rather than in one flat list because both hot questions are per
        /// band: "is this band worth walking at all" is one rectangle test
        /// against the viewports, and "is this band full" is a size().
        std::vector<LatentMob> latent;
        /// Live mobs this band placed, as of the last census. Stale by up to
        /// one census between passes, which self-corrects: an over-count
        /// delays a replacement by half a second, an under-count is spent on a
        /// record the next census absorbs.
        int liveMobs = 0;
    };

    void bind(World& world);

    /// Pairs each position the caller handed over with the flower standing on
    /// it. The list stays the caller's -- it decides WHO the population is
    /// kept awake for -- and this only fills in what a bare coordinate cannot
    /// say.
    void gatherViewers(World& world, const std::vector<RealmPoint>& players);

    /// Counts what is alive, refreshes every mob's "last seen by somebody"
    /// stamp, and puts the ones nobody has been near for the grace period back
    /// into their band's records. It also rebuilds the placement record the
    /// stocking pass spaces itself against, which is why it runs before one.
    void takeCensus(const ContentRegistry& content, const std::vector<Viewer>& viewers,
                    double nowMillis, CommandBuffer& commands);
    void runNests(World& world, const Terrain& terrain, const ContentRegistry& content,
                  Rng& rng, double nowMillis);
    void expireEscorts(double dt, CommandBuffer& commands);

    /// Tops every spawn band up to the population its own area buys, wherever
    /// it is and whoever is looking. THE ONLY ambient spawn path there is: a
    /// point no band covers is never sampled by anything, so it never grows a
    /// mob.
    void stockSpawnZones(World& world, const Terrain& terrain, const ContentRegistry& content,
                         const std::vector<Viewer>& viewers, Rng& rng, double nowMillis);

    /// One mob's worth of population added to `zone` -- a record, or an entity
    /// when the roll came out at a tier that has to be awake. False when the
    /// band's outline had nowhere to put it, which ends the pass for that band.
    ///
    /// `scatter` says WHERE. Zero or less samples the band's whole outline,
    /// which is the cold start; a positive radius samples within that of
    /// `anchor`, which is how a casualty's slot goes back where it was lost.
    bool stockZone(World& world, const Terrain& terrain, const ContentRegistry& content,
                   SpawnZone& zone, std::uint16_t zoneIndex, const std::vector<Viewer>& viewers,
                   Rng& rng, double nowMillis, Vec2 anchor = Vec2{}, double scatter = 0.0);

    /// Gives each band back the population that died in it, where it died.
    ///
    /// The counterpart to takeCensus's sleep path: a mob that was KILLED
    /// leaves no record behind it, so without this its slot would be refilled
    /// by the band-wide top-up -- somewhere else entirely, in a band the size
    /// of a district. A corpse is claimed exactly once, by clearing the band
    /// it belonged to off it.
    void bankCasualties(World& world, const Terrain& terrain, const ContentRegistry& content,
                        const std::vector<Viewer>& viewers, Rng& rng, double nowMillis);

    /// Brings every record somebody can now see to life. The cheap half is the
    /// rejection: a band whose box no viewport touches is one rectangle test.
    void promoteLatent(World& world, const Terrain& terrain, const ContentRegistry& content,
                       const std::vector<Viewer>& viewers, Rng& rng, double nowMillis);

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
    void announceIfNotable(const ContentRegistry& content, Entity entity, std::uint16_t mobIndex,
                           Rarity rarity, Vec2 position, Realm realm);

    void rebuildZones(const ContentRegistry& content);

    /// The mob a band should place: the band's own distribution when it
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

    /// True when a body of `halfSize` at `position` would touch a mob the last
    /// census saw, or a record this band already holds, with `extraGap` of
    /// clearance on top. The one scan behind every spacing test there is.
    ///
    /// The live half is world-wide and the latent half is the band's own,
    /// which is the one seam in it: two records either side of a band boundary
    /// can stand closer than the gap. That is a pixel of overlap between two
    /// mobs nobody is looking at yet, and it buys a stocking pass that costs
    /// the band's own population rather than the map's.
    bool crowdedAt(Realm realm, Vec2 position, double halfSize, double extraGap,
                   const SpawnZone& zone) const;

    /// True when `position` is inside somebody's viewport, grown by `margin`.
    bool seenBy(const std::vector<Viewer>& viewers, Realm realm, Vec2 position,
                double margin) const;

    Entity spawnMobAt(World& world, const Terrain& terrain, const ContentRegistry& content,
                      std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                      double nowMillis, Rng& rng, int depth, std::uint16_t zone);

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
    /// The corpses, which the pass above steps over: a band's own dead are the
    /// population it has to put back.
    std::optional<Query<MobTag, Transform, AmbientMob, Dead>> casualties_;
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
    /// once. Bands from EVERY staged map, each tagged with its realm, each
    /// holding its own share of the world's latent population.
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
    /// Starts due, so the first tick stocks the map rather than waiting out an
    /// interval first.
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
    /// Where each of this pass's dead fell, and which band is owed it.
    /// Collected before any of it is spent, because spending it creates
    /// entities and that moves the very rows the walk is holding.
    struct Casualty { Vec2 position; std::uint16_t zone = kInvalidIndex; };
    std::vector<Casualty> casualtyList_;
    std::vector<Entity> doomed_;
    std::vector<Entity> scratchChildren_;
};

} // namespace flix

FLIX_COMPONENT(flix::AmbientMob);
FLIX_COMPONENT(flix::NestWaves);
