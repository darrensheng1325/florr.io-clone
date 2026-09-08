#include "server/game_server.h"

#include "shared/core/process_stats.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include <thread>

#include "server/auto_update.h"
#include "server/bot_identity.h"
#include "server/guilds.h"
#include "server/text.h"
#include "server/systems/combat.h"
#include "server/systems/loot.h"
#include "server/systems/mob_ai.h"
#include "server/systems/movement.h"
#include "server/systems/petals.h"
#include "server/systems/spawning.h"
#include "shared/game/config.h"
#include "shared/game/shop.h"
#include "shared/game/skin_format.h"
#include "shared/game/skills.h"

namespace flix {

double monotonicMillis() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

namespace {

/// Clamp on the viewport a client may claim. A client asking to see the whole
/// map is asking for an advantage, and for the server to build it a snapshot
/// of the entire world.
constexpr double kMaxViewportAxis = 2600.0;

/// How often account progress is written back from the live entity.
constexpr double kPersistIntervalMillis = 30000.0;

/// Ceiling on one simulation step, as a multiple of the nominal one. A long
/// stall must not be paid back as a single giant integration step that walks
/// every flower through a wall.
constexpr double kMaxDeltaSeconds = net::kTickSeconds * 3.0;
/// Low-pass factor on the step, ~a ten-tick time constant.
constexpr double kDeltaSmoothing = 0.1;

/// How often the leaderboard reward tiers are recomputed. The ranking is a
/// sort of every account and its answer changes over hours, so it is cached
/// rather than resolved per kill.
constexpr double kRankRefreshMillis = 15000.0;

/// The batch a craft consumes.
constexpr int kCraftBatch = 5;

// The database stores petals by NAME, because that is the shape real accounts
// are already saved in; the wire uses a dense index, because a name in every
// snapshot would cost more than the whole rest of the message. These two
// functions are the only place the two spellings meet.
//
// Inventory keys additionally carry a "petal_" prefix that loadout entries do
// not. That is the old schema's one real wart, and it is load-bearing: change
// either side and every stored loadout is orphaned.

std::string inventoryKey(std::uint16_t petalIndex) {
    return "petal_" + content().petal(petalIndex).id;
}

std::uint16_t petalIndexFromInventoryKey(const std::string& key) {
    const std::string id = key.rfind("petal_", 0) == 0 ? key.substr(6) : key;
    return content().petalIndex(id);
}

/// Takes `count` from the inventory, or nothing at all when short. Never
/// partially succeeds: a craft that consumed three of the five it needed and
/// then failed would quietly destroy them.
bool takeFromInventory(PlayerRecord& record, std::uint16_t petalIndex, Rarity rarity, int count) {
    if (petalIndex == kNoPetal || count <= 0) return false;
    const std::string key = inventoryKey(petalIndex);
    if (record.itemCount(rarity, key) < count) return false;
    record.addItem(rarity, key, -count);
    return true;
}

void giveToInventory(PlayerRecord& record, std::uint16_t petalIndex, Rarity rarity, int count) {
    if (petalIndex == kNoPetal || count <= 0) return;
    record.addItem(rarity, inventoryKey(petalIndex), count);
}

/// What a brand-new account starts with.
///
/// An empty loadout is a flower that cannot fight anything, which makes the
/// first minute of the game a walk through a field of mobs that can only hurt
/// it. Five Basic petals is the smallest kit that is actually playable, and
/// the five spares beside them are exactly one craft batch -- a brand-new
/// account can walk to the crafting panel and roll its first Unusual.
void grantStarterKit(PlayerRecord& record) {
    const std::uint16_t basic = content().petalIndex("basic");
    if (basic == kInvalidIndex) return;

    constexpr int kStartingEquipped = 5;
    constexpr int kStartingSpares = 5;

    record.loadout.assign(kLoadoutSlots, std::nullopt);
    for (int i = 0; i < kStartingEquipped; ++i) {
        StoredItem item;
        item.type = "petal";
        item.petalType = content().petal(basic).id;
        item.rarity = Rarity::Common;
        record.loadout[static_cast<std::size_t>(i)] = item;
    }
    giveToInventory(record, basic, Rarity::Common, kStartingSpares);
}

/// "Legendary Ladybug": the killer's tier and its type, each with only its
/// first character upper-cased.
///
/// Deliberately the mob's config ID rather than its display name, and
/// deliberately only one character of each word, because that is what the
/// reference's death card builds out of `{type, tier}` -- so a mob whose id is
/// `baby_ant` reads "Common Baby_ant" in both clients. A player killer reads
/// "Common Player", as it does there. An empty string means the killer is
/// unknown, which is what leaves the client on its own fallback wording.
std::string killerLabel(const World& world, Entity killer) {
    if (killer == NULL_ENTITY || !world.isAlive(killer)) return {};
    if (world.has<PlayerTag>(killer)) return "Common Player";
    const MobType* type = world.tryGet<MobType>(killer);
    if (type == nullptr) return {};
    std::string id = content().mob(type->configIndex).id;
    if (id.empty()) return {};
    id[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(id[0])));
    return std::string(rarityLabel(type->rarity)) + " " + id;
}

/// The five type tags the browser stores, as the wire enum. Anything else is
/// Generic, which is also what an older notification with no type reads as.
net::NotificationKind notificationKind(const std::string& type) {
    if (type == "super_craft") return net::NotificationKind::SuperCraft;
    if (type == "unique_craft") return net::NotificationKind::UniqueCraft;
    if (type == "apex_craft") return net::NotificationKind::ApexCraft;
    if (type == "star_code") return net::NotificationKind::StarCode;
    return net::NotificationKind::Generic;
}

} // namespace

GameServer::GameServer() = default;
GameServer::~GameServer() = default;

bool GameServer::start(const ServerConfig& config, std::string& errorOut) {
    config_ = config;

    if (!loadContent(config.dataDir, errorOut)) return false;
    for (const std::string& warning : content().warnings()) {
        std::fprintf(stderr, "[content] %s\n", warning.c_str());
    }

    // A database that exists but will not parse is fatal at startup. Coming up
    // with an empty one would serve every returning player a blank account and,
    // worse, save that over the file they still had.
    if (!database_.load(config.databasePath, errorOut)) return false;

    rng_.reseed(config.worldSeed);
    terrain_ = std::make_unique<Terrain>();
    if (!terrain_->loadMapBundle(config.dataDir + "/map_bundle.ts", errorOut)) return false;
    // The annotation layer is optional: without it every spawn falls back to
    // the middle of the map, which is survivable for a server operator to see
    // in a warning but not worth refusing to start over.
    std::string mapWarning;
    if (!mapData_.load(config.dataDir + "/map_bundle.ts", mapWarning)) {
        std::fprintf(stderr, "[map] %s; spawns will fall back to the map centre\n",
                     mapWarning.c_str());
    }

    movement_ = std::make_unique<MovementSystem>();
    // The AI caches queries against one world and wanders from its own
    // stream, so it takes both at construction rather than per call.
    mobAi_ = std::make_unique<MobAiSystem>(world_, config.worldSeed ^ 0x9E3779B9ull);
    petals_ = std::make_unique<PetalSystem>();
    combat_ = std::make_unique<CombatSystem>();
    spawning_ = std::make_unique<SpawnSystem>();
    loot_ = std::make_unique<LootSystem>();
    if (!loot_->loadTables(content(), config.dataDir + "/mob_drops.json", errorOut)) return false;

    // Wire ids are unique across the whole server, so the id space belongs
    // here rather than to any one system. A system left unwired still
    // simulates -- its entities are simply not replicated, which is what a
    // headless test wants and what this must not be in production.
    petals_->allocateNetId = [this] { return netIds_.next(); };
    mobAi_->allocateNetId = [this] { return netIds_.next(); };
    spawning_->netIds = &netIds_;
    loot_->netIds = &netIds_;
    // One table, two readers: what a corpse pays XP for and what it reserves
    // its drops for must be the same answer. rebuildSquadIndex() refreshes it
    // once a tick; the pointer never moves.
    loot_->squads = &squadIndex_;
    combat_->squads = &squadIndex_;

    // The annotation layer is a read-only service, so the systems that need it
    // hold a pointer rather than being handed it per call. Left null they fall
    // back to behaviour with no map at all, which is what every unit test gets.
    // In production that would silently cost the spawn rectangles, the biome
    // tiers and every teleporter on the map, so it is wired here, once.
    spawning_->mapData = &mapData_;
    movement_->mapData = &mapData_;
    loot_->terrain = terrain_.get();

    // A revived flower has to be un-announced to its own client, and only the
    // connection layer can do that.
    petals_->onPlayerRevived = [this](Entity revived, Entity reviver) {
        onPlayerRevived(revived, reviver);
    };

    // The maze is a daily-seeded region of the world with its own walls. Its
    // geometry is a pure function of the day number, so the server picks the
    // day once at boot; every collision query inside the region then answers
    // against the same maze for the whole session.
    setActiveMazeDay(currentMazeDay());

    listener_.certPath = config.certPath;
    listener_.keyPath = config.keyPath;
    listener_.webRoot = config.webRoot;
    if (!listener_.start(config.port, errorOut)) return false;

    // Seeded before the first tick can set it: a message may be serviced ahead
    // of the first tick, and a deadline stamped from a zero clock is one that
    // has already passed.
    clockMillis_ = monotonicMillis();
    running_ = true;
    return true;
}

void GameServer::run() {
    nextTickMillis_ = monotonicMillis();
    clockMillis_ = nextTickMillis_;
    while (step()) {
    }
    shutdown();
}

void GameServer::shutdown() {
    // Flush account progress before exiting; an orderly shutdown must not cost
    // anyone their session.
    persistAll();
    listener_.stop();
}

std::size_t GameServer::persistAll() {
    std::size_t saved = 0;
    for (const auto& entry : sessions_) {
        if (!entry.second.playing()) continue;
        persistPlayer(entry.second);
        ++saved;
    }
    database_.save();
    return saved;
}

bool GameServer::step() {
    if (!running_.load()) return false;

    const double now = monotonicMillis();

    // Sleep in the network poll rather than in a bare sleep, so a packet
    // arriving mid-frame is picked up immediately instead of waiting out
    // the remainder of the tick.
    const int waitMillis = static_cast<int>(std::max(0.0, nextTickMillis_ - now));
    serviceNetwork(std::min(waitMillis, 5));

    const double tickNow = monotonicMillis();
    if (tickNow >= nextTickMillis_) {
        // Real elapsed time, clamped, then low-pass filtered -- see
        // smoothedDeltaSeconds_. Sampled before the tick so a slow tick
        // shows up in the NEXT step's delta, exactly as the reference's
        // performance.now() sample at the top of its interval does.
        double raw = lastTickWallMillis_ > 0
                         ? (tickNow - lastTickWallMillis_) / 1000.0
                         : net::kTickSeconds;
        lastTickWallMillis_ = tickNow;
        if (raw > kMaxDeltaSeconds) raw = kMaxDeltaSeconds;
        smoothedDeltaSeconds_ += (raw - smoothedDeltaSeconds_) * kDeltaSmoothing;

        // The REAL clock, not the scheduled slot. Every absolute deadline
        // in the world -- poison, slows, reloads, hit cooldowns, despawn --
        // is compared against this, so handing over a nominal time that
        // lags wall clock makes all of them fire late and then jump.
        const double tickStarted = monotonicMillis();
        tick(tickNow);
        const double tookMillis = monotonicMillis() - tickStarted;
        debugTickAccumMillis_ += tookMillis;
        if (tookMillis > debugTickMaxMillis_) debugTickMaxMillis_ = tookMillis;
        ++debugTickSamples_;

        nextTickMillis_ += net::kTickMillis;
        // A timer never queues up the fires it missed: a tick that overran
        // simply makes the next one land immediately, it does not run twice
        // to catch up. Replaying would advance the fixed-step mob half once
        // per replay while the dt-scaled half, which has just had its delta
        // reset to almost nothing, stood still.
        if (nextTickMillis_ < monotonicMillis()) nextTickMillis_ = monotonicMillis();

        if (tickNow >= nextDebugStatsMillis_) {
            broadcastDebugStats();
            nextDebugStatsMillis_ = tickNow + 1000.0;
        }
    }

    return running_.load();
}

void GameServer::serviceNetwork(int timeoutMillis) {
    listener_.poll(*this, timeoutMillis);
}

std::size_t GameServer::playerCount() const {
    std::size_t n = 0;
    for (const auto& entry : sessions_) {
        if (entry.second.playing()) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void GameServer::tick(double nowMillis) {
    ++tick_;
    clockMillis_ = nowMillis;

    for (auto& entry : sessions_) refillAllowances(entry.second, nowMillis);

    // Invitations lapse on the server's own clock rather than on a timer per
    // invite, which is a timer that has to be cancelled when its target
    // disconnects. The ranking table is rebuilt beside it because a member's
    // BODY changes on every death: cached against the roster it would go stale
    // without the roster ever moving.
    squads_.expire(static_cast<std::int64_t>(nowMillis));
    rebuildSquadIndex();

    // Above the idle gate on purpose: a server with nobody left on it is
    // exactly the one a scheduled restart is usually waiting for, and an
    // install that finished while the last player left still has a restart to
    // schedule.
    serviceAutoUpdate();
    serviceScheduledRestart(nowMillis);

    // Housekeeping, ABOVE the idle gate: an account registered by somebody
    // sitting on the title screen is dirty in memory and would otherwise wait
    // for the next player to actually join before it reached the disk.
    if (nowMillis >= nextPersistMillis_) {
        nextPersistMillis_ = nowMillis + kPersistIntervalMillis;
        for (const auto& entry : sessions_) {
            if (entry.second.playing()) persistPlayer(entry.second);
        }
        database_.pruneExpiredSessions();
        database_.save();
    }

    // Bot population is maintained on every tick, INCLUDING the idle ones the
    // gate below returns out of -- that is where the reference calls it, and
    // it is what lets an empty server retire its bots after the grace period
    // rather than leaving them simulating for nobody.
    maintainBots(nowMillis);

    // Nobody in the world means nothing to simulate. The reference returns out
    // of its tick here and the whole world freezes: mobs stop wandering, nests
    // stop firing, lifetimes stop burning down and the unseen-despawn census
    // never runs. Without this gate an idle server quietly empties itself of
    // every mob it had and the first player back arrives on a bare map.
    //
    // The command buffer is still flushed: a disconnect between ticks queues a
    // destroy, and leaving it queued would keep the departed body in the world
    // and in every query until somebody joined.
    if (playerCount() == 0) {
        events_.clear();
        commands_.flush();
        listener_.flush();
        return;
    }

    // Record which input each player's movement is about to consume. The
    // snapshot reports this back, and it is the whole basis of reconciliation:
    // the client discards the predicted inputs at or below it and replays only
    // what is still outstanding. Left at zero, every client replays its entire
    // queue on top of an already-current position and drifts further each tick.
    Query<PlayerTag, PlayerInput> inputs{world_};
    inputs.each([](Entity, PlayerTag&, PlayerInput& input) {
        input.lastAppliedSequence = input.current.sequence;
    });

    runSystems(nowMillis, smoothedDeltaSeconds_);

    reapDead(nowMillis);
    commands_.flush();

    // The wire runs slower than the simulation and on its own clock: physics
    // and combat want 30 Hz, clients do not, and the per-recipient encode/cull/
    // delta pass is the largest thing in the tick that nothing simulated
    // depends on. Nothing about WHAT happens changes -- one-shot events are
    // queued for the frame either way -- only how often it is described.
    if (nowMillis >= nextSnapshotMillis_) {
        // Stepped from the deadline, not from now, so the send rate does not
        // slew with tick jitter; resynced when it falls a whole interval behind
        // rather than firing a burst of catch-up frames.
        nextSnapshotMillis_ += net::kSnapshotMillis;
        if (nextSnapshotMillis_ < nowMillis) nextSnapshotMillis_ = nowMillis + net::kSnapshotMillis;
        replicate(nowMillis);
        // One-shot events ride inside the snapshot here, where the reference
        // has its own per-tick outbox, so they are BANKED across the ticks that
        // send nothing rather than dropped: a hit that landed on a simulation
        // tick with no snapshot must still produce its number.
        events_.clear();
    }
    listener_.flush();
}

void GameServer::runSystems(double nowMillis, double dt) {
    // Order matters and is the tick's whole contract:
    //   bot intent -> players -> petals -> mob intent/movement -> projectiles
    //   -> combat -> spawning -> loot.
    // TypeScript closes the player movement window and runs the player's petal
    // pipeline before moveEnemies(), then advances projectiles after mobs.
    //
    // `dt` is the smoothed real step and drives the dt-SCALED half -- flowers,
    // petals, projectiles, fields. The mob half is a FIXED per-call step on
    // both sides (src/server.ts:1321 hands moveEnemies a hard 1/30), so it is
    // given net::kTickSeconds explicitly below rather than the tick's delta.

    // Bots decide before anything moves, which is where the reference samples
    // input: their decisions are made against the world as this tick found it
    // and are consumed by the very next stage, not one tick later.
    stepBots(nowMillis);

    // Modifiers before movement, as the reference schedules them (its
    // playerModifiers system sits in Phase.Input, ahead of playerMovement).
    // Folded only inside the petal phase below, a speed or size petal would not
    // reach movement until the tick after it was equipped.
    petals_->foldModifiers(world_, content());

    movement_->runPlayerPhase(world_, *terrain_, nowMillis, dt);
    petals_->run(world_, content(), nowMillis, dt, commands_, terrain_.get());

    // Mob targeting must see the flowers' newly committed positions. Refresh
    // both the LOD list and broadphase after player movement instead of asking
    // AI to make this tick's decision from last tick's coordinates.
    //
    // Two lists, because the reference draws the line in two different places.
    // The mob LOD counts EVERY flower as an observer, bots included -- a bot
    // fighting a mob is something worth simulating properly. The spawner
    // counts only real connections: a world that spawns a neighbourhood's
    // worth of mobs around each of two dozen bots fills up with mobs nobody
    // asked for, and bots keeping the unseen-despawn census fed would stop the
    // world ever recycling.
    activePlayers_.clear();
    Query<PlayerTag, Transform> players{world_};
    players.each([&](Entity, PlayerTag&, Transform& transform) {
        activePlayers_.push_back(transform.position);
    });
    humanPlayers_.clear();
    for (const auto& entry : sessions_) {
        if (!entry.second.playing()) continue;
        if (const Transform* transform = world_.tryGet<Transform>(entry.second.entity)) {
            humanPlayers_.push_back(transform->position);
        }
    }
    grid_.clear();
    Query<Transform, Body> afterPlayers{world_};
    afterPlayers.each([&](Entity e, Transform& transform, Body& body) {
        grid_.insert(e, transform.position, body.radius);
    });

    // The reference server resolves flower bodies and the petal ring inside
    // the player pipeline, before moveEnemies(). Keep that temporal boundary:
    // a mob cannot escape a petal it was already touching by moving first.
    combat_->beginTick(world_, nowMillis, dt, events_);
    combat_->runContactPhase(world_, grid_, content(), nowMillis);

    mobAi_->run(world_, *terrain_, grid_, activePlayers_, nowMillis, net::kTickSeconds, commands_);
    movement_->runWorldPhase(world_, *terrain_, nowMillis, net::kTickSeconds);

    // Combat exact-tests current transforms, but its candidate set comes from
    // this grid. Rebuild after mob/projectile flight so a cell crossing cannot
    // make a real overlap invisible for one tick.
    grid_.clear();
    Query<Transform, Body> afterMovement{world_};
    afterMovement.each([&](Entity e, Transform& transform, Body& body) {
        grid_.insert(e, transform.position, body.radius);
    });
    combat_->runWorldPhase(world_, grid_, content(), nowMillis, dt);
    spawning_->run(world_, *terrain_, content(), humanPlayers_, rng_, nowMillis, net::kTickSeconds,
                   commands_);
    loot_->run(world_, grid_, content(), rng_, nowMillis, dt, commands_, events_);

    // A pickup is a world event; owning it is an account fact. The loot system
    // deliberately knows nothing about the database, so the hand-off is here.
    bankPickups();
    // Same reasoning for kills: combat marks the corpse, the account keeps the
    // tally. Runs before the reaper, while MobType is still readable.
    bankKills();

    // The spawner has no view of the socket list, so it queues the bosses it
    // admitted rather than announcing them.
    announceBossSpawns();

    // The leaderboard reward tiers ride on the account component, refreshed on
    // a slow clock rather than resolved per kill.
    refreshRankMultipliers(nowMillis);
}

void GameServer::announceBossSpawns() {
    for (const SpawnSystem::BossSpawn& boss : spawning_->bossSpawns) {
        // Underscores read as spaces in the reference's wording, so
        // `soldier_ant` announces itself as "soldier ant".
        std::string name = content().mob(boss.mobIndex).id;
        for (char& c : name) {
            if (c == '_') c = ' ';
        }
        const int section = sectionAt(boss.position);
        const std::string tier = rarityLabel(boss.rarity);
        // Wrapped in the tier's own colour, as the reference server wraps it.
        // The client parses the markup; sending the announcement bare left it
        // the one boss line in the game with no tier colour on it.
        char colorAttribute[32];
        std::snprintf(colorAttribute, sizeof colorAttribute, "#%06x",
                      rarityColor(boss.rarity));

        for (auto& entry : sessions_) {
            Session& session = entry.second;
            if (!session.playing()) continue;
            net::Connection* connection = listener_.find(session.connection);
            if (connection == nullptr) continue;
            // Personalised: a player standing in the boss's own section is told
            // it spawned, everyone else that it spawned "somewhere".
            const Transform* transform = world_.tryGet<Transform>(session.entity);
            const bool here = transform != nullptr && sectionAt(transform->position) == section;
            sendChatTo(*connection, net::ChatChannel::System, "",
                       std::string("<b style=\"color: ") + colorAttribute + ";\">A " + tier +
                           " " + name + " has spawned" + (here ? "" : " somewhere") + "!</b>");
        }
    }
    spawning_->bossSpawns.clear();
}

void GameServer::refreshRankMultipliers(double nowMillis) {
    if (nowMillis < nextRankRefreshMillis_) return;
    nextRankRefreshMillis_ = nowMillis + kRankRefreshMillis;

    // The ranking is POSITIONAL, not a score threshold: the top ten accounts by
    // lifetime XP trade half their kill XP for a fifth again as many drops, and
    // the next ten trade a quarter for a tenth. Staff are off the board, as
    // they are off the leaderboard panel.
    struct Row {
        const std::string* userId;
        double totalXp;
    };
    std::vector<Row> rows;
    rows.reserve(database_.userCount());
    for (const std::string& username : database_.usernames()) {
        const Account* account = database_.findUser(username);
        if (account == nullptr || account->admin) continue;
        const PlayerRecord* record = database_.findProgress(account->id);
        rows.push_back({&account->id, record ? record->totalXp : 0.0});
    }
    constexpr std::size_t kTopDrop = 10;
    constexpr std::size_t kTopHalf = 20;
    const std::size_t ranked = std::min(kTopHalf, rows.size());
    std::partial_sort(rows.begin(), rows.begin() + static_cast<long>(ranked), rows.end(),
                      [](const Row& a, const Row& b) { return a.totalXp > b.totalXp; });

    Query<PlayerTag, PlayerAccount> accounts{world_};
    accounts.each([&](Entity, PlayerTag&, PlayerAccount& account) {
        // A guest -- and every bot -- ranks nowhere, which is the reference's
        // answer for an undefined user id.
        double xpMultiplier = 1.0;
        double dropMultiplier = 1.0;
        for (std::size_t i = 0; i < ranked; ++i) {
            if (*rows[i].userId != account.userId) continue;
            if (i < kTopDrop) { xpMultiplier = 0.5; dropMultiplier = 1.2; }
            else { xpMultiplier = 0.75; dropMultiplier = 1.1; }
            break;
        }
        account.xpMultiplier = xpMultiplier;
        account.dropMultiplier = dropMultiplier;
    });
}

void GameServer::bankPickups() {
    for (const LootSystem::Pickup& pickup : loot_->pickups()) {
        Session* session = sessionForEntity(pickup.player);
        if (!session || session->userId.empty()) continue;
        if (pickup.petalIndex >= content().petalCount()) continue;

        giveToInventory(database_.progress(session->userId), pickup.petalIndex, pickup.rarity, 1);
        database_.markDirty();

        if (net::Connection* connection = listener_.find(session->connection)) {
            sendProfile(*session, *connection);
        }
    }
}

void GameServer::reapDead(double nowMillis) {
    // Death is a component, not a destroy, so everything later in the SAME tick
    // still sees the entity -- a mob that dies during combat must still be
    // there for the loot system to read its contributor list. The actual
    // destroy happens here, once, at the end.
    Query<Dead> dead{world_};
    std::vector<Entity> doomed;
    dead.collect(doomed);
    for (const Entity e : doomed) {
        const bool isPlayer = world_.has<PlayerTag>(e);
        Session* session = isPlayer ? sessionForEntity(e) : nullptr;
        // A bot has no session, so the roster is what tells a bot's body apart
        // from a flower whose connection simply went away.
        Bot* bot = isPlayer && session == nullptr ? botForEntity(e) : nullptr;

        // A player's corpse KEEPS its Dead tag, which is what puts the dead
        // face and the dead state on the wire and what makes every system step
        // over the body. That means this loop meets the same corpse on every
        // later tick, so everything below has to happen exactly once.
        if (isPlayer) {
            if (session == nullptr && bot == nullptr) {
                // Nobody is watching through this body and nothing owns it. It
                // leaves no corpse and no death notice, and its ring goes with
                // it: a petal outliving its owner orbits a point in space
                // forever.
                if (const Loadout* loadout = world_.tryGet<Loadout>(e)) {
                    for (const Entity petal : loadout->spawned) commands_.destroy(petal);
                }
                commands_.destroy(e);
                continue;
            }
            if (session != nullptr ? session->deathReported : bot->deathAnnounced) continue;
        }

        if (world_.has<NetId>(e)) {
            const Transform* transform = world_.tryGet<Transform>(e);
            events_.killed(world_.get<NetId>(e).value, transform ? transform->position : Vec2{});
        }

        if (isPlayer) {
            if (session != nullptr) {
                session->deathReported = true;
                persistPlayer(*session);
                if (net::Connection* connection = listener_.find(session->connection)) {
                    ByteWriter w;
                    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Died));
                    w.str(killerLabel(world_, world_.get<Dead>(e).killer));
                    w.u32(0);
                    w.u32(tick_);
                    connection->send(w);
                }
            } else {
                // A bot leaves a corpse too, which is not tidiness: bots carry
                // yggdrasil for each other and actively path to each other's
                // bodies, and a body destroyed on the tick it died is one
                // nothing can ever revive. maintainBots owns the replacement
                // and takes this body away when it builds the new one.
                bot->deathAnnounced = true;
            }
            if (Health* health = world_.tryGet<Health>(e)) {
                health->current = 0;
            }
            // A corpse lies where it fell, at whatever angle it fell at. The
            // roll is the server's so every client sees the same body.
            if (Transform* transform = world_.tryGet<Transform>(e)) {
                transform->angle = rng_.angle();
            }
            continue;
        }
        commands_.destroy(e);
    }
    (void)nowMillis;
}

void GameServer::replicate(double nowMillis) {
    Replicator::Frame frame;
    frame.tick = tick_;
    frame.nowMillis = nowMillis;
    frame.events = &events_;

    // Reused across recipients rather than allocated per session: it is at
    // most three entities and almost always none.
    std::vector<Entity> squadBodies;
    for (auto& entry : sessions_) {
        Session& session = entry.second;
        if (!session.playing()) continue;
        net::Connection* connection = listener_.find(session.connection);
        if (!connection) continue;

        collectSquadBodies(session, squadBodies);
        frame.alwaysVisible = squadBodies.empty() ? nullptr : &squadBodies;

        scratch_.clear();
        replicator_.build(world_, session.entity, views_[session.connection], frame, scratch_);
        if (!scratch_.empty()) connection->send(scratch_);
    }
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

Session* GameServer::sessionFor(net::ConnectionId id) {
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : &it->second;
}

Session* GameServer::sessionForEntity(Entity e) {
    for (auto& entry : sessions_) {
        if (entry.second.entity == e) return &entry.second;
    }
    return nullptr;
}

void GameServer::onConnect(net::Connection& connection) {
    Session session;
    session.connection = connection.id();
    session.connectedAtMillis = monotonicMillis();
    session.lastHeardMillis = session.connectedAtMillis;
    sessions_[connection.id()] = session;
    views_[connection.id()] = ClientView{};
}

void GameServer::onDisconnect(net::Connection& connection, const std::string&) {
    if (Session* session = sessionFor(connection.id())) {
        // Before the body is destroyed, so the line the squad is told still
        // knows what this flower was called.
        departSquad(*session, nullptr, squadDisplayName(squadIdOf(*session)));
        if (session->playing()) {
            persistPlayer(*session);
            despawnPlayer(*session, false);
        }
    }
    revokeTempAdmin(connection.id());
    sessions_.erase(connection.id());
    views_.erase(connection.id());
}

void GameServer::onMessage(net::Connection& connection, ByteReader& reader) {
    Session* session = sessionFor(connection.id());
    if (!session) return;
    session->lastHeardMillis = monotonicMillis();

    const auto id = static_cast<net::ClientMessage>(reader.u8());

    // Nothing but the handshake is accepted until the protocol has been agreed.
    // A client that skips it cannot reach any game logic at all.
    if (session->stage == SessionStage::Greeting && id != net::ClientMessage::Hello) {
        connection.closeGracefully();
        return;
    }

    switch (id) {
        case net::ClientMessage::Hello:         handleHello(*session, connection, reader); break;
        case net::ClientMessage::Register:      handleRegister(*session, connection, reader); break;
        case net::ClientMessage::Login:         handleLogin(*session, connection, reader); break;
        case net::ClientMessage::ResumeSession: handleResume(*session, connection, reader); break;
        case net::ClientMessage::JoinGame:      handleJoin(*session, connection, reader); break;
        case net::ClientMessage::LeaveGame:     handleLeave(*session, connection); break;
        case net::ClientMessage::Input:         handleInput(*session, reader); break;
        case net::ClientMessage::Chat:          handleChat(*session, connection, reader); break;
        case net::ClientMessage::SetLoadout:    handleSetLoadout(*session, reader); break;
        case net::ClientMessage::SwapLoadout:   handleSwapLoadout(*session, reader); break;
        case net::ClientMessage::Craft:         handleCraft(*session, connection, reader); break;
        case net::ClientMessage::Respawn:       handleRespawn(*session); break;
        case net::ClientMessage::Ping:          handlePing(connection, reader); break;
        case net::ClientMessage::UpgradeSkill:  handleUpgradeSkill(*session, connection, reader); break;
        case net::ClientMessage::ResetSkills:   handleResetSkills(*session, connection); break;
        case net::ClientMessage::BuyPetal:      handleBuyPetal(*session, connection, reader); break;
        case net::ClientMessage::RedeemCode:    handleRedeemCode(*session, connection, reader); break;
        case net::ClientMessage::SetSkin:       handleSetSkin(*session, connection, reader); break;
        case net::ClientMessage::RequestLeaderboard: handleLeaderboard(*session, connection); break;
        case net::ClientMessage::RequestNotifications: handleNotifications(connection, reader); break;
        case net::ClientMessage::GuildCreate:   handleGuildCreate(*session, connection, reader); break;
        case net::ClientMessage::GuildInvite:   handleGuildInvite(*session, connection, reader); break;
        case net::ClientMessage::GuildAccept:   handleGuildAccept(*session, connection); break;
        case net::ClientMessage::GuildDecline:  handleGuildDecline(*session, connection); break;
        case net::ClientMessage::GuildKick:     handleGuildKick(*session, connection, reader); break;
        case net::ClientMessage::GuildLeave:    handleGuildLeave(*session, connection); break;
        case net::ClientMessage::GuildSquadAll: handleGuildSquadAll(*session, connection); break;
        case net::ClientMessage::GuildInviteToSquad:
            handleGuildInviteToSquad(*session, connection, reader);
            break;
        case net::ClientMessage::PublishSkin:   handlePublishSkin(*session, connection, reader); break;
        case net::ClientMessage::EquipSkin:     handleEquipSkin(*session, connection, reader); break;
        case net::ClientMessage::DeleteSkin:    handleDeleteSkin(*session, connection, reader); break;
        case net::ClientMessage::Logout:
            // A body in the world belongs to the account that is going away,
            // so it comes off first -- and with its progress saved, because a
            // logout is a deliberate exit, not a drop. Left alone it would be
            // an orphan: a flower nobody can steer and no account can persist.
            if (session->playing()) {
                persistPlayer(*session);
                despawnPlayer(*session, false);
            }
            revokeTempAdmin(session->connection);
            if (!session->token.empty()) database_.revokeSession(session->token);
            session->token.clear();
            session->userId.clear();
            // Both of these gate later messages -- `admin` unlocks the console
            // commands, `username` is who the chat and the guild take this
            // connection for -- so an anonymous session must not keep either.
            session->username.clear();
            session->admin = false;
            session->displayName.clear();
            // Last: despawnPlayer() puts the stage back to Authenticated, and
            // this is what the socket actually is now.
            session->stage = SessionStage::Anonymous;
            break;
        default:
            break;
    }
}

void GameServer::handleHello(Session& session, net::Connection& connection, ByteReader& reader) {
    const std::uint16_t version = reader.u16();
    const std::uint32_t clientContent = reader.u32();
    if (!reader.ok()) { connection.closeGracefully(); return; }

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Welcome));
    w.u16(net::kProtocolVersion);

    if (version != net::kProtocolVersion) {
        w.boolean(false);
        w.str("This client speaks protocol " + std::to_string(version) + "; the server speaks " +
              std::to_string(net::kProtocolVersion) + ". Please update.");
        connection.send(w);
        connection.closeGracefully();
        return;
    }
    if (clientContent != content().contentHash()) {
        // Mob and petal stats are read from JSON by both sides. If the two read
        // different files, every number the client shows is quietly wrong --
        // far better to say so at connect time.
        w.boolean(false);
        w.str("Your game content does not match the server's. Please update.");
        connection.send(w);
        connection.closeGracefully();
        return;
    }

    w.boolean(true);
    w.str("");
    connection.send(w);
    session.stage = SessionStage::Anonymous;
}

void GameServer::sendAuthResult(net::Connection& connection, net::AuthStatus status,
                                const std::string& token, const std::string& username,
                                const std::string& reason) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::AuthResult));
    w.u8(static_cast<std::uint8_t>(status));
    w.str(token);
    w.str(username);
    w.str(reason);
    connection.send(w);
}

void GameServer::handleRegister(Session& session, net::Connection& connection, ByteReader& reader) {
    const std::string username = reader.str();
    const std::string password = reader.str();
    if (!reader.ok()) return;

    if (!spend(session.loginAttemptsAllowed)) {
        sendAuthResult(connection, net::AuthStatus::RateLimited, "", "", "Too many attempts. Wait a moment.");
        return;
    }

    std::string reason;
    if (!validUsername(username, reason)) {
        sendAuthResult(connection, net::AuthStatus::UsernameInvalid, "", "", reason);
        return;
    }
    if (!validPassword(password, reason)) {
        sendAuthResult(connection, net::AuthStatus::PasswordInvalid, "", "", reason);
        return;
    }

    // Below the format checks, so mistyping a password does not cost one of the
    // three accounts an address gets: those checks are pure string work and are
    // already bounded by the per-session budget above. Above the bcrypt hash and
    // the database write, which are the work an unlimited registration endpoint
    // hands an attacker.
    //
    // That session budget bounds one SOCKET; it does not bound one client, which
    // can hang up and dial again for a fresh allowance. These limits are keyed
    // on the address instead, and outlive any one connection.
    const std::string address = addressKey(connection.peer());
    const std::string addressHash = database_.accountAddressHash(address);
    const LimitVerdict verdict = accountLimits_.spendRegistration(
        address,
        [&] { return database_.countAccountsCreatedBy(addressHash, kRegisterDailyWindowMillis); },
        monotonicMillis());
    if (!verdict.allowed) {
        const std::string line =
            accountLimits_.refusalLogLine(address, verdict.scope, monotonicMillis());
        if (!line.empty()) std::printf("%s\n", line.c_str());
        sendAuthResult(connection, net::AuthStatus::RateLimited, "", "", verdict.message);
        return;
    }

    const CreateResult result = database_.createUser(username, password, addressHash);
    if (!result.ok() || !result.account) {
        sendAuthResult(connection, net::AuthStatus::UsernameTaken, "", "", result.reason);
        return;
    }

    session.userId = result.account->id;
    session.username = result.account->username;
    session.token = database_.createSession(session.userId, session.username);

    grantStarterKit(database_.progress(session.userId));
    database_.markDirty();
    session.stage = SessionStage::Authenticated;
    sendAuthResult(connection, net::AuthStatus::Ok, session.token, session.username, "");
    sendDailyStreak(session, connection);
    sendProfile(session, connection);
    sendSkinCatalog(session, connection);
    sendGuildState(session, connection);
}

void GameServer::handleLogin(Session& session, net::Connection& connection, ByteReader& reader) {
    const std::string username = reader.str();
    const std::string password = reader.str();
    if (!reader.ok()) return;

    if (!spend(session.loginAttemptsAllowed)) {
        sendAuthResult(connection, net::AuthStatus::RateLimited, "", "", "Too many attempts. Wait a moment.");
        return;
    }
    // Same reasoning as handleRegister: a reconnect resets the session budget,
    // and every attempt costs this server a bcrypt verify.
    const std::string address = addressKey(connection.peer());
    const LimitVerdict verdict = accountLimits_.spendLoginAttempt(address, monotonicMillis());
    if (!verdict.allowed) {
        sendAuthResult(connection, net::AuthStatus::RateLimited, "", "", verdict.message);
        return;
    }

    if (!database_.verifyPassword(username, password)) {
        // Deliberately the same answer for a wrong password and an unknown
        // account: distinguishing them tells an attacker which names exist.
        sendAuthResult(connection, net::AuthStatus::BadCredentials, "", "",
                       "Invalid username or password");
        return;
    }

    const Account* account = database_.findUser(username);
    if (!account) {
        sendAuthResult(connection, net::AuthStatus::ServerError, "", "", "Account could not be read.");
        return;
    }
    // A player who signed in is not what the limit is for.
    accountLimits_.refundLoginAttempt(address);

    session.userId = account->id;
    session.username = account->username;
    session.admin = account->admin;
    session.token = database_.createSession(account->id, account->username);
    session.stage = SessionStage::Authenticated;
    sendAuthResult(connection, net::AuthStatus::Ok, session.token, account->username, "");
    sendDailyStreak(session, connection);
    sendProfile(session, connection);
    sendSkinCatalog(session, connection);
    sendGuildState(session, connection);
}

void GameServer::handleResume(Session& session, net::Connection& connection, ByteReader& reader) {
    const std::string token = reader.str();
    if (!reader.ok()) return;

    const Database::Session* record = database_.resolveSession(token);
    if (!record) {
        sendAuthResult(connection, net::AuthStatus::SessionExpired, "", "", "Please log in again.");
        return;
    }

    session.userId = record->userId;
    session.username = record->username;
    session.token = token;
    session.stage = SessionStage::Authenticated;
    if (const Account* account = database_.findUser(record->username)) session.admin = account->admin;
    sendAuthResult(connection, net::AuthStatus::Ok, token, record->username, "");
    sendDailyStreak(session, connection);
    sendProfile(session, connection);
    sendSkinCatalog(session, connection);
    sendGuildState(session, connection);
}

void GameServer::sendProfile(Session& session, net::Connection& connection) {
    const PlayerRecord& record = database_.progress(session.userId);

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Profile));
    w.str(session.username);
    w.f64(record.totalXp);
    w.u16(static_cast<std::uint16_t>(levelFromTotalXp(record.totalXp).level));
    w.u32(static_cast<std::uint32_t>(std::max(0, record.stars)));

    // The inventory is a sparse dictionary of rarity -> item name -> count.
    // Anything whose name this build does not know (an item from a newer
    // server, or a non-petal) is skipped for the wire but left untouched in
    // the record, so it survives the next save.
    const std::size_t stackCountAt = w.reserveU16();
    std::uint16_t stackCount = 0;
    for (const std::string& rarityName : record.inventory.keys()) {
        const Rarity rarity = parseRarity(rarityName);
        const Json& byType = record.inventory[rarityName];
        for (const std::string& key : byType.keys()) {
            const std::uint32_t count = static_cast<std::uint32_t>(std::max(0, byType[key].asInt()));
            if (count == 0) continue;
            const std::uint16_t index = petalIndexFromInventoryKey(key);
            if (index == kInvalidIndex) continue;
            w.u16(index);
            w.u8(static_cast<std::uint8_t>(rarity));
            w.u32(count);
            ++stackCount;
        }
    }
    w.patchU16(stackCountAt, stackCount);

    w.u8(static_cast<std::uint8_t>(kLoadoutSlots));
    for (std::size_t i = 0; i < kLoadoutSlots; ++i) {
        std::uint16_t index = kNoPetal;
        Rarity rarity = Rarity::Common;
        if (i < record.loadout.size() && record.loadout[i].has_value()) {
            const StoredItem& item = *record.loadout[i];
            index = content().petalIndex(item.petalType);
            if (index == kInvalidIndex) index = kNoPetal;
            rarity = item.rarity;
        }
        w.u16(index);
        w.u8(static_cast<std::uint8_t>(rarity));
    }

    w.u32(record.renderFlags);

    // The talent tree. Only branches that have been bought are sent; the
    // balance is derived on both sides from the level, so it is not on the
    // wire at all and cannot arrive disagreeing with the tiers beside it.
    const std::size_t skillCountAt = w.reserveU16();
    std::uint16_t skillCount = 0;
    for (int i = 0; i < kSkillCount; ++i) {
        const int tier = record.skills.tier[static_cast<std::size_t>(i)];
        if (tier < 0) continue;
        w.u8(static_cast<std::uint8_t>(i));
        w.u8(static_cast<std::uint8_t>(tier));
        ++skillCount;
    }
    w.patchU16(skillCountAt, skillCount);

    // The mob-kill ledger, for the gallery. Mob ids this build does not know
    // are skipped for the wire and left in the record, exactly as the
    // inventory is: a gallery cell is worth less than an account's history.
    const std::size_t killCountAt = w.reserveU16();
    std::uint16_t killCount = 0;
    for (const std::string& mobId : record.mobKills.keys()) {
        const std::uint16_t index = content().mobIndex(mobId);
        if (index == kInvalidIndex) continue;
        const Json& byTier = record.mobKills[mobId];
        if (!byTier.isObject()) continue;
        for (const std::string& tier : byTier.keys()) {
            const std::uint32_t count = static_cast<std::uint32_t>(std::max(0, byTier[tier].asInt()));
            if (count == 0) continue;
            w.u16(index);
            w.u8(static_cast<std::uint8_t>(parseRarity(tier)));
            w.u32(count);
            ++killCount;
        }
    }
    w.patchU16(killCountAt, killCount);

    connection.send(w);
}

void GameServer::sendDailyStreak(Session& session, net::Connection& connection) {
    const DailyStreakResult streak = database_.processDailyStreak(session.userId);

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::DailyStreak));
    w.u16(static_cast<std::uint16_t>(std::max(0, streak.streak)));
    w.boolean(streak.newDay);
    w.u16(static_cast<std::uint16_t>(std::max(0, streak.starsAwarded)));
    w.i64(streak.nextClaimAtMillis);
    w.i64(streak.streakExpiresAtMillis);
    connection.send(w);
}

void GameServer::sendNotice(net::Connection& connection, net::NoticeSeverity severity,
                            const std::string& text) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Notice));
    w.u8(static_cast<std::uint8_t>(severity));
    w.str(text);
    connection.send(w);
}

void GameServer::sendShopResult(net::Connection& connection, net::ShopResultKind kind, bool ok,
                                int stars, const std::string& message) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::ShopResult));
    w.u8(static_cast<std::uint8_t>(kind));
    w.boolean(ok);
    w.u32(static_cast<std::uint32_t>(std::max(0, stars)));
    w.str(message);
    connection.send(w);
}

void GameServer::broadcastChat(net::ChatChannel channel, const std::string& author,
                               const std::string& text) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Chat));
    w.u8(static_cast<std::uint8_t>(channel));
    w.str(author);
    w.str(text);
    listener_.each([&](net::Connection& connection) {
        const Session* session = sessionFor(connection.id());
        if (session && session->authenticated()) connection.send(w);
    });
}

void GameServer::handleJoin(Session& session, net::Connection& connection, ByteReader& reader) {
    const double width = reader.u16();
    const double height = reader.u16();
    const std::string biome = reader.str();
    const std::string name = reader.str();
    if (!reader.ok()) return;
    if (!session.authenticated()) return;
    if (session.playing()) return;

    // The nameplate, not the account. Kept on the session so a respawn keeps
    // the name the player typed rather than reverting to their login.
    session.displayName = sanitizePlayerName(name);

    // Remembered on the session so a respawn returns to the biome the player
    // chose, rather than quietly sending them back to the beginner ground.
    // A biome the map has no safe area for is dropped here, once, with a
    // notice -- rather than silently every time they die.
    session.spawnBiome.clear();
    if (!biome.empty() && biome != "default") {
        Vec2 probe;
        if (mapData_.spawnInBiome(biome, rng_, *terrain_, probe)) {
            session.spawnBiome = biome;
        } else {
            sendNotice(connection, net::NoticeSeverity::Warning,
                       "No safe ground in that biome; starting in the garden.");
        }
    }

    if (playerCount() >= config_.maxPlayers) {
        sendNotice(connection, net::NoticeSeverity::Bad, "The server is full.");
        return;
    }

    const Entity entity = spawnPlayer(session);
    if (entity == NULL_ENTITY) {
        sendNotice(connection, net::NoticeSeverity::Bad, "Could not find a spawn point.");
        return;
    }

    PlayerLocation& location = world_.get<PlayerLocation>(entity);
    location.viewport = {clamp(width, 320.0, kMaxViewportAxis),
                         clamp(height, 240.0, kMaxViewportAxis)};

    views_[session.connection].reset();

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::JoinAccepted));
    w.u32(world_.get<NetId>(entity).value);
    w.position(world_.get<Transform>(entity).position);
    w.u32(tick_);
    // This is the exact decoded TypeScript wall grid. Forty kilobytes once per
    // join is comfortably below the frame cap and cannot drift from collision.
    w.u16(static_cast<std::uint16_t>(terrain_->tileCount()));
    w.raw(terrain_->tiles(), terrain_->tileCount());
    connection.send(w);

    broadcastChat(net::ChatChannel::System, "", session.username + " joined");
}

void GameServer::handleLeave(Session& session, net::Connection& connection) {
    if (!session.playing()) return;
    revokeTempAdmin(session.connection);
    persistPlayer(session);
    despawnPlayer(session, true);
    sendProfile(session, connection);
}

void GameServer::handleInput(Session& session, ByteReader& reader) {
    const net::InputFrame input = net::InputFrame::read(reader);
    if (!reader.ok() || !session.playing()) return;
    if (!spend(session.inputAllowance)) return;

    // A replayed or reordered input must not move the player twice. TCP gives
    // ordering, but a client is free to send its own duplicates.
    if (input.sequence <= session.lastInputSequence) return;
    session.lastInputSequence = input.sequence;

    if (PlayerInput* state = world_.tryGet<PlayerInput>(session.entity)) {
        state->current = input;
        state->aimDirection = Vec2::fromAngle(input.aimAngle);
    }

    // The window the client is drawing rides every input packet, so a resize or
    // a zoom widens what is replicated on the next tick rather than at the next
    // join. Zero means "unchanged", which is what a client that never learned
    // to report it sends.
    if (input.viewportWidth > 0 && input.viewportHeight > 0) {
        if (PlayerLocation* location = world_.tryGet<PlayerLocation>(session.entity)) {
            location->viewport = {clamp(static_cast<double>(input.viewportWidth), 320.0, kMaxViewportAxis),
                                  clamp(static_cast<double>(input.viewportHeight), 240.0, kMaxViewportAxis)};
        }
    }
}

namespace {

/// True when a chat line names a raid tier as a bare word.
///
/// The reference tests `/\b(super|unique)\b/i`, and the word boundary is the
/// whole point: "supercell" is not a raid call, and neither is a name that
/// happens to contain the letters. Ultra is deliberately absent -- bots treat
/// an ultra as a mob to fight, not as a rally point.
bool mentionsRaidTier(const std::string& text) {
    static const char* const kWords[] = {"super", "unique"};
    const auto wordChar = [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_';
    };
    for (const char* word : kWords) {
        const std::size_t length = std::char_traits<char>::length(word);
        for (std::size_t at = 0; at + length <= text.size(); ++at) {
            bool match = true;
            for (std::size_t i = 0; i < length && match; ++i) {
                match = std::tolower(static_cast<unsigned char>(text[at + i])) == word[i];
            }
            if (!match) continue;
            if (at > 0 && wordChar(static_cast<unsigned char>(text[at - 1]))) continue;
            if (at + length < text.size() &&
                wordChar(static_cast<unsigned char>(text[at + length]))) {
                continue;
            }
            return true;
        }
    }
    return false;
}

} // namespace

void GameServer::handleChat(Session& session, net::Connection& connection, ByteReader& reader) {
    const std::string raw = reader.str();
    if (!reader.ok() || !session.authenticated()) return;
    const std::string text = sanitizeChat(raw);
    if (text.empty()) return;

    // A leading slash is a command, and a command is answered rather than
    // said. This has to come BEFORE the mute check: a muted player is barred
    // from talking to other players, not from asking the server questions
    // about their own account.
    //
    // It also spends a DIFFERENT budget. The chat allowance exists to stop one
    // player flooding everyone else, and a command's output goes back to its
    // sender alone -- billed to the chat bucket, four commands emptied it and
    // the console became one command every two seconds.
    if (!text.empty() && text[0] == '/') {
        if (!spend(session.commandAllowance)) {
            sendNotice(connection, net::NoticeSeverity::Warning,
                       "You are sending commands too quickly.");
            return;
        }
        if (handleChatCommand(session, connection, text)) return;
    }

    if (!spend(session.chatAllowance)) {
        sendNotice(connection, net::NoticeSeverity::Warning, "You are sending messages too quickly.");
        return;
    }

    // Everything from here is broadcast to other players, which is exactly
    // what a mute blocks.
    const Account* account = database_.findUser(session.username);
    if (account != nullptr && account->muted) {
        sendSystem(connection, "<span style=\"color: #ff8866;\">You are muted and cannot "
                               "send chat messages.</span>");
        return;
    }

    broadcastChat(net::ChatChannel::Global, session.username, text);

    // Somebody saying "super" or "unique" rallies every bot onto the best boss
    // in the world, exactly as the reference's chat handler does. Only those
    // two words: an ultra is a high-tier mob to fight, never a raid to call.
    // No-ops when no qualifying boss exists.
    if (mentionsRaidTier(text)) triggerBotRaid(clockMillis_);
}

void GameServer::handleSetLoadout(Session& session, ByteReader& reader) {
    const std::uint8_t slot = reader.u8();
    const std::uint16_t petalIndex = reader.u16();
    const Rarity rarity = clampRarity(reader.u8());
    if (!reader.ok() || !session.authenticated()) return;
    if (slot >= kLoadoutSlots) return;
    if (petalIndex != kNoPetal && petalIndex >= content().petalCount()) return;

    PlayerRecord& record = database_.progress(session.userId);
    if (record.loadout.size() < kLoadoutSlots) record.loadout.resize(kLoadoutSlots);

    // Equipping must come out of the inventory, or a client can name any petal
    // it likes and simply be given it.
    if (petalIndex != kNoPetal && !takeFromInventory(record, petalIndex, rarity, 1)) return;

    // Whatever was in the slot goes back to the inventory. Doing this after the
    // take, not before, means a failed take cannot also have emptied the slot.
    if (record.loadout[slot].has_value()) {
        const StoredItem& previous = *record.loadout[slot];
        const std::uint16_t previousIndex = content().petalIndex(previous.petalType);
        if (previousIndex != kInvalidIndex) {
            giveToInventory(record, previousIndex, previous.rarity, 1);
        }
    }

    if (petalIndex == kNoPetal) {
        record.loadout[slot].reset();
    } else {
        StoredItem item;
        item.type = "petal";
        item.petalType = content().petal(petalIndex).id;
        item.rarity = rarity;
        record.loadout[slot] = item;
    }
    database_.markDirty();

    if (session.playing()) applyAccountToEntity(record, session.entity);
    if (net::Connection* connection = listener_.find(session.connection)) {
        sendProfile(session, *connection);
    }
}

void GameServer::handleSwapLoadout(Session& session, ByteReader& reader) {
    const std::uint8_t a = reader.u8();
    const std::uint8_t b = reader.u8();
    if (!reader.ok() || !session.authenticated()) return;
    if (a >= kLoadoutSlots || b >= kLoadoutSlots || a == b) return;

    PlayerRecord& record = database_.progress(session.userId);
    if (record.loadout.size() < kLoadoutSlots) record.loadout.resize(kLoadoutSlots);
    std::swap(record.loadout[a], record.loadout[b]);
    database_.markDirty();

    if (session.playing()) applyAccountToEntity(record, session.entity);
    if (net::Connection* connection = listener_.find(session.connection)) {
        sendProfile(session, *connection);
    }
}

void GameServer::handleCraft(Session& session, net::Connection& connection, ByteReader& reader) {
    const std::uint16_t petalIndex = reader.u16();
    const Rarity rarity = clampRarity(reader.u8());
    const int count = reader.u16();
    if (!reader.ok() || !session.authenticated()) return;

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::CraftResult));

    PlayerRecord& record = database_.progress(session.userId);
    const bool valid = count >= kCraftBatch && rarity != Rarity::Apex &&
                       petalIndex < content().petalCount();
    if (!valid || !takeFromInventory(record, petalIndex, rarity, count)) {
        w.boolean(false);
        w.u16(petalIndex);
        w.u8(static_cast<std::uint8_t>(rarity));
        w.u16(0);
        w.u8(0);
        w.str("Not enough petals to craft.");
        connection.send(w);
        return;
    }

    // The whole request is crafted as one POOL rather than as one batch per
    // click: every attempt eats five, and the survivors a failure hands back
    // drop straight into the pool to be tried again, until fewer than five are
    // left. That is what makes the returns actually get crafted instead of
    // piling up in the inventory, and it is why the client plays one spin for
    // a staged xN. Bounded: an attempt removes five and returns at most four,
    // so the pool strictly shrinks.
    // Every equipped clover of the tier being crafted nudges the odds up. The
    // reference counts the PRIMARY ten slots only -- the storage row behind
    // them is not equipped and does not help -- and the bonus is stated in
    // percentage points against a 0..100 roll, which is five ten-thousandths
    // of the fraction this ladder is expressed in.
    constexpr double kCloverCraftBonus = 0.0005;
    const std::uint16_t clover = content().petalIndex("clover");
    int clovers = 0;
    if (clover != kInvalidIndex) {
        const std::size_t equipped =
            std::min<std::size_t>(record.loadout.size(), kLoadoutActiveSlots);
        for (std::size_t i = 0; i < equipped; ++i) {
            if (!record.loadout[i].has_value()) continue;
            const StoredItem& slot = *record.loadout[i];
            if (slot.rarity != rarity) continue;
            if (content().petalIndex(slot.petalType) == clover) ++clovers;
        }
    }
    const double chance =
        std::min(1.0, craftSuccessChance(rarity) + kCloverCraftBonus * clovers);
    int pool = count;
    int crafted = 0;
    while (pool >= kCraftBatch) {
        pool -= kCraftBatch;
        if (rng_.chance(chance)) ++crafted;
        else pool += kCraftBatch - static_cast<int>(1 + rng_.below(kCraftBatch - 1));
    }
    const int petalsReturned = pool;   // the sub-batch tail, 0..4

    if (petalsReturned > 0) giveToInventory(record, petalIndex, rarity, petalsReturned);
    if (crafted > 0) giveToInventory(record, petalIndex, upgradeRarity(rarity), crafted);
    database_.markDirty();

    w.boolean(crafted > 0);
    w.u16(petalIndex);
    w.u8(static_cast<std::uint8_t>(crafted > 0 ? upgradeRarity(rarity) : rarity));
    w.u16(static_cast<std::uint16_t>(crafted));
    w.u8(static_cast<std::uint8_t>(petalsReturned));
    w.str(crafted > 0 ? "" : "The craft failed.");
    connection.send(w);
    sendProfile(session, connection);
}

void GameServer::bankKills() {
    std::vector<Bounty::Share> ranked;
    for (const CombatSystem::DeathRecord& death : combat_->deaths()) {
        if (death.wasPlayer) continue;
        const MobType* type = world_.tryGet<MobType>(death.entity);
        if (type == nullptr) continue;

        // A credited killing blow is the gate, exactly as it is in the
        // reference: a mob finished by another mob is nobody's kill and enters
        // nobody's gallery. A pet's kill belongs to the player who summoned it,
        // and combat has already resolved that attribution onto Dead::killer.
        if (death.killer == NULL_ENTITY || !world_.isAlive(death.killer) ||
            !world_.has<PlayerTag>(death.killer)) {
            continue;
        }

        // The gallery entry and the star bounty go to every player who earned
        // LOOT rights on the corpse, not to the finisher alone: five flowers
        // that bring down an apex are five apex kills and five lots of 250
        // stars. Same ledger, same ranking and same per-tier slot cap the XP
        // and drop paths already use, so the three cannot disagree about who
        // was in on a kill.
        ranked.clear();
        if (const Bounty* bounty = world_.tryGet<Bounty>(death.entity)) {
            for (const Bounty::Share& share : bounty->contributors) {
                if (share.damage <= 0.0) continue;
                if (world_.isAlive(share.player) && !world_.has<PlayerTag>(share.player)) continue;
                ranked.push_back(share);
            }
        }
        // Stable, because the ledger is in first-hit order and the reference's
        // sort is specified stable: on an exact damage tie the slot at the cut
        // belongs to whoever landed their damage first.
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const Bounty::Share& a, const Bounty::Share& b) {
                             return a.damage > b.damage;
                         });
        int slots = 4;
        if (type->rarity == Rarity::Ultra) slots = 15;
        else if (type->rarity == Rarity::Super) slots = 20;
        else if (type->rarity == Rarity::Unique || type->rarity == Rarity::Apex) slots = 25;
        if (static_cast<int>(ranked.size()) > slots) {
            ranked.resize(static_cast<std::size_t>(slots));
        }

        const std::string mobId = content().mob(type->configIndex).id;
        const int stars = starsForKill(type->rarity);

        for (const Bounty::Share& share : ranked) {
            // A contributor who has left still holds their slot -- nobody is
            // promoted into the gap -- but there is no account left to pay.
            Session* session = sessionForEntity(share.player);
            if (session == nullptr || session->userId.empty()) continue;

            PlayerRecord& record = database_.progress(session->userId);
            record.recordKill(mobId, type->rarity);

            // Stars are the mythic-and-above bounty. Awarded on the live entity
            // rather than the record so the HUD sees them this tick;
            // persistPlayer copies them back the same way it does XP.
            // Silently: the reference pays the bounty and updates the star
            // counter, and says nothing. A notice per mythic kill is a line
            // every few seconds in a mythic zone.
            if (stars > 0) {
                if (PlayerProgress* live = world_.tryGet<PlayerProgress>(share.player)) {
                    live->stars += stars;
                    record.stars = live->stars;
                } else {
                    record.stars += stars;
                }
            }
            database_.markDirty();

            if (net::Connection* connection = listener_.find(session->connection)) {
                sendProfile(*session, *connection);
            }
        }

        announceBossDefeat(*type, ranked);
    }
}

void GameServer::announceBossDefeat(const MobType& type,
                                    const std::vector<Bounty::Share>& ranked) {
    // The three announced tiers are the three the spawn line announces. An
    // ultra dies as quietly as it spawned.
    if (type.rarity != Rarity::Super && type.rarity != Rarity::Unique &&
        type.rarity != Rarity::Apex) {
        return;
    }
    // Credited to the top damage dealer alone, whatever the kill was shared
    // with: `ranked` is already sorted by damage, so that is its first row.
    if (ranked.empty()) return;
    const Session* session = sessionForEntity(ranked.front().player);
    if (session == nullptr || !session->authenticated()) return;

    std::string name = content().mob(type.configIndex).id;
    for (char& c : name) {
        if (c == '_') c = ' ';
    }
    char colorAttribute[32];
    std::snprintf(colorAttribute, sizeof colorAttribute, "#%06x", rarityColor(type.rarity));

    // The flower's name, not the account's, in the brackets -- the account is
    // the @handle before them.
    const std::string playerName =
        session->displayName.empty() ? session->username : session->displayName;
    broadcastChat(net::ChatChannel::System, "",
                  std::string("<b style=\"color: ") + colorAttribute + ";\">A " +
                      rarityLabel(type.rarity) + " " + name +
                      " has been defeated by <span style=\"color: #00ff00;\">@" +
                      session->username + "</span> [<span style=\"color: yellow;\">" +
                      playerName + "</span>]</b>");
}

void GameServer::handleUpgradeSkill(Session& session, net::Connection& connection,
                                    ByteReader& reader) {
    const std::uint8_t rawSkill = reader.u8();
    const int tier = reader.u8();
    if (!reader.ok() || !session.authenticated()) return;
    if (rawSkill >= kSkillCount) return;

    const SkillId id = static_cast<SkillId>(rawSkill);
    PlayerRecord& record = database_.progress(session.userId);

    // Tiers are bought one at a time, in order. Accepting an arbitrary target
    // would let a client skip the tiers below it and pay for one of them.
    if (tier < 0 || tier >= skillTierCount(id) || tier != record.skills.level(id) + 1) {
        sendNotice(connection, net::NoticeSeverity::Warning, "Talents are bought in order.");
        return;
    }
    if (id == SkillId::SecondChance && !record.skills.secondChanceUnlocked()) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   std::string("Second Chance needs ") + rarityLabel(kSecondChanceRequirement) +
                       " " + kSkillLabels[static_cast<std::size_t>(kSecondChanceParent)] + ".");
        return;
    }

    const int cost = kTierCost[static_cast<std::size_t>(tier)];
    if (record.talentPoints() < cost) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "Not enough talent points (need " + std::to_string(cost) + ").");
        return;
    }

    record.skills.set(id, tier);
    database_.markDirty();
    if (session.playing() && world_.isAlive(session.entity)) {
        applyAccountToEntity(record, session.entity);
    }
    sendProfile(session, connection);
}

void GameServer::handleResetSkills(Session& session, net::Connection& connection) {
    if (!session.authenticated()) return;
    PlayerRecord& record = database_.progress(session.userId);
    record.skills.clear();
    database_.markDirty();
    if (session.playing() && world_.isAlive(session.entity)) {
        applyAccountToEntity(record, session.entity);
    }
    sendProfile(session, connection);
    sendNotice(connection, net::NoticeSeverity::Info, "Talents reset; every point refunded.");
}

void GameServer::handleBuyPetal(Session& session, net::Connection& connection, ByteReader& reader) {
    const std::uint16_t petalIndex = reader.u16();
    const Rarity rarity = clampRarity(reader.u8());
    const std::uint8_t offerSlot = reader.u8();
    if (!reader.ok() || !session.authenticated()) return;

    // The client sends what it wants, never what it costs. A price that came
    // off the wire is a price the client chose.
    if (!shopSellsPetal(petalIndex) || !shopSellsRarity(rarity)) {
        sendShopResult(connection, net::ShopResultKind::Purchase, false, 0, "That is not for sale.");
        return;
    }

    PlayerRecord& record = database_.progress(session.userId);
    // A card off the rotating store costs what that card showed, discount
    // included; anything else costs the full ladder price. The slot is only a
    // claim about WHICH card was clicked -- the offers are regenerated here,
    // and a slot whose petal and tier do not match the current rotation's is a
    // click on a store that has since changed.
    double price = shopPrice(petalIndex, rarity);
    if (offerSlot != net::kNoShopOffer) {
        const std::vector<ShopOffer> offers = shopOffers(shopRotation(shopClockNow()));
        const auto slot = static_cast<std::size_t>(offerSlot);
        if (slot >= offers.size() || offers[slot].petalIndex != petalIndex ||
            offers[slot].rarity != rarity) {
            sendShopResult(connection, net::ShopResultKind::Purchase, false, 0,
                           "That offer has expired.");
            return;
        }
        price = offers[slot].price;
    }
    PlayerProgress* live = session.playing() && world_.isAlive(session.entity)
                               ? world_.tryGet<PlayerProgress>(session.entity)
                               : nullptr;
    const int stars = live ? live->stars : record.stars;
    if (static_cast<double>(stars) < price) {
        sendShopResult(connection, net::ShopResultKind::Purchase, false, 0, "Not enough stars.");
        return;
    }

    const int spent = static_cast<int>(price);
    record.stars = stars - spent;
    if (live) live->stars = record.stars;
    giveToInventory(record, petalIndex, rarity, 1);
    database_.markDirty();
    sendProfile(session, connection);
    sendShopResult(connection, net::ShopResultKind::Purchase, true, 0, {});
}

/// Redeems a star code.
///
/// The codes live in the database's own `codes` table -- the one the browser
/// build's admin commands write and this build round-trips -- so a code minted
/// against that file works here without a second registry to keep in step.
void GameServer::handleRedeemCode(Session& session, net::Connection& connection,
                                  ByteReader& reader) {
    const std::string typed = reader.str();
    if (!reader.ok() || !session.authenticated()) return;

    // Trimmed and upper-cased before the lookup, exactly as the browser build
    // normalises it: codes are printed in capitals and pasted with whitespace.
    std::string code = typed;
    code.erase(code.begin(),
               std::find_if(code.begin(), code.end(),
                            [](unsigned char c) { return c > ' '; }));
    code.erase(std::find_if(code.rbegin(), code.rend(),
                            [](unsigned char c) { return c > ' '; })
                   .base(),
               code.end());
    for (char& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    Json& codes = database_.rawTable("codes");
    if (code.empty() || !codes.contains(code)) {
        sendShopResult(connection, net::ShopResultKind::Redeem, false, 0, "Invalid code");
        return;
    }

    // Read through a const alias: Json's mutable operator[] inserts, and a
    // code this call is about to refuse must not grow keys on the way out.
    const Json& stored = const_cast<const Json&>(codes)[code];
    const int maxUses = stored["maxUses"].asInt(0);
    const int uses = stored["uses"].asInt(0);
    // Keyed on the ACCOUNT, not the socket: a player who reconnects is the
    // same person, and a code that reset with the connection would be
    // unlimited to anyone willing to press Play twice.
    Json owners = stored["usedBy"];
    if (!owners.isArray()) owners = Json::array();
    for (const Json& owner : owners.items()) {
        if (owner.asString() == session.userId) {
            sendShopResult(connection, net::ShopResultKind::Redeem, false, 0,
                           "Code already redeemed");
            return;
        }
    }
    if (maxUses > 0 && uses >= maxUses) {
        sendShopResult(connection, net::ShopResultKind::Redeem, false, 0,
                       "Code has reached maximum uses");
        return;
    }

    const int stars = stored["stars"].asInt(0);
    PlayerRecord& record = database_.progress(session.userId);
    record.stars += stars;
    if (session.playing() && world_.isAlive(session.entity)) {
        if (PlayerProgress* live = world_.tryGet<PlayerProgress>(session.entity)) {
            live->stars = record.stars;
        }
    }

    owners.push(Json(session.userId));
    Json& entry = codes[code];
    entry["uses"] = Json(uses + 1);
    entry["usedBy"] = std::move(owners);
    // A fully spent code is dropped rather than left to be rejected forever,
    // which is what the browser build does with it.
    if (maxUses > 0 && uses + 1 >= maxUses) codes.erase(code);
    database_.markDirty();

    sendProfile(session, connection);
    sendShopResult(connection, net::ShopResultKind::Redeem, true, stars, {});
}

void GameServer::handleSetSkin(Session& session, net::Connection& connection, ByteReader& reader) {
    const std::uint32_t requested = reader.u32();
    if (!reader.ok() || !session.authenticated()) return;

    // Exactly one cosmetic bit, or none. Glitch is deliberately absent: it is
    // a transient effect PlayerVisuals ORs in, not something to wear.
    constexpr std::uint32_t kWearable[] = {PlayerRenderPumpkin, PlayerRenderRobot};
    std::uint32_t flags = PlayerRenderNone;
    for (const std::uint32_t bit : kWearable) {
        if (requested == bit) { flags = bit; break; }
    }
    if (requested != PlayerRenderNone && flags == PlayerRenderNone) return;

    PlayerRecord& record = database_.progress(session.userId);
    record.renderFlags = flags;
    database_.markDirty();
    if (session.playing() && world_.isAlive(session.entity)) {
        world_.ensure<PlayerVisuals>(session.entity).renderFlags = flags;
    }
    sendProfile(session, connection);
}

// --- user-created skins ----------------------------------------------------
//
// The catalog is one shared, public list: anything published renders on every
// screen that sees the author wearing it, which is exactly why nothing a
// client sends is trusted. sanitizeSkin() runs here as well as in the studio,
// and the id, the author and the timestamp are the server's to assign.
//
// Stored as raw JSON under the database's `customSkins` key rather than as a
// typed table, because that key belongs to the shared inventory.json the
// browser build also reads and writes: a skin published in one build has to
// come back in the other.

namespace {

/// The studio's own reply channel. Every outcome the reference reports --
/// success included -- is a chat line from "Skins", not a notice or a toast.
void sendSkinChat(net::Connection& connection, const std::string& text) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Chat));
    w.u8(static_cast<std::uint8_t>(net::ChatChannel::System));
    w.str("Skins");
    w.str(text);
    connection.send(w);
}

/// A guest account, which may play but may not add to the shared catalog.
///
/// The reference tests `/^User\d{8}$/`; this build's guest minting does not
/// zero-pad (client/app.cpp), so the digit run is matched at any length rather
/// than at exactly eight -- otherwise the rule would miss the very accounts it
/// exists to stop.
bool isGuestName(const std::string& name) {
    if (name.size() <= 4 || name.compare(0, 4, "User") != 0) return false;
    for (std::size_t i = 4; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

/// Usernames are compared case-insensitively for ownership, as the reference
/// does: the account "Rose" and the author string "rose" are one person.
bool sameUser(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string base36(std::uint64_t value) {
    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (value == 0) return "0";
    std::string out;
    while (value > 0) {
        out += digits[value % 36];
        value /= 36;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace

void GameServer::broadcastToAuthenticated(const ByteWriter& message) {
    listener_.each([&](net::Connection& connection) {
        const Session* session = sessionFor(connection.id());
        if (session && session->authenticated()) connection.send(message);
    });
}

void GameServer::broadcastDebugStats() {
    // Drained whether or not anyone is listening, so a window that spanned an
    // empty server does not pour its samples into the first client to join.
    const double avgMillis =
        debugTickSamples_ > 0 ? debugTickAccumMillis_ / debugTickSamples_ : 0.0;
    const double maxMillis = debugTickMaxMillis_;
    debugTickAccumMillis_ = 0;
    debugTickMaxMillis_ = 0;
    debugTickSamples_ = 0;

    bool anyone = false;
    for (const auto& entry : sessions_) {
        if (entry.second.authenticated()) { anyone = true; break; }
    }
    if (!anyone) return;

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::DebugStats));
    w.f64(static_cast<double>(residentBytes()));
    w.f64(static_cast<double>(heapBytes()));
    w.f32(static_cast<float>(avgMillis));
    w.f32(static_cast<float>(maxMillis));
    broadcastToAuthenticated(w);
}

void GameServer::sendSkinCatalog(Session& session, net::Connection& connection) {
    const Json& skins = database_.rawTable("customSkins");
    const PlayerRecord& record = database_.progress(session.userId);

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::SkinCatalog));
    // Advisory only: it decides whether the takedown button is drawn and
    // whether the chat autocomplete offers the /admin rows. Every delete, and
    // every admin command, is re-checked server-side regardless of what the
    // client believes -- so a temporary grant may safely raise it, which is
    // how a grantee's console appears without them logging out and back in.
    w.boolean(effectiveAdmin(session));
    w.str(record.equippedSkinId);
    const std::size_t at = w.reserveU16();
    std::uint16_t count = 0;
    for (const std::string& id : skins.keys()) {
        const CustomSkin skin = skinFromJson(skins[id]);
        if (skin.id.empty() || skin.shapes.empty()) continue;
        writeCustomSkin(w, skin);
        ++count;
    }
    w.patchU16(at, count);
    connection.send(w);
}

void GameServer::handlePublishSkin(Session& session, net::Connection& connection,
                                   ByteReader& reader) {
    const std::string name = reader.str();
    const std::uint8_t shapeCount = reader.u8();
    std::vector<SkinShape> shapes;
    shapes.reserve(std::min<std::size_t>(shapeCount, kMaxSkinShapes));
    for (int i = 0; i < shapeCount; ++i) {
        SkinShape shape;
        if (readSkinShape(reader, shape) && shapes.size() < static_cast<std::size_t>(kMaxSkinShapes)) {
            shapes.push_back(std::move(shape));
        }
    }
    if (!reader.ok() || !session.authenticated()) return;

    if (isGuestName(session.username)) {
        sendSkinChat(connection, "Create a (non-guest) account to publish skins.");
        return;
    }
    const SkinCheck check = sanitizeSkin(name, shapes);
    if (!check.ok()) {
        sendSkinChat(connection, check.error);
        return;
    }

    Json& catalog = database_.rawTable("customSkins");
    // Read through a const view: Json's non-const operator[] CREATES the key
    // it is handed, so a lookup that misses would quietly grow the table.
    const Json& stored = catalog;
    int mine = 0;
    for (const std::string& id : stored.keys()) {
        if (sameUser(stored[id]["author"].asString(), session.username)) ++mine;
    }
    if (mine >= kMaxSkinsPerUser) {
        sendSkinChat(connection, "You've reached the limit of " +
                                     std::to_string(kMaxSkinsPerUser) +
                                     " published skins. Delete one first.");
        return;
    }

    CustomSkin skin;
    // Time plus a random tail, as the reference mints it: the clock alone
    // collides when two players publish in the same millisecond.
    skin.id = "sk_" + base36(static_cast<std::uint64_t>(database_.nowMillis())) + "_" +
              base36(rng_.next() % 2176782336ull);
    skin.name = check.name;
    skin.author = session.username;
    skin.shapes = check.shapes;
    skin.createdAt = static_cast<double>(database_.nowMillis());
    catalog[skin.id] = skinToJson(skin);
    database_.markDirty();

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::SkinPublished));
    writeCustomSkin(w, skin);
    // Everyone, not just the author: a skin nobody else has cannot be drawn on
    // the player wearing it.
    broadcastToAuthenticated(w);
    sendSkinChat(connection, "Published \"" + skin.name + "\". It's now in the Browse tab.");
}

void GameServer::handleEquipSkin(Session& session, net::Connection& connection,
                                 ByteReader& reader) {
    const std::string id = reader.str();
    if (!reader.ok() || !session.authenticated()) return;

    const Json& catalog = database_.rawTable("customSkins");
    if (!id.empty() && !catalog[id].isObject()) {
        sendSkinChat(connection, "That skin no longer exists.");
        return;
    }

    PlayerRecord& record = database_.progress(session.userId);
    record.equippedSkinId = id;
    // A custom skin replaces any built-in cosmetic so the two cannot fight
    // over the same body.
    if (!id.empty()) record.renderFlags = PlayerRenderNone;
    database_.markDirty();
    if (session.playing() && world_.isAlive(session.entity)) {
        world_.ensure<PlayerVisuals>(session.entity).renderFlags = record.renderFlags;
    }
}

void GameServer::handleDeleteSkin(Session& session, net::Connection& connection,
                                  ByteReader& reader) {
    const std::string id = reader.str();
    if (!reader.ok() || !session.authenticated()) return;

    Json& catalog = database_.rawTable("customSkins");
    const Json& stored = catalog;
    if (!stored[id].isObject()) return;
    const std::string author = stored[id]["author"].asString();
    const std::string name = stored[id]["name"].asString();

    const bool owner = sameUser(author, session.username);
    if (!session.admin && !owner) {
        sendSkinChat(connection, "You can only take down your own skins.");
        return;
    }
    catalog.erase(id);
    database_.markDirty();

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::SkinDeleted));
    w.str(id);
    broadcastToAuthenticated(w);
    sendSkinChat(connection, owner ? "Deleted \"" + name + "\"."
                                   : "Took down \"" + name + "\" by " + author + ".");
}

void GameServer::handleLeaderboard(const Session& session, net::Connection& connection) {
    // Ranked over ACCOUNTS, not over the players currently online: the board is
    // a record of progress, and a top player who logged off has not lost it.
    struct Row {
        const std::string* name;
        double totalXp;
    };
    std::vector<Row> rows;
    rows.reserve(database_.userCount());
    for (const std::string& username : database_.usernames()) {
        const Account* account = database_.findUser(username);
        if (account == nullptr) continue;
        // Staff are off the board. The browser build's getLeaderboard() takes
        // includeAdmins and the client only ever passes false, so an admin
        // account would rank here and nowhere in the reference.
        if (account->admin) continue;
        const PlayerRecord* record = database_.findProgress(account->id);
        rows.push_back({&account->username, record ? record->totalXp : 0.0});
    }

    // The browser client asks for limit=50 and the panel scrolls all fifty;
    // still one byte on the wire, so the count below stays a u8.
    constexpr std::size_t kRows = 50;
    const std::size_t shown = std::min(kRows, rows.size());
    std::partial_sort(rows.begin(), rows.begin() + static_cast<long>(shown), rows.end(),
                      [](const Row& a, const Row& b) { return a.totalXp > b.totalXp; });

    // The count beside the title is over every account, not over the 25 rows
    // that fit. The active-today figure rides along only for an admin, which is
    // how the browser's payload leaves the field out for everyone else -- and 0
    // is the same answer as "absent" to the panel, since the asker is always
    // active today themselves.
    const std::int64_t dayAgo = database_.nowMillis() - 24 * 60 * 60 * 1000;
    std::uint32_t activeToday = 0;
    if (session.admin) {
        for (const std::string& username : database_.usernames()) {
            const Account* account = database_.findUser(username);
            if (account && account->lastActiveAtMillis >= dayAgo) ++activeToday;
        }
    }

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Leaderboard));
    w.u8(static_cast<std::uint8_t>(shown));
    w.u32(static_cast<std::uint32_t>(database_.userCount()));
    w.u32(activeToday);
    for (std::size_t i = 0; i < shown; ++i) {
        w.str(*rows[i].name);
        w.u16(static_cast<std::uint16_t>(levelFromTotalXp(rows[i].totalXp).level));
        w.f64(rows[i].totalXp);
    }
    connection.send(w);
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void GameServer::handleNotifications(net::Connection& connection, ByteReader& reader) {
    const std::uint16_t limit = reader.u16();
    const double before = reader.f64();
    if (!reader.ok()) return;

    // Read through storedTable(), never rawTable(): this is the one unmodelled
    // table the browser stores as an ARRAY, and rawTable() would coerce the
    // whole feed to an empty object on the way past.
    const Json& table = database_.storedTable("notifications");

    struct Row {
        const Json* entry;
        double stamp;
    };
    std::vector<Row> rows;
    if (table.isArray()) {
        rows.reserve(table.size());
        for (const Json& entry : table.items()) {
            const double stamp = entry["timestamp"].asDouble();
            // `before` is exclusive, which is what lets a page request start
            // exactly at the oldest entry the client already holds.
            if (before > 0 && stamp >= before) continue;
            rows.push_back({&entry, stamp});
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.stamp > b.stamp; });

    const std::size_t want = std::min<std::size_t>(limit == 0 ? 50 : limit, 200);
    const std::size_t shown = std::min(want, rows.size());

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Notifications));
    // "More" is a full page, not a count of what is left: the browser reads
    // `batch.length === limit` and so pages one request past the end.
    w.boolean(shown == want);
    w.u16(static_cast<std::uint16_t>(shown));
    for (std::size_t i = 0; i < shown; ++i) {
        const Json& entry = *rows[i].entry;
        w.str(entry["id"].asString());
        w.u8(static_cast<std::uint8_t>(notificationKind(entry["type"].asString())));
        w.str(entry["message"].asString());
        w.f64(rows[i].stamp);
    }
    connection.send(w);
}

// ---------------------------------------------------------------------------
// Guilds
// ---------------------------------------------------------------------------

std::string GameServer::guildNameForUser(const std::string& username) const {
    if (username.empty()) return {};
    const Json& guilds = database_.storedTable("guilds");
    if (!guilds.isObject()) return {};
    for (const std::string& key : guilds.keys()) {
        if (guildMemberIndex(guilds[key], username) >= 0) return key;
    }
    return {};
}

Session* GameServer::sessionForUser(const std::string& username) {
    const std::string key = lowerCase(username);
    if (key.empty()) return nullptr;
    for (auto& [id, session] : sessions_) {
        if (session.authenticated() && lowerCase(session.username) == key) return &session;
    }
    return nullptr;
}

net::Connection* GameServer::connectionForUser(const std::string& username) {
    const Session* session = sessionForUser(username);
    return session ? listener_.find(session->connection) : nullptr;
}

void GameServer::sendChatTo(net::Connection& connection, net::ChatChannel channel,
                            const std::string& author, const std::string& text) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Chat));
    w.u8(static_cast<std::uint8_t>(channel));
    w.str(author);
    w.str(text);
    connection.send(w);
}

void GameServer::sendGuildRoster(net::Connection& connection, const Json& guild) {
    const Json& members = guild["memberUsernames"];

    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::GuildUpdate));
    w.boolean(true);
    w.str(guild["name"].asString());
    w.str(guild["leaderUsername"].asString());
    w.u16(static_cast<std::uint16_t>(members.size()));
    for (std::size_t i = 0; i < members.size(); ++i) {
        const std::string member = members[i].asString();
        w.str(member);
        w.boolean(sessionForUser(member) != nullptr);
    }
    connection.send(w);
}

void GameServer::sendNoGuild(net::Connection& connection) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::GuildUpdate));
    w.boolean(false);
    connection.send(w);
}

void GameServer::broadcastGuildRoster(const Json& guild) {
    // Rebuilt per member rather than sent once: the online flags are the same
    // for everyone, but the roster is only sent to people in the guild, and
    // there is no room concept here to address them as a group.
    const Json& members = guild["memberUsernames"];
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (net::Connection* peer = connectionForUser(members[i].asString())) {
            sendGuildRoster(*peer, guild);
        }
    }
}

void GameServer::sendGuildState(const Session& session, net::Connection& connection) {
    const std::string name = guildNameForUser(session.username);
    if (name.empty()) {
        sendNoGuild(connection);
        return;
    }
    // Every member is told, not just this one: somebody logging in changes the
    // online column of every roster that lists them.
    broadcastGuildRoster(database_.storedTable("guilds")[name]);
}

void GameServer::handleGuildCreate(Session& session, net::Connection& connection,
                                   ByteReader& reader) {
    const std::string raw = reader.str();
    if (!reader.ok() || !session.authenticated()) return;
    guildCreate(session, connection, raw);
}

void GameServer::guildCreate(Session& session, net::Connection& connection,
                             const std::string& raw) {
    if (!session.authenticated()) return;

    const std::string name = normalizeGuildName(raw);
    if (name.empty()) {
        sendNotice(connection, net::NoticeSeverity::Warning, "Guild name cannot be empty.");
        return;
    }
    if (!validGuildName(name)) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "Guild name must be exactly 5 alphanumeric characters (A–Z, 0–9).");
        return;
    }
    if (!guildNameForUser(session.username).empty()) {
        sendNotice(connection, net::NoticeSeverity::Warning, "You are already in a guild.");
        return;
    }

    Json& guilds = database_.rawTable("guilds");
    if (guilds.contains(name)) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "A guild named \"" + name + "\" already exists.");
        return;
    }

    Json guild = Json::object();
    guild["name"] = name;
    guild["leaderUsername"] = session.username;
    Json members = Json::array();
    members.push(session.username);
    guild["memberUsernames"] = std::move(members);
    guild["createdAt"] = static_cast<double>(database_.nowMillis());
    guilds[name] = std::move(guild);
    database_.markDirty();

    sendNotice(connection, net::NoticeSeverity::Good, "Guild \"" + name + "\" created.");
    broadcastGuildRoster(guilds[name]);
}

void GameServer::handleGuildInvite(Session& session, net::Connection& connection,
                                   ByteReader& reader) {
    const std::string raw = reader.str();
    if (!reader.ok() || !session.authenticated()) return;
    guildInvite(session, connection, raw);
}

void GameServer::guildInvite(Session& session, net::Connection& connection,
                             const std::string& raw) {
    if (!session.authenticated()) return;

    const std::string guildName = guildNameForUser(session.username);
    if (guildName.empty()) {
        sendNotice(connection, net::NoticeSeverity::Warning, "You are not in a guild.");
        return;
    }
    const Json& guild = database_.storedTable("guilds")[guildName];
    if (lowerCase(guild["leaderUsername"].asString()) != lowerCase(session.username)) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "Only the guild leader can invite players.");
        return;
    }
    if (guild["memberUsernames"].size() >= kMaxGuildSize) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "Guild is full (max " + std::to_string(kMaxGuildSize) + " members).");
        return;
    }

    const std::string target = trimmed(raw);
    if (target.empty()) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "Please provide a username to invite.");
        return;
    }
    if (database_.findUser(target) == nullptr) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "No player named \"" + target + "\" exists.");
        return;
    }
    if (lowerCase(target) == lowerCase(session.username)) {
        sendNotice(connection, net::NoticeSeverity::Warning, "You cannot invite yourself.");
        return;
    }
    if (!guildNameForUser(target).empty()) {
        sendNotice(connection, net::NoticeSeverity::Warning, target + " is already in a guild.");
        return;
    }

    const std::string key = lowerCase(target);
    const std::int64_t now = database_.nowMillis();
    const auto existing = guildInvites_.find(key);
    if (existing != guildInvites_.end() && existing->second.expiresAtMillis > now) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   target + " already has a pending guild invite.");
        return;
    }

    guildInvites_[key] = {guildName, session.username, now + kGuildInviteMillis};
    sendNotice(connection, net::NoticeSeverity::Good, "Guild invite sent to " + target + ".");

    if (net::Connection* peer = connectionForUser(target)) {
        ByteWriter w;
        w.u8(static_cast<std::uint8_t>(net::ServerMessage::GuildInviteReceived));
        w.str(guildName);
        w.str(session.username);
        peer->send(w);
        sendNotice(*peer, net::NoticeSeverity::Info,
                   "@" + session.username + " has invited you to guild \"" + guildName +
                       "\". Use /guild-accept or /guild-decline.");
    }
}

void GameServer::handleGuildAccept(Session& session, net::Connection& connection) {
    if (!session.authenticated()) return;
    const std::string key = lowerCase(session.username);
    const auto invite = guildInvites_.find(key);
    if (invite == guildInvites_.end()) {
        sendNotice(connection, net::NoticeSeverity::Warning, "You have no pending guild invite.");
        return;
    }
    // Every failure below drops the invitation: an invite that cannot be taken
    // up is spent, or a player refused by a full guild would keep a banner they
    // can never clear.
    const std::string guildName = invite->second.guildName;
    if (invite->second.expiresAtMillis < database_.nowMillis()) {
        guildInvites_.erase(invite);
        sendNotice(connection, net::NoticeSeverity::Warning, "Guild invite has expired.");
        return;
    }

    Json& guilds = database_.rawTable("guilds");
    if (!guilds.contains(guildName)) {
        guildInvites_.erase(invite);
        sendNotice(connection, net::NoticeSeverity::Warning, "Guild no longer exists.");
        return;
    }
    Json& guild = guilds[guildName];
    if (guild["memberUsernames"].size() >= kMaxGuildSize) {
        guildInvites_.erase(invite);
        sendNotice(connection, net::NoticeSeverity::Warning, "Guild is full.");
        return;
    }
    if (!guildNameForUser(session.username).empty()) {
        guildInvites_.erase(invite);
        sendNotice(connection, net::NoticeSeverity::Warning, "You are already in a guild.");
        return;
    }

    guild["memberUsernames"].push(session.username);
    guildInvites_.erase(invite);
    database_.markDirty();

    const std::string author = "[Guild " + guildName + "]";
    const Json& members = guild["memberUsernames"];
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (net::Connection* peer = connectionForUser(members[i].asString())) {
            sendChatTo(*peer, net::ChatChannel::System, author,
                       session.username + " has joined the guild.");
        }
    }
    broadcastGuildRoster(guild);
}

void GameServer::handleGuildDecline(Session& session, net::Connection& connection) {
    if (!session.authenticated()) return;
    guildInvites_.erase(lowerCase(session.username));
    sendNotice(connection, net::NoticeSeverity::Info, "Guild invite declined.");
}

void GameServer::handleGuildLeave(Session& session, net::Connection& connection) {
    if (!session.authenticated()) return;
    const std::string guildName = guildNameForUser(session.username);
    if (guildName.empty()) {
        sendNotice(connection, net::NoticeSeverity::Warning, "You are not in a guild.");
        return;
    }

    Json& guilds = database_.rawTable("guilds");
    Json& guild = guilds[guildName];
    const int at = guildMemberIndex(guild, session.username);
    if (at >= 0) {
        Json& members = guild["memberUsernames"];
        members.items().erase(members.items().begin() + at);
    }

    // The client is told it has no guild before anything else, so the panel
    // flips to the no-guild view on the same frame the confirmation lands.
    sendNoGuild(connection);
    sendNotice(connection, net::NoticeSeverity::Info, "You have left the guild.");

    if (guild["memberUsernames"].size() == 0) {
        // The last member out disbands it rather than leaving an empty guild
        // holding a five-character name nobody else can claim.
        guilds.erase(guildName);
        database_.markDirty();
        return;
    }

    std::string promoted;
    if (lowerCase(guild["leaderUsername"].asString()) == lowerCase(session.username)) {
        promoted = guild["memberUsernames"][std::size_t{0}].asString();
        guild["leaderUsername"] = promoted;
    }
    database_.markDirty();

    const std::string author = "[Guild " + guildName + "]";
    const Json& members = guild["memberUsernames"];
    for (std::size_t i = 0; i < members.size(); ++i) {
        net::Connection* peer = connectionForUser(members[i].asString());
        if (peer == nullptr) continue;
        sendChatTo(*peer, net::ChatChannel::System, author,
                   session.username + " has left the guild.");
        if (!promoted.empty()) {
            sendChatTo(*peer, net::ChatChannel::System, author,
                       promoted + " is now the guild leader.");
        }
    }
    broadcastGuildRoster(guild);
}

void GameServer::handleGuildKick(Session& session, net::Connection& connection,
                                 ByteReader& reader) {
    const std::string raw = reader.str();
    if (!reader.ok() || !session.authenticated()) return;
    guildKick(session, connection, raw);
}

void GameServer::guildKick(Session& session, net::Connection& connection,
                           const std::string& raw) {
    if (!session.authenticated()) return;

    const std::string guildName = guildNameForUser(session.username);
    if (guildName.empty()) {
        sendNotice(connection, net::NoticeSeverity::Warning, "You are not in a guild.");
        return;
    }
    Json& guild = database_.rawTable("guilds")[guildName];
    if (lowerCase(guild["leaderUsername"].asString()) != lowerCase(session.username)) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "Only the guild leader can kick players.");
        return;
    }
    const std::string target = trimmed(raw);
    if (lowerCase(target) == lowerCase(session.username)) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "You cannot kick yourself. Use /guild-leave instead.");
        return;
    }
    const int at = guildMemberIndex(guild, target);
    if (at < 0) {
        sendNotice(connection, net::NoticeSeverity::Warning, target + " is not in your guild.");
        return;
    }

    // The stored spelling, not what the leader typed: the kicked player's own
    // messages read back the name their account carries.
    const std::string member = guild["memberUsernames"][static_cast<std::size_t>(at)].asString();
    Json& members = guild["memberUsernames"];
    members.items().erase(members.items().begin() + at);
    database_.markDirty();

    const std::string author = "[Guild " + guildName + "]";
    const Json& remaining = guild["memberUsernames"];
    for (std::size_t i = 0; i < remaining.size(); ++i) {
        if (net::Connection* peer = connectionForUser(remaining[i].asString())) {
            sendChatTo(*peer, net::ChatChannel::System, author,
                       member + " was kicked from the guild by " + session.username + ".");
        }
    }
    if (net::Connection* peer = connectionForUser(member)) {
        sendNoGuild(*peer);
        sendNotice(*peer, net::NoticeSeverity::Bad,
                   "You were kicked from guild \"" + guildName + "\".");
    }
    broadcastGuildRoster(guild);
}

// ---------------------------------------------------------------------------
// Scheduled restart
// ---------------------------------------------------------------------------

namespace {

/// How long before a restart each warning goes out, longest first. The
/// reference's RESTART_WARNINGS_MS.
constexpr std::array<double, 4> kRestartWarnings = {600000.0, 300000.0, 60000.0, 10000.0};

/// "daily" is the only reason with a friendlier name than itself.
std::string restartReasonText(const std::string& reason) {
    return reason == "daily" ? "daily maintenance" : reason;
}

/// The reference prefixes both of these with U+26A0. It is dropped here for
/// the same reason /guild-info's bullet was: there is one font in this build
/// and it cannot draw that glyph, so the sign would arrive as a hole in the
/// sentence. The words are the reference's, unchanged.
std::string restartWarning(double millis, const std::string& reason) {
    if (millis >= 60000.0) {
        const long minutes = std::lround(millis / 60000.0);
        return "<span style=\"color:#ffb74d;\">Server will restart in " +
               std::to_string(minutes) + (minutes == 1 ? " minute (" : " minutes (") +
               restartReasonText(reason) + ").</span>";
    }
    const long seconds = std::lround(millis / 1000.0);
    return "<span style=\"color:#ff6b6b;\">Server restarting in " + std::to_string(seconds) +
           (seconds == 1 ? " second!</span>" : " seconds!</span>");
}

} // namespace

bool GameServer::scheduleRestart(double delayMillis, const std::string& reason) {
    // A restart that has already announced itself and closed the door is not
    // one anybody can reschedule.
    if (restart_.firing) return false;
    if (delayMillis < 0) delayMillis = 0;

    restart_.pending = true;
    restart_.atMillis = clockMillis_ + delayMillis;
    restart_.reason = reason;
    // Warnings longer than the delay itself are skipped outright rather than
    // fired late: "restarting in 10 minutes" said one second before the exit
    // is worse than saying nothing.
    restart_.warningsSaid = 0;
    while (restart_.warningsSaid < kRestartWarnings.size() &&
           kRestartWarnings[restart_.warningsSaid] >= delayMillis) {
        ++restart_.warningsSaid;
    }
    return true;
}

bool GameServer::cancelScheduledRestart() {
    if (restart_.firing || !restart_.pending) return false;
    restart_ = ScheduledRestart{};
    return true;
}

bool GameServer::scheduledRestartInfo(double& remainingMillis, std::string& reason) const {
    if (!restart_.pending && !restart_.firing) return false;
    remainingMillis = std::max(0.0, restart_.atMillis - clockMillis_);
    reason = restart_.reason;
    return true;
}

void GameServer::serviceScheduledRestart(double nowMillis) {
    if (restart_.firing) {
        // The gap between the last word and the exit exists so the socket
        // writes actually leave: stopping in the same breath as the broadcast
        // drops the message that explains the disconnect.
        if (nowMillis >= restart_.stopAtMillis) stop();
        return;
    }
    if (!restart_.pending) return;

    const double remaining = restart_.atMillis - nowMillis;
    while (restart_.warningsSaid < kRestartWarnings.size() &&
           remaining <= kRestartWarnings[restart_.warningsSaid]) {
        broadcastChat(net::ChatChannel::System, "System",
                      restartWarning(kRestartWarnings[restart_.warningsSaid], restart_.reason));
        ++restart_.warningsSaid;
    }
    if (remaining > 0) return;

    restart_.pending = false;
    restart_.firing = true;
    restart_.stopAtMillis = nowMillis + 1000.0;
    broadcastChat(net::ChatChannel::System, "System",
                  "<span style=\"color:#ff6b6b;\">Server restarting now (" +
                      restartReasonText(restart_.reason) +
                      "). Reconnecting shortly...</span>");
    std::printf("[restart] scheduled restart triggered (reason: %s)\n", restart_.reason.c_str());
}

// ---------------------------------------------------------------------------
// Self-update
// ---------------------------------------------------------------------------

void GameServer::serviceAutoUpdate() {
    net::Connection* requester =
        updateRequester_ != 0 ? listener_.find(updateRequester_) : nullptr;

    // Drained whether or not anybody is still listening: the lines are also
    // the JS side's console output, and leaving them queued would replay a
    // finished update's whole log at the next admin who asks for one.
    for (const std::string& line : autoupdate::drainLog()) {
        if (requester != nullptr) sendSystem(*requester, line);
    }

    switch (autoupdate::takeOutcome()) {
        case autoupdate::Outcome::Installed: {
            const bool scheduled = scheduleRestart(updateRestartDelayMillis_, "update");
            if (requester == nullptr) break;
            if (scheduled) {
                const long seconds = std::lround(updateRestartDelayMillis_ / 1000.0);
                sendSystem(*requester,
                           "[UPDATE] Done. Server restarts in " + std::to_string(seconds) +
                               "s to load the new build (\"restart cancel\" or \"update cancel\" "
                               "to abort the restart -- the new files stay installed either "
                               "way).");
            } else {
                sendSystem(*requester,
                           "[UPDATE] Done. A restart is already firing -- the new build loads "
                           "when the server comes back up.");
            }
            break;
        }
        case autoupdate::Outcome::Failed:
            // The JS side already said what went wrong, through the log above.
            break;
        case autoupdate::Outcome::None:
            break;
    }

    if (!autoupdate::inProgress()) updateRequester_ = 0;
}

// ---------------------------------------------------------------------------
// The maze
// ---------------------------------------------------------------------------

std::string GameServer::adminChangeMaze(const std::string& argument) {
    static const std::array<const char*, 3> kBiomeNames = {{"garden", "desert", "ocean"}};
    const auto biomeName = [](MazeBiome biome) {
        return kBiomeNames[static_cast<std::size_t>(biome)];
    };

    const std::int64_t currentDay = activeMaze().day();
    const std::string token = lowerCase(trimmed(argument));

    std::int64_t targetDay = 0;
    if (token.empty() || token == "next") {
        targetDay = currentDay + 1;
    } else if (token == "garden" || token == "desert" || token == "ocean") {
        // The three layouts are authored, not generated: the day number only
        // picks which one is active, so asking for a biome means asking for the
        // nearest day that lands on it.
        const auto want = static_cast<std::int64_t>(
            token == "garden" ? 0 : (token == "desert" ? 1 : 2));
        const std::int64_t current = ((currentDay % 3) + 3) % 3;
        const std::int64_t advance = ((want - current) + 3) % 3;
        if (advance == 0) return "Maze is already " + token + ".";
        targetDay = currentDay + advance;
    } else {
        int parsed = 0;
        bool numeric = !token.empty();
        std::size_t at = (token[0] == '-') ? 1 : 0;
        if (at >= token.size()) numeric = false;
        for (std::size_t i = at; numeric && i < token.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(token[i]))) numeric = false;
        }
        if (!numeric) {
            return std::string("Usage: change-maze [next|garden|desert|ocean|<dayNumber>] ")
                       .append("\xE2\x80\x94 current: day ") +
                   std::to_string(currentDay) + " (" + biomeName(activeMaze().biome()) + ")";
        }
        parsed = std::atoi(token.c_str());
        targetDay = parsed;
    }

    if (targetDay == currentDay) {
        return "Maze is already day " + std::to_string(currentDay) + " (" +
               biomeName(activeMaze().biome()) + ").";
    }

    mazeDayOffset_ = targetDay - currentMazeDay();
    setActiveMazeDay(targetDay);

    // Anyone standing in the old layout is standing in the new one's walls.
    // The reference moves them to the new entrance; so does this, and the
    // client cuts its interpolation on the jump by itself.
    const Vec2 entrance = activeMaze().spawn();
    Query<PlayerTag, Transform> flowers{world_};
    std::vector<Entity> inside;
    flowers.each([&](Entity entity, PlayerTag&, Transform& transform) {
        if (isInMazeRegion(transform.position)) inside.push_back(entity);
    });
    for (const Entity entity : inside) teleportEntity(entity, entrance);

    return "Maze changed to day " + std::to_string(activeMaze().day()) + " (" +
           biomeName(activeMaze().biome()) + "). Offset from real day: " +
           (mazeDayOffset_ >= 0 ? "+" : "") + std::to_string(mazeDayOffset_) + ".";
}

// ---------------------------------------------------------------------------
// Squads
// ---------------------------------------------------------------------------
//
// The rules are server/squads.h. What lives here is everything those rules
// deliberately know nothing about: which body a member currently owns, what to
// call them, and who to tell.

std::string GameServer::squadAccountName(SquadMemberId member) {
    if (!member.bot()) {
        const Session* session = sessionFor(member.connection);
        return session != nullptr && !session->username.empty() ? session->username : "Unknown";
    }
    // A bot has no account. The reference reads its "username" off its
    // nameplate for exactly this listing, and `/squad-invite` matches on the
    // same string, so the two agree by construction.
    if (const PlayerAccount* account = world_.tryGet<PlayerAccount>(member.entity)) {
        return account->username;
    }
    return "Unknown";
}

std::string GameServer::squadDisplayName(SquadMemberId member) {
    if (!member.bot()) {
        const Session* session = sessionFor(member.connection);
        if (session == nullptr) return "Unknown";
        return session->displayName.empty() ? session->username : session->displayName;
    }
    return squadAccountName(member);
}

Entity GameServer::squadEntity(SquadMemberId member) {
    if (member.bot()) {
        return world_.isAlive(member.entity) ? member.entity : NULL_ENTITY;
    }
    const Session* session = sessionFor(member.connection);
    if (session == nullptr || !session->playing()) return NULL_ENTITY;
    return session->entity;
}

net::Connection* GameServer::squadConnection(SquadMemberId member) {
    return member.bot() ? nullptr : listener_.find(member.connection);
}

void GameServer::sendSquadUpdate(net::Connection& connection, const Squad* squad) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::SquadUpdate));
    if (squad == nullptr) {
        w.boolean(false);
        connection.send(w);
        return;
    }
    w.boolean(true);
    w.str(squad->id);
    w.boolean(squad->isPublic);
    w.u8(static_cast<std::uint8_t>(squad->members.size()));
    for (const SquadMemberId& member : squad->members) {
        w.str(squadAccountName(member));
        w.str(squadDisplayName(member));
        // The wire id, not the entity: it is what the client's own world is
        // keyed by, and it is 0 for a member with no body just now -- somebody
        // sitting on the title screen, or waiting on a death card.
        const Entity body = squadEntity(member);
        const NetId* id = body != NULL_ENTITY ? world_.tryGet<NetId>(body) : nullptr;
        w.u32(id != nullptr ? id->value : 0);
        std::uint8_t flags = 0;
        if (squad->leader == member) flags |= 1u;
        if (member.bot()) flags |= 2u;
        w.u8(flags);
    }
    connection.send(w);
}

void GameServer::broadcastSquadUpdate(const Squad& squad) {
    for (const SquadMemberId& member : squad.members) {
        if (net::Connection* peer = squadConnection(member)) sendSquadUpdate(*peer, &squad);
    }
}

void GameServer::sendSquadSystem(const Squad& squad, const std::string& text) {
    // Signed "[Squad]" rather than "System", which is what tells a member
    // whether a line was said to the world or to the four of them.
    for (const SquadMemberId& member : squad.members) {
        if (net::Connection* peer = squadConnection(member)) {
            sendChatTo(*peer, net::ChatChannel::Squad, "[Squad]",
                       "<span style=\"color: #4fc3f7;\">" + text + "</span>");
        }
    }
}

bool GameServer::resolveSquadTarget(const std::string& name, SquadMemberId& out) {
    if (Session* session = sessionForUser(name)) {
        out = squadIdOf(*session);
        return true;
    }
    // Then a bot, by nameplate: bots own no account, and inviting one is half
    // of what squads are for on a quiet server.
    const std::string key = lowerCase(trimmed(name));
    if (key.empty()) return false;
    for (const Bot& bot : bots_) {
        if (bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity)) continue;
        if (lowerCase(bot.name) != key) continue;
        out = SquadMemberId::ofBot(bot.entity);
        return true;
    }
    return false;
}

void GameServer::departSquad(Session& session, net::Connection* connection,
                             const std::string& leaverName) {
    const SquadRoster::Departure departure = squads_.leave(squadIdOf(session));
    if (!departure.wasMember) return;
    if (connection != nullptr) sendSquadUpdate(*connection, nullptr);

    Squad* remaining = squads_.find(departure.squadId);
    if (remaining == nullptr) return;
    // Promotion first, then the departure: that is the order the reference
    // emits them in, because the promotion happens inside its leaveSquad and
    // the "has left" line is said by the caller afterwards.
    if (departure.promoted.valid()) {
        sendSquadSystem(*remaining, squadDisplayName(departure.promoted) +
                                        " is now the squad leader.");
    }
    sendSquadSystem(*remaining, leaverName + " has left the squad.");
    broadcastSquadUpdate(*remaining);
}

void GameServer::removeBotFromSquad(Entity body) {
    const SquadMemberId member = SquadMemberId::ofBot(body);
    if (squads_.forMember(member) == nullptr) return;
    // Read while the body is still alive: a destroyed bot has no nameplate to
    // put in the line its squad is about to be sent.
    const std::string name = squadDisplayName(member);
    const SquadRoster::Departure departure = squads_.leave(member);
    Squad* remaining = squads_.find(departure.squadId);
    if (remaining == nullptr) return;
    if (departure.promoted.valid()) {
        sendSquadSystem(*remaining, squadDisplayName(departure.promoted) +
                                        " is now the squad leader.");
    }
    sendSquadSystem(*remaining, name + " has left the squad.");
    broadcastSquadUpdate(*remaining);
}

void GameServer::rebuildSquadIndex() {
    squadIndex_.clear();
    if (squads_.empty()) return;
    for (const auto& entry : squads_.all()) {
        std::vector<Entity> bodies;
        for (const SquadMemberId& member : entry.second.members) {
            const Entity body = squadEntity(member);
            if (body != NULL_ENTITY) bodies.push_back(body);
        }
        // One body in the world is not a pool: leaving it out keeps the
        // ordinary case -- a squad whose other members are on the title screen
        // -- ranking exactly as a solo player does.
        if (bodies.size() < 2) continue;
        const std::size_t group = squadIndex_.groups.size();
        for (const Entity body : bodies) squadIndex_.group[body] = group;
        squadIndex_.groups.push_back(std::move(bodies));
    }
}

Squad* GameServer::squadOrCreate(Session& session, net::Connection& connection) {
    const SquadMemberId me = squadIdOf(session);
    if (Squad* existing = squads_.forMember(me)) return existing;
    Squad* squad = squads_.create(me, false, rng_);
    // A squad made this way is announced nowhere else, so the client that
    // caused it would otherwise not know it exists.
    if (squad != nullptr) sendSquadUpdate(connection, squad);
    return squad;
}

void GameServer::collectSquadBodies(const Session& session, std::vector<Entity>& out) {
    out.clear();
    const SquadMemberId me = squadIdOf(session);
    const Squad* squad = squads_.forMember(me);
    if (squad == nullptr) return;
    for (const SquadMemberId& member : squad->members) {
        if (member == me) continue;
        const Entity body = squadEntity(member);
        if (body != NULL_ENTITY) out.push_back(body);
    }
}

void GameServer::handleGuildSquadAll(Session& session, net::Connection& connection) {
    if (!session.authenticated()) return;
    const std::string guildName = guildNameForUser(session.username);
    if (guildName.empty()) {
        sendNotice(connection, net::NoticeSeverity::Warning, "You are not in a guild.");
        return;
    }
    Squad* squad = squadOrCreate(session, connection);
    if (squad == nullptr || !(squad->leader == squadIdOf(session))) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "Only your squad leader can invite guildmates into the squad.");
        return;
    }

    const Json& members = database_.storedTable("guilds")[guildName]["memberUsernames"];
    const auto now = static_cast<std::int64_t>(clockMillis_);
    int invited = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const std::string member = members[i].asString();
        if (lowerCase(member) == lowerCase(session.username)) continue;
        // +1 for the invitation already in flight this iteration: an invite is
        // a claim on a seat, and the reference stops one short for it.
        if (squad->members.size() + 1 >= kMaxSquadSize) break;
        Session* target = sessionForUser(member);
        if (target == nullptr) continue;
        if (!squads_.invite(squadIdOf(session), squadIdOf(*target), session.username, now).empty()) {
            continue;
        }
        ++invited;
        if (net::Connection* peer = listener_.find(target->connection)) {
            sendSystem(*peer, "<span style=\"color: #4fc3f7;\">@" + session.username +
                                  " (guild) invited you to their squad. Use /squad-accept or "
                                  "/squad-decline.</span>");
        }
    }
    if (invited == 0) {
        sendNotice(connection, net::NoticeSeverity::Warning,
                   "No online guildmates available to invite (or squad is full).");
        return;
    }
    sendNotice(connection, net::NoticeSeverity::Good,
               "Sent squad invites to " + std::to_string(invited) + " online guildmate(s).");
}

void GameServer::handleGuildInviteToSquad(Session& session, net::Connection& connection,
                                          ByteReader& reader) {
    const std::string raw = reader.str();
    if (!reader.ok() || !session.authenticated()) return;
    guildInviteToSquad(session, connection, raw);
}

void GameServer::guildInviteToSquad(Session& session, net::Connection& connection,
                                    const std::string& raw) {
    if (!session.authenticated()) return;

    const std::string target = trimmed(raw);
    const std::string guildName = guildNameForUser(session.username);
    if (guildName.empty() ||
        guildMemberIndex(database_.storedTable("guilds")[guildName], target) < 0) {
        sendNotice(connection, net::NoticeSeverity::Warning, target + " is not in your guild.");
        return;
    }
    Session* peerSession = sessionForUser(target);
    if (peerSession == nullptr) {
        sendNotice(connection, net::NoticeSeverity::Warning, target + " is offline.");
        return;
    }
    Squad* squad = squadOrCreate(session, connection);
    if (squad == nullptr) {
        sendNotice(connection, net::NoticeSeverity::Warning, "Failed to create a squad.");
        return;
    }
    const std::string error =
        squads_.invite(squadIdOf(session), squadIdOf(*peerSession), session.username,
                       static_cast<std::int64_t>(clockMillis_));
    if (!error.empty()) {
        sendNotice(connection, net::NoticeSeverity::Warning, error);
        return;
    }
    sendNotice(connection, net::NoticeSeverity::Good, "Squad invite sent to " + target + ".");
    if (net::Connection* peer = listener_.find(peerSession->connection)) {
        sendSystem(*peer, "<span style=\"color: #4fc3f7;\">@" + session.username +
                              " (guild) invited you to their squad. Use /squad-accept or "
                              "/squad-decline.</span>");
    }
}

void GameServer::handleRespawn(Session& session) {
    if (!session.authenticated()) return;
    // A lent console is lent for one life. Dropping it here rather than on
    // death means the grantee keeps it while they are looking at the death
    // card, which is where the reference leaves it too.
    revokeTempAdmin(session.connection);
    if (session.playing() && world_.isAlive(session.entity)) {
        Health* health = world_.tryGet<Health>(session.entity);
        if (health && health->alive()) return;   // not actually dead
        despawnPlayer(session, false);
    }
    spawnPlayer(session);
}

void GameServer::handlePing(net::Connection& connection, ByteReader& reader) {
    const std::uint64_t clientTime = reader.u64();
    if (!reader.ok()) return;
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(net::ServerMessage::Pong));
    w.u64(clientTime);
    w.u64(static_cast<std::uint64_t>(monotonicMillis()));
    connection.send(w);
}

// ---------------------------------------------------------------------------
// Bots
// ---------------------------------------------------------------------------
//
// The reference keeps the world populated whether or not anyone else is
// online: it tops the flower count up to ~23 with server-owned players that
// hunt, wander, die and respawn. Without them a solo player meets an empty
// map -- no company, no competition for aggro or loot, and a leaderboard with
// one row on it.
//
// A bot here is an ORDINARY player entity with no Session behind it. That is
// the whole trick: combat, loot eligibility, replication and the death reaper
// all treat it as a flower without knowing bots exist, and the handful of
// places that need an account (banking a kill, a pickup, a persist) already
// walk the session table and simply find nothing.
//
// What lives here is the POPULATION: the target, the jitter, the burst cap,
// idle retirement, and the name-seeded level and loadout. What a bot DOES --
// the whole of src/server/botManager.ts's decision tree -- is
// server/bot_ai.cpp.


// ---------------------------------------------------------------------------
// Player lifecycle
// ---------------------------------------------------------------------------

void GameServer::collectSpawnBlockers(std::vector<MobDisc>& out) const {
    out.clear();
    Query<MobTag, Transform, Body> mobs{const_cast<World&>(world_)};
    mobs.each([&](Entity, MobTag&, Transform& transform, Body& body) {
        out.push_back({transform.position, body.radius});
    });
}

void GameServer::onPlayerRevived(Entity revived, Entity reviver) {
    (void)reviver;
    Session* session = sessionForEntity(revived);
    if (session == nullptr) return;

    // The corpse's death has already been announced and the reaper has stopped
    // looking at it. Clearing the flag is what lets this body die a second
    // time: without it the next death is silent and the player is left standing
    // as a corpse nobody told them about.
    session->deathReported = false;
    if (net::Connection* connection = listener_.find(session->connection)) {
        sendNotice(*connection, net::NoticeSeverity::Good, "A yggdrasil pulled you back up.");
    }
}

Entity GameServer::spawnPlayer(Session& session) {
    // Where a player appears is a property of the MAP, not of the player.
    // Level deliberately does not enter into it: picking a zone by tier reads
    // as if a high-level flower should start in high-tier ground, and what it
    // actually does is drop everyone into the mythic band in the middle of the
    // world, which nothing can walk out of.
    // The candidate has to be clear of the mobs standing on it, not only of
    // the walls: the reference refuses a spawn point that overlaps a body or
    // that has more than a handful of mobs within 200 units, which is what
    // stops a fresh flower materialising inside a swarm.
    std::vector<MobDisc> blockers;
    collectSpawnBlockers(blockers);

    Vec2 spawn;
    if (session.spawnBiome.empty() ||
        !mapData_.spawnInBiome(session.spawnBiome, rng_, *terrain_, spawn, &blockers)) {
        spawn = mapData_.defaultSpawn(rng_, *terrain_, &blockers);
    }

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
    // A flower's body damages mobs on contact in the TypeScript server. The
    // zero interval means every separated re-contact may land; the fixed
    // 25-unit bump normally prevents it from becoming a per-tick damage beam.
    world_.add<ContactDamage>(entity, ContactDamage{kPlayerBaseDamage, 0.0});
    world_.add<HitCooldowns>(entity);
    world_.add<Afflictions>(entity);
    world_.add<ShieldState>(entity);
    // The nameplate carries the flower's name, which is what the title screen
    // asked for; the account name stays on the session for chat and for saves.
    world_.add<PlayerAccount>(entity,
                              PlayerAccount{session.userId,
                                            session.displayName.empty() ? session.username
                                                                        : session.displayName,
                                            session.connection, session.admin});

    const PlayerRecord& record = database_.progress(session.userId);
    applyAccountToEntity(record, entity);

    // A FRESH body only: full health and the respawn window. applyAccountToEntity
    // must never do either -- see the note there -- because it also runs on
    // every loadout edit and talent purchase, corpse included.
    Health& health = world_.get<Health>(entity);
    health.current = health.max;
    health.invulnerableUntilMillis = monotonicMillis() + kRespawnInvulnerabilitySeconds * 1000.0;

    world_.add<NetId>(entity, NetId{netIds_.next()});
    Replicated replicated;
    replicated.kind = net::EntityKind::Player;
    world_.add<Replicated>(entity, replicated);

    world_.bindName(entity, "conn:" + std::to_string(session.connection));

    session.entity = entity;
    session.stage = SessionStage::Playing;
    // A fresh body has a death of its own still to announce.
    session.deathReported = false;
    // The roster carries WIRE IDS, and this player just acquired a new one.
    // Without this a squadmate's dot and party bar stay pinned to the body
    // they had before they died.
    if (const Squad* squad = squads_.forMember(squadIdOf(session))) broadcastSquadUpdate(*squad);
    return entity;
}

void GameServer::applyAccountToEntity(const PlayerRecord& record, Entity entity) {
    const LevelProgress progress = levelFromTotalXp(record.totalXp);

    PlayerProgress& state = world_.ensure<PlayerProgress>(entity);
    state.totalXp = record.totalXp;
    state.level = progress.level;
    state.stars = record.stars;

    // Cosmetic skin bits are account data. The temporary glitch bit stays on
    // PlayerVisuals and is intentionally not reset by a loadout edit.
    world_.ensure<PlayerVisuals>(entity).renderFlags = record.renderFlags;

    // The tree is copied onto the body so the tick never reaches into storage.
    // Every path that changes it -- login, respawn, buying a tier, a reset --
    // comes back through here, which is what keeps the two in step.
    world_.ensure<PlayerSkillTree>(entity).skills = record.skills;

    Body& body = world_.ensure<Body>(entity);
    body.radius = playerRadiusForLevel(progress.level);
    body.mass = 1.0;

    Health& health = world_.ensure<Health>(entity);
    const double previousFraction = health.max > 0 ? health.current / health.max : 1.0;
    health.max = maxHealthForLevel(progress.level) * record.skills.statScale(SkillId::PlayerHealth);
    // Preserve the FRACTION across a max-health change, so levelling up mid
    // fight neither heals you to full nor leaves you proportionally worse off.
    //
    // Clamped DOWNWARD only. This function also runs on every loadout edit and
    // every talent purchase, including ones sent from the death screen -- the
    // client keeps its panels live there and a corpse keeps its session. A
    // rescue that read "empty means full" would refill a dead flower's health
    // bar in front of everyone watching it, and arming respawn protection here
    // would hand out three seconds of immunity for the price of swapping two
    // empty loadout slots, over and over. Both belong to a fresh body, so both
    // live in spawnPlayer.
    health.current = clamp(health.max * clamp(previousFraction, 0.0, 1.0), 0.0, health.max);

    world_.ensure<ContactDamage>(entity).amount = bodyDamageForLevel(progress.level);
    world_.get<ContactDamage>(entity).intervalMillis = 0.0;

    Loadout& loadout = world_.ensure<Loadout>(entity);
    for (std::size_t i = 0; i < kLoadoutSlots; ++i) {
        std::uint16_t index = kNoPetal;
        Rarity rarity = Rarity::Common;
        if (i < record.loadout.size() && record.loadout[i].has_value()) {
            const StoredItem& item = *record.loadout[i];
            index = content().petalIndex(item.petalType);
            // A stored petal this build no longer has leaves the slot empty
            // rather than resolving to whatever index 0 happens to be.
            if (index == kInvalidIndex) index = kNoPetal;
            rarity = item.rarity;
        }
        loadout.slots[i].configIndex = index;
        loadout.slots[i].rarity = rarity;
        // `broken` and `reloadReadyAtMillis` are deliberately NOT touched. They
        // are the ring's own bookkeeping: the petal pass arms a full reload on
        // any slot whose contents changed and leaves an unchanged one alone, so
        // clearing them here would wipe the timer of a slot the player did not
        // touch, and would make re-equipping the same petal over one that just
        // broke a way to dodge its reload entirely.
    }
}

void GameServer::persistPlayer(const Session& session) {
    if (!session.playing() || !world_.isAlive(session.entity)) return;
    const PlayerProgress* progress = world_.tryGet<PlayerProgress>(session.entity);
    if (!progress) return;

    PlayerRecord& record = database_.progress(session.userId);
    record.totalXp = progress->totalXp;
    record.stars = progress->stars;
    database_.markDirty();
}

void GameServer::despawnPlayer(Session& session, bool persist) {
    if (session.entity == NULL_ENTITY) return;
    if (persist) persistPlayer(session);

    // The petals belong to the body, not the account, so they go with it.
    if (const Loadout* loadout = world_.tryGet<Loadout>(session.entity)) {
        for (const Entity petal : loadout->spawned) commands_.destroy(petal);
    }
    commands_.destroy(session.entity);

    session.entity = NULL_ENTITY;
    session.stage = SessionStage::Authenticated;
    views_[session.connection].reset();
    // Same reason as the spawn: the id this member was known by is gone, and a
    // roster still naming it points every squadmate's HUD at nothing.
    if (const Squad* squad = squads_.forMember(squadIdOf(session))) broadcastSquadUpdate(*squad);
}

} // namespace flix
