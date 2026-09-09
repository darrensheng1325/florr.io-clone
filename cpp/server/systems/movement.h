#pragma once
// The movement phase: velocity in, position out, for everything that moves.
//
// Players are the reason this file is careful. The client predicts its own
// flower with desiredVelocity() and integrateVelocity() out of constants.h,
// and the server authorises it with the SAME two functions -- so in open
// movement the two agree bit for bit and nothing visibly corrects. Any
// shortcut taken here that the client does not take is a rubber-band.
//
// Everything else -- mobs, projectiles -- shares the collision half of that
// path and differs only in where its velocity came from.

#include <cstdint>
#include <optional>
#include <vector>

#include "shared/core/types.h"
#include "shared/core/world.h"
#include "shared/game/components.h"
#include "shared/game/spatial.h"
#include "shared/game/terrain.h"

namespace flix {

class MapData;

/// Water costs a mob less than it costs a player.
///
/// A player picks their route and can see the bank; a mob is steered by a
/// heading and has no way to walk around a river, so slowing it as hard as a
/// player turns every stream into a place where mobs go to be shot for free.
/// Water stays a real player advantage at 0.8 without disarming the AI.
inline constexpr double kMobWaterSpeedScale = 0.8;

/// Rate a projectile could correct its heading at, radians per second.
///
/// Nothing steers in flight: a seeking shot picks its bearing once, at launch,
/// and then holds it, because the client dead-reckons a projectile along a
/// fixed heading and any in-flight curve desynchronises what it draws.
inline constexpr double kProjectileTurnRate = 4.0;

// -- mob separation -----------------------------------------------------------
//
// Mobs push each other apart so a spawn wave, an escort group or a chasing
// pack spreads into a ring instead of collapsing onto one point. The push is
// radius-driven and symmetric; mass plays no part in it. The gap, the per-pair
// cap and the Jacobi headroom are shared constants; what is local to this pass
// is the grid it buckets into.

/// Broad-phase cell size for the separation pass. Its own grid rather than the
/// tick's shared one, exactly as in the reference: this pass must see pets,
/// and the shared broadphase holds whatever the systems that build it needed.
inline constexpr double kMobCollisionCellSize = 512.0;

/// Coordinates past this make cell-range loops non-terminating, so a body
/// carrying one sits the pass out entirely.
inline constexpr double kMaxSaneWorldCoord = 1e9;

// -- integration safety rails -------------------------------------------------
//
// The failure this file exists to prevent is a body crossing a wall between
// two samples of the tile grid, and the failure the rails prevent is the fix
// for it becoming an unbounded loop. A corrupt velocity is a bug somewhere
// else; it must cost that entity a slow tick and never cost the server one.

/// Longest a single collision substep may be.
///
/// Half a tile is NOT the right number, which is why this subtracts twice.
/// Detection inflates a tile by the jagged protrusion and the scan buffer, so
/// the midline that matters sits that much inside the geometric one -- and a
/// substep that carries the centre past it flips the resolver's
/// least-penetration ejection to the tile's FAR face, which is a teleport
/// through the wall. TypeScript spells this out as `MAX_STEP_HARD` in
/// stepPlayerMovement (src/constants.ts); this is the same expression, and it
/// only ever binds on bodies wider than half a tile -- a super-tier mob, or a
/// flower stacked with size petals.
inline constexpr double kMaxSubstepLength =
    kTileSize * 0.5 - kJaggedMaxProtrusion - kCollisionScanBuffer;

/// Shortest a substep may be, whatever the body's radius says. A zero-radius
/// projectile -- or a NaN one -- would otherwise ask for infinitely many
/// substeps to cross a single tile.
inline constexpr double kMinSubstepLength = 24.0;

/// Hard cap on substeps per entity per tick. Reached only by a velocity that
/// should not exist; the body is moved as far as this many substeps allow and
/// the rest of the tick's displacement is dropped. Taking LONGER substeps
/// instead is precisely how a body ends up on the far side of a wall.
inline constexpr int kMaxSubstepCount = 16;

/// Speed ceiling applied to every entity before it is integrated, so one
/// arithmetic accident upstream cannot put a body 1e30 units away.
inline constexpr double kMaxMovementSpeed = 20000.0;

/// Radii above this are clamped: collision resolution is a per-tile scan, and
/// nothing in the game is four tiles wide.
inline constexpr double kMaxCollisionRadius = kTileSize * 4.0;

/// Hull given to a body with no radius of its own. A true point has nothing
/// for the tile push-out to act on, so it can come to rest exactly ON a wall
/// tile's edge and read as inside the wall from then on. Half a unit is
/// invisible and makes the push-out well defined for every body.
inline constexpr double kMinCollisionRadius = 0.5;

// ---------------------------------------------------------------------------
// The step
// ---------------------------------------------------------------------------

struct StepOutcome {
    /// What the body actually achieved, which is not `velocity * dt` once a
    /// wall, the map edge or the substep cap has had its say.
    Vec2 displacement;
    /// True when tile collision or the world clamp moved the body off the
    /// path it asked for. Callers use it to rebuild velocity, so that pushing
    /// into a wall bleeds speed instead of storing it up.
    bool blocked = false;
};

/// Advances `position` by `velocity * dt`, substepped so that a fast body
/// samples the tile grid often enough never to cross a wall, and clamped to
/// the world. Free rather than a member so tests -- and one day the client's
/// prediction -- can drive it without a World.
///
/// `refuseWallCrossing` adds the reference's player containment guard: a
/// substep whose wall ejection would carry the CENTRE across solid is thrown
/// away and the body stops where it started. Off by default because the
/// reference only guards flowers -- mobs and projectiles take the resolver's
/// word for it.
StepOutcome stepCollide(const Terrain& terrain, Realm realm, Vec2& position, Vec2 velocity,
                        double radius, double dt, bool collideTerrain = true,
                        bool refuseWallCrossing = false);

/// Total, non-throwing sanitisers. Every one of them maps NaN to a safe value,
/// which is why they are written as failed `>` tests rather than `<=` ones.
double sanitizeCollisionRadius(double radius);
Vec2 sanitizeMovementVelocity(Vec2 velocity);

// ---------------------------------------------------------------------------
// MovementSystem
// ---------------------------------------------------------------------------

class MovementSystem {
public:
    /// The map's annotation layer, or null when the server has none.
    ///
    /// Set once rather than passed per tick: a teleporter pad is a fixture of
    /// the map, not of the frame. Null leaves the pads inert, which is what a
    /// focused test or a bench that never loads a map bundle wants.
    const MapData* mapData = nullptr;

    /// Convenience entry point used by focused tests.
    void run(World& world, const Terrain& terrain, double nowMillis, double dt);

    /// TypeScript advances flowers and their post-movement petal pipeline
    /// before mob AI. The server loop calls these two phases separately to
    /// preserve that ordering while this class still owns all integration.
    void runPlayerPhase(World& world, const Terrain& terrain, double nowMillis, double dt);
    void runWorldPhase(World& world, const Terrain& terrain, double nowMillis, double dt);

private:
    /// Queries are cached because rebuilding one per tick throws away the
    /// matched-archetype list that makes iteration free. They cannot be built
    /// in the constructor: the system is created before any World exists and
    /// is handed one per call. So they are built on first use and REBOUND when
    /// the World changes -- a cached archetype list belongs to the world it
    /// was built against, and reusing it against another reads freed storage.
    struct Queries {
        explicit Queries(World& world);

        Query<PlayerTag, Transform, Motion, Body, PlayerInput> players;
        Query<MobTag, Transform, Motion, Body> mobs;
        Query<ProjectileTag, Transform, Motion, Projectile> projectiles;
        Query<MobTag, Transform, Faction, Health> mobTargets;
        /// The separation pass wants every mob that has a place and a size,
        /// whether or not it is a mover: a nest still occupies its ground.
        Query<MobTag, Transform, Body> mobBodies;
        /// LOD is measured against every flower, dead ones included -- a
        /// player about to respawn is still standing there watching.
        Query<PlayerTag, Transform> playerPositions;
    };

    /// A homing candidate, flattened out of the ECS once per tick. Projectiles
    /// are few and targets are many, so one pass to collect beats one query
    /// walk per projectile.
    struct SeekTarget {
        Entity entity = NULL_ENTITY;
        Vec2 position;
        Team team = Team::Hostiles;
    };

    /// One mob in the separation pass, flattened out of the ECS.
    ///
    /// Every field is read once per neighbour tested, so the pass pays for the
    /// component lookups once rather than once per pair.
    struct SeparationEntry {
        Entity entity = NULL_ENTITY;
        Vec2 position;
        Realm realm = Realm::Overworld;
        double radius = 0;
        /// The centipede this mob belongs to, NULL_ENTITY for anything else.
        Entity chainHead = NULL_ENTITY;
        /// The config's `no_mob_collision`: neither pushes nor is pushed.
        bool noCollision = false;
        /// Accumulated push, applied after every pair has been evaluated.
        Vec2 push;
    };

    /// Not an index into the set. Fills the slot table for every entity that
    /// is not in this pass.
    static constexpr std::uint32_t kNoSeparationEntry = 0xFFFFFFFFu;

    void bind(World& world);
    void movePlayers(World& world, const Terrain& terrain, double nowMillis, double dt);
    /// The teleporter pads: the suction well, the dwell and the jump.
    ///
    /// Part of the player pipeline rather than a system of its own because it
    /// is the last thing the reference does to a flower's position, after
    /// everything else that tick has had its say.
    void stepTeleporters(World& world, double nowMillis, double dt);
    void moveMobs(World& world, const Terrain& terrain, double nowMillis, double dt);
    void moveProjectiles(World& world, const Terrain& terrain, double dt);

    /// Pushes overlapping mobs apart. Runs once everything has moved, so a
    /// shove is never undone by the mover it was computed against.
    void separateMobs(World& world, const Terrain& terrain);
    /// Flattens the eligible mobs into `separationSet_` and files them in
    /// `separationGrid_`.
    void buildSeparationSet(World& world);
    /// The LOD gate: false for a mob too far from every flower to be worth
    /// colliding this tick.
    bool activeForSeparation(Vec2 position, Realm realm) const;

    /// Collected lazily: a tick with no seeking projectile pays nothing.
    void collectSeekTargets();
    Entity findSeekTarget(Entity self, const Projectile& projectile, Team team,
                          Vec2 position, double heading) const;
    /// The one-shot launch correction. Returns the velocity the shot leaves
    /// with, which is `velocity` itself when nothing was in the cone.
    Vec2 aimAtLaunch(World& world, Entity self, Vec2 position,
                     const Projectile& projectile, Vec2 velocity);

    /// Flowers to run the pads against, collected before any of them is
    /// touched: a flower that has never stood on a pad acquires its
    /// TeleporterState here, and adding a component moves the entity to
    /// another archetype -- which is not something a query walk survives.
    std::vector<Entity> teleportPlayers_;

    /// Projectiles whose distance budget or terrain step ended this tick.
    /// Collected during the query walk and marked Dead afterwards: adding a
    /// component relocates an entity between archetypes, so doing it inline
    /// would invalidate the columns moveProjectiles is iterating.
    std::vector<Entity> spentProjectiles_;

    World* boundWorld_ = nullptr;
    std::optional<Queries> queries_;
    std::vector<SeekTarget> seekTargets_;
    bool seekTargetsReady_ = false;

    /// Separation scratch, reused every tick so a steady state allocates
    /// nothing. `separationSlot_` is keyed by entity INDEX, which is how a
    /// grid candidate gets back to its entry in O(1).
    std::vector<RealmPoint> separationPlayers_;
    std::vector<SeparationEntry> separationSet_;
    std::vector<std::uint32_t> separationSlot_;
    std::vector<Entity> separationCandidates_;
    SpatialGrid separationGrid_{kMobCollisionCellSize};
};

} // namespace flix
