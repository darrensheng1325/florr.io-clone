#pragma once
// Per-connection state: the handshake, authentication, and the link between a
// socket, an account, and a player entity.
//
// A connection is deliberately NOT a player. It exists from the moment a
// socket opens; it acquires an account at login and a body at join, and it can
// drop the body (back to the title screen) without losing the account. Keeping
// those three lifetimes separate is what makes "leave to menu" and "reconnect"
// ordinary operations rather than special cases.

#include <cstdint>
#include <memory>
#include <string>

#include "server/db.h"
#include "shared/core/entity.h"
#include "shared/game/realm.h"
#include "shared/net/protocol.h"

namespace flix {

enum class SessionStage : std::uint8_t {
    /// Connected, but has not sent a compatible Hello yet. Nothing else is
    /// accepted in this state, so an incompatible or hostile client cannot
    /// reach any game logic.
    Greeting = 0,
    /// Protocol accepted; may register, log in, or resume a session.
    Anonymous,
    /// Logged in, sitting on the title screen.
    Authenticated,
    /// Has a body in the world.
    Playing,
    /// Marked for disconnect; drained and dropped at the end of the tick.
    Closing,
};

struct Session {
    net::ConnectionId connection = 0;
    SessionStage stage = SessionStage::Greeting;

    std::string userId;
    std::string username;
    std::string token;
    bool admin = false;

    /// The player's body, while Playing.
    Entity entity = NULL_ENTITY;

    /// The spawn point this connection asked to start at, from JoinGame.
    ///
    /// One of WorldMaps::spawnChoices()' ids -- a player spawn rectangle on
    /// some map -- or "pvp"/"maze" for the two realms that have no map file.
    /// Empty means the overworld's default. Kept on the session rather than
    /// passed down so a respawn lands where the player chose, not back at the
    /// default.
    std::string spawnChoice;

    /// The coordinate space the body is in while Playing (realm.h). Overworld
    /// between bodies, so a title-screen session reads as ordinary ground.
    Realm realm = Realm::Overworld;

    /// The arena run's account, while there is one.
    ///
    /// A flower in the PVP ring plays on a scratch copy of its account: a fixed
    /// starter ring, an empty inventory, no talents, and whatever it loots in
    /// there. Every handler that reads "the player's inventory or loadout"
    /// reads this instead while it is set (GameServer::liveRecord), which is
    /// how the real record is never touched by a run -- the reference's
    /// regularInventory/regularLoadout stash, turned inside out. A quarter of
    /// the run's loot reaches the real account when the run ends
    /// (src/server/playerManager.ts:99-158). Null outside the ring.
    std::unique_ptr<PlayerRecord> arena;

    /// The flower's name, typed on the title screen and sent with JoinGame.
    /// Separate from `username`: the account is who you are, this is what the
    /// nameplate says, and one player may use several. Empty means "Unnamed".
    std::string displayName;

    /// Input sequence already applied. Inputs at or below this are replays and
    /// are dropped, so a duplicated packet cannot move the player twice.
    std::uint32_t lastInputSequence = 0;

    /// Whether this body's death has already been announced. A player's corpse
    /// KEEPS its Dead tag so the client goes on being sent a dead flower, so
    /// the reaper sees it again every tick and needs to know it is done with
    /// it. Cleared when a new body is spawned.
    bool deathReported = false;

    // -- abuse limits ------------------------------------------------------
    //
    // A client controls how often it sends; the server controls how often it
    // will listen. Every counter here is refilled on a timer rather than reset
    // per tick, so a burst is absorbed but a sustained flood is not.

    double loginAttemptsAllowed = 5;
    double chatAllowance = 4;
    /// Slash commands, budgeted separately from chat. A command's output goes
    /// back to its sender, so the flood the chat budget is guarding against is
    /// not the one a command can cause -- see session.cpp.
    double commandAllowance = 12;
    double inputAllowance = 60;
    double lastRefillMillis = 0;

    /// Snapshot of what this connection knows, owned by the replicator.
    std::uint32_t viewGeneration = 0;

    double connectedAtMillis = 0;
    double lastHeardMillis = 0;

    bool authenticated() const {
        return stage == SessionStage::Authenticated || stage == SessionStage::Playing;
    }
    bool playing() const { return stage == SessionStage::Playing && entity != NULL_ENTITY; }
};

/// Refills the per-session allowances. Called once per tick for every session.
void refillAllowances(Session& session, double nowMillis);

/// Takes one unit from `allowance` if available. Returns false when the client
/// has exceeded its budget, in which case the caller drops the message.
bool spend(double& allowance, double cost = 1.0);

/// Username rules, enforced at registration. Deliberately conservative: names
/// are rendered in chat and on nameplates, so control characters, homoglyph
/// padding and unbounded length are all rejected rather than sanitised later.
bool validUsername(const std::string& name, std::string& reasonOut);
bool validPassword(const std::string& password, std::string& reasonOut);

/// Strips control characters and clamps a typed flower name to the browser
/// build's twenty characters. An empty or all-blank name becomes "Unnamed",
/// because a nameplate has to say something.
std::string sanitizePlayerName(const std::string& name);

/// Strips control characters and clamps length for anything a player typed
/// that another player will see.
std::string sanitizeChat(const std::string& text);

} // namespace flix
