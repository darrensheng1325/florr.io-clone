#pragma once
// The petal ring: spawning it, placing it, breaking it and reloading it.
//
// The two components below carry this system's state and both live on the
// PLAYER, never on a petal. A broken slot has no petal entity left, so state
// kept there would be destroyed by the very event that has to be remembered --
// the break -- and the reload timer would go with it.
//
// Every number the ring uses comes out of the content registry as PetalStats.
// Nothing here re-derives a stat from a rarity: that ladder is rarity.h's job,
// and a second copy of it is how the two drift apart.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "shared/core/types.h"
#include "shared/core/world.h"
#include "shared/game/components.h"
#include "shared/game/config.h"
#include "shared/game/spatial.h"

namespace flix {

class Terrain;

/// Live state for one loadout slot's petals.
///
/// LoadoutSlot is the account-facing half -- what is equipped, whether it is
/// reloading -- and it is persisted. This is the simulation half, torn down and
/// rebuilt whenever the slot's petal or rarity changes.
struct PetalSlotState {
    struct Slot {
        /// The slot's shared health pool, used by every slot whose instances
        /// are NOT independent. Each instance mirrors it so combat can hit
        /// whichever is nearest, and the fold at the top of the next tick pulls
        /// the damage they took back into the pool.
        double poolHealth = 0;
        double poolMax = 0;
        /// What was last mirrored onto every instance. Whatever is missing from
        /// an instance now is exactly the damage it has taken since, which is
        /// why no per-instance previous-health array is needed.
        double syncedHealth = 0;

        /// Clumped or `independentHealth`: each instance owns its health and
        /// breaks alone. The pool is then unused and instanceReadyAtMillis
        /// drives reloading, one grain at a time.
        bool independent = false;

        /// False until the slot's instances have been spawned at least once.
        /// Without it the first tick would read "no petals on the field" as
        /// "the whole cluster was just destroyed" and break the slot instantly.
        bool populated = false;

        /// What the pool was built for. A slot whose petal or rarity no longer
        /// matches is rebuilt, which is also what makes swapping a petal hand
        /// you a fresh one instead of the previous occupant's health bar.
        std::uint16_t configIndex = kNoPetal;
        Rarity rarity = Rarity::Common;

        /// How much of the slot is still standing, 1.0 for untouched. Derived
        /// each tick from whichever of the two health models the slot uses, so
        /// that the one place that has to tell them apart is the pass that
        /// already does. The loadout bar drains its tile by it.
        double healthFraction = 1.0;

        /// When cluster member i comes back, or 0 when it is already out.
        /// Sized to the petal's count; meaningful only while `independent`.
        std::vector<double> instanceReadyAtMillis;

        /// Mobs this slot has summoned. Handles are generation-checked, so a
        /// pet killed in the field simply stops being alive and is re-summoned.
        std::vector<Entity> pets;
    };
    std::array<Slot, kLoadoutSlots> slots{};

    /// Next time the raindrop aura may pulse. The aura is not a petal action:
    /// it belongs to the FLOWER, is maximised across every equipped raindrop
    /// and fires whether or not any one of them is due to act.
    double raindropReadyAtMillis = 0;

    /// Next time a lightning cutter may strike. The reference's limiter is per
    /// PLAYER rather than per petal, so two cutters share one cadence.
    double nextLightningMillis = 0;

    /// The flower's position at the END of the previous tick, which is the
    /// centre the ring orbits.
    ///
    /// The reference lays the ring out from the player's last COMMITTED
    /// position while this tick's integrated one is still parked in a staging
    /// pair, and says so at length: orbiting the live centre is what stops
    /// petals trailing a sprinting flower. This system runs after movement, so
    /// the previous centre has to be remembered rather than read.
    Vec2 ringCentre;
    /// False until a centre has been recorded, and cleared whenever the flower
    /// stops being where it was -- a corpse's ring is torn down and the
    /// respawn must not fly its petals in from where it died.
    bool ringCentreValid = false;
};

/// What a petal (or a projectile it fired) does to what it touches, resolved
/// once at spawn.
///
/// The combat system owns damage; it does not own the rules for turning a
/// config plus a rarity into these numbers. Publishing them on the entity keeps
/// contact resolution to a component read, and keeps the two systems from
/// disagreeing about what a tier means.
struct PetalEffect {
    double poisonPerSecond = 0;
    double poisonDurationMillis = 0;
    double knockback = 0;
    double slowFactor = 1.0;      ///< multiplies the victim's speed; 1 = none
    double slowDurationMillis = 0;
};

/// When a petal's scripted behaviour fires.
///
/// The reference keeps four triggers rather than one because two of its entry
/// points had different semantics: a spawn ran the petal's script with its
/// guards applied and could park it until it touched something, while a break
/// ran the same script in "immediate mode", firing every effect
/// unconditionally. Starfish therefore heals on break whatever the flower's
/// health is, and a bomb explodes when it is destroyed as well as when it hits.
enum class PetalTrigger : std::uint8_t { Spawn, Collision, Interval, Break };

/// Owns a player's ring: which petals exist, where they are, when they break
/// and when they come back.
class PetalSystem {
public:
    /// One tick. Phase 4: after movement, so the ring orbits where the flower
    /// ended up; before combat, so a petal hits from its final position.
    void run(World& world, const ContentRegistry& registry, double nowMillis, double dt,
             CommandBuffer& commands, const Terrain* terrain = nullptr);

    /// Fold every flower's loadout into its PlayerModifiers, body radius and
    /// max health, without touching the ring.
    ///
    /// Split out of run() because the reference recomputes modifiers in
    /// Phase.Input (src/ecs/systems/playerModifiers.ts, scheduled ahead of
    /// playerMovement), while the ring itself is placed after movement. Folded
    /// inside run() alone, a speed or size petal would not reach movement until
    /// the tick after it was equipped, and movement would spend that tick
    /// stepping the flower at its previous speed.
    void foldModifiers(World& world, const ContentRegistry& registry);

    /// Wire ids for the entities this system spawns -- petals, projectiles,
    /// pets and ground effects.
    ///
    /// A net id has to be unique across the whole server, so the id space
    /// cannot belong to a system; whoever owns the allocator installs this
    /// hook. Left unset, everything still simulates and simply is not
    /// replicated, which is what a headless test wants.
    std::function<std::uint32_t()> allocateNetId;

    /// A yggdrasil raised `revived` off the ground.
    ///
    /// The world half of a revival is this system's -- the Dead tag comes off,
    /// the health bar refills and respawn protection is armed -- but the
    /// SESSION half is not: the corpse's death has already been announced to
    /// its owner, and only the connection layer can retract that and clear the
    /// flag that stops a second death being announced. Left unset the flower
    /// still stands up; its own client simply is not told.
    std::function<void(Entity revived, Entity reviver)> onPlayerRevived;

private:
    /// The tick's summed passive bonuses. PlayerModifiers has no field for the
    /// ring's spin and only the few lines that place the ring want one, so it
    /// rides along here rather than growing the component.
    struct Aggregate {
        PlayerModifiers modifiers;
        double spinScale = 1.0;
    };

    /// A mob a petal has latched onto, as the ring needs to see it.
    ///
    /// Two different radii of the same mob are in play and the reference is
    /// deliberate about it: eligibility is measured against the BODY radius
    /// the broadphase files -- the one carrying the per-spawn size jitter --
    /// while the point the petal is projected onto is re-read from the stat
    /// table, which has none. Collapsing them would move where an attracted
    /// petal sits on every mob that rolled a body.
    struct AttractionTarget {
        Entity mob = NULL_ENTITY;
        Vec2 position;
        double radius = 0;
    };

    void bindTo(World& world);

    /// Files every wild mob once per tick so the attraction lookup below is a
    /// cell walk rather than a scan of the world per petal per player.
    void rebuildAttractionGrid(World& world);
    /// The nearest attractable mob whose body reaches within `radius` of `at`,
    /// which is the petal's IDEAL orbit point rather than where the spring has
    /// actually left it: "30 units of attraction" then lights up when a mob is
    /// 30 units from where the petal is about to swing past.
    bool findAttractionTarget(World& world, Vec2 at, double radius, AttractionTarget& out);

    void clearRing(World& world, Entity player);
    void reconcileSlots(World& world, const ContentRegistry& registry, Entity player,
                        double nowMillis);
    Aggregate recomputeModifiers(World& world, const ContentRegistry& registry, Entity player);
    void applyPassiveHeal(World& world, Entity player, const Aggregate& aggregate, double dt);
    void updateRing(World& world, Entity player, const Aggregate& aggregate, double dt);
    /// Step every one of the player's petals: where its orbit point is, which
    /// mob (if any) has captured it, and the spring or glide that carries it
    /// there. `aggregate` is wanted for the ring's spin rate, which is the one
    /// modifier PlayerModifiers does not carry.
    void placePetals(World& world, const ContentRegistry& registry, Entity player,
                     const Aggregate& aggregate, double nowMillis, double dt,
                     const Terrain* terrain);
    /// Carry one petal from where it is to where it wants to be.
    ///
    /// A petal is sprung toward its orbit point, not pinned to it: that is what
    /// makes the ring trail a running flower, overshoot when it extends and
    /// settle rather than snap. Two windows replace the spring with an
    /// overshoot-free approach instead -- the fly-out when the petal appears
    /// and the release after the mob it was orbiting died -- because the spring
    /// crossing either of those gaps reads as the whole ring teleporting.
    void stepPetalPhysics(World& world, const ContentRegistry& registry, PetalInstance& instance,
                          Transform& transform, Vec2 centre, Vec2 orbit, double orbitAngle,
                          double attractionRadius, double spinScale, double nowMillis, double dt);
    void runActions(World& world, const ContentRegistry& registry, Entity player,
                    double nowMillis, const Terrain* terrain);

    /// The flower's own damage field, which no petal emits.
    ///
    /// Raindrop is not an orbiting petal that happens to hurt: the reference
    /// runs it in the per-PLAYER pipeline, centred on the flower, with damage
    /// and radius maximised INDEPENDENTLY across every equipped, off-cooldown
    /// raindrop -- so the widest one sets the reach and the strongest one the
    /// hit, even when they are two different petals.
    void applyRaindropAura(World& world, const ContentRegistry& registry, Entity player,
                           PetalSlotState& state, const Aggregate& aggregate, double nowMillis);

    /// `health` <= 0 spawns the petal with no Health component at all, which is
    /// what an unbreakable petal is: not one with zero hit points, which would
    /// break on its first tick.
    Entity spawnPetal(World& world, Entity player, Loadout& loadout, std::uint8_t slot,
                      std::uint8_t subIndex, std::uint8_t subCount, const PetalConfig& config,
                      const PetalStats& stats, std::uint16_t configIndex, Rarity rarity,
                      double health, double nowMillis);
    void destroySlotPetals(World& world, Loadout& loadout, std::uint8_t slot);
    void recallPets(World& world, PetalSlotState::Slot& state);
    void assignNetId(World& world, Entity e);

    /// Fire one volley. Returns whether a shot actually left the petal: a spec
    /// with no speed or no reach fires nothing, and a petal that fired nothing
    /// must not be spent for it.
    bool fireProjectiles(World& world, Entity player, Entity petal, const PetalConfig& config,
                         const PetalStats& stats, std::uint16_t configIndex, Rarity rarity);

    /// Take a petal that spent itself firing off the ring and put its slot on
    /// the reload, whichever of the two health models the slot runs on.
    void spendPetal(World& world, Entity player, Entity petal, std::uint8_t slotId,
                    std::uint8_t subIndex, const PetalStats& stats, double nowMillis);

    /// Run one petal's scripted behaviour. `at` is where the effect lands,
    /// which is the flower for a spawn and the petal itself thereafter.
    void runBehaviour(World& world, Entity player, Entity petal, const PetalConfig& config,
                      const PetalStats& stats, Rarity rarity, Vec2 at, PetalTrigger trigger,
                      double nowMillis);
    /// Remember a petal that just broke, so its break effect can run once the
    /// player's own columns are no longer held open by the slot pass.
    void queueBreakBehaviour(World& world, const ContentRegistry& registry, Entity petal);
    void drainBehaviourQueues(World& world, const ContentRegistry& registry, Entity player,
                              double nowMillis);

    /// An instantaneous area hit: everything a strike or an explosion needs
    /// beyond its own radius and damage.
    void emitDamageBurst(World& world, Entity player, Vec2 at, double radius, double damage);
    void strikeLightning(World& world, Entity player, Vec2 at, double damage);
    void explodePetal(World& world, Entity player, Vec2 at, double petalSize, double damage,
                      double nowMillis);
    /// The scripted heal, which is a different curve from a burst petal's:
    /// the reference multiplies the script's literal by sqrt(3) per rarity and
    /// by a flat 3 on top of the Healing talent.
    void healFromBehaviour(World& world, Entity player, double amount, Rarity rarity);
    /// True once the petal's body overlaps any live wild mob, which is what
    /// arms a behaviour that parks until it hits something.
    bool touchesMob(World& world, Vec2 at, double radius);
    /// Wild mobs whose CENTRE is inside `radius`, which is the test both the
    /// strike and the explosion use.
    void collectMobsNear(World& world, Vec2 at, double radius, std::vector<Entity>& out);
    /// Raise the first corpse within reach of a yggdrasil. Returns whether one
    /// was raised, which is what spends the petal.
    bool revivePlayerNear(World& world, Entity reviver, Vec2 at, double nowMillis);
    void emitGroundEffect(World& world, Entity player, Vec2 at, GroundEffectKind kind,
                          double radius, double damagePerSecond, double slowFactor,
                          Rarity rarity, double lifetimeSeconds,
                          double damagePerHit = 0.0, double damageIntervalMillis = 0.0);
    /// Retire the summons that have drifted off their owner's screen.
    ///
    /// Only the two kinds that never find their own way home: a pet that
    /// fights is steered back to the flower, while a passive one holds where
    /// it stands and a sandstorm deliberately outruns its owner. The reference
    /// lets those two go and charges the loss to the petal that hatched them,
    /// which is the whole life cycle of a stick's sandstorms -- they pull
    /// ahead, they vanish, the slot reloads and hatches a fresh pair.
    void retireDistantPets(World& world, const ContentRegistry& registry, Entity player,
                           double nowMillis);
    /// Put the petal that hatches `mobIndex` back on its reload, if one is
    /// equipped and not already reloading.
    void reloadEggForPet(World& world, const ContentRegistry& registry, PetalSlotState& state,
                         Loadout& loadout, std::uint16_t mobIndex, double nowMillis);

    /// `rarity` is the EQUIPPED petal's tier, which is what the pet inherits:
    /// the config's own `petMobRarity` is written in the JSON and read by
    /// nothing.
    ///
    /// The whole ring state rather than the one slot's: a summon is counted,
    /// replaced and capped per PLAYER and per mob type, so two stick slots
    /// share one pair of sandstorms rather than keeping a pair each.
    void maintainPets(World& world, const ContentRegistry& registry, Entity player,
                      std::uint8_t slot, const PetalConfig& config, Rarity rarity,
                      PetalSlotState& state);

    /// The player's live summons of one mob type, over every slot.
    int countPetsOfType(World& world, const PetalSlotState& state, std::uint16_t mobIndex);
    /// Everything the player has summoned, which is what the entity cap counts.
    int countOwnedPets(World& world, const PetalSlotState& state);
    /// Take back every summon of one mob type, wherever it was hatched. The
    /// reference clears the type before it summons, so a squad replaces its
    /// predecessor instead of joining it.
    void recallPetsOfType(World& world, PetalSlotState& state, std::uint16_t mobIndex);
    /// Put `count` summons of `mobIndex` on the field around `at`, filed under
    /// `slot`. Stops short at the per-player entity cap.
    void summonPets(World& world, const ContentRegistry& registry, Entity player,
                    std::uint8_t slot, std::uint16_t mobIndex, Rarity rarity, int count, Vec2 at,
                    PetalSlotState& state);

    /// The flower petal opening on the mob it touched: it is spent, and either
    /// the flower is corrupted or a squad of glitch flowers lands on the mob.
    void crackFlowerPetal(World& world, const ContentRegistry& registry, Entity player,
                          Entity petal, std::uint8_t slot, Rarity rarity, Vec2 at);

    /// A petal that broke this tick, held until its scripted break effect can
    /// safely run. The break is discovered while the player's columns are open
    /// and a scripted effect may add a component to the flower, which moves
    /// them; the petal entity itself is gone by then, so what it needs is
    /// copied out rather than looked up again.
    struct PendingBreak {
        std::uint16_t configIndex = kNoPetal;
        Rarity rarity = Rarity::Common;
        Vec2 at;
    };

    World* bound_ = nullptr;
    std::unique_ptr<Query<PlayerTag, Transform, Loadout, PetalRing>> players_;
    /// Wild mobs only: a summon is never a target for its owner's strike, its
    /// explosion or a behaviour waiting on first contact.
    std::unique_ptr<Query<MobTag, Transform, Body>> mobs_;

    std::vector<Entity> playerList_;
    std::vector<Entity> actionList_;
    /// One player's live petals bucketed by slot, rebuilt per player. Members
    /// so the per-tick work does not allocate.
    std::array<std::vector<Entity>, kLoadoutSlots> bySlot_;
    /// Petals whose scripted spawn effect has not run yet, and petals whose
    /// break effect has not. Both are filled by the slot pass and drained by
    /// the action pass, for the reason PendingBreak documents.
    std::vector<Entity> pendingSpawns_;
    std::vector<PendingBreak> pendingBreaks_;
    std::vector<Entity> mobScratch_;

    /// Wild mobs, rebuilt once at the top of the tick. Its own grid rather
    /// than the server's: the reference's attraction reads the enemy grid as
    /// it stood at the START of the tick, and the grid the other systems share
    /// is rebuilt twice more before this system runs.
    SpatialGrid attractionGrid_;
    std::vector<Entity> attractionCandidates_;

    /// The whole server detonates at most once per kExplosionThrottleMillis.
    /// A server-wide throttle rather than a per-petal one, exactly as the
    /// reference states it: a ring of bombs going off together is ONE
    /// explosion, and a port without this hits far harder than production.
    double lastExplosionMillis_ = 0;

    /// Pet spawn jitter and the flower petal's corruption roll. Neither has to
    /// agree with the client, and a fixed seed makes a failing test
    /// reproducible.
    Rng rng_{0xB17E5EEDull};
};

} // namespace flix

FLIX_COMPONENT(flix::PetalSlotState);
FLIX_COMPONENT(flix::PetalEffect);
