#include "server/systems/combat.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "server/loot_eligibility.h"

namespace flix {

namespace {

/// The entity one step up the ownership chain, or NULL_ENTITY at the top.
///
/// A projectile prefers creditTo over owner because a pet's shot is fired by
/// the pet but answerable to the player; every other link has one owner.
Entity ownerOf(const World& world, Entity e) {
    if (const Projectile* projectile = world.tryGet<Projectile>(e)) {
        return projectile->creditTo != NULL_ENTITY ? projectile->creditTo : projectile->owner;
    }
    if (const PetalInstance* petal = world.tryGet<PetalInstance>(e)) return petal->owner;
    if (const Pet* pet = world.tryGet<Pet>(e)) return pet->owner;
    if (const GroundEffect* effect = world.tryGet<GroundEffect>(e)) return effect->owner;
    return NULL_ENTITY;
}

struct TeamInfo {
    Team team = Team::Neutral;
    bool friendlyFire = false;
    bool known = false;
};

/// The side an entity fights on, walking up to whoever owns it.
///
/// A petal carries no Faction of its own: it is on its flower's side by
/// definition, and giving each of the eight a copy would be eight more fields
/// to keep in step with a player joining a PvP arena.
TeamInfo teamOf(const World& world, Entity e) {
    Entity current = e;
    for (int hop = 0; hop < kMaxOwnerHops && current != NULL_ENTITY; ++hop) {
        if (const Faction* faction = world.tryGet<Faction>(current)) {
            return {faction->team, faction->friendlyFireEnabled, true};
        }
        current = ownerOf(world, current);
    }
    return {};
}

/// Whether a flower has been turned hostile to every other flower.
///
/// Corruption is the Flower petal's 5% break outcome and it is symmetric on
/// purpose: a corrupted flower may attack anyone anywhere in the world and
/// anyone may fight it back, because a one-way version would leave it
/// untouchable by everything except mobs.
bool isCorrupted(const World& world, Entity player) {
    if (player == NULL_ENTITY) return false;
    const PlayerVisuals* visuals = world.tryGet<PlayerVisuals>(player);
    return visuals != nullptr && visuals->corrupted;
}

/// Whether one flower's petals may swing at another's.
///
/// The two ways are not the same shape. BOTH duellists must be in the arena --
/// friendlyFireEnabled is the flag the arena sets -- while EITHER side being
/// corrupted is enough. Nothing here looks at distance: the caller only
/// reaches it after an overlap test, and the arena sits far enough from the
/// world that no pair spanning the two could ever touch.
bool canPetalsDamagePlayer(const World& world, Entity attacker, Entity victim) {
    if (isCorrupted(world, attacker) || isCorrupted(world, victim)) return true;
    const Faction* attackerFaction = world.tryGet<Faction>(attacker);
    const Faction* victimFaction = world.tryGet<Faction>(victim);
    return attackerFaction != nullptr && attackerFaction->friendlyFireEnabled &&
           victimFaction != nullptr && victimFaction->friendlyFireEnabled;
}

/// The actor behind a hit: a shot's mob, a petal's flower, a pet's owner.
///
/// Damage arrives from the thing that touched the victim, but "who killed me"
/// has to name something the player recognises -- a projectile handle is
/// already gone by the time the Died message is written.
Entity attributedSource(const World& world, Entity source) {
    Entity current = source;
    for (int hop = 0; hop < kMaxOwnerHops; ++hop) {
        const Entity owner = ownerOf(world, current);
        if (owner == NULL_ENTITY || !world.isAlive(owner)) return current;
        current = owner;
    }
    return current;
}

/// The tier a slow is landing against. Players have no rarity of their own and
/// count as common, so stallPower() neither helps nor hinders a slow on them.
Rarity rarityOf(const World& world, Entity e) {
    if (const MobType* type = world.tryGet<MobType>(e)) return type->rarity;
    if (const Replicated* replicated = world.tryGet<Replicated>(e)) return replicated->rarity;
    return Rarity::Common;
}

/// Whether a projectile should test against this entity at all. Drops and
/// ground effects are in the broadphase because they have bodies, not because
/// they are targets.
///
/// Other SHOTS are targets, and deliberately so: two of them meeting is the
/// bullet-vs-bullet rule, and canDamage() is what keeps a volley from
/// detonating against itself -- both shots resolve to the same player or the
/// same team and are refused before any damage is exchanged.
bool isShootable(const World& world, Entity e) {
    return !world.has<DropTag>(e) && !world.has<GroundEffectTag>(e);
}

/// What being hit COSTS the shot that hit it.
///
/// Symmetric with the damage flowing the other way: a shot pays the victim's
/// body damage, and a victim that happens to be another shot charges its own
/// damage stat. That is the whole of penetration -- nothing counts hits, the
/// pool simply runs out.
///
/// A PETAL charges its damage stat, the same way another shot does. A ring
/// eating an incoming volley is a real defence and it has to survive this
/// change: a basic petal's ten points is a common missile's whole pool, so the
/// ring still stops what it always stopped, and only a shot fat enough to
/// outlast it now gets through. It is read off the config rather than off a
/// component because a petal carries no ContactDamage -- the melee pass
/// resolves a ring's damage from the registry too.
///
/// A FLOWER is the exception and returns infinity. It has no body damage to
/// charge with, and post-hit invulnerability means a shot that survived would
/// sit inside the victim until its range expired rather than landing again.
/// Mob shots have always been consumed by the flower they hit, and they still
/// are.
double bodyDamageOf(const World& world, const ContentRegistry& content, Entity victim) {
    if (world.has<PlayerTag>(victim)) return std::numeric_limits<double>::infinity();
    if (const Projectile* shot = world.tryGet<Projectile>(victim)) return shot->damage;
    if (const PetalInstance* petal = world.tryGet<PetalInstance>(victim)) {
        const double damage = content.petalStats(petal->configIndex, petal->rarity).damage;
        return damage > 0.0 ? damage : kProjectileDefaultBodyDamage;
    }
    if (const ContactDamage* contact = world.tryGet<ContactDamage>(victim)) return contact->amount;
    return kProjectileDefaultBodyDamage;
}

/// Fold the live poison stacks back into the scalar summary on Afflictions.
///
/// The stacks are what tick; the summary exists because replication asks
/// Afflictions::poisoned() whether to light the poisoned state bit, and
/// because "how fast is this mob losing health" is worth having in one place.
/// Rates ADD, exactly as poisonStackSystem applies every live stack in turn.
void summarisePoison(Afflictions& afflictions, double nowMillis) {
    double total = 0;
    double until = 0;
    double strongest = 0;
    Entity owner = NULL_ENTITY;
    for (const PoisonStack& stack : afflictions.poisonStacks) {
        if (stack.untilMillis <= nowMillis || stack.perSecond <= 0.0) continue;
        total += stack.perSecond;
        until = std::max(until, stack.untilMillis);
        // Named for the client and for a kill notice, both of which want one
        // culprit; the ledger credit is per stack and does not go through here.
        if (stack.perSecond > strongest) {
            strongest = stack.perSecond;
            owner = stack.source;
        }
    }
    afflictions.poisonPerSecond = total;
    afflictions.poisonUntilMillis = until;
    afflictions.poisonSource = owner;
}

/// The victim radius a ground field reaches with, which is deliberately not
/// the same number for every kind.
///
/// A pollen puff tests the mob's CONFIG radius for its tier -- the reference
/// kept that from the legacy loop and says so at ecsRuntime.ts:745 -- so a mob
/// that rolled a big body is no easier to dust than one that rolled a small
/// one. A web and a uranium cloud test the body that is actually there.
double fieldTargetRadius(const World& world, const ContentRegistry& content, Entity victim,
                         GroundEffectKind kind, const Body* body) {
    if (kind == GroundEffectKind::Poison) {
        if (const MobType* type = world.tryGet<MobType>(victim)) {
            return content.mobStats(type->configIndex, type->rarity).radius;
        }
    }
    return body != nullptr ? body->radius : 0.0;
}

/// What a petal pays to land a hit: the mob's own `damage` stat, which is
/// exactly what its body would have dealt anyway. One, not zero, for a victim
/// carrying no stats at all -- the reference's explicit fallback.
double contactDamageOf(const World& world, Entity mob) {
    const ContactDamage* contact = world.tryGet<ContactDamage>(mob);
    return contact != nullptr ? contact->amount : 1.0;
}

/// Write a swing into the victim's contributor ledger.
///
/// The number SWUNG, never the number that fitted in the health that was left.
/// trackDamage() stores what the caller computed, and the ledger is what ranks
/// contributors for the mob's four to twenty-five loot slots -- clamping to the
/// remainder would rank whoever lands the killing blow by the sliver they took
/// rather than by the hit they threw, and sort them under the chip damage that
/// softened it up.
void creditSwing(World& world, Entity victim, Entity source, double amount) {
    Bounty* bounty = world.tryGet<Bounty>(victim);
    if (bounty == nullptr) return;
    const Entity credited = CombatSystem::creditedPlayer(world, source);
    if (credited != NULL_ENTITY) bounty->credit(credited, amount);
}

} // namespace

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

struct CombatSystem::Queries {
    explicit Queries(World& world)
        : progress(world), afflicted(world), auras(world), contact(world), petals(world),
          projectiles(world), fields(world), cooldowns(world), auraCooldowns(world) {
        // A dead flower projects nothing, which is the same guard the
        // reference's pre-movement pass opens with.
        auras.without<Dead>();
        // A corpse stops fighting the instant it is marked, not at the end of
        // the tick when the reaper gets to it -- otherwise a mob killed by the
        // first petal in the ring still gets its contact hit in.
        afflicted.without<Dead>();
        // Petals and projectiles carry their damage in their own config and
        // component; a ContactDamage on either would otherwise be counted a
        // second time by this pass.
        contact.without<PetalInstance, Projectile, Dead>();
        petals.without<Dead>();
        projectiles.without<Dead>();
        fields.without<Dead>();
    }

    Query<PlayerProgress> progress;
    Query<Afflictions, Health> afflicted;
    /// Flowers that might be wearing a raindrop. Every loadout is walked each
    /// tick rather than cached: a slot's petal, tier and broken flag all change
    /// under the ring pipeline, and ten array reads are cheaper than keeping a
    /// second copy of them honest.
    Query<PlayerTag, Loadout, Transform> auras;
    /// Petals are excluded and handled separately: their damage, reload and
    /// riders all come out of the petal config, and reading it once beats
    /// mirroring six numbers onto every petal entity every tick.
    Query<ContactDamage, Transform, Body> contact;
    Query<PetalInstance, Transform, Body> petals;
    Query<Projectile, Transform, Body, Motion> projectiles;
    Query<GroundEffect, Transform> fields;
    Query<HitCooldowns> cooldowns;
    Query<AuraCooldowns> auraCooldowns;
};

CombatSystem::CombatSystem() = default;
CombatSystem::~CombatSystem() = default;

void CombatSystem::bind(World& world) {
    if (boundWorld_ == &world && queries_) return;
    queries_ = std::make_unique<Queries>(world);
    boundWorld_ = &world;
}

// ---------------------------------------------------------------------------
// Faction and credit
// ---------------------------------------------------------------------------

Entity CombatSystem::creditedPlayer(const World& world, Entity source) {
    Entity current = source;
    for (int hop = 0; hop < kMaxOwnerHops && current != NULL_ENTITY; ++hop) {
        if (!world.isAlive(current)) return NULL_ENTITY;
        if (world.has<PlayerTag>(current)) return current;
        current = ownerOf(world, current);
    }
    return NULL_ENTITY;
}

bool CombatSystem::canDamage(const World& world, Entity source, Entity victim) {
    if (victim == NULL_ENTITY || source == victim) return false;

    // Resolving BOTH sides to the player behind them covers every self-harm
    // pair at once -- petal vs own flower, pet vs own petal, shot vs the pet
    // that fired it -- without a rule per pair.
    const Entity sourcePlayer = source != NULL_ENTITY ? creditedPlayer(world, source)
                                                      : NULL_ENTITY;
    const Entity victimPlayer = creditedPlayer(world, victim);
    if (sourcePlayer != NULL_ENTITY && sourcePlayer == victimPlayer) return false;

    const TeamInfo attacker = teamOf(world, source);
    const TeamInfo defender = teamOf(world, victim);
    // An entity with no side at all is scenery or a hazard: it hurts, and is
    // hurt by, everything. Refusing here instead would silently disarm any
    // spawner that forgot a Faction.
    if (!attacker.known || !defender.known) return true;
    if (attacker.team != defender.team) return true;
    // Corruption is checked on the resolved PLAYER and not on the Faction
    // beside it, because a petal is spawned with a copy of its flower's
    // Faction: a flower corrupted after its ring was strung would otherwise
    // keep swinging harmlessly.
    if (isCorrupted(world, sourcePlayer) || isCorrupted(world, victimPlayer)) return true;
    // Either side being in a PvP region is enough. Requiring both would leave
    // a duellist's pets -- which carry no flag of their own -- unable to fight.
    return attacker.friendlyFire || defender.friendlyFire;
}

bool CombatSystem::canHit(const World& world, Entity victim, Entity source, double nowMillis) {
    if (!world.isAlive(victim) || world.has<Dead>(victim)) return false;
    const Health* health = world.tryGet<Health>(victim);
    if (!health) return false;
    // A body already at zero is a corpse this tick has not reaped yet. Hitting
    // it again would mark it Dead a second time and pay its bounty twice.
    if (health->current <= 0.0) return false;
    if (nowMillis < health->invulnerableUntilMillis) return false;
    return canDamage(world, source, victim);
}

// ---------------------------------------------------------------------------
// The one damage path
// ---------------------------------------------------------------------------

DamageResult CombatSystem::applyDamage(World& world, Entity victim, Entity source,
                                       double amount, double nowMillis, DamageKind kind) {
    DamageResult result;
    // Non-finite damage reaches here from a config that multiplied a zero by
    // an infinity; it must cost a refused hit, not a NaN health bar that no
    // comparison can ever bring back below zero.
    if (!std::isfinite(amount) || amount == 0.0) {
        result.refused = true;
        return result;
    }
    if (!canHit(world, victim, source, nowMillis)) {
        // One refusal still pays the ledger. Every reference path writes the
        // contribution BEFORE it tests whether the mob is already dead --
        // trackDamage() at playerState.ts:2751 and creditDamage() at
        // projectileCollision.ts:437 both sit above their `isDead` continue --
        // so a swing that lands on something another petal killed earlier in
        // this same tick still buys its dealer a loot slot. That is the
        // difference between two players sharing a kill and one of them
        // getting nothing.
        //
        // Only that refusal: a same-side hit never reaches trackDamage at all,
        // and there is no ledger left on an entity the reaper has taken.
        const Health* health = world.tryGet<Health>(victim);
        const bool corpse = world.isAlive(victim) && health != nullptr &&
                            (world.has<Dead>(victim) || health->current <= 0.0);
        if (corpse && canDamage(world, source, victim)) {
            creditSwing(world, victim, source, amount);
        }
        result.refused = true;
        return result;
    }

    // TypeScript's mob health writer is `max(0, health - amount)`.  Glitch is
    // authored with negative damage, so its contact deliberately heals mobs;
    // PVP's separate player damage path rejects non-positive values.  Preserve
    // that asymmetry instead of sanitising the content into another petal.
    if (amount < 0.0) {
        if (!world.has<MobTag>(victim)) {
            result.refused = true;
            return result;
        }
        Health& health = world.get<Health>(victim);
        health.current -= amount;
        if (kind == DamageKind::Direct) {
            health.flashUntilMillis = std::max(health.flashUntilMillis,
                                               nowMillis + kHurtFlashMillis);
        }
        result.applied = amount;
        creditSwing(world, victim, source, amount);
        return result;
    }

    // Shell's shield is a temporary flat reduction per DIRECT hit. It neither
    // depletes nor applies to poison/radiation, matching getShieldAmount() in
    // the TypeScript hit paths.
    const bool directPlayerHit = kind == DamageKind::Direct && world.has<PlayerTag>(victim);
    if (directPlayerHit) {
        if (ShieldState* shield = world.tryGet<ShieldState>(victim)) {
            if (shield->active(nowMillis)) amount = std::max(0.0, amount - shield->amount);
            else {
                shield->amount = 0;
                shield->untilMillis = 0;
            }
        }
    }
    if (amount <= 0.0) {
        // TypeScript still grants the brief post-hit protection after a shield
        // absorbs the full number; this was a legitimate hit, not a rejected
        // target. Callers therefore still arm their attacker cooldown.
        world.get<Health>(victim).invulnerableUntilMillis =
            std::max(world.get<Health>(victim).invulnerableUntilMillis,
                     nowMillis + kPostHitInvulnerabilityMillis);
        return result;
    }

    if (directPlayerHit) {
        const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(victim);
        const double durationMillis = modifiers ? modifiers->spongeDamageDurationMillis : 0.0;
        if (durationMillis > 0.0) {
            SpongeDamageEffect effect;
            effect.remainingDamage = amount;
            effect.damagePerSecond = amount / (durationMillis / 1000.0);
            effect.source = source;
            world.ensure<SpongeDamageState>(victim).effects.push_back(effect);
            Health& health = world.get<Health>(victim);
            health.invulnerableUntilMillis =
                std::max(health.invulnerableUntilMillis,
                         nowMillis + kPostHitInvulnerabilityMillis);
            return result;
        }
    }

    Health& health = world.get<Health>(victim);
    // What the health bar actually lost, which is what the caller is told and
    // what the floating number reads. The LEDGER is credited the full swing
    // instead -- see creditSwing().
    const double applied = std::min(amount, health.current);
    health.current -= applied;
    if (health.current < 0.0) health.current = 0.0;
    bool fatal = health.current <= 0.0;
    if (kind == DamageKind::Direct) {
        health.flashUntilMillis = std::max(health.flashUntilMillis, nowMillis + kHurtFlashMillis);
    }
    result.applied = applied;
    if (directPlayerHit) {
        health.invulnerableUntilMillis =
            std::max(health.invulnerableUntilMillis,
                     nowMillis + kPostHitInvulnerabilityMillis);
    }

    // Second Chance turns a killing blow on a flower into 1 HP. It is asked
    // here rather than at the call sites because every lethal path in the game
    // funnels through this function, where the reference has to remember to
    // ask on five of them -- one of which is a poison tick.
    //
    // `health` dangles from this call on: the lockout is a component, and
    // adding one relocates the victim's row.
    if (fatal && trySecondChance(world, victim, nowMillis)) fatal = false;

    creditSwing(world, victim, source, amount);

    // Every kind narrates itself. A poison tick and a sponge repayment reach
    // the reference's client as ordinary damage on the same two channels a
    // petal hit does -- markPoisonDamaged() and emitPlayerDamaged() -- and the
    // client already colours and offsets a poison number away from the petal
    // hit that landed in the same tick, so nothing is buried by reporting one.
    //
    // Those two channels are also the WHOLE of it: `enemiesDamaged` carries
    // mobs and `playerDamaged` carries flowers, and nothing else in the world
    // is ever narrated. A petal paying for its own swing therefore loses
    // health silently, which is what setInstanceHealth() does.
    if (events_ != nullptr && (world.has<MobTag>(victim) || world.has<PlayerTag>(victim))) {
        if (const NetId* id = world.tryGet<NetId>(victim)) {
            const Transform* transform = world.tryGet<Transform>(victim);
            const std::uint8_t flags = kind == DamageKind::Poison
                                           ? static_cast<std::uint8_t>(net::DamagePoison)
                                           : std::uint8_t{0};
            events_->damage(id->value, applied, transform ? transform->position : Vec2{}, flags);
        }
    }

    if (fatal) {
        // Marked, never destroyed here. Loot, the death notice and replication
        // all still have to read this entity later in the same tick; the
        // reaper is the only thing that removes it.
        //
        // `health` dangles from this line on: adding a component relocates the
        // entity to another archetype.
        const Entity killer = attributedSource(world, source);
        world.add<Dead>(victim, Dead{killer});
        result.killed = true;
        deaths_.push_back({victim, killer, world.has<PlayerTag>(victim)});
        // A player drops nothing: the account keeps the inventory, and the
        // Died message the server sends is the whole of the consequence.
        if (!world.has<PlayerTag>(victim)) awardBounty(world, victim);
    }
    return result;
}

void CombatSystem::awardBounty(World& world, Entity victim) {
    const Bounty* bounty = world.tryGet<Bounty>(victim);
    if (bounty == nullptr || bounty->xp <= 0.0) return;

    // TypeScript ranks positive contributors and grants the mob's FULL XP to
    // each eligible looter; it does not divide one pool in damage proportion.
    // The ranking itself is the loot system's -- one rule, so the players a
    // corpse pays XP to are exactly the ones it reserves its drops for, squads
    // included.
    std::vector<Bounty::Share> shares;
    for (const Bounty::Share& share : bounty->contributors) {
        if (share.damage > 0.0 && world.has<PlayerTag>(share.player)) shares.push_back(share);
    }
    std::stable_sort(shares.begin(), shares.end(),
                     [](const Bounty::Share& a, const Bounty::Share& b) {
                         return a.damage > b.damage;
                     });
    Rarity rarity = Rarity::Common;
    if (const MobType* type = world.tryGet<MobType>(victim)) rarity = type->rarity;
    std::vector<Entity> recipients;
    selectLootRecipients(shares, lootSlotsForRarity(rarity), squads, recipients);

    // Resolved and rounded per RECIPIENT rather than once per mob, because the
    // leaderboard factor is a property of the account being paid: the top ten
    // accounts earn half XP off every kill and the next ten three quarters,
    // which is the whole of the catch-up mechanic. payFullXpToEach() rounds
    // AFTER multiplying, so a 0.5x share of 45 XP is 23 and not 22.
    const double baseXp = bounty->xp;
    for (const Entity recipient : recipients) {
        PlayerProgress* progress = world.tryGet<PlayerProgress>(recipient);
        if (progress == nullptr) continue;
        const PlayerAccount* account = world.tryGet<PlayerAccount>(recipient);
        // A ranking the owner has not written yet is worth full XP, never a
        // NaN: this figure is added to a total that is persisted.
        const double multiplier =
            (account != nullptr && std::isfinite(account->xpMultiplier) &&
             account->xpMultiplier >= 0.0)
                ? account->xpMultiplier
                : 1.0;
        const double xp = std::round(baseXp * multiplier);
        progress->totalXp += xp;
        const int level = levelFromTotalXp(progress->totalXp).level;
        if (level != progress->level) {
            progress->level = level;
            progress->leveledThisTick = true;

            // Level-up stat recalculation and full heal happen immediately in
            // addXPToPlayer(), before the death broadcast. Waiting for next
            // tick's petal pass leaves one snapshot with the new level and old
            // health/damage.
            const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(recipient);
            const double healthScale = modifiers ? modifiers->maxHealthScale : 1.0;
            if (Health* health = world.tryGet<Health>(recipient)) {
                health->max = std::round(maxHealthForLevel(level) * healthScale);
                health->current = health->max;
            }
            if (ContactDamage* contact = world.tryGet<ContactDamage>(recipient)) {
                contact->amount = bodyDamageForLevel(level);
            }
        }
    }
}

bool CombatSystem::trySecondChance(World& world, Entity victim, double nowMillis) {
    if (!world.has<PlayerTag>(victim)) return false;
    // Skills are disabled inside the PvP arena, and friendly fire is the only
    // thing that marks a flower as being in one.
    if (const Faction* faction = world.tryGet<Faction>(victim)) {
        if (faction->friendlyFireEnabled) return false;
    }
    const PlayerSkillTree* tree = world.tryGet<PlayerSkillTree>(victim);
    if (tree == nullptr) return false;
    // {window, lockout}, and {0, 0} for a tier the talent does not define --
    // the reference's `if (!duration) return false` makes those a no-op rather
    // than an extrapolation, so a corrupt record cannot buy immortality.
    const std::array<double, 2> effect = secondChanceEffect(tree->skills.level(SkillId::SecondChance));
    if (effect[0] <= 0.0) return false;

    // ensure(), not tryGet(): a flower that has never been saved carries no
    // lockout, and the first save is exactly where one starts.
    SecondChance& lockout = world.ensure<SecondChance>(victim);
    if (nowMillis < lockout.readyAtMillis) return false;
    lockout.readyAtMillis = nowMillis + effect[1];

    Health& health = world.get<Health>(victim);
    health.current = 1.0;
    // The talent's window replaces the 50 ms post-hit one rather than adding
    // to it; every tier is longer, so taking the later of the two is the same
    // rule stated without a special case.
    health.invulnerableUntilMillis = std::max(health.invulnerableUntilMillis,
                                              nowMillis + effect[0]);
    return true;
}

// ---------------------------------------------------------------------------
// Riders: knockback, poison, slow
// ---------------------------------------------------------------------------

void CombatSystem::applyKnockback(World& world, Entity victim, Vec2 offset, double strength) {
    if (!std::isfinite(strength) || strength <= 0.0) return;
    if (!world.isAlive(victim)) return;
    // Only things that move can be pushed. Testing for Motion rather than
    // adding Knockback to everything keeps a nest or a drop out of the
    // archetype churn for a push it would ignore anyway.
    if (!world.has<Motion>(victim)) return;

    const Body* body = world.tryGet<Body>(victim);
    const double mass = (body != nullptr && body->mass > 1e-6) ? body->mass : 1.0;
    Vec2 direction = offset.normalized();
    if (direction.lengthSq() < 1e-12) return;   // exactly co-located: no direction to push along

    // This mirrors playerState.ts exactly: `effectiveKnockback` is the petal
    // stat divided by mob mass, and setMobKnockback() REPLACES the old vector.
    // It is a positional offset, consumed by movement next tick -- no scale,
    // friction or second mass division is involved.
    world.ensure<Knockback>(victim).impulse = direction * (strength / mass);
}

namespace {

/// A flower is shoved out of a contact by moving it 25 units immediately: the
/// same fixed displacement whether a mob walked into it or another duellist's
/// petal swung at it, neither mass-scaled nor turned into velocity. For mob
/// contact it happens before the damage/invulnerability branch, so an
/// invulnerable player still gets bumped. Petal knockback on a MOB deliberately
/// does not use this path: that one is queued for the mob's next movement pass.
void applyMobContactKnockback(World& world, Entity player, Vec2 offset) {
    Transform* transform = world.tryGet<Transform>(player);
    if (transform == nullptr) return;
    const Vec2 direction = offset.normalized();
    if (direction.lengthSq() < 1e-12) return;
    transform->position += direction * kMobContactKnockback;
}

} // namespace

void CombatSystem::applyPoison(World& world, Entity victim, Entity source, double perSecond,
                               double durationMillis, double nowMillis) {
    if (!std::isfinite(perSecond) || perSecond <= 0.0) return;
    if (!std::isfinite(durationMillis) || durationMillis <= 0.0) return;
    if (!world.isAlive(victim) || !world.has<Health>(victim)) return;

    // Credited to the PLAYER rather than to the petal that applied it: the
    // petal is often destroyed before the poison finishes ticking, and a kill
    // whose source has been reaped would award nobody anything.
    const Entity credited = creditedPlayer(world, source);
    const Entity attribution = credited != NULL_ENTITY ? credited : source;

    const double untilMillis = nowMillis + durationMillis;
    Afflictions& afflictions = world.ensure<Afflictions>(victim);

    // A flower carries exactly one bite, refreshed rather than accumulated:
    // playerState.ts overwrites poisonDamage and poisonUntil outright, so a
    // common mob's nip replaces a mythic's and may even shorten it.
    if (world.has<PlayerTag>(victim)) {
        afflictions.poisonPerSecond = perSecond;
        afflictions.poisonSource = attribution;
        afflictions.poisonUntilMillis = untilMillis;
        return;
    }

    afflictions.pruneStacks(nowMillis);
    if (PoisonStack* existing = afflictions.stackFrom(attribution)) {
        // gardn's outlast rule: a fresh bite takes over only when it would
        // last longer, and then it carries its own rate in with it. Without
        // the guard a short weak poison stomps a long strong one; without the
        // rate coming along, a long weak one silently keeps the strong rate.
        if (existing->untilMillis < untilMillis) {
            existing->perSecond = perSecond;
            existing->untilMillis = untilMillis;
        }
    } else {
        afflictions.poisonStacks.push_back({attribution, perSecond, untilMillis});
    }
    summarisePoison(afflictions, nowMillis);
}

void CombatSystem::applySlow(World& world, Entity victim, double factor, double durationMillis,
                             Rarity sourceRarity, double nowMillis) {
    if (!std::isfinite(factor) || factor >= 1.0) return;
    if (!std::isfinite(durationMillis) || durationMillis <= 0.0) return;
    if (!world.isAlive(victim) || !world.has<Health>(victim)) return;
    // A flower is never slowed by anything. applyMobSlow() is the reference's
    // one and only slow implementation -- both the petal bridge and the web
    // field resolve to it -- and it opens by refusing anything without
    // MobKind, so no web, honey petal or pincer has ever cost a player speed.
    //
    // The guard is not academic here: a thrown web outlives the flower that
    // threw it by up to ten seconds, and an orphaned one loses its side along
    // with its owner, which makes it a hazard that hurts everything. Without
    // this it would halve the speed of whoever walked through it.
    if (!world.has<MobTag>(victim)) return;

    // stallPower() is the rarity gate: out-tiering a mob buys reliability, not
    // a deeper slow, so it scales how much of the factor lands and nothing else.
    const double power = stallPower(sourceRarity, rarityOf(world, victim));
    const double landed = clamp(1.0 - (1.0 - factor) * power, 0.0, 1.0);
    if (landed >= 1.0) return;

    Afflictions& afflictions = world.ensure<Afflictions>(victim);
    if (!afflictions.slowed(nowMillis)) {
        afflictions.slowFactor = landed;
        afflictions.slowUntilMillis = nowMillis + durationMillis;
        return;
    }
    // Lower factor is a deeper slow, so min() keeps the stronger of the two.
    afflictions.slowFactor = std::min(afflictions.slowFactor, landed);
    afflictions.slowUntilMillis = std::max(afflictions.slowUntilMillis, nowMillis + durationMillis);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void CombatSystem::run(World& world, const SpatialGrid& grid, const ContentRegistry& content,
                       double nowMillis, double dt, CommandBuffer& commands, EventQueue& events) {
    // Combat's only structural change is the Dead tag, and that one is
    // deliberately immediate rather than deferred: the loot and death passes
    // later in THIS tick are the readers, and a deferred flush lands after
    // them. Nothing here needs the command buffer.
    (void)commands;

    beginTick(world, nowMillis, dt, events);
    runContactPhase(world, grid, content, nowMillis);
    runWorldPhase(world, grid, content, nowMillis, dt);
}

void CombatSystem::beginTick(World& world, double nowMillis, double dt, EventQueue& events) {
    bind(world);
    events_ = &events;
    deaths_.clear();
    ++tick_;

    // Owned here because combat is the only thing that awards XP. Cleared at
    // the top of the phase so replication, which runs later in the same tick,
    // sees precisely this tick's level-ups and no stale ones.
    queries_->progress.each([](Entity, PlayerProgress& progress) {
        progress.leveledThisTick = false;
    });

    // Standing damage first, so a mob that was already dying from poison is
    // dead before the petals that poisoned it swing again -- and its bounty is
    // paid once, by the poison, rather than to whoever happened to touch it.
    // This is the reference's order too: registerAfflictionSystems lands in
    // Phase.Combat ahead of registerMobCollisionSystem, and says so
    // (src/server/ecsRuntime.ts:651-654, "legacy ticked poison before the
    // melee pass"). Do not move it behind contact.
    tickAfflictions(world, nowMillis, dt);
    tickSpongeDamage(world, nowMillis, dt);
}

void CombatSystem::runContactPhase(World& world, const SpatialGrid& grid,
                                   const ContentRegistry& content, double nowMillis) {
    melee_.clear();
    auras_.clear();
    mobContactedPlayers_.clear();
    // The raindrop field runs in updatePlayerPreMovement, ahead of the movement
    // window and therefore ahead of the flower's own body and ring contact. It
    // matters: a mob the field finishes off is already Dead when the ring
    // swings, so the ring neither hits it again nor is credited for it.
    gatherAuras(world, content);
    resolveAuras(world, grid, nowMillis);
    gatherContact(world, content);
    gatherPetals(world, content);
    resolveMelee(world, grid, nowMillis);
}

void CombatSystem::runWorldPhase(World& world, const SpatialGrid& grid,
                                 const ContentRegistry& content, double nowMillis, double dt) {
    // TypeScript advances projectiles and then world fields after mobs move.
    // Damage fields are commutative within this phase, while projectile impact
    // remains the discrete collision whose post-movement position matters.
    tickProjectiles(world, grid, content, nowMillis, dt);
    tickGroundEffects(world, grid, content, nowMillis, dt);

    if (tick_ % kCooldownPruneTicks == 0) {
        queries_->cooldowns.each([&](Entity, HitCooldowns& cooldowns) { cooldowns.prune(nowMillis); });
        // The reference drops a mob's aura timestamps the moment it leaves the
        // world (forgetEnemyFromRaindropAura) because the map is keyed by mob
        // id and would otherwise grow by one entry per mob ever seen in range.
        // A deadline sweep is the same rule: every entry is stale 500 ms after
        // it was written, whether or not the mob it names still exists.
        queries_->auraCooldowns.each([&](Entity, AuraCooldowns& cooldowns) {
            cooldowns.hits.prune(nowMillis);
        });
    }

    // Not left dangling between ticks: a stray applyDamage() from a test or a
    // later system must not write into an EventQueue that has been cleared.
    events_ = nullptr;
}

void CombatSystem::tickAfflictions(World& world, double nowMillis, double dt) {
    poison_.clear();
    queries_->afflicted.each([&](Entity e, Afflictions& afflictions, Health&) {
        if (afflictions.slowFactor < 1.0 && nowMillis >= afflictions.slowUntilMillis) {
            afflictions.slowFactor = 1.0;
            afflictions.slowUntilMillis = 0;
        }
        // A mob holds one stack per poisoning player and EVERY one of them
        // ticks, so three flowers running blue_iris on the same boss deal
        // three times the damage and each is credited its own share. A single
        // slot would keep only the strongest, which loses the other two their
        // XP and loot ranking as well as their damage.
        if (!afflictions.poisonStacks.empty()) {
            afflictions.pruneStacks(nowMillis);
            for (const PoisonStack& stack : afflictions.poisonStacks) {
                if (stack.perSecond > 0.0) {
                    poison_.push_back({e, stack.source, stack.perSecond * dt});
                }
            }
            summarisePoison(afflictions, nowMillis);
            return;
        }

        if (afflictions.poisonPerSecond <= 0.0) return;
        if (nowMillis < afflictions.poisonUntilMillis) {
            // Poison armor is a flower's modifier, and the scalar slot is the
            // flower's path -- a mob never reaches this branch.
            double dps = afflictions.poisonPerSecond;
            if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e)) {
                dps = std::max(0.0, dps - modifiers->poisonArmor);
            }
            if (dps > 0.0) poison_.push_back({e, afflictions.poisonSource, dps * dt});
        } else {
            afflictions.poisonPerSecond = 0;
            afflictions.poisonUntilMillis = 0;
            afflictions.poisonSource = NULL_ENTITY;
        }
    });

    for (const PoisonTick& tick : poison_) {
        // A source that has since been reaped becomes environmental damage.
        // Passing the stale handle would be worse than useless: entity slots
        // are recycled, so it could name something on the victim's own side
        // and the poison would quietly stop working.
        const Entity source = world.isAlive(tick.source) ? tick.source : NULL_ENTITY;
        // A mob's poison is reported purple and nudged sideways so it cannot
        // land under the petal hit that arrived in the same tick. A flower's
        // is reported in the ordinary colour: playerDamaged carries one number
        // for every cause, and the reference has never tinted one of them.
        const DamageKind kind = world.has<PlayerTag>(tick.victim) ? DamageKind::Periodic
                                                                  : DamageKind::Poison;
        applyDamage(world, tick.victim, source, tick.amount, nowMillis, kind);
    }
}

void CombatSystem::tickSpongeDamage(World& world, double nowMillis, double dt) {
    spongeTicks_.clear();
    Query<SpongeDamageState, Health> deferred{world};
    deferred.without<Dead>();
    deferred.each([&](Entity player, SpongeDamageState& state, Health& health) {
        // TypeScript pauses the repayment while any invulnerability is live;
        // it does not silently consume that portion of the stored hit.
        if (nowMillis < health.invulnerableUntilMillis) return;

        std::size_t kept = 0;
        for (SpongeDamageEffect& effect : state.effects) {
            const double amount = std::min(effect.remainingDamage,
                                           effect.damagePerSecond * std::max(0.0, dt));
            if (amount > 0.0) {
                spongeTicks_.push_back({player, effect.source, amount});
                effect.remainingDamage -= amount;
            }
            if (effect.remainingDamage > 0.001) state.effects[kept++] = effect;
        }
        state.effects.resize(kept);
    });

    for (const PoisonTick& tick : spongeTicks_) {
        const Entity source = world.isAlive(tick.source) ? tick.source : NULL_ENTITY;
        applyDamage(world, tick.victim, source, tick.amount, nowMillis, DamageKind::Periodic);
    }
}

void CombatSystem::tickGroundEffects(World& world, const SpatialGrid& grid,
                                     const ContentRegistry& content, double nowMillis, double dt) {
    fields_.clear();
    queries_->fields.each([&](Entity e, GroundEffect& effect, Transform& transform) {
        if (effect.radius <= 0.0) return;
        fields_.push_back({e, effect.kind, transform.position, effect.radius,
                           effect.damagePerSecond, effect.slowFactor, effect.rarity,
                           effect.damagePerHit, effect.damageIntervalMillis});
    });

    for (const FieldSource& field : fields_) {
        if (!world.isAlive(field.effect)) continue;
        if (Lifetime* lifetime = world.tryGet<Lifetime>(field.effect)) {
            lifetime->remainingSeconds -= dt;
            if (lifetime->remainingSeconds <= 0.0) {
                world.add<Dead>(field.effect);
                continue;
            }
        }
        if (field.damagePerSecond <= 0.0 && field.damagePerHit <= 0.0 &&
            field.slowFactor >= 1.0) continue;
        grid.query(field.position, field.radius + kBroadphasePad, candidates_);
        for (const Entity victim : candidates_) {
            const Transform* transform = world.tryGet<Transform>(victim);
            if (transform == nullptr) continue;
            // Circle overlap, not centre-in-radius: every one of the three
            // reference fields adds the victim's own radius to its reach, so a
            // boss whose body edge is in the cloud is irradiated even though
            // its centre is 200 units outside it. A centre test shrinks the
            // effective area against a big mob by (r/(r+R))^2, which is most of
            // a web's or a puff's job.
            const Body* body = world.tryGet<Body>(victim);
            const double reach =
                field.radius + fieldTargetRadius(world, content, victim, field.kind, body);
            const double gapSq = distanceSq(transform->position, field.position);
            // The uranium pulse excludes on a strict `>` (playerState.ts:2686)
            // where pollen and web exclude on `>=` (groundEffects.ts:163, :231),
            // so a victim touching at exactly the rim is irradiated but is
            // neither dusted nor webbed.
            const bool inside = field.kind == GroundEffectKind::Radiation
                                    ? gapSq <= reach * reach
                                    : gapSq < reach * reach;
            if (!inside) continue;
            if (!canHit(world, victim, field.effect, nowMillis)) continue;

            if (field.damagePerHit > 0.0 && world.has<MobTag>(victim) &&
                !world.has<Pet>(victim)) {
                const HitCooldowns* cooldowns = world.tryGet<HitCooldowns>(field.effect);
                if (cooldowns == nullptr || cooldowns->ready(victim, nowMillis)) {
                    // A discrete chip, not a drip: a puff bites once every
                    // 500 ms and a lightning burst exactly once. The reference
                    // reports both through markEnemyDamaged(), the ordinary
                    // hit channel, and credits them through creditDamage(),
                    // which provokes a neutral mob. Direct is how a C++ chip
                    // buys both of those -- the flash it also lights is what
                    // steerAggressive reads as "something just hurt me".
                    const DamageResult hit = applyDamage(world, victim, field.effect,
                                                         field.damagePerHit, nowMillis);
                    if (!hit.refused) {
                        world.ensure<HitCooldowns>(field.effect)
                            .arm(victim, nowMillis + field.damageIntervalMillis);
                    }
                }
            } else if (field.damagePerSecond > 0.0) {
                applyDamage(world, victim, field.effect, field.damagePerSecond * dt, nowMillis,
                            DamageKind::Periodic);
            }
            // Refreshed every tick while inside, so the linger is the tail
            // after walking out rather than the length of the debuff.
            applySlow(world, victim, field.slowFactor, kGroundEffectSlowLingerMillis,
                      field.rarity, nowMillis);
        }
    }
}

void CombatSystem::gatherAuras(World& world, const ContentRegistry& content) {
    // One name lookup for the whole pass. The registry is a hash map and every
    // flower would otherwise pay for it once per equipped slot.
    const std::uint16_t raindrop = content.petalIndex("raindrop");
    if (raindrop == kNoPetal) return;

    queries_->auras.each([&](Entity e, PlayerTag&, Loadout& loadout, Transform& transform) {
        // Damage and radius are maximised INDEPENDENTLY, which is what the
        // reference does: two raindrops of different tiers give the better
        // number of each rather than the better petal's pair.
        double bestDamage = 0;
        double bestRadius = 0;
        for (int i = 0; i < kLoadoutActiveSlots; ++i) {
            const LoadoutSlot& slot = loadout.slots[static_cast<std::size_t>(i)];
            if (slot.configIndex != raindrop) continue;
            // A broken raindrop's field switches off until the slot reloads,
            // and raindrop reloads slowly, so this is a visible outage rather
            // than a technicality.
            if (slot.broken) continue;
            bestDamage = std::max(bestDamage, content.petalStats(raindrop, slot.rarity).damage);
            bestRadius = std::max(bestRadius,
                                  kRaindropAuraBaseRadius +
                                      rarityIndex(slot.rarity) * kRaindropAuraRadiusPerRarity);
        }
        if (bestRadius <= 0.0 || bestDamage <= 0.0) return;

        // The field is petal output, so it takes the petal curve -- the same
        // multiplier the ring's own contact takes, not the flower's body one.
        const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e);
        auras_.push_back({e, transform.position, bestRadius,
                          bestDamage * (modifiers != nullptr ? modifiers->petalDamageScale : 1.0)});
    });
}

void CombatSystem::resolveAuras(World& world, const SpatialGrid& grid, double nowMillis) {
    for (const AuraSource& aura : auras_) {
        if (!world.isAlive(aura.player) || world.has<Dead>(aura.player)) continue;

        grid.query(aura.position, aura.radius + kBroadphasePad, candidates_);
        for (const Entity victim : candidates_) {
            // Pets are not in the reference's enemy grid at all, so the field
            // sweeps wild mobs and never the flower's own summons.
            if (!world.has<MobTag>(victim) || world.has<Pet>(victim)) continue;
            if (!world.isAlive(victim) || world.has<Dead>(victim)) continue;
            const Transform* transform = world.tryGet<Transform>(victim);
            if (transform == nullptr) continue;

            // Circle overlap against the mob's LIVE radius, excluding on
            // exactly touching -- the field's own test, not the pollen puff's
            // config-radius one.
            const Body* body = world.tryGet<Body>(victim);
            const double reach = aura.radius + (body != nullptr ? body->radius : 0.0);
            if (distanceSq(transform->position, aura.position) >= reach * reach) continue;

            const AuraCooldowns* armed = world.tryGet<AuraCooldowns>(aura.player);
            if (armed != nullptr && !armed->hits.ready(victim, nowMillis)) continue;
            // Stamped before the hit and regardless of what the hit does, as
            // the reference stamps it: dwelling in the field costs a chip every
            // 500 ms rather than one on every tick of contact.
            world.ensure<AuraCooldowns>(aura.player)
                .hits.arm(victim, nowMillis + kRaindropAuraDamageIntervalMillis);
            applyDamage(world, victim, aura.player, aura.damage, nowMillis);
        }
    }
}

void CombatSystem::gatherContact(World& world, const ContentRegistry& content) {
    queries_->contact.each([&](Entity e, ContactDamage& contact, Transform& transform, Body& body) {
        MeleeSource source;
        source.attacker = e;
        source.position = transform.position;
        source.radius = body.radius;
        source.damage = contact.amount;
        source.hitIntervalMillis = contact.intervalMillis;
        source.isMobBody = world.has<MobTag>(e);
        source.isPet = world.has<Pet>(e);
        source.isPlayerBody = world.has<PlayerTag>(e);
        // Contact with a mob moves a player by the fixed TypeScript 25-unit
        // displacement; resolveMelee handles that special case directly.
        source.knockback = 0.0;

        if (const MobType* type = world.tryGet<MobType>(e)) {
            const MobStats stats = content.mobStats(type->configIndex, type->rarity);
            source.poisonPerSecond = stats.poisonPerSecond;
            source.poisonDurationMillis = stats.poisonDurationMillis;
            source.rarity = type->rarity;
        }
        if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e)) {
            source.damage *= modifiers->damageScale;
        }
        // recalculatePlayerStats() stores the flower's body damage ROUNDED and
        // every contact hit deals exactly that integer, so a level-10 flower
        // with the rare Damage talent hits for 44 rather than 44.4 and a mob
        // sitting on an exact health boundary dies on the same hit it dies on
        // in the reference. Mob contact damage is unrounded on both sides.
        if (source.isPlayerBody) source.damage = std::round(source.damage);
        if (source.damage <= 0.0 && source.poisonPerSecond <= 0.0) return;
        melee_.push_back(source);
    });
}

void CombatSystem::gatherPetals(World& world, const ContentRegistry& content) {
    queries_->petals.each([&](Entity e, PetalInstance& petal, Transform& transform, Body& body) {
        const PetalConfig& config = content.petal(petal.configIndex);
        if (config.noPhysics) return;   // a pure modifier has no body to hit with

        const PetalStats stats = content.petalStats(petal.configIndex, petal.rarity);
        const bool inert = stats.damage <= 0.0 && stats.poisonPerSecond <= 0.0 &&
                           stats.slowFactor >= 1.0 && stats.knockback <= 0.0;
        if (inert) return;

        MeleeSource source;
        source.attacker = e;
        source.position = transform.position;
        source.radius = body.radius;
        source.damage = stats.damage;
        source.hitIntervalMillis = stats.damageIntervalMillis;
        source.knockback = stats.knockback;
        source.poisonPerSecond = stats.poisonPerSecond;
        source.poisonDurationMillis = stats.poisonDurationMillis;
        source.slowFactor = stats.slowFactor;
        source.slowDurationMillis = stats.slowDurationMillis;
        source.rarity = petal.rarity;
        source.isPetal = true;
        // The flower's damage bonus is a property of the flower, not of the
        // petal entity, so it is read here rather than baked in at spawn --
        // swapping a damage petal in must affect the ring on the same tick.
        //
        // The PETAL curve, not the body one. getDamageMultiplier() puts
        // everything a petal does -- ring contact, a shot, a puff, a pulse --
        // on the steep effect table, where a body slam takes the gentle stat
        // table; sharing one factor costs a fully talented ring most of its
        // damage.
        if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(petal.owner)) {
            source.damage *= modifiers->petalDamageScale;
        }
        melee_.push_back(source);
    });
}

void CombatSystem::resolveMelee(World& world, const SpatialGrid& grid, double nowMillis) {
    for (const MeleeSource& source : melee_) {
        if (!world.isAlive(source.attacker) || world.has<Dead>(source.attacker)) continue;

        // The grid files each entity under every cell its own radius touches,
        // so a query at the attacker's radius already returns everything whose
        // circle could overlap; the exact test below is the one that decides.
        grid.query(source.position, source.radius + kBroadphasePad, candidates_);
        for (const Entity victim : candidates_) {
            if (victim == source.attacker) continue;
            const Transform* transform = world.tryGet<Transform>(victim);
            const Body* body = world.tryGet<Body>(victim);
            if (transform == nullptr || body == nullptr) continue;

            // A petal only ever bleeds inside its OWN hit block, below. The
            // reference has no mob-vs-petal collision at all, so letting a mob
            // body melee the ring as well would charge the exchange twice.
            if (!source.isPetal && world.has<PetalInstance>(victim)) continue;

            if (source.isPetal && world.has<PlayerTag>(victim)) {
                // Flower vs flower is a wholly separate collision in the
                // reference -- applyPvpDamage() has exactly one caller, and it
                // is the petal loop -- with its own gate, its own throttle and
                // its own cost to the petal.
                resolvePetalPvp(world, source, victim, transform->position, body->radius,
                                nowMillis);
                continue;
            }
            // A flower's body never hurts another flower, whatever side the
            // two are on: resolvePlayerMobContact walks the ENEMY grid, so
            // another flower is not a candidate for it in the first place.
            if (source.isPlayerBody && world.has<PlayerTag>(victim)) continue;

            // A pet and a wild mob meet in the separation kernel's contact
            // list, which is built with the same gap the kernel keeps between
            // bodies rather than at exact circle overlap.
            const bool petPair = source.isMobBody && world.has<MobTag>(victim) &&
                                 source.isPet != world.has<Pet>(victim);

            const Vec2 offset = transform->position - source.position;
            const double reach = source.radius + body->radius +
                                 (petPair ? kMobCollisionBuffer : 0.0);
            if (offset.lengthSq() > reach * reach) continue;

            // A TypeScript mob bump is independent of damage: it still lands
            // during respawn invulnerability, is not throttled by the damage
            // cooldown, and occurs before a lethal hit is handled.
            const bool mobTouchesPlayer = source.isMobBody &&
                                          world.has<PlayerTag>(victim) &&
                                          !world.has<Dead>(victim) &&
                                          world.has<Health>(victim) &&
                                          canDamage(world, source.attacker, victim);
            if (mobTouchesPlayer) {
                // resolvePlayerMobContact() breaks after its first collision:
                // one flower wedged in a pile takes one hit/bump per tick, not
                // a full stack. Preserve that rule across C++'s source-first
                // combat loop.
                if (std::find(mobContactedPlayers_.begin(), mobContactedPlayers_.end(), victim) !=
                    mobContactedPlayers_.end()) {
                    continue;
                }
                mobContactedPlayers_.push_back(victim);
                applyMobContactKnockback(world, victim, offset);
            }

            if (!canHit(world, victim, source.attacker, nowMillis)) continue;

            // Almost nothing is throttled in the reference, and the throttle
            // that survives is not keyed the way a hit ledger usually is.
            //
            // Mob body contact is paced by the victim's own 50 ms post-hit
            // window and by nothing else; pet/wild contact by nothing at all;
            // a flower's body by nothing. Only a petal whose config names a
            // `damageCooldown` waits, and that wait is keyed on the petal
            // INSTANCE with no victim in the key -- one glass petal lands one
            // hit per window however many mobs it is sitting on, rather than
            // one per mob, which would make it a full-rate area attack.
            const double interval = (mobTouchesPlayer || petPair)
                                        ? 0.0
                                        : std::max(0.0, source.hitIntervalMillis);
            // Read, do not create: an attacker that has never landed a hit
            // should not pay an archetype move just for being near something.
            const HitCooldowns* armed =
                interval > 0.0 ? world.tryGet<HitCooldowns>(source.attacker) : nullptr;
            if (armed != nullptr &&
                !(source.isPetal ? armed->globalReady(nowMillis) : armed->ready(victim, nowMillis))) {
                continue;
            }

            const DamageResult hit = applyDamage(world, victim, source.attacker,
                                                 source.damage, nowMillis);
            // A swing of NOTHING is still a swing. canHit() has already vouched
            // for this victim one line above -- alive, not a corpse, not
            // invulnerable, on the other side -- so the only refusal
            // applyDamage can add here is a damage figure of zero, and several
            // petals carry one deliberately: iris and blue_iris are pure
            // poison, bubble and bomb pure utility. The reference never looks
            // at the number; it applies every rider and charges the petal its
            // own health BELOW damageMob(), so a zero-damage petal poisons,
            // shoves and wears out exactly like any other. Read as a refusal,
            // those four petals landed nothing at all and never broke.
            const bool landed = !hit.refused || (source.isPetal && source.damage == 0.0);
            // Petal knockback is set after the hit using its own stat and the
            // victim's mass, exactly like playerState.ts. Mob contact already
            // performed its fixed player displacement above.
            //
            // Every rider sits BELOW the reference's already-dead `continue`
            // and inside its not-invulnerable branch, so a refused hit lands
            // none of them: it credits the ledger and nothing else.
            if (landed && !hit.killed) {
                if (source.isPetal) {
                    applyKnockback(world, victim, offset, source.knockback);
                }
                applyPoison(world, victim, source.attacker, source.poisonPerSecond,
                            source.poisonDurationMillis, nowMillis);
                applySlow(world, victim, source.slowFactor, source.slowDurationMillis,
                          source.rarity, nowMillis);
            }

            // The petal pays for the hit out of its own health, at the mob's
            // full damage stat, in the same block and on the same tick as the
            // damage it dealt. This IS the reload cycle: a ring that hits
            // thirty times a second and never bleeds is unbreakable, and the
            // break/reload rhythm is most of what paces the fight.
            //
            // Exempt for a petal that names a `damageCooldown` -- glass and
            // infinity never break on contact -- and Periodic, so the ring
            // does not flash white for a cost the reference pays silently.
            // The floating number takes care of itself: a petal is on neither
            // of the reference's two damage-report channels.
            if (landed && source.isPetal && source.hitIntervalMillis <= 0.0 &&
                world.has<MobTag>(victim)) {
                applyDamage(world, source.attacker, victim, contactDamageOf(world, victim),
                            nowMillis, DamageKind::Periodic);
            }

            // Re-fetched, never cached across the calls above: adding Dead or
            // Knockback to the victim relocates archetype rows, and the
            // attacker's own row may be one of the ones that moved.
            if (interval > 0.0) {
                HitCooldowns& cooldowns = world.ensure<HitCooldowns>(source.attacker);
                if (source.isPetal) cooldowns.armGlobal(nowMillis + interval);
                else cooldowns.arm(victim, nowMillis + interval);
            }

            // The other half of the one-per-tick contact rule, for the
            // direction mobContactedPlayers_ cannot see. resolvePlayerMobContact
            // breaks out of its candidate loop on the first LIVE mob it
            // collides with, so a flower's body lands one hit per tick however
            // many mobs it is wedged between -- a corpse it is also touching
            // costs it nothing, because that candidate is skipped rather than
            // ending the loop, which is what canHit() refusing above does.
            if (source.isPlayerBody && !hit.refused) break;
        }
    }
}

void CombatSystem::resolvePetalPvp(World& world, const MeleeSource& source, Entity victim,
                                   Vec2 victimPosition, double victimRadius, double nowMillis) {
    const PetalInstance* petal = world.tryGet<PetalInstance>(source.attacker);
    if (petal == nullptr) return;
    const Entity owner = petal->owner;
    // The reference skips its own flower by id and its splitter half by socket
    // -- one person, however many bodies. The native server has no splitter, so
    // resolving the ring back to its owner is the whole of that rule here, and
    // it is the only thing standing between a corrupted flower and its own ring.
    if (owner == victim) return;
    if (!canPetalsDamagePlayer(world, owner, victim)) return;

    // Exclusive, and exactly co-located misses: the reference skips on
    // `distSqP >= minDistSq || distSqP <= 0`, so two flowers standing on the
    // same pixel trade nothing until one of them moves.
    const Vec2 offset = victimPosition - source.position;
    const double reach = source.radius + victimRadius;
    const double gapSq = offset.lengthSq();
    if (gapSq >= reach * reach || gapSq <= 0.0) return;

    // Keyed on the VICTIM here, where the same petal's window against mobs is
    // keyed on the instance alone: the reference's PvP key carries the other
    // flower's id, so one petal reaches two duellists in a tick and neither of
    // them twice. A petal that declares a `damageCooldown` brings its own.
    const double interval = source.hitIntervalMillis > 0.0 ? source.hitIntervalMillis
                                                           : kPvpPetalHitIntervalMillis;
    const HitCooldowns* armed = world.tryGet<HitCooldowns>(source.attacker);
    if (armed != nullptr && !armed->ready(victim, nowMillis)) return;
    // Stamped before the swing and whatever the swing turns out to do, which
    // is where the reference stamps it: a petal that swings at an invulnerable
    // flower still waits its 250 ms, and still pays for the swing below.
    world.ensure<HitCooldowns>(source.attacker).arm(victim, nowMillis + interval);

    const DamageResult hit = applyDamage(world, victim, source.attacker, source.damage, nowMillis);
    // Away from the FLOWER rather than from the petal: applyPvpDamage measures
    // from the attacker's own centre, and a spinning ring puts its petals on
    // the far side of the victim half the time. A refused swing -- dead,
    // invulnerable, same side -- shoves nobody, because the reference returns
    // above its knockback.
    if (!hit.refused && world.isAlive(owner)) {
        if (const Transform* attacker = world.tryGet<Transform>(owner)) {
            applyMobContactKnockback(world, victim, victimPosition - attacker->position);
        }
    }

    // A flat point, never the victim's damage stat, and charged whatever the
    // swing did: the reference pays it outside applyPvpDamage's early returns,
    // so a swing at an invulnerable flower still wears the ring down. Skipped
    // only for a petal that declares a `damageCooldown` -- the same exemption
    // that makes glass and infinity unbreakable against mobs. The environment
    // is the source because the reference attributes this to nobody, and
    // naming the victim would let their own faction refuse the cost.
    if (source.hitIntervalMillis <= 0.0) {
        applyDamage(world, source.attacker, NULL_ENTITY, kPvpPetalSelfDamage, nowMillis,
                    DamageKind::Periodic);
    }
}

void CombatSystem::tickProjectiles(World& world, const SpatialGrid& grid,
                                   const ContentRegistry& content, double nowMillis, double dt) {
    shots_.clear();
    queries_->projectiles.each([&](Entity e, Projectile&, Transform& transform, Body& body,
                                   Motion& motion) {
        // Movement has already flown the shot this tick, so the budget spent
        // is the distance it just covered -- not one it is about to.
        shots_.push_back({e, transform.position, body.radius, motion.velocity.length() * dt,
                          body.mass, motion.velocity.length()});
    });

    for (const ShotSource& shot : shots_) {
        if (!world.isAlive(shot.entity) || world.has<Dead>(shot.entity)) continue;
        Projectile* projectile = world.tryGet<Projectile>(shot.entity);
        if (projectile == nullptr) continue;

        // MovementSystem already spent the distance budget while flying the
        // shot. Subtracting the same travel again halves every projectile's
        // configured range.
        if (projectile->remainingDistance <= 0.0) {
            world.add<Dead>(shot.entity);
            continue;
        }
        // Copied before anything structural: the component moves the moment a
        // victim is marked Dead.
        const double damage = projectile->damage;
        const std::uint16_t petalIndex = projectile->petalConfigIndex;
        const Rarity rarity = projectile->rarity;

        grid.query(shot.position, shot.radius + shot.travelled + kBroadphasePad, candidates_);

        // Gathered whole before a single hit lands. The loop below marks
        // victims Dead and that relocates their rows, so nothing here may hold
        // a pointer into the world across an applyDamage().
        impacts_.clear();
        for (const Entity victim : candidates_) {
            if (victim == shot.entity || !world.isAlive(victim)) continue;
            if (!isShootable(world, victim)) continue;
            const Transform* transform = world.tryGet<Transform>(victim);
            const Body* body = world.tryGet<Body>(victim);
            if (transform == nullptr || body == nullptr) continue;

            const Vec2 offset = transform->position - shot.position;
            const double reach = shot.radius + body->radius;
            const double distanceSquared = offset.lengthSq();
            if (distanceSquared > reach * reach) continue;
            if (!canHit(world, victim, shot.entity, nowMillis)) continue;
            impacts_.push_back({victim, offset, distanceSquared});
        }
        if (impacts_.empty()) continue;

        // Nearest first. A shot arriving into a clump spends its health pool
        // front to back, and ordering by distance is the only answer that does
        // not depend on grid bucket order.
        std::sort(impacts_.begin(), impacts_.end(),
                  [](const ShotImpact& a, const ShotImpact& b) {
                      return a.distanceSquared < b.distanceSquared;
                  });

        // Only resolved once something is actually going to be hit: this is a
        // per-shot table lookup and most shots spend their life hitting
        // nothing at all.
        const bool hasStats = petalIndex != kNoPetal;
        const PetalStats stats =
            hasStats ? content.petalStats(petalIndex, rarity) : PetalStats{};
        const double reloadInterval = hasStats ? stats.damageIntervalMillis
                                               : kPetalHitIntervalMillis;

        for (const ShotImpact& impact : impacts_) {
            if (!world.isAlive(shot.entity) || world.has<Dead>(shot.entity)) break;
            // Re-tested per victim rather than trusted from the gather: an
            // earlier impact in this same pass may have killed this one, and a
            // corpse must not pay out twice.
            if (!canHit(world, impact.victim, shot.entity, nowMillis)) continue;

            // The shot's own ledger, so a body it is PASSING THROUGH is hit
            // once per damage interval and not once per tick of overlap. It is
            // added at spawn precisely so arming it here cannot relocate the
            // shot out from under this loop.
            if (const HitCooldowns* ledger = world.tryGet<HitCooldowns>(shot.entity)) {
                if (!ledger->ready(impact.victim, nowMillis)) continue;
            }

            // Charged BEFORE the hit lands, because a victim marked Dead by it
            // no longer has the components this reads.
            const double cost = bodyDamageOf(world, content, impact.victim);
            const bool victimIsShot = world.has<Projectile>(impact.victim);

            const DamageResult hit =
                applyDamage(world, impact.victim, shot.entity, damage, nowMillis);

            // RE-FETCHED, never carried across applyDamage(). Marking the
            // victim Dead moves it between archetypes, and an archetype
            // swap-removes -- so when the victim is ANOTHER SHOT, the row that
            // slides into its slot may be this one's. A ledger pointer taken
            // before the call is exactly the dangling write the bullet-vs-
            // bullet rule made reachable.
            if (HitCooldowns* ledger = world.tryGet<HitCooldowns>(shot.entity)) {
                ledger->arm(impact.victim, nowMillis + reloadInterval);
            }

            // Riders belong to flesh. A shot cannot be poisoned, slowed or
            // shoved -- it has no Afflictions and its flight is a straight
            // line by contract with the client's interpolation.
            if (!hit.killed && !victimIsShot && hasStats) {
                // The petal's own `knockback` stat is the RING's, not the
                // volley's: projectileCollision.ts stamps a flat force on a
                // mob whatever fired it. What replaces the flat number here is
                // the shot's momentum, which is the same value for stock
                // ammunition and grows only when the shot itself does.
                applyKnockback(world, impact.victim, impact.offset,
                               world.has<MobTag>(impact.victim) ? kMobKnockbackForce
                                                                : stats.knockback);
                pushFromImpact(world, impact.victim, impact.offset, shot.mass, shot.speed);
                applyPoison(world, impact.victim, shot.entity, stats.poisonPerSecond,
                            stats.poisonDurationMillis, nowMillis);
                applySlow(world, impact.victim, stats.slowFactor, stats.slowDurationMillis,
                          rarity, nowMillis);
            }

            // What the hit cost the shot. Its own health pool is the whole of
            // penetration: nothing counts victims, the pool simply runs out.
            // A shot whose ammunition declares no pool was born with one point
            // and dies here, exactly as it did before it had one.
            if (!world.isAlive(shot.entity) || world.has<Dead>(shot.entity)) break;
            if (!std::isfinite(cost) || cost <= 0.0) {
                world.add<Dead>(shot.entity);
                break;
            }
            Health* health = world.tryGet<Health>(shot.entity);
            if (health == nullptr) {
                world.add<Dead>(shot.entity);
                break;
            }
            health->current -= cost;
            if (health->current <= 0.0) {
                health->current = 0.0;
                world.add<Dead>(shot.entity);
                break;
            }
        }
    }
}

void CombatSystem::pushFromImpact(World& world, Entity victim, Vec2 offset, double shotMass,
                                  double shotSpeed) {
    if (!world.isAlive(victim)) return;
    // Flowers are not pushed from here. applyKnockback above already writes
    // the flower's displacement and movement drains it; doubling that up would
    // be two shoves for one hit.
    if (!world.has<MobTag>(victim)) return;

    Transform* transform = world.tryGet<Transform>(victim);
    if (transform == nullptr) return;
    const Body* body = world.tryGet<Body>(victim);
    const double push = projectilePush(shotMass, shotSpeed, body ? body->mass : 1.0);
    if (!(push > 0.0)) return;

    const Vec2 direction = offset.normalized();
    if (direction.lengthSq() < 1e-12) return;   // exactly co-located: no direction to push along

    // Committed to the position rather than to Knockback, because a MOB's
    // Knockback component is a pure record: moveMobs() deliberately never
    // drains it (see the note there), so a mob written to it is a mob that
    // never moves. This is the same shape as the shove a flower takes from mob
    // contact -- an immediate displacement, no wall resolve, small enough that
    // the next movement step puts it back on legal ground.
    transform->position += direction * push;
}

} // namespace flix
