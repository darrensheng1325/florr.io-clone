#include "client/net_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "client/web/reload.h"
#include "shared/game/map_elements.h"

namespace flix {

namespace {

double nowMillis() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

/// The redial backoff, which is the browser socket's own (src/ws_client.ts):
/// a second to start, half again per failure, capped at ten seconds.
constexpr double kReconnectDelayMillis = 1000.0;
constexpr double kMaxReconnectDelayMillis = 10000.0;
constexpr double kReconnectBackoff = 1.5;

/// Unix milliseconds. A chat line is stamped with the time of day it arrived,
/// which the monotonic clock above cannot answer.
std::int64_t wallClockMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// What the bandwidth readout calls each opcode. The browser keys its
/// per-event byte counters by the socket event name; these are the same names
/// this protocol's messages go by, so the two overlays read alike.
const char* clientMessageName(std::uint8_t id) {
    switch (static_cast<net::ClientMessage>(id)) {
        case net::ClientMessage::Hello:               return "hello";
        case net::ClientMessage::Register:            return "register";
        case net::ClientMessage::Login:               return "login";
        case net::ClientMessage::ResumeSession:       return "resumeSession";
        case net::ClientMessage::Logout:              return "logout";
        case net::ClientMessage::JoinGame:            return "joinGame";
        case net::ClientMessage::LeaveGame:           return "leaveGame";
        case net::ClientMessage::Input:               return "input";
        case net::ClientMessage::SetLoadout:          return "setLoadout";
        case net::ClientMessage::SwapLoadout:         return "swapLoadout";
        case net::ClientMessage::Craft:               return "craft";
        case net::ClientMessage::Chat:                return "chat";
        case net::ClientMessage::Respawn:             return "respawn";
        case net::ClientMessage::Ping:                return "ping";
        case net::ClientMessage::UpgradeSkill:        return "upgradeSkill";
        case net::ClientMessage::ResetSkills:         return "resetSkills";
        case net::ClientMessage::BuyPetal:            return "buyPetal";
        case net::ClientMessage::SetSkin:             return "setSkin";
        case net::ClientMessage::RequestLeaderboard:  return "leaderboard";
        case net::ClientMessage::RedeemCode:          return "redeemCode";
        case net::ClientMessage::PublishSkin:         return "publishSkin";
        case net::ClientMessage::EquipSkin:           return "equipSkin";
        case net::ClientMessage::DeleteSkin:          return "deleteSkin";
        case net::ClientMessage::RequestNotifications:return "notifications";
        case net::ClientMessage::GuildCreate:         return "guildCreate";
        case net::ClientMessage::GuildInvite:         return "guildInvite";
        case net::ClientMessage::GuildAccept:         return "guildAccept";
        case net::ClientMessage::GuildDecline:        return "guildDecline";
        case net::ClientMessage::GuildKick:           return "guildKick";
        case net::ClientMessage::GuildLeave:          return "guildLeave";
        case net::ClientMessage::GuildSquadAll:       return "guildSquadAll";
        case net::ClientMessage::GuildInviteToSquad:  return "guildInviteToSquad";
    }
    return "unknown";
}

const char* serverMessageName(std::uint8_t id) {
    switch (static_cast<net::ServerMessage>(id)) {
        case net::ServerMessage::Welcome:             return "welcome";
        case net::ServerMessage::AuthResult:          return "authResult";
        case net::ServerMessage::Profile:             return "profile";
        case net::ServerMessage::JoinAccepted:        return "joinAccepted";
        case net::ServerMessage::Snapshot:            return "gameStateUpdate";
        case net::ServerMessage::Chat:                return "chat";
        case net::ServerMessage::Notice:              return "notice";
        case net::ServerMessage::Died:                return "died";
        case net::ServerMessage::CraftResult:         return "craftResult";
        case net::ServerMessage::Leaderboard:         return "leaderboard";
        case net::ServerMessage::Pong:                return "pong";
        case net::ServerMessage::Kick:                return "kick";
        case net::ServerMessage::DailyStreak:         return "dailyStreak";
        case net::ServerMessage::ShopResult:          return "shopResult";
        case net::ServerMessage::SkinCatalog:         return "skinCatalog";
        case net::ServerMessage::SkinPublished:       return "skinPublished";
        case net::ServerMessage::SkinDeleted:         return "skinDeleted";
        case net::ServerMessage::Notifications:       return "notifications";
        case net::ServerMessage::SquadUpdate:         return "squadUpdate";
        case net::ServerMessage::GuildUpdate:         return "guildUpdate";
        case net::ServerMessage::GuildInviteReceived: return "guildInvite";
        case net::ServerMessage::DebugStats:          return "debugStats";
        case net::ServerMessage::MazeInfo:            return "mazeInfo";
        case net::ServerMessage::RealmChange:         return "realmChange";
    }
    return "unknown";
}

} // namespace

NetClient::NetClient() = default;
NetClient::~NetClient() = default;

bool NetClient::connect(const std::string& host, std::uint16_t port) {
    disconnect();
    // Remembered before the dial, so a redial has somewhere to go even when
    // this attempt is the one that failed.
    host_ = host;
    port_ = port;
    std::string error;
    if (!dialer_.connect(host, port, error)) {
        status_ = Status::Failed;
        lastError_ = error;
        armReconnect();
        return false;
    }
    status_ = Status::Connecting;
    lastError_.clear();
    return true;
}

void NetClient::disconnect() {
    dialer_.disconnect();
    status_ = Status::Offline;
    view_.clear();
    dead_ = false;
    // An explicit disconnect is a decision, not an accident: nothing redials
    // after one. connect() re-arms by dialling.
    retryAtMillis_ = 0;
    retryDelayMillis_ = 0;
}

void NetClient::armReconnect() {
    if (host_.empty()) return;
    // The browser socket's backoff: one second, then half again per failure,
    // capped at ten. Long enough that a restarting server is not hammered,
    // short enough that a player watching the banner sees it heal.
    retryDelayMillis_ = retryDelayMillis_ <= 0
                            ? kReconnectDelayMillis
                            : std::min(retryDelayMillis_ * kReconnectBackoff,
                                       kMaxReconnectDelayMillis);
    retryAtMillis_ = nowMillis() + retryDelayMillis_;
}

void NetClient::poll(int timeoutMillis) {
    if (status_ == Status::Failed && retryAtMillis_ > 0 && nowMillis() >= retryAtMillis_) {
        // The delay is deliberately NOT reset here: it grows until a handshake
        // lands, which is what keeps a server that is down for a minute from
        // being dialled sixty times.
        const double delay = retryDelayMillis_;
        const std::string host = host_;
        const std::uint16_t port = port_;
        retryAtMillis_ = 0;
        connect(host, port);
        retryDelayMillis_ = delay;
        if (status_ == Status::Failed) armReconnect();
    }
    if (status_ == Status::Offline || status_ == Status::Failed) return;
    dialer_.poll(*this, timeoutMillis);
    dialer_.flush();
    if (dialer_.state() == net::Dialer::State::Failed && status_ != Status::Failed) {
        status_ = Status::Failed;
        if (lastError_.empty()) lastError_ = dialer_.error();
        armReconnect();
    }
}

void NetClient::beginMessage(ByteWriter& w, net::ClientMessage id) {
    w.clear();
    w.u8(static_cast<std::uint8_t>(id));
}

void NetClient::send(ByteWriter& w) {
    // Counted here rather than in the dialer: this is the one place that has
    // both the opcode and the finished buffer, and every request goes through
    // it. The four framing bytes the transport prepends are added in so the
    // number matches what actually leaves the socket.
    if (!w.empty()) {
        const auto id = static_cast<std::uint8_t>(w.data()[0]);
        outgoingBytes_[id] += static_cast<std::uint32_t>(w.size() + 4);
    }
    dialer_.send(w);
}

// -- outgoing ---------------------------------------------------------------

void NetClient::onConnect(net::Connection&) {
    status_ = Status::Handshaking;
    // The handshake goes out unprompted: the server answers with Welcome, and
    // until then nothing else this client sends will be accepted.
    ByteWriter w;
    beginMessage(w, net::ClientMessage::Hello);
    w.u16(net::kProtocolVersion);
    w.u32(contentHash);
    send(w);
}

void NetClient::requestRegister(const std::string& username, const std::string& password) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::Register);
    w.str(username);
    w.str(password);
    send(w);
}

void NetClient::requestLogin(const std::string& username, const std::string& password) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::Login);
    w.str(username);
    w.str(password);
    send(w);
}

void NetClient::resumeSession(const std::string& token) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::ResumeSession);
    w.str(token);
    send(w);
}

void NetClient::logout() {
    // Leave first if there is a body out there: the server drops the session's
    // account on Logout, and a player left in the world would have nothing
    // behind it to save into.
    if (status_ == Status::Playing) leaveGame();

    ByteWriter w;
    beginMessage(w, net::ClientMessage::Logout);
    send(w);

    // Everything the account owned goes with the account. Whatever logs in
    // next must not inherit the last one's inventory, chat, guild or skins --
    // the panels read these fields directly, and a stale one would be drawn
    // on the lobby's first frame before the server's fresh copy arrived.
    sessionToken_.clear();
    profile_ = Profile{};
    chat_.clear();
    dailyStreak_ = DailyStreak{};
    skinCatalog_.clear();
    equippedSkinId_.clear();
    skinAdmin_ = false;
    leaderboard_.clear();
    leaderboardPending_ = false;
    notifications_.clear();
    notificationsPending_ = false;
    notificationsMore_ = true;
    notificationsPaging_ = false;
    squad_ = SquadState{};
    guild_ = GuildState{};
    guildInvite_ = GuildInvite{};
    craftOutcome_ = CraftOutcome{};
    shopOutcome_ = ShopOutcome{};
    view_.clear();
    dead_ = false;
    killerName_.clear();

    // Not an auth answer: the form must open blank rather than showing the
    // reason the LAST attempt gave.
    authAnswered = false;
    authStatus = net::AuthStatus::Ok;
    authMessage.clear();

    // A dropped socket stays dropped -- logging out cannot repair it.
    if (status_ != Status::Offline && status_ != Status::Failed &&
        status_ != Status::Connecting && status_ != Status::Handshaking) {
        status_ = Status::Ready;
    }
}

void NetClient::joinGame(int viewportWidth, int viewportHeight, const std::string& spawnChoice,
                         const std::string& playerName) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::JoinGame);
    w.u16(static_cast<std::uint16_t>(viewportWidth));
    w.u16(static_cast<std::uint16_t>(viewportHeight));
    w.str(spawnChoice);
    w.str(playerName);
    send(w);
}

void NetClient::leaveGame() {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::LeaveGame);
    send(w);
    if (status_ == Status::Playing) status_ = Status::LoggedIn;
    view_.clear();
    dead_ = false;
}

void NetClient::sendInput(const net::InputFrame& input) {
    if (status_ != Status::Playing) return;
    ByteWriter w;
    beginMessage(w, net::ClientMessage::Input);
    input.write(w);
    send(w);
}

void NetClient::sendChat(const std::string& text) {
    if (text.empty()) return;
    ByteWriter w;
    beginMessage(w, net::ClientMessage::Chat);
    w.str(text);
    send(w);
}

void NetClient::setLoadoutSlot(int slot, std::uint16_t petalIndex, Rarity rarity) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::SetLoadout);
    w.u8(static_cast<std::uint8_t>(slot));
    w.u16(petalIndex);
    w.u8(static_cast<std::uint8_t>(rarity));
    send(w);
}

void NetClient::swapLoadoutSlots(int a, int b) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::SwapLoadout);
    w.u8(static_cast<std::uint8_t>(a));
    w.u8(static_cast<std::uint8_t>(b));
    send(w);
}

void NetClient::requestCraft(std::uint16_t petalIndex, Rarity rarity, int count) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::Craft);
    w.u16(petalIndex);
    w.u8(static_cast<std::uint8_t>(rarity));
    w.u16(static_cast<std::uint16_t>(std::max(0, count)));
    send(w);
}

void NetClient::requestUpgradeSkill(SkillId skill, int tier) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::UpgradeSkill);
    w.u8(static_cast<std::uint8_t>(skill));
    w.u8(static_cast<std::uint8_t>(tier));
    send(w);
}

void NetClient::requestResetSkills() {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::ResetSkills);
    send(w);
}

void NetClient::requestBuyPetal(std::uint16_t petalIndex, Rarity rarity, int offerSlot) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::BuyPetal);
    w.u16(petalIndex);
    w.u8(static_cast<std::uint8_t>(rarity));
    w.u8(offerSlot >= 0 && offerSlot < net::kNoShopOffer ? static_cast<std::uint8_t>(offerSlot)
                                                         : net::kNoShopOffer);
    send(w);
}

void NetClient::requestRedeemCode(const std::string& code) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::RedeemCode);
    w.str(code);
    send(w);
}

void NetClient::requestSkin(std::uint32_t renderFlags) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::SetSkin);
    w.u32(renderFlags);
    send(w);
}

void NetClient::publishSkin(const std::string& name, const std::vector<SkinShape>& shapes) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::PublishSkin);
    w.str(name);
    const std::size_t count = std::min(shapes.size(), static_cast<std::size_t>(kMaxSkinShapes));
    w.u8(static_cast<std::uint8_t>(count));
    for (std::size_t i = 0; i < count; ++i) writeSkinShape(w, shapes[i]);
    send(w);
}

void NetClient::equipSkin(const std::string& id) {
    // Moved before the send, not after the reply: the reference sets its own
    // equippedId the moment the button is clicked, and the studio's tick is
    // what the player sees change.
    equippedSkinId_ = id;
    if (!id.empty()) profile_.renderFlags = PlayerRenderNone;
    ByteWriter w;
    beginMessage(w, net::ClientMessage::EquipSkin);
    w.str(id);
    send(w);
}

void NetClient::deleteSkin(const std::string& id) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::DeleteSkin);
    w.str(id);
    send(w);
}

void NetClient::requestLeaderboard() {
    leaderboardPending_ = true;
    ByteWriter w;
    beginMessage(w, net::ClientMessage::RequestLeaderboard);
    send(w);
}

void NetClient::requestNotifications(int limit, double beforeMillis) {
    notificationsPending_ = true;
    notificationsPaging_ = beforeMillis > 0;
    ByteWriter w;
    beginMessage(w, net::ClientMessage::RequestNotifications);
    w.u16(static_cast<std::uint16_t>(clamp(limit, 1, 200)));
    w.f64(beforeMillis);
    send(w);
}

void NetClient::requestGuildCreate(const std::string& name) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::GuildCreate);
    w.str(name);
    send(w);
}

void NetClient::requestGuildInvite(const std::string& username) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::GuildInvite);
    w.str(username);
    send(w);
}

void NetClient::requestGuildAccept() {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::GuildAccept);
    send(w);
}

void NetClient::requestGuildDecline() {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::GuildDecline);
    send(w);
}

void NetClient::requestGuildKick(const std::string& username) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::GuildKick);
    w.str(username);
    send(w);
}

void NetClient::requestGuildLeave() {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::GuildLeave);
    send(w);
}

void NetClient::requestGuildSquadAll() {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::GuildSquadAll);
    send(w);
}

void NetClient::requestGuildInviteToSquad(const std::string& username) {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::GuildInviteToSquad);
    w.str(username);
    send(w);
}

void NetClient::requestRespawn() {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::Respawn);
    send(w);
}

void NetClient::sendPing() {
    ByteWriter w;
    beginMessage(w, net::ClientMessage::Ping);
    w.u64(static_cast<std::uint64_t>(nowMillis()));
    send(w);
}

// -- incoming ---------------------------------------------------------------

void NetClient::onMessage(net::Connection&, ByteReader& reader) {
    // Before the opcode is read, so this is the whole frame; plus the four
    // framing bytes the transport has already stripped.
    const std::size_t frameBytes = reader.remaining() + 4;
    const std::uint8_t rawId = reader.u8();
    incomingBytes_[rawId] += static_cast<std::uint32_t>(frameBytes);
    const auto id = static_cast<net::ServerMessage>(rawId);

    // A logout does not empty the pipe. Whatever the server had already sent
    // -- the profile handleLeave answers a LeaveGame with, a streak, a skin
    // catalog, a guild -- is still in flight, and applying any of it would
    // repopulate the account this client just finished forgetting. Anything
    // that describes an account or a world is dropped until there is a session
    // for it to belong to again; only the handshake, the auth answer, a server
    // notice, a kick and a ping mean anything without one.
    if (!haveSession()) {
        switch (id) {
            case net::ServerMessage::Welcome:
            case net::ServerMessage::AuthResult:
            case net::ServerMessage::Notice:
            case net::ServerMessage::Kick:
            case net::ServerMessage::Pong:
                break;
            default:
                return;
        }
    }

    switch (id) {
        case net::ServerMessage::Welcome:      handleWelcome(reader); break;
        case net::ServerMessage::AuthResult:   handleAuthResult(reader); break;
        case net::ServerMessage::Profile:      handleProfile(reader); break;
        case net::ServerMessage::JoinAccepted: handleJoinAccepted(reader); break;
        case net::ServerMessage::Snapshot:     view_.applySnapshot(reader); break;
        case net::ServerMessage::Chat:         handleChat(reader); break;
        case net::ServerMessage::Notice:       handleNotice(reader); break;
        case net::ServerMessage::Died:         handleDied(reader); break;
        case net::ServerMessage::CraftResult:  handleCraftResult(reader); break;
        case net::ServerMessage::Leaderboard:  handleLeaderboard(reader); break;
        case net::ServerMessage::Pong:         handlePong(reader); break;
        case net::ServerMessage::Kick:         handleKick(reader); break;
        case net::ServerMessage::DailyStreak:  handleDailyStreak(reader); break;
        case net::ServerMessage::ShopResult:   handleShopResult(reader); break;
        case net::ServerMessage::SkinCatalog:   handleSkinCatalog(reader); break;
        case net::ServerMessage::SkinPublished: handleSkinPublished(reader); break;
        case net::ServerMessage::SkinDeleted:   handleSkinDeleted(reader); break;
        case net::ServerMessage::Notifications: handleNotifications(reader); break;
        case net::ServerMessage::SquadUpdate:   handleSquadUpdate(reader); break;
        case net::ServerMessage::GuildUpdate:   handleGuildUpdate(reader); break;
        case net::ServerMessage::GuildInviteReceived: handleGuildInviteReceived(reader); break;
        case net::ServerMessage::DebugStats:    handleDebugStats(reader); break;
        case net::ServerMessage::MazeInfo:      handleMazeInfo(reader); break;
        case net::ServerMessage::RealmChange:   handleRealmChange(reader); break;
        default:
            // An unknown id means the server is newer than this build. The
            // frame is already fully buffered, so skipping it is safe and
            // keeps older clients working against additive changes.
            break;
    }
}

void NetClient::onDisconnect(net::Connection&, const std::string& reason) {
    status_ = Status::Failed;
    lastError_ = reason;
    view_.clear();
    // A server restart looks exactly like this from here, and it is the case
    // the redial exists for.
    armReconnect();
}

void NetClient::handleWelcome(ByteReader& reader) {
    const std::uint16_t version = reader.u16();
    const bool accepted = reader.boolean();
    const std::string reason = reader.str();
    if (!reader.ok()) return;

    if (!accepted) {
        status_ = Status::Failed;
        lastError_ = reason.empty()
            ? ("Server speaks protocol " + std::to_string(version) + ", this client speaks " +
               std::to_string(net::kProtocolVersion))
            : reason;
        dialer_.disconnect();
        // Deliberately NOT redialled. A server that refuses this build refuses
        // it every time, and a client looping on that is a client hammering a
        // server it can never talk to. The build is what has to change, which
        // is what `staleBuild` is for.
        retryAtMillis_ = 0;
        staleBuild = true;
        return;
    }
    status_ = Status::Ready;

    // Every handshake but the first is a reconnection, and a reconnection is
    // the signal that the backoff worked and the session needs rebuilding.
    retryDelayMillis_ = 0;
    retryAtMillis_ = 0;
    if (handshakes_++ > 0) reconnected = true;
    web::clearStaleBuildGuard();
}

void NetClient::handleAuthResult(ByteReader& reader) {
    const auto result = static_cast<net::AuthStatus>(reader.u8());
    const std::string token = reader.str();
    const std::string username = reader.str();
    const std::string reason = reader.str();
    if (!reader.ok()) return;

    authAnswered = true;
    authStatus = result;
    authMessage = reason;

    if (result == net::AuthStatus::Ok) {
        sessionToken_ = token;
        profile_.username = username;
        status_ = Status::LoggedIn;
    } else if (result == net::AuthStatus::SessionExpired) {
        // The stored token is no longer usable; drop it so the UI falls back
        // to the login form instead of retrying it forever.
        sessionToken_.clear();
        status_ = Status::Ready;
    } else {
        status_ = Status::Ready;
    }
}

void NetClient::handleProfile(ByteReader& reader) {
    Profile next;
    next.username = reader.str();
    next.totalXp = reader.f64();
    next.level = reader.u16();
    next.stars = static_cast<int>(reader.u32());

    const std::uint16_t stackCount = reader.u16();
    next.inventory.reserve(stackCount);
    for (std::uint16_t i = 0; i < stackCount; ++i) {
        Profile::Stack stack;
        stack.petalIndex = reader.u16();
        stack.rarity = clampRarity(reader.u8());
        stack.count = reader.u32();
        next.inventory.push_back(stack);
    }

    const std::uint8_t slotCount = reader.u8();
    next.loadout.reserve(slotCount);
    for (std::uint8_t i = 0; i < slotCount; ++i) {
        Profile::Slot slot;
        slot.petalIndex = reader.u16();
        slot.rarity = clampRarity(reader.u8());
        next.loadout.push_back(slot);
    }

    next.renderFlags = reader.u32();

    const std::uint16_t skillCount = reader.u16();
    for (std::uint16_t i = 0; i < skillCount; ++i) {
        const std::uint8_t id = reader.u8();
        const std::uint8_t tier = reader.u8();
        if (id < kSkillCount && tier < kRarityCount) {
            next.skills.set(static_cast<SkillId>(id), tier);
        }
    }

    next.mobKills.assign(content().mobCount() * kRarityCount, 0);
    const std::uint16_t killEntries = reader.u16();
    for (std::uint16_t i = 0; i < killEntries; ++i) {
        const std::uint16_t mobIndex = reader.u16();
        const Rarity rarity = clampRarity(reader.u8());
        const std::uint32_t count = reader.u32();
        const std::size_t at = static_cast<std::size_t>(mobIndex) * kRarityCount + rarityIndex(rarity);
        if (at < next.mobKills.size()) next.mobKills[at] = count;
    }

    // Replace wholesale only once the whole message decoded. A partially
    // applied inventory is how duplication bugs start.
    if (!reader.ok()) return;
    profile_ = std::move(next);
}

void NetClient::handleCraftResult(ByteReader& reader) {
    CraftOutcome outcome;
    outcome.success = reader.boolean();
    outcome.petalIndex = reader.u16();
    outcome.rarity = clampRarity(reader.u8());
    outcome.crafted = reader.u16();
    outcome.petalsReturned = reader.u8();
    outcome.reason = reader.str();
    if (!reader.ok()) return;
    outcome.pending = true;
    craftOutcome_ = std::move(outcome);
}

void NetClient::handleShopResult(ByteReader& reader) {
    ShopOutcome outcome;
    outcome.redeem =
        static_cast<net::ShopResultKind>(reader.u8()) == net::ShopResultKind::Redeem;
    outcome.ok = reader.boolean();
    outcome.stars = static_cast<int>(reader.u32());
    outcome.message = reader.str();
    if (!reader.ok()) return;
    outcome.pending = true;
    shopOutcome_ = std::move(outcome);
}

void NetClient::handleLeaderboard(ByteReader& reader) {
    std::vector<LeaderboardRow> rows;
    const std::uint8_t count = reader.u8();
    const std::uint32_t total = reader.u32();
    const std::uint32_t dau = reader.u32();
    rows.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        LeaderboardRow row;
        row.name = reader.str();
        row.level = reader.u16();
        row.totalXp = reader.f64();
        rows.push_back(std::move(row));
    }
    if (!reader.ok()) return;
    leaderboard_ = std::move(rows);
    totalAccounts_ = total;
    dailyActiveUsers_ = dau;
    leaderboardPending_ = false;
}

void NetClient::handleNotifications(ByteReader& reader) {
    const bool more = reader.boolean();
    const std::uint16_t count = reader.u16();
    std::vector<NotificationEntry> page;
    page.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        NotificationEntry entry;
        entry.id = reader.str();
        entry.kind = static_cast<net::NotificationKind>(reader.u8());
        entry.message = reader.str();
        entry.timestampMillis = reader.f64();
        page.push_back(std::move(entry));
    }
    if (!reader.ok()) return;

    // A page asked for from the newest end REPLACES the feed; one asked for
    // from behind the oldest entry held APPENDS to it. That is the browser's
    // rule, and it is what lets the panel keep scrolling into older pages
    // without the newest page arriving twice.
    if (notificationsPaging_) {
        notifications_.insert(notifications_.end(), page.begin(), page.end());
    } else {
        notifications_ = std::move(page);
    }
    notificationsMore_ = more;
    notificationsPending_ = false;
}

void NetClient::handleSquadUpdate(ByteReader& reader) {
    SquadState next;
    next.inSquad = reader.boolean();
    if (next.inSquad) {
        next.id = reader.str();
        next.isPublic = reader.boolean();
        const std::uint8_t count = reader.u8();
        next.members.reserve(count);
        for (std::uint8_t i = 0; i < count; ++i) {
            SquadState::Member member;
            member.account = reader.str();
            member.name = reader.str();
            member.netId = reader.u32();
            const std::uint8_t flags = reader.u8();
            member.leader = (flags & 1u) != 0;
            member.bot = (flags & 2u) != 0;
            next.members.push_back(std::move(member));
        }
    }
    if (!reader.ok()) return;
    squad_ = std::move(next);
}

void NetClient::handleGuildUpdate(ByteReader& reader) {
    GuildState next;
    next.joined = reader.boolean();
    if (next.joined) {
        next.name = reader.str();
        next.leader = reader.str();
        const std::uint16_t count = reader.u16();
        next.members.reserve(count);
        for (std::uint16_t i = 0; i < count; ++i) {
            std::string member = reader.str();
            const bool online = reader.boolean();
            if (online) next.online.push_back(member);
            next.members.push_back(std::move(member));
        }
    }
    if (!reader.ok()) return;
    guild_ = std::move(next);
    // Joining answers whatever invitation was outstanding, exactly as
    // applyGuildUpdate drops `pendingInvite` when it is handed a guild.
    if (guild_.joined) guildInvite_ = {};
}

void NetClient::handleGuildInviteReceived(ByteReader& reader) {
    GuildInvite invite;
    invite.guildName = reader.str();
    invite.fromUsername = reader.str();
    if (!reader.ok()) return;
    invite.waiting = true;
    invite.justArrived = true;
    guildInvite_ = std::move(invite);
}

void NetClient::installLocalCollision(Realm realm) {
    // The wire has just installed this realm's COARSE grid, which is
    // authoritative and is what the minimap paints and what every shape test
    // fast-rejects against. What it cannot carry is the geometry: a cell blocks
    // with the shapes its tile was drawn with -- a triangle under a diagonal, an
    // L inside a hut -- and there are thousands of them. Those live in the map
    // file, which this client already stages for its art, and which the
    // handshake's content hash has proved identical to the server's copy --
    // the TILESET the map names included, since that is where the shapes
    // themselves live (ContentRegistry::foldMapsIntoHash).
    //
    // So the exact answer is rebuilt here, from the file, through the SAME
    // function the server loads with (Terrain::setCollisionShapes, by way of
    // loadCollisionShapes). There is one place that turns a map into collision
    // and this is not a second one.
    terrain_.clearCollisionShapes(realm);

    const MapData* map = worldMaps_ != nullptr ? worldMaps_->forRealm(realm) : nullptr;
    const std::string path = map != nullptr ? map->sourcePath() : std::string();
    if (path.empty()) {
        // No local map for this realm: this client collides against whole
        // cells, straight off the wire's grid. That is the SAFE direction to be
        // wrong in -- a cell that blocks anywhere is treated as blocking
        // everywhere, so this client believes in more wall than there is, never
        // less. It loses a body-width of ground at a slope and it never draws a
        // flower standing inside something solid. The realms with no map file
        // at all (the arena, the maze) answer for their own geometry and never
        // come through here.
        return;
    }

    std::string error;
    if (!terrain_.loadCollisionShapes(path, error, realm)) {
        // Worth a line: it means this client is playing with coarser collision
        // than the server is enforcing, which is playable but visibly wrong at
        // every authored edge.
        std::fprintf(stderr,
                     "[map] %s: could not read the collision shapes (%s); this client falls back "
                     "to whole-cell collision\n",
                     path.c_str(), error.c_str());
    }
}

void NetClient::handleJoinAccepted(ByteReader& reader) {
    const std::uint32_t selfNetId = reader.u32();
    const Vec2 spawn = reader.position();
    reader.u32();   // tick, informational
    const std::int64_t mazeDay = reader.i64();

    // The map the body was put in: which realm it is, how big it is, and its
    // coarse grid. All three travel together because the server is the one that
    // decides which world a body went into, and maps are not all one size. The
    // exact shapes inside those cells are not on the wire; they are rebuilt from
    // the local file below.
    Realm realm = Realm::Overworld;
    std::string mapError;
    if (!readMapGrid(reader, terrain_, realm, mapError)) {
        status_ = Status::Failed;
        lastError_ = "Server sent an invalid map: " + mapError;
        dialer_.disconnect();
        return;
    }

    // The grid is in; give the realm its exact geometry from the local file.
    installLocalCollision(realm);

    (void)selfNetId;
    status_ = Status::Playing;
    dead_ = false;
    view_.clear();
    // After the clear, which resets it: the realm is the one thing about the
    // new body the snapshot stream never restates. The maze is built from the
    // server's day number so the walls drawn are the walls collided with.
    view_.setRealm(realm);
    setActiveMazeDay(mazeDay);
    // Where the body is until the first snapshot says so: the App pins the
    // camera here rather than on the view's zeroed self. Not flagged as a
    // realm change -- the join's own snap (App::startGame) covers the camera.
    realmArrival_ = spawn;
}

void NetClient::handleRealmChange(ByteReader& reader) {
    const Vec2 arrival = reader.position();
    Realm realm = Realm::Overworld;
    std::string mapError;
    if (!readMapGrid(reader, terrain_, realm, mapError)) {
        // A malformed grid is not worth dropping the connection over the way a
        // join is: the flower is already in the world, and the next snapshot
        // will still draw it. Keeping the old map would draw the WRONG walls,
        // so the realm is left alone and the jump simply does not take here.
        std::fprintf(stderr, "[net] realm change carried an invalid map: %s\n",
                     mapError.c_str());
        return;
    }
    // A new realm is a new map: its shapes have to be built from ITS file, and
    // the realm left behind keeps the ones it already had, because a pad leads
    // back.
    installLocalCollision(realm);
    // The view holds entities in the realm it was streaming; every one of them
    // is in another coordinate space now and none of them will be restated.
    view_.clear();
    view_.setRealm(realm);
    // The camera must not ease across two worlds. The App snaps it to the
    // arrival on the frame it sees the realm change, which is what this flag
    // is for -- the snapshot stream restates the body a moment later, and
    // between the two there would otherwise be nothing to look at.
    realmArrival_ = arrival;
    realmChanged_ = true;
}

void NetClient::handleMazeInfo(ByteReader& reader) {
    const std::int64_t day = reader.i64();
    if (!reader.ok()) return;
    setActiveMazeDay(day);
}

void NetClient::pushChat(net::ChatChannel channel, std::string author, std::string text) {
    ChatLine line;
    line.channel = channel;
    line.author = std::move(author);
    line.text = std::move(text);
    line.receivedAtMillis = nowMillis();
    line.wallClockMillis = wallClockMillis();

    chat_.push_back(std::move(line));
    if (chat_.size() > kMaxChatLines) {
        chat_.erase(chat_.begin(),
                    chat_.begin() + static_cast<std::ptrdiff_t>(chat_.size() - kMaxChatLines));
    }
}

void NetClient::addSystemMessage(const std::string& text) {
    pushChat(net::ChatChannel::System, "System", text);
}

void NetClient::addLocalChat(const std::string& author, const std::string& text) {
    pushChat(net::ChatChannel::System, author, text);
}

const CustomSkin* NetClient::findSkin(const std::string& id) const {
    if (id.empty()) return nullptr;
    for (const CustomSkin& skin : skinCatalog_) {
        if (skin.id == id) return &skin;
    }
    return nullptr;
}

void NetClient::handleSkinCatalog(ByteReader& reader) {
    const bool admin = reader.boolean();
    const std::string equipped = reader.str();
    const std::uint16_t count = reader.u16();
    std::vector<CustomSkin> skins;
    skins.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        CustomSkin skin;
        if (!readCustomSkin(reader, skin)) break;
        skins.push_back(std::move(skin));
    }
    if (!reader.ok()) return;
    skinCatalog_ = std::move(skins);
    skinAdmin_ = admin;
    equippedSkinId_ = equipped;
}

void NetClient::handleSkinPublished(ByteReader& reader) {
    CustomSkin skin;
    if (!readCustomSkin(reader, skin)) return;
    // Replace in place when it is already known: a republished id is an edit,
    // and appending it would leave the Browse tab showing the skin twice.
    for (CustomSkin& known : skinCatalog_) {
        if (known.id == skin.id) {
            known = std::move(skin);
            return;
        }
    }
    skinCatalog_.push_back(std::move(skin));
}

void NetClient::handleSkinDeleted(ByteReader& reader) {
    const std::string id = reader.str();
    if (!reader.ok() || id.empty()) return;
    skinCatalog_.erase(std::remove_if(skinCatalog_.begin(), skinCatalog_.end(),
                                      [&](const CustomSkin& s) { return s.id == id; }),
                       skinCatalog_.end());
    if (equippedSkinId_ == id) equippedSkinId_.clear();
}

void NetClient::handleChat(ByteReader& reader) {
    const auto channel = static_cast<net::ChatChannel>(reader.u8());
    std::string author = reader.str();
    std::string text = reader.str();
    if (!reader.ok()) return;
    pushChat(channel, std::move(author), std::move(text));
}

void NetClient::handleNotice(ByteReader& reader) {
    // The reference has no toast layer: a server announcement is a System line
    // in the transcript and nothing else, so the severity byte is read to keep
    // the frame aligned and then dropped.
    reader.u8();
    std::string text = reader.str();
    if (!reader.ok()) return;
    addSystemMessage(text);
}

void NetClient::handleDied(ByteReader& reader) {
    killerName_ = reader.str();
    reader.u32();   // xp lost
    reader.u32();   // ticks survived
    if (!reader.ok()) return;
    dead_ = true;
}

void NetClient::handlePong(ByteReader& reader) {
    const std::uint64_t sentAt = reader.u64();
    reader.u64();   // server time
    if (!reader.ok()) return;
    pingMillis_ = nowMillis() - static_cast<double>(sentAt);

    pingHistory_.push_back(pingMillis_);
    if (pingHistory_.size() > kPingSamples) pingHistory_.erase(pingHistory_.begin());
    double total = 0;
    for (const double sample : pingHistory_) total += sample;
    averagePingMillis_ = total / static_cast<double>(pingHistory_.size());
}

const char* NetClient::connectionQuality() const {
    if (averagePingMillis_ > 200.0) return "slow";
    if (averagePingMillis_ > 100.0) return "medium";
    return "good";
}

void NetClient::handleDebugStats(ByteReader& reader) {
    ServerDebugStats stats;
    stats.residentBytes = reader.f64();
    stats.heapBytes = reader.f64();
    stats.tickAvgMillis = reader.f32();
    stats.tickMaxMillis = reader.f32();
    if (!reader.ok()) return;
    serverDebugStats_ = stats;
    haveServerDebugStats_ = true;
    serverDebugStatsFresh_ = true;
}

bool NetClient::takeServerDebugStats(ServerDebugStats& out) {
    if (!serverDebugStatsFresh_) return false;
    serverDebugStatsFresh_ = false;
    out = serverDebugStats_;
    return true;
}

void NetClient::takeWireStats(std::uint32_t& inBytes, std::uint32_t& outBytes,
                              std::vector<WireEvent>& top, std::size_t topCount) {
    inBytes = 0;
    outBytes = 0;
    top.clear();
    for (std::size_t id = 0; id < incomingBytes_.size(); ++id) {
        const std::uint32_t bytes = incomingBytes_[id];
        if (bytes == 0) continue;
        inBytes += bytes;
        top.push_back({serverMessageName(static_cast<std::uint8_t>(id)), bytes, true});
    }
    for (std::size_t id = 0; id < outgoingBytes_.size(); ++id) {
        const std::uint32_t bytes = outgoingBytes_[id];
        if (bytes == 0) continue;
        outBytes += bytes;
        top.push_back({clientMessageName(static_cast<std::uint8_t>(id)), bytes, false});
    }
    incomingBytes_.fill(0);
    outgoingBytes_.fill(0);
    std::sort(top.begin(), top.end(),
              [](const WireEvent& a, const WireEvent& b) { return a.bytes > b.bytes; });
    if (top.size() > topCount) top.resize(topCount);
}

void NetClient::handleKick(ByteReader& reader) {
    const std::string reason = reader.str();
    if (!reader.ok()) return;
    lastError_ = reason;
    status_ = Status::Failed;
    dialer_.disconnect();
}

void NetClient::handleDailyStreak(ByteReader& reader) {
    DailyStreak next;
    next.streak = reader.u16();
    next.newDay = reader.boolean();
    next.starsAwarded = reader.u16();
    next.nextClaimAtMillis = reader.i64();
    next.streakExpiresAtMillis = reader.i64();
    if (!reader.ok()) return;
    next.known = true;
    dailyStreak_ = next;
}

} // namespace flix
