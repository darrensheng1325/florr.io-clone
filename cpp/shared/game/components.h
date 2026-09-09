#pragma once
// Every component in the game.
//
// Two rules shape this list:
//
//  * A component is what an entity HAS, not what it is allowed to do. Systems
//    are selected by which components an entity carries, so "can be damaged"
//    is a Health component and not a flag inside a giant Actor struct.
//
//  * State that toggles frequently lives in a FIELD, not in its own tag.
//    Adding or removing a component moves the entity between archetypes, which
//    copies all of its data. Tags are reserved for what an entity fundamentally
//    is -- and that changes at most once in its lifetime.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "shared/core/component.h"
#include "shared/core/entity.h"
#include "shared/core/types.h"
#include "shared/game/constants.h"
#include "shared/game/rarity.h"
#include "shared/game/realm.h"
#include "shared/game/skills.h"
#include "shared/net/protocol.h"

namespace flix {

// ---------------------------------------------------------------------------
// Kind tags -- exactly one per entity, fixed for its lifetime
// ---------------------------------------------------------------------------

struct PlayerTag {};
struct MobTag {};
struct PetalTag {};
struct ProjectileTag {};
struct DropTag {};
struct GroundEffectTag {};

/// Marks an entity as finished. The reaper destroys these at the end of the
/// tick, so a death that happens mid-system is visible to every later system
/// in the same tick instead of vanishing underneath it.
struct Dead {
    Entity killer = NULL_ENTITY;
};

// ---------------------------------------------------------------------------
// Spatial
// ---------------------------------------------------------------------------

struct Transform {
    Vec2 position;
    double angle = 0;      ///< facing, radians
    /// Which coordinate space `position` is in. Copied from whatever an entity
    /// is spawned by -- a petal from its flower, a drop from its mob -- so a
    /// thing never appears in a realm other than the one that made it.
    Realm realm = Realm::Overworld;
};

struct Motion {
    Vec2 velocity;         ///< units per second
};

struct Body {
    double radius = 10;
    /// Resistance to being pushed, in collision and knockback. Scales with
    /// area so a boss shrugs off what launches a ladybug.
    double mass = 1;
};

/// One pending positional displacement from a knockback-producing hit.
///
/// This intentionally is not momentum. TypeScript writes the resulting x/y
/// offset to the mob's Knockback component and the next movement pass applies
/// it directly, leaving ordinary velocity untouched. A later hit replaces the
/// previous value rather than launching a mob with an accumulated volley.
struct Knockback {
    Vec2 impulse;
};

// ---------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------

/// Who an entity will not hurt. Players and their pets share a team; every
/// wild mob is on the hostile team. PvP swaps players onto distinct teams.
enum class Team : std::uint8_t { Players = 0, Hostiles = 1, Neutral = 2 };

struct Faction {
    Team team = Team::Hostiles;
    /// In PvP, players are hostile to each other; elsewhere they are not.
    bool friendlyFireEnabled = false;
};

struct Health {
    double current = 1;
    double max = 1;
    /// When the white damage flash ends. Purely visual, but it lives here so
    /// the networking layer can derive StateHurt without a second lookup.
    double flashUntilMillis = 0;
    /// Damage is refused entirely before this time -- respawn protection.
    double invulnerableUntilMillis = 0;

    double fraction() const { return max > 0 ? clamp(current / max, 0.0, 1.0) : 0.0; }
    bool alive() const { return current > 0; }
};

/// Damage dealt by touching this entity.
struct ContactDamage {
    double amount = 0;
    /// Minimum gap between two hits on the same victim.
    double intervalMillis = kMobHitIntervalMillis;
};

/// Recent hits, so a damage source that stays in contact does not deal its
/// damage every tick.
///
/// A flat vector rather than a map: the list is a handful of entries and is
/// scanned linearly, which beats hashing at this size and keeps the component
/// relocatable without allocating a node per hit.
struct HitCooldowns {
    struct Entry {
        Entity victim = NULL_ENTITY;
        double readyAtMillis = 0;
    };
    std::vector<Entry> entries;

    bool ready(Entity victim, double nowMillis) const {
        for (const Entry& e : entries) {
            if (e.victim == victim) return nowMillis >= e.readyAtMillis;
        }
        return true;
    }

    void arm(Entity victim, double readyAtMillis) {
        for (Entry& e : entries) {
            if (e.victim == victim) { e.readyAtMillis = readyAtMillis; return; }
        }
        entries.push_back({victim, readyAtMillis});
    }

    /// A cooldown that names no victim.
    ///
    /// A petal with a `damageCooldown` is throttled on the petal INSTANCE
    /// alone: one glass petal lands one hit per window no matter how many mobs
    /// it is touching, because the reference keys the throttle on
    /// `${player}_${slot}_${instance}` with the victim left out entirely
    /// (src/server/playerState.ts:2729). Per-victim entries would turn the same
    /// petal into a full-rate area attack in a clump.
    double nextHitAtMillis = 0;

    bool globalReady(double nowMillis) const { return nowMillis >= nextHitAtMillis; }
    void armGlobal(double readyAtMillis) { nextHitAtMillis = readyAtMillis; }

    /// Drops expired entries. Called on a slow cadence: without it a petal
    /// that has grazed a thousand mobs carries all thousand forever.
    void prune(double nowMillis) {
        std::size_t out = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].readyAtMillis > nowMillis) entries[out++] = entries[i];
        }
        entries.resize(out);
    }
};

/// One poisoner's hold on one victim.
///
/// `source` is the credited PLAYER, never the petal: a flower carrying five
/// blue_iris still owns exactly one stack, while three flowers biting the same
/// boss own three and their damage ADDS. Keying on the petal would triple a
/// solo player's poison and keying on the victim alone -- which is what a
/// single scalar does -- throws away everyone but the strongest poisoner, both
/// in damage and in the kill credit that decides who gets loot.
struct PoisonStack {
    Entity source = NULL_ENTITY;
    double perSecond = 0;
    double untilMillis = 0;
};

/// Damage-over-time and movement debuffs.
///
/// One component rather than one per affliction: they are read together every
/// tick, and splitting them would mean three archetype moves every time a mob
/// is poisoned, slowed and then recovers.
struct Afflictions {
    double poisonPerSecond = 0;
    double poisonUntilMillis = 0;
    Entity poisonSource = NULL_ENTITY;

    /// Concurrent per-source poison. A refresh from a source already in the
    /// list REPLACES both fields when the new bite outlasts the live one, so a
    /// weaker-but-longer bite dilutes the rate rather than being merged into
    /// it, and a shorter bite is ignored outright however strong it is.
    std::vector<PoisonStack> poisonStacks;

    double slowFactor = 1.0;      ///< multiplies speed; 1 = unaffected
    double slowUntilMillis = 0;

    bool poisoned(double nowMillis) const { return nowMillis < poisonUntilMillis && poisonPerSecond > 0; }
    bool slowed(double nowMillis) const { return nowMillis < slowUntilMillis && slowFactor < 1.0; }

    /// The live stack owned by `source`, or nullptr. Linear: a victim carries a
    /// handful of these at most, one per player currently fighting it.
    PoisonStack* stackFrom(Entity source) {
        for (PoisonStack& s : poisonStacks) {
            if (s.source == source) return &s;
        }
        return nullptr;
    }

    /// Drops lapsed stacks. Without this a boss accumulates one entry per
    /// player who has ever bitten it.
    void pruneStacks(double nowMillis) {
        std::size_t out = 0;
        for (std::size_t i = 0; i < poisonStacks.size(); ++i) {
            if (poisonStacks[i].untilMillis > nowMillis) poisonStacks[out++] = poisonStacks[i];
        }
        poisonStacks.resize(out);
    }
};

/// A killing blow the Second Chance talent turned into 1 HP.
///
/// Only the lockout is stored: the tier (and therefore the invulnerability
/// window and the lockout length) is read from the player's own skill tree at
/// the moment the blow lands. Skills are disabled inside the PvP arena, so the
/// revive must not fire there either.
struct SecondChance {
    double readyAtMillis = 0;
};

/// Shell's temporary flat reduction for direct hits. It is refreshed rather
/// than stacked and does not deplete; poison/radiation bypass it.
struct ShieldState {
    double amount = 0;
    double untilMillis = 0;

    bool active(double nowMillis) const {
        return amount > 0.0 && nowMillis < untilMillis;
    }
};

struct SpongeDamageEffect {
    double remainingDamage = 0;
    double damagePerSecond = 0;
    Entity source = NULL_ENTITY;
};

/// Direct hits accepted while a live sponge is equipped are paid back over
/// time. Effects stack independently, exactly like ServerPlayer.spongeDamageEffects.
struct SpongeDamageState {
    std::vector<SpongeDamageEffect> effects;
};

/// XP awarded for landing the killing blow, and the ledger of who contributed.
///
/// Contributions are tracked so that loot and XP go to everyone who fought the
/// mob rather than whoever happened to land the last hit.
struct Bounty {
    double xp = 0;
    struct Share {
        Entity player = NULL_ENTITY;
        double damage = 0;
    };
    std::vector<Share> contributors;

    void credit(Entity player, double damage) {
        for (Share& s : contributors) {
            if (s.player == player) { s.damage += damage; return; }
        }
        contributors.push_back({player, damage});
    }
};

// ---------------------------------------------------------------------------
// Players
// ---------------------------------------------------------------------------

/// One equipped petal type. An empty slot has `configIndex == kNoPetal`.
inline constexpr std::uint16_t kNoPetal = 0xFFFF;

struct LoadoutSlot {
    std::uint16_t configIndex = kNoPetal;
    Rarity rarity = Rarity::Common;

    /// When a broken slot comes back. Held on the slot rather than the petal
    /// entity because the petal entity is destroyed while it is broken.
    double reloadReadyAtMillis = 0;
    bool broken = false;

    bool empty() const { return configIndex == kNoPetal; }
};

struct Loadout {
    std::array<LoadoutSlot, kLoadoutSlots> slots{};
    /// Petal entities currently spawned for each slot. A slot may own several
    /// (clumped petals such as sand spawn as a cluster).
    std::vector<Entity> spawned;
};

/// The orbiting ring's live geometry. On the player, not on each petal, so all
/// of a flower's petals necessarily agree about where the ring is.
struct PetalRing {
    double radius = kPetalOrbitRestRadius;
    double targetRadius = kPetalOrbitRestRadius;
    double spin = 0;       ///< current ring rotation, radians
    /// The multiplier the ring's rest radius is scaled by, 0.7 (defend) to 2.0
    /// (attack). It is the EXTENSION that ramps -- linearly, at 12 per second
    /// -- and the radius follows from it; easing the radius instead turns a
    /// tap-attack that should reach full extension in a twelfth of a second
    /// into a half-second swell.
    double extension = 1.0;
};

struct PlayerInput {
    net::InputFrame current;
    std::uint32_t lastAppliedSequence = 0;
    /// Where the cursor is in world space, derived from the input's aim angle
    /// and used for both movement and petal facing.
    Vec2 aimDirection{1, 0};
};

/// Account-scoped identity. Cold: read on join, on save, and when someone
/// types in chat.
struct PlayerAccount {
    std::string userId;
    std::string username;
    net::ConnectionId connection = 0;
    bool admin = false;

    /// This account's leaderboard reward tier, refreshed from the ranking on
    /// the owner's cadence rather than looked up per kill. The top accounts
    /// trade XP for drops, and the two halves are resolved differently: the XP
    /// factor applies per RECIPIENT of a kill, the drop factor once per mob
    /// from whoever led its damage.
    double xpMultiplier = 1.0;
    double dropMultiplier = 1.0;
};

/// The talent tree as the simulation sees it.
///
/// A copy of the account's tree rather than a pointer into the database: the
/// tick must not reach into storage, and a tree only changes when the player
/// buys a tier, at which point the owner writes this component too.
struct PlayerSkillTree {
    SkillSet skills;
};

/// The arena scoreboard. Only a flower fighting in the PVP ring carries one:
/// the score is the XP it has earned since it walked in, plus whatever a
/// flower it killed had built up (src/server/playerManager.ts:929,
/// src/server/playerState.ts:1354). It is what the arena leaderboard ranks.
struct ArenaScore {
    double score = 0;
};

struct PlayerProgress {
    double totalXp = 0;
    int level = 1;
    int stars = 0;
    /// Set when the level changed this tick so the networking layer can emit a
    /// LevelUp event without diffing.
    bool leveledThisTick = false;
};

/// Player body presentation that is not implied by a generic entity state.
///
/// `faceFlags` holds persistent/special-case bits (dandelion, square eyes and
/// corruption); poison, attack, defend and death are layered on by snapshot
/// construction. `equipFlags` is recomputed from the loadout by PetalSystem.
/// `glitched` is transient and is ORed into renderFlags for the wire, matching
/// TypeScript's effectiveRenderFlags() rather than persisting an affliction.
struct PlayerVisuals {
    std::uint8_t faceFlags = FaceNone;
    std::uint8_t equipFlags = EquipNone;
    std::uint32_t renderFlags = PlayerRenderNone;
    bool glitched = false;
    /// Set by the Flower petal's 5% break outcome. Distinct from `glitched`:
    /// corruption turns the flower hostile to everyone rather than only
    /// marking it, which is why it rides faceFlags and not renderFlags.
    bool corrupted = false;
};

/// Passive bonuses summed from equipped petals each tick.
///
/// Recomputed from scratch rather than applied incrementally on equip: an
/// incremental version has to unwind exactly what it applied, and a single
/// missed unwind is a permanent stat the player keeps forever.
struct PlayerModifiers {
    double maxHealthScale = 1.0;
    double speedScale = 1.0;
    /// The flower's BODY damage: equipped petal damage modifiers times the
    /// Damage talent on the gentle STAT curve (1.0 -> 1.9).
    double damageScale = 1.0;
    /// What a petal, a pollen puff, a radiation pulse or a projectile is
    /// multiplied by. A different quantity from `damageScale`: the reference
    /// puts petal output on the steep EFFECT curve (1.0 -> 4.8) and folds in
    /// NO petal damage modifier at all, so collapsing the two costs a fully
    /// talented ring roughly two and a half times its damage.
    double petalDamageScale = 1.0;
    double sizeScale = 1.0;
    double luck = 1.0;          ///< TypeScript's neutral luck value
    double magnetism = 0.0;     ///< adds to pickup radius
    double aggroRadiusBonus = 0.0;
    double petalAttractionRadius = 30.0; ///< base loose-petal attraction radius
    double rangeScale = 1.0;    ///< petal reach
    double cameraZoom = 1.0;
    double passiveHealPerSecond = 0.0;
    /// Flat poison DPS absorbed. Multiple lotus petals use the strongest one,
    /// rather than stacking.
    double poisonArmor = 0.0;
    double spongeDamageDurationMillis = 0.0;
};

/// Where the player is: the open world, or one of the detached regions.
enum class Region : std::uint8_t { Overworld = 0, Arena = 1 };

struct PlayerLocation {
    Region region = Region::Overworld;
    /// Viewport in world units, reported by the client, used to decide what to
    /// replicate. Clamped server-side -- a client claiming a 40000-unit
    /// viewport is asking to see the whole map.
    Vec2 viewport{kViewportWidth, kViewportHeight};
};

/// Progress toward a teleporter jump.
///
/// A pad has to be held, not crossed: the flower is sucked in from well beyond
/// the pad, has to stay on it for a full second, and is then locked out of
/// every pad -- suction included -- for five, so it does not bounce straight
/// back through the one it arrived on.
struct TeleporterState {
    /// Index into MapData's element list, or -1 when standing on no pad.
    int pad = -1;
    double enteredAtMillis = 0;
    double cooldownUntilMillis = 0;
};

/// Per-(player, victim) throttle for the raindrop petal's aura.
///
/// A separate component from HitCooldowns even though the shape is identical:
/// the flower already carries one of those for its own body contact, on a
/// different clock, and the two must not share entries.
struct AuraCooldowns {
    HitCooldowns hits;
};

// ---------------------------------------------------------------------------
// Mobs
// ---------------------------------------------------------------------------

struct MobType {
    std::uint16_t configIndex = 0;
    Rarity rarity = Rarity::Common;
    /// Per-spawn size multiplier on the tier's nominal size: the config's
    /// `random_size` roll, times the fixture ramp that keeps a high-rarity
    /// target dummy from becoming a wall. One number because the reference
    /// resolves both in one function (src/mobs.ts getEnemySizeScale) and
    /// everything downstream multiplies by exactly that.
    double sizeJitter = 1.0;
};

enum class AiKind : std::uint8_t {
    Passive = 0,    ///< never attacks; flees when hurt
    Neutral,        ///< retaliates, but does not seek
    Hostile,        ///< seeks players within its range
    Sandstorm,      ///< drifts on its own heading, ignoring players
    Stationary,     ///< a nest or hole: never moves, spawns escorts
};

struct MobAi {
    AiKind kind = AiKind::Neutral;
    Entity target = NULL_ENTITY;
    /// Where the mob was spawned; it wanders around this rather than drifting
    /// across the map over a long session.
    Vec2 anchor;
    double aggroRange = 0;
    double wanderAngle = 0;
    double nextDecisionMillis = 0;
    /// A heading clock independent of the decision clock. A sandstorm re-rolls
    /// its direction three times a second, far faster than a mob re-decides,
    /// and sharing one clock turns a churning storm into a smooth sweep.
    double nextHeadingMillis = 0;
    double lastAttackMillis = 0;
    /// Set while the mob is fleeing after being hurt (passive mobs).
    double fleeUntilMillis = 0;
    /// When this mob last let a volley go. Kept apart from `lastAttackMillis`,
    /// which paces contact damage: a hornet shoots on its config cooldown the
    /// whole way in and touches on a different clock once it arrives.
    double lastProjectileMillis = 0;
};

/// Where a mob is walking to, when it walks to a POINT rather than steering on
/// a heading.
///
/// Only three movers use this -- centipede heads, sandstorms and ownerless
/// pets. Every other mob with no target runs the stop-and-go passive machine
/// below instead, which has no destination at all.
struct WanderTarget {
    Vec2 destination;
    double pickedAtMillis = 0;
};

/// The two phases of the idle drift machine.
enum class PassiveState : std::uint8_t { Idle = 0, Moving };

/// The gardn stop-and-go drift every idle mob runs.
///
/// Not a wander heading held at constant speed: the mob sits still for a
/// second, picks a heading, coasts for half a second under friction alone, then
/// accelerates through a two-second parabolic ramp and stops. Acceleration
/// scales with the mob's radius so a big mob's hop covers ground in proportion
/// to its body rather than crawling.
struct PassiveMotion {
    PassiveState state = PassiveState::Idle;
    double stateStartMillis = 0;
    /// Drift velocity. Deliberately its own field rather than Motion: this
    /// integrator owns it, applies its own per-tick friction and its own clamp,
    /// and nothing else may write it.
    Vec2 velocity;
};

/// Per-mob phase offset for the bee cruise, so a field of bees weaves out of
/// step with itself instead of moving as one body.
struct Wobble {
    double phase = 0;
};

/// A child a nest (or a queen) put into the world, and its leash.
///
/// Dragged more than the retreat radius from its parent, the child forgets its
/// target and walks home at full chase speed, so a hole cannot be stripped of
/// its defenders by kiting them away. The parent is held as an entity rather
/// than a point because a queen moves.
struct HoleTether {
    Entity hole = NULL_ENTITY;
    /// Where to walk back to when the parent is gone but the tether has not
    /// been cleared yet.
    Vec2 home;
    bool returning = false;
};

/// A mob summoned by a player, which fights for them and does not drop loot.
struct Pet {
    Entity owner = NULL_ENTITY;
    /// Slot index of the petal that summoned it, so a broken petal can recall
    /// exactly the pets it owns.
    std::uint8_t slot = 0;
    /// The rarity of the petal that summoned it. NOT always the pet's own
    /// tier: an apex egg opens into three UNIQUE pets rather than one apex
    /// one, so the summoning tier has to be remembered separately to know how
    /// large the squad should be.
    Rarity summonRarity = Rarity::Common;
};

/// A segmented body (centipedes). Each segment follows the one ahead.
struct BodySegment {
    Entity ahead = NULL_ENTITY;
    Entity behind = NULL_ENTITY;
    /// Distance this segment holds behind the one in front.
    double spacing = 0;
    bool head = false;
    /// The chain's head, carried by every segment INCLUDING the head itself.
    /// Two segments of one centipede must not push each other apart, and that
    /// test has to be a field compare rather than a walk up the chain.
    Entity chainHead = NULL_ENTITY;
    /// 0 for the head, 1..n back along the body.
    int segmentIndex = 0;
};

/// A nest that periodically produces escorts, up to a live cap.
struct Spawner {
    std::uint16_t childConfigIndex = 0;
    int rarityOffset = 0;
    double intervalMillis = 2000;
    double nextSpawnMillis = 0;
    double childLifetimeMillis = 0;
    int maxAlive = 5;
    std::vector<Entity> children;
};

// ---------------------------------------------------------------------------
// Petals and projectiles
// ---------------------------------------------------------------------------

struct PetalInstance {
    Entity owner = NULL_ENTITY;
    std::uint16_t configIndex = 0;
    Rarity rarity = Rarity::Common;
    std::uint8_t slot = 0;
    /// Index within a clumped slot, used to fan the cluster out around its
    /// shared ring position.
    std::uint8_t subIndex = 0;
    std::uint8_t subCount = 1;
    /// Angle offset from the ring's rotation, fixed at spawn.
    double ringOffset = 0;
    /// The direction this INSTANCE faces, refreshed by the ring step. It is the
    /// slot's bearing for a lone petal, and the outward sub-bearing that places
    /// the grain inside its cluster for a clumped one -- so a clump of four
    /// peas faces four ways. `Transform::angle` stays the slot bearing because
    /// that is what the clump is drawn against; anything a grain LAUNCHES goes
    /// down this instead.
    double facingAngle = 0;
    /// Projectile firing has its own attack-gated clock. Keeping it separate
    /// means an idle ring does not spend a shot cooldown, and an aura on the
    /// same petal cannot delay its projectile.
    double nextProjectileMillis = 0;
    double spawnedAtMillis = 0;
    bool homing = false;
    /// Next time this petal may heal, emit a field, or maintain summons.
    double nextActionMillis = 0;

    /// Ring physics. The petal is sprung toward its orbit point rather than
    /// pinned to it, so `Transform::position` is the integrated position and
    /// this is the velocity carrying it there.
    Vec2 ringVelocity;
    /// While this has not passed the petal eases toward its target with a
    /// first-order approach instead of the spring: the fly-out from the flower
    /// when it appears, and the release after a mob it was orbiting dies.
    double glideUntilMillis = 0;
    /// The mob this petal is currently whipping around, or NULL_ENTITY. Kept
    /// so that losing the lock because the mob DIED can arm the release glide,
    /// while losing it because the mob walked away simply lets the spring pull
    /// the petal home.
    Entity attractedTo = NULL_ENTITY;

    /// Set once this petal has run a behaviour that waits for its first mob
    /// contact -- lightning's strike, a bomb's detonation, the flower petal
    /// cracking open. Without it the behaviour fires on every overlapping tick
    /// instead of once per life.
    bool collisionFired = false;
};

struct Projectile {
    Entity owner = NULL_ENTITY;
    /// The player credited with any kill, which is not the owner when the
    /// shooter was a pet.
    Entity creditTo = NULL_ENTITY;
    double damage = 0;
    double remainingDistance = 0;
    std::uint16_t petalConfigIndex = kNoPetal;
    Rarity rarity = Rarity::Common;
    /// Homing cone and range, both zero for a dumb projectile.
    double seekRange = 0;
    double seekCone = 0;
    /// Fired by a glitch-family mob: touching this shot leaves the flower
    /// glitched. Stamped at spawn from the shooter's config, as the reference
    /// stamps `sourceType`, so a shot still infects after its shooter is gone
    /// and never asks a recycled entity id what it was.
    bool glitchInfecting = false;
};

struct Lifetime {
    double remainingSeconds = 0;
};

// ---------------------------------------------------------------------------
// Drops and ground effects
// ---------------------------------------------------------------------------

struct DropItem {
    std::uint16_t configIndex = 0;
    Rarity rarity = Rarity::Common;
    /// Only these players may pick it up. Each eligible player receives one
    /// copy; the world entity remains for the others until all have collected.
    std::vector<Entity> eligible;
    std::vector<Entity> pickedUpBy;
};

enum class GroundEffectKind : std::uint8_t { Poison = 0, Web = 1, Radiation = 2 };

struct GroundEffect {
    GroundEffectKind kind = GroundEffectKind::Poison;
    Entity owner = NULL_ENTITY;
    double radius = 0;
    double damagePerSecond = 0;
    double slowFactor = 1.0;
    Rarity rarity = Rarity::Common;
    /// Pollen deals one discrete hit per victim on a fixed cadence; radiation
    /// instead uses damagePerSecond every simulation step.
    double damagePerHit = 0;
    double damageIntervalMillis = 0;
};

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------

/// A stable id for an entity on the wire.
///
/// Distinct from Entity: an Entity handle encodes a slot that gets recycled,
/// and a client that missed a removal would otherwise apply an update meant
/// for a new entity to the corpse of the old one. Net ids are never reused
/// within a session.
struct NetId {
    std::uint32_t value = 0;
};

/// What the client needs in order to draw this entity, and what changed.
struct Replicated {
    net::EntityKind kind = net::EntityKind::Mob;
    std::uint8_t state = 0;      ///< net::EntityState bits, rebuilt each tick
    std::uint16_t typeIndex = 0; ///< index into the mob or petal config table
    Rarity rarity = Rarity::Common;
    std::uint8_t spawnFlags = 0;
};

} // namespace flix

// Component registration. Global scope, once per type.
FLIX_COMPONENT(flix::PlayerTag);
FLIX_COMPONENT(flix::MobTag);
FLIX_COMPONENT(flix::PetalTag);
FLIX_COMPONENT(flix::ProjectileTag);
FLIX_COMPONENT(flix::DropTag);
FLIX_COMPONENT(flix::GroundEffectTag);
FLIX_COMPONENT(flix::Dead);
FLIX_COMPONENT(flix::Transform);
FLIX_COMPONENT(flix::Motion);
FLIX_COMPONENT(flix::Body);
FLIX_COMPONENT(flix::Knockback);
FLIX_COMPONENT(flix::Faction);
FLIX_COMPONENT(flix::Health);
FLIX_COMPONENT(flix::ContactDamage);
FLIX_COMPONENT(flix::HitCooldowns);
FLIX_COMPONENT(flix::Afflictions);
FLIX_COMPONENT(flix::ShieldState);
FLIX_COMPONENT(flix::SpongeDamageState);
FLIX_COMPONENT(flix::SecondChance);
FLIX_COMPONENT(flix::Bounty);
FLIX_COMPONENT(flix::Loadout);
FLIX_COMPONENT(flix::PetalRing);
FLIX_COMPONENT(flix::PlayerInput);
FLIX_COMPONENT(flix::PlayerAccount);
FLIX_COMPONENT(flix::PlayerSkillTree);
FLIX_COMPONENT(flix::PlayerProgress);
FLIX_COMPONENT(flix::ArenaScore);
FLIX_COMPONENT(flix::PlayerVisuals);
FLIX_COMPONENT(flix::PlayerModifiers);
FLIX_COMPONENT(flix::PlayerLocation);
FLIX_COMPONENT(flix::TeleporterState);
FLIX_COMPONENT(flix::AuraCooldowns);
FLIX_COMPONENT(flix::MobType);
FLIX_COMPONENT(flix::MobAi);
FLIX_COMPONENT(flix::WanderTarget);
FLIX_COMPONENT(flix::PassiveMotion);
FLIX_COMPONENT(flix::Wobble);
FLIX_COMPONENT(flix::HoleTether);
FLIX_COMPONENT(flix::Pet);
FLIX_COMPONENT(flix::BodySegment);
FLIX_COMPONENT(flix::Spawner);
FLIX_COMPONENT(flix::PetalInstance);
FLIX_COMPONENT(flix::Projectile);
FLIX_COMPONENT(flix::Lifetime);
FLIX_COMPONENT(flix::DropItem);
FLIX_COMPONENT(flix::GroundEffect);
FLIX_COMPONENT(flix::NetId);
FLIX_COMPONENT(flix::Replicated);
