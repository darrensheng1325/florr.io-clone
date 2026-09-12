// What the bots actually DO, measured rather than watched.
//
// A bot controller is a pile of local decisions, and every one of them can be
// defensible while the crowd they add up to reads as broken -- a field of
// flowers marching to one coordinate, or shoving through a mob without ever
// hitting it. The only way to tell is to run the real server for a few
// simulated minutes and count. This boots exactly that (server/game_server.h,
// real bots, real mobs, real terrain, one real client on loopback so the tick
// gate opens) and prints one block of numbers.
//
// The numbers, and the complaint each one answers:
//
//   engaged   ticks with a mob inside petal reach -- "they don't fight"
//   ram       ticks with a mob body OVERLAPPING the bot's -- "they run into mobs"
//   kills     mobs that died near a bot -- whether the fighting accomplishes anything
//   hp        mean health fraction -- whether they are being chewed on
//   cells     distinct 600-unit cells each bot visited -- "they repeat a lot"
//   straight  net displacement over path length, per 10s window -- pacing detector
//   spacing   mean nearest-other-bot distance -- "they all pile onto one spot"
//   spread    standard deviation of the population's position
//
// A world stocked at the reference's density is what the second half of this
// answers. The shipped map's bands are enormous and the spawner recycles
// anything outside a player's own viewport, so the live overworld carries a
// fraction of a mob per screen -- which caps what ANY controller can be
// measured doing. `dense` mode therefore keeps mobs coming in around the bot
// population through the admin console, which is the same path an operator
// uses, and measures the fighting rather than the walking.
//
// Usage: bot_probe [seconds] [bots] [dense]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../tests/server_harness.h"
#include "server/db.h"
#include "shared/game/components.h"
#include "shared/game/config.h"

using namespace flix;
using namespace flix::testsupport;

namespace {

constexpr double kCellSize = 600.0;
/// The window the straightness ratio is measured over: long enough that an
/// orbit closes, short enough that a purposeful walk still scores high.
constexpr int kWindowTicks = 300;

struct BotTrace {
    Vec2 last;
    bool started = false;
    double pathLength = 0;
    Vec2 windowStart;
    double windowPath = 0;
    int windowTicks = 0;
    double straightSum = 0;
    int straightCount = 0;
    std::unordered_set<std::uint64_t> cells;
    long engagedTicks = 0;
    long ramTicks = 0;
    long passedTicks = 0;
    long aliveTicks = 0;
    double hpSum = 0;
    long deaths = 0;
    bool wasDead = false;
    long idleTicks = 0;
    long reversals = 0;
    long passedClaimedTicks = 0;
    double lastMoveAngle = 0;
    bool hasMoveAngle = false;
};

/// An admin account, seeded before the server opens the database. `dense` mode
/// needs the console, and the console needs an admin.
void seedAdmin(const std::string& path) {
    Database db;
    std::string error;
    db.load(path, error);
    db.setPasswordCost(4);   // the default cost makes this the slowest part
    CreateResult created = db.createUser("probeboss", "password7");
    if (created.ok()) created.account->admin = true;
    db.markDirty();
    db.save();
}

/// The roster `dense` mode tops the world up with: ordinary garden fare, at a
/// tier a mid-level flower has to actually fight rather than brush past.
constexpr const char* const kDenseMobs[] = {"bee", "ant", "hornet", "spider", "ladybug"};
constexpr const char* const kDenseRarities[] = {"rare", "epic", "legendary"};

std::uint64_t cellKey(Vec2 at) {
    const auto x = static_cast<std::int32_t>(std::floor(at.x / kCellSize));
    const auto y = static_cast<std::int32_t>(std::floor(at.y / kCellSize));
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
           static_cast<std::uint32_t>(y);
}

} // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 120.0;
    const int bots = argc > 2 ? std::atoi(argv[2]) : 24;
    const bool dense = argc > 3 && std::string(argv[3]) == "dense";

    Harness h("bot-probe", dense ? seedAdmin : std::function<void(const std::string&)>{},
              dataDir(), bots);
    if (!h.ready) {
        std::fprintf(stderr, "bot_probe: could not start a server\n");
        return 1;
    }

    NetClient client;
    if (dense) {
        if (!connectClient(h, client)) {
            std::fprintf(stderr, "bot_probe: could not connect the watcher\n");
            return 1;
        }
        client.requestLogin("probeboss", "password7");
        if (!h.stepUntil({&client},
                         [&] { return client.status() == NetClient::Status::LoggedIn; })) {
            std::fprintf(stderr, "bot_probe: could not log the admin in\n");
            return 1;
        }
    } else if (!loginNew(h, client, "probewatcher", "hunter22")) {
        std::fprintf(stderr, "bot_probe: could not log a watcher in\n");
        return 1;
    }
    client.joinGame(1280, 720, {}, dense ? "probeboss" : "probewatcher");
    if (!h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; })) {
        std::fprintf(stderr, "bot_probe: the watcher never reached the world\n");
        return 1;
    }
    // Let the population fill before anything is counted.
    h.step(300, {&client});

    World& world = h.server.world();
    std::unordered_map<Entity, BotTrace> traces;
    std::unordered_map<Entity, double> mobHealth;
    std::unordered_map<Entity, double> mobHealthFraction;
    // Where each mob was last seen, and whether a bot was standing on top of
    // it. A high-level bot one-shots a common: the mob is at full health on
    // one sample and gone on the next, so neither a health drop nor a low-
    // health disappearance can see the kill. Being next to a flower when you
    // vanish is what distinguishes it from the despawn sweep.
    std::unordered_map<Entity, bool> mobNearBot;
    std::unordered_set<Entity> seenMobs;
    double damageToMobs = 0;
    long mobDeaths = 0;
    double nearestMobSum = 0;
    long nearestMobSamples = 0;
    double mobToHumanSum = 0;
    long mobToHumanSamples = 0;
    double onScreenSum = 0;
    double mobsOnScreenSum = 0;
    long clumpSamples = 0;
    double spacingSum = 0;
    double spreadSum = 0;
    double mobCountSum = 0;
    double mobsNearSum = 0;
    double watcherDistSum = 0;
    long censusSamples = 0;

    const int ticks = static_cast<int>(seconds * 1000.0 / net::kTickMillis);
    std::vector<Entity> botList;
    std::vector<Vec2> botAt;
    std::vector<Vec2> humanAt;
    std::vector<Entity> mobList;
    std::vector<Vec2> mobAt;
    std::vector<double> mobRadius;

    Rng probe{0xB07};
    for (int tick = 0; tick < ticks; ++tick) {
        h.step(1, {&client});

        // What the population is DOING, straight out of the console the
        // operator uses. Sampled rather than derived: an activity is a
        // decision, and nothing about it is visible in the world.
        if (dense && tick % 30 == 15) client.sendChat("/admin bots");

        // Dense mode: keep mobs arriving on top of the population, so what is
        // being measured is how the bots FIGHT rather than how long they spend
        // looking for something to fight.
        if (dense && tick % 12 == 0 && !botList.empty()) {
            const Vec2 at = botAt[probe.below(static_cast<std::uint32_t>(botAt.size()))] +
                            probe.insideCircle(700.0);
            client.sendChat(std::string("/admin spawn ") +
                            kDenseMobs[probe.below(std::size(kDenseMobs))] + " " +
                            kDenseRarities[probe.below(std::size(kDenseRarities))] + " " +
                            std::to_string(static_cast<int>(at.x)) + " " +
                            std::to_string(static_cast<int>(at.y)) + " 6");
        }

        botList.clear();
        botAt.clear();
        humanAt.clear();
        Query<PlayerTag, PlayerAccount, Transform> players{world};
        players.each([&](Entity e, PlayerTag&, PlayerAccount& account, Transform& transform) {
            if (!account.userId.empty()) { humanAt.push_back(transform.position); return; }
            botList.push_back(e);
            botAt.push_back(transform.position);
        });

        mobList.clear();
        mobAt.clear();
        mobRadius.clear();
        std::unordered_set<Entity> liveMobs;
        Query<MobTag, Transform, Body> mobs{world};
        mobs.each([&](Entity e, MobTag&, Transform& transform, Body& body) {
            if (world.has<Pet>(e)) return;
            // The overworld only. The arena's crowd and the maze's corridors
            // are populated whole by ModeSpawner, in their own coordinate
            // spaces, and counting them makes an empty overworld look busy.
            if (transform.realm != Realm::Overworld) return;
            liveMobs.insert(e);
            // Damage, tracked by watching health rather than by catching a
            // death: a mob that dies is marked Dead and reaped inside the same
            // tick, so there is no frame in which a probe can see the corpse.
            if (const Health* health = world.tryGet<Health>(e)) {
                const auto seen = mobHealth.find(e);
                if (seen != mobHealth.end() && health->current < seen->second) {
                    damageToMobs += seen->second - health->current;
                }
                mobHealth[e] = health->current;
                mobHealthFraction[e] = health->fraction();
            }
            mobNearBot[e] = false;
            if (world.has<Dead>(e)) return;
            mobList.push_back(e);
            mobAt.push_back(transform.position);
            mobRadius.push_back(body.radius);
        });
        // Mark every mob a bot could currently be hitting, so that when one
        // vanishes the reason is known.
        for (std::size_t m = 0; m < mobList.size(); ++m) {
            for (const Vec2 a : botAt) {
                if ((mobAt[m] - a).lengthSq() < 300.0 * 300.0) {
                    mobNearBot[mobList[m]] = true;
                    break;
                }
            }
        }
        // A mob that vanished while nearly dead, or while a bot was on top of
        // it, was killed; one that vanished at full health in an empty field
        // was recycled by the unseen-despawn sweep.
        for (auto it = mobHealth.begin(); it != mobHealth.end();) {
            if (liveMobs.count(it->first) != 0) { ++it; continue; }
            if (mobHealthFraction[it->first] < 0.45 || mobNearBot[it->first]) ++mobDeaths;
            mobHealthFraction.erase(it->first);
            mobNearBot.erase(it->first);
            it = mobHealth.erase(it);
        }

        // Per-bot sampling.
        for (std::size_t i = 0; i < botList.size(); ++i) {
            const Entity bot = botList[i];
            BotTrace& trace = traces[bot];
            const Vec2 at = botAt[i];
            const bool dead = world.has<Dead>(bot);
            if (dead) {
                if (!trace.wasDead) ++trace.deaths;
                trace.wasDead = true;
                trace.started = false;
                continue;
            }
            trace.wasDead = false;
            ++trace.aliveTicks;

            if (const Health* health = world.tryGet<Health>(bot)) {
                trace.hpSum += health->fraction();
            }
            const double bodyRadius =
                world.has<Body>(bot) ? world.get<Body>(bot).radius : kPlayerBaseRadius;

            // Reach: the ring at full attack extension, which is what the
            // controller parks on. Approximated the same way for every bot so
            // the number compares across runs.
            constexpr double kReach = 155.0;
            bool engaged = false;
            bool ramming = false;
            bool engagedUnclaimed = false;
            double nearestMob = 1e18;
            for (std::size_t m = 0; m < mobList.size(); ++m) {
                const double dist = (mobAt[m] - at).length();
                nearestMob = std::min(nearestMob, dist - bodyRadius - mobRadius[m]);
                if (dist < bodyRadius + mobRadius[m] + kReach) {
                    engaged = true;
                    // A mob a human is standing over is deliberately left
                    // alone (kBotPlayerClaimRadius), so a bot walking past one
                    // is the rule working rather than the rule failing.
                    bool claimed = false;
                    for (const Vec2 hp : humanAt) {
                        if ((mobAt[m] - hp).lengthSq() < 1500.0 * 1500.0) { claimed = true; break; }
                    }
                    if (!claimed) engagedUnclaimed = true;
                }
                if (dist < bodyRadius + mobRadius[m]) ramming = true;
            }
            if (nearestMob < 1e17) {
                nearestMobSum += nearestMob;
                ++nearestMobSamples;
            }
            if (engaged) ++trace.engagedTicks;
            if (ramming) ++trace.ramTicks;
            // Standing next to something and not fighting it. THE symptom the
            // rewrite is for: a bot that walks through a field of mobs with
            // its petals tucked in is a bot that looks broken, whether or not
            // the collision ever happens.
            if (engaged) {
                const PlayerInput* input = world.tryGet<PlayerInput>(bot);
                if (input == nullptr || !input->current.attacking()) {
                    ++trace.passedTicks;
                    if (!engagedUnclaimed) ++trace.passedClaimedTicks;
                }
            }

            // Standing still, and hard reversals. Both are pacing tells: a
            // bot that stops dead and turns around is the single most
            // recognisable thing a bad controller does.
            if (const PlayerInput* input = world.tryGet<PlayerInput>(bot)) {
                if (input->current.moveStrength <= 0.01) {
                    ++trace.idleTicks;
                } else {
                    if (trace.hasMoveAngle) {
                        const double delta =
                            std::fabs(wrapAngle(input->current.moveAngle - trace.lastMoveAngle));
                        if (delta > 2.0) ++trace.reversals;
                    }
                    trace.lastMoveAngle = input->current.moveAngle;
                    trace.hasMoveAngle = true;
                }
            }

            trace.cells.insert(cellKey(at));
            if (trace.started) {
                const double step = (at - trace.last).length();
                trace.pathLength += step;
                trace.windowPath += step;
            } else {
                trace.windowStart = at;
                trace.windowPath = 0;
                trace.windowTicks = 0;
            }
            trace.last = at;
            trace.started = true;
            if (++trace.windowTicks >= kWindowTicks) {
                if (trace.windowPath > 1.0) {
                    trace.straightSum += (at - trace.windowStart).length() / trace.windowPath;
                    ++trace.straightCount;
                }
                trace.windowStart = at;
                trace.windowPath = 0;
                trace.windowTicks = 0;
            }
        }

        // Population context, once a second: how many mobs the world is
        // holding, how many of them are anywhere near a bot, and how far the
        // crowd has walked from the one human. Ambient mobs are stocked around
        // PLAYERS, so a bot that walks out of the watcher's neighbourhood is
        // walking into an empty map -- which reads as "the bots never fight"
        // and is not the controller's fault.
        if (tick % 30 == 0 && !botList.empty()) {
            Vec2 watcher{0, 0};
            bool haveWatcher = false;
            Query<PlayerTag, PlayerAccount, Transform> humans{world};
            humans.each([&](Entity, PlayerTag&, PlayerAccount& account, Transform& transform) {
                if (account.userId.empty()) return;
                watcher = transform.position;
                haveWatcher = true;
            });
            double near = 0;
            for (std::size_t m = 0; m < mobList.size(); ++m) {
                for (const Vec2 a : botAt) {
                    if ((mobAt[m] - a).lengthSq() < 2000.0 * 2000.0) { ++near; break; }
                }
            }
            double watcherDist = 0;
            if (haveWatcher && !botAt.empty()) {
                for (const Vec2 a : botAt) watcherDist += (a - watcher).length();
                watcherDist /= static_cast<double>(botAt.size());
            }
            if (haveWatcher && !mobAt.empty()) {
                double toHuman = 0;
                for (const Vec2 m : mobAt) toHuman += (m - watcher).length();
                mobToHumanSum += toHuman / static_cast<double>(mobAt.size());
                ++mobToHumanSamples;
            }
            if (haveWatcher) {
                int mobsOnScreen = 0;
                for (const Vec2 m : mobAt) {
                    if (std::fabs(m.x - watcher.x) < 960.0 &&
                        std::fabs(m.y - watcher.y) < 540.0) {
                        ++mobsOnScreen;
                    }
                }
                mobsOnScreenSum += mobsOnScreen;
                int onScreen = 0;
                for (const Vec2 a : botAt) {
                    if (std::fabs(a.x - watcher.x) < 960.0 &&
                        std::fabs(a.y - watcher.y) < 540.0) {
                        ++onScreen;
                    }
                }
                onScreenSum += onScreen;
            }
            mobCountSum += static_cast<double>(mobList.size());
            mobsNearSum += near;
            watcherDistSum += watcherDist;
            ++censusSamples;
        }

        // Crowding, once a second.
        if (tick % 30 == 0 && botList.size() > 1) {
            double nearestSum = 0;
            Vec2 mean{0, 0};
            for (const Vec2 a : botAt) mean += a;
            mean = mean / static_cast<double>(botAt.size());
            double variance = 0;
            for (std::size_t i = 0; i < botAt.size(); ++i) {
                double nearest = 1e18;
                for (std::size_t j = 0; j < botAt.size(); ++j) {
                    if (i == j) continue;
                    nearest = std::min(nearest, (botAt[j] - botAt[i]).length());
                }
                nearestSum += nearest;
                variance += (botAt[i] - mean).lengthSq();
            }
            spacingSum += nearestSum / static_cast<double>(botAt.size());
            spreadSum += std::sqrt(variance / static_cast<double>(botAt.size()));
            ++clumpSamples;
        }
    }

    long aliveTicks = 0;
    long engagedTicks = 0;
    long ramTicks = 0;
    long passedTicks = 0;
    long passedClaimedTicks = 0;
    long deaths = 0;
    double hpSum = 0;
    double straight = 0;
    int straightCount = 0;
    double cells = 0;
    double pathLength = 0;
    long idleTicks = 0;
    long reversals = 0;
    for (const auto& entry : traces) {
        const BotTrace& trace = entry.second;
        aliveTicks += trace.aliveTicks;
        engagedTicks += trace.engagedTicks;
        ramTicks += trace.ramTicks;
        passedTicks += trace.passedTicks;
        passedClaimedTicks += trace.passedClaimedTicks;
        deaths += trace.deaths;
        hpSum += trace.hpSum;
        straight += trace.straightSum;
        straightCount += trace.straightCount;
        cells += static_cast<double>(trace.cells.size());
        idleTicks += trace.idleTicks;
        reversals += trace.reversals;
        pathLength += trace.pathLength;
    }
    const double bodies = std::max<std::size_t>(1, traces.size());
    const double alive = std::max(1L, aliveTicks);

    std::printf("\n--- bot_probe: %g simulated seconds, %d bots ----------------\n", seconds,
                bots);
    std::printf("  engaged   %5.1f%%   (a mob inside petal reach)\n",
                100.0 * static_cast<double>(engagedTicks) / alive);
    std::printf("  ram       %5.1f%%   (a mob body overlapping the bot's)\n",
                100.0 * static_cast<double>(ramTicks) / alive);
    std::printf("  ignored   %5.1f%%   of in-reach ticks spent NOT attacking"
                " (%0.1f%% of those the player's own mobs)\n",
                engagedTicks > 0 ? 100.0 * static_cast<double>(passedTicks) /
                                       static_cast<double>(engagedTicks)
                                 : 0.0,
                passedTicks > 0 ? 100.0 * static_cast<double>(passedClaimedTicks) /
                                      static_cast<double>(passedTicks)
                                : 0.0);
    std::printf("  kills     %5ld     mobs killed (vanished while nearly dead)\n", mobDeaths);
    std::printf("  damage    %5.0f     total health taken off mobs\n", damageToMobs);
    if (nearestMobSamples > 0) {
        std::printf("  nearest   %5.0f     mean gap from a bot to the closest mob\n",
                    nearestMobSum / nearestMobSamples);
    }
    std::printf("  hp        %5.1f%%   mean health\n", 100.0 * hpSum / alive);
    std::printf("  deaths    %5ld\n", deaths);
    std::printf("  cells     %5.1f     distinct 600-unit cells per bot\n", cells / bodies);
    std::printf("  travel    %5.0f     units walked per bot\n", pathLength / bodies);
    std::printf("  idle      %5.1f%%   of ticks standing still\n",
                100.0 * static_cast<double>(idleTicks) / alive);
    std::printf("  reversals %5.1f     hard turns (>115 deg) per bot per minute\n",
                static_cast<double>(reversals) / bodies / (seconds / 60.0));
    std::printf("  straight  %5.2f     net/path over %ds windows (1 = a straight line)\n",
                straightCount > 0 ? straight / straightCount : 0.0,
                kWindowTicks / 30);
    if (clumpSamples > 0) {
        std::printf("  spacing   %5.0f     mean nearest-other-bot distance\n",
                    spacingSum / clumpSamples);
        std::printf("  spread    %5.0f     stdev of the population's position\n",
                    spreadSum / clumpSamples);
    }
    if (censusSamples > 0) {
        std::printf("  mobs      %5.0f     alive in the world (%0.0f within 2000 of a bot)\n",
                    mobCountSum / censusSamples, mobsNearSum / censusSamples);
        std::printf("  toHuman   %5.0f     mean bot distance from the one human\n",
                    watcherDistSum / censusSamples);
        std::printf("  onScreen  %5.1f     bots inside the human's 1920x1080 viewport\n",
                    onScreenSum / censusSamples);
        std::printf("  mobsSeen  %5.1f     MOBS inside that same viewport\n",
                    mobsOnScreenSum / censusSamples);
        if (mobToHumanSamples > 0) {
            std::printf("  mobRange  %5.0f     mean MOB distance from the one human\n",
                        mobToHumanSum / mobToHumanSamples);
        }
    }
    // A snapshot of where everything ended up, for a probe run that wants to
    // be looked at rather than compared. `BOT_PROBE_DUMP=<path>` writes one
    // CSV row per body: kind, x, y.
    if (dense) {
        // The last few activity lines the console sent back.
        int shown = 0;
        for (auto it = client.chat().rbegin(); it != client.chat().rend() && shown < 4; ++it) {
            if (it->text.find(" alive:") == std::string::npos) continue;
            std::printf("  activity  %s\n", it->text.c_str());
            ++shown;
        }
    }
    if (const char* dump = std::getenv("BOT_PROBE_DUMP")) {
        if (std::FILE* out = std::fopen(dump, "w")) {
            std::fprintf(out, "kind,x,y\n");
            Query<PlayerTag, PlayerAccount, Transform> players{world};
            players.each([&](Entity, PlayerTag&, PlayerAccount& account, Transform& transform) {
                std::fprintf(out, "%s,%.1f,%.1f\n",
                             account.userId.empty() ? "bot" : "human", transform.position.x,
                             transform.position.y);
            });
            Query<MobTag, Transform> allMobs{world};
            allMobs.each([&](Entity e, MobTag&, Transform& transform) {
                if (world.has<Pet>(e) || transform.realm != Realm::Overworld) return;
                std::fprintf(out, "mob,%.1f,%.1f\n", transform.position.x, transform.position.y);
            });
            std::fclose(out);
            std::printf("  dumped positions to %s\n\n", dump);
        }
    }
    std::printf("\n");
    return 0;
}
