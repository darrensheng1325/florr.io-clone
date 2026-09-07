#pragma once
// Mob intent: what every wild mob and every summoned pet decides to do this
// tick.
//
// The system belongs to the INTENT phase and writes exactly two things -- the
// mob's velocity for this tick (Motion::velocity) and where it points
// (Transform::angle). Positions belong to the movement phase, which integrates
// that velocity as it stands, applies knockback and pushes the body out of
// walls.
//
// The velocity is published RAW rather than eased toward, because the
// reference server has no acceleration model for a mob: a chase is a fixed
// step at the mob's speed, and an idle mob runs the gardn stop-and-go machine,
// which carries its own friction, its own clamp and its own drift store
// (PassiveMotion::velocity). Easing either of those a second time is an
// invented acceleration that shortens every hop and every pursuit. There is
// one deliberate exception, at placeFollower(): a centipede's trailing segment
// has no motion of its own, its place is a CONSTRAINT on the segment ahead of
// it, and expressing a constraint as a velocity leaves the chain permanently a
// tick behind and visibly elastic.
//
// Damage is not this file's business either. A mob that reaches its target
// stamps MobAi::lastAttackMillis; the combat system reads the overlap and
// applies ContactDamage. Two systems that can both hurt the same player in one
// tick is how a mob ends up dealing double damage on the tick it arrives. A
// volley is the exception and belongs here, because WHEN a mob shoots is a
// decision and the shot is aimed along the pre-move bearing to the target.
// Damage is READ, though: what turns a neutral mob on a player is its damage
// ledger growing, since over there every source funnels through one credit
// call, while the hurt flash is lit by direct hits alone and is otherwise
// purely something to look at.
//
// What shapes the rest is the cost model. A 60000-unit world holds thousands
// of mobs and a few dozen players, so the two gates below are not tuning, they
// are the reason the tick fits its budget:
//
//   LOD      a mob further than kMobActiveRadius from every active player
//            thinks once every kMobFarStride ticks instead of every tick, and
//            holds still on the ticks in between. It is not frozen: it
//            simulates at a fifth of the rate, which is what stops the far
//            world draining into wall lines while nobody is looking at it. The
//            one thing that keeps full rate out there is the idle drift, which
//            carries no gate in the reference either. The caller supplies the
//            player positions, so a region with nobody in it costs one
//            distance test per mob.
//   CADENCE  every mob near a player runs its whole brain every tick,
//            acquisition included -- that is what the reference pays, and a
//            mob that scans on a timer ignores a player who walked into its
//            range for as long as the timer says. LOD, not a clock, is what
//            bounds the cost. The one genuinely periodic heading left, a
//            sandstorm's, keeps a clock of its own rather than a shared one:
//            it re-rolls three times a second, which is far faster than
//            anything else here would want to re-decide.
//
// Pets get their own pass rather than being fed through the wild-mob one: a
// summoned mob follows its owner, pops back to them when a wall breaks line of
// sight, and is retired when it drifts off their screen. None of that is "wild
// mob with a different target list".

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shared/core/types.h"
#include "shared/core/world.h"
#include "shared/game/components.h"
#include "shared/game/spatial.h"
#include "shared/game/terrain.h"

namespace flix {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

/// A turn rate, in radians per second, for a caller of steerFacing() that
/// wants one.
///
/// The AI is not such a caller: the reference assigns a mob's facing outright
/// from the vector it is travelling along, every tick, with no rate limit
/// anywhere -- so a mob that turns round faces its new heading on the tick it
/// picks it rather than swinging there over a third of a second while its
/// sprite points at where it used to be going.
inline constexpr double kMobTurnRate = 9.0;

/// Added to the two bodies' radii before a mob counts as in contact, so an
/// attack lands when the sprites touch rather than when the centres do.
inline constexpr double kMobContactSlack = 4.0;

/// Line-of-sight rays one acquisition may spend. Candidates are tested
/// nearest-first, so the cap costs only the pathological case of eight players
/// stacked behind the same wall.
inline constexpr int kTargetLosRayCap = 8;

// -- the idle drift machine --------------------------------------------------
//
// Every idle mob that is not a centipede head runs gardn's two-state hop:
// stand still for a second, pick a heading, coast half a second on friction
// alone, then accelerate through a two-second parabolic ramp and stop. It is
// what gives the field its characteristic pulse -- a mob that instead cruises
// on a heading at a fraction of its speed reads as gliding, however close the
// average distance travelled comes out.

/// Radius the drift's acceleration and the wander range are stated against, so
/// a mob ten times wider hops ten times as far rather than crawling relative to
/// its own body. Above the common tier's radius so small mobs settle down in
/// absolute terms too.
inline constexpr double kWanderRefRadius = 50.0;

/// Fraction of the drift velocity lost per TICK. Calibrated per tick and not
/// per second, exactly as in the reference: it is gardn's 1/3 at 20 TPS
/// restated for this server's 30.
inline constexpr double kPassiveFriction = 0.25;

/// The hop's acceleration, as a fraction of the mob's speed, before the size
/// factor and the ramp. Distance per hop is the sum of the accelerations
/// divided by the friction, so scaling this is what scales the hop.
inline constexpr double kPassiveAccelScale = 0.25;

/// The two-state clock, in milliseconds: a second idle, then a Moving phase
/// that coasts for the first half-second and ramps over the two after it.
inline constexpr double kPassiveIdleMillis = 1000.0;
inline constexpr double kPassiveCoastMillis = 500.0;
inline constexpr double kPassiveRampMillis = 2000.0;
inline constexpr double kPassiveMoveMillis = kPassiveCoastMillis + kPassiveRampMillis;

/// Ceiling on the drift, so radius-proportional acceleration cannot drift an
/// apex mob at seven times a player's top speed. The reference states it as
/// 300/30 units per tick, which is this in units per second.
inline constexpr double kMaxWanderSpeed = kPlayerMaxSpeed;

// -- the bee cruise ----------------------------------------------------------
//
// Bees do not hop. They cruise continuously along a heading that sways
// sinusoidally -- the wavy flight line -- re-picking a base heading every five
// seconds and pulsing their speed once per one-and-a-half.

inline constexpr double kBeeHeadingMillis = 5000.0;
inline constexpr double kBeeCruiseAccelScale = 3.0;
/// Peak angular sway rate, radians per second.
inline constexpr double kBeeWobbleRate = 1.5;
inline constexpr double kBeePulsePeriodMillis = 1500.0;
inline constexpr double kBeePulseMillis = 500.0;
inline constexpr double kBeePulseScale = 0.5;

// -- walking to a point ------------------------------------------------------
//
// Centipede heads and ownerless pets wander to a POINT rather than on a
// heading, which is what keeps a centipede's turns long and smooth instead of
// hopping like the animals behind it.

/// Base wander range, scaled per mob by its size factor.
inline constexpr double kEnemyWanderRange = 200.0;

/// How long a wander destination stands before a fresh one is picked.
inline constexpr double kWanderRepickMillis = 3000.0;

/// Inside this the destination counts as reached and the mob stops, rather
/// than jittering across the last unit of it.
inline constexpr double kWanderArriveDistance = 5.0;

/// Walking to a point is a stroll, not a charge.
inline constexpr double kMobWanderSpeedScale = 0.5;

// -- sandstorms --------------------------------------------------------------
//
// A storm walks to a point like a centipede head does, but on its own clock
// and at its full speed: it re-rolls a completely fresh heading three times a
// second, so it doubles back on itself constantly and churns across a small
// patch rather than sweeping the map. Nudging the old heading instead -- which
// is what a drift would be -- reads as a cloud on a course, and moves the storm
// several times further per second than the reference's does.

/// How long one of a storm's headings stands.
inline constexpr double kSandstormHeadingMillis = 300.0;

/// How far ahead the storm sets the point it is blowing toward. Fixed rather
/// than scaled by its body, and re-picked long before it is ever reached, so
/// this is a heading with an arrival test rather than a destination.
inline constexpr double kSandstormWanderRange = kEnemyWanderRange * 2.0;

/// From this tier up a sandstorm drags players toward its centre. Below it a
/// storm is only something to walk around.
inline constexpr Rarity kSandstormSuckRarity = Rarity::Super;

/// How close a player has to be to feel the drag, and how hard it pulls at the
/// storm's own centre -- falling linearly to nothing at the edge of the range.
///
/// The pull is a rate in units per second: the reference states it as 1.5 units
/// per 30Hz tick, and stating it per second instead keeps a player dragged the
/// same distance however long the tick is.
inline constexpr double kSandstormSuckRange = 400.0;
inline constexpr double kSandstormSuckSpeed = 45.0;

/// Unused by the AI: a storm re-rolls its heading outright rather than swinging
/// the old one, because the reference does, and a limit on the swing turns a
/// churning storm into weather crossing the map. Kept because a test names it.
inline constexpr double kSandstormTurnPerDecision = 0.5;

// -- volleys -----------------------------------------------------------------

/// Volley cadence for a mob whose config states no cooldown.
inline constexpr double kDefaultVolleyCooldownMillis = 2000.0;

/// Shot speed for a projectile block that omits one, units per second.
inline constexpr double kDefaultProjectileSpeed = 200.0;

/// The tier a mob's authored projectile `distance` is stated AGAINST.
///
/// Reach is `distance * SIZE_SCALING[tier] / this`, so with common's own scale
/// here the number in mobs.json is literally the reach a COMMON shooter gets,
/// and every higher tier reaches proportionally further up the same body-size
/// ladder its missiles grow on.
///
/// The reference divides by a flat 9 instead, which is 1/6 at common and 4.8x
/// at apex -- a 29x spread that made `distance` mean nothing you could reason
/// about. It also made the shipped hornet unable to shoot: 500 authored came
/// out as 83 units of reach against a 300-unit aggro range, so it fired
/// missiles that expired well before the flower it had locked onto. Only the
/// units changed here; every authored number was rescaled to match, so no
/// mob's actual reach moved.
inline constexpr double kProjectileReachReferenceScale = kMobSizeScale[0];

/// The shooter's rarity also scales the shot's SIZE, on its own divisor --
/// reach and size deliberately grow at different rates.
inline constexpr double kProjectileSizeDivisor = 3.0;

// -- pets --------------------------------------------------------------------

/// A pet sees exactly what its owner's screen shows: its target scan is
/// clipped to this rectangle around the owner rather than to its own aggro
/// range, and a passive or sandstorm pet that leaves it is retired.
inline constexpr double kPetViewHalfWidth = kViewportWidth * 0.5;
inline constexpr double kPetViewHalfHeight = kViewportHeight * 0.5;

// -- chains and nests --------------------------------------------------------

/// Segment spacing used when BodySegment::spacing was never filled in, as a
/// multiple of the segment's own radius. Slightly under a full diameter so the
/// body reads as one animal rather than a string of beads.
inline constexpr double kSegmentSpacingPerRadius = 1.8;

/// Extra room beyond the nest's own body when placing an escort.
inline constexpr double kNestSpawnMargin = 40.0;

/// How far a nest's child may be drawn from its parent before it gives up
/// whatever it was chasing and marches home.
///
/// This is the whole reason a hole cannot be stripped of its defenders: with
/// no leash a player leads the brood away one escort at a time and the nest is
/// left permanently undefended, which is also how a swarm accumulates
/// permanent pursuers.
inline constexpr double kSummonRetreatRadius = 600.0;

/// Inside this the child counts as home and goes back to idling. Well short of
/// the retreat radius so an escort does not oscillate across the boundary.
inline constexpr double kSummonArriveDistance = 100.0;

/// Floor under Spawner::intervalMillis. A nest configured with 0 would ask for
/// a mob every tick and cap out its brood in under a second.
inline constexpr double kMinSpawnIntervalMillis = 100.0;

// ---------------------------------------------------------------------------
// Facing
// ---------------------------------------------------------------------------

/// The angle a mob holds this tick, given how it is trying to travel.
///
/// Free-standing and pure because it carries a per-config exception on each
/// side -- `hideRotation` mobs are drawn upright and `reversed` art points
/// backwards along the travel -- and those are the two things here that fail
/// silently and only ever look like an art bug.
///
/// `maxTurn` is the turn allowed this step, i.e. a rate multiplied by dt. A
/// travel vector of no length holds the current angle rather than snapping to
/// zero, which is what keeps a stopped mob facing where it was going.
double steerFacing(double current, Vec2 travel, bool hideRotation, bool reversed, double maxTurn);

// ---------------------------------------------------------------------------
// Spawning escorts
// ---------------------------------------------------------------------------

/// A nest's request for one escort.
///
/// The AI decides WHEN a nest spawns and WHAT: assembling the entity -- prefab,
/// net id, loot table, faction -- is the spawning system's job. The hook below
/// is where the two meet, and it is why this file needs to know nothing about
/// how a mob is built.
struct MobSpawnRequest {
    Entity parent = NULL_ENTITY;
    std::uint16_t configIndex = 0;
    Rarity rarity = Rarity::Common;
    Vec2 position;
    /// 0 means the escort never expires on its own.
    double lifetimeMillis = 0;
};

// ---------------------------------------------------------------------------
// MobAiSystem
// ---------------------------------------------------------------------------

class MobAiSystem {
public:
    /// Queries are built once against `world` and reused every tick; run() must
    /// be handed that same World. `seed` fixes the wander and spawn-placement
    /// rolls so a test gets the same walk twice.
    explicit MobAiSystem(World& world, std::uint64_t seed = 0xA1B2C3D4E5F60717ull);

    /// Builds one escort at flush time. Returning NULL_ENTITY means "could not
    /// spawn"; the nest simply tries again on its next interval. With no hook
    /// set a nest keeps its cadence and produces nothing.
    using SpawnHook = std::function<Entity(World&, const MobSpawnRequest&)>;
    void setSpawnHook(SpawnHook hook) { spawnHook_ = std::move(hook); }

    /// Mints the wire id a projectile is broadcast under. Unset, a volley still
    /// flies and still hurts, but no client ever sees it -- replication carries
    /// only entities that own a NetId.
    std::function<std::uint32_t()> allocateNetId;

    /// One tick of mob intent.
    ///
    /// `grid` must already hold this tick's players -- it is the only way a mob
    /// finds one. `activePlayers` are the positions LOD is measured against; an
    /// EMPTY list means EVERYTHING is active, exactly as it does in the
    /// reference: with nobody connected there is no one to save the work for,
    /// and a test or a bench that forgets to pass players sees unmodified
    /// behaviour rather than a world running at a fifth speed. Pets are steered
    /// regardless: they belong to a player who is by definition present. `dt`
    /// is in seconds.
    ///
    /// A nest's spawn and a volley both land as deferred commands, so
    /// `commands` must be flushed while this system is still alive.
    void run(World& world, const Terrain& terrain, const SpatialGrid& grid,
             const std::vector<Vec2>& activePlayers,
             double nowMillis, double dt, CommandBuffer& commands);

    /// Per-run counters. Reset at the top of every run(), so they describe the
    /// last tick and nothing else. `targetScans` is the one that matters: it is
    /// the number of broadphase queries the AI spent, and it is what tells a
    /// test that target caching is actually caching.
    struct Stats {
        std::uint64_t considered = 0;    ///< mobs the query produced
        std::uint64_t thought = 0;       ///< mobs that passed the LOD gate
        std::uint64_t skipped = 0;       ///< mobs the far stride held back
        std::uint64_t targetScans = 0;   ///< broadphase acquisitions run
        std::uint64_t attacks = 0;       ///< contact attacks intended
        std::uint64_t volleys = 0;       ///< projectile volleys let go
        std::uint64_t spawnRequests = 0; ///< escorts asked of the spawn hook
        std::uint64_t promotions = 0;    ///< segments promoted to chain heads
    };
    const Stats& stats() const { return stats_; }

private:
    /// The per-tier numbers the AI needs, flattened out of the content tables.
    ///
    /// Memoised by (config, rarity): mobStats() is a dozen multiplies and two
    /// table lookups, and running it for every mob every tick is pure repeat
    /// work over data that cannot change while the server is up.
    struct Drive {
        double speed = 0;
        /// What the mob moves at while PURSUING. Ten types chase at the
        /// flower's own top speed and wander at their authored one, so a single
        /// speed cannot express both.
        double chaseSpeed = 0;
        double attackCooldownMillis = 0;
        /// The behaviour the CONFIG asks for. Read for pets, whose MobAi::kind
        /// is set by whoever summoned them.
        AiKind ai = AiKind::Neutral;
        bool playerSpeedChaser = false;
        bool hideRotation = false;
        bool reversed = false;
        /// Cruises with a sinusoidal sway instead of hopping.
        bool beeFlight = false;
        /// Has a projectile block, so the volley path is worth entering.
        bool shoots = false;
        bool valid = false;
    };

    struct Candidate {
        Entity entity = NULL_ENTITY;
        Vec2 position;
        double score = 0;   ///< distance less the player's aggro bonus
    };

    Drive driveFor(std::uint16_t configIndex, Rarity rarity);

    /// Adds the behaviour components this mob's TYPE calls for and it was not
    /// born with. Structural, so it runs before any component pointer is taken.
    void equipBehaviour(World& world, Entity self, const Drive& drive, double nowMillis);

    /// Whether this mob runs its brain on this tick.
    ///
    /// Always, within the active radius. Beyond it, one tick in kMobFarStride,
    /// offset by the mob's own slot so the far world does not decide in
    /// lockstep -- a shared stride would move the spike rather than remove it.
    bool stepsThisTick(Entity self, Vec2 position, const std::vector<Vec2>& activePlayers) const;

    /// What a far mob does on a tick it did not think.
    ///
    /// The reference has no mob velocity integrator -- its AI writes position
    /// directly -- so a mob whose step is skipped simply does not advance.
    /// Here the movement phase integrates Motion::velocity whatever the AI
    /// did, so the velocity has to be dropped, or a mob that went out of range
    /// mid-chase keeps flying on that heading at five times the reference's
    /// travel until something stops it. The exception is the idle drift: the
    /// reference's passive integrator carries no distance gate at all, so a
    /// mob left idling keeps hopping at full rate however far away it is.
    void driftUnwatched(World& world, Entity self, double nowMillis, double dt);

    void steerMob(World& world, const Terrain& terrain, const SpatialGrid& grid,
                  Entity self, Transform& transform, Motion& motion, const Body& body,
                  const MobType& type, MobAi& ai, const Drive& drive,
                  double nowMillis, double dt, CommandBuffer& commands);

    /// The idle branch. Writes the velocity itself -- the drift machine owns
    /// its own store and its own friction -- and returns the heading facing
    /// should follow.
    Vec2 steerIdle(World& world, Entity self, const Transform& transform, Motion& motion,
                   const Body& body, MobAi& ai, double speed, double nowMillis, double dt);
    void driftPassive(World& world, Entity self, const Body& body, Motion& motion, MobAi& ai,
                      double speed, double nowMillis, double dt);
    Vec2 wanderToPoint(WanderTarget& wander, Vec2 from, const Body& body, double speed,
                       double nowMillis);

    /// Weather. Blows toward a point re-picked on its own fast clock, and from
    /// kSandstormSuckRarity up drags the players around it in as it goes.
    Vec2 steerSandstorm(World& world, const SpatialGrid& grid, Entity self, const MobType& type,
                        const Transform& transform, MobAi& ai, double speed, double nowMillis,
                        double dt);
    /// Drags every player within kSandstormSuckRange toward `from`, which is
    /// where the storm ends this tick rather than where it started it.
    void suckPlayers(World& world, const SpatialGrid& grid, Vec2 from, double dt);
    /// True when the mob spent this tick walking back to whatever spawned it,
    /// with `desired` holding that walk. Crossing the retreat radius is also
    /// what makes it drop the target it was dragged out on.
    bool walkHome(World& world, Entity self, const Transform& transform, MobAi& ai,
                  double chaseSpeed, double nowMillis, Vec2& desired, CommandBuffer& commands);
    /// True when the mob has something to chase, with `desired` holding this
    /// tick's pursuit velocity. Lets the mob's volley go on the way in.
    bool steerAggressive(World& world, const Terrain& terrain, const SpatialGrid& grid,
                         Entity self, const MobType& type, const Transform& transform,
                         const Body& body, MobAi& ai, const Drive& drive, double chaseSpeed,
                         double nowMillis, Vec2& desired, CommandBuffer& commands);

    Entity acquireTarget(World& world, const Terrain& terrain, const SpatialGrid& grid,
                         Entity self, Vec2 from, double range);
    /// The PET a wild mob settles for when no flower is to be had. Distinct
    /// from acquirePetPrey() below, which is the wild mob a pet goes after.
    ///
    /// Walked off this tick's pet snapshot rather than queried out of the
    /// broadphase: a player fields one or two summons, so the list is shorter
    /// than a single grid cell, and a world with no pets in it must not cost a
    /// query per hostile mob per tick.
    Entity acquirePetTarget(const Terrain& terrain, Entity self, Vec2 from, double range);
    Entity nearestAttacker(World& world, Entity self, Vec2 from, double radius) const;
    bool targetHeld(World& world, const Terrain& terrain, Vec2 from, Entity target) const;
    /// Whether a pet target still stands. Held on the mob's own aggro RANGE
    /// rather than the five viewports a flower is chased across: a summon is a
    /// target of opportunity, not a grudge.
    bool petTargetHeld(World& world, const Terrain& terrain, Vec2 from, Entity target,
                       double range) const;

    /// The player a NEUTRAL mob turns on this tick, or NULL_ENTITY.
    ///
    /// Provocation hangs off the damage LEDGER over there, not off the hurt
    /// flash: every source of damage funnels through one credit call, so a
    /// poison tick, a radiation cloud or a pollen puff rounds a mob on its
    /// attacker exactly as a petal swing does, while the flash is lit by
    /// direct hits alone and is otherwise purely cosmetic. What a ledger
    /// cannot say is WHEN, so the growth in it since this mob last thought is
    /// what stands in for the call.
    Entity freshProvoker(World& world, Entity self, Vec2 from);

    /// `self` and, when it heads a chain, every segment behind it: hurting any
    /// part of a centipede provokes the whole animal, so the whole animal's
    /// damage is one ledger.
    void collectChain(World& world, Entity self, std::vector<Entity>& out) const;
    void stampAttack(World& world, Entity self, MobAi& ai, double nowMillis, const Drive& drive);
    /// `shooterVelocity` is the velocity the shooter is about to travel at,
    /// not the one it is still carrying. The wild-mob path computes its
    /// `desired` vector and only writes it to Motion after this returns, so
    /// reading Motion here would inherit the PREVIOUS tick's travel -- and on
    /// the tick a mob acquires a target and fires, that is zero.
    void fireVolley(World& world, Entity shooter, const MobType& type, MobAi& ai,
                    const Drive& drive, Vec2 from, double aimAngle, Vec2 shooterVelocity,
                    double nowMillis, CommandBuffer& commands);

    // -- pets ---------------------------------------------------------------

    void steerPets(World& world, const Terrain& terrain, const SpatialGrid& grid,
                   double nowMillis, double dt, CommandBuffer& commands);
    void steerPet(World& world, const Terrain& terrain, const SpatialGrid& grid, Entity self,
                  Transform& transform, Motion& motion, const Body& body, const MobType& type,
                  MobAi& ai, Entity owner, bool ownerAlive, const Drive& drive,
                  double nowMillis, double dt, CommandBuffer& commands);
    /// The wild mob a pet is fighting: the cached one while it is still on the
    /// OWNER's screen and in sight, else the nearest one that is.
    Entity acquirePetPrey(World& world, const Terrain& terrain, const SpatialGrid& grid,
                          Entity self, Vec2 from, MobAi& ai, bool hasOwner, Vec2 ownerPosition,
                          double range);
    /// Pops a pet onto a clear, visible ring position around its owner. False
    /// when nothing was clear and the pet stayed where it was.
    bool teleportPetToOwner(const Terrain& terrain, Transform& transform, Vec2 ownerPosition);

    void repairChains(World& world);
    void followChains(World& world, const Terrain& terrain, const std::vector<Vec2>& activePlayers);
    void placeFollower(World& world, const Terrain& terrain, Entity self, Entity ahead);
    void driveSpawners(World& world, const Terrain& terrain, double nowMillis, CommandBuffer& commands);

    Query<Transform, Motion, Body, MobType, MobAi> mobs_;
    Query<Pet, Transform, Motion, Body, MobType, MobAi> pets_;
    Query<BodySegment, Transform> segments_;
    Query<Spawner, Transform, MobType> nests_;
    Query<PlayerTag, PlayerModifiers> playerModifiers_;

    SpawnHook spawnHook_;
    Rng rng_;
    Stats stats_;

    /// Ticks run, which is the phase the far stride is measured against.
    std::uint64_t tick_ = 0;

    /// The widest aggro bonus any live player carries, recomputed once per
    /// tick. The bonus is per-player but the broadphase radius is per-query, so
    /// without it a mob would query its bare range and miss exactly the player
    /// whose petals were meant to draw attention.
    double maxAggroBonus_ = 0;

    std::vector<Drive> drives_;
    std::uint32_t drivesHash_ = 0;

    /// Damage recorded against each animal the last time it thought, keyed by
    /// the entity that steers it. Provocation is a CHANGE in this rather than
    /// a value of it, so a mob that dropped a target does not silently pick it
    /// back up off damage it took minutes ago.
    std::unordered_map<Entity, double> ledgerSeen_;

    // Scratch, reused so that a steady-state tick allocates nothing.
    std::vector<Entity> stepList_;
    std::vector<Entity> gridScratch_;
    std::vector<Candidate> candidates_;
    /// This tick's pets, snapshotted before anything moves -- which is where
    /// the reference builds it, so a pet is hunted from where it stood at the
    /// top of the tick rather than from wherever the pet pass has since
    /// carried it.
    std::vector<Candidate> petList_;
    std::vector<Entity> chainScratch_;
    std::vector<Entity> chainHeads_;
    std::unordered_map<Entity, Entity> followerOf_;
    std::unordered_set<Entity> visited_;
};

} // namespace flix
