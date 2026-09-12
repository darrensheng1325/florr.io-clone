#include "server/systems/mob_ai.h"

#include "shared/game/config.h"
#include "shared/game/constants.h"
#include "shared/game/rarity.h"

#include <algorithm>
#include <cmath>

namespace flix {
namespace {

/// Below this a vector carries no usable direction and normalising it would
/// produce a heading out of pure rounding noise.
constexpr double kDirectionEpsilonSq = 1e-9;

/// A pet's owner counts as standing still below this, in units per second.
/// Friction leaves a residual after the keys are released, and a sandstorm pet
/// shadowing that residual would creep away from a motionless flower.
constexpr double kOwnerMovingSpeed = 1.0;

/// How often the provocation watermarks are swept for mobs that have since
/// died. Nothing tells this system a mob was reaped, and a per-tick scan of
/// the whole table would cost far more than the handful of stale doubles it
/// reclaims -- ten seconds of them is a few dozen entries.
constexpr std::uint64_t kLedgerSweepTicks = 300;

bool entityUsable(World& world, Entity e) {
    if (!world.isAlive(e)) return false;
    if (world.tryGet<Dead>(e) != nullptr) return false;
    const Health* health = world.tryGet<Health>(e);
    return health == nullptr || health->alive();
}

/// Whether a mob is close enough to a player to be worth simulating fully.
///
/// An EMPTY list is the permissive case, as it is in the reference: with
/// nobody connected there is nothing to save the work for, and a caller that
/// forgot to pass its players gets unmodified behaviour rather than a world
/// silently running at a fifth speed.
bool mobActive(Vec2 position, Realm realm, const std::vector<RealmPoint>& activePlayers) {
    if (activePlayers.empty()) return true;
    const double reachSq = kMobActiveRadius * kMobActiveRadius;
    for (const RealmPoint& player : activePlayers) {
        if (player.realm != realm) continue;
        if (distanceSq(position, player.position) <= reachSq) return true;
    }
    return false;
}

/// What a web, a honey pool or a pincer leaves of a mob's speed this tick.
///
/// A slow changes what the mob ASKS for, not how physics answers it, so it is
/// folded into the desired speed here rather than into the movement
/// integrator -- applied in both places it would land twice on one tick.
double slowFactorOf(World& world, Entity self, double nowMillis) {
    const Afflictions* afflictions = world.tryGet<Afflictions>(self);
    if (afflictions == nullptr || !afflictions->slowed(nowMillis)) return 1.0;
    return clamp(afflictions->slowFactor, 0.0, 1.0);
}

/// Whether a mob's last decision left it drifting rather than doing something.
///
/// The reference keeps this as a tag its AI maintains and its passive
/// integrator reads, which is exactly what lets that integrator run with no
/// distance gate. Here it is derived from the same facts the tag is written
/// from: what the mob is, whether it is holding a target, and whether it walks
/// to a point instead of hopping.
bool idleDrifting(World& world, Entity self, const MobAi& ai) {
    if (ai.kind == AiKind::Stationary || ai.kind == AiKind::Sandstorm) return false;
    // Chasing something.
    if (ai.target != NULL_ENTITY) return false;
    // Marching home to its nest: it has somewhere to be, and the reference
    // clears the idle tag for exactly this case, so a tethered mob dragged out
    // of range walks back a stride at a time rather than hopping at full rate.
    const HoleTether* tether = world.tryGet<HoleTether>(self);
    if (tether != nullptr && tether->returning) return false;
    // A chain head strolls to a destination point; the reference does not tag
    // it idle either, so it stops when its stride does.
    return !world.has<BodySegment>(self);
}

/// Both the drift acceleration and the wander range are stated per body.
///
/// Mob speed is constant across rarities -- only size grows -- so an unscaled
/// step that reads as a few body-lengths for a common is a tenth of one for an
/// apex, which is what makes big mobs look frozen.
double sizeFactor(double radius) { return radius / kWanderRefRadius; }

/// One projectile, resolved where the mob decided to shoot and assembled at
/// flush time. Creating the entity from inside the mob walk would relocate the
/// very columns that walk is holding.
struct VolleyShot {
    Vec2 from;
    double angle = 0;
    double speed = 0;
    double radius = 0;
    double distance = 0;
    double damage = 0;
    /// The pool that makes the shot penetrate rather than being eaten by the
    /// first thing it touches.
    double health = kProjectileDefaultHealth;
    /// The shooter's own velocity at the moment of firing, added to the launch
    /// vector. A mob strafing across a flower leads its own volley.
    Vec2 inherited;
    /// The shooter's realm; the shot is born into the same space.
    Realm realm = Realm::Overworld;
    Entity owner = NULL_ENTITY;
    Entity creditTo = NULL_ENTITY;
    Faction faction;
    double seekRange = 0;
    double seekCone = 0;
    std::uint16_t petalIndex = kNoPetal;
    Rarity rarity = Rarity::Common;
    std::uint32_t netId = 0;
    bool identified = false;
    /// Whether touching the shot glitches a flower; see Projectile.
    bool glitchInfecting = false;
};

/// The same archetype a petal's shot is built with -- a mob's projectile is
/// the same object fired by the other side, and combat resolves both through
/// one path.
void spawnShot(World& world, const VolleyShot& shot) {
    const Entity e = world.create();
    world.add<ProjectileTag>(e);
    world.add<Transform>(e, Transform{shot.from, shot.angle, shot.realm});
    world.add<Motion>(e, Motion{Vec2::fromAngle(shot.angle, shot.speed) + shot.inherited});
    // Mass is area on the same scale a mob's body uses, so a bigger shooter's
    // bigger shot shoves proportionally harder instead of only looking heavier.
    world.add<Body>(e, Body{shot.radius, projectileMass(shot.radius)});
    world.add<Faction>(e, shot.faction);
    world.add<Health>(e, Health{shot.health, shot.health});
    // Added at spawn, never mid-flight: arming this during the hit loop would
    // relocate the shot out from under it.
    world.add<HitCooldowns>(e, HitCooldowns{});

    Projectile projectile;
    projectile.owner = shot.owner;
    projectile.creditTo = shot.creditTo;
    projectile.damage = shot.damage;
    projectile.remainingDistance = shot.distance;
    projectile.petalConfigIndex = shot.petalIndex;
    projectile.rarity = shot.rarity;
    projectile.seekRange = shot.seekRange;
    projectile.seekCone = shot.seekCone;
    projectile.glitchInfecting = shot.glitchInfecting;
    world.add<Projectile>(e, projectile);

    // Distance is the authority on range; the lifetime is the same limit
    // expressed in time, so a shot that never hits anything still dies on
    // schedule even if nothing decrements the distance.
    world.add<Lifetime>(e, Lifetime{shot.distance / shot.speed});

    Replicated replicated;
    replicated.kind = net::EntityKind::Projectile;
    replicated.typeIndex = shot.petalIndex;
    replicated.rarity = shot.rarity;
    world.add<Replicated>(e, replicated);
    if (shot.identified) world.add<NetId>(e, NetId{shot.netId});
}

} // namespace

// ---------------------------------------------------------------------------
// Facing
// ---------------------------------------------------------------------------

double steerFacing(double current, Vec2 travel, bool hideRotation, bool reversed, double maxTurn) {
    // Drawn upright whatever it is doing: a hole and a sandstorm have no front.
    if (hideRotation) return 0.0;
    // No heading to adopt -- hold the last one. Falling back to zero here is
    // what would make a mob snap east every time it stopped.
    if (!(travel.lengthSq() > kDirectionEpsilonSq)) return wrapAngle(current);

    double want = travel.angle();
    if (reversed) want = wrapAngle(want + kPi);
    // Written as a failed > so a NaN step turns nothing rather than poisoning
    // the angle for the rest of the entity's life.
    if (!(maxTurn > 0.0)) return wrapAngle(current);

    const double delta = angleDelta(current, want);
    return wrapAngle(current + clamp(delta, -maxTurn, maxTurn));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MobAiSystem::MobAiSystem(World& world, std::uint64_t seed)
    : mobs_(world), pets_(world), segments_(world), nests_(world), playerModifiers_(world),
      rng_(seed) {
    // A pet is not a wild mob with a different target list: it follows an
    // owner, pops back to them and is retired off their screen, so it runs its
    // own pass below. Dead mobs still exist until the reaper runs, and a corpse
    // must not keep steering.
    mobs_.without<Dead, Pet>();
    pets_.without<Dead>();
    segments_.without<Dead>();
    playerModifiers_.without<Dead>();
    // nests_ deliberately keeps Dead spawners: a dying nest is exactly when its
    // brood has to be released.
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

MobAiSystem::Drive MobAiSystem::driveFor(std::uint16_t configIndex, Rarity rarity) {
    const ContentRegistry& registry = content();

    // Content is immutable once loaded, so the per-tier numbers can be cached
    // for the life of the process; a hot reload changes the hash and throws the
    // whole table away rather than leaving one stale row behind.
    if (registry.contentHash() != drivesHash_) {
        drives_.clear();
        drivesHash_ = registry.contentHash();
    }

    // An index from outside the tables gets an inert Drive and is never given a
    // row: sizing the cache from an arbitrary u16 would reserve half a million
    // entries for one corrupt mob.
    if (configIndex >= registry.mobCount()) return Drive{};

    const std::size_t tier = static_cast<std::size_t>(clamp(rarityIndex(rarity), 0, kRarityCount - 1));
    const std::size_t key = static_cast<std::size_t>(configIndex) * kRarityCount + tier;
    if (key >= drives_.size()) drives_.resize(registry.mobCount() * kRarityCount);

    Drive& drive = drives_[key];
    if (!drive.valid) {
        const MobConfig& config = registry.mob(configIndex);
        const MobStats stats = registry.mobStats(configIndex, rarity);
        drive.speed = stats.speed;
        drive.chaseSpeed = stats.chaseSpeed > 0.0 ? stats.chaseSpeed : stats.speed;
        drive.attackCooldownMillis = stats.attackCooldownMillis;
        drive.ai = stats.ai;
        drive.playerSpeedChaser = stats.playerSpeedChaser;
        drive.hideRotation = config.hideRotation;
        drive.reversed = config.reversed;
        // The one type that cruises instead of hopping. A name test, because
        // the reference selects the machine by mob type and nothing in the
        // numbers distinguishes a bee from anything else that flies.
        drive.beeFlight = config.id == "bee";
        drive.shoots = config.projectile.present &&
                       config.projectile.ammoPetalIndex != kInvalidIndex;
        drive.valid = true;
    }
    return drive;
}

void MobAiSystem::equipBehaviour(World& world, Entity self, const Drive& drive, double nowMillis) {
    // What the mob is ACTUALLY running, which is what decides the components it
    // needs: a summon or a test may hand a mob a behaviour its config never
    // asked for. Read out by value up front because the adds below relocate the
    // row it comes from.
    const MobAi* brain = world.tryGet<MobAi>(self);
    const AiKind kind = brain != nullptr ? brain->kind : drive.ai;

    // Only a mob that can move idles: a hole has nothing to drift with, and
    // giving it the columns costs an archetype per nest for no behaviour.
    if (drive.speed > 0.0 && drive.ai != AiKind::Stationary) {
        if (!world.has<PassiveMotion>(self)) {
            world.add<PassiveMotion>(self, PassiveMotion{PassiveState::Idle, nowMillis, Vec2{0, 0}});
        }
        if (drive.beeFlight && !world.has<Wobble>(self)) {
            // The phase is per-bee so a field of them weaves out of step with
            // itself rather than moving as one body.
            world.add<Wobble>(self, Wobble{rng_.angle()});
        }
        // The two movers that walk to a POINT rather than hopping on a
        // heading: a chain head, whose turns have to stay long enough for the
        // body to trace, and a storm, which blows toward one.
        if ((world.has<BodySegment>(self) || kind == AiKind::Sandstorm) &&
            !world.has<WanderTarget>(self)) {
            world.add<WanderTarget>(self);
        }
    }
}

// ---------------------------------------------------------------------------
// Targeting
// ---------------------------------------------------------------------------

Entity MobAiSystem::acquireTarget(World& world, const Terrain& terrain, const SpatialGrid& grid,
                                  Entity self, Vec2 from, Realm realm, double range) {
    ++stats_.targetScans;

    grid.query(realm, from, range + maxAggroBonus_, gridScratch_);
    candidates_.clear();
    for (const Entity candidate : gridScratch_) {
        if (candidate == self) continue;
        if (!world.has<PlayerTag>(candidate)) continue;
        if (!entityUsable(world, candidate)) continue;
        const Transform* transform = world.tryGet<Transform>(candidate);
        if (transform == nullptr) continue;

        // A raised aggro radius makes the player read as that many units
        // closer, so one comparison covers both "is anyone in range" and "who
        // is the most conspicuous".
        const PlayerModifiers* mods = world.tryGet<PlayerModifiers>(candidate);
        const double bonus = mods != nullptr ? mods->aggroRadiusBonus : 0.0;
        const double score = distance(from, transform->position) - bonus;
        if (score > range) continue;
        candidates_.push_back(Candidate{candidate, transform->position, score});
    }

    std::sort(candidates_.begin(), candidates_.end(),
              [](const Candidate& a, const Candidate& b) { return a.score < b.score; });

    // Nearest-first, stopping at the first one actually visible. Raycasting
    // every candidate is what turns a crowded spawn into a quadratic tick, so
    // the walk is capped -- past the cap the mob simply notices nobody this
    // decision and looks again on the next one.
    const std::size_t rays = std::min<std::size_t>(candidates_.size(),
                                                   static_cast<std::size_t>(kTargetLosRayCap));
    for (std::size_t i = 0; i < rays; ++i) {
        if (!terrain.segmentBlocked(from, candidates_[i].position, realm)) return candidates_[i].entity;
    }
    return NULL_ENTITY;
}

Entity MobAiSystem::acquirePetTarget(const Terrain& terrain, Entity self, Vec2 from, Realm realm,
                                     double range) {
    if (petList_.empty()) return NULL_ENTITY;

    // Not counted as a target scan: the stat exists to report what the
    // broadphase cost, and this is a walk over a list a player's summons fit
    // in twice over.
    const double reachSq = range * range;
    candidates_.clear();
    for (const Candidate& pet : petList_) {
        if (pet.entity == self) continue;
        if (pet.realm != realm) continue;
        // Strictly inside, and with no aggro bonus applied: a bonus is
        // something a FLOWER carries to make itself conspicuous, and a summon
        // carries none of it.
        const double gapSq = distanceSq(from, pet.position);
        if (!(gapSq < reachSq)) continue;
        candidates_.push_back(Candidate{pet.entity, pet.position, gapSq});
    }

    // Squared distance sorts in the same order the distance does, so the scan
    // never takes a square root it does not have to.
    std::sort(candidates_.begin(), candidates_.end(),
              [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
    const std::size_t rays = std::min<std::size_t>(candidates_.size(),
                                                   static_cast<std::size_t>(kTargetLosRayCap));
    for (std::size_t i = 0; i < rays; ++i) {
        if (!terrain.segmentBlocked(from, candidates_[i].position, realm)) return candidates_[i].entity;
    }
    return NULL_ENTITY;
}

Entity MobAiSystem::nearestAttacker(World& world, Entity self, Vec2 from, Realm realm,
                                    double radius) const {
    // Bounty is the ledger combat already keeps of who hurt this mob, so
    // working out who to turn on is a walk over a handful of entries rather
    // than another broadphase query -- which is what lets retaliation happen on
    // the tick the hit lands instead of waiting for the decision clock.
    const Bounty* bounty = world.tryGet<Bounty>(self);
    if (bounty == nullptr) return NULL_ENTITY;

    Entity best = NULL_ENTITY;
    double bestSq = radius * radius;
    for (const Bounty::Share& share : bounty->contributors) {
        if (share.damage <= 0.0) continue;
        if (!entityUsable(world, share.player)) continue;
        const Transform* transform = world.tryGet<Transform>(share.player);
        if (transform == nullptr) continue;
        if (transform->realm != realm) continue;
        const double gapSq = distanceSq(from, transform->position);
        // Nearest rather than biggest contributor: the ledger accumulates over
        // the mob's entire life, so the heaviest hitter is often someone who
        // left minutes ago, while whoever is standing next to it is who a
        // player expects it to round on.
        if (gapSq <= bestSq) {
            bestSq = gapSq;
            best = share.player;
        }
    }
    return best;
}

bool MobAiSystem::targetHeld(World& world, const Terrain& terrain, Vec2 from, Realm realm,
                             Entity target) const {
    if (!entityUsable(world, target)) return false;
    const Transform* transform = world.tryGet<Transform>(target);
    if (transform == nullptr) return false;

    // Aggro RANGE governs acquisition only. Retention is a flat five viewports
    // -- it reads neither the mob's own range nor the player's aggro bonus --
    // and a wall coming between the two drops the target on the spot, which is
    // what sends the mob back to wandering instead of grinding into geometry.
    if (distanceSq(from, transform->position) >
        kMobTargetRetainRadius * kMobTargetRetainRadius) {
        return false;
    }
    if (transform->realm != realm) return false;
    return !terrain.segmentBlocked(from, transform->position, realm);
}

bool MobAiSystem::petTargetHeld(World& world, const Terrain& terrain, Vec2 from, Realm realm,
                                Entity target, double range) const {
    if (!world.has<Pet>(target)) return false;
    if (!entityUsable(world, target)) return false;
    const Transform* transform = world.tryGet<Transform>(target);
    if (transform == nullptr) return false;

    // The mob's own aggro range, not the five viewports a flower is chased
    // across. A summon that walks out of range is simply forgotten, and the
    // mob goes back to looking for the player it would rather have.
    if (!(distanceSq(from, transform->position) < range * range)) return false;
    if (transform->realm != realm) return false;
    return !terrain.segmentBlocked(from, transform->position, realm);
}

void MobAiSystem::collectChain(World& world, Entity self, std::vector<Entity>& out) const {
    out.clear();
    out.push_back(self);
    if (!world.has<BodySegment>(self)) return;

    // Bounded by the number of links that exist rather than by reaching the
    // tail: repairChains() cuts cycles in the pass AFTER this one, so a chain
    // that closed on itself this tick must not be walked forever.
    Entity ahead = self;
    for (std::size_t links = followerOf_.size(); links > 0; --links) {
        const auto next = followerOf_.find(ahead);
        if (next == followerOf_.end()) return;
        out.push_back(next->second);
        ahead = next->second;
    }
}

Entity MobAiSystem::freshProvoker(World& world, Entity self, Vec2 from, Realm realm) {
    collectChain(world, self, chainScratch_);

    double total = 0.0;
    for (const Entity part : chainScratch_) {
        const Bounty* bounty = world.tryGet<Bounty>(part);
        if (bounty == nullptr) continue;
        for (const Bounty::Share& share : bounty->contributors) total += share.damage;
    }
    if (!(total > 0.0)) return NULL_ENTITY;

    // No watermark means this mob has never been looked at, and it is looked
    // at on the first tick of its life, so anything already on the ledger got
    // there since it was born and counts as fresh.
    const auto seen = ledgerSeen_.find(self);
    const double before = seen != ledgerSeen_.end() ? seen->second : 0.0;
    if (seen != ledgerSeen_.end()) seen->second = total;
    else ledgerSeen_.emplace(self, total);
    if (!(total > before)) return NULL_ENTITY;

    // Nearest contributor rather than whichever share grew: the ledger records
    // how much each player has taken off the animal, never who took it last,
    // and with one attacker -- which is nearly every fight -- they are the same
    // player anyway. The radius is a cost bound and not a rule: the reference
    // provokes from any distance at all, and a provocation from further out
    // than this is dropped by the retention test on the very same tick.
    Entity best = NULL_ENTITY;
    double bestSq = kMobTargetRetainRadius * kMobTargetRetainRadius;
    for (const Entity part : chainScratch_) {
        const Entity found = nearestAttacker(world, part, from, realm, kMobTargetRetainRadius);
        if (found == NULL_ENTITY) continue;
        const Transform* at = world.tryGet<Transform>(found);
        if (at == nullptr) continue;
        const double gapSq = distanceSq(from, at->position);
        if (gapSq <= bestSq) {
            bestSq = gapSq;
            best = found;
        }
    }
    return best;
}

void MobAiSystem::stampAttack(World& world, Entity self, MobAi& ai, double nowMillis,
                              const Drive& drive) {
    double cooldown = drive.attackCooldownMillis;
    if (!(cooldown > 0.0)) {
        const ContactDamage* contact = world.tryGet<ContactDamage>(self);
        cooldown = contact != nullptr ? contact->intervalMillis : kMobHitIntervalMillis;
    }
    if (nowMillis - ai.lastAttackMillis < cooldown) return;

    // Combat owns the per-victim ledger; the AI only reads it, so the two can
    // never disagree about whether this victim has already been hit.
    const HitCooldowns* hits = world.tryGet<HitCooldowns>(self);
    if (hits != nullptr && !hits->ready(ai.target, nowMillis)) return;

    ai.lastAttackMillis = nowMillis;
    ++stats_.attacks;
}

void MobAiSystem::fireVolley(World& world, Entity shooter, const MobType& type, MobAi& ai,
                             const Drive& drive, Vec2 from, double aimAngle,
                             Vec2 shooterVelocity, double nowMillis, CommandBuffer& commands) {
    const ContentRegistry& registry = content();
    const ProjectileSpec& spec = registry.mob(type.configIndex).projectile;
    if (!spec.present || spec.ammoPetalIndex == kInvalidIndex) return;

    // The mob's own cadence. Two thousand milliseconds is the fallback for a
    // config that states none, not the rate every shooter fires at.
    const double cooldown = drive.attackCooldownMillis > 0.0 ? drive.attackCooldownMillis
                                                             : kDefaultVolleyCooldownMillis;
    if (nowMillis - ai.lastProjectileMillis < cooldown) return;

    // Ammunition is graded at the SHOOTER's tier rather than at the rarity its
    // config names, so an apex hornet fires apex missiles.
    const PetalStats ammo = registry.petalStats(spec.ammoPetalIndex, type.rarity);
    const std::size_t tier = static_cast<std::size_t>(clamp(rarityIndex(type.rarity), 0, kRarityCount - 1));
    const double scaling = kMobSizeScale[tier];

    const double speed = spec.speed > 0.0 ? spec.speed : kDefaultProjectileSpeed;
    // Stated in COMMON-TIER units: `distance` IS the reach a common shooter
    // gets, and a higher tier reaches further in proportion to its body.
    // Deliberately the tier scale and not the shooter's own body (which the
    // SIZE below uses): two same-tier mobs of different authored sizes shoot
    // equally far, they just fire different-calibre shots.
    const double reach = spec.distance * scaling / kProjectileReachReferenceScale;
    if (!(reach > 0.0) || !(speed > 0.0)) return;

    // How much bigger than a stock body this shooter is. It carries BOTH the
    // rarity step and the mob's own authored `size`, which is the difference
    // from the bare tier scale it replaces: a hornet is a size-1.3 mob, and
    // its missiles are now 1.3x an ordinary shooter's rather than identical to
    // them. For a size-1 mob this is exactly kMobSizeScale[tier] and nothing
    // about its volley moves.
    const Body* shooterBody = world.tryGet<Body>(shooter);
    const double ownerScale = shooterBody != nullptr && kMobBaseRadius > 0.0
                                  ? std::max(0.05, shooterBody->radius / kMobBaseRadius)
                                  : scaling;

    VolleyShot shot;
    shot.from = from;
    if (const Transform* shooterAt = world.tryGet<Transform>(shooter)) shot.realm = shooterAt->realm;
    shot.speed = speed;
    // The shot's calibre comes off the ammunition petal's `size` stat, then is
    // scaled by the SHOOTER's body on its own divisor -- reach and size grow at
    // different rates with rarity. Off `size` and not off the ammunition's
    // radius, matching the reference (`petalStats.size * scaling / 3` into a
    // `size * 20 / 2` body): a hornet's missile is the same calibre it always
    // was however the petal it is made of collides.
    shot.radius = std::max(
        1.0, ammo.size * kProjectileRadiusPerSize * ownerScale / kProjectileSizeDivisor);
    shot.distance = reach;
    shot.damage = ammo.damage;
    // Graded at the shooter's tier alongside the damage, so an apex hornet's
    // missiles punch through what a common hornet's are stopped by.
    shot.health = ammo.breakable && ammo.health > 0.0 ? ammo.health : kProjectileDefaultHealth;
    // The shooter's own travel, carried into the volley.
    shot.inherited = shooterVelocity;
    shot.owner = shooter;
    // A pet's shot is fired by the pet and answerable to the player, so a kill
    // it lands credits the flower that summoned it.
    const Pet* pet = world.tryGet<Pet>(shooter);
    shot.creditTo = pet != nullptr ? pet->owner : shooter;
    const Faction* own = world.tryGet<Faction>(shooter);
    shot.faction = own != nullptr ? *own : Faction{Team::Hostiles, false};
    // Poison, knockback and the slow are NOT stamped on the shot: combat
    // resolves them from the ammunition index and the tier it was fired at, so
    // the two sides of a hit can never disagree about what the tier means.
    shot.seekRange = spec.seekRange;
    shot.seekCone = spec.seekCone;
    shot.petalIndex = spec.ammoPetalIndex;
    shot.rarity = type.rarity;
    // The reference stamps the shooter's TYPE on every shot so the player
    // hook can ask isGlitchInfectingType() of it later; the answer to that
    // question is all this side needs.
    shot.glitchInfecting = registry.mob(type.configIndex).glitchInfecting;

    ai.lastProjectileMillis = nowMillis;
    ++stats_.volleys;

    const int count = std::max(1, spec.count);
    for (int i = 0; i < count; ++i) {
        // spreadAngle is the STEP between adjacent shots, so a volley is
        // centred on the bearing and a single shot takes it untouched.
        shot.angle = count > 1
                         ? wrapAngle(aimAngle + (i - (count - 1) * 0.5) * spec.spreadAngle)
                         : wrapAngle(aimAngle);
        shot.identified = static_cast<bool>(allocateNetId);
        shot.netId = shot.identified ? allocateNetId() : 0;
        commands.defer([shot](World& deferred) { spawnShot(deferred, shot); });
    }

    // Recoil. The whole volley's momentum, repaid to the shooter at a small
    // fraction and capped low: this game's kick is a rock backwards on the
    // shot, not diep.io's reverse thruster, and it is the reason a firing mob
    // reads as firing at all when its missile is off-screen.
    //
    // Written straight to the position and not to Knockback, because moveMobs
    // deliberately never drains that component (see the note there). FLOWERS
    // get none of this at all: their movement is eased from the wire with no
    // prediction, so a server-side kick arrives as a rubber-band rather than
    // as recoil.
    if (Transform* transform = world.tryGet<Transform>(shooter)) {
        // Re-fetched rather than reusing shooterBody: the defers above only
        // queue work, but a component pointer held across anything that could
        // touch the world is the bug this file is one archetype away from.
        const Body* body = world.tryGet<Body>(shooter);
        const double mass = body != nullptr && body->mass > 1e-6 ? body->mass : 1.0;
        const double kick = std::min(
            kProjectileMaxRecoil,
            kProjectileRecoilScale * count *
                projectilePush(projectileMass(shot.radius), speed, mass));
        if (kick > 0.0) transform->position -= Vec2::fromAngle(aimAngle, kick);
    }
}

// ---------------------------------------------------------------------------
// Behaviours
// ---------------------------------------------------------------------------

void MobAiSystem::driftPassive(World& world, Entity self, const Body& body, Motion& motion,
                               MobAi& ai, double speed, double nowMillis, double dt) {
    PassiveMotion* passive = world.tryGet<PassiveMotion>(self);
    // An immobile mob never drifts, and one that was never given the machine
    // has nothing to run: either way it holds still rather than coasting on
    // whatever it was doing before.
    if (passive == nullptr || !(speed > 0.0)) {
        motion.velocity = Vec2{0, 0};
        return;
    }

    // Distance per hop is the sum of the accelerations divided by the friction,
    // so scaling the ACCELERATION by the size factor scales how far a hop
    // carries while the phase durations stay fixed. That is what makes a big
    // mob's hop proportional to its body instead of merely slower.
    const double accel = speed * kPassiveAccelScale * sizeFactor(body.radius);
    Vec2 push{0, 0};

    if (const Wobble* wobble = world.tryGet<Wobble>(self)) {
        // Bees do not hop. They cruise, and the heading sways at 1.5 rad/s
        // scaled by sin(2t) -- which integrates to the +-0.75 rad weave of the
        // flight line -- around a base heading re-picked every five seconds.
        if (nowMillis - passive->stateStartMillis >= kBeeHeadingMillis) {
            ai.wanderAngle = rng_.angle();
            passive->stateStartMillis = nowMillis;
        }
        const double t = nowMillis / 1000.0 + wobble->phase;
        ai.wanderAngle = wrapAngle(ai.wanderAngle + kBeeWobbleRate * std::sin(2.0 * t) * dt);

        // Sustained rather than ramped, and pulsed down for the first third of
        // every window, which is what gives the cruise its beat.
        double magnitude = accel * kBeeCruiseAccelScale;
        if (std::fmod(t * 1000.0, kBeePulsePeriodMillis) < kBeePulseMillis) {
            magnitude *= kBeePulseScale;
        }
        push = Vec2::fromAngle(ai.wanderAngle, magnitude);
    } else {
        const double elapsed = nowMillis - passive->stateStartMillis;
        if (passive->state == PassiveState::Idle) {
            if (elapsed >= kPassiveIdleMillis) {
                // The heading is drawn at the transition, so the mob turns to
                // face it while still standing still and only then sets off.
                ai.wanderAngle = rng_.angle();
                passive->state = PassiveState::Moving;
                passive->stateStartMillis = nowMillis;
            }
        } else if (elapsed >= kPassiveMoveMillis) {
            passive->state = PassiveState::Idle;
            passive->stateStartMillis = nowMillis;
        } else if (elapsed >= kPassiveCoastMillis) {
            // Half a second of coasting on friction alone -- the mob is in the
            // Moving state but is not being pushed yet -- then a parabolic ramp
            // peaking halfway through the two seconds after it.
            const double r = (elapsed - kPassiveCoastMillis) / kPassiveRampMillis;
            push = Vec2::fromAngle(ai.wanderAngle, accel * 2.0 * (r - r * r));
        }
    }

    // Friction is per TICK, not per second: this is a fixed-step integrator and
    // spreading it over dt changes how far every idle mob in the world travels.
    // The clamp is what stops radius-proportional acceleration from drifting an
    // apex mob at several times a player's top speed.
    passive->velocity =
        (passive->velocity * (1.0 - kPassiveFriction) + push).clampedLength(kMaxWanderSpeed);
    motion.velocity = passive->velocity;
}

Vec2 MobAiSystem::wanderToPoint(WanderTarget& wander, Vec2 from, const Body& body, double speed,
                                double nowMillis) {
    const double factor = sizeFactor(body.radius);
    if (wander.pickedAtMillis <= 0.0 || nowMillis - wander.pickedAtMillis > kWanderRepickMillis) {
        const double range = kEnemyWanderRange * factor;
        wander.destination = from + Vec2{rng_.range(-range, range), rng_.range(-range, range)};
        wander.pickedAtMillis = nowMillis;
    }
    if (!(speed > 0.0)) return Vec2{0, 0};

    const Vec2 offset = wander.destination - from;
    const double gap = offset.length();
    // Arrived: stop rather than jitter across the last unit of it.
    if (!(gap > kWanderArriveDistance)) return Vec2{0, 0};
    return offset * (std::min(speed * kMobWanderSpeedScale * factor, kMaxWanderSpeed) / gap);
}

Vec2 MobAiSystem::steerIdle(World& world, Entity self, const Transform& transform, Motion& motion,
                            const Body& body, MobAi& ai, double speed, double nowMillis, double dt) {
    // A centipede head keeps the destination wander deliberately: a chain that
    // stopped dead every second would read as a broken animal rather than a
    // crawling one, and the head is what the whole body traces.
    if (world.has<BodySegment>(self)) {
        if (WanderTarget* wander = world.tryGet<WanderTarget>(self)) {
            motion.velocity = wanderToPoint(*wander, transform.position, body, speed, nowMillis);
            return motion.velocity;
        }
    }

    driftPassive(world, self, body, motion, ai, speed, nowMillis, dt);
    // Facing follows the HEADING rather than the velocity here: the machine
    // turns the mob when it picks a direction, while it is still at rest, and
    // accelerates along it afterwards.
    return Vec2::fromAngle(ai.wanderAngle);
}

Vec2 MobAiSystem::steerSandstorm(World& world, const SpatialGrid& grid, Entity self,
                                 const MobType& type, const Transform& transform, MobAi& ai,
                                 double speed, double nowMillis, double dt) {
    // It never had a target and never will; clearing it means nothing that
    // reads MobAi can mistake a sandstorm for something that is hunting.
    ai.target = NULL_ENTITY;

    WanderTarget* wander = world.tryGet<WanderTarget>(self);
    // Nothing to blow toward yet -- equipBehaviour hands the storm one on the
    // first tick it thinks -- and a storm holding still for a tick is better
    // than one blowing along a heading it never picked.
    if (wander == nullptr) return Vec2{0, 0};

    // Re-rolled OUTRIGHT, three times a second, on the storm's own clock.
    // Swinging the previous heading instead is what turns this into a cloud on
    // a course: the reference's storm doubles back on itself constantly and
    // covers a fraction of the ground per second that a drifting one does.
    if (nowMillis >= ai.nextHeadingMillis) {
        ai.nextHeadingMillis = nowMillis + kSandstormHeadingMillis;
        ai.wanderAngle = rng_.angle();
        wander->destination =
            transform.position + Vec2::fromAngle(ai.wanderAngle, kSandstormWanderRange);
        wander->pickedAtMillis = nowMillis;
    }

    Vec2 blow{0, 0};
    const Vec2 offset = wander->destination - transform.position;
    const double gap = offset.length();
    // Weather travels at its FULL speed, not the stroll a mob walks a wander
    // destination at.
    if (speed > 0.0 && gap > kWanderArriveDistance) blow = offset * (speed / gap);

    // Measured from where the storm ENDS this tick, which is the order the
    // reference resolves the two in: it takes its own step and then drags.
    if (rarityIndex(type.rarity) >= rarityIndex(kSandstormSuckRarity)) {
        suckPlayers(world, grid, transform.position + blow * dt, transform.realm, dt);
    }
    return blow;
}

void MobAiSystem::suckPlayers(World& world, const SpatialGrid& grid, Vec2 from, Realm realm,
                              double dt) {
    grid.query(realm, from, kSandstormSuckRange, gridScratch_);
    for (const Entity candidate : gridScratch_) {
        if (!world.has<PlayerTag>(candidate)) continue;
        if (!entityUsable(world, candidate)) continue;
        Transform* at = world.tryGet<Transform>(candidate);
        if (at == nullptr) continue;

        const Vec2 offset = from - at->position;
        const double gap = offset.length();
        if (!(gap > 0.0) || !(gap < kSandstormSuckRange)) continue;

        // Written straight into the position: the drag is not a force the
        // flower gets to fight with its own movement, and it does not stop at
        // walls either -- which is the whole threat of standing near a storm.
        // Two overlapping storms compound within the tick, because the second
        // reads the position the first just wrote.
        const double pull = kSandstormSuckSpeed * (1.0 - gap / kSandstormSuckRange) * dt;
        at->position += offset * (pull / gap);
    }
}

bool MobAiSystem::walkHome(World& world, Entity self, const Transform& transform, MobAi& ai,
                           double chaseSpeed, double nowMillis, Vec2& desired,
                           CommandBuffer& commands) {
    HoleTether* tether = world.tryGet<HoleTether>(self);
    if (tether == nullptr) return false;

    // The nest is gone, so there is nothing left to defend: the child is
    // unparented and roams free from here on rather than guarding a hole that
    // no longer exists. Deferred, because dropping a component relocates the
    // very rows the mob walk above is holding.
    if (!entityUsable(world, tether->hole)) {
        tether->hole = NULL_ENTITY;
        tether->returning = false;
        commands.removeComponent<HoleTether>(self);
        return false;
    }

    const Transform* nest = world.tryGet<Transform>(tether->hole);
    // Read live rather than remembered, because a queen carries her brood's
    // home around with her; the spawn point is only the fallback for a parent
    // with no position of its own.
    const Vec2 home = nest != nullptr ? nest->position : tether->home;
    const Vec2 offset = home - transform.position;
    const double gap = offset.length();

    if (!tether->returning && gap > kSummonRetreatRadius) {
        // The target is DROPPED, not merely ignored for the walk: an escort
        // that kept it would turn round and resume the chase the moment it got
        // home, which is the kiting this leash exists to stop.
        ai.target = NULL_ENTITY;
        tether->returning = true;
    }
    if (!tether->returning) return false;

    if (gap < kSummonArriveDistance) {
        tether->returning = false;
        // Home, and idling from a standing start rather than partway through
        // whatever hop the machine was in when it was dragged away.
        if (PassiveMotion* passive = world.tryGet<PassiveMotion>(self)) {
            passive->state = PassiveState::Idle;
            passive->stateStartMillis = nowMillis;
        }
        return false;
    }

    // Home at the pace it would chase at: the walk back is a march, and an
    // escort that ambled would be picked off on the way.
    desired = offset * (chaseSpeed / gap);
    return true;
}

bool MobAiSystem::steerAggressive(World& world, const Terrain& terrain, const SpatialGrid& grid,
                                  Entity self, const MobType& type, const Transform& transform,
                                  const Body& body, MobAi& ai, const Drive& drive,
                                  double chaseSpeed, double nowMillis, Vec2& desired,
                                  CommandBuffer& commands) {
    const double range = ai.aggroRange > 0.0 ? ai.aggroRange : kEnemyChaseRange;

    // Provocation first, which is where the reference has it: over there it
    // happens in the damage phase and the AI merely validates what it left
    // behind, so a mob provoked from behind a wall or from five viewports away
    // loses the target again on this very tick.
    //
    // NEUTRAL only. A hostile mob is not provoked by anything -- it finds its
    // own targets, inside its own range -- so sniping one from beyond that
    // range leaves it wandering rather than charging the horizon.
    if (ai.kind == AiKind::Neutral && ai.target == NULL_ENTITY) {
        ai.target = freshProvoker(world, self, transform.position, transform.realm);
    }

    // Both halves run every tick. Keeping a target is a pointer chase, a
    // distance and one ray; gaining one is a broadphase query and a fan of
    // rays, and is much the more expensive -- but the reference pays it per
    // mob per tick, and a mob that only looks on a clock ignores a player who
    // walks up to it for as long as the clock says, which reads as the whole
    // field lagging behind the flower. The LOD stride is what bounds this.
    if (ai.target != NULL_ENTITY) {
        const bool held =
            world.has<Pet>(ai.target)
                ? petTargetHeld(world, terrain, transform.position, transform.realm, ai.target, range)
                : targetHeld(world, terrain, transform.position, transform.realm, ai.target);
        if (!held) ai.target = NULL_ENTITY;
    }
    // A neutral mob never goes looking: it only ever has the target that hurt it.
    if (ai.kind == AiKind::Hostile) {
        // The player scan runs even while a pet is being chased: a flower
        // outranks a summon, so it takes the mob over the moment one comes
        // into range rather than waiting for the pet to die or walk off.
        if (ai.target == NULL_ENTITY || world.has<Pet>(ai.target)) {
            const Entity player = acquireTarget(world, terrain, grid, self, transform.position,
                                                transform.realm, range);
            if (player != NULL_ENTITY) ai.target = player;
        }
        // A pet is what is left when no player is in range, which is what lets
        // a summon soak the aggro it was sent out to soak.
        if (ai.target == NULL_ENTITY) {
            ai.target = acquirePetTarget(terrain, self, transform.position, transform.realm, range);
        }
    }
    if (ai.target == NULL_ENTITY) return false;

    const Transform* threat = world.tryGet<Transform>(ai.target);
    if (threat == nullptr) {
        ai.target = NULL_ENTITY;
        return false;
    }

    // Taken BEFORE the mob moves, and the volley is aimed along it: a shot led
    // by one tick of the shooter's own travel is a different weapon.
    const Vec2 toTarget = threat->position - transform.position;
    const double gap = toTarget.length();
    const Body* threatBody = world.tryGet<Body>(ai.target);
    const double reach = body.radius + (threatBody != nullptr ? threatBody->radius : 0.0) + kMobContactSlack;
    if (gap <= reach) stampAttack(world, self, ai, nowMillis, drive);

    desired = gap > 0.0 ? toTarget * (chaseSpeed / gap) : Vec2{0, 0};
    if (drive.shoots) {
        fireVolley(world, self, type, ai, drive, transform.position, toTarget.angle(), desired,
                   nowMillis, commands);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Level of detail
// ---------------------------------------------------------------------------

bool MobAiSystem::stepsThisTick(Entity self, Vec2 position, Realm realm, Rarity rarity,
                                const std::vector<RealmPoint>& activePlayers) const {
    // A boss is never off: see the header. It is a few mobs' worth of full-rate
    // AI in exchange for the one class of mob whose behaviour anybody watches
    // from a distance.
    if (isBossRarity(rarity)) return true;
    if (mobActive(position, realm, activePlayers)) return true;
    return (tick_ + entityIndex(self)) % static_cast<std::uint64_t>(kMobFarStride) == 0;
}

void MobAiSystem::driftUnwatched(World& world, Entity self, double nowMillis, double dt) {
    Motion* motion = world.tryGet<Motion>(self);
    if (motion == nullptr) return;

    MobAi* ai = world.tryGet<MobAi>(self);
    const Body* body = world.tryGet<Body>(self);
    const MobType* type = world.tryGet<MobType>(self);
    if (ai == nullptr || body == nullptr || type == nullptr ||
        !idleDrifting(world, self, *ai)) {
        // Nothing decided this tick means nothing travelled this tick.
        motion->velocity = Vec2{0, 0};
        return;
    }

    // A mob that has never thought has never been handed the drift machine
    // either; driftPassive() holds it still until its first step equips it.
    const Drive drive = driveFor(type->configIndex, type->rarity);
    driftPassive(world, self, *body, *motion, *ai,
                 drive.speed * slowFactorOf(world, self, nowMillis), nowMillis, dt);

    // The drift's heading IS the mob's facing over there -- the same ungated
    // integrator writes both -- so the angle keeps full rate as well, rather
    // than trailing the heading by up to a stride's worth of ticks.
    if (Transform* transform = world.tryGet<Transform>(self)) {
        transform->angle = steerFacing(transform->angle, Vec2::fromAngle(ai->wanderAngle),
                                       drive.hideRotation, drive.reversed, kPi);
    }
}

// ---------------------------------------------------------------------------
// One mob
// ---------------------------------------------------------------------------

void MobAiSystem::steerMob(World& world, const Terrain& terrain, const SpatialGrid& grid,
                           Entity self, Transform& transform, Motion& motion, const Body& body,
                           const MobType& type, MobAi& ai, const Drive& drive, double nowMillis,
                           double dt, CommandBuffer& commands) {
    // The ten player-speed chasers never read `speed` while pursuing, so the
    // slow has to reach the override as well or a web slows every mob in the
    // game except the ones it is for.
    const double factor = slowFactorOf(world, self, nowMillis);
    const double speed = drive.speed * factor;
    const double chaseSpeed = drive.chaseSpeed * factor;

    Vec2 desired{0, 0};
    // True when the mob is actively holding a velocity of its own -- a pursuit,
    // weather, a march home. False hands both the velocity and the heading to
    // the idle machines.
    bool driven = false;

    // Ahead of everything else, exactly where the reference tests it: a child
    // dragged too far from the nest that made it abandons what it was chasing,
    // and a tick spent walking home is spent on nothing else.
    if (walkHome(world, self, transform, ai, chaseSpeed, nowMillis, desired, commands)) {
        motion.velocity = desired;
        transform.angle =
            steerFacing(transform.angle, desired, drive.hideRotation, drive.reversed, kPi);
        return;
    }

    switch (ai.kind) {
    case AiKind::Stationary:
        // Never moves, and the velocity is CLEARED rather than merely left
        // untargeted: easing toward zero would let a nest slide for half a
        // second after a knockback, and a hole that drifts is a hole that ends
        // up inside a wall.
        motion.velocity = Vec2{0, 0};
        ai.target = NULL_ENTITY;
        transform.angle = steerFacing(transform.angle, Vec2{0, 0}, drive.hideRotation,
                                      drive.reversed, kPi);
        return;

    case AiKind::Sandstorm:
        desired = steerSandstorm(world, grid, self, type, transform, ai, speed, nowMillis, dt);
        driven = true;
        break;

    case AiKind::Passive:
        // Nothing to hunt, and nothing to run from either: the reference has no
        // mob flee state at all, so a passive mob being hit keeps hopping about
        // on the idle machine and stays where the player's petals can reach it
        // rather than bolting. The target is cleared because only something
        // else writing one could have put it there.
        ai.target = NULL_ENTITY;
        break;

    case AiKind::Neutral:
    case AiKind::Hostile:
        driven = steerAggressive(world, terrain, grid, self, type, transform, body, ai, drive,
                                 chaseSpeed, nowMillis, desired, commands);
        break;
    }

    if (driven) {
        motion.velocity = desired;
    } else {
        desired = steerIdle(world, self, transform, motion, body, ai, speed, nowMillis, dt);
    }

    // Facing follows what the mob is TRYING to do, falling back to what it is
    // actually doing: a mob pinned against a wall or mid-knockback keeps facing
    // its target rather than spinning to face the shove.
    //
    // Adopted outright, with no turn limit: the reference assigns the angle
    // from this tick's movement vector, so a mob that picks a new heading is
    // already pointing along it. Easing there instead leaves the sprite aimed
    // at where the mob used to be going for a third of a second, which is most
    // of a hop and the whole of a turn onto a target that came up behind it.
    const Vec2 travel = desired.lengthSq() > kDirectionEpsilonSq ? desired : motion.velocity;
    transform.angle = steerFacing(transform.angle, travel, drive.hideRotation, drive.reversed, kPi);
}

// ---------------------------------------------------------------------------
// Pets
// ---------------------------------------------------------------------------

Entity MobAiSystem::acquirePetPrey(World& world, const Terrain& terrain, const SpatialGrid& grid,
                                   Entity self, Vec2 from, Realm realm, MobAi& ai, bool hasOwner,
                                   Vec2 ownerPosition, double range) {
    // With a living owner the pet sees exactly what the owner's SCREEN shows,
    // not what its own aggro range reaches. An ownerless pet has no screen to
    // be clipped to and falls back to that range.
    const auto visible = [&](Vec2 at, double gapSq) {
        if (!hasOwner) return gapSq < range * range;
        return std::abs(at.x - ownerPosition.x) <= kPetViewHalfWidth &&
               std::abs(at.y - ownerPosition.y) <= kPetViewHalfHeight;
    };

    if (ai.target != NULL_ENTITY) {
        const Transform* at = world.tryGet<Transform>(ai.target);
        if (at != nullptr && world.has<MobTag>(ai.target) && !world.has<Pet>(ai.target) &&
            entityUsable(world, ai.target) &&
            at->realm == realm && visible(at->position, distanceSq(from, at->position)) &&
            !terrain.segmentBlocked(from, at->position, realm)) {
            return ai.target;
        }
        ai.target = NULL_ENTITY;
    }

    ++stats_.targetScans;
    // Centred on whatever the clip is stated against, and wide enough to reach
    // the corner of it: a pet running ahead of its owner must still see
    // everything the owner can.
    const Vec2 centre = hasOwner ? ownerPosition : from;
    const double reach = hasOwner
                             ? std::sqrt(kPetViewHalfWidth * kPetViewHalfWidth +
                                         kPetViewHalfHeight * kPetViewHalfHeight)
                             : range;
    grid.query(realm, centre, reach, gridScratch_);
    candidates_.clear();
    for (const Entity candidate : gridScratch_) {
        if (candidate == self) continue;
        // Wild mobs only: pets do not fight each other, and a pet chasing its
        // owner's other summon is a squad that never reaches anything.
        if (!world.has<MobTag>(candidate) || world.has<Pet>(candidate)) continue;
        if (!entityUsable(world, candidate)) continue;
        const Transform* at = world.tryGet<Transform>(candidate);
        if (at == nullptr) continue;
        const double gapSq = distanceSq(from, at->position);
        if (!visible(at->position, gapSq)) continue;
        // Scored nearest to the PET even though the clip is the owner's.
        candidates_.push_back(Candidate{candidate, at->position, gapSq});
    }

    std::sort(candidates_.begin(), candidates_.end(),
              [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
    const std::size_t rays = std::min<std::size_t>(candidates_.size(),
                                                   static_cast<std::size_t>(kTargetLosRayCap));
    for (std::size_t i = 0; i < rays; ++i) {
        if (!terrain.segmentBlocked(from, candidates_[i].position, realm)) return candidates_[i].entity;
    }
    return NULL_ENTITY;
}

bool MobAiSystem::teleportPetToOwner(const Terrain& terrain, Transform& transform,
                                     Vec2 ownerPosition) {
    // Eight positions around the owner, taken in order, so a pet recovered from
    // behind a wall lands in a predictable place rather than wherever a roll
    // put it.
    for (int i = 0; i < 8; ++i) {
        const Vec2 at = ownerPosition + Vec2::fromAngle(kPi * 0.25 * i, kPetTeleportDistance);
        if (terrain.blocked(at, transform.realm)) continue;
        if (terrain.segmentBlocked(at, ownerPosition, transform.realm)) continue;
        transform.position = at;
        return true;
    }
    // Nothing on the ring was clear: the owner's own tile will do, and if even
    // that is blocked the pet stays where it is and tries again next tick.
    if (!terrain.blocked(ownerPosition, transform.realm)) {
        transform.position = ownerPosition;
        return true;
    }
    return false;
}

void MobAiSystem::steerPet(World& world, const Terrain& terrain, const SpatialGrid& grid,
                           Entity self, Transform& transform, Motion& motion, const Body& body,
                           const MobType& type, MobAi& ai, Entity owner, bool ownerAlive,
                           const Drive& drive, double nowMillis, double dt,
                           CommandBuffer& commands) {
    const double speed = drive.speed * slowFactorOf(world, self, nowMillis);

    // A tamed neutral has nothing left to be neutral ABOUT -- it fights for its
    // owner -- so neutral and hostile run the same pet AI. Passive stays
    // passive, and a sandstorm keeps drifting.
    const bool attacks = drive.ai == AiKind::Hostile || drive.ai == AiKind::Neutral;
    const double range = ai.aggroRange > 0.0 ? ai.aggroRange : kEnemyChaseRange;

    Vec2 desired{0, 0};
    Vec2 facing{0, 0};

    if (ownerAlive) {
        const Transform* ownerTransform = world.tryGet<Transform>(owner);
        if (ownerTransform == nullptr) {
            // An owner with no position is nothing to follow, and a pet left
            // holding last tick's velocity would sail off on its own.
            motion.velocity = Vec2{0, 0};
            return;
        }
        const Vec2 ownerPosition = ownerTransform->position;

        // Passive and sandstorm pets never pop back to the ring: off the
        // owner's screen they are retired, and the petal that summoned them
        // hatches a replacement.
        if (drive.ai == AiKind::Sandstorm || drive.ai == AiKind::Passive) {
            const Vec2 offset = transform.position - ownerPosition;
            if (std::abs(offset.x) > kPetViewHalfWidth || std::abs(offset.y) > kPetViewHalfHeight) {
                commands.destroy(self);
                return;
            }
        }

        if (drive.ai == AiKind::Sandstorm) {
            // Shadows the owner's heading slightly faster than the owner moves,
            // which is precisely why it steadily pulls ahead until it leaves
            // the screen and recycles.
            const Motion* ownerMotion = world.tryGet<Motion>(owner);
            if (speed > 0.0 && ownerMotion != nullptr &&
                ownerMotion->velocity.length() > kOwnerMovingSpeed) {
                desired = ownerMotion->velocity * kSandstormPetSpeedFactor;
            }
        } else if (!terrain.segmentBlocked(transform.position, ownerPosition, transform.realm)) {
            // Follows directly, with no distance limit at all while sight holds.
            const Vec2 toOwner = ownerPosition - transform.position;
            const double gap = toOwner.length();
            if (gap > 0.0 && speed > 0.0) desired = toOwner * (speed / gap);
        } else if (drive.ai == AiKind::Passive) {
            // Sight-blocked passive pet: holds position. The off-screen rule
            // above is what recovers it, not a teleport.
        } else if (teleportPetToOwner(terrain, transform, ownerPosition)) {
            facing = ownerPosition - transform.position;
        }

        if (attacks && speed > 0.0) {
            ai.target = acquirePetPrey(world, terrain, grid, self, transform.position,
                                       transform.realm, ai, true, ownerPosition, range);
            if (ai.target != NULL_ENTITY) {
                if (const Transform* prey = world.tryGet<Transform>(ai.target)) {
                    // Charging the target REPLACES the follow step rather than
                    // blending with it: a pet that averaged the two would circle
                    // between its owner and the mob and reach neither.
                    const Vec2 toPrey = prey->position - transform.position;
                    const double gap = toPrey.length();
                    if (gap > 0.0) desired = toPrey * (speed / gap);
                }
            }
        } else {
            ai.target = NULL_ENTITY;
        }
    } else {
        // Owner dead or gone: wander, straight at the destination.
        ai.target = NULL_ENTITY;
        if (attacks && drive.shoots && speed > 0.0) {
            ai.target = acquirePetPrey(world, terrain, grid, self, transform.position,
                                       transform.realm, ai, false, Vec2{0, 0}, range);
        }
        if (WanderTarget* wander = world.tryGet<WanderTarget>(self)) {
            desired = wanderToPoint(*wander, transform.position, body, speed, nowMillis);
        }
    }

    motion.velocity = desired;
    if (desired.lengthSq() > kDirectionEpsilonSq) facing = desired;

    if (attacks && drive.shoots && speed > 0.0 && ai.target != NULL_ENTITY) {
        if (const Transform* prey = world.tryGet<Transform>(ai.target)) {
            fireVolley(world, self, type, ai, drive, transform.position,
                       (prey->position - transform.position).angle(), motion.velocity, nowMillis,
                       commands);
        }
    }

    transform.angle = steerFacing(transform.angle, facing, drive.hideRotation, drive.reversed, kPi);
}

void MobAiSystem::steerPets(World& world, const Terrain& terrain, const SpatialGrid& grid,
                            double nowMillis, double dt, CommandBuffer& commands) {
    // Deliberately not LOD-gated: a pet belongs to a player, and a player is by
    // definition somewhere someone is looking.
    pets_.collect(stepList_);
    for (const Entity self : stepList_) {
        if (!world.isAlive(self) || world.tryGet<Dead>(self) != nullptr) continue;

        const MobType* type = world.tryGet<MobType>(self);
        const Pet* pet = world.tryGet<Pet>(self);
        if (type == nullptr || pet == nullptr) continue;
        const Drive drive = driveFor(type->configIndex, type->rarity);
        const Entity owner = pet->owner;
        const bool ownerAlive = entityUsable(world, owner);

        // Structural, so it happens before any component pointer is taken: only
        // an ownerless pet needs somewhere to walk to.
        if (!ownerAlive && !world.has<WanderTarget>(self)) world.add<WanderTarget>(self);

        Transform* transform = world.tryGet<Transform>(self);
        Motion* motion = world.tryGet<Motion>(self);
        const Body* body = world.tryGet<Body>(self);
        const MobType* kind = world.tryGet<MobType>(self);
        MobAi* ai = world.tryGet<MobAi>(self);
        if (transform == nullptr || motion == nullptr || body == nullptr || kind == nullptr ||
            ai == nullptr) {
            continue;
        }

        steerPet(world, terrain, grid, self, *transform, *motion, *body, *kind, *ai, owner,
                 ownerAlive, drive, nowMillis, dt, commands);
    }
}

// ---------------------------------------------------------------------------
// Segmented bodies
// ---------------------------------------------------------------------------

void MobAiSystem::repairChains(World& world) {
    followerOf_.clear();
    chainHeads_.clear();

    segments_.each([&](Entity self, BodySegment& segment, Transform&) {
        if (segment.ahead != NULL_ENTITY &&
            (!world.isAlive(segment.ahead) || world.tryGet<Dead>(segment.ahead) != nullptr)) {
            // Cut in half: the piece behind the wound becomes its own animal.
            // Entity handles carry a generation, so a recycled slot reads as
            // dead here rather than splicing an unrelated mob into the chain.
            segment.ahead = NULL_ENTITY;
            ++stats_.promotions;
        }
        // `head` is derived, never trusted: the link is the truth and the flag
        // is a cache of it for everyone downstream.
        segment.head = segment.ahead == NULL_ENTITY;
        if (segment.head) {
            chainHeads_.push_back(self);
            return;
        }
        if (!followerOf_.emplace(segment.ahead, self).second) {
            // Two segments claiming the same leader would make the chain a
            // tree. The first keeps the link; this one starts a chain of its own.
            segment.ahead = NULL_ENTITY;
            segment.head = true;
            chainHeads_.push_back(self);
            ++stats_.promotions;
        }
    });
}

void MobAiSystem::followChains(World& world, const Terrain& terrain,
                               const std::vector<RealmPoint>& activePlayers) {
    visited_.clear();

    for (const Entity head : chainHeads_) {
        const Transform* headTransform = world.tryGet<Transform>(head);
        // The chain is still WALKED when nobody is near -- only the placement
        // is skipped. Walking is what marks the segments visited, and a segment
        // the walk never reached is indistinguishable from one in a cycle.
        //
        // Placement rides the HEAD's stride, so the body moves on exactly the
        // ticks the head did. The reference places every segment every tick and
        // gets the same positions out of it, because a follower re-placed
        // against a head that has not moved lands where it already was.
        const MobType* headType = world.tryGet<MobType>(head);
        const bool active =
            headTransform != nullptr && headType != nullptr &&
            stepsThisTick(head, headTransform->position, headTransform->realm, headType->rarity,
                          activePlayers);

        visited_.insert(head);
        Entity ahead = head;
        for (;;) {
            const auto link = followerOf_.find(ahead);
            if (link == followerOf_.end()) {
                if (BodySegment* tail = world.tryGet<BodySegment>(ahead)) tail->behind = NULL_ENTITY;
                break;
            }
            const Entity self = link->second;
            if (!visited_.insert(self).second) {
                // A cycle in the follower graph. Walking it spins the tick at
                // 100% CPU, which stops the server logging as well as serving,
                // so the link is cut instead of followed.
                if (BodySegment* looped = world.tryGet<BodySegment>(self)) {
                    looped->ahead = NULL_ENTITY;
                    looped->head = true;
                    ++stats_.promotions;
                }
                break;
            }
            if (BodySegment* leader = world.tryGet<BodySegment>(ahead)) leader->behind = self;
            if (active) placeFollower(world, terrain, self, ahead);
            ahead = self;
        }
    }

    // Anything still naming a live leader that no walk reached sits in a cycle
    // with no head at all, so there is no root to have started from. Promote it
    // and next tick has one.
    segments_.each([&](Entity self, BodySegment& segment, Transform&) {
        if (segment.ahead == NULL_ENTITY) return;
        if (visited_.count(self) != 0) return;
        segment.ahead = NULL_ENTITY;
        segment.head = true;
        ++stats_.promotions;
    });
}

void MobAiSystem::placeFollower(World& world, const Terrain& terrain, Entity self, Entity ahead) {
    const Transform* leader = world.tryGet<Transform>(ahead);
    Transform* transform = world.tryGet<Transform>(self);
    const BodySegment* segment = world.tryGet<BodySegment>(self);
    if (leader == nullptr || transform == nullptr || segment == nullptr) return;

    const Body* body = world.tryGet<Body>(self);
    const double radius = body != nullptr ? body->radius : 0.0;
    const double spacing = segment->spacing > 0.0 ? segment->spacing : radius * kSegmentSpacingPerRadius;

    const Vec2 back = transform->position - leader->position;
    // Exactly coincident with its leader there is no trailing direction left to
    // keep, so the chain unfolds behind the leader's facing rather than
    // dividing by zero and placing the segment at NaN.
    const Vec2 direction = back.lengthSq() > kDirectionEpsilonSq
                               ? back.normalized()
                               : Vec2::fromAngle(leader->angle + kPi);

    // A segment lives in its leader's space, whatever it was born into.
    transform->realm = leader->realm;
    const Vec2 placed =
        terrain.resolveCircle(leader->position + direction * spacing, radius, leader->realm);
    transform->position = placed;
    // The follower is carried, not driven. Leaving a velocity on it would have
    // the movement phase integrate it a second time this tick.
    if (Motion* motion = world.tryGet<Motion>(self)) motion->velocity = Vec2{0, 0};

    Drive drive;
    if (const MobType* type = world.tryGet<MobType>(self)) drive = driveFor(type->configIndex, type->rarity);

    // A segment points ALONG its joint, with no turn limit: there is no inertia
    // here to justify a lag, and a segment lagging its own link is exactly what
    // makes a centipede look broken.
    const Vec2 forward = leader->position - placed;
    transform->angle = steerFacing(transform->angle, forward, drive.hideRotation, drive.reversed, kPi);
}

// ---------------------------------------------------------------------------
// Nests
// ---------------------------------------------------------------------------

void MobAiSystem::driveSpawners(World& world, const Terrain& terrain, double nowMillis,
                                CommandBuffer& commands) {
    // Deliberately not LOD-gated. maxAlive bounds the work whatever happens,
    // and a nest that stopped topping up while nobody was looking would be
    // standing empty for the first player who walked in on it.
    nests_.each([&](Entity self, Spawner& nest, Transform& transform, MobType& type) {
        // Pruned first and unconditionally. A nest that went on counting
        // corpses reaches maxAlive once and then never spawns again -- and
        // because nothing else touches this list, nothing else would fix it.
        std::size_t kept = 0;
        for (const Entity child : nest.children) {
            if (world.isAlive(child) && world.tryGet<Dead>(child) == nullptr) {
                nest.children[kept++] = child;
            }
        }
        nest.children.resize(kept);

        if (world.tryGet<Dead>(self) != nullptr) {
            // The nest is dying; its escorts are not. They were spawned into
            // the world and go on living without it, so the list is RELEASED
            // rather than destroyed.
            nest.children.clear();
            return;
        }

        if (!spawnHook_ || nest.maxAlive <= 0) return;
        if (static_cast<int>(nest.children.size()) >= nest.maxAlive) return;
        if (nowMillis < nest.nextSpawnMillis) return;
        nest.nextSpawnMillis = nowMillis + std::max(nest.intervalMillis, kMinSpawnIntervalMillis);

        MobSpawnRequest request;
        request.parent = self;
        request.configIndex = nest.childConfigIndex;
        // Offsets are relative to the parent, so a rare queen fields uncommon
        // soldiers; clamping keeps a hand-edited -9 from wrapping to apex.
        request.rarity = clampRarity(rarityIndex(type.rarity) + nest.rarityOffset);
        const Body* body = world.tryGet<Body>(self);
        const double margin = (body != nullptr ? body->radius : 0.0) + kNestSpawnMargin;
        request.position = terrain.findOpenSpawn(rng_, transform.position, margin, transform.realm);
        request.realm = transform.realm;
        request.lifetimeMillis = nest.childLifetimeMillis;
        ++stats_.spawnRequests;

        commands.defer([this, request](World& deferred) {
            Spawner* nest2 = deferred.tryGet<Spawner>(request.parent);
            if (nest2 == nullptr) return;                                   // died before the flush
            if (deferred.tryGet<Dead>(request.parent) != nullptr) return;
            // Re-checked here because several ticks' commands can be flushed
            // together, and a nest must never overshoot its cap.
            if (static_cast<int>(nest2->children.size()) >= nest2->maxAlive) return;
            const Entity child = spawnHook_(deferred, request);
            if (child != NULL_ENTITY) nest2->children.push_back(child);
        });
    });
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

void MobAiSystem::run(World& world, const Terrain& terrain, const SpatialGrid& grid,
                      const std::vector<RealmPoint>& activePlayers,
                      double nowMillis, double dt, CommandBuffer& commands) {
    stats_ = Stats{};
    // Written as a failed > so a NaN step takes this branch too. A zero step
    // has nothing to integrate and would hand every mob an infinite turn.
    if (!(dt > 0.0) || !std::isfinite(dt)) return;
    ++tick_;

    maxAggroBonus_ = 0.0;
    playerModifiers_.each([&](Entity, PlayerTag&, PlayerModifiers& mods) {
        if (mods.aggroRadiusBonus > maxAggroBonus_) maxAggroBonus_ = mods.aggroRadiusBonus;
    });

    // Watermarks belong to mobs, and mobs are reaped without telling this
    // system, so the table is swept rather than pruned on death.
    if (tick_ % kLedgerSweepTicks == 0) {
        for (auto it = ledgerSeen_.begin(); it != ledgerSeen_.end();) {
            if (world.isAlive(it->first)) ++it;
            else it = ledgerSeen_.erase(it);
        }
    }

    repairChains(world);

    // Snapshotted before anything moves, which is where the reference builds
    // it: a wild mob hunts a pet from where it stood at the top of the tick,
    // not from wherever the pet pass below has since carried it.
    petList_.clear();
    pets_.each([&](Entity self, Pet&, Transform& transform, Motion&, Body&, MobType&, MobAi&) {
        if (!entityUsable(world, self)) return;
        petList_.push_back(Candidate{self, transform.position, 0.0, transform.realm});
    });

    // Collected rather than walked in place: a mob is handed the behaviour
    // components its type calls for on the first tick it thinks, and that --
    // like a volley -- is a structural change, which relocates the very
    // columns an in-place walk would be holding.
    mobs_.collect(stepList_);
    for (const Entity self : stepList_) {
        if (!world.isAlive(self) || world.tryGet<Dead>(self) != nullptr) continue;
        ++stats_.considered;

        // A trailing segment is carried by the chain pass; only the head steers.
        const BodySegment* segment = world.tryGet<BodySegment>(self);
        if (segment != nullptr && !segment->head) continue;

        const Transform* at = world.tryGet<Transform>(self);
        const MobType* type = world.tryGet<MobType>(self);
        if (at == nullptr || type == nullptr) continue;
        if (!stepsThisTick(self, at->position, at->realm, type->rarity, activePlayers)) {
            ++stats_.skipped;
            driftUnwatched(world, self, nowMillis, dt);
            continue;
        }
        ++stats_.thought;

        const Drive drive = driveFor(type->configIndex, type->rarity);
        equipBehaviour(world, self, drive, nowMillis);

        // Re-fetched: equipBehaviour may have moved the mob to another
        // archetype, and every pointer taken before it with the old rows.
        Transform* transform = world.tryGet<Transform>(self);
        Motion* motion = world.tryGet<Motion>(self);
        const Body* body = world.tryGet<Body>(self);
        const MobType* kind = world.tryGet<MobType>(self);
        MobAi* ai = world.tryGet<MobAi>(self);
        if (transform == nullptr || motion == nullptr || body == nullptr || kind == nullptr ||
            ai == nullptr) {
            continue;
        }
        steerMob(world, terrain, grid, self, *transform, *motion, *body, *kind, *ai, drive,
                 nowMillis, dt, commands);
    }

    steerPets(world, terrain, grid, nowMillis, dt, commands);
    followChains(world, terrain, activePlayers);
    driveSpawners(world, terrain, nowMillis, commands);
}

} // namespace flix
