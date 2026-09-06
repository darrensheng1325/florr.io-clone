// The chat command surface: everything a player can type that begins with '/'.
//
// This build used to have none. handleChat sanitised the line and broadcast
// it, so "/help" reached every player as the literal text "/help" and the
// command table the client offers in its autocomplete answered nothing at all.
//
// Three rules run through the whole file:
//
//  * A slash line is never broadcast. It is answered, refused, or reported
//    unknown. handleChatCommand returning true is what says "dealt with".
//
//  * Every System line here says exactly what the browser build's server says
//    for the same command, word for word. That build renders HTML and joins
//    its output with `<br/>` inside one message; this client renders text, so
//    the same words arrive with the markup dropped and one message per line.
//    Nothing gets a message the reference does not send, and nothing gets
//    wording of its own -- src/server is the source of truth for the text.
//
//  * A command this build has no counterpart for is simply not answered, the
//    way the reference leaves an unrecognised admin verb unanswered. It is not
//    told about, apologised for, or listed in /help.

#include "server/game_server.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "server/auto_update.h"
#include "server/bot_identity.h"
#include "server/guilds.h"
#include "server/systems/spawning.h"
#include "server/text.h"
#include "shared/game/config.h"
#include "shared/game/constants.h"
#include "shared/game/player_flags.h"
#include "shared/game/terrain.h"

namespace flix {

namespace {

/// Parses a decimal integer, whole-token. Returns false on anything with
/// trailing rubbish, so `spawn bee rare 10x` is a diagnostic rather than ten.
bool parseInteger(const std::string& text, int& out) {
    if (text.empty()) return false;
    std::size_t used = 0;
    long value = 0;
    try {
        value = std::stol(text, &used);
    } catch (...) {
        return false;
    }
    if (used != text.size()) return false;
    out = static_cast<int>(value);
    return true;
}

bool parseNumber(const std::string& text, double& out) {
    if (text.empty()) return false;
    std::size_t used = 0;
    double value = 0;
    try {
        value = std::stod(text, &used);
    } catch (...) {
        return false;
    }
    if (used != text.size() || !std::isfinite(value)) return false;
    out = value;
    return true;
}

/// The map is exactly kWorldSize on a side and nothing outside it is
/// meaningful: the terrain has no tiles there, the section lookup returns -1,
/// and a body parked past the edge is invisible to the spawner's whole
/// neighbourhood pass. Refusing is better than clamping, because a typo'd
/// coordinate should be reported rather than silently turned into a corner.
bool saneCoordinate(double v) {
    return std::isfinite(v) && v >= 0.0 && v <= kWorldSize;
}

/// The reference prints "Max is ±1000000" from its own anti-hang guard; this
/// build's guard is the map itself, so the same sentence carries this number.
std::string maxCoordinateText() {
    return std::to_string(static_cast<long>(kWorldSize));
}

/// Rarity by name, strictly -- unlike parseRarity(), which reads unknown text
/// as Common so a hand-edited save degrades instead of failing to load. A
/// typed command wants the typo reported.
bool parseRarityStrict(const std::string& text, Rarity& out) {
    const std::string key = lowerCase(text);
    for (int i = 0; i < kRarityCount; ++i) {
        if (key == kRarityNames[static_cast<std::size_t>(i)]) {
            out = static_cast<Rarity>(i);
            return true;
        }
    }
    return false;
}

std::string rarityList() {
    std::string out;
    for (int i = 0; i < kRarityCount; ++i) {
        if (i > 0) out += ", ";
        out += kRarityNames[static_cast<std::size_t>(i)];
    }
    return out;
}

/// The three cosmetic bits, in the reference's own enum order. `set_skin`
/// takes a name from this list, "none", or a raw bitmask, and its usage line
/// is built from it exactly as the reference builds its own.
constexpr std::array<const char*, 3> kSkinNames = {"Pumpkin", "Robot", "Glitch"};

std::string skinNameList(const char* separator) {
    std::string out;
    for (std::size_t i = 0; i < kSkinNames.size(); ++i) {
        if (i > 0) out += separator;
        out += kSkinNames[i];
    }
    return out;
}

std::string joined(const std::vector<std::string>& words, std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < words.size(); ++i) {
        if (!out.empty()) out += ' ';
        out += words[i];
    }
    return out;
}

/// Everything after the first run of whitespace, trimmed. The argument of a
/// one-argument command, kept whole so a guild name or a notification message
/// may contain spaces.
std::string argumentOf(const std::string& line) {
    const std::size_t space = line.find_first_of(" \t");
    if (space == std::string::npos) return {};
    return trimmed(line.substr(space + 1));
}

/// `30`, `30s`, `5m`, `2h` -- the duration grammar `restart` and `update`
/// share. A bare number is SECONDS, which is what an operator typing
/// `restart 30` means; whitespace between the number and its unit is allowed
/// because the reference's own pattern allows it.
bool parseDurationMillis(const std::string& text, double& out) {
    std::size_t digits = 0;
    while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) {
        ++digits;
    }
    if (digits == 0) return false;
    std::string unit = text.substr(digits);
    while (!unit.empty() && (unit.front() == ' ' || unit.front() == '\t')) unit.erase(0, 1);
    for (char& c : unit) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    double scale = 0;
    if (unit.empty() || unit == "s" || unit == "sec" || unit == "secs") scale = 1000.0;
    else if (unit == "m" || unit == "min" || unit == "mins") scale = 60000.0;
    else if (unit == "h" || unit == "hr" || unit == "hrs") scale = 3600000.0;
    else return false;

    out = std::strtod(text.substr(0, digits).c_str(), nullptr) * scale;
    return true;
}

/// The reference prints byte counts as `(bytes / 1024).toFixed(1)` KB.
std::string kilobytes(std::size_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", static_cast<double>(bytes) / 1024.0);
    return buffer;
}

/// The verb: the first word, lower-cased.
std::string verbOf(const std::string& line) {
    const std::vector<std::string> words = splitWords(line);
    return words.empty() ? std::string() : lowerCase(words[0]);
}

std::string plural(int n, const char* singular, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? singular : many);
}

/// "3m ago", "2h 5m ago", "just now" -- the browser build's own wording for
/// the last-active column.
std::string agoLabel(std::int64_t millis) {
    const long minutes = static_cast<long>(millis / 60000);
    if (minutes < 1) return "just now";
    if (minutes < 60) return std::to_string(minutes) + "m ago";
    return std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m ago";
}

/// The reference prints `new Date(createdAt).toLocaleString()`, which on its
/// servers is the en-US form. Same shape, from the same instant.
std::string localeTimestamp(std::int64_t millis) {
    const std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    char buffer[64];
    if (std::strftime(buffer, sizeof buffer, "%m/%d/%Y, %I:%M:%S %p", &parts) == 0) return {};
    return buffer;
}

/// Eight characters of A-Z0-9, as the browser build mints them, so a code from
/// either server looks the same and pastes into the same shop field.
std::string mintCode(Rng& rng) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string code;
    code.reserve(8);
    for (int i = 0; i < 8; ++i) code.push_back(kAlphabet[rng.below(36)]);
    return code;
}

/// The two lines several commands share, spelled once. The mute notice is
/// the reference's own coloured span; the unknown-command line names the same
/// five commands its chat handler names.
constexpr const char* kMutedNotice =
    "<span style=\"color: #ff8866;\">You are muted and cannot send chat messages.</span>";
constexpr const char* kUnknownCommand =
    "Unknown command. Available commands: /biome, /level-from-string, /loadout-from-string, "
    "/create-api-key, /delete-api-key";
/// The squad usage line, which is both the answer to a bare `/squad` and to a
/// subcommand nothing matches.
constexpr const char* kSquadCommands =
    "Squad commands: /squad-create [public|private], /squad-invite &lt;username&gt;, "
    "/squad-find-public, /squad-join &lt;squadId&gt;, /squad-public, /squad-private, "
    "/squad-accept, /squad-decline, /squad-leave, /squad-info";

/// Chat content is markup, so a '<' in an admin command's output would open a
/// tag and take the rest of the line with it -- which is exactly what happens
/// in the browser, where `Usage: teleport <playerId/username> <x> <y>` renders
/// as "Usage: teleport" and nothing else. The admin console sends no markup of
/// its own, so escaping every line of it is how the same words reach the
/// screen instead of being swallowed.
std::string escaped(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

/// The four type tags the browser stores against a notification.
bool validNotificationType(const std::string& type) {
    return type == "super_craft" || type == "unique_craft" || type == "apex_craft" ||
           type == "star_code";
}

} // namespace

// ---------------------------------------------------------------------------
// Plumbing
// ---------------------------------------------------------------------------

void GameServer::sendSystem(net::Connection& connection, const std::string& text) {
    sendChatTo(connection, net::ChatChannel::System, "System", text);
}

bool GameServer::effectiveAdmin(const Session& session) const {
    return session.admin || tempAdmins_.count(session.connection) != 0;
}

void GameServer::revokeTempAdmin(net::ConnectionId id) {
    if (tempAdmins_.erase(id) == 0) return;
    // The client learns its admin standing from the skin catalog's leading
    // flag, and that flag is what un-hides the admin rows in its command
    // autocomplete. Resending it is how a revoked console disappears from the
    // grantee's screen rather than lingering until their next login.
    if (Session* session = sessionFor(id)) {
        if (net::Connection* connection = listener_.find(id)) sendSkinCatalog(*session, *connection);
    }
}

bool GameServer::resolveCommandTarget(const std::string& identifier, CommandTarget& out) {
    const std::string key = lowerCase(trimmed(identifier));
    if (key.empty()) return false;

    // Account name first: it is the spelling an admin types, and it is unique.
    // A nameplate is neither -- two flowers may share one -- so it is only
    // consulted when nothing owns the name as an account.
    for (auto& entry : sessions_) {
        Session& session = entry.second;
        if (!session.playing()) continue;
        if (lowerCase(session.username) != key) continue;
        out = {session.entity, &session,
               session.displayName.empty() ? session.username : session.displayName};
        return true;
    }

    Query<PlayerTag, PlayerAccount> flowers{world_};
    Entity found = NULL_ENTITY;
    std::string foundName;
    flowers.each([&](Entity entity, PlayerTag&, PlayerAccount& account) {
        if (found != NULL_ENTITY) return;
        if (lowerCase(account.username) != key) return;
        found = entity;
        foundName = account.username;
    });
    if (found == NULL_ENTITY) return false;

    out = {found, sessionForEntity(found), foundName};
    return true;
}

void GameServer::teleportEntity(Entity entity, Vec2 position) {
    if (!world_.isAlive(entity)) return;
    if (Transform* transform = world_.tryGet<Transform>(entity)) transform->position = position;
    // Velocity and knockback are properties of where the body WAS going. A
    // flower dropped across the map still carrying the impulse that was
    // pushing it there arrives sliding.
    if (Motion* motion = world_.tryGet<Motion>(entity)) motion->velocity = {0, 0};
    if (Knockback* knockback = world_.tryGet<Knockback>(entity)) knockback->impulse = {0, 0};
    // No wire message: the client cuts its interpolation on any jump past
    // kTeleportSnapDistance (600 units) on its own, which every teleport worth
    // the name exceeds. One under that distance is close enough that easing to
    // it looks better than snapping.
}

// ---------------------------------------------------------------------------
// The '/' dispatch
// ---------------------------------------------------------------------------

bool GameServer::handleChatCommand(Session& session, net::Connection& connection,
                                   const std::string& message) {
    if (message.empty() || message[0] != '/') return false;

    const std::string verb = verbOf(message);
    const std::string argument = argumentOf(message);
    const auto out = [&](const std::string& text) { sendSystem(connection, text); };

    // -- channel shorthands ------------------------------------------------

    if (verb == "/g") {
        // An empty `/g` is not answered at all, which is what the reference
        // does with it: there is nothing to say and nothing to send.
        if (argument.empty()) return true;
        const Account* account = database_.findUser(session.username);
        if (account != nullptr && account->muted) {
            out(kMutedNotice);
            return true;
        }
        const std::string guildName = guildNameForUser(session.username);
        if (guildName.empty()) {
            out("You are not in a guild.");
            return true;
        }
        const Json& guild = database_.storedTable("guilds")[guildName];
        const Json& members = guild["memberUsernames"];
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (net::Connection* peer = connectionForUser(members[i].asString())) {
                // The reference's own signature: "[Guild NAME] @username", so
                // the guild tag and the speaker both read off the sender line.
                sendChatTo(*peer, net::ChatChannel::System,
                           "[Guild " + guildName + "] @" + session.username, argument);
            }
        }
        return true;
    }

    if (verb == "/s") {
        if (argument.empty()) return true;
        const Account* account = database_.findUser(session.username);
        if (account != nullptr && account->muted) {
            out(kMutedNotice);
            return true;
        }
        const Squad* squad = squads_.forMember(squadIdOf(session));
        if (squad == nullptr) {
            out("You are not in a squad.");
            return true;
        }
        // The reference's own signature: "[Squad] @username" over the line, and
        // the flower's nameplate in yellow inside it, so a squad line says both
        // who typed it and which flower on the screen that is.
        const std::string body = "[<span style=\"color: yellow;\">" +
                                 squadDisplayName(squadIdOf(session)) + "</span>] " + argument;
        for (const SquadMemberId& member : squad->members) {
            if (net::Connection* peer = squadConnection(member)) {
                sendChatTo(*peer, net::ChatChannel::Squad, "[Squad] @" + session.username, body);
            }
        }
        return true;
    }

    // -- squads ------------------------------------------------------------
    //
    // The reference accepts both spellings of every one of these -- `/squad
    // invite bob` and `/squad-invite bob` -- by rewriting the hyphen form into
    // the space form before it parses. Same here, so there is ONE parser and
    // the two spellings cannot answer differently.
    if (verb == "/squad" || verb.rfind("/squad-", 0) == 0) {
        const std::vector<std::string> words = splitWords(message);
        std::string sub;
        std::size_t firstArgument = 1;
        if (verb == "/squad") {
            sub = words.size() > 1 ? lowerCase(words[1]) : std::string();
            firstArgument = 2;
        } else {
            sub = verb.substr(std::string("/squad-").size());
        }
        runSquadCommand(session, connection,
                        sub, words.size() > firstArgument ? words[firstArgument] : std::string());
        return true;
    }

    // -- the admin console -------------------------------------------------

    if (verb == "/admin" || verb == "/cmd") {
        if (!effectiveAdmin(session)) {
            // The browser build denies the command's EXISTENCE rather than the
            // permission, and so does this: telling a stranger that a console
            // is there is half of finding a way into it.
            out("Command does not exist.");
            return true;
        }
        if (argument.empty()) {
            out(kUnknownCommand);
            return true;
        }
        runAdminCommand(session, connection, argument);
        return true;
    }

    // -- guilds ------------------------------------------------------------
    //
    // Routed into the same cores the guild panel's binary messages use, so the
    // two roads cannot disagree about who may do what.

    if (verb == "/guild-create") { guildCreate(session, connection, argument); return true; }
    if (verb == "/guild-invite") { guildInvite(session, connection, argument); return true; }
    if (verb == "/guild-kick")   { guildKick(session, connection, argument); return true; }
    if (verb == "/guild-accept") { handleGuildAccept(session, connection); return true; }
    if (verb == "/guild-decline"){ handleGuildDecline(session, connection); return true; }
    if (verb == "/guild-leave")  { handleGuildLeave(session, connection); return true; }
    if (verb == "/guild-squad")  { handleGuildSquadAll(session, connection); return true; }

    if (verb == "/guild-info") {
        const std::string guildName = guildNameForUser(session.username);
        if (guildName.empty()) {
            out("You are not in a guild.");
            return true;
        }
        const Json& guild = database_.storedTable("guilds")[guildName];
        const Json& members = guild["memberUsernames"];
        const std::string leader = lowerCase(guild["leaderUsername"].asString());
        std::string lines;
        for (std::size_t i = 0; i < members.size(); ++i) {
            const std::string member = members[i].asString();
            if (i > 0) lines += "<br/>";
            // A green dot for an online member, grey for one who is not --
            // with U+2022 where the reference writes U+25CF, which is the one
            // character in any of these lines this client's font cannot draw.
            lines += sessionForUser(member) != nullptr
                         ? "<span style=\"color: #6eff6e;\">&#8226;</span>"
                         : "<span style=\"color: #888;\">&#8226;</span>";
            lines += " @" + member;
            if (lowerCase(member) == leader) {
                lines += " <span style=\"color: #ffd54f;\">(Leader)</span>";
            }
        }
        out("<span style=\"color: #ffb74d;\">Guild \"" + guildName + "\" (" +
            std::to_string(members.size()) + "/" + std::to_string(kMaxGuildSize) + "):<br/>" +
            lines + "</span>");
        return true;
    }

    if (verb == "/guild-list") {
        const Json& guilds = database_.storedTable("guilds");
        if (!guilds.isObject() || guilds.keys().empty()) {
            out("No guilds exist yet.");
            return true;
        }
        std::string lines;
        for (const std::string& key : guilds.keys()) {
            const Json& guild = guilds[key];
            if (!lines.empty()) lines += "<br/>";
            lines += "\"" + key + "\" \xE2\x80\x94 " +
                     std::to_string(guild["memberUsernames"].size()) + "/" +
                     std::to_string(kMaxGuildSize) + " \xE2\x80\x94 leader @" +
                     guild["leaderUsername"].asString();
        }
        out("<span style=\"color: #ffb74d;\">Guilds:<br/>" + lines + "</span>");
        return true;
    }

    // Any other /guild-* line: the reference normalises it to `/guild <word>`,
    // matches no subcommand, and answers with the list of the ones it has.
    if (verb.rfind("/guild", 0) == 0) {
        out("Guild commands: /guild-create &lt;name&gt;, /guild-invite &lt;username&gt;, "
            "/guild-accept, /guild-decline, /guild-leave, /guild-kick &lt;username&gt;, "
            "/guild-info, /guild-squad, /guild-list");
        return true;
    }

    // -- account and world questions ---------------------------------------

    if (verb == "/biome") {
        // Every flower counts, bots included: the question is where the world
        // is busy, and a section full of bots is a section full of fights.
        std::array<int, kSectionCount> counts{};
        int total = 0;
        Query<PlayerTag, Transform> flowers{world_};
        flowers.each([&](Entity entity, PlayerTag&, Transform& transform) {
            if (world_.has<Dead>(entity)) return;
            const int section = sectionAt(transform.position);
            if (section < 0) return;
            ++counts[static_cast<std::size_t>(section)];
            ++total;
        });
        if (total == 0) {
            out("No players are currently in any section.");
            return true;
        }
        // Busiest first, which is the order the reference sorts its breakdown
        // into and where it reads the headline off.
        std::vector<int> populated;
        for (int i = 0; i < kSectionCount; ++i) {
            if (counts[static_cast<std::size_t>(i)] > 0) populated.push_back(i);
        }
        std::stable_sort(populated.begin(), populated.end(), [&](int a, int b) {
            return counts[static_cast<std::size_t>(a)] > counts[static_cast<std::size_t>(b)];
        });
        std::string breakdown;
        for (int section : populated) {
            if (!breakdown.empty()) breakdown += "<br/>";
            breakdown += std::string(biomeOf(section).name) + ": " +
                         std::to_string(counts[static_cast<std::size_t>(section)]);
        }
        const int best = populated.front();
        out(std::string("<span style=\"color: #4fc3f7;\">Most populated biome: <b>") +
            biomeOf(best).name + "</b> (" +
            plural(counts[static_cast<std::size_t>(best)], "player", "players") + ")</span><br/>" +
            breakdown);
        return true;
    }

    if (verb == "/level-from-string" || verb == "/loadout-from-string") {
        if (argument.empty()) {
            out("Usage: " + verb + " &lt;name&gt;");
            return true;
        }
        const BotIdentity identity = botIdentityForName(argument, kLoadoutActiveSlots, kMaxLevel);
        if (verb == "/level-from-string") {
            out("\"" + argument + "\" would be level " + std::to_string(identity.level) + ".");
            return true;
        }
        std::string lines;
        for (std::size_t i = 0; i < identity.slots.size(); ++i) {
            const BotIdentity::Slot& slot = identity.slots[i];
            const std::string petal = slot.petalIndex == kInvalidIndex
                                          ? std::string("(none)")
                                          : content().petal(slot.petalIndex).id;
            if (i > 0) lines += "<br/>";
            lines += "Slot " + std::to_string(i + 1) + ": " + rarityName(slot.rarity) + " " + petal;
        }
        out("\"" + argument + "\" loadout:<br/>" + lines);
        return true;
    }

    if (verb == "/create-api-key") {
        // The key is minted into the same `apiKeys` table the browser build's
        // HTTP API authenticates against, so a key made here works there.
        const std::string label = argument.empty() ? session.username : argument;
        std::string body;
        Rng keyRng(static_cast<std::uint64_t>(database_.nowMillis()) ^
                   (static_cast<std::uint64_t>(hashName(session.userId)) << 21));
        static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        body.reserve(64);
        for (int i = 0; i < 64; ++i) body.push_back(kAlphabet[keyRng.below(36)]);
        const std::string key = "sk_" + body;

        Json entry = Json::object();
        entry["key"] = key;
        entry["username"] = session.username;
        entry["label"] = label;
        entry["createdAt"] = static_cast<double>(database_.nowMillis());
        database_.rawTable("apiKeys")[key] = std::move(entry);
        database_.markDirty();

        out("<b>[API KEY CREATED]</b><br/>Label: " + label + "<br/>Key: <b>" + key +
            "</b><br/>Send this on requests as the X-API-Key header, or append "
            "?api_key=&lt;key&gt; to the URL. Save it now \xE2\x80\x94 the full key is not shown "
            "again.<br/>" +
            (session.admin
                 ? "Your account is admin, so this key has admin scope (can create star codes, "
                   "broadcast notifications, etc.)."
                 : "Your account is not admin, so this key has user scope only (read events, "
                   "whoami). Admin endpoints will return 403."));
        return true;
    }

    if (verb == "/delete-api-key") {
        if (argument.empty()) {
            out("Usage: /delete-api-key &lt;key-or-prefix&gt;");
            return true;
        }
        // Only this account's own keys are visible here, by prefix or in full.
        // Removing somebody else's stays an out-of-band operation, so the
        // command can never escalate across users however it is typed at.
        Json& keys = database_.rawTable("apiKeys");
        std::vector<std::string> owned;
        for (const std::string& key : keys.keys()) {
            if (lowerCase(keys[key]["username"].asString()) == lowerCase(session.username)) {
                owned.push_back(key);
            }
        }
        std::vector<std::string> matches;
        for (const std::string& key : owned) {
            if (key == argument) { matches.assign(1, key); break; }
            if (key.rfind(argument, 0) == 0) matches.push_back(key);
        }
        if (matches.size() > 1) {
            out("Prefix \"" + argument + "\" is ambiguous \xE2\x80\x94 matches " +
                std::to_string(matches.size()) + " of your keys. Provide more characters.");
            return true;
        }
        if (matches.empty()) {
            out("No API key of yours matched that key or prefix.");
            return true;
        }
        const std::string label = keys[matches[0]]["label"].asString();
        keys.erase(matches[0]);
        database_.markDirty();
        out("Deleted API key \"" + label + "\" (" + matches[0].substr(0, 10) + "...).");
        return true;
    }

    if (verb == "/help") {
        // One message, laid out with the reference's own markup: the client
        // parses this subset, so the listing arrives looking the way it looks
        // in the browser rather than as a wall of tags.
        std::string help = "Available commands:\n";
        help += "/biome - Show the most populated biome <br/>";
        help += "/level-from-string &lt;name&gt; - Show what level a bot named &lt;name&gt; would "
                "roll <br/>";
        help += "/loadout-from-string &lt;name&gt; - Show the loadout a bot named &lt;name&gt; "
                "would roll <br/>";
        help += "/create-api-key [label] - Issue an API key tied to your account for /api/v1/* "
                "<br/>";
        help += "/delete-api-key &lt;key-or-prefix&gt; - Revoke one of your API keys <br/>";
        help += "<br/><b>Squad commands (groups of " + std::to_string(kMaxSquadSize) +
                ", share loot as one instance):</b><br/>";
        help += "/squad-create [public|private] - Create a new squad (defaults to private)<br/>";
        help += "/squad-invite &lt;username&gt; - Invite a player to your squad<br/>";
        help += "/squad-find-public - List joinable public squads<br/>";
        help += "/squad-join &lt;squadId&gt; - Join a public squad<br/>";
        help += "/squad-public / /squad-private - Toggle your squad's visibility (leader "
                "only)<br/>";
        help += "/squad-accept / /squad-decline - Respond to an invite<br/>";
        help += "/squad-leave - Leave your squad<br/>";
        help += "/squad-info - Show squad members<br/>";
        help += "/s &lt;message&gt; - Send a message to your squad<br/>";
        help += "<br/><b>Guild commands (up to " + std::to_string(kMaxGuildSize) +
                " members, persistent):</b><br/>";
        help += "/guild-create &lt;name&gt; - Create a new guild (5-char alphanumeric ID)<br/>";
        help += "/guild-invite &lt;username&gt; - Invite a player (leader only)<br/>";
        help += "/guild-accept / /guild-decline - Respond to a guild invite<br/>";
        help += "/guild-leave - Leave your guild<br/>";
        help += "/guild-kick &lt;username&gt; - Kick a member (leader only)<br/>";
        help += "/guild-info - Show guild info<br/>";
        help += "/guild-squad - Invite online guildmates into a squad<br/>";
        help += "/guild-list - List all guilds<br/>";
        help += "/guild-menu - Toggle guild menu panel (client, also \"G\" key)<br/>";
        help += "/g &lt;message&gt; - Send a message to your guild<br/>";
        help += "<br/>Chat supports HTML tags: <b>bold</b>, <i>italic</i>, <u>underline</u>, "
                "<span style=\"color: red\">colored text</span>, <blink>blinking text</blink>";
        if (effectiveAdmin(session)) {
            // The reference leaves its own angle brackets unescaped here, which
            // its sanitiser then eats along with the rest of the line. Escaped,
            // so the same words survive to the screen.
            help += "<br/><br/>Admin commands:<br/>";
            help += "/admin &lt;command&gt; - Execute server command<br/>";
            help += "/cmd &lt;command&gt; - Execute server command (alternative)<br/>";
            help += "Available server commands: save, list-players, list-sockets, "
                    "set_max_enemies, set_bot_count &lt;0-" + std::to_string(kMaxBots) +
                    "|default&gt;, spawn_special_mobs, spawn &lt;mobType&gt; &lt;rarity&gt; "
                    "[x] [y] [amount] [stack|unstack], killall (kill all wild mobs), teleport "
                    "&lt;playerId/username&gt; &lt;x&gt; &lt;y&gt;, teleport_all &lt;x&gt; "
                    "&lt;y&gt; (move every player and bot), teleport_bots &lt;x&gt; &lt;y&gt; "
                    "(move every bot only), give &lt;playerId/username&gt; &lt;itemType&gt; "
                    "&lt;rarity&gt; [amount], set_skin &lt;playerId/username&gt; "
                    "&lt;skin|none&gt;, corrupt &lt;playerId/username&gt; [on|off|toggle] "
                    "(corrupted flowers fight players anywhere, not just in PVP), grant_admin "
                    "&lt;playerId/username&gt; (lend the admin console until they respawn), "
                    "revoke_admin &lt;playerId/username&gt;, list_admins, mute "
                    "&lt;playerId/username&gt; (bar an account from chat, persists across "
                    "sessions), unmute &lt;playerId/username&gt;, notification &lt;type&gt; "
                    "&lt;message&gt;, clear_notifications, delete_guests, list_today_logins, "
                    "guild_list, guild_info &lt;guild name&gt;, guild_force_join &lt;guild "
                    "name&gt; &lt;username&gt;, restart [&lt;N&gt;(s|m|h)|cancel|status], "
                    "backup_db [list], update [now|&lt;N&gt;(s|m|h)|status|cancel] (backs up "
                    "DB first, then installs latest build + restarts), change-maze "
                    "[next|garden|desert|ocean|&lt;dayNumber&gt;]";
        }
        out(help);
        return true;
    }

    out(kUnknownCommand);
    return true;
}

// ---------------------------------------------------------------------------
// Squads
// ---------------------------------------------------------------------------

void GameServer::runSquadCommand(Session& session, net::Connection& connection,
                                 const std::string& sub, const std::string& argument) {
    const auto out = [&](const std::string& text) { sendSystem(connection, text); };
    const SquadMemberId me = squadIdOf(session);
    const auto now = static_cast<std::int64_t>(clockMillis_);

    if (sub == "create") {
        const bool isPublic = lowerCase(argument) == "public";
        Squad* squad = squads_.create(me, isPublic, rng_);
        if (squad == nullptr) {
            out("You are already in a squad.");
            return;
        }
        sendSquadUpdate(connection, squad);
        out(std::string("<span style=\"color: #4fc3f7;\">") + (isPublic ? "public" : "private") +
            " squad created! Use /squad-invite &lt;username&gt; to invite players" +
            (isPublic ? ", or wait for others to join via /squad-find-public" : "") + ".</span>");
        return;
    }

    if (sub == "invite" && !argument.empty()) {
        SquadMemberId target;
        if (!resolveSquadTarget(argument, target)) {
            out("Player \"" + argument + "\" not found.");
            return;
        }
        if (target == me) {
            out("You cannot invite yourself.");
            return;
        }
        if (target.bot()) {
            // A bot has nobody to answer an invitation, so it joins outright.
            // That is also the ONLY way a squad ever gets one, which is why the
            // leader test is repeated here rather than left to invite().
            Squad* squad = squads_.forMember(me);
            if (squad == nullptr) {
                out("You are not in a squad. Use /squad create first.");
                return;
            }
            if (!(squad->leader == me)) {
                out("Only the squad leader can invite players.");
                return;
            }
            const std::string error = squads_.addBot(squad->id, target);
            if (!error.empty()) {
                out(error);
                return;
            }
            sendSquadSystem(*squad, squadDisplayName(target) + " has joined the squad.");
            broadcastSquadUpdate(*squad);
            return;
        }
        const std::string error = squads_.invite(me, target, session.username, now);
        if (!error.empty()) {
            out(error);
            return;
        }
        out("<span style=\"color: #4fc3f7;\">Invite sent to " + argument + ".</span>");
        if (net::Connection* peer = squadConnection(target)) {
            sendSystem(*peer, "<span style=\"color: #4fc3f7;\">@" + session.username +
                                  " has invited you to their squad. Use /squad accept or "
                                  "/squad decline.</span>");
        }
        return;
    }

    if (sub == "find-public") {
        const std::vector<const Squad*> open = squads_.publicSquads();
        if (open.empty()) {
            out("No public squads available. Create one with /squad create public.");
            return;
        }
        std::string lines;
        for (const Squad* squad : open) {
            if (!lines.empty()) lines += "<br/>";
            // An em dash as bytes, where the reference writes `&mdash;`: the
            // client's entity table carries only the handful the server
            // actually needs, and one more name in it is one more thing to
            // keep true.
            lines += squad->id + " \xE2\x80\x94 leader: " + squadDisplayName(squad->leader) +
                     " (" + std::to_string(squad->members.size()) + "/" +
                     std::to_string(kMaxSquadSize) + ") [/squad-join " + squad->id + "]";
        }
        out("<span style=\"color: #4fc3f7;\">Public squads:<br/>" + lines + "</span>");
        return;
    }

    if (sub == "join" && !argument.empty()) {
        const std::string error = squads_.joinPublic(argument, me);
        if (!error.empty()) {
            out(error);
            return;
        }
        Squad* squad = squads_.find(argument);
        if (squad == nullptr) return;
        sendSquadSystem(*squad, squadDisplayName(me) + " has joined the squad.");
        broadcastSquadUpdate(*squad);
        return;
    }

    if (sub == "public" || sub == "private") {
        Squad* squad = nullptr;
        const std::string error = squads_.setVisibility(me, sub == "public", &squad);
        if (!error.empty()) {
            out(error);
            return;
        }
        sendSquadSystem(*squad, "Squad is now " + sub + ".");
        broadcastSquadUpdate(*squad);
        return;
    }

    if (sub == "accept") {
        std::string squadId;
        const std::string error = squads_.accept(me, now, squadId);
        if (!error.empty()) {
            out(error);
            return;
        }
        Squad* squad = squads_.find(squadId);
        if (squad == nullptr) return;
        sendSquadSystem(*squad, squadDisplayName(me) + " has joined the squad.");
        broadcastSquadUpdate(*squad);
        return;
    }

    if (sub == "decline") {
        squads_.decline(me);
        out("Squad invite declined.");
        return;
    }

    if (sub == "leave") {
        if (squads_.forMember(me) == nullptr) {
            out("You are not in a squad.");
            return;
        }
        departSquad(session, &connection, squadDisplayName(me));
        out("You have left the squad.");
        return;
    }

    if (sub == "info") {
        const Squad* squad = squads_.forMember(me);
        if (squad == nullptr) {
            out("You are not in a squad.");
            return;
        }
        std::string lines;
        for (const SquadMemberId& member : squad->members) {
            if (!lines.empty()) lines += "<br/>";
            lines += "@" + squadAccountName(member) + " [" + squadDisplayName(member) + "]";
            if (squad->leader == member) lines += " (Leader)";
        }
        out("<span style=\"color: #4fc3f7;\">Squad " + squad->id + " [" +
            (squad->isPublic ? "public" : "private") + "] (" +
            std::to_string(squad->members.size()) + "/" + std::to_string(kMaxSquadSize) +
            "):<br/>" + lines + "</span>");
        return;
    }

    out(kSquadCommands);
}

// ---------------------------------------------------------------------------
// The admin console
// ---------------------------------------------------------------------------

void GameServer::runAdminCommand(Session& session, net::Connection& connection,
                                 const std::string& command) {
    // Escaped: the console's own output is plain text, and an unescaped
    // "<x>" in a usage line would be read as a tag and swallow the rest of
    // it (which is what it does in the browser).
    const auto out = [&](const std::string& text) { sendSystem(connection, escaped(text)); };

    const std::vector<std::string> words = splitWords(command);
    if (words.empty()) return;
    const std::string verb = lowerCase(words[0]);
    const std::string rest = argumentOf(trimmed(command));

    out("[ADMIN] " + session.username + " executed: " + command);

    // -- accounts and sessions ---------------------------------------------

    if (verb == "save") {
        if (words.size() >= 2) {
            CommandTarget target;
            if (!resolveCommandTarget(words[1], target) || target.session == nullptr) {
                out("Player " + words[1] + " not found");
                return;
            }
            persistPlayer(*target.session);
            out("Saved player " + target.name + " (" +
                std::to_string(target.session->connection) + ")");
            return;
        }
        int saved = 0;
        for (auto& entry : sessions_) {
            if (!entry.second.playing()) continue;
            persistPlayer(entry.second);
            ++saved;
        }
        // Straight to disk rather than waiting out the rate limiter: an
        // operator typing `save` is usually about to restart something.
        database_.save();
        out("Saved " + std::to_string(saved) + " player(s)");
        return;
    }

    if (verb == "list-players") {
        std::vector<std::string> rows;
        for (auto& entry : sessions_) {
            const Session& other = entry.second;
            if (!other.playing()) continue;
            const PlayerProgress* progress = world_.tryGet<PlayerProgress>(other.entity);
            rows.push_back("Player ID: " + std::to_string(entry.first) + ", Username: " +
                           other.username + ", Nickname: " +
                           (other.displayName.empty() ? other.username : other.displayName) +
                           ", Level: " + std::to_string(progress != nullptr ? progress->level : 0));
        }
        // Bots are players to every system in the world; hiding them here
        // would make the count disagree with what the console can teleport.
        // They own no socket, so the reference reads their username as
        // "Unknown" and this does the same.
        for (const Bot& bot : bots_) {
            if (bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity)) continue;
            const PlayerProgress* progress = world_.tryGet<PlayerProgress>(bot.entity);
            rows.push_back("Player ID: " + std::to_string(bot.entity) +
                           ", Username: Unknown, Nickname: " +
                           (bot.name.empty() ? std::string("(unnamed)") : bot.name) + ", Level: " +
                           std::to_string(progress != nullptr ? progress->level : 0));
        }
        if (rows.empty()) {
            out("No players online");
            return;
        }
        out("Players (" + std::to_string(rows.size()) + "):");
        for (const std::string& row : rows) out(row);
        return;
    }

    if (verb == "list-sockets") {
        if (sessions_.empty()) {
            out("No sockets connected");
            return;
        }
        out("Sockets (" + std::to_string(sessions_.size()) + "):");
        for (auto& entry : sessions_) out("Socket ID: " + std::to_string(entry.first));
        return;
    }

    if (verb == "delete_guests") {
        // Only guests that never played: a guest who levelled up is a player
        // whose account happens to be named like a guest.
        std::vector<std::string> doomed;
        for (const std::string& username : database_.usernames()) {
            // The browser build's pattern: "User" followed by exactly eight
            // digits.
            if (username.size() != 12 || username.rfind("User", 0) != 0) continue;
            bool digits = true;
            for (std::size_t i = 4; i < username.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(username[i]))) digits = false;
            }
            if (!digits) continue;
            const Account* account = database_.findUser(username);
            if (account == nullptr) continue;
            const PlayerRecord* record = database_.findProgress(account->id);
            if (record != nullptr && !Database::isStarterProgress(*record)) continue;
            // Never delete an account that is logged in right now: the session
            // holding it would go on writing progress to a record that no
            // longer exists.
            if (sessionForUser(username) != nullptr) continue;
            doomed.push_back(username);
        }
        for (const std::string& username : doomed) database_.eraseUser(username);
        out("Deleted " + std::to_string(doomed.size()) +
            " guest account(s) and their player data.");
        return;
    }

    if (verb == "list_today_logins" || verb == "list_active") {
        const std::int64_t now = database_.nowMillis();
        const std::int64_t dayAgo = now - 24LL * 60 * 60 * 1000;
        std::vector<std::pair<std::string, std::int64_t>> active;
        for (const std::string& username : database_.usernames()) {
            const Account* account = database_.findUser(username);
            if (account == nullptr || account->lastActiveAtMillis < dayAgo) continue;
            active.emplace_back(account->username, account->lastActiveAtMillis);
        }
        if (active.empty()) {
            out("No accounts active in the last 24 hours.");
            return;
        }
        std::sort(active.begin(), active.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        out("Accounts active in last 24 hours (" + std::to_string(active.size()) + "):");
        for (const auto& [username, stamp] : active) {
            out("  " + username + " \xE2\x80\x94 " + agoLabel(now - stamp));
        }
        return;
    }

    // -- population knobs --------------------------------------------------

    if (verb == "set_max_enemies") {
        int count = 0;
        if (words.size() < 2 || !parseInteger(words[1], count) || count < 0) {
            out("Invalid enemy count. Please provide a valid number.");
            return;
        }
        spawning_->mobCap = count;
        // Lowering the cap does not cull: every spawn path tests it, so the
        // population drains through the ordinary despawn rather than a few
        // hundred mobs vanishing in front of whoever is fighting them.
        out("Max enemies set to " + std::to_string(count));
        return;
    }

    if (verb == "set_bot_count") {
        if (words.size() >= 2 && lowerCase(words[1]) == "default") {
            botCountOverride_ = -1;
            out("Bot count override cleared (using default formula).");
            return;
        }
        int requested = 0;
        if (words.size() < 2 || !parseInteger(words[1], requested) || requested < 0) {
            out("Usage: set_bot_count <0-" + std::to_string(kMaxBots) +
                "|default> \xE2\x80\x94 current override: " +
                (botCountOverride_ < 0 ? std::string("default")
                                       : std::to_string(botCountOverride_)));
            return;
        }
        // Over the cap CLAMPS and applies. The browser build reported the
        // clamp and then returned without applying anything, so
        // `set_bot_count 100` said it had capped at 50 and did nothing at all.
        const int applied = std::min(requested, kMaxBots);
        botCountOverride_ = applied;
        out(applied == requested
                ? "Bot count target set to " + std::to_string(applied) + "."
                : "Bot count target set to " + std::to_string(applied) + " (requested " +
                      std::to_string(requested) + ", capped at " + std::to_string(kMaxBots) + ").");
        return;
    }

    if (verb == "spawn_special_mobs") {
        spawning_->requestSpecialPass();
        out("Special mobs spawned");
        return;
    }

    if (verb == "killall" || verb == "kill_all" || verb == "clear_mobs") {
        int removed = 0;
        Query<MobTag> mobs{world_};
        mobs.without<Pet>();
        mobs.each([&](Entity entity, MobTag&) {
            commands_.destroy(entity);
            ++removed;
        });
        out("Killed " + plural(removed, "mob", "mobs") + " (pets left intact)");
        return;
    }

    if (verb == "spawn") {
        // Grammar, everything after <mob> <rarity> optional:
        //   spawn <mob> <rarity>                          -> 1 on you
        //   spawn <mob> <rarity> <amount> [stack|unstack] -> N on you
        //   spawn <mob> <rarity> <x> <y>                  -> 1 at (x,y)
        //   spawn <mob> <rarity> <x> <y> <amount> [stack] -> N at (x,y)
        const auto isStackWord = [](const std::string& s) {
            const std::string k = lowerCase(s);
            return k == "stack" || k == "stacked";
        };
        const auto isUnstackWord = [](const std::string& s) {
            const std::string k = lowerCase(s);
            return k == "unstack" || k == "unstacked" || k == "nostack";
        };
        const auto isStackFlag = [&](const std::string& s) {
            return isStackWord(s) || isUnstackWord(s);
        };

        if (words.size() < 3) {
            std::string mobTypes;
            for (std::size_t i = 0; i < content().mobCount(); ++i) {
                if (!mobTypes.empty()) mobTypes += ", ";
                mobTypes += content().mob(static_cast<std::uint16_t>(i)).id;
            }
            out("Usage: spawn <mobType> <rarity> [x] [y] [amount] [stack|unstack]");
            out("  No x/y spawns on you (or randomly if run from the server console).");
            out("  amount: how many to spawn (default 1, max 500). stack piles them on");
            out("  one spot; the default (unstacked) spreads them via mob collision.");
            out("  Examples:");
            out("    spawn bee rare");
            out("    spawn bee rare 10                (10 bees, spread apart)");
            out("    spawn bee rare 10 stack          (10 bees in a pile)");
            out("    spawn bee legendary 1000 2000    (1 bee at 1000,2000)");
            out("    spawn bee legendary 1000 2000 5  (5 bees at 1000,2000)");
            out("Available mob types: " + mobTypes);
            out("Valid rarities: common, uncommon, rare, epic, legendary, mythic, ultra, super, "
                "unique");
            return;
        }

        // A bad mob type or rarity is a console diagnostic and nothing else,
        // exactly as it is in the reference: its spawnMob logs the two lines
        // and returns, and the chat still gets the "Spawned" acknowledgement.
        const std::uint16_t mobIndex = content().mobIndex(words[1]);
        Rarity rarity = Rarity::Common;
        const bool knownRarity = parseRarityStrict(words[2], rarity);
        if (mobIndex == kInvalidIndex) std::printf("Invalid mob type: %s\n", words[1].c_str());
        if (!knownRarity) std::printf("Invalid rarity: %s\n", words[2].c_str());

        // Coordinates are present only when BOTH slots 3 and 4 parse as
        // numbers. `spawn bee rare 10 stack` has a stack word in slot 4, so
        // slot 3 there is an amount, not an x.
        double x = 0;
        double y = 0;
        bool hasCoords = false;
        std::size_t amountAt = 3;
        if (words.size() >= 5 && !isStackFlag(words[4]) && parseNumber(words[3], x) &&
            parseNumber(words[4], y)) {
            if (!saneCoordinate(x) || !saneCoordinate(y)) {
                out("Coordinates out of range: (" + words[3] + ", " + words[4] +
                    "). Max is \xC2\xB1" + maxCoordinateText() + ".");
                return;
            }
            hasCoords = true;
            amountAt = 5;
        }

        int count = 1;
        bool stack = false;
        std::size_t stackAt = amountAt + 1;
        if (amountAt < words.size() && !isStackFlag(words[amountAt])) {
            if (!parseInteger(words[amountAt], count) || count < 1) {
                out("Invalid amount \"" + words[amountAt] +
                    "\". Amount must be a positive whole number.");
                return;
            }
        } else {
            stackAt = amountAt;
        }
        if (stackAt < words.size()) {
            if (isStackWord(words[stackAt])) {
                stack = true;
            } else if (!isUnstackWord(words[stackAt])) {
                out("Unknown option \"" + words[stackAt] + "\". Expected \"stack\" or \"unstack\".");
                return;
            }
        }
        // Clamped silently, as the reference clamps it inside spawnMob.
        constexpr int kMaxSpawnBatch = 500;
        count = std::min(count, kMaxSpawnBatch);

        // With no explicit coordinates the mobs land on whoever ran the
        // command, and on a random legal point when that player has no body --
        // a console command run from the title screen still has to put the mob
        // somewhere the world will accept.
        std::string where = hasCoords ? " at (" + words[3] + ", " + words[4] + ")" : std::string();
        bool placed = hasCoords;
        if (!placed && session.playing() && world_.isAlive(session.entity)) {
            if (const Transform* transform = world_.tryGet<Transform>(session.entity)) {
                x = transform->position.x;
                y = transform->position.y;
                where = " at your location";
                placed = true;
            }
        }
        if (!placed) {
            const Vec2 point = pickBotSpawn();
            x = point.x;
            y = point.y;
            where = " at a random location";
        }

        if (mobIndex != kInvalidIndex && knownRarity) {
            for (int i = 0; i < count; ++i) {
                Vec2 at{x, y};
                if (!stack && count > 1) {
                    // A ring rather than a pile: spawnMob pushes a point out of
                    // the terrain but does nothing about mobs standing on each
                    // other, so unstacked has to mean actually apart.
                    const double angle = rng_.angle();
                    const double radius = rng_.range(0.0, 40.0 + 8.0 * static_cast<double>(i));
                    at = {x + std::cos(angle) * radius, y + std::sin(angle) * radius};
                }
                spawning_->spawnMob(world_, *terrain_, content(), mobIndex, rarity, at,
                                    monotonicMillis(), rng_);
            }
        }
        out("Spawned " + (count > 1 ? std::to_string(count) + "x " : std::string()) + words[2] +
            " " + words[1] + where +
            (count > 1 ? (stack ? ", stacked" : ", unstacked") : ""));
        return;
    }

    // -- teleports ---------------------------------------------------------

    if (verb == "teleport" || verb == "tp") {
        if (words.size() != 4) {
            out("Usage: teleport <playerId/username> <x> <y>");
            out("  Examples:");
            out("    teleport abc123 1000 2000");
            out("    teleport Username 5000 3000");
            out("    tp abc123 1000 2000  (shorthand)");
            return;
        }
        double x = 0;
        double y = 0;
        if (!parseNumber(words[2], x) || !parseNumber(words[3], y)) {
            out("Invalid coordinates. Usage: teleport <playerId/name> <x> <y>");
            return;
        }
        if (!saneCoordinate(x) || !saneCoordinate(y)) {
            out("Coordinates out of range: (" + words[2] + ", " + words[3] + "). Max is \xC2\xB1" +
                maxCoordinateText() + ".");
            return;
        }
        CommandTarget target;
        if (!resolveCommandTarget(words[1], target)) {
            out("Player \"" + words[1] + "\" not found. Use list-players to see available "
                "players.");
            return;
        }
        teleportEntity(target.entity, {x, y});
        out("Teleported player " + target.name + " (" +
            std::to_string(target.session != nullptr
                               ? static_cast<std::uint64_t>(target.session->connection)
                               : static_cast<std::uint64_t>(target.entity)) +
            ") to (" + words[2] + ", " + words[3] + ")");
        return;
    }

    if (verb == "teleport_all" || verb == "tpall" || verb == "teleport_bots" || verb == "tpbots") {
        const bool botsOnly = verb == "teleport_bots" || verb == "tpbots";
        const std::string name = botsOnly ? "teleport_bots" : "teleport_all";
        if (words.size() != 3) {
            out("Usage: " + name + " <x> <y>");
            out(botsOnly ? "  Teleports every bot to (x, y); real players are untouched."
                         : "  Teleports every online player and bot to (x, y).");
            out(botsOnly ? "  Example: teleport_bots 1000 2000   (tpbots works too)"
                         : "  Example: teleport_all 1000 2000   (tpall works too)");
            return;
        }
        double x = 0;
        double y = 0;
        if (!parseNumber(words[1], x) || !parseNumber(words[2], y)) {
            out("Invalid coordinates. Usage: " + name + " <x> <y>");
            return;
        }
        if (!saneCoordinate(x) || !saneCoordinate(y)) {
            out("Coordinates out of range: (" + words[1] + ", " + words[2] + "). Max is \xC2\xB1" +
                maxCoordinateText() + ".");
            return;
        }
        int movedBots = 0;
        for (const Bot& bot : bots_) {
            if (bot.entity == NULL_ENTITY || !world_.isAlive(bot.entity)) continue;
            teleportEntity(bot.entity, {x, y});
            ++movedBots;
        }
        if (botsOnly) {
            out("Teleported " + plural(movedBots, "bot", "bots") + " to (" + words[1] + ", " +
                words[2] + ")");
            return;
        }
        int moved = 0;
        for (auto& entry : sessions_) {
            if (!entry.second.playing()) continue;
            teleportEntity(entry.second.entity, {x, y});
            ++moved;
        }
        out("Teleported " + plural(moved, "player", "players") + " and " +
            plural(movedBots, "bot", "bots") + " to (" + words[1] + ", " + words[2] + ")");
        return;
    }

    // -- player state ------------------------------------------------------

    if (verb == "corrupt") {
        if (words.size() < 2 || words.size() > 3) {
            out("Usage: corrupt <playerId/username> [on|off|toggle]  (default: toggle)");
            return;
        }
        const std::string mode = words.size() == 3 ? lowerCase(words[2]) : "toggle";
        if (mode != "on" && mode != "off" && mode != "toggle") {
            out("Unknown mode \"" + mode + "\". Use on, off or toggle.");
            return;
        }
        CommandTarget target;
        PlayerVisuals* visuals = nullptr;
        if (resolveCommandTarget(words[1], target)) {
            visuals = world_.tryGet<PlayerVisuals>(target.entity);
        }
        if (visuals == nullptr) {
            out("Player \"" + words[1] + "\" not found. Use list-players to see available "
                "players.");
            return;
        }
        // Setting the flag is the whole operation: combat resolves corruption
        // on the live flower every hit, and replication folds it into the face
        // flags, so a ring strung before this still turns hostile.
        visuals->corrupted = mode == "toggle" ? !visuals->corrupted : mode == "on";
        out((visuals->corrupted ? std::string("Corrupted ") : std::string("Cleansed ")) +
            target.name + " (" +
            std::to_string(target.session != nullptr
                               ? static_cast<std::uint64_t>(target.session->connection)
                               : static_cast<std::uint64_t>(target.entity)) +
            ")");
        return;
    }

    if (verb == "set_skin") {
        if (words.size() != 3) {
            out("Usage: set_skin <playerId/username> <" + skinNameList("|") + "|none|bitmask>");
            return;
        }
        // A skin name, "none", or a raw bitmask -- the three spellings the
        // reference accepts, resolved in the same order.
        int flags = -1;
        const std::string wanted = lowerCase(words[2]);
        if (wanted == "none") {
            flags = 0;
        } else {
            for (std::size_t i = 0; i < kSkinNames.size(); ++i) {
                if (wanted == lowerCase(kSkinNames[i])) flags = 1 << static_cast<int>(i);
            }
        }
        if (flags < 0 && (!parseInteger(words[2], flags) || flags < 0)) {
            out("Unknown skin \"" + words[2] + "\". Available: " + skinNameList(", ") +
                ", none, or a numeric bitmask.");
            return;
        }
        CommandTarget target;
        if (!resolveCommandTarget(words[1], target) || target.session == nullptr) {
            out("Player \"" + words[1] + "\" not found. Use list-players to see available "
                "players.");
            return;
        }
        if (PlayerVisuals* visuals = world_.tryGet<PlayerVisuals>(target.entity)) {
            visuals->renderFlags = static_cast<std::uint32_t>(flags);
        }
        // Persisted immediately: a skin is account content, and waiting for
        // the periodic save would lose it to a restart.
        database_.progress(target.session->userId).renderFlags = static_cast<std::uint32_t>(flags);
        database_.markDirty();
        out("Set " + target.name + "'s renderFlags to " + std::to_string(flags) +
            (flags == 0 ? " (default flower)" : ""));
        return;
    }

    if (verb == "give") {
        if (words.size() < 4 || words.size() > 5) {
            out("Usage: give <playerId/username> <itemType> <rarity> [amount]");
            out("  amount: how many to give (default 1).");
            out("  Works for online players (by socket id or username) and offline");
            out("  accounts (by username) \xE2\x80\x94 offline gives are saved directly to the "
                "account.");
            out("  Examples:");
            out("    give abc123 basic rare");
            out("    give Username rose legendary 5");
            out("  Item types:");
            out("    Petals: any petal type (e.g., basic, rose, stinger)");
            out("  Valid rarities: " + rarityList());
            return;
        }
        int amount = 1;
        if (words.size() == 5 && (!parseInteger(words[4], amount) || amount < 1)) {
            out("Invalid amount \"" + words[4] + "\". Amount must be a positive whole number.");
            return;
        }
        const std::string itemType = lowerCase(words[2]);
        const std::string rarityText = lowerCase(words[3]);
        Rarity rarity = Rarity::Common;
        if (!parseRarityStrict(rarityText, rarity)) {
            out("Invalid rarity. Valid rarities: " + rarityList());
            return;
        }
        const std::uint16_t petalIndex = content().petalIndex(itemType);
        if (petalIndex == kInvalidIndex) {
            out("Petal type \"" + itemType + "\" does not exist or does not have rarity \"" +
                rarityText + "\"");
            return;
        }

        // Online by account name, otherwise straight into the persisted
        // account: a give that only worked for people currently logged in is
        // half a command, and the live inventory is rebuilt from the record on
        // the next connection anyway.
        CommandTarget target;
        const bool online = resolveCommandTarget(words[1], target) && target.session != nullptr;
        const std::string userId =
            online ? target.session->userId
                   : [&] {
                         const Account* account = database_.findUser(words[1]);
                         return account != nullptr ? account->id : std::string();
                     }();
        if (userId.empty()) {
            out("Player \"" + words[1] + "\" not found. Use list-players to see online players, "
                "or double-check the username for offline accounts.");
            return;
        }

        PlayerRecord& record = database_.progress(userId);
        record.addItem(rarity, "petal_" + content().petal(petalIndex).id, amount);
        database_.markDirty();
        if (online) {
            if (net::Connection* peer = listener_.find(target.session->connection)) {
                sendProfile(*target.session, *peer);
            }
        }
        const std::string amountLabel = amount > 1 ? std::to_string(amount) + "x " : std::string();
        out("Gave " + amountLabel + rarityText + " " + itemType + " petal to " +
            (online ? target.name + " (" + std::to_string(target.session->connection) + ")"
                    : database_.canonicalUsername(words[1]) + " (offline)"));
        return;
    }

    // -- moderation --------------------------------------------------------

    if (verb == "grant_admin" || verb == "revoke_admin") {
        const bool granting = verb == "grant_admin";
        // The console is lent, not given: only a database-flagged admin hands
        // grants out or takes them back, so a grantee cannot extend the loan.
        if (!session.admin) {
            out("Only a full admin can grant or revoke admin access.");
            return;
        }
        if (words.size() != 2) {
            out("Usage: " + verb + " <playerId/username>");
            return;
        }
        CommandTarget target;
        if (!resolveCommandTarget(words[1], target) || target.session == nullptr) {
            out("Player \"" + words[1] + "\" not found. Use list-players to see available "
                "players.");
            return;
        }
        const std::string grantId = std::to_string(target.session->connection);
        net::Connection* peer = listener_.find(target.session->connection);
        if (granting) {
            if (target.session->admin) {
                out(target.name + " is already a full admin.");
                return;
            }
            if (tempAdmins_.count(target.session->connection) != 0) {
                out(target.name + " already has a temporary admin grant.");
                return;
            }
            tempAdmins_[target.session->connection] = {
                session.username, static_cast<double>(database_.nowMillis())};
            if (peer != nullptr) {
                // Resent so the grantee's own client learns it is admin and
                // stops hiding the /admin rows in its command autocomplete.
                sendSkinCatalog(*target.session, *peer);
                sendSystem(*peer, "<span style=\"color: #ffb74d;\">You have been granted admin "
                                  "commands until you respawn. Use /admin &lt;command&gt; or "
                                  "/help.</span>");
            }
            out("Granted temporary admin to " + target.name + " (" + grantId +
                ") until they respawn.");
            return;
        }
        if (tempAdmins_.count(target.session->connection) == 0) {
            out(target.name + " has no temporary admin grant.");
            return;
        }
        revokeTempAdmin(target.session->connection);
        if (peer != nullptr) {
            sendSystem(*peer, "<span style=\"color: #ff8866;\">Your temporary admin access has "
                              "been revoked.</span>");
        }
        out("Revoked temporary admin from " + target.name + " (" + grantId + ").");
        return;
    }

    if (verb == "list_admins") {
        // Temporary grants only. Permanent admins are a database flag, not a
        // thing with a lifetime, and listing them here would read as if they
        // were about to expire.
        if (tempAdmins_.empty()) {
            out("No temporary admin grants are active.");
            return;
        }
        out("Temporary admins (" + std::to_string(tempAdmins_.size()) + "):");
        const double now = static_cast<double>(database_.nowMillis());
        for (const auto& [id, grant] : tempAdmins_) {
            const Session* holder = sessionFor(id);
            const std::string name = holder != nullptr ? holder->username : "Unknown";
            out(name + " (" + std::to_string(id) + ") \xE2\x80\x94 granted by " + grant.grantedBy +
                ", " + std::to_string(static_cast<long>((now - grant.grantedAtMillis) / 1000)) +
                "s ago");
        }
        return;
    }

    if (verb == "mute" || verb == "unmute") {
        const bool muting = verb == "mute";
        if (words.size() != 2) {
            out("Usage: " + verb + " <playerId/username>");
            return;
        }
        // Straight to the account, so an offline player can be muted too: the
        // flag is persisted and outlives the session.
        Account* account = database_.findUser(words[1]);
        if (account == nullptr) {
            CommandTarget target;
            if (resolveCommandTarget(words[1], target) && target.session != nullptr) {
                account = database_.findUser(target.session->username);
            }
        }
        if (account == nullptr) {
            out("No account named \"" + words[1] +
                "\" exists. Use list-players to see online players.");
            return;
        }
        // Muting a full admin is refused, so a temporary grantee cannot
        // silence the admin who lent them the console.
        if (muting && account->admin) {
            out(account->username + " is a full admin and cannot be muted.");
            return;
        }
        if (account->muted == muting) {
            out(account->username + " is already " + (muting ? "muted." : "not muted."));
            return;
        }
        account->muted = muting;
        account->mutedAtMillis = muting ? database_.nowMillis() : 0;
        account->mutedBy = muting ? session.username : std::string();
        database_.markDirty();

        Session* holder = sessionForUser(account->username);
        if (holder != nullptr) {
            if (net::Connection* peer = listener_.find(holder->connection)) {
                sendSystem(*peer,
                           muting ? "<span style=\"color: #ff8866;\">You have been muted by an "
                                    "admin and can no longer send chat messages.</span>"
                                  : "<span style=\"color: #6eff6e;\">You have been unmuted and "
                                    "can send chat messages again.</span>");
            }
        }
        out(std::string(muting ? "Muted " : "Unmuted ") + account->username +
            (holder != nullptr ? "." : " (offline)."));
        return;
    }

    // -- star codes --------------------------------------------------------

    if (verb == "generate_code" || verb == "gen_code") {
        if (words.size() < 2) {
            out("Usage: generate_code <stars> [maxUses]");
            out("  Default maxUses is 1. Use 0 for unlimited.");
            out("  Examples:");
            out("    generate_code 100  (single use)");
            out("    generate_code 500 10  (max 10 uses)");
            out("    generate_code 1000 0  (unlimited uses)");
            out("    gen_code 1000  (shorthand, single use)");
            return;
        }
        int stars = 0;
        if (!parseInteger(words[1], stars) || stars <= 0) {
            out("Invalid stars amount. Usage: generate_code <stars> [maxUses]");
            out("  Default maxUses is 1. Use 0 for unlimited.");
            return;
        }
        // A maxUses that does not parse leaves the default of 1 standing, as
        // it does in the reference; only a valid 0 means unlimited.
        int maxUses = 1;
        if (words.size() >= 3) {
            int requested = 0;
            if (parseInteger(words[2], requested) && requested >= 0) maxUses = requested;
        }

        Json& codes = database_.rawTable("codes");
        Rng codeRng(static_cast<std::uint64_t>(database_.nowMillis()) * 0x9E3779B97F4A7C15ull ^
                    static_cast<std::uint64_t>(codes.keys().size()));
        std::string code;
        for (int attempt = 0; attempt < 100; ++attempt) {
            code = mintCode(codeRng);
            if (!codes.contains(code)) break;
            code.clear();
        }
        if (code.empty()) {
            out("Failed to generate unique code after 100 attempts");
            return;
        }

        Json entry = Json::object();
        entry["code"] = code;
        entry["stars"] = stars;
        // Absent rather than zero for unlimited, which is the shape the
        // browser build writes and handleRedeemCode already reads: it treats a
        // maxUses of 0 as no limit, and an absent key reads as 0.
        if (maxUses > 0) entry["maxUses"] = maxUses;
        entry["uses"] = 0;
        entry["usedBy"] = Json::array();
        entry["createdBy"] = session.username;
        entry["createdAt"] = static_cast<double>(database_.nowMillis());
        codes[code] = std::move(entry);
        database_.markDirty();

        out("[CODE GENERATED]");
        out("Code: " + code);
        out("Stars: " + std::to_string(stars));
        out(maxUses > 0 ? "Max Uses: " + std::to_string(maxUses) : "Max Uses: Unlimited");
        out("Created by: " + session.username);
        out("Players can redeem this code in the shop!");
        return;
    }

    if (verb == "list_codes") {
        const Json& codes = database_.storedTable("codes");
        if (!codes.isObject() || codes.keys().empty()) {
            out("No codes have been generated.");
            return;
        }
        out("[GENERATED CODES] (" + std::to_string(codes.keys().size()) + " total)");
        for (const std::string& code : codes.keys()) {
            const Json& entry = codes[code];
            const int maxUses = entry["maxUses"].asInt(0);
            out("Code: " + code);
            out("  Stars: " + std::to_string(entry["stars"].asInt(0)));
            out("  Uses: " + std::to_string(entry["uses"].asInt(0)) +
                (maxUses > 0 ? "/" + std::to_string(maxUses) : std::string(" (unlimited)")));
            out("  Created by: " + entry["createdBy"].asString("Unknown"));
            const std::int64_t createdAt = static_cast<std::int64_t>(entry["createdAt"].asDouble(0));
            if (createdAt > 0) out("  Created: " + localeTimestamp(createdAt));
        }
        return;
    }

    if (verb == "delete_code") {
        if (words.size() != 2) {
            out("Usage: delete_code <code>");
            return;
        }
        std::string code = words[1];
        for (char& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        Json& codes = database_.rawTable("codes");
        if (!codes.contains(code)) {
            out("Code " + code + " not found.");
            return;
        }
        codes.erase(code);
        database_.markDirty();
        out("Code " + code + " has been deleted.");
        return;
    }

    // -- notifications -----------------------------------------------------

    if (verb == "notification" || verb == "notify") {
        const std::string type = words.size() >= 2 ? lowerCase(words[1]) : std::string();
        const std::string text = words.size() >= 3 ? joined(words, 2) : std::string();
        if (text.empty()) {
            out("Usage: notification <type> <message>");
            out("  Or: notify <type> <message> (shorthand)");
            out("  Valid types: super_craft, unique_craft, apex_craft, star_code");
            out("  Examples:");
            out("    notification star_code Special event starting now!");
            out("    notify unique_craft New unique petal discovered!");
            return;
        }
        if (!validNotificationType(type)) {
            out("Invalid notification type. Valid types: super_craft, unique_craft, apex_craft, "
                "star_code");
            return;
        }
        // rawArrayTable, never rawTable: this is the one unmodelled table the
        // browser stores as an ARRAY, and coercing it would replace the whole
        // feed with an empty object.
        Json& feed = database_.rawArrayTable("notifications");
        const std::int64_t now = database_.nowMillis();
        Json entry = Json::object();
        entry["id"] = std::to_string(now) + "-" + std::to_string(feed.size());
        entry["type"] = type;
        entry["message"] = text;
        entry["timestamp"] = static_cast<double>(now);
        feed.push(std::move(entry));
        database_.markDirty();
        out("Notification created: " + text);
        return;
    }

    if (verb == "clear_notifications" || verb == "clear_notifs") {
        Json& feed = database_.rawArrayTable("notifications");
        const std::size_t count = feed.size();
        feed = Json::array();
        database_.markDirty();
        out("Cleared " + std::to_string(count) + " notification(s)");
        return;
    }

    // -- guilds ------------------------------------------------------------

    if (verb == "guild_list" || verb == "list_guilds") {
        const Json& guilds = database_.storedTable("guilds");
        if (!guilds.isObject() || guilds.keys().empty()) {
            out("No guilds exist.");
            return;
        }
        out("Guilds (" + std::to_string(guilds.keys().size()) + "):");
        for (const std::string& key : guilds.keys()) {
            const Json& guild = guilds[key];
            out("  \"" + key + "\" \xE2\x80\x94 " +
                std::to_string(guild["memberUsernames"].size()) + "/" +
                std::to_string(kMaxGuildSize) + " \xE2\x80\x94 leader @" +
                guild["leaderUsername"].asString());
        }
        return;
    }

    if (verb == "guild_info") {
        const std::string name = normalizeGuildName(rest);
        const Json& guilds = database_.storedTable("guilds");
        if (name.empty()) {
            out("Usage: guild_info <guild name>");
            return;
        }
        if (!guilds.isObject() || !guilds.contains(name)) {
            out("Guild \"" + name + "\" not found.");
            return;
        }
        const Json& guild = guilds[name];
        const Json& members = guild["memberUsernames"];
        out("\"" + name + "\" \xE2\x80\x94 leader @" + guild["leaderUsername"].asString() +
            " \xE2\x80\x94 " + std::to_string(members.size()) + "/" +
            std::to_string(kMaxGuildSize));
        std::string list;
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (i > 0) list += ", ";
            list += members[i].asString();
        }
        out("Members: " + list);
        return;
    }

    if (verb == "guild_force_join" || verb == "guild_force") {
        // The guild name may contain spaces in the browser build, so the LAST
        // token is the username and everything between is the name.
        if (words.size() < 3) {
            out("Usage: guild_force_join <guild name> <username>");
            return;
        }
        const std::string targetUser = words.back();
        std::string guildName;
        for (std::size_t i = 1; i + 1 < words.size(); ++i) {
            if (!guildName.empty()) guildName += ' ';
            guildName += words[i];
        }
        guildName = normalizeGuildName(guildName);

        Json& guilds = database_.rawTable("guilds");
        if (!guilds.contains(guildName)) {
            out("Guild \"" + guildName + "\" not found.");
            return;
        }
        if (guilds[guildName]["memberUsernames"].size() >= kMaxGuildSize) {
            out("Guild is full.");
            return;
        }
        const std::string canonical = database_.canonicalUsername(targetUser);
        if (canonical.empty()) {
            out("No player named \"" + targetUser + "\" exists.");
            return;
        }

        // Leaving the previous guild is part of joining: membership is stored
        // on the guild, so a player left in two member arrays IS in two guilds
        // and guildNameForUser would answer with whichever it walked first.
        const std::string previous = guildNameForUser(canonical);
        if (previous == guildName) {
            out(canonical + " is already in this guild.");
            return;
        }
        if (!previous.empty()) {
            Json& old = guilds[previous];
            const int index = guildMemberIndex(old, canonical);
            if (index >= 0) {
                Json kept = Json::array();
                const Json& members = old["memberUsernames"];
                for (std::size_t i = 0; i < members.size(); ++i) {
                    if (static_cast<int>(i) != index) kept.push(members[i]);
                }
                old["memberUsernames"] = std::move(kept);
            }
            broadcastGuildRoster(guilds[previous]);
        }
        guilds[guildName]["memberUsernames"].push(Json(canonical));
        database_.markDirty();

        out("Force-joined " + canonical + " into guild \"" + guildName + "\".");

        const std::string author = "[Guild " + guildName + "]";
        const Json& members = guilds[guildName]["memberUsernames"];
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (net::Connection* peer = connectionForUser(members[i].asString())) {
                sendChatTo(*peer, net::ChatChannel::System, author,
                           canonical + " was added to the guild by an admin.");
            }
        }
        if (net::Connection* peer = connectionForUser(canonical)) {
            sendSystem(*peer, "<span style=\"color: #ffb74d;\">You were added to guild \"" +
                                  guildName + "\" by an admin.</span>");
        }
        broadcastGuildRoster(guilds[guildName]);
        return;
    }

    // -- the process itself ------------------------------------------------
    //
    // A restart IS a process exit: pm2, systemd or docker is what brings the
    // server back, on both builds. Players get the reference's warnings on the
    // way down, which is the whole reason it is scheduled rather than done.

    if (verb == "restart") {
        const std::string arg = lowerCase(rest);
        if (arg.empty()) {
            out(scheduleRestart(60000.0, "admin")
                    ? "Restart scheduled in 60 seconds. Use \"restart cancel\" to abort."
                    : "Cannot schedule: a restart is already firing.");
            return;
        }
        if (arg == "status") {
            double remaining = 0;
            std::string reason;
            if (!scheduledRestartInfo(remaining, reason)) {
                out("No restart scheduled.");
                return;
            }
            const long minutes = static_cast<long>(remaining / 60000.0);
            const long seconds = static_cast<long>(std::fmod(remaining, 60000.0) / 1000.0);
            out("Restart scheduled in " + std::to_string(minutes) + "m " +
                std::to_string(seconds) + "s (reason: " + reason + ").");
            return;
        }
        if (arg == "cancel" || arg == "abort") {
            out(cancelScheduledRestart() ? "Pending restart cancelled."
                                         : "No pending restart to cancel.");
            return;
        }
        double millis = 0;
        if (!parseDurationMillis(arg, millis)) {
            out("Usage: restart [<seconds>|<N>(s|m|h)|cancel|status]");
            return;
        }
        if (!scheduleRestart(millis, "admin")) {
            out("Cannot schedule: a restart is already firing.");
            return;
        }
        out("Restart scheduled in " + std::to_string(std::lround(millis / 1000.0)) +
            "s. Use \"restart cancel\" to abort.");
        return;
    }

    if (verb == "backup_db" || verb == "db_backup") {
        const std::string arg = lowerCase(rest);
        if (arg == "list") {
            const std::vector<Database::BackupInfo> backups = database_.listBackups();
            if (backups.empty()) {
                out("No database backups yet. Run \"backup_db\" to create one.");
                return;
            }
            out("Database backups (" + std::to_string(backups.size()) + ", newest first):");
            for (const Database::BackupInfo& backup : backups) {
                out("  " + backup.file + " \xE2\x80\x94 " + kilobytes(backup.bytes) +
                    " KB \xE2\x80\x94 " + localeTimestamp(backup.modifiedMillis));
            }
            out("To restore: copy a backup over inventory.json (in dist/) and restart the "
                "server.");
            return;
        }
        if (!arg.empty()) {
            out("Usage: backup_db [list]");
            return;
        }
        Database::BackupInfo backup;
        std::string error;
        if (!database_.backup("manual-" + session.username, backup, error)) {
            out("Database backup FAILED: " + error);
            return;
        }
        out("Database backed up to " + backup.file + " (" + kilobytes(backup.bytes) + " KB)");
        return;
    }

    if (verb == "update") {
        const std::string arg = lowerCase(rest);
        if (arg == "status") {
            out(autoupdate::inProgress() ? "Update in progress. " + autoupdate::lastStatus()
                                         : autoupdate::lastStatus());
            double remaining = 0;
            std::string reason;
            if (scheduledRestartInfo(remaining, reason) && reason == "update") {
                out("Post-update restart in " +
                    std::to_string(static_cast<long>(std::ceil(remaining / 1000.0))) + "s.");
            }
            return;
        }
        if (arg == "cancel" || arg == "abort") {
            if (autoupdate::inProgress()) {
                out("Update is mid-install and cannot be cancelled (it only takes a few "
                    "seconds).");
                return;
            }
            double remaining = 0;
            std::string reason;
            if (scheduledRestartInfo(remaining, reason) && reason == "update" &&
                cancelScheduledRestart()) {
                out("Post-update restart cancelled. The new build is already on disk and will "
                    "load on the next restart.");
            } else {
                out("No pending post-update restart to cancel.");
            }
            return;
        }
        if (arg == "help" || arg == "?") {
            out("Usage: update [now|<N>(s|m|h)|status|cancel]");
            out("  Backs up the database FIRST (aborts if that fails), downloads the latest");
            out("  build (dist/) from the GitHub repo, installs it over the running server");
            out("  (inventory.json is never touched), then restarts. Default restart delay");
            out("  is 60s so players get warned.");
            return;
        }

        double delayMillis = 60000.0;
        if (arg == "now") {
            delayMillis = 0;
        } else if (!arg.empty() && !parseDurationMillis(arg, delayMillis)) {
            out("Usage: update [now|<N>(s|m|h)|status|cancel]");
            return;
        }
        if (!autoupdate::supported()) {
            out(autoupdate::lastStatus());
            return;
        }
        if (autoupdate::inProgress()) {
            out("An update is already in progress. Use \"update status\" to check on it.");
            return;
        }

        // Step one is the server's own, not the installer's: the backup has to
        // succeed before a single file is touched, and a failure here aborts
        // with the running build untouched.
        out("[UPDATE] Step 1/4: backing up database (required before updating)...");
        Database::BackupInfo backup;
        std::string error;
        if (!database_.backup("pre-update", backup, error)) {
            out("[UPDATE] FAILED: Database backup FAILED - update aborted, nothing was "
                "changed. (" + error + ")");
            return;
        }
        out("[UPDATE] Database backed up to " + backup.file + " (" + kilobytes(backup.bytes) +
            " KB)");

        updateRestartDelayMillis_ = delayMillis;
        // The progress lines follow the SOCKET, not the session: the answer is
        // for whoever is still on the other end of it when each step lands.
        updateRequester_ = session.connection;
        if (!autoupdate::start(autoupdate::defaultUrl())) {
            out("An update is already in progress. Use \"update status\" to check on it.");
        }
        return;
    }

    if (verb == "change-maze" || verb == "change_maze") {
        out(adminChangeMaze(rest));
        return;
    }

    // An unrecognised verb is not answered. The reference's command chain
    // simply falls off its last `else if`, and the only thing the operator
    // sees is the "[ADMIN] ... executed" echo above.
}

} // namespace flix
