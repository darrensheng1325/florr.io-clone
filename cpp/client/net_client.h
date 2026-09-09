#pragma once
// The client's connection to a server.
//
// Owns the socket, the handshake, and the mapping from server messages to
// client state. Everything it learns lands in a WorldView or in one of the
// fields below; nothing in the renderer talks to a socket.

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "client/world_view.h"
#include "shared/game/config.h"
#include "shared/game/skin_format.h"
#include "shared/game/skills.h"
#include "shared/game/terrain.h"
#include "shared/net/protocol.h"
#include "shared/net/transport.h"

namespace flix {

/// The account state the server sends after login, and keeps up to date.
struct Profile {
    std::string username;
    double totalXp = 0;
    int level = 1;
    int stars = 0;

    struct Stack {
        std::uint16_t petalIndex = 0;
        Rarity rarity = Rarity::Common;
        std::uint32_t count = 0;
    };
    std::vector<Stack> inventory;

    struct Slot {
        std::uint16_t petalIndex = 0xFFFF;
        Rarity rarity = Rarity::Common;
        bool empty() const { return petalIndex == 0xFFFF; }
    };
    std::vector<Slot> loadout;

    /// The cosmetic bit currently worn, or PlayerRenderNone.
    std::uint32_t renderFlags = 0;

    SkillSet skills;

    /// Kills per mob and tier, flattened as `mobIndex * kRarityCount + tier`.
    /// A dense grid rather than a map: the gallery reads every cell of it once
    /// per frame, and there are only a few hundred.
    std::vector<std::uint32_t> mobKills;

    std::uint32_t killCount(std::uint16_t mobIndex, Rarity rarity) const {
        const std::size_t at = static_cast<std::size_t>(mobIndex) * kRarityCount + rarityIndex(rarity);
        return at < mobKills.size() ? mobKills[at] : 0;
    }

    /// Unspent talent points. Derived from the level exactly as the server
    /// derives it, so the panel never shows a balance the server would refuse.
    int talentPoints() const { return availableTalentPoints(level, skills); }

    /// How many of one petal at one tier the account holds.
    std::uint32_t stackCount(std::uint16_t petalIndex, Rarity rarity) const {
        for (const Stack& stack : inventory) {
            if (stack.petalIndex == petalIndex && stack.rarity == rarity) return stack.count;
        }
        return 0;
    }
};

/// One row of the account leaderboard.
struct LeaderboardRow {
    std::string name;
    int level = 1;
    double totalXp = 0;
};

/// One entry of the global notification feed.
struct NotificationEntry {
    /// The server's own id string. It is what the read set is keyed on, so it
    /// travels verbatim rather than being re-derived from the timestamp.
    std::string id;
    net::NotificationKind kind = net::NotificationKind::Generic;
    std::string message;
    /// Unix milliseconds, so "3 days ago" can be measured against a wall clock
    /// rather than against this process's uptime.
    double timestampMillis = 0;
};

/// The player's guild, as the last GuildUpdate described it.
///
/// `joined` false is the browser's `guildUpdate null`: the panel's no-guild
/// view, not merely "not fetched yet".
struct GuildState {
    bool joined = false;
    std::string name;
    std::string leader;
    std::vector<std::string> members;
    /// The subset of `members` currently connected. Kept as names rather than
    /// as flags on the member list because that is the shape the panel sorts by.
    std::vector<std::string> online;
};

/// The player's squad, as the last SquadUpdate described it.
///
/// `inSquad` false is the browser's `squadUpdate null`. The wire ids are what
/// the minimap and the party HUD find the members' bodies by; a member with no
/// body just now -- sitting on the title screen, or looking at a death card --
/// carries a zero, and both surfaces simply skip them.
struct SquadState {
    struct Member {
        std::string account;    ///< the account name, or a bot's nameplate
        std::string name;       ///< what the flower is labelled
        std::uint32_t netId = 0;
        bool leader = false;
        bool bot = false;
    };

    bool inSquad = false;
    std::string id;
    bool isPublic = false;
    std::vector<Member> members;

    /// True when `netId` belongs to a squadmate. Linear over at most four
    /// entries, which is cheaper than the set that would replace it.
    bool contains(std::uint32_t netId) const {
        if (!inSquad || netId == 0) return false;
        for (const Member& member : members) {
            if (member.netId == netId) return true;
        }
        return false;
    }
};

/// A guild invitation waiting on an answer.
struct GuildInvite {
    bool waiting = false;
    std::string guildName;
    std::string fromUsername;
    /// Raised when an invite lands, so the menu system can force the panel
    /// open once and then clear it. Separate from `waiting`, which stays set
    /// for as long as the invite is unanswered.
    bool justArrived = false;
};

/// The outcome of the last craft, for the crafting panel's result animation.
struct CraftOutcome {
    bool pending = false;      ///< a result arrived that the panel has not read
    bool success = false;
    std::uint16_t petalIndex = 0;
    Rarity rarity = Rarity::Common;
    /// How many upgrades the whole staged pool produced. The panel's result
    /// caption counts these, so a staged x3 that landed twice reads "x2".
    int crafted = 0;
    /// The sub-batch tail the pool could not spend, 0-4. The ring keeps this
    /// many slots filled when nothing was crafted.
    int petalsReturned = 0;
    std::string reason;
};

/// The shop's own reply channel: what came back from a purchase or a code.
///
/// Separate from the notice stream because the reference answers both in a
/// modal on the shop card -- "Purchase failed: ..." / "Code redemption failed:
/// ..." -- and a chat line is not that.
struct ShopOutcome {
    bool pending = false;      ///< a result arrived that the panel has not read
    bool redeem = false;       ///< false for a purchase
    bool ok = false;
    int stars = 0;             ///< what a redeemed code paid
    std::string message;
};

struct ChatLine {
    net::ChatChannel channel = net::ChatChannel::Global;
    std::string author;
    std::string text;
    double receivedAtMillis = 0;
    /// Unix milliseconds, for the "[3:04:05 PM]" stamp the transcript prints.
    /// Separate from `receivedAtMillis`, which is monotonic uptime and so
    /// cannot be turned into a wall-clock time of day.
    std::int64_t wallClockMillis = 0;
};

/// The daily-login streak, as the server computed it at authentication. The
/// title screen's streak card is the only reader; it counts down to the two
/// timestamps, which are Unix millis and so are comparable with the wall clock
/// rather than with the app's own uptime.
struct DailyStreak {
    /// False until an authentication has answered. The card does not paint
    /// before then, because "Day 0" is not a thing it can say.
    bool known = false;
    int streak = 0;
    /// True when this login is the one that claimed today. Latched for the
    /// session, exactly as the browser build latches it, so the card keeps its
    /// brighter star until the next login.
    bool newDay = false;
    int starsAwarded = 0;
    std::int64_t nextClaimAtMillis = 0;
    std::int64_t streakExpiresAtMillis = 0;
};

class NetClient : public net::TransportHandler {
public:
    enum class Status {
        Offline,
        Connecting,
        /// Socket up, waiting for the server to accept our protocol version.
        Handshaking,
        /// Protocol accepted; may log in.
        Ready,
        LoggedIn,
        Playing,
        Failed,
    };

    NetClient();
    ~NetClient() override;

    bool connect(const std::string& host, std::uint16_t port);
    void disconnect();

    /// True while a dropped socket is waiting to be redialled.
    ///
    /// A server restart is what this exists for: the process goes away, every
    /// socket closes, and a client that did nothing about it would sit on a
    /// dead connection until somebody refreshed the page. The redial is the
    /// browser socket's own -- one second, times 1.5 per failure, capped at
    /// ten, reset the moment a handshake lands.
    bool reconnecting() const {
        // Both halves of the wait: the pause between attempts, and an attempt
        // in flight. After the first handshake there is no other reason to be
        // dialling, so a Connecting status IS a reconnection -- and without
        // this the banner's line would blink out for the length of every try.
        return retryAtMillis_ > 0 || (status_ == Status::Connecting && handshakes_ > 0);
    }

    /// Set once for every handshake AFTER the first: the socket came back.
    ///
    /// Cleared by whoever reads it, like `authAnswered`. Putting the SESSION
    /// back together is not something the socket can do for itself -- the
    /// account has to be resumed and the screen has to be steered somewhere
    /// that makes sense, and only the app knows where it was.
    bool reconnected = false;

    /// Set when the server refused the handshake: this build is not the one it
    /// is serving. Cleared by whoever reads it. See client/web/reload.h for
    /// what the browser does about it.
    bool staleBuild = false;

    /// Services the socket. Call once per frame with a small timeout so the
    /// render loop keeps its cadence even when nothing arrives.
    void poll(int timeoutMillis = 0);

    Status status() const { return status_; }
    /// Whether this client is holding an account: logged in, or in a game.
    /// Everything the server says about an account or a world is only
    /// meaningful while this holds.
    bool haveSession() const { return status_ == Status::LoggedIn || status_ == Status::Playing; }
    const std::string& lastError() const { return lastError_; }

    // -- requests ----------------------------------------------------------
    void requestRegister(const std::string& username, const std::string& password);
    void requestLogin(const std::string& username, const std::string& password);
    void resumeSession(const std::string& token);
    /// Ends the session: tells the server to revoke the token, then drops
    /// every scrap of the account this client was holding. The socket stays
    /// up -- the connection is not the session -- so the status falls back to
    /// Ready and the login form can be used again without a reconnect.
    void logout();
    /// `spawnBiome` is empty (or "default") for the beginner ground.
    /// `playerName` is the flower's nameplate; empty spawns as "Unnamed".
    void joinGame(int viewportWidth, int viewportHeight,
                  const std::string& spawnBiome = {}, const std::string& playerName = {});
    void leaveGame();
    void sendInput(const net::InputFrame&);
    void sendChat(const std::string& text);
    void setLoadoutSlot(int slot, std::uint16_t petalIndex, Rarity rarity);
    void swapLoadoutSlots(int a, int b);
    void requestCraft(std::uint16_t petalIndex, Rarity rarity, int count);
    void requestRespawn();
    void sendPing();
    /// Buys the NEXT tier of a branch. The server refuses anything else, so
    /// the panel never has to guess whether a jump would be allowed.
    void requestUpgradeSkill(SkillId skill, int tier);
    void requestResetSkills();
    /// Buys a petal. `offerSlot` is which card of the rotating store was
    /// clicked; the default buys at the full ladder price. Either way the
    /// price is the server's to decide -- the slot says which card, not what
    /// it costs.
    void requestBuyPetal(std::uint16_t petalIndex, Rarity rarity, int offerSlot = -1);
    /// Redeems a star code. The answer lands in shopOutcome().
    void requestRedeemCode(const std::string& code);
    void requestSkin(std::uint32_t renderFlags);
    /// Offers an authored skin to the shared catalog. The server re-sanitizes
    /// and assigns the id, so nothing here is authoritative -- the studio runs
    /// the same check first only so a rejection is instant.
    void publishSkin(const std::string& name, const std::vector<SkinShape>& shapes);
    /// Wears a published skin, or takes the current one off when `id` is empty.
    /// The local field moves immediately: the reference equips optimistically
    /// and the server's answer is the persistence, not the confirmation.
    void equipSkin(const std::string& id);
    void deleteSkin(const std::string& id);
    void requestLeaderboard();
    /// Asks for one page of the global notification feed, newest first.
    /// `beforeMillis` of 0 asks for the newest page; anything else pages back
    /// past the oldest entry already held, as the browser's `?before=` does.
    void requestNotifications(int limit, double beforeMillis);

    void requestGuildCreate(const std::string& name);
    void requestGuildInvite(const std::string& username);
    void requestGuildAccept();
    void requestGuildDecline();
    void requestGuildKick(const std::string& username);
    void requestGuildLeave();
    void requestGuildSquadAll();
    void requestGuildInviteToSquad(const std::string& username);

    // -- state -------------------------------------------------------------
    WorldView& view() { return view_; }
    const WorldView& view() const { return view_; }
    /// Locally regenerated from the authoritative seed in JoinAccepted.
    const Terrain& terrain() const { return terrain_; }
    const Profile& profile() const { return profile_; }
    const std::string& sessionToken() const { return sessionToken_; }
    const std::vector<ChatLine>& chat() const { return chat_; }
    const DailyStreak& dailyStreak() const { return dailyStreak_; }

    /// Every published skin the server has told this client about, the one this
    /// account is wearing, and whether it may take other people's skins down.
    ///
    /// One registry for two readers: the studio's Browse tab lists it, and the
    /// world renderer resolves a wearer's id through it. A skin nobody has been
    /// sent cannot be drawn, which is why the catalog arrives at login rather
    /// than when the studio opens.
    const std::vector<CustomSkin>& skinCatalog() const { return skinCatalog_; }
    const CustomSkin* findSkin(const std::string& id) const;
    const std::string& equippedSkinId() const { return equippedSkinId_; }
    bool isSkinAdmin() const { return skinAdmin_; }

    /// Puts a locally generated System line in the transcript. There is no
    /// separate notice or toast layer: every announcement the reference makes,
    /// its own included, is a chat line.
    void addSystemMessage(const std::string& text);
    /// The same, under a chosen sender. The studio reports its own failures as
    /// lines from "Skins", exactly as the reference does, so the author is not
    /// always "System".
    void addLocalChat(const std::string& author, const std::string& text);

    /// Round-trip time in milliseconds, from the last Ping/Pong exchange.
    double pingMillis() const { return pingMillis_; }
    /// The mean of the last ten round trips, which is what the readout shows:
    /// a single sample jitters too much to read, and the quality band below is
    /// derived from the same average so the two never disagree.
    double averagePingMillis() const { return averagePingMillis_; }
    /// "good", "medium" or "slow", at the reference's 100 ms / 200 ms bands.
    const char* connectionQuality() const;

    /// One opcode's share of the wire over the last window.
    struct WireEvent {
        const char* name = "";
        std::uint32_t bytes = 0;
        bool incoming = false;
    };
    /// Totals the bytes seen since the last call, fills `top` with the
    /// heaviest events first, and resets the counters -- so the caller is
    /// asking for bytes per window, and should ask once a second.
    ///
    /// Bytes are real framed sizes, not an estimate: both the send path and
    /// the dispatch have the encoded buffer in hand when they count it.
    void takeWireStats(std::uint32_t& inBytes, std::uint32_t& outBytes,
                       std::vector<WireEvent>& top, std::size_t topCount = 5);

    /// The server's own memory and tick cost, from the last DebugStats. Null
    /// until one arrives, which is what the debug panel draws as "no data".
    struct ServerDebugStats {
        double residentBytes = 0;
        double heapBytes = 0;
        double tickAvgMillis = 0;
        double tickMaxMillis = 0;
    };
    const ServerDebugStats* serverDebugStats() const {
        return haveServerDebugStats_ ? &serverDebugStats_ : nullptr;
    }
    /// True exactly once per DebugStats, so the panel appends one graph sample
    /// per packet rather than resampling whatever it last saw.
    bool takeServerDebugStats(ServerDebugStats& out);

    const std::vector<LeaderboardRow>& leaderboard() const { return leaderboard_; }
    /// True between asking for the board and the answer arriving.
    bool leaderboardPending() const { return leaderboardPending_; }
    /// How many accounts the server holds, and how many of them authenticated
    /// in the last day. The second is 0 for a non-admin, which is how the
    /// browser's payload leaves the field out.
    std::uint32_t totalAccounts() const { return totalAccounts_; }
    std::uint32_t dailyActiveUsers() const { return dailyActiveUsers_; }

    const std::vector<NotificationEntry>& notifications() const { return notifications_; }
    /// True between asking for a page and the answer arriving.
    bool notificationsPending() const { return notificationsPending_; }
    /// True while there may be an older page to ask for. Starts true and is
    /// only ever answered by a reply, exactly as the reference's `hasMore` is:
    /// a panel opening for the first time draws its loading footer on the
    /// strength of it.
    bool notificationsHaveMore() const { return notificationsMore_; }

    const SquadState& squad() const { return squad_; }

    const GuildState& guild() const { return guild_; }
    /// Mutable because the panel answers an invite locally the moment it sends
    /// the reply, exactly as the reference clears `pendingInvite` on click.
    GuildInvite& guildInvite() { return guildInvite_; }
    const GuildInvite& guildInvite() const { return guildInvite_; }

    /// The last craft result. The panel clears `pending` once it has started
    /// the animation for it.
    CraftOutcome& craftOutcome() { return craftOutcome_; }

    /// The last purchase or code redemption. The shop panel clears `pending`
    /// once it has raised the modal for it.
    ShopOutcome& shopOutcome() { return shopOutcome_; }

    /// Set when the server reports the player died; cleared by respawning.
    bool dead() const { return dead_; }
    const std::string& killerName() const { return killerName_; }

    /// Set when an auth attempt finished. The UI reads and clears it.
    bool authAnswered = false;
    net::AuthStatus authStatus = net::AuthStatus::Ok;
    std::string authMessage;

    /// The content hash this client loaded, sent in the handshake so a server
    /// with different mob/petal data can say so instead of quietly disagreeing.
    std::uint32_t contentHash = 0;

    // net::TransportHandler
    void onConnect(net::Connection&) override;
    void onMessage(net::Connection&, ByteReader&) override;
    void onDisconnect(net::Connection&, const std::string& reason) override;

private:
    void send(ByteWriter&);
    void beginMessage(ByteWriter&, net::ClientMessage);

    void handleWelcome(ByteReader&);
    void handleAuthResult(ByteReader&);
    void handleProfile(ByteReader&);
    void handleJoinAccepted(ByteReader&);
    void handleMazeInfo(ByteReader&);
    void handleChat(ByteReader&);
    void handleNotice(ByteReader&);
    /// Appends one line and trims the transcript to its cap.
    void pushChat(net::ChatChannel, std::string author, std::string text);
    void handleDied(ByteReader&);
    void handleCraftResult(ByteReader&);
    void handleShopResult(ByteReader&);
    void handleLeaderboard(ByteReader&);
    void handleNotifications(ByteReader&);
    /// Schedules the next redial, lengthening the wait each time it is
    /// called without a handshake in between.
    void armReconnect();

    void handleSquadUpdate(ByteReader&);
    void handleGuildUpdate(ByteReader&);
    void handleGuildInviteReceived(ByteReader&);
    void handlePong(ByteReader&);
    void handleDebugStats(ByteReader&);
    void handleKick(ByteReader&);
    void handleDailyStreak(ByteReader&);
    void handleSkinCatalog(ByteReader&);
    void handleSkinPublished(ByteReader&);
    void handleSkinDeleted(ByteReader&);

    net::Dialer dialer_;
    Status status_ = Status::Offline;

    /// Where connect() was last pointed, so a drop can be redialled without
    /// the caller having to remember for it.
    std::string host_;
    std::uint16_t port_ = 0;
    /// The render clock at which the next redial is due, or 0 for none, and
    /// the delay that produced it.
    double retryAtMillis_ = 0;
    double retryDelayMillis_ = 0;
    /// How many handshakes this client has completed. The first is the
    /// ordinary connect; every one after it is a reconnection.
    int handshakes_ = 0;
    std::string lastError_;

    WorldView view_;
    Terrain terrain_;
    Profile profile_;
    std::string sessionToken_;
    std::vector<ChatLine> chat_;
    DailyStreak dailyStreak_;

    std::vector<CustomSkin> skinCatalog_;
    std::string equippedSkinId_;
    bool skinAdmin_ = false;

    std::vector<LeaderboardRow> leaderboard_;
    bool leaderboardPending_ = false;
    std::uint32_t totalAccounts_ = 0;
    std::uint32_t dailyActiveUsers_ = 0;

    std::vector<NotificationEntry> notifications_;
    bool notificationsPending_ = false;
    bool notificationsMore_ = true;
    /// Whether the request in flight asked for an OLDER page. A page from the
    /// newest end replaces the feed; one from behind the oldest entry appends
    /// to it, and only the request knows which this is.
    bool notificationsPaging_ = false;

    SquadState squad_;
    GuildState guild_;
    GuildInvite guildInvite_;
    CraftOutcome craftOutcome_;
    ShopOutcome shopOutcome_;

    double pingMillis_ = 0;
    /// The last ten round trips and their mean. Ten is the reference's window.
    static constexpr std::size_t kPingSamples = 10;
    std::vector<double> pingHistory_;
    double averagePingMillis_ = 0;

    /// Wire bytes since the last takeWireStats(), indexed by opcode. Two flat
    /// arrays rather than a map keyed by name: the counting happens on every
    /// frame of every message, and the names are only needed once a second
    /// when the overlay asks.
    std::array<std::uint32_t, 256> incomingBytes_{};
    std::array<std::uint32_t, 256> outgoingBytes_{};

    ServerDebugStats serverDebugStats_;
    bool haveServerDebugStats_ = false;
    bool serverDebugStatsFresh_ = false;

    bool dead_ = false;
    std::string killerName_;

    /// Chat is capped so a long session cannot grow without bound; the oldest
    /// lines scroll off, which is what the panel shows anyway.
    static constexpr std::size_t kMaxChatLines = 100;
};

} // namespace flix
