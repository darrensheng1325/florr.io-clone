#pragma once
// Combat: every path in the game that removes health, and the death that
// follows.
//
// The whole point of this file is that there is exactly ONE function that
// writes Health::current -- applyDamage(). Invulnerability, the same-team
// refusal, the hurt flash, the contribution ledger, the damage event and the
// transition to Dead are consequences of a hit, and a consequence that lives
// in only one place cannot be forgotten by the next damage source somebody
// adds. No other system may touch Health::current; it should reach for
// applyDamage instead.
//
// STRUCTURAL TRAP: applyDamage() adds the Dead tag, which relocates the victim
// between archetypes and invalidates every column pointer a Query::each is
// holding. It must therefore never be called from inside each(). Every pass
// below is written as gather-then-apply for exactly this reason, and any
// system that wants to deal damage must do the same.

#include <cstdint>
#include <memory>
#include <vector>

#include "server/replication.h"
#include "server/squads.h"
#include "shared/core/types.h"
#include "shared/core/world.h"
#include "shared/game/components.h"
#include "shared/game/config.h"
#include "shared/game/rarity.h"
#include "shared/game/spatial.h"

namespace flix {

/// How long a hit keeps the client's white flash lit. Short: it reads as an
/// impact rather than a status, and it is refreshed by every landed hit, so a
/// sustained beating stays lit without anything having to track that.
inline constexpr double kHurtFlashMillis = 120.0;

/// TypeScript's fixed player-vs-mob contact displacement, in world units.
/// It is neither mass-scaled nor converted into velocity.
inline constexpr double kMobContactKnockback = 25.0;

/// How long a petal waits between swings at the SAME flower.
///
/// Against a mob an ordinary petal is not throttled at all; against a duellist
/// it is, and per victim rather than per petal, so one petal can reach two
/// flowers in a tick but neither of them twice. A petal that declares its own
/// `damageCooldown` uses that instead.
inline constexpr double kPvpPetalHitIntervalMillis = 250.0;

/// What a swing at another flower costs the petal: a flat point, never the
/// victim's damage stat. A flower's body damage would shatter a common ring on
/// the first hit, which is why the reference charges a fixed number here and
/// the mob's own damage against a mob.
inline constexpr double kPvpPetalSelfDamage = 1.0;

/// How long a ground effect's slow outlives standing in it. Refreshed every
/// tick while inside, so this is only the tail after walking out -- long
/// enough that the debuff does not strobe at the boundary, short enough that
/// escaping a web means something.
inline constexpr double kGroundEffectSlowLingerMillis = 250.0;
inline constexpr double kPostHitInvulnerabilityMillis = 50.0;

/// Slack added to every broadphase query, in world units.
///
/// The exact circle test decides what was hit. This slack also covers bodies
/// whose own radius is not represented in a point-centred field query and
/// protects focused tests that intentionally build a minimal broadphase.
inline constexpr double kBroadphasePad = 24.0;

/// Ticks between HitCooldowns sweeps. The entries are only a correctness
/// concern while they are in the future; the sweep exists so a petal that has
/// grazed ten thousand mobs is not still carrying all ten thousand.
inline constexpr int kCooldownPruneTicks = 50;

/// Depth limit when resolving a petal/pet/projectile back to the player behind
/// it. A cycle in the owner links would otherwise hang the tick, and this is
/// a wire-fed graph -- it is not allowed to be able to.
inline constexpr int kMaxOwnerHops = 8;

/// Why damage is being applied, which decides how the client hears about it.
///
/// The reference narrates damage on exactly two channels -- `enemiesDamaged`
/// for mobs, `playerDamaged` for flowers -- and both carry a number for a
/// poison tick and for a sponge repayment exactly as they do for a petal hit.
/// What the kind actually decides is the white flash, which is also the signal
/// a neutral mob retaliates on, and the colour the number is drawn in.
enum class DamageKind : std::uint8_t {
    Direct = 0,     ///< a landed hit: flashes, and so provokes a neutral mob
    Periodic = 1,   ///< a drip, reported in the ordinary colour
    Poison = 2,     ///< a drip the client tints purple and offsets sideways
};

struct DamageResult {
    double applied = 0;      ///< health actually removed, after clamping to what was left
    bool killed = false;     ///< this application is the one that marked Dead
    bool refused = false;    ///< nothing happened: invulnerable, same side, already dead
};

class CombatSystem {
public:
    CombatSystem();
    ~CombatSystem();
    CombatSystem(const CombatSystem&) = delete;
    CombatSystem& operator=(const CombatSystem&) = delete;

    /// Who ranks together when a mob's XP is shared out. Null means nobody is
    /// squadded, which is the ordinary case; the loot system reads the same
    /// table so the two payouts cannot disagree about who earned the kill.
    const SquadEntityIndex* squads = nullptr;

    /// One complete combat tick. Kept for focused simulations; GameServer uses
    /// the three phase methods below so flower/petal contact can happen before
    /// mob movement while projectiles and ground effects happen after it.
    void run(World& world, const SpatialGrid& grid, const ContentRegistry& content,
             double nowMillis, double dt, CommandBuffer& commands, EventQueue& events);

    /// Opens a combat tick and applies poison/status expiry. Call exactly once.
    void beginTick(World& world, double nowMillis, double dt, EventQueue& events);

    /// Resolves body and petal contact from the pre-mob-movement world, which
    /// is where the TypeScript player pipeline performs those collisions.
    void runContactPhase(World& world, const SpatialGrid& grid,
                         const ContentRegistry& content, double nowMillis);

    /// Resolves post-movement projectiles/ground fields and closes the tick.
    void runWorldPhase(World& world, const SpatialGrid& grid,
                       const ContentRegistry& content, double nowMillis, double dt);

    /// The single damage path. Returns what actually landed.
    ///
    /// `source` is whatever dealt the hit -- a mob, a petal, a projectile, a
    /// ground effect -- not necessarily a player; the player to credit is
    /// resolved from it. NULL_ENTITY is a legitimate source and means the
    /// environment, which no faction rule protects anyone from.
    ///
    /// Not const-safe against iteration: see the structural trap above.
    DamageResult applyDamage(World& world, Entity victim, Entity source, double amount,
                             double nowMillis, DamageKind kind = DamageKind::Direct);

    /// Queue the TypeScript petal/projectile mob push: direction times
    /// `strength / victimMass`. This REPLACES a prior queued push, matching
    /// `setMobKnockback()` rather than accumulating momentum.
    void applyKnockback(World& world, Entity victim, Vec2 offset, double strength);

    /// A mob holds one poison stack per poisoning PLAYER and every one of them
    /// ticks, so two flowers biting the same boss deal both their rates and
    /// each is credited its own share. A refresh from a source that already
    /// holds a stack takes over only when it would OUTLAST the live one, and
    /// then it replaces the RATE as well -- a weaker but longer bite dilutes,
    /// and a shorter one is ignored however strong it is.
    ///
    /// A flower is different on purpose: it carries exactly one bite, replaced
    /// outright by the next one even when that shortens it.
    void applyPoison(World& world, Entity victim, Entity source, double perSecond,
                     double durationMillis, double nowMillis);

    /// A slow never weakens while it is live: the deeper factor wins and the
    /// expiry never comes closer, so a common petal grazing a mob cannot wipe
    /// the mythic stall already on it.
    ///
    /// Only a MOB can be slowed. applyMobSlow() is the reference's single slow
    /// implementation -- the petal bridge and the web field both resolve to it
    /// -- and it opens by refusing anything that is not a mob, so no web, honey
    /// petal or pincer has ever taken a flower's speed away.
    void applySlow(World& world, Entity victim, double factor, double durationMillis,
                   Rarity sourceRarity, double nowMillis);

    /// The PLAYER answerable for what `source` does: through Projectile::
    /// creditTo, Pet::owner, PetalInstance::owner and GroundEffect::owner,
    /// transitively. NULL_ENTITY when nothing player-owned is behind it, which
    /// is the normal answer for a wild mob.
    static Entity creditedPlayer(const World& world, Entity source);

    /// Whether `source` is allowed to hurt `victim` at all: different sides,
    /// or the same side with friendly fire on. Two things resolving to the
    /// same player never hurt each other, which is what stops a flower's own
    /// petals and pets from killing it the moment PvP is enabled.
    static bool canDamage(const World& world, Entity source, Entity victim);

    /// canDamage() plus the victim's own state: alive, not already Dead, and
    /// past its respawn invulnerability. Shared with the contact path so that
    /// a refused hit consumes no cooldown and lands no poison either.
    static bool canHit(const World& world, Entity victim, Entity source, double nowMillis);

    /// Deaths marked during the last run(), oldest first. The loot system
    /// reads the ledger off the corpse itself (Bounty survives until the
    /// reaper runs at the end of the tick); this is for the server and for
    /// tests that want the list without a query.
    struct DeathRecord {
        Entity entity = NULL_ENTITY;
        Entity killer = NULL_ENTITY;
        bool wasPlayer = false;
    };
    const std::vector<DeathRecord>& deaths() const { return deaths_; }

private:
    /// A touching body about to be tested against everything near it. Mob
    /// bodies, flower bodies and petals all reduce to this, so there is one
    /// overlap-and-cooldown loop rather than three that drift apart.
    struct MeleeSource {
        Entity attacker = NULL_ENTITY;
        Vec2 position;
        double radius = 0;
        double damage = 0;
        double hitIntervalMillis = kMobHitIntervalMillis;
        double knockback = 0;
        double poisonPerSecond = 0;
        double poisonDurationMillis = 0;
        double slowFactor = 1.0;
        double slowDurationMillis = 0;
        Rarity rarity = Rarity::Common;
        /// What kind of body this is, decided once in the gather rather than
        /// re-derived per candidate. The throttle, the reciprocal petal bleed,
        /// the pet/wild contact gap and the one-mob-per-tick body rule all key
        /// off these.
        bool isPetal = false;
        bool isMobBody = false;
        bool isPet = false;
        bool isPlayerBody = false;
    };

    /// A flower's raindrop field, resolved once per tick.
    ///
    /// Raindrop is not an orbiting petal that happens to hurt: it projects a
    /// damaging circle from the flower itself, so it has no petal entity to
    /// hang a MeleeSource on and its own reach rather than the ring's.
    struct AuraSource {
        Entity player = NULL_ENTITY;
        Vec2 position;
        double radius = 0;
        double damage = 0;
    };

    struct FieldSource {
        Entity effect = NULL_ENTITY;
        GroundEffectKind kind = GroundEffectKind::Poison;
        Vec2 position;
        double radius = 0;
        double damagePerSecond = 0;
        double slowFactor = 1.0;
        Rarity rarity = Rarity::Common;
        double damagePerHit = 0;
        double damageIntervalMillis = 0;
    };

    struct ShotSource {
        Entity entity = NULL_ENTITY;
        Vec2 position;
        double radius = 0;
        double travelled = 0;
    };

    struct PoisonTick {
        Entity victim = NULL_ENTITY;
        Entity source = NULL_ENTITY;
        double amount = 0;
    };

    struct Queries;

    /// Queries cache the archetypes they match, so they are built once and
    /// reused. The world is not known until the first run(), and a test may
    /// hand over a different one, hence the rebind rather than a member.
    void bind(World& world);

    void tickAfflictions(World& world, double nowMillis, double dt);
    void tickSpongeDamage(World& world, double nowMillis, double dt);
    /// Takes the registry because a pollen puff reaches for the victim's
    /// CONFIG radius rather than the body it actually spawned with.
    void tickGroundEffects(World& world, const SpatialGrid& grid, const ContentRegistry& content,
                           double nowMillis, double dt);
    /// The strongest equipped raindrop on every live flower, and the mobs its
    /// field chips. Split in two for the usual reason: resolving a hit can
    /// mark a mob Dead, which relocates the row the gather is walking.
    void gatherAuras(World& world, const ContentRegistry& content);
    void resolveAuras(World& world, const SpatialGrid& grid, double nowMillis);
    void gatherContact(World& world, const ContentRegistry& content);
    void gatherPetals(World& world, const ContentRegistry& content);
    void resolveMelee(World& world, const SpatialGrid& grid, double nowMillis);
    /// A petal swinging at another flower, which is a different collision from
    /// the petal-vs-mob one beside it: gated by the arena/corruption rule
    /// rather than by the faction alone, throttled per victim, costing the
    /// petal a flat point, and shoving the victim away from the FLOWER rather
    /// than away from the petal. It carries neither poison nor slow.
    void resolvePetalPvp(World& world, const MeleeSource& source, Entity victim,
                         Vec2 victimPosition, double victimRadius, double nowMillis);
    void tickProjectiles(World& world, const SpatialGrid& grid, const ContentRegistry& content,
                         double nowMillis, double dt);
    void awardBounty(World& world, Entity victim);

    /// Turns a killing blow on a flower into 1 HP plus the talent's own
    /// invulnerability, or leaves it lethal. It lives behind applyDamage so
    /// that body contact, a petal ring, a poison tick and a sponge repayment
    /// are all covered by one lookup rather than by five call sites that each
    /// have to remember.
    bool trySecondChance(World& world, Entity victim, double nowMillis);

    std::unique_ptr<Queries> queries_;
    World* boundWorld_ = nullptr;
    EventQueue* events_ = nullptr;

    std::vector<MeleeSource> melee_;
    std::vector<AuraSource> auras_;
    std::vector<FieldSource> fields_;
    std::vector<ShotSource> shots_;
    std::vector<PoisonTick> poison_;
    std::vector<PoisonTick> spongeTicks_;
    std::vector<Entity> candidates_;
    /// TypeScript stops after the first legitimate mob body-contact for a
    /// player each tick. Reused rather than allocated in resolveMelee().
    std::vector<Entity> mobContactedPlayers_;
    std::vector<DeathRecord> deaths_;

    std::uint64_t tick_ = 0;
};

} // namespace flix
