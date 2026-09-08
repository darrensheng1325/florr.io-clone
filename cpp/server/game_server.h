#pragma once
// The authoritative game server: one world, one fixed-rate tick, N clients.
//
// Everything the simulation needs is owned here and passed down. Systems hold
// no global state and no references to each other -- they are given the world
// and whatever read-only services they need, and they communicate through
// components and the command buffer. That is what makes them testable in
// isolation and what keeps the tick order legible in one function.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/account_limits.h"
#include "server/bot_ai.h"
#include "server/db.h"
#include "server/replication.h"
#include "server/session.h"
#include "server/squads.h"
#include "shared/core/world.h"
#include "shared/game/components.h"
#include "shared/game/map_elements.h"
#include "shared/game/spatial.h"
#include "shared/game/terrain.h"
#include "shared/net/transport.h"

namespace flix {

class MovementSystem;
class MobAiSystem;
class PetalSystem;
class CombatSystem;
class SpawnSystem;
class LootSystem;

/// Milliseconds since the first call, from a steady clock.
///
/// The simulation's clock: every `nowMillis` a system is handed comes from
/// here, so anything that has to place itself on the same timeline -- an
/// admin-spawned mob's lifetime, a cooldown -- must read it rather than a
/// clock of its own.
double monotonicMillis();

/// Hard ceiling on the bot population, whatever the target arithmetic or an
/// admin override says. Named here rather than in game_server.cpp because the
/// console quotes it back in `/admin set_bot_count`'s usage line.
///
/// Twice the browser build's MAX_BOT_COUNT, deliberately: a bot costs this
/// server about 0.02ms of tick, so a hundred of them is around two
/// milliseconds of a thirty-three millisecond budget, and the ceiling exists
/// to stop an operator asking for something absurd rather than to hold a line
/// the machine cannot cross. The DEFAULT population is untouched -- that is
/// kBotTargetTotalPlayers, and this only bounds what `set_bot_count` may ask
/// for.
inline constexpr int kMaxBots = 100;

struct ServerConfig {
    std::uint16_t port = 3000;
    std::string dataDir = "data";
    std::string databasePath = "inventory.json";
    std::uint64_t worldSeed = 0x5EED10;
    /// Refuses connections past this; the tick cost is linear in players and
    /// the snapshot cost is worse, so this is a real limit, not a formality.
    std::size_t maxPlayers = 64;
    /// TLS material, used only by the emscripten build. With it the listener
    /// serves https and offers WebTransport alongside WebSocket; without it,
    /// plain http and WebSocket only -- WebTransport is secure-context only
    /// and cannot be offered at all. The native build speaks TCP and ignores
    /// both.
    std::string certPath;
    std::string keyPath;
    /// Directory the emscripten build serves the client from, over the same
    /// port the game itself uses. Empty means the build directory the server
    /// module was loaded from. Unused natively.
    std::string webRoot;
};

class GameServer : public net::TransportHandler {
public:
    GameServer();
    ~GameServer() override;

    /// Loads content and the database, generates the world, and binds the
    /// port. Returns false with `errorOut` set on any failure -- a server that
    /// cannot load its accounts must not start and silently serve none.
    bool start(const ServerConfig& config, std::string& errorOut);

    /// Runs until stop() is called or a signal is caught.
    void run();

    /// One pass of that loop: service the network, and tick if a tick is due.
    /// Returns false once the server is finished. run() is a loop over this;
    /// the emscripten build cannot own the loop -- blocking the Node event
    /// loop is what would stop every WebSocket message from ever arriving --
    /// and drives this from a timer callback instead.
    bool step();

    /// Flushes every playing account and the database, then drops the
    /// listener. run() does this on the way out; the emscripten build calls it
    /// when step() first returns false.
    void shutdown();

    /// The flush without the stop: every playing account is written to the
    /// database and the database to disk, and the server carries on. What
    /// the periodic save does on its own timer, and what the `save` console
    /// command and the offline page's unload handler do on demand -- a tab
    /// that is closing has no shutdown() coming, only this. Returns how many
    /// accounts were written.
    std::size_t persistAll();

    /// Safe to call from a signal handler: it only stores to an atomic flag,
    /// and the shutdown work itself happens on the main thread.
    void stop() { running_.store(false); }

    /// One fixed simulation step. Deliberately does NOT touch the network, so
    /// a test can drive the simulation deterministically without a clock.
    void tick(double nowMillis);

    /// Accepts connections and dispatches whatever has arrived, for up to
    /// `timeoutMillis`. run() interleaves this with tick(); a test drives the
    /// two itself.
    void serviceNetwork(int timeoutMillis);

    World& world() { return world_; }
    const Terrain& terrain() const { return *terrain_; }
    const MapData& mapData() const { return mapData_; }
    std::size_t playerCount() const;

    // net::TransportHandler
    void onConnect(net::Connection& c) override;
    void onMessage(net::Connection& c, ByteReader& reader) override;
    void onDisconnect(net::Connection& c, const std::string& reason) override;

private:
    // -- message handling --------------------------------------------------
    void handleHello(Session&, net::Connection&, ByteReader&);
    void handleRegister(Session&, net::Connection&, ByteReader&);
    void handleLogin(Session&, net::Connection&, ByteReader&);
    void handleResume(Session&, net::Connection&, ByteReader&);
    void handleJoin(Session&, net::Connection&, ByteReader&);
    void handleLeave(Session&, net::Connection&);
    void handleInput(Session&, ByteReader&);
    void handleChat(Session&, net::Connection&, ByteReader&);
    void handleSetLoadout(Session&, ByteReader&);
    void handleSwapLoadout(Session&, ByteReader&);
    void handleCraft(Session&, net::Connection&, ByteReader&);
    void handleRespawn(Session&);
    void handlePing(net::Connection&, ByteReader&);
    void handleUpgradeSkill(Session&, net::Connection&, ByteReader&);
    void handleResetSkills(Session&, net::Connection&);
    void handleBuyPetal(Session&, net::Connection&, ByteReader&);
    void handleRedeemCode(Session&, net::Connection&, ByteReader&);
    void handleSetSkin(Session&, net::Connection&, ByteReader&);
    void handlePublishSkin(Session&, net::Connection&, ByteReader&);
    void handleEquipSkin(Session&, net::Connection&, ByteReader&);
    void handleDeleteSkin(Session&, net::Connection&, ByteReader&);
    void handleLeaderboard(const Session&, net::Connection&);
    void handleNotifications(net::Connection&, ByteReader&);

    // -- guilds ------------------------------------------------------------
    //
    // Storage is the database's own `guilds` table, in the browser build's
    // shape: an object keyed by the upper-cased five-character name, each
    // value carrying {name, leaderUsername, memberUsernames, createdAt}. Held
    // as JSON rather than mirrored into a typed cache because the same file is
    // read by the browser build, and a second copy is a second thing to keep
    // true.
    void handleGuildCreate(Session&, net::Connection&, ByteReader&);
    void handleGuildInvite(Session&, net::Connection&, ByteReader&);
    void handleGuildAccept(Session&, net::Connection&);
    void handleGuildDecline(Session&, net::Connection&);
    void handleGuildKick(Session&, net::Connection&, ByteReader&);
    void handleGuildLeave(Session&, net::Connection&);
    void handleGuildSquadAll(Session&, net::Connection&);
    void handleGuildInviteToSquad(Session&, net::Connection&, ByteReader&);

    // The argument-taking cores behind the four guild messages that carry one.
    //
    // Split out because the same operations arrive by two roads: the guild
    // panel's binary messages, and the `/guild-create`-style chat commands the
    // reference also accepts. A second implementation of "may this player
    // invite?" would be a second answer to it.
    void guildCreate(Session&, net::Connection&, const std::string& name);
    void guildInvite(Session&, net::Connection&, const std::string& target);
    void guildKick(Session&, net::Connection&, const std::string& target);
    void guildInviteToSquad(Session&, net::Connection&, const std::string& target);

    // -- squads ------------------------------------------------------------
    //
    // The roster itself is server/squads.h; what lives here is everything that
    // needs the world or a socket -- naming a member, telling one, and keeping
    // the loot ranking's table of who fights together up to date.

    /// How this session is named inside a squad. A connection, not an entity:
    /// a body is destroyed and rebuilt on every death, and a party that lost
    /// its members each time somebody died would not be a party.
    static SquadMemberId squadIdOf(const Session& session) {
        return SquadMemberId::ofSession(session.connection);
    }
    /// The account name a member answers to. A bot has no account, so it
    /// answers to its nameplate -- which is what `/squad-invite` matches on.
    std::string squadAccountName(SquadMemberId);
    /// What this member's flower is labelled, which is what the squad's own
    /// announcements name it by.
    std::string squadDisplayName(SquadMemberId);
    Entity squadEntity(SquadMemberId);
    net::Connection* squadConnection(SquadMemberId);

    /// The roster as this client should see it. A null squad is the browser's
    /// `squadUpdate null` and carries nothing after its flag.
    void sendSquadUpdate(net::Connection&, const Squad*);
    /// Sends the roster to every human in it. Called after every membership,
    /// leadership or visibility change -- and after a member spawns or
    /// despawns, because the wire ids in it belong to bodies.
    void broadcastSquadUpdate(const Squad&);
    /// The squad's own "[Squad]" system line, to every human member.
    void sendSquadSystem(const Squad&, const std::string& text);
    /// Resolves an invite target: a signed-in player first, then a bot by
    /// nameplate, exactly as the reference resolves it.
    bool resolveSquadTarget(const std::string& name, SquadMemberId& out);
    /// Takes this session out of its squad and tells everyone concerned.
    void departSquad(Session&, net::Connection*, const std::string& leaverName);
    /// Drops a bot out of whatever squad it was in, on its way out of the
    /// world. A squad holding a destroyed body would rank a corpse for loot.
    void removeBotFromSquad(Entity body);
    /// Rebuilds the loot ranking's table of who fights together. Once a tick
    /// rather than on membership change: a member's BODY changes on every
    /// death, so a table cached against the roster goes stale without the
    /// roster ever moving.
    void rebuildSquadIndex();
    /// The caller's squad, creating a private one if they have none. Four
    /// copies of this create-then-announce dance lived across the reference's
    /// own squad commands.
    Squad* squadOrCreate(Session&, net::Connection&);
    /// Every squadmate's body, for the replicator: a squad member is streamed
    /// however far away they are, which is what makes the party HUD and the
    /// pink minimap dots work across the map.
    void collectSquadBodies(const Session&, std::vector<Entity>& out);

    // -- chat commands -----------------------------------------------------
    //
    // Implemented in server/chat_commands.cpp. A chat line beginning with '/'
    // never reaches the global channel: it is answered, refused, or reported
    // unknown, which is the difference between a command surface and a player
    // typing "/help" at everyone.

    /// True when `message` was a command -- handled, refused or unknown -- and
    /// must not be broadcast. False means an ordinary chat line.
    bool handleChatCommand(Session&, net::Connection&, const std::string& message);

    /// One `/squad ...` line, already split into its subcommand and first
    /// argument. Both spellings the reference accepts -- `/squad invite x` and
    /// `/squad-invite x` -- are rewritten into this one shape before they get
    /// here, so the two cannot drift apart.
    void runSquadCommand(Session&, net::Connection&, const std::string& sub,
                         const std::string& argument);

    /// One `/admin` (or `/cmd`) body, already stripped of its prefix. The
    /// caller has checked that this session may run it.
    void runAdminCommand(Session&, net::Connection&, const std::string& command);

    /// One System line to one connection. Command output is one line per
    /// message rather than one message with embedded newlines: the browser
    /// build joins its lines with `<br/>`, and this client has no markup.
    void sendSystem(net::Connection&, const std::string& text);

    // -- scheduled restart -------------------------------------------------
    //
    // The console's `restart`, and the last step of `update`. A restart IS a
    // process exit: pm2, systemd or docker is what actually brings the server
    // back up, exactly as it is for the browser build. Players are warned on
    // the way down, which is the whole reason it is scheduled rather than
    // immediate.

    struct ScheduledRestart {
        bool pending = false;
        /// Past the point of cancelling: the last word has been said and the
        /// process is on its way out.
        bool firing = false;
        double atMillis = 0;
        std::string reason;
        /// How many of the warning marks have already been announced. Starts
        /// past the ones a short delay skips entirely.
        std::size_t warningsSaid = 0;
        /// When the process actually stops, a second after the final notice,
        /// so it reaches the sockets before they close.
        double stopAtMillis = 0;
    };
    ScheduledRestart restart_;

    /// Schedules a restart in `delayMillis`, replacing any pending one.
    /// False when one is already firing, which cannot be called off.
    bool scheduleRestart(double delayMillis, const std::string& reason);
    bool cancelScheduledRestart();
    /// Remaining time and reason; false when nothing is scheduled.
    bool scheduledRestartInfo(double& remainingMillis, std::string& reason) const;
    /// Says the warnings that have come due and fires the restart at its
    /// moment. Called once a tick, ABOVE the idle gate: a server with nobody
    /// on it is exactly the one a restart is usually waiting for.
    void serviceScheduledRestart(double nowMillis);

    /// Forwards a running install's progress to whoever asked for it, and
    /// schedules the restart a finished one has earned. Called once a tick,
    /// beside the restart service and for the same reason.
    void serviceAutoUpdate();
    /// Who asked for the running install. A connection, not a session: the
    /// answer follows the socket, and a socket that has gone is simply not
    /// written to.
    net::ConnectionId updateRequester_ = 0;
    /// How long after a successful install its restart is scheduled for.
    double updateRestartDelayMillis_ = 60000;

    /// Rotates the maze the server is playing. Returns the line the console
    /// prints, which is the reference's own answer for each case.
    std::string adminChangeMaze(const std::string& argument);
    /// How far the active maze has been pushed from the real UTC day by
    /// `change-maze`. Reported back so an operator can see they are off it.
    std::int64_t mazeDayOffset_ = 0;

    /// Whether this session may run admin commands: a database admin, or the
    /// holder of a temporary grant. `session.admin` alone is the database flag
    /// and deliberately does not move when a grant is made.
    bool effectiveAdmin(const Session&) const;

    /// What an admin command's `<player>` argument resolved to.
    ///
    /// Bots resolve as well as people -- teleporting them is half of what the
    /// command exists for -- so the session is optional and the entity is not.
    struct CommandTarget {
        Entity entity = NULL_ENTITY;
        Session* session = nullptr;   ///< null for a bot
        std::string name;             ///< the nameplate, for output
    };
    /// Matches a live flower by nameplate or account name, case-insensitively.
    /// Accounts that are not online do not resolve here; the commands that
    /// work offline (give, mute) fall back to the database themselves.
    bool resolveCommandTarget(const std::string& identifier, CommandTarget& out);

    /// Moves a body and tells its owner, so the client cuts its interpolation
    /// instead of gliding across the map.
    void teleportEntity(Entity, Vec2);

    /// Drops a temporary admin grant, if this connection holds one. Called on
    /// respawn, on leaving to the title screen, and on disconnect -- a grant
    /// is lent for one life, as the reference lends it.
    void revokeTempAdmin(net::ConnectionId);

    void sendAuthResult(net::Connection&, net::AuthStatus, const std::string& token,
                        const std::string& username, const std::string& reason);
    void sendProfile(Session&, net::Connection&);
    /// Claims today's daily-login reward and tells the client, so the title
    /// screen's streak card has something to count down. Called BEFORE
    /// sendProfile: the claim credits stars to the record, and a profile built
    /// first would arrive one day's reward short.
    void sendDailyStreak(Session&, net::Connection&);
    void sendNotice(net::Connection&, net::NoticeSeverity, const std::string& text);
    /// The shop's own reply channel. A refused purchase or code is a modal on
    /// the shop card in the reference, not a line in the chat, so it cannot
    /// travel as a Notice.
    void sendShopResult(net::Connection&, net::ShopResultKind, bool ok, int stars,
                        const std::string& message);
    void broadcastChat(net::ChatChannel, const std::string& author, const std::string& text);
    /// One chat line to one connection, under a chosen author. The guild's own
    /// announcements are signed "[Guild NAME]" rather than "System", which a
    /// Notice -- whose author is always System -- cannot express.
    void sendChatTo(net::Connection&, net::ChatChannel, const std::string& author,
                    const std::string& text);

    /// The guild `username` belongs to, or an empty string. Searched rather
    /// than indexed: membership lives on the guild, not on the account, and a
    /// derived index would be a second thing to keep true.
    std::string guildNameForUser(const std::string& username) const;
    Session* sessionForUser(const std::string& username);
    net::Connection* connectionForUser(const std::string& username);
    /// One roster message: the guild as it stands, each member flagged online.
    void sendGuildRoster(net::Connection&, const Json& guild);
    /// The no-guild answer, which is the browser's `guildUpdate null`.
    void sendNoGuild(net::Connection&);
    /// Sends `guild` to every one of its members who is connected.
    void broadcastGuildRoster(const Json& guild);
    /// Tells this connection about its own guild, or that it has none. Sent
    /// once per authentication, and after every change that could alter it.
    void sendGuildState(const Session&, net::Connection&);
    /// The whole published-skin catalog, this client's admin flag and the skin
    /// its account is wearing. Sent once per authentication, beside the
    /// profile: a client that has not seen a skin cannot draw whoever wears it.
    void sendSkinCatalog(Session&, net::Connection&);
    /// One prebuilt message to every logged-in connection. A published or
    /// deleted skin changes what EVERY screen must be able to draw, not just
    /// the author's.
    void broadcastToAuthenticated(const ByteWriter& message);
    /// Memory and tick-time for the client debug menu's graphs, once a second.
    /// Skipped entirely while nobody is logged in, exactly as the browser
    /// server's own interval is: an idle server should send nothing.
    void broadcastDebugStats();

    // -- lifecycle ---------------------------------------------------------
    Entity spawnPlayer(Session&);
    void despawnPlayer(Session&, bool persist);
    /// Copies the live entity's progress back onto the account record. Called
    /// on leave, on death, and periodically -- a crash must not cost a session
    /// of progress.
    void persistPlayer(const Session&);
    /// Writes the account's stats, tree and loadout onto a body that already
    /// exists. Deliberately NOT a spawn: it must not heal, protect or reload
    /// anything, because it also runs on every loadout edit and talent
    /// purchase, including one sent from the death screen.
    void applyAccountToEntity(const PlayerRecord&, Entity);
    /// Credits the mob kills from this tick to every player who earned loot
    /// rights on the corpse: the gallery ledger, and the stars a mythic-or-
    /// better kill is worth.
    void bankKills();
    /// Broadcasts the "has been defeated by" line a super, unique or apex kill
    /// earns, credited to the top damage dealer on the corpse. `ranked` is the
    /// same damage-sorted ledger bankKills() paid the bounty out of.
    void announceBossDefeat(const MobType&, const std::vector<Bounty::Share>& ranked);
    /// The world half of a yggdrasil revival has already happened when this is
    /// called; the SESSION half is here -- a body whose death was announced
    /// needs that announcement retracted, or its next death is silent.
    void onPlayerRevived(Entity revived, Entity reviver);

    /// Every live mob body, for the spawn-placement tests that refuse a point
    /// standing on one. Rebuilt per call: a spawn is rare and the alternative
    /// is a cache that has to be kept true.
    void collectSpawnBlockers(std::vector<MobDisc>& out) const;

    /// Refreshes each live account's leaderboard reward tier from the ranking.
    /// Cached rather than looked up per kill, as the reference caches it: the
    /// ranking is a sort of every account and the answer changes slowly.
    void refreshRankMultipliers(double nowMillis);

    /// Drains the spawner's boss queue into chat. Worded per recipient: a
    /// player standing in the boss's own section is told it spawned, everyone
    /// else that it spawned "somewhere".
    void announceBossSpawns();

    Session* sessionFor(net::ConnectionId id);
    Session* sessionForEntity(Entity e);

    // -- bots --------------------------------------------------------------
    //
    // A world with one flower in it is not the game the reference serves: it
    // tops the population up to ~23 with server-owned flowers that fight,
    // wander and die like anyone else. They are ordinary player entities with
    // no Session behind them, which is what makes every system -- combat,
    // loot eligibility, replication, the death reaper -- treat them as players
    // without knowing they exist.
    struct Bot {
        Entity entity = NULL_ENTITY;
        std::string name;
        /// This bot's own identity for everything the reference seeds off its
        /// socket id rather than its name: its persona, its strafe direction
        /// and which of the map's farming zones it gravitates to. A bot has no
        /// id string here, so it is given a number at creation and keeps it
        /// across every death -- two bots that happen to share a NAME still
        /// play differently, which is what the reference's random ids do.
        std::uint32_t id = 0;
        /// Where the bot currently calls home, and how far it may stray from
        /// it. Both are re-derived every tick by the mode controller (raid
        /// rally point, group centroid, or its band's farming zone); they are
        /// held only so the wander and tether branches agree within a tick.
        Vec2 anchor;
        bool hasAnchor = false;
        /// Wall-clock at which a dead bot's body is replaced. A corpse that
        /// respawns instantly reads as a flower that never died.
        double respawnAtMillis = 0;
        /// Whether this body's death has already been put on the wire. A bot
        /// corpse LINGERS -- bots revive each other -- so the reaper meets it
        /// on every tick until it is replaced, and the kill event must be sent
        /// exactly once. Cleared when the body comes back up, or a yggdrasil
        /// revival would leave the next death silent.
        bool deathAnnounced = false;
        BotAiState ai;
    };

    void maintainBots(double nowMillis);
    void stepBots(double nowMillis);
    /// Builds one bot body: every component a flower needs, plus the level and
    /// loadout the NAME seeds -- so a bot called "m28" is the same build every
    /// time it appears, exactly as it is in the reference.
    Entity createBotBody(const std::string& name, Vec2 spawn);
    void destroyBot(Bot& bot);
    /// How removable a bot is; higher goes first. Squared distance to the
    /// nearest human, so an unwatched bot on the far side of the map is
    /// retired before one a player is standing next to.
    double cullScore(const Bot& bot) const;
    /// Where a bot appears. Collects the mob bodies a candidate must be clear
    /// of, which is a walk over every mob in the world -- so a caller placing
    /// SEVERAL bots must collect once and use the overload below, or a large
    /// `set_bot_count` pays that walk per bot and lands as a tick spike.
    Vec2 pickBotSpawn();
    Vec2 pickBotSpawn(const std::vector<MobDisc>& blockers);
    /// The roster entry owning this body, or null. How the reaper tells a bot
    /// apart from a flower whose connection went away: neither has a session.
    Bot* botForEntity(Entity);

    // -- bot AI ------------------------------------------------------------
    //
    // All of this is server/bot_ai.cpp. It is a port of the reference's
    // src/server/botManager.ts decision tree, and the names are that file's so
    // the two can be read side by side.

    /// One bot's decision for this tick, written into its PlayerInput.
    void stepOneBot(Bot&, double nowMillis);

    /// What a bot is doing right now, and the ground that goes with it.
    enum class BotMode : std::uint8_t { Raid, HighRarity, Normal };
    struct BotModeContext {
        BotMode kind = BotMode::Normal;
        Vec2 anchor;
        bool hasAnchor = false;
        double tetherRadius = 0;
        double returnRadius = 0;
    };
    BotModeContext computeBotMode(Bot&, double nowMillis);

    /// Per-tick indexes, built once for the whole pass rather than per bot.
    void rebuildBotBossIndex(double nowMillis);
    void computeBotGroups();
    void computeBotRaidSlots(double nowMillis);
    void updateBotSquads(double nowMillis);
    /// Bots call fresh super/unique sightings out in chat, which is also what
    /// rallies the raid -- the reference's chat trigger cannot fire for a line
    /// the server emitted itself.
    void announceNewBosses(double nowMillis);

    /// Rallies every bot onto the best boss in the world (unique over super,
    /// never ultra). Also the chat handler's entry point: someone typing
    /// "super" is how a raid usually starts.
    bool triggerBotRaid(double nowMillis);
    /// The forced rally point while one is live and its tier still exists.
    bool activeForcedRaidAnchor(double nowMillis, Vec2& out);

    // -- bot movement primitives -------------------------------------------

    /// Writes the finished heading into the bot's input: separation from other
    /// bots, repulsion from every mob body except the one being engaged, the
    /// persona's bias, and a turn-rate limit on top.
    void botDriveMove(Bot&, Vec2 direction, double speedMultiplier, double petalExtension,
                      double agility = 1.0, Entity avoidExcept = NULL_ENTITY);
    /// Stands still, petals neutral. The reference's `useMouse = false`.
    void botHold(Bot&, double petalExtension = 1.0);

    /// The reference's sampled wall raycast, sample for sample: every half
    /// tile along the segment. Deliberately NOT Terrain::segmentBlocked, whose
    /// exact walk refuses gaps this one steers through.
    bool botRayHitsWall(Vec2 from, Vec2 to) const;
    /// Probes the requested direction then progressively wider offsets, and
    /// answers with the first clear one.
    Vec2 botSteerAroundWalls(Vec2 from, Vec2 direction,
                             double probeDistance = kTileSize * 1.4) const;
    Vec2 botAvoidNearbyMobs(Vec2 at, Entity except);

    /// Follows (and lazily computes) an A* path toward a goal. False when no
    /// path is available or it is finished, so the caller falls back to the
    /// cheap steering probe.
    bool botFollowPath(Bot&, double nowMillis, Vec2 goal, double speedMultiplier,
                       double petalExtension, Entity avoidExcept = NULL_ENTITY);
    /// A* over the tile grid. Fills `out` with waypoints from the tile after
    /// the start through the goal; false when no route was found.
    bool botFindPath(Vec2 start, Vec2 goal, std::vector<Vec2>& out);
    void botClearPath(BotAiState&);

    // -- bot decisions -----------------------------------------------------

    bool botDetectOscillation(Bot&, double nowMillis);
    void botResetOscillation(Bot&, double nowMillis);
    Vec2 botPickUnstickDirection(const Bot&);
    int botTangentDirection(Bot&, double nowMillis);

    /// The best mob to fight, or NULL_ENTITY. Scored priority-first with a
    /// bonus for the tier this bot's gear says it should be farming, and a
    /// flat bonus for whatever it is already committed to.
    Entity botPickTarget(const Bot&, const BotModeContext&, double nowMillis, double& distOut);
    /// A non-target mob sitting in the bot's path close enough to body-slam.
    Entity botFindInterceptingMob(Vec2 at, Vec2 direction, Entity except, double range,
                                  double& distOut);
    Entity botFindPickup(const Bot&, const BotModeContext&, double& distOut);
    /// The closest downed bot worth diverting to revive, or NULL_ENTITY.
    Entity botFindReviveTarget(const Bot&) const;
    bool botHasNearbyBuddy(const Bot&, double range) const;
    /// The nearest boss within raid range of this bot, preferring uniques and
    /// then the most recently seen.
    bool botNearestBoss(const Bot&, Vec2& out, double& distOut);
    bool botHasHighRarityMobNearby(const Bot&, double range);
    bool botPickFarmZone(const Bot&, int rarityIndex, int rotation, Vec2& out) const;

    // -- bot loadout swaps -------------------------------------------------

    void botEquipPowder(Bot&);
    void botUnequipPowder(Bot&);
    void botEquipYggdrasil(Bot&);
    void botUnequipYggdrasil(Bot&);

    // -- bot reach ---------------------------------------------------------

    /// The farthest a petal edge can be from this bot's centre, plus the
    /// standoff buffer -- the one number the whole combat controller is built
    /// on. Derived from the ring's own geometry rather than mirrored.
    double botPetalReach(const Bot&, double petalExtension) const;
    /// Highest petal rarity across the bot's active row, which is what decides
    /// the tier it hunts and the zone it farms.
    int botMaxRarityIndex(const Bot&) const;

    /// Long-haul raid routing: hop through a teleporter when one puts the bot
    /// meaningfully closer. True when this tick is handled.
    bool botRaidShortcut(Bot&, double nowMillis, Vec2 anchor, double distToAnchor);

    // -- tick phases -------------------------------------------------------
    void runSystems(double nowMillis, double dt);

    /// Moves what the loot system handed out into the owning accounts.
    void bankPickups();
    void replicate(double nowMillis);
    void reapDead(double nowMillis);

    ServerConfig config_;
    std::atomic<bool> running_{false};

    World world_;
    CommandBuffer commands_{world_};
    std::unique_ptr<Terrain> terrain_;
    /// The map's annotation layer: which ground is the beginner's, and which
    /// rectangle is which biome. Read only when a player spawns.
    MapData mapData_;
    SpatialGrid grid_;
    Rng rng_;

    Database database_;
    /// Registration and login limits, keyed on the peer address rather than on
    /// the session -- a session is a socket, and a socket is free. See
    /// server/account_limits.h.
    AccountLimiter accountLimits_;
    net::Listener listener_;

    /// Guild invitations waiting on an answer, keyed by the lower-cased
    /// invitee. In memory only and one deep per player, exactly as the
    /// reference's `pendingGuildInvites` is: an invitation is a conversation,
    /// not a record.
    struct PendingGuildInvite {
        std::string guildName;
        std::string fromUsername;
        std::int64_t expiresAtMillis = 0;
    };
    std::unordered_map<std::string, PendingGuildInvite> guildInvites_;

    SquadRoster squads_;
    /// The flattened form the loot and XP rules read, rebuilt each tick.
    SquadEntityIndex squadIndex_;

    /// Admin consoles lent to players who are not database admins.
    ///
    /// Keyed by connection and held in memory only: a grant is for one life,
    /// so there is nothing here worth surviving a restart. See
    /// revokeTempAdmin() for the three ways one ends.
    struct TempAdminGrant {
        std::string grantedBy;
        double grantedAtMillis = 0;
    };
    std::unordered_map<net::ConnectionId, TempAdminGrant> tempAdmins_;

    /// `/admin set_bot_count`'s override, or -1 for the default formula.
    /// Negative rather than optional because -1 is already what "no override"
    /// means everywhere this is read.
    int botCountOverride_ = -1;

    std::unordered_map<net::ConnectionId, Session> sessions_;
    std::unordered_map<net::ConnectionId, ClientView> views_;

    NetIdAllocator netIds_;
    Replicator replicator_;
    EventQueue events_;

    std::unique_ptr<MovementSystem> movement_;
    std::unique_ptr<MobAiSystem> mobAi_;
    std::unique_ptr<PetalSystem> petals_;
    std::unique_ptr<CombatSystem> combat_;
    std::unique_ptr<SpawnSystem> spawning_;
    std::unique_ptr<LootSystem> loot_;

    /// Positions of every live flower, bots included, rebuilt each tick. The
    /// mob LOD counts a bot as an observer, so this is the list it gets.
    std::vector<Vec2> activePlayers_;
    /// The same, restricted to real connections. The spawner drives population
    /// and the unseen-despawn census off THIS one: bots must not each pull a
    /// neighbourhood of mobs into existence, nor keep the whole world alive.
    std::vector<Vec2> humanPlayers_;

    std::vector<Bot> bots_;
    /// Broadphase scratch for the bot controller, reused so a per-tick scan
    /// over two dozen bots does not allocate two dozen times.
    std::vector<Entity> botCandidates_;
    /// A second one, for the queries that run from INSIDE a decision that is
    /// still holding results from the first (mob avoidance runs while the
    /// combat controller holds its target's row).
    std::vector<Entity> botAvoidCandidates_;
    /// The world's boss-tier mobs, collected once per bot pass. A boss draws
    /// bots in from four thousand units away and asking the broadphase for that
    /// radius once per bot would query most of the map two dozen times a tick.
    std::vector<Entity> botBosses_;
    /// When each live boss was first seen, which is what "most recently
    /// spawned" means to a raid picker. The reference reads a spawn timestamp
    /// off the mob; nothing here carries one, and first sight is within a tick
    /// of the spawn because this pass runs every tick.
    std::unordered_map<Entity, double> botBossFirstSeen_;
    /// Bosses that have already been called out, so one is not announced twice.
    std::unordered_map<Entity, bool> botAnnouncedBosses_;
    /// Suppresses a burst of callouts for the bosses that were already alive
    /// when the first pass ran.
    bool botBossAnnounceReady_ = false;
    double botNextBossAnnounceMillis_ = 0;

    /// The chat-triggered rally: every bot converges on this point regardless
    /// of distance until it lapses or its boss dies.
    struct BotForcedRaid {
        bool active = false;
        Vec2 at;
        Rarity tier = Rarity::Super;
        double untilMillis = 0;
    };
    BotForcedRaid botForcedRaid_;

    /// This tick's bot grouping for high-rarity mode, and the angular slot each
    /// raider owns around its boss. Rebuilt per tick: the centroids follow the
    /// group as it moves, and the slots are redealt as raiders join and die.
    struct BotGroup {
        Vec2 centre;
        int size = 0;
    };
    std::unordered_map<Entity, BotGroup> botGroups_;
    std::unordered_map<Entity, double> botRaidSlots_;
    /// Scratch for both of the above, so the per-tick rebuild allocates
    /// nothing once the population has settled.
    std::vector<std::size_t> botOrderScratch_;

    BotPathScratch botPath_;
    /// How many A* recomputes are left this tick. A whole raid replanning
    /// together would otherwise spike the frame.
    int botPathBudget_ = 0;
    /// Wall-clock of the last tick with a human in the world. Bots outlive an
    /// empty server by a grace period so a quick reconnect does not land in a
    /// world that was just emptied.
    double lastHumanSeenMillis_ = 0;
    double nextBotMaintainMillis_ = 0;
    double nextBotJitterMillis_ = 0;
    int botCountJitter_ = 0;

    /// Simulation delta, low-pass filtered over the real elapsed time between
    /// ticks and clamped to three nominal steps.
    ///
    /// Driving the dt-scaled half off a constant 1/30 makes a flower's real
    /// speed scale with however fast the server is actually ticking, so it
    /// crawls under load; feeding the raw sample straight in makes each tick
    /// advance an uneven amount and the flower stutters. The filter keeps the
    /// speed honest and the motion smooth, and the clamp keeps a long stall
    /// from producing one giant step.
    ///
    /// When the next fixed step is due, in the monotonic clock. A member
    /// rather than a local in run(), because the loop is no longer
    /// necessarily this object's.
    double nextTickMillis_ = 0;
    /// Only run() writes it, so a test driving tick() by hand still gets an
    /// exactly fixed step.
    double smoothedDeltaSeconds_ = net::kTickSeconds;
    double lastTickWallMillis_ = 0;

    /// How long tick() itself took, drained once a second into a DebugStats
    /// broadcast. The mean says what the server costs at rest; the worst
    /// single tick of the window is the one that shows up as a stutter, and
    /// averaging it away would hide exactly the thing the graph is for.
    double debugTickAccumMillis_ = 0;
    double debugTickMaxMillis_ = 0;
    int debugTickSamples_ = 0;
    double nextDebugStatsMillis_ = 0;

    /// When the next snapshot is due. The wire runs slower than the
    /// simulation: physics wants 30 Hz resolution, clients do not, and the
    /// per-recipient encode/cull/delta pass is the most expensive thing in the
    /// tick that nothing simulated depends on.
    double nextSnapshotMillis_ = 0;

    double nextRankRefreshMillis_ = 0;

    /// The clock every deadline this class owns is measured against: the
    /// `nowMillis` the last tick was given.
    ///
    /// NOT monotonicMillis() directly. tick() is handed its time by the caller
    /// -- run() passes the monotonic clock, a test passes a synthetic one --
    /// and a squad invite stamped from one clock while the expiry sweep reads
    /// the other is an invite that never lapses, or one that lapses at once.
    double clockMillis_ = 0;

    std::uint32_t tick_ = 0;
    double nextPersistMillis_ = 0;
    ByteWriter scratch_;
};

} // namespace flix
