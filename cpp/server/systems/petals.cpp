#include "server/systems/petals.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "shared/game/terrain.h"

namespace flix {
namespace {

/// Cluster spacing as a multiple of a petal's own radius. The reference states
/// it as `size * 20 * 0.5`, which is exactly the 10 x size a petal's body
/// already is, so the grains sit one radius out from the slot centre -- and
/// gardn authors the same figure as a flat `clump_radius` of 10.
constexpr double kClumpSpacing = 1.0;
/// A pollen puff is stated as 12 x the petal's size across, and the field takes
/// half of that. Stated against `size` rather than against the petal's radius:
/// dividing the radius back out is what hid the halved petal scale.
constexpr double kPollenRadiusPerSize = 6.0;
/// How fast the ring's extension multiplier travels, per second.
///
/// The reference does no server-side smoothing at all: the BROWSER ramps its
/// petalExtension linearly at this rate between 0.7 and 2.0 and ships the
/// finished multiplier in the input frame, which the server consumes raw. So
/// the ramp lives here, on the multiplier, and the radius follows from it --
/// easing the radius instead turned a tap-attack that reaches full extension
/// in a twelfth of a second into a half-second swell.
constexpr double kPetalExtensionRampPerSecond = 12.0;

constexpr double kBurstShieldLifetimeMillis = 10000.0;
constexpr double kWebThrowDistance = 620.0;
constexpr double kWebLifetimeSeconds = 10.0;
constexpr double kPollenLifetimeSeconds = 5.0;
constexpr double kPollenHitIntervalMillis = 500.0;

/// Bubble dash. The pop is a displacement, not a velocity, so it is walked out
/// in steps no longer than the flower's own radius: one jump of up to 384 units
/// resolved only at its destination lands on whichever side of a wall the
/// minimum-translation push happens to prefer, which is how a high-rarity
/// bubble tunnelled out of sealed rooms.
constexpr double kBubbleBounceDamping = 0.7;
constexpr int kBubbleDashMaxSteps = 32;

/// An apex egg does not hatch an apex pet. The reference substitutes three
/// unique ones, which is the top of the ladder a pet can actually reach.
constexpr int kApexPetCount = 3;

/// Live summons one player may hold, counting every slot's squad together.
///
/// Far above any ordinary loadout: it is a backstop against deliberate
/// stacking, which is what the reference records it as -- a full row of eggs
/// hatching multi-entity pets put hundreds of mobs in the world per player and
/// stalled the tick loop until the proxy answered 502.
constexpr int kMaxPetEntitiesPerPlayer = 50;

/// The flower petal cracks on the mob it touches: nineteen breaks in twenty
/// open onto a squad of glitch flowers, and the twentieth takes the flower
/// that was carrying it.
constexpr double kFlowerCorruptChance = 0.05;
constexpr int kFlowerPetCount = 3;
constexpr const char* kFlowerPetMobId = "glitch_flower";

// -- scripted behaviours -----------------------------------------------------
//
// A handful of petals do something beyond orbiting and hurting what they touch:
// they strike, detonate, heal on a threshold or hand out a shield. The
// reference keeps them in one table rather than in the stat block, because what
// they do is code and not a number, and because each of them fires at a
// different moment in a petal's life.

/// A strike reaches this far and, when the petal declares no damage of its own,
/// lands this much. Deliberately NOT scaled by the Damage talent: the reference
/// hands the strike the raw petal stat, unlike ring contact.
constexpr double kLightningRadius = 1000.0;
constexpr double kLightningFallbackDamage = 25.0;

/// A lightning cutter is authored as a one-millisecond petal that breaks itself
/// on every strike and is instantly restored, so what actually paces it is the
/// per-PLAYER limiter: 500 ms apart, at most twice a second. The two rules
/// collapse into one gap, because two strikes 500 ms apart are never three in
/// a second.
constexpr double kLightningCutterIntervalMillis = 500.0;

/// An explosion reaches three times the petal's DRAWN size, and the reference
/// arrives at that by multiplying by 40 twice -- once turning the size stat
/// into pixels and once again inside the blast -- so the radius really is this
/// large. It is why the throttle below exists.
constexpr double kExplosionRadiusPerSize = 4800.0;
/// A flat positional shove, undivided by the victim's mass: the reference
/// writes it straight onto the mob rather than routing it through the
/// mass-scaled push a petal's own knockback stat takes.
constexpr double kExplosionKnockback = 20.0;
/// One explosion per this window for the WHOLE server. At a 33 ms tick that is
/// one detonation per tick however many petals went off together.
constexpr double kExplosionThrottleMillis = 20.0;

/// The shield petal grants a flat amount for a flat window, at spawn, on break
/// and on its own interval. Neither number is rarity-scaled.
constexpr double kBehaviourShieldAmount = 50.0;
constexpr double kBehaviourShieldMillis = 10000.0;
constexpr double kBehaviourShieldIntervalMillis = 10000.0;
constexpr double kBehaviourHealIntervalMillis = 2000.0;
constexpr double kBehaviourExplodeIntervalMillis = 3000.0;

/// Starfish compares ABSOLUTE health, not a fraction: it is an emergency heal
/// for a low-level flower and stops mattering once the bar outgrows it.
constexpr double kStarfishHealthThreshold = 75.0;

/// A scripted heal is a steeper curve than a burst petal's. `stats.heal`
/// already carries its rarity; a script's literal does not, so the reference
/// multiplies it by sqrt(3) per tier and by a flat 3 on top of the talent.
constexpr double kBehaviourHealScale = 3.0;

/// How close a yggdrasil has to pass to a corpse to raise it.
constexpr double kYggdrasilRevivalRange = 80.0;

enum class PetalBehaviourKind : std::uint8_t {
    None,
    Lightning,
    LightningCutter,
    BloodLeaf,
    Starfish,
    Bomb,
    Shield,
    Healing,
    TestExplosive,
};

struct PetalBehaviour {
    PetalBehaviourKind kind = PetalBehaviourKind::None;
    /// The instance runs nothing at spawn and parks until it touches a mob.
    bool waitsForCollision = false;
    /// That contact also destroys it, which is what stops the break effect
    /// from running a second time on the way out.
    bool breaksSelfOnCollision = false;
    /// Period of the repeating effect, or 0 for a petal that has none.
    double intervalMillis = 0;
};

/// Petal id -> what it does. `explosive` is `bomb` under an admin-only name and
/// `test_explosive` is the repeating version of the same blast.
PetalBehaviour behaviourOf(const std::string& id) {
    if (id == "lightning") return {PetalBehaviourKind::Lightning, true, false, 0.0};
    if (id == "lightning_cutter") {
        return {PetalBehaviourKind::LightningCutter, false, false, net::kTickMillis};
    }
    if (id == "blood_leaf") return {PetalBehaviourKind::BloodLeaf, false, false, 0.0};
    if (id == "starfish") return {PetalBehaviourKind::Starfish, false, false, 0.0};
    if (id == "bomb" || id == "explosive") return {PetalBehaviourKind::Bomb, true, true, 0.0};
    if (id == "shield") {
        return {PetalBehaviourKind::Shield, false, false, kBehaviourShieldIntervalMillis};
    }
    if (id == "healing") {
        return {PetalBehaviourKind::Healing, false, false, kBehaviourHealIntervalMillis};
    }
    if (id == "test_explosive") {
        return {PetalBehaviourKind::TestExplosive, false, false, kBehaviourExplodeIntervalMillis};
    }
    return {};
}

/// Summoned-only stat multipliers, applied on top of rarity scaling. The
/// digger's wild line is tuned for a mob that crawls out of a dying ant hole;
/// as a permanent escort it outclasses every other egg at the same tier.
double petStatMultiplier(const std::string& mobId) {
    return mobId == "digger" ? 0.5 : 1.0;
}

/// A cooldown of zero is a config that forgot to say how long, not a petal that
/// returns the instant it breaks.
double reloadMillisFor(const PetalStats& stats) {
    return stats.reloadMillis > 0.0 ? stats.reloadMillis : kDefaultPetalReloadMillis;
}

/// `range` is a multiple of the ring radius. The JSON leaves it out for every
/// ordinary petal and writes 0 for the ones that sit on the flower -- and those
/// are all noPhysics, which is what placement actually keys off.
double rangeMultiplier(const PetalConfig& config) {
    return config.range > 0.0 ? config.range : 1.0;
}

bool hasTimedAction(const PetalConfig& config, const PetalStats& stats) {
    return config.projectile.present || stats.heal > 0.0 || stats.shield > 0.0 ||
           config.radiation.present || config.petMobIndex != kInvalidIndex ||
           behaviourOf(config.id).intervalMillis > 0.0;
}

bool hasNonProjectileAction(const PetalConfig& config, const PetalStats& stats) {
    return stats.heal > 0.0 || stats.shield > 0.0 || config.radiation.present ||
           config.petMobIndex != kInvalidIndex || behaviourOf(config.id).intervalMillis > 0.0;
}

/// The gap between two of a petal's actions. A petal may declare more than one
/// kind (dahlia both heals and clumps); the slowest wins, so a petal never acts
/// more often than its longest declared gap. The tick floor keeps a config with
/// a one-millisecond cooldown from becoming a per-tick emitter.
double actionIntervalMillis(const PetalConfig& config, const PetalStats& stats) {
    double interval = 0;
    if (stats.heal > 0.0 || stats.shield > 0.0) {
        interval = std::max(interval, stats.healChargeMillis);
    }
    if (config.radiation.present) interval = std::max(interval, config.radiation.intervalMillis);
    if (config.petMobIndex != kInvalidIndex) interval = std::max(interval, stats.reloadMillis);
    interval = std::max(interval, behaviourOf(config.id).intervalMillis);
    return std::max(interval, net::kTickMillis);
}

/// A petal the reaper has not got to yet is already gone as far as the ring is
/// concerned, so its remaining health reads as none.
double instanceHealth(World& world, Entity petal) {
    if (world.has<Dead>(petal)) return 0.0;
    const Health* health = world.tryGet<Health>(petal);
    return health ? health->current : 0.0;
}

/// How much of a slot is still standing, for the loadout bar's tile fill.
///
/// Two different quantities wear the same name here. A shared pool is ONE
/// health bar the whole cluster mirrors, so it is read straight off the pool; a
/// clump is `count` separate ones, and a grain that is off the field counts as
/// zero rather than as absent -- three grains of four is a tile three quarters
/// full, which is also what makes a partly broken clump legible at a glance.
double slotHealthFraction(World& world, const PetalSlotState::Slot& state,
                          const PetalStats& stats, int count,
                          const std::vector<Entity>& live) {
    if (!stats.breakable || count <= 0) return 1.0;
    if (!state.independent) {
        if (state.poolMax <= 0.0) return 1.0;
        return clamp(state.poolHealth / state.poolMax, 0.0, 1.0);
    }
    if (stats.health <= 0.0) return 1.0;
    double standing = 0;
    for (const Entity petal : live) standing += std::max(0.0, instanceHealth(world, petal));
    return clamp(standing / (stats.health * count), 0.0, 1.0);
}

void healPlayer(World& world, Entity player, double amount) {
    if (amount <= 0.0) return;
    Health* health = world.tryGet<Health>(player);
    // Healing a corpse is a respawn, and that decision is not a petal's.
    if (!health || !health->alive()) return;
    health->current = std::min(health->max, health->current + amount);
}

/// Walks a bubble pop out over the ground instead of teleporting the flower to
/// its far end.
///
/// `resolveCircle` is a minimum-translation push, so it ejects a centre out of
/// whichever face of a blocking tile is nearest -- resolve only the endpoint of
/// a 384-unit jump and anything past the tile's midline comes out on the FAR
/// side, which is how a legendary-or-better bubble walked through the wall
/// bands. Stepping at most one body radius at a time keeps every intermediate
/// position on the near side, and a step the terrain clipped reflects the
/// remaining impulse across the blocked axis so the flower bounces off the wall
/// rather than stopping dead against it.
void dashPlayer(Transform& owner, Vec2 velocity, double distance, double radius,
                const Terrain* terrain) {
    double remaining = distance;
    for (int step = 0; step < kBubbleDashMaxSteps && remaining > 0.5; ++step) {
        const double stepLength = std::min(radius, remaining);
        // A zero-length velocity divides by one, as in the reference: the step
        // is then empty and the loop simply spends its budget.
        const double speed = velocity.length();
        const Vec2 attempted = velocity * (stepLength / (speed > 0.0 ? speed : 1.0));

        const Vec2 trial = owner.position + attempted;
        const Vec2 resolved = terrain ? terrain->resolveCircle(trial, radius) : trial;
        const Vec2 applied = resolved - owner.position;
        owner.position = resolved;
        remaining -= stepLength;

        // Which axis the wall took is inferred from which component of the step
        // the resolver ate; half of it is the threshold, so a grazing slide
        // along a surface is not read as a head-on hit.
        const Vec2 clipped = attempted - applied;
        const bool blockedX = std::abs(clipped.x) > std::abs(attempted.x) * 0.5;
        const bool blockedY = std::abs(clipped.y) > std::abs(attempted.y) * 0.5;
        if (!blockedX && !blockedY) continue;
        if (blockedX) velocity.x = -velocity.x * kBubbleBounceDamping;
        if (blockedY) velocity.y = -velocity.y * kBubbleBounceDamping;
        // Wedged in a corner: reflecting both axes would only bounce the flower
        // between the two faces for the rest of its budget.
        if (blockedX && blockedY) break;
    }
}

bool playerIsDown(World& world, Entity player) {
    if (world.has<Dead>(player)) return true;
    const Health* health = world.tryGet<Health>(player);
    return health != nullptr && !health->alive();
}

} // namespace

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void PetalSystem::run(World& world, const ContentRegistry& registry, double nowMillis, double dt,
                      CommandBuffer& commands, const Terrain* terrain) {
    bindTo(world);
    // A snapshot of handles, not a live query: everything this system does --
    // spawning a petal, breaking one, firing a volley -- is structural, and
    // none of it may happen while a query holds column pointers. Taking the
    // snapshot first is what makes the direct create/destroy calls below legal,
    // and is why the phase's CommandBuffer goes unused here.
    (void)commands;
    players_->collect(playerList_);
    // Once for the tick, before any petal is placed: every player's ring reads
    // the same set of mob positions, as the reference's does.
    rebuildAttractionGrid(world);

    for (const Entity player : playerList_) {
        if (!world.isAlive(player)) continue;
        // Per-player, and never carried between two of them: a scripted effect
        // queued for one flower must not fire out of the next one's ring.
        pendingSpawns_.clear();
        pendingBreaks_.clear();
        if (playerIsDown(world, player)) {
            clearRing(world, player);
            continue;
        }
        // Before the slot pass, because retiring a pet puts the petal that
        // hatched it back on cooldown and the slot pass is what serves that.
        retireDistantPets(world, registry, player, nowMillis);
        reconcileSlots(world, registry, player, nowMillis);
        const Aggregate aggregate = recomputeModifiers(world, registry, player);
        applyPassiveHeal(world, player, aggregate, dt);
        applyRaindropAura(world, registry, player, world.get<PetalSlotState>(player), aggregate,
                          nowMillis);
        updateRing(world, player, aggregate, dt);
        placePetals(world, registry, player, aggregate, nowMillis, dt, terrain);
        runActions(world, registry, player, nowMillis, terrain);
    }
}

void PetalSystem::foldModifiers(World& world, const ContentRegistry& registry) {
    bindTo(world);
    players_->collect(playerList_);
    for (const Entity player : playerList_) {
        // A corpse keeps the modifiers it died with: run() clears its ring
        // rather than recomputing, and respawn re-folds from the live loadout.
        if (!world.isAlive(player) || playerIsDown(world, player)) continue;
        recomputeModifiers(world, registry, player);
    }
}

void PetalSystem::bindTo(World& world) {
    // A Query caches archetype indices, and those belong to one world. A test
    // that reuses the system across worlds must not inherit the old cache.
    if (bound_ == &world && players_) return;
    bound_ = &world;
    players_ = std::make_unique<Query<PlayerTag, Transform, Loadout, PetalRing>>(world);
    mobs_ = std::make_unique<Query<MobTag, Transform, Body>>(world);
    // Pets are excluded at the source rather than at each call site: no strike,
    // blast or contact trigger in the reference ever sees a summon, because the
    // grid it queries does not file them.
    mobs_->without<Dead, Pet>();
}

void PetalSystem::rebuildAttractionGrid(World& world) {
    bindTo(world);
    attractionGrid_.clear();
    // Filed under the mob's own radius, so a boss whose edge reaches a petal
    // is a candidate for it even though its centre is cells away.
    mobs_->each([&](Entity mob, MobTag&, Transform& transform, Body& body) {
        attractionGrid_.insert(mob, transform.position, body.radius);
    });
}

bool PetalSystem::findAttractionTarget(World& world, Vec2 at, double radius,
                                       AttractionTarget& out) {
    if (!(radius > 0.0)) return false;
    attractionGrid_.query(at, radius, attractionCandidates_);

    Entity closest = NULL_ENTITY;
    double closestDistanceSq = 0;
    Vec2 closestPosition;
    double closestRadius = 0;
    for (const Entity mob : attractionCandidates_) {
        // The grid is last tick's; a mob an earlier petal killed is still in
        // it, and the reference skips exactly that case.
        if (!world.isAlive(mob) || world.has<Dead>(mob)) continue;
        const Transform* transform = world.tryGet<Transform>(mob);
        const Body* body = world.tryGet<Body>(mob);
        if (transform == nullptr || body == nullptr) continue;
        // Eligibility reaches to the mob's scaled BODY, but the winner is the
        // one whose CENTRE is nearest -- both as the reference has it.
        const double reach = radius + body->radius;
        const double gap = distanceSq(transform->position, at);
        if (gap > reach * reach) continue;
        if (closest != NULL_ENTITY && gap >= closestDistanceSq) continue;
        closest = mob;
        closestDistanceSq = gap;
        closestPosition = transform->position;
        closestRadius = body->radius;
    }
    if (closest == NULL_ENTITY) return false;

    // The BODY's radius, which is the circle the collision test uses, and not
    // the tier radius out of the stat table.
    //
    // The two differ by the per-spawn size jitter, and the reference projects
    // onto the tier figure -- which is a capture that cannot land a hit on any
    // mob that rolled small. kMobOrbitRadiusScale pins the petal 0.85 tier
    // radii from the centre while contact needs it inside `body + petal`, so a
    // 0.55 roll on a big mob parks the ring in the gap: the petal visibly whips
    // around its victim, deals nothing, and -- since a petal pays for its
    // health only when it lands a hit -- never wears out either. Measuring off
    // the body makes 0.85 mean 0.85 of the edge that is actually there, so the
    // projection is inside the hitbox for every roll.
    out.mob = closest;
    out.position = closestPosition;
    out.radius = closestRadius > 0.0 ? closestRadius : kMobBaseRadius;
    return true;
}

// ---------------------------------------------------------------------------
// Slots: spawning, breaking, reloading
// ---------------------------------------------------------------------------

void PetalSystem::clearRing(World& world, Entity player) {
    // The field only. A corpse's LOADOUT is left exactly as it was -- which
    // petal is in which slot, what health it had, how much of its reload is
    // still to run -- because that is all a downed flower is in the reference:
    // a player the ring pass steps over, not one whose slots are reset. It
    // matters because a corpse can be raised: a yggdrasil hands the flower back
    // the ring it went down with, rather than eight slots' worth of fresh
    // reload. A respawn pays those reloads regardless, because it arrives on a
    // brand new entity whose slot state has never seen a petal.
    if (Loadout* loadout = world.tryGet<Loadout>(player)) {
        for (const Entity petal : loadout->spawned) {
            if (world.isAlive(petal)) world.destroy(petal);
        }
        loadout->spawned.clear();
    }
    if (PetalSlotState* state = world.tryGet<PetalSlotState>(player)) {
        for (PetalSlotState::Slot& slot : state->slots) {
            recallPets(world, slot);
            // Not "these petals were just destroyed": an empty ring on the tick
            // after a revive would otherwise charge every missing instance to
            // the shared pool and break the whole loadout at once.
            slot.populated = false;
        }
        // The flower may come back somewhere else entirely, and the ring is
        // laid out from where it was LAST tick: keeping the corpse's centre
        // would fly the ring in from the place it went down.
        state->ringCentreValid = false;
    }
}

void PetalSystem::reconcileSlots(World& world, const ContentRegistry& registry, Entity player,
                                 double nowMillis) {
    if (!world.has<Loadout>(player)) return;
    // ensure() can relocate the player, so it happens before any pointer into
    // its columns is taken. After this line nothing moves the player between
    // archetypes: a petal carries PetalTag and a player carries PlayerTag, so
    // no petal can ever land in the player's archetype and spawning or
    // destroying one cannot touch the columns held below.
    PetalSlotState& state = world.ensure<PetalSlotState>(player);
    Loadout& loadout = world.get<Loadout>(player);
    const PlayerSkillTree* tree = world.tryGet<PlayerSkillTree>(player);
    const double petalHealthScale =
        tree ? tree->skills.statScale(SkillId::PetalHealth) : 1.0;

    // Bucket the live petals by slot, dropping the handles the world has
    // already reaped and the ones combat killed this tick. Both count as
    // instances lost, which the health fold below charges to the pool.
    for (auto& bucket : bySlot_) bucket.clear();
    std::size_t kept = 0;
    for (const Entity petal : loadout.spawned) {
        const PetalInstance* instance = world.tryGet<PetalInstance>(petal);
        if (!instance) continue;
        // Keep a just-killed independent instance in its slot bucket for one
        // fold so its sub-index can start the correct per-instance reload.
        // It is omitted from spawned immediately, because the ring/combat must
        // already treat it as gone.
        if (instance->slot < kLoadoutSlots) bySlot_[instance->slot].push_back(petal);
        if (world.has<Dead>(petal)) continue;
        loadout.spawned[kept++] = petal;
    }
    loadout.spawned.resize(kept);

    // Only the primary row is equipped. Slots 10..19 are the storage the bar's
    // second row shows: they hold a petal, they never spawn one.
    for (int i = 0; i < kLoadoutActiveSlots; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const auto slotId = static_cast<std::uint8_t>(i);
        LoadoutSlot& slot = loadout.slots[index];
        PetalSlotState::Slot& slotState = state.slots[index];
        std::vector<Entity>& live = bySlot_[index];

        if (slot.empty()) {
            if (slotState.configIndex != kNoPetal) {
                destroySlotPetals(world, loadout, slotId);
                recallPets(world, slotState);
                slotState = PetalSlotState::Slot{};
                slot.broken = false;
                slot.reloadReadyAtMillis = 0;
                live.clear();
            }
            continue;
        }

        const PetalConfig& config = registry.petal(slot.configIndex);
        PetalStats stats = registry.petalStats(slot.configIndex, slot.rarity);
        // The Petal Health talent is folded in HERE, before the pool is sized,
        // rather than at each of the four places a pool figure is written. A
        // petal's health reaches the field through poolMax, poolHealth,
        // syncedHealth and spawnHealth; scaling one of them and not the rest is
        // how a talent turns into a petal that reloads to more than its own max.
        //
        // Rounded, as the reference rounds it at every one of the three places
        // it recomputes a petal's max. Left fractional, a pool that lands just
        // above an integer survives the exact run of hits that empties it in
        // production: a rare stinger under the legendary talent is 25 there and
        // 25.2 here, and blocks a sixth 5-damage hit the reference lets kill it.
        stats.health = std::round(stats.health * petalHealthScale);
        const int count = std::max(0, stats.count);
        const double reload = reloadMillisFor(stats);

        if (slotState.configIndex != slot.configIndex || slotState.rarity != slot.rarity) {
            destroySlotPetals(world, loadout, slotId);
            recallPets(world, slotState);
            live.clear();
            slotState = PetalSlotState::Slot{};
            slotState.configIndex = slot.configIndex;
            slotState.rarity = slot.rarity;
            // `clumped` alone is enough: a four-grain clump of sand is four
            // petals that break and reload one at a time, not one health bar
            // shared four ways. `independentHealth` is the second, rarer way
            // the same thing is declared (light, whose grains orbit apart).
            slotState.independent = (config.independentHealth || config.clumped) && count > 1;
            slotState.poolMax = stats.health;
            slotState.poolHealth = stats.health;
            slotState.syncedHealth = stats.health;
            slotState.instanceReadyAtMillis.assign(static_cast<std::size_t>(count), 0.0);
            // A petal serves its full reload BEFORE it appears, every time the
            // slot's contents change. The reference puts a newly equipped petal
            // on cooldown at the moment it is equipped, and rebuilds a saved
            // loadout the same way -- so a ring is never handed back whole: it
            // fills in one slot at a time, on login as well as on a swap. The
            // free reload this used to grant was also a way to dodge one, by
            // dragging a spare into the slot of a petal that had just broken.
            //
            // Not a clump, though. The reference asks a per-instance cooldown
            // array that it has just built EMPTY, so the slot-level flag it
            // sets is never consulted for a petal whose instances are
            // independent, and sand and dahlia really do arrive ready.
            slot.broken = !slotState.independent;
            slot.reloadReadyAtMillis = slot.broken ? nowMillis + reload : 0.0;
        }

        // Pure modifiers and emitters have no health pool at all, so neither
        // break path below applies to them. An equip reload still has to run
        // out, though -- the reference puts the slot on cooldown whether or not
        // the petal has health -- so the flag is left for the expiry to clear.
        const bool hasPool = stats.breakable && count > 0;
        if (hasPool && slotState.independent) {
            // Each instance owns its health: the ones at zero go, alone, and
            // start their own timer. The slot as a whole only reads as broken
            // when nothing of it is left on the field.
            std::size_t survivors = 0;
            for (const Entity petal : live) {
                if (instanceHealth(world, petal) > 0.0) {
                    live[survivors++] = petal;
                    continue;
                }
                const PetalInstance* instance = world.tryGet<PetalInstance>(petal);
                if (instance && instance->subIndex < slotState.instanceReadyAtMillis.size()) {
                    slotState.instanceReadyAtMillis[instance->subIndex] = nowMillis + reload;
                }
                queueBreakBehaviour(world, registry, petal);
                world.destroy(petal);
            }
            live.resize(survivors);
        } else if (hasPool && !slot.broken && slotState.populated) {
            // Fold the cluster's damage back into the shared pool. Every
            // instance was left holding syncedHealth at the end of the last
            // tick, so whatever is missing from one now is damage combat dealt.
            double taken = 0;
            for (const Entity petal : live) {
                taken += std::max(0.0, slotState.syncedHealth - instanceHealth(world, petal));
            }
            const int missing = count - static_cast<int>(live.size());
            if (missing > 0) taken += slotState.syncedHealth * missing;
            slotState.poolHealth -= taken;

            if (slotState.poolHealth <= 0.0) {
                slotState.poolHealth = 0;
                // Once for the SLOT, not once per grain: the reference puts the
                // whole slot on cooldown the moment its first instance breaks,
                // and every later instance in the same pass reads as already
                // reloading and runs nothing.
                if (!live.empty()) queueBreakBehaviour(world, registry, live.front());
                for (const Entity petal : live) {
                    if (world.isAlive(petal) && world.has<Dead>(petal)) world.destroy(petal);
                }
                destroySlotPetals(world, loadout, slotId);
                live.clear();
                // The summons stay. An egg can break in combat while its pet is
                // still fighting, and the reference recalls a squad only when
                // the slot's petal actually CHANGES -- its reload then finds
                // the pet still out and hatches nothing. Recalling here deleted
                // a player's pet the instant something killed the egg.
                slotState.populated = false;
                slot.broken = true;
                slot.reloadReadyAtMillis = nowMillis + reload;
            }
        }

        if (slot.broken && !slotState.independent && nowMillis >= slot.reloadReadyAtMillis) {
            slot.broken = false;
            slot.reloadReadyAtMillis = 0;
            slotState.poolHealth = slotState.poolMax;
            slotState.syncedHealth = slotState.poolMax;
        }

        // Which cluster members are on the field. count is capped at 64 by the
        // config loader, so one word is enough and the loop below stays linear.
        std::uint64_t present = 0;
        for (const Entity petal : live) {
            const PetalInstance* instance = world.tryGet<PetalInstance>(petal);
            if (instance && instance->subIndex < 64) present |= 1ull << instance->subIndex;
        }

        const double spawnHealth = stats.breakable
                                       ? (slotState.independent ? stats.health : slotState.poolHealth)
                                       : 0.0;
        for (int k = 0; k < count && k < 64; ++k) {
            if ((present & (1ull << k)) != 0) continue;
            // Gated on the cooldown, not on whether the petal can break: an
            // unbreakable petal cannot have been destroyed, but it can have
            // been equipped a moment ago and still owe its reload.
            if (slotState.independent) {
                const double ready = slotState.instanceReadyAtMillis[static_cast<std::size_t>(k)];
                if (slotState.populated && nowMillis < ready) continue;
            } else if (slot.broken) {
                continue;
            }
            const Entity petal = spawnPetal(world, player, loadout, slotId,
                                            static_cast<std::uint8_t>(k),
                                            static_cast<std::uint8_t>(count), config, stats,
                                            slot.configIndex, slot.rarity, spawnHealth, nowMillis);
            if (petal == NULL_ENTITY) continue;
            live.push_back(petal);
            present |= 1ull << k;
            if (slotState.independent) {
                slotState.instanceReadyAtMillis[static_cast<std::size_t>(k)] = 0.0;
            }
        }
        // Sticky: once a slot has actually put petals on the field, an empty
        // ring means "they are all down", never "they have not spawned yet".
        // Only a rebuild or a shared-pool break clears it.
        if (!live.empty()) slotState.populated = true;

        if (stats.breakable && count > 0 && slotState.independent) {
            // Derived, not stored: the slot is on cooldown exactly while none
            // of its instances are out, and it comes back with the first one.
            int alive = 0;
            double earliest = 0;
            for (int k = 0; k < count && k < 64; ++k) {
                if ((present & (1ull << k)) != 0) {
                    ++alive;
                    continue;
                }
                const double ready = slotState.instanceReadyAtMillis[static_cast<std::size_t>(k)];
                if (earliest == 0 || ready < earliest) earliest = ready;
            }
            slot.broken = alive == 0;
            slot.reloadReadyAtMillis = slot.broken ? earliest : 0;
        }

        // Mirror the pool onto every instance so combat can damage whichever
        // grain it reaches and the fold above still adds up to one health bar.
        if (stats.breakable && !slotState.independent && !slot.broken) {
            slotState.syncedHealth = slotState.poolHealth;
            for (const Entity petal : live) {
                if (Health* health = world.tryGet<Health>(petal)) {
                    health->max = slotState.poolMax;
                    health->current = slotState.poolHealth;
                }
            }
        }

        // Last, so it reads the pool this tick's damage has already been folded
        // into and the instances this tick's respawns have already put back.
        slotState.healthFraction = slotHealthFraction(world, slotState, stats, count, live);
    }

    // Instances destroyed above are still named by the loadout's list. Dropping
    // them here keeps the invariant that everything in `spawned` is a live
    // petal, which placement and the action pass both rely on this same tick.
    kept = 0;
    for (const Entity petal : loadout.spawned) {
        if (!world.isAlive(petal)) continue;
        loadout.spawned[kept++] = petal;
    }
    loadout.spawned.resize(kept);
}

Entity PetalSystem::spawnPetal(World& world, Entity player, Loadout& loadout, std::uint8_t slot,
                               std::uint8_t subIndex, std::uint8_t subCount,
                               const PetalConfig& config, const PetalStats& stats,
                               std::uint16_t configIndex, Rarity rarity, double health,
                               double nowMillis) {
    const Transform* ownerTransform = world.tryGet<Transform>(player);
    if (!ownerTransform) return NULL_ENTITY;
    // The ring's centre, not the flower's live position: placement this same
    // tick springs from the previous committed centre, and starting the petal
    // anywhere else would give it a tick of velocity it never earned.
    const PetalSlotState* ringState = world.tryGet<PetalSlotState>(player);
    const Vec2 origin = ringState != nullptr && ringState->ringCentreValid
                            ? ringState->ringCentre
                            : ownerTransform->position;
    const Faction* ownerFaction = world.tryGet<Faction>(player);
    const Faction faction = ownerFaction ? *ownerFaction : Faction{Team::Players, false};

    const Entity petal = world.create();
    world.add<PetalTag>(petal);
    // Born ON the flower and flown out over the spawn glide below, which is
    // what makes a reload read as the petal coming back out rather than
    // reappearing on the ring.
    world.add<Transform>(petal, Transform{origin, 0.0});
    // Deliberately no Motion and no Knockback: the ring dictates a petal's
    // position every tick, so integrating or pushing it would be overwritten,
    // and the movement system would be doing work it cannot keep.
    if (!config.noPhysics) {
        world.add<Body>(petal, Body{stats.radius, 1.0});
        world.add<ContactDamage>(petal,
                                 ContactDamage{stats.damage,
                                               std::max(0.0, stats.damageIntervalMillis)});
        world.add<HitCooldowns>(petal);
    }
    if (health > 0.0) world.add<Health>(petal, Health{health, stats.health, 0.0, 0.0});
    world.add<Faction>(petal, faction);

    PetalInstance instance;
    instance.owner = player;
    instance.configIndex = configIndex;
    instance.rarity = rarity;
    instance.slot = slot;
    instance.subIndex = subIndex;
    instance.subCount = std::max<std::uint8_t>(1, subCount);
    // A petal that has just arrived waits out a full interval before acting,
    // so a broken projectile petal cannot be reloaded into an instant volley.
    // TypeScript's absent `lastShotTime` reads as zero against an epoch clock,
    // so a newly equipped projectile is ready immediately when attack extends
    // the ring. Its cooldown begins only after an actual shot.
    instance.nextProjectileMillis = config.projectile.present ? nowMillis : 0.0;
    instance.spawnedAtMillis = nowMillis;
    // The fly-out. A first-order approach for this window instead of the
    // spring, so a petal that starts on top of the flower does not get
    // slingshotted past its orbit point on the way out.
    instance.glideUntilMillis = nowMillis + kPetalSpawnGlideMillis;
    instance.nextActionMillis = hasNonProjectileAction(config, stats)
                                    ? nowMillis + actionIntervalMillis(config, stats)
                                    : 0.0;
    world.add<PetalInstance>(petal, instance);

    world.add<PetalEffect>(petal, PetalEffect{stats.poisonPerSecond, stats.poisonDurationMillis,
                                              stats.knockback, stats.slowFactor,
                                              stats.slowDurationMillis});

    Replicated replicated;
    replicated.kind = net::EntityKind::Petal;
    replicated.typeIndex = configIndex;
    replicated.rarity = rarity;
    world.add<Replicated>(petal, replicated);
    assignNetId(world, petal);

    // Deferred rather than run here: a scripted spawn effect may add a
    // component to the FLOWER, and the slot pass that called this is holding
    // the flower's columns open. The action pass drains it, from the flower's
    // own position -- which is where the reference builds the spawn context.
    if (behaviourOf(config.id).kind != PetalBehaviourKind::None) pendingSpawns_.push_back(petal);

    loadout.spawned.push_back(petal);
    return petal;
}

void PetalSystem::destroySlotPetals(World& world, Loadout& loadout, std::uint8_t slot) {
    std::size_t kept = 0;
    for (const Entity petal : loadout.spawned) {
        const PetalInstance* instance = world.tryGet<PetalInstance>(petal);
        if (instance && instance->slot == slot) {
            world.destroy(petal);
            continue;
        }
        loadout.spawned[kept++] = petal;
    }
    loadout.spawned.resize(kept);
}

void PetalSystem::recallPets(World& world, PetalSlotState::Slot& state) {
    // Destroyed, not marked Dead: a recalled summon has not been killed, so it
    // must not raise a death event, award XP or drop anything.
    for (const Entity pet : state.pets) {
        if (world.isAlive(pet)) world.destroy(pet);
    }
    state.pets.clear();
}

void PetalSystem::assignNetId(World& world, Entity e) {
    if (allocateNetId) world.add<NetId>(e, NetId{allocateNetId()});
}

// ---------------------------------------------------------------------------
// Modifiers and ring geometry
// ---------------------------------------------------------------------------

PetalSystem::Aggregate PetalSystem::recomputeModifiers(World& world,
                                                       const ContentRegistry& registry,
                                                       Entity player) {
    Aggregate aggregate;
    aggregate.modifiers.magnetism = kBaseMagnetism;
    aggregate.modifiers.luck = 1.0;
    // Every living flower regenerates one health per second even with an empty
    // loadout. Petal healing is added below and talent-scaled separately.
    aggregate.modifiers.passiveHealPerSecond = 1.0;
    std::uint8_t equipFlags = EquipNone;

    if (const Loadout* loadout = world.tryGet<Loadout>(player)) {
        // Storage grants nothing: the browser breaks out of this same sum at
        // PRIMARY_LOADOUT_SLOTS, so a stashed clover is not a worn one.
        for (int i = 0; i < kLoadoutActiveSlots; ++i) {
            const LoadoutSlot& slot = loadout->slots[static_cast<std::size_t>(i)];
            if (!slot.empty()) {
                // Equipment is worn for the whole loadout, including while a
                // petal is reloading; this is the same rule as tickBroadcast.
                equipFlags |= registry.petal(slot.configIndex).equipFlags;
            }
            // TypeScript derives modifiers from equipped loadout entries, not
            // from their onCooldown flag. A broken body leaves a ring gap but
            // its equipment modifier and passive heal remain equipped.
            if (slot.empty()) continue;
            const PetalStats stats = registry.petalStats(slot.configIndex, slot.rarity);
            const PetalModifiers& mods = stats.modifiers;

            // ONE contribution per SLOT. A four-grain clump of sand is one
            // equipped petal, not four; summing per instance would make `count`
            // the strongest stat in the game.
            aggregate.modifiers.maxHealthScale *= mods.maxHealth;
            aggregate.modifiers.speedScale *= mods.speed;
            aggregate.modifiers.damageScale *= mods.damage;
            aggregate.modifiers.sizeScale *= mods.playerRadius;
            aggregate.modifiers.rangeScale *= mods.range;
            // Camera zoom is deliberately not summed. It is the BROWSER's:
            // the smallest cameraZoom over the whole bar wins and two of them
            // never stack, and the reference server neither computes the
            // figure nor puts it on the wire.

            // Rotation is the one additive multiplier in the reference:
            // `rotationSpeed += modifier - 1`.
            aggregate.spinScale += mods.rotationSpeed - 1.0;

            aggregate.modifiers.luck += mods.luck;
            aggregate.modifiers.magnetism += mods.magnetism;
            aggregate.modifiers.aggroRadiusBonus += mods.aggroRadius;
            aggregate.modifiers.petalAttractionRadius += mods.petalAttractionRadius;
            aggregate.modifiers.passiveHealPerSecond += stats.passiveHealPerSecond;
            aggregate.modifiers.poisonArmor =
                std::max(aggregate.modifiers.poisonArmor, mods.poisonArmor);
            if (!slot.broken) {
                aggregate.modifiers.spongeDamageDurationMillis =
                    std::max(aggregate.modifiers.spongeDamageDurationMillis,
                             stats.spongeDamageDurationMillis);
            }
        }
    }

    // Talents multiply what the loadout already produces, so they are applied
    // to the finished aggregate: a tree bonus is one factor over the whole
    // build, never a per-slot bonus that a five-petal loadout collects five
    // times over.
    if (const PlayerSkillTree* tree = world.tryGet<PlayerSkillTree>(player)) {
        aggregate.modifiers.damageScale *= tree->skills.statScale(SkillId::Damage);
        // The SAME talent, on a different curve. The reference scales a body
        // slam on the stat table and everything a petal does -- ring contact,
        // a shot, a pollen puff, a radiation pulse -- on the steeper effect
        // table, and it folds no petal `playerModifiers.damage` into the petal
        // side at all. Collapsing the two costs an apex-talented ring 2.5x its
        // damage, so the two factors are published separately.
        aggregate.modifiers.petalDamageScale = tree->skills.effectScale(SkillId::Damage);
        aggregate.modifiers.maxHealthScale *= tree->skills.statScale(SkillId::PlayerHealth);
        const double petalHealing = aggregate.modifiers.passiveHealPerSecond - 1.0;
        aggregate.modifiers.passiveHealPerSecond =
            1.0 + petalHealing * tree->skills.effectScale(SkillId::Healing);
    }

    // Written wholesale. The component is never edited in place on equip: an
    // incremental version has to unwind exactly what it applied, and one missed
    // unwind is a stat the player keeps for the rest of the session.
    if (PlayerModifiers* out = world.tryGet<PlayerModifiers>(player)) *out = aggregate.modifiers;
    const PlayerProgress* progress = world.tryGet<PlayerProgress>(player);
    const int level = progress ? progress->level : 1;
    double sizeScale = aggregate.modifiers.sizeScale;
    if (!(sizeScale > 0.0) || !std::isfinite(sizeScale)) sizeScale = 1.0;
    sizeScale = std::min(sizeScale, 6.0);
    if (Body* body = world.tryGet<Body>(player)) {
        body->radius = playerRadiusForLevel(level) * sizeScale;
    }
    if (Health* health = world.tryGet<Health>(player)) {
        const double newMax = std::round(maxHealthForLevel(level) *
                                         aggregate.modifiers.maxHealthScale);
        if (newMax > 0.0 && newMax != health->max) {
            const double fraction = health->max > 0.0 ? health->current / health->max : 1.0;
            health->max = newMax;
            health->current = std::round(newMax * clamp(fraction, 0.0, 1.0));
        }
    }
    if (ContactDamage* contact = world.tryGet<ContactDamage>(player)) {
        contact->amount = bodyDamageForLevel(level);
        contact->intervalMillis = 0.0;
    }
    if (PlayerVisuals* visuals = world.tryGet<PlayerVisuals>(player)) {
        visuals->equipFlags = equipFlags;
    }
    return aggregate;
}

void PetalSystem::applyPassiveHeal(World& world, Entity player, const Aggregate& aggregate,
                                   double dt) {
    // Applied here, once per player, rather than per petal: the aggregate has
    // already summed every slot's contribution, and healing again per instance
    // would pay a clump its bonus `count` times.
    healPlayer(world, player, aggregate.modifiers.passiveHealPerSecond * dt);
}

void PetalSystem::updateRing(World& world, Entity player, const Aggregate& aggregate, double dt) {
    PetalRing* ring = world.tryGet<PetalRing>(player);
    if (!ring) return;
    const Body* body = world.tryGet<Body>(player);
    const PlayerInput* input = world.tryGet<PlayerInput>(player);
    const double playerRadius = body ? body->radius : kPlayerBaseRadius;
    // Matches TypeScript's `60 + (PLAYER_SIZE / 2) * (sizeMultiplier - 1)`.
    // `playerRadius` is the equivalent scaled hitbox radius in C++.
    const double neutralRadius = kPetalOrbitRestRadius + playerRadius - kPlayerBaseRadius;

    // Attack wins over defend: the reference tests the extend button first and
    // reaches the retract branch only when it is up, so a player holding both
    // lunges rather than blocks.
    double target = 1.0;
    if (input) {
        if (input->current.attacking()) target = kPetalOrbitAttackExtension;
        else if (input->current.defending()) target = kPetalOrbitDefendExtension;
    }

    // Linear, and on the MULTIPLIER rather than on the radius. The reference
    // ramps the extension at a fixed 12 per second and derives the radius from
    // whatever it currently is, so a tap reaches full reach in a twelfth of a
    // second; easing the radius toward a target instead spent nearly half a
    // second closing the same gap, which changed the reach of every burst.
    const double step = kPetalExtensionRampPerSecond * std::max(0.0, dt);
    ring->extension = clamp(ring->extension + clamp(target - ring->extension, -step, step),
                            kPetalOrbitDefendExtension, kPetalOrbitAttackExtension);

    const double rangeScale = std::max(0.0, aggregate.modifiers.rangeScale);
    // What the ring is heading for, and where it is now. Both are published:
    // the extension is what a defendOnly petal clamps, and the radius is what
    // every other petal's orbit point is measured from.
    ring->targetRadius = neutralRadius * target * rangeScale;
    ring->radius = neutralRadius * ring->extension * rangeScale;
    ring->spin = wrapAngle(ring->spin + kPetalSpinRate * aggregate.spinScale * dt);
}

void PetalSystem::placePetals(World& world, const ContentRegistry& registry, Entity player,
                              const Aggregate& aggregate, double nowMillis, double dt,
                              const Terrain* terrain) {
    const Transform* ownerTransform = world.tryGet<Transform>(player);
    const PetalRing* ring = world.tryGet<PetalRing>(player);
    const Loadout* loadout = world.tryGet<Loadout>(player);
    if (!ownerTransform || !ring || !loadout) return;

    // The ring orbits where the flower WAS at the end of the last tick, not
    // where movement has just put it. That one tick of lag is what makes the
    // ring trail a sprinting flower instead of being welded to it, and the
    // reference is emphatic that it is deliberate. Nothing here may be
    // "fixed" to the live centre.
    PetalSlotState* ringState = world.tryGet<PetalSlotState>(player);
    const Vec2 committed = ownerTransform->position;
    const Vec2 centre =
        ringState != nullptr && ringState->ringCentreValid ? ringState->ringCentre : committed;
    if (ringState != nullptr) {
        ringState->ringCentre = committed;
        ringState->ringCentreValid = true;
    }

    const double facing = ownerTransform->angle;
    const double ringRadius = ring->radius;
    const double spin = ring->spin;
    const Body* ownerBody = world.tryGet<Body>(player);
    const double playerRadius = ownerBody ? ownerBody->radius : kPlayerBaseRadius;
    const double neutralRadius = kPetalOrbitRestRadius + playerRadius - kPlayerBaseRadius;
    const double rangeScale = std::max(0.0, aggregate.modifiers.rangeScale);
    // A defendOnly petal never flies out on attack but still pulls in on
    // defend, which the reference states as clamping the extension at 1 rather
    // than as a branch on the button.
    const double defendOnlyRadius =
        neutralRadius * std::min(ring->extension, 1.0) * rangeScale;
    const double attractionRadius = std::max(0.0, aggregate.modifiers.petalAttractionRadius);

    // The ring is shared out among INSTANCES, not among slots. A clump counts
    // once because its grains share one point on the circle; every other
    // multi-count petal -- light, pollen -- takes a ring place per instance, so
    // a five-count pollen next to a basic is six petals evenly spread rather
    // than a fan of five and a lone sixth opposite it. The walk over the
    // loadout is what fixes both the divisor and each instance's place in it.
    //
    // Counted from the LOADOUT rather than from the live petals: a broken slot
    // keeps its share of the circle, so its gap stays open instead of making
    // the rest of the ring lurch round to close it.
    std::array<int, kLoadoutActiveSlots> ordinal{};
    int occupied = 0;
    for (int i = 0; i < kLoadoutActiveSlots; ++i) {
        ordinal[static_cast<std::size_t>(i)] = occupied;
        const LoadoutSlot& slot = loadout->slots[static_cast<std::size_t>(i)];
        if (slot.empty()) continue;
        // A count of zero still occupies one place, as the reference's
        // `stats.count || 1` does.
        const int count = std::max(1, registry.petalStats(slot.configIndex, slot.rarity).count);
        occupied += registry.petal(slot.configIndex).clumped ? 1 : count;
    }
    if (occupied == 0) return;
    const double wedge = kTau / occupied;

    for (const Entity petal : loadout->spawned) {
        PetalInstance* instance = world.tryGet<PetalInstance>(petal);
        Transform* transform = world.tryGet<Transform>(petal);
        // Bounded by the ACTIVE row, not the loadout: `ordinal` is only that
        // wide, and nothing outside it was ever spawned.
        if (!instance || !transform || instance->slot >= kLoadoutActiveSlots) continue;
        const PetalConfig& config = registry.petal(instance->configIndex);
        const int subCount = std::max<int>(1, instance->subCount);

        // A clumped slot's grains all sit on its one ring place and fan out
        // around it below; every other instance owns the next place along.
        const int ringSlot =
            ordinal[instance->slot] + (config.clumped ? 0 : instance->subIndex);
        const double offset = wedge * ringSlot;
        instance->ringOffset = offset;

        // A fixed-direction petal is pinned to the flower's facing and does not
        // travel with the ring.
        const double angle = config.hasFixedDirection ? wrapAngle(facing + config.fixedDirection)
                                                      : wrapAngle(spin + offset);
        // noPhysics petals are pure modifiers and emitters: they ride on the
        // flower instead of taking a place on the ring.
        const double base = config.defendOnly ? defendOnlyRadius : ringRadius;
        const double reach = config.noPhysics ? 0.0 : base * rangeMultiplier(config);

        Vec2 orbit = centre + Vec2::fromAngle(angle, reach);
        double facingAngle = angle;
        if (config.clumped && subCount > 1) {
            const Body* body = world.tryGet<Body>(petal);
            const double spacing = (body ? body->radius : 0.0) * kClumpSpacing;
            // The grain sits out along this bearing from the clump centre, so
            // this is the way it faces -- and the way its volley goes. The
            // offset is fed to `fromAngle` unwrapped, exactly as before, so the
            // clump's positions are bit-for-bit what they were.
            const double sub = angle + kTau * instance->subIndex / subCount;
            orbit += Vec2::fromAngle(sub, spacing);
            facingAngle = wrapAngle(sub);
        }
        transform->angle = angle;
        instance->facingAngle = facingAngle;

        // Three position modes, as the reference has them. A petal with no
        // physics -- and every fixed-direction petal is one -- is snapped onto
        // its point and keeps no state: giving it a spring would let it lag
        // behind the flower it is supposed to be painted on.
        if (config.noPhysics || config.hasFixedDirection) {
            transform->position = orbit;
        } else {
            stepPetalPhysics(world, registry, *instance, *transform, centre, orbit, angle,
                             attractionRadius, aggregate.spinScale, nowMillis, dt);
        }

        // A petal that collides with the world is pushed back out of it, in
        // every one of the three modes: the reference resolves the finished
        // position rather than the orbit point, so a bulb dragged into a rock
        // slides along its face instead of orbiting through it. The velocity
        // into the wall goes with it, or the spring drives the petal straight
        // back inside on the next tick.
        if (config.wallCollide && terrain != nullptr) {
            const double radius =
                registry.petalStats(instance->configIndex, instance->rarity).radius;
            const Vec2 resolved = terrain->resolveCircle(transform->position, radius);
            if (!(resolved == transform->position)) {
                transform->position = resolved;
                instance->ringVelocity = Vec2{};
            }
        }
    }
}

void PetalSystem::stepPetalPhysics(World& world, const ContentRegistry& registry,
                                   PetalInstance& instance, Transform& transform, Vec2 centre,
                                   Vec2 orbit, double orbitAngle, double attractionRadius,
                                   double spinScale, double nowMillis, double dt) {
    // The force ramps in over the smoothing window rather than arriving at
    // full strength, so a petal that appears on top of the flower is not
    // slingshotted off the ring on its first tick.
    const double sinceSpawn = std::max(0.0, nowMillis - instance.spawnedAtMillis);
    const double smooth = std::min(1.0, sinceSpawn / kPetalSpawnSmoothMillis);

    Vec2 target = orbit;

    // Attraction. Measured from the ORBIT point rather than from where the
    // spring has actually left the petal, so "30 units of attraction" lights
    // up when a mob is 30 units from where the petal is about to swing past.
    AttractionTarget locked;
    const bool captured = !instance.homing &&
                          findAttractionTarget(world, orbit, attractionRadius, locked);
    if (captured) {
        instance.attractedTo = locked.mob;
    } else if (instance.attractedTo != NULL_ENTITY) {
        const Entity released = instance.attractedTo;
        instance.attractedTo = NULL_ENTITY;
        // Only a mob that DIED earns the glide. One the ring simply swept past
        // is still there to be re-acquired, and the spring closing that gap
        // reads as the petal falling back into line; the same spring closing a
        // corpse's gap reads as the whole ring jumping.
        if (!world.isAlive(released) || world.has<Dead>(released)) {
            instance.glideUntilMillis = nowMillis + kPetalReleaseGlideMillis;
        }
    }

    if (captured) {
        // Project onto a point just inside the mob's edge, along the bearing
        // the orbit point has FROM the mob. As the ring turns, that bearing
        // turns with it -- so the petal grinding its way around its victim
        // falls out of the rotation already there, with no angular code.
        const Vec2 fromMob = orbit - locked.position;
        const double bearing = fromMob.lengthSq() > 0.0 ? fromMob.angle() : orbitAngle;
        // The extra kick that makes the whip read as a whip. Stated against
        // the ring's own rate, which is where the reference's 0.002 rad/ms
        // came from.
        const double kick = kPetalSpinRate * kMobOrbitSpinBoost * spinScale * dt;
        target = locked.position +
                 Vec2::fromAngle(bearing + kick, locked.radius * kMobOrbitRadiusScale);
    }

    if (instance.homing) {
        // Rose and shell fly home to deliver their burst. Re-arming the glide
        // every tick keeps the overshoot-free approach in play instead of the
        // spring, so the petal tracks a moving flower and lands cleanly.
        target = centre;
        instance.glideUntilMillis = nowMillis + kPetalReleaseGlideMillis;
    }

    if (instance.glideUntilMillis != 0.0 && nowMillis < instance.glideUntilMillis) {
        const double approach = 1.0 - std::exp(-kPetalGlideRate * std::max(0.0, dt));
        const Vec2 glided = transform.position + (target - transform.position) * approach;
        // The glide still writes a velocity, so the spring picks up the motion
        // already under way when the window closes instead of starting cold.
        instance.ringVelocity = dt > 0.0 ? (glided - transform.position) / dt : Vec2{};
        transform.position = glided;
        return;
    }
    instance.glideUntilMillis = 0.0;

    // Semi-implicit Euler, substepped. The integrator diverges outright once a
    // slice exceeds ~0.089 s at these constants -- that is a petal flying off
    // and never coming back -- and a loaded server is allowed a 0.1 s tick, so
    // the step is cut rather than trusted. At the ordinary tick this is one
    // substep of dt; the rest of the range only engages on a catch-up frame.
    const int substeps =
        std::min(kPetalSpringMaxSubsteps,
                 std::max(1, static_cast<int>(std::ceil(std::max(0.0, dt) /
                                                        kPetalSpringSubstepSeconds))));
    const double subDt = std::max(0.0, dt) / substeps;
    for (int step = 0; step < substeps; ++step) {
        const Vec2 delta = target - transform.position;
        const double distance = delta.length();
        if (distance > 0.0) {
            // Force proportional to the distance, so the pull grows the
            // further the flower has run from its ring.
            instance.ringVelocity +=
                (delta / distance) * (kPetalSpringForce * distance * subDt * smooth);
        }
        instance.ringVelocity *= kPetalDamping;
        transform.position += instance.ringVelocity * subDt;
    }

    // Defence in depth. If the integrator ever does go non-finite, a petal
    // stranded at NaN is invisible and unkillable forever; snapping it back on
    // its target costs one frame of motion.
    if (!std::isfinite(transform.position.x) || !std::isfinite(transform.position.y)) {
        transform.position = target;
        instance.ringVelocity = Vec2{};
    }
}

// ---------------------------------------------------------------------------
// Per-petal actions
// ---------------------------------------------------------------------------

void PetalSystem::runActions(World& world, const ContentRegistry& registry, Entity player,
                             double nowMillis, const Terrain* terrain) {
    {
        ShieldState& shield = world.ensure<ShieldState>(player);
        if (!shield.active(nowMillis)) {
            shield.amount = 0;
            shield.untilMillis = 0;
        }
    }
    // Ahead of every pointer into the flower's columns, because a scripted
    // effect is allowed to write to the flower and the queues were filled while
    // the slot pass held those columns open.
    drainBehaviourQueues(world, registry, player, nowMillis);

    const Loadout* loadout = world.tryGet<Loadout>(player);
    if (!loadout || !world.has<PetalSlotState>(player)) return;
    const PlayerInput* input = world.tryGet<PlayerInput>(player);
    const bool defending = input && input->current.defending();
    const bool attacking = input && input->current.attacking() && !defending;

    // Snapshotted because an action creates entities, and a petal's slot state
    // is reached through the player's columns while that happens.
    actionList_ = loadout->spawned;

    for (const Entity petal : actionList_) {
        PetalInstance* instance = world.tryGet<PetalInstance>(petal);
        const Transform* transform = world.tryGet<Transform>(petal);
        if (!instance || !transform || instance->slot >= kLoadoutSlots) continue;

        const PetalConfig& config = registry.petal(instance->configIndex);
        const PetalStats stats = registry.petalStats(instance->configIndex, instance->rarity);

        // Scripted behaviour comes first: a petal that parks until it touches
        // something is armed by contact rather than by a timer, and lightning
        // has no timed action at all to reach the gate further down.
        const PetalBehaviour behaviour = behaviourOf(config.id);
        if (behaviour.waitsForCollision && !instance->collisionFired &&
            touchesMob(world, transform->position, stats.radius)) {
            // Armed BEFORE firing: the effect is allowed to destroy this very
            // petal, which moves the instance out from under the pointer.
            instance->collisionFired = true;
            runBehaviour(world, player, petal, config, stats, instance->rarity,
                         transform->position, PetalTrigger::Collision, nowMillis);
            continue;
        }

        // The flower cracks open on the first mob it brushes, whatever that
        // mob's damage was. The reference does it inside the contact loop, one
        // line after the hit it just dealt, so the crack costs the petal
        // nothing it had already earned: zeroing its health hands the slot to
        // the ordinary break path and the 50-second reload with it.
        if (config.id == "flower" && !instance->collisionFired &&
            touchesMob(world, transform->position, stats.radius)) {
            // Armed before the crack, which may summon and therefore may move
            // the instance out from under this pointer.
            instance->collisionFired = true;
            crackFlowerPetal(world, registry, player, petal, instance->slot, instance->rarity,
                             transform->position);
            continue;
        }

        // Yggdrasil raises a corpse it is carried over, and is spent doing it.
        // Not gated on PvP, on extension or on any action window: the reference
        // runs this for every live instance, every tick.
        if (config.id == "yggdrasil" &&
            revivePlayerNear(world, player, transform->position, nowMillis)) {
            // Zeroed rather than destroyed, as the reference does it, so the
            // slot's ordinary break path pays the reload: a revive costs the
            // petal exactly what a mob killing it would.
            if (Health* health = world.tryGet<Health>(petal)) health->current = 0.0;
            continue;
        }

        // Extension consumes web and pollen immediately. Web stays at its
        // neutral orbit while attacking and is thrown 620 units along its own
        // bearing; defending plants it where the pulled-in ring put it.
        if (attacking || defending) {
            if (stats.webRadius > 0.0) {
                Vec2 at = transform->position;
                if (attacking) at += Vec2::fromAngle(transform->angle, kWebThrowDistance);
                emitGroundEffect(world, player, at, GroundEffectKind::Web,
                                 stats.webRadius, 0.0, 0.5, instance->rarity,
                                 kWebLifetimeSeconds);
                world.add<Dead>(petal, Dead{player});
                continue;
            }
            if (config.id == "pollen") {
                double damage = stats.damage;
                if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(player)) {
                    damage *= modifiers->petalDamageScale;
                }
                emitGroundEffect(world, player, transform->position,
                                 GroundEffectKind::Poison,
                                 stats.size * kPollenRadiusPerSize,
                                 0.0, 1.0, instance->rarity,
                                 kPollenLifetimeSeconds, damage,
                                 kPollenHitIntervalMillis);
                world.add<Dead>(petal, Dead{player});
                continue;
            }
        }

        // Rose and shell charge in orbit, fly home only when useful, deliver
        // on body contact, and are consumed into the normal reload path.
        if (stats.heal > 0.0 || stats.shield > 0.0) {
            // Re-read per petal rather than held across the loop: raising a
            // corpse moves that flower into the living archetype, and every
            // column the player owns can move with it.
            ShieldState& shield = world.get<ShieldState>(player);
            const Health* ownerHealth = world.tryGet<Health>(player);
            const bool wantsHeal = stats.heal > 0.0 && ownerHealth && ownerHealth->alive() &&
                                   ownerHealth->current < ownerHealth->max;
            const bool wantsShield = stats.shield > 0.0 && !shield.active(nowMillis);
            const double charge = std::max(0.0, stats.healChargeMillis);
            instance->homing = nowMillis - instance->spawnedAtMillis >= charge &&
                               (wantsHeal || wantsShield);
            if (instance->homing) {
                const Transform* owner = world.tryGet<Transform>(player);
                const Body* ownerBody = world.tryGet<Body>(player);
                const double contact = ownerBody ? ownerBody->radius : kPlayerBaseRadius;
                if (owner && distanceSq(transform->position, owner->position) <= contact * contact) {
                    if (wantsHeal) {
                        const PlayerSkillTree* tree = world.tryGet<PlayerSkillTree>(player);
                        const double scale = tree ? tree->skills.effectScale(SkillId::Healing) : 1.0;
                        healPlayer(world, player, stats.heal * scale);
                    } else if (wantsShield) {
                        shield.amount = stats.shield;
                        shield.untilMillis = nowMillis + kBurstShieldLifetimeMillis;
                    }
                    world.add<Dead>(petal, Dead{player});
                }
                continue;
            }
        }

        // Bubble pops as soon as the ring is pulled in and propels the flower
        // directly away from the bubble, then pays its rarity-scaled reload.
        if (config.id == "bubble" && defending) {
            Transform* owner = world.tryGet<Transform>(player);
            const Body* ownerBody = world.tryGet<Body>(player);
            if (owner) {
                const Vec2 away = owner->position - transform->position;
                if (away.lengthSq() > 0.0) {
                    const double radius = ownerBody ? ownerBody->radius : kPlayerBaseRadius;
                    const double boost = 60.0 * (1.0 + rarityIndex(instance->rarity) * 0.6);
                    dashPlayer(*owner, away.normalized() * boost, boost, radius, terrain);
                }
            }
            world.add<Dead>(petal, Dead{player});
            continue;
        }

        // Other defend-only actions (notably web) fire only while pulled in.
        // Burst heal/shield was handled above because it homes when useful,
        // independently of the current input, as in the reference ring.
        if (config.defendOnly && !defending) continue;
        if (!hasTimedAction(config, stats)) continue;

        if (config.projectile.present && attacking &&
            nowMillis >= instance->nextProjectileMillis) {
            instance->nextProjectileMillis =
                nowMillis + std::max(stats.reloadMillis, net::kTickMillis);
            fireProjectiles(world, player, petal, config, stats, instance->configIndex,
                            instance->rarity);
        }

        if (!hasNonProjectileAction(config, stats)) continue;
        if (nowMillis < instance->nextActionMillis) continue;

        // Armed before acting: the actions below create entities, and the
        // instance pointer is not worth carrying across that.
        instance->nextActionMillis = nowMillis + actionIntervalMillis(config, stats);
        const Vec2 at = transform->position;
        const Rarity rarity = instance->rarity;
        const auto slot = instance->slot;

        if (config.radiation.present) {
            // The field is re-laid at the petal's feet every interval rather
            // than parented to it: a ground effect has no transform to follow,
            // and a patch that expires on the interval tracks the ring for free.
            const double interval = std::max(config.radiation.intervalMillis, net::kTickMillis);
            double pulse = stats.damage;
            if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(player)) {
                pulse *= modifiers->petalDamageScale;
            }
            emitGroundEffect(world, player, at, GroundEffectKind::Radiation,
                             config.radiation.radius, pulse, 1.0, rarity,
                             interval / 1000.0);
        }
        if (config.petMobIndex != kInvalidIndex) {
            // Looked up here rather than carried down the loop: raising a
            // corpse moves a flower between archetypes, and the player's
            // columns move with it.
            maintainPets(world, registry, player, slot, config, rarity,
                         world.get<PetalSlotState>(player));
        }
        if (behaviour.intervalMillis > 0.0) {
            runBehaviour(world, player, petal, config, stats, rarity, at, PetalTrigger::Interval,
                         nowMillis);
        }
    }
}

void PetalSystem::fireProjectiles(World& world, Entity player, Entity petal,
                                  const PetalConfig& config, const PetalStats& stats,
                                  std::uint16_t configIndex, Rarity rarity) {
    const ProjectileSpec& spec = config.projectile;
    // A shot with no speed or no reach is not a volley, it is an entity that
    // expires where it was born.
    if (spec.speed <= 0.0 || spec.distance <= 0.0) return;

    const Transform* origin = world.tryGet<Transform>(petal);
    if (!origin) return;
    const Vec2 from = origin->position;
    // The INSTANCE's facing, not the slot's: the grains of a clump each sit out
    // on their own sub-bearing and each fires down it, so four peas leave in
    // four directions instead of stacking on one. Reported by the ring step
    // rather than recomputed here, so the two cannot drift apart.
    const PetalInstance* shooter = world.tryGet<PetalInstance>(petal);
    const double heading = shooter != nullptr ? shooter->facingAngle : origin->angle;
    const Faction* ownerFaction = world.tryGet<Faction>(player);
    const Faction faction = ownerFaction ? *ownerFaction : Faction{Team::Players, false};
    // Baked in at spawn because a shot outlives the ring that fired it: the
    // reference stamps the flower's petal-damage multiplier onto the volley
    // when it leaves, and nothing re-reads the shooter afterwards.
    const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(player);
    const double damage = stats.damage * (modifiers ? modifiers->petalDamageScale : 1.0);

    // A shot is a scale model of the flower that fired it. The petal states
    // the shot's size at a STOCK flower, and a flower that has grown -- a
    // level, a size-changing petal -- fires proportionally bigger ones. Stated
    // against the body radius rather than against the modifier that produced
    // it so that every future way of growing a flower is inherited for free.
    //
    // The shot's own calibre comes off the petal's `size` stat and not off its
    // radius, which is what the reference does and what keeps a volley where
    // it was when the petal's body changed scale.
    const Body* ownerBody = world.tryGet<Body>(player);
    const double ownerScale =
        ownerBody != nullptr && kPlayerBaseRadius > 0.0
            ? std::max(0.05, ownerBody->radius / kPlayerBaseRadius)
            : 1.0;
    const double shotRadius = std::max(1.0, stats.size * kProjectileRadiusPerSize * ownerScale);

    // The flower's own motion, carried into the volley. Taken from the FLOWER
    // and not from the petal because a petal's velocity is dominated by its
    // orbit, which would fan a volley sideways by however far round the ring
    // the petal happened to be.
    const Motion* ownerMotion = world.tryGet<Motion>(player);
    const Vec2 inherited = ownerMotion != nullptr ? ownerMotion->velocity : Vec2{};

    // The pool that makes a shot penetrate. It is the petal's own health,
    // graded at the tier it was fired at, so nothing new has to be tuned: a
    // petal that survives a mob's bite survives shooting through the same mob.
    const double shotHealth =
        stats.breakable && stats.health > 0.0 ? stats.health : kProjectileDefaultHealth;

    const int count = std::max(1, spec.count);
    // spreadAngle is the STEP between adjacent shots, not the width of the fan,
    // so the volley is centred on the petal's outward heading.
    const double first = heading - spec.spreadAngle * (count - 1) * 0.5;

    for (int i = 0; i < count; ++i) {
        const double angle = wrapAngle(first + spec.spreadAngle * i);
        const Entity shot = world.create();
        world.add<ProjectileTag>(shot);
        world.add<Transform>(shot, Transform{from, angle});
        world.add<Motion>(shot, Motion{Vec2::fromAngle(angle, spec.speed) + inherited});
        // Mass is area on the same scale a mob's is, so the shove a shot
        // delivers grows with the square of the flower that grew it.
        world.add<Body>(shot, Body{shotRadius, projectileMass(shotRadius)});
        world.add<Faction>(shot, faction);
        world.add<Health>(shot, Health{shotHealth, shotHealth});
        // Added at spawn, never mid-flight: arming a ledger during the hit
        // loop would relocate the shot out from under it.
        world.add<HitCooldowns>(shot, HitCooldowns{});

        Projectile projectile;
        projectile.owner = player;
        projectile.creditTo = player;
        projectile.damage = damage;
        projectile.remainingDistance = spec.distance;
        projectile.petalConfigIndex = configIndex;
        projectile.rarity = rarity;
        projectile.seekRange = spec.seekRange;
        projectile.seekCone = spec.seekCone;
        world.add<Projectile>(shot, projectile);

        // Distance is the authority on range; the lifetime is the same limit
        // expressed in time, so a projectile that never hits anything still
        // dies on schedule even if nothing decrements the distance.
        world.add<Lifetime>(shot, Lifetime{spec.distance / spec.speed});
        world.add<PetalEffect>(shot, PetalEffect{stats.poisonPerSecond, stats.poisonDurationMillis,
                                                 stats.knockback, stats.slowFactor,
                                                 stats.slowDurationMillis});

        Replicated replicated;
        replicated.kind = net::EntityKind::Projectile;
        replicated.typeIndex = configIndex;
        replicated.rarity = rarity;
        world.add<Replicated>(shot, replicated);
        assignNetId(world, shot);
    }
}

void PetalSystem::emitGroundEffect(World& world, Entity player, Vec2 at, GroundEffectKind kind,
                                   double radius, double damagePerSecond, double slowFactor,
                                   Rarity rarity, double lifetimeSeconds,
                                   double damagePerHit, double damageIntervalMillis) {
    if (radius <= 0.0 || lifetimeSeconds <= 0.0) return;
    const Entity effect = world.create();
    world.add<GroundEffectTag>(effect);
    world.add<Transform>(effect, Transform{at, 0.0});
    world.add<GroundEffect>(effect, GroundEffect{kind, player, radius, damagePerSecond,
                                                 slowFactor, rarity, damagePerHit,
                                                 damageIntervalMillis});
    world.add<Lifetime>(effect, Lifetime{lifetimeSeconds});

    Replicated replicated;
    replicated.kind = net::EntityKind::Effect;
    // The kind IS the artwork for a ground effect -- a web field and a
    // radiation patch share every other replicated field -- so it rides in
    // typeIndex, which no other Effect uses.
    replicated.typeIndex = static_cast<std::uint16_t>(kind);
    replicated.rarity = rarity;
    world.add<Replicated>(effect, replicated);
    assignNetId(world, effect);
}

// ---------------------------------------------------------------------------
// Scripted behaviours
// ---------------------------------------------------------------------------

void PetalSystem::drainBehaviourQueues(World& world, const ContentRegistry& registry,
                                       Entity player, double nowMillis) {
    // A break effect runs before a spawn effect for the same reason the
    // reference's does: the petal that broke is already gone, and the one that
    // replaces it has not been armed yet.
    for (const PendingBreak& broken : pendingBreaks_) {
        const PetalConfig& config = registry.petal(broken.configIndex);
        const PetalStats stats = registry.petalStats(broken.configIndex, broken.rarity);
        runBehaviour(world, player, NULL_ENTITY, config, stats, broken.rarity, broken.at,
                     PetalTrigger::Break, nowMillis);
    }
    pendingBreaks_.clear();

    // The flower's position, not the ring point: the reference builds a spawn
    // context before the petal has been placed, so a blood leaf that detonates
    // the moment it appears does so on top of the flower.
    const Transform* ownerTransform = world.tryGet<Transform>(player);
    const Vec2 origin = ownerTransform ? ownerTransform->position : Vec2{};
    for (const Entity petal : pendingSpawns_) {
        const PetalInstance* instance = world.tryGet<PetalInstance>(petal);
        if (instance == nullptr) continue;
        const std::uint16_t configIndex = instance->configIndex;
        const Rarity rarity = instance->rarity;
        const PetalConfig& config = registry.petal(configIndex);
        const PetalStats stats = registry.petalStats(configIndex, rarity);
        runBehaviour(world, player, petal, config, stats, rarity, origin, PetalTrigger::Spawn,
                     nowMillis);
    }
    pendingSpawns_.clear();
}

void PetalSystem::queueBreakBehaviour(World& world, const ContentRegistry& registry,
                                      Entity petal) {
    const PetalInstance* instance = world.tryGet<PetalInstance>(petal);
    const Transform* transform = world.tryGet<Transform>(petal);
    if (instance == nullptr || transform == nullptr) return;
    const PetalBehaviour behaviour = behaviourOf(registry.petal(instance->configIndex).id);
    if (behaviour.kind == PetalBehaviourKind::None) return;
    // A petal that detonated itself on contact is already spent. The reference
    // puts it straight on cooldown, and a slot on cooldown never reaches the
    // break branch -- which is what stops a bomb exploding twice for one hit.
    if (behaviour.breaksSelfOnCollision && instance->collisionFired) return;
    pendingBreaks_.push_back({instance->configIndex, instance->rarity, transform->position});
}

void PetalSystem::runBehaviour(World& world, Entity player, Entity petal,
                               const PetalConfig& config, const PetalStats& stats, Rarity rarity,
                               Vec2 at, PetalTrigger trigger, double nowMillis) {
    const PetalBehaviour behaviour = behaviourOf(config.id);
    // A break fires every effect the petal has, unconditionally: the reference
    // ran the same script in an "immediate mode" that skipped every guard, so
    // a starfish heals on the way out whatever the flower's health was and a
    // parked bomb detonates when something else destroys it.
    const bool guarded = trigger != PetalTrigger::Break;

    switch (behaviour.kind) {
        case PetalBehaviourKind::None:
            return;

        case PetalBehaviourKind::Lightning:
            // Parked at spawn, so only a contact or a break strikes.
            if (trigger == PetalTrigger::Spawn || trigger == PetalTrigger::Interval) return;
            strikeLightning(world, player, at, stats.damage);
            return;

        case PetalBehaviourKind::LightningCutter: {
            if (trigger == PetalTrigger::Collision) return;
            // Per PLAYER, not per petal: the reference's limiter is keyed on
            // the flower, so a second cutter shares the first one's cadence
            // instead of doubling the strike rate.
            PetalSlotState* state = world.tryGet<PetalSlotState>(player);
            if (state != nullptr) {
                if (nowMillis < state->nextLightningMillis) return;
                state->nextLightningMillis = nowMillis + kLightningCutterIntervalMillis;
            }
            strikeLightning(world, player, at, stats.damage);
            return;
        }

        case PetalBehaviourKind::BloodLeaf: {
            if (trigger == PetalTrigger::Collision || trigger == PetalTrigger::Interval) return;
            if (guarded) {
                // `memory:player:extended`: the leaf only opens while the ring
                // is held out.
                const PlayerInput* input = world.tryGet<PlayerInput>(player);
                const bool extended = input != nullptr && input->current.attacking() &&
                                      !input->current.defending();
                if (!extended) return;
            }
            explodePetal(world, player, at, stats.size, 100.0, nowMillis);
            healFromBehaviour(world, player, -1.0, rarity);
            return;
        }

        case PetalBehaviourKind::Starfish: {
            if (trigger == PetalTrigger::Collision || trigger == PetalTrigger::Interval) return;
            if (guarded) {
                const Health* health = world.tryGet<Health>(player);
                if (health == nullptr || health->current >= kStarfishHealthThreshold) return;
            }
            healFromBehaviour(world, player, 25.0, rarity);
            return;
        }

        case PetalBehaviourKind::Bomb:
            if (trigger == PetalTrigger::Spawn || trigger == PetalTrigger::Interval) return;
            explodePetal(world, player, at, stats.size, 30.0, nowMillis);
            // Consumed by its own blast, which is what puts the slot on its
            // reload; the break effect above is guarded against firing again.
            if (trigger == PetalTrigger::Collision && world.isAlive(petal)) {
                world.add<Dead>(petal, Dead{player});
            }
            return;

        case PetalBehaviourKind::Shield: {
            if (trigger == PetalTrigger::Collision) return;
            // Flat, and neither rarity- nor talent-scaled. Replaces rather than
            // stacks, exactly as a second shell's burst does.
            ShieldState& shield = world.ensure<ShieldState>(player);
            shield.amount = kBehaviourShieldAmount;
            shield.untilMillis = nowMillis + kBehaviourShieldMillis;
            return;
        }

        case PetalBehaviourKind::Healing:
            if (trigger == PetalTrigger::Collision) return;
            healFromBehaviour(world, player, 20.0, rarity);
            return;

        case PetalBehaviourKind::TestExplosive:
            if (trigger == PetalTrigger::Collision) return;
            explodePetal(world, player, at, stats.size, 50.0, nowMillis);
            return;
    }
}

void PetalSystem::emitDamageBurst(World& world, Entity player, Vec2 at, double radius,
                                  double damage) {
    if (radius <= 0.0 || damage <= 0.0) return;
    // An instantaneous area hit, expressed as a damage field that lives for a
    // single tick.
    //
    // The reference walks the mobs itself and damages each one, but the one
    // damage path here belongs to combat -- it is what credits the kill, pays
    // the bounty and raises the death -- and this system runs before it. So the
    // strike leaves behind exactly the thing combat already knows how to
    // resolve: a per-hit field, which carries the same "every non-pet mob whose
    // centre is inside, once" rule the strike and the blast both want. One and
    // a half ticks of life makes it fire on this tick's field pass and expire
    // on the next one's.
    //
    // Deliberately not replicated: the wire has no strike or explosion effect,
    // and dressing one up as a poison cloud would draw a lie.
    const Entity burst = world.create();
    world.add<GroundEffectTag>(burst);
    world.add<Transform>(burst, Transform{at, 0.0});
    world.add<GroundEffect>(burst, GroundEffect{GroundEffectKind::Poison, player, radius, 0.0,
                                                1.0, Rarity::Common, damage, 0.0});
    world.add<Lifetime>(burst, Lifetime{net::kTickSeconds * 1.5});
}

void PetalSystem::strikeLightning(World& world, Entity player, Vec2 at, double damage) {
    // The strike carries the petal's raw stat: unlike ring contact, the
    // reference hands it straight over without the flower's damage multiplier.
    emitDamageBurst(world, player, at, kLightningRadius,
                    damage > 0.0 ? damage : kLightningFallbackDamage);
}

void PetalSystem::explodePetal(World& world, Entity player, Vec2 at, double petalSize,
                               double damage, double nowMillis) {
    if (nowMillis - lastExplosionMillis_ < kExplosionThrottleMillis) return;
    lastExplosionMillis_ = nowMillis;

    const double radius = petalSize * kExplosionRadiusPerSize;
    emitDamageBurst(world, player, at, radius, damage);

    // Knockback is dealt here rather than left to the field, because a field
    // has no direction to push along. Collected first: writing the impulse adds
    // a component, which moves the mob between archetypes.
    collectMobsNear(world, at, radius, mobScratch_);
    for (const Entity mob : mobScratch_) {
        const Transform* transform = world.tryGet<Transform>(mob);
        if (transform == nullptr) continue;
        const Vec2 away = transform->position - at;
        if (away.lengthSq() <= 0.0) continue;   // exactly co-located: no direction to push along
        world.ensure<Knockback>(mob).impulse = away.normalized() * kExplosionKnockback;
    }
}

void PetalSystem::healFromBehaviour(World& world, Entity player, double amount, Rarity rarity) {
    Health* health = world.tryGet<Health>(player);
    if (health == nullptr || !health->alive()) return;
    const PlayerSkillTree* tree = world.tryGet<PlayerSkillTree>(player);
    const double talent = tree ? tree->skills.effectScale(SkillId::Healing) : 1.0;
    const double scaled = amount * std::pow(std::sqrt(3.0), rarityIndex(rarity)) * talent *
                          kBehaviourHealScale;
    health->current = std::min(health->max, health->current + scaled);
    // A scripted `heal -1` is self-damage, and it stops one point short of
    // lethal: taking a flower to zero raises a death, and the death notice
    // belongs to the combat path this system cannot reach.
    if (health->current < 1.0) health->current = 1.0;
}

bool PetalSystem::touchesMob(World& world, Vec2 at, double radius) {
    bindTo(world);
    bool touching = false;
    // A linear sweep: the broadphase is rebuilt after this system runs, so what
    // it holds here is last tick's world. Only the two petals that park for a
    // contact ask, and only until their first one.
    mobs_->each([&](Entity, MobTag&, Transform& transform, Body& body) {
        if (touching) return;
        const double reach = radius + body.radius;
        const double gap = distanceSq(transform.position, at);
        // `> 0` as the reference has it: a mob exactly on the petal is a
        // degenerate overlap with no direction, not a hit.
        if (gap < reach * reach && gap > 0.0) touching = true;
    });
    return touching;
}

void PetalSystem::collectMobsNear(World& world, Vec2 at, double radius, std::vector<Entity>& out) {
    bindTo(world);
    out.clear();
    const double reachSq = radius * radius;
    mobs_->each([&](Entity e, MobTag&, Transform& transform, Body&) {
        if (distanceSq(transform.position, at) <= reachSq) out.push_back(e);
    });
}

bool PetalSystem::revivePlayerNear(World& world, Entity reviver, Vec2 at, double nowMillis) {
    for (const Entity other : playerList_) {
        if (other == reviver || !world.isAlive(other)) continue;
        if (!playerIsDown(world, other)) continue;
        const Transform* transform = world.tryGet<Transform>(other);
        if (transform == nullptr) continue;
        if (distanceSq(transform->position, at) >
            kYggdrasilRevivalRange * kYggdrasilRevivalRange) {
            continue;
        }
        Health* health = world.tryGet<Health>(other);
        if (health == nullptr) continue;
        health->current = health->max;
        health->invulnerableUntilMillis =
            nowMillis + kRespawnInvulnerabilitySeconds * 1000.0;
        // Written before the tag comes off: removing a component moves the
        // flower to another archetype and `health` goes with it.
        if (world.has<Dead>(other)) world.remove<Dead>(other);
        if (onPlayerRevived) onPlayerRevived(other, reviver);
        // One corpse per petal. The reference breaks out of its scan here, so a
        // yggdrasil carried through a pile raises one body per reload.
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The flower's own aura
// ---------------------------------------------------------------------------

void PetalSystem::applyRaindropAura(World& world, const ContentRegistry& registry, Entity player,
                                    PetalSlotState& state, const Aggregate& aggregate,
                                    double nowMillis) {
    if (nowMillis < state.raindropReadyAtMillis) return;
    const Loadout* loadout = world.tryGet<Loadout>(player);
    const Transform* transform = world.tryGet<Transform>(player);
    if (loadout == nullptr || transform == nullptr) return;

    double bestDamage = 0;
    double bestRadius = 0;
    for (int i = 0; i < kLoadoutActiveSlots; ++i) {
        const LoadoutSlot& slot = loadout->slots[static_cast<std::size_t>(i)];
        // A reloading raindrop projects nothing, as an on-cooldown one does not
        // in the reference.
        if (slot.empty() || slot.broken) continue;
        if (registry.petal(slot.configIndex).id != "raindrop") continue;
        // Maximised INDEPENDENTLY: a wide common raindrop next to a narrow
        // mythic one projects the common's reach at the mythic's damage.
        bestDamage = std::max(bestDamage,
                              registry.petalStats(slot.configIndex, slot.rarity).damage);
        bestRadius = std::max(bestRadius,
                              kRaindropAuraBaseRadius +
                                  rarityIndex(slot.rarity) * kRaindropAuraRadiusPerRarity);
    }
    if (bestDamage <= 0.0 || bestRadius <= 0.0) return;

    // Armed before the pulse: emitting creates an entity, and nothing after
    // this line may depend on the flower's columns staying put.
    state.raindropReadyAtMillis = nowMillis + kRaindropAuraDamageIntervalMillis;
    emitDamageBurst(world, player, transform->position, bestRadius,
                    bestDamage * aggregate.modifiers.petalDamageScale);
}

void PetalSystem::retireDistantPets(World& world, const ContentRegistry& registry, Entity player,
                                    double nowMillis) {
    PetalSlotState* state = world.tryGet<PetalSlotState>(player);
    Loadout* loadout = world.tryGet<Loadout>(player);
    const Transform* ownerTransform = world.tryGet<Transform>(player);
    if (state == nullptr || loadout == nullptr || ownerTransform == nullptr) return;
    const Vec2 owner = ownerTransform->position;

    for (PetalSlotState::Slot& slotState : state->slots) {
        std::size_t kept = 0;
        for (const Entity pet : slotState.pets) {
            if (!world.isAlive(pet)) continue;
            const MobAi* ai = world.tryGet<MobAi>(pet);
            const Transform* transform = world.tryGet<Transform>(pet);
            // A summon something killed is not one that wandered off: it
            // keeps its handle for maintainPets to prune, and its egg reloads
            // on the ordinary schedule rather than this one.
            const bool retires = ai != nullptr && transform != nullptr &&
                                 !world.has<Dead>(pet) &&
                                 (ai->kind == AiKind::Passive || ai->kind == AiKind::Sandstorm);
            if (!retires) {
                slotState.pets[kept++] = pet;
                continue;
            }
            // The fixed viewport, not the one the client reported: the
            // reference clips every pet to the same rectangle, so a player on
            // a wide window would otherwise keep a sandstorm twice as long.
            const Vec2 offset = transform->position - owner;
            if (std::abs(offset.x) <= kViewportWidth * 0.5 &&
                std::abs(offset.y) <= kViewportHeight * 0.5) {
                slotState.pets[kept++] = pet;
                continue;
            }

            const MobType* type = world.tryGet<MobType>(pet);
            const std::uint16_t mobIndex = type ? type->configIndex : kInvalidIndex;
            // Destroyed rather than killed: a summon that walked off the
            // screen has not died, so it raises no death event and drops
            // nothing.
            world.destroy(pet);
            reloadEggForPet(world, registry, *state, *loadout, mobIndex, nowMillis);
        }
        slotState.pets.resize(kept);
    }
}

void PetalSystem::reloadEggForPet(World& world, const ContentRegistry& registry,
                                  PetalSlotState& state, Loadout& loadout,
                                  std::uint16_t mobIndex, double nowMillis) {
    if (mobIndex == kInvalidIndex) return;
    for (int i = 0; i < kLoadoutActiveSlots; ++i) {
        const auto index = static_cast<std::size_t>(i);
        LoadoutSlot& slot = loadout.slots[index];
        // Keyed on the mob rather than on the slot the pet was filed under,
        // and skipping a slot that is already reloading: two sticks that lose
        // a sandstorm each pay one cycle each, and a pet nothing equipped can
        // hatch -- a cracked flower's squad -- costs the ring nothing at all.
        if (slot.empty() || slot.broken) continue;
        if (registry.petal(slot.configIndex).petMobIndex != mobIndex) continue;

        const PetalStats stats = registry.petalStats(slot.configIndex, slot.rarity);
        const double reload = reloadMillisFor(stats);
        PetalSlotState::Slot& slotState = state.slots[index];
        destroySlotPetals(world, loadout, static_cast<std::uint8_t>(i));
        // Not "the cluster was just destroyed": left set, the fold at the top
        // of the next tick would charge every missing instance to the shared
        // pool and break a slot that is already serving its reload.
        slotState.populated = false;
        if (slotState.independent) {
            for (double& ready : slotState.instanceReadyAtMillis) ready = nowMillis + reload;
        }
        slot.broken = true;
        slot.reloadReadyAtMillis = nowMillis + reload;
        return;
    }
}

void PetalSystem::maintainPets(World& world, const ContentRegistry& registry, Entity player,
                               std::uint8_t slot, const PetalConfig& config, Rarity rarity,
                               PetalSlotState& state) {
    PetalSlotState::Slot& slotState = state.slots[static_cast<std::size_t>(slot)];
    std::size_t kept = 0;
    for (const Entity pet : slotState.pets) {
        if (world.isAlive(pet) && !world.has<Dead>(pet)) slotState.pets[kept++] = pet;
    }
    slotState.pets.resize(kept);

    // The EQUIPPED petal's rarity is the pet's tier. `petMobRarity` is written
    // in the JSON but no spawn path in the reference reads it, and taking it
    // instead pinned every summon to common: mob stats scale by 3 per tier, so
    // an egg above common was worth nothing.
    const bool apex = rarity == Rarity::Apex;
    const Rarity petRarity = apex ? Rarity::Unique : rarity;
    const int wanted = std::max(0, config.petCount) * (apex ? kApexPetCount : 1);
    if (wanted <= 0) return;

    // Counted per MOB TYPE across the whole player, never per slot. The
    // reference asks how many of this mob the OWNER has out before it summons,
    // so a second stick egg finds the first one's pair already there and
    // hatches nothing; keying the count on the slot gave a full row of sticks
    // twenty sandstorms where production shows two.
    if (countPetsOfType(world, state, config.petMobIndex) >= wanted) return;

    const Transform* ownerTransform = world.tryGet<Transform>(player);
    if (!ownerTransform) return;
    const Vec2 ownerPosition = ownerTransform->position;

    // A squad REPLACES its predecessor rather than joining it: the reference
    // clears every pet of the type before summoning, so a squad rebuilt after
    // one of its members died comes back whole rather than as a mixture.
    recallPetsOfType(world, state, config.petMobIndex);
    summonPets(world, registry, player, slot, config.petMobIndex, petRarity, wanted, ownerPosition,
               state);
}

int PetalSystem::countPetsOfType(World& world, const PetalSlotState& state,
                                 std::uint16_t mobIndex) {
    if (mobIndex == kInvalidIndex) return 0;
    int count = 0;
    for (const PetalSlotState::Slot& slot : state.slots) {
        for (const Entity pet : slot.pets) {
            if (!world.isAlive(pet) || world.has<Dead>(pet)) continue;
            const MobType* type = world.tryGet<MobType>(pet);
            if (type != nullptr && type->configIndex == mobIndex) ++count;
        }
    }
    return count;
}

int PetalSystem::countOwnedPets(World& world, const PetalSlotState& state) {
    int count = 0;
    for (const PetalSlotState::Slot& slot : state.slots) {
        for (const Entity pet : slot.pets) {
            if (world.isAlive(pet) && !world.has<Dead>(pet)) ++count;
        }
    }
    return count;
}

void PetalSystem::recallPetsOfType(World& world, PetalSlotState& state, std::uint16_t mobIndex) {
    if (mobIndex == kInvalidIndex) return;
    for (PetalSlotState::Slot& slot : state.slots) {
        std::size_t kept = 0;
        for (const Entity pet : slot.pets) {
            // A summon the world has already reaped leaves its handle behind.
            // Dropping it here is the only prune a slot whose petal summons
            // nothing itself -- the flower's -- ever gets.
            if (!world.isAlive(pet)) continue;
            const MobType* type = world.tryGet<MobType>(pet);
            if (type != nullptr && type->configIndex == mobIndex) {
                // Destroyed, not marked Dead: a recalled summon has not been
                // killed, so it must not raise a death event or drop anything.
                world.destroy(pet);
                continue;
            }
            slot.pets[kept++] = pet;
        }
        slot.pets.resize(kept);
    }
}

void PetalSystem::summonPets(World& world, const ContentRegistry& registry, Entity player,
                             std::uint8_t slot, std::uint16_t mobIndex, Rarity rarity, int count,
                             Vec2 at, PetalSlotState& state) {
    if (mobIndex == kInvalidIndex || count <= 0) return;
    PetalSlotState::Slot& slotState = state.slots[static_cast<std::size_t>(slot)];

    const Faction* ownerFaction = world.tryGet<Faction>(player);
    const Faction faction = ownerFaction ? *ownerFaction : Faction{Team::Players, false};
    MobStats mob = registry.mobStats(mobIndex, rarity);
    const double petScale = petStatMultiplier(registry.mob(mobIndex).id);
    mob.health *= petScale;
    mob.damage *= petScale;
    // Zero means "unstated", and an unstated range reads as the default chase
    // range wherever a pet's target acquisition consumes it -- so the bonus is
    // added first and the fallback only covers a pet that still has none.
    const double range = mob.aggroRange + rarityIndex(rarity) * kPetAggroRangePerRarity;
    const double aggroRange = range > 0.0 ? range : kEnemyChaseRange;

    // Counted once and carried, because the summons below are what change it.
    int owned = countOwnedPets(world, state);
    for (int i = 0; i < count; ++i) {
        // Checked per summon, as the reference checks it: a squad that runs
        // into the ceiling lands the members it had room for.
        if (owned >= kMaxPetEntitiesPerPlayer) return;
        const Vec2 spawnAt = at + rng_.insideCircle(mob.radius * 2.0 + kPlayerBaseRadius);
        const Entity pet = world.create();
        world.add<MobTag>(pet);
        world.add<Transform>(pet, Transform{spawnAt, rng_.angle()});
        world.add<Motion>(pet);
        world.add<Knockback>(pet);
        world.add<Body>(pet, Body{mob.radius, mob.mass});
        world.add<Health>(pet, Health{mob.health, mob.health, 0.0, 0.0});
        world.add<Faction>(pet, faction);
        world.add<MobType>(pet, MobType{mobIndex, rarity, 1.0});

        MobAi ai;
        // Whatever the mob is in the wild, at this tier. The reference is
        // explicit that taming overrides nothing: a hostile or neutral mob
        // fights for its owner, a passive one still will not, and a sandstorm
        // still drifts -- and it is that drift, plus the retirement rule it
        // feeds, that is the whole life cycle of a stick's summons.
        ai.kind = mob.ai;
        ai.anchor = spawnAt;
        ai.aggroRange = aggroRange;
        world.add<MobAi>(pet, ai);

        world.add<ContactDamage>(pet, ContactDamage{mob.damage, kMobHitIntervalMillis});
        world.add<HitCooldowns>(pet);
        world.add<Afflictions>(pet);
        // Deliberately no Bounty: a pet awards no XP and drops nothing, so a
        // player's summons cannot be farmed by whoever kills them.
        world.add<Pet>(pet, Pet{player, slot});

        Replicated replicated;
        replicated.kind = net::EntityKind::Mob;
        replicated.typeIndex = mobIndex;
        replicated.rarity = rarity;
        replicated.spawnFlags = net::SpawnIsPet;
        world.add<Replicated>(pet, replicated);
        assignNetId(world, pet);

        slotState.pets.push_back(pet);
        ++owned;
    }
}

void PetalSystem::crackFlowerPetal(World& world, const ContentRegistry& registry, Entity player,
                                   Entity petal, std::uint8_t slot, Rarity rarity, Vec2 at) {
    // Zeroed rather than destroyed, exactly as the reference zeroes the
    // instance: the slot's ordinary break path then pays the reload, so a
    // cracked flower comes back on its cooldown like any other spent petal.
    if (Health* health = world.tryGet<Health>(petal)) health->current = 0.0;

    // One crack in twenty is the glitch itself, and it takes the flower that
    // was carrying the petal instead of opening onto a squad.
    if (rng_.chance(kFlowerCorruptChance)) {
        // The splitter half the reference corrupts alongside this one has no
        // C++ counterpart; there is only ever the one flower here.
        if (PlayerVisuals* visuals = world.tryGet<PlayerVisuals>(player)) {
            visuals->corrupted = true;
        }
        return;
    }

    const std::uint16_t mobIndex = registry.mobIndex(kFlowerPetMobId);
    if (mobIndex == kInvalidIndex) return;
    PetalSlotState* state = world.tryGet<PetalSlotState>(player);
    if (state == nullptr) return;
    // Apex is clamped here rather than left to the summon's own apex rule,
    // which would turn the three this petal promises into nine.
    const Rarity petRarity = rarity == Rarity::Apex ? Rarity::Unique : rarity;
    recallPetsOfType(world, *state, mobIndex);
    // At the PETAL, not at the flower: the squad lands on the mob that broke
    // it, which is the whole point of the petal.
    summonPets(world, registry, player, slot, mobIndex, petRarity, kFlowerPetCount, at, *state);
}

} // namespace flix
