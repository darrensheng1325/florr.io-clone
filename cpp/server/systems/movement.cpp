#include "server/systems/movement.h"

#include <algorithm>
#include <cmath>

#include "shared/game/config.h"
#include "shared/game/constants.h"
#include "shared/game/map_elements.h"

namespace flix {
namespace {

/// A correction from resolveCircle smaller than this is floating-point noise
/// from the push-out arithmetic, not contact with a wall.
constexpr double kContactEpsilon = 1e-6;

/// Below this a step is not worth dividing by: a dt of zero (a paused server,
/// a test stepping with 0) must not turn into a division.
constexpr double kMinStepSeconds = 1e-9;

/// Below this an environmental scale is a full stop, and dividing a
/// displacement back out through it would manufacture speed.
constexpr double kMinEnvScale = 1e-4;

/// A projectile below this speed has no meaningful heading, so it neither
/// homes nor updates its facing.
constexpr double kMinProjectileSpeed = 1e-3;

/// Range left on a projectile below this is rounding noise, not reach, and the
/// shot is spent. The last step of a flight is clamped to exactly the budget,
/// but `velocity * fraction * dt` and the displacement it produces round
/// differently, and the difference (~1e-13) is smaller than one ulp of a
/// coordinate near 5000. Asked to fly that residual, the shot moves by exactly
/// zero, spends nothing, and lives forever. A millionth of a unit is far below
/// anything drawable and far above any residual the arithmetic can leave.
constexpr double kSpentRangeEpsilon = 1e-6;

Vec2 sanitizePosition(const Terrain& terrain, Vec2 p, Realm realm) {
    // A body that arrived here non-finite has already lost its place in the
    // world; putting it at the centre of its realm is recoverable, propagating
    // NaN is not.
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
        const Vec2 extent = terrain.realmExtent(realm);
        return {extent.x * 0.5, extent.y * 0.5};
    }
    return p;
}

/// Ceiling on a speed modifier, so one bad petal stat cannot make a player
/// uncatchable -- or outrun the substep budget.
constexpr double kMaxSpeedScale = 8.0;

/// A speed modifier that is missing, zero or corrupt means UNMODIFIED, not
/// rooted. Nothing in the game roots a player, and a PlayerModifiers that no
/// phase has recomputed yet reads as all zeroes -- which must not come out as
/// a flower frozen on its first tick.
double sanitizeSpeedScale(double scale) {
    if (!(scale > 0.0)) return 1.0;
    return scale < kMaxSpeedScale ? scale : kMaxSpeedScale;
}

double substepLength(double sanitizedRadius) {
    // Written against kMinSubstepLength rather than 0 so a NaN radius (which
    // fails every comparison) lands on the floor and not on the cap.
    if (!(sanitizedRadius > kMinSubstepLength)) return kMinSubstepLength;
    return sanitizedRadius < kMaxSubstepLength ? sanitizedRadius : kMaxSubstepLength;
}

/// True when the straight path between two body CENTRES touches solid.
///
/// The push-out picks a face per shape and is free to choose the far one, so a
/// centre pressed into a diagonal seam can be ejected into the open quadrant on
/// the other side of the wall. Accepting that is a teleport through solid, which
/// is why the caller refuses any ejection this reports.
///
/// Terrain's own test, against the AUTHORED SHAPES of `realm`. It used to walk
/// the grid here and test whole 300-unit cell rectangles, which was the same
/// question while a cell was all wall or all air -- and is a different, much
/// stricter one now that a cell blocks only the triangle under its diagonal: a
/// body standing legitimately inside the open half of a wall cell had every
/// push-out refused, so it stopped a body-width short of an authored slope and
/// could not slide along one at all. There is one answer to "did this path cross
/// the wall" and it lives in Terrain.
bool centerPathCrossesWall(const Terrain& terrain, Vec2 a, Vec2 b, Realm realm) {
    // Terrain::kCenterPathInflation is the margin: every blocking shape is
    // grown by half a unit first, so a path that grazes the shared corner of a
    // diagonal seam -- which it can do by a fraction of a pixel -- still counts
    // as crossing it. Its default, rather than a second copy of the number.
    return terrain.segmentTouchesBlockingTile(a, b, Terrain::kCenterPathInflation, realm);
}

/// Drains the pending positional offset written by combat.
///
/// TypeScript's mob knockback is not velocity: it is `x += knockbackX`, then
/// `y += knockbackY` on the next movement step. Clearing it here makes the
/// effect one-shot and lets the normal movement velocity continue unchanged.
Vec2 takeKnockback(World& world, Entity e) {
    Knockback* kb = world.tryGet<Knockback>(e);
    if (!kb) return {0, 0};
    const Vec2 impulse = kb->impulse;
    kb->impulse = {0, 0};
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y)) return {0, 0};
    return impulse;
}

void applyPendingKnockback(World& world, Entity e, Transform& transform) {
    const Vec2 displacement = takeKnockback(world, e);
    if (!std::isfinite(displacement.x) || !std::isfinite(displacement.y)) return;
    transform.position += displacement;
}

/// The velocity to store after a step.
///
/// On contact, velocity is rebuilt from what the body actually achieved. That
/// is sliding, for free: the component into the wall is gone because the body
/// did not move that way, and the component along it survives because it did.
/// The length is capped at what was attempted so that being EJECTED from a
/// wall -- a body spawned inside geometry -- cannot be read as speed.
Vec2 velocityAfterStep(Vec2 attempted, const StepOutcome& out, double dt, double envScale) {
    Vec2 result = attempted;
    if (out.blocked && dt > kMinStepSeconds) {
        result = (out.displacement / dt).clampedLength(attempted.length());
    }
    if (envScale <= kMinEnvScale) return {0, 0};
    return sanitizeMovementVelocity(result / envScale);
}

} // namespace

// ---------------------------------------------------------------------------
// Sanitisers and the step
// ---------------------------------------------------------------------------

double sanitizeCollisionRadius(double radius) {
    if (!(radius > 0.0)) return 0.0;                 // negative, zero, or NaN
    return radius < kMaxCollisionRadius ? radius : kMaxCollisionRadius;
}

Vec2 sanitizeMovementVelocity(Vec2 velocity) {
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y)) return {0, 0};
    return velocity.clampedLength(kMaxMovementSpeed);
}

StepOutcome stepCollide(const Terrain& terrain, Realm realm, Vec2& position, Vec2 velocity,
                        double radius, double dt, bool collideTerrain,
                        bool refuseWallCrossing) {
    StepOutcome out;
    const double r = sanitizeCollisionRadius(radius);
    const double hull = r > kMinCollisionRadius ? r : kMinCollisionRadius;
    const Vec2 start = sanitizePosition(terrain, position, realm);
    position = start;

    Vec2 delta = sanitizeMovementVelocity(velocity) * (dt > 0.0 ? dt : 0.0);
    double distance = delta.length();

    const double stepLength = substepLength(hull);
    const double reach = stepLength * kMaxSubstepCount;
    if (distance > reach) {
        // Truncate the tick's travel instead of lengthening the substeps. A
        // body that only crawls this tick is a visible glitch; a body that
        // teleported through a wall is a lost server.
        delta *= reach / distance;
        distance = reach;
    }

    int steps = 1;
    if (distance > stepLength) {
        steps = static_cast<int>(std::ceil(distance / stepLength));
        if (steps > kMaxSubstepCount) steps = kMaxSubstepCount;   // ceil() rounding
    }

    const Vec2 stepDelta = delta / static_cast<double>(steps);
    for (int i = 0; i < steps; ++i) {
        const Vec2 from = position;
        const Vec2 want = from + stepDelta;
        Vec2 got = collideTerrain ? terrain.resolveCircle(want, hull, realm) : want;

        // The containment guard is a TILE question, so it is asked of every
        // authored map -- each world realm has a grid of its own -- and of
        // nothing else: the maze's resolver slides along its own geometry and
        // the arena has nothing to cross. Same geometry, same physics,
        // whichever realm id the map was loaded into.
        if (refuseWallCrossing && collideTerrain && isWorldRealm(realm)
            && distanceSq(got, want) > kContactEpsilon * kContactEpsilon
            // A centre already inside a blocking tile is exempt: the
            // resolver's output is its only way out, arbitrary as the
            // direction may be.
            && !terrain.blocked(from, realm)
            && centerPathCrossesWall(terrain, from, got, realm)) {
            // The ejection would carry the centre across solid. Refuse it and
            // end the tick's movement where this substep started.
            out.blocked = true;
            break;
        }

        got = terrain.clampInside(got, hull, realm);
        if (distanceSq(got, want) > kContactEpsilon * kContactEpsilon) out.blocked = true;
        position = got;
    }

    out.displacement = position - start;
    return out;
}

// ---------------------------------------------------------------------------
// MovementSystem
// ---------------------------------------------------------------------------

MovementSystem::Queries::Queries(World& world)
    : players(world), mobs(world), projectiles(world),
      mobTargets(world), mobBodies(world), playerPositions(world) {
    // A body marked Dead is still in the world so later systems can see it die,
    // but a corpse must not keep walking.
    players.without<Dead>();
    mobs.without<Dead>();
    projectiles.without<Dead>();
    mobTargets.without<Dead>();
    mobBodies.without<Dead>();
    // playerPositions deliberately keeps corpses: a dead flower is about to
    // respawn where it stands, and letting the mobs around it coast for those
    // few ticks is the artefact the LOD gate exists to avoid.
}

void MovementSystem::bind(World& world) {
    if (boundWorld_ == &world && queries_) return;
    boundWorld_ = &world;
    queries_.emplace(world);
}

void MovementSystem::run(World& world, const Terrain& terrain, double nowMillis, double dt) {
    runPlayerPhase(world, terrain, nowMillis, dt);
    runWorldPhase(world, terrain, nowMillis, dt);
}

void MovementSystem::runPlayerPhase(World& world, const Terrain& terrain,
                                    double nowMillis, double dt) {
    bind(world);
    movePlayers(world, terrain, nowMillis, dt);
    // The pads act on the position the tick has already settled on, which is
    // what makes the suction able to beat a shove: the reference runs them at
    // the very end of its per-player pipeline, on the coordinates it is about
    // to commit.
    if (worldMaps != nullptr) {
        teleportTerrain_ = &terrain;
        stepTeleporters(world, nowMillis, dt);
    }
}

void MovementSystem::runWorldPhase(World& world, const Terrain& terrain,
                                   double nowMillis, double dt) {
    bind(world);
    seekTargetsReady_ = false;
    moveMobs(world, terrain, nowMillis, dt);
    // Projectiles after mobs: a shot fired this tick takes its launch bearing
    // from where the mobs ended up, not from where they were.
    moveProjectiles(world, terrain, dt);
    // Separation last, once everything has moved -- the reference resolves
    // mob-vs-mob overlap in its combat phase for the same reason. A pass run
    // before the movers would have its work undone the same tick.
    separateMobs(world, terrain);
}

void MovementSystem::movePlayers(World& world, const Terrain& terrain,
                                 double nowMillis, double dt) {
    queries_->players.each([&](Entity e, PlayerTag&, Transform& transform, Motion& motion,
                               Body& body, PlayerInput& input) {
        // The cursor is one value with two readers -- movement and petal aim --
        // so it is derived once, here, before anything downstream looks at it.
        input.aimDirection = Vec2::fromAngle(input.current.aimAngle);

        // Facing is the WALK heading, and it is frozen while the flower is
        // standing still: it is where the flower went, not where its owner is
        // pointing. Under cursor control the two are the same value, so this
        // only reads differently on WASD -- where the client keeps sending the
        // cursor angle as the aim -- and at rest, where the reference holds
        // the heading it stopped on. The angle is what both clients ease the
        // pupils toward, so writing the cursor here makes every flower's eyes
        // track a mouse the reference never showed them.
        if (input.current.moveStrength > 0.0) {
            transform.angle = input.current.moveAngle;
        }

        // Every scale folds into the TARGET speed, never into the velocity
        // that came out of the last tick. That is what the client does, and a
        // player who walks into water must ease down to the slower speed
        // rather than have it applied retroactively to momentum they had.
        double maxSpeed = kPlayerMaxSpeed;
        if (const PlayerModifiers* mods = world.tryGet<PlayerModifiers>(e)) {
            maxSpeed *= sanitizeSpeedScale(mods->speedScale);
        }
        // No slow term. Slows are a mob affliction in the reference -- its one
        // writer refuses any victim without a mob kind -- so a flower standing
        // in a web keeps full speed even where an orphaned field can still
        // stamp an Afflictions onto it.
        if (terrain.inWater(transform.position, transform.realm)) maxSpeed *= kWaterSpeedScale;

        MoveState state{transform.position, motion.velocity};
        integrateVelocity(state, desiredVelocity(input.current.moveAngle,
                                                 input.current.moveStrength, maxSpeed), dt);

        applyPendingKnockback(world, e, transform);
        const Vec2 velocity = sanitizeMovementVelocity(state.velocity);
        // The containment guard is the flower's alone, as it is in the
        // reference: it lives in stepPlayerMovement, and mobs and projectiles
        // take the resolver's word for it. A player arrives here from a
        // contact knockback that already overlapped wall geometry often
        // enough that without it, diagonal seams are passable.
        stepCollide(terrain, transform.realm, transform.position, velocity, body.radius, dt, true,
                    true);
        // TypeScript's stepPlayerMovement returns the friction-integrated
        // velocity unchanged when wall resolution alters the position. Keeping
        // the attempted velocity is observable on the following tick (the
        // flower continues pressing/sliding); rebuilding it from achieved
        // displacement changes both acceleration and the wall trajectory.
        motion.velocity = velocity;
    });
}

void MovementSystem::stepTeleporters(World& world, double nowMillis, double dt) {
    teleportPlayers_.clear();
    queries_->players.each([&](Entity e, PlayerTag&, Transform&, Motion&, Body&, PlayerInput&) {
        teleportPlayers_.push_back(e);
    });

    for (const Entity e : teleportPlayers_) {
        // A flower the server keeps off the pads -- its bots -- is not even
        // pulled by one: the suction is the pad's first act, and a bot half
        // dragged onto a pad it can never take would stand there forever.
        if (takesTeleporters && !takesTeleporters(e)) continue;
        Transform* transform = world.tryGet<Transform>(e);
        if (!transform) continue;
        // Every world map has its own pads. The arena and the maze have none:
        // they are generated rather than authored, so there is nothing there
        // to stand on.
        const MapData* map = worldMaps->forRealm(transform->realm);
        if (map == nullptr) continue;
        // The state is per flower and starts empty, so it is created on the
        // first tick this runs for a player rather than by the prefab -- one
        // more component on every flower for a feature a handful of pads use.
        TeleporterState& state = world.ensure<TeleporterState>(e);
        const MapData::TeleportStep step =
            map->stepTeleporters(transform->position, dt, nowMillis, state);
        // The suction is committed raw, with no wall resolution and no world
        // clamp, exactly as the reference commits it: the pull is small and
        // every pad is authored on open ground.
        transform->position = step.position;

        if (step.fired < 0) continue;
        // A pad that fired leads to ANOTHER map, which means a realm change --
        // a new tile grid on the wire and a cleared view on the client. Only
        // the connection layer can do that, so it is handed up rather than
        // done here. Without a handler the pad is inert, which is what a
        // focused movement test gets and what it should get: a test with no
        // server has nowhere to send anyone.
        if (!onTeleport) continue;
        const MapElement& pad = map->elements()[static_cast<std::size_t>(step.fired)];
        WorldMaps::Destination destination;
        // No mob list: the arrival is a player being put down on authored
        // ground, and refusing to send them because a mob is standing on the
        // pad would leave them charging it forever.
        if (!worldMaps->resolveTeleporter(pad, teleportRng_, *teleportTerrain_, destination)) {
            continue;   // reported at load; nothing to do per tick
        }
        onTeleport(e, destination.realm, destination.position);
    }
}

void MovementSystem::moveMobs(World& world, const Terrain& terrain,
                              double nowMillis, double dt) {
    queries_->mobs.each([&](Entity, MobTag&, Transform& transform, Motion& motion, Body& body) {
        // Knockback is deliberately NOT drained here. The reference writes a
        // mob's knockback vector on every petal and projectile hit and then no
        // system ever reads it back into a position -- a mob walks straight
        // through a petal ring rather than being shoved out of it, and that
        // sets both the DPS-in-contact and the feel of melee. The component
        // stays a pure record, which is exactly what it is over there.
        const Vec2 velocity = sanitizeMovementVelocity(motion.velocity);

        // Unlike a player's, a mob's slow and water penalty scale the DISPLACEMENT
        // and are not written back into Motion. The AI publishes the velocity it
        // wants in world terms; if the penalty were folded into the stored value
        // it would compound every tick the AI left the velocity alone, and a mob
        // that paused in a river would never get out of it.
        double envScale = 1.0;
        if (terrain.inWater(transform.position, transform.realm)) envScale *= kMobWaterSpeedScale;
        // Slow already scales the desired speed in MobAiSystem. Applying it
        // again here squares the factor (a 0.5 web becomes 0.25 speed).

        const Vec2 attempted = velocity * envScale;
        const StepOutcome out = stepCollide(terrain, transform.realm, transform.position, attempted,
                                            body.radius, dt);

        // No friction is applied here. The AI phase runs the shared
        // integrateVelocity() against its desired heading and so owns a mob's
        // acceleration and coast-down. A TypeScript knockback is positional,
        // so it does not alter this stored velocity at all.
        motion.velocity = velocityAfterStep(attempted, out, dt, envScale);
    });
}

void MovementSystem::moveProjectiles(World& world, const Terrain& terrain, double dt) {
    spentProjectiles_.clear();
    queries_->projectiles.each([&](Entity e, ProjectileTag&, Transform& transform,
                                   Motion& motion, Projectile& projectile) {
        if (!(projectile.remainingDistance > 0.0)) {
            // Park immediately so a spent shot cannot move or hit anything
            // else before the lifecycle phase reaps it below.
            motion.velocity = {0, 0};
            spentProjectiles_.push_back(e);
            return;
        }

        Vec2 velocity = sanitizeMovementVelocity(motion.velocity);

        // Seeking is a LAUNCH correction, not a guidance system: the shot
        // snaps onto the nearest mob inside a cone around the bearing it was
        // fired on, once, and then flies straight. That is what the reference
        // does, and it is load-bearing -- the client dead-reckons a projectile
        // along a fixed heading, so a shot that curves in flight is drawn
        // somewhere the server does not have it.
        //
        // The correction belongs to the volley, and the petal system spawns
        // the shot on the petal's orbit bearing; this runs on the shot's first
        // step, before it has moved, so it still measures from the petal's
        // own position. Consuming seekRange is what makes it one-shot.
        if (projectile.seekRange > 0.0 && velocity.lengthSq() > kMinProjectileSpeed * kMinProjectileSpeed) {
            velocity = aimAtLaunch(world, e, transform.position, projectile, velocity);
            projectile.seekRange = 0.0;
        }
        const double speed = velocity.length();

        // Range is a distance budget, not a timer: the last tick is shortened
        // so a fast shot dies exactly at its stated reach rather than one whole
        // tick past it.
        Vec2 attempted = velocity;
        const double travel = speed * dt;
        if (travel > projectile.remainingDistance && travel > kMinStepSeconds) {
            attempted = velocity * (projectile.remainingDistance / travel);
        }

        double radius = 0.0;
        if (const Body* body = world.tryGet<Body>(e)) radius = body->radius;

        const StepOutcome out =
            stepCollide(terrain, transform.realm, transform.position, attempted, radius, dt);
        projectile.remainingDistance -= out.displacement.length();
        if (!(projectile.remainingDistance > kSpentRangeEpsilon)) projectile.remainingDistance = 0.0;

        if (out.blocked) {
            // Terrain and the map edge eat shots.
            projectile.remainingDistance = 0.0;
            motion.velocity = {0, 0};
            spentProjectiles_.push_back(e);
            return;
        }

        motion.velocity = velocity;
        if (speed > kMinProjectileSpeed) transform.angle = velocity.angle();
        if (!(projectile.remainingDistance > 0.0)) {
            motion.velocity = {0, 0};
            spentProjectiles_.push_back(e);
            return;
        }

        // The net under the distance rule. Every shot is spawned with its
        // range restated as a flight time, and a shot that cannot spend its
        // range -- a NaN velocity sanitised to zero, a step that rounds to no
        // movement -- must still die on that schedule instead of lying on the
        // ground until something walks into it. A whole tick of slack keeps
        // this strictly the backstop: the two clocks tie to within rounding,
        // and firing on the tie would cut a tick off ordinary flights.
        if (Lifetime* lifetime = world.tryGet<Lifetime>(e)) {
            lifetime->remainingSeconds -= dt;
            if (lifetime->remainingSeconds <= -net::kTickSeconds) {
                projectile.remainingDistance = 0.0;
                motion.velocity = {0, 0};
                spentProjectiles_.push_back(e);
            }
        }
    });

    // Expiry is a movement result, so movement owns the transition to Dead.
    // Combat retains its zero-range check as a backstop for callers that edit
    // Projectile state between phases, but ordinary flight no longer depends
    // on combat happening to clean up a projectile parked on the ground.
    for (const Entity e : spentProjectiles_) {
        if (world.isAlive(e) && !world.has<Dead>(e)) world.add<Dead>(e);
    }
}

void MovementSystem::collectSeekTargets() {
    seekTargets_.clear();
    seekTargetsReady_ = true;

    // Mobs only. The reference's seek walks the shared enemy broadphase, which
    // deliberately holds no players and no pets, so a guided shot locks onto
    // wild mobs and nothing else; the team test below is what keeps a pet off
    // the list here, since a pet is a mob on the players' team.
    queries_->mobTargets.each([&](Entity e, MobTag&, Transform& t, Faction& f, Health& h) {
        if (!h.alive()) return;
        seekTargets_.push_back({e, t.position, f.team});
    });
}

Entity MovementSystem::findSeekTarget(Entity self, const Projectile& projectile, Team team,
                                      Vec2 position, double heading) const {
    // A config with a range but no cone re-aims within 45 degrees of the
    // firing bearing -- the reference's default. A full circle would let a
    // shot leave along a bearing nothing was ever fired on.
    const double cone = projectile.seekCone > 0.0 ? projectile.seekCone : kPi * 0.25;
    const double rangeSq = projectile.seekRange * projectile.seekRange;
    double bestDistanceSq = 0.0;
    Entity best = NULL_ENTITY;

    for (const SeekTarget& candidate : seekTargets_) {
        if (candidate.entity == self || candidate.entity == projectile.owner) continue;
        if (candidate.entity == projectile.creditTo) continue;
        if (candidate.team == team || candidate.team == Team::Neutral) continue;

        const Vec2 toTarget = candidate.position - position;
        const double distanceSquared = toTarget.lengthSq();
        // A target sitting exactly on the shot has no bearing to aim at.
        if (distanceSquared > rangeSq || distanceSquared < 1e-12) continue;
        // Strictly nearer, so an exact tie keeps the one found first.
        if (best != NULL_ENTITY && distanceSquared >= bestDistanceSq) continue;
        if (std::fabs(angleDelta(heading, toTarget.angle())) > cone) continue;

        bestDistanceSq = distanceSquared;
        best = candidate.entity;
    }
    return best;
}

Vec2 MovementSystem::aimAtLaunch(World& world, Entity self, Vec2 position,
                                 const Projectile& projectile, Vec2 velocity) {
    if (!seekTargetsReady_) collectSeekTargets();

    // A shot inherits its owner's team when it has none of its own, so a mob's
    // volley does not lock onto the mobs beside it.
    Team team = Team::Players;
    if (const Faction* faction = world.tryGet<Faction>(self)) team = faction->team;
    else if (const Faction* owner = world.tryGet<Faction>(projectile.owner)) team = owner->team;

    const double speed = velocity.length();
    const double heading = velocity.angle();
    const Entity target = findSeekTarget(self, projectile, team, position, heading);
    if (target == NULL_ENTITY) return velocity;

    const Transform* targetTransform = world.tryGet<Transform>(target);
    if (!targetTransform) return velocity;
    return Vec2::fromAngle((targetTransform->position - position).angle(), speed);
}

// ---------------------------------------------------------------------------
// Mob separation
// ---------------------------------------------------------------------------

bool MovementSystem::activeForSeparation(Vec2 position, Realm realm) const {
    // Nobody connected is the PERMISSIVE case, as it is in the reference's
    // activity field: with no observer there is nothing to save the work for,
    // and a bench or a test that never adds a player sees the unmodified rule.
    if (separationPlayers_.empty()) return true;
    const double reachSq = kMobActiveRadius * kMobActiveRadius;
    for (const RealmPoint& player : separationPlayers_) {
        if (player.realm != realm) continue;
        if (distanceSq(position, player.position) <= reachSq) return true;
    }
    return false;
}

void MovementSystem::buildSeparationSet(World& world) {
    separationPlayers_.clear();
    queries_->playerPositions.each([&](Entity, PlayerTag&, Transform& transform) {
        if (!std::isfinite(transform.position.x) || !std::isfinite(transform.position.y)) return;
        separationPlayers_.push_back({transform.position, transform.realm});
    });

    // Retire last pass's slots before the set they index is dropped. The table
    // is keyed by entity INDEX, which the world recycles.
    for (const SeparationEntry& entry : separationSet_) {
        const std::uint32_t index = entityIndex(entry.entity);
        if (index < separationSlot_.size()) separationSlot_[index] = kNoSeparationEntry;
    }
    separationSet_.clear();
    separationGrid_.clear();

    const ContentRegistry& registry = content();
    queries_->mobBodies.each([&](Entity e, MobTag&, Transform& transform, Body& body) {
        const Vec2 position = transform.position;
        // A degenerate coordinate makes the cell walks non-terminating and
        // would put a NaN into every push the mob takes part in. Such a mob
        // sits the pass out -- and, never entering the set, is excluded as a
        // push TARGET as well as a pusher.
        if (!std::isfinite(position.x) || !std::isfinite(position.y)) return;
        if (std::fabs(position.x) > kMaxSaneWorldCoord) return;
        if (std::fabs(position.y) > kMaxSaneWorldCoord) return;
        // Far from every flower: sit this tick out, the same LOD rule the AI
        // phase applies. A shove nobody is near enough to see is missed
        // outright rather than applied one-sided.
        if (!activeForSeparation(position, transform.realm)) return;

        SeparationEntry entry;
        entry.entity = e;
        entry.position = position;
        entry.realm = transform.realm;
        entry.radius = sanitizeCollisionRadius(body.radius);
        if (const BodySegment* segment = world.tryGet<BodySegment>(e)) {
            entry.chainHead = segment->chainHead;
        }
        if (const MobType* type = world.tryGet<MobType>(e)) {
            entry.noCollision = registry.mob(type->configIndex).noMobCollision;
        }

        const std::uint32_t index = entityIndex(e);
        if (index >= separationSlot_.size()) {
            separationSlot_.resize(static_cast<std::size_t>(index) + 1, kNoSeparationEntry);
        }
        separationSlot_[index] = static_cast<std::uint32_t>(separationSet_.size());
        separationSet_.push_back(entry);
        separationGrid_.insert(e, transform.realm, position, entry.radius);
    });
}

void MovementSystem::separateMobs(World& world, const Terrain& terrain) {
    buildSeparationSet(world);
    if (separationSet_.empty()) return;

    // Jacobi, not in-place: every push is computed from the positions the pass
    // started with and applied afterwards, so the outcome does not depend on
    // the order the set happens to be walked in. Mass plays no part -- the
    // separation is symmetric and driven purely by the two radii.
    for (SeparationEntry& self : separationSet_) {
        if (self.noCollision) continue;
        // The grid files a mob under every cell its own circle touches, so a
        // query of this mob's radius plus the buffer already returns every
        // neighbour that can be inside the sum of the two radii.
        separationGrid_.query(self.realm, self.position, self.radius + kMobCollisionBuffer,
                              separationCandidates_);

        for (const Entity candidate : separationCandidates_) {
            const std::uint32_t index = entityIndex(candidate);
            const std::uint32_t slot =
                index < separationSlot_.size() ? separationSlot_[index] : kNoSeparationEntry;
            if (slot == kNoSeparationEntry) continue;
            const SeparationEntry& other = separationSet_[slot];
            if (other.entity != candidate || other.entity == self.entity) continue;
            // Segments of one centipede never push each other: the chain pass
            // holds them in formation, and a physical shove makes them tangle.
            if (self.chainHead != NULL_ENTITY && self.chainHead == other.chainHead) continue;
            // The exemption belongs to the PAIR: a mob flagged
            // no_mob_collision neither pushes nor is pushed.
            if (other.noCollision) continue;

            const Vec2 toOther = other.position - self.position;
            const double distance = toOther.length();
            const double minDistance = self.radius + other.radius + kMobCollisionBuffer;
            if (!(distance < minDistance && distance > 0.0)) continue;

            const double push = std::min((minDistance - distance) * 0.5, kMobSeparationMaxPushPerPair);
            self.push -= toOther * (push / distance);
        }
    }

    // The cap is on the SUM, not on each pair: a mob wedged in a crowd would
    // otherwise be moved by every neighbour at once.
    const double cap = kMobSeparationMaxPushPerPair * kMobSeparationPushHeadroom;
    for (const SeparationEntry& entry : separationSet_) {
        if (entry.push.x == 0.0 && entry.push.y == 0.0) continue;
        Transform* transform = world.tryGet<Transform>(entry.entity);
        if (!transform) continue;
        // Separation must not shove a mob into a wall. This runs after the
        // wall pass, so a violation would be on screen for a whole tick.
        transform->position = terrain.resolveCircle(
            entry.position + entry.push.clampedLength(cap), entry.radius, entry.realm);
    }
}

} // namespace flix
