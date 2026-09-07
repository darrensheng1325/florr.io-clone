// What a bot does.
//
// A port of src/server/botManager.ts's decision tree, function for function
// and constant for constant. The tuning lives in server/bot_ai.h so the two
// files can be read side by side; this is the behaviour.
//
// The shape of one tick, in priority order, is the reference's:
//
//   watchdog escape  ->  mode (raid / high-rarity group / farm zone)
//   ->  traversal loadout swaps  ->  revive a downed bot  ->  raid shortcut
//   ->  target selection (sticky, with a reaction delay)  ->  regroup
//   ->  flee  ->  fight  ->  pick up a drop  ->  idle / wander
//
// Everything a bot writes is its PlayerInput, which the ordinary player
// movement and petal pipeline then consumes. No system anywhere else knows a
// bot exists, which is the whole reason a bot is a plain player entity.
//
// Two places read differently from the reference, deliberately:
//
//   * A mob's radius is its BODY radius here, which includes the per-spawn
//     size jitter. The reference recomputes `size * 40 / 2` from the config
//     and so ignores the jitter -- it is asking for the same quantity and
//     getting a slightly wrong answer for the mob actually standing there.
//   * Reach ignores `noPhysics` petals. The reference counts them, and a
//     noPhysics petal rides on the flower rather than taking a place on the
//     ring, so counting one over-states how far the bot can hit from.
//
// Everything else is the same numbers in the same order.

#include "server/game_server.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>

#include "server/bot_identity.h"
#include "server/systems/loot.h"
#include "shared/game/config.h"

namespace flix {

namespace {

/// The mob kinds a bot must never treat as a target, an obstacle or a threat.
///
/// A target dummy cannot hurt anything and exists to be hit deliberately; an
/// item spawner is scenery, and steering around one would push bots off the
/// drops they are walking to. Resolved once -- content is immutable after load
/// and this is asked per candidate mob per bot per tick.
struct ExcludedMobs {
    std::uint16_t dummy = kInvalidIndex;
    std::uint16_t spawner = kInvalidIndex;
};

const ExcludedMobs& excludedMobs() {
    static const ExcludedMobs kExcluded{content().mobIndex("target_dummy"),
                                        content().mobIndex("item_spawner")};
    return kExcluded;
}

bool excludedMobType(std::uint16_t configIndex) {
    const ExcludedMobs& excluded = excludedMobs();
    return configIndex == excluded.dummy || configIndex == excluded.spawner;
}

/// Casual phrasings for a boss sighting.
///
/// Kept lower-case and inconsistently punctuated on purpose so bot chatter
/// blends in with the usual player chat. Every template names its tier as a
/// bare word, so a human retyping one triggers the chat handler's own raid the
/// same way. {tier} = "super"|"unique", {mob} = e.g. "beetle", {code} = squad
/// id -- and a {code} template is only drawn from when the announcer is
/// actually in a squad.
constexpr const char* const kBossShoutSuper[] = {
    "{tier} {mob}",
    "{tier} {mob} come",
    "{tier} {mob} lets go",
    "who wants {tier} {mob}",
    "need help {tier} {mob}",
    "{tier} {mob} anyone",
    "{mob} {tier} here",
    "{tier} {mob} spawn",
    "{tier} {mob} free",
    "{tier} {mob} free mzone",
    "{tier} {mob} free lzone",
    "super shiny",
    "free {tier} {mob}",
    "{tier} {mob} deep",
    "{tier} {mob} lured",
    "s{mob}",
    "s{mob} unfree",
    "less than 20 ppl at {tier} {mob}",
    "{tier} {mob} free carry",
    "I have previously said things which I regret. Now I ponder in silence.",
    "pls carry",
    "free carry {code}",
    "{tier} {mob} carry code {code}",
};

constexpr const char* const kBossShoutUnique[] = {
    "q{mob}",
    "how {tier} {mob}",
    "{tier} {mob} come",
    "{tier} {mob} lets go",
    "who wants {tier} {mob}",
    "{tier} {mob} anyone",
    "{mob} {tier} here",
    "{tier} {mob} so free",
    "WHAT {tier} {mob}",
    "q{mob} pls loot",
    "q{mob} pls carry",
    "{tier} {mob} pls carry",
    "{tier} {mob} pls loot",
    "q{mob} so free",
    "less than 20 ppl at {tier} {mob}",
    "I have previously said things which I regret. Now I ponder in silence.",
    "pls carry",
    "free carry {code}",
    "{tier} {mob} carry code {code}",
};

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

/// A mob id as chat says it out loud: `baby_ant` is "baby ant".
std::string spokenMobName(const std::string& id) {
    std::string out = id;
    std::replace(out.begin(), out.end(), '_', ' ');
    return out;
}

/// The reference's steering probe offsets, in order: straight on first, then
/// progressively wider to either side.
constexpr double kSteerOffsets[] = {
    0.0,        kPi / 6.0,  -kPi / 6.0,        kPi / 3.0,        -kPi / 3.0,
    kPi / 2.0,  -kPi / 2.0, 2.0 * kPi / 3.0,   -2.0 * kPi / 3.0,
};

/// 8-connected A* neighbourhood: orthogonal costs 1, diagonal costs root two.
constexpr double kSqrt2 = 1.41421356237309504880;
struct Neighbour { int dx, dy; double cost; };
constexpr Neighbour kAStarNeighbours[] = {
    {1, 0, 1.0},  {-1, 0, 1.0},  {0, 1, 1.0},  {0, -1, 1.0},
    {1, 1, kSqrt2}, {1, -1, kSqrt2}, {-1, 1, kSqrt2}, {-1, -1, kSqrt2},
};

double octileHeuristic(int ax, int ay, int bx, int by) {
    const double dx = std::abs(ax - bx);
    const double dy = std::abs(ay - by);
    return (dx + dy) + (kSqrt2 - 2.0) * std::min(dx, dy);
}

/// The bucket key raiders converging on one boss share. Rounded, so a boss
/// drifting by a pixel or two does not split its raid into two crowds.
std::uint64_t raidBucketKey(Vec2 anchor) {
    const auto qx = static_cast<std::int32_t>(std::llround(anchor.x / 8.0));
    const auto qy = static_cast<std::int32_t>(std::llround(anchor.y / 8.0));
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(qx)) << 32) |
           static_cast<std::uint32_t>(qy);
}

} // namespace

// ---------------------------------------------------------------------------
// Population
// ---------------------------------------------------------------------------

void GameServer::maintainBots(double nowMillis) {
    // A human in the world resets the idle clock. Past the grace period with
    // nobody online the bots are retired: there is nobody to see them, and the
    // tick gate has already stopped simulating anyway.
    //
    // Skipped entirely while `set_bot_count N` is in force, exactly as the
    // reference skips it: title-screen sessions are not players, so an
    // operator sitting on the menu would otherwise set a count, watch nothing
    // happen, and have no way to tell why.
    if (playerCount() > 0) {
        lastHumanSeenMillis_ = nowMillis;
    } else if (botCountOverride_ < 0 &&
               nowMillis - lastHumanSeenMillis_ >= kBotIdleTimeoutMillis) {
        for (Bot& bot : bots_) destroyBot(bot);
        bots_.clear();
        return;
    }

    if (nowMillis < nextBotMaintainMillis_) return;
    nextBotMaintainMillis_ = nowMillis + kBotMaintainMillis;

    // Retire the bodies the world has already taken away -- a bot killed by a
    // mob is reaped like any other flower -- and hand the survivors a new one
    // once their respawn delay is up.
    // Collected ONCE for the whole pass: the placement test needs every mob
    // body, and asking for them per bot is a walk over the world per bot --
    // which is what turned a large `set_bot_count` into a visible stall.
    std::vector<MobDisc> blockers;
    collectSpawnBlockers(blockers);

    for (Bot& bot : bots_) {
        // Two ways a bot is down: its body was taken away outright (a mob's
        // kill on a flower nothing owns, an admin), or it is lying there as a
        // corpse waiting to be revived or replaced.
        const bool gone = bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity);
        const bool down = !gone && world_.has<Dead>(bot.entity);
        if (gone) bot.entity = NULL_ENTITY;
        if ((gone || down) && bot.respawnAtMillis <= 0) {
            bot.respawnAtMillis = nowMillis + kBotRespawnDelayMillis;
        }
        if ((gone || down) && bot.respawnAtMillis > 0 && nowMillis >= bot.respawnAtMillis) {
            // Somewhere else entirely, as the reference respawns them: the
            // ground it died on is exactly the ground that killed it.
            const Vec2 spawn = pickBotSpawn(blockers);
            // Takes the corpse and its ring away, if one is still lying there.
            destroyBot(bot);
            bot.entity = createBotBody(bot.name, spawn);
            bot.deathAnnounced = false;
            bot.anchor = spawn;
            bot.hasAnchor = false;
            bot.respawnAtMillis = 0;

            // The bot reappears somewhere else entirely, so everything derived
            // from where it used to be -- target, path, heading, watchdog
            // samples -- is stale, and a leftover unstick manoeuvre would
            // drive it straight back out of its new spawn for no reason. The
            // FARMING ZONE survives: the bot goes back to where it farms.
            BotAiState& ai = bot.ai;
            ai.wanderTarget = spawn;
            ai.nextWanderMillis = 0;
            ai.idleUntilMillis = 0;
            ai.atWanderTarget = false;
            ai.target = NULL_ENTITY;
            ai.targetAcquiredMillis = 0;
            ai.pickup = NULL_ENTITY;
            ai.fleeing = false;
            ai.fleeUntilMillis = 0;
            ai.hasHeading = false;
            ai.hasSlotAngle = false;
            ai.unstickUntilMillis = 0;
            ai.suppressTargetUntilMillis = 0;
            // The swap bookkeeping described a loadout that no longer exists:
            // the body carrying it was destroyed, and the new one was rebuilt
            // from the name. Dropping the record rather than restoring it is
            // what stops the next unequip writing a dead body's slot back over
            // a live one.
            ai.powderSwapped = false;
            ai.yggSwapped = false;
            botClearPath(ai);
            botResetOscillation(bot, nowMillis);
        }
    }

    // Drift the target by +-1 on a slow clock, bounded, so the population
    // wanders instead of sitting on an exact number -- and slowly enough that
    // the drift does not read as bots blinking in and out.
    if (nowMillis >= nextBotJitterMillis_) {
        nextBotJitterMillis_ = nowMillis + kBotJitterIntervalMillis;
        if (rng_.chance(kBotJitterStepChance)) {
            botCountJitter_ += rng_.chance(0.5) ? -1 : 1;
            botCountJitter_ = std::max(kBotJitterMin, std::min(kBotJitterMax, botCountJitter_));
        }
    }

    const int humans = static_cast<int>(playerCount());
    // An override from `/admin set_bot_count` is an exact target, not a
    // correction to the formula: an operator asking for twelve bots wants
    // twelve, not twelve minus however many people are online.
    const bool overridden = botCountOverride_ >= 0;
    const int desired =
        overridden
            ? std::min(kMaxBots, botCountOverride_)
            : std::min(kMaxBots,
                       std::max(0, kBotTargetTotalPlayers - humans + botCountJitter_));
    const int current = static_cast<int>(bots_.size());

    if (current < desired) {
        // The burst cap is about making a RESTART look like players arriving
        // rather than like a crowd appearing. An operator typing a number is
        // not a restart: dribbling their instruction out four bots at a time
        // means `set_bot_count 100` sits there looking inert for half a minute
        // and reads as a command that does nothing. An explicit target is
        // filled in one pass.
        const int wanted =
            overridden ? desired - current : std::min(desired - current, kBotSpawnBurstCap);
        for (int i = 0; i < wanted; ++i) {
            Bot bot;
            bot.name = kBotNames[rng_.below(static_cast<std::uint32_t>(std::size(kBotNames)))];
            // Its own identity, independent of the name: two bots that roll
            // the same name share a BUILD, as the reference intends, but not a
            // persona, a strafe direction or a farming zone.
            bot.id = static_cast<std::uint32_t>(rng_.next());
            const Vec2 spawn = pickBotSpawn(blockers);
            bot.anchor = spawn;
            bot.ai.wanderTarget = spawn;
            bot.entity = createBotBody(bot.name, spawn);
            bots_.push_back(std::move(bot));
        }
    } else if (current > desired) {
        // Cull the bots FARTHEST from any human first. Taking whichever came
        // first out of the list routinely takes one standing next to a player,
        // which simply vanishes in front of them.
        botOrderScratch_.resize(bots_.size());
        for (std::size_t i = 0; i < botOrderScratch_.size(); ++i) botOrderScratch_[i] = i;
        std::stable_sort(botOrderScratch_.begin(), botOrderScratch_.end(),
                         [&](std::size_t a, std::size_t b) {
                             return cullScore(bots_[a]) > cullScore(bots_[b]);
                         });
        const std::size_t excess = static_cast<std::size_t>(current - desired);
        std::vector<bool> doomed(bots_.size(), false);
        for (std::size_t i = 0; i < excess && i < botOrderScratch_.size(); ++i) {
            doomed[botOrderScratch_[i]] = true;
        }
        std::vector<Bot> kept;
        kept.reserve(bots_.size() - excess);
        for (std::size_t i = 0; i < bots_.size(); ++i) {
            if (doomed[i]) destroyBot(bots_[i]);
            else kept.push_back(std::move(bots_[i]));
        }
        bots_ = std::move(kept);
    }
}

GameServer::Bot* GameServer::botForEntity(Entity body) {
    for (Bot& bot : bots_) {
        if (bot.entity == body) return &bot;
    }
    return nullptr;
}

double GameServer::cullScore(const Bot& bot) const {
    // Higher culls sooner. A body the world has already taken, or one nobody
    // is anywhere near, goes before one a player is standing next to.
    if (bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity) || world_.has<Dead>(bot.entity)) {
        return std::numeric_limits<double>::max();
    }
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return std::numeric_limits<double>::max();

    double nearest = std::numeric_limits<double>::max();
    for (const auto& entry : sessions_) {
        const Session& session = entry.second;
        if (!session.playing()) continue;
        const Transform* other = world_.tryGet<Transform>(session.entity);
        if (other == nullptr) continue;
        nearest = std::min(nearest, distanceSq(other->position, transform->position));
    }
    // Nobody watching anyone: the order does not matter.
    return nearest == std::numeric_limits<double>::max() ? 0.0 : nearest;
}

Vec2 GameServer::pickBotSpawn() {
    std::vector<MobDisc> blockers;
    collectSpawnBlockers(blockers);
    return pickBotSpawn(blockers);
}

Vec2 GameServer::pickBotSpawn(const std::vector<MobDisc>& blockers) {
    // EVERY area a real player can actually appear in, and nothing else: a
    // `common` spawn zone, or a biome whose own table admits nothing above
    // uncommon. That is exactly what the join handler allows, and it is why a
    // bot never turns up deep in mythic ground where it is under attack from
    // the moment it appears.
    //
    // Sampled uniformly over the whole set, which is what spreads the
    // population over the map instead of stacking it in the beginner's corner
    // the way `defaultSpawn` deliberately does for a joining player.
    std::vector<const MapElement*> anchors;
    for (const MapElement& element : mapData_.elements()) {
        if (element.bounds.w <= 0 || element.bounds.h <= 0) continue;
        if (element.kind == MapElementKind::Spawn && element.hasSpawnTier &&
            element.spawnTier == Rarity::Common) {
            anchors.push_back(&element);
        } else if (element.kind == MapElementKind::Biome && MapData::safeForSpawn(element)) {
            anchors.push_back(&element);
        }
    }

    // Several areas get a real number of attempts each, because they are large
    // and can be densely populated with mobs. Leaving the spawnable SET is
    // never one of the fallbacks: standing next to a mob for a tick is
    // recoverable (a fresh body spawns invulnerable), being dropped into Hel
    // is not.
    const int tries = std::min<int>(8, static_cast<int>(anchors.size()));
    Vec2 spawn;
    for (int i = 0; i < tries; ++i) {
        const MapElement* anchor =
            anchors[rng_.below(static_cast<std::uint32_t>(anchors.size()))];
        if (mapData_.spawnInElement(*anchor, rng_, *terrain_, spawn, &blockers)) return spawn;
    }
    if (!anchors.empty()) {
        // Every sampled area was crowded or walled over. The centre of one of
        // them is still inside the spawnable set, and movement pushes a body
        // out of a wall.
        return anchors[rng_.below(static_cast<std::uint32_t>(anchors.size()))]->centre();
    }
    return mapData_.defaultSpawn(rng_, *terrain_, &blockers);
}

Entity GameServer::createBotBody(const std::string& name, Vec2 spawn) {
    // Level and loadout are derived from the NAME, not from the spawn, so a
    // bot called "m28" is the same flower every time it appears. The rolls
    // come from server/bot_identity.h, which is also what the admin console's
    // /level-from-string and /loadout-from-string answer out of -- one roll,
    // so the console cannot describe a bot the world would not build.
    const BotIdentity identity = botIdentityForName(name, kLoadoutActiveSlots, kMaxLevel);
    const int level = identity.level;

    const Entity entity = world_.create();
    world_.add<PlayerTag>(entity);
    world_.add<Transform>(entity, Transform{spawn, 0.0});
    world_.add<Motion>(entity);
    world_.add<Knockback>(entity);
    world_.add<Faction>(entity, Faction{Team::Players, false});
    world_.add<PlayerInput>(entity);
    world_.add<PlayerLocation>(entity);
    world_.add<PlayerModifiers>(entity);
    world_.add<PlayerVisuals>(entity);
    world_.add<PlayerSkillTree>(entity);
    world_.add<Loadout>(entity);
    world_.add<PetalRing>(entity);
    world_.add<ContactDamage>(entity, ContactDamage{bodyDamageForLevel(level), 0.0});
    world_.add<HitCooldowns>(entity);
    world_.add<Afflictions>(entity);
    world_.add<ShieldState>(entity);
    // No userId: a bot owns no account, so every path that banks progress --
    // kills, stars, pickups, the periodic persist -- walks the session table,
    // finds nothing, and skips it without needing to know what a bot is.
    world_.add<PlayerAccount>(entity, PlayerAccount{std::string(), name, 0, false});

    PlayerProgress progress;
    progress.level = level;
    for (int l = 1; l < level; ++l) progress.totalXp += xpForNextLevel(l);
    world_.add<PlayerProgress>(entity, progress);

    Body body;
    body.radius = playerRadiusForLevel(level);
    body.mass = 1.0;
    world_.add<Body>(entity, body);

    Health health;
    health.max = maxHealthForLevel(level);
    health.current = health.max;
    health.invulnerableUntilMillis = monotonicMillis() + kRespawnInvulnerabilitySeconds * 1000.0;
    world_.add<Health>(entity, health);

    // All ten active slots, matching a real player's maximum: a bot with five
    // petals reads as a beginner whatever its level says.
    Loadout& loadout = world_.get<Loadout>(entity);
    for (std::size_t i = 0; i < identity.slots.size(); ++i) {
        const BotIdentity::Slot& slot = identity.slots[i];
        if (slot.petalIndex == kInvalidIndex) continue;
        loadout.slots[i].configIndex = slot.petalIndex;
        loadout.slots[i].rarity = slot.rarity;
    }

    world_.add<NetId>(entity, NetId{netIds_.next()});
    Replicated replicated;
    replicated.kind = net::EntityKind::Player;
    world_.add<Replicated>(entity, replicated);
    return entity;
}

void GameServer::destroyBot(Bot& bot) {
    if (bot.entity == NULL_ENTITY) return;
    // Before the body goes: a squad holding a destroyed entity would rank a
    // corpse for loot, and the line its squadmates get needs its nameplate.
    removeBotFromSquad(bot.entity);
    botBossFirstSeen_.erase(bot.entity);
    botGroups_.erase(bot.entity);
    botRaidSlots_.erase(bot.entity);
    if (world_.isAlive(bot.entity)) {
        // The ring belongs to the body, not to the name, so it goes with it.
        if (const Loadout* loadout = world_.tryGet<Loadout>(bot.entity)) {
            for (const Entity petal : loadout->spawned) commands_.destroy(petal);
        }
        commands_.destroy(bot.entity);
    }
    bot.entity = NULL_ENTITY;
}

// ---------------------------------------------------------------------------
// Reach and gear
// ---------------------------------------------------------------------------

int GameServer::botMaxRarityIndex(const Bot& bot) const {
    const Loadout* loadout = world_.tryGet<Loadout>(bot.entity);
    if (loadout == nullptr) return 0;
    int best = 0;
    for (int i = 0; i < kLoadoutActiveSlots; ++i) {
        const LoadoutSlot& slot = loadout->slots[static_cast<std::size_t>(i)];
        if (slot.empty()) continue;
        best = std::max(best, rarityIndex(slot.rarity));
    }
    return best;
}

double GameServer::botPetalReach(const Bot& bot, double petalExtension) const {
    // The whole combat controller is built on this one number: the distance at
    // which the orbiting petals reach the mob being fought. It is DERIVED from
    // the ring's own geometry rather than mirrored, because a hand copy of a
    // formula does not fail when it drifts -- it goes on returning a number,
    // and a bot whose estimate is thirty units optimistic parks just outside
    // where its petals connect and orbits a mob it never damages, forever.
    const Loadout* loadout = world_.tryGet<Loadout>(bot.entity);
    if (loadout == nullptr) return kBotStandoffBuffer;

    const Body* body = world_.tryGet<Body>(bot.entity);
    const double playerRadius = body ? body->radius : kPlayerBaseRadius;
    const double neutralRadius = kPetalOrbitRestRadius + playerRadius - kPlayerBaseRadius;

    double rangeScale = 1.0;
    if (const PlayerModifiers* mods = world_.tryGet<PlayerModifiers>(bot.entity)) {
        rangeScale = std::max(0.0, mods->rangeScale);
    }
    const double base = neutralRadius * petalExtension * rangeScale;
    // A defendOnly petal (rose) never flies out on attack, so its radius is
    // capped at the neutral orbit however far the ring is thrown.
    const double defendOnlyBase = neutralRadius * std::min(petalExtension, 1.0) * rangeScale;

    double farthest = 0;
    for (int i = 0; i < kLoadoutActiveSlots; ++i) {
        const LoadoutSlot& slot = loadout->slots[static_cast<std::size_t>(i)];
        if (slot.empty()) continue;
        const PetalConfig& config = content().petal(slot.configIndex);
        // A noPhysics petal rides on the flower instead of taking a place on
        // the ring, so it contributes no reach at all.
        if (config.noPhysics) continue;
        const PetalStats stats = content().petalStats(slot.configIndex, slot.rarity);

        double radius = (config.defendOnly ? defendOnlyBase : base) *
                        (config.range > 0.0 ? config.range : 1.0);
        // A clumped petal's grains fan out around their shared ring place, and
        // one of them points outward on every revolution.
        if (config.clumped && stats.count > 1) radius += stats.radius;
        // Measured to the petal's far EDGE, which is what actually touches.
        farthest = std::max(farthest, radius + stats.radius);
    }
    return farthest + kBotStandoffBuffer;
}

// ---------------------------------------------------------------------------
// Steering primitives
// ---------------------------------------------------------------------------

bool GameServer::botRayHitsWall(Vec2 from, Vec2 to) const {
    // The reference's raycast, sample for sample: every half tile along the
    // segment. Deliberately NOT Terrain::segmentBlocked -- that one is an
    // exact swept walk, and it refuses the diagonal seams and narrow gaps this
    // sampled test steers through, which would visibly change where bots are
    // willing to go.
    const Vec2 delta = to - from;
    const double dist = delta.length();
    // A zero, NaN or runaway distance means no hit: a ray toward a body flung
    // to an enormous coordinate would otherwise blow the step count up.
    if (!(dist > 0.0) || !std::isfinite(dist)) return false;
    const double step = kTileSize / 2.0;
    const int steps = std::min(1024, static_cast<int>(std::ceil(dist / step)));
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        if (terrain_->blocked(from + delta * t)) return true;
    }
    return false;
}

Vec2 GameServer::botSteerAroundWalls(Vec2 from, Vec2 direction, double probeDistance) const {
    for (const double offset : kSteerOffsets) {
        const double c = std::cos(offset);
        const double s = std::sin(offset);
        const Vec2 rotated{direction.x * c - direction.y * s, direction.x * s + direction.y * c};
        if (!botRayHitsWall(from, from + rotated * probeDistance)) return rotated;
    }
    return direction;
}

Vec2 GameServer::botAvoidNearbyMobs(Vec2 at, Entity except) {
    // A steering BIAS, not a hard constraint: summed with the requested
    // heading the same way bot-vs-bot separation is, and capped below one so
    // it can bend a heading around a mob but can never reverse the bot's
    // intent and leave it unable to reach a goal that happens to be guarded.
    Vec2 out{0, 0};
    botAvoidCandidates_.clear();
    grid_.query(at, kBotMobAvoidQueryRadius, botAvoidCandidates_);
    for (const Entity candidate : botAvoidCandidates_) {
        if (candidate == except) continue;
        if (!world_.isAlive(candidate) || world_.has<Dead>(candidate)) continue;
        if (!world_.has<MobTag>(candidate)) continue;
        const MobType* type = world_.tryGet<MobType>(candidate);
        if (type == nullptr || excludedMobType(type->configIndex)) continue;
        const Transform* other = world_.tryGet<Transform>(candidate);
        const Body* body = world_.tryGet<Body>(candidate);
        if (other == nullptr || body == nullptr) continue;

        const Vec2 away = at - other->position;
        const double distSq = away.lengthSq();
        if (distSq == 0.0) continue;
        const double ring = kPlayerBaseRadius + body->radius + kBotMobAvoidMargin;
        const double outer = ring + kBotMobAvoidLookahead;
        if (distSq >= outer * outer) continue;
        const double dist = std::sqrt(distSq);
        // Zero at the outer edge, one at the ring, up to two once overlapping
        // -- so a bot already touching a mob pushes off far harder than one
        // just drifting close. Divided by the distance to normalise in the
        // same step.
        const double weight = std::min(2.0, (outer - dist) / kBotMobAvoidLookahead) / dist;
        out += away * weight;
    }
    const double magnitude = out.length();
    if (magnitude > kBotMobAvoidMax) out = out * (kBotMobAvoidMax / magnitude);
    return out;
}

void GameServer::botHold(Bot& bot, double petalExtension) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr) return;
    input->current.moveStrength = 0.0;
    input->current.flags = petalExtension >= 1.5   ? static_cast<std::uint8_t>(net::InputAttack)
                           : petalExtension <= 0.85 ? static_cast<std::uint8_t>(net::InputDefend)
                                                    : std::uint8_t{0};
}

void GameServer::botDriveMove(Bot& bot, Vec2 direction, double speedMultiplier,
                              double petalExtension, double agility, Entity avoidExcept) {
    Transform* transform = world_.tryGet<Transform>(bot.entity);
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (transform == nullptr || input == nullptr) return;
    const Vec2 at = transform->position;

    // Separation: push away from any other bot inside the separation radius,
    // weighted so near-touches dominate over mid-range neighbours. Keeps a
    // crowd from collapsing to a single point.
    Vec2 separation{0, 0};
    for (const Bot& other : bots_) {
        if (other.entity == bot.entity || other.entity == NULL_ENTITY) continue;
        if (!world_.isAlive(other.entity) || world_.has<Dead>(other.entity)) continue;
        const Transform* otherTransform = world_.tryGet<Transform>(other.entity);
        if (otherTransform == nullptr) continue;
        const Vec2 away = at - otherTransform->position;
        const double distSq = away.lengthSq();
        if (distSq == 0.0 || distSq > kBotSeparationRadius * kBotSeparationRadius) continue;
        const double dist = std::sqrt(distSq);
        separation += away * ((1.0 - dist / kBotSeparationRadius) / dist);
    }

    // Persistent per-bot bias plus a tiny tick-level wobble, so motion looks
    // alive instead of pixel-locked. Kept small enough never to override
    // intent.
    const BotPersona& persona = bot.ai.persona;
    const Vec2 wobble{persona.bias.x * 0.08 + (rng_.unit() - 0.5) * 0.06,
                      persona.bias.y * 0.08 + (rng_.unit() - 0.5) * 0.06};

    const Vec2 avoid = botAvoidNearbyMobs(at, avoidExcept);

    Vec2 out = direction + separation * kBotSeparationStrength + avoid * kBotMobAvoidStrength +
               wobble;
    const double magnitude = out.length();
    if (magnitude > 0.0) out = out / magnitude;
    else out = direction;

    // Turn-rate limiting. The AI can request any direction on any tick; a real
    // player's hand cannot. Easing the heading toward the request does two
    // things: movement reads as a flower arcing around rather than a turret
    // snapping, and a decision that flip-flops between two opposite directions
    // can no longer become visible per-tick vibration -- the heading hovers
    // near the midpoint until something breaks the tie.
    BotAiState& ai = bot.ai;
    const Motion* motion = world_.tryGet<Motion>(bot.entity);
    const double speed = motion ? motion->velocity.length() : 0.0;
    if (!ai.hasHeading || speed < kBotFreeTurnSpeed) {
        ai.heading = out;
        ai.hasHeading = true;
    } else {
        const double current = std::atan2(ai.heading.y, ai.heading.x);
        const double wanted = std::atan2(out.y, out.x);
        const double maxTurn = persona.turnRate * agility;
        const double delta = clamp(wrapAngle(wanted - current), -maxTurn, maxTurn);
        ai.heading = Vec2::fromAngle(current + delta);
    }
    out = ai.heading;

    if (out.lengthSq() < 1e-12) {
        input->current.moveStrength = 0.0;
    } else {
        input->current.moveAngle = std::atan2(out.y, out.x);
        input->current.moveStrength = clamp(speedMultiplier, 0.0, 1.0);
    }
    input->current.flags = petalExtension >= 1.5   ? static_cast<std::uint8_t>(net::InputAttack)
                           : petalExtension <= 0.85 ? static_cast<std::uint8_t>(net::InputDefend)
                                                    : std::uint8_t{0};
}

// ---------------------------------------------------------------------------
// A*
// ---------------------------------------------------------------------------

void GameServer::botClearPath(BotAiState& ai) {
    ai.pathNodes.clear();
    ai.pathIndex = 0;
    ai.hasPath = false;
    ai.hasPathGoal = false;
    ai.pathCreatedMillis = 0;
}

bool GameServer::botFindPath(Vec2 start, Vec2 goal, std::vector<Vec2>& out) {
    // 8-connected, octile heuristic, no corner cutting, and a blocked goal
    // snapped to the nearest walkable tile. Bounded by kBotPathMaxNodes per
    // call and by a per-tick budget above that, so a whole raid replanning
    // together cannot dominate a frame.
    out.clear();
    constexpr int kDim = kTilesPerAxis;
    const auto blockedTile = [&](int tx, int ty) {
        return tileBlocks(terrain_->atTile(tx, ty));
    };

    const int sx = Terrain::toTileCoord(start.x);
    const int sy = Terrain::toTileCoord(start.y);
    int gx = Terrain::toTileCoord(goal.x);
    int gy = Terrain::toTileCoord(goal.y);
    if (sx < 0 || sy < 0 || sx >= kDim || sy >= kDim) return false;

    if (blockedTile(gx, gy)) {
        bool snapped = false;
        for (int r = 1; r <= 4 && !snapped; ++r) {
            for (int dy = -r; dy <= r && !snapped; ++dy) {
                for (int dx = -r; dx <= r && !snapped; ++dx) {
                    // Only the shell at radius r.
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    if (blockedTile(gx + dx, gy + dy)) continue;
                    gx += dx;
                    gy += dy;
                    snapped = true;
                }
            }
        }
        if (!snapped) return false;
    }
    if (gx < 0 || gy < 0 || gx >= kDim || gy >= kDim) return false;
    if (sx == gx && sy == gy) return false;

    BotPathScratch& scratch = botPath_;
    constexpr std::size_t kCells = static_cast<std::size_t>(kDim) * kDim;
    if (scratch.stamp.size() != kCells) {
        scratch.gScore.assign(kCells, 0.0);
        scratch.cameFrom.assign(kCells, -1);
        scratch.stamp.assign(kCells, 0);
        scratch.stampValue = 0;
    }
    // A wrap would make every stale entry suddenly "match", so the table is
    // cleared once in the four-billionth search rather than compared against a
    // live set every read.
    if (++scratch.stampValue == 0xFFFFFFFFu) {
        std::fill(scratch.stamp.begin(), scratch.stamp.end(), 0u);
        scratch.stampValue = 1;
    }
    const std::uint32_t stamp = scratch.stampValue;

    scratch.heapF.clear();
    scratch.heapX.clear();
    scratch.heapY.clear();
    scratch.heapSize = 0;
    const auto heapPush = [&](double f, int tx, int ty) {
        scratch.heapF.push_back(f);
        scratch.heapX.push_back(tx);
        scratch.heapY.push_back(ty);
        std::size_t i = scratch.heapSize++;
        while (i > 0) {
            const std::size_t parent = (i - 1) / 2;
            if (scratch.heapF[parent] <= scratch.heapF[i]) break;
            std::swap(scratch.heapF[parent], scratch.heapF[i]);
            std::swap(scratch.heapX[parent], scratch.heapX[i]);
            std::swap(scratch.heapY[parent], scratch.heapY[i]);
            i = parent;
        }
    };
    double popF = 0;
    int popX = 0;
    int popY = 0;
    const auto heapPop = [&]() {
        popF = scratch.heapF[0];
        popX = scratch.heapX[0];
        popY = scratch.heapY[0];
        --scratch.heapSize;
        if (scratch.heapSize == 0) {
            scratch.heapF.clear();
            scratch.heapX.clear();
            scratch.heapY.clear();
            return;
        }
        scratch.heapF[0] = scratch.heapF[scratch.heapSize];
        scratch.heapX[0] = scratch.heapX[scratch.heapSize];
        scratch.heapY[0] = scratch.heapY[scratch.heapSize];
        scratch.heapF.pop_back();
        scratch.heapX.pop_back();
        scratch.heapY.pop_back();
        std::size_t i = 0;
        while (true) {
            const std::size_t left = 2 * i + 1;
            const std::size_t right = 2 * i + 2;
            std::size_t smallest = i;
            if (left < scratch.heapSize && scratch.heapF[left] < scratch.heapF[smallest]) {
                smallest = left;
            }
            if (right < scratch.heapSize && scratch.heapF[right] < scratch.heapF[smallest]) {
                smallest = right;
            }
            if (smallest == i) break;
            std::swap(scratch.heapF[i], scratch.heapF[smallest]);
            std::swap(scratch.heapX[i], scratch.heapX[smallest]);
            std::swap(scratch.heapY[i], scratch.heapY[smallest]);
            i = smallest;
        }
    };

    const std::size_t startIndex = static_cast<std::size_t>(sy) * kDim + sx;
    scratch.gScore[startIndex] = 0.0;
    scratch.cameFrom[startIndex] = -1;
    scratch.stamp[startIndex] = stamp;
    heapPush(octileHeuristic(sx, sy, gx, gy), sx, sy);

    int expanded = 0;
    while (scratch.heapSize > 0 && expanded < kBotPathMaxNodes) {
        heapPop();
        const double f = popF;
        const int tx = popX;
        const int ty = popY;

        if (tx == gx && ty == gy) {
            // Reconstruct from the goal back to the start, exclusive.
            scratch.path.clear();
            std::size_t index = static_cast<std::size_t>(ty) * kDim + tx;
            while (index != startIndex) {
                const int px = static_cast<int>(index % kDim);
                const int py = static_cast<int>(index / kDim);
                scratch.path.push_back(Terrain::tileCenter(px, py));
                if (scratch.stamp[index] != stamp) break;
                const std::int32_t previous = scratch.cameFrom[index];
                if (previous < 0) break;
                index = static_cast<std::size_t>(previous);
            }
            out.assign(scratch.path.rbegin(), scratch.path.rend());
            return !out.empty();
        }
        ++expanded;

        const std::size_t index = static_cast<std::size_t>(ty) * kDim + tx;
        // Stale heap entry: the node was re-pushed with a lower f.
        if (scratch.stamp[index] != stamp) continue;
        const double g = scratch.gScore[index];
        if (f > g + octileHeuristic(tx, ty, gx, gy) + 1e-9) continue;

        for (const Neighbour& step : kAStarNeighbours) {
            const int nx = tx + step.dx;
            const int ny = ty + step.dy;
            if (nx < 0 || ny < 0 || nx >= kDim || ny >= kDim) continue;
            if (blockedTile(nx, ny)) continue;
            // No corner cutting: a diagonal needs both orthogonals clear.
            if (step.dx != 0 && step.dy != 0) {
                if (blockedTile(tx + step.dx, ty)) continue;
                if (blockedTile(tx, ty + step.dy)) continue;
            }
            const double tentative = g + step.cost;
            const std::size_t neighbourIndex = static_cast<std::size_t>(ny) * kDim + nx;
            const bool seen = scratch.stamp[neighbourIndex] == stamp;
            if (!seen || tentative < scratch.gScore[neighbourIndex]) {
                scratch.stamp[neighbourIndex] = stamp;
                scratch.gScore[neighbourIndex] = tentative;
                scratch.cameFrom[neighbourIndex] = static_cast<std::int32_t>(index);
                heapPush(tentative + octileHeuristic(nx, ny, gx, gy), nx, ny);
            }
        }
    }
    return false;
}

bool GameServer::botFollowPath(Bot& bot, double nowMillis, Vec2 goal, double speedMultiplier,
                               double petalExtension, Entity avoidExcept) {
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return false;
    const Vec2 at = transform->position;
    BotAiState& ai = bot.ai;

    const int goalTx = Terrain::toTileCoord(goal.x);
    const int goalTy = Terrain::toTileCoord(goal.y);

    const bool exhausted = ai.hasPath && ai.pathIndex >= ai.pathNodes.size();
    const bool goalMoved = !ai.hasPathGoal ||
                           std::abs(ai.pathGoalTileX - goalTx) > kBotPathGoalInvalidateTiles ||
                           std::abs(ai.pathGoalTileY - goalTy) > kBotPathGoalInvalidateTiles;

    bool stale = !ai.hasPath || nowMillis - ai.pathCreatedMillis > kBotPathStaleMillis ||
                 exhausted || goalMoved;

    // Staleness may not force a recompute more often than the repath floor:
    // the bot keeps following a slightly stale path meanwhile. A bot with NO
    // usable path is exempt -- without one it cannot move at all. Chasing a
    // moving boss otherwise invalidates the goal every couple of ticks and
    // drains the whole per-tick budget forever.
    const bool usable = ai.hasPath && !exhausted && !ai.pathNodes.empty();
    if (stale && usable && ai.hasRepathed && nowMillis - ai.lastRepathMillis < kBotPathMinRepathMillis) {
        stale = false;
    }

    if (stale) {
        if (botPathBudget_ <= 0) return false;
        --botPathBudget_;
        ai.hasRepathed = true;
        ai.lastRepathMillis = nowMillis;
        ai.pathIndex = 0;
        ai.pathGoalTileX = goalTx;
        ai.pathGoalTileY = goalTy;
        ai.hasPathGoal = true;
        ai.pathCreatedMillis = nowMillis;
        ai.hasPath = true;
        // A failed search caches an EMPTY path rather than nothing, so a
        // blocked bot does not burn the budget every tick; it retries once the
        // cache goes stale.
        if (!botFindPath(at, goal, ai.pathNodes)) {
            ai.pathNodes.clear();
            return false;
        }
    }

    // Skip waypoints already reached; movement smoothing routinely overshoots.
    const double reachedSq = kBotPathWaypointReachedDist * kBotPathWaypointReachedDist;
    while (ai.pathIndex < ai.pathNodes.size() &&
           distanceSq(ai.pathNodes[ai.pathIndex], at) < reachedSq) {
        ++ai.pathIndex;
    }
    if (ai.pathIndex >= ai.pathNodes.size()) return false;

    // Greedy line-of-sight smoothing: skip ahead to the farthest waypoint in
    // sight. Without it bots steer tile centre to tile centre, which is a
    // visible zigzag that becomes fast left-right snapping under powder's
    // doubled speed. The full multi-ray pass only runs on its own interval;
    // between passes one ray re-validates the current waypoint and, if the bot
    // has slid behind a corner, the full pass runs immediately.
    const bool needFullSmooth = !ai.hasSmoothed ||
                                nowMillis - ai.lastSmoothMillis >= kBotPathSmoothIntervalMillis ||
                                botRayHitsWall(at, ai.pathNodes[ai.pathIndex]);
    if (needFullSmooth) {
        ai.hasSmoothed = true;
        ai.lastSmoothMillis = nowMillis;
        while (ai.pathIndex + 1 < ai.pathNodes.size() &&
               !botRayHitsWall(at, ai.pathNodes[ai.pathIndex + 1])) {
            ++ai.pathIndex;
        }
    }

    const Vec2 toward = ai.pathNodes[ai.pathIndex] - at;
    const double dist = std::max(1e-6, toward.length());
    botDriveMove(bot, toward / dist, speedMultiplier, petalExtension, 1.0, avoidExcept);
    return true;
}

// ---------------------------------------------------------------------------
// Trajectory watchdog
// ---------------------------------------------------------------------------

void GameServer::botResetOscillation(Bot& bot, double nowMillis) {
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    BotAiState& ai = bot.ai;
    ai.oscStarted = true;
    ai.oscSampleMillis = nowMillis;
    ai.oscPosition = transform ? transform->position : ai.oscPosition;
    ai.oscStill = 0;
    ai.oscReversals = 0;
    ai.hasOscPrevStep = false;
    ai.oscTrack.clear();
}

bool GameServer::botDetectOscillation(Bot& bot, double nowMillis) {
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return false;
    BotAiState& ai = bot.ai;
    if (!ai.oscStarted) {
        botResetOscillation(bot, nowMillis);
        return false;
    }
    if (nowMillis - ai.oscSampleMillis < kBotOscSampleMillis) return false;

    const Vec2 at = transform->position;
    const Vec2 step = at - ai.oscPosition;
    ai.oscSampleMillis = nowMillis;
    ai.oscPosition = at;

    const double length = step.length();
    if (length < kBotOscMinStep) {
        ++ai.oscStill;
        ai.oscReversals = 0;
    } else {
        ai.oscStill = 0;
        if (ai.hasOscPrevStep) {
            const double previousLength = std::max(1e-6, ai.oscPrevStep.length());
            const double dot = (step.x * ai.oscPrevStep.x + step.y * ai.oscPrevStep.y) /
                               (length * previousLength);
            ai.oscReversals = dot < kBotOscReversalDot ? ai.oscReversals + 1 : 0;
        }
        ai.oscPrevStep = step;
        ai.hasOscPrevStep = true;
    }

    if (ai.oscReversals >= kBotOscTripReversals || ai.oscStill >= kBotOscStillTrips) {
        ai.oscReversals = 0;
        ai.oscStill = 0;
        ai.hasOscPrevStep = false;
        return true;
    }

    // Net-displacement test -- the one that catches circling.
    //
    // A bot going round in a circle never reverses (consecutive samples sit at
    // a small angle, nowhere near the reversal threshold) and is never still,
    // so it can circle indefinitely while the two tests above report
    // everything is fine. Only run while the bot has nothing to fight or pick
    // up: circling a mob at the standoff ring produces exactly this signature
    // and is the bot doing its job. The target and pickup here still hold LAST
    // tick's decision, which is the right thing to ask -- it says what the bot
    // was doing while it drew the trajectory being judged.
    if (ai.target != NULL_ENTITY || ai.pickup != NULL_ENTITY) {
        ai.oscTrack.clear();
        return false;
    }

    ai.oscTrack.push_back(at);
    if (ai.oscTrack.size() > static_cast<std::size_t>(kBotOscNetWindow)) {
        ai.oscTrack.erase(ai.oscTrack.begin());
    }
    if (ai.oscTrack.size() == static_cast<std::size_t>(kBotOscNetWindow)) {
        double pathLength = 0;
        for (std::size_t i = 1; i < ai.oscTrack.size(); ++i) {
            pathLength += (ai.oscTrack[i] - ai.oscTrack[i - 1]).length();
        }
        const double drift = (ai.oscTrack.back() - ai.oscTrack.front()).length();
        if (pathLength > kBotOscNetMinPath && drift < kBotOscNetMaxDrift) {
            ai.oscTrack.clear();
            return true;
        }
    }
    return false;
}

Vec2 GameServer::botPickUnstickDirection(const Bot& bot) {
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    const Vec2 at = transform ? transform->position : Vec2{kWorldHalf, kWorldHalf};
    const BotAiState& ai = bot.ai;

    // Sidestep first -- perpendicular to the rut is the shortest way out of a
    // two-point shuffle -- then progressively wider, and finally straight
    // back. The leading side is randomised so a knot of stuck bots does not
    // all peel off the same way.
    const double base = std::atan2(ai.heading.y, ai.heading.x);
    const double side = rng_.chance(0.5) ? 1.0 : -1.0;
    const double offsets[] = {
        (kPi / 2.0) * side,        -(kPi / 2.0) * side,
        (2.0 * kPi / 3.0) * side,  -(2.0 * kPi / 3.0) * side,
        kPi,
        (kPi / 3.0) * side,        -(kPi / 3.0) * side,
        0.0,
    };
    // Full probe distance first, then progressively shorter. In a corner every
    // direction is blocked at the full distance, and answering that with a
    // uniformly random heading points back into a wall more often than not.
    // Shortening the probe asks the smaller question the bot can act on: not
    // "where can I run 260 units?" but "which way is there any room at all?".
    for (const double probe :
         {kBotUnstickProbeDist, kBotUnstickProbeDist / 2.0, kBotUnstickProbeDist / 4.0}) {
        for (const double offset : offsets) {
            const Vec2 direction = Vec2::fromAngle(base + offset);
            if (!botRayHitsWall(at, at + direction * probe)) return direction;
        }
    }
    // Genuinely walled in on every heading even at close range. Aim at the
    // middle of the map rather than at random: it is the one direction
    // guaranteed not to be further into the map edge.
    const Vec2 toCentre = Vec2{kWorldHalf, kWorldHalf} - at;
    if (toCentre.length() > 1.0) return toCentre.normalized();
    return Vec2::fromAngle(rng_.angle());
}

int GameServer::botTangentDirection(Bot& bot, double nowMillis) {
    BotAiState& ai = bot.ai;
    if (ai.strafeDir == 0) {
        ai.strafeDir = (bot.id & 1u) == 0u ? 1 : -1;
        ai.strafeFlipMillis =
            nowMillis + rng_.range(kBotStrafeFlipMinMillis, kBotStrafeFlipMaxMillis);
    } else if (nowMillis >= ai.strafeFlipMillis) {
        ai.strafeDir = -ai.strafeDir;
        ai.strafeFlipMillis =
            nowMillis + rng_.range(kBotStrafeFlipMinMillis, kBotStrafeFlipMaxMillis);
    }
    return ai.strafeDir;
}

// ---------------------------------------------------------------------------
// Per-tick indexes
// ---------------------------------------------------------------------------

void GameServer::rebuildBotBossIndex(double nowMillis) {
    botBosses_.clear();
    Query<MobTag, MobType, Transform> mobs{world_};
    mobs.each([&](Entity e, MobTag&, MobType& type, Transform&) {
        if (!isBotBossTier(type.rarity)) return;
        if (world_.has<Dead>(e) || world_.has<Pet>(e)) return;
        if (excludedMobType(type.configIndex)) return;
        botBosses_.push_back(e);
        botBossFirstSeen_.emplace(e, nowMillis);
    });

    // Forget the ones that are gone, or both maps grow for the life of the
    // server. Handles carry a generation, so a recycled slot is a new key.
    if (botBossFirstSeen_.size() > 64) {
        for (auto it = botBossFirstSeen_.begin(); it != botBossFirstSeen_.end();) {
            it = (world_.isAlive(it->first) && !world_.has<Dead>(it->first))
                     ? std::next(it)
                     : botBossFirstSeen_.erase(it);
        }
    }
    if (botAnnouncedBosses_.size() > 32) {
        for (auto it = botAnnouncedBosses_.begin(); it != botAnnouncedBosses_.end();) {
            it = (world_.isAlive(it->first) && !world_.has<Dead>(it->first))
                     ? std::next(it)
                     : botAnnouncedBosses_.erase(it);
        }
    }
}

void GameServer::computeBotGroups() {
    // Deterministic grouping: the roster is sorted by id and dealt round-robin
    // into buckets of about seven, so a bot keeps its group across ticks even
    // as members die and respawn.
    botGroups_.clear();
    botOrderScratch_.clear();
    for (std::size_t i = 0; i < bots_.size(); ++i) {
        const Bot& bot = bots_[i];
        if (bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity)) continue;
        if (world_.has<Dead>(bot.entity)) continue;
        botOrderScratch_.push_back(i);
    }
    if (botOrderScratch_.empty()) return;
    std::sort(botOrderScratch_.begin(), botOrderScratch_.end(),
              [&](std::size_t a, std::size_t b) { return bots_[a].id < bots_[b].id; });

    const std::size_t count = botOrderScratch_.size();
    const std::size_t groups =
        std::max<std::size_t>(1, (count + kBotGroupTargetSize - 1) / kBotGroupTargetSize);
    std::vector<Vec2> centres(groups, Vec2{0, 0});
    std::vector<int> sizes(groups, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const Bot& bot = bots_[botOrderScratch_[i]];
        const Transform* transform = world_.tryGet<Transform>(bot.entity);
        if (transform == nullptr) continue;
        centres[i % groups] += transform->position;
        ++sizes[i % groups];
    }
    for (std::size_t g = 0; g < groups; ++g) {
        if (sizes[g] > 0) centres[g] = centres[g] / static_cast<double>(sizes[g]);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const Bot& bot = bots_[botOrderScratch_[i]];
        botGroups_[bot.entity] = BotGroup{centres[i % groups], sizes[i % groups]};
    }
}

void GameServer::computeBotRaidSlots(double nowMillis) {
    // Each raiding bot owns a fixed angular slot around its rally point, so a
    // raid spreads evenly around the boss rather than stacking on one side.
    botRaidSlots_.clear();
    Vec2 forced;
    const bool hasForced = activeForcedRaidAnchor(nowMillis, forced);

    std::unordered_map<std::uint64_t, std::vector<std::size_t>> buckets;
    for (std::size_t i = 0; i < bots_.size(); ++i) {
        const Bot& bot = bots_[i];
        if (bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity)) continue;
        if (world_.has<Dead>(bot.entity)) continue;

        Vec2 anchor;
        if (hasForced) {
            anchor = forced;
        } else {
            double distance = 0;
            if (!botNearestBoss(bot, anchor, distance)) continue;
        }
        buckets[raidBucketKey(anchor)].push_back(i);
    }

    for (auto& entry : buckets) {
        std::vector<std::size_t>& members = entry.second;
        // Sorted by id, so the slots do not shuffle from tick to tick.
        std::sort(members.begin(), members.end(),
                  [&](std::size_t a, std::size_t b) { return bots_[a].id < bots_[b].id; });
        const double count = static_cast<double>(members.size());
        for (std::size_t i = 0; i < members.size(); ++i) {
            botRaidSlots_[bots_[members[i]].entity] = (static_cast<double>(i) / count) * kTau;
        }
    }
}

// ---------------------------------------------------------------------------
// Raids
// ---------------------------------------------------------------------------

namespace {

/// Squared distance to the nearest live human, or infinity when nobody is
/// connected -- in which case recency alone decides the raid target.
double distSqToNearestHuman(const World& world,
                            const std::unordered_map<net::ConnectionId, Session>& sessions,
                            Vec2 at) {
    double best = std::numeric_limits<double>::max();
    for (const auto& entry : sessions) {
        const Session& session = entry.second;
        if (!session.playing()) continue;
        if (world.has<Dead>(session.entity)) continue;
        const Transform* transform = world.tryGet<Transform>(session.entity);
        if (transform == nullptr) continue;
        best = std::min(best, distanceSq(transform->position, at));
    }
    return best;
}

} // namespace

bool GameServer::botNearestBoss(const Bot& bot, Vec2& out, double& distOut) {
    // Only bosses within raid range of THIS bot. Without the range gate every
    // bot in the world enters raid mode for any boss anywhere -- they equip
    // powder, blitz across the map and ram every mob in their path, because
    // the traversal branch skips the standoff bands. Among those in range,
    // uniques beat supers, then most recently seen, then proximity to a human.
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return false;
    const Vec2 at = transform->position;

    Entity best = NULL_ENTITY;
    bool preferUnique = false;
    double bestSeen = 0;
    double bestHumanDistSq = 0;

    for (const Entity boss : botBosses_) {
        if (!world_.isAlive(boss) || world_.has<Dead>(boss)) continue;
        const MobType* type = world_.tryGet<MobType>(boss);
        const Transform* bossTransform = world_.tryGet<Transform>(boss);
        if (type == nullptr || bossTransform == nullptr) continue;
        if (distanceSq(bossTransform->position, at) > kBotBossRaidRange * kBotBossRaidRange) {
            continue;
        }
        // A unique outranks every super outright: the first one seen discards
        // whatever supers were collected, and supers stop counting from then
        // on.
        const bool unique = type->rarity == Rarity::Unique;
        if (!unique && preferUnique) continue;
        if (unique && !preferUnique) {
            preferUnique = true;
            best = NULL_ENTITY;
        }
        // Apex is a boss tier for the aggro table and for target priority, but
        // the reference's raid POOL only ever admits supers and uniques.
        if (!unique && type->rarity != Rarity::Super) continue;

        const auto seenIt = botBossFirstSeen_.find(boss);
        const double seen = seenIt == botBossFirstSeen_.end() ? 0.0 : seenIt->second;
        const double humanDistSq =
            distSqToNearestHuman(world_, sessions_, bossTransform->position);
        if (best == NULL_ENTITY || seen > bestSeen ||
            (seen == bestSeen && humanDistSq < bestHumanDistSq)) {
            best = boss;
            bestSeen = seen;
            bestHumanDistSq = humanDistSq;
        }
    }
    if (best == NULL_ENTITY) return false;
    out = world_.get<Transform>(best).position;
    distOut = (out - at).length();
    return true;
}

bool GameServer::triggerBotRaid(double nowMillis) {
    // The best boss in the WORLD, ignoring distance: uniques strictly first,
    // then most recently seen, then whichever is closest to a human. That
    // makes bots commit to a fresh boss bothering somebody rather than to
    // whatever stale one happens to come first out of the world.
    Entity best = NULL_ENTITY;
    bool preferUnique = false;
    double bestSeen = 0;
    double bestHumanDistSq = 0;

    for (const Entity boss : botBosses_) {
        if (!world_.isAlive(boss) || world_.has<Dead>(boss)) continue;
        const MobType* type = world_.tryGet<MobType>(boss);
        const Transform* transform = world_.tryGet<Transform>(boss);
        if (type == nullptr || transform == nullptr) continue;
        const bool unique = type->rarity == Rarity::Unique;
        if (!unique && preferUnique) continue;
        if (unique && !preferUnique) {
            preferUnique = true;
            best = NULL_ENTITY;
        }
        if (!unique && type->rarity != Rarity::Super) continue;

        const auto seenIt = botBossFirstSeen_.find(boss);
        const double seen = seenIt == botBossFirstSeen_.end() ? 0.0 : seenIt->second;
        const double humanDistSq = distSqToNearestHuman(world_, sessions_, transform->position);
        if (best == NULL_ENTITY || seen > bestSeen ||
            (seen == bestSeen && humanDistSq < bestHumanDistSq)) {
            best = boss;
            bestSeen = seen;
            bestHumanDistSq = humanDistSq;
        }
    }

    if (best == NULL_ENTITY) {
        botForcedRaid_.active = false;
        return false;
    }
    botForcedRaid_.active = true;
    botForcedRaid_.at = world_.get<Transform>(best).position;
    botForcedRaid_.tier = world_.get<MobType>(best).rarity;
    botForcedRaid_.untilMillis = nowMillis + kBotForcedRaidMillis;
    // Every cached path now aims at the wrong place.
    for (Bot& bot : bots_) botClearPath(bot.ai);
    return true;
}

bool GameServer::activeForcedRaidAnchor(double nowMillis, Vec2& out) {
    if (!botForcedRaid_.active) return false;
    if (nowMillis > botForcedRaid_.untilMillis) {
        botForcedRaid_.active = false;
        return false;
    }
    // Refresh the rally point to a live boss of the preferred tier, so bots
    // home in on something that is still there rather than on a stale point.
    Entity best = NULL_ENTITY;
    bool preferUnique = false;
    double bestSeen = 0;
    for (const Entity boss : botBosses_) {
        if (!world_.isAlive(boss) || world_.has<Dead>(boss)) continue;
        const MobType* type = world_.tryGet<MobType>(boss);
        if (type == nullptr) continue;
        const bool unique = type->rarity == Rarity::Unique;
        if (!unique && preferUnique) continue;
        if (unique && !preferUnique) {
            preferUnique = true;
            best = NULL_ENTITY;
        }
        if (!unique && type->rarity != Rarity::Super) continue;
        const auto seenIt = botBossFirstSeen_.find(boss);
        const double seen = seenIt == botBossFirstSeen_.end() ? 0.0 : seenIt->second;
        if (best == NULL_ENTITY || seen > bestSeen) {
            best = boss;
            bestSeen = seen;
        }
    }
    if (best == NULL_ENTITY) {
        botForcedRaid_.active = false;
        return false;
    }
    botForcedRaid_.at = world_.get<Transform>(best).position;
    botForcedRaid_.tier = world_.get<MobType>(best).rarity;
    out = botForcedRaid_.at;
    return true;
}

void GameServer::announceNewBosses(double nowMillis) {
    // First pass: silently absorb every boss that was already alive when the
    // controller came up, so a restart does not produce a burst of callouts
    // for the whole standing wave.
    if (!botBossAnnounceReady_) {
        for (const Entity boss : botBosses_) botAnnouncedBosses_[boss] = true;
        botBossAnnounceReady_ = true;
        // A full cooldown before the very first real announcement, too.
        botNextBossAnnounceMillis_ =
            nowMillis + rng_.range(kBotBossAnnounceMinMillis, kBotBossAnnounceMaxMillis);
        return;
    }
    if (nowMillis < botNextBossAnnounceMillis_) return;

    for (const Entity boss : botBosses_) {
        if (botAnnouncedBosses_.count(boss) != 0) continue;
        const MobType* type = world_.tryGet<MobType>(boss);
        const Transform* transform = world_.tryGet<Transform>(boss);
        if (type == nullptr || transform == nullptr) continue;

        // The nearest live bot does the shouting.
        const Bot* announcer = nullptr;
        double bestDistSq = std::numeric_limits<double>::max();
        for (const Bot& bot : bots_) {
            if (bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity)) continue;
            if (world_.has<Dead>(bot.entity)) continue;
            const Transform* botTransform = world_.tryGet<Transform>(bot.entity);
            if (botTransform == nullptr) continue;
            const double distSq = distanceSq(botTransform->position, transform->position);
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                announcer = &bot;
            }
        }
        if (announcer == nullptr) return;

        const bool unique = type->rarity == Rarity::Unique;
        const std::string tierWord = unique ? "unique" : "super";
        const Squad* squad = squads_.forMember(SquadMemberId::ofBot(announcer->entity));

        // A {code} template only makes sense from a bot that is in a squad.
        std::string shout;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const char* const* pool = unique ? kBossShoutUnique : kBossShoutSuper;
            const std::size_t size =
                unique ? std::size(kBossShoutUnique) : std::size(kBossShoutSuper);
            shout = pool[rng_.below(static_cast<std::uint32_t>(size))];
            if (squad != nullptr || shout.find("{code}") == std::string::npos) break;
        }
        if (squad == nullptr && shout.find("{code}") != std::string::npos) {
            shout = "{tier} {mob}";
        }
        replaceAll(shout, "{tier}", tierWord);
        replaceAll(shout, "{mob}", spokenMobName(content().mob(type->configIndex).id));
        if (squad != nullptr) replaceAll(shout, "{code}", squad->id);

        broadcastChat(net::ChatChannel::Global, announcer->name, shout);
        botAnnouncedBosses_[boss] = true;
        botNextBossAnnounceMillis_ =
            nowMillis + rng_.range(kBotBossAnnounceMinMillis, kBotBossAnnounceMaxMillis);
        // Rally everyone: the chat handler's own trigger cannot fire for a
        // line the server emitted itself.
        triggerBotRaid(nowMillis);
        return;  // One announcement per pass.
    }
}

// ---------------------------------------------------------------------------
// Squads
// ---------------------------------------------------------------------------

void GameServer::updateBotSquads(double nowMillis) {
    for (Bot& bot : bots_) {
        if (bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity)) continue;
        if (world_.has<Dead>(bot.entity)) continue;

        if (nowMillis < bot.ai.nextSquadMillis) continue;
        // Jittered, so bots do not all evaluate on the same tick.
        bot.ai.nextSquadMillis = nowMillis + kBotSquadTickMillis + rng_.unit() * 4000.0;

        const SquadMemberId member = SquadMemberId::ofBot(bot.entity);
        if (squads_.forMember(member) != nullptr) continue;

        // Joining an existing public squad comes first: a bot that hosts is
        // only useful once somebody can find it.
        const std::vector<const Squad*> open = squads_.publicSquads();
        if (!open.empty() && rng_.chance(kBotSquadJoinChance)) {
            const std::string id = open[rng_.below(static_cast<std::uint32_t>(open.size()))]->id;
            if (squads_.addBot(id, member).empty()) {
                if (Squad* joined = squads_.find(id)) {
                    sendSquadSystem(*joined, bot.name + " has joined the squad.");
                    broadcastSquadUpdate(*joined);
                }
            }
            continue;
        }

        // Occasionally host one. The bot advertises the code on its next boss
        // callout, through the {code} templates.
        if (rng_.chance(kBotSquadCreateChance)) squads_.create(member, true, rng_);
    }
}

// ---------------------------------------------------------------------------
// Loadout swaps
// ---------------------------------------------------------------------------

namespace {

/// The tier a swapped-in petal is rolled at: the best the bot already wears,
/// so an apex bot does not walk around with a permanently rolled apex petal
/// nobody else can craft, and a common bot is not handed something it could
/// never own.
Rarity swapRarity(const Loadout& loadout, int floorIndex) {
    int best = 0;
    for (int i = 0; i < kLoadoutActiveSlots; ++i) {
        const LoadoutSlot& slot = loadout.slots[static_cast<std::size_t>(i)];
        if (slot.empty()) continue;
        best = std::max(best, rarityIndex(slot.rarity));
    }
    return static_cast<Rarity>(std::max(floorIndex, best));
}

} // namespace

void GameServer::botEquipPowder(Bot& bot) {
    BotAiState& ai = bot.ai;
    if (ai.powderSwapped) return;
    Loadout* loadout = world_.tryGet<Loadout>(bot.entity);
    if (loadout == nullptr) return;
    const std::uint16_t powder = content().petalIndex("powder");
    if (powder == kInvalidIndex) return;

    LoadoutSlot& slot = loadout->slots[0];
    // Already carrying one -- nothing to swap, and nothing to remember.
    if (slot.configIndex == powder) return;

    ai.powderOriginal = slot;
    ai.powderSwapped = true;
    slot = LoadoutSlot{};
    slot.configIndex = powder;
    slot.rarity = swapRarity(*loadout, kBotPowderMinRarityIndex);
}

void GameServer::botUnequipPowder(Bot& bot) {
    BotAiState& ai = bot.ai;
    if (!ai.powderSwapped) return;
    ai.powderSwapped = false;
    if (Loadout* loadout = world_.tryGet<Loadout>(bot.entity)) {
        loadout->slots[0] = ai.powderOriginal;
    }
}

void GameServer::botEquipYggdrasil(Bot& bot) {
    BotAiState& ai = bot.ai;
    if (ai.yggSwapped) return;
    Loadout* loadout = world_.tryGet<Loadout>(bot.entity);
    if (loadout == nullptr) return;
    const std::uint16_t yggdrasil = content().petalIndex("yggdrasil");
    if (yggdrasil == kInvalidIndex) return;

    LoadoutSlot& slot = loadout->slots[1];
    if (slot.configIndex == yggdrasil) return;

    ai.yggOriginal = slot;
    ai.yggSwapped = true;
    slot = LoadoutSlot{};
    slot.configIndex = yggdrasil;
    slot.rarity = swapRarity(*loadout, 0);
}

void GameServer::botUnequipYggdrasil(Bot& bot) {
    BotAiState& ai = bot.ai;
    if (!ai.yggSwapped) return;
    ai.yggSwapped = false;
    if (Loadout* loadout = world_.tryGet<Loadout>(bot.entity)) {
        loadout->slots[1] = ai.yggOriginal;
    }
}

bool GameServer::botHasNearbyBuddy(const Bot& bot, double range) const {
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return false;
    const double rangeSq = range * range;
    for (const Bot& other : bots_) {
        if (other.entity == bot.entity || other.entity == NULL_ENTITY) continue;
        if (!world_.isAlive(other.entity) || world_.has<Dead>(other.entity)) continue;
        const Transform* otherTransform = world_.tryGet<Transform>(other.entity);
        if (otherTransform == nullptr) continue;
        if (distanceSq(otherTransform->position, transform->position) <= rangeSq) return true;
    }
    return false;
}

Entity GameServer::botFindReviveTarget(const Bot& bot) const {
    // No yggdrasil, no revive capability, and no reason to make the trip. The
    // swap is checked first; a bot whose NATIVE loadout rolled a yggdrasil
    // also seeks corpses, because the petal works either way.
    if (!bot.ai.yggSwapped) {
        const Loadout* loadout = world_.tryGet<Loadout>(bot.entity);
        if (loadout == nullptr) return NULL_ENTITY;
        const std::uint16_t yggdrasil = content().petalIndex("yggdrasil");
        bool carries = false;
        for (int i = 0; i < kLoadoutActiveSlots && !carries; ++i) {
            carries = loadout->slots[static_cast<std::size_t>(i)].configIndex == yggdrasil;
        }
        if (!carries) return NULL_ENTITY;
    }

    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return NULL_ENTITY;
    const double rangeSq = kBotYggReviveSeekRange * kBotYggReviveSeekRange;

    Entity best = NULL_ENTITY;
    double bestDistSq = std::numeric_limits<double>::max();
    for (const Bot& other : bots_) {
        if (other.entity == bot.entity || other.entity == NULL_ENTITY) continue;
        if (!world_.isAlive(other.entity) || !world_.has<Dead>(other.entity)) continue;
        const Transform* otherTransform = world_.tryGet<Transform>(other.entity);
        if (otherTransform == nullptr) continue;
        const double distSq = distanceSq(otherTransform->position, transform->position);
        if (distSq > rangeSq || distSq >= bestDistSq) continue;
        best = other.entity;
        bestDistSq = distSq;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Targets and loot
// ---------------------------------------------------------------------------

Entity GameServer::botPickTarget(const Bot& bot, const BotModeContext& mode, double nowMillis,
                                 double& distOut) {
    (void)nowMillis;
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return NULL_ENTITY;
    const Vec2 at = transform->position;

    // Score = priority * 10000 - distance, so a boss inside its own aggro
    // range beats every regular mob and the closer of two same-tier mobs wins.
    // The tier this bot's gear says it should be farming gets a half-priority
    // bump, enough to beat an unpreferred mob of the same class but never a
    // boss. Whatever the bot is already committed to gets a flat bonus on top,
    // so a tie resolves in favour of staying committed rather than walking
    // half-way to one mob and turning back toward the other.
    const int rarityIdx = botMaxRarityIndex(bot);
    const auto preferred = [&](Rarity tier) {
        // "One tier above your gear", except mythic stays on mythic and ultra+
        // goes boss hunting.
        if (rarityIdx >= rarityIndex(Rarity::Ultra)) return isBotBossTier(tier);
        if (rarityIdx == rarityIndex(Rarity::Mythic)) return tier == Rarity::Mythic;
        if (rarityIdx + 1 < kRarityCount) return rarityIndex(tier) == rarityIdx + 1;
        return false;
    };

    Entity best = NULL_ENTITY;
    double bestScore = -std::numeric_limits<double>::max();
    double bestDist = 0;

    const auto score = [&](Entity candidate) {
        if (!world_.isAlive(candidate) || world_.has<Dead>(candidate)) return;
        if (!world_.has<MobTag>(candidate) || world_.has<Pet>(candidate)) return;
        const MobType* type = world_.tryGet<MobType>(candidate);
        const Transform* other = world_.tryGet<Transform>(candidate);
        if (type == nullptr || other == nullptr) return;
        if (excludedMobType(type->configIndex)) return;

        const bool boss = isBotBossTier(type->rarity);
        // The tether applies to everything except a boss: a boss is a raid,
        // and bots are allowed to crowd in from across the map for one.
        if (!boss && mode.hasAnchor &&
            distanceSq(mode.anchor, other->position) > mode.tetherRadius * mode.tetherRadius) {
            return;
        }
        const double dist = (other->position - at).length();
        if (dist > botAggroRangeForTier(type->rarity)) return;

        const double priority = botTierPriority(type->rarity) + (preferred(type->rarity) ? 0.5 : 0.0);
        const double sticky = candidate == bot.ai.target ? kBotTargetStickiness : 0.0;
        const double value = priority * 10000.0 - dist + sticky;
        if (value > bestScore) {
            bestScore = value;
            best = candidate;
            bestDist = dist;
        }
    };

    // A non-boss can only pass the range gate inside the widest non-boss aggro
    // range, so one grid query at that radius sees every possible winner.
    // Bosses reach much further and skip the tether, so they come from the
    // per-tick index instead of from the grid.
    botCandidates_.clear();
    grid_.query(at, kBotHighTierAggroRange, botCandidates_);
    for (const Entity candidate : botCandidates_) {
        const MobType* type = world_.tryGet<MobType>(candidate);
        if (type != nullptr && isBotBossTier(type->rarity)) continue;
        score(candidate);
    }
    for (const Entity boss : botBosses_) score(boss);

    distOut = bestDist;
    return best;
}

Entity GameServer::botFindInterceptingMob(Vec2 at, Vec2 direction, Entity except, double range,
                                          double& distOut) {
    // Any non-target mob sitting inside a forward cone close enough that
    // carrying on without engaging would body-slam into it. The bot drops into
    // its ordinary combat bands against the obstacle for a tick, lets its
    // petals hit it, and resumes the chase next tick -- without this, a bot
    // beelining for a far-off boss (powdered up, most of all) ploughs through
    // every mob in between without its petals ever locking on.
    Entity best = NULL_ENTITY;
    double bestDist = std::numeric_limits<double>::max();
    botCandidates_.clear();
    grid_.query(at, range, botCandidates_);
    for (const Entity candidate : botCandidates_) {
        if (candidate == except) continue;
        if (!world_.isAlive(candidate) || world_.has<Dead>(candidate)) continue;
        if (!world_.has<MobTag>(candidate) || world_.has<Pet>(candidate)) continue;
        const MobType* type = world_.tryGet<MobType>(candidate);
        const Transform* other = world_.tryGet<Transform>(candidate);
        const Body* body = world_.tryGet<Body>(candidate);
        if (type == nullptr || other == nullptr || body == nullptr) continue;
        if (excludedMobType(type->configIndex)) continue;

        const Vec2 toward = other->position - at;
        const double distSq = toward.lengthSq();
        if (distSq == 0.0) continue;
        const double cutoff = range + body->radius;
        if (distSq > cutoff * cutoff) continue;
        const double dist = std::sqrt(distSq);
        // A dot above 0.3 is roughly a 70-degree arc ahead of the bot.
        if ((toward.x / dist) * direction.x + (toward.y / dist) * direction.y < 0.3) continue;
        if (dist < bestDist) {
            best = candidate;
            bestDist = dist;
        }
    }
    distOut = bestDist;
    return best;
}

Entity GameServer::botFindPickup(const Bot& bot, const BotModeContext& mode, double& distOut) {
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return NULL_ENTITY;
    const Vec2 at = transform->position;

    Entity best = NULL_ENTITY;
    // The item the bot is already walking to competes with a discount, so two
    // drops at similar range do not trade places every tick and leave the bot
    // shuffling between them.
    double bestScore = kBotItemSeekRange;
    double bestDist = kBotItemSeekRange;

    // Widened by the stickiness discount: a drop the bot is already walking to
    // stays the best pick out to seek range PLUS that discount, so the query
    // has to be able to see it or the commitment silently expires at the range
    // gate instead of at the score.
    botCandidates_.clear();
    grid_.query(at, kBotItemSeekRange + kBotPickupStickiness, botCandidates_);
    for (const Entity candidate : botCandidates_) {
        if (!world_.isAlive(candidate)) continue;
        const DropItem* drop = world_.tryGet<DropItem>(candidate);
        const Transform* other = world_.tryGet<Transform>(candidate);
        if (drop == nullptr || other == nullptr) continue;
        if (std::find(drop->pickedUpBy.begin(), drop->pickedUpBy.end(), bot.entity) !=
            drop->pickedUpBy.end()) {
            continue;
        }
        // Only chase what this bot actually earned: a drop with a reservation
        // list is reserved.
        if (!drop->eligible.empty() &&
            std::find(drop->eligible.begin(), drop->eligible.end(), bot.entity) ==
                drop->eligible.end()) {
            continue;
        }
        // And nothing that would drag it out of its cluster.
        if (mode.hasAnchor &&
            distanceSq(mode.anchor, other->position) > mode.tetherRadius * mode.tetherRadius) {
            continue;
        }
        const double dist = (other->position - at).length();
        const double scored = candidate == bot.ai.pickup ? dist - kBotPickupStickiness : dist;
        if (scored < bestScore) {
            bestScore = scored;
            bestDist = dist;
            best = candidate;
        }
    }
    distOut = bestDist;
    return best;
}

bool GameServer::botHasHighRarityMobNearby(const Bot& bot, double range) {
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return false;
    const Vec2 at = transform->position;
    const double rangeSq = range * range;
    botCandidates_.clear();
    grid_.query(at, range, botCandidates_);
    for (const Entity candidate : botCandidates_) {
        if (!world_.isAlive(candidate) || world_.has<Dead>(candidate)) continue;
        if (!world_.has<MobTag>(candidate) || world_.has<Pet>(candidate)) continue;
        const MobType* type = world_.tryGet<MobType>(candidate);
        const Transform* other = world_.tryGet<Transform>(candidate);
        if (type == nullptr || other == nullptr) continue;
        if (!isBotHighTier(type->rarity)) continue;
        if (distanceSq(other->position, at) <= rangeSq) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------

bool GameServer::botPickFarmZone(const Bot& bot, int rarityIndexValue, int rotation,
                                 Vec2& out) const {
    // The zone type that produces the tier this bot should be fighting: the
    // bot already prefers a mob one tier above its gear, and a spawn zone
    // spawns mobs of its declared tier.
    const auto zoneTierFor = [](int index) {
        if (index >= rarityIndex(Rarity::Ultra)) return Rarity::Mythic;   // ultra+ hunt mythic ground
        if (index == rarityIndex(Rarity::Mythic)) return Rarity::Mythic;  // mythic stays on mythic
        if (index == rarityIndex(Rarity::Legendary)) return Rarity::Mythic;
        if (index == rarityIndex(Rarity::Epic)) return Rarity::Legendary;
        if (index == rarityIndex(Rarity::Rare)) return Rarity::Epic;
        if (index == rarityIndex(Rarity::Uncommon)) return Rarity::Rare;
        return Rarity::Uncommon;
    };

    // Preferred type first, then down toward common, then up toward mythic.
    // Stops at the first type the map actually declares any zones for, so a
    // map without rare zones still routes an uncommon-tier bot somewhere
    // sensible.
    std::vector<Rarity> candidates;
    const auto push = [&](Rarity tier) {
        if (std::find(candidates.begin(), candidates.end(), tier) == candidates.end()) {
            candidates.push_back(tier);
        }
    };
    push(zoneTierFor(rarityIndexValue));
    for (int i = rarityIndexValue; i >= 0; --i) push(zoneTierFor(i));
    for (int i = rarityIndexValue + 1; i <= rarityIndex(Rarity::Ultra); ++i) push(zoneTierFor(i));

    std::vector<Vec2> zones;
    for (const Rarity tier : candidates) {
        for (const MapElement& element : mapData_.elements()) {
            if (element.kind != MapElementKind::Spawn || !element.hasSpawnTier) continue;
            if (element.spawnTier != tier) continue;
            if (element.bounds.w <= 0 || element.bounds.h <= 0) continue;
            zones.push_back(element.centre());
        }
        if (!zones.empty()) break;
    }
    if (zones.empty()) return false;

    // Ordered by WORLD POSITION, not by distance from the bot. Distance
    // ordering made the pick unstable: as the bot walked toward the k-th
    // nearest zone that zone became the (k-1)-th, the pick slid onto another
    // one, and the bot turned around -- a permanent two-point shuffle between
    // whichever pair kept trading places. `rotation` is how a bot is moved on
    // deliberately instead of it happening as a side effect of moving.
    std::sort(zones.begin(), zones.end(), [](Vec2 a, Vec2 b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    const std::size_t count = zones.size();
    // The bot's own id, so different bots gravitate to different zones instead
    // of all piling onto the same one.
    const std::size_t index =
        (static_cast<std::size_t>(bot.id) + static_cast<std::size_t>(rotation)) % count;
    out = zones[index];
    return true;
}

GameServer::BotModeContext GameServer::computeBotMode(Bot& bot, double nowMillis) {
    BotModeContext mode;
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    const Vec2 at = transform ? transform->position : Vec2{};
    BotAiState& ai = bot.ai;

    // Forced raid: every bot rallies regardless of distance and regardless of
    // whether any human is in that biome. Without it bots stay tethered to
    // their own ground and never cross into an empty one to engage a boss that
    // spawned there.
    Vec2 forced;
    if (activeForcedRaidAnchor(nowMillis, forced)) {
        mode.kind = BotMode::Raid;
        mode.anchor = forced;
        mode.hasAnchor = true;
        mode.tetherRadius = kBotRaidClusterRadius;
        mode.returnRadius = kBotRaidClusterReturn;
        return mode;
    }

    // Any boss within raid range. Rally on it, and clump tight enough that
    // every raider shares most of its viewport with every other raider.
    Vec2 boss;
    double bossDist = 0;
    if (botNearestBoss(bot, boss, bossDist)) {
        mode.kind = BotMode::Raid;
        mode.anchor = boss;
        mode.hasAnchor = true;
        mode.tetherRadius = kBotRaidClusterRadius;
        mode.returnRadius = kBotRaidClusterReturn;
        return mode;
    }

    // High rarity: a legendary-or-better mob nearby AND a group with enough
    // members to justify grouping. The scan result is latched, or a mob
    // wandering across the boundary would flip the anchor between the group
    // centroid and a far-away farm zone on alternating ticks and the bot would
    // visibly stutter between the two.
    if (botHasHighRarityMobNearby(bot, kBotHighRarityScanRange)) {
        ai.highRarityUntilMillis = nowMillis + kBotHighRarityLingerMillis;
    }
    if (nowMillis < ai.highRarityUntilMillis) {
        const auto group = botGroups_.find(bot.entity);
        if (group != botGroups_.end() && group->second.size >= kBotGroupMinForMode) {
            mode.kind = BotMode::HighRarity;
            mode.anchor = group->second.centre;
            mode.hasAnchor = true;
            mode.tetherRadius = kBotGroupClusterRadius;
            mode.returnRadius = kBotGroupClusterReturn;
            return mode;
        }
    }

    // Otherwise every bot heads for the spawn zone matching its band, so it
    // farms the tier its loadout was tuned for instead of wandering wherever
    // it happened to spawn. The zone is cached on the bot: it changes when the
    // gear band changes, or when the bot has actually settled in the zone and
    // its farm timer runs out -- at which point it moves on, the way a player
    // eventually rotates elsewhere.
    const int rarityIdx = botMaxRarityIndex(bot);
    bool haveZone = false;
    Vec2 zone;
    if (ai.hasFarmZone && ai.farmZoneRarityIndex == rarityIdx) {
        zone = ai.farmZone;
        haveZone = true;
        const bool arrived =
            distanceSq(zone, at) < kBotFarmZoneArrivedDist * kBotFarmZoneArrivedDist;
        if (arrived && nowMillis >= ai.farmZoneUntilMillis) {
            haveZone = false;   // Timer expired on station -- rotate onward.
            ++ai.farmZoneRotation;
        }
    }
    if (!haveZone && botPickFarmZone(bot, rarityIdx, ai.farmZoneRotation, zone)) {
        ai.hasFarmZone = true;
        ai.farmZone = zone;
        ai.farmZoneRarityIndex = rarityIdx;
        ai.farmZoneUntilMillis =
            nowMillis + rng_.range(kBotFarmZoneMinMillis, kBotFarmZoneMaxMillis);
        haveZone = true;
    }
    if (haveZone) {
        const bool highTier = rarityIdx >= rarityIndex(Rarity::Ultra);
        mode.kind = BotMode::Normal;
        mode.anchor = zone;
        mode.hasAnchor = true;
        mode.tetherRadius = highTier ? kBotUltraRoamRadius : kBotTetherRadius;
        mode.returnRadius = highTier ? kBotUltraRoamReturn : kBotTetherReturnRadius;
        return mode;
    }

    // A map with no spawn zones at all: tether to a human if one exists,
    // otherwise free-roam from wherever the bot is standing.
    double bestDistSq = std::numeric_limits<double>::max();
    for (const auto& entry : sessions_) {
        const Session& session = entry.second;
        if (!session.playing() || world_.has<Dead>(session.entity)) continue;
        const Transform* other = world_.tryGet<Transform>(session.entity);
        if (other == nullptr) continue;
        const double distSq = distanceSq(other->position, at);
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            mode.anchor = other->position;
            mode.hasAnchor = true;
        }
    }
    mode.kind = BotMode::Normal;
    mode.tetherRadius = kBotTetherRadius;
    mode.returnRadius = kBotTetherReturnRadius;
    return mode;
}

// ---------------------------------------------------------------------------
// Long-haul raid routing
// ---------------------------------------------------------------------------

bool GameServer::botRaidShortcut(Bot& bot, double nowMillis, Vec2 anchor, double distToAnchor) {
    if (distToAnchor < kBotRaidShortcutMinDist) return false;
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return false;
    const Vec2 at = transform->position;

    // The single-hop teleporter whose destination minimises total travel to
    // the boss, if any beats simply walking.
    const double direct = (anchor - at).length();
    double bestTotal = direct;
    Vec2 bestSource;
    bool found = false;
    for (const MapElement& element : mapData_.elements()) {
        if (element.kind != MapElementKind::Teleporter || !element.hasTeleportTo) continue;
        const Vec2 source = element.centre();
        const double destToBoss = (element.teleportTo - anchor).length();
        // Only worth it when the destination actually lands near the boss.
        if (destToBoss > direct * kBotRaidTelePayoffRatio) continue;
        const double total = (source - at).length() + destToBoss;
        if (total < bestTotal) {
            bestTotal = total;
            bestSource = source;
            found = true;
        }
    }
    if (!found) return false;   // No teleporter helps; the bot walks.

    const Vec2 toward = bestSource - at;
    const double dist = toward.length();
    // Inside the pad's activation radius: hold still so the dwell timer fires.
    // The teleporter pass runs for every flower, bots included.
    if (dist < kTeleporterRadius * 0.6) {
        botHold(bot);
        return true;
    }
    if (botFollowPath(bot, nowMillis, bestSource, 1.0, 1.0)) return true;
    const Vec2 steered = botSteerAroundWalls(at, toward / std::max(1e-6, dist));
    botDriveMove(bot, steered, 1.0, 1.0);
    return true;
}

// ---------------------------------------------------------------------------
// The pass
// ---------------------------------------------------------------------------

void GameServer::stepBots(double nowMillis) {
    if (bots_.empty()) return;

    rebuildBotBossIndex(nowMillis);
    updateBotSquads(nowMillis);
    // Reset the per-tick A* budget, so one tick cannot be dominated by
    // simultaneous recomputes -- a whole raid replanning at once.
    botPathBudget_ = kBotPathMaxPerTick;
    announceNewBosses(nowMillis);
    // Groups are only needed by high-rarity mode and slots only by raiders,
    // but both are cheaper built once than recomputed per bot.
    computeBotGroups();
    computeBotRaidSlots(nowMillis);

    for (Bot& bot : bots_) {
        if (bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity)) continue;
        stepOneBot(bot, nowMillis);
    }
}

void GameServer::stepOneBot(Bot& bot, double nowMillis) {
    Transform* transform = world_.tryGet<Transform>(bot.entity);
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (transform == nullptr || input == nullptr) return;
    BotAiState& ai = bot.ai;

    // The persona: a persistent set of small behavioural offsets so no two
    // bots play identically. Rolled once and held, seeded off the bot's id so
    // it survives a death and is reproducible when debugging one.
    if (!ai.personaReady) {
        ai.personaReady = true;
        BotRng rng(bot.id);
        ai.persona.bias = Vec2::fromAngle(rng.unit() * kTau);
        ai.persona.standoffBias = -(2.0 + rng.unit() * 38.0);    // 2-40 units inside max reach
        ai.persona.reactionMillis = 120.0 + rng.unit() * 320.0;
        ai.persona.turnRate = 0.26 + rng.unit() * 0.22;          // rad/tick
        ai.persona.wanderSpeed = 0.40 + rng.unit() * 0.30;
        ai.persona.idleChance = rng.unit() * 0.45;
        ai.persona.aggression = 0.85 + rng.unit() * 0.30;
    }
    const BotPersona& persona = ai.persona;

    // A corpse holds still and waits to be replaced; maintainBots owns the
    // replacement. The traversal petals go back first, so the rebuilt body is
    // not the one that inherits them.
    if (world_.has<Dead>(bot.entity)) {
        botUnequipPowder(bot);
        botUnequipYggdrasil(bot);
        botHold(bot);
        if (bot.respawnAtMillis <= 0) bot.respawnAtMillis = nowMillis + kBotRespawnDelayMillis;
        return;
    }
    // Alive, so any pending respawn deadline is void.
    //
    // Not defensive tidying: a yggdrasil petal revives a bot without the
    // controller knowing, and bots equip yggdrasil whenever another is nearby
    // and actively path to each other's corpses, so this happens constantly. A
    // revived bot that kept a deadline in the past would, the next time it
    // died, satisfy it on the very tick of death -- warping to a spawn zone at
    // full health with no corpse and no wait, which reads as a healthy bot
    // teleporting to spawn rather than dying.
    bot.respawnAtMillis = 0;
    bot.deathAnnounced = false;

    const Vec2 at = transform->position;

    // --- trajectory watchdog ---------------------------------------------
    //
    // Ahead of every decision branch, so it also covers bots oscillating in
    // combat, on a path or while regrouping.
    if (nowMillis < ai.unstickUntilMillis) {
        botDriveMove(bot, botSteerAroundWalls(at, ai.unstickDir), 0.8, 1.0, 1.6);
        return;
    }
    if (botDetectOscillation(bot, nowMillis)) {
        const Vec2 direction = botPickUnstickDirection(bot);
        ai.unstickDir = direction;
        ai.unstickUntilMillis =
            nowMillis + rng_.range(kBotUnstickMinMillis, kBotUnstickMaxMillis);
        ai.suppressTargetUntilMillis = nowMillis + kBotUnstickTargetSuppressMillis;
        ai.target = NULL_ENTITY;
        ai.pickup = NULL_ENTITY;
        ai.nextWanderMillis = 0;
        ai.idleUntilMillis = 0;
        botClearPath(ai);
        // Commit the heading immediately rather than easing into it.
        ai.heading = direction;
        ai.hasHeading = true;

        // Repeat offender? Then the destination is the problem, not the local
        // geometry -- normal-mode navigation is a cheap steering probe and
        // cannot reason its way out of a concave wall -- so give the bot a
        // different place to be.
        if (!ai.hasUnstickTrips ||
            nowMillis - ai.unstickTripsAtMillis > kBotUnstickEscalateWindowMillis) {
            ai.hasUnstickTrips = true;
            ai.unstickTripsAtMillis = nowMillis;
            ai.unstickTrips = 1;
        } else {
            ++ai.unstickTrips;
        }
        if (ai.unstickTrips >= kBotUnstickEscalateTrips) {
            ai.unstickTrips = 0;
            ai.unstickTripsAtMillis = nowMillis;
            ++ai.farmZoneRotation;
            ai.hasFarmZone = false;
            ai.farmZoneUntilMillis = 0;
        }
        botDriveMove(bot, botSteerAroundWalls(at, direction), 0.8, 1.0, 1.6);
        return;
    }

    // --- where this bot belongs ------------------------------------------
    const BotModeContext mode = computeBotMode(bot, nowMillis);
    bot.hasAnchor = mode.hasAnchor;
    if (mode.hasAnchor) bot.anchor = mode.anchor;
    const double anchorDist = mode.hasAnchor ? (mode.anchor - at).length() : 0.0;

    // Swap in a powder petal whenever the bot is far enough from its anchor
    // that traversal speed actually matters -- raid traversal and the walk to
    // a farming zone both qualify. Restored once it is back in engagement
    // range, so combat slot 0 is online before petals are needed.
    if (anchorDist > kBotRaidPowderEquipDist) botEquipPowder(bot);
    else if (anchorDist < kBotRaidPowderUnequipDist) botUnequipPowder(bot);

    // Yggdrasil buddy swap: when another bot is close enough that it could
    // plausibly need a revive, slot 1 becomes a yggdrasil. Dropped at a WIDER
    // range than it was equipped, so the swap cannot chatter.
    if (botHasNearbyBuddy(bot, ai.yggSwapped ? kBotYggBuddyDropRange : kBotYggBuddyRange)) {
        botEquipYggdrasil(bot);
    } else {
        botUnequipYggdrasil(bot);
    }

    // Revive seek: a teammate has gone down within range and this bot carries
    // a yggdrasil. The revive itself fires when an orbiting petal touches the
    // body, so all the bot has to do is get the corpse inside its ring --
    // which is why it stands right on top of it with petals extended.
    if (const Entity revive = botFindReviveTarget(bot); revive != NULL_ENTITY) {
        const Vec2 toward = world_.get<Transform>(revive).position - at;
        const double dist = std::max(1e-6, toward.length());
        botDriveMove(bot, botSteerAroundWalls(at, toward / dist), 0.95, 2.0);
        return;
    }

    // Long-haul raid routing: hop a teleporter when one puts the bot
    // meaningfully closer to the boss.
    if (mode.kind == BotMode::Raid && mode.hasAnchor &&
        botRaidShortcut(bot, nowMillis, mode.anchor, anchorDist)) {
        return;
    }

    // --- what to fight ----------------------------------------------------
    double targetDist = 0;
    Entity target = nowMillis < ai.suppressTargetUntilMillis
                        ? NULL_ENTITY
                        : botPickTarget(bot, mode, nowMillis, targetDist);
    if (target != NULL_ENTITY) {
        if (target != ai.target) {
            ai.target = target;
            ai.targetAcquiredMillis = nowMillis;
        }
        // Reaction time: a player does not lock on the instant a mob crosses
        // into range. Until the delay elapses the bot carries on with what it
        // was doing, which reads as noticing rather than tracking.
        if (nowMillis - ai.targetAcquiredMillis < persona.reactionMillis) target = NULL_ENTITY;
    } else {
        ai.target = NULL_ENTITY;
        ai.targetAcquiredMillis = 0;
    }

    bool bossTarget = target != NULL_ENTITY && world_.has<MobType>(target) &&
                      isBotBossTier(world_.get<MobType>(target).rarity);

    // Ram interception. Only diverts when the interceptor is meaningfully
    // closer than the real target -- otherwise the bot would swap to the mob
    // it is already engaging and clobber the raid-slot logic.
    if (target != NULL_ENTITY) {
        const Vec2 toward = world_.get<Transform>(target).position - at;
        const double dist = std::max(1e-6, toward.length());
        double interceptDist = 0;
        const Entity intercept =
            botFindInterceptingMob(at, toward / dist, target, 160.0, interceptDist);
        if (intercept != NULL_ENTITY && interceptDist < dist - 40.0) {
            target = intercept;
            targetDist = interceptDist;
            bossTarget = false;
        }
    }

    // --- regroup ----------------------------------------------------------
    //
    // Drifted outside the cluster: abandon the current task and walk back.
    // Skipped while actually fighting a boss -- bots commit to that fight even
    // when the anchor is far.
    if (mode.hasAnchor && anchorDist > mode.returnRadius &&
        !(mode.kind == BotMode::Raid && bossTarget)) {
        // Raid and group crowds use A* to get around wall clusters; a
        // normal-mode regroup stays on the cheap steering probe.
        if (mode.kind != BotMode::Normal &&
            botFollowPath(bot, nowMillis, mode.anchor, 1.0, 1.0)) {
            return;
        }
        const Vec2 toward = mode.anchor - at;
        const double dist = std::max(1e-6, anchorDist);
        botDriveMove(bot, botSteerAroundWalls(at, toward / dist), 1.0, 1.0);
        return;
    }

    // --- flee -------------------------------------------------------------
    const Health* health = world_.tryGet<Health>(bot.entity);
    const double healthRatio =
        health != nullptr && health->max > 0 ? health->current / health->max : 1.0;
    // A boss is too valuable to run from: commit unless critically low. The
    // persona shifts the bar either way -- some bots bail at the first sign of
    // trouble, others stay in far too long.
    const double baseFlee = kBotFleeHealthRatio * (2.0 - persona.aggression);
    const double fleeThreshold = bossTarget ? baseFlee * 0.5 : baseFlee;

    // Hysteresis. Sitting exactly on the threshold flips the decision every
    // tick -- back off, heal a sliver, re-engage, get hit, back off -- which
    // is the two-position shuffle driven by health rather than geometry.
    if (ai.fleeing) {
        if (healthRatio > fleeThreshold * kBotFleeRecoverRatio && nowMillis >= ai.fleeUntilMillis) {
            ai.fleeing = false;
        }
    } else if (healthRatio < fleeThreshold) {
        ai.fleeing = true;
        ai.fleeUntilMillis = nowMillis + kBotFleeMinMillis;
    }

    if (target != NULL_ENTITY && ai.fleeing) {
        const Vec2 away = at - world_.get<Transform>(target).position;
        const double dist = std::max(1e-6, away.length());
        // Break away at an angle instead of straight back: a dead-straight
        // retreat line from a chasing mob is a bot tell.
        const double strafe = botTangentDirection(bot, nowMillis);
        const Vec2 escape{away.x / dist + (-away.y / dist) * 0.35 * strafe,
                          away.y / dist + (away.x / dist) * 0.35 * strafe};
        botDriveMove(bot, botSteerAroundWalls(at, escape.normalized()), 1.0, 0.7, 1.5);
        return;
    }

    // --- fight ------------------------------------------------------------
    if (target != NULL_ENTITY) {
        const Vec2 targetAt = world_.get<Transform>(target).position;
        const Vec2 toward = targetAt - at;
        const double dist = targetDist > 0 ? targetDist : std::max(1e-6, toward.length());
        const Vec2 direction = toward / std::max(1e-6, toward.length());
        // Petals point at what the bot is fighting.
        input->current.aimAngle = std::atan2(toward.y, toward.x);
        input->aimDirection = Vec2::fromAngle(input->current.aimAngle);

        // 2.0 is the player's own maximum attack extension.
        constexpr double kAttackExtension = kPetalOrbitAttackExtension;
        const double reach = botPetalReach(bot, kAttackExtension);
        const Body* targetBody = world_.tryGet<Body>(target);
        const double targetRadius = targetBody ? targetBody->radius : 0.0;
        const Body* body = world_.tryGet<Body>(bot.entity);
        const double bodyRadius = body ? body->radius : kPlayerBaseRadius;

        // The true maximum hit distance, centre to centre, is where a petal's
        // far edge just touches the mob's edge: reach, less the buffer folded
        // into it, plus the mob's radius. Stand about ten units inside that so
        // position jitter still lands hits -- without the subtraction the
        // buffer is counted twice and the bot parks just outside real reach.
        const double baseStandoff = reach - kBotStandoffBuffer + targetRadius - 10.0;
        const double dangerDist = bodyRadius + targetRadius + 6.0;
        // The persona's radial bias spreads the equilibrium ring so
        // neighbouring bots do not all sit at one distance and get shoved in
        // and out of it together by separation. Floored above dangerDist: the
        // bias reaches -40 and a defend-only build's reach is around 98, so
        // the two together can put the ring INSIDE the mob's collision circle
        // -- and the controller then has two branches fighting each other
        // every tick, which from outside is a bot repeatedly ramming the mob
        // it is fighting.
        const double standoff =
            std::max(dangerDist + 8.0, baseStandoff + persona.standoffBias);

        if (dist < dangerDist) {
            // Too close -- shove off, but stay in attack state so the petals
            // stay extended while killing it. High agility: this is the one
            // case where an instant direction change is right.
            botDriveMove(bot, -direction, 1.0, kAttackExtension, 3.0, target);
            return;
        }

        if (dist > standoff + 80.0) {
            // Far away -- close at full speed. No speed-mod compensation: this
            // is the traversal branch, where powder is supposed to help.
            if (mode.kind != BotMode::Normal &&
                botFollowPath(bot, nowMillis, targetAt, 0.95, kAttackExtension, target)) {
                return;
            }
            botDriveMove(bot, botSteerAroundWalls(at, direction), 0.95, kAttackExtension, 1.0,
                         target);
            return;
        }

        const double strafe = botTangentDirection(bot, nowMillis);
        Vec2 move;
        double speedMultiplier;

        // A boss raider owns an angular slot around the boss, so a raid
        // spreads out instead of stacking on one side.
        const auto slot = bossTarget ? botRaidSlots_.find(bot.entity) : botRaidSlots_.end();
        if (slot != botRaidSlots_.end()) {
            // Ease toward the assigned slot. The raw assignment jumps every
            // time a raider joins or dies -- the slots are redealt -- and
            // snapping to the new angle sent bots sprinting around the boss,
            // or, with two raiders trading slots, back and forth forever.
            double use = slot->second;
            if (ai.hasSlotAngle) {
                use = ai.slotAngle + clamp(wrapAngle(slot->second - ai.slotAngle), -0.06, 0.06);
            }
            ai.slotAngle = use;
            ai.hasSlotAngle = true;

            const Vec2 toSlot = targetAt + Vec2::fromAngle(use, standoff) - at;
            const double slotDist = toSlot.length();
            if (slotDist > 12.0) {
                // Speed tapers continuously to the strafe speed as the bot
                // settles in, so there is no threshold to flip across.
                move = toSlot / slotDist;
                speedMultiplier = std::min(0.8, 0.15 + slotDist / 300.0);
            } else {
                move = Vec2{-direction.y * strafe, direction.x * strafe};
                speedMultiplier = 0.18;
            }
        } else {
            // A continuous orbit controller, not a ladder of discrete distance
            // bands: a bot whose distance wobbled across a band edge would
            // flip between backing off and closing in every tick, which is the
            // in-combat form of the two-position shuffle. The radial
            // correction is proportional to how far off the ring the bot is
            // and passes smoothly through zero at the ring itself, so there is
            // nothing to flip between.
            const double error = dist - standoff;   // positive = too far out
            const double radial = clamp(error / kBotOrbitRadialGain, -1.0, 1.0);
            // Circle hardest when settled on the ring, less while correcting.
            const double tangential = 1.0 - 0.55 * std::min(1.0, std::fabs(radial));
            move = Vec2{direction.x * radial + (-direction.y * strafe) * tangential,
                        direction.y * radial + (direction.x * strafe) * tangential}
                       .normalized();
            if (move.lengthSq() < 1e-12) move = direction;
            speedMultiplier = 0.28 + 0.45 * std::min(1.0, std::fabs(error) / 110.0);
        }

        // Close range: cancel the bot's aggregate speed modifier (powder and
        // friends) so per-tick movement matches what the controller was tuned
        // for. A powder-wearing bot moving at double speed through the
        // standoff zone otherwise overshoots the ring every tick and
        // ping-pongs across it instead of orbiting.
        double speedMod = 1.0;
        if (const PlayerModifiers* mods = world_.tryGet<PlayerModifiers>(bot.entity)) {
            speedMod = mods->speedScale;
        }
        const double effective = speedMod > 1.0 ? speedMultiplier / speedMod : speedMultiplier;
        botDriveMove(bot, move, effective, kAttackExtension, 1.0, target);
        return;
    }

    // Nothing to fight: petals follow the walk instead.
    input->current.aimAngle = input->current.moveAngle;
    input->aimDirection = Vec2::fromAngle(input->current.aimAngle);

    // --- loot -------------------------------------------------------------
    double pickupDist = 0;
    if (const Entity pickup = botFindPickup(bot, mode, pickupDist); pickup != NULL_ENTITY) {
        ai.pickup = pickup;
        const Vec2 toward = world_.get<Transform>(pickup).position - at;
        const double dist = std::max(1e-6, toward.length());
        // Only steer when the drop is far enough that a wall could genuinely
        // be in the way; close-range pickup does not need pathing.
        const Vec2 direction = toward / dist;
        botDriveMove(bot, dist > kTileSize ? botSteerAroundWalls(at, direction) : direction, 0.9,
                     1.0);
        return;
    }
    ai.pickup = NULL_ENTITY;

    // --- idle -------------------------------------------------------------
    if (nowMillis < ai.idleUntilMillis) {
        botHold(bot);
        // Standing still on purpose is not being stuck: keep the watchdog from
        // reading a deliberate pause as a jam.
        botResetOscillation(bot, nowMillis);
        return;
    }
    ai.idleUntilMillis = 0;

    // --- wander -----------------------------------------------------------
    //
    // The target stays inside the current cluster radius, so raid and group
    // bots stay tight and normal bots stay tethered.
    if (nowMillis > ai.nextWanderMillis) {
        const Vec2 centre = mode.hasAnchor ? mode.anchor : at;
        const double maxDist = std::max(80.0, mode.tetherRadius - 100.0);
        // A few random angles, taking the first with line of sight from where
        // the bot stands. A bot that picks a wander target through a wall has
        // no way to reach it and parks against the wall until the timer fires
        // again.
        Vec2 picked = at;
        bool clear = false;
        for (int attempt = 0; attempt < 8 && !clear; ++attempt) {
            const double angle = rng_.angle();
            const double distance =
                std::min(maxDist, 200.0) + rng_.unit() * std::max(0.0, maxDist - 200.0);
            const Vec2 candidate = centre + Vec2::fromAngle(angle, distance);
            const Vec2 clamped{
                clamp(candidate.x, kWorldBoundaryThreshold, kWorldSize - kWorldBoundaryThreshold),
                clamp(candidate.y, kWorldBoundaryThreshold, kWorldSize - kWorldBoundaryThreshold)};
            if (botRayHitsWall(at, clamped)) continue;
            picked = clamped;
            clear = true;
        }
        // Walled off on every heading: scoot a short way in any clear
        // direction so the bot at least moves and unsticks itself.
        if (!clear) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                const Vec2 candidate = at + Vec2::fromAngle(rng_.angle(), 200.0);
                const Vec2 clamped{clamp(candidate.x, kWorldBoundaryThreshold,
                                         kWorldSize - kWorldBoundaryThreshold),
                                   clamp(candidate.y, kWorldBoundaryThreshold,
                                         kWorldSize - kWorldBoundaryThreshold)};
                if (botRayHitsWall(at, clamped)) continue;
                picked = clamped;
                break;
            }
        }
        ai.wanderTarget = picked;
        ai.nextWanderMillis = nowMillis + 3000.0 + rng_.unit() * 4000.0;
        ai.atWanderTarget = false;
    }

    const Vec2 toward = ai.wanderTarget - at;
    const double dist = toward.length();
    if (dist < 30.0) {
        // Arrived. Decide ONCE -- not every tick -- whether to linger here,
        // then line up the next hop shortly. The re-pick is deliberately not
        // immediate: a bot walled in on all sides has its wander target
        // snapped back onto its own position, and re-running the sixteen-ray
        // pick every tick for it would be pure waste.
        if (!ai.atWanderTarget) {
            ai.atWanderTarget = true;
            if (rng_.chance(persona.idleChance)) {
                ai.idleUntilMillis = nowMillis + 500.0 + rng_.unit() * 2200.0;
            }
        }
        ai.nextWanderMillis =
            std::min(ai.nextWanderMillis, nowMillis + 250.0 + rng_.unit() * 500.0);
        botHold(bot);
        botResetOscillation(bot, nowMillis);
        return;
    }

    ai.atWanderTarget = false;
    // Per-bot cruise speed with a slow drift, so a field of wandering bots
    // does not move like one formation at a single fixed pace.
    const double cruise =
        persona.wanderSpeed * (0.9 + 0.1 * std::sin(nowMillis / 900.0 + persona.bias.x * 6.0));
    const Vec2 direction = toward / dist;
    // Steer around walls on wanders too, or bots park against a wall tile
    // until the timer fires and the target is reshuffled.
    botDriveMove(bot, dist > kTileSize ? botSteerAroundWalls(at, direction) : direction, cruise,
                 1.0);
}

} // namespace flix
