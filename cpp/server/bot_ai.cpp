// What a bot does.
//
// server/bot_ai.h carries the tuning and explains the shape; this is the
// behaviour. One tick of one bot, in order:
//
//   sense  ->  update the hunting ground  ->  unjam  ->  choose an activity
//   ->  run it
//
// The sensing pass is one broadphase query, and every decision below is
// answered out of it. That is not only cheaper than the four queries this used
// to run: it is what makes the targeting and the steering agree. The old
// controller filtered mobs out of its target search that its steering still
// pushed away from, and the gap between those two lists is precisely where a
// bot ends up shouldering through a field of mobs without ever hitting one.
//
// Everything a bot writes is its PlayerInput, which the ordinary player
// movement and petal pipeline then consumes. No system anywhere else knows a
// bot exists, which is the whole reason a bot is a plain player entity.

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
#include "server/systems/spawning.h"
#include "shared/game/config.h"
#include "shared/game/difficulty.h"

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

/// Wall-probe offsets, in order: straight on first, then progressively wider
/// to either side.
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

/// Smooth per-bot noise in [-1, 1]. Two incommensurable sines, so the pattern
/// does not repeat on any interval a player would notice.
///
/// This exists because the old controller's "liveliness" was a fresh random
/// number every tick, which does not read as a living thing: it reads as
/// vibration. A slow continuous wobble reads as a hand on a mouse.
double botNoise(double nowMillis, double phase) {
    return 0.6 * std::sin(nowMillis / 1300.0 + phase) +
           0.4 * std::sin(nowMillis / 431.0 + phase * 2.7);
}

/// How much a bot wants to fight a mob of this tier, in the same units the
/// score below counts distance in. Rarer is worth walking further for.
double tierAppetite(Rarity rarity) {
    if (isBotBossTier(rarity)) return 6000.0;
    return static_cast<double>(rarityIndex(rarity)) * 230.0;
}

} // namespace

const char* botActivityName(BotActivity activity) {
    switch (activity) {
        case BotActivity::Roam: return "roam";
        case BotActivity::Travel: return "travel";
        case BotActivity::Hunt: return "hunt";
        case BotActivity::Fight: return "fight";
        case BotActivity::Loot: return "loot";
        case BotActivity::Retreat: return "retreat";
        case BotActivity::Revive: return "revive";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Population
// ---------------------------------------------------------------------------

void GameServer::maintainBots(double nowMillis) {
    // A human in the world resets the idle clock. Past the grace period with
    // nobody online the bots are retired: there is nobody to see them, and the
    // tick gate has already stopped simulating anyway.
    //
    // Skipped entirely while `set_bot_count N` is in force: title-screen
    // sessions are not players, so an operator sitting on the menu would
    // otherwise set a count, watch nothing happen, and have no way to tell why.
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
    collectSpawnBlockers(Realm::Overworld, blockers);   // bots are born on the overworld

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
            // Somewhere else entirely: the ground it died on is exactly the
            // ground that killed it.
            const Vec2 spawn = pickBotSpawn(blockers);
            // Takes the corpse and its ring away, if one is still lying there.
            destroyBot(bot);
            bot.entity = createBotBody(bot.name, spawn);
            bot.deathAnnounced = false;
            bot.anchor = spawn;
            bot.hasAnchor = false;
            bot.respawnAtMillis = 0;

            // The bot reappears somewhere else entirely, so everything derived
            // from where it used to be -- target, path, heading, hunting
            // ground -- is stale.
            BotAiState& ai = bot.ai;
            ai.activity = BotActivity::Roam;
            ai.activitySinceMillis = nowMillis;
            ai.hasHome = false;
            ai.homeReached = false;
            ai.target = NULL_ENTITY;
            ai.targetCommitted = false;
            ai.pickup = NULL_ENTITY;
            ai.lastHealth = -1.0;
            ai.hurtUntilMillis = 0;
            ai.fleeUntilMillis = 0;
            ai.hasHeading = false;
            ai.hasSlotAngle = false;
            ai.roamReady = false;
            ai.idleUntilMillis = 0;
            ai.stuckMillis = 0;
            ai.unstickUntilMillis = 0;
            // The swap bookkeeping described a loadout that no longer exists:
            // the body carrying it was destroyed, and the new one was rebuilt
            // from the name. Dropping the record rather than restoring it is
            // what stops the next unequip writing a dead body's slot back over
            // a live one.
            ai.powderSwapped = false;
            ai.yggSwapped = false;
            botClearPath(ai);
        }
    }

    // Drift the target by +-1 on a slow clock, bounded, so the population
    // wanders instead of sitting on an exact number -- and slowly enough that
    // the drift does not read as bots blinking in and out.
    if (nowMillis >= nextBotJitterMillis_) {
        nextBotJitterMillis_ = nowMillis + kBotJitterIntervalMillis;
        if (botRng_.chance(kBotJitterStepChance)) {
            botCountJitter_ += botRng_.chance(0.5) ? -1 : 1;
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
        // not a restart: an explicit target is filled in one pass.
        const int wanted =
            overridden ? desired - current : std::min(desired - current, kBotSpawnBurstCap);
        for (int i = 0; i < wanted; ++i) {
            Bot bot;
            bot.name = kBotNames[botRng_.below(static_cast<std::uint32_t>(std::size(kBotNames)))];
            // Its own identity, independent of the name: two bots that roll
            // the same name share a BUILD, but not a persona, a strafe
            // direction or a patch of ground.
            bot.id = static_cast<std::uint32_t>(botRng_.next());
            const Vec2 spawn = pickBotSpawn(blockers);
            bot.anchor = spawn;
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
    collectSpawnBlockers(Realm::Overworld, blockers);   // bots are born on the overworld
    return pickBotSpawn(blockers);
}

Vec2 GameServer::pickBotSpawn(const std::vector<MobDisc>& blockers) {
    // EVERY area a real player can actually appear in, and nothing else: one
    // of the overworld's player spawn rectangles, or a BEGINNER band -- one
    // whose difficulty stays below the first tier a fresh flower cannot fight.
    // That is exactly what the join handler allows, and it is why a bot never
    // turns up deep in high-difficulty ground where it is under attack from the
    // moment it appears.
    const MapData* map = worldMaps_.forRealm(Realm::Overworld);
    std::vector<const MapElement*> anchors;
    if (map != nullptr) {
        for (const MapElement* point : map->playerSpawns()) anchors.push_back(point);
        for (const MapElement& element : map->elements()) {
            if (element.bounds.w <= 0 || element.bounds.h <= 0) continue;
            if (!element.isSpawnBand()) continue;
            if (element.difficulty >= kDangerousGroundDifficulty) continue;
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
            anchors[botRng_.below(static_cast<std::uint32_t>(anchors.size()))];
        if (map != nullptr && map->spawnInElement(*anchor, botRng_, *terrain_, spawn, &blockers)) {
            return spawn;
        }
    }
    if (!anchors.empty()) {
        // Every sampled area was crowded or walled over -- routine on a map
        // that is more than half solid. findOpenSpawn samples a disc around the
        // centre and then rings of tiles outward, so a bot is never born inside
        // a block the movement step cannot push it out of.
        const MapElement& area =
            *anchors[botRng_.below(static_cast<std::uint32_t>(anchors.size()))];
        const double reach = std::max(area.bounds.w, area.bounds.h) * 0.5;
        return terrain_->findOpenSpawn(botRng_, area.centre(), reach, Realm::Overworld);
    }
    const MapData* overworld = worldMaps_.forRealm(Realm::Overworld);
    if (overworld == nullptr) return terrain_->spawnPoint(Realm::Overworld);
    return overworld->defaultSpawn(botRng_, *terrain_, &blockers);
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

int GameServer::botMapTierCeiling() const {
    if (botMapCeiling_ >= 0) return botMapCeiling_;
    double best = 0;
    for (const MapData& map : worldMaps_.maps()) {
        for (const MapElement& element : map.elements()) {
            if (!element.isSpawnBand()) continue;
            best = std::max(best, tierValueForDifficulty(element.difficulty));
        }
    }
    botMapCeiling_ = clamp(static_cast<int>(std::floor(best)), 0, kRarityCount - 1);
    return botMapCeiling_;
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
    // A sampled raycast, every half tile along the segment. Deliberately NOT
    // Terrain::segmentBlocked -- that one is an exact swept walk, and it
    // refuses the diagonal seams and narrow gaps this sampled test steers
    // through, which would visibly change where bots are willing to go.
    const Vec2 delta = to - from;
    const double dist = delta.length();
    // A zero, NaN or runaway distance means no hit: a ray toward a body flung
    // to an enormous coordinate would otherwise blow the step count up.
    if (!(dist > 0.0) || !std::isfinite(dist)) return false;
    const double step = kTileSize / 2.0;
    const int steps = std::min(1024, static_cast<int>(std::ceil(dist / step)));
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        if (terrain_->blocked(from + delta * t, Realm::Overworld)) return true;
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

Vec2 GameServer::botAvoidMobs(Vec2 at, Entity except) {
    // A steering BIAS, not a hard constraint: summed with the requested
    // heading the same way bot-vs-bot separation is, and capped below one so
    // it can bend a heading around a mob but can never reverse the bot's
    // intent and leave it unable to reach a goal that happens to be guarded.
    Vec2 out{0, 0};
    botAvoidCandidates_.clear();
    grid_.query(Realm::Overworld, at, kBotMobAvoidQueryRadius, botAvoidCandidates_);
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

void GameServer::botSetPetals(Bot& bot, bool out, double nowMillis, bool defending) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr) return;
    BotAiState& ai = bot.ai;

    // Defend is immediate: it is what a player hits when something is about to
    // land on them, and a hold on it would be a hold on the reaction.
    if (defending) {
        ai.petalsOut = false;
        input->current.flags = static_cast<std::uint8_t>(net::InputDefend);
        return;
    }
    // Attack is held once flipped. Petals cost nothing to throw, so the AI
    // would happily toggle them every other tick -- and a ring snapping in and
    // out at fifteen hertz is the most visible bot tell there is.
    if (out != ai.petalsOut && nowMillis - ai.petalsFlippedMillis >= kBotPetalHoldMillis) {
        ai.petalsOut = out;
        ai.petalsFlippedMillis = nowMillis;
    }
    input->current.flags =
        ai.petalsOut ? static_cast<std::uint8_t>(net::InputAttack) : std::uint8_t{0};
}

void GameServer::botHold(Bot& bot) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr) return;
    input->current.moveStrength = 0.0;
}

void GameServer::botDrive(Bot& bot, Vec2 direction, double speedMultiplier, double avoidStrength,
                          double agility, Entity engaging) {
    Transform* transform = world_.tryGet<Transform>(bot.entity);
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (transform == nullptr || input == nullptr) return;
    const Vec2 at = transform->position;
    BotAiState& ai = bot.ai;
    const BotPersona& persona = ai.persona;

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

    Vec2 out = direction + separation * kBotSeparationStrength;
    if (avoidStrength > 0.0) out += botAvoidMobs(at, engaging) * avoidStrength;

    // A slow lateral weave, perpendicular to where the bot is going. Small,
    // continuous, and per-bot: this is the difference between a flower walking
    // and a sprite being translated along a vector. Deliberately NOT a fresh
    // random number per tick, which is what the old controller used -- that
    // does not read as a living thing, it reads as vibration.
    const double weave = botNoise(clockMillis_, persona.noisePhase) * 0.07;
    out += Vec2{-direction.y, direction.x} * weave;

    const double magnitude = out.length();
    if (magnitude > 0.0) out = out / magnitude;
    else out = direction;

    // Turn-rate limiting. The AI can request any direction on any tick; a real
    // player's hand cannot. Easing the heading toward the request does two
    // things: movement reads as a flower arcing around rather than a turret
    // snapping, and a decision that flip-flops between two opposite directions
    // can no longer become visible per-tick vibration -- the heading hovers
    // near the midpoint until something breaks the tie.
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
}

bool GameServer::botHandleStuck(Bot& bot, double nowMillis) {
    BotAiState& ai = bot.ai;
    if (nowMillis < ai.unstickUntilMillis) {
        botDrive(bot, ai.unstickDir, 0.85, kBotAvoidStrengthTravel, 2.2);
        return true;
    }

    // The one unambiguous fault: the bot ASKED to move last tick and did not.
    // Everything else the old trajectory watchdog looked for -- reversals,
    // circling -- is also what a bot orbiting a mob or working a patch looks
    // like, which is why answering those with a timed sprint in a random
    // direction threw bots out of fights they were winning.
    const PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    const Motion* motion = world_.tryGet<Motion>(bot.entity);
    const bool asked = input != nullptr && input->current.moveStrength > 0.05;
    const double speed = motion != nullptr ? motion->velocity.length() : 0.0;
    if (asked && speed < kBotStuckSpeed) ai.stuckMillis += net::kTickMillis;
    else ai.stuckMillis = 0;
    if (ai.stuckMillis < kBotStuckTripMillis) return false;
    ai.stuckMillis = 0;

    // Sidestep first -- perpendicular to the rut is the shortest way out --
    // then progressively wider, and finally straight back. The leading side is
    // randomised so a knot of stuck bots does not all peel off the same way.
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    const Vec2 at = transform != nullptr ? transform->position : ai.home;
    const double base = std::atan2(ai.heading.y, ai.heading.x);
    const double side = botRng_.chance(0.5) ? 1.0 : -1.0;
    const double offsets[] = {
        (kPi / 2.0) * side,       -(kPi / 2.0) * side,
        (2.0 * kPi / 3.0) * side, -(2.0 * kPi / 3.0) * side,
        kPi,
        (kPi / 3.0) * side,       -(kPi / 3.0) * side,
    };
    Vec2 escape = Vec2::fromAngle(base + kPi);
    // Full probe distance first, then progressively shorter: in a corner every
    // direction is blocked at the full distance, and the smaller question the
    // bot can actually act on is "which way is there any room at all?".
    bool found = false;
    for (const double probe : {kBotStuckProbeDist, kBotStuckProbeDist / 2.0,
                               kBotStuckProbeDist / 4.0}) {
        for (const double offset : offsets) {
            const Vec2 direction = Vec2::fromAngle(base + offset);
            if (botRayHitsWall(at, at + direction * probe)) continue;
            escape = direction;
            found = true;
            break;
        }
        if (found) break;
    }

    ai.unstickDir = escape;
    ai.unstickUntilMillis = nowMillis + kBotUnstickHoldMillis;
    // The roam walk carries on from the escape heading rather than from the
    // one that jammed, so the bot does not simply turn back into it.
    ai.roamAngle = std::atan2(escape.y, escape.x);
    ai.roamReady = true;
    ai.hasHeading = true;
    ai.heading = escape;
    botClearPath(ai);
    botDrive(bot, escape, 0.85, kBotAvoidStrengthTravel, 2.2);
    return true;
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
    // The OVERWORLD's own grid dimensions, asked of the terrain rather than
    // assumed: a map states its own size, and sizing the search to the
    // historical 200-square on a 128-square map walks it off the end of the
    // world the map actually has.
    const int cols = terrain_->tileCols(Realm::Overworld);
    const int rows = terrain_->tileRows(Realm::Overworld);
    if (cols <= 0 || rows <= 0) return false;
    const auto blockedTile = [&](int tx, int ty) {
        return tileBlocks(terrain_->atTile(tx, ty));
    };

    const int sx = Terrain::toTileCoord(start.x);
    const int sy = Terrain::toTileCoord(start.y);
    int gx = Terrain::toTileCoord(goal.x);
    int gy = Terrain::toTileCoord(goal.y);
    if (sx < 0 || sy < 0 || sx >= cols || sy >= rows) return false;

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
    if (gx < 0 || gy < 0 || gx >= cols || gy >= rows) return false;
    if (sx == gx && sy == gy) return false;

    BotPathScratch& scratch = botPath_;
    const std::size_t cells = static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
    if (scratch.cols != cols || scratch.rows != rows || scratch.stamp.size() != cells) {
        scratch.cols = cols;
        scratch.rows = rows;
        scratch.gScore.assign(cells, 0.0);
        scratch.cameFrom.assign(cells, -1);
        scratch.stamp.assign(cells, 0);
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

    const std::size_t startIndex = static_cast<std::size_t>(sy) * cols + sx;
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
            std::size_t index = static_cast<std::size_t>(ty) * cols + tx;
            while (index != startIndex) {
                const int px = static_cast<int>(index % static_cast<std::size_t>(cols));
                const int py = static_cast<int>(index / static_cast<std::size_t>(cols));
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

        const std::size_t index = static_cast<std::size_t>(ty) * cols + tx;
        // Stale heap entry: the node was re-pushed with a lower f.
        if (scratch.stamp[index] != stamp) continue;
        const double g = scratch.gScore[index];
        if (f > g + octileHeuristic(tx, ty, gx, gy) + 1e-9) continue;

        for (const Neighbour& step : kAStarNeighbours) {
            const int nx = tx + step.dx;
            const int ny = ty + step.dy;
            if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
            if (blockedTile(nx, ny)) continue;
            // No corner cutting: a diagonal needs both orthogonals clear.
            if (step.dx != 0 && step.dy != 0) {
                if (blockedTile(tx + step.dx, ty)) continue;
                if (blockedTile(tx, ty + step.dy)) continue;
            }
            const double tentative = g + step.cost;
            const std::size_t neighbourIndex = static_cast<std::size_t>(ny) * cols + nx;
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
                               double avoidStrength, Entity engaging) {
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return false;
    const Vec2 at = transform->position;
    BotAiState& ai = bot.ai;

    // Nothing between here and there: walk. A* is for getting around walls,
    // and running it for a goal in plain sight produces tile-centre waypoints
    // that are strictly worse than a straight line.
    if (!botRayHitsWall(at, goal)) {
        botClearPath(ai);
        const Vec2 toward = goal - at;
        const double dist = std::max(1e-6, toward.length());
        botDrive(bot, toward / dist, speedMultiplier, avoidStrength, 1.0, engaging);
        return true;
    }

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
    if (stale && usable && ai.hasRepathed &&
        nowMillis - ai.lastRepathMillis < kBotPathMinRepathMillis) {
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
    botDrive(bot, toward / dist, speedMultiplier, avoidStrength, 1.0, engaging);
    return true;
}

int GameServer::botStrafeDirection(Bot& bot, double nowMillis) {
    BotAiState& ai = bot.ai;
    if (ai.strafeDir == 0) {
        ai.strafeDir = (bot.id & 1u) == 0u ? 1 : -1;
        ai.strafeFlipMillis =
            nowMillis + botRng_.range(kBotStrafeFlipMinMillis, kBotStrafeFlipMaxMillis);
    } else if (nowMillis >= ai.strafeFlipMillis) {
        ai.strafeDir = -ai.strafeDir;
        ai.strafeFlipMillis =
            nowMillis + botRng_.range(kBotStrafeFlipMinMillis, kBotStrafeFlipMaxMillis);
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

void GameServer::rebuildBotMobHeat() {
    const Vec2 extent = terrain_->realmExtent(Realm::Overworld);
    const int cols = std::max(1, static_cast<int>(std::ceil(extent.x / kBotHeatCellSize)));
    const int rows = std::max(1, static_cast<int>(std::ceil(extent.y / kBotHeatCellSize)));
    if (cols != botHeatCols_ || rows != botHeatRows_) {
        botHeatCols_ = cols;
        botHeatRows_ = rows;
        botMobHeat_.assign(static_cast<std::size_t>(cols) * rows, 0);
    } else {
        std::fill(botMobHeat_.begin(), botMobHeat_.end(), std::uint16_t{0});
    }

    Query<MobTag, MobType, Transform> mobs{world_};
    mobs.each([&](Entity e, MobTag&, MobType& type, Transform& transform) {
        if (transform.realm != Realm::Overworld) return;
        if (world_.has<Dead>(e) || world_.has<Pet>(e)) return;
        if (excludedMobType(type.configIndex)) return;
        // A mob a person is standing next to is not one a bot may take
        // (kBotPlayerClaimRadius), so counting it here would send bots to work
        // ground whose mobs they are not allowed to touch.
        for (const Vec2 spot : botHumanSpots_) {
            if (distanceSq(spot, transform.position) <
                kBotPlayerClaimRadius * kBotPlayerClaimRadius) {
                return;
            }
        }
        const int cx = static_cast<int>(transform.position.x / kBotHeatCellSize);
        const int cy = static_cast<int>(transform.position.y / kBotHeatCellSize);
        if (cx < 0 || cy < 0 || cx >= botHeatCols_ || cy >= botHeatRows_) return;
        std::uint16_t& cell =
            botMobHeat_[static_cast<std::size_t>(cy) * botHeatCols_ + cx];
        if (cell < 0xFFFFu) ++cell;
    });
}

int GameServer::botMobHeatAt(Vec2 at) const {
    if (botMobHeat_.empty()) return 0;
    const int cx = static_cast<int>(at.x / kBotHeatCellSize);
    const int cy = static_cast<int>(at.y / kBotHeatCellSize);
    if (cx < 0 || cy < 0 || cx >= botHeatCols_ || cy >= botHeatRows_) return 0;
    // The cell and its eight neighbours: a bot works a patch about this wide,
    // so what matters is what is around the point rather than what is in the
    // one cell it happens to land in.
    int total = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = cx + dx;
            const int ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= botHeatCols_ || ny >= botHeatRows_) continue;
            total += botMobHeat_[static_cast<std::size_t>(ny) * botHeatCols_ + nx];
        }
    }
    return total;
}

void GameServer::computeBotRaidSlots(double nowMillis) {
    // Each raiding bot owns a fixed angular slot around its rally point, so a
    // raid spreads evenly around the boss rather than stacking on one side,
    // and the crowd is sized off how many of them there are.
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
        const double crowd =
            std::max(kBotRaidRingMin, count * kBotRaidRingPerRaider);
        for (std::size_t i = 0; i < members.size(); ++i) {
            botRaidSlots_[bots_[members[i]].entity] =
                BotRaidSlot{(static_cast<double>(i) / count) * kTau, crowd};
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
    // Only bosses within rally range of THIS bot. Without the range gate every
    // bot in the world rallies on any boss anywhere, which is a map-wide
    // stampede rather than a raid. Among those in range, uniques beat supers,
    // then most recently seen, then proximity to a human.
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
        if (distanceSq(bossTransform->position, at) > kBotBossRallyRange * kBotBossRallyRange) {
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
        // Apex is a boss tier for the notice table and for target priority,
        // but the raid POOL only ever admits supers and uniques.
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
    for (Bot& bot : bots_) {
        botClearPath(bot.ai);
        bot.ai.homeReached = false;
    }
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
            nowMillis + botRng_.range(kBotBossAnnounceMinMillis, kBotBossAnnounceMaxMillis);
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
            shout = pool[botRng_.below(static_cast<std::uint32_t>(size))];
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
            nowMillis + botRng_.range(kBotBossAnnounceMinMillis, kBotBossAnnounceMaxMillis);
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
        bot.ai.nextSquadMillis = nowMillis + kBotSquadTickMillis + botRng_.unit() * 4000.0;

        const SquadMemberId member = SquadMemberId::ofBot(bot.entity);
        if (squads_.forMember(member) != nullptr) continue;

        // Joining an existing public squad comes first: a bot that hosts is
        // only useful once somebody can find it.
        const std::vector<const Squad*> open = squads_.publicSquads();
        if (!open.empty() && botRng_.chance(kBotSquadJoinChance)) {
            const std::string id = open[botRng_.below(static_cast<std::uint32_t>(open.size()))]->id;
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
        if (botRng_.chance(kBotSquadCreateChance)) squads_.create(member, true, botRng_);
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
// Senses
// ---------------------------------------------------------------------------

void GameServer::botSense(Bot& bot, double nowMillis, BotSenses& out) {
    out = BotSenses{};
    const Transform* transform = world_.tryGet<Transform>(bot.entity);
    if (transform == nullptr) return;
    BotAiState& ai = bot.ai;

    out.at = transform->position;
    const Body* body = world_.tryGet<Body>(bot.entity);
    out.bodyRadius = body != nullptr ? body->radius : kPlayerBaseRadius;
    out.reach = botPetalReach(bot, kPetalOrbitAttackExtension);

    // Damage taken since last tick. Nothing tells the controller it was hit,
    // and nothing needs to: the health it remembers from last tick is the
    // whole signal, and it costs one comparison.
    const Health* health = world_.tryGet<Health>(bot.entity);
    if (health != nullptr) {
        out.healthRatio = health->max > 0 ? clamp(health->current / health->max, 0.0, 1.0) : 1.0;
        if (ai.lastHealth >= 0.0 && health->current < ai.lastHealth - 1e-6) {
            ai.hurtUntilMillis = nowMillis + kBotAggressorMemoryMillis;
            // Being hit is when a player jukes. Rate-limited by the strafe
            // timer itself, so a mob chewing on a bot cannot make it stutter.
            if (botRng_.chance(0.3) && nowMillis + 900.0 < ai.strafeFlipMillis) {
                ai.strafeDir = -ai.strafeDir;
                ai.strafeFlipMillis = nowMillis + botRng_.range(kBotStrafeFlipMinMillis,
                                                                kBotStrafeFlipMaxMillis);
            }
        }
        ai.lastHealth = health->current;
    }

    // How far off its patch a target may be before it is somebody else's
    // problem. A boss, a blocker and anything actively hunting the bot are
    // exempt: those are not choices about where to farm.
    const double leash = (ai.hasHome ? ai.homeRadius : kBotHomeRadius) + 900.0;
    const double leashSq = leash * leash;
    // Capped at what this world actually grows: a bot geared past the map's
    // best band still has to farm something, and standards nothing can meet
    // are standards that produce a flower walking past every mob it sees.
    const int gearIndex = std::min(botMaxRarityIndex(bot), botMapTierCeiling());

    double bestScore = -std::numeric_limits<double>::max();
    // A drop the bot is already walking to competes with a discount, so two
    // drops at similar range do not trade places every tick and leave the bot
    // shuffling between them.
    double bestPickupScore = kBotItemSeekRange;
    double nearestBlocker = std::numeric_limits<double>::max();
    double nearestThreat = std::numeric_limits<double>::max();
    const auto claimedByAHuman = [&](Vec2 at) {
        for (const Vec2 spot : botHumanSpots_) {
            if (distanceSq(spot, at) < kBotPlayerClaimRadius * kBotPlayerClaimRadius) return true;
        }
        return false;
    };

    botCandidates_.clear();
    grid_.query(Realm::Overworld, out.at, kBotSenseRadius, botCandidates_);
    for (const Entity candidate : botCandidates_) {
        if (candidate == bot.entity) continue;
        if (!world_.isAlive(candidate)) continue;
        const Transform* other = world_.tryGet<Transform>(candidate);
        if (other == nullptr) continue;
        const double dist = (other->position - out.at).length();

        // -- ground loot ---------------------------------------------------
        if (const DropItem* drop = world_.tryGet<DropItem>(candidate)) {
            if (std::find(drop->pickedUpBy.begin(), drop->pickedUpBy.end(), bot.entity) !=
                drop->pickedUpBy.end()) {
                continue;
            }
            // Only chase what this bot actually earned: a drop with a
            // reservation list is reserved.
            if (!drop->eligible.empty() &&
                std::find(drop->eligible.begin(), drop->eligible.end(), bot.entity) ==
                    drop->eligible.end()) {
                continue;
            }
            const double scored =
                (candidate == ai.pickup ? dist - kBotPickupStickiness : dist) /
                std::max(0.35, ai.persona.greed);
            if (scored < bestPickupScore) {
                bestPickupScore = scored;
                out.pickup = candidate;
            }
            continue;
        }

        // -- mobs ----------------------------------------------------------
        if (world_.has<Dead>(candidate)) continue;
        if (!world_.has<MobTag>(candidate) || world_.has<Pet>(candidate)) continue;
        const MobType* type = world_.tryGet<MobType>(candidate);
        if (type == nullptr || excludedMobType(type->configIndex)) continue;
        const Body* mobBody = world_.tryGet<Body>(candidate);
        const double mobRadius = mobBody != nullptr ? mobBody->radius : kMobBaseRadius;
        ++out.mobsInRange;

        // Skin to skin, which is the distance that decides a collision. A
        // giant mob's centre can be far away while its edge is touching.
        const double gap = dist - out.bodyRadius - mobRadius;
        const bool blocking = gap < kBotBlockerMargin;
        if (blocking && dist < nearestBlocker) {
            nearestBlocker = dist;
            out.blocker = candidate;
        }

        const MobAi* mobAi = world_.tryGet<MobAi>(candidate);
        const bool hunting = mobAi != nullptr && mobAi->target == bot.entity;
        if (hunting && dist < nearestThreat) {
            nearestThreat = dist;
            out.threat = candidate;
        }

        const bool boss = isBotBossTier(type->rarity);
        const bool forced = blocking || hunting;
        if (!forced && dist > botNoticeRangeForTier(type->rarity)) continue;
        // Somebody real is farming this one. See kBotPlayerClaimRadius: the
        // bots exist to make the world look inhabited, not to strip it in
        // front of the person who came to play in it.
        if (!forced && claimedByAHuman(other->position)) continue;
        // Beneath this bot's notice. See kBotTierFloorBelowGear.
        if (!forced && !boss && rarityIndex(type->rarity) < gearIndex - kBotTierFloorBelowGear) {
            continue;
        }
        if (!forced && !boss && ai.hasHome &&
            distanceSq(ai.home, other->position) > leashSq) {
            continue;
        }

        // Score in distance units, so every term is readable as "worth this
        // many units of walking".
        double score = tierAppetite(type->rarity) - dist;
        // Something already touching the bot is not a choice: it is in the
        // way, and walking past it is free damage for the mob. This is the
        // term that stops bots shouldering through a field of mobs.
        if (blocking) score += 2800.0;
        // And something already biting is what a player turns around for.
        if (hunting) score += 1500.0;
        if (candidate == ai.target) score += kBotTargetStickiness;
        // A mob at or near the bot's own tier is what its build is for.
        const int mobIndex = rarityIndex(type->rarity);
        if (mobIndex >= gearIndex - 1 && mobIndex <= gearIndex + 1) score += 260.0;

        if (score > bestScore) {
            bestScore = score;
            out.target = candidate;
            out.targetDist = dist;
            out.targetRadius = mobRadius;
            out.targetIsBoss = boss;
        }
    }

    if (out.mobsInRange > 0) ai.lastMobSeenMillis = nowMillis;
}

// ---------------------------------------------------------------------------
// Hunting grounds
// ---------------------------------------------------------------------------

bool GameServer::botPickHuntingGround(const Bot& bot, Vec2& out) {
    const Transform* self = world_.tryGet<Transform>(bot.entity);
    const Vec2 at = self != nullptr ? self->position : Vec2{};
    const MapData* map = worldMaps_.forRealm(Realm::Overworld);
    if (map == nullptr) return false;

    // Where the humans are. Not because a bot wants to stand next to one, but
    // because of what their presence DOES: the spawner stocks a band whose
    // bounding box overlaps somebody's viewport, and it then fills the WHOLE
    // band uniformly -- not the part near the player (systems/spawning.cpp,
    // runSpawnZones). So the ground that has mobs on it is the band a human is
    // standing in, all nine thousand units of it, and a bot that hugs the
    // player is standing in the emptiest part of stocked ground.
    std::vector<Entity> humans;
    for (const auto& entry : sessions_) {
        const Session& session = entry.second;
        if (!session.playing() || world_.has<Dead>(session.entity)) continue;
        const Transform* transform = world_.tryGet<Transform>(session.entity);
        if (transform == nullptr || transform->realm != Realm::Overworld) continue;
        humans.push_back(session.entity);
    }

    // The bands a human is currently keeping stocked. The overlap test is the
    // spawner's own, at the default viewport -- a client reports its own, so
    // this is an estimate, but it is an estimate of a box nine thousand units
    // wide and being a few hundred out cannot change the answer.
    std::vector<const MapElement*> live;
    std::vector<const MapElement*> all;
    for (const MapElement& element : map->elements()) {
        if (!element.isSpawnBand()) continue;
        if (element.bounds.w <= 0 || element.bounds.h <= 0) continue;
        all.push_back(&element);
        for (const Entity human : humans) {
            const Vec2 at = world_.get<Transform>(human).position;
            if (element.bounds.left() < at.x + kSpawnViewportHalfWidth &&
                element.bounds.right() > at.x - kSpawnViewportHalfWidth &&
                element.bounds.top() < at.y + kSpawnViewportHalfHeight &&
                element.bounds.bottom() > at.y - kSpawnViewportHalfHeight) {
                live.push_back(&element);
                break;
            }
        }
    }

    // Nothing is being stocked -- nobody online, or everybody standing on
    // ground no band covers. Fall back to the band this bot's gear suits, so
    // that when somebody does arrive the world already looks inhabited.
    std::vector<const MapElement*>& pool = live.empty() ? all : live;
    if (pool.empty()) return false;

    const MapElement* band = nullptr;
    if (pool.size() == 1) {
        band = pool.front();
    } else {
        // Weighted by how well the band's tier suits the bot's gear, with the
        // bot's own roll breaking the tie. A strict "closest tier wins" pick
        // gives every bot on the server the same answer, which is how the old
        // controller marched the whole population onto one coordinate.
        const double wanted =
            std::min(static_cast<double>(rarityIndex(Rarity::Ultra)),
                     static_cast<double>(botMaxRarityIndex(bot)) + 1.0);
        double total = 0;
        std::vector<double> weights;
        weights.reserve(pool.size());
        for (const MapElement* element : pool) {
            const double miss = std::abs(tierValueForDifficulty(element->difficulty) - wanted);
            // Never zero: a bot whose tier nothing on the map matches still has
            // to farm somewhere, and the alternative is it standing still.
            const double weight = 1.0 / (1.0 + miss * miss);
            weights.push_back(weight);
            total += weight;
        }
        double roll = botRng_.unit() * total;
        for (std::size_t i = 0; i < pool.size(); ++i) {
            roll -= weights[i];
            if (roll <= 0.0) { band = pool[i]; break; }
        }
        if (band == nullptr) band = pool.back();
    }

    // A spot INSIDE the outline -- a band is a hand-drawn polygon several
    // thousand units across and its bounding-box centre may not even be in it
    // -- chosen at roughly the distance from the nearest player that this bot
    // likes to work at. That keeps the population spread over ground that
    // actually grows mobs while still putting some of them where a player can
    // see them, which is the whole reason bots exist.
    // How far out this bot works, measured from the nearest player. The range
    // is the BAND'S OWN SIZE, not a fixed number of units, and that is the
    // whole trick: mobs are stocked uniformly over the band, so a population
    // that all sits within a couple of thousand units of the player occupies a
    // fifth of the stocked ground, eats it bare, and then waits for a trickle
    // that is refilling the other four fifths. Spreading the bots over the
    // band is what puts each of them next to something to kill.
    //
    // The tempers pull in opposite directions on purpose. A hunter ranges over
    // the whole band, which is where the mobs are; a drifter stays within
    // sight of whoever is online. Both are needed: a population that ALL
    // ranges wide leaves a player who has just joined looking at an empty
    // field and concluding the server is dead, and one that all crowds the
    // door starves.
    const BotPersona& persona = bot.ai.persona;
    const double pull = persona.temper == BotTemper::Hunter     ? 1.0
                        : persona.temper == BotTemper::Skirmisher ? 0.75
                        : persona.temper == BotTemper::Forager    ? 0.5
                                                                  : 0.22;
    const double span = std::max(kBotHumanOrbitMax,
                                 0.55 * std::max(band->bounds.w, band->bounds.h));
    const double preferred =
        kBotHumanOrbitMin + (span - kBotHumanOrbitMin) * pull * std::sqrt(botRng_.unit());

    Vec2 best;
    bool haveBest = false;
    double bestScore = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        Vec2 candidate;
        if (!map->spawnInElement(*band, botRng_, *terrain_, candidate)) continue;
        double miss = 0;
        if (!humans.empty()) {
            double nearest = std::numeric_limits<double>::max();
            for (const Entity human : humans) {
                const double dist = (world_.get<Transform>(human).position - candidate).length();
                nearest = std::min(nearest, dist);
            }
            miss = std::abs(nearest - preferred);
        }
        // What is standing there, less what it costs to get there, less how
        // far it is from where this bot likes to work. A bot that picks its
        // patch by geometry alone finds out whether anything lives on it by
        // walking there; one that picks by heat alone spends its life walking
        // to wherever was busiest a minute ago.
        const double commute = (candidate - at).length();
        const double score = static_cast<double>(botMobHeatAt(candidate)) * kBotHeatWorth -
                             commute - miss * 0.5;
        if (!haveBest || score > bestScore) {
            best = candidate;
            bestScore = score;
            haveBest = true;
        }
    }
    if (!haveBest) {
        out = band->centre();
        return true;
    }
    out = best;
    return true;
}

void GameServer::botUpdateHome(Bot& bot, double nowMillis, const BotSenses& senses) {
    BotAiState& ai = bot.ai;

    // A rally overrides the patch entirely, and is re-read every tick so the
    // crowd follows a boss that moves. The crowd radius comes from the slot
    // table, which sized it off how many raiders there are.
    Vec2 rally;
    double bossDist = 0;
    bool rallied = activeForcedRaidAnchor(nowMillis, rally);
    if (!rallied) rallied = botNearestBoss(bot, rally, bossDist);
    if (rallied) {
        const auto slot = botRaidSlots_.find(bot.entity);
        ai.home = rally;
        ai.homeRadius = slot != botRaidSlots_.end() ? slot->second.crowdRadius : kBotRaidRingMin;
        ai.hasHome = true;
        ai.homeReached = false;
        // Short, so the moment the boss is gone the bot re-picks properly
        // rather than working the empty ground it died on.
        ai.homeUntilMillis = nowMillis + 1000.0;
        return;
    }

    if (ai.hasHome && !ai.homeReached &&
        distanceSq(ai.home, senses.at) < ai.homeRadius * ai.homeRadius) {
        // Arrived. Now the real working clock starts -- until here it was
        // running on the much shorter "can this bot even get there?" one.
        ai.homeReached = true;
        ai.homeUntilMillis = nowMillis + botRng_.range(kBotHomeMinMillis, kBotHomeMaxMillis) *
                                             (2.0 - ai.persona.restlessness);
    }

    // A patch with nothing living on it is not a patch. Only judged once the
    // bot is actually standing on it: everywhere looks barren from two
    // thousand units away, and re-rolling mid-walk is how a bot ends up
    // oscillating between two destinations forever.
    const bool barren = ai.homeReached && senses.mobsInRange == 0 &&
                        nowMillis - ai.lastMobSeenMillis > kBotBarrenPatienceMillis;
    if (ai.hasHome && !barren && nowMillis < ai.homeUntilMillis) return;

    Vec2 picked;
    if (!botPickHuntingGround(bot, picked)) {
        // No map, no people: work where it stands rather than freezing.
        if (!ai.hasHome) {
            ai.home = senses.at;
            ai.homeRadius = kBotHomeRadius;
            ai.hasHome = true;
            ai.homeReached = true;
        }
        ai.homeUntilMillis = nowMillis + kBotHomeUnreachedMillis;
        return;
    }

    ai.home = picked;
    ai.homeRadius = kBotHomeRadius;
    ai.hasHome = true;
    ai.homeReached = distanceSq(picked, senses.at) < kBotHomeRadius * kBotHomeRadius;
    ai.homeUntilMillis =
        nowMillis + (ai.homeReached ? botRng_.range(kBotHomeMinMillis, kBotHomeMaxMillis)
                                    : kBotHomeUnreachedMillis);
    ai.lastMobSeenMillis = nowMillis;
    botClearPath(ai);
}

// ---------------------------------------------------------------------------
// The activities
// ---------------------------------------------------------------------------

void GameServer::botEnter(Bot& bot, BotActivity activity, double nowMillis) {
    BotAiState& ai = bot.ai;
    if (ai.activity == activity) return;
    ai.activity = activity;
    ai.activitySinceMillis = nowMillis;
    // A path is only ever about getting somewhere far. Every other activity
    // steers locally, and a stale path outliving the trip that made it is a
    // bot walking to where it used to be going.
    if (activity != BotActivity::Travel && activity != BotActivity::Hunt) botClearPath(ai);
    if (activity == BotActivity::Roam) {
        // Carry on in the direction it was already facing rather than snapping
        // to a fresh random heading: a fight that ends should leave the flower
        // drifting off, not pivoting.
        if (ai.hasHeading) ai.roamAngle = std::atan2(ai.heading.y, ai.heading.x);
        ai.roamReady = true;
    }
    if (activity != BotActivity::Roam) ai.idleUntilMillis = 0;
}

void GameServer::botFight(Bot& bot, const BotSenses& senses, double nowMillis) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr || senses.target == NULL_ENTITY) return;
    BotAiState& ai = bot.ai;
    const BotPersona& persona = ai.persona;

    const Vec2 targetAt = world_.get<Transform>(senses.target).position;
    const Vec2 toward = targetAt - senses.at;
    const double dist = std::max(1e-6, toward.length());
    const Vec2 direction = toward / dist;

    // Petals point at what the bot is fighting, with a slow drift on the aim
    // so the ring is not welded to the mob's centre.
    const double aimWobble = botNoise(nowMillis, persona.noisePhase * 1.7) * 0.05;
    input->current.aimAngle = std::atan2(toward.y, toward.x) + aimWobble;
    input->aimDirection = Vec2::fromAngle(input->current.aimAngle);

    // The true maximum hit distance, centre to centre, is where a petal's far
    // edge just touches the mob's edge: reach, less the buffer folded into it,
    // plus the mob's radius. Stand about ten units inside that so position
    // jitter still lands hits -- without the subtraction the buffer is counted
    // twice and the bot parks just outside real reach.
    const double baseStandoff = senses.reach - kBotStandoffBuffer + senses.targetRadius - 10.0;
    const double dangerDist = senses.bodyRadius + senses.targetRadius + 6.0;
    // The persona's radial bias spreads the equilibrium ring so neighbouring
    // bots do not all sit at one distance and get shoved in and out of it
    // together by separation. Floored above dangerDist: the bias reaches -40
    // and a defend-only build's reach is around 98, so the two together can
    // put the ring INSIDE the mob's collision circle -- and the controller
    // then has two branches fighting each other every tick, which from outside
    // is a bot repeatedly ramming the mob it is fighting.
    const double standoff = std::max(dangerDist + 8.0, baseStandoff + persona.standoffBias);

    // In the fight: petals out. The hold inside botSetPetals keeps this from
    // chattering when the distance crosses back and forth.
    botSetPetals(bot, true, nowMillis);

    if (dist < dangerDist) {
        // Too close -- shove off, but stay in attack state so the petals stay
        // extended while killing it. High agility: this is the one case where
        // an instant direction change is right.
        botDrive(bot, -direction, 1.0, kBotAvoidStrengthFight, 3.0, senses.target);
        return;
    }

    const double strafe = static_cast<double>(botStrafeDirection(bot, nowMillis));
    Vec2 move;
    double speedMultiplier;

    // A boss raider owns an angular slot around the boss, so a raid spreads
    // out instead of stacking on one side.
    const auto slot = senses.targetIsBoss ? botRaidSlots_.find(bot.entity) : botRaidSlots_.end();
    if (slot != botRaidSlots_.end()) {
        // Ease toward the assigned slot. The raw assignment jumps every time a
        // raider joins or dies -- the slots are redealt -- and snapping to the
        // new angle sent bots sprinting around the boss, or, with two raiders
        // trading slots, back and forth forever.
        double use = slot->second.angle;
        if (ai.hasSlotAngle) {
            use = ai.slotAngle + clamp(wrapAngle(slot->second.angle - ai.slotAngle), -0.06, 0.06);
        }
        ai.slotAngle = use;
        ai.hasSlotAngle = true;

        const Vec2 toSlot = targetAt + Vec2::fromAngle(use, standoff) - senses.at;
        const double slotDist = toSlot.length();
        if (slotDist > 12.0) {
            // Speed tapers continuously to the strafe speed as the bot settles
            // in, so there is no threshold to flip across.
            move = toSlot / slotDist;
            speedMultiplier = std::min(0.8, 0.15 + slotDist / 300.0);
        } else {
            move = Vec2{-direction.y * strafe, direction.x * strafe};
            speedMultiplier = 0.18;
        }
    } else {
        // A continuous orbit controller, not a ladder of discrete distance
        // bands: a bot whose distance wobbled across a band edge would flip
        // between backing off and closing in every tick, which is the
        // in-combat form of the two-position shuffle. The radial correction is
        // proportional to how far off the ring the bot is and passes smoothly
        // through zero at the ring itself, so there is nothing to flip
        // between.
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
    // friends) so per-tick movement matches what the controller was tuned for.
    // A powder-wearing bot moving at double speed through the standoff zone
    // otherwise overshoots the ring every tick and ping-pongs across it
    // instead of orbiting.
    double speedMod = 1.0;
    if (const PlayerModifiers* mods = world_.tryGet<PlayerModifiers>(bot.entity)) {
        speedMod = mods->speedScale;
    }
    const double effective = speedMod > 1.0 ? speedMultiplier / speedMod : speedMultiplier;
    botDrive(bot, move, effective, kBotAvoidStrengthFight, 1.0, senses.target);
}

void GameServer::botHunt(Bot& bot, const BotSenses& senses, double nowMillis) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr || senses.target == NULL_ENTITY) return;

    const Vec2 targetAt = world_.get<Transform>(senses.target).position;
    const Vec2 toward = targetAt - senses.at;
    const double dist = std::max(1e-6, toward.length());
    input->current.aimAngle = std::atan2(toward.y, toward.x);
    input->aimDirection = Vec2::fromAngle(input->current.aimAngle);

    // Petals carried neutral on the way in, thrown out as the bot arrives.
    // A flower sprinting across a field with its ring at full extension is not
    // something a player does; one that flares them as it closes is.
    const double flare = senses.reach + senses.targetRadius + 90.0;
    botSetPetals(bot, dist < flare, nowMillis);

    // Mob repulsion stays low while closing: a hunter that dodged every mob
    // between it and its target would circle the field forever. Anything
    // actually in the way has already been promoted to the target by the
    // sensing pass.
    const double avoid = kBotAvoidStrengthFight;
    if (dist > kTileSize * 2.0 &&
        botFollowPath(bot, nowMillis, targetAt, 0.95, avoid, senses.target)) {
        return;
    }
    botDrive(bot, botSteerAroundWalls(senses.at, toward / dist), 0.95, avoid, 1.0, senses.target);
}

void GameServer::botLoot(Bot& bot, const BotSenses& senses, double nowMillis) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr || senses.pickup == NULL_ENTITY) return;
    bot.ai.pickup = senses.pickup;

    const Vec2 toward = world_.get<Transform>(senses.pickup).position - senses.at;
    const double dist = std::max(1e-6, toward.length());
    const Vec2 direction = toward / dist;
    input->current.aimAngle = std::atan2(direction.y, direction.x);
    input->aimDirection = direction;
    botSetPetals(bot, false, nowMillis);
    // Only steer around walls when the drop is far enough that one could
    // genuinely be in the way; close-range pickup does not need it.
    botDrive(bot, dist > kTileSize ? botSteerAroundWalls(senses.at, direction) : direction, 0.9,
             kBotAvoidStrengthTravel);
}

void GameServer::botRetreat(Bot& bot, const BotSenses& senses, double nowMillis) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr) return;
    BotAiState& ai = bot.ai;

    const Entity from = senses.threat != NULL_ENTITY ? senses.threat : senses.target;
    Vec2 away = ai.hasHeading ? ai.heading : Vec2{1, 0};
    if (from != NULL_ENTITY && world_.isAlive(from) && world_.has<Transform>(from)) {
        const Vec2 delta = senses.at - world_.get<Transform>(from).position;
        if (delta.lengthSq() > 1e-9) away = delta.normalized();
    }

    // Break away at an angle rather than straight back: a dead-straight
    // retreat line from a chasing mob is a bot tell, and the angle is also
    // what gets a flower around the thing rather than backed into a wall.
    const double strafe = static_cast<double>(botStrafeDirection(bot, nowMillis));
    Vec2 escape{away.x + (-away.y) * 0.4 * strafe, away.y + away.x * 0.4 * strafe};

    // Bias the run back toward its own ground, which is also where the other
    // bots are. A hurt player runs toward their friends, not into the dark.
    if (ai.hasHome) {
        const Vec2 toHome = ai.home - senses.at;
        const double homeDist = toHome.length();
        if (homeDist > 1.0) escape += (toHome / homeDist) * 0.45;
    }

    // Petals in: the retracted ring is the smaller target, and it is what a
    // player holds while running.
    botSetPetals(bot, false, nowMillis, true);
    input->current.aimAngle = std::atan2(escape.y, escape.x);
    input->aimDirection = Vec2::fromAngle(input->current.aimAngle);
    botDrive(bot, botSteerAroundWalls(senses.at, escape.normalized()), 1.0,
             kBotAvoidStrengthTravel, 1.6);
}

void GameServer::botTravel(Bot& bot, const BotSenses& senses, double nowMillis, Vec2 goal) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr) return;
    botSetPetals(bot, false, nowMillis);

    const Vec2 toward = goal - senses.at;
    const double dist = std::max(1e-6, toward.length());
    input->current.aimAngle = std::atan2(toward.y, toward.x);
    input->aimDirection = Vec2::fromAngle(input->current.aimAngle);

    if (botFollowPath(bot, nowMillis, goal, 1.0, kBotAvoidStrengthTravel)) return;
    botDrive(bot, botSteerAroundWalls(senses.at, toward / dist), 1.0, kBotAvoidStrengthTravel);
}

void GameServer::botRoam(Bot& bot, const BotSenses& senses, double nowMillis) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr) return;
    BotAiState& ai = bot.ai;
    const BotPersona& persona = ai.persona;

    botSetPetals(bot, false, nowMillis);

    // Stops. Considered on its own clock rather than per tick, so the chance
    // means "how often does this bot pause" rather than "what fraction of
    // ticks is it frozen".
    if (nowMillis >= ai.roamPauseCheckMillis) {
        ai.roamPauseCheckMillis =
            nowMillis + kBotRoamPauseCheckMillis * botRng_.range(0.6, 1.6);
        if (botRng_.chance(persona.stillness)) {
            ai.idleUntilMillis =
                nowMillis + botRng_.range(kBotRoamPauseMinMillis, kBotRoamPauseMaxMillis);
        }
    }
    if (nowMillis < ai.idleUntilMillis) {
        botHold(bot);
        // Standing still on purpose is not being stuck.
        ai.stuckMillis = 0;
        // Look around while stopped: the aim drifts rather than staying
        // welded to the last heading.
        input->current.aimAngle =
            wrapAngle(input->current.aimAngle + botNoise(nowMillis, persona.noisePhase) * 0.06);
        input->aimDirection = Vec2::fromAngle(input->current.aimAngle);
        return;
    }
    ai.idleUntilMillis = 0;

    if (!ai.roamReady) {
        ai.roamAngle = std::atan2(persona.bias.y, persona.bias.x);
        ai.roamReady = true;
    }

    // The walk itself: the heading DRIFTS. It is not re-picked, which is what
    // makes the path a curve instead of a sequence of straight dashes with
    // hard corners and dead stops between them.
    constexpr double dt = net::kTickSeconds;
    ai.roamAngle = wrapAngle(ai.roamAngle + botRng_.range(-1.0, 1.0) * kBotRoamDriftRate * dt);

    // Near the edge of its ground, the heading is pulled back toward the
    // middle -- continuously, in proportion to how far out it is, so there is
    // no boundary to bounce off.
    if (ai.hasHome) {
        const Vec2 offset = senses.at - ai.home;
        const double distance = offset.length();
        const double edge = ai.homeRadius * kBotRoamEdgeFraction;
        if (distance > edge && distance > 1.0) {
            const double over =
                clamp((distance - edge) / std::max(1.0, ai.homeRadius - edge), 0.0, 1.0);
            const double inward = std::atan2(-offset.y, -offset.x);
            ai.roamAngle =
                lerpAngle(ai.roamAngle, inward, std::min(1.0, over * kBotRoamHomePull * dt));
        }
    }

    // A wall ahead turns the walk rather than stopping it, and the turn is
    // adopted, so the bot carries on the new way instead of grinding back into
    // the wall next tick.
    const Vec2 steered = botSteerAroundWalls(senses.at, Vec2::fromAngle(ai.roamAngle));
    ai.roamAngle = std::atan2(steered.y, steered.x);

    // Per-bot cruise speed with a slow drift, so a field of wandering bots
    // does not move like one formation at a single fixed pace.
    const double cruise = persona.cruise * (0.88 + 0.12 * botNoise(nowMillis, persona.noisePhase));
    input->current.aimAngle = ai.roamAngle;
    input->aimDirection = Vec2::fromAngle(ai.roamAngle);
    botDrive(bot, steered, cruise, kBotAvoidStrengthTravel);
}

void GameServer::botRevive(Bot& bot, const BotSenses& senses, double nowMillis, Entity downed) {
    PlayerInput* input = world_.tryGet<PlayerInput>(bot.entity);
    if (input == nullptr || downed == NULL_ENTITY) return;
    // The revive fires when an orbiting petal touches the body, so all the bot
    // has to do is get the corpse inside its ring -- which is why it stands
    // right on top of it with petals extended.
    const Vec2 toward = world_.get<Transform>(downed).position - senses.at;
    const double dist = std::max(1e-6, toward.length());
    input->current.aimAngle = std::atan2(toward.y, toward.x);
    input->aimDirection = Vec2::fromAngle(input->current.aimAngle);
    botSetPetals(bot, true, nowMillis);
    botDrive(bot, botSteerAroundWalls(senses.at, toward / dist), 0.95, kBotAvoidStrengthFight);
}

// ---------------------------------------------------------------------------
// The pass
// ---------------------------------------------------------------------------

void GameServer::stepBots(double nowMillis) {
    if (bots_.empty()) return;

    // Where the people are. Read once for the whole pass: it gates every
    // candidate mob of every bot, and it changes at walking pace.
    botHumanSpots_.clear();
    for (const auto& entry : sessions_) {
        const Session& session = entry.second;
        if (!session.playing() || world_.has<Dead>(session.entity)) continue;
        const Transform* transform = world_.tryGet<Transform>(session.entity);
        if (transform == nullptr || transform->realm != Realm::Overworld) continue;
        botHumanSpots_.push_back(transform->position);
    }

    rebuildBotBossIndex(nowMillis);
    rebuildBotMobHeat();
    updateBotSquads(nowMillis);
    // Reset the per-tick A* budget, so one tick cannot be dominated by
    // simultaneous recomputes -- a whole raid replanning at once.
    botPathBudget_ = kBotPathMaxPerTick;
    announceNewBosses(nowMillis);
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

    // The persona: how this bot plays, rolled once and held, seeded off its id
    // so it survives a death and is reproducible when debugging one.
    if (!ai.personaReady) {
        ai.personaReady = true;
        BotRng rng(bot.id);
        const double temper = rng.unit();
        ai.persona.temper = temper < 0.34   ? BotTemper::Hunter
                            : temper < 0.62 ? BotTemper::Skirmisher
                            : temper < 0.85 ? BotTemper::Forager
                                            : BotTemper::Drifter;
        ai.persona.bias = Vec2::fromAngle(rng.unit() * kTau);
        ai.persona.noisePhase = rng.unit() * kTau;
        ai.persona.standoffBias = -(2.0 + rng.unit() * 38.0);   // 2-40 units inside max reach
        ai.persona.reactionMillis = 110.0 + rng.unit() * 300.0;
        ai.persona.turnRate = 0.26 + rng.unit() * 0.22;         // rad/tick
        ai.persona.cruise = 0.42 + rng.unit() * 0.32;
        ai.persona.aggression = 0.85 + rng.unit() * 0.30;
        ai.persona.restlessness = 0.7 + rng.unit() * 0.6;
        ai.persona.stillness = rng.unit() * 0.4;
        ai.persona.greed = 0.7 + rng.unit() * 0.7;

        // The tempers, applied. These are the parts that make two bots pick
        // DIFFERENTLY rather than pick the same thing at different speeds.
        switch (ai.persona.temper) {
            case BotTemper::Hunter:
                ai.persona.aggression *= 1.35;
                ai.persona.restlessness *= 0.8;
                break;
            case BotTemper::Skirmisher:
                ai.persona.standoffBias *= 0.4;   // fights at arm's length
                ai.persona.aggression *= 0.75;
                break;
            case BotTemper::Forager:
                ai.persona.greed *= 1.9;
                ai.persona.aggression *= 0.9;
                break;
            case BotTemper::Drifter:
                ai.persona.cruise *= 0.85;
                ai.persona.stillness += 0.25;
                ai.persona.restlessness *= 1.4;
                break;
        }
    }
    const BotPersona& persona = ai.persona;

    // A corpse holds still and waits to be replaced; maintainBots owns the
    // replacement. The traversal petals go back first, so the rebuilt body is
    // not the one that inherits them.
    if (world_.has<Dead>(bot.entity)) {
        botUnequipPowder(bot);
        botUnequipYggdrasil(bot);
        botHold(bot);
        input->current.flags = 0;
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

    // ONE sensing pass, which every decision below is answered out of. The
    // leash it applies is last tick's hunting ground, because the ground is
    // updated from what the senses found -- a tick of lag on a number that
    // moves at walking pace.
    BotSenses senses;
    botSense(bot, nowMillis, senses);
    botUpdateHome(bot, nowMillis, senses);
    bot.hasAnchor = ai.hasHome;
    if (ai.hasHome) bot.anchor = ai.home;

    const double homeDist = ai.hasHome ? (ai.home - senses.at).length() : 0.0;

    // Traversal gear. Powder only while genuinely crossing ground: a bot that
    // wears it in a fight moves at double speed through its own standoff ring
    // and cannot hold an orbit.
    if (ai.hasHome && homeDist > kBotPowderEquipDist) botEquipPowder(bot);
    else if (!ai.hasHome || homeDist < kBotPowderUnequipDist) botUnequipPowder(bot);

    // Yggdrasil buddy swap: when another bot is close enough that it could
    // plausibly need a revive, slot 1 becomes a yggdrasil. Dropped at a WIDER
    // range than it was equipped, so the swap cannot chatter.
    if (botHasNearbyBuddy(bot, ai.yggSwapped ? kBotYggBuddyDropRange : kBotYggBuddyRange)) {
        botEquipYggdrasil(bot);
    } else {
        botUnequipYggdrasil(bot);
    }

    // Commitment and the reaction delay. A player does not lock on the instant
    // a mob crosses into range -- but they do react instantly to something
    // that is already touching them or already biting.
    if (senses.target != ai.target) {
        ai.target = senses.target;
        ai.targetNoticedMillis = nowMillis;
        ai.targetCommitted = false;
    }
    if (!ai.targetCommitted) {
        const bool urgent = senses.target != NULL_ENTITY &&
                            (senses.target == senses.blocker || senses.target == senses.threat);
        if (urgent || nowMillis - ai.targetNoticedMillis >= persona.reactionMillis) {
            ai.targetCommitted = true;
        }
    }
    const Entity target = ai.targetCommitted ? ai.target : NULL_ENTITY;

    // Flee hysteresis. Sitting exactly on the threshold flips the decision
    // every tick -- back off, heal a sliver, re-engage, get hit, back off --
    // which is the two-position shuffle driven by health rather than geometry.
    // A boss is too valuable to run from: commit unless critically low.
    const double baseFlee = kBotFleeHealthRatio * (2.0 - persona.aggression);
    const double fleeThreshold = senses.targetIsBoss ? baseFlee * 0.5 : baseFlee;
    bool fleeing = nowMillis < ai.fleeUntilMillis;
    if (fleeing) {
        if (senses.healthRatio > fleeThreshold * kBotFleeRecoverRatio) {
            ai.fleeUntilMillis = 0;
            fleeing = false;
        }
    } else if (senses.healthRatio < fleeThreshold) {
        ai.fleeUntilMillis = nowMillis + kBotFleeMinMillis;
        fleeing = true;
    }

    const Entity downed = botFindReviveTarget(bot);

    // --- what to do ------------------------------------------------------
    //
    // One pass, top down. Each case is a reason to be doing something rather
    // than a filter on the last one, and whatever it picks is held for a dwell
    // period so a reason that is momentarily true cannot make the bot twitch.
    const double engageBand = senses.reach + senses.targetRadius + kBotEngageSlack;
    const double pursue = (senses.targetIsBoss ? kBotBossPursueRange : kBotPursueRange) *
                          persona.aggression;

    // "Under attack" includes being shot by something the senses never found:
    // a projectile from off-screen takes health off a bot whose threat and
    // target are both empty, and a bot that answered that by carrying on
    // wandering would be a flower being whittled down in plain sight.
    const bool underAttack = senses.threat != NULL_ENTITY || target != NULL_ENTITY ||
                             nowMillis < ai.hurtUntilMillis;

    BotActivity want;
    if (fleeing && underAttack) {
        want = BotActivity::Retreat;
    } else if (downed != NULL_ENTITY && senses.blocker == NULL_ENTITY) {
        // Reviving outranks farming, but not something already standing on the
        // bot: nobody walks off to a corpse while being chewed on, and a bot
        // that did would arrive at the corpse as a second one.
        want = BotActivity::Revive;
    } else if (target != NULL_ENTITY && senses.targetDist <= engageBand) {
        want = BotActivity::Fight;
    } else if (target != NULL_ENTITY && senses.targetDist <= pursue) {
        want = BotActivity::Hunt;
    } else if (senses.pickup != NULL_ENTITY) {
        want = BotActivity::Loot;
    } else if (ai.hasHome && homeDist > kBotTravelDistance) {
        want = BotActivity::Travel;
    } else {
        want = BotActivity::Roam;
    }

    // Whether what it is ALREADY doing still makes sense. An activity whose
    // subject has gone -- the mob died, the drop was taken -- is swapped out
    // immediately; the dwell is about indecision, not about staleness.
    bool valid = true;
    switch (ai.activity) {
        case BotActivity::Fight:
        case BotActivity::Hunt:
            valid = target != NULL_ENTITY && senses.targetDist <= pursue;
            break;
        case BotActivity::Loot: valid = senses.pickup != NULL_ENTITY; break;
        case BotActivity::Revive:
            valid = downed != NULL_ENTITY && senses.blocker == NULL_ENTITY;
            break;
        case BotActivity::Retreat: valid = fleeing; break;
        // The trip runs until the bot is actually HOME, not until it is back
        // inside the radius that started it: equal thresholds either side of a
        // decision are a decision that flips.
        case BotActivity::Travel: valid = ai.hasHome && homeDist > ai.homeRadius; break;
        case BotActivity::Roam: valid = true; break;
    }
    // Emergencies pre-empt the dwell; everything else waits its turn.
    const bool urgent = want == BotActivity::Retreat || want == BotActivity::Fight ||
                        want == BotActivity::Revive;
    if (want != ai.activity &&
        (urgent || !valid || nowMillis - ai.activitySinceMillis >= kBotActivityDwellMillis)) {
        botEnter(bot, want, nowMillis);
    }

    // A bot with something inside its own ring is not stuck, however slowly it
    // is moving: that is an orbit, and shoving it sideways for half a second
    // would be shoving it out of a fight it is winning.
    const bool fightingClose = ai.activity == BotActivity::Fight && target != NULL_ENTITY &&
                               senses.targetDist < senses.reach + senses.targetRadius;
    if (fightingClose) ai.stuckMillis = 0;
    else if (botHandleStuck(bot, nowMillis)) return;

    ai.pickup = senses.pickup;

    switch (ai.activity) {
        case BotActivity::Fight: botFight(bot, senses, nowMillis); break;
        case BotActivity::Hunt: botHunt(bot, senses, nowMillis); break;
        case BotActivity::Loot: botLoot(bot, senses, nowMillis); break;
        case BotActivity::Retreat: botRetreat(bot, senses, nowMillis); break;
        case BotActivity::Revive: botRevive(bot, senses, nowMillis, downed); break;
        case BotActivity::Travel: botTravel(bot, senses, nowMillis, ai.home); break;
        case BotActivity::Roam: botRoam(bot, senses, nowMillis); break;
    }
}

} // namespace flix
